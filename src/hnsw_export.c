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
 *   Blocks 1..N: interleaved element tuples (type=1) and neighbor tuples (type=2)
 *     One page per element pair; unused space left empty.
 *
 * We do NOT include pgvector's hnsw.h (not installed).  All structs are
 * defined locally matching the pgvector 0.8.x binary layout.
 */

#include "postgres.h"
#include "fmgr.h"
#include "access/generic_xlog.h"
#include "access/xlog.h"            /* log_newpage_buffer */
#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/pg_attribute.h"  /* Form_pg_attribute, ATTNUM */
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"      /* Anum_pg_index_indclass */
#include "catalog/pg_opclass.h"    /* Form_pg_opclass, CLAOID */
#include "utils/syscache.h"        /* SearchSysCache1/2, RELOID, ATTNUM, INDEXRELID, CLAOID */
#include "common/relpath.h"
#include "storage/bufmgr.h"
#include "storage/itemptr.h"
#include "storage/smgr.h"
#include "catalog/storage.h"       /* RelationTruncate */
#include "utils/rel.h"
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
#include "nodes/pg_list.h"       /* list_make1 */
#include "access/genam.h"        /* GetSysCacheOid3 */
#include "access/table.h"        /* table_open/table_close (pgvector version guard) */
#include "access/stratnum.h"     /* BTEqualStrategyNumber */
#include "catalog/pg_extension.h"/* ExtensionRelationId, Anum_pg_extension_* */
#include "utils/fmgroids.h"      /* F_OIDEQ */
#include "utils/acl.h"
#include "utils/lsyscache.h"     /* get_rel_name */
#include "utils/jsonb.h"
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
#include "cuvs_hnsw_build.h"
#include "hnsw_export.h"
#include "storage/buffile.h"

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
#define CUVS_HNSW_BUILD_M       32
#define CUVS_HNSW_BUILD_EF      128

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
 * Errors if not even one element+neighbor pair at the worst-case level
 * fits on a page (#162/#161) — a failure here means the index cannot be
 * built at all with this dim/type/M, regardless of packing.
 */
static void
check_hnsw_page_fit(int dim, int bytes_per_dim, int m, int maxlevel)
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
    uint8_t  reserved;
} HnswNodeLocation;

_Static_assert(sizeof(HnswNodeLocation) == 8,
               "HNSW location records must stay compact");

typedef void (*HnswInterruptCheck)(void);
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
hnsw_read_tid_for_label(int tids_fd, uint64_t n_vecs, uint64_t label,
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
        !hnsw_pread_exact(tids_fd, &tid, sizeof(tid), file_offset))
        return hnsw_set_file_error(error, error_size,
                                   "cannot read TID for HNSW label");
    if (!hnsw_tid_is_valid(tid))
        return hnsw_set_file_error(error, error_size,
                                   "TID file contains an invalid heap TID");
    *tid_out = tid;
    return true;
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

static bool
hnsw_data_page_count(uint64_t n_nodes, bool packed_nsw, uint32_t pack_k,
                     uint32_t *page_count_out)
{
    uint64_t page_count;

    if (n_nodes == 0 || page_count_out == NULL ||
        (packed_nsw && pack_k == 0))
        return false;
    page_count = packed_nsw ? 1 + (n_nodes - 1) / pack_k : n_nodes;
    if (page_count == 0 || page_count >= (uint64_t)InvalidBlockNumber)
        return false;
    *page_count_out = (uint32_t)page_count;
    return true;
}

static bool
hnsw_page_pair_fits(int dim, int bytes_per_dim, int m, int level)
{
    size_t esize;
    size_t nsize;
    size_t needed;

    if (dim <= 0 || (bytes_per_dim != 2 && bytes_per_dim != 4) ||
        m <= 0 || level < 0)
        return false;
    esize = elem_tuple_size(dim, bytes_per_dim);
    nsize = neigh_tuple_size(level, m);
    needed = esize + nsize + 4 * PGV_ITEM_OVERHEAD;
    return needed <= PGV_USABLE_BYTES;
}

