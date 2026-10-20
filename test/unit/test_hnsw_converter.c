#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "postgres.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/palloc.h"

/* The local development machine has PostgreSQL 14 headers. Keep unrelated
 * PG16-only branches in hnsw_export.c parseable while this test exercises
 * only the PostgreSQL-free preflight routines. */
#if PG_VERSION_NUM < 150000
#define object_ownercheck(objtype, objid, roleid) true
#define InvalidRelFileNumber InvalidOid
#endif

static Page test_buffer_get_page(Buffer buffer);
#undef BufferGetPage
#define BufferGetPage(buffer) test_buffer_get_page(buffer)
static void *test_palloc(Size size);
#undef palloc
#define palloc(size) test_palloc(size)
/* Simulate a cancel arriving after the first location-table read. */
static jmp_buf test_interrupt_jump;
static bool test_readback_cancel_enabled;
static unsigned test_readback_reads;
static void test_check_for_interrupts(void)
{
    if (test_readback_cancel_enabled && test_readback_reads > 0)
        longjmp(test_interrupt_jump, 1);
}
#undef CHECK_FOR_INTERRUPTS
#define CHECK_FOR_INTERRUPTS() test_check_for_interrupts()

/* Include the implementation so this bounded test can exercise its private
 * file preflight and label lookup without a running PostgreSQL backend. */
#include "../../src/hnsw_export.c"

/* The fixture has PG page headers but no running backend. These small stubs
 * provide the backend primitives used by the production page writer. */
/* Unexpected backend ERRORs must fail the standalone fixture, rather than
 * relying on unresolved symbols that a platform linker might tolerate. */
bool
errstart(int elevel, const char *domain)
{
    (void)elevel;
    (void)domain;
    return true;
}

bool
errstart_cold(int elevel, const char *domain)
{
    return errstart(elevel, domain);
}

int
errmsg(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    return 0;
}

void
errfinish(const char *filename, int lineno, const char *funcname)
{
    fprintf(stderr, "\nunexpected backend ERROR at %s:%d (%s)\n",
            filename, lineno, funcname);
    abort();
}

Size
PageGetFreeSpace(Page page)
{
    PageHeader header = (PageHeader)page;
    int space = (int)header->pd_upper - (int)header->pd_lower - (int)sizeof(ItemIdData);
    return space > 0 ? (Size)space : 0;
}

void
PageInit(Page page, Size page_size, Size special_size)
{
    PageHeader header = (PageHeader)page;

    assert(page != NULL && page_size <= BLCKSZ && special_size <= page_size);
    memset(page, 0, page_size);
    header->pd_lower = SizeOfPageHeaderData;
    header->pd_upper = page_size - special_size;
    header->pd_special = page_size - special_size;
    PageSetPageSizeAndVersion(page, page_size, PG_PAGE_LAYOUT_VERSION);
}

OffsetNumber
PageAddItemExtended(Page page, Item item, Size size,
                    OffsetNumber offset_number, int flags)
{
    PageHeader header = (PageHeader)page;
    OffsetNumber offno;
    Size aligned_size = MAXALIGN(size);

    if (item == NULL || flags != 0 || offset_number != InvalidOffsetNumber ||
        header->pd_upper < header->pd_lower ||
        aligned_size + sizeof(ItemIdData) > (size_t)(header->pd_upper - header->pd_lower))
        return InvalidOffsetNumber;
    offno = (OffsetNumber)(PageGetMaxOffsetNumber(page) + 1);
    if (offno > MaxOffsetNumber)
        return InvalidOffsetNumber;
    header->pd_lower += sizeof(ItemIdData);
    header->pd_upper -= aligned_size;
    ItemIdSetNormal(PageGetItemId(page, offno), header->pd_upper, size);
    memcpy((char *)page + header->pd_upper, item, size);
    return offno;
}

#ifndef __linux__
/* Linux links the equivalent frontend allocators from libpgcommon. */
void *
palloc0(Size size)
{
    return calloc(1, size);
}

void
pfree(void *pointer)
{
    free(pointer);
}
#endif

/* A regular-file stand-in for BufFile lets this standalone test exercise the
 * production location-table callbacks without initializing a PG backend. */
struct BufFile
{
    int fd;
    off_t offset;
    uint8_t *logical_bytes;
    size_t logical_size;
    size_t logical_capacity;
};

