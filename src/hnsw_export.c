/*
 * hnsw_export.c — Phase 3I-2: import hnswlib binary sidecar into a pgvector
 * HNSW relation.
 *
 * The hnswlib binary format is described in the Phase 3I-2 design doc.
 * We parse it in pure C (no C++ / hnswlib headers) and write pgvector-
 * compatible pages using ReadBuffer(P_NEW) + MarkBufferDirty + log_newpage_buffer.
 * GenericXLogFinish was tried but the FULL_IMAGE copy-back was unreliable on
 * PG16 for direct-to-PageGetContents writes; the pgvector build path (direct
 * buffer write + log_newpage) is correct and crash-safe.
 *
 * WAL safety: every new page is logged via log_newpage_buffer(buf, true) before
 * UnlockReleaseBuffer.  Crash recovery replays the full-page images.
 *
 * pgvector version dependency: structs are pinned to pgvector 0.5.0+ layout
 * (HNSW_VERSION=1, stable since Aug 2023).  A pgvector major-version bump that
 * changes the on-disk format would require updating the PgvHnsw* structs and
 * the HNSW_VERSION / HNSW_MAGIC constants below.
 *
 * pgvector HNSW on-disk layout (all sizes in bytes, LE):
 *   Block 0: metapage (HnswMetaPageData + HnswPageOpaqueData)
 *   Data blocks: interleaved element tuples (type=1) and neighbor tuples (type=2).
 *     Source-less imports pack consecutive pairs, including variable levels;
 *     legacy import paths may retain one page per pair.
 *
 * We do NOT include pgvector's hnsw.h (not installed).  All structs are
 * defined locally matching the pgvector 0.8.x binary layout.
 */

#include "postgres.h"
#include <sys/mman.h>
#include <time.h>
#include "cuvs_connectivity.h"
#include "fmgr.h"
#include "access/generic_xlog.h"
#include "access/xlog.h"            /* log_newpage_buffer */
#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/pg_attribute.h"  /* Form_pg_attribute, ATTNUM */
#include "catalog/pg_cast.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"      /* Anum_pg_index_indclass */
#include "catalog/pg_opclass.h"    /* Form_pg_opclass, CLAOID */
#include "utils/syscache.h"        /* SearchSysCache1/2, RELOID, ATTNUM, INDEXRELID, CLAOID */
#include "common/relpath.h"
#include "storage/bufmgr.h"
#include "storage/itemptr.h"
#include "storage/smgr.h"
#include "storage/lmgr.h"         /* CheckRelationLockedByMe */
#include "catalog/storage.h"       /* RelationTruncate */
#include "utils/rel.h"
#include "cuvs_cagra_pages.h"
#include "utils/relcache.h"
#include "utils/builtins.h"
#include "catalog/index.h"       /* index_create, INDEX_CREATE_SKIP_BUILD, BuildIndexInfo */
#include "catalog/pg_am.h"        /* AMOID, Form_pg_am */
#include "commands/defrem.h"     /* get_am_oid */
#include "commands/extension.h"  /* get_extension_oid */
#include "access/amapi.h"        /* IndexAmRoutine, makeNode(T_IndexAmRoutine) */
#include "access/reloptions.h"   /* relopt_kind, add_*_reloption, build_reloptions */
#include "utils/regproc.h"       /* regclassin */
#include "miscadmin.h"
#include "libpq/pqsignal.h"
#include "nodes/pg_list.h"       /* list_make1 */
#include "nodes/parsenodes.h"    /* DefElem */
#include "nodes/makefuncs.h"
#include "access/genam.h"        /* GetSysCacheOid3 */
#include "access/table.h"        /* table_open/table_close (pgvector version guard) */
#include "access/stratnum.h"     /* BTEqualStrategyNumber */
#include "catalog/pg_extension.h"/* ExtensionRelationId, Anum_pg_extension_* */
#include "utils/fmgroids.h"      /* F_OIDEQ */
#include "utils/acl.h"
#include "utils/lsyscache.h"     /* get_rel_name */
#include "utils/jsonb.h"
#include "funcapi.h"               /* get_call_result_type (layer counts) */
#include "utils/guc.h"
#include "catalog/pg_type.h"     /* Form_pg_type (#162: vector/halfvec type detection) */

#include <math.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cuvs_ipc.h"
#include "cuvs_util.h"
#include "cuvs_hnsw_source.h"
#include "cuvs_hnsw_build.h"
#include "hnsw_export.h"
#include "cuvs_hnsw_scan.h"
#include "cuvs_cover_format.h"
#include "cuvs_cover_ledger.h"
#include "cuvs_cover_insert.h"
#include "cuvs_cover_recert.h"
#include "storage/buffile.h"
#include "storage/fd.h"

/* GUC declared in pg_cuvs.c — socket path for daemon IPC */
extern char *cuvs_socket_path;

/* #162: read-only accessor into pg_cuvs.c's local CuvsCagraOptions —
 * used by the pre-GPU dim-ceiling check below. */
extern int cuvs_cagra_reloption_graph_degree(Relation cagra_rel);

static uint32_t cagra_index_metric(Oid index_oid);

/* ----------------------------------------------------------------
 * pgvector HNSW on-disk constants (must match pgvector source)
 * ---------------------------------------------------------------- */
#define PGV_HNSW_MAGIC          0xA953A953u
#define PGV_HNSW_VERSION        1u
#define PGV_HNSW_PAGE_ID        0xFF90u
/* Heap builds default to M 32, which requests a graph degree of 64. */
#define CUVS_HNSW_DEFAULT_M     32

/* Byte size of the 13-field hnswlib binary header (fixed across versions):
 * 10 × size_t(8) + 2 × int(4) + 1 × double(8) = 96 bytes. */
#define HNSWLIB_HEADER_BYTES    96

/* Maximum heaptids stored inline in an element tuple */
#define PGV_HNSW_MAX_HEAPTIDS   10

/*
 * ItemPointerData: (BlockIdData blkhi:blklo, OffsetNumber).
 * BlockIdData is { uint16 bi_hi, uint16 bi_lo }.
 * We replicate the struct so we do not need to include heapam internals.
 */
typedef struct {
    uint16_t bi_hi;
    uint16_t bi_lo;
} PgvBlockId;

typedef struct {
    PgvBlockId  ip_blkid;
    uint16_t    ip_posid;
} PgvItemPointer;   /* 6 bytes */

/* ---- metapage (block 0) ---- */
typedef struct {
    uint32_t        magicNumber;
    uint32_t        version;
    uint32_t        dimensions;
    uint16_t        m;
    uint16_t        efConstruction;
    uint32_t        entryBlkno;
    uint16_t        entryOffno;
    int16_t         entryLevel;
    uint32_t        insertPage;
} PgvHnswMeta;      /* 28 bytes */

/* Reserve a metadata tail OUTSIDE the WAL free-space hole. pd_lower and the
 * historical policy/ACE receipt prefix remain unchanged for older readers. */
/* cuvs.hnsw_connectivity_check (defined and registered in pg_cuvs.c). When off
 * the adapters return a "disabled" record before any scratch allocation, input
 * mapping, interrupt poll or graph traversal, and report no NOTICE. */
extern bool cuvs_hnsw_connectivity_check;

/* Anonymous scratch respects maintenance_work_mem. Larger checks use a
 * preallocated, immediately unlinked file under the default PG temp directory.
 * File-backed pages can be reclaimed by the OS instead of requiring swap.
 * Reserve disk space before mmap writes, avoiding ENOSPC-induced SIGBUS. */
static void *
hnsw_connectivity_workspace(size_t bytes, bool *spilled)
{
    void *p;
    char path[MAXPGPATH], dir[MAXPGPATH];
    int fd, rc, save_errno;
    sigset_t save_sigmask;
    *spilled=false;
    if(bytes <= (uint64_t)maintenance_work_mem*1024) {
        p=malloc(bytes);
        if(p)return p;
    }
    /* This independent mapping is not charged by PostgreSQL VFD accounting.
     * Never bypass an explicitly finite temp_file_limit: report unavailable
     * instead. The ANN build itself continues under its existing policy. */
    if(temp_file_limit>=0)return NULL;
    if(DataDir) {
        if(snprintf(dir,sizeof(dir),"%s/base/pgsql_tmp",DataDir)>=(int)sizeof(dir))return NULL;
        if(mkdir(dir,0700) && errno!=EEXIST)return NULL;
    } else snprintf(dir,sizeof(dir),"/tmp"); /* standalone CPU fixture */
    if(snprintf(path,sizeof(path),"%s/conn203-XXXXXX",dir)>=(int)sizeof(path))return NULL;
    fd=mkstemp(path);
    if(fd<0)return NULL;
    unlink(path);
    /* As in dsm_impl_posix_resize, repeated SIGUSR1 must not keep restarting
     * this all-or-nothing reservation. Defer backend signals until allocation
     * finishes; SIGCONT can still require an EINTR retry. Do not run CFI here:
     * the raw fd is not yet protected by the caller's PG_FINALLY block. */
    if(IsUnderPostmaster)
        sigprocmask(SIG_SETMASK,&BlockSig,&save_sigmask);
    do { rc=posix_fallocate(fd,0,(off_t)bytes); } while(rc==EINTR);
    if(IsUnderPostmaster) {
        save_errno=errno;
        sigprocmask(SIG_SETMASK,&save_sigmask,NULL);
        errno=save_errno;
    }
    if(rc!=0){close(fd);errno=rc;return NULL;}
    p=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    close(fd);
    if(p==MAP_FAILED)return NULL;
    *spilled=true;
    return p;
}
static void
hnsw_connectivity_workspace_free(void *p,size_t bytes,bool spilled)
{
    if(!p)return;
    if(spilled)munmap(p,bytes);else free(p);
}
static void
hnsw_store_connectivity(Page page, const CuvsConnectivity *c)
{
    PageHeader h=(PageHeader)page;
    Size at=h->pd_special-sizeof(*c);
    if (!c || h->pd_special<sizeof(*c) || h->pd_lower>at) return;
    memcpy((char*)page+at,c,sizeof(*c));
    h->pd_upper=at;
}

/* An index whose access method routine came from the NEW handler symbol carries a
 * cover descriptor above pd_upper; one from the old symbol is built exactly as before. The routine is the marker:
 * only the new one installs cuvs_cover_buildempty. */
static void cuvs_cover_buildempty(Relation indexRel);
StaticAssertDecl(sizeof(CuvsConnectivity) == CUVS_COVER_CONN_SIZE, "connectivity record is 64 bytes");

static bool
hnsw_cover_enabled(Relation rel)
{
    return rel->rd_indam != NULL && rel->rd_indam->ambuildempty == cuvs_cover_buildempty;
}

bool
cuvs_hnsw_index_is_cover(Relation rel)
{
    return hnsw_cover_enabled(rel);
}

/* The single place that finishes a metapage. `empty` is a build over zero rows: nothing can be
 * unreachable, so it starts CERTIFIED with an empty ledger at block 2. Every other build starts LOST(uncertified)
 * until a certification has run. For an old-symbol index this is hnsw_store_connectivity and nothing else. */
static void
hnsw_stage_metapage_tail(Relation rel, Page page, const CuvsConnectivity *c, bool empty)
{
    CuvsCoverDescriptor d;

    if (!hnsw_cover_enabled(rel))
    {
        hnsw_store_connectivity(page, c);
        return;
    }
    memset(&d, 0, sizeof(d));
    d.magic = CUVS_COVER_D_MAGIC;
    d.format_version = CUVS_COVER_FORMAT_VERSION;
    d.min_reader_version = 1;
    d.d_len = CUVS_COVER_D_SIZE;
    d.free_head = CUVS_COVER_INVALID_BLOCK;
    d.ledger_head = CUVS_COVER_INVALID_BLOCK;
    d.ledger_tail = CUVS_COVER_INVALID_BLOCK;
    if (empty)
    {
        d.state = CUVS_COVER_STATE_CERTIFIED;
        d.generation = 1;
        d.ledger_head = d.ledger_tail = 2;
        d.graph_nblocks_at_cert = 3;
    }
    else
    {
        d.state = CUVS_COVER_STATE_LOST;
        d.reason = CUVS_COVER_REASON_UNCERTIFIED;
    }
    cuvs_cover_descriptor_seal(&d);
    if (cuvs_cover_stage_tail((uint8_t *) page, (const uint8_t *) c, &d) != 0)
        ereport(ERROR, (errmsg("pg_cuvs: the metapage has no room for the cover descriptor")));
}

/* Fail closed: a pgvector metapage of a cover index is only written when it classifies as a descriptor state. */
static void
hnsw_cover_check_metapage(Relation rel, Page staged)
{
    CuvsCoverResult r;

    if (!hnsw_cover_enabled(rel) || cuvs_cover_get32((const uint8_t *) staged, CUVS_COVER_OFF_PGV_MAGIC) != CUVS_COVER_PGV_MAGIC)
        return;
    cuvs_cover_classify((const uint8_t *) staged, NULL, &r);
    if (!((r.cls == CUVS_COVER_CERTIFIED || r.cls == CUVS_COVER_LOST || r.cls == CUVS_COVER_OFF) && r.writable))
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                        errmsg("pg_cuvs: refusing to publish a metapage without a cover descriptor")));
}
static CuvsConnectivity
hnsw_read_connectivity(Page page)
{
    CuvsConnectivity c={0};
    PageHeader h=(PageHeader)page;
    /* The record sits right below the special area. An index with a cover descriptor has pd_upper further down
     * (above the descriptor), so the position is special - 64, not pd_upper; a page whose pd_upper is the special
     * offset has no record. */
    if(h->pd_special<=BLCKSZ && h->pd_special>=sizeof(c) &&
       h->pd_upper<=h->pd_special-sizeof(c) && h->pd_lower<=h->pd_upper)
        memcpy(&c,(char*)page+h->pd_special-sizeof(c),sizeof(c));
    if(c.magic!=CUVS_CONNECTIVITY_MAGIC || c.version!=1 ||
       c.available>CUVS_CONNECTIVITY_DISABLED || c.spilled>1 ||
       c.unreachable>c.nodes || c.unreachable_l0>c.nodes ||
       c.wcc_all>c.nodes || c.wcc_l0>c.nodes ||
       (c.available==CUVS_CONNECTIVITY_DISABLED &&
        (c.spilled || c.nodes || c.unreachable || c.unreachable_l0 ||
         c.wcc_l0 || c.wcc_all || c.elapsed_us)))
        memset(&c,0,sizeof(c));
    return c;
}
static void
hnsw_append_connectivity(StringInfo json, const CuvsConnectivity *c)
{
    if(c->magic!=CUVS_CONNECTIVITY_MAGIC)
        appendStringInfoString(json,",\"connectivity\":{\"status\":\"not_recorded\",\"scope\":\"build_snapshot\"}");
    else if(c->available==CUVS_CONNECTIVITY_DISABLED)
        appendStringInfoString(json,",\"connectivity\":{\"status\":\"disabled\",\"scope\":\"build_snapshot\",\"setting\":\"cuvs.hnsw_connectivity_check\"}");
    else if(c->available!=CUVS_CONNECTIVITY_MEASURED)
        appendStringInfo(json,",\"connectivity\":{\"status\":\"unavailable\",\"scope\":\"build_snapshot\",\"nodes\":%llu,\"unreachable\":null}",(unsigned long long)c->nodes);
    else
        appendStringInfo(json,",\"connectivity\":{\"status\":\"measured\",\"scope\":\"build_snapshot\",\"nodes\":%llu,\"unreachable\":%llu,\"unreachable_l0\":%llu,\"wcc_all\":%llu,\"wcc_l0\":%llu,\"check_us\":%llu,\"spilled\":%s}",
            (unsigned long long)c->nodes,(unsigned long long)c->unreachable,
            (unsigned long long)c->unreachable_l0,(unsigned long long)c->wcc_all,
            (unsigned long long)c->wcc_l0,(unsigned long long)c->elapsed_us,c->spilled?"true":"false");
}
static void
hnsw_notice_connectivity(const CuvsConnectivity *c)
{
    if(c->available==CUVS_CONNECTIVITY_DISABLED)
        return; /* The check was not run; there is nothing to report. */
    if(c->available==CUVS_CONNECTIVITY_MEASURED)
        ereport(NOTICE,(errmsg("pg_cuvs: build connectivity: unreachable=%llu, L0 unreachable=%llu, WCC=%llu (L0=%llu), check_us=%llu",
            (unsigned long long)c->unreachable,(unsigned long long)c->unreachable_l0,
            (unsigned long long)c->wcc_all,(unsigned long long)c->wcc_l0,
            (unsigned long long)c->elapsed_us),
            errdetail("Structural possible-path check; it does not guarantee finite-ef recall. Search policy is unchanged.")));
    else
        ereport(NOTICE,(errmsg("pg_cuvs: build connectivity check unavailable; search policy is unchanged")));
}

/* Keep the pgvector prefix untouched. The shared bounded decoder consumes
 * only the stored receipt tail, never current GUCs or a guessed build route. */
static CuvsHnswPolicyReadStatus
hnsw_read_disk_policy(Page page, CuvsHnswPolicy *out)
{
    Size prefix = SizeOfPageHeaderData + sizeof(PgvHnswMeta);
    PageHeader header = (PageHeader)page;
    memset(out, 0, sizeof(*out));
    if (header->pd_lower < prefix) return CUVS_HNSW_POLICY_NOT_RECORDED;
    if (header->pd_lower > BLCKSZ || header->pd_lower > header->pd_upper)
        return CUVS_HNSW_POLICY_CORRUPT;
    return cuvs_hnsw_policy_read((const char *)page + prefix,
                                 header->pd_lower - prefix, out);
}

/* The ACE partition-builder readback after a recorded ACE receipt. */
static CuvsHnswPolicyReadStatus
hnsw_read_disk_ace(Page page, const CuvsHnswPolicy *policy, CuvsHnswAceReadback *out)
{
    Size at = SizeOfPageHeaderData + sizeof(PgvHnswMeta) + sizeof(*policy);
    PageHeader header = (PageHeader)page;
    memset(out, 0, sizeof(*out));
    if (header->pd_lower <= at) return CUVS_HNSW_POLICY_NOT_RECORDED;
    return cuvs_hnsw_ace_readback_read((const char *)page + at, header->pd_lower - at,
                                       policy, out);
}

/* ---- page opaque (special area, 8 bytes) ---- */
typedef struct {
    uint32_t    nextblkno;
    uint16_t    unused;
    uint16_t    page_id;
} PgvHnswPageOpaque;   /* 8 bytes */

static void
hnsw_init_disk_page(Page page)
{
    PgvHnswPageOpaque *opaque;

    PageInit(page, BLCKSZ, sizeof(PgvHnswPageOpaque));
    opaque = (PgvHnswPageOpaque *)PageGetSpecialPointer(page);
    opaque->nextblkno = InvalidBlockNumber;
    opaque->unused = 0;
    opaque->page_id = PGV_HNSW_PAGE_ID;
}

/* pgvector's vacuum walkers start at block 1 and follow nextblkno. */
static bool
hnsw_set_disk_page_chain(Page page, uint64_t data_blkno,
                         uint64_t data_page_count)
{
    PgvHnswPageOpaque *opaque;

    if (page == NULL || data_blkno == 0 || data_page_count == 0 ||
        data_page_count >= (uint64_t)InvalidBlockNumber ||
        data_blkno > data_page_count)
        return false;

    opaque = (PgvHnswPageOpaque *)PageGetSpecialPointer(page);
    if (opaque->page_id != PGV_HNSW_PAGE_ID)
        return false;

    opaque->nextblkno = data_blkno < data_page_count
        ? (BlockNumber)(data_blkno + 1)
        : InvalidBlockNumber;
    return true;
}

static bool
hnsw_init_metapage(Page page, uint32_t dim, uint16_t m, uint16_t ef_construction,
                   BlockNumber entry_blkno, OffsetNumber entry_offno,
                   int16_t entry_level, BlockNumber insert_page)
{
    PgvHnswMeta *metap;

    if (page == NULL || dim == 0 || dim > INT16_MAX || m == 0 ||
        ef_construction == 0 || insert_page == InvalidBlockNumber)
        return false;
    if (entry_blkno == InvalidBlockNumber)
    {
        if (entry_offno != InvalidOffsetNumber || entry_level != -1)
            return false;
    }
    else if (entry_offno == InvalidOffsetNumber || entry_level < 0)
        return false;

    hnsw_init_disk_page(page);
    metap = (PgvHnswMeta *)PageGetContents(page);
    memset(metap, 0, sizeof(*metap));
    metap->magicNumber = PGV_HNSW_MAGIC;
    metap->version = PGV_HNSW_VERSION;
    metap->dimensions = dim;
    metap->m = m;
    metap->efConstruction = ef_construction;
    metap->entryBlkno = entry_blkno;
    metap->entryOffno = entry_offno;
    metap->entryLevel = entry_level;
    metap->insertPage = insert_page;
    ((PageHeader)page)->pd_lower =
        (uint16_t)(((char *)metap + sizeof(PgvHnswMeta)) - (char *)page);
    return true;
}

static bool
hnsw_init_empty_pages(Page metapage, Page first_data_page, uint32_t dim,
                      uint16_t m, uint16_t ef_construction)
{
    if (metapage == NULL || first_data_page == NULL ||
        !hnsw_init_metapage(metapage, dim, m, ef_construction,
                            InvalidBlockNumber, InvalidOffsetNumber, -1, 1))
        return false;
    hnsw_init_disk_page(first_data_page);
    return true;
}

/*
 * Element tuple header (type = 1).
 * The varlena vector follows immediately after the fixed header.
 * Vector varlena layout: int32 vl_len_ | int16 dim | int16 unused | float x[dim]
 */
typedef struct {
    uint8_t         type;           /* 1 */
    uint8_t         level;
    uint8_t         deleted;        /* 0 */
    uint8_t         version;        /* 0 */
    PgvItemPointer  heaptids[PGV_HNSW_MAX_HEAPTIDS]; /* 60 bytes */
    PgvItemPointer  neighbortid;    /* 6 bytes — points to neighbor tuple */
    uint16_t        unused;         /* 0 */
    /* followed by: int32 vl_len_ | int16 dim | int16 unused | float x[dim] */
} PgvHnswElemHdr;   /* 72 bytes */

/* Neighbor tuple header (type = 2) */
typedef struct {
    uint8_t         type;           /* 2 */
    uint8_t         version;        /* 0 */
    uint16_t        count;          /* total slots = (level+2)*m */
    /* followed by: PgvItemPointer indextids[count] */
} PgvHnswNeighHdr;  /* 4 bytes */

/* ----------------------------------------------------------------
 * Page geometry
 * ---------------------------------------------------------------- */
#define BLCKSZ_VAL              8192
#define PG_PAGE_HEADER_SIZE     24      /* SizeOfPageHeaderData */
#define PGV_OPAQUE_SIZE         8       /* sizeof(PgvHnswPageOpaque), MAXALIGN'd */
#define PGV_USABLE_BYTES        (BLCKSZ_VAL - PG_PAGE_HEADER_SIZE - PGV_OPAQUE_SIZE)
/* Each item also uses 4 bytes in the ItemId array at the page head */
#define PGV_ITEM_OVERHEAD       4

/* MAXALIGN to 8 bytes (standard on x86-64 LP64) */
#define MAXALIGN8(n)  (((n) + 7) & ~7)

/* Vector varlena header: 4 bytes vl_len_ + 2 bytes dim + 2 bytes unused */
#define PGV_VEC_HEADER_SIZE     8

/*
 * Size of an element tuple for a vector of `dim` elements, each
 * `bytes_per_dim` wide (4 for pgvector `vector`/fp32, 2 for `halfvec`/fp16).
 * = MAXALIGN(sizeof(PgvHnswElemHdr) + PGV_VEC_HEADER_SIZE + dim*bytes_per_dim)
 */
static size_t
elem_tuple_size(int dim, int bytes_per_dim)
{
    return MAXALIGN8(sizeof(PgvHnswElemHdr) + PGV_VEC_HEADER_SIZE
                     + (size_t)dim * (size_t)bytes_per_dim);
}

/*
 * Size of a neighbor tuple for an element at `level` with graph M.
 * count = (level + 2) * m  (level 0 uses 2m, each upper level uses m)
 * = MAXALIGN(sizeof(PgvHnswNeighHdr) + count * sizeof(PgvItemPointer))
 */
static size_t
neigh_tuple_size(int level, int m)
{
    int count = (level + 2) * m;
    return MAXALIGN8(sizeof(PgvHnswNeighHdr)
                     + (size_t)count * sizeof(PgvItemPointer));
}

/*
 * How many element+neighbor pairs at (dim, bytes_per_dim, m, level) fit on
 * one page (#161). 0 means not even one pair fits. Shared by
 * check_hnsw_page_fit() (below) and the #161 packed-layout pass, which
 * needs the same arithmetic to pick a fixed pairs-per-page count.
 *
 * A negative dim (no typmod, e.g. an unconstrained `vector` column or an
 * expression index whose typmod didn't resolve) would make the size_t
 * arithmetic below wrap around instead of erroring — silently claiming
 * pairs fit that don't. Downstream code (pgvector's own "column does not
 * have dimensions" check) still catches this, but not before this pre-check
 * would have claimed the build is safe.
 */
static int
elems_per_page(int dim, int bytes_per_dim, int m, int level)
{
    if (dim <= 0)
        ereport(ERROR,
                (errmsg("pg_cuvs: target index column has no fixed dimension "
                        "(dim=%d) — cannot size-check the HNSW page layout",
                        dim)));
    size_t esize  = elem_tuple_size(dim, bytes_per_dim);
    size_t nsize  = neigh_tuple_size(level, m);
    size_t needed = esize + 2 * PGV_ITEM_OVERHEAD + nsize + 2 * PGV_ITEM_OVERHEAD;
    return (int)((size_t)PGV_USABLE_BYTES / needed);
}

/*
 * Errors if the element tuple or the neighbor tuple at the worst-case level
 * does not fit on a page by itself (#162/#161) — a failure here means the
 * index cannot be built at all with this dim/type/M. A pair that does not fit
 * one page together is split the way pgvector's own build splits it (
 * hnswbuild.c keeps the element and puts its neighbor tuple first on the next
 * page; hnsw_pack_location_next()).
 */
static void
check_hnsw_page_fit(int dim, int bytes_per_dim, int m, int maxlevel)
{
    if (dim <= 0)
        (void) elems_per_page(dim, bytes_per_dim, m, maxlevel);	/* its "no fixed dimension" ERROR */
    {
        size_t eneed = elem_tuple_size(dim, bytes_per_dim) + 2 * PGV_ITEM_OVERHEAD;
        size_t nneed = neigh_tuple_size(maxlevel, m) + 2 * PGV_ITEM_OVERHEAD;

        if (eneed > (size_t) PGV_USABLE_BYTES || nneed > (size_t) PGV_USABLE_BYTES)
            ereport(ERROR,
                    (errmsg("pg_cuvs: HNSW tuple too large for one page: "
                            "element=%zu, neighbor=%zu, available=%d "
                            "(dim=%d, bytes_per_dim=%d, maxlevel=%d, M=%d)",
                            eneed, nneed, PGV_USABLE_BYTES, dim, bytes_per_dim,
                            maxlevel, m)));
    }
}

/*
 * The one-pair-per-page layout (the CAGRA-source 'hnswlib' / 'hnswlib_file'
 * import, write_elem_page()) has no split pairs: errors unless one
 * element+neighbor pair at the worst-case level fits a page.
 */
static void
check_hnsw_pair_fit(int dim, int bytes_per_dim, int m, int maxlevel)
{
    if (elems_per_page(dim, bytes_per_dim, m, maxlevel) < 1)
    {
        size_t esize  = elem_tuple_size(dim, bytes_per_dim);
        size_t nsize  = neigh_tuple_size(maxlevel, m);
        size_t needed = esize + 2 * PGV_ITEM_OVERHEAD + nsize + 2 * PGV_ITEM_OVERHEAD;
        ereport(ERROR,
                (errmsg("pg_cuvs: element+neighbor tuple pair too large for "
                        "one page: needed=%zu, available=%d "
                        "(dim=%d, bytes_per_dim=%d, maxlevel=%d, M=%d)",
                        needed, PGV_USABLE_BYTES, dim, bytes_per_dim,
                        maxlevel, m)));
    }
}

/*
 * IEEE-754 round-to-nearest-even float32 -> float16 (#162). Matches
 * pgvector halfvec's `half` representation: 1 sign + 5 exponent + 10
 * mantissa bits. Hand-rolled because this is plain-C PGXS backend code (no
 * CUDA __float2half available here — that intrinsic backs the *other* fp16
 * feature in this codebase, cuvs_wrapper.cu's brute-force precision='float16',
 * which is a .cu translation unit; this file is deliberately not one, see
 * ADR-001) and pgvector's own internal conversion routine is not an exported
 * stable C ABI this file may depend on (file header: "We do NOT include
 * pgvector's hnsw.h... All structs are defined locally").
 *
 * Verified bit-exact against a live pgvector 0.8.6 install's own
 * `vector::halfvec` cast across 335 finite in-range values, including an
 * exact round-to-even tie (-1.5229491687270953 -> both this function and
 * pgvector agree on 0xbe18, not the nearer-looking-but-wrong 0xbe17 a naive
 * reference — numpy's float32->float16 cast — produces for that same tie).
 */
