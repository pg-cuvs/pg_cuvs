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
#include <signal.h>
#include <sys/wait.h>
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
static void test_pfree(void *pointer);
#undef palloc
#define palloc(size) test_palloc(size)
#undef pfree
#define pfree(pointer) test_pfree(pointer)
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

/* Normal-path PG_TRY scaffolding for this standalone fixture. Real PG error
 * propagation and cleanup are tested by test-connectivity-pg-interrupts. */
sigjmp_buf *PG_exception_stack = NULL;
ErrorContextCallback *error_context_stack = NULL;
bool IsUnderPostmaster = false;
sigset_t BlockSig;
void pg_re_throw(void)
{
    abort(); /* an unexpected backend exception must fail this fixture */
}

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

static void
test_pfree(void *pointer)
{
    uintptr_t address = (uintptr_t)pointer;
    uintptr_t start = (uintptr_t)chain_test_palloc_arena;

    /* palloc scratch is reset with the fixture context; palloc0 uses malloc. */
    if (address < start || address >= start + sizeof(chain_test_palloc_arena))
        free(pointer);
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
test_disk_writer_page_chain_case(bool hierarchical)
{
    Fixture f = fixture_create(hierarchical);
    struct BufFile table_file = {.fd = make_temp_fd()};
    HnswLocationTable locations;
    HnswlibHeader header;
    CuvsTidsHeader tids_header;
    char error[256] = {0};

    memset(&locations, 0, sizeof(locations));
    locations.file = &table_file;
    locations.n_nodes = 2;
    assert(hnsw_file_preflight(f.hnsw_fd, f.tids_fd, 2, 2,
                               CUVS_METRIC_COSINE, 4, !hierarchical,
                               &header, &tids_header,
                               hnsw_location_table_append, &locations, NULL,
                               error, sizeof(error)));
    assert(locations.n_written == 2);

    chain_test_reset();
    hnsw_write_disk_pages(NULL, f.hnsw_fd, f.tids_fd, &header, 2, 2, 4,
                          1, true, &locations);
    assert_source_data_page_chain(1);
    assert(PageGetMaxOffsetNumber((Page)chain_test_pages[1]) == 4);

    BufFileClose(&table_file);
    fixture_close(&f);
}

static void
test_disk_writer_page_chain(void)
{
    test_disk_writer_page_chain_case(true);
    test_disk_writer_page_chain_case(false);
}

static void
test_hierarchical_preflight_packs_locations(void)
{
    Fixture f = fixture_create(true);
    struct BufFile table_file = {.fd = make_temp_fd()};
    HnswLocationTable locations = {0};
    HnswlibHeader header;
    CuvsTidsHeader tids_header;
    HnswNodeLocation first;
    HnswNodeLocation second;
    char error[256] = {0};

    locations.file = &table_file;
    locations.n_nodes = 2;
    assert(hnsw_file_preflight(f.hnsw_fd, f.tids_fd, 2, 2,
                               CUVS_METRIC_COSINE, 4, false,
                               &header, &tids_header,
                               hnsw_location_table_append, &locations, NULL,
                               error, sizeof(error)));
    assert(hnsw_location_table_rewind(&locations, 2));
    assert(hnsw_location_table_lookup(&locations, 0, &first));
    assert(hnsw_location_table_lookup(&locations, 1, &second));
    assert(first.elem_blkno == 1 && first.elem_offno == 1 && first.level == 1);
    assert(second.elem_blkno == 1 && second.elem_offno == 3 && second.level == 0);
    BufFileClose(&table_file);
    fixture_close(&f);
}

static void
test_hierarchical_packing_cursor_bounds(void)
{
    HnswPackingCursor cursor = {0};
    HnswNodeLocation location;

    assert(!hnsw_pack_location_next(&cursor, 1, 3, 0, 898, 4, 32, &location));
    assert(cursor.next_node == 0 && cursor.block == 0);
    assert(!hnsw_pack_location_next(&cursor, 0, 3, 0, 0, 4, 32, &location));
    assert(!hnsw_pack_location_next(&cursor, 0, 3, 0, 898, 3, 32, &location));
    assert(!hnsw_pack_location_next(&cursor, 0, 3, 256, 898, 4, 32, &location));
    assert(!hnsw_pack_location_next(&cursor, 0, 3, 0, INT16_MAX, 4, 32, &location));
    /* At D898/M32, two conservative reservations exactly fill 8160 bytes. */
    assert(hnsw_pack_location_next(&cursor, 0, 3, 0, 898, 4, 32, &location));
    assert(location.elem_blkno == 1 && location.elem_offno == 1);
    assert(hnsw_pack_location_next(&cursor, 1, 3, 0, 898, 4, 32, &location));
    assert(location.elem_blkno == 1 && location.elem_offno == 3);
    assert(cursor.remaining == 0);
    assert(hnsw_pack_location_next(&cursor, 2, 3, 0, 898, 4, 32, &location));
    assert(location.elem_blkno == 2 && location.elem_offno == 1);
    assert(!hnsw_pack_location_next(&cursor, 3, 3, 0, 898, 4, 32, &location));
    cursor = (HnswPackingCursor){.block = InvalidBlockNumber - 1};
    assert(!hnsw_pack_location_next(&cursor, 0, 1, 0, 898, 4, 32, &location));
}

static uint32_t
test_pointer_block(const PgvItemPointer *pointer)
{
    return ((uint32_t)pointer->ip_blkid.bi_hi << 16) | pointer->ip_blkid.bi_lo;
}

/* Exercise native files -> production preflight/location table -> staged
 * metapage/page writer. Compare every tuple against the same row-local
 * consumer rendered with the original one-pair layout, translating only
 * physical pointers. Reversed labels and duplicate ordered edges are kept. */
static void
test_hierarchical_native_packing_case(int dim, int bytes_per_dim, int m)
{
    enum { N = 7 };
    const int levels[N] = {0, 2, 0, 1, 0, 0, 0};
    Fixture f = {make_temp_fd(), make_temp_fd()};
    struct BufFile table_file = {.fd = make_temp_fd()};
    HnswLocationTable table = {.file = &table_file, .n_nodes = N};
    HnswlibHeader header = {0};
    HnswlibHeader parsed;
    CuvsTidsHeader tids_header = {0};
    CuvsTidsHeader parsed_tids;
    CuvsHnswPolicy policy = {0};
    HnswNodeLocation locations[N];
    ElemDesc reference_locations[N] = {{0}};
    uint64_t tids[N];
    size_t row_bytes = (size_t)(1 + 2 * m) * sizeof(int)
        + (size_t)dim * sizeof(float) + sizeof(size_t);
    char *raw_row = calloc(1, row_bytes);
    char error[256] = {0};
    char *packed;
    uint32_t n_pages;
    HnswDiskRow row;
    uint64_t upper_cursor;

    assert(raw_row != NULL);
    header.maxElements = header.curElementCount = N;
    header.sizeDataPerElement = row_bytes;
    header.offsetData = (size_t)(1 + 2 * m) * sizeof(int);
    header.labelOffset = header.offsetData + (size_t)dim * sizeof(float);
    header.maxlevel = 2;
    header.enterpointNode = 1;
    header.maxM = header.M = (size_t)m;
    header.maxM0 = (size_t)(2 * m);
    header.mult = 1.0;
    header.efConstruction = 200;
    assert(write(f.hnsw_fd, &header, sizeof(header)) == sizeof(header));
    for (int node = 0; node < N; node++)
    {
        int count = 3;
        int links[3] = {N - 1, N - 1, 0};
        size_t label = (size_t)(N - 1 - node);
        memset(raw_row, 0, row_bytes);
        memcpy(raw_row, &count, sizeof(count));
        memcpy(raw_row + sizeof(count), links, sizeof(links));
        for (int d = 0; d < dim; d++)
        {
            float value = (float)(node * 16 + d) / 32.0f;
            memcpy(raw_row + header.offsetData + (size_t)d * sizeof(float),
                   &value, sizeof(value));
        }
        memcpy(raw_row + header.labelOffset, &label, sizeof(label));
        assert(write(f.hnsw_fd, raw_row, row_bytes) == (ssize_t)row_bytes);
        tids[node] = cuvs_tid_encode((uint32_t)(100 + node), (uint16_t)(node + 1));
        reference_locations[node].elem_blkno = (uint32_t)node + 1;
        reference_locations[node].elem_offno = 1;
    }
    free(raw_row);
    for (int node = 0; node < N; node++)
    {
        uint32_t bytes = (uint32_t)(levels[node] * (m + 1) * sizeof(int));
        int *links = calloc((size_t)(m + 1), sizeof(int));
        assert(links != NULL);
        links[0] = 2;
        links[1] = links[2] = 1;
        assert(write(f.hnsw_fd, &bytes, sizeof(bytes)) == sizeof(bytes));
        for (int level = 0; level < levels[node]; level++)
            assert(write(f.hnsw_fd, links, (size_t)(m + 1) * sizeof(int)) ==
                   (ssize_t)((m + 1) * sizeof(int)));
        free(links);
    }
    tids_header.magic = CUVS_TIDS_MAGIC;
    tids_header.version = CUVS_TIDS_VERSION;
    tids_header.n_vecs = N;
    tids_header.dim = (uint32_t)dim;
    tids_header.metric = CUVS_METRIC_L2;
    tids_header.body_crc32 = cuvs_crc32(tids, sizeof(tids));
    assert(write(f.tids_fd, &tids_header, sizeof(tids_header)) == sizeof(tids_header));
    assert(write(f.tids_fd, tids, sizeof(tids)) == sizeof(tids));
    assert(hnsw_file_preflight(f.hnsw_fd, f.tids_fd, N, (uint32_t)dim,
                               CUVS_METRIC_L2, bytes_per_dim, false,
                               &parsed, &parsed_tids, hnsw_location_table_append,
                               &table, NULL, error, sizeof(error)));
    assert(hnsw_location_table_rewind(&table, N));
    for (int node = 0; node < N; node++)
        assert(hnsw_location_table_lookup(&table, (uint32_t)node, &locations[node]));
    n_pages = locations[N - 1].elem_blkno;
    assert(n_pages > 0 && n_pages <= N);
    policy.magic = CUVS_HNSW_POLICY_MAGIC;
    policy.version = CUVS_HNSW_POLICY_VERSION;
    policy.bytes = sizeof(policy);
    policy.route = CUVS_HNSW_ROUTE_TINY;
    policy.internal_builder = 1;
    policy.requested_ef = 64;
    policy.guard_passed = 1;
    chain_test_reset();
    chain_test_nblocks = 0;
    hnsw_write_disk_metapage(NULL, &parsed, (uint32_t)dim, n_pages,
                             true, &table, &policy, NULL, NULL);
    hnsw_write_disk_pages(NULL, f.hnsw_fd, f.tids_fd, &parsed, N,
                          (uint32_t)dim, bytes_per_dim, n_pages, true, &table);
    assert_source_data_page_chain(n_pages);
    {
        PgvHnswMeta *meta = (PgvHnswMeta *)PageGetContents(chain_test_pages[0]);
        assert(meta->entryBlkno == locations[1].elem_blkno &&
               meta->entryOffno == locations[1].elem_offno && meta->entryLevel == 2);
        assert(meta->insertPage == n_pages);
        assert(memcmp((char *)meta + sizeof(*meta), &policy, sizeof(policy)) == 0);
        if (dim == 768 && bytes_per_dim == 4 && m == 32)
            assert(n_pages == 4 && meta->entryOffno == 3);
    }
    packed = malloc(chain_test_nblocks * BLCKSZ);
    assert(packed != NULL);
    memcpy(packed, chain_test_pages, chain_test_nblocks * BLCKSZ);
    if (getenv("HNSW_PACKING_FIXTURE_OUTPUT") != NULL && (m == 32 || m == 4))
    {
        char directory[1024];
        char path[1100];
        const int fds[2] = {f.hnsw_fd, f.tids_fd};
        const char *names[2] = {"native.bin", "tids.bin"};
        FILE *out;
        unsigned char buffer[65536];

        assert(snprintf(directory, sizeof(directory), "%s/d%d-b%d-m%d",
                        getenv("HNSW_PACKING_FIXTURE_OUTPUT"), dim,
                        bytes_per_dim, m) < (int)sizeof(directory));
        assert(mkdir(directory, 0700) == 0);
        for (int input = 0; input < 2; input++)
        {
            off_t offset = 0;
            ssize_t bytes;
            snprintf(path, sizeof(path), "%s/%s", directory, names[input]);
            out = fopen(path, "wbx");
            assert(out != NULL);
            while ((bytes = pread(fds[input], buffer, sizeof(buffer), offset)) > 0)
            {
                assert(fwrite(buffer, 1, (size_t)bytes, out) == (size_t)bytes);
                offset += bytes;
            }
            assert(bytes == 0 && fclose(out) == 0);
        }
        snprintf(path, sizeof(path), "%s/pgindex", directory);
        out = fopen(path, "wbx");
        assert(out != NULL && fwrite(packed, BLCKSZ, n_pages + 1, out) == n_pages + 1);
        assert(fclose(out) == 0);
    }

    chain_test_reset();
    hnsw_disk_row_init(&row, &parsed, (uint32_t)dim);
    upper_cursor = HNSWLIB_HEADER_BYTES + N * parsed.sizeDataPerElement;
    for (int node = 0; node < N; node++)
    {
        hnsw_read_disk_row(f.hnsw_fd, f.tids_fd, &parsed, N, (uint32_t)dim,
                           (uint64_t)node, &upper_cursor, &table, &row);
        row.elem.elem_blkno = row.elem.neigh_blkno = (uint32_t)node + 1;
        row.elem.elem_offno = 1;
        row.elem.neigh_offno = 2;
        write_elem_page(NULL, (uint32_t)node + 1, &row.elem, row.vector,
                        dim, bytes_per_dim, m, N, true,
                        hnsw_lookup_elem_array, reference_locations);
    }
    for (int node = 0; node < N; node++)
    {
        Page old_page = chain_test_pages[node + 1];
        Page new_page = packed + (size_t)locations[node].elem_blkno * BLCKSZ;
        char *old_element = PageGetItem(old_page, PageGetItemId(old_page, 1));
        char *new_element = PageGetItem(new_page, PageGetItemId(new_page, locations[node].elem_offno));
        char *old_neighbors = PageGetItem(old_page, PageGetItemId(old_page, 2));
        char *new_neighbors = PageGetItem(new_page, PageGetItemId(new_page, locations[node].elem_offno + 1));
        size_t esize = elem_tuple_size(dim, bytes_per_dim);
        int slots = (levels[node] + 2) * m;
        PgvItemPointer *op = (PgvItemPointer *)(old_neighbors + sizeof(PgvHnswNeighHdr));
        PgvItemPointer *np = (PgvItemPointer *)(new_neighbors + sizeof(PgvHnswNeighHdr));
        PgvHnswElemHdr *elem = (PgvHnswElemHdr *)new_element;
        assert(memcmp(old_element, new_element, 64) == 0);
        assert(memcmp(old_element + 70, new_element + 70, esize - 70) == 0);
        assert(test_pointer_block(&elem->heaptids[0]) == (uint32_t)(100 + N - 1 - node));
        assert(elem->heaptids[0].ip_posid == N - node);
        assert(test_pointer_block(&elem->neighbortid) == locations[node].elem_blkno);
        assert(elem->neighbortid.ip_posid == locations[node].elem_offno + 1);
        assert(memcmp(old_neighbors, new_neighbors, sizeof(PgvHnswNeighHdr)) == 0);
        for (int slot = 0; slot < slots; slot++)
        {
            if (op[slot].ip_posid == 0)
                assert(memcmp(&op[slot], &np[slot], sizeof(*op)) == 0);
            else
            {
                uint32_t target = test_pointer_block(&op[slot]) - 1;
                assert(target < N);
                assert(test_pointer_block(&np[slot]) == locations[target].elem_blkno);
                assert(np[slot].ip_posid == locations[target].elem_offno);
            }
        }
    }
    free(packed);
    BufFileClose(&table_file);
    fixture_close(&f);
}

static void
test_hierarchical_native_packing_shapes(void)
{
    test_hierarchical_native_packing_case(768, 4, 32);
    test_hierarchical_native_packing_case(1536, 4, 32);
    test_hierarchical_native_packing_case(768, 2, 32);
    test_hierarchical_native_packing_case(1536, 2, 32);
    test_hierarchical_native_packing_case(3072, 2, 32);
    test_hierarchical_native_packing_case(768, 2, 4);
    test_hierarchical_native_packing_case(8, 4, 2);
    test_hierarchical_native_packing_case(16, 4, 64);
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

static void test_stored_policy_versions(void)
{
    Page page = calloc(1, BLCKSZ), saved = malloc(BLCKSZ);
    CuvsHnswPolicy p = {0}, out;
    assert(page && saved && hnsw_init_metapage(page, 768, 32, 200, 1, FirstOffsetNumber, 0, 1));
    Size prefix = ((PageHeader)page)->pd_lower;
    assert(hnsw_read_disk_policy(page, &out) == CUVS_HNSW_POLICY_NOT_RECORDED);
    p.magic = CUVS_HNSW_POLICY_MAGIC; p.version = CUVS_HNSW_POLICY_LEGACY_VERSION;
    p.bytes = sizeof(p); p.route = CUVS_HNSW_ROUTE_ACE;
    p.requested_algo = CUVS_HNSW_ALGO_AUTO; p.requested_ef = 64; p.guard_passed = 1;
    memcpy((char *)page + prefix, &p, sizeof(p));
    ((PageHeader)page)->pd_lower += sizeof(p); memcpy(saved, page, BLCKSZ);
    assert(hnsw_read_disk_policy(page, &out) == CUVS_HNSW_POLICY_RECORDED && out.version == 1);
    assert(!memcmp(saved, page, BLCKSZ)); /* read never rewrites historical metadata */
    p.version = CUVS_HNSW_POLICY_VERSION; p.requested_algo = CUVS_HNSW_ALGO_ACE;
    p.internal_builder = 3; p.partitions = 2;
    memcpy((char *)page + prefix, &p, sizeof(p));
    assert(hnsw_read_disk_policy(page, &out) == CUVS_HNSW_POLICY_RECORDED && out.version == 2);
    p.requested_algo = CUVS_HNSW_ALGO_AUTO; memcpy((char *)page + prefix, &p, sizeof(p));
    assert(hnsw_read_disk_policy(page, &out) == CUVS_HNSW_POLICY_CORRUPT);
    p.version = 99; memcpy((char *)page + prefix, &p, sizeof(p));
    assert(hnsw_read_disk_policy(page, &out) == CUVS_HNSW_POLICY_UNSUPPORTED);
    ((PageHeader)page)->pd_lower = prefix + 2;
    assert(hnsw_read_disk_policy(page, &out) == CUVS_HNSW_POLICY_CORRUPT);
    ((PageHeader)page)->pd_lower = BLCKSZ + 1;
    assert(hnsw_read_disk_policy(page, &out) == CUVS_HNSW_POLICY_CORRUPT);
    assert(!memcmp(PageGetContents(saved), PageGetContents(page), sizeof(PgvHnswMeta)));
    free(saved); free(page);
    puts("PASS stored policy page v1/v2, no receipt, truncation/future/corruption, prefix and historical bytes unchanged");
}

/* d0a51e7 B2: the ACE partition-builder readback written by the production metapage
 * writer reads back byte-for-byte at prefix + receipt, a resident receipt carries no
 * tail, and an invalid readback is refused by the writer (backend ERROR). */
static void
test_ace_readback_metapage_roundtrip(void)
{
    Fixture f = fixture_create(false);
    struct BufFile fake_file = {.fd = make_temp_fd()};
    HnswLocationTable table;
    HnswlibHeader hdr;
    CuvsTidsHeader tids_hdr;
    CuvsHnswPolicy p = {0}, out, other, resident;
    CuvsHnswAceReadback ace, ace_out;
    char error[256] = {0}, saved_prefix[sizeof(PgvHnswMeta)];
    const Size prefix = SizeOfPageHeaderData + sizeof(PgvHnswMeta);
    Page meta;
    pid_t child;
    int status;

    memset(&table, 0, sizeof(table));
    table.file = &fake_file;
    table.n_nodes = 2;
    assert(hnsw_file_preflight(f.hnsw_fd, f.tids_fd, 2, 2, CUVS_METRIC_COSINE, 4, true,
                               &hdr, &tids_hdr, hnsw_location_table_append, &table, NULL,
                               error, sizeof(error)));
    p.magic = CUVS_HNSW_POLICY_MAGIC; p.version = CUVS_HNSW_POLICY_VERSION; p.bytes = sizeof(p);
    p.route = CUVS_HNSW_ROUTE_ACE; p.internal_builder = CUVS_HNSW_ROUTE_ACE;
    p.requested_algo = CUVS_HNSW_ALGO_ACE; p.requested_ef = 64; p.guard_passed = 1;
    p.partitions = 2; p.degree = 64; p.intermediate = 128; p.ace_ef = 240;
    p.n_rows = 999000; p.dim = 2;
    assert(cuvs_hnsw_policy_valid(&p));
    cuvs_hnsw_ace_readback_fill(&ace, CUVS_HNSW_ACE_QUALITY_AUTO, p.ace_ef, p.degree,
                                p.n_rows, p.partitions, 20);
    assert(cuvs_hnsw_ace_readback_valid(&ace, &p));

    {
        CuvsConnectivity c={0}, readback;
        c.magic=CUVS_CONNECTIVITY_MAGIC;c.version=1;c.available=1;c.nodes=p.n_rows;c.unreachable=70;c.wcc_all=1;
        chain_test_reset();
        hnsw_write_disk_metapage(NULL,&hdr,2,1,true,&table,&p,&ace,&c);
        meta=(Page)chain_test_pages[chain_test_nblocks-1];
        readback=hnsw_read_connectivity(meta);
        assert(readback.magic==c.magic && readback.unreachable==70 && readback.wcc_all==1);
    }
    chain_test_reset();
    hnsw_write_disk_metapage(NULL, &hdr, 2, 1, true, &table, &p, &ace, NULL);
    meta = (Page)chain_test_pages[chain_test_nblocks - 1];
    assert(((PageHeader)meta)->pd_lower == prefix + sizeof(p) + sizeof(ace));
    assert(memcmp((char *)meta + prefix, &p, sizeof(p)) == 0); /* receipt bytes unchanged */
    assert(((PgvHnswMeta *)PageGetContents(meta))->dimensions == 2);
    memcpy(saved_prefix, PageGetContents(meta), sizeof(saved_prefix));
    assert(hnsw_read_disk_policy(meta, &out) == CUVS_HNSW_POLICY_RECORDED);
    assert(memcmp(&out, &p, sizeof(p)) == 0);
    assert(hnsw_read_disk_ace(meta, &out, &ace_out) == CUVS_HNSW_POLICY_RECORDED);
    assert(memcmp(&ace_out, &ace, sizeof(ace)) == 0);
    assert(ace_out.quality == 15 && ace_out.local_intermediate == 96 && ace_out.local_iterations == 20);
    assert(memcmp(saved_prefix, PageGetContents(meta), sizeof(saved_prefix)) == 0);

    /* An ACE index built before the readback existed: receipt only, no tail. */
    ((PageHeader)meta)->pd_lower = prefix + sizeof(p);
    assert(hnsw_read_disk_ace(meta, &out, &ace_out) == CUVS_HNSW_POLICY_NOT_RECORDED);
    ((PageHeader)meta)->pd_lower = prefix + sizeof(p) + sizeof(ace);
    /* A tail that does not belong to this receipt is corrupt, not silently accepted. */
    other = p; other.ace_ef = 112;
    assert(hnsw_read_disk_ace(meta, &other, &ace_out) == CUVS_HNSW_POLICY_CORRUPT);
    /* A truncated tail is corrupt. */
    ((PageHeader)meta)->pd_lower = prefix + sizeof(p) + sizeof(ace) - 1;
    assert(hnsw_read_disk_ace(meta, &out, &ace_out) == CUVS_HNSW_POLICY_CORRUPT);

    /* A resident receipt writes no tail, even when a readback is passed. */
    resident = p;
    resident.route = resident.internal_builder = CUVS_HNSW_ROUTE_IVF_PQ;
    resident.requested_algo = CUVS_HNSW_ALGO_IVF_PQ; resident.partitions = 0;
    assert(cuvs_hnsw_policy_valid(&resident));
    chain_test_reset();
    hnsw_write_disk_metapage(NULL, &hdr, 2, 1, true, &table, &resident, &ace, NULL);
    meta = (Page)chain_test_pages[chain_test_nblocks - 1];
    assert(((PageHeader)meta)->pd_lower == prefix + sizeof(resident));

    /* An ACE receipt without a valid readback is refused by the writer. */
    fflush(stderr);
    child = fork();
    assert(child >= 0);
    if (child == 0)
    {
        CuvsHnswAceReadback bad = ace;
        bad.local_intermediate = 128;
        assert(freopen("/dev/null", "w", stderr) != NULL);
        chain_test_reset();
        hnsw_write_disk_metapage(NULL, &hdr, 2, 1, true, &table, &p, &bad, NULL);
        _exit(0); /* reaching here means the writer accepted it */
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

    fixture_close(&f);
    BufFileClose(&fake_file);
    puts("PASS ACE readback metapage roundtrip: bytes at prefix+receipt, pd_lower, older/foreign/truncated tails, resident has none, writer refuses invalid");
}

/* Execute the real file adapter and metapage writer, not just the graph kernel. */
char *DataDir = NULL;
int temp_file_limit = -1;
int maintenance_work_mem = 65536;
volatile sig_atomic_t InterruptPending = 0;
static void test_connectivity_file_path(void)
{
    Fixture f=fixture_create(false);
    HnswlibHeader h;
    CuvsConnectivity c, saved;
    uint32_t zero=0;
    assert(pread(f.hnsw_fd,&h,sizeof(h),0)==sizeof(h));
    hnsw_check_connectivity(f.hnsw_fd,&h,&c);
    assert(c.available && c.unreachable==0 && c.wcc_all==1);
    /* Delete only the root's outgoing edge: WCC stays one, one node is unreachable. */
    write_at(f.hnsw_fd,&zero,4,HNSWLIB_HEADER_BYTES);
    hnsw_check_connectivity(f.hnsw_fd,&h,&c);
    assert(c.available && c.unreachable==1 && c.wcc_all==1);
    maintenance_work_mem=0;
    hnsw_check_connectivity(f.hnsw_fd,&h,&c);
    assert(c.available && c.spilled && c.unreachable==1);
    temp_file_limit=0;
    hnsw_check_connectivity(f.hnsw_fd,&h,&c);
    assert(!c.available); /* A finite operator cap is never bypassed. */
    temp_file_limit=-1;
    {
        char dir[]="/tmp/conn203-data-XXXXXX", base[MAXPGPATH], scratch[MAXPGPATH];
        assert(mkdtemp(dir));snprintf(base,sizeof(base),"%s/base",dir);assert(mkdir(base,0700)==0);
        DataDir=dir;hnsw_check_connectivity(f.hnsw_fd,&h,&c);
        assert(c.available && c.spilled);
        snprintf(scratch,sizeof(scratch),"%s/base/pgsql_tmp",dir);
        assert(rmdir(scratch)==0 && rmdir(base)==0 && rmdir(dir)==0);
    }
    DataDir="/nonexistent-conn203-parent/path";
    hnsw_check_connectivity(f.hnsw_fd,&h,&c);
    assert(!c.available); /* Temp creation failure is diagnostic, not a build ERROR. */
    DataDir=NULL;maintenance_work_mem=65536;
    (void)saved;
    fixture_close(&f);
    puts("PASS connectivity file adapter, directed-miss RED fixture, automatic file spill");
}

int
main(int argc, char **argv)
{
    if(argc==3 && (strcmp(argv[1],"--connectivity-file")==0 || strcmp(argv[1],"--connectivity-file-spill")==0)) {
        if(strcmp(argv[1],"--connectivity-file-spill")==0)maintenance_work_mem=1;
        int fd=open(argv[2],O_RDONLY);HnswlibHeader h;CuvsConnectivity c;
        assert(fd>=0 && pread(fd,&h,sizeof(h),0)==sizeof(h));
        for(int i=0;i<3;i++) {
            hnsw_check_connectivity(fd,&h,&c);
            printf("{\"rows\":%llu,\"available\":%u,\"unreachable\":%llu,\"wcc\":%llu,\"us\":%llu,\"spilled\":%u}\n",
                (unsigned long long)c.nodes,c.available,(unsigned long long)c.unreachable,
                (unsigned long long)c.wcc_all,(unsigned long long)c.elapsed_us,c.spilled);
            assert(c.available);
        }
        close(fd);return 0;
    }
    test_connectivity_file_path();
    test_stored_policy_versions();
    test_ace_readback_metapage_roundtrip();
    test_valid_flat_and_original_labels();
    test_valid_hierarchy();
    test_disk_location_layout();
    test_empty_page_images();
    test_packed_page_geometry();
    test_legacy_writer_page_chains();
    test_hierarchical_preflight_packs_locations();
    test_disk_writer_page_chain();
    test_hierarchical_packing_cursor_bounds();
    test_hierarchical_native_packing_shapes();
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