size_t
BufFileRead(BufFile *file, void *buffer, size_t size)
{
    size_t available;

    if (file->offset < 0 || (uint64_t)file->offset >= file->logical_size)
        return 0;
    available = file->logical_size - (size_t)file->offset;
    if (size > available)
        size = available;
    memcpy(buffer, file->logical_bytes + file->offset, size);
    file->offset += (off_t)size;
    if (test_readback_cancel_enabled)
        test_readback_reads++;
    return size;
}

#if PG_VERSION_NUM >= 160000
void
BufFileWrite(BufFile *file, const void *buffer, size_t size)
#else
void
BufFileWrite(BufFile *file, void *buffer, size_t size)
#endif
{
    size_t start;
    size_t end;

    assert(file->offset >= 0 && (uint64_t)file->offset <= SIZE_MAX);
    start = (size_t)file->offset;
    assert(size <= SIZE_MAX - start);
    end = start + size;
    if (end > file->logical_capacity)
    {
        size_t capacity = file->logical_capacity ? file->logical_capacity : BLCKSZ;
        uint8_t *grown;

        while (capacity < end)
        {
            assert(capacity <= SIZE_MAX / 2);
            capacity *= 2;
        }
        grown = (uint8_t *)realloc(file->logical_bytes, capacity);
        assert(grown != NULL);
        file->logical_bytes = grown;
        file->logical_capacity = capacity;
    }
    if (start > file->logical_size)
        memset(file->logical_bytes + file->logical_size, 0,
               start - file->logical_size);
    memcpy(file->logical_bytes + start, buffer, size);
    if (end > file->logical_size)
        file->logical_size = end;
    file->offset = (off_t)end;
}

int
BufFileSeekBlock(BufFile *file, long block)
{
    if (block < 0)
        return -1;
    file->offset = (off_t)block * BLCKSZ;
    return 0;
}

int64
BufFileSize(BufFile *file)
{
    (void)file;
    abort(); /* PostgreSQL documents this API for shared filesets only. */
}

void
BufFileClose(BufFile *file)
{
    free(file->logical_bytes);
    file->logical_bytes = NULL;
    file->logical_size = 0;
    file->logical_capacity = 0;
    close(file->fd);
    file->fd = -1;
}

/* A tiny block store lets the standalone fixture call the production page
 * writers and follow their on-page nextblkno fields like pgvector vacuum. */
#define CHAIN_TEST_MAX_BLOCKS 16
static char chain_test_pages[CHAIN_TEST_MAX_BLOCKS][BLCKSZ];
static size_t chain_test_nblocks;
static char chain_test_palloc_arena[256 * 1024];
static size_t chain_test_palloc_used;

static void *
test_palloc(Size size)
{
    size_t aligned_size = MAXALIGN(size);
    void *result;

    assert(aligned_size <= sizeof(chain_test_palloc_arena) -
                           chain_test_palloc_used);
    result = &chain_test_palloc_arena[chain_test_palloc_used];
    chain_test_palloc_used += aligned_size;
    return result;
}

Buffer
ReadBuffer(Relation rel, BlockNumber block_num)
{
    (void)rel;
    assert(block_num == P_NEW);
    assert(chain_test_nblocks < CHAIN_TEST_MAX_BLOCKS);
    memset(chain_test_pages[chain_test_nblocks], 0, BLCKSZ);
    return (Buffer)(++chain_test_nblocks);
}

static Page
test_buffer_get_page(Buffer buffer)
{
    assert(buffer > 0 && (size_t)buffer <= chain_test_nblocks);
    return (Page)chain_test_pages[buffer - 1];
}

void
LockBuffer(Buffer buffer, int mode)
{
    (void)buffer;
    (void)mode;
}

void
UnlockReleaseBuffer(Buffer buffer)
{
    (void)buffer;
}

void
MarkBufferDirty(Buffer buffer)
{
    (void)buffer;
}

XLogRecPtr
log_newpage_buffer(Buffer buffer, bool page_std)
{
    (void)buffer;
    (void)page_std;
    return InvalidXLogRecPtr;
}

static void
chain_test_reset(void)
{
    chain_test_nblocks = 1;
    chain_test_palloc_used = 0;
    memset(chain_test_pages, 0, sizeof(chain_test_pages));
    hnsw_init_disk_page((Page)chain_test_pages[0]);
}