static uint16_t
float_to_half(float f)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t  exp  = (int32_t)((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;

    if (((x >> 23) & 0xFF) == 0xFF)              /* Inf/NaN */
        return (uint16_t)(sign | 0x7C00u | (mant ? 0x0200u : 0));
    if (exp >= 0x1F)                              /* overflow -> Inf */
        return (uint16_t)(sign | 0x7C00u);
    if (exp <= 0)                                 /* underflow -> subnormal/zero */
    {
        if (exp < -10) return (uint16_t)sign;     /* too small, flush to zero */
        mant |= 0x800000u;                        /* implicit leading 1 */
        uint32_t shift = (uint32_t)(14 - exp);
        uint32_t half_mant = mant >> shift;
        /* round to nearest even */
        if ((mant >> (shift - 1)) & 1)
            if (((mant & ((1u << (shift - 1)) - 1)) != 0) || (half_mant & 1))
                half_mant++;
        return (uint16_t)(sign | half_mant);
    }
    /* normal range: round mantissa from 23 to 10 bits, RNE */
    uint32_t half_mant = mant >> 13;
    if (mant & 0x1000u)
        if ((mant & 0xFFFu) || (half_mant & 1))
            half_mant++;
    if (half_mant & 0x400u)
    {
        half_mant = 0;
        exp++;
        if (exp >= 0x1F) return (uint16_t)(sign | 0x7C00u);
    }
    return (uint16_t)(sign | ((uint32_t)exp << 10) | half_mant);
}

/*
 * Resolve a target column's element width in bytes from its actual PG type
 * OID (#162): 4 for vector (fp32), 2 for halfvec (fp16). ERRORs on anything
 * else — this file only ever writes pgvector-compatible vector/halfvec page
 * bytes, so a third type reaching here is a caller bug, not a runtime
 * condition to degrade gracefully from.
 *
 * Looks up the type NAME from the OID via a direct syscache hit, rather
 * than resolving vector/halfvec's OID from their names via
 * TypenameGetTypid() (an earlier version of this function did that, cached
 * process-lifetime). TypenameGetTypid() resolves an UNQUALIFIED name
 * through the CALLER'S search_path (namespace.c) — under a restricted
 * search_path (SECURITY DEFINER, a schema-qualified extension install,
 * `SET search_path = ''`) it can return InvalidOid even though pgvector's
 * vector/halfvec types are perfectly well-defined by the OID this function
 * was actually given, turning an ordinary fp32 build into a spurious ERROR.
 * An OID->name lookup has no such dependency.
 */
static int
bytes_per_dim_for_typid(Oid typid)
{
    HeapTuple   tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
    const char *name;

    if (!HeapTupleIsValid(tup))
        ereport(ERROR,
                (errmsg("pg_cuvs: target index column type (typoid=%u) not found",
                        typid)));
    name = NameStr(((Form_pg_type) GETSTRUCT(tup))->typname);
    if (strcmp(name, "halfvec") == 0)
    {
        ReleaseSysCache(tup);
        return 2;
    }
    if (strcmp(name, "vector") == 0)
    {
        ReleaseSysCache(tup);
        return 4;
    }
    ereport(ERROR,
            (errmsg("pg_cuvs: target index column type (typoid=%u, typname=\"%s\") "
                    "is neither vector nor halfvec", typid, name)));
    return 4; /* unreached */
}

/* ----------------------------------------------------------------
 * hnswlib binary header (all fields are size_t / int / double)
 * ---------------------------------------------------------------- */
typedef struct {
    size_t  offsetLevel0;
    size_t  maxElements;
    size_t  curElementCount;
    size_t  sizeDataPerElement;
    size_t  labelOffset;
    size_t  offsetData;
    int     maxlevel;
    int     enterpointNode;
    size_t  maxM;
    size_t  maxM0;
    size_t  M;
    double  mult;
    size_t  efConstruction;
} HnswlibHeader;

_Static_assert(sizeof(HnswlibHeader) == HNSWLIB_HEADER_BYTES,
               "hnswlib binary header layout changed");
_Static_assert(sizeof(CuvsTidsHeader) == 32,
               "CuvsTidsHeader layout changed");

/* One fixed-size, disk-backed record per serialized node.  A location is
 * enough to resolve every neighbor reference; node vectors, links, TIDs and
 * ElemDesc records remain row-local scratch in the source-less importer. */
typedef struct HnswNodeLocation
{
    uint32_t elem_blkno;
    uint16_t elem_offno;
    uint8_t  level;
    uint8_t  reserved;	/* 0, or HNSW_LOCATION_SPLIT */
} HnswNodeLocation;

/* a pair too large for one page keeps the element at (elem_blkno, elem_offno) and puts its neighbor tuple first on the next
 * page, as pgvector's build does (hnswbuild.c: neighborPage = blkno + 1, neighborOffno = FirstOffsetNumber). Otherwise the neighbor
 * tuple follows the element on the same page. */
#define HNSW_LOCATION_SPLIT 1

static inline uint32_t
hnsw_location_neigh_blkno(const HnswNodeLocation *location)
{
    return location->elem_blkno + (location->reserved == HNSW_LOCATION_SPLIT ? 1 : 0);
}

static inline uint16_t
hnsw_location_neigh_offno(const HnswNodeLocation *location)
{
    return location->reserved == HNSW_LOCATION_SPLIT ? 1 : (uint16_t) (location->elem_offno + 1);
}

_Static_assert(sizeof(HnswNodeLocation) == 8,
               "HNSW location records must stay compact");

/* Neighbor links consume only the address; they must never receive a
 * synthesized level from the periodic address fast path. */
typedef struct HnswNodeAddress
{
    uint32_t elem_blkno;
    uint16_t elem_offno;
} HnswNodeAddress;

_Static_assert(sizeof(HnswNodeAddress) <= sizeof(HnswNodeLocation),
               "HNSW address template must fit in one location-table page");

/* The callback owns the two scratch allocations only if it throws. */
typedef void (*HnswInterruptCheck)(int *links, void *window_buffer);
typedef bool (*HnswLocationWriter)(void *arg, uint64_t node,
                                   const HnswNodeLocation *location,
                                   char *error, size_t error_size);

#define HNSW_FILE_IO_CHUNK (64 * 1024)

static bool
hnsw_set_file_error(char *error, size_t error_size, const char *message)
{
    if (error && error_size > 0)
        snprintf(error, error_size, "%s", message);
    return false;
}

static bool
hnsw_set_file_errorf(char *error, size_t error_size,
                     const char *format, uint64_t a, uint64_t b)
{
    if (error && error_size > 0)
        snprintf(error, error_size, format, (unsigned long long)a,
                 (unsigned long long)b);
    return false;
}

static bool
hnsw_add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a > UINT64_MAX - b)
        return false;
    *out = a + b;
    return true;
}

static bool
hnsw_mul_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a != 0 && b > UINT64_MAX / a)
        return false;
    *out = a * b;
    return true;
}

static bool
hnsw_pread_exact(int fd, void *buffer, size_t length, uint64_t offset)
{
    char *p = (char *)buffer;
    size_t done = 0;

    if (offset > (uint64_t)INT64_MAX ||
        length > (uint64_t)INT64_MAX - offset)
        return false;
    while (done < length)
    {
        ssize_t n = pread(fd, p + done, length - done,
                          (off_t)(offset + (uint64_t)done));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        done += (size_t)n;
    }
    return true;
}

/* Bounded read-ahead for the sequential import passes.  Each logical field
 * read keeps hnsw_pread_exact's bounds and result: a window only supplies
 * bytes it already holds completely, and any request it cannot satisfy is
 * read with hnsw_pread_exact itself.  A failed or short window fill is never
 * reported; only the requested range decides success, as before.  A fill
 * that serves fewer than HNSW_READ_WINDOW_MIN_SERVED reads is idle; after two
 * consecutive or eight total idle fills (scattered TID labels) the stream
 * reads directly for the rest of its life, so it never issues more than a
 * few reads beyond the direct path. */
#define HNSW_READ_WINDOW_BYTES (1024 * 1024)
#define HNSW_READ_WINDOW_MIN_SERVED 8
#define HNSW_READ_WINDOW_IDLE_RUN 2
#define HNSW_READ_WINDOW_IDLE_TOTAL 8

typedef struct HnswReadWindow
{
    int fd;
    unsigned char *buffer;      /* NULL: every read is hnsw_pread_exact */
    size_t capacity;
    uint64_t start;
    size_t length;              /* valid bytes at start */
    bool filled;                /* a fill has been attempted at start */
    uint32_t served;            /* reads served by the current fill */
    uint32_t idle_run;          /* consecutive idle fills */
    uint32_t idle_total;        /* idle fills over the window's life */
} HnswReadWindow;

static void
hnsw_window_init(HnswReadWindow *window, int fd, void *buffer, size_t capacity)
{
    window->fd = fd;
    window->buffer = (unsigned char *)buffer;
    window->capacity = buffer ? capacity : 0;
    window->start = 0;
    window->length = 0;
    window->filled = false;
    window->served = 0;
    window->idle_run = 0;
    window->idle_total = 0;
}

static void
hnsw_window_bind(HnswReadWindow *window, int fd)
{
    if (window->fd == fd)
        return;
    window->fd = fd;
    window->start = 0;
    window->length = 0;
    window->filled = false;
    window->served = 0;
    window->idle_run = 0;
    window->idle_total = 0;
}

static bool
hnsw_window_covers(const HnswReadWindow *window, uint64_t offset, size_t length)
{
    return window->filled && offset >= window->start &&
        offset - window->start <= window->length &&
        length <= window->length - (size_t)(offset - window->start);
}

/* Read up to capacity bytes from offset.  Stops at EOF or a read error;
 * retries EINTR and continues after short positive reads. */
static void
hnsw_window_fill(HnswReadWindow *window, uint64_t offset)
{
    size_t want = window->capacity;

    if (window->filled)
    {
        if (window->served < HNSW_READ_WINDOW_MIN_SERVED)
        {
            window->idle_run++;
            window->idle_total++;
        }
        else
            window->idle_run = 0;
        if (window->idle_run >= HNSW_READ_WINDOW_IDLE_RUN ||
            window->idle_total >= HNSW_READ_WINDOW_IDLE_TOTAL)
        {
            window->buffer = NULL;      /* direct reads from now on */
            window->capacity = 0;
            window->filled = false;
            window->length = 0;
            return;
        }
    }
    window->start = offset;
    window->length = 0;
    window->filled = true;
    window->served = 0;
    if (offset > (uint64_t)INT64_MAX)
        return;
    if ((uint64_t)want > (uint64_t)INT64_MAX - offset)
        want = (size_t)((uint64_t)INT64_MAX - offset);
    while (window->length < want)
    {
        ssize_t n = pread(window->fd, window->buffer + window->length,
                          want - window->length,
                          (off_t)(offset + (uint64_t)window->length));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        window->length += (size_t)n;
    }
}

/* Make [offset, offset + length) resident when it fits; never fails. */
static void
hnsw_window_reserve(HnswReadWindow *window, uint64_t offset, uint64_t length)
{
    if (window->buffer == NULL || length > window->capacity ||
        offset > (uint64_t)INT64_MAX ||
        length > (uint64_t)INT64_MAX - offset ||
        hnsw_window_covers(window, offset, (size_t)length))
        return;
    hnsw_window_fill(window, offset);
}

static bool
hnsw_window_read(HnswReadWindow *window, void *buffer, size_t length,
                 uint64_t offset)
{
    if (offset > (uint64_t)INT64_MAX ||
        length > (uint64_t)INT64_MAX - offset)
        return false;
    if (length == 0)
        return true;
    if (window->buffer != NULL && length <= window->capacity)
    {
        if (!hnsw_window_covers(window, offset, length))
            hnsw_window_fill(window, offset);
        if (window->buffer != NULL &&
            hnsw_window_covers(window, offset, length))
        {
            memcpy(buffer, window->buffer + (offset - window->start), length);
            window->served++;
            return true;
        }
    }
    return hnsw_pread_exact(window->fd, buffer, length, offset);
}

static bool
hnsw_tid_is_valid(uint64_t tid)
{
    uint32_t block;
    uint16_t offset;

    cuvs_tid_decode(tid, &block, &offset);
    return block != InvalidBlockNumber && offset > InvalidOffsetNumber &&
        offset <= MaxOffsetNumber;
}

static bool
hnsw_read_tid_window(HnswReadWindow *tids, uint64_t n_vecs, uint64_t label,
                     uint64_t *tid_out, char *error, size_t error_size)
{
    uint64_t body_offset;
    uint64_t file_offset;
    uint64_t tid;

    if (label >= n_vecs)
        return hnsw_set_file_error(error, error_size,
                                   "HNSW label is outside the TID row count");
    if (!hnsw_mul_u64(label, sizeof(uint64_t), &body_offset) ||
        !hnsw_add_u64(sizeof(CuvsTidsHeader), body_offset, &file_offset) ||
        !hnsw_window_read(tids, &tid, sizeof(tid), file_offset))
        return hnsw_set_file_error(error, error_size,
                                   "cannot read TID for HNSW label");
    if (!hnsw_tid_is_valid(tid))
        return hnsw_set_file_error(error, error_size,
                                   "TID file contains an invalid heap TID");
    *tid_out = tid;
    return true;
}

pg_attribute_unused()
static bool
hnsw_read_tid_for_label(int tids_fd, uint64_t n_vecs, uint64_t label,
                        uint64_t *tid_out, char *error, size_t error_size)
{
    HnswReadWindow direct;

    hnsw_window_init(&direct, tids_fd, NULL, 0);
    return hnsw_read_tid_window(&direct, n_vecs, label, tid_out,
                                error, error_size);
}

static bool
hnsw_layout_location(uint64_t node, uint64_t n_nodes, uint8_t level,
                     bool packed_nsw, uint32_t pairs_per_page,
                     HnswNodeLocation *location)
{
    uint64_t block;
    uint64_t slot;

    if (node >= n_nodes)
        return false;
    if (packed_nsw)
    {
        if (pairs_per_page == 0 || level != 0)
            return false;
        block = 1 + node / pairs_per_page;
        slot = node % pairs_per_page;
        location->elem_offno = (uint16_t)(1 + 2 * slot);
    }
    else
    {
        block = 1 + node;
        location->elem_offno = 1;
    }
    if (block >= (uint64_t)InvalidBlockNumber ||
        location->elem_offno == InvalidOffsetNumber ||
        location->elem_offno + 1 > MaxOffsetNumber)
        return false;
    location->elem_blkno = (uint32_t)block;
    location->level = level;
    location->reserved = 0;
    return true;
}

static bool
hnsw_compute_pack_k(int dim, int bytes_per_dim, int m, uint32_t *k_out)
{
    size_t esize;
    size_t nsize;
    size_t needed;

    if (dim <= 0 || (bytes_per_dim != 2 && bytes_per_dim != 4) || m <= 0)
        return false;
    esize = elem_tuple_size(dim, bytes_per_dim);
    nsize = neigh_tuple_size(0, m);
    needed = esize + nsize + 4 * PGV_ITEM_OVERHEAD;
    if (needed == 0 || needed > PGV_USABLE_BYTES)
        return false;
    *k_out = (uint32_t)(PGV_USABLE_BYTES / needed);
    return *k_out > 0;
}

/* Each tuple fits a page by itself; a pair too large for one page is split (HNSW_LOCATION_SPLIT). */
static bool
hnsw_page_pair_fits(int dim, int bytes_per_dim, int m, int level)
{
    if (dim <= 0 || (bytes_per_dim != 2 && bytes_per_dim != 4) ||
        m <= 0 || level < 0)
        return false;
    return elem_tuple_size(dim, bytes_per_dim) + 2 * PGV_ITEM_OVERHEAD <= PGV_USABLE_BYTES &&
        neigh_tuple_size(level, m) + 2 * PGV_ITEM_OVERHEAD <= PGV_USABLE_BYTES;
}

/* A bounded cursor assigns hierarchical rows in their serialized order.
 * The reservation matches the existing page-fit check, including its
 * conservative item-pointer margin. Node/edge order never changes. */
typedef struct HnswPackingCursor
{
    uint64_t next_node;
    uint32_t block;
    uint16_t next_offset;
    size_t remaining;
} HnswPackingCursor;

static bool
hnsw_pack_location_next(HnswPackingCursor *cursor,
                        uint64_t node, uint64_t n_nodes, int level,
                        int dim, int bytes_per_dim, int m,
                        HnswNodeLocation *location)
{
    size_t esize;
    size_t nsize;
    size_t needed;
    bool split;

    if (!cursor || !location || node != cursor->next_node || node >= n_nodes ||
        level < 0 || level > UINT8_MAX ||
        !hnsw_page_pair_fits(dim, bytes_per_dim, m, level))
        return false;
    esize = elem_tuple_size(dim, bytes_per_dim) + 2 * PGV_ITEM_OVERHEAD;
    nsize = neigh_tuple_size(level, m) + 2 * PGV_ITEM_OVERHEAD;
    /* pgvector's build rule (hnswbuild.c): keep the pair on one page if it fits one page, else the element here and its
     * neighbor tuple first on the next page. */
    split = esize + nsize > PGV_USABLE_BYTES;
    needed = split ? esize : esize + nsize;
    if (cursor->block == 0 || cursor->remaining < needed)
    {
        if (cursor->block >= InvalidBlockNumber - 1)
            return false;
        cursor->block++;
        cursor->next_offset = 1;
        cursor->remaining = PGV_USABLE_BYTES;
    }
    if (cursor->next_offset == InvalidOffsetNumber ||
        cursor->next_offset + 1 > MaxOffsetNumber)
        return false;
    location->elem_blkno = cursor->block;
    location->elem_offno = cursor->next_offset;
    location->level = (uint8_t)level;
    location->reserved = split ? HNSW_LOCATION_SPLIT : 0;
    if (split)
    {
        if (cursor->block >= InvalidBlockNumber - 1)
            return false;
        cursor->block++;
        cursor->next_offset = 2;
        cursor->remaining = PGV_USABLE_BYTES - nsize;
    }
    else
    {
        cursor->remaining -= needed;
        cursor->next_offset += 2;
    }
    cursor->next_node++;
    return true;
}

static bool
hnsw_file_preflight(int hnsw_fd, int tids_fd,
                    uint64_t expected_n_vecs,
                    uint32_t expected_dim, uint32_t expected_metric,
                    int bytes_per_dim, bool packed_nsw, uint16_t heap_m,
                    HnswlibHeader *hnsw_out,
                    CuvsTidsHeader *tids_out,
                    HnswLocationWriter location_writer,
                    void *location_writer_arg,
                    HnswInterruptCheck check_interrupts,
                    char *error, size_t error_size)
{
    struct stat hs;
    struct stat ts;
    HnswlibHeader h;
    CuvsTidsHeader tids_hdr;
    uint64_t level0_bytes;
    uint64_t level0_end;
    uint64_t expected_tids_body;
    uint64_t expected_tids_size;
    uint64_t file_cursor;
    uint64_t observed_max_level = 0;
    uint64_t entrypoint_level = UINT64_MAX;
    uint64_t per_level_bytes;
    uint32_t pairs_per_page = 0;
    HnswPackingCursor packing = {0};
    unsigned char crc_buffer[HNSW_FILE_IO_CHUNK];
    uint32_t crc_state;
    uint64_t tids_remaining;
    uint64_t tids_offset;
    int *links = NULL;
    void *window_buffer = NULL;
    HnswReadWindow window;
    bool ok = false;

    if (fstat(hnsw_fd, &hs) != 0 || fstat(tids_fd, &ts) != 0)
        return hnsw_set_file_error(error, error_size,
                                   "cannot stat HNSW or TID input file");
    if (!S_ISREG(hs.st_mode) || !S_ISREG(ts.st_mode) ||
        hs.st_size < 0 || ts.st_size < 0)
        return hnsw_set_file_error(error, error_size,
                                   "HNSW and TID inputs must be regular files");
    if ((uint64_t)hs.st_size > (uint64_t)INT64_MAX ||
        (uint64_t)ts.st_size > (uint64_t)INT64_MAX)
        return hnsw_set_file_error(error, error_size,
                                   "HNSW or TID input exceeds supported file size");
    if (expected_n_vecs == 0 || expected_n_vecs > CUVS_TIDS_MAX_VECS ||
        expected_dim == 0 || expected_dim > INT16_MAX || expected_metric > CUVS_METRIC_IP)
        return hnsw_set_file_error(error, error_size,
                                   "invalid expected HNSW build dimensions or count");
    if (sizeof(size_t) != 8 || sizeof(int) != 4 || sizeof(double) != 8)
        return hnsw_set_file_error(error, error_size,
                                   "unsupported hnswlib host ABI");
    {
        const uint16_t endian_probe = 1;
        if (*(const uint8_t *)&endian_probe != 1)
            return hnsw_set_file_error(error, error_size,
                                       "hnswlib and TID inputs require little-endian host ABI");
    }
    if ((uint64_t)hs.st_size < HNSWLIB_HEADER_BYTES ||
        !hnsw_pread_exact(hnsw_fd, &h, sizeof(h), 0))
        return hnsw_set_file_error(error, error_size,
                                   "short read on hnswlib header");
    if ((uint64_t)ts.st_size < sizeof(tids_hdr) ||
        !hnsw_pread_exact(tids_fd, &tids_hdr, sizeof(tids_hdr), 0))
        return hnsw_set_file_error(error, error_size,
                                   "short read on TID header");
    if (tids_hdr.magic != CUVS_TIDS_MAGIC ||
        tids_hdr.version != CUVS_TIDS_VERSION || tids_hdr.reserved != 0)
        return hnsw_set_file_error(error, error_size,
                                   "unsupported or malformed TID file version");
    if (tids_hdr.n_vecs <= 0 ||
        (uint64_t)tids_hdr.n_vecs != expected_n_vecs ||
        tids_hdr.dim != expected_dim || tids_hdr.metric != expected_metric)
        return hnsw_set_file_error(error, error_size,
                                   "TID header count, dimension or metric mismatch");
    if (!hnsw_mul_u64(expected_n_vecs, sizeof(uint64_t), &expected_tids_body) ||
        !hnsw_add_u64(sizeof(tids_hdr), expected_tids_body, &expected_tids_size) ||
        expected_tids_size != (uint64_t)ts.st_size)
        return hnsw_set_file_error(error, error_size,
                                   "TID file size does not match its row count");

    crc_state = cuvs_crc32_stream_begin();
    tids_remaining = expected_tids_body;
    tids_offset = sizeof(tids_hdr);
    while (tids_remaining > 0)
    {
        size_t chunk = tids_remaining > sizeof(crc_buffer)
            ? sizeof(crc_buffer) : (size_t)tids_remaining;
        uint64_t *chunk_tids = (uint64_t *)crc_buffer;

        if (check_interrupts)
            check_interrupts(links, window_buffer);
        if (!hnsw_pread_exact(tids_fd, crc_buffer, chunk, tids_offset))
        {
            hnsw_set_file_error(error, error_size,
                                "short read on TID body");
            goto done;
        }
        crc_state = cuvs_crc32_stream_update(crc_state, crc_buffer, chunk);
        for (size_t off = 0; off < chunk / sizeof(uint64_t); off++)
        {
            if (!hnsw_tid_is_valid(chunk_tids[off]))
            {
                hnsw_set_file_error(error, error_size,
                                    "TID file contains an invalid heap TID");
                goto done;
            }
        }
        tids_remaining -= chunk;
        tids_offset += chunk;
    }
    if (cuvs_crc32_stream_end(crc_state) != tids_hdr.body_crc32)
    {
        hnsw_set_file_error(error, error_size, "TID body checksum mismatch");
        goto done;
    }

    if (h.curElementCount != expected_n_vecs || h.maxElements < h.curElementCount ||
        h.sizeDataPerElement == 0 || h.maxlevel < 0 || h.maxlevel > UINT8_MAX ||
        h.enterpointNode < 0 || (uint64_t)h.enterpointNode >= expected_n_vecs ||
        h.M == 0 || h.M > INT_MAX || h.maxM != h.M ||
        h.M > SIZE_MAX / 2 || h.maxM0 != 2 * h.M || h.maxM0 > INT_MAX ||
        h.efConstruction == 0 || h.efConstruction > UINT16_MAX ||
        !isfinite(h.mult) || h.mult <= 0.0)
    {
        hnsw_set_file_error(error, error_size, "invalid hnswlib header fields");
        goto done;
    }
    /* Decode the actual graph width and reserve the heap index's full
     * configured neighbor capacity in the PostgreSQL layout. */
    int layout_m = heap_m ? heap_m : (int)h.M;
    int layout_maxlevel = packed_nsw ? 0 : h.maxlevel;

    if ((size_t)layout_m < h.M)
    {
        hnsw_set_file_error(error, error_size,
                            "HNSW graph width exceeds the requested heap M");
        goto done;
    }
    if ((uint64_t)(layout_maxlevel + 2) * layout_m > UINT16_MAX)
    {
        hnsw_set_file_error(error, error_size,
                            "HNSW neighbor slot count exceeds the page format");
        goto done;
    }
    if (!hnsw_page_pair_fits((int)expected_dim, bytes_per_dim,
                             layout_m, layout_maxlevel))
    {
        hnsw_set_file_error(error, error_size,
                            "HNSW element and neighbor tuples do not fit on a page");
        goto done;
    }
    if (packed_nsw && h.maxlevel > 1)
    {
        hnsw_set_file_error(error, error_size,
                            "NSW header maximum level exceeds the serializer allowance");
        goto done;
    }
    if (packed_nsw && !hnsw_compute_pack_k((int)expected_dim, bytes_per_dim,
                                          layout_m, &pairs_per_page))
    {
        hnsw_set_file_error(error, error_size,
                            "NSW element and neighbor tuples do not fit on a page");
        goto done;
    }
    if (!hnsw_mul_u64(expected_n_vecs, h.sizeDataPerElement, &level0_bytes) ||
        !hnsw_add_u64(HNSWLIB_HEADER_BYTES, level0_bytes, &level0_end) ||
        level0_end > (uint64_t)hs.st_size)
    {
        hnsw_set_file_error(error, error_size,
                            "level-0 element count or file offset overflows input size");
        goto done;
    }

    {
        uint64_t links_bytes;
        uint64_t links_end;
        uint64_t vector_bytes;
        uint64_t vector_end;
        uint64_t label_end;

        if (!hnsw_mul_u64(h.maxM0, sizeof(int), &links_bytes) ||
            !hnsw_add_u64(links_bytes, sizeof(int), &links_bytes) ||
            !hnsw_add_u64(h.offsetLevel0, links_bytes, &links_end) ||
            !hnsw_mul_u64(expected_dim, sizeof(float), &vector_bytes) ||
            !hnsw_add_u64(h.offsetData, vector_bytes, &vector_end) ||
            !hnsw_add_u64(h.labelOffset, sizeof(size_t), &label_end) ||
            links_end > h.sizeDataPerElement || vector_end > h.sizeDataPerElement ||
            label_end > h.sizeDataPerElement ||
            (h.offsetLevel0 < vector_end && h.offsetData < links_end) ||
            (h.offsetLevel0 < label_end && h.labelOffset < links_end) ||
            (h.offsetData < label_end && h.labelOffset < vector_end))
        {
            hnsw_set_file_error(error, error_size,
                                "hnswlib element offsets overlap or exceed record size");
            goto done;
        }
    }

    links = (int *)malloc((size_t)h.maxM0 * sizeof(int));
    if (!links)
    {
        hnsw_set_file_error(error, error_size,
                            "cannot allocate bounded HNSW link scratch");
        goto done;
    }
    /* Without the optional read-ahead buffer every read stays direct. */
    window_buffer = malloc(HNSW_READ_WINDOW_BYTES);
    hnsw_window_init(&window, hnsw_fd, window_buffer, HNSW_READ_WINDOW_BYTES);
    for (uint64_t node = 0; node < expected_n_vecs; node++)
    {
        uint64_t row_offset;
        uint64_t base;
        uint64_t label_offset;
        int count;
        size_t label;

        if (check_interrupts)
            check_interrupts(links, window_buffer);
        if (hnsw_mul_u64(node, h.sizeDataPerElement, &row_offset) &&
            hnsw_add_u64(HNSWLIB_HEADER_BYTES, row_offset, &base))
            hnsw_window_reserve(&window, base, h.sizeDataPerElement);
        if (!hnsw_mul_u64(node, h.sizeDataPerElement, &row_offset) ||
            !hnsw_add_u64(HNSWLIB_HEADER_BYTES, row_offset, &base) ||
            !hnsw_add_u64(base, h.offsetLevel0, &row_offset) ||
            !hnsw_window_read(&window, &count, sizeof(count), row_offset) ||
            count < 0 || (uint64_t)count > h.maxM0)
        {
            hnsw_set_file_errorf(error, error_size,
                                 "invalid level-0 link count at node %llu (capacity %llu)",
                                 node, h.maxM0);
            goto done;
        }
        if (!hnsw_add_u64(row_offset, sizeof(count), &row_offset) ||
            !hnsw_window_read(&window, links, (size_t)count * sizeof(int), row_offset))
        {
            hnsw_set_file_error(error, error_size,
                                "short read on level-0 links");
            goto done;
        }
        for (int link = 0; link < count; link++)
        {
            if (links[link] < 0 || (uint64_t)links[link] >= expected_n_vecs)
            {
                hnsw_set_file_error(error, error_size,
                                    "level-0 link is outside the HNSW node count");
                goto done;
            }
        }
        if (!hnsw_add_u64(base, h.labelOffset, &label_offset) ||
            !hnsw_window_read(&window, &label, sizeof(label), label_offset) ||
            (uint64_t)label >= expected_n_vecs)
        {
            hnsw_set_file_error(error, error_size,
                                "HNSW label is outside the original row count");
            goto done;
        }
    }

    per_level_bytes = (h.M + 1) * sizeof(int);
    file_cursor = level0_end;
    hnsw_window_init(&window, hnsw_fd, window_buffer, HNSW_READ_WINDOW_BYTES);
    for (uint64_t node = 0; node < expected_n_vecs; node++)
    {
        uint32_t list_bytes;
        uint64_t links_start;
        uint64_t row_end;
        uint64_t level;
        uint64_t location_node = node;
        HnswNodeLocation location;

        if (check_interrupts)
            check_interrupts(links, window_buffer);
        if (file_cursor > (uint64_t)hs.st_size ||
            (uint64_t)hs.st_size - file_cursor < sizeof(list_bytes) ||
            !hnsw_window_read(&window, &list_bytes, sizeof(list_bytes), file_cursor))
        {
            hnsw_set_file_error(error, error_size,
                                "short read on upper-level link size");
            goto done;
        }
        if (!hnsw_add_u64(file_cursor, sizeof(list_bytes), &links_start) ||
            !hnsw_add_u64(links_start, list_bytes, &row_end) ||
            row_end > (uint64_t)hs.st_size ||
            (list_bytes != 0 &&
             (per_level_bytes == 0 || list_bytes % per_level_bytes != 0)))
        {
            hnsw_set_file_error(error, error_size,
                                "upper-level link length is malformed or out of bounds");
            goto done;
        }
        level = list_bytes == 0 ? 0 : list_bytes / per_level_bytes;
        if (level > (uint64_t)h.maxlevel || level > UINT8_MAX)
        {
            hnsw_set_file_error(error, error_size,
                                "node level exceeds the HNSW header maximum");
            goto done;
        }
        for (uint64_t lv = 0; lv < level; lv++)
        {
            int upper_count;
            uint64_t level_offset;

            if (!hnsw_mul_u64(lv, per_level_bytes, &level_offset) ||
                !hnsw_add_u64(links_start, level_offset, &level_offset) ||
                !hnsw_window_read(&window, &upper_count, sizeof(upper_count),
                                  level_offset) ||
                upper_count < 0 || (uint64_t)upper_count > h.M)
            {
                hnsw_set_file_error(error, error_size,
                                    "invalid upper-level neighbor count");
                goto done;
            }
            if (!hnsw_add_u64(level_offset, sizeof(upper_count), &level_offset) ||
                !hnsw_window_read(&window, links,
                                  (size_t)upper_count * sizeof(int), level_offset))
            {
                hnsw_set_file_error(error, error_size,
                                    "short read on upper-level links");
                goto done;
            }
            for (int link = 0; link < upper_count; link++)
            {
                if (links[link] < 0 || (uint64_t)links[link] >= expected_n_vecs)
                {
                    hnsw_set_file_error(error, error_size,
                                        "upper-level link is outside the HNSW node count");
                    goto done;
                }
            }
        }
        if (level > observed_max_level)
            observed_max_level = level;
        if (node == (uint64_t)h.enterpointNode)
            entrypoint_level = level;
        if (!(packed_nsw
              ? hnsw_layout_location(node, expected_n_vecs, (uint8_t)level,
                                      true, pairs_per_page, &location)
              : hnsw_pack_location_next(&packing, node, expected_n_vecs,
                                         (int)level, (int)expected_dim,
                                         bytes_per_dim, layout_m, &location)))
        {
            hnsw_set_file_error(error, error_size,
                                "HNSW node layout exceeds PostgreSQL page limits");
            goto done;
        }
        if (location_writer &&
            !location_writer(location_writer_arg, location_node, &location,
                             error, error_size))
            goto done;
        file_cursor = row_end;
    }
    if (file_cursor != (uint64_t)hs.st_size)
    {
        hnsw_set_file_error(error, error_size,
                            "hnswlib input has trailing bytes after upper-level data");
        goto done;
    }
    if ((packed_nsw &&
         (observed_max_level != 0 || entrypoint_level != 0)) ||
        (!packed_nsw &&
         (observed_max_level != (uint64_t)h.maxlevel ||
          entrypoint_level != observed_max_level)))
    {
        hnsw_set_file_error(error, error_size,
                            "HNSW row levels and entrypoint metadata disagree");
        goto done;
    }
    if (hnsw_out)
        *hnsw_out = h;
    if (tids_out)
        *tids_out = tids_hdr;
    ok = true;

done:
    free(window_buffer);
    free(links);
    return ok;
}

/* ----------------------------------------------------------------
 * In-memory element descriptor (filled during parse phase)
 * ---------------------------------------------------------------- */
typedef struct {
    int             level;          /* max level for this element */
    uint64_t        tid_encoded;    /* heap TID from .tids sidecar */
    /* layer-0 links: link_count + links[maxM0] */
    int             lv0_count;
    int            *lv0_links;      /* malloc'd; size maxM0 */
    /* upper-level links: for levels 1..level */
    /* upper_links[l-1] is an array of int[link_count_l], length upper_counts[l-1] */
    int           **upper_links;    /* malloc'd outer array; NULL if level==0 */
    int            *upper_counts;
    /* assigned page layout (filled in pass 2) */
    uint32_t        elem_blkno;
    uint16_t        elem_offno;
    uint32_t        neigh_blkno;
    uint16_t        neigh_offno;
} ElemDesc;