static bool
hnsw_file_preflight(int hnsw_fd, int tids_fd,
                    uint64_t expected_n_vecs,
                    uint32_t expected_dim, uint32_t expected_metric,
                    int bytes_per_dim, bool packed_nsw,
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
    unsigned char crc_buffer[HNSW_FILE_IO_CHUNK];
    uint32_t crc_state;
    uint64_t tids_remaining;
    uint64_t tids_offset;
    int *links = NULL;
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
            check_interrupts();
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
    int layout_maxlevel = packed_nsw ? 0 : h.maxlevel;
    if ((uint64_t)(layout_maxlevel + 2) * h.M > UINT16_MAX)
    {
        hnsw_set_file_error(error, error_size,
                            "HNSW neighbor slot count exceeds the page format");
        goto done;
    }
    if (!hnsw_page_pair_fits((int)expected_dim, bytes_per_dim,
                             (int)h.M, layout_maxlevel))
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
                                          (int)h.M, &pairs_per_page))
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
    for (uint64_t node = 0; node < expected_n_vecs; node++)
    {
        uint64_t row_offset;
        uint64_t base;
        uint64_t label_offset;
        int count;
        size_t label;

        if (check_interrupts)
            check_interrupts();
        if (!hnsw_mul_u64(node, h.sizeDataPerElement, &row_offset) ||
            !hnsw_add_u64(HNSWLIB_HEADER_BYTES, row_offset, &base) ||
            !hnsw_add_u64(base, h.offsetLevel0, &row_offset) ||
            !hnsw_pread_exact(hnsw_fd, &count, sizeof(count), row_offset) ||
            count < 0 || (uint64_t)count > h.maxM0)
        {
            hnsw_set_file_errorf(error, error_size,
                                 "invalid level-0 link count at node %llu (capacity %llu)",
                                 node, h.maxM0);
            goto done;
        }
        if (!hnsw_add_u64(row_offset, sizeof(count), &row_offset) ||
            !hnsw_pread_exact(hnsw_fd, links, (size_t)count * sizeof(int), row_offset))
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
            !hnsw_pread_exact(hnsw_fd, &label, sizeof(label), label_offset) ||
            (uint64_t)label >= expected_n_vecs)
        {
            hnsw_set_file_error(error, error_size,
                                "HNSW label is outside the original row count");
            goto done;
        }
    }

    per_level_bytes = (h.M + 1) * sizeof(int);
    file_cursor = level0_end;
    for (uint64_t node = 0; node < expected_n_vecs; node++)
    {
        uint32_t list_bytes;
        uint64_t links_start;
        uint64_t row_end;
        uint64_t level;
        uint64_t location_node = node;
        HnswNodeLocation location;

        if (check_interrupts)
            check_interrupts();
        if (file_cursor > (uint64_t)hs.st_size ||
            (uint64_t)hs.st_size - file_cursor < sizeof(list_bytes) ||
            !hnsw_pread_exact(hnsw_fd, &list_bytes, sizeof(list_bytes), file_cursor))
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
                !hnsw_pread_exact(hnsw_fd, &upper_count, sizeof(upper_count),
                                  level_offset) ||
                upper_count < 0 || (uint64_t)upper_count > h.M)
            {
                hnsw_set_file_error(error, error_size,
                                    "invalid upper-level neighbor count");
                goto done;
            }
            if (!hnsw_add_u64(level_offset, sizeof(upper_count), &level_offset) ||
                !hnsw_pread_exact(hnsw_fd, links,
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
        if (!hnsw_layout_location(node, expected_n_vecs, (uint8_t)level,
                                  packed_nsw, pairs_per_page, &location))
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
    free(links);
    return ok;
}

/* ----------------------------------------------------------------
 * External GUCs declared in pg_cuvs.c
 * ---------------------------------------------------------------- */
extern char *cuvs_index_dir;

/* Forward declaration of get_index_dir from pg_cuvs.c — we replicate the
 * logic here to avoid exposing it as a public symbol. */
static const char *
hnsw_get_index_dir(void)
{
    static char buf[1024];
    if (cuvs_index_dir && cuvs_index_dir[0] != '\0')
        return cuvs_index_dir;
    snprintf(buf, sizeof(buf), "%s/cuvs_indexes", DataDir);
    return buf;
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

typedef bool (*HnswNodeLocationLookup)(void *arg, uint32_t node,
                                       HnswNodeLocation *location);

static bool
hnsw_lookup_elem_array(void *arg, uint32_t node, HnswNodeLocation *location)
{
    const ElemDesc *elems = (const ElemDesc *)arg;

    location->elem_blkno = elems[node].elem_blkno;
    location->elem_offno = elems[node].elem_offno;
    location->level = (uint8_t)elems[node].level;
    location->reserved = 0;
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
                ElemDesc *e,
                const float *vec,
                int dim,
                int bytes_per_dim,
                int m,
                size_t n_elems,
                HnswNodeLocationLookup location_lookup,
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
                HnswNodeLocation location;
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
            HnswNodeLocation location;
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

    /* Add neighbor tuple, same assertion. */
    OffsetNumber noff = PageAddItem(page, (Item)ntup, nsize, InvalidOffsetNumber,
                                    false, false);
    if (noff == InvalidOffsetNumber)
        ereport(ERROR,
                (errmsg("pg_cuvs: hnsw_export: neighbor tuple does not fit on page "
                        "(nsize=%zu, free=%zu)", nsize,
                        (size_t)PageGetFreeSpace(page))));
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
                HnswNodeLocationLookup location_lookup,
                void *location_lookup_arg)
{
    Buffer              buf;
    Page                page;
    PgvHnswPageOpaque  *opaque;

    /* always extend with P_NEW; caller loops in blkno order so each call
     * appends the next block in sequence. */
    buf  = ReadBuffer(rel, P_NEW);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    page = BufferGetPage(buf);
    PageInit(page, BLCKSZ, sizeof(PgvHnswPageOpaque));

    opaque = (PgvHnswPageOpaque *) PageGetSpecialPointer(page);
    opaque->nextblkno = InvalidBlockNumber;
    opaque->unused    = 0;
    opaque->page_id   = PGV_HNSW_PAGE_ID;
    if (!hnsw_set_disk_page_chain(page, blkno, n_elems))
        ereport(ERROR,
                (errmsg("pg_cuvs: invalid HNSW data page chain position")));

    write_elem_pair(page, e, vec, dim, bytes_per_dim, m, n_elems,
                    location_lookup, location_lookup_arg);

    MarkBufferDirty(buf);
    if (!skip_wal)
        log_newpage_buffer(buf, true); /* WAL full-page image for crash safety */
    UnlockReleaseBuffer(buf);
}

/* #161: writes N element+neighbor pairs packed k-per-page (fixed k, not
 * greedy — same k for every page, chosen once from (dim, bytes_per_dim, m,
 * level) so index_bytes stays a pure function of the build parameters, not
 * of runtime packing choices). elems[i].elem_blkno/elem_offno/neigh_blkno/
 * neigh_offno must already encode this k (the layout pass's job, not this
 * function's) — this function only opens/fills/closes each page in that
 * pre-assigned order and asserts (via write_elem_pair()) that what it
 * writes lands where the layout pass said it would.
 *
 * Only valid for a uniform-size element sequence, i.e. every elems[i] at
 * the SAME level (today: the nsw path, where every element is level 0).
 * A hierarchical sequence has per-element neighbor-tuple sizes that vary
 * with level, which a single fixed k cannot pack losslessly — callers with
 * varying levels must keep using write_elem_page() in a per-element loop. */
static void
write_elem_pages_packed(Relation rel,
                        ElemDesc *elems,
                        const float *vecs,
                        int dim,
                        int bytes_per_dim,
                        int m,
                        size_t n_elems,
                        int k,
                        bool skip_wal,
                        HnswNodeLocationLookup location_lookup,
                        void *location_lookup_arg)
{
    uint32_t data_page_count;

    if (n_elems == 0)
        return;
    if (k <= 0 ||
        !hnsw_data_page_count((uint64_t)n_elems, true, (uint32_t)k,
                              &data_page_count))
        ereport(ERROR,
                (errmsg("pg_cuvs: invalid packed HNSW data page count")));

    for (size_t page_start = 0; page_start < n_elems; page_start += (size_t) k)
    {
        size_t page_end = page_start + (size_t) k;
        if (page_end > n_elems)
            page_end = n_elems;

        Buffer buf  = ReadBuffer(rel, P_NEW);
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        Page   page = BufferGetPage(buf);
        PageInit(page, BLCKSZ, sizeof(PgvHnswPageOpaque));

        PgvHnswPageOpaque *opaque = (PgvHnswPageOpaque *) PageGetSpecialPointer(page);
        opaque->nextblkno = InvalidBlockNumber;
        opaque->unused    = 0;
        opaque->page_id   = PGV_HNSW_PAGE_ID;
        if (!hnsw_set_disk_page_chain(page,
                                      (uint64_t)(page_start / (size_t)k) + 1,
                                      data_page_count))
            ereport(ERROR,
                    (errmsg("pg_cuvs: invalid packed HNSW data page chain position")));

        for (size_t i = page_start; i < page_end; i++)
        {
            const float *vec = vecs + i * (size_t) dim;
            write_elem_pair(page, &elems[i], vec, dim, bytes_per_dim, m,
                            n_elems, location_lookup, location_lookup_arg);
        }

        MarkBufferDirty(buf);
        if (!skip_wal)
            log_newpage_buffer(buf, true);
        UnlockReleaseBuffer(buf);
    }
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
/* Internal helper: fill an existing HNSW index from hnswlib sidecar.
 * Called by pg_cuvs_build_hnsw() for modes 'hnswlib' and 'hnswlib_file'.
 * use_shm=true  → daemon runs from_cagra() → /dev/shm (no disk I/O)
 * use_shm=false → reads pre-built .hnsw sidecar from index_dir */
static void
fill_hnsw_from_hnswlib_impl(Oid cagra_oid, Oid hnsw_oid, bool use_shm,
                            HnswFillRes *res)
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
    const char *idir = hnsw_get_index_dir();

    if (use_shm)
    {
        /* Request daemon to run from_cagra() → /dev/shm (no disk I/O) */
        if (!cuvs_socket_path || cuvs_socket_path[0] == '\0')
            ereport(ERROR, (errmsg("pg_cuvs: daemon socket not set")));
        int shm_rc = cuvs_ipc_export_hnsw_shm(
            cuvs_socket_path, db_oid, index_oid, hnsw_path, sizeof(hnsw_path),
            &res->shm_fd);
        if (shm_rc != CUVS_STATUS_OK)
        {
            cuvs_report_proto_mismatch(shm_rc);
            ereport(ERROR,
                    (errmsg("pg_cuvs: export_hnsw_shm failed (status=%d); "
                            "ensure cagra index %u is loaded in daemon", shm_rc, cagra_oid)));
        }
        /* .tids comes from the regular index_dir sidecar */
        snprintf(tids_path, sizeof(tids_path), "%s/%u_%u.tids", idir, db_oid, index_oid);
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

    /* ---- 4. Read .tids sidecar ---- */
    FILE *tf = fopen(tids_path, "rb");
    if (!tf)
    {
        ereport(ERROR,
                (errmsg("pg_cuvs: cannot open .tids sidecar \"%s\": %m",
                        tids_path)));
    }

    CuvsTidsHeader tids_hdr;
    uint64_t      *tids = NULL;
    if (cuvs_tids_read(tf, &tids_hdr, &tids) != 0)
    {
        fclose(tf);
        ereport(ERROR,
                (errmsg("pg_cuvs: failed to read/validate .tids sidecar \"%s\"",
                        tids_path)));
    }
    fclose(tf);
    res->tids = tids;

    if ((size_t)tids_hdr.n_vecs < N)
    {
        ereport(ERROR,
                (errmsg("pg_cuvs: .tids has %lld entries but .hnsw has %zu elements",
                        (long long)tids_hdr.n_vecs, N)));
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
    check_hnsw_page_fit(dim, bytes_per_dim, M, hdr.maxlevel);

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

    /* ---- 11. Write metapage (block 0) ---- */
    /* Use BufferGetPage directly (pgvector-style build path; WAL safety via
     * transaction rollback on error — the caller's transaction covers all writes). */
    {
        Buffer buf = ReadBuffer(hnsw_rel, P_NEW);   /* → block 0 */
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        Page page = BufferGetPage(buf);
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
        metap->efConstruction = (uint16_t)hdr.efConstruction;

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

        MarkBufferDirty(buf);
        if (!hnsw_unlogged)
            log_newpage_buffer(buf, true); /* WAL full-page image for crash safety */
        UnlockReleaseBuffer(buf);
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
fill_hnsw_from_hnswlib(Oid cagra_oid, Oid hnsw_oid, bool use_shm)
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
    PG_TRY();
    {
        fill_hnsw_from_hnswlib_impl(cagra_oid, hnsw_oid, use_shm,
                                    (HnswFillRes *) &res);
    }
    PG_FINALLY();
    {
        hnsw_fill_res_free((HnswFillRes *) &res);
    }
    PG_END_TRY();
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
    HnswLocationCachePage cache[HNSW_LOCATION_CACHE_PAGES];
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
} HnswDiskRow;

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
    table->n_written++;
    return true;
}

static bool
hnsw_location_table_rewind(HnswLocationTable *table, uint64_t expected_rows)
{
    HnswNodeLocation rows[HNSW_LOCATION_ROWS_PER_PAGE];
    uint64_t logical_bytes;
    uint64_t bytes_remaining;

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
        bytes_remaining -= bytes;
    }

    /* Rewind after readback so the first lookup starts at logical row zero. */
    return BufFileSeekBlock(table->file, 0) == 0;
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

static void
hnsw_pg_check_interrupts(void)
{
    CHECK_FOR_INTERRUPTS();
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

    if (node >= n_nodes ||
        !hnsw_mul_u64(node, header->sizeDataPerElement, &row_delta) ||
        !hnsw_add_u64(HNSWLIB_HEADER_BYTES, row_delta, &row_base) ||
        !hnsw_add_u64(row_base, header->offsetLevel0, &level0_links_offset) ||
        !hnsw_pread_exact(hnsw_fd, &link_count, sizeof(link_count),
                          level0_links_offset) ||
        link_count < 0 || (uint64_t)link_count > header->maxM0 ||
        !hnsw_add_u64(level0_links_offset, sizeof(link_count), &field_offset) ||
        !hnsw_pread_exact(hnsw_fd, row->elem.lv0_links,
                          (size_t)link_count * sizeof(int), field_offset))
        ereport(ERROR,
                (errmsg("pg_cuvs: cannot read HNSW level-0 row %llu",
                        (unsigned long long)node)));

    if (!hnsw_add_u64(row_base, header->offsetData, &field_offset) ||
        !hnsw_pread_exact(hnsw_fd, row->vector,
                          (size_t)dim * sizeof(float), field_offset) ||
        !hnsw_add_u64(row_base, header->labelOffset, &field_offset) ||
        !hnsw_pread_exact(hnsw_fd, &label, sizeof(size_t), field_offset) ||
        !hnsw_read_tid_for_label(tids_fd, n_nodes, label, &tid,
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
    row->elem.neigh_blkno = location.elem_blkno;
    row->elem.neigh_offno = location.elem_offno + 1;

    if (!hnsw_pread_exact(hnsw_fd, &upper_bytes, sizeof(upper_bytes),
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
            !hnsw_pread_exact(hnsw_fd, &count, sizeof(count), links_offset) ||
            count < 0 || (uint64_t)count > header->M ||
            !hnsw_add_u64(links_offset, sizeof(count), &links_offset) ||
            !hnsw_pread_exact(hnsw_fd, row->upper_links[level],
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
    Buffer buf = ReadBuffer(rel, P_NEW);

    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    memcpy(BufferGetPage(buf), staged, BLCKSZ);
    MarkBufferDirty(buf);
    if (!skip_wal)
        log_newpage_buffer(buf, true);
    UnlockReleaseBuffer(buf);
}

static void
hnsw_write_disk_pages(Relation rel, int hnsw_fd, int tids_fd,
                      const HnswlibHeader *header, uint64_t n_nodes,
                      uint32_t dim, int bytes_per_dim, bool packed_nsw,
                      uint32_t pack_k, uint32_t data_page_count,
                      bool skip_wal,
                      HnswLocationTable *locations)
{
    HnswDiskRow row;
    Page staged = (Page)palloc(BLCKSZ);
    uint64_t upper_cursor;
    uint64_t level0_bytes;
    uint64_t page_start = 0;
    int m = (int)header->M;

    if (!hnsw_mul_u64(n_nodes, header->sizeDataPerElement, &level0_bytes) ||
        !hnsw_add_u64(HNSWLIB_HEADER_BYTES, level0_bytes, &upper_cursor))
        ereport(ERROR, (errmsg("pg_cuvs: HNSW level-0 offset overflow")));
    hnsw_disk_row_init(&row, header, dim);

    while (page_start < n_nodes)
    {
        uint64_t page_end;

        CHECK_FOR_INTERRUPTS();
        if (packed_nsw)
        {
            page_end = page_start + pack_k;
            if (page_end > n_nodes)
                page_end = n_nodes;
        }
        else
            page_end = page_start + 1;

        hnsw_init_disk_page(staged);
        if (!hnsw_set_disk_page_chain(
                staged,
                (packed_nsw ? page_start / pack_k : page_start) + 1,
                data_page_count))
            ereport(ERROR,
                    (errmsg("pg_cuvs: invalid source-less HNSW data page chain position")));

        for (uint64_t node = page_start; node < page_end; node++)
        {
            hnsw_read_disk_row(hnsw_fd, tids_fd, header, n_nodes, dim,
                               node, &upper_cursor, locations, &row);
            write_elem_pair(staged, &row.elem, row.vector, (int)dim,
                            bytes_per_dim, m, (size_t)n_nodes,
                            hnsw_location_table_lookup, locations);
        }
        hnsw_publish_staged_page(rel, staged, skip_wal);
        page_start = page_end;
    }
}

static void
hnsw_write_disk_metapage(Relation rel, const HnswlibHeader *header,
                         uint32_t dim, uint32_t n_data_pages, bool skip_wal,
                         HnswLocationTable *locations, const CuvsHnswPolicy *policy)
{
    Page staged = (Page)palloc(BLCKSZ);
    HnswNodeLocation entry;

    if (!hnsw_location_table_lookup(locations, (uint32_t)header->enterpointNode,
                                    &entry))
        ereport(ERROR, (errmsg("pg_cuvs: cannot resolve HNSW entrypoint")));
    if (!hnsw_init_metapage(staged, dim, (uint16_t)header->M,
                            (uint16_t)header->efConstruction,
                            entry.elem_blkno, entry.elem_offno, entry.level,
                            n_data_pages))
        ereport(ERROR, (errmsg("pg_cuvs: invalid HNSW metapage fields")));
    if (!cuvs_hnsw_policy_valid(policy))
        ereport(ERROR, (errmsg("pg_cuvs: missing or invalid native HNSW build policy receipt")));
    memcpy((char *)PageGetContents(staged) + sizeof(PgvHnswMeta), policy, sizeof(*policy));
    ((PageHeader)staged)->pd_lower += sizeof(*policy);
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
                               const CuvsHnswBuildResult *build)
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
                                   CUVS_HNSW_BUILD_M, CUVS_HNSW_BUILD_EF))
            ereport(ERROR, (errmsg("pg_cuvs: cannot initialize empty HNSW pages")));

        s->target = index_open(index_oid, AccessExclusiveLock);
        skip_wal = RelationGetForm(s->target)->relpersistence == RELPERSISTENCE_UNLOGGED;
        if (skip_wal)
            ereport(NOTICE,
                    (errmsg("pg_cuvs: target HNSW index is UNLOGGED — WAL skipped; "
                            "index must be rebuilt after crash recovery")));
        {
            SMgrRelation smgr = RelationGetSmgr(s->target);
            BlockNumber current = smgrnblocks(smgr, MAIN_FORKNUM);
            if (current > 0)
                RelationTruncate(s->target, 0);
            RelationSetTargetBlock(s->target, InvalidBlockNumber);
        }
        hnsw_publish_staged_page(s->target, metapage, skip_wal);
        hnsw_publish_staged_page(s->target, first_data_page, skip_wal);
        index_close(s->target, AccessExclusiveLock);
        s->target = NULL;
    }
    PG_FINALLY();
    {
        HnswDiskImportState *s = (HnswDiskImportState *)&state;
        if (s->target)
        {
            index_close(s->target, AccessExclusiveLock);
            s->target = NULL;
        }
    }
    PG_END_TRY();
}

static void
hnsw_import_build_result(Relation indexRel,
                         const CuvsHnswBuildResult *build,
                         bool packed_nsw)
{
    volatile HnswDiskImportState state;
    HnswlibHeader header;
    CuvsTidsHeader tids_header;
    uint32_t pack_k = 1;
    char error[256] = {0};
    Oid index_oid = RelationGetRelid(indexRel);
    Form_pg_attribute attr = TupleDescAttr(RelationGetDescr(indexRel), 0);
    int dim = (int)attr->atttypmod;
    int bytes_per_dim = bytes_per_dim_for_typid(attr->atttypid);
    bool skip_wal;
    uint32_t data_page_count;

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
                                 bytes_per_dim, packed_nsw,
                                 &header, &tids_header,
                                 hnsw_location_table_append, &s->locations,
                                 hnsw_pg_check_interrupts,
                                 error, sizeof(error)))
            ereport(ERROR,
                    (errmsg("pg_cuvs: invalid source-less HNSW files: %s", error)));
        if (!hnsw_location_table_rewind(&s->locations, build->n_vecs))
            ereport(ERROR,
                    (errmsg("pg_cuvs: incomplete disk-backed HNSW location table")));
        if (packed_nsw &&
            !hnsw_compute_pack_k(dim, bytes_per_dim, (int)header.M, &pack_k))
            ereport(ERROR,
                    (errmsg("pg_cuvs: NSW element and neighbor tuples do not fit on a page")));
        if (!hnsw_data_page_count(build->n_vecs, packed_nsw, pack_k,
                                  &data_page_count))
            ereport(ERROR,
                    (errmsg("pg_cuvs: HNSW data page count exceeds block number limits")));

        s->target = index_open(index_oid, AccessExclusiveLock);
        skip_wal = RelationGetForm(s->target)->relpersistence == RELPERSISTENCE_UNLOGGED;
        if (skip_wal)
            ereport(NOTICE,
                    (errmsg("pg_cuvs: target HNSW index is UNLOGGED — WAL skipped; "
                            "index must be rebuilt after crash recovery")));
        {
            SMgrRelation smgr = RelationGetSmgr(s->target);
            BlockNumber current = smgrnblocks(smgr, MAIN_FORKNUM);
            if (current > 0)
                RelationTruncate(s->target, 0);
            RelationSetTargetBlock(s->target, InvalidBlockNumber);
        }
        hnsw_write_disk_metapage(s->target, &header, build->dim,
                                 data_page_count, skip_wal, &s->locations, &build->policy);
        hnsw_write_disk_pages(s->target, build->hnsw_fd, build->tids_fd,
                              &header, build->n_vecs, build->dim,
                              bytes_per_dim, packed_nsw, pack_k,
                              data_page_count, skip_wal,
                              &s->locations);
        index_close(s->target, AccessExclusiveLock);
        s->target = NULL;
    }
    PG_FINALLY();
    {
        HnswDiskImportState *s = (HnswDiskImportState *)&state;
        if (s->target)
        {
            index_close(s->target, AccessExclusiveLock);
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
 * Skips the hnswlib intermediate format entirely. Gets CAGRA adjacency
 * + corpus vectors from the daemon via IPC, builds a flat pgvector HNSW
 * (all nodes at level 0), and writes it directly into the target relation.
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
 * mode='nsw'  → flat NSW (no hierarchy, level 0 only)
 * mode='hnsw' → hierarchical with heuristic neighbor selection */
static void
fill_hnsw_from_cagra_ipc_impl(Oid cagra_oid, Oid hnsw_oid, const char *mode,
                              HnswFillRes *res)
{
    bool do_hierarchy = (strcmp(mode, "hnsw") == 0);

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
    /* Verify elem+neigh fit on one page (worst-case: max_level, M neighbors)
     * BEFORE computing pack_k below — pack_k divides the loop that follows,
     * and elems_per_page() (which both this check and pack_k are built on)
     * returns 0 when a pair doesn't fit at all. This call is what turns
     * that 0 into a clean ERROR instead of a SIGFPE two lines down; it must
     * run first, not after. (It used to run after the old one-element/page
     * loop too, harmlessly, since that loop never divided by anything —
     * this ordering only became load-bearing when the loop below started
     * dividing by pack_k.) */
    check_hnsw_page_fit((int)ipc_dim, bytes_per_dim, M, max_level);

    /* #161: pack k elements/page on the nsw path (do_hierarchy=false), where
     * every element is level 0 and neighbor-tuple size is therefore uniform
     * — the one case a single fixed k packs losslessly (see
     * write_elem_pages_packed()'s header comment). The hierarchical path
     * keeps one element per page: per-element level varies there, so no
     * single k fits every element without wasting space on the low-level
     * majority — out of scope for #161 (mode='nsw' is the recommended
     * default per #161's own lever-2b measurement; 'hnsw' is not). */
    int pack_k = do_hierarchy ? 1 : elems_per_page((int)ipc_dim, bytes_per_dim, M, 0);
    if (pack_k < 1)
        ereport(ERROR,
                (errmsg("pg_cuvs: internal error — pack_k=%d after "
                        "check_hnsw_page_fit() passed (dim=%d, bytes_per_dim=%d, M=%d)",
                        pack_k, (int)ipc_dim, bytes_per_dim, M)));
    for (size_t i = 0; i < N; i++)
    {
        size_t   page_idx = i / (size_t) pack_k;
        size_t   slot      = i % (size_t) pack_k;
        uint32_t blkno = (uint32_t)(page_idx + 1);   /* block 0 = meta */
        elems[i].elem_blkno  = blkno;
        elems[i].elem_offno  = (uint16_t)(2 * slot + 1);
        elems[i].neigh_blkno = blkno;
        elems[i].neigh_offno = (uint16_t)(2 * slot + 2);
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

    /* ---- 6. Write metapage (block 0) ---- */
    {
        Buffer buf = ReadBuffer(hnsw_rel, P_NEW);
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        Page page = BufferGetPage(buf);
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
        metap->efConstruction = (uint16_t)(M0 * 2);  /* reasonable default */
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
        metap->insertPage = (N > 0) ? elems[N - 1].elem_blkno : (uint32_t) N;

        ((PageHeader)page)->pd_lower =
            (uint16_t)(((char *)metap + sizeof(PgvHnswMeta)) - (char *)page);

        MarkBufferDirty(buf);
        if (!hnsw_unlogged)
            log_newpage_buffer(buf, true);
        UnlockReleaseBuffer(buf);
    }

    /* ---- 7. Write element+neighbor pages ---- */
    /* #161: pack_k>1 only when !do_hierarchy (nsw); the packed writer with
     * pack_k==1 would behave identically to the per-element loop below (one
     * ReadBuffer(P_NEW) per element) but takes the dedicated path anyway to
     * keep this branch a direct, obviously-correct mirror of the layout
     * pass's pack_k choice above. */
    if (pack_k > 1)
    {
        write_elem_pages_packed(hnsw_rel, elems, vecs, ipc_dim, bytes_per_dim,
                                M, N, pack_k, hnsw_unlogged,
                                hnsw_lookup_elem_array, elems);
    }
    else
    {
        for (size_t i = 0; i < N; i++)
        {
            const float *vec = vecs + i * (size_t)ipc_dim;

            write_elem_page(hnsw_rel,
                            elems[i].elem_blkno,
                            &elems[i],
                            vec,
                            ipc_dim, bytes_per_dim, M,
                            N, hnsw_unlogged,
                            hnsw_lookup_elem_array, elems);
        }
    }

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
fill_hnsw_from_cagra_ipc(Oid cagra_oid, Oid hnsw_oid, const char *mode)
{
    /* #166 review 3: volatile across the longjmp — see fill_hnsw_from_hnswlib. */
    volatile HnswFillRes res;

    memset((void *) &res, 0, sizeof(res));
    res.shm_fd = -1;            /* #165: 0 is stdin — never a valid sentinel */
    PG_TRY();
    {
        fill_hnsw_from_cagra_ipc_impl(cagra_oid, hnsw_oid, mode,
                                      (HnswFillRes *) &res);
    }
    PG_FINALLY();
    {
        hnsw_fill_res_free((HnswFillRes *) &res);
    }
    PG_END_TRY();
    return res.n_elems;
}

/* ================================================================
 * pg_cuvs_build_hnsw — unified GPU import: creates HNSW WITHOUT CPU build.
 *
 * Uses INDEX_CREATE_SKIP_BUILD so pgvector's CPU ambuild() is never called.
 * The 285s CPU HNSW build is eliminated entirely.
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
create_empty_hnsw(Oid cagra_oid)
{
    HeapTuple ix_tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(cagra_oid));
    if (!HeapTupleIsValid(ix_tup))
        ereport(ERROR, (errmsg("pg_cuvs: CAGRA index %u not found", cagra_oid)));
    Oid heap_oid = ((Form_pg_index) GETSTRUCT(ix_tup))->indrelid;
    ReleaseSysCache(ix_tup);

    if (!object_ownercheck(RelationRelationId, heap_oid, GetUserId()))
        aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE,
                       get_rel_name(heap_oid));

    uint32_t metric       = cagra_index_metric(cagra_oid);
    Oid      hnsw_opclass = find_hnsw_opclass_oid(metric);

    Relation heap_rel  = table_open(heap_oid, ShareLock);
    Relation cagra_rel = index_open(cagra_oid, AccessShareLock);

    AttrNumber col_attnum = cagra_rel->rd_index->indkey.values[0];
    Form_pg_attribute att = TupleDescAttr(heap_rel->rd_att, col_attnum - 1);
    char  *col_name  = pstrdup(NameStr(att->attname));
    Oid    collation = att->attcollation;
    int16  coloption = 0;

    IndexInfo *iinfo = BuildIndexInfo(cagra_rel);
    iinfo->ii_Predicate      = NIL;
    iinfo->ii_PredicateState = NULL;

    index_close(cagra_rel, AccessShareLock);

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

    table_close(heap_rel, ShareLock);
    CommandCounterIncrement();
    return hnsw_oid;
}

PG_FUNCTION_INFO_V1(pg_cuvs_build_hnsw);
Datum
pg_cuvs_build_hnsw(PG_FUNCTION_ARGS)
{
    Oid         cagra_oid = PG_GETARG_OID(0);
    const char *mode = (PG_NARGS() >= 2 && !PG_ARGISNULL(1)) ?
                       text_to_cstring(PG_GETARG_TEXT_PP(1)) : "nsw";

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
    {
        Relation cagra_rel_pc = index_open(cagra_oid, AccessShareLock);
        if (cagra_rel_pc->rd_rel->relam != get_am_oid("cagra", false))
        {
            index_close(cagra_rel_pc, AccessShareLock);
            ereport(ERROR,
                    (errmsg("pg_cuvs_build_hnsw: \"%s\" is not a cagra index",
                            get_rel_name(cagra_oid))));
        }
        int precheck_dim = (int) TupleDescAttr(RelationGetDescr(cagra_rel_pc), 0)->atttypmod;
        int graph_degree  = cuvs_cagra_reloption_graph_degree(cagra_rel_pc);
        index_close(cagra_rel_pc, AccessShareLock);
        int m0 = (graph_degree < 64) ? graph_degree : 64;
        /* mode='nsw' always builds level 0 only (see cuvs_hnsw_ambuild()'s
         * matching comment) -- the exact bound, not a guess, there. */
        int precheck_maxlevel = (strcmp(mode, "nsw") == 0) ? 0 : 5;
        check_hnsw_page_fit(precheck_dim, 4, m0 / 2, precheck_maxlevel);
    }

    /* Create empty HNSW on parent table — INDEX_CREATE_SKIP_BUILD, no CPU build */
    Oid hnsw_oid = create_empty_hnsw(cagra_oid);

    /* Dispatch to internal C helpers — no SQL/SPI overhead.
     *
     * RECOMMENDED modes:
     *   'nsw'     — flat NSW via IPC adjacency. 117s, 2.4x speedup.
     *               Empirically equal quality at ef>=40 (Cohere 1M×1024 benchmark).
     *   'hnswlib' — from_cagra() hierarchy via /dev/shm (no disk I/O). 139s, 2.0x.
     *               Slight recall advantage at ef<20.
     *
     * HIDDEN modes (kept for research, not recommended):
     *   'hnsw'         — direct hierarchy with heuristic neighbor selection.
     *                    Currently 144s with no quality gain over 'nsw'.
     *                    Retained pending future improvement of level assignment.
     *   'hnswlib_file' — on-disk .hnsw sidecar; superceded by 'hnswlib' (shm).
     */
    if (strcmp(mode, "nsw") == 0 || strcmp(mode, "hnsw") == 0)
        fill_hnsw_from_cagra_ipc(cagra_oid, hnsw_oid, mode);
    else if (strcmp(mode, "hnswlib") == 0)
        fill_hnsw_from_hnswlib(cagra_oid, hnsw_oid, true);
    else  /* hnswlib_file */
        fill_hnsw_from_hnswlib(cagra_oid, hnsw_oid, false);

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
 * (scan/cost/insert/bulkdelete/vacuum) is delegated to pgvector's hnsw AM by
 * copying its IndexAmRoutine and overriding only ambuild + amoptions. A single
 * catalog index results, so pg_indexes / DROP INDEX / REINDEX / pg_dump behave
 * naturally.
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
    if (!value || !strcmp(value, "auto") || !strcmp(value, "ivf_pq")) return;
    if (!strcmp(value, "nn_descent"))
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("pg_cuvs_hnsw: build_algo=nn_descent is reserved but unsupported"),
            errdetail("NN-descent has no supported memory estimate for this build path."),
            errhint("Use build_algo=auto (bounded resident IVF-PQ or ACE-disk).")));
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
        errmsg("pg_cuvs_hnsw: invalid build_algo \"%s\"", value),
        errhint("Valid supported values: auto, ivf_pq.")));
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
                         "HNSW route: auto or explicit ivf_pq; estimates are not memory upper bounds",
                         "auto", cuvs_hnsw_validate_build_algo, AccessExclusiveLock);
    add_int_reloption(cuvs_hnsw_relopt_kind, "m",
                      "pgvector HNSW M; informational (graph degree drives layout)",
                      16, 2, 100, AccessExclusiveLock);
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

/* Read the build-time receipt, never infer history from today's GUCs. The
 * pgvector-compatible prefix stays unchanged, including for INSERT/VACUUM. */
PG_FUNCTION_INFO_V1(pg_cuvs_hnsw_build_info);
Datum
pg_cuvs_hnsw_build_info(PG_FUNCTION_ARGS)
{
    Oid oid = PG_GETARG_OID(0);
    Relation rel = index_open(oid, AccessShareLock);
    CuvsHnswPolicy policy = {0};
    StringInfoData json;
    Oid heap;
    if (rel->rd_rel->relam != get_am_oid("pg_cuvs_hnsw", true))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("pg_cuvs_hnsw_build_info requires a pg_cuvs_hnsw index")));
    heap = IndexGetRelation(oid, false);
    if (pg_class_aclcheck(heap, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
        aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, get_rel_name(heap));
    if (RelationGetNumberOfBlocks(rel) > 0)
    {
        Buffer buffer = ReadBuffer(rel, 0);
        Page page;
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buffer);
        if (((PageHeader)page)->pd_lower >= SizeOfPageHeaderData + sizeof(PgvHnswMeta) + sizeof(policy))
            memcpy(&policy, (char *)PageGetContents(page) + sizeof(PgvHnswMeta), sizeof(policy));
        UnlockReleaseBuffer(buffer);
    }
    index_close(rel, AccessShareLock);
    if (policy.magic == CUVS_HNSW_POLICY_MAGIC && !cuvs_hnsw_policy_valid(&policy))
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
            errmsg("pg_cuvs: invalid or unsupported stored HNSW policy receipt")));
    if (!cuvs_hnsw_policy_valid(&policy))
        PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in,
            CStringGetDatum("{\"status\":\"not_recorded\",\"reason\":\"legacy, empty, or reused source graph; rebuild to record a native policy\"}")));
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
        "\"guard\":\"selected_route_returned\",\"memory_upper_bound_guaranteed\":false}",
        policy.version, cuvs_hnsw_route_name(policy.route),
        (unsigned long long)policy.n_rows, policy.dim, policy.metric,
        policy.requested_algo ? "ivf_pq" : "auto", policy.requested_ef,
        policy.degree, policy.intermediate, policy.hierarchy,
        policy.hierarchy_ef, policy.ace_ef, policy.partitions,
        policy.route == CUVS_HNSW_ROUTE_ACE ? "same_SDK_stable_explicit_plan" : "not_applicable",
        policy.internal_builder == 1 ? "exact" : policy.internal_builder == 2 ? "ivf_pq" : "nn_descent",
        policy.n_lists, policy.pq_dim, policy.pq_bits, policy.n_probes, policy.query_batch,
        (unsigned long long)policy.training_requested, (unsigned long long)policy.training_actual,
        (unsigned long long)policy.host_estimate, (unsigned long long)policy.gpu_estimate,
        (unsigned long long)policy.host_budget, (unsigned long long)policy.gpu_budget,
        (unsigned long long)policy.sdk_host_budget, (unsigned long long)policy.sdk_gpu_budget);
    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(json.data)));
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
static IndexBuildResult *
cuvs_hnsw_ambuild(Relation heapRel, Relation indexRel, IndexInfo *indexInfo)
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
    cuvs_hnsw_validate_build_algo(algorithm);
    uint32_t build_algo = !strcmp(algorithm, "ivf_pq") ? CUVS_HNSW_ALGO_IVF_PQ : CUVS_HNSW_ALGO_AUTO;
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
     * mode='nsw' is the one case where maxlevel is not a guess: do_hierarchy
     * in fill_hnsw_from_cagra_ipc_impl() is only true for mode='hnsw', so
     * 'nsw' always builds every element at level 0 (elems[i].level stays
     * calloc'd 0, max_level=0). Using the conservative 5 there anyway
     * rejects fp32 builds in the dim≈1679–1918 band that built fine before
     * this pre-check existed — a real regression on the default mode, not a
     * conservative-but-safe over-reject. Every other mode ('hnsw',
     * 'hnswlib', 'hnswlib_file') genuinely can reach a nonzero level, so
     * they keep the conservative bound. */
    Form_pg_attribute precheck_att = TupleDescAttr(RelationGetDescr(indexRel), 0);
    int precheck_dim           = (int) precheck_att->atttypmod;
    int precheck_bytes_per_dim = bytes_per_dim_for_typid(precheck_att->atttypid);
    const int safe_maxlevel = (strcmp(mode, "nsw") == 0) ? 0 : 5;

    /* ── Source-less: build and convert from regular disk files. ── */
    if (source_name[0] == '\0')
    {
        volatile CuvsHnswBuildResult build;

        /* The helper uses graph degree 64, which serializes HNSW M=32. */
        check_hnsw_page_fit(precheck_dim, precheck_bytes_per_dim,
                            CUVS_HNSW_BUILD_M, safe_maxlevel);
        memset((void *)&build, 0, sizeof(build));
        ((CuvsHnswBuildResult *)&build)->hnsw_fd = -1;
        ((CuvsHnswBuildResult *)&build)->tids_fd = -1;
        PG_TRY();
        {
            CuvsHnswBuildResult *b = (CuvsHnswBuildResult *)&build;
            cuvs_build_hnsw_from_heap(heapRel, indexRel, indexInfo, mode, build_algo, requested_ef, b);
            result->heap_tuples = b->reltuples;
            result->index_tuples = (double)b->n_vecs;
            if (b->n_vecs > 0)
                hnsw_import_build_result(indexRel, b, strcmp(mode, "nsw") == 0);
            else
                hnsw_import_empty_build_result(indexRel, b);
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
    /* Resolve source name (schema-qualified, search_path aware) to an OID. */
    Oid cagra_oid = DatumGetObjectId(
        DirectFunctionCall1(regclassin, CStringGetDatum((char *) source_name)));

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
        {
            Relation cagra_rel_gd = index_open(cagra_oid, AccessShareLock);
            int graph_degree = cuvs_cagra_reloption_graph_degree(cagra_rel_gd);
            index_close(cagra_rel_gd, AccessShareLock);
            int m0 = (graph_degree < 64) ? graph_degree : 64;
            check_hnsw_page_fit(precheck_dim, precheck_bytes_per_dim,
                                 m0 / 2, safe_maxlevel);
        }

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

    /* Fill THIS index's relation (pgvector page format) from the CAGRA graph.
     * fill_* open the target by OID with AccessExclusiveLock; re-locking an
     * object this backend already holds (the index under construction) is a
     * no-op re-grant, and the freshly created relation has 0 blocks so no
     * truncate happens. */
    size_t n_elems_written;
    if (strcmp(mode, "nsw") == 0 || strcmp(mode, "hnsw") == 0)
        n_elems_written = fill_hnsw_from_cagra_ipc(cagra_oid, self_oid, mode);
    else if (strcmp(mode, "hnswlib") == 0)
        n_elems_written = fill_hnsw_from_hnswlib(cagra_oid, self_oid, true);
    else  /* hnswlib_file */
        n_elems_written = fill_hnsw_from_hnswlib(cagra_oid, self_oid, false);

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

PG_FUNCTION_INFO_V1(pg_cuvs_hnsw_handler);
Datum
pg_cuvs_hnsw_handler(PG_FUNCTION_ARGS)
{
    /* Borrow pgvector hnsw's IndexAmRoutine for the entire read path, then
     * override only the build + options entry points (Phase 3K). Copying the
     * routine inherits amsupport / amcanorderbyop / scan / cost / vacuum so the
     * built index is served by pgvector's own hnsw code. */
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

    amroutine->ambuild   = cuvs_hnsw_ambuild;   /* GPU CAGRA -> HNSW pages */
    amroutine->amoptions = cuvs_hnsw_amoptions;  /* WITH (source, mode, ...) */
    /* ambuildempty / aminsert / scan / cost / vacuum stay pgvector's. */

    PG_RETURN_POINTER(amroutine);
}