static void
assert_source_data_page_chain(uint32_t expected_pages)
{
    BlockNumber block_num = 1; /* pgvector HNSW_HEAD_BLKNO */
    uint32_t visited = 0;

    while (BlockNumberIsValid(block_num))
    {
        PgvHnswPageOpaque *opaque;

        assert(block_num > 0 && (size_t)block_num < chain_test_nblocks);
        assert(visited < expected_pages);
        opaque = (PgvHnswPageOpaque *)PageGetSpecialPointer(
            (Page)chain_test_pages[block_num]);
        assert(opaque->page_id == PGV_HNSW_PAGE_ID);
        visited++;
        block_num = opaque->nextblkno;
    }

    assert(visited == expected_pages);
}

typedef struct Fixture
{
    int hnsw_fd;
    int tids_fd;
} Fixture;

static void
write_at(int fd, const void *data, size_t n, off_t offset)
{
    const char *p = data;
    size_t done = 0;

    while (done < n)
    {
        ssize_t written = pwrite(fd, p + done, n - done, offset + (off_t)done);
        assert(written > 0);
        done += (size_t)written;
    }
}

static void
fixture_close(Fixture *f)
{
    if (f->hnsw_fd >= 0)
        close(f->hnsw_fd);
    if (f->tids_fd >= 0)
        close(f->tids_fd);
    f->hnsw_fd = -1;
    f->tids_fd = -1;
}

static int
make_temp_fd(void)
{
    char path[] = "/tmp/pg-cuvs-hnsw-fixture-XXXXXX";
    int fd = mkstemp(path);

    assert(fd >= 0);
    assert(unlink(path) == 0);
    return fd;
}

static Fixture
fixture_create(bool hierarchical)
{
    Fixture f = {make_temp_fd(), make_temp_fd()};
    HnswlibHeader h;
    CuvsTidsHeader tids_hdr;
    uint8_t rows[2][28];
    uint64_t tids[2] = {
        cuvs_tid_encode(4, 1),
        cuvs_tid_encode(8, 3)
    };
    uint32_t no_upper = 0;
    uint32_t upper_size = 2 * sizeof(int);
    int upper_count = 1;
    int upper_link = 1;
    uint32_t upper_zero = 0;
    uint32_t crc;

    memset(&h, 0, sizeof(h));
    h.offsetLevel0 = 0;
    h.maxElements = 2;
    h.curElementCount = 2;
    h.sizeDataPerElement = sizeof(rows[0]);
    h.labelOffset = 20;
    h.offsetData = 12;
    /* The patched NONE serializer may retain maxlevel=1 while every stored
     * node and the entrypoint are actually at level zero. */
    h.maxlevel = 1;
    h.enterpointNode = 0;
    h.maxM = 1;
    h.maxM0 = 2;
    h.M = 1;
    h.mult = 1.0;
    h.efConstruction = 100;

    memset(rows, 0, sizeof(rows));
    for (int i = 0; i < 2; i++)
    {
        int link_count = 1;
        int links[2] = {1 - i, -1};
        float vec[2] = {(float)i + 0.25f, -(float)i - 0.5f};
        size_t label = (size_t)i;

        memcpy(rows[i] + h.offsetLevel0, &link_count, sizeof(link_count));
        memcpy(rows[i] + h.offsetLevel0 + sizeof(link_count), links, sizeof(links));
        memcpy(rows[i] + h.offsetData, vec, sizeof(vec));
        memcpy(rows[i] + h.labelOffset, &label, sizeof(label));
    }

    assert(write(f.hnsw_fd, &h, sizeof(h)) == (ssize_t)sizeof(h));
    assert(write(f.hnsw_fd, rows, sizeof(rows)) == (ssize_t)sizeof(rows));
    if (hierarchical)
    {
        assert(write(f.hnsw_fd, &upper_size, sizeof(upper_size)) ==
               (ssize_t)sizeof(upper_size));
        assert(write(f.hnsw_fd, &upper_count, sizeof(upper_count)) ==
               (ssize_t)sizeof(upper_count));
        assert(write(f.hnsw_fd, &upper_link, sizeof(upper_link)) ==
               (ssize_t)sizeof(upper_link));
        assert(write(f.hnsw_fd, &upper_zero, sizeof(upper_zero)) ==
               (ssize_t)sizeof(upper_zero));
    }
    else
    {
        assert(write(f.hnsw_fd, &no_upper, sizeof(no_upper)) ==
               (ssize_t)sizeof(no_upper));
        assert(write(f.hnsw_fd, &no_upper, sizeof(no_upper)) ==
               (ssize_t)sizeof(no_upper));
    }

    memset(&tids_hdr, 0, sizeof(tids_hdr));
    tids_hdr.magic = CUVS_TIDS_MAGIC;
    tids_hdr.version = CUVS_TIDS_VERSION;
    tids_hdr.n_vecs = 2;
    tids_hdr.dim = 2;
    tids_hdr.metric = CUVS_METRIC_COSINE;
    crc = cuvs_crc32(tids, sizeof(tids));
    tids_hdr.body_crc32 = crc;
    assert(write(f.tids_fd, &tids_hdr, sizeof(tids_hdr)) ==
           (ssize_t)sizeof(tids_hdr));
    assert(write(f.tids_fd, tids, sizeof(tids)) == (ssize_t)sizeof(tids));
    return f;
}