typedef bool (*HnswNodeAddressLookup)(void *arg, uint32_t node,
                                      HnswNodeAddress *address);

static int hnsw_connectivity_cancel(void *arg);

static const unsigned char *
hnsw_element_links(void *ctx,uint32_t node,uint32_t level,uint32_t *count)
{
    ElemDesc *e=&((ElemDesc*)ctx)[node];
    if(level>(uint32_t)e->level){*count=UINT32_MAX;return NULL;}
    *count=level ? e->upper_counts[level-1] : e->lv0_count;
    return (const unsigned char*)(level ? e->upper_links[level-1] : e->lv0_links);
}
static CuvsConnectivity
hnsw_check_elements(ElemDesc *elems,size_t n,int entry)
{
    CuvsConnectivity c={0};
    struct timespec start,end;
    uint32_t maxlevel=0;
    void *workspace;size_t bytes;bool spilled;
    if(!cuvs_hnsw_connectivity_check){cuvs_connectivity_set_disabled(&c);return c;}
    CHECK_FOR_INTERRUPTS();
    c.magic=CUVS_CONNECTIVITY_MAGIC;c.version=1;c.nodes=n;
    if(n==0){c.available=CUVS_CONNECTIVITY_MEASURED;return c;}
    if(n>UINT32_MAX || entry<0 || (size_t)entry>=n || n>SIZE_MAX/17)return c;
    bytes=n*17;workspace=hnsw_connectivity_workspace(bytes,&spilled);
    if(!workspace){CHECK_FOR_INTERRUPTS();return c;}
    clock_gettime(CLOCK_MONOTONIC,&start);
    PG_TRY();
    {
        for(size_t i=0;i<n;i++) {
            if((i&4095)==0)CHECK_FOR_INTERRUPTS();
            if((uint32_t)elems[i].level>maxlevel)maxlevel=elems[i].level;
        }
        (void)cuvs_connectivity_scratch((uint32_t)n,(uint32_t)entry,elems[entry].level,maxlevel,
            hnsw_element_links,elems,hnsw_connectivity_cancel,workspace,&c);
        c.spilled=spilled;
    }
    PG_FINALLY();
    {
        hnsw_connectivity_workspace_free(workspace,bytes,spilled);
    }
    PG_END_TRY();
    CHECK_FOR_INTERRUPTS();
    clock_gettime(CLOCK_MONOTONIC,&end);
    c.elapsed_us=(uint64_t)(end.tv_sec-start.tv_sec)*1000000+(end.tv_nsec-start.tv_nsec)/1000;
    return c;
}

static bool
hnsw_lookup_elem_array(void *arg, uint32_t node, HnswNodeAddress *address)
{
    const ElemDesc *elems = (const ElemDesc *)arg;

    address->elem_blkno = elems[node].elem_blkno;
    address->elem_offno = elems[node].elem_offno;
    return true;
}

/* ----------------------------------------------------------------
 * Helper: set a PgvItemPointer from block + offset
 * ---------------------------------------------------------------- */
static void
set_item_ptr(PgvItemPointer *ip, uint32_t blkno, uint16_t offno)
{
    ip->ip_blkid.bi_hi = (uint16_t)(blkno >> 16);
    ip->ip_blkid.bi_lo = (uint16_t)(blkno & 0xFFFF);
    ip->ip_posid       = offno;
}

static void
set_invalid_item_ptr(PgvItemPointer *ip)
{
    ip->ip_blkid.bi_hi = 0xFFFF;
    ip->ip_blkid.bi_lo = 0xFFFF;
    ip->ip_posid       = 0;
}

/* ----------------------------------------------------------------
 * Helper: decode heap TID from .tids encoding into a PgvItemPointer.
 * cuvs_tid_encode: block<<16 | offset
 * ---------------------------------------------------------------- */
static void
tid_to_item_ptr(uint64_t encoded, PgvItemPointer *ip)
{
    uint32_t block;
    uint16_t offset;
    cuvs_tid_decode(encoded, &block, &offset);
    set_item_ptr(ip, block, offset);
}

/* ----------------------------------------------------------------
 * Free all memory owned by an ElemDesc array
 * ---------------------------------------------------------------- */
static void
free_elem_descs(ElemDesc *elems, size_t n, size_t maxM0)
{
    for (size_t i = 0; i < n; i++)
    {
        if (elems[i].lv0_links)
            free(elems[i].lv0_links);
        if (elems[i].upper_links)
        {
            for (int l = 0; l < elems[i].level; l++)
                if (elems[i].upper_links[l])
                    free(elems[i].upper_links[l]);
            free(elems[i].upper_links);
        }
        if (elems[i].upper_counts)
            free(elems[i].upper_counts);
    }
    (void)maxM0;
}

/* ----------------------------------------------------------------
 * Raw (non-palloc) resources held by the fill_hnsw_from_* bodies.
 *
 * Every PG throw site inside those bodies — write_elem_page's PageAddItem
 * checks, index_open, RelationTruncate, ReadBuffer, ... — longjmps past
 * plain malloc/fopen cleanup.  The bodies therefore register each buffer
 * here as soon as they own it and never free on the error path; the
 * PG_TRY/PG_FINALLY wrappers below run hnsw_fill_res_free() on both the
 * success and the error path.
 * ---------------------------------------------------------------- */
typedef enum HnswSourceTidResult
{
    HNSW_SOURCE_TIDS_UNIQUE,
    HNSW_SOURCE_TIDS_REDUNDANT,
    HNSW_SOURCE_TIDS_CONFLICTING
} HnswSourceTidResult;

typedef struct HnswFillRes
{
    FILE     *hf;           /* .hnsw sidecar */
    uint64_t *tids;         /* .tids payload / IPC tids */
    char     *lv0_block;    /* hnswlib level-0 block */
    char     *llbuf;        /* hnswlib per-element upper-link block */
    ElemDesc *elems;
    size_t    n_elems;      /* elems entries to release (calloc'd, so NULL-safe) */
    size_t    elems_maxM0;
    uint32_t *adj;          /* IPC CAGRA adjacency */
    float    *vecs;         /* IPC corpus vectors */
    int      *cand_buf;     /* heuristic candidate scratch */
    int       shm_fd;       /* #165: SCM_RIGHTS sidecar fd (-1 = none) */
    int       tids_fd;      /* EXPORT_HNSW_UPPER TID memfd (-1 = none) */
    HnswSourceTidResult tid_result;
} HnswFillRes;

/*
 * #166 review: read the hnswlib sidecar through the descriptor the daemon
 * passed, never by re-opening a path. The segment's name is already unlinked,
 * and a "/proc/self/fd/N" stand-in re-checks inode permissions — which fails
 * EACCES cross-uid as soon as the daemon's umask leaves the file 0600. Each
 * caller gets its own dup so the two read passes hold independent offsets and
 * fclose() stays symmetric; the original fd is owned by HnswFillRes.
 */
static FILE *
hnsw_open_stream(const char *path, int passed_fd)
{
    int   dupfd;
    FILE *f;

    if (passed_fd < 0)
        return fopen(path, "rb");      /* index_dir sidecar (use_shm = false) */

    dupfd = dup(passed_fd);
    if (dupfd < 0)
        return NULL;
    f = fdopen(dupfd, "rb");
    if (!f)
    {
        close(dupfd);
        return NULL;
    }
    rewind(f);
    return f;
}

static void
hnsw_fill_res_free(HnswFillRes *r)
{
    if (r->hf)        { fclose(r->hf);      r->hf = NULL; }
    if (r->shm_fd >= 0) { close(r->shm_fd); r->shm_fd = -1; }
    if (r->tids_fd >= 0) { close(r->tids_fd); r->tids_fd = -1; }
    if (r->tids)      { free(r->tids);      r->tids = NULL; }
    if (r->lv0_block) { free(r->lv0_block); r->lv0_block = NULL; }
    if (r->llbuf)     { free(r->llbuf);     r->llbuf = NULL; }
    if (r->cand_buf)  { free(r->cand_buf);  r->cand_buf = NULL; }
    if (r->adj)       { free(r->adj);       r->adj = NULL; }
    if (r->vecs)      { free(r->vecs);      r->vecs = NULL; }
    if (r->elems)
    {
        free_elem_descs(r->elems, r->n_elems, r->elems_maxM0);
        free(r->elems);
        r->elems = NULL;
    }
}

/* ----------------------------------------------------------------
 * Write one element+neighbor tuple pair onto an ALREADY-OPEN page (#161:
 * extracted from write_elem_page() so a caller can pack multiple pairs
 * onto one page — buffer lifecycle (ReadBuffer/PageInit/dirty/WAL/release)
 * is the caller's responsibility, not this function's). e->elem_offno /
 * e->neigh_offno (set by the layout pass) determine where on THIS page
 * the tuples land; this function does not choose or validate offsets
 * itself, it only asserts PageAddItem returns them.
 * ---------------------------------------------------------------- */
static void
write_elem_pair(Page page,
                Page npage,
                ElemDesc *e,
                const float *vec,
                int dim,
                int bytes_per_dim,
                int m,
                size_t n_elems,
                HnswNodeAddressLookup location_lookup,
                void *location_lookup_arg)
{
    /* elem tuple */
    size_t esize = elem_tuple_size(dim, bytes_per_dim);
    size_t nsize = neigh_tuple_size(e->level, m);

    PgvHnswElemHdr *etup = (PgvHnswElemHdr *) palloc0(esize);
    etup->type    = 1;
    etup->level   = (uint8_t) e->level;
    etup->deleted = 0;
    etup->version = 0;

    /* fill heaptids[0] with the heap TID; rest are invalid */
    tid_to_item_ptr(e->tid_encoded, &etup->heaptids[0]);
    for (int t = 1; t < PGV_HNSW_MAX_HEAPTIDS; t++)
        set_invalid_item_ptr(&etup->heaptids[t]);

    /* neighbortid points to the neighbor tuple on the same page, at
     * whatever offno the layout pass assigned (e->neigh_offno — always 2
     * for one-pair-per-page, but a packed page's Nth pair lands at a
     * higher offno; see the layout pass in fill_hnsw_from_cagra_ipc_impl). */
    set_item_ptr(&etup->neighbortid, e->neigh_blkno, e->neigh_offno);
    etup->unused = 0;

    /* write the vector varlena immediately after the element header.
     * pgvector uses SET_VARSIZE (standard 4-byte varlena): vl_len_ = size << 2.
     * `vec` is always the fp32 buffer read from the .hnsw sidecar / IPC
     * adjacency (#162: GPU CAGRA build stays fp32 throughout — cuVS has no
     * fp16 CAGRA path, ADR-054). The narrow-cast to halfvec happens ONLY
     * here, at the final page-write boundary, never upstream. */
    char *vdata = (char *)etup + sizeof(PgvHnswElemHdr);
    int32_t vl_len = (int32_t)(PGV_VEC_HEADER_SIZE + (size_t)dim * (size_t)bytes_per_dim);
    int32_t vl_len_field = vl_len << 2;
    memcpy(vdata, &vl_len_field, 4);
    int16_t vdim   = (int16_t)dim;
    int16_t vunused = 0;
    memcpy(vdata + 4, &vdim,    2);
    memcpy(vdata + 6, &vunused, 2);
    if (bytes_per_dim == 4)
    {
        memcpy(vdata + 8, vec, (size_t)dim * sizeof(float));
    }
    else /* bytes_per_dim == 2: halfvec */
    {
        uint16_t *hbuf = (uint16_t *) (vdata + 8);
        for (int i = 0; i < dim; i++)
            hbuf[i] = float_to_half(vec[i]);
    }

    /* neighbor tuple */
    int count = (e->level + 2) * m;
    PgvHnswNeighHdr *ntup = (PgvHnswNeighHdr *) palloc0(nsize);
    ntup->type    = 2;
    ntup->version = 0;
    ntup->count   = (uint16_t)count;

    PgvItemPointer *slots = (PgvItemPointer *)((char *)ntup + sizeof(PgvHnswNeighHdr));

    /*
     * Slots are stored DESCENDING by level (highest level first):
     *   slots[0..m-1]:      level e->level neighbors
     *   slots[m..2m-1]:     level e->level-1 neighbors
     *   ...
     *   slots[(level+1)*m .. (level+2)*m-1]:  level 0 neighbors (2m slots)
     *
     * Note: level 0 always has 2m slots (HnswGetLayerM(m,0)=2m), but all
     * other levels have m slots each.  The total = m*(level) + 2m = (level+2)*m.
     */
    int slot = 0;

    /* highest level first, down to level 1 (each m slots) */
    for (int lv = e->level; lv >= 1; lv--)
    {
        int lv_idx = lv - 1;   /* upper_links index (0 = level 1) */
        int lv_count = (lv_idx < e->level && e->upper_links)
                       ? e->upper_counts[lv_idx] : 0;
        for (int s = 0; s < m; s++, slot++)
        {
            if (s < lv_count && e->upper_links && e->upper_links[lv_idx])
            {
                int nb = e->upper_links[lv_idx][s];
                HnswNodeAddress location;
                if (nb >= 0 && (size_t)nb < n_elems &&
                    location_lookup(location_lookup_arg, (uint32_t)nb, &location))
                    set_item_ptr(&slots[slot], location.elem_blkno,
                                 location.elem_offno);
                else
                    set_invalid_item_ptr(&slots[slot]);
            }
            else
                set_invalid_item_ptr(&slots[slot]);
        }
    }

    /* level 0 — 2m slots */
    for (int s = 0; s < 2 * m; s++, slot++)
    {
        if (s < e->lv0_count && e->lv0_links)
        {
            int nb = e->lv0_links[s];
            HnswNodeAddress location;
            if (nb >= 0 && (size_t)nb < n_elems &&
                location_lookup(location_lookup_arg, (uint32_t)nb, &location))
                set_item_ptr(&slots[slot], location.elem_blkno,
                             location.elem_offno);
            else
                set_invalid_item_ptr(&slots[slot]);
        }
        else
            set_invalid_item_ptr(&slots[slot]);
    }

    /* Add element tuple. InvalidOffsetNumber lets PG choose the next free
     * slot on THIS page — for a single-pair page that is always offno 1,
     * for a packed page it is whatever slot this pair falls into. Assert
     * it matches what the layout pass already assigned to e->elem_offno
     * (set_item_ptr()-encoded neighbor references elsewhere depend on that
     * assignment being correct, not on what PageAddItem happens to return). */
    OffsetNumber eoff = PageAddItem(page, (Item)etup, esize, InvalidOffsetNumber,
                                    false, false);
    if (eoff == InvalidOffsetNumber)
        ereport(ERROR,
                (errmsg("pg_cuvs: hnsw_export: element tuple does not fit on page "
                        "(esize=%zu, free=%zu)", esize,
                        (size_t)PageGetFreeSpace(page))));
    if (eoff != (OffsetNumber) e->elem_offno)
        ereport(ERROR,
                (errmsg("pg_cuvs: hnsw_export: internal error — element landed at "
                        "offno %u, layout pass assigned %u",
                        eoff, e->elem_offno)));

    /* Add neighbor tuple, same assertion. npage is page unless the layout split the pair (then the next page, at offno 1). */
    OffsetNumber noff = PageAddItem(npage, (Item)ntup, nsize, InvalidOffsetNumber,
                                    false, false);
    if (noff == InvalidOffsetNumber)
        ereport(ERROR,
                (errmsg("pg_cuvs: hnsw_export: neighbor tuple does not fit on page "
                        "(nsize=%zu, free=%zu)", nsize,
                        (size_t)PageGetFreeSpace(npage))));
    if (noff != (OffsetNumber) e->neigh_offno)
        ereport(ERROR,
                (errmsg("pg_cuvs: hnsw_export: internal error — neighbor tuple "
                        "landed at offno %u, layout pass assigned %u",
                        noff, e->neigh_offno)));

    pfree(etup);
    pfree(ntup);
}

/* ----------------------------------------------------------------
 * Write a single page holding exactly one element+neighbor pair — the
 * layout used by every caller except #161's packed nsw write loop
 * (write_elem_pages_packed() below). Thin wrapper: owns the buffer
 * lifecycle, delegates the tuple construction to write_elem_pair().
 * ---------------------------------------------------------------- */
static void hnsw_publish_staged_page(Relation rel, Page staged, bool skip_wal);

static void
write_elem_page(Relation rel,
                uint32_t blkno,
                ElemDesc *e,
                const float *vec,
                int dim,
                int bytes_per_dim,
                int m,
                size_t n_elems,
                bool skip_wal,
                HnswNodeAddressLookup location_lookup,
                void *location_lookup_arg)
{
    Page                page = (Page)palloc(BLCKSZ);
    PgvHnswPageOpaque  *opaque;

    /* Validate/allocate tuples in private memory.  Only publication touches
     * shared buffers, in the common helper's critical section. */
    PageInit(page, BLCKSZ, sizeof(PgvHnswPageOpaque));

    opaque = (PgvHnswPageOpaque *) PageGetSpecialPointer(page);
    opaque->nextblkno = InvalidBlockNumber;
    opaque->unused    = 0;
    opaque->page_id   = PGV_HNSW_PAGE_ID;
    if (!hnsw_set_disk_page_chain(page, blkno, n_elems))
        ereport(ERROR,
                (errmsg("pg_cuvs: invalid HNSW data page chain position")));

    write_elem_pair(page, page, e, vec, dim, bytes_per_dim, m, n_elems,
                    location_lookup, location_lookup_arg);

    hnsw_publish_staged_page(rel, page, skip_wal);
    pfree(page);
}

/* #161: writes N element+neighbor pairs onto the pages the layout pass assigned: elems[i].elem_blkno must be ascending
 * from 1 with no gap, elems[i].elem_offno/neigh_offno the slots in that page in order, and a split pair's neighbor tuple
 * (neigh_blkno = elem_blkno + 1, neigh_offno 1) first on the next page (the layout pass's job, hnsw_pack_location_next()).
 * This function fills each page in element order and asserts (via write_elem_pair()) that each tuple lands where the
 * layout said. */
static void
hnsw_init_source_data_page(Page page, uint32_t blkno, uint32_t data_page_count)
{
    PgvHnswPageOpaque *opaque;

    PageInit(page, BLCKSZ, sizeof(PgvHnswPageOpaque));
    opaque = (PgvHnswPageOpaque *) PageGetSpecialPointer(page);
    opaque->nextblkno = InvalidBlockNumber;
    opaque->unused    = 0;
    opaque->page_id   = PGV_HNSW_PAGE_ID;
    if (!hnsw_set_disk_page_chain(page, (uint64_t) blkno, data_page_count))
        ereport(ERROR,
                (errmsg("pg_cuvs: invalid packed HNSW data page chain position")));
}

static void
write_elem_pages_packed(Relation rel,
                        ElemDesc *elems,
                        const float *vecs,
                        int dim,
                        int bytes_per_dim,
                        int m,
                        size_t n_elems,
                        bool skip_wal,
                        HnswNodeAddressLookup location_lookup,
                        void *location_lookup_arg)
{
    uint32_t data_page_count;
    Page cur;
    Page next;
    uint32_t cur_blkno = 0;
    bool next_started = false;

    if (n_elems == 0)
        return;
    data_page_count = elems[n_elems - 1].neigh_blkno;
    if (elems[0].elem_blkno != 1 || data_page_count == 0 ||
        data_page_count >= InvalidBlockNumber)
        ereport(ERROR,
                (errmsg("pg_cuvs: invalid packed HNSW data page count")));

    cur = (Page)palloc(BLCKSZ);
    next = (Page)palloc(BLCKSZ);
    for (size_t i = 0; i < n_elems; i++)
    {
        ElemDesc *e = &elems[i];
        const float *vec = vecs + i * (size_t) dim;

        if (e->elem_blkno != cur_blkno)
        {
            if (cur_blkno != 0)
                hnsw_publish_staged_page(rel, cur, skip_wal);
            if (next_started)
            {
                /* the page holding the last split pair's neighbor tuple; the element may start on it or on the page after */
                Page t = cur;

                cur = next;
                next = t;
                next_started = false;
                cur_blkno++;
                if (e->elem_blkno != cur_blkno)
                    hnsw_publish_staged_page(rel, cur, skip_wal);
            }
            if (e->elem_blkno != cur_blkno)
            {
                if (e->elem_blkno != cur_blkno + 1)
                    ereport(ERROR,
                            (errmsg("pg_cuvs: packed HNSW layout skips a page after block %u", cur_blkno)));
                hnsw_init_source_data_page(cur, e->elem_blkno, data_page_count);
                cur_blkno = e->elem_blkno;
            }
        }
        if (e->neigh_blkno == cur_blkno)
            write_elem_pair(cur, cur, e, vec, dim, bytes_per_dim, m,
                            n_elems, location_lookup, location_lookup_arg);
        else
        {
            if (e->neigh_blkno != cur_blkno + 1 || e->neigh_offno != 1 || next_started)
                ereport(ERROR,
                        (errmsg("pg_cuvs: packed HNSW layout places a neighbor tuple off the next page's first slot")));
            hnsw_init_source_data_page(next, cur_blkno + 1, data_page_count);
            next_started = true;
            write_elem_pair(cur, next, e, vec, dim, bytes_per_dim, m,
                            n_elems, location_lookup, location_lookup_arg);
        }
    }
    hnsw_publish_staged_page(rel, cur, skip_wal);
    if (next_started)
        hnsw_publish_staged_page(rel, next, skip_wal);
    pfree(cur);
    pfree(next);
}

/* pg_cuvs writes pgvector on-disk HNSW_VERSION=1 pages, tested with pgvector
 * 0.5.x–0.8.x. Existing import callers retain warning-only behavior; the new
 * source-less converter rejects versions outside that range before touching
 * target pages. */
static bool
hnsw_pgvector_version_supported(const char *version)
{
    int major = -1;
    int minor = -1;

    return version && sscanf(version, "%d.%d", &major, &minor) == 2 &&
        major == 0 && minor >= 5 && minor <= 8;
}

static bool
cuvs_warn_pgvector_version(void)
{
    Oid          ext_oid = get_extension_oid("vector", true /* missing_ok */);
    Relation     rel;
    SysScanDesc  scan;
    ScanKeyData  key;
    HeapTuple    tup;

    bool supported = false;

    if (!OidIsValid(ext_oid))
        return false;   /* absence is handled (ERROR) by the callers */

    rel = table_open(ExtensionRelationId, AccessShareLock);
    ScanKeyInit(&key, Anum_pg_extension_oid, BTEqualStrategyNumber,
                F_OIDEQ, ObjectIdGetDatum(ext_oid));
    scan = systable_beginscan(rel, ExtensionOidIndexId, true, NULL, 1, &key);
    tup = systable_getnext(scan);
    if (HeapTupleIsValid(tup))
    {
        bool  isnull;
        Datum d = heap_getattr(tup, Anum_pg_extension_extversion,
                               RelationGetDescr(rel), &isnull);
        if (!isnull)
        {
            char *ver = text_to_cstring(DatumGetTextPP(d));

            /* known-good: pgvector 0.5.x – 0.8.x (HNSW_VERSION=1). */
            if (hnsw_pgvector_version_supported(ver))
                supported = true;
            else
                ereport(WARNING,
                        (errmsg("pg_cuvs: pgvector %s is outside the tested range "
                                "(0.5.x–0.8.x) for HNSW page export", ver),
                         errhint("pg_cuvs writes pgvector HNSW_VERSION=1 on-disk pages; "
                                 "a pgvector on-disk format change could make exported "
                                 "indexes unreadable. Verify compatibility before "
                                 "relying on this build.")));
            pfree(ver);
        }
    }
    systable_endscan(scan);
    table_close(rel, AccessShareLock);
    return supported;
}

/* ================================================================
 * Main SQL function
 * ================================================================ */
/* A zero request is internal only: the source decides M when m was omitted. */
static void
hnsw_report_source_m(int requested_m, int actual_m)
{
    if (requested_m != 0 && requested_m != actual_m)
        ereport(WARNING,
                (errmsg("pg_cuvs_hnsw: source graph uses M = %d, ignoring m = %d",
                        actual_m, requested_m)));
}

typedef struct HnswTidMember
{
    uint64_t tid;
    uint32_t label;
} HnswTidMember;

static int
hnsw_compare_tid_member(const void *a, const void *b)
{
    uint64_t x = ((const HnswTidMember *) a)->tid;
    uint64_t y = ((const HnswTidMember *) b)->tid;

    return (x > y) - (x < y);
}

/* Keep original graph labels in the sorted copy. The common heap-order
 * source needs neither sorting nor vector reads. */
static HnswTidMember *
hnsw_duplicate_members(const uint64_t *tids, size_t n)
{
    HnswTidMember *members;
    size_t i;

    if (n < 2)
        return NULL;
    for (i = 1; i < n && tids[i - 1] < tids[i]; i++)
        ;
    if (i == n)
        return NULL;
    if (n > CUVS_TIDS_MAX_VECS)
        ereport(ERROR, (errmsg("pg_cuvs: exported CAGRA TID count is out of range")));
    members = palloc_extended(n * sizeof(*members), MCXT_ALLOC_HUGE);
    for (i = 0; i < n; i++)
    {
        members[i].tid = tids[i];
        members[i].label = (uint32_t) i;
    }
    qsort(members, n, sizeof(*members), hnsw_compare_tid_member);
    for (i = 1; i < n && members[i - 1].tid != members[i].tid; i++)
        ;
    if (i < n)
        return members;
    pfree(members);
    return NULL;
}

static bool
hnsw_source_vectors_equal(const float *a, const float *b, uint32_t dim)
{
    if (memcmp(a, b, (size_t) dim * sizeof(*a)) == 0)
        return true;
    for (uint32_t i = 0; i < dim; i++)
        if (a[i] != b[i])
            return false;
    return true;
}

static HnswSourceTidResult
hnsw_array_tid_result(const uint64_t *tids, const float *vecs, size_t n, uint32_t dim)
{
    HnswTidMember *members = hnsw_duplicate_members(tids, n);
    HnswSourceTidResult result = HNSW_SOURCE_TIDS_REDUNDANT;

    if (members == NULL)
        return HNSW_SOURCE_TIDS_UNIQUE;
    for (size_t i = 1; i < n; i++)
        if (members[i - 1].tid == members[i].tid &&
            !hnsw_source_vectors_equal(vecs + (size_t) members[i - 1].label * dim,
                                       vecs + (size_t) members[i].label * dim, dim))
        {
            result = HNSW_SOURCE_TIDS_CONFLICTING;
            break;
        }
    pfree(members);
    return result;
}

static void
hnsw_pg_check_interrupts(int *links, void *window_buffer)
{
    if (!INTERRUPTS_PENDING_CONDITION())
        return;

    PG_TRY();
    {
        CHECK_FOR_INTERRUPTS();
    }
    PG_CATCH();
    {
        free(window_buffer);
        free(links);
        PG_RE_THROW();
    }
    PG_END_TRY();
}

static HnswSourceTidResult
hnsw_file_tid_result(int graph_fd, int tids_fd, const uint64_t *tids,
                     const CuvsTidsHeader *tids_header, int bytes_per_dim)
{
    size_t n = (size_t) tids_header->n_vecs;
    uint32_t dim = tids_header->dim;
    HnswTidMember *members = hnsw_duplicate_members(tids, n);
    HnswlibHeader header;
    char error[160];
    HnswReadWindow window;
    void *window_buffer;
    uint32_t *row_for_label;
    float *a, *b;
    size_t vector_bytes;
    HnswSourceTidResult result = HNSW_SOURCE_TIDS_REDUNDANT;

    if (members == NULL)
        return HNSW_SOURCE_TIDS_UNIQUE;
    if (!hnsw_file_preflight(graph_fd, tids_fd, n, dim, tids_header->metric,
                             bytes_per_dim, false, 0, &header, NULL, NULL, NULL,
                             hnsw_pg_check_interrupts, error, sizeof(error)))
        ereport(ERROR, (errmsg("pg_cuvs: cannot validate duplicate source entries: %s", error)));

    /* Level-0 rows need not be in label order. Validate the label mapping
     * and only compare vectors in duplicate TID groups. */
    row_for_label = palloc_extended(n * sizeof(*row_for_label), MCXT_ALLOC_HUGE);
    memset(row_for_label, 0xff, n * sizeof(*row_for_label));
    window_buffer = palloc(HNSW_READ_WINDOW_BYTES);
    hnsw_window_init(&window, graph_fd, window_buffer, HNSW_READ_WINDOW_BYTES);
    for (size_t node = 0; node < n; node++)
    {
        uint64_t offset = HNSWLIB_HEADER_BYTES +
                          (uint64_t) node * header.sizeDataPerElement + header.labelOffset;
        size_t label;

        CHECK_FOR_INTERRUPTS();
        if (!hnsw_window_read(&window, &label, sizeof(label), offset) ||
            label >= n || row_for_label[label] != UINT32_MAX)
            ereport(ERROR, (errmsg("pg_cuvs: invalid source graph label mapping")));
        row_for_label[label] = (uint32_t) node;
    }

    vector_bytes = (size_t) dim * sizeof(float);
    a = palloc(vector_bytes);
    b = palloc(vector_bytes);
    for (size_t i = 1; i < n; i++)
    {
        if (members[i - 1].tid != members[i].tid)
            continue;
        CHECK_FOR_INTERRUPTS();
        uint64_t offset_a = HNSWLIB_HEADER_BYTES + header.offsetData +
            (uint64_t) row_for_label[members[i - 1].label] * header.sizeDataPerElement;
        uint64_t offset_b = HNSWLIB_HEADER_BYTES + header.offsetData +
            (uint64_t) row_for_label[members[i].label] * header.sizeDataPerElement;
        if (!hnsw_pread_exact(graph_fd, a, vector_bytes, offset_a) ||
            !hnsw_pread_exact(graph_fd, b, vector_bytes, offset_b))
            ereport(ERROR, (errmsg("pg_cuvs: cannot read duplicate source vectors")));
        if (!hnsw_source_vectors_equal(a, b, dim))
        {
            result = HNSW_SOURCE_TIDS_CONFLICTING;
            break;
        }
    }
    pfree(a);
    pfree(b);
    pfree(window_buffer);
    pfree(row_for_label);
    pfree(members);
    return result;
}

static void
hnsw_check_exported_tids(HnswFillRes *res, int bytes_per_dim)
{
    FILE *file = hnsw_open_stream("exported CAGRA TIDs", res->tids_fd);
    CuvsTidsHeader header;
    int rc;

    if (file == NULL)
        ereport(ERROR, (errmsg("pg_cuvs: cannot open exported CAGRA TIDs: %m")));
    rc = cuvs_tids_read(file, &header, &res->tids);
    fclose(file);
    if (rc != 0)
        ereport(ERROR, (errmsg("pg_cuvs: invalid exported CAGRA TIDs")));
    res->tid_result = hnsw_file_tid_result(res->shm_fd, res->tids_fd, res->tids,
                                          &header, bytes_per_dim);
    free(res->tids);
    res->tids = NULL;
}

static void
hnsw_reject_conflicting_source_tids(void)
{
    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("pg_cuvs: source CAGRA index has duplicate heap TIDs with different vectors"),
             errhint("REINDEX the source CAGRA index before converting it, or build the HNSW index from the heap.")));
}

static bool
hnsw_source_binding_cancel_pending(void)
{
    return QueryCancelPending || ProcDiePending;
}

static bool
hnsw_source_binding_matches(const char *graph_path, FILE *graph, FILE *tids)
{
    char path[1056];
    CuvsHnswSourceBinding recorded;
    FILE *file;
    bool found;

    snprintf(path, sizeof(path), "%s%s", graph_path, CUVS_HNSW_SOURCE_SUFFIX);
    file = AllocateFile(path, PG_BINARY_R);
    if (file == NULL)
        return false;
    found = cuvs_hnsw_source_binding_read(file, fileno(graph), fileno(tids),
                                         &recorded, hnsw_source_binding_cancel_pending);
    FreeFile(file);
    CHECK_FOR_INTERRUPTS();
    return found;
}