static bool
preflight(Fixture *f, uint64_t n, uint32_t dim, uint32_t metric,
          bool packed_nsw, HnswlibHeader *out)
{
    CuvsTidsHeader tids_hdr;
    char error[256] = {0};
    bool ok;

    ok = hnsw_file_preflight(f->hnsw_fd, f->tids_fd, n, dim, metric,
                             4, packed_nsw, out, &tids_hdr,
                             NULL, NULL, NULL, error, sizeof(error));
    if (!ok)
        fprintf(stderr, "preflight rejected fixture: %s\n", error);
    return ok;
}

static void
test_valid_flat_and_original_labels(void)
{
    Fixture f = fixture_create(false);
    HnswlibHeader h;
    uint64_t tid = 0;
    char error[256] = {0};

    assert(preflight(&f, 2, 2, CUVS_METRIC_COSINE, true, &h));
    assert(h.curElementCount == 2);
    assert(h.maxlevel == 1);
    assert(hnsw_read_tid_for_label(f.tids_fd, 2, 1, &tid,
                                   error, sizeof(error)));
    assert(tid == cuvs_tid_encode(8, 3));
    fixture_close(&f);
}

static void
test_valid_hierarchy(void)
{
    Fixture f = fixture_create(true);

    /* Make this a real hierarchical fixture by putting node zero at level 1. */
    {
        uint32_t upper_size = 2 * sizeof(int);
        write_at(f.hnsw_fd, &upper_size, sizeof(upper_size),
                 HNSWLIB_HEADER_BYTES + 2 * 28);
    }
    assert(preflight(&f, 2, 2, CUVS_METRIC_COSINE, false, NULL));
    fixture_close(&f);
}

static void
test_disk_location_layout(void)
{
    HnswNodeLocation location;

    assert(hnsw_layout_location(0, 5, 0, true, 2, &location));
    assert(location.elem_blkno == 1 && location.elem_offno == 1);
    assert(hnsw_layout_location(1, 5, 0, true, 2, &location));
    assert(location.elem_blkno == 1 && location.elem_offno == 3);
    assert(hnsw_layout_location(2, 5, 0, true, 2, &location));
    assert(location.elem_blkno == 2 && location.elem_offno == 1);
    assert(hnsw_layout_location(2, 5, 3, false, 1, &location));
    assert(location.elem_blkno == 3 && location.elem_offno == 1 &&
           location.level == 3);
    assert(!hnsw_layout_location(0, 1, 1, true, 2, &location));
}

static void
test_empty_page_images(void)
{
    Page metapage = calloc(1, BLCKSZ);
    Page first_data_page = calloc(1, BLCKSZ);
    PgvHnswMeta *meta;
    PgvHnswPageOpaque *opaque;

    assert(metapage != NULL && first_data_page != NULL);
    assert(hnsw_init_empty_pages(metapage, first_data_page, 4, 32, 128));
    meta = (PgvHnswMeta *)PageGetContents(metapage);
    assert(meta->magicNumber == PGV_HNSW_MAGIC);
    assert(meta->version == PGV_HNSW_VERSION);
    assert(meta->dimensions == 4 && meta->m == 32 &&
           meta->efConstruction == 128);
    assert(meta->entryBlkno == InvalidBlockNumber);
    assert(meta->entryOffno == InvalidOffsetNumber && meta->entryLevel == -1);
    assert(meta->insertPage == 1);
    assert(((PageHeader)metapage)->pd_lower ==
           SizeOfPageHeaderData + sizeof(PgvHnswMeta));
    opaque = (PgvHnswPageOpaque *)PageGetSpecialPointer(metapage);
    assert(opaque->page_id == PGV_HNSW_PAGE_ID &&
           opaque->nextblkno == InvalidBlockNumber);
    opaque = (PgvHnswPageOpaque *)PageGetSpecialPointer(first_data_page);
    assert(opaque->page_id == PGV_HNSW_PAGE_ID &&
           opaque->nextblkno == InvalidBlockNumber && opaque->unused == 0);
    assert(((PageHeader)first_data_page)->pd_lower == SizeOfPageHeaderData);
    assert(!hnsw_init_empty_pages(metapage, first_data_page, 0, 32, 128));
    assert(!hnsw_init_empty_pages(metapage, first_data_page, 4, 32, 0));
    free(metapage);
    free(first_data_page);
}

static void
test_packed_page_geometry(void)
{
    const int dim = 16;
    const int m = 32;
    const int bytes_per_dim = 4;
    uint32_t pack_k;
    uint32_t page_count;
    uint64_t n_nodes;
    size_t esize = elem_tuple_size(dim, bytes_per_dim);
    size_t nsize = neigh_tuple_size(0, m);
    size_t reserved_pair_bytes = esize + nsize + 4 * PGV_ITEM_OVERHEAD;
    Page page;
    HnswNodeLocation location;
    float vector[16] = {0};

    assert(hnsw_compute_pack_k(dim, bytes_per_dim, m, &pack_k));
    assert(pack_k > 1);
    assert((uint64_t)pack_k * reserved_pair_bytes <= PGV_USABLE_BYTES);
    assert((uint64_t)(pack_k + 1) * reserved_pair_bytes > PGV_USABLE_BYTES);
    n_nodes = 2 * (uint64_t)pack_k + 1;
    assert(hnsw_data_page_count(n_nodes, true, pack_k, &page_count));
    assert(page_count == 3);
    assert(hnsw_layout_location(n_nodes - 1, n_nodes, 0, true, pack_k,
                                &location));
    assert(location.elem_blkno == page_count && location.elem_offno == 1);

    page = calloc(1, BLCKSZ);
    assert(page != NULL);
    hnsw_init_disk_page(page);
    for (uint32_t slot = 0; slot < pack_k; slot++)
    {
        ElemDesc elem = {0};
        OffsetNumber elem_off = (OffsetNumber)(1 + 2 * slot);
        OffsetNumber neigh_off = (OffsetNumber)(elem_off + 1);

        elem.level = 0;
        elem.tid_encoded = cuvs_tid_encode((uint32_t)slot + 1, 1);
        elem.elem_blkno = 1;
        elem.elem_offno = elem_off;
        elem.neigh_blkno = 1;
        elem.neigh_offno = neigh_off;
        write_elem_pair(page, &elem, vector, dim, bytes_per_dim, m,
                        (size_t)n_nodes, hnsw_lookup_elem_array, &elem);
    }
    assert(PageGetMaxOffsetNumber(page) == (OffsetNumber)(2 * pack_k));
    assert(((PageHeader)page)->pd_lower ==
           SizeOfPageHeaderData + 2 * pack_k * sizeof(ItemIdData));
    assert(((PageHeader)page)->pd_upper >= ((PageHeader)page)->pd_lower);
    assert(((PageHeader)page)->pd_upper ==
           BLCKSZ - sizeof(PgvHnswPageOpaque) - pack_k * (esize + nsize));
    free(page);

    assert(!hnsw_data_page_count(0, true, pack_k, &page_count));
    assert(!hnsw_data_page_count(n_nodes, true, 0, &page_count));
    assert(hnsw_data_page_count((uint64_t)InvalidBlockNumber - 1, false,
                                1, &page_count));
    assert(page_count == InvalidBlockNumber - 1);
    assert(!hnsw_data_page_count((uint64_t)InvalidBlockNumber, false,
                                 1, &page_count));
    assert(!hnsw_data_page_count(UINT64_MAX, true, pack_k, &page_count));
}