/* Internal helper: fill an existing HNSW index from hnswlib sidecar.
 * Called by pg_cuvs_build_hnsw() for modes 'hnswlib' and 'hnswlib_file'.
 * use_shm=true  → daemon runs from_cagra() → /dev/shm (no disk I/O)
 * use_shm=false → reads pre-built .hnsw sidecar from index_dir */
static void
fill_hnsw_from_hnswlib_impl(Oid cagra_oid, Oid hnsw_oid, bool use_shm,
                           int source_m, uint16_t metapage_ef, HnswFillRes *res)
{

    /* ---- 0. pgvector compatibility check ---- */
    /* pg_cuvs_import_hnsw is pinned to pgvector HNSW_VERSION=1 (stable since
     * pgvector 0.5.0, Aug 2023). Warn if pgvector is not installed; error if
     * the target index's metapage format would be unreadable. */
    {
        Oid vec_oid = get_extension_oid("vector", true /* missing_ok */);
        if (!OidIsValid(vec_oid))
            ereport(ERROR,
                    (errmsg("pg_cuvs: pgvector extension is not installed; "
                            "pg_cuvs_build_hnsw requires pgvector 0.5.0+"),
                     errhint("Run: CREATE EXTENSION vector;")));
        cuvs_warn_pgvector_version();   /* Wave 1: version-range guard */
    }

    /* ---- 1. Open CAGRA source index to get db_oid, index_oid, dim ---- */
    Relation cagra_rel = index_open(cagra_oid, AccessShareLock);

    uint32_t db_oid    = (uint32_t)MyDatabaseId;
    uint32_t index_oid = (uint32_t)cagra_oid;
    const char *idir   = pstrdup(cuvs_resolve_index_dir_rel(cagra_rel));

    /* Infer dim from the index tuple descriptor (atttypmod on the first attr). */
    int dim = 0;
    if (RelationGetDescr(cagra_rel)->natts >= 1)
        dim = (int)TupleDescAttr(RelationGetDescr(cagra_rel), 0)->atttypmod;

    if (dim <= 0)
        ereport(ERROR,
                (errmsg("pg_cuvs: cannot determine vector dimension from cagra "
                        "index %u; atttypmod=%d", cagra_oid, dim)));

    index_close(cagra_rel, AccessShareLock);

    /* ---- 1b. Validate target index before opening it ---- */
    /* Look up via syscache to avoid opening a non-HNSW index first. */
    bool hnsw_unlogged = false;  /* set below; true = skip WAL on page writes */
    int  bytes_per_dim = 4;      /* #162: set from the target's actual type below */
    {
        HeapTuple tup = SearchSysCache1(RELOID, ObjectIdGetDatum(hnsw_oid));
        if (!HeapTupleIsValid(tup))
            ereport(ERROR,
                    (errmsg("pg_cuvs: target relation with OID %u not found",
                            hnsw_oid)));

        Form_pg_class cf = (Form_pg_class) GETSTRUCT(tup);

        /* Must be an index */
        if (cf->relkind != RELKIND_INDEX)
        {
            char *name = pstrdup(NameStr(cf->relname));
            ReleaseSysCache(tup);
            ereport(ERROR,
                    (errmsg("pg_cuvs: \"%s\" is not an index", name)));
        }

        /* Must use the 'hnsw' or 'pg_cuvs_hnsw' access method. The latter is
         * the Phase 3K AM whose ambuild fills its own relation in pgvector
         * page format, so it is a valid conversion target as well. */
        {
            Oid hnsw_amoid      = get_am_oid("hnsw", true);
            Oid cuvs_hnsw_amoid = get_am_oid("pg_cuvs_hnsw", true);
            if (cf->relam != hnsw_amoid && cf->relam != cuvs_hnsw_amoid)
            {
                char *name = pstrdup(NameStr(cf->relname));
                ReleaseSysCache(tup);
                ereport(ERROR,
                        (errmsg("pg_cuvs: index \"%s\" is not a pgvector HNSW index; "
                                "pg_cuvs_build_hnsw requires USING hnsw",
                                name)));
            }
        }

        /* Dimension must match the source CAGRA index */
        {
            int tgt_dim = 0;
            AttrNumber  natts = cf->relnatts;
            if (natts >= 1)
            {
                HeapTuple attup = SearchSysCache2(ATTNUM,
                                                   ObjectIdGetDatum(hnsw_oid),
                                                   Int16GetDatum(1));
                if (HeapTupleIsValid(attup))
                {
                    Form_pg_attribute att = (Form_pg_attribute) GETSTRUCT(attup);
                    tgt_dim = (int)att->atttypmod;
                    /* #162: must read atttypid before ReleaseSysCache — the
                     * tuple (and att, which points into it) is invalid after. */
                    bytes_per_dim = bytes_per_dim_for_typid(att->atttypid);
                    ReleaseSysCache(attup);
                }
            }
            if (tgt_dim > 0 && tgt_dim != dim)
            {
                char *name = pstrdup(NameStr(cf->relname));
                ReleaseSysCache(tup);
                ereport(ERROR,
                        (errmsg("pg_cuvs: dimension mismatch: source CAGRA index has "
                                "dim=%d but target HNSW index \"%s\" has dim=%d",
                                dim, name, tgt_dim)));
            }
        }

        /* Detect UNLOGGED target: WAL will be skipped for faster import.
         * UNLOGGED indexes are not crash-safe; the caller is responsible
         * for rebuilding after crash recovery. */
        hnsw_unlogged = (cf->relpersistence == RELPERSISTENCE_UNLOGGED);

        ReleaseSysCache(tup);
    }

    if (hnsw_unlogged)
        ereport(NOTICE,
                (errmsg("pg_cuvs: target HNSW index is UNLOGGED — "
                        "WAL skipped for faster import; "
                        "index must be rebuilt after crash recovery")));

    /* ---- 2. Build sidecar paths ---- */
    char hnsw_path[1024];
    char tids_path[1024];

    if (use_shm)
    {
        /* Request daemon to run from_cagra() → /dev/shm (no disk I/O) */
        if (!cuvs_socket_path || cuvs_socket_path[0] == '\0')
            ereport(ERROR, (errmsg("pg_cuvs: daemon socket not set")));
        int shm_rc = cuvs_ipc_export_hnsw_shm(
            cuvs_socket_path, db_oid, index_oid, hnsw_path, sizeof(hnsw_path),
            &res->shm_fd, &res->tids_fd);
        if (shm_rc != CUVS_STATUS_OK)
        {
            cuvs_report_proto_mismatch(shm_rc);
            ereport(ERROR,
                    (errmsg("pg_cuvs: export_hnsw_shm failed (status=%d); "
                            "ensure cagra index %u is loaded in daemon", shm_rc, cagra_oid)));
        }
        snprintf(tids_path, sizeof(tids_path), "exported CAGRA TIDs");
    }
    else
    {
        snprintf(hnsw_path, sizeof(hnsw_path), "%s/%u_%u.hnsw", idir, db_oid, index_oid);
        snprintf(tids_path, sizeof(tids_path), "%s/%u_%u.tids", idir, db_oid, index_oid);
    }

    /* ---- 3. Parse hnswlib binary header ---- */
    FILE *hf = hnsw_open_stream(hnsw_path, res->shm_fd);
    if (!hf)
        ereport(ERROR,
                (errmsg("pg_cuvs: cannot open .hnsw sidecar \"%s\": %m",
                        hnsw_path)));
    res->hf = hf;

    /* Read the matching TID file before parsing the graph. */
    FILE *tf = use_shm ? hnsw_open_stream(tids_path, res->tids_fd)
                      : AllocateFile(tids_path, PG_BINARY_R);
    if (!tf)
    {
        ereport(ERROR,
                (errmsg("pg_cuvs: cannot open .tids sidecar \"%s\": %m",
                        tids_path)));
    }

    if (!use_shm && !hnsw_source_binding_matches(hnsw_path, hf, tf))
    {
        FreeFile(tf);
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pg_cuvs: hnswlib_file sidecar has no matching CAGRA generation binding"),
                 errhint("Use mode 'nsw' to convert the current CAGRA graph, or REINDEX the source with cuvs.cpu_hnsw_fallback enabled. "
                         "If CREATE INDEX CONCURRENTLY left an invalid target index, drop that index before retrying.")));
    }

    CuvsTidsHeader tids_hdr;
    uint64_t *tids = NULL;

    PG_TRY();
    {
        if (cuvs_tids_read(tf, &tids_hdr, &tids) != 0)
            ereport(ERROR,
                    (errmsg("pg_cuvs: failed to read/validate .tids sidecar \"%s\"",
                            tids_path)));
        res->tids = tids;
        res->tid_result = hnsw_file_tid_result(fileno(hf), fileno(tf), tids,
                                               &tids_hdr, bytes_per_dim);
    }
    PG_FINALLY();
    {
        if (use_shm)
            fclose(tf);
        else
            FreeFile(tf);
    }
    PG_END_TRY();
    if (res->tid_result != HNSW_SOURCE_TIDS_UNIQUE)
        return;

    HnswlibHeader hdr;
    memset(&hdr, 0, sizeof(hdr));

    if (fread(&hdr.offsetLevel0,       sizeof(size_t), 1, hf) != 1 ||
        fread(&hdr.maxElements,        sizeof(size_t), 1, hf) != 1 ||
        fread(&hdr.curElementCount,    sizeof(size_t), 1, hf) != 1 ||
        fread(&hdr.sizeDataPerElement, sizeof(size_t), 1, hf) != 1 ||
        fread(&hdr.labelOffset,        sizeof(size_t), 1, hf) != 1 ||
        fread(&hdr.offsetData,         sizeof(size_t), 1, hf) != 1 ||
        fread(&hdr.maxlevel,           sizeof(int),    1, hf) != 1 ||
        fread(&hdr.enterpointNode,     sizeof(int),    1, hf) != 1 ||
        fread(&hdr.maxM,               sizeof(size_t), 1, hf) != 1 ||
        fread(&hdr.maxM0,              sizeof(size_t), 1, hf) != 1 ||
        fread(&hdr.M,                  sizeof(size_t), 1, hf) != 1 ||
        fread(&hdr.mult,               sizeof(double), 1, hf) != 1 ||
        fread(&hdr.efConstruction,     sizeof(size_t), 1, hf) != 1)
    {
        ereport(ERROR,
                (errmsg("pg_cuvs: short read on .hnsw header \"%s\"",
                        hnsw_path)));
    }

    size_t N  = hdr.curElementCount;
    int    M  = (int)hdr.M;
    int    M0 = (int)hdr.maxM0;   /* should == 2*M */

    if (N == 0)
    {
        ereport(ERROR, (errmsg("pg_cuvs: .hnsw sidecar is empty (N=0)")));
    }
    if (M <= 0 || M0 <= 0)
    {
        ereport(ERROR, (errmsg("pg_cuvs: invalid M=%d M0=%d in .hnsw header",
                               M, M0)));
    }

    hnsw_report_source_m(source_m, M);

    if ((size_t)tids_hdr.n_vecs != N)
    {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pg_cuvs: .tids has %lld entries but .hnsw has %zu elements",
                        (long long)tids_hdr.n_vecs, N),
                 errhint("Build the HNSW index from the heap, or use mode 'nsw' to convert the resident CAGRA graph.")));
    }

    /* ---- 4b. Metric (opclass) compatibility check ---- */
    /* Map the target HNSW's opclass name to a CUVS_METRIC_* value and verify
     * it matches the source CAGRA's metric stored in the .tids header.
     * Importing L2 CAGRA into cosine/IP HNSW would return nonsense distances. */
    {
        HeapTuple   idxtup;
        Datum       indclassDatum;
        bool        isnull;
        uint32_t    tgt_metric = CUVS_METRIC_L2; /* default if lookup fails */

        idxtup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(hnsw_oid));
        if (HeapTupleIsValid(idxtup))
        {
            indclassDatum = SysCacheGetAttr(INDEXRELID, idxtup,
                                             Anum_pg_index_indclass, &isnull);
            if (!isnull)
            {
                oidvector *indclass = (oidvector *) DatumGetPointer(indclassDatum);
                Oid        opclassOid = indclass->values[0];
                HeapTuple  opctup = SearchSysCache1(CLAOID,
                                                     ObjectIdGetDatum(opclassOid));
                if (HeapTupleIsValid(opctup))
                {
                    Form_pg_opclass opcf = (Form_pg_opclass) GETSTRUCT(opctup);
                    const char *nm = NameStr(opcf->opcname);
                    if (strstr(nm, "cosine"))    tgt_metric = CUVS_METRIC_COSINE;
                    else if (strstr(nm, "_ip"))  tgt_metric = CUVS_METRIC_IP;
                    ReleaseSysCache(opctup);
                }
            }
            ReleaseSysCache(idxtup);
        }

        if (tgt_metric != (uint32_t)tids_hdr.metric)
        {
            static const char *mname[] = { "l2", "cosine", "ip" };
            const char *src_m = (tids_hdr.metric < 3) ? mname[tids_hdr.metric] : "unknown";
            const char *tgt_m = (tgt_metric < 3) ? mname[tgt_metric] : "unknown";
            ereport(ERROR,
                    (errmsg("pg_cuvs: metric mismatch: source CAGRA uses %s "
                            "but target HNSW opclass uses %s",
                            src_m, tgt_m)));
        }
    }

    /* ---- 5. Read level-0 block into memory ---- */
    /*
     * After reading the 13 header fields the file pointer is at position 96
     * (the header byte count). The level-0 DATA block starts immediately after
     * the header; hdr.offsetLevel0 is an INTRA-ELEMENT offset (where the link
     * array sits within each sizeDataPerElement slot), not a file offset.
     *
     * No seek needed here: file is already positioned at byte 96 = start of
     * the level-0 block. (An explicit seek with SEEK_SET would rewind to 0.)
     */

    size_t lv0_block_bytes = N * hdr.sizeDataPerElement;
    char  *lv0_block = (char *) malloc(lv0_block_bytes);
    if (!lv0_block)
    {
        ereport(ERROR, (errmsg("pg_cuvs: out of memory for level-0 block "
                               "(%zu bytes)", lv0_block_bytes)));
    }
    res->lv0_block = lv0_block;

    if (fread(lv0_block, 1, lv0_block_bytes, hf) != lv0_block_bytes)
    {
        ereport(ERROR,
                (errmsg("pg_cuvs: short read on level-0 block in \"%s\"",
                        hnsw_path)));
    }

    /* ---- 6. Parse element descriptors ---- */
    ElemDesc *elems = (ElemDesc *) calloc(N, sizeof(ElemDesc));
    if (!elems)
    {
        ereport(ERROR, (errmsg("pg_cuvs: out of memory for element descriptors")));
    }
    res->elems       = elems;
    res->n_elems     = N;
    res->elems_maxM0 = (size_t)M0;

    for (size_t i = 0; i < N; i++)
    {
        char *base = lv0_block + i * hdr.sizeDataPerElement;

        /* level-0 links: [int link_count][int links[M0]] at offsetLevel0 within element */
        char *lv0ptr = base + hdr.offsetLevel0;
        int lv0_count;
        memcpy(&lv0_count, lv0ptr, sizeof(int));
        if (lv0_count < 0 || lv0_count > M0)
            lv0_count = 0;

        elems[i].lv0_count = lv0_count;
        elems[i].lv0_links = (int *) malloc((size_t)M0 * sizeof(int));
        if (!elems[i].lv0_links)
        {
            ereport(ERROR, (errmsg("pg_cuvs: out of memory for lv0 links")));
        }
        memcpy(elems[i].lv0_links, lv0ptr + sizeof(int),
               (size_t)M0 * sizeof(int));

        /* label (item_id) at labelOffset — used to look up heap TID.
         * An out-of-range label has no heap TID; falling back to 0 would write
         * an invalid ItemPointer (posid=0), and pgvector's heaptids loop stops
         * at the first invalid entry — the element would silently return
         * nothing and look deletable to vacuum.  Fail loudly instead. */
        size_t label;
        memcpy(&label, base + hdr.labelOffset, sizeof(size_t));
        if (label >= (size_t)tids_hdr.n_vecs)
            ereport(ERROR,
                    (errmsg("pg_cuvs: .hnsw element %zu has label %zu but "
                            ".tids has only %lld entries",
                            i, label, (long long)tids_hdr.n_vecs)));
        elems[i].tid_encoded = tids[label];

        /* level and upper links come from the upper-level block; set defaults */
        elems[i].level        = 0;
        elems[i].upper_links  = NULL;
        elems[i].upper_counts = NULL;
    }

    free(lv0_block);
    res->lv0_block = NULL;

    /* ---- 7. Read upper-level link lists ---- */
    /*
     * Upper-level data follows the level-0 block.  For each element i:
     *   uint32 linkListSize
     *   if linkListSize > 0:
     *     for levels 1..element_level: [int link_count][int links[M]]
     *     element_level = linkListSize / sizeLinksPerElement
     *     sizeLinksPerElement = (M + 1) * sizeof(int)
     */
    size_t sizeLinksPerElement = ((size_t)M + 1) * sizeof(int);

    for (size_t i = 0; i < N; i++)
    {
        uint32_t llsize = 0;
        if (fread(&llsize, sizeof(uint32_t), 1, hf) != 1)
        {
            ereport(ERROR,
                    (errmsg("pg_cuvs: short read on upper-level size for "
                            "element %zu in \"%s\"", i, hnsw_path)));
        }

        if (llsize == 0)
        {
            elems[i].level = 0;
            continue;
        }

        int elem_level = (int)(llsize / sizeLinksPerElement);
        if (elem_level <= 0)
        {
            /* skip unexpected data */
            fseek(hf, (long)llsize, SEEK_CUR);
            elems[i].level = 0;
            continue;
        }

        elems[i].level = elem_level;

        char *llbuf = (char *) malloc(llsize);
        if (!llbuf)
        {
            ereport(ERROR, (errmsg("pg_cuvs: out of memory for upper links "
                                   "element %zu", i)));
        }
        res->llbuf = llbuf;

        if (fread(llbuf, 1, llsize, hf) != llsize)
        {
            ereport(ERROR,
                    (errmsg("pg_cuvs: short read on upper links for element "
                            "%zu in \"%s\"", i, hnsw_path)));
        }

        elems[i].upper_links  = (int **) calloc((size_t)elem_level, sizeof(int *));
        elems[i].upper_counts = (int *)  calloc((size_t)elem_level, sizeof(int));
        if (!elems[i].upper_links || !elems[i].upper_counts)
        {
            ereport(ERROR, (errmsg("pg_cuvs: out of memory for upper link arrays")));
        }

        char *p = llbuf;
        /* levels 1..elem_level stored in order from level 1 upward */
        for (int lv = 1; lv <= elem_level; lv++)
        {
            int lv_idx = lv - 1;
            int lcount;
            memcpy(&lcount, p, sizeof(int));
            p += sizeof(int);
            if (lcount < 0 || lcount > M)
                lcount = 0;
            elems[i].upper_counts[lv_idx] = lcount;
            elems[i].upper_links[lv_idx]  = (int *) malloc((size_t)M * sizeof(int));
            if (!elems[i].upper_links[lv_idx])
            {
                ereport(ERROR,
                        (errmsg("pg_cuvs: out of memory for upper links lv=%d", lv)));
            }
            memcpy(elems[i].upper_links[lv_idx], p, (size_t)M * sizeof(int));
            p += (size_t)M * sizeof(int);
        }

        free(llbuf);
        res->llbuf = NULL;
    }

    fclose(hf);
    res->hf = NULL;
    free(tids);
    res->tids = NULL;

    /* ---- 8. Layout pass: assign (blkno, offno) to each element ---- */
    /*
     * One page per element (elem tuple at offno=1, neigh tuple at offno=2).
     * Block 0 is the metapage, so elements start at block 1.
     */
    for (size_t i = 0; i < N; i++)
    {
        uint32_t blkno = (uint32_t)(i + 1);  /* block 0 is meta */
        elems[i].elem_blkno  = blkno;
        elems[i].elem_offno  = 1;
        elems[i].neigh_blkno = blkno;
        elems[i].neigh_offno = 2;
    }

    /* Verify that elem+neigh fit on one page for the worst-case element level.
     * We check against the largest possible level. */
    check_hnsw_pair_fit(dim, bytes_per_dim, M, hdr.maxlevel);

    /* ---- 9. Re-open .hnsw to read vectors sequentially ---- */
    hf = hnsw_open_stream(hnsw_path, res->shm_fd);
    if (!hf)
    {
        ereport(ERROR,
                (errmsg("pg_cuvs: cannot re-open .hnsw \"%s\": %m", hnsw_path)));
    }
    res->hf = hf;

    /* Seek to level-0 block, then to the offsetData of element 0 */
    if (fseek(hf, (long)hdr.offsetLevel0, SEEK_SET) != 0)
    {
        ereport(ERROR,
                (errmsg("pg_cuvs: fseek failed on re-opened \"%s\"", hnsw_path)));
    }

    /* ---- 10. Open target HNSW relation and validate before truncate ---- */
    /*
     * AccessExclusiveLock is required — not just ExclusiveLock.
     * ExclusiveLock does NOT conflict with AccessShareLock, which is held by
     * concurrent index scans.  A truncate + full page rewrite while a reader
     * holds AccessShareLock would expose partially-written pages and corrupt
     * search results.  AccessExclusiveLock blocks all concurrent access until
     * the import transaction commits (or rolls back), matching the semantics
     * of REINDEX and other index-rebuild DDL.
     *
     * Implication: pg_cuvs_import_hnsw is an OFFLINE operation.  No SELECT
     * queries against the target HNSW index can proceed until this function
     * returns and the caller's transaction commits.
     */
    Relation hnsw_rel = index_open(hnsw_oid, AccessExclusiveLock);

    /*
     * Truncate the relation to 0 blocks so we can write fresh pages.
     * RelationTruncate (catalog/storage.h) rather than a bare smgrtruncate:
     * it emits XLOG_SMGR_TRUNCATE, without which a standby keeps the old
     * file length and retains ghost element pages past the new end.
     * It skips the WAL record itself when !RelationNeedsWAL, so the UNLOGGED
     * target needs no separate branch here.
     */
    {
        SMgrRelation smgr = RelationGetSmgr(hnsw_rel);
        BlockNumber  cur  = smgrnblocks(smgr, MAIN_FORKNUM);
        if (cur > 0)
            RelationTruncate(hnsw_rel, 0);
        /* Invalidate the buffer manager's knowledge of this relation's size. */
        RelationSetTargetBlock(hnsw_rel, InvalidBlockNumber);
    }

    CuvsConnectivity connectivity=hnsw_check_elements(elems,N,hdr.enterpointNode);
    hnsw_notice_connectivity(&connectivity);
    /* ---- 11. Write metapage (block 0) ---- */
    /* Build privately so validation/allocation errors remain ordinary ERRORs
     * before the shared-buffer critical section. */
    {
        Page page = (Page)palloc(BLCKSZ);
        PageInit(page, BLCKSZ, sizeof(PgvHnswPageOpaque));

        PgvHnswPageOpaque *opaque =
            (PgvHnswPageOpaque *) PageGetSpecialPointer(page);
        opaque->nextblkno = InvalidBlockNumber;
        opaque->unused    = 0;
        opaque->page_id   = PGV_HNSW_PAGE_ID;

        /*
         * Write metapage data directly into PageGetContents(page).
         * pgvector reads it as HnswPageGetMeta(page) = PageGetContents(page),
         * NOT via the item-pointer table. Using PageAddItem would put the data
         * at the wrong location (pd_upper area vs PageGetContents area).
         */
        PgvHnswMeta *metap = (PgvHnswMeta *) PageGetContents(page);
        memset(metap, 0, sizeof(*metap));
        metap->magicNumber   = PGV_HNSW_MAGIC;
        metap->version       = PGV_HNSW_VERSION;
        metap->dimensions    = (uint32_t)dim;
        metap->m             = (uint16_t)M;
        metap->efConstruction = metapage_ef ? metapage_ef : (uint16_t)hdr.efConstruction;

        /* entrypoint: the node with the highest level (= hdr.enterpointNode) */
        int ep = hdr.enterpointNode;
        if (ep >= 0 && (size_t)ep < N)
        {
            metap->entryBlkno  = elems[ep].elem_blkno;
            metap->entryOffno  = elems[ep].elem_offno;
            metap->entryLevel  = (int16_t)elems[ep].level;
        }
        else
        {
            metap->entryBlkno  = InvalidBlockNumber;
            metap->entryOffno  = 0;
            metap->entryLevel  = -1;
        }
        /* insertPage: last element block (block N = last element page). */
        metap->insertPage = (uint32_t)N;

        /* Update pd_lower to reflect the metapage data (pgvector pattern). */
        ((PageHeader)page)->pd_lower =
            (uint16_t)(((char *)metap + sizeof(PgvHnswMeta)) - (char *)page);
        hnsw_stage_metapage_tail(hnsw_rel,page,&connectivity,false);

        hnsw_publish_staged_page(hnsw_rel, page, hnsw_unlogged);
        pfree(page);
    }

    /* ---- 12. Write element+neighbor pages ---- */
    /* Allocate a temporary buffer for one element's vector */
    float *vec_buf = (float *) palloc((size_t)dim * sizeof(float));

    for (size_t i = 0; i < N; i++)
    {
        /* Read the vector from the .hnsw file.
         * Level-0 block starts at HNSWLIB_HEADER_BYTES (96) in the file.
         * offsetLevel0 is an intra-element offset (0 in practice).
         * offsetData is the byte offset of vectors within each element block. */
        long elem_offset = (long)HNSWLIB_HEADER_BYTES
                         + (long)hdr.offsetLevel0
                         + (long)(i * hdr.sizeDataPerElement)
                         + (long)hdr.offsetData;
        if (fseek(hf, elem_offset, SEEK_SET) != 0)
        {
            ereport(ERROR,
                    (errmsg("pg_cuvs: fseek to vector for element %zu failed",
                            i)));
        }

        if (fread(vec_buf, sizeof(float), (size_t)dim, hf) != (size_t)dim)
        {
            ereport(ERROR,
                    (errmsg("pg_cuvs: short read on vector for element %zu", i)));
        }

        write_elem_page(hnsw_rel,
                        elems[i].elem_blkno,
                        &elems[i],
                        vec_buf,
                        dim, bytes_per_dim, M,
                        N, hnsw_unlogged,
                        hnsw_lookup_elem_array, elems);
    }

    pfree(vec_buf);
    /* hf / elems are released by the PG_FINALLY in fill_hnsw_from_hnswlib(). */

    index_close(hnsw_rel, AccessExclusiveLock);

    ereport(NOTICE,
            (errmsg("pg_cuvs: imported %zu elements (dim=%d, M=%d) from "
                    "\"%s\" (use_shm=%d) into hnsw index %u",
                    N, dim, M, hnsw_path, (int)use_shm, hnsw_oid)));

    /* #165: nothing to unlink — the daemon removed the name before handing the
     * fd over, and this process (a different uid) could not have removed it.
     * hnsw_fill_res_free() closes the fd, which releases the inode. */
}

/* #161: returns the actual element count written, so ambuild() can report
 * accurate index_tuples/heap_tuples without inferring it from the relation's
 * block count — an inference that assumed exactly one element per page and
 * breaks once packing (fill_hnsw_from_cagra_ipc()'s nsw path) makes that
 * assumption false. res.n_elems is set by the _impl on the success path
 * (before PG_FINALLY frees the pointer fields, which never touches this
 * scalar) and is unreachable on the error path, where PG_TRY's longjmp
 * skips the return entirely — no stale-value case to guard against. */
static size_t
fill_hnsw_from_hnswlib(Oid cagra_oid, Oid hnsw_oid, bool use_shm,
                      int source_m, uint16_t metapage_ef, HnswSourceTidResult *tid_result)
{
    /*
     * #166 review 3: `res` is written inside PG_TRY (through the pointer the
     * impl receives) and read in PG_FINALLY, i.e. across a possible longjmp.
     * PostgreSQL requires such locals to be volatile; the cast back is the
     * usual idiom, safe because the value is only consumed after the jump.
     */
    volatile HnswFillRes res;

    memset((void *) &res, 0, sizeof(res));
    res.shm_fd = -1;            /* #165: 0 is stdin — never a valid sentinel */
    res.tids_fd = -1;
    PG_TRY();
    {
        fill_hnsw_from_hnswlib_impl(cagra_oid, hnsw_oid, use_shm, source_m, metapage_ef,
                                    (HnswFillRes *) &res);
    }
    PG_FINALLY();
    {
        hnsw_fill_res_free((HnswFillRes *) &res);
    }
    PG_END_TRY();
    *tid_result = res.tid_result;
    return res.n_elems;
}

/* ----------------------------------------------------------------
 * Source-less HNSW conversion.  The serialized graph is validated in a
 * bounded first pass while node locations are written to a PostgreSQL temp
 * tablespace BufFile.  A second pass reads one node, its vector, links and
 * label at a time, then emits staged PostgreSQL pages.  No graph-sized heap
 * arrays are created on this path.
 * ---------------------------------------------------------------- */
#define HNSW_LOCATION_CACHE_PAGES 4
#define HNSW_LOCATION_ROWS_PER_PAGE (BLCKSZ / sizeof(HnswNodeLocation))

_Static_assert(BLCKSZ % sizeof(HnswNodeLocation) == 0,
               "location records must tile PostgreSQL temp-file blocks");

typedef struct HnswLocationCachePage
{
    bool valid;
    uint64 page_no;
    size_t n_rows;
    HnswNodeLocation rows[HNSW_LOCATION_ROWS_PER_PAGE];
} HnswLocationCachePage;

typedef struct HnswLocationTable
{
    BufFile *file;
    uint64_t n_nodes;
    uint64_t n_written;
    bool formula_candidate;
    bool formula_ready;
    uint32_t formula_template_rows;
    uint32_t formula_period;
    uint64_t formula_stride;
    uint64_t formula_verified_rows;
    HnswLocationCachePage cache[HNSW_LOCATION_CACHE_PAGES];
    /* Independent of the borrowed full-location cache: a full lookup may
     * replace cache[0] while neighbor addresses remain periodic. */
    bool address_candidate;
    bool address_ready;
    uint32_t address_template_rows;
    uint32_t address_period;
    uint64_t address_stride;
    uint64_t address_verified_rows;
    HnswNodeAddress address_template[HNSW_LOCATION_ROWS_PER_PAGE];
} HnswLocationTable;

typedef struct HnswDiskImportState
{
    HnswLocationTable locations;
    Relation target;
} HnswDiskImportState;

typedef struct HnswDiskRow
{
    ElemDesc elem;
    float *vector;
    int **upper_links;
    int *upper_counts;
    int *upper_data;
    /* Read-ahead for level-0 records, upper-level records and TIDs.  The
     * buffers live in the caller's memory context for the whole import. */
    HnswReadWindow level0;
    HnswReadWindow upper;
    HnswReadWindow tids;
} HnswDiskRow;

/* Reuse one existing cache row array as a bounded first-data-page template.
 * The spill file and its full readback remain authoritative. */
static bool
hnsw_location_formula(const HnswLocationTable *table, uint64_t node,
                       HnswNodeLocation *location)
{
    uint32_t count = table->formula_template_rows;
    const HnswNodeLocation *pattern;
    uint64_t page;
    uint64_t delta;
    uint64_t block;

    if (!table->formula_candidate || count == 0 ||
        count > HNSW_LOCATION_ROWS_PER_PAGE || node >= table->n_nodes ||
        (table->formula_period != 0 && table->formula_period != count))
        return false;
    page = node / count;
    if (table->formula_period == 0 && page != 0)
        return false; /* No second page was observed: only this page is proved. */
    pattern = &table->cache[0].rows[node % count];
    if (!hnsw_mul_u64(page, table->formula_stride, &delta) ||
        !hnsw_add_u64(pattern->elem_blkno, delta, &block) ||
        block == 0 || block >= (uint64_t)InvalidBlockNumber)
        return false;
    location->elem_blkno = (uint32_t)block;
    location->elem_offno = pattern->elem_offno;
    location->level = pattern->level;
    location->reserved = pattern->reserved;
    return true;
}