static void
test_legacy_writer_page_chains(void)
{
    ElemDesc elems[5] = {{0}};
    float vectors[5][2] = {{0}};
    HnswNodeLocation location;

    /* The hierarchical/one-node-per-page writer must link actual page images. */
    for (size_t node = 0; node < 3; node++)
    {
        elems[node].level = 0;
        elems[node].tid_encoded = cuvs_tid_encode((uint32_t)node + 1, 1);
        elems[node].elem_blkno = (uint32_t)node + 1;
        elems[node].elem_offno = 1;
        elems[node].neigh_blkno = (uint32_t)node + 1;
        elems[node].neigh_offno = 2;
        vectors[node][0] = (float)node;
    }

    chain_test_reset();
    for (size_t node = 0; node < 3; node++)
        write_elem_page(NULL, (uint32_t)node + 1, &elems[node], vectors[node],
                        2, 4, 1, 3, true, hnsw_lookup_elem_array, elems);
    assert_source_data_page_chain(3);

    /* Exercise the packed legacy writer across a final partial third page. */
    for (size_t node = 0; node < 5; node++)
    {
        assert(hnsw_layout_location(node, 5, 0, true, 2, &location));
        elems[node].level = 0;
        elems[node].tid_encoded = cuvs_tid_encode((uint32_t)node + 1, 1);
        elems[node].elem_blkno = location.elem_blkno;
        elems[node].elem_offno = location.elem_offno;
        elems[node].neigh_blkno = location.elem_blkno;
        elems[node].neigh_offno = (uint16_t)(location.elem_offno + 1);
        vectors[node][0] = (float)node;
    }

    chain_test_reset();
    write_elem_pages_packed(NULL, elems, &vectors[0][0], 2, 4, 1, 5, 2,
                            true, hnsw_lookup_elem_array, elems);
    assert_source_data_page_chain(3);
}

static void
test_disk_writer_page_chain(void)
{
    Fixture f = fixture_create(true);
    struct BufFile table_file = {.fd = make_temp_fd()};
    HnswLocationTable locations;
    HnswlibHeader header;
    CuvsTidsHeader tids_header;
    char error[256] = {0};

    memset(&locations, 0, sizeof(locations));
    locations.file = &table_file;
    locations.n_nodes = 2;
    assert(hnsw_file_preflight(f.hnsw_fd, f.tids_fd, 2, 2,
                               CUVS_METRIC_COSINE, 4, false,
                               &header, &tids_header,
                               hnsw_location_table_append, &locations, NULL,
                               error, sizeof(error)));
    assert(locations.n_written == 2);

    chain_test_reset();
    hnsw_write_disk_pages(NULL, f.hnsw_fd, f.tids_fd, &header, 2, 2, 4,
                          false, 1, 2, true, &locations);
    assert_source_data_page_chain(2);

    BufFileClose(&table_file);
    fixture_close(&f);
}

static void
test_pgvector_page_version_range(void)
{
    assert(hnsw_pgvector_version_supported("0.5.0"));
    assert(hnsw_pgvector_version_supported("0.8.7"));
    assert(!hnsw_pgvector_version_supported("0.4.9"));
    assert(!hnsw_pgvector_version_supported("0.9.0"));
    assert(!hnsw_pgvector_version_supported("1.0.0"));
    assert(!hnsw_pgvector_version_supported("unknown"));
}

static void
test_buffered_location_table_contract(void)
{
    struct BufFile file = {.fd = make_temp_fd()};
    HnswLocationTable table;
    HnswNodeLocation first = {1, 1, 0, 0};
    HnswNodeLocation second = {2, 3, 1, 0};
    HnswNodeLocation got;
    struct stat backing_stat;
    char error[128] = {0};

    memset(&table, 0, sizeof(table));
    table.file = &file;
    table.n_nodes = 2;
    assert(hnsw_location_table_append(&table, 0, &first,
                                      error, sizeof(error)));
    assert(hnsw_location_table_append(&table, 1, &second,
                                      error, sizeof(error)));
    /* Buffered writes are logically present while the backing fd is empty. */
    assert(fstat(file.fd, &backing_stat) == 0 && backing_stat.st_size == 0);
    assert(hnsw_location_table_rewind(&table, 2));
    assert(!hnsw_location_table_rewind(&table, 3));
    assert(hnsw_location_table_lookup(&table, 1, &got));
    assert(got.elem_blkno == second.elem_blkno &&
           got.elem_offno == second.elem_offno && got.level == second.level);
    file.logical_size -= sizeof(second);
    assert(!hnsw_location_table_rewind(&table, 2));
    BufFileClose(&file);
}

static void
test_location_readback_interrupt(void)
{
    struct BufFile *file = calloc(1, sizeof(*file));
    HnswLocationTable table = {0};
    HnswNodeLocation record = {1, 1, 0, 0};
    char error[128] = {0};

    assert(file != NULL);
    file->fd = make_temp_fd();
    table.file = file;
    table.n_nodes = HNSW_LOCATION_ROWS_PER_PAGE * 2 + 1;
    for (uint64_t node = 0; node < table.n_nodes; node++)
        assert(hnsw_location_table_append(&table, node, &record,
                                          error, sizeof(error)));
    test_readback_reads = 0;
    test_readback_cancel_enabled = true;
    if (setjmp(test_interrupt_jump) == 0)
    {
        (void)hnsw_location_table_rewind(&table, table.n_nodes);
        assert(!"location readback did not service cancellation between chunks");
    }
    test_readback_cancel_enabled = false;
    assert(test_readback_reads == 1);
    BufFileClose(file);
    free(file);
}

static void
test_disk_location_table_roundtrip(void)
{
    Fixture f = fixture_create(false);
    struct BufFile fake_file = {.fd = make_temp_fd()};
    HnswLocationTable table;
    HnswlibHeader hnsw_hdr;
    CuvsTidsHeader tids_hdr;
    HnswNodeLocation location;
    uint32_t pack_k;
    char error[256] = {0};

    memset(&table, 0, sizeof(table));
    table.file = &fake_file;
    table.n_nodes = 2;
    assert(hnsw_file_preflight(f.hnsw_fd, f.tids_fd, 2, 2,
                               CUVS_METRIC_COSINE, 4, true,
                               &hnsw_hdr, &tids_hdr,
                               hnsw_location_table_append, &table, NULL,
                               error, sizeof(error)));
    assert(table.n_written == 2);
    assert(hnsw_compute_pack_k(2, 4, 1, &pack_k));
    assert(hnsw_location_table_lookup(&table, 0, &location));
    assert(location.elem_blkno == 1 && location.elem_offno == 1 &&
           location.level == 0);
    assert(hnsw_location_table_lookup(&table, 1, &location));
    assert(location.elem_blkno == 1 && location.elem_offno == 3 &&
           location.level == 0);
    assert(pack_k > 1);
    fixture_close(&f);
    BufFileClose(&fake_file);

    memset(&fake_file, 0, sizeof(fake_file));
    fake_file.fd = make_temp_fd();
    memset(&table, 0, sizeof(table));
    table.file = &fake_file;
    table.n_nodes = HNSW_LOCATION_ROWS_PER_PAGE + 1;
    for (uint64_t node = 0; node < table.n_nodes; node++)
    {
        HnswNodeLocation record = {
            .elem_blkno = (uint32_t)node + 1,
            .elem_offno = 1,
            .level = (uint8_t)(node % 4),
            .reserved = 0
        };
        assert(hnsw_location_table_append(&table, node, &record,
                                          error, sizeof(error)));
    }
    assert(hnsw_location_table_lookup(&table,
                                      HNSW_LOCATION_ROWS_PER_PAGE - 1,
                                      &location));
    assert(location.elem_blkno == HNSW_LOCATION_ROWS_PER_PAGE);
    assert(hnsw_location_table_lookup(&table, HNSW_LOCATION_ROWS_PER_PAGE,
                                      &location));
    assert(location.elem_blkno == HNSW_LOCATION_ROWS_PER_PAGE + 1);
    BufFileClose(&fake_file);
}

static void
test_rejects_count_dimension_metric_and_version_mismatch(void)
{
    Fixture f = fixture_create(false);
    CuvsTidsHeader tids_hdr;

    assert(!preflight(&f, 3, 2, CUVS_METRIC_COSINE, true, NULL));
    assert(!preflight(&f, 2, 3, CUVS_METRIC_COSINE, true, NULL));
    assert(!preflight(&f, 2, 2, CUVS_METRIC_L2, true, NULL));
    assert(pread(f.tids_fd, &tids_hdr, sizeof(tids_hdr), 0) ==
           (ssize_t)sizeof(tids_hdr));
    tids_hdr.version++;
    write_at(f.tids_fd, &tids_hdr, sizeof(tids_hdr), 0);
    assert(!preflight(&f, 2, 2, CUVS_METRIC_COSINE, true, NULL));
    fixture_close(&f);
}

static void
test_rejects_bad_crc_and_truncated_inputs(void)
{
    Fixture f = fixture_create(false);
    uint64_t tid = 0;

    tid = cuvs_tid_encode(2, 2);
    write_at(f.tids_fd, &tid, sizeof(tid), sizeof(CuvsTidsHeader));
    assert(!preflight(&f, 2, 2, CUVS_METRIC_COSINE, true, NULL));
    fixture_close(&f);

    f = fixture_create(false);
    assert(ftruncate(f.hnsw_fd, HNSWLIB_HEADER_BYTES + 4) == 0);
    assert(!preflight(&f, 2, 2, CUVS_METRIC_COSINE, true, NULL));
    fixture_close(&f);
}