static bool
hnsw_location_formula_matches(const HnswLocationTable *table, uint64_t node,
                               const HnswNodeLocation *location)
{
    HnswNodeLocation expected;

    return hnsw_location_formula(table, node, &expected) &&
        expected.elem_blkno == location->elem_blkno &&
        expected.elem_offno == location->elem_offno &&
        expected.level == location->level &&
        expected.reserved == location->reserved;
}

static void
hnsw_location_formula_append(HnswLocationTable *table, uint64_t node,
                              const HnswNodeLocation *location)
{
    if (node == 0)
    {
        table->formula_candidate = true;
        table->formula_period = table->formula_template_rows = 0;
        table->formula_stride = table->formula_verified_rows = 0;
        /* Do not overwrite an already-populated legacy cache on reuse. */
        for (int i = 0; i < HNSW_LOCATION_CACHE_PAGES; i++)
            if (table->cache[i].valid)
                table->formula_candidate = false;
    }
    table->formula_ready = false;
    if (!table->formula_candidate)
        return;
    if (table->formula_template_rows == 0 ||
        (table->formula_period == 0 &&
         location->elem_blkno == table->cache[0].rows[0].elem_blkno))
    {
        if (table->formula_template_rows >= HNSW_LOCATION_ROWS_PER_PAGE)
        {
            table->formula_candidate = false;
            return;
        }
        table->cache[0].rows[table->formula_template_rows++] = *location;
        return;
    }
    if (table->formula_period == 0)
    {
        if (location->elem_blkno <= table->cache[0].rows[0].elem_blkno)
        {
            table->formula_candidate = false;
            return;
        }
        table->formula_period = table->formula_template_rows;
        table->formula_stride = (uint64_t)location->elem_blkno -
            table->cache[0].rows[0].elem_blkno;
    }
    if (!hnsw_location_formula_matches(table, node, location))
        table->formula_candidate = false;
}

/* Only block/offset are predicted.  The full location, including level,
 * always uses its own independently verified formula or the spill file. */
static bool
hnsw_address_formula(const HnswLocationTable *table, uint64_t node,
                     HnswNodeAddress *address)
{
    uint32_t count = table->address_template_rows;
    const HnswNodeAddress *pattern;
    uint64_t page, delta, block;

    if (!table->address_candidate || count == 0 ||
        count > HNSW_LOCATION_ROWS_PER_PAGE || node >= table->n_nodes ||
        (table->address_period != 0 && table->address_period != count))
        return false;
    page = node / count;
    if (table->address_period == 0 && page != 0)
        return false;
    pattern = &table->address_template[node % count];
    if (!hnsw_mul_u64(page, table->address_stride, &delta) ||
        !hnsw_add_u64(pattern->elem_blkno, delta, &block) ||
        block == 0 || block >= (uint64_t)InvalidBlockNumber)
        return false;
    address->elem_blkno = (uint32_t)block;
    address->elem_offno = pattern->elem_offno;
    return true;
}

static bool
hnsw_address_formula_matches(const HnswLocationTable *table, uint64_t node,
                             const HnswNodeLocation *location)
{
    HnswNodeAddress expected;

    return hnsw_address_formula(table, node, &expected) &&
        expected.elem_blkno == location->elem_blkno &&
        expected.elem_offno == location->elem_offno;
}

static void
hnsw_address_formula_append(HnswLocationTable *table, uint64_t node,
                            const HnswNodeLocation *location)
{
    if (node == 0)
    {
        table->address_candidate = true;
        table->address_period = table->address_template_rows = 0;
        table->address_stride = table->address_verified_rows = 0;
    }
    table->address_ready = false;
    if (!table->address_candidate)
        return;
    if (table->address_template_rows == 0 ||
        (table->address_period == 0 &&
         location->elem_blkno == table->address_template[0].elem_blkno))
    {
        HnswNodeAddress *slot;

        if (table->address_template_rows >= HNSW_LOCATION_ROWS_PER_PAGE)
        {
            table->address_candidate = false;
            return;
        }
        slot = &table->address_template[table->address_template_rows++];
        slot->elem_blkno = location->elem_blkno;
        slot->elem_offno = location->elem_offno;
        return;
    }
    if (table->address_period == 0)
    {
        if (location->elem_blkno <= table->address_template[0].elem_blkno)
        {
            table->address_candidate = false;
            return;
        }
        table->address_period = table->address_template_rows;
        table->address_stride = (uint64_t)location->elem_blkno -
            table->address_template[0].elem_blkno;
    }
    if (!hnsw_address_formula_matches(table, node, location))
        table->address_candidate = false;
}

static bool
hnsw_location_table_append(void *arg, uint64_t node,
                           const HnswNodeLocation *location,
                           char *error, size_t error_size)
{
    HnswLocationTable *table = (HnswLocationTable *)arg;

    if (!table->file || node != table->n_written || node >= table->n_nodes)
        return hnsw_set_file_error(error, error_size,
                                   "internal node-location table write order error");
    BufFileWrite(table->file, (void *)location, sizeof(*location));
    hnsw_location_formula_append(table, node, location);
    hnsw_address_formula_append(table, node, location);
    table->n_written++;
    return true;
}

static bool
hnsw_location_table_rewind(HnswLocationTable *table, uint64_t expected_rows)
{
    HnswNodeLocation rows[HNSW_LOCATION_ROWS_PER_PAGE];
    uint64_t logical_bytes;
    uint64_t bytes_remaining;
    uint64_t read_node = 0;

    if (table)
    {
        table->formula_ready = false;
        table->address_ready = false;
    }
    if (!table || !table->file || table->n_nodes != expected_rows ||
        table->n_written != expected_rows ||
        !hnsw_mul_u64(expected_rows, sizeof(HnswNodeLocation), &logical_bytes) ||
        (logical_bytes > 0 && (logical_bytes - 1) / BLCKSZ > LONG_MAX))
        return false;

    /* BufFile is buffered and BufFileSize only supports shared filesets. */
    if (BufFileSeekBlock(table->file, 0) != 0)
        return false;
    bytes_remaining = logical_bytes;
    while (bytes_remaining > 0)
    {
        size_t bytes = bytes_remaining > sizeof(rows)
            ? sizeof(rows) : (size_t)bytes_remaining;

        CHECK_FOR_INTERRUPTS();
        if (BufFileRead(table->file, rows, bytes) != bytes)
            return false;
        if (table->formula_candidate)
            for (size_t i = 0; i < bytes / sizeof(rows[0]); i++)
                if (!hnsw_location_formula_matches(table, read_node + i, &rows[i]))
                {
                    table->formula_candidate = false;
                    break;
                }
        if (table->address_candidate)
            for (size_t i = 0; i < bytes / sizeof(rows[0]); i++)
                if (!hnsw_address_formula_matches(table, read_node + i, &rows[i]))
                {
                    table->address_candidate = false;
                    break;
                }
        read_node += bytes / sizeof(rows[0]);
        bytes_remaining -= bytes;
    }

    /* Rewind after readback so the first lookup starts at logical row zero. */
    if (BufFileSeekBlock(table->file, 0) != 0)
        return false;
    table->formula_ready = table->formula_candidate && expected_rows > 0 &&
        read_node == expected_rows && table->formula_template_rows > 0;
    table->formula_verified_rows = table->formula_ready ? expected_rows : 0;
    table->address_ready = table->address_candidate && expected_rows > 0 &&
        read_node == expected_rows && table->address_template_rows > 0;
    table->address_verified_rows = table->address_ready ? expected_rows : 0;
    return true;
}

static bool
hnsw_location_table_lookup(void *arg, uint32_t node,
                           HnswNodeLocation *location)
{
    HnswLocationTable *table = (HnswLocationTable *)arg;
    uint64_t page_no;
    uint64_t first_node;
    size_t row_index;
    HnswLocationCachePage *cache;

    if ((uint64_t)node >= table->n_nodes)
        ereport(ERROR,
                (errmsg("pg_cuvs: HNSW node-location lookup is out of range")));
    if (table->formula_ready && table->file &&
        table->formula_verified_rows == table->n_nodes &&
        table->n_written == table->n_nodes &&
        hnsw_location_formula(table, node, location))
        return true;
    /* Any legacy lookup can replace the borrowed template rows. */
    table->formula_ready = table->formula_candidate = false;
    page_no = (uint64_t)node / HNSW_LOCATION_ROWS_PER_PAGE;
    first_node = page_no * HNSW_LOCATION_ROWS_PER_PAGE;
    row_index = (size_t)((uint64_t)node - first_node);
    cache = &table->cache[page_no % HNSW_LOCATION_CACHE_PAGES];
    if (!cache->valid || cache->page_no != page_no)
    {
        uint64_t remaining = table->n_nodes - first_node;
        size_t n_rows = remaining > HNSW_LOCATION_ROWS_PER_PAGE
            ? HNSW_LOCATION_ROWS_PER_PAGE : (size_t)remaining;
        size_t n_bytes = n_rows * sizeof(HnswNodeLocation);

        if (page_no > LONG_MAX ||
            BufFileSeekBlock(table->file, (long)page_no) != 0 ||
            BufFileRead(table->file, cache->rows, n_bytes) != n_bytes)
            ereport(ERROR,
                    (errmsg("pg_cuvs: cannot read disk-backed HNSW node locations")));
        cache->page_no = page_no;
        cache->n_rows = n_rows;
        cache->valid = true;
    }
    if (row_index >= cache->n_rows)
        ereport(ERROR,
                (errmsg("pg_cuvs: HNSW node-location cache entry is out of range")));
    *location = cache->rows[row_index];
    return true;
}

static bool
hnsw_location_table_address_lookup(void *arg, uint32_t node,
                                   HnswNodeAddress *address)
{
    HnswLocationTable *table = (HnswLocationTable *)arg;
    HnswNodeLocation location;

    if ((uint64_t)node >= table->n_nodes)
        ereport(ERROR,
                (errmsg("pg_cuvs: HNSW node-location lookup is out of range")));
    if (table->address_ready && table->file &&
        table->address_verified_rows == table->n_nodes &&
        table->n_written == table->n_nodes &&
        hnsw_address_formula(table, node, address))
        return true;
    if (!hnsw_location_table_lookup(arg, node, &location))
        return false;
    address->elem_blkno = location.elem_blkno;
    address->elem_offno = location.elem_offno;
    return true;
}

static void
hnsw_disk_row_init(HnswDiskRow *row, const HnswlibHeader *header,
                   uint32_t dim)
{
    size_t upper_values;

    memset(row, 0, sizeof(*row));
    row->elem.lv0_links = (int *)palloc((size_t)header->maxM0 * sizeof(int));
    row->vector = (float *)palloc((size_t)dim * sizeof(float));
    if (header->maxlevel > 0)
    {
        upper_values = (size_t)header->maxlevel * (size_t)header->M;
        row->upper_links = (int **)palloc0((size_t)header->maxlevel * sizeof(int *));
        row->upper_counts = (int *)palloc0((size_t)header->maxlevel * sizeof(int));
        row->upper_data = (int *)palloc(upper_values * sizeof(int));
        for (int level = 0; level < header->maxlevel; level++)
            row->upper_links[level] = row->upper_data +
                (size_t)level * (size_t)header->M;
    }
    hnsw_window_init(&row->level0, -1, palloc(HNSW_READ_WINDOW_BYTES),
                     HNSW_READ_WINDOW_BYTES);
    hnsw_window_init(&row->upper, -1, palloc(HNSW_READ_WINDOW_BYTES),
                     HNSW_READ_WINDOW_BYTES);
    hnsw_window_init(&row->tids, -1, palloc(HNSW_READ_WINDOW_BYTES),
                     HNSW_READ_WINDOW_BYTES);
}

static void
hnsw_read_disk_row(int hnsw_fd, int tids_fd,
                   const HnswlibHeader *header, uint64_t n_nodes,
                   uint32_t dim, uint64_t node, uint64_t *upper_cursor,
                   HnswLocationTable *locations, HnswDiskRow *row)
{
    uint64_t row_delta;
    uint64_t row_base;
    uint64_t field_offset;
    uint64_t level0_links_offset;
    uint64_t label;
    uint64_t tid;
    uint32_t upper_bytes;
    uint64_t per_level_bytes = (header->M + 1) * sizeof(int);
    int link_count;
    HnswNodeLocation location;
    char error[256] = {0};

    hnsw_window_bind(&row->level0, hnsw_fd);
    hnsw_window_bind(&row->upper, hnsw_fd);
    hnsw_window_bind(&row->tids, tids_fd);
    if (node < n_nodes &&
        hnsw_mul_u64(node, header->sizeDataPerElement, &row_delta) &&
        hnsw_add_u64(HNSWLIB_HEADER_BYTES, row_delta, &row_base))
        hnsw_window_reserve(&row->level0, row_base,
                            header->sizeDataPerElement);
    if (node >= n_nodes ||
        !hnsw_mul_u64(node, header->sizeDataPerElement, &row_delta) ||
        !hnsw_add_u64(HNSWLIB_HEADER_BYTES, row_delta, &row_base) ||
        !hnsw_add_u64(row_base, header->offsetLevel0, &level0_links_offset) ||
        !hnsw_window_read(&row->level0, &link_count, sizeof(link_count),
                          level0_links_offset) ||
        link_count < 0 || (uint64_t)link_count > header->maxM0 ||
        !hnsw_add_u64(level0_links_offset, sizeof(link_count), &field_offset) ||
        !hnsw_window_read(&row->level0, row->elem.lv0_links,
                          (size_t)link_count * sizeof(int), field_offset))
        ereport(ERROR,
                (errmsg("pg_cuvs: cannot read HNSW level-0 row %llu",
                        (unsigned long long)node)));

    if (!hnsw_add_u64(row_base, header->offsetData, &field_offset) ||
        !hnsw_window_read(&row->level0, row->vector,
                          (size_t)dim * sizeof(float), field_offset) ||
        !hnsw_add_u64(row_base, header->labelOffset, &field_offset) ||
        !hnsw_window_read(&row->level0, &label, sizeof(size_t), field_offset) ||
        !hnsw_read_tid_window(&row->tids, n_nodes, label, &tid,
                              error, sizeof(error)) ||
        !hnsw_location_table_lookup(locations, (uint32_t)node, &location))
        ereport(ERROR,
                (errmsg("pg_cuvs: cannot read HNSW vector, label or location for row %llu: %s",
                        (unsigned long long)node, error)));

    row->elem.level = location.level;
    row->elem.tid_encoded = tid;
    row->elem.lv0_count = link_count;
    row->elem.upper_links = row->upper_links;
    row->elem.upper_counts = row->upper_counts;
    row->elem.elem_blkno = location.elem_blkno;
    row->elem.elem_offno = location.elem_offno;
    row->elem.neigh_blkno = hnsw_location_neigh_blkno(&location);
    row->elem.neigh_offno = hnsw_location_neigh_offno(&location);

    if (!hnsw_window_read(&row->upper, &upper_bytes, sizeof(upper_bytes),
                          *upper_cursor))
        ereport(ERROR,
                (errmsg("pg_cuvs: cannot read HNSW upper-level size for row %llu",
                        (unsigned long long)node)));
    if ((uint64_t)upper_bytes != (uint64_t)location.level * per_level_bytes)
        ereport(ERROR,
                (errmsg("pg_cuvs: HNSW row level changed after preflight")));
    if (!hnsw_add_u64(*upper_cursor, sizeof(upper_bytes), &field_offset))
        ereport(ERROR, (errmsg("pg_cuvs: HNSW upper-level offset overflow")));
    for (int level = 0; level < location.level; level++)
    {
        uint64_t level_delta;
        uint64_t links_offset;
        int count;

        if (!hnsw_mul_u64((uint64_t)level, per_level_bytes, &level_delta) ||
            !hnsw_add_u64(field_offset, level_delta, &links_offset) ||
            !hnsw_window_read(&row->upper, &count, sizeof(count), links_offset) ||
            count < 0 || (uint64_t)count > header->M ||
            !hnsw_add_u64(links_offset, sizeof(count), &links_offset) ||
            !hnsw_window_read(&row->upper, row->upper_links[level],
                              (size_t)header->M * sizeof(int), links_offset))
            ereport(ERROR,
                    (errmsg("pg_cuvs: cannot read HNSW upper-level links for row %llu",
                            (unsigned long long)node)));
        row->upper_counts[level] = count;
    }
    row->elem.level = location.level;
    if (!hnsw_add_u64(*upper_cursor,
                      sizeof(upper_bytes) + upper_bytes, upper_cursor))
        ereport(ERROR, (errmsg("pg_cuvs: HNSW upper-level cursor overflow")));
}

static void
hnsw_publish_staged_page(Relation rel, Page staged, bool skip_wal)
{
    Buffer buf;

    hnsw_cover_check_metapage(rel, staged);
    buf = ReadBuffer(rel, P_NEW);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    START_CRIT_SECTION();
    memcpy(BufferGetPage(buf), staged, BLCKSZ);
    MarkBufferDirty(buf);
    if (!skip_wal)
        log_newpage_buffer(buf, true);
    UnlockReleaseBuffer(buf);
    END_CRIT_SECTION();
}

/* Source-less data pages have a checked, contiguous layout.  Extend a
 * bounded group while retaining the existing per-page WAL image and order.
 * Buffer pins belong to the current ResourceOwner, including on ERROR. */
#define HNSW_DISK_EXTEND_BATCH 32
typedef struct HnswDiskPageBatch
{
    Buffer buffers[HNSW_DISK_EXTEND_BATCH];
    uint32 count;
    uint32 next;
} HnswDiskPageBatch;

static void
hnsw_publish_staged_data_page(Relation rel, Page staged, bool skip_wal,
                              uint32 expected_block, uint32 page_count,
                              HnswDiskPageBatch *batch)
{
#if PG_VERSION_NUM >= 160000
    Buffer buf;

    if (expected_block == 0 || expected_block > page_count)
        ereport(ERROR, (errmsg("pg_cuvs: invalid HNSW data page position")));
    if (batch->next == batch->count)
    {
        uint32 remaining = page_count - expected_block + 1;
        uint32 requested = Min(remaining, (uint32)HNSW_DISK_EXTEND_BATCH);
        BlockNumber first;

        batch->next = 0;
        batch->count = 0;
        first = ExtendBufferedRelBy(BMR_REL(rel), MAIN_FORKNUM, NULL, 0,
                                    requested, batch->buffers, &batch->count);
        /* PostgreSQL may return fewer buffers than requested. */
        if (first != expected_block || batch->count == 0 ||
            batch->count > requested)
            ereport(ERROR, (errmsg("pg_cuvs: unexpected HNSW relation extension")));
    }
    buf = batch->buffers[batch->next];
    if (BufferGetBlockNumber(buf) != expected_block)
        ereport(ERROR, (errmsg("pg_cuvs: noncontiguous HNSW extension buffers")));
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    START_CRIT_SECTION();
    memcpy(BufferGetPage(buf), staged, BLCKSZ);
    MarkBufferDirty(buf);
    if (!skip_wal)
        log_newpage_buffer(buf, true);
    UnlockReleaseBuffer(buf);
    END_CRIT_SECTION();
    batch->next++;
#else
    hnsw_publish_staged_page(rel, staged, skip_wal);
#endif
}

static void
hnsw_write_disk_pages(Relation rel, int hnsw_fd, int tids_fd,
                      const HnswlibHeader *header, int m, uint64_t n_nodes,
                      uint32_t dim, int bytes_per_dim, uint32_t data_page_count,
                      bool skip_wal,
                      HnswLocationTable *locations)
{
    HnswDiskRow row;
    Page staged = (Page)palloc(BLCKSZ);
    Page next = (Page)palloc(BLCKSZ);
    bool next_started = false;
    uint64_t upper_cursor;
    uint64_t level0_bytes;
    uint32_t current_block = 0;
    HnswDiskPageBatch batch = {0};

    if (!hnsw_mul_u64(n_nodes, header->sizeDataPerElement, &level0_bytes) ||
        !hnsw_add_u64(HNSWLIB_HEADER_BYTES, level0_bytes, &upper_cursor))
        ereport(ERROR, (errmsg("pg_cuvs: HNSW level-0 offset overflow")));
    hnsw_disk_row_init(&row, header, dim);

    for (uint64_t node = 0; node < n_nodes; node++)
    {
        CHECK_FOR_INTERRUPTS();
        hnsw_read_disk_row(hnsw_fd, tids_fd, header, n_nodes, dim,
                           node, &upper_cursor, locations, &row);
        if (row.elem.elem_blkno != current_block)
        {
            if (current_block != 0)
                hnsw_publish_staged_data_page(rel, staged, skip_wal,
                                              current_block, data_page_count, &batch);
            if (next_started)
            {
                /* the page holding the last split pair's neighbor tuple; the element may start on it or on the page after */
                Page t = staged;

                staged = next;
                next = t;
                next_started = false;
                current_block++;
                if (row.elem.elem_blkno != current_block)
                    hnsw_publish_staged_data_page(rel, staged, skip_wal,
                                                  current_block, data_page_count, &batch);
            }
            if (row.elem.elem_blkno != current_block)
            {
                if (row.elem.elem_blkno != current_block + 1 ||
                    row.elem.elem_blkno > data_page_count)
                    ereport(ERROR,
                            (errmsg("pg_cuvs: noncontiguous source-less HNSW page layout")));
                current_block = row.elem.elem_blkno;
                hnsw_init_disk_page(staged);
                if (!hnsw_set_disk_page_chain(staged, current_block, data_page_count))
                    ereport(ERROR,
                            (errmsg("pg_cuvs: invalid source-less HNSW data page chain position")));
            }
        }
        if (row.elem.neigh_blkno == current_block)
            write_elem_pair(staged, staged, &row.elem, row.vector, (int)dim,
                            bytes_per_dim, m, (size_t)n_nodes,
                            hnsw_location_table_address_lookup, locations);
        else
        {
            /* a split pair's neighbor tuple goes first on the next page (pgvector's build rule). */
            if (row.elem.neigh_blkno != current_block + 1 || row.elem.neigh_offno != 1 ||
                next_started || row.elem.neigh_blkno > data_page_count)
                ereport(ERROR,
                        (errmsg("pg_cuvs: source-less HNSW layout places a neighbor tuple off the next page's first slot")));
            hnsw_init_disk_page(next);
            if (!hnsw_set_disk_page_chain(next, row.elem.neigh_blkno, data_page_count))
                ereport(ERROR,
                        (errmsg("pg_cuvs: invalid source-less HNSW data page chain position")));
            next_started = true;
            write_elem_pair(staged, next, &row.elem, row.vector, (int)dim,
                            bytes_per_dim, m, (size_t)n_nodes,
                            hnsw_location_table_address_lookup, locations);
        }
    }
    if (current_block + (next_started ? 1 : 0) != data_page_count)
        ereport(ERROR,
                (errmsg("pg_cuvs: source-less HNSW page count differs from layout")));
    if (current_block != 0)
        hnsw_publish_staged_data_page(rel, staged, skip_wal,
                                      current_block, data_page_count, &batch);
    if (next_started)
        hnsw_publish_staged_data_page(rel, next, skip_wal,
                                      current_block + 1, data_page_count, &batch);
    if (batch.next != batch.count)
        ereport(ERROR, (errmsg("pg_cuvs: unused HNSW extension buffers")));
}

/* Inputs have passed hnsw_file_preflight. Only adjacency pages are touched;
 * vector bytes are not copied into another host buffer. */
typedef struct HnswConnectivityInput {
    const unsigned char *map;
    const HnswlibHeader *h;
    size_t *upper;
    uint32_t *levels;
} HnswConnectivityInput;
static const unsigned char *
hnsw_connectivity_links(void *arg, uint32_t node, uint32_t level, uint32_t *count)
{
    HnswConnectivityInput *in = arg;
    const unsigned char *p;
    uint16_t n;
    *count = 0;
    if (level > in->levels[node]) { *count=UINT32_MAX; return NULL; }
    p = level ? in->map + in->upper[node] + (size_t)(level-1)*(4+4*in->h->maxM)
              : in->map + sizeof(HnswlibHeader) + (size_t)node*in->h->sizeDataPerElement + in->h->offsetLevel0;
    memcpy(&n, p, 2); *count=n;
    return p+4;
}
static int hnsw_connectivity_cancel(void *arg)
{
    (void)arg;
    /* InterruptPending also covers routine PG work (barriers, memory-context
     * logging, etc.). Let PG service it and distinguish cancellation/termination.
     * Those errors propagate through the adapters' workspace cleanup blocks. */
    CHECK_FOR_INTERRUPTS();
    return 0;
}
static void
hnsw_check_connectivity(int fd, const HnswlibHeader *h, CuvsConnectivity *out)
{
    struct stat st;
    struct timespec start, end;
    HnswConnectivityInput in = {0};
    size_t cursor, aux, bytes;
    uint32_t n = (uint32_t)h->curElementCount;
    void *workspace=NULL;
    bool spilled=false;
    if(!cuvs_hnsw_connectivity_check){cuvs_connectivity_set_disabled(out);return;}
    CHECK_FOR_INTERRUPTS();
    memset(out, 0, sizeof(*out));
    out->magic=CUVS_CONNECTIVITY_MAGIC;out->version=1;out->nodes=n;
    clock_gettime(CLOCK_MONOTONIC, &start);
    if (fstat(fd,&st) || st.st_size<=0 || (uint64_t)st.st_size>SIZE_MAX || (uint64_t)n*29+8>SIZE_MAX)return;
    in.map=mmap(NULL,(size_t)st.st_size,PROT_READ,MAP_PRIVATE,fd,0);
    if(in.map==MAP_FAILED)return;
    aux=((size_t)n*17+7)&~(size_t)7;bytes=aux+(size_t)n*12;
    workspace=hnsw_connectivity_workspace(bytes,&spilled);
    if(workspace) {
        in.h=h;in.upper=(size_t*)((char*)workspace+aux);in.levels=(uint32_t*)(in.upper+n);
    }
    PG_TRY();
    {
      if(workspace) {
        cursor=sizeof(*h)+(size_t)n*h->sizeDataPerElement;
        for(uint32_t i=0;i<n;i++) {
            uint32_t length;
            if((i&4095)==0)CHECK_FOR_INTERRUPTS();
            memcpy(&length,in.map+cursor,4);cursor+=4;
            in.upper[i]=cursor;in.levels[i]=length/(4+4*h->maxM);cursor+=length;
        }
        (void)cuvs_connectivity_scratch(n,(uint32_t)h->enterpointNode,in.levels[h->enterpointNode],(uint32_t)h->maxlevel,
            hnsw_connectivity_links,&in,hnsw_connectivity_cancel,workspace,out);
      }
        out->spilled=spilled;
    }
    PG_FINALLY();
    {
        hnsw_connectivity_workspace_free(workspace,bytes,spilled);
        munmap((void*)in.map,(size_t)st.st_size);
    }
    PG_END_TRY();
    clock_gettime(CLOCK_MONOTONIC,&end);
    out->elapsed_us=(uint64_t)(end.tv_sec-start.tv_sec)*1000000+(end.tv_nsec-start.tv_nsec)/1000;
    CHECK_FOR_INTERRUPTS();
}

static void
hnsw_write_disk_metapage(Relation rel, const HnswlibHeader *header,
                         uint16_t m, uint32_t dim, uint32_t n_data_pages, bool skip_wal,
                         HnswLocationTable *locations, const CuvsHnswPolicy *policy,
                         const CuvsHnswAceReadback *ace, const CuvsHnswNnReadback *nn,
                         const CuvsConnectivity *connectivity)
{
    Page staged = (Page)palloc(BLCKSZ);
    HnswNodeLocation entry;

    if (!hnsw_location_table_lookup(locations, (uint32_t)header->enterpointNode,
                                    &entry))
        ereport(ERROR, (errmsg("pg_cuvs: cannot resolve HNSW entrypoint")));
    if (!hnsw_init_metapage(staged, dim, m, (uint16_t)header->efConstruction,
                            entry.elem_blkno, entry.elem_offno, entry.level,
                            n_data_pages))
        ereport(ERROR, (errmsg("pg_cuvs: invalid HNSW metapage fields")));
    /* a graph converted from a CAGRA index (policy NULL) has no build
     * receipt, as the backend's own conversion writes none. */
    if (policy && !cuvs_hnsw_policy_valid(policy))
        ereport(ERROR, (errmsg("pg_cuvs: missing or invalid native HNSW build policy receipt")));
    if (policy)
    {
        memcpy((char *)PageGetContents(staged) + sizeof(PgvHnswMeta), policy, sizeof(*policy));
        ((PageHeader)staged)->pd_lower += sizeof(*policy);
    }
    /* ACE partition-builder readback right after the receipt (receipt layout unchanged). */
    if (policy && policy->route == CUVS_HNSW_ROUTE_ACE)
    {
        if (!cuvs_hnsw_ace_readback_valid(ace, policy))
            ereport(ERROR, (errmsg("pg_cuvs: missing or invalid ACE partition-builder readback")));
        memcpy((char *)PageGetContents(staged) + sizeof(PgvHnswMeta) + sizeof(*policy),
               ace, sizeof(*ace));
        ((PageHeader)staged)->pd_lower += sizeof(*ace);
    }
    if (policy && policy->route == CUVS_HNSW_ROUTE_NN_DESCENT)
    {
        if (!cuvs_hnsw_nn_readback_valid(nn, policy)
            || ((PageHeader)staged)->pd_lower + sizeof(*nn) > ((PageHeader)staged)->pd_upper)
            ereport(ERROR, (errmsg("pg_cuvs: invalid NN readback or insufficient metapage space")));
        memcpy((char *)PageGetContents(staged) + sizeof(PgvHnswMeta) + sizeof(*policy),
               nn, sizeof(*nn));
        ((PageHeader)staged)->pd_lower += sizeof(*nn);
    }
    hnsw_stage_metapage_tail(rel, staged, connectivity, false);
    hnsw_publish_staged_page(rel, staged, skip_wal);
    pfree(staged);
}

static void
hnsw_validate_build_target(Relation indexRel,
                           const CuvsHnswBuildResult *build)
{
    Oid index_oid = RelationGetRelid(indexRel);
    Oid vector_ext = get_extension_oid("vector", true);
    Oid hnsw_am = get_am_oid("hnsw", true);
    Oid cuvs_hnsw_am = get_am_oid("pg_cuvs_hnsw", true);
    Form_pg_class index_form = RelationGetForm(indexRel);
    Form_pg_attribute attr = TupleDescAttr(RelationGetDescr(indexRel), 0);
    int dim = (int)attr->atttypmod;

    if (!OidIsValid(vector_ext))
        ereport(ERROR,
                (errmsg("pg_cuvs: pgvector extension is not installed; "
                        "HNSW conversion requires pgvector 0.5.0+")));
    if (!cuvs_warn_pgvector_version())
        ereport(ERROR,
                (errmsg("pg_cuvs: unsupported pgvector HNSW page format version"),
                 errhint("Source-less conversion requires pgvector 0.5.x–0.8.x.")));
    if (index_form->relkind != RELKIND_INDEX ||
        (index_form->relam != hnsw_am && index_form->relam != cuvs_hnsw_am))
        ereport(ERROR,
                (errmsg("pg_cuvs: target relation is not a pgvector HNSW index")));
    if (dim <= 0 || (uint32_t)dim != build->dim)
        ereport(ERROR,
                (errmsg("pg_cuvs: HNSW build dimension %u does not match target dimension %d",
                        build->dim, dim)));
    if (cagra_index_metric(index_oid) != build->metric)
        ereport(ERROR,
                (errmsg("pg_cuvs: HNSW build metric does not match target opclass")));
}

static void
hnsw_import_empty_build_result(Relation indexRel,
                               const CuvsHnswBuildResult *build,
                               uint16_t m, uint16_t metapage_ef)
{
    volatile HnswDiskImportState state;
    bool skip_wal;

    memset((void *)&state, 0, sizeof(state));
    PG_TRY();
    {
        HnswDiskImportState *s = (HnswDiskImportState *)&state;
        Page metapage = (Page)palloc(BLCKSZ);
        Page first_data_page = (Page)palloc(BLCKSZ);
        Oid index_oid = RelationGetRelid(indexRel);

        hnsw_validate_build_target(indexRel, build);
        if (build->n_vecs != 0 || build->hnsw_fd != -1 || build->tids_fd != -1)
            ereport(ERROR,
                    (errmsg("pg_cuvs: empty HNSW build returned files or a nonzero count")));
        if (!hnsw_init_empty_pages(metapage, first_data_page, build->dim,
                                   m, metapage_ef))
            ereport(ERROR, (errmsg("pg_cuvs: cannot initialize empty HNSW pages")));

        {
            CuvsConnectivity c={0};
            if(cuvs_hnsw_connectivity_check){
                c.magic=CUVS_CONNECTIVITY_MAGIC;c.version=1;c.available=CUVS_CONNECTIVITY_MEASURED;
            } else
                cuvs_connectivity_set_disabled(&c);
            hnsw_stage_metapage_tail(indexRel,metapage,&c,true);
            hnsw_notice_connectivity(&c);
        }
        /* The caller holds the lock its command takes on the index being built: AccessExclusiveLock for CREATE INDEX
         * and REINDEX, RowExclusiveLock for a concurrent build (index_concurrently_build). Asking for more here would
         * make every writer of the table wait, since a writer opens a not-yet-ready index with RowExclusiveLock too
         * (ExecOpenIndices); nobody else reads or writes the pages of an index that is not ready. */
        s->target = index_open(index_oid, NoLock);
        skip_wal = RelationGetForm(s->target)->relpersistence == RELPERSISTENCE_UNLOGGED;
        if (skip_wal)
            ereport(NOTICE,
                    (errmsg("pg_cuvs: target HNSW index is UNLOGGED — WAL skipped; "
                            "index must be rebuilt after crash recovery")));
        {
            SMgrRelation smgr = RelationGetSmgr(s->target);
            BlockNumber current = smgrnblocks(smgr, MAIN_FORKNUM);
            if (current > 0)
            {
                /* Only an AccessExclusiveLock holder may cut pages others could read; a concurrent build's new
                 * relation has none. */
                if (!CheckRelationLockedByMe(s->target, AccessExclusiveLock, false))
                    ereport(ERROR,
                            (errmsg("pg_cuvs: HNSW build target \"%s\" already has %u pages",
                                    RelationGetRelationName(s->target), current)));
                RelationTruncate(s->target, 0);
            }
            RelationSetTargetBlock(s->target, InvalidBlockNumber);
        }
        hnsw_publish_staged_page(s->target, metapage, skip_wal);
        hnsw_publish_staged_page(s->target, first_data_page, skip_wal);
        if (hnsw_cover_enabled(indexRel))
        {
            Page ledger_page = (Page)palloc(BLCKSZ);

            cuvs_ledger_page_init((uint8_t *) ledger_page, CUVS_LEDGER_KIND_KEYS, 1, 0, CUVS_LEDGER_NO_BLOCK, 3);
            hnsw_publish_staged_page(s->target, ledger_page, skip_wal);
            pfree(ledger_page);
            if (RelationGetNumberOfBlocks(s->target) != 3)
                ereport(ERROR, (errmsg("pg_cuvs: the ledger page of an empty index must be block 2")));
        }
        index_close(s->target, NoLock);
        s->target = NULL;
    }
    PG_FINALLY();
    {
        HnswDiskImportState *s = (HnswDiskImportState *)&state;
        if (s->target)
        {
            index_close(s->target, NoLock);
            s->target = NULL;
        }
    }
    PG_END_TRY();
}

static void
hnsw_import_build_result(Relation indexRel,
                         const CuvsHnswBuildResult *build,
                         bool packed_nsw, uint16_t heap_m, int source_m,
                         uint16_t metapage_ef)
{
    volatile HnswDiskImportState state;
    HnswlibHeader header;
    CuvsConnectivity connectivity;
    CuvsTidsHeader tids_header;
    HnswNodeLocation last_location;
    char error[256] = {0};
    Oid index_oid = RelationGetRelid(indexRel);
    Form_pg_attribute attr = TupleDescAttr(RelationGetDescr(indexRel), 0);
    int bytes_per_dim = bytes_per_dim_for_typid(attr->atttypid);
    bool skip_wal;
    uint32_t data_page_count;
    uint16_t layout_m;

    memset((void *)&state, 0, sizeof(state));
    PG_TRY();
    {
        HnswDiskImportState *s = (HnswDiskImportState *)&state;
        hnsw_validate_build_target(indexRel, build);
        if (build->n_vecs == 0 || build->hnsw_fd < 0 || build->tids_fd < 0 ||
            build->n_vecs > SIZE_MAX)
            ereport(ERROR,
                    (errmsg("pg_cuvs: source-less HNSW build returned invalid files or count")));

        s->locations.n_nodes = build->n_vecs;
        s->locations.file = BufFileCreateTemp(false);
        if (!s->locations.file)
            ereport(ERROR,
                    (errmsg("pg_cuvs: cannot create disk-backed HNSW location table")));
        if (!hnsw_file_preflight(build->hnsw_fd, build->tids_fd,
                                 build->n_vecs, build->dim, build->metric,
                                 bytes_per_dim, packed_nsw, heap_m,
                                 &header, &tids_header,
                                 hnsw_location_table_append, &s->locations,
                                 hnsw_pg_check_interrupts,
                                 error, sizeof(error)))
            ereport(ERROR,
                    (errmsg("pg_cuvs: invalid source-less HNSW files: %s", error)));
        hnsw_report_source_m(source_m, (int)header.M);
        layout_m = heap_m ? heap_m : (uint16_t)header.M;
        hnsw_check_connectivity(build->hnsw_fd, &header, &connectivity);
        hnsw_notice_connectivity(&connectivity);
        if (!hnsw_location_table_rewind(&s->locations, build->n_vecs))
            ereport(ERROR,
                    (errmsg("pg_cuvs: incomplete disk-backed HNSW location table")));
        if (!hnsw_location_table_lookup(&s->locations,
                                         (uint32_t)(build->n_vecs - 1),
                                         &last_location) ||
            last_location.elem_blkno == 0 ||
            last_location.elem_blkno == InvalidBlockNumber)
            ereport(ERROR,
                    (errmsg("pg_cuvs: HNSW data page count exceeds block number limits")));
        data_page_count = hnsw_location_neigh_blkno(&last_location);	/* a split last pair ends one page later */

        /* The caller holds the lock its command takes on the index being built: AccessExclusiveLock for CREATE INDEX
         * and REINDEX, RowExclusiveLock for a concurrent build (index_concurrently_build). Asking for more here would
         * make every writer of the table wait, since a writer opens a not-yet-ready index with RowExclusiveLock too
         * (ExecOpenIndices); nobody else reads or writes the pages of an index that is not ready. */
        s->target = index_open(index_oid, NoLock);
        skip_wal = RelationGetForm(s->target)->relpersistence == RELPERSISTENCE_UNLOGGED;
        if (skip_wal)
            ereport(NOTICE,
                    (errmsg("pg_cuvs: target HNSW index is UNLOGGED — WAL skipped; "
                            "index must be rebuilt after crash recovery")));
        {
            SMgrRelation smgr = RelationGetSmgr(s->target);
            BlockNumber current = smgrnblocks(smgr, MAIN_FORKNUM);
            if (current > 0)
            {
                /* Only an AccessExclusiveLock holder may cut pages others could read; a concurrent build's new
                 * relation has none. */
                if (!CheckRelationLockedByMe(s->target, AccessExclusiveLock, false))
                    ereport(ERROR,
                            (errmsg("pg_cuvs: HNSW build target \"%s\" already has %u pages",
                                    RelationGetRelationName(s->target), current)));
                RelationTruncate(s->target, 0);
            }
            RelationSetTargetBlock(s->target, InvalidBlockNumber);
        }
        /* The receipt already records the effective serialized effort.
         * Build-time SQL options belong in the PostgreSQL metapage. */
        if (metapage_ef)
            header.efConstruction = metapage_ef;
        hnsw_write_disk_metapage(s->target, &header, layout_m, build->dim,
                                 data_page_count, skip_wal, &s->locations,
                                 /* Only direct heap builds carry a native receipt. */
                                 heap_m ? &build->policy : NULL,
                                 &build->ace, &build->nn, &connectivity);
        hnsw_write_disk_pages(s->target, build->hnsw_fd, build->tids_fd,
                              &header, layout_m, build->n_vecs, build->dim,
                              bytes_per_dim, data_page_count, skip_wal,
                              &s->locations);
        index_close(s->target, NoLock);
        s->target = NULL;
    }
    PG_FINALLY();
    {
        HnswDiskImportState *s = (HnswDiskImportState *)&state;
        if (s->target)
        {
            index_close(s->target, NoLock);
            s->target = NULL;
        }
        if (s->locations.file)
        {
            BufFileClose(s->locations.file);
            s->locations.file = NULL;
        }
    }
    PG_END_TRY();
}

/* ================================================================
 * pg_cuvs_import_cagra — Phase 3J: direct CAGRA → pgvector HNSW
 *
 * Skips the hnswlib intermediate format of mode 'hnswlib'. For 'nsw' and
 * 'hnsw' the daemon builds the upper layers over a copy of the CAGRA graph
 * (CUVS_OP_EXPORT_HNSW_UPPER) and the heap build's importer writes the
 * pages; if the daemon builds none (CUVS_STATUS_HNSW_UPPER_SKIPPED), CAGRA
 * adjacency + corpus vectors come over IPC and the levels are drawn here.
 *
 * Does NOT require cuvs.cpu_hnsw_fallback=on.
 * Supports UNLOGGED target for faster import (see pg_cuvs_import_hnsw).
 * ================================================================ */

/* ----------------------------------------------------------------
 * Squared L2 distance between two float32 vectors.
 * Used by heuristic_select_neighbors; squared form is sufficient
 * for comparisons (monotone with actual L2).
 * ---------------------------------------------------------------- */
static float
vec_dist_sq(const float *a, const float *b, int dim)
{
    float d = 0.0f;
    for (int k = 0; k < dim; k++)
    {
        float diff = a[k] - b[k];
        d += diff * diff;
    }
    return d;
}

/* ----------------------------------------------------------------
 * HNSW heuristic neighbor selection (Algorithm 4, Malkov & Yashunin 2018).
 *
 * For each candidate c (processed nearest-first to q), add c to the
 * result only if d(q, c) < d(c, s) for ALL s already selected.
 * This ensures selected neighbors cover diverse directions from q rather
 * than clustering in one region.
 *
 * candidates    : CAGRA neighbor indices, assumed approximately sorted by
 *                 distance to q (NSG adjacency is nearest-first in practice).
 * n_candidates  : number of candidates
 * M             : max neighbors to select
 * out_selected  : output array of size >= M
 * returns       : number of neighbors selected (<= M)
 * ---------------------------------------------------------------- */
static int
heuristic_select_neighbors(
    const float *query_vec,
    const float *all_vecs,
    int          dim,
    const int   *candidates,
    int          n_candidates,
    int          M,
    int         *out_selected)
{
    int n_sel = 0;
    for (int i = 0; i < n_candidates && n_sel < M; i++)
    {
        int          c     = candidates[i];
        const float *c_vec = all_vecs + (size_t)c * dim;
        float        d_q_c = vec_dist_sq(query_vec, c_vec, dim);

        /* c is kept only if it is closer to q than to every selected s */
        bool dominated = false;
        for (int j = 0; j < n_sel && !dominated; j++)
        {
            const float *s_vec = all_vecs + (size_t)out_selected[j] * dim;
            /* hnswlib condition: reject if any selected s is closer to c than q is
             * (strict < matches getNeighborsByHeuristic2 in hnswalg.h line 470) */
            if (vec_dist_sq(c_vec, s_vec, dim) < d_q_c)
                dominated = true;
        }

        if (!dominated)
            out_selected[n_sel++] = c;
    }
    return n_sel;
}

/* ----------------------------------------------------------------
 * Level assignment helper for HNSW mode.
 * Uses geometric distribution: level = floor(-log(r) / log(M)).
 * Matches hnswlib / from_cagra() convention.
 * ---------------------------------------------------------------- */
static int
cagra_assign_level(int M, unsigned int *seed)
{
    double mL = 1.0 / log((double)(M > 1 ? M : 2));
    /* (0,1) exclusive to avoid log(0) */
    double r = ((double)rand_r(seed) + 1.0) / ((double)RAND_MAX + 2.0);
    int lv   = (int)(-log(r) * mL);
    return (lv > 16) ? 16 : lv;   /* cap to avoid degenerate indexes */
}

/* Internal helper: fill an existing HNSW index from CAGRA adjacency via IPC.
 * Called by pg_cuvs_build_hnsw() for modes 'nsw' and 'hnsw'.
 * mode='nsw' and mode='hnsw' → upper layers from the daemon (CUVS_OP_EXPORT_HNSW_UPPER); levels drawn here and
 * heuristic neighbor selection among each row's CAGRA neighbors only when it builds none ('nsw' was level 0 only) */
static void
fill_hnsw_from_cagra_ipc_impl(Oid cagra_oid, Oid hnsw_oid, const char *mode,
                             int source_m, uint16_t metapage_ef, HnswFillRes *res)
{
    /* 'nsw' assigns levels and links the upper layers too. A graph with layer 0 only lets pgvector's insert make a new element
     * the entry point (hnswinsert.c, HNSW_UPDATE_ENTRY_GREATER) and link the upper layers among inserted rows only, so a search
     * descends to them and misses the converted rows when the inserted rows lie elsewhere. */
    bool do_hierarchy = (strcmp(mode, "hnsw") == 0 || strcmp(mode, "nsw") == 0);

    /* ---- 0. pgvector compatibility check ---- */
    {
        Oid vec_oid = get_extension_oid("vector", true);
        if (!OidIsValid(vec_oid))
            ereport(ERROR,
                    (errmsg("pg_cuvs: pgvector extension is not installed; "
                            "pg_cuvs_import_cagra requires pgvector 0.5.0+"),
                     errhint("Run: CREATE EXTENSION vector;")));
        cuvs_warn_pgvector_version();   /* Wave 1: version-range guard */
    }

    /* ---- 1. Source CAGRA index: get dim ---- */
    Relation cagra_rel = index_open(cagra_oid, AccessShareLock);
    int dim = 0;
    if (RelationGetDescr(cagra_rel)->natts >= 1)
        dim = (int)TupleDescAttr(RelationGetDescr(cagra_rel), 0)->atttypmod;
    if (dim <= 0)
        ereport(ERROR,
                (errmsg("pg_cuvs: cannot determine vector dimension from cagra "
                        "index %u", cagra_oid)));
    index_close(cagra_rel, AccessShareLock);

    /* ---- 1b. Validate target index (AM, dim, metric) ---- */
    bool hnsw_unlogged = false;
    uint32_t tgt_metric = CUVS_METRIC_L2;
    int  bytes_per_dim = 4;  /* #162: set from the target's actual type below */
    {
        HeapTuple tup = SearchSysCache1(RELOID, ObjectIdGetDatum(hnsw_oid));
        if (!HeapTupleIsValid(tup))
            ereport(ERROR,
                    (errmsg("pg_cuvs: target relation %u not found", hnsw_oid)));
        Form_pg_class cf = (Form_pg_class) GETSTRUCT(tup);

        if (cf->relkind != RELKIND_INDEX)
        {
            char *name = pstrdup(NameStr(cf->relname));
            ReleaseSysCache(tup);
            ereport(ERROR, (errmsg("pg_cuvs: \"%s\" is not an index", name)));
        }
        Oid hnsw_amoid      = get_am_oid("hnsw", true);
        Oid cuvs_hnsw_amoid = get_am_oid("pg_cuvs_hnsw", true);  /* Phase 3K AM */
        if (cf->relam != hnsw_amoid && cf->relam != cuvs_hnsw_amoid)
        {
            char *name = pstrdup(NameStr(cf->relname));
            ReleaseSysCache(tup);
            ereport(ERROR,
                    (errmsg("pg_cuvs: \"%s\" is not a pgvector HNSW index", name)));
        }
        /* Dimension check */
        {
            int tgt_dim = 0;
            HeapTuple attup = SearchSysCache2(ATTNUM,
                                               ObjectIdGetDatum(hnsw_oid),
                                               Int16GetDatum(1));
            if (HeapTupleIsValid(attup))
            {
                Form_pg_attribute att = (Form_pg_attribute) GETSTRUCT(attup);
                tgt_dim = (int)att->atttypmod;
                /* #162: read atttypid before ReleaseSysCache — att points
                 * into the tuple, invalid after release. */
                bytes_per_dim = bytes_per_dim_for_typid(att->atttypid);
                ReleaseSysCache(attup);
            }
            if (tgt_dim > 0 && tgt_dim != dim)
            {
                char *name = pstrdup(NameStr(cf->relname));
                ReleaseSysCache(tup);
                ereport(ERROR,
                        (errmsg("pg_cuvs: dimension mismatch: source CAGRA dim=%d "
                                "but target HNSW \"%s\" dim=%d", dim, name, tgt_dim)));
            }
        }
        /* Opclass → metric */
        {
            HeapTuple idxtup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(hnsw_oid));
            if (HeapTupleIsValid(idxtup))
            {
                bool isnull;
                Datum d = SysCacheGetAttr(INDEXRELID, idxtup,
                                           Anum_pg_index_indclass, &isnull);
                if (!isnull)
                {
                    oidvector *ov = (oidvector *)DatumGetPointer(d);
                    HeapTuple opctup = SearchSysCache1(CLAOID,
                                                        ObjectIdGetDatum(ov->values[0]));
                    if (HeapTupleIsValid(opctup))
                    {
                        const char *nm = NameStr(((Form_pg_opclass)GETSTRUCT(opctup))->opcname);
                        if (strstr(nm, "cosine"))   tgt_metric = CUVS_METRIC_COSINE;
                        else if (strstr(nm, "_ip")) tgt_metric = CUVS_METRIC_IP;
                        ReleaseSysCache(opctup);
                    }
                }
                ReleaseSysCache(idxtup);
            }
        }
        hnsw_unlogged = (cf->relpersistence == RELPERSISTENCE_UNLOGGED);
        ReleaseSysCache(tup);
    }

    if (hnsw_unlogged)
        ereport(NOTICE,
                (errmsg("pg_cuvs: target HNSW is UNLOGGED — WAL skipped; "
                        "index must be rebuilt after crash recovery")));

    /* ---- 2. Fetch adjacency + vectors + tids from daemon ---- */
    if (cuvs_socket_path == NULL || cuvs_socket_path[0] == '\0')
        ereport(ERROR,
                (errmsg("pg_cuvs: daemon socket not set (cuvs.socket_path); "
                        "is pg_cuvs_server running?")));

    /* the upper layers come from the daemon, built by hnswlib insertion
     * over a copy of this graph (CUVS_OP_EXPORT_HNSW_UPPER), and are written by
     * the heap build's importer. Levels drawn here and linked among each
     * row's CAGRA neighbors leave most upper-layer elements without a link on
     * level 1. This path is kept for graphs the daemon builds no upper layers
     * for (CUVS_STATUS_HNSW_UPPER_SKIPPED). */
    if (do_hierarchy)
    {
        size_t up_n = 0;
        int    up_degree = 0, up_dim = 0;
        int    urc = cuvs_ipc_export_hnsw_upper(cuvs_socket_path, (uint32_t)MyDatabaseId,
                                                (uint32_t)cagra_oid, &res->shm_fd,
                                                &res->tids_fd, &up_n, &up_degree, &up_dim);

        if (urc == CUVS_STATUS_OK)
        {
            CuvsTidsHeader th;
            CuvsHnswBuildResult build;
            Relation target;

            if (pread(res->tids_fd, &th, sizeof(th), 0) != (ssize_t) sizeof(th))
                ereport(ERROR,
                        (errmsg("pg_cuvs: cannot read the TID header from the daemon")));
            if (th.metric != tgt_metric)
            {
                static const char *mname[] = {"l2", "cosine", "ip"};
                ereport(ERROR,
                        (errmsg("pg_cuvs: metric mismatch: source CAGRA uses %s but "
                                "target HNSW uses %s",
                                th.metric < 3 ? mname[th.metric] : "unknown",
                                tgt_metric < 3 ? mname[tgt_metric] : "unknown")));
            }
            if (up_dim != dim)
                ereport(ERROR,
                        (errmsg("pg_cuvs: dimension mismatch: daemon graph dim=%d but "
                                "source CAGRA dim=%d", up_dim, dim)));
            memset(&build, 0, sizeof(build));
            build.hnsw_fd = res->shm_fd;
            build.tids_fd = res->tids_fd;
            build.n_vecs  = up_n;
            build.dim     = (uint32_t) up_dim;
            build.metric  = tgt_metric;
            hnsw_check_exported_tids(res, bytes_per_dim);
            if (res->tid_result != HNSW_SOURCE_TIDS_UNIQUE)
                return;
            target = index_open(hnsw_oid, AccessExclusiveLock);
            hnsw_import_build_result(target, &build, false, 0, source_m, metapage_ef);
            index_close(target, AccessExclusiveLock);
            ereport(NOTICE,
                    (errmsg("pg_cuvs: direct import %zu elements (dim=%d, M=%d, "
                            "graph_degree=%d, upper layers built by the daemon, mode=%s) "
                            "from cagra index %u into hnsw index %u",
                            up_n, up_dim, up_degree / 2, up_degree, mode,
                            cagra_oid, hnsw_oid)));
            res->n_elems = up_n;    /* the element count returned; elems stays NULL */
            return;
        }
        if (urc == CUVS_STATUS_CANCELED)
            CHECK_FOR_INTERRUPTS();     /* the wait stopped for a pending cancel */
        if (urc != CUVS_STATUS_HNSW_UPPER_SKIPPED)
        {
            cuvs_report_proto_mismatch(urc);
            ereport(ERROR,
                    (errmsg("pg_cuvs: IPC export_hnsw_upper failed (status=%d); "
                            "ensure index %u is loaded in daemon", urc, cagra_oid),
                     urc == CUVS_STATUS_ERROR
                     ? errhint("A daemon older than this extension answers this request with status 1 "
                               "(unknown op): restart pg_cuvs_server after installing the extension.")
                     : 0));
        }
    }

    uint32_t *adj   = NULL;
    float    *vecs  = NULL;
    uint64_t *tids  = NULL;
    size_t    N;
    int       graph_degree, ipc_dim;
    uint32_t  src_metric;

    int rc = cuvs_ipc_export_adjacency(
        cuvs_socket_path,
        (uint32_t)MyDatabaseId,
        (uint32_t)cagra_oid,
        &adj, &vecs, &tids,
        &N, &graph_degree, &ipc_dim,
        &src_metric);

    if (rc != CUVS_STATUS_OK)
    {
        cuvs_report_proto_mismatch(rc);
        ereport(ERROR,
                (errmsg("pg_cuvs: IPC export_adjacency failed (status=%d); "
                        "ensure index %u is loaded in daemon", rc, cagra_oid)));
    }
    res->adj  = adj;
    res->vecs = vecs;
    res->tids = tids;
    res->tid_result = hnsw_array_tid_result(tids, vecs, N, (uint32_t) ipc_dim);
    if (res->tid_result != HNSW_SOURCE_TIDS_UNIQUE)
        return;

    /* Metric mismatch check */
    if (src_metric != tgt_metric)
    {
        static const char *mname[] = {"l2", "cosine", "ip"};
        const char *sm = (src_metric < 3) ? mname[src_metric] : "unknown";
        const char *tm = (tgt_metric < 3) ? mname[tgt_metric] : "unknown";
        ereport(ERROR,
                (errmsg("pg_cuvs: metric mismatch: source CAGRA uses %s but "
                        "target HNSW uses %s", sm, tm)));
    }

    /* ---- 3. Build ElemDesc array ---- */
    /* M0 = level-0 neighbor slots (pgvector: 2*M).
     * Clamp graph_degree to M0 = min(graph_degree, 64); M = M0/2. */
    int M0 = (graph_degree < 64) ? graph_degree : 64;
    if (M0 % 2 != 0) M0--;
    if (M0 < 2) M0 = 2;
    int M = M0 / 2;
    hnsw_report_source_m(source_m, M);

    ElemDesc *elems = (ElemDesc *)calloc(N, sizeof(ElemDesc));
    if (!elems)
    {
        ereport(ERROR, (errmsg("pg_cuvs: out of memory for element descriptors")));
    }
    res->elems       = elems;
    res->n_elems     = N;
    res->elems_maxM0 = (size_t)M0;

    int    max_level  = 0;
    size_t entry_elem = 0;

    if (do_hierarchy)
    {
        /* ---- 3a. Assign levels (geometric distribution, same as hnswlib) ---- */
        unsigned int seed = (unsigned int)(N * 31337u ^ (size_t)M * 13u);
        for (size_t i = 0; i < N; i++)
        {
            elems[i].level = cagra_assign_level(M, &seed);
            if (elems[i].level > max_level)
            {
                max_level  = elems[i].level;
                entry_elem = i;
            }
        }
    }
    /* else: all elems[i].level remain 0 (calloc'd), max_level=0, entry_elem=0 */

    /* ---- 3b. Level-0 and upper-level links ---- */
    for (size_t i = 0; i < N; i++)
    {
        elems[i].tid_encoded = tids[i];
        const uint32_t *row = adj + i * (size_t)graph_degree;

        /* Level 0: up to M0 CAGRA neighbors */
        int cnt0 = (graph_degree < M0) ? graph_degree : M0;
        elems[i].lv0_count = cnt0;
        if (cnt0 > 0)
        {
            elems[i].lv0_links = (int *)malloc((size_t)cnt0 * sizeof(int));
            if (!elems[i].lv0_links)
            {
                ereport(ERROR, (errmsg("pg_cuvs: out of memory for lv0_links")));
            }
            for (int j = 0; j < cnt0; j++)
                elems[i].lv0_links[j] = (int)row[j];
        }

        /* Upper levels (only when do_hierarchy and level > 0) */
        int lv = elems[i].level;
        if (do_hierarchy && lv > 0)
        {
            elems[i].upper_links  = (int **)calloc((size_t)lv, sizeof(int *));
            elems[i].upper_counts = (int  *)calloc((size_t)lv, sizeof(int));
            if (!elems[i].upper_links || !elems[i].upper_counts)
            {
                ereport(ERROR, (errmsg("pg_cuvs: out of memory for upper_links")));
            }
            /* Convert CAGRA uint32_t adjacency to int candidates for heuristic */
            int *cand_buf = (int *)malloc((size_t)graph_degree * sizeof(int));
            int  n_cand   = 0;
            if (!cand_buf)
            {
                ereport(ERROR, (errmsg("pg_cuvs: out of memory for heuristic candidates")));
            }
            res->cand_buf = cand_buf;
            for (int j = 0; j < graph_degree; j++)
            {
                uint32_t c = row[j];
                if ((size_t)c < N)
                    cand_buf[n_cand++] = (int)c;
            }

            for (int l = 1; l <= lv; l++)
            {
                int keep = 0;

                elems[i].upper_links[l - 1]  = (int *)malloc((size_t)M * sizeof(int));
                if (!elems[i].upper_links[l - 1])
                {
                    ereport(ERROR, (errmsg("pg_cuvs: out of memory for upper_links[%d]", l)));
                }
                /*
                 * pgvector requires that an element linked at level lc has
                 * level >= lc: HnswGetNeighborArray computes
                 * start = (level - lc) * m and would read before the neighbor
                 * array for a level-0 element parked in a level-3 list.
                 * Levels here come from cagra_assign_level (geometric), so most
                 * candidates are level 0 and must be dropped.
                 *
                 * The filter is monotone in l, so compacting cand_buf in place
                 * carries over to the next iteration: level l+1 sees a strict
                 * subset of level l's candidates.  That is also what gives each
                 * level a distinct neighbor list — heuristic_select_neighbors is
                 * deterministic, so an unchanged candidate set would hand every
                 * upper level the exact same links.
                 */
                for (int j = 0; j < n_cand; j++)
                    if (elems[cand_buf[j]].level >= l)
                        cand_buf[keep++] = cand_buf[j];
                n_cand = keep;

                /* Heuristic neighbor selection: diverse coverage of directions */
                int n_sel = heuristic_select_neighbors(
                    vecs + (size_t)i * ipc_dim,
                    vecs,
                    ipc_dim,
                    cand_buf,
                    n_cand,
                    M,
                    elems[i].upper_links[l - 1]);
                elems[i].upper_counts[l - 1] = n_sel;
            }
            free(cand_buf);
            res->cand_buf = NULL;
        }
    }
    free(adj);
    res->adj = NULL;
    free(tids);
    res->tids = NULL;

    /* ---- 4. Layout pass ---- */
    /* Verify elem+neigh fit on one page (worst case: max_level, M neighbors) before the layout below, so a pair that cannot fit
     * at all is the user-facing page-fit ERROR rather than the layout's internal one. */
    check_hnsw_page_fit((int)ipc_dim, bytes_per_dim, M, max_level);

    /* #161: pack element+neighbor pairs onto pages in element order, each pair taking its own size (a level-l element's
     * neighbor tuple has (l + 2) * M slots), with the cursor the heap build's importer uses (hnsw_pack_location_next). With every
     * element at level 0 this is the fixed k per page of #161; upper-layer elements take more room. */
    {
        HnswPackingCursor packing = {0};

        for (size_t i = 0; i < N; i++)
        {
            HnswNodeLocation location;

            if (!hnsw_pack_location_next(&packing, (uint64_t) i, (uint64_t) N, elems[i].level,
                                         (int) ipc_dim, bytes_per_dim, M, &location))
                ereport(ERROR,
                        (errmsg("pg_cuvs: HNSW element %zu (level %d) does not fit the page layout "
                                "(dim=%d, bytes_per_dim=%d, M=%d)",
                                i, elems[i].level, (int) ipc_dim, bytes_per_dim, M)));
            elems[i].elem_blkno  = location.elem_blkno;
            elems[i].elem_offno  = location.elem_offno;
            elems[i].neigh_blkno = hnsw_location_neigh_blkno(&location);
            elems[i].neigh_offno = hnsw_location_neigh_offno(&location);
        }
    }

    /* ---- 5. Open target HNSW with AccessExclusiveLock and truncate ---- */
    Relation hnsw_rel = index_open(hnsw_oid, AccessExclusiveLock);
    {
        /* RelationTruncate, not a bare smgrtruncate — see the matching comment
         * in fill_hnsw_from_hnswlib_impl for why the WAL record matters. */
        SMgrRelation smgr = RelationGetSmgr(hnsw_rel);
        BlockNumber  cur  = smgrnblocks(smgr, MAIN_FORKNUM);
        if (cur > 0)
            RelationTruncate(hnsw_rel, 0);
        RelationSetTargetBlock(hnsw_rel, InvalidBlockNumber);
    }

    CuvsConnectivity connectivity=hnsw_check_elements(elems,N,(int)entry_elem);
    hnsw_notice_connectivity(&connectivity);
    /* ---- 6. Write metapage (block 0) ---- */
    {
        Page page = (Page)palloc(BLCKSZ);
        PageInit(page, BLCKSZ, sizeof(PgvHnswPageOpaque));

        PgvHnswPageOpaque *opaque =
            (PgvHnswPageOpaque *) PageGetSpecialPointer(page);
        opaque->nextblkno = InvalidBlockNumber;
        opaque->unused    = 0;
        opaque->page_id   = PGV_HNSW_PAGE_ID;

        PgvHnswMeta *metap = (PgvHnswMeta *) PageGetContents(page);
        memset(metap, 0, sizeof(*metap));
        metap->magicNumber    = PGV_HNSW_MAGIC;
        metap->version        = PGV_HNSW_VERSION;
        metap->dimensions     = (uint16_t)ipc_dim;
        metap->m              = (uint16_t)M;
        metap->efConstruction = metapage_ef ? metapage_ef : (uint16_t)(M0 * 2);
        if (N > 0)
        {
            /* entry_elem is the node with highest level (0 for flat NSW).
             * #161: entryOffno used to be hard-coded 1, correct only while
             * every element had the whole page to itself; under packing an
             * entry element can land at any slot on its page. */
            metap->entryBlkno = elems[entry_elem].elem_blkno;
            metap->entryOffno = elems[entry_elem].elem_offno;
            metap->entryLevel = (int16_t)max_level;
        }
        else
        {
            metap->entryBlkno = InvalidBlockNumber;
            metap->entryOffno = 0;
            metap->entryLevel = -1;
        }
        /* #161: insertPage tells pgvector's own future single-row INSERT
         * which page to try appending to. It used to be N (block N = the
         * last element's own block, since block == i+1). Under packing the
         * last element's actual block is elems[N-1].elem_blkno
         * (= 1 + (N-1)/pack_k) — pointing INSERT at the true last page,
         * not one past it, preserves the "append into free space" property
         * pgvector's own writer already relies on (hnswinsert.c). */
        metap->insertPage = (N > 0) ? elems[N - 1].neigh_blkno : (uint32_t) N;	/* the last page, also when the last pair is split */

        ((PageHeader)page)->pd_lower =
            (uint16_t)(((char *)metap + sizeof(PgvHnswMeta)) - (char *)page);
        hnsw_stage_metapage_tail(hnsw_rel,page,&connectivity,false);

        hnsw_publish_staged_page(hnsw_rel, page, hnsw_unlogged);
        pfree(page);
    }

    /* ---- 7. Write element+neighbor pages ---- */
    write_elem_pages_packed(hnsw_rel, elems, vecs, ipc_dim, bytes_per_dim,
                            M, N, hnsw_unlogged,
                            hnsw_lookup_elem_array, elems);

    /* vecs / elems are released by the PG_FINALLY in fill_hnsw_from_cagra_ipc(). */

    index_close(hnsw_rel, AccessExclusiveLock);

    ereport(NOTICE,
            (errmsg("pg_cuvs: direct import %zu elements (dim=%d, M=%d, "
                    "graph_degree=%d, max_level=%d, mode=%s) "
                    "from cagra index %u into hnsw index %u",
                    N, ipc_dim, M, graph_degree, max_level, mode,
                    cagra_oid, hnsw_oid)));
}