static void
test_rejects_invalid_heap_tid_offset_and_tids_size(void)
{
    Fixture f = fixture_create(false);
    CuvsTidsHeader tids_hdr;
    uint64_t tids[2] = {
        cuvs_tid_encode(4, MaxOffsetNumber + 1),
        cuvs_tid_encode(8, 3)
    };

    assert(pread(f.tids_fd, &tids_hdr, sizeof(tids_hdr), 0) ==
           (ssize_t)sizeof(tids_hdr));
    tids_hdr.body_crc32 = cuvs_crc32(tids, sizeof(tids));
    write_at(f.tids_fd, &tids_hdr, sizeof(tids_hdr), 0);
    write_at(f.tids_fd, tids, sizeof(tids), sizeof(tids_hdr));
    assert(!preflight(&f, 2, 2, CUVS_METRIC_COSINE, true, NULL));
    fixture_close(&f);

    f = fixture_create(false);
    assert(ftruncate(f.tids_fd, sizeof(tids_hdr) + sizeof(uint64_t)) == 0);
    assert(!preflight(&f, 2, 2, CUVS_METRIC_COSINE, true, NULL));
    fixture_close(&f);
}

static void
test_rejects_invalid_offsets_labels_and_links(void)
{
    Fixture f = fixture_create(false);
    HnswlibHeader h;
    size_t bad_label = 9;
    int bad_link = 9;

    assert(pread(f.hnsw_fd, &h, sizeof(h), 0) == (ssize_t)sizeof(h));
    h.offsetData = SIZE_MAX - 4;
    write_at(f.hnsw_fd, &h, sizeof(h), 0);
    assert(!preflight(&f, 2, 2, CUVS_METRIC_COSINE, true, NULL));
    fixture_close(&f);

    f = fixture_create(false);
    write_at(f.hnsw_fd, &bad_label, sizeof(bad_label),
             HNSWLIB_HEADER_BYTES + 20);
    assert(!preflight(&f, 2, 2, CUVS_METRIC_COSINE, true, NULL));
    fixture_close(&f);

    f = fixture_create(false);
    write_at(f.hnsw_fd, &bad_link, sizeof(bad_link),
             HNSWLIB_HEADER_BYTES + sizeof(int));
    assert(!preflight(&f, 2, 2, CUVS_METRIC_COSINE, true, NULL));
    fixture_close(&f);
}

static void
test_rejects_malformed_upper_links_and_overflow(void)
{
    Fixture f = fixture_create(true);
    uint32_t bad_size = 1;
    HnswlibHeader h;
    size_t bad_record_size = SIZE_MAX;

    write_at(f.hnsw_fd, &bad_size, sizeof(bad_size),
             HNSWLIB_HEADER_BYTES + 2 * 28);
    assert(!preflight(&f, 2, 2, CUVS_METRIC_COSINE, false, NULL));
    fixture_close(&f);

    f = fixture_create(false);
    assert(pread(f.hnsw_fd, &h, sizeof(h), 0) == (ssize_t)sizeof(h));
    h.sizeDataPerElement = bad_record_size;
    write_at(f.hnsw_fd, &h, sizeof(h), 0);
    assert(!preflight(&f, 2, 2, CUVS_METRIC_COSINE, true, NULL));
    fixture_close(&f);
}

int
main(void)
{
    test_valid_flat_and_original_labels();
    test_valid_hierarchy();
    test_disk_location_layout();
    test_empty_page_images();
    test_packed_page_geometry();
    test_legacy_writer_page_chains();
    test_disk_writer_page_chain();
    test_pgvector_page_version_range();
    test_buffered_location_table_contract();
    test_location_readback_interrupt();
    test_disk_location_table_roundtrip();
    test_rejects_count_dimension_metric_and_version_mismatch();
    test_rejects_bad_crc_and_truncated_inputs();
    test_rejects_invalid_heap_tid_offset_and_tids_size();
    test_rejects_invalid_offsets_labels_and_links();
    test_rejects_malformed_upper_links_and_overflow();
    puts("hnsw converter preflight fixtures: ok");
    return 0;
}