/* #161: returns the actual element count — see the matching comment on
 * fill_hnsw_from_hnswlib(). */
static size_t
fill_hnsw_from_cagra_ipc(Oid cagra_oid, Oid hnsw_oid, const char *mode,
                        int source_m, uint16_t metapage_ef, HnswSourceTidResult *tid_result)
{
    /* #166 review 3: volatile across the longjmp — see fill_hnsw_from_hnswlib. */
    volatile HnswFillRes res;

    memset((void *) &res, 0, sizeof(res));
    res.shm_fd = -1;            /* #165: 0 is stdin — never a valid sentinel */
    res.tids_fd = -1;
    PG_TRY();
    {
        fill_hnsw_from_cagra_ipc_impl(cagra_oid, hnsw_oid, mode, source_m, metapage_ef,
                                      (HnswFillRes *) &res);
    }
    PG_FINALLY();
    {
        hnsw_fill_res_free((HnswFillRes *) &res);
    }
    PG_END_TRY();
    *tid_result = res.tid_result;
    return res.n_elems;
}

static size_t
hnsw_fill_source(Oid source, Oid target, const char *mode, int source_m,
                 uint16_t metapage_ef, HnswSourceTidResult *tid_result)
{
    if (strcmp(mode, "nsw") == 0 || strcmp(mode, "hnsw") == 0)
        return fill_hnsw_from_cagra_ipc(source, target, mode, source_m, metapage_ef, tid_result);
    return fill_hnsw_from_hnswlib(source, target, strcmp(mode, "hnswlib") == 0,
                                  source_m, metapage_ef, tid_result);
}

/* ================================================================
 * pg_cuvs_build_hnsw — unified GPU import: creates HNSW WITHOUT CPU build.
 *
 * Creates the target with INDEX_CREATE_SKIP_BUILD. Import normally populates
 * it directly. Redundant source entries require an ordinary heap build.
 *
 * Usage: SELECT pg_cuvs_build_hnsw('my_cagra'::regclass, 'hnsw');
 * Returns: OID of the newly created HNSW index (regclass).
 * ================================================================ */

/* ---- find HNSW opclass OID by metric ---- */
static Oid
find_hnsw_opclass_oid(uint32_t metric)
{
    const char *opcname;
    switch (metric) {
        case CUVS_METRIC_COSINE: opcname = "vector_cosine_ops"; break;
        case CUVS_METRIC_IP:     opcname = "vector_ip_ops";     break;
        default:                  opcname = "vector_l2_ops";     break;
    }
    Oid hnsw_amoid = get_am_oid("hnsw", true);
    if (!OidIsValid(hnsw_amoid))
        ereport(ERROR, (errmsg("pg_cuvs: pgvector hnsw AM not found")));

    Relation    opcrel = table_open(OperatorClassRelationId, AccessShareLock);
    SysScanDesc sc     = systable_beginscan(opcrel, InvalidOid, false, NULL, 0, NULL);
    HeapTuple   tup;
    Oid         result = InvalidOid;
    while (HeapTupleIsValid(tup = systable_getnext(sc))) {
        Form_pg_opclass opc = (Form_pg_opclass) GETSTRUCT(tup);
        if (opc->opcmethod == hnsw_amoid &&
            strcmp(NameStr(opc->opcname), opcname) == 0) {
            result = opc->oid;
            break;
        }
    }
    systable_endscan(sc);
    table_close(opcrel, AccessShareLock);

    if (!OidIsValid(result))
        ereport(ERROR,
                (errmsg("pg_cuvs: HNSW opclass '%s' not found; install pgvector",
                        opcname)));
    return result;
}

/* ---- detect CAGRA metric from its opclass name ---- */
static uint32_t
cagra_index_metric(Oid cagra_oid)
{
    HeapTuple idxtup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(cagra_oid));
    if (!HeapTupleIsValid(idxtup)) return CUVS_METRIC_L2;
    bool isnull = false;
    Datum d   = SysCacheGetAttr(INDEXRELID, idxtup, Anum_pg_index_indclass, &isnull);
    oidvector *ov = (oidvector *) DatumGetPointer(d);
    Oid cagra_opc = ov->values[0];
    ReleaseSysCache(idxtup);

    HeapTuple opctup = SearchSysCache1(CLAOID, ObjectIdGetDatum(cagra_opc));
    if (!HeapTupleIsValid(opctup)) return CUVS_METRIC_L2;
    const char *nm = NameStr(((Form_pg_opclass) GETSTRUCT(opctup))->opcname);
    uint32_t metric = CUVS_METRIC_L2;
    if (strstr(nm, "cosine"))   metric = CUVS_METRIC_COSINE;
    else if (strstr(nm, "_ip")) metric = CUVS_METRIC_IP;
    ReleaseSysCache(opctup);
    return metric;
}

/* ---- create empty HNSW on parent table, no CPU build ---- */
static Oid
create_empty_hnsw(Relation heap_rel, Relation cagra_rel)
{
    Oid      cagra_oid    = RelationGetRelid(cagra_rel);
    uint32_t metric       = cagra_index_metric(cagra_oid);
    Oid      hnsw_opclass = find_hnsw_opclass_oid(metric);
    Form_pg_attribute att = TupleDescAttr(RelationGetDescr(cagra_rel), 0);
    char  *col_name  = pstrdup(NameStr(att->attname));
    Oid    collation = cagra_rel->rd_indcollation[0];
    int16  coloption = 0;

    IndexInfo *iinfo = BuildIndexInfo(cagra_rel);

    /* Include a sequence so multiple calls with the same CAGRA produce unique names. */
    static int hnsw_build_seq = 0;
    char *idx_name = psprintf("pg_cuvs_hnsw_%u_%d", cagra_oid,
                               __atomic_fetch_add(&hnsw_build_seq, 1, __ATOMIC_RELAXED));

    /* INDEX_CREATE_SKIP_BUILD: catalog entries created, ambuild() skipped.
     * pgvector CPU build (285s for 1M×1024) is never called. */
    Oid tablespace = heap_rel->rd_rel->reltablespace;
    if (!OidIsValid(tablespace))
        tablespace = MyDatabaseTableSpace;

    Oid hnsw_oid = index_create(
        heap_rel,
        idx_name,
        InvalidOid,           /* auto-assign OID */
        InvalidOid,           /* parentIndexRelid */
        InvalidOid,           /* parentConstraintId */
        InvalidRelFileNumber, /* auto relfilenode */
        iinfo,
        list_make1(makeString(col_name)),
        get_am_oid("hnsw", false),
        tablespace,
        &collation,           /* collationObjectId */
        &hnsw_opclass,        /* classObjectId */
        &coloption,           /* coloptions */
        (Datum) 0,            /* reloptions: defaults (m=16, ef=64) */
        INDEX_CREATE_SKIP_BUILD,  /* flags: skip pgvector CPU build */
        0,                    /* constr_flags */
        false,                /* allow_system_table_mods */
        true,                 /* is_internal */
        NULL);                /* constraintId */

    CommandCounterIncrement();
    return hnsw_oid;
}

static void
hnsw_require_legacy_cagra_source(Relation source)
{
#ifdef CUVS_PAGE_FORMAT
    if (cuvs_cagra_has_pages(source))
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("page-format CAGRA is not yet a supported HNSW source"),
                        errhint("Build the HNSW index from the heap without a source.")));
#else
    cuvs_cagra_require_legacy(source);
#endif
}

/* A pending file with a valid empty header contains no work. Treat missing
 * files as empty, but refuse files whose emptiness cannot be established. */
static bool
hnsw_pending_file_empty(const char *stem,
                        const char *suffix)
{
    char path[MAXPGPATH];
    struct stat st;
    FILE *file;
    bool empty = false;

    if (snprintf(path, sizeof(path), "%s.%s", stem, suffix) >= sizeof(path))
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                        errmsg("pg_cuvs: CAGRA source path is too long")));
    file = AllocateFile(path, PG_BINARY_R);
    if (file == NULL && errno == ENOENT)
        return true;
    if (file != NULL)
    {
        if (fstat(fileno(file), &st) == 0 && S_ISREG(st.st_mode))
        {
            if (strcmp(suffix, "delta") == 0)
            {
                CuvsDeltaHeader header;
                empty = st.st_size == sizeof(header) &&
                    cuvs_delta_read_header(file, &header) == 0 &&
                    cuvs_delta_validate(&header, 0) == 0;
            }
            else
            {
                CuvsTombstoneHeader header;
                empty = st.st_size == sizeof(header) &&
                    cuvs_tombstone_read_header(file, &header) == 0 &&
                    cuvs_tombstone_validate(&header, 0) == 0;
            }
        }
        FreeFile(file);
    }
    return empty;
}

static const char *
hnsw_cagra_source_problem(Relation source, Relation heap)
{
    char stem[MAXPGPATH];
    char path[MAXPGPATH];
    struct stat st;
    FILE *file;
    uint32 heap_rfn, table_oid;
    bool same_heap;

    hnsw_require_legacy_cagra_source(source);
    if (!source->rd_index->indisvalid || !source->rd_index->indislive)
        return "is not valid";

    if ((size_t) snprintf(stem, sizeof(stem), "%s/%u_%u",
                          cuvs_resolve_index_dir_rel(source), MyDatabaseId,
                          RelationGetRelid(source)) > sizeof(path) - sizeof(".relfilenode"))
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                        errmsg("pg_cuvs: CAGRA source path is too long")));
    snprintf(path, sizeof(path), "%s.stale", stem);
    if (stat(path, &st) == 0)
        return "is stale";
    if (errno != ENOENT)
        return "has an unreadable stale marker";

    /* The sidecar describes the heap whose TIDs were exported by CAGRA.
     * VACUUM FULL and CLUSTER can rebuild the HNSW target before CAGRA. */
    snprintf(path, sizeof(path), "%s.relfilenode", stem);
    file = AllocateFile(path, PG_BINARY_R);
    same_heap = file != NULL &&
        fscanf(file, "%u %u", &heap_rfn, &table_oid) == 2 &&
        table_oid == RelationGetRelid(heap) && heap_rfn == heap->rd_rel->relfilenode;
    if (file != NULL)
        FreeFile(file);
    if (!same_heap)
        return "does not match the current heap generation";

    if (!hnsw_pending_file_empty(stem, "delta"))
        return "has pending inserts or an unreadable delta file";
    if (!hnsw_pending_file_empty(stem, "tombstone"))
        return "has pending deletes or an unreadable tombstone file";
    return NULL;
}

static void
hnsw_report_source_problem(Relation source, const char *problem)
{
    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("pg_cuvs: source CAGRA index \"%s\" %s",
                    RelationGetRelationName(source), problem),
             errhint("REINDEX the source CAGRA index before converting it, or build the HNSW index from the heap.")));
}

static bool
hnsw_file_source_available(Relation source)
{
    const char *dir = cuvs_resolve_index_dir_rel(source);
    char graph_path[MAXPGPATH];
    char tids_path[MAXPGPATH];
    FILE *graph;
    FILE *tids;
    bool available;

    if ((size_t) snprintf(graph_path, sizeof(graph_path), "%s/%u_%u.hnsw",
                          dir, MyDatabaseId, RelationGetRelid(source)) >= sizeof(graph_path) ||
        (size_t) snprintf(tids_path, sizeof(tids_path), "%s/%u_%u.tids",
                          dir, MyDatabaseId, RelationGetRelid(source)) >= sizeof(tids_path))
        return false;
    graph = AllocateFile(graph_path, PG_BINARY_R);
    tids = AllocateFile(tids_path, PG_BINARY_R);
    available = graph != NULL && tids != NULL &&
                hnsw_source_binding_matches(graph_path, graph, tids);
    if (graph != NULL)
        FreeFile(graph);
    if (tids != NULL)
        FreeFile(tids);
    return available;
}

/* The existing vector -> halfvec import applies the pgvector cast in the
 * page writer. Keep that conversion when it wraps exactly the source key. */
static bool
hnsw_source_key_matches(Relation source, Relation target)
{
    AttrNumber src_attno = source->rd_index->indkey.values[0];
    AttrNumber dst_attno = target->rd_index->indkey.values[0];
    List *src_exprs = RelationGetIndexExpressions(source);
    List *dst_exprs = RelationGetIndexExpressions(target);
    Form_pg_attribute src = TupleDescAttr(RelationGetDescr(source), 0);
    Form_pg_attribute dst = TupleDescAttr(RelationGetDescr(target), 0);
    FuncExpr *cast;
    HeapTuple cast_tuple;
    Oid cast_function;
    Node *source_key;

    if (src_attno == dst_attno && equal(src_exprs, dst_exprs))
        return true;
    if (dst_attno != 0 || list_length(dst_exprs) != 1 ||
        !IsA(linitial(dst_exprs), FuncExpr) ||
        bytes_per_dim_for_typid(dst->atttypid) != 2 ||
        src->atttypmod != dst->atttypmod)
        return false;

    cast = linitial_node(FuncExpr, dst_exprs);
    cast_tuple = SearchSysCache2(CASTSOURCETARGET,
                                ObjectIdGetDatum(src->atttypid),
                                ObjectIdGetDatum(dst->atttypid));
    if (!HeapTupleIsValid(cast_tuple))
        return false;
    cast_function = ((Form_pg_cast) GETSTRUCT(cast_tuple))->castfunc;
    ReleaseSysCache(cast_tuple);
    if (cast->funcid != cast_function || cast->args == NIL)
        return false;

    source_key = src_attno != 0
        ? (Node *) makeVar(1, src_attno, src->atttypid, src->atttypmod,
                          src->attcollation, 0)
        : linitial(src_exprs);
    return equal(source_key, linitial(cast->args));
}

PG_FUNCTION_INFO_V1(pg_cuvs_build_hnsw);
Datum
pg_cuvs_build_hnsw(PG_FUNCTION_ARGS)
{
    Oid         cagra_oid = PG_GETARG_OID(0);
    const char *mode = (PG_NARGS() >= 2 && !PG_ARGISNULL(1)) ?
                       text_to_cstring(PG_GETARG_TEXT_PP(1)) : "nsw";
    Oid heap_oid = IndexGetRelation(cagra_oid, false);
    Relation heap_rel;
    Relation cagra_rel;
    int graph_degree;
    int dim;
    Oid hnsw_oid;

    /* Phase 3K (ADR-038): the function form is deprecated in favor of the
     * CREATE INDEX ... USING pg_cuvs_hnsw DDL, which integrates with the
     * catalog, DROP INDEX/REINDEX, and pg_dump. Kept working for back-compat. */
    ereport(NOTICE,
            (errmsg("pg_cuvs_build_hnsw() is deprecated; prefer "
                    "CREATE INDEX ... USING pg_cuvs_hnsw (col opclass) "
                    "WITH (source = '<cagra index>', mode = '<mode>')"),
             errhint("The DDL form integrates with pg_indexes, DROP INDEX, "
                     "REINDEX, and pg_dump.")));

    /* #162: dim-ceiling pre-check, before any GPU work. This deprecated
     * function form only ever creates a vector (fp32) target — see
     * find_hnsw_opclass_oid()/create_empty_hnsw() below, which hardcode
     * vector_*_ops opclasses — so bytes_per_dim is always 4 here; a halfvec
     * target requires the CREATE INDEX ... USING pg_cuvs_hnsw DDL form
     * instead (cuvs_hnsw_ambuild(), which carries its own pre-check).
     *
     * Validate cagra_oid IS a cagra index before reading its reloptions as
     * CuvsCagraOptions: this runs before create_empty_hnsw()'s own AM check
     * further down, and cuvs_cagra_reloption_graph_degree() blindly casts
     * rd_options to CuvsCagraOptions, so a non-cagra argument here would
     * read that AM's option struct at CuvsCagraOptions's field offsets
     * instead of failing with a clear "not a cagra index" error. */
    if (!object_ownercheck(RelationRelationId, heap_oid, GetUserId()))
        aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, get_rel_name(heap_oid));
    heap_rel = table_open(heap_oid, ShareLock);
    cagra_rel = index_open(cagra_oid, AccessShareLock);
    if (cagra_rel->rd_rel->relam != get_am_oid("cagra", false))
        ereport(ERROR,
                (errmsg("pg_cuvs_build_hnsw: \"%s\" is not a cagra index",
                        get_rel_name(cagra_oid))));
    const char *problem = hnsw_cagra_source_problem(cagra_rel, heap_rel);
    if (problem != NULL)
        hnsw_report_source_problem(cagra_rel, problem);
    dim = (int) TupleDescAttr(RelationGetDescr(cagra_rel), 0)->atttypmod;
    graph_degree = cuvs_cagra_reloption_graph_degree(cagra_rel);
    check_hnsw_page_fit(dim, 4, Min(graph_degree, 64) / 2, 5);

    /* Create empty HNSW on parent table — INDEX_CREATE_SKIP_BUILD, no CPU build */
    hnsw_oid = create_empty_hnsw(heap_rel, cagra_rel);

    /* Dispatch to internal C helpers — no SQL/SPI overhead.
     *
     * RECOMMENDED modes:
     *   'nsw'     — level 0 from the CAGRA graph, upper layers built by the daemon
     *               (the same as 'hnsw' now). The 117s / 2.4x of the old level-0-only
     *               conversion (Cohere 1M×1024) are not measured again.
     *   'hnswlib' — from_cagra() hierarchy via /dev/shm (no disk I/O). 139s, 2.0x.
     *               Slight recall advantage at ef<20.
     *
     * HIDDEN modes (kept for research, not recommended):
     *   'hnsw'         — direct hierarchy with heuristic neighbor selection.
     *                    Currently 144s with no quality gain over 'nsw'.
     *                    Retained pending future improvement of level assignment.
     *   'hnswlib_file' — on-disk .hnsw sidecar; superceded by 'hnswlib' (shm).
     */
    HnswSourceTidResult tid_result;

    (void) hnsw_fill_source(cagra_oid, hnsw_oid, mode, 0, 0, &tid_result);
    if (tid_result == HNSW_SOURCE_TIDS_CONFLICTING)
        hnsw_reject_conflicting_source_tids();
    if (tid_result == HNSW_SOURCE_TIDS_REDUNDANT)
    {
        Relation target = index_open(hnsw_oid, AccessExclusiveLock);
        IndexInfo *index_info = BuildIndexInfo(target);

        /* Let PostgreSQL run expressions and predicates as the heap owner,
         * with security-restricted operations and command-local GUCs. */
        index_build(heap_rel, target, index_info, false, false);
        ereport(NOTICE,
                (errmsg("pg_cuvs_hnsw: rebuilt from the heap because the source has redundant entries")));
        index_close(target, AccessExclusiveLock);
    }

    /* the same tail as a build by CREATE INDEX: certify the new index (it started LOST, uncertified) */
    {
        Relation hnsw_idx = index_open(hnsw_oid, ShareUpdateExclusiveLock);

        if (hnsw_cover_enabled(hnsw_idx))
        {
            Relation hnsw_heap = table_open(IndexGetRelation(hnsw_oid, false), ShareUpdateExclusiveLock);

            cuvs_cover_certify_after_build(hnsw_heap, hnsw_idx);
            table_close(hnsw_heap, NoLock);
        }
        index_close(hnsw_idx, NoLock);
    }

    index_close(cagra_rel, AccessShareLock);
    /* As in a regular CREATE INDEX, block table writes through commit of the
     * populated, ready index. The source probe and the import use one lock. */
    table_close(heap_rel, NoLock);
    PG_RETURN_OID(hnsw_oid);
}

/* ================================================================
 * Phase 3K — pg_cuvs_hnsw access method (ADR-038).
 *
 * Exposes the pg_cuvs_build_hnsw() conversion as standard DDL:
 *
 *   CREATE INDEX my_idx ON items USING pg_cuvs_hnsw (embedding vector_l2_ops)
 *     WITH (source = 'my_cagra', mode = 'nsw');
 *
 * ambuild() writes pgvector-HNSW page format into the index's OWN relation
 * (reusing fill_hnsw_from_cagra_ipc / fill_hnsw_from_hnswlib). The read path
 * (scan/cost/insert/bulkdelete/vacuum) delegates to pgvector's hnsw AM. The
 * wrapper supplies the build callbacks and reloption parser. A single catalog
 * index results, so pg_indexes / DROP INDEX / REINDEX / pg_dump behave naturally.
 * ================================================================ */

/* WITH() storage. The m / ef_construction prefix mirrors pgvector's HnswOptions
 * so that any delegated pgvector code inspecting rd_options sees a compatible
 * layout; source / mode are pg_cuvs extensions. */
typedef struct CuvsHnswOptions
{
    int32 vl_len_;          /* varlena header — do not access directly */
    int   m;                /* pgvector HnswOptions-compatible */
    int   ef_construction;  /* pgvector HnswOptions-compatible */
    int   source_offset;    /* relopt string offset: source CAGRA index name */
    int   mode_offset;      /* relopt string offset: conversion mode */
    int   build_algo_offset; /* HNSW policy only */
} CuvsHnswOptions;

static relopt_kind cuvs_hnsw_relopt_kind;

/* Restrict mode to the four conversion strategies fill_* understand. */
static void
cuvs_hnsw_validate_mode(const char *value)
{
    if (value == NULL || value[0] == '\0')
        return;  /* empty -> default applied in ambuild */
    if (strcmp(value, "nsw") != 0 && strcmp(value, "hnsw") != 0 &&
        strcmp(value, "hnswlib") != 0 && strcmp(value, "hnswlib_file") != 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pg_cuvs_hnsw: invalid mode \"%s\"", value),
                 errhint("Valid modes: nsw (default), hnsw, hnswlib, hnswlib_file.")));
}

static void
cuvs_hnsw_validate_build_algo(const char *value)
{
    if (cuvs_hnsw_algo_supported(cuvs_hnsw_algo_from_name(value))) return;
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
        errmsg("pg_cuvs_hnsw: invalid build_algo \"%s\"", value),
        errhint("Valid supported values: auto, ivf_pq, nn_descent, ace.")));
}

/* One-time relopt registration; called from _PG_init. */
void
cuvs_hnsw_init_reloptions(void)
{
    cuvs_hnsw_relopt_kind = add_reloption_kind();
    add_string_reloption(cuvs_hnsw_relopt_kind, "source",
                         "Source CAGRA index whose graph is converted to HNSW",
                         "", NULL, AccessExclusiveLock);
    add_string_reloption(cuvs_hnsw_relopt_kind, "mode",
                         "Conversion mode: nsw (default), hnsw, hnswlib, hnswlib_file",
                         "nsw", cuvs_hnsw_validate_mode, AccessExclusiveLock);
    add_string_reloption(cuvs_hnsw_relopt_kind, "build_algo",
                         "HNSW route: auto=tiny/resident IVF-PQ, explicit ivf_pq, nn_descent or ace; estimates are not memory upper bounds",
                         "auto", cuvs_hnsw_validate_build_algo, AccessExclusiveLock);
    add_int_reloption(cuvs_hnsw_relopt_kind, "m",
                      "HNSW M for heap builds; source builds use the converted graph's M and warn about a differing explicit m",
                      CUVS_HNSW_DEFAULT_M, 2, 100, AccessExclusiveLock);
    add_int_reloption(cuvs_hnsw_relopt_kind, "ef_construction",
                      "Construction effort: default 64 preserves route defaults; changes intermediate degree / ACE effort",
                      64, 4, 1000, AccessExclusiveLock);
}

static bytea *
cuvs_hnsw_amoptions(Datum reloptions, bool validate)
{
    static const relopt_parse_elt tab[] = {
        {"m",               RELOPT_TYPE_INT,    offsetof(CuvsHnswOptions, m)},
        {"ef_construction", RELOPT_TYPE_INT,    offsetof(CuvsHnswOptions, ef_construction)},
        {"source",          RELOPT_TYPE_STRING, offsetof(CuvsHnswOptions, source_offset)},
        {"mode",            RELOPT_TYPE_STRING, offsetof(CuvsHnswOptions, mode_offset)},
        {"build_algo",      RELOPT_TYPE_STRING, offsetof(CuvsHnswOptions, build_algo_offset)},
    };
    return (bytea *) build_reloptions(reloptions, validate,
                                      cuvs_hnsw_relopt_kind,
                                      sizeof(CuvsHnswOptions),
                                      tab, lengthof(tab));
}

/* A concurrent build writes the pages of an index that is not ready yet without a lock that keeps readers out (its
 * writers' RowExclusiveLock must not wait), so a function that reads those pages refuses the index until it is ready;
 * a failed concurrent build leaves such an index behind too. */
static void
hnsw_require_ready_index(Relation rel, const char *fn)
{
    if (!rel->rd_index->indisready)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("%s: index \"%s\" is not ready: a concurrent build is writing it, or one failed",
                        fn, RelationGetRelationName(rel)),
                 errhint("Wait for CREATE INDEX CONCURRENTLY to finish, or DROP INDEX the invalid index and build it again.")));
}

/* Read the build-time receipt, never infer history from today's GUCs. The
 * pgvector-compatible prefix stays unchanged, including for INSERT/VACUUM. */
PG_FUNCTION_INFO_V1(pg_cuvs_hnsw_build_info);
Datum
pg_cuvs_hnsw_build_info(PG_FUNCTION_ARGS)
{
    Oid oid = PG_GETARG_OID(0);
    Relation rel = index_open(oid, AccessShareLock);
    CuvsHnswPolicy policy = {0};
    CuvsHnswPolicyReadStatus policy_status = CUVS_HNSW_POLICY_NOT_RECORDED;
    CuvsHnswAceReadback ace = {0};
    CuvsHnswNnReadback nn = {0};
    CuvsHnswPolicyReadStatus ace_status = CUVS_HNSW_POLICY_NOT_RECORDED;
    StringInfoData json;
    CuvsConnectivity connectivity = {0};
    uint16_t meta_m = 0;
    Oid heap;
    if (rel->rd_rel->relam != get_am_oid("pg_cuvs_hnsw", true))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("pg_cuvs_hnsw_build_info requires a pg_cuvs_hnsw index")));
    hnsw_require_ready_index(rel, "pg_cuvs_hnsw_build_info");
    heap = IndexGetRelation(oid, false);
    if (pg_class_aclcheck(heap, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
        aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, get_rel_name(heap));
    if (RelationGetNumberOfBlocks(rel) > 0)
    {
        Buffer buffer = ReadBuffer(rel, 0);
        Page page;
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buffer);
        policy_status = hnsw_read_disk_policy(page, &policy);
        if (policy_status == CUVS_HNSW_POLICY_RECORDED)
            meta_m = ((const PgvHnswMeta *)PageGetContents(page))->m;
        if (policy_status == CUVS_HNSW_POLICY_RECORDED && policy.route == CUVS_HNSW_ROUTE_ACE)
            ace_status = hnsw_read_disk_ace(page, &policy, &ace);
        if (policy_status == CUVS_HNSW_POLICY_RECORDED && policy.route == CUVS_HNSW_ROUTE_NN_DESCENT)
            memcpy(&nn, (const char *)PageGetContents(page) + sizeof(PgvHnswMeta) + sizeof(policy), sizeof(nn));
        connectivity=hnsw_read_connectivity(page);
        UnlockReleaseBuffer(buffer);
    }
    index_close(rel, AccessShareLock);
    if (policy_status == CUVS_HNSW_POLICY_UNSUPPORTED)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("pg_cuvs: unsupported stored HNSW policy receipt version"),
            errhint("Use an extension version supporting this receipt; do not rewrite historical metadata.")));
    if (policy_status == CUVS_HNSW_POLICY_CORRUPT)
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
            errmsg("pg_cuvs: invalid stored HNSW policy receipt")));
    if (policy_status == CUVS_HNSW_POLICY_NOT_RECORDED)
    {
        initStringInfo(&json);
        appendStringInfoString(&json,"{\"status\":\"not_recorded\",\"reason\":\"native build policy not recorded\"");
        hnsw_append_connectivity(&json,&connectivity);
        appendStringInfoChar(&json,'}');
        PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in,CStringGetDatum(json.data)));
    }
    if (ace_status == CUVS_HNSW_POLICY_UNSUPPORTED)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("pg_cuvs: unsupported stored ACE partition-builder readback version")));
    if (ace_status == CUVS_HNSW_POLICY_CORRUPT)
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
            errmsg("pg_cuvs: invalid stored ACE partition-builder readback")));
    initStringInfo(&json);
    appendStringInfo(&json,
        "{\"status\":\"recorded\",\"policy_version\":%u,\"route\":\"%s\","
        "\"rows\":%llu,\"dimensions\":%u,\"metric\":%u,"
        "\"requested_algo\":\"%s\",\"requested_ef_construction\":%u,"
        "\"graph_degree\":%u,\"intermediate_degree\":%u,\"hierarchy\":%u,"
        "\"hierarchy_ef_construction\":%u,\"ace_ef_construction\":%u,"
        "\"ace_partitions\":%u,\"ace_partitions_origin\":\"%s\",\"internal_builder\":\"%s\","
        "\"n_lists\":%u,\"pq_dim\":%u,\"pq_bits\":%u,\"n_probes\":%u,\"query_batch\":%u,"
        "\"resident_training_requested\":%llu,\"resident_training_actual\":%llu,"
        "\"resident_host_estimate_bytes\":%llu,\"resident_gpu_estimate_bytes\":%llu,"
        "\"host_budget_bytes\":%llu,\"gpu_budget_bytes\":%llu,"
        "\"sdk_host_budget_bytes\":%llu,\"sdk_gpu_budget_bytes\":%llu,"
        "\"auto_overflow\":%s,\"route_decided\":\"%s\","
        "\"ivfpq_distance_dtype\":\"%s\",\"graph_fallback\":%s,"
        "\"guard\":\"selected_route_returned\",\"memory_upper_bound_guaranteed\":false",
        policy.version, cuvs_hnsw_route_name(policy.route),
        (unsigned long long)policy.n_rows, policy.dim, policy.metric,
        cuvs_hnsw_algo_name(policy.requested_algo), policy.requested_ef,
        policy.degree, policy.intermediate, policy.hierarchy,
        policy.hierarchy_ef, policy.ace_ef, policy.partitions,
        cuvs_hnsw_ace_partitions_origin(&policy),
        policy.internal_builder == 1 ? "exact" : policy.internal_builder == 2 ? "ivf_pq" : "nn_descent",
        policy.n_lists, policy.pq_dim, policy.pq_bits, policy.n_probes, policy.query_batch,
        (unsigned long long)policy.training_requested, (unsigned long long)policy.training_actual,
        (unsigned long long)policy.host_estimate, (unsigned long long)policy.gpu_estimate,
        (unsigned long long)policy.host_budget, (unsigned long long)policy.gpu_budget,
        (unsigned long long)policy.sdk_host_budget, (unsigned long long)policy.sdk_gpu_budget,
        (policy.route_flags & CUVS_HNSW_ROUTE_FLAG_AUTO_OVERFLOW) ? "true" : "false",
        cuvs_hnsw_route_decided_name(policy.route_flags),
        policy.route != CUVS_HNSW_ROUTE_IVF_PQ ? "not_applicable"
            : (policy.route_flags & CUVS_HNSW_ROUTE_FLAG_IVFPQ_FP32) ? "fp32" : "fp16",
        (policy.route_flags & CUVS_HNSW_ROUTE_FLAG_GRAPH_FALLBACK) ? "true" : "false");
    if (policy.degree < 2u * meta_m)
        appendStringInfoString(&json, ",\"graph_degree_limited_by\":\"input_rows\"");
    /* ace_ef_construction above is A; the SDK consumes q = A / 16 (cuvs_hnsw_policy.h). */
    if (policy.route != CUVS_HNSW_ROUTE_ACE)
        appendStringInfoString(&json, ",\"ace_readback\":\"not_applicable\"");
    else if (ace_status != CUVS_HNSW_POLICY_RECORDED)
        appendStringInfoString(&json, ",\"ace_readback\":\"not_recorded\"");
    else
    {
        appendStringInfo(&json,
            ",\"ace_readback\":\"recorded\",\"ace_build_quality_source\":\"%s\","
            "\"ace_build_quality\":%u,\"ace_local_intermediate_degree\":%u,",
            ace.quality_source == CUVS_HNSW_ACE_SOURCE_MANUAL ? "manual" : "auto",
            ace.quality, ace.local_intermediate);
        if (!cuvs_hnsw_ace_iterations_guaranteed(&policy, &ace))
            appendStringInfoString(&json, "\"ace_local_nn_descent_iterations\":\"not_guaranteed\"");
        else if (ace.local_iterations)
            appendStringInfo(&json, "\"ace_local_nn_descent_iterations\":%u", ace.local_iterations);
        else
            appendStringInfoString(&json, "\"ace_local_nn_descent_iterations\":\"varies_by_partition\"");
    }
    /* Before the NN branch: test_hnsw_nn_upper_receipt.py compiles the NN branch (up to the closing
     * brace) as a fixture without this helper. JSON key order carries no meaning. */
    hnsw_append_connectivity(&json, &connectivity);
    if (policy.route == CUVS_HNSW_ROUTE_NN_DESCENT)
        appendStringInfo(&json,
            ",\"nn_readback_version\":%u,\"nn_graph_degree\":%u,\"nn_internal_degree\":%u,"
            "\"nn_sdk_default_iterations\":%u,\"nn_configured_max_iterations\":%u,"
            "\"nn_actual_iterations\":null,\"nn_seed\":null,\"nn_distance_dtype\":\"%s\",\"nn_termination_threshold\":%.9g,"
            "\"nn_upper_builder\":\"%s\",\"nn_upper_dtype\":\"%s\","
            "\"nn_upper_dtype_configured\":%s",
            nn.version, nn.graph_degree, nn.internal_degree, nn.sdk_default_iterations, nn.max_iterations,
            nn.dtype == CUVS_HNSW_NN_DTYPE_FP32 ? "fp32" : "fp16", nn.termination_threshold,
            policy.hierarchy ? "configured_nn_descent_if_built" : "not_applicable",
            policy.hierarchy ? "per_level_not_recorded" : "not_applicable",
            policy.hierarchy ? "null" : "\"not_applicable\"");
    appendStringInfoChar(&json, '}');
    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(json.data)));
}

/* The parsed rd_options fills in defaults. Keep the catalog's distinction
 * between omission and an explicit default, including after ALTER or REINDEX. */
static bool
cuvs_hnsw_m_is_explicit(Oid index_oid)
{
    HeapTuple tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(index_oid));
    bool isnull;
    Datum options;
    List *defs;
    ListCell *cell;

    if (!HeapTupleIsValid(tuple))
        elog(ERROR, "cache lookup failed for relation %u", index_oid);
    options = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions, &isnull);
    defs = isnull ? NIL : untransformRelOptions(options);
    ReleaseSysCache(tuple);

    foreach(cell, defs)
    {
        DefElem *def = lfirst_node(DefElem, cell);

        if (strcmp(def->defname, "m") == 0)
            return true;
    }
    return false;
}

/* ambuild: build a pgvector-HNSW index, served by pgvector (Phase 3K).
 *
 * Two modes (ADR-041):
 *   WITH (source = 'my_cagra')  reuse an existing CAGRA index (saves a build;
 *                               keeps the CAGRA for GPU search alongside).
 *   source omitted              build an EPHEMERAL CAGRA from the heap, convert
 *                               it into this index's pages, then drop it. One
 *                               self-contained DDL; REINDEX rebuilds from the
 *                               heap with no source dependency. The CAGRA is an
 *                               intermediate build artifact (like a .o), so it
 *                               is dropped — to keep a CAGRA, use source mode. */
/* pgvector uses the target's IndexInfo and reads its options from the
 * pgvector-compatible prefix of CuvsHnswOptions. A source index can store
 * a pair that pgvector would reject at build time, so raise ef locally.
 * A NULL heap skips the scan. A NULL IndexInfo selects the init fork. */
static IndexBuildResult *
hnsw_cpu_build(Relation heap, Relation index, IndexInfo *index_info,
               const char *reason)
{
    IndexAmRoutine *hnsw = GetIndexAmRoutineByAmId(get_am_oid("hnsw", false), false);
    ambuild_function build = hnsw->ambuild;
    ambuildempty_function build_empty = hnsw->ambuildempty;
    CuvsHnswOptions *opts = (CuvsHnswOptions *) index->rd_options;
    int m = opts ? opts->m : 16;
    int configured_ef = opts ? opts->ef_construction : 64;
    bool adjust_ef = opts != NULL && opts->source_offset != 0 &&
        ((const char *) opts)[opts->source_offset] != '\0' &&
        configured_ef < 2 * m;
    int build_ef = adjust_ef ? 2 * m : configured_ef;
    int guc_nestlevel = adjust_ef && heap != NULL ? NewGUCNestLevel() : 0;
    IndexBuildResult *result = NULL;

    pfree(hnsw);
    PG_TRY();
    {
        /* Workers reopen the index and read the catalog's unadjusted
         * options. pgvector chooses its own worker count via this GUC. */
        if (guc_nestlevel != 0)
            set_config_option("max_parallel_maintenance_workers", "0",
                              PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE,
                              true, ERROR, false);
        if (adjust_ef)
            opts->ef_construction = build_ef;
        if (index_info != NULL)
            result = build(heap, index, index_info);
        else
            build_empty(index);
    }
    PG_FINALLY();
    {
        /* If relcache reloaded the options, it restored the catalog value. */
        if (adjust_ef && index->rd_options == (bytea *) opts)
            opts->ef_construction = configured_ef;
        if (guc_nestlevel != 0)
            AtEOXact_GUC(false, guc_nestlevel);
    }
    PG_END_TRY();

    if (reason != NULL)
        ereport(NOTICE,
                (errmsg("pg_cuvs_hnsw: rebuilt from the heap because %s", reason),
                 errdetail("pgvector build used m = %d, ef_construction = %d (configured ef_construction = %d).",
                           m, build_ef, configured_ef)));
    return result;
}

static void
cuvs_hnsw_ambuildempty(Relation index)
{
    (void) hnsw_cpu_build(NULL, index, NULL, NULL);
}

static IndexBuildResult *
cuvs_hnsw_ambuild_impl(Relation heapRel, Relation indexRel, IndexInfo *indexInfo)
{
    CuvsHnswOptions *opts = (CuvsHnswOptions *) indexRel->rd_options;
    const char *source_name = (opts != NULL && opts->source_offset != 0)
        ? (const char *) opts + opts->source_offset : "";
    const char *mode = (opts != NULL && opts->mode_offset != 0)
        ? (const char *) opts + opts->mode_offset : "nsw";
    if (mode[0] == '\0')
        mode = "nsw";
    const char *algorithm = (opts && opts->build_algo_offset)
        ? (const char *)opts + opts->build_algo_offset : "auto";
    uint32_t requested_ef = opts ? opts->ef_construction : 64;
    uint32_t requested_m = opts ? (uint32_t) opts->m : CUVS_HNSW_DEFAULT_M;
    cuvs_hnsw_validate_build_algo(algorithm);
    uint32_t build_algo = cuvs_hnsw_algo_from_name(algorithm);
    if (source_name[0] && build_algo != CUVS_HNSW_ALGO_AUTO)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("pg_cuvs_hnsw: build_algo applies to heap builds, not source graph reuse")));


    Oid self_oid = RelationGetRelid(indexRel);
    IndexBuildResult *result = palloc0(sizeof(IndexBuildResult));

    /* #162: dim-ceiling pre-check, before any GPU work. The real ceiling is
     * maxlevel-dependent (narrower at higher levels); safe_maxlevel=5 is a
     * fixed conservative bound chosen to always reject before paying for a
     * CAGRA build or IPC round-trip, per the issue's "다소 보수적으로
     * 거부하더라도 항상 GPU 작업 전에" policy. check_hnsw_page_fit() below
     * (after the real M/maxlevel are known) remains as a backstop for the
     * rare case where this pre-check's M assumption undershoots the actual
     * build.
     *
     * every mode builds upper layers, and the check asks that the element
     * tuple and the level-5 neighbor tuple each fit a page (a pair too large for
     * one page is split as pgvector's build splits it), so the bound of 5 no
     * longer narrows the dimension ceiling below pgvector's own. */
    Form_pg_attribute precheck_att = TupleDescAttr(RelationGetDescr(indexRel), 0);
    int precheck_dim           = (int) precheck_att->atttypmod;
    int precheck_bytes_per_dim = bytes_per_dim_for_typid(precheck_att->atttypid);
    const int safe_maxlevel = 5;	/* nsw builds upper layers too, so its elements reach nonzero levels like every other mode */

    /* ── Source-less: build and convert from regular disk files. ── */
    if (source_name[0] == '\0')
    {
        volatile CuvsHnswBuildResult build;

        /* The daemon builds the graph with degree 2 * m, which serializes HNSW M = m. */
        check_hnsw_page_fit(precheck_dim, precheck_bytes_per_dim,
                            (int) requested_m, safe_maxlevel);
        memset((void *)&build, 0, sizeof(build));
        ((CuvsHnswBuildResult *)&build)->hnsw_fd = -1;
        ((CuvsHnswBuildResult *)&build)->tids_fd = -1;
        PG_TRY();
        {
            CuvsHnswBuildResult *b = (CuvsHnswBuildResult *)&build;
            cuvs_build_hnsw_from_heap(heapRel, indexRel, indexInfo, mode, build_algo, requested_ef, requested_m, b);
            result->heap_tuples = b->reltuples;
            result->index_tuples = (double)b->n_vecs;
            if (b->n_vecs > 0)
                hnsw_import_build_result(indexRel, b, false, (uint16_t)requested_m, 0,
                                         (uint16_t)requested_ef);	/* the graph has upper layers, so not the level-0-only packing */
            else
                hnsw_import_empty_build_result(indexRel, b, (uint16_t)requested_m,
                                               (uint16_t)requested_ef);
        }
        PG_FINALLY();
        {
            CuvsHnswBuildResult *b = (CuvsHnswBuildResult *)&build;
            if (b->hnsw_fd >= 0)
            {
                close(b->hnsw_fd);
                b->hnsw_fd = -1;
            }
            if (b->tids_fd >= 0)
            {
                close(b->tids_fd);
                b->tids_fd = -1;
            }
        }
        PG_END_TRY();

        return result;
    }

    /* ── Source mode: reuse an existing CAGRA index. ── */
    bool rebuilding = ReindexIsProcessingIndex(self_oid);

    /* In-place TRUNCATE rebuilds an existing index without setting the
     * reindex flag. A regular CREATE has a newly created index with relpages
     * zero until index_build() records the pages, even for an empty HNSW.
     * Concurrent CREATE also gets a new index, in an earlier transaction.
     * Leave both CREATE paths subject to the source checks below. */
    bool existing_index = !indexInfo->ii_Concurrent &&
        (indexRel->rd_createSubid == InvalidSubTransactionId ||
         indexRel->rd_rel->relpages > 0);
    if ((rebuilding || existing_index) && RelationGetNumberOfBlocks(heapRel) == 0)
    {
        pfree(result);
        /* pgvector can initialize the main fork without planning a scan or
         * consulting expressions/predicates from the dummy IndexInfo. */
        return hnsw_cpu_build(NULL, indexRel, indexInfo, "the heap is empty");
    }

    /* Resolve source name (schema-qualified, search_path aware) to an OID. */
    Oid cagra_oid = DatumGetObjectId(
        DirectFunctionCall1(regclassin, CStringGetDatum((char *) source_name)));
    Relation cagra_rel;

    /* Source must be a cagra index on the same table we are indexing. */
    {
        HeapTuple ixtup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(cagra_oid));
        if (!HeapTupleIsValid(ixtup))
            ereport(ERROR,
                    (errmsg("pg_cuvs_hnsw: source \"%s\" is not an index", source_name)));
        Oid src_heap = ((Form_pg_index) GETSTRUCT(ixtup))->indrelid;
        ReleaseSysCache(ixtup);
        if (src_heap != RelationGetRelid(heapRel))
            ereport(ERROR,
                    (errmsg("pg_cuvs_hnsw: source index \"%s\" is on a different "
                            "table than the one being indexed", source_name)));

        Oid cagra_am = get_am_oid("cagra", true);
        HeapTuple ctup = SearchSysCache1(RELOID, ObjectIdGetDatum(cagra_oid));
        Oid src_relam = HeapTupleIsValid(ctup)
            ? ((Form_pg_class) GETSTRUCT(ctup))->relam : InvalidOid;
        if (HeapTupleIsValid(ctup))
            ReleaseSysCache(ctup);
        if (!OidIsValid(cagra_am) || src_relam != cagra_am)
            ereport(ERROR,
                    (errmsg("pg_cuvs_hnsw: source \"%s\" must be a cagra index",
                            source_name),
                     errhint("Build it first: CREATE INDEX ... USING cagra (col opclass).")));

        /* #162: dim-ceiling pre-check using the source CAGRA's own
         * graph_degree reloption, before paying the IPC round-trip. */
        cagra_rel = index_open(cagra_oid, AccessShareLock);
        int graph_degree = cuvs_cagra_reloption_graph_degree(cagra_rel);
        check_hnsw_page_fit(precheck_dim, precheck_bytes_per_dim,
                            Min(graph_degree, 64) / 2, safe_maxlevel);

        /* Fail fast: the source CAGRA's metric must match this index's opclass
         * BEFORE paying an IPC round-trip. cagra_index_metric() is opclass-name
         * based, so it works for both the cagra and pg_cuvs_hnsw AMs. fill_*
         * re-checks after fetching the adjacency; this just gives an earlier,
         * clearer error (a CAGRA graph is bound to its build metric and cannot
         * be reinterpreted under a different one). */
        uint32_t src_metric = cagra_index_metric(cagra_oid);
        uint32_t tgt_metric = cagra_index_metric(self_oid);
        if (src_metric != tgt_metric)
        {
            static const char *mname[] = {"l2", "cosine", "ip"};
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                     errmsg("pg_cuvs_hnsw: metric mismatch: source cagra \"%s\" uses %s "
                            "but this index uses %s",
                            source_name,
                            (src_metric < 3) ? mname[src_metric] : "unknown",
                            (tgt_metric < 3) ? mname[tgt_metric] : "unknown"),
                     errhint("Use the same opclass (vector_l2_ops / vector_cosine_ops "
                             "/ vector_ip_ops) as the source cagra index.")));
        }
    }

    int source_m = cuvs_hnsw_m_is_explicit(self_oid) ? (int) requested_m : 0;
    const char *problem = hnsw_cagra_source_problem(cagra_rel, heapRel);
    bool predicate_matches = equal(RelationGetIndexPredicate(cagra_rel),
                                  RelationGetIndexPredicate(indexRel));
    bool key_matches = predicate_matches && hnsw_source_key_matches(cagra_rel, indexRel);
    if (rebuilding && problem == NULL)
    {
        if (!predicate_matches || !key_matches)
            problem = "does not match the target definition";
        else if (strcmp(mode, "hnswlib_file") == 0 &&
                 !hnsw_file_source_available(cagra_rel))
            problem = "has no matching HNSW file binding";
    }
    if (problem != NULL && rebuilding)
    {
        char *reason = psprintf("source CAGRA index \"%s\" %s",
                                RelationGetRelationName(cagra_rel), problem);

        index_close(cagra_rel, AccessShareLock);
        pfree(result);
        return hnsw_cpu_build(heapRel, indexRel, indexInfo, reason);
    }
    if (problem != NULL)
        hnsw_report_source_problem(cagra_rel, problem);
    if (!predicate_matches)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                 errmsg("pg_cuvs_hnsw: source CAGRA predicate does not match the target"),
                 errhint("Use the same WHERE clause as the source index, or build from the heap without a source.")));
    if (!key_matches)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                 errmsg("pg_cuvs_hnsw: source CAGRA index key does not match the target"),
                 errhint("Use the same column or expression as the source index, or build from the heap without a source.")));

    /* Fill THIS index's relation (pgvector page format) from the CAGRA graph.
     * fill_* open the target by OID with AccessExclusiveLock. For CREATE INDEX
     * and REINDEX this backend already holds that lock on the index under
     * construction (a no-op re-grant); a concurrent build holds RowExclusiveLock
     * only, so there the request waits for the table's writers and queues new
     * ones behind it. The freshly created relation has 0 blocks so no truncate
     * happens. */
    HnswSourceTidResult tid_result;
    size_t n_elems_written = hnsw_fill_source(cagra_oid, self_oid, mode, source_m,
                                             (uint16_t)requested_ef, &tid_result);

    if (tid_result == HNSW_SOURCE_TIDS_CONFLICTING && !rebuilding)
        hnsw_reject_conflicting_source_tids();
    if (tid_result != HNSW_SOURCE_TIDS_UNIQUE)
    {
        index_close(cagra_rel, AccessShareLock);
        pfree(result);
        return hnsw_cpu_build(heapRel, indexRel, indexInfo, "the source contains duplicate TIDs");
    }
    index_close(cagra_rel, AccessShareLock);

    /* #161: element count from fill_*'s own return, not inferred from block
     * count. nblocks-1 == N only holds while every page carries exactly one
     * element — true today, but a later commit packs multiple elements per
     * page on the nsw path, at which point that inference goes silently
     * wrong. Switching to the exact count now removes the assumption before
     * it can be violated. */
    {
        double ntuples = (double) n_elems_written;
        result->heap_tuples  = ntuples;
        result->index_tuples = ntuples;
    }
    return result;
}

/*
 * mode='nsw' before 0.9.3 wrote every element of a pg_cuvs_hnsw index at level 0, so the upper layers of such an index
 * hold only rows pgvector inserted after the build, and a search that descends through them can miss the built rows. This
 * counts, for one index, the elements that are not marked deleted, those above level 0, the metapage's m and entry level,
 * the hierarchy flag of the heap build's policy receipt (NULL when the build recorded none, e.g. a CAGRA-source build), and
 * how many elements above level 0 have no link at level 1 (a search descending through level 1 stops at such an element;
 * a connected upper layer has none). pg_cuvs_hnsw_layer_status() turns these into a verdict. It reads every page of the
 * index, and the neighbor tuple of every element above level 0.
 */
PG_FUNCTION_INFO_V1(pg_cuvs_hnsw_layer_counts);
Datum
pg_cuvs_hnsw_layer_counts(PG_FUNCTION_ARGS)
{
    Oid oid = PG_GETARG_OID(0);
    Relation rel;
    Oid heap;
    TupleDesc tupdesc;
    Datum values[6];
    bool nulls[6] = {false, false, true, true, true, false};
    int64 elements = 0;
    int64 upper = 0;
    int64 unlinked = 0;
    int m = 0;
    BlockNumber nblocks;
    BufferAccessStrategy strategy;
    ItemPointerData *ntids = NULL;
    uint8 *nlevels = NULL;
    int64 nupper_alloc = 0;

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "pg_cuvs_hnsw_layer_counts: return type must be a row type");
    heap = IndexGetRelation(oid, false);
    if (pg_class_aclcheck(heap, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
        aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, get_rel_name(heap));
    rel = index_open(oid, AccessShareLock);
    if (rel->rd_rel->relam != get_am_oid("pg_cuvs_hnsw", true))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("pg_cuvs_hnsw_layer_counts requires a pg_cuvs_hnsw index")));
    hnsw_require_ready_index(rel, "pg_cuvs_hnsw_layer_counts");
    nblocks = RelationGetNumberOfBlocks(rel);
    if (nblocks > 0)
    {
        Buffer buffer = ReadBuffer(rel, 0);
        Page page;
        PgvHnswMeta *meta;
        CuvsHnswPolicy policy = {0};

        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buffer);
        meta = (PgvHnswMeta *) PageGetContents(page);
        if (meta->magicNumber == PGV_HNSW_MAGIC)
        {
            values[2] = Int32GetDatum((int32) meta->m);
            m = (int) meta->m;
            values[3] = Int32GetDatum((int32) meta->entryLevel);
            nulls[2] = nulls[3] = false;
        }
        if (hnsw_read_disk_policy(page, &policy) == CUVS_HNSW_POLICY_RECORDED)
        {
            values[4] = Int32GetDatum((int32) policy.hierarchy);
            nulls[4] = false;
        }
        UnlockReleaseBuffer(buffer);
    }
    strategy = GetAccessStrategy(BAS_BULKREAD);
    for (BlockNumber blkno = 1; blkno < nblocks; blkno++)
    {
        Buffer buffer;
        Page page;
        OffsetNumber maxoff;

        CHECK_FOR_INTERRUPTS();
        buffer = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, strategy);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buffer);
        maxoff = PageGetMaxOffsetNumber(page);
        for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off = OffsetNumberNext(off))
        {
            ItemId item = PageGetItemId(page, off);
            const PgvHnswElemHdr *etup;

            if (!ItemIdIsUsed(item) || !ItemIdHasStorage(item) ||
                ItemIdGetLength(item) < sizeof(PgvHnswElemHdr))
                continue;
            etup = (const PgvHnswElemHdr *) PageGetItem(page, item);
            if (etup->type != 1 || etup->deleted)
                continue;
            elements++;
            if (etup->level > 0)
            {
                if (upper == nupper_alloc)
                {
                    nupper_alloc = nupper_alloc ? 2 * nupper_alloc : 1024;
                    ntids = ntids ? repalloc(ntids, nupper_alloc * sizeof(ItemPointerData))
                        : palloc(nupper_alloc * sizeof(ItemPointerData));
                    nlevels = nlevels ? repalloc(nlevels, nupper_alloc) : palloc(nupper_alloc);
                }
                ItemPointerSet(&ntids[upper],
                               ((BlockNumber) etup->neighbortid.ip_blkid.bi_hi << 16) | etup->neighbortid.ip_blkid.bi_lo,
                               etup->neighbortid.ip_posid);
                nlevels[upper] = etup->level;
                upper++;
            }
        }
        UnlockReleaseBuffer(buffer);
    }
    /* Level-1 links of the elements above level 0: their neighbor tuple lists the highest level first, m slots per level
     * above 0, so level 1 is slots [(level - 1) * m, level * m). An empty slot has offset 0. */
    for (int64 i = 0; i < upper && m > 0; i++)
    {
        BlockNumber nblk = ItemPointerGetBlockNumber(&ntids[i]);
        OffsetNumber noff = ItemPointerGetOffsetNumber(&ntids[i]);
        Buffer buffer;
        Page page;
        bool linked = false;

        CHECK_FOR_INTERRUPTS();
        if (nblk == 0 || nblk >= nblocks)
        {
            unlinked++;
            continue;
        }
        buffer = ReadBufferExtended(rel, MAIN_FORKNUM, nblk, RBM_NORMAL, strategy);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buffer);
        if (noff >= FirstOffsetNumber && noff <= PageGetMaxOffsetNumber(page))
        {
            ItemId item = PageGetItemId(page, noff);
            const PgvHnswNeighHdr *ntup = (const PgvHnswNeighHdr *) PageGetItem(page, item);
            size_t first = (size_t) (nlevels[i] - 1) * (size_t) m;

            if (ItemIdHasStorage(item) && ntup->type == 2 &&
                first + (size_t) m <= ntup->count &&
                ItemIdGetLength(item) >= sizeof(PgvHnswNeighHdr) + (first + (size_t) m) * sizeof(PgvItemPointer))
            {
                const PgvItemPointer *slots = (const PgvItemPointer *) ((const char *) ntup + sizeof(PgvHnswNeighHdr));

                for (int s = 0; s < m && !linked; s++)
                    linked = slots[first + s].ip_posid != 0;
            }
        }
        UnlockReleaseBuffer(buffer);
        if (!linked)
            unlinked++;
    }
    FreeAccessStrategy(strategy);
    index_close(rel, AccessShareLock);
    values[0] = Int64GetDatum(elements);
    values[1] = Int64GetDatum(upper);
    values[5] = Int64GetDatum(unlinked);
    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/* ambuild: the build above, then (for an index of the new handler symbol) the certification every build of rows ends with.
 * An empty table starts CERTIFIED already and the certification finds nothing to do. */
static IndexBuildResult *
cuvs_hnsw_ambuild(Relation heapRel, Relation indexRel, IndexInfo *indexInfo)
{
    IndexBuildResult *result = cuvs_hnsw_ambuild_impl(heapRel, indexRel, indexInfo);

    if (hnsw_cover_enabled(indexRel))
        cuvs_cover_certify_after_build(heapRel, indexRel);
    return result;
}

/* ambuildempty of a cover index: let pgvector write the init fork, then give its metapage the descriptor (CERTIFIED,
 * empty ledger) and append the ledger page as block 2. UNLOGGED indexes are copied from this fork by crash recovery,
 * so a crash reset yields the same classification as a fresh empty build. */
static void (*pgv_ambuildempty) (Relation) = NULL;

static void
cuvs_cover_buildempty(Relation indexRel)
{
    Buffer      buf;
    Page        meta = (Page) palloc(BLCKSZ);
    Page        ledger = (Page) palloc(BLCKSZ);
    CuvsConnectivity c;

    Assert(pgv_ambuildempty != NULL);
    pgv_ambuildempty(indexRel);

    memset(&c, 0, sizeof(c));
    if (cuvs_hnsw_connectivity_check)
    {
        c.magic = CUVS_CONNECTIVITY_MAGIC;
        c.version = 1;
        c.available = CUVS_CONNECTIVITY_MEASURED;
    }
    else
        cuvs_connectivity_set_disabled(&c);

    buf = ReadBufferExtended(indexRel, INIT_FORKNUM, 0, RBM_NORMAL, NULL);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    memcpy(meta, BufferGetPage(buf), BLCKSZ);
    hnsw_stage_metapage_tail(indexRel, meta, &c, true);
    hnsw_cover_check_metapage(indexRel, meta);
    START_CRIT_SECTION();
    memcpy(BufferGetPage(buf), meta, BLCKSZ);
    MarkBufferDirty(buf);
    log_newpage_buffer(buf, true);
    END_CRIT_SECTION();
    UnlockReleaseBuffer(buf);

    cuvs_ledger_page_init((uint8_t *) ledger, CUVS_LEDGER_KIND_KEYS, 1, 0, CUVS_LEDGER_NO_BLOCK, 3);
    buf = ReadBufferExtended(indexRel, INIT_FORKNUM, P_NEW, RBM_NORMAL, NULL);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    if (BufferGetBlockNumber(buf) != 2)
        ereport(ERROR, (errmsg("pg_cuvs: the ledger page of an empty index must be block 2, got %u", BufferGetBlockNumber(buf))));
    START_CRIT_SECTION();
    memcpy(BufferGetPage(buf), ledger, BLCKSZ);
    MarkBufferDirty(buf);
    log_newpage_buffer(buf, true);
    END_CRIT_SECTION();
    UnlockReleaseBuffer(buf);
    pfree(meta);
    pfree(ledger);
}


/*
 * Two C symbols name the same access method handler on purpose.
 *
 *   pg_cuvs_hnsw_handler      the OLD symbol: SQL of 0.9.2 and earlier points here. It must
 *                             keep returning the routine of an index without a cover
 *                             descriptor (no cover wrappers), so a cluster that has not run
 *                             ALTER EXTENSION UPDATE never gets a new-format index.
 *   pg_cuvs_hnsw_handler_c1p  the NEW symbol: SQL of 0.9.3 points here. An older library does
 *                             not export it, so an index of an updated database cannot be
 *                             opened by it: the failure is loud (could not find function)
 *                             instead of an old build writing into a new-format index.
 *
 * Both return the same routine today because no cover wrapper is wired into the product yet.
 * When one is, only the NEW symbol may install it; test-cover-symbols checks that both
 * symbols are exported and that the SQL points at the right one.
 */
static IndexAmRoutine *
cuvs_hnsw_handler_routine(bool cover)
{
    /* Borrow pgvector's build-independent operations and read capabilities.
     * The scan wrapper delegates by default and selects exact heap execution
     * only when the explicit session policy requests it. */
    Oid hnsw_am = get_am_oid("hnsw", false);
    HeapTuple amtup = SearchSysCache1(AMOID, ObjectIdGetDatum(hnsw_am));
    if (!HeapTupleIsValid(amtup))
        ereport(ERROR, (errmsg("pg_cuvs_hnsw: pgvector hnsw access method not found")));
    Oid amhandler = ((Form_pg_am) GETSTRUCT(amtup))->amhandler;
    ReleaseSysCache(amtup);
    if (!OidIsValid(amhandler))
        ereport(ERROR, (errmsg("pg_cuvs_hnsw: pgvector hnsw AM has no handler")));

    IndexAmRoutine *amroutine = (IndexAmRoutine *)
        DatumGetPointer(OidFunctionCall1(amhandler, (Datum) 0));

    amroutine->ambuild = cuvs_hnsw_ambuild;   /* GPU CAGRA -> HNSW pages */
    amroutine->ambuildempty = cuvs_hnsw_ambuildempty;
    amroutine->amoptions = cuvs_hnsw_amoptions;  /* WITH (source, mode, ...) */
    /* Inserts and vacuum retain pgvector's behavior. The scan wrapper keeps
     * its delegate path and connects the explicitly selected exact heap mode. */
    cuvs_hnsw_scan_wrap(amroutine);

    /* cover wrappers belong here and only when `cover`. ambuildempty is also the marker hnsw_cover_enabled() looks for. */
    if (cover)
    {
        pgv_ambuildempty = amroutine->ambuildempty;
        amroutine->ambuildempty = cuvs_cover_buildempty;
        cuvs_cover_wrap_insert(amroutine);
        cuvs_cover_wrap_vacuum(amroutine);
    }

    return amroutine;
}

PG_FUNCTION_INFO_V1(pg_cuvs_hnsw_handler);
Datum
pg_cuvs_hnsw_handler(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(cuvs_hnsw_handler_routine(false));
}

PG_FUNCTION_INFO_V1(pg_cuvs_hnsw_handler_c1p);
Datum
pg_cuvs_hnsw_handler_c1p(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(cuvs_hnsw_handler_routine(true));
}
