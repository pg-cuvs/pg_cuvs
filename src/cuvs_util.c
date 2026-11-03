/*
 * cuvs_util.c — shared, dependency-free helpers for pg_cuvs.
 *
 * Compiled into both the PostgreSQL extension (.so) and the standalone
 * daemon, and linked directly by the unit-test binary. Contains no
 * PostgreSQL, CUDA, or cuVS dependencies.
 */

#include "cuvs_util.h"
#include "cuvs_ipc.h"   /* CUVS_STATUS_* defines (PG-free, CUDA-free) */
#include "cuvs_admission_contract.h"

#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>   /* offsetof (cuvs_hw_profile crc range) */

int
cuvs_parse_rmm_pool_mb(const char *value, uint32_t *mb_out)
{
    char *end;
    unsigned long long mb;
    if (!value || !value[0] || strspn(value, "0123456789") != strlen(value))
        return -1;
    errno = 0;
    mb = strtoull(value, &end, 10);
    if (errno || *end || mb > INT_MAX || mb > SIZE_MAX / (1024u * 1024u))
        return -1;
    *mb_out = (uint32_t) mb;
    return 0;
}

int
cuvs_parse_rmm_pool_percent(const char *value, uint32_t *percent_out)
{
    uint32_t percent;
    if (cuvs_parse_rmm_pool_mb(value, &percent) != 0 || percent < 1 || percent > 100)
        return -1;
    *percent_out = percent;
    return 0;
}

int
cuvs_parse_scratch_permille(const char *value, uint32_t *permille_out)
{
    uint32_t permille;
    if (cuvs_parse_rmm_pool_mb(value, &permille) != 0 ||
        permille > CUVS_ADMISSION_RHO_PERMILLE_MAX)
        return -1;
    *permille_out = permille;
    return 0;
}

size_t
cuvs_rmm_pool_max_bytes(size_t total_bytes, uint32_t percent)
{
    size_t bytes;
    if (percent < 1 || percent > 100)
        return 0;
    /* Divide first so even SIZE_MAX * 100 cannot overflow. */
    bytes = (total_bytes / 100) * percent + ((total_bytes % 100) * percent) / 100;
    return bytes & ~(size_t)255;
}

size_t
cuvs_rmm_pool_headroom(size_t physical_free, size_t async_reclaimable,
                       size_t quota, size_t reserved, size_t used)
{
    size_t physical = async_reclaimable > SIZE_MAX - physical_free
                    ? SIZE_MAX : physical_free + async_reclaimable;
    size_t growable;
    if (quota == 0)
        return physical;
    if (reserved > quota || used > reserved)
        return 0;
    growable = quota - reserved;
    if (growable > physical)
        growable = physical;
    return reserved - used + growable;
}

/* ----------------------------------------------------------------
 * Fail-closed filter-frame gate (ADR-063 tenant isolation).
 * ---------------------------------------------------------------- */
bool
cuvs_filter_frame_refuses(uint32_t n_filter_tids, bool filter_available)
{
    return n_filter_tids > 0 && !filter_available;
}

/* ----------------------------------------------------------------
 * Index filename parsing: "<db_oid>_<index_oid>.cagra"
 * ---------------------------------------------------------------- */
int
cuvs_parse_index_filename(const char *name, uint32_t *db_oid, uint32_t *index_oid)
{
    int consumed = 0;

    if (!name)
        return -1;

    /* Require the ENTIRE name to match "<db>_<index>.cagra". The %n captures
     * how many chars were consumed once the ".cagra" literal matches; requiring
     * consumed == strlen(name) rejects trailing extensions (".cagra.bak") and,
     * importantly, Phase 3F shard artifacts ("<db>_<idx>.s000.cagra") whose
     * ".cagra" suffix would otherwise fool a plain suffix check. */
    if (sscanf(name, "%u_%u.cagra%n", db_oid, index_oid, &consumed) == 2
        && consumed == (int)strlen(name))
        return 0;
    return -1;
}

/* ----------------------------------------------------------------
 * IPC status code -> human-readable string
 * ---------------------------------------------------------------- */
const char *
cuvs_status_str(int status)
{
    switch (status)
    {
        case CUVS_STATUS_OK:           return "ok";
        case CUVS_STATUS_ERROR:        return "error";
        case CUVS_STATUS_OOM_FALLBACK: return "oom_fallback";
        case CUVS_STATUS_NOT_FOUND:    return "not_found";
        case CUVS_STATUS_UNAVAILABLE:  return "unavailable";
        case CUVS_STATUS_BUILD_FAILED:   return "build_failed";
        case CUVS_STATUS_PERSIST_FAILED: return "persist_failed";
        case CUVS_STATUS_DIM_MISMATCH:   return "dim_mismatch";
        case CUVS_STATUS_METRIC_MISMATCH: return "metric_mismatch";
        case CUVS_STATUS_STALE:          return "stale";
        case CUVS_STATUS_PROTO_MISMATCH: return "proto_mismatch";
        default:                       return "unknown";
    }
}

/* ----------------------------------------------------------------
 * Opclass name -> CUVS_METRIC_*. Returns -1 for an unrecognized name.
 * Pure (no PG/CUDA); the PG glue resolves the index's opclass name and
 * calls this so build and search agree on the metric.
 * ---------------------------------------------------------------- */
int
cuvs_metric_from_opclass_name(const char *name)
{
    if (!name)
        return -1;
    if (strcmp(name, "vector_l2_ops") == 0)
        return CUVS_METRIC_L2;
    if (strcmp(name, "vector_cosine_ops") == 0)
        return CUVS_METRIC_COSINE;
    if (strcmp(name, "vector_ip_ops") == 0)
        return CUVS_METRIC_IP;
    return -1;
}

static int
compare_u64(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *) left;
    uint64_t b = *(const uint64_t *) right;

    return (a > b) - (a < b);
}

void
cuvs_u64_sort(uint64_t *values, size_t n)
{
    if (values && n > 1)
        qsort(values, n, sizeof(*values), compare_u64);
}

int
cuvs_u64_contains(const uint64_t *sorted_values, size_t n, uint64_t target)
{
    size_t lo = 0;
    size_t hi = n;

    while (lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2;
        uint64_t value = sorted_values[mid];

        if (value == target)
            return 1;
        if (value < target)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

/* ----------------------------------------------------------------
 * Latency histogram: log2-spaced buckets + approximate percentiles.
 * ---------------------------------------------------------------- */
uint32_t
cuvs_lat_bucket_index(uint32_t us)
{
    uint32_t idx = 0;
    /* bucket 0 = {0}; bucket k>=1 covers [2^(k-1), 2^k). */
    while (us > 0 && idx < CUVS_LAT_BUCKETS - 1)
    {
        us >>= 1;
        idx++;
    }
    return idx;
}

/* Upper-edge latency (us) represented by a bucket: bucket 0 -> 0,
 * bucket k>=1 -> 2^k. Clamped index never exceeds CUVS_LAT_BUCKETS-1. */
static uint32_t
lat_bucket_upper_us(int idx)
{
    if (idx <= 0)
        return 0;
    if (idx >= 31)              /* avoid UB: 1u<<31 fits, beyond would overflow */
        return 0x80000000u;
    return 1u << idx;
}

uint32_t
cuvs_lat_percentile(const uint32_t *buckets, int nbuckets, double q)
{
    uint64_t total = 0;
    uint64_t target;
    uint64_t cum = 0;

    for (int i = 0; i < nbuckets; i++)
        total += buckets[i];
    if (total == 0)
        return 0;

    if (q < 0.0) q = 0.0;
    if (q > 1.0) q = 1.0;

    /* Rank of the target sample (1-based): the smallest count whose
     * cumulative coverage reaches the q-quantile. */
    target = (uint64_t)(q * (double)total);
    if (target == 0)
        target = 1;

    for (int i = 0; i < nbuckets; i++)
    {
        cum += buckets[i];
        if (cum >= target)
            return lat_bucket_upper_us(i);
    }
    return lat_bucket_upper_us(nbuckets - 1);
}

/* ----------------------------------------------------------------
 * CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) and versioned .tids I/O.
 * LE-only: see cuvs_util.h header comment.
 * ---------------------------------------------------------------- */
static uint32_t crc32_table[256];
static int      crc32_table_init = 0;

static void
crc32_ensure_table(void)
{
    if (crc32_table_init)
        return;
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_table_init = 1;
}

uint32_t
cuvs_crc32_stream_begin(void)
{
    crc32_ensure_table();
    return 0xFFFFFFFFu;
}

uint32_t
cuvs_crc32_stream_update(uint32_t crc, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    crc32_ensure_table();
    for (size_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

uint32_t
cuvs_crc32_stream_end(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFu;
}

uint32_t
cuvs_crc32(const void *data, size_t len)
{
    return cuvs_crc32_stream_end(
        cuvs_crc32_stream_update(cuvs_crc32_stream_begin(), data, len));
}

int
cuvs_tids_write(FILE *f, int64_t n_vecs, uint32_t dim, uint32_t metric,
                const uint64_t *tids)
{
    CuvsTidsHeader hdr;
    hdr.magic      = CUVS_TIDS_MAGIC;
    hdr.version    = CUVS_TIDS_VERSION;
    hdr.n_vecs     = n_vecs;
    hdr.dim        = dim;
    hdr.metric     = metric;
    hdr.body_crc32 = cuvs_crc32(tids, (size_t)n_vecs * sizeof(uint64_t));
    hdr.reserved   = 0;

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1)
        return -1;
    if (fwrite(tids, sizeof(uint64_t), (size_t)n_vecs, f) != (size_t)n_vecs)
        return -1;
    return 0;
}

int
cuvs_tids_read(FILE *f, CuvsTidsHeader *hdr_out, uint64_t **tids_out)
{
    CuvsTidsHeader hdr;
    size_t body_bytes;
    uint64_t *tids;

    *tids_out = NULL;

    if (fread(&hdr, sizeof(hdr), 1, f) != 1)
        return -1;
    if (hdr.magic != CUVS_TIDS_MAGIC)
        return -1;
    if (hdr.version != CUVS_TIDS_VERSION)
        return -1;
    if (hdr.n_vecs <= 0 || hdr.n_vecs > CUVS_TIDS_MAX_VECS)
        return -1;
    /* reserved must be 0 (forward-compat: a future format may use it, so a
     * v1 reader must reject files that set it rather than silently accept). */
    if (hdr.reserved != 0)
        return -1;

    body_bytes = (size_t)hdr.n_vecs * sizeof(uint64_t);
    tids = malloc(body_bytes);
    if (!tids)
        return -1;

    if (fread(tids, sizeof(uint64_t), (size_t)hdr.n_vecs, f) != (size_t)hdr.n_vecs)
    {
        free(tids);
        return -1;
    }
    if (cuvs_crc32(tids, body_bytes) != hdr.body_crc32)
    {
        free(tids);
        return -1;
    }

    if (hdr_out)
        *hdr_out = hdr;
    *tids_out = tids;
    return 0;
}

/* ----------------------------------------------------------------
 * Versioned .vectors sidecar I/O (Phase 3L brute-force mode).
 * Mirrors cuvs_tids_write/read; body is the row-major float matrix.
 * ---------------------------------------------------------------- */
int
cuvs_vectors_write(FILE *f, int64_t n_vecs, uint32_t dim, uint32_t metric,
                   uint32_t base_tids_crc32, const float *vecs)
{
    CuvsVectorsHeader hdr;
    size_t body_n = (size_t)n_vecs * (size_t)dim;

    memset(&hdr, 0, sizeof(hdr));   /* zero struct padding -> deterministic header */
    hdr.magic           = CUVS_VECTORS_MAGIC;
    hdr.version         = CUVS_VECTORS_VERSION;
    hdr.n_vecs          = n_vecs;
    hdr.dim             = dim;
    hdr.metric          = metric;
    hdr.body_crc32      = cuvs_crc32(vecs, body_n * sizeof(float));
    hdr.base_tids_crc32 = base_tids_crc32;
    hdr.reserved        = 0;

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1)
        return -1;
    if (fwrite(vecs, sizeof(float), body_n, f) != body_n)
        return -1;
    return 0;
}

/* ADR-059: write the .vectors sidecar from N partitions without a contiguous
 * host copy. The body crc32 is accumulated incrementally over the partitions in
 * concatenation order, then the body is written partition-by-partition. Output
 * is byte-identical to cuvs_vectors_write over the logically concatenated
 * corpus (Σ n_each rows). Empty partitions (n_each[i] <= 0) are skipped. */
int
cuvs_vectors_write_multi(FILE *f, const int64_t *n_each, int n_parts, uint32_t dim,
                         uint32_t metric, uint32_t base_tids_crc32,
                         const float *const *part_vecs)
{
    CuvsVectorsHeader hdr;
    int64_t  total = 0;
    uint32_t crc = cuvs_crc32_stream_begin();
    int      i;

    memset(&hdr, 0, sizeof(hdr));   /* zero struct padding -> deterministic header */

    for (i = 0; i < n_parts; i++)
    {
        size_t body_n;
        if (n_each[i] <= 0)
            continue;
        body_n = (size_t)n_each[i] * (size_t)dim;
        crc = cuvs_crc32_stream_update(crc, part_vecs[i], body_n * sizeof(float));
        total += n_each[i];
    }

    hdr.magic           = CUVS_VECTORS_MAGIC;
    hdr.version         = CUVS_VECTORS_VERSION;
    hdr.n_vecs          = total;
    hdr.dim             = dim;
    hdr.metric          = metric;
    hdr.body_crc32      = cuvs_crc32_stream_end(crc);
    hdr.base_tids_crc32 = base_tids_crc32;
    hdr.reserved        = 0;

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1)
        return -1;
    for (i = 0; i < n_parts; i++)
    {
        size_t body_n;
        if (n_each[i] <= 0)
            continue;
        body_n = (size_t)n_each[i] * (size_t)dim;
        if (fwrite(part_vecs[i], sizeof(float), body_n, f) != body_n)
            return -1;
    }
    return 0;
}

int
cuvs_vectors_read(FILE *f, CuvsVectorsHeader *hdr_out, float **vecs_out)
{
    CuvsVectorsHeader hdr;
    size_t body_n;
    float *vecs;

    *vecs_out = NULL;

    if (fread(&hdr, sizeof(hdr), 1, f) != 1)
        return -1;
    if (hdr.magic != CUVS_VECTORS_MAGIC)
        return -1;
    if (hdr.version != CUVS_VECTORS_VERSION)
        return -1;
    if (hdr.n_vecs <= 0 || hdr.n_vecs > CUVS_TIDS_MAX_VECS)
        return -1;
    if (hdr.dim == 0 || hdr.dim > CUVS_VECTORS_MAX_DIM)
        return -1;
    /* reserved must be 0 (forward-compat, like .tids). */
    if (hdr.reserved != 0)
        return -1;

    body_n = (size_t)hdr.n_vecs * (size_t)hdr.dim;
    vecs = malloc(body_n * sizeof(float));
    if (!vecs)
        return -1;

    if (fread(vecs, sizeof(float), body_n, f) != body_n)
    {
        free(vecs);
        return -1;
    }
    if (cuvs_crc32(vecs, body_n * sizeof(float)) != hdr.body_crc32)
    {
        free(vecs);
        return -1;
    }

    if (hdr_out)
        *hdr_out = hdr;
    *vecs_out = vecs;
    return 0;
}

/* ----------------------------------------------------------------
 * Hardware profile (ADR-075) write/read. Self-contained struct; body_crc32
 * covers all bytes after it (offsetof(probe_status)..end).
 * ---------------------------------------------------------------- */
/* On-disk wire format is fixed; a field reorder that changes these sizes would
 * silently break cross-daemon reads. Fail the build instead. */
_Static_assert(CUVS_HWPROFILE_SZ_V1 == 136, "cuvs_hw_profile v1 wire size drifted");
_Static_assert(CUVS_HWPROFILE_SZ_V2 == 152, "cuvs_hw_profile v2 wire size drifted");

int
cuvs_hw_profile_write(FILE *f, CuvsHwProfile *p)
{
    size_t off = offsetof(CuvsHwProfile, probe_status);

    p->magic      = CUVS_HWPROFILE_MAGIC;
    p->version    = CUVS_HWPROFILE_VERSION;
    p->reserved   = 0;
    p->body_crc32 = cuvs_crc32((const char *)p + off, sizeof(*p) - off);

    if (fwrite(p, sizeof(*p), 1, f) != 1)
        return -1;
    return 0;
}

int
cuvs_hw_profile_read(FILE *f, CuvsHwProfile *out)
{
    CuvsHwProfile p;
    size_t        off = offsetof(CuvsHwProfile, probe_status);
    size_t        total_sz, body_sz;
    uint32_t      hdr[3];   /* magic, version, body_crc32 — the pre-CRC header */

    /* Seed DEFAULTs so reading a v1 file leaves the v2 tail (cpu_dist_tput,
     * gpu_cagra_lat_us) at compiled DEFAULT with their probe bits clear → the
     * planner falls back to the legacy cost path for those coefficients. */
    memset(&p, 0, sizeof(p));
    p.cpu_dist_tput    = CUVS_HWP_DEFAULT_CPU_DIST;
    p.gpu_cagra_lat_us = CUVS_HWP_DEFAULT_CAGRA_LAT;

    if (fread(hdr, sizeof(hdr), 1, f) != 1)   /* off == sizeof(hdr) == 12 */
        return -1;
    if (hdr[0] != CUVS_HWPROFILE_MAGIC)
        return -1;
    if (hdr[1] == 1u)
        total_sz = CUVS_HWPROFILE_SZ_V1;
    else if (hdr[1] == 2u)
        total_sz = CUVS_HWPROFILE_SZ_V2;
    else
        return -1;                            /* unknown version */
    p.magic = hdr[0]; p.version = hdr[1]; p.body_crc32 = hdr[2];

    body_sz = total_sz - off;                 /* probe_status..end for this version */
    if (fread((char *) &p + off, body_sz, 1, f) != 1)
        return -1;
    if (p.reserved != 0)
        return -1;
    if (cuvs_crc32((const char *) &p + off, body_sz) != p.body_crc32)
        return -1;

    if (out)
        *out = p;
    return 0;
}

/* ----------------------------------------------------------------
 * Versioned .shards manifest I/O (Phase 3F multi-GPU sharding).
 * ---------------------------------------------------------------- */
int
cuvs_shards_write(FILE *f, uint32_t shard_count, int64_t n_vecs,
                  uint32_t dim, uint32_t metric, uint32_t base_tids_crc32,
                  const CuvsShardRecord *recs)
{
    CuvsShardsHeader hdr;
    size_t body_bytes = (size_t)shard_count * sizeof(CuvsShardRecord);

    hdr.magic           = CUVS_SHARDS_MAGIC;
    hdr.version         = CUVS_SHARDS_VERSION;
    hdr.shard_count     = shard_count;
    hdr.base_tids_crc32 = base_tids_crc32;
    hdr.n_vecs          = n_vecs;
    hdr.dim             = dim;
    hdr.metric          = metric;
    hdr.body_crc32      = cuvs_crc32(recs, body_bytes);
    hdr.reserved        = 0;

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1)
        return -1;
    if (fwrite(recs, sizeof(CuvsShardRecord), shard_count, f) != shard_count)
        return -1;
    return 0;
}

int
cuvs_shards_read(FILE *f, CuvsShardsHeader *hdr_out, CuvsShardRecord **recs_out)
{
    CuvsShardsHeader hdr;
    CuvsShardRecord *recs;
    size_t body_bytes;
    int64_t sum_n;

    *recs_out = NULL;

    if (fread(&hdr, sizeof(hdr), 1, f) != 1)
        return -1;
    if (hdr.magic != CUVS_SHARDS_MAGIC)
        return -1;
    if (hdr.version != CUVS_SHARDS_VERSION)
        return -1;
    if (hdr.reserved != 0)
        return -1;
    if (hdr.shard_count == 0 || hdr.shard_count > CUVS_SHARDS_MAX)
        return -1;
    if (hdr.n_vecs <= 0 || hdr.n_vecs > CUVS_TIDS_MAX_VECS)
        return -1;
    if (hdr.dim == 0)
        return -1;

    body_bytes = (size_t)hdr.shard_count * sizeof(CuvsShardRecord);
    recs = malloc(body_bytes);
    if (!recs)
        return -1;

    if (fread(recs, sizeof(CuvsShardRecord), hdr.shard_count, f) != hdr.shard_count)
    {
        free(recs);
        return -1;
    }
    if (cuvs_crc32(recs, body_bytes) != hdr.body_crc32)
    {
        free(recs);
        return -1;
    }

    /* Semantic invariants: a structurally valid manifest must describe a
     * complete, gap-free covering of [0, n_vecs). Reject otherwise (fail
     * closed) so a corrupt/incoherent manifest never yields a partial index. */
    sum_n = 0;
    for (uint32_t i = 0; i < hdr.shard_count; i++)
    {
        if (recs[i].reserved != 0)
            goto reject;
        if (recs[i].shard_id != i)              /* records ordered by shard id */
            goto reject;
        if (recs[i].tid_offset != sum_n)        /* contiguous from 0 */
            goto reject;
        if (recs[i].n_vecs <= 0 || recs[i].n_vecs > CUVS_TIDS_MAX_VECS)
            goto reject;
        if (recs[i].dim != hdr.dim || recs[i].metric != hdr.metric)
            goto reject;
        sum_n += recs[i].n_vecs;
    }
    if (sum_n != hdr.n_vecs)                     /* full coverage */
        goto reject;

    if (hdr_out)
        *hdr_out = hdr;
    *recs_out = recs;
    return 0;

reject:
    free(recs);
    return -1;
}

/* ----------------------------------------------------------------
 * Auto VRAM-based shard count (Phase 3G). Pure arithmetic, no CUDA — the
 * daemon supplies the runtime inputs (per-GPU VRAM budget, usable GPU count)
 * and this decides how many shards a logical index needs.
 *
 * Returns:
 *   1            -> unsharded (fits one GPU, or budget unknown/unlimited, or
 *                   corpus too small to shard). Preserves legacy behavior.
 *   2..max       -> split into this many contiguous shards.
 *   0            -> does not fit even when maximally sharded -> caller fails
 *                   closed (build error), never a partial/over-budget index.
 *
 * `needed` mirrors estimate_vram_bytes() in pg_cuvs_server.c exactly:
 * n_vecs * (dim*sizeof(float) + 16*4). Keep the two in sync.
 * ---------------------------------------------------------------- */
int
cuvs_auto_shard_count(int64_t n_vecs, int dim, size_t per_gpu_budget_bytes,
                      int n_gpus, int max_shards)
{
    size_t needed;
    int    cap;
    int    want;

    if (n_vecs <= 0 || dim <= 0 || n_gpus <= 0)
        return 1;
    if (per_gpu_budget_bytes == 0)
        return 1;                       /* unlimited/unknown budget: don't auto-shard */

    needed = (size_t) n_vecs * ((size_t) dim * sizeof(float) + 16 * 4);
    if (needed <= per_gpu_budget_bytes)
        return 1;                       /* fits a single GPU */

    /* Cap shard count by usable GPUs, the sanity max, and the >=2-vectors-per-
     * shard floor (CAGRA aborts on a 1-vector shard). */
    cap = n_gpus;
    if (max_shards > 0 && cap > max_shards)
        cap = max_shards;
    if ((int64_t) cap > n_vecs / 2)
        cap = (int) (n_vecs / 2);
    if (cap < 2)
        return 0;                       /* can't split enough to fit */

    /* ceil(needed / per_gpu_budget_bytes) shards of ~equal size. */
    want = (int) ((needed + per_gpu_budget_bytes - 1) / per_gpu_budget_bytes);
    if (want < 2)
        want = 2;
    if (want > cap)
        return 0;                       /* even cap shards each exceed budget */
    return want;
}

/* ----------------------------------------------------------------
 * Versioned .delta pending-insert sidecar I/O (Phase 3A).
 * ---------------------------------------------------------------- */
void
cuvs_delta_header_init(CuvsDeltaHeader *h, uint32_t dim, uint32_t metric,
                       uint32_t base_tids_crc32)
{
    h->magic           = CUVS_DELTA_MAGIC;
    h->version         = CUVS_DELTA_VERSION;
    h->n_rows          = 0;
    h->dim             = dim;
    h->metric          = metric;
    h->base_tids_crc32 = base_tids_crc32;
    h->reserved        = 0;
}

int
cuvs_delta_validate(const CuvsDeltaHeader *h, int64_t body_bytes)
{
    if (h->magic != CUVS_DELTA_MAGIC)
        return -1;
    if (h->version != CUVS_DELTA_VERSION)
        return -1;
    if (h->reserved != 0)
        return -1;
    if (h->dim == 0)
        return -1;
    if (h->n_rows < 0 || h->n_rows > CUVS_TIDS_MAX_VECS)
        return -1;
    if (body_bytes < 0)
        return -1;
    /* Body must be exactly n_rows fixed-width records — catches truncation. */
    if ((uint64_t) body_bytes
        != (uint64_t) h->n_rows * cuvs_delta_record_bytes(h->dim))
        return -1;
    return 0;
}

int
cuvs_delta_read_header(FILE *f, CuvsDeltaHeader *out)
{
    CuvsDeltaHeader h;

    if (fread(&h, sizeof(h), 1, f) != 1)
        return -1;
    if (h.magic != CUVS_DELTA_MAGIC)
        return -1;
    if (h.version != CUVS_DELTA_VERSION)
        return -1;
    if (h.reserved != 0)
        return -1;
    if (out)
        *out = h;
    return 0;
}

/* ----------------------------------------------------------------
 * Tombstone sidecar helpers (Phase 3A-4)
 * ---------------------------------------------------------------- */
void
cuvs_tombstone_header_init(CuvsTombstoneHeader *h, uint32_t base_tids_crc32)
{
    h->magic           = CUVS_TOMBSTONE_MAGIC;
    h->version         = CUVS_TOMBSTONE_VERSION;
    h->n_entries       = 0;
    h->base_tids_crc32 = base_tids_crc32;
    h->reserved        = 0;
    h->_pad0           = 0;
}

int
cuvs_tombstone_validate(const CuvsTombstoneHeader *h, int64_t body_bytes)
{
    if (h->magic != CUVS_TOMBSTONE_MAGIC)   return -1;
    if (h->version != CUVS_TOMBSTONE_VERSION) return -1;
    if (h->reserved != 0)                    return -1;
    if (h->n_entries < 0 || h->n_entries > CUVS_TIDS_MAX_VECS) return -1;
    if (body_bytes < 0)                      return -1;
    if ((uint64_t)body_bytes != (uint64_t)h->n_entries * sizeof(CuvsTombstoneRecord))
        return -1;
    return 0;
}

int
cuvs_tombstone_read_header(FILE *f, CuvsTombstoneHeader *out)
{
    CuvsTombstoneHeader h;
    if (fread(&h, sizeof(h), 1, f) != 1)
        return -1;
    if (h.magic != CUVS_TOMBSTONE_MAGIC)
        return -1;
    if (h.version != CUVS_TOMBSTONE_VERSION)
        return -1;
    if (h.reserved != 0)
        return -1;
    if (out)
        *out = h;
    return 0;
}

#ifdef CUVS_TEST_HOOKS
int
cuvs_fault(const char *name)
{
    return getenv(name) != NULL;
}
#endif

/* ----------------------------------------------------------------
 * Circuit breaker state (process-local)
 * ---------------------------------------------------------------- */
CuvsCircuitBreaker cuvs_circuit_breakers[CUVS_MAX_TRACKED_INDEXES];
int                cuvs_n_circuit_breakers = 0;

static CuvsCircuitBreaker *
find_or_create_breaker(uint32_t index_oid)
{
    for (int i = 0; i < cuvs_n_circuit_breakers; i++)
        if (cuvs_circuit_breakers[i].index_oid == index_oid)
            return &cuvs_circuit_breakers[i];

    if (cuvs_n_circuit_breakers < CUVS_MAX_TRACKED_INDEXES)
    {
        CuvsCircuitBreaker *b = &cuvs_circuit_breakers[cuvs_n_circuit_breakers++];
        b->index_oid        = index_oid;
        b->consecutive_errors = 0;
        b->open             = 0;
        return b;
    }
    return NULL;
}

void
cuvs_circuit_record_error(uint32_t index_oid, int threshold)
{
    CuvsCircuitBreaker *b = find_or_create_breaker(index_oid);
    if (!b)
        return;
    b->consecutive_errors++;
    if (b->consecutive_errors >= threshold)
        b->open = 1;
}

void
cuvs_circuit_record_status(uint32_t index_oid, int threshold, int status)
{
    if (status == CUVS_STATUS_OK
        || status == CUVS_STATUS_CANCELED
        || status == CUVS_STATUS_DIM_MISMATCH
        || status == CUVS_STATUS_METRIC_MISMATCH
        || status == CUVS_STATUS_STALE
        || status == CUVS_STATUS_NO_VECTORS)
        return;

    cuvs_circuit_record_error(index_oid, threshold);
}

void
cuvs_circuit_record_success(uint32_t index_oid)
{
    for (int i = 0; i < cuvs_n_circuit_breakers; i++)
        if (cuvs_circuit_breakers[i].index_oid == index_oid)
        {
            cuvs_circuit_breakers[i].consecutive_errors = 0;
            return;
        }
}

void
cuvs_circuit_reset(uint32_t index_oid)
{
    for (int i = 0; i < cuvs_n_circuit_breakers; i++)
    {
        if (cuvs_circuit_breakers[i].index_oid == index_oid)
        {
            cuvs_circuit_breakers[i].consecutive_errors = 0;
            cuvs_circuit_breakers[i].open = 0;
            return;
        }
    }
}

int
cuvs_circuit_is_open(uint32_t index_oid)
{
    for (int i = 0; i < cuvs_n_circuit_breakers; i++)
        if (cuvs_circuit_breakers[i].index_oid == index_oid)
            return cuvs_circuit_breakers[i].open;
    return 0;
}

void
cuvs_circuit_reset_all(void)
{
    memset(cuvs_circuit_breakers, 0, sizeof(cuvs_circuit_breakers));
    cuvs_n_circuit_breakers = 0;
}

/* ----------------------------------------------------------------
 * Phase 3L-9 / #160: micro-batch grouping (pure).
 * ---------------------------------------------------------------- */
void
cuvs_batch_group(const CuvsBatchKey *keys, int n,
                    int *group_id_out, int *n_groups_out)
{
    int ng = 0;
    for (int i = 0; i < n; i++)
    {
        int g = -1;
        /* Reuse the group of the first earlier request with an identical key. */
        for (int j = 0; j < i; j++)
        {
            if (keys[j].db_oid    == keys[i].db_oid &&
                keys[j].index_oid == keys[i].index_oid &&
                keys[j].precision == keys[i].precision &&
                keys[j].dim       == keys[i].dim &&
                keys[j].mode      == keys[i].mode &&
                (keys[i].mode != 0 ||
                 (keys[j].itopk_size == keys[i].itopk_size &&
                  keys[j].search_width == keys[i].search_width)))
            {
                g = group_id_out[j];
                break;
            }
        }
        if (g < 0)
            g = ng++;               /* new key -> next dense, first-seen group id */
        group_id_out[i] = g;
    }
    if (n_groups_out)
        *n_groups_out = ng;
}

/* ----------------------------------------------------------------
 * Build shape class for [admit] (v5 C3 definition, carried by v6 C3).
 * ---------------------------------------------------------------- */
const char *
cuvs_build_shape_class(int64_t n, uint32_t dim, uint32_t metric, uint32_t gd,
                       uint32_t igd, uint32_t build_algo)
{
    int ivf  = build_algo == CUVS_CAGRA_BUILD_IVF_PQ;
    int autoa = build_algo == CUVS_CAGRA_BUILD_AUTO;

    if (gd == 0)
        gd = 64;
    if (igd == 0)
        igd = 128;
    if (dim != 768 || metric != CUVS_METRIC_L2)
        return "extrapolated";
    if ((n == 999000 &&
         ((ivf && ((gd == 32 && igd == 128) || (gd == 64 && igd == 256) ||
                   (gd == 32 && igd == 64))) ||
          (autoa && gd == 64 && igd == 128))) ||
        ((n == 250000 || n == 500000) && ivf && gd == 32 && igd == 128))
        return "checked";
    if ((ivf || autoa) && gd >= 32 && gd <= 64 && igd >= 64 && igd <= 256 && igd >= gd &&
        n >= 250000 && n <= 999000)
        return "bracketed";
    return "extrapolated";
}

/* ----------------------------------------------------------------
 * I3 admission rejection payload codec (cuvs_util.h).
 * ---------------------------------------------------------------- */
static const char *const cuvs_admission_reason_names[] = {
    "AR0", "AR1", "AR2", "AR3", "AR4", "AR5", "AR6", "AR7", "OOM", "ATT"
};
#define CUVS_ADMISSION_N_REASONS \
    ((int) (sizeof(cuvs_admission_reason_names) / sizeof(cuvs_admission_reason_names[0])))

uint32_t
cuvs_admission_mib(uint64_t bytes)
{
    uint64_t mib = bytes >> 20;
    return mib > CUVS_ADMISSION_SCALAR_MAX ? CUVS_ADMISSION_SCALAR_MAX : (uint32_t) mib;
}

uint32_t
cuvs_admission_count(uint64_t count)
{
    return count > CUVS_ADMISSION_SCALAR_MAX ? CUVS_ADMISSION_SCALAR_MAX : (uint32_t) count;
}

const char *
cuvs_admission_reason_name(CuvsAdmissionReason reason)
{
    if ((int) reason < 0 || (int) reason >= CUVS_ADMISSION_N_REASONS)
        return NULL;
    return cuvs_admission_reason_names[reason];
}

const char *
cuvs_admission_sqlstate(CuvsAdmissionReason reason)
{
    if (cuvs_admission_reason_name(reason) == NULL)
        return NULL;
    if (reason == CUVS_ADMISSION_AR0)
        return CUVS_ADMISSION_SQLSTATE_LIMIT;
    if (reason == CUVS_ADMISSION_AR6)
        return CUVS_ADMISSION_SQLSTATE_WORKSPACE;
    return CUVS_ADMISSION_SQLSTATE_MEMORY;
}

int
cuvs_admission_reason_transient(CuvsAdmissionReason reason)
{
    const char *state = cuvs_admission_sqlstate(reason);
    return state != NULL && strcmp(state, CUVS_ADMISSION_SQLSTATE_MEMORY) == 0;
}

static uint32_t
cuvs_admission_sat(uint32_t v)
{
    return v > CUVS_ADMISSION_SCALAR_MAX ? CUVS_ADMISSION_SCALAR_MAX : v;
}

/* Append to buf at *pos; -1 when it does not fit. */
static int
cuvs_admission_put(char *buf, size_t cap, size_t *pos, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static int
cuvs_admission_put(char *buf, size_t cap, size_t *pos, const char *fmt, ...)
{
    va_list ap;
    int     w;

    va_start(ap, fmt);
    w = vsnprintf(buf + *pos, cap - *pos, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t) w >= cap - *pos)
        return -1;
    *pos += (size_t) w;
    return 0;
}

int
cuvs_admission_encode(const CuvsAdmissionFields *in, char *buf, size_t cap)
{
    const char *name;
    size_t      pos = 0;
    uint32_t    pm;
    int         ok = 0;

    if (buf == NULL || cap == 0)
        return -1;
    buf[0] = '\0';
    if (in == NULL || (name = cuvs_admission_reason_name(in->reason)) == NULL)
        return -1;
    pm = in->present;
    if (in->n_cand > CUVS_ADMISSION_MAX_CANDIDATES)
        return -1;
    if ((pm & CUVS_ADM_F_W) && in->w != CUVS_ADMISSION_WAIT_QUERIES &&
        in->w != CUVS_ADMISSION_WAIT_UNPIN)
        return -1;
    if ((pm & CUVS_ADM_F_R) && in->r > 1)
        return -1;
    for (uint32_t c = 0; c < in->n_cand; c++)
        if (in->cand[c].idle_s < CUVS_ADMISSION_IDLE_NEVER)
            return -1;

#define PUT1(bit, key, v) \
    if (ok == 0 && (pm & (bit))) \
        ok = cuvs_admission_put(buf, cap, &pos, " " key "=%u", cuvs_admission_sat(v))
#define PUT2(bit, key, a, b) \
    if (ok == 0 && (pm & (bit))) \
        ok = cuvs_admission_put(buf, cap, &pos, " " key "=%u/%u", \
                                cuvs_admission_sat(a), cuvs_admission_sat(b))
    ok = cuvs_admission_put(buf, cap, &pos, "%s", name);
    PUT1(CUVS_ADM_F_N, "n", in->n);
    PUT1(CUVS_ADM_F_F, "f", in->f);
    PUT1(CUVS_ADM_F_Q, "q", in->q);
    PUT1(CUVS_ADM_F_P, "p", in->p);
    PUT2(CUVS_ADM_F_H, "h", in->h_mib, in->h_cnt);
    PUT2(CUVS_ADM_F_E, "e", in->e_mib, in->e_cnt);
    PUT2(CUVS_ADM_F_K, "k", in->k_mib, in->k_cnt);
    PUT1(CUVS_ADM_F_L, "l", in->l);
    PUT1(CUVS_ADM_F_Y, "y", in->y);
    PUT1(CUVS_ADM_F_U, "u", in->u);
    PUT1(CUVS_ADM_F_G, "g", in->g);
    PUT1(CUVS_ADM_F_S, "s", in->s);
    PUT1(CUVS_ADM_F_R, "r", in->r);
#undef PUT1
#undef PUT2
    for (uint32_t c = 0; ok == 0 && c < in->n_cand; c++)
    {
        const CuvsAdmissionCandidate *t = &in->cand[c];
        int32_t idle = t->idle_s > (int32_t) CUVS_ADMISSION_SCALAR_MAX
            ? (int32_t) CUVS_ADMISSION_SCALAR_MAX : t->idle_s;
        ok = cuvs_admission_put(buf, cap, &pos, " t=%u:%u:%u:%d", t->db_oid,
                                t->index_oid, cuvs_admission_sat(t->mib), idle);
    }
    if (ok == 0 && (pm & CUVS_ADM_F_W))
        ok = cuvs_admission_put(buf, cap, &pos, " w=%d", in->w);
    if (ok == 0 && (pm & CUVS_ADM_F_I))
        ok = cuvs_admission_put(buf, cap, &pos, " i=%u",
                                in->i > CUVS_ADMISSION_SHARD_MAX ? CUVS_ADMISSION_SHARD_MAX : in->i);
    if (ok != 0)
    {
        buf[0] = '\0';
        return -1;
    }
    return (int) pos;
}

/* Parse decimal digits in [p, end) as an unsigned value <= max. */
static int
cuvs_admission_uint(const char *p, const char *end, uint64_t max, uint64_t *out)
{
    uint64_t v = 0;

    if (p >= end)
        return -1;
    for (; p < end; p++)
    {
        if (*p < '0' || *p > '9')
            return -1;
        v = v * 10u + (uint64_t) (*p - '0');
        if (v > max)
            return -1;
    }
    *out = v;
    return 0;
}

static int
cuvs_admission_int(const char *p, const char *end, int64_t min, uint64_t max,
                   int64_t *out)
{
    uint64_t v;

    if (p < end && *p == '-')
    {
        if (cuvs_admission_uint(p + 1, end, (uint64_t) (-min), &v) != 0)
            return -1;
        *out = -(int64_t) v;
        return 0;
    }
    if (cuvs_admission_uint(p, end, max, &v) != 0)
        return -1;
    *out = (int64_t) v;
    return 0;
}

/* "a/b" or "a:b:c:d" style: split [p, end) at sep into exactly n parts. */
static int
cuvs_admission_split(const char *p, const char *end, char sep, int n,
                     const char **starts, const char **ends)
{
    int k = 0;

    starts[0] = p;
    for (; p < end; p++)
        if (*p == sep)
        {
            if (k + 1 >= n)
                return -1;
            ends[k++] = p;
            starts[k] = p + 1;
        }
    ends[k] = end;
    return k + 1 == n ? 0 : -1;
}

int
cuvs_admission_decode(const char *buf, size_t len, CuvsAdmissionFields *out)
{
    const char *nul, *p, *tok_end;
    size_t      lim;
    int         reason = -1;

    if (out == NULL)
        return -1;
    memset(out, 0, sizeof(*out));
    out->reason = CUVS_ADMISSION_NONE;
    if (buf == NULL)
        return -1;
    lim = len < CUVS_ADMISSION_ERR_OUT_BYTES ? len : CUVS_ADMISSION_ERR_OUT_BYTES;
    nul = memchr(buf, '\0', lim);
    if (nul == NULL)
        return -1;

    tok_end = memchr(buf, ' ', (size_t) (nul - buf));
    if (tok_end == NULL)
        tok_end = nul;
    for (int r = 0; r < CUVS_ADMISSION_N_REASONS; r++)
        if ((size_t) (tok_end - buf) == strlen(cuvs_admission_reason_names[r]) &&
            memcmp(buf, cuvs_admission_reason_names[r], (size_t) (tok_end - buf)) == 0)
            reason = r;
    if (reason < 0)
        goto bad;
    out->reason = (CuvsAdmissionReason) reason;

    for (p = tok_end; p < nul; p = tok_end)
    {
        const char *key, *val, *s[4], *e[4];
        uint64_t    a, b;
        int64_t     si;
        uint32_t    bit = 0;

        if (*p != ' ' || p + 1 >= nul)
            goto bad;
        key = p + 1;
        tok_end = memchr(key, ' ', (size_t) (nul - key));
        if (tok_end == NULL)
            tok_end = nul;
        if (tok_end - key < 3 || key[1] != '=')
            goto bad;
        val = key + 2;
        switch (key[0])
        {
            case 'n': bit = CUVS_ADM_F_N; break;
            case 'f': bit = CUVS_ADM_F_F; break;
            case 'q': bit = CUVS_ADM_F_Q; break;
            case 'p': bit = CUVS_ADM_F_P; break;
            case 'h': bit = CUVS_ADM_F_H; break;
            case 'e': bit = CUVS_ADM_F_E; break;
            case 'k': bit = CUVS_ADM_F_K; break;
            case 'l': bit = CUVS_ADM_F_L; break;
            case 'y': bit = CUVS_ADM_F_Y; break;
            case 'u': bit = CUVS_ADM_F_U; break;
            case 'g': bit = CUVS_ADM_F_G; break;
            case 's': bit = CUVS_ADM_F_S; break;
            case 'r': bit = CUVS_ADM_F_R; break;
            case 'w': bit = CUVS_ADM_F_W; break;
            case 'i': bit = CUVS_ADM_F_I; break;
            case 't': bit = 0; break;
            default: goto bad;
        }
        if (bit != 0 && (out->present & bit))
            goto bad;
        out->present |= bit;
        switch (key[0])
        {
            case 'h': case 'e': case 'k':
            {
                uint32_t *pair = key[0] == 'h' ? &out->h_mib
                               : key[0] == 'e' ? &out->e_mib : &out->k_mib;
                if (cuvs_admission_split(val, tok_end, '/', 2, s, e) != 0 ||
                    cuvs_admission_uint(s[0], e[0], CUVS_ADMISSION_SCALAR_MAX, &a) != 0 ||
                    cuvs_admission_uint(s[1], e[1], CUVS_ADMISSION_SCALAR_MAX, &b) != 0)
                    goto bad;
                pair[0] = (uint32_t) a;
                pair[1] = (uint32_t) b;
                break;
            }
            case 't':
            {
                CuvsAdmissionCandidate *t;
                uint64_t oid_db, oid_ix;
                if (out->n_cand >= CUVS_ADMISSION_MAX_CANDIDATES ||
                    cuvs_admission_split(val, tok_end, ':', 4, s, e) != 0 ||
                    cuvs_admission_uint(s[0], e[0], UINT32_MAX, &oid_db) != 0 ||
                    cuvs_admission_uint(s[1], e[1], UINT32_MAX, &oid_ix) != 0 ||
                    cuvs_admission_uint(s[2], e[2], CUVS_ADMISSION_SCALAR_MAX, &a) != 0 ||
                    cuvs_admission_int(s[3], e[3], CUVS_ADMISSION_IDLE_NEVER,
                                       CUVS_ADMISSION_SCALAR_MAX, &si) != 0)
                    goto bad;
                t = &out->cand[out->n_cand++];
                t->db_oid = (uint32_t) oid_db;
                t->index_oid = (uint32_t) oid_ix;
                t->mib = (uint32_t) a;
                t->idle_s = (int32_t) si;
                break;
            }
            case 'w':
                if (cuvs_admission_int(val, tok_end, CUVS_ADMISSION_WAIT_UNPIN, 0, &si) != 0)
                    goto bad;
                out->w = (int32_t) si;
                break;
            case 'r':
                if (cuvs_admission_uint(val, tok_end, 1, &a) != 0)
                    goto bad;
                out->r = (uint32_t) a;
                break;
            case 'i':
                if (cuvs_admission_uint(val, tok_end, CUVS_ADMISSION_SHARD_MAX, &a) != 0)
                    goto bad;
                out->i = (uint32_t) a;
                break;
            default:
            {
                uint32_t *slot = key[0] == 'n' ? &out->n : key[0] == 'f' ? &out->f
                               : key[0] == 'q' ? &out->q : key[0] == 'p' ? &out->p
                               : key[0] == 'l' ? &out->l : key[0] == 'y' ? &out->y
                               : key[0] == 'u' ? &out->u : key[0] == 'g' ? &out->g
                               : &out->s;
                if (cuvs_admission_uint(val, tok_end, CUVS_ADMISSION_SCALAR_MAX, &a) != 0)
                    goto bad;
                *slot = (uint32_t) a;
                break;
            }
        }
    }
    return 0;

bad:
    memset(out, 0, sizeof(*out));
    out->reason = CUVS_ADMISSION_NONE;
    return -1;
}

/* ----------------------------------------------------------------
 * User-visible rejection text (cuvs_util.h, v6 r18 C5).
 * ---------------------------------------------------------------- */
typedef struct CuvsStrBuf { char *p; size_t cap, len; } CuvsStrBuf;

static void cuvs_sb_add(CuvsStrBuf *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void
cuvs_sb_add(CuvsStrBuf *b, const char *fmt, ...)
{
    va_list ap;
    int     w;

    if (b->cap == 0 || b->len + 1 >= b->cap)
        return;
    va_start(ap, fmt);
    w = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (w < 0)
        return;
    b->len += (size_t) w;
    if (b->len >= b->cap)
        b->len = b->cap - 1;           /* truncated, still NUL-terminated */
}

/* "{v}" with the saturation marker. */
static void
cuvs_sb_mib(CuvsStrBuf *b, uint32_t v)
{
    if (v >= CUVS_ADMISSION_SCALAR_MAX)
        cuvs_sb_add(b, "%u+", CUVS_ADMISSION_SCALAR_MAX);
    else
        cuvs_sb_add(b, "%u", v);
}

/* Append s as the body of a SQL string literal (single quotes doubled). */
static void
cuvs_sb_literal(CuvsStrBuf *b, const char *s)
{
    for (; *s; s++)
        cuvs_sb_add(b, *s == '\'' ? "''" : "%c", *s);
}

static void
cuvs_admission_cand_name(const CuvsAdmissionCatalog *cat,
                         const CuvsAdmissionCandidate *t, char *buf, size_t cap,
                         int *resolved)
{
    *resolved = 0;
    if (t->db_oid != cat->my_db_oid)
    {
        snprintf(buf, cap, "(db %u) oid %u", t->db_oid, t->index_oid);
        return;
    }
    if (cat->index_name != NULL && cat->index_name(cat->ctx, t->index_oid, buf, cap) == 0)
    {
        *resolved = 1;
        return;
    }
    snprintf(buf, cap, "oid %u", t->index_oid);
}

/* "need ... (pinned P in M)" core of the candidate-bearing DETAIL. */
static void
cuvs_admission_core(CuvsStrBuf *b, const CuvsAdmissionFields *f)
{
    cuvs_sb_add(b, "need "); cuvs_sb_mib(b, f->n);
    cuvs_sb_add(b, " MiB, free "); cuvs_sb_mib(b, f->f);
    cuvs_sb_add(b, " MiB");
    /* Absent keys are not a measured zero: omit their clause. */
    if (f->present & CUVS_ADM_F_H)
    {
        cuvs_sb_add(b, ", resident "); cuvs_sb_mib(b, f->h_mib);
        cuvs_sb_add(b, " MiB in "); cuvs_sb_mib(b, f->h_cnt);
        cuvs_sb_add(b, " indexes");
    }
    if (f->present & CUVS_ADM_F_K)
    {
        cuvs_sb_add(b, " (pinned "); cuvs_sb_mib(b, f->k_mib);
        cuvs_sb_add(b, " MiB in "); cuvs_sb_mib(b, f->k_cnt);
        cuvs_sb_add(b, ")");
    }
}

static void
cuvs_admission_cands(CuvsStrBuf *b, const CuvsAdmissionFields *f,
                     const CuvsAdmissionCatalog *cat)
{
    cuvs_sb_add(b, ". Idle candidates: ");
    if (f->n_cand == 0)
    {
        cuvs_sb_add(b, "none (every other resident index is pinned or in use)");
        return;
    }
    for (uint32_t c = 0; c < f->n_cand; c++)
    {
        char name[256];
        int  resolved;

        cuvs_admission_cand_name(cat, &f->cand[c], name, sizeof(name), &resolved);
        cuvs_sb_add(b, "%s%s ", c ? "; " : "", name);
        cuvs_sb_mib(b, f->cand[c].mib);
        if (f->cand[c].idle_s == CUVS_ADMISSION_IDLE_NEVER)
            cuvs_sb_add(b, " MiB idle never");
        else
            cuvs_sb_add(b, " MiB idle %d min", f->cand[c].idle_s / 60);
    }
}

#define CUVS_ADM_UNLOAD_NOTES \
    " (the unloaded index reloads on its next query)" \
    " (prefer an index whose pg_cuvs_index_status().reclaim_exempt is none;" \
    " unloading an index that has delta or tombstone sidecar files and" \
    " pg_cuvs_gpu_search_stats().extend_count > 0 switches its GPU path to" \
    " CPU replan until REINDEX)"
#define CUVS_ADM_UNPIN_CLAUSE \
    " (if the index itself is pinned, release it first with pg_cuvs_unpin(index))"

/* HINT 2 kinds (1)/(2) or the zero-candidate variants (2b). */
static void
cuvs_admission_unload_hint(CuvsStrBuf *b, const CuvsAdmissionFields *f,
                           const CuvsAdmissionCatalog *cat, int load,
                           int *recommends_unload)
{
    const char *retry = load ? "then retry the query." : "then retry CREATE INDEX.";

    *recommends_unload = 0;
    if (f->n_cand == 0)
    {
        if (f->k_cnt > 0)
        {
            cuvs_sb_add(b, "Pinned indexes hold ");
            cuvs_sb_mib(b, f->k_mib);
            cuvs_sb_add(b, " MiB; release one with pg_cuvs_unpin(index) (owner or "
                           "superuser), or retry after current queries finish");
        }
        else
            cuvs_sb_add(b, "All resident indexes are in use; retry after current "
                           "queries finish");
        return;
    }
    *recommends_unload = 1;
    for (uint32_t c = 0; c < f->n_cand; c++)
    {
        const CuvsAdmissionCandidate *t = &f->cand[c];
        char name[256];
        int  resolved;

        if (cat->can_unload == NULL || !cat->can_unload(cat->ctx, t->db_oid, t->index_oid))
            continue;
        cuvs_admission_cand_name(cat, t, name, sizeof(name), &resolved);
        if (resolved)
        {
            cuvs_sb_add(b, "SELECT pg_cuvs_unload('");
            cuvs_sb_literal(b, name);
            cuvs_sb_add(b, "'); %s", retry);
        }
        else if (t->db_oid != cat->my_db_oid && cat->superuser)
            cuvs_sb_add(b, "SELECT pg_cuvs_unload(%u, %u); %s",
                        t->db_oid, t->index_oid, retry);
        else
            continue;                  /* unresolvable here: try the next one */
        cuvs_sb_add(b, CUVS_ADM_UNLOAD_NOTES);
        return;
    }
    cuvs_sb_add(b, "Ask a superuser to unload an idle index (see pg_cuvs_index_status())."
                   CUVS_ADM_UNLOAD_NOTES);
}

static int
cuvs_admission_reason_fits(CuvsAdmissionReason r, CuvsAdmissionObject obj)
{
    switch (obj)
    {
        case CUVS_ADMISSION_OBJ_CAGRA:
            return r >= CUVS_ADMISSION_AR0 && r <= CUVS_ADMISSION_ATT;
        case CUVS_ADMISSION_OBJ_IVFPQ:
        case CUVS_ADMISSION_OBJ_LOAD:
            return r >= CUVS_ADMISSION_AR0 && r <= CUVS_ADMISSION_AR5;
        case CUVS_ADMISSION_OBJ_SHARD:
            /* C5 lists AR0 and AR3..AR5; AR1/AR2 still arise when another
             * build holds a reservation, and get the same text. */
            return r >= CUVS_ADMISSION_AR0 && r <= CUVS_ADMISSION_AR5;
    }
    return 0;
}

const char *
cuvs_admission_format(const CuvsAdmissionFields *f, CuvsAdmissionObject obj,
                      const char *load_index_name, const CuvsAdmissionCatalog *cat,
                      char *msg, size_t msg_cap, char *detail, size_t detail_cap,
                      char *hint, size_t hint_cap)
{
    CuvsStrBuf  m = {msg, msg_cap, 0}, d = {detail, detail_cap, 0}, h = {hint, hint_cap, 0};
    const char *what, *state;
    int         load = obj == CUVS_ADMISSION_OBJ_LOAD;
    int         unload = 0;

    if (msg_cap) msg[0] = '\0';
    if (detail_cap) detail[0] = '\0';
    if (hint_cap) hint[0] = '\0';
    if (f == NULL || cat == NULL || f->n_cand > CUVS_ADMISSION_MAX_CANDIDATES ||
        !cuvs_admission_reason_fits(f->reason, obj) ||
        (state = cuvs_admission_sqlstate(f->reason)) == NULL)
        return NULL;

    what = obj == CUVS_ADMISSION_OBJ_IVFPQ ? "this IVF-PQ build"
         : obj == CUVS_ADMISSION_OBJ_SHARD ? "this shard of the CAGRA build"
         : "this CAGRA build";

    /* errmsg */
    if (load)
        cuvs_sb_add(&m, "pg_cuvs: GPU memory is unavailable to load index \"%s\"",
                    load_index_name ? load_index_name : "?");
    else switch (f->reason)
    {
        case CUVS_ADMISSION_AR0:
            cuvs_sb_add(&m, "pg_cuvs: GPU memory is insufficient for %s (needs more than "
                            "the configured limit); CREATE INDEX aborted",
                        obj == CUVS_ADMISSION_OBJ_SHARD ? "this CAGRA build" : what);
            break;
        case CUVS_ADMISSION_AR1:
        case CUVS_ADMISSION_AR2:
            cuvs_sb_add(&m, "pg_cuvs: GPU memory is insufficient while another CREATE "
                            "INDEX holds GPU memory; CREATE INDEX aborted");
            break;
        case CUVS_ADMISSION_AR3:
        case CUVS_ADMISSION_AR4:
            cuvs_sb_add(&m, "pg_cuvs: GPU memory is insufficient for %s; CREATE INDEX "
                            "aborted", what);
            break;
        case CUVS_ADMISSION_AR5:
            cuvs_sb_add(&m, "pg_cuvs: %s would require evicting resident indexes, which "
                            "the build eviction scope does not allow; CREATE INDEX aborted",
                        what);
            break;
        case CUVS_ADMISSION_AR6:
            cuvs_sb_add(&m, "pg_cuvs: large_workspace would not fit in GPU memory and "
                            "managed overcommit is disabled; CREATE INDEX aborted");
            break;
        case CUVS_ADMISSION_AR7:
            cuvs_sb_add(&m, "pg_cuvs: large_workspace would spill beyond the host memory "
                            "guard; CREATE INDEX aborted");
            break;
        case CUVS_ADMISSION_OOM:
            cuvs_sb_add(&m, "pg_cuvs: GPU memory exhausted during CAGRA build; CREATE "
                            "INDEX aborted. No resident index was evicted");
            break;
        case CUVS_ADMISSION_ATT:
            cuvs_sb_add(&m, "pg_cuvs: could not attach the dataset to the CAGRA index in "
                            "GPU memory; CREATE INDEX aborted. No resident index was "
                            "evicted");
            break;
        default:
            return NULL;
    }

    /* DETAIL */
    if (obj == CUVS_ADMISSION_OBJ_SHARD && (f->present & CUVS_ADM_F_I))
        cuvs_sb_add(&d, "shard %u: ", f->i);
    switch (f->reason)
    {
        case CUVS_ADMISSION_AR0:
            cuvs_sb_add(&d, "needs "); cuvs_sb_mib(&d, f->n);
            cuvs_sb_add(&d, " MiB; the limit for one build is "); cuvs_sb_mib(&d, f->q);
            cuvs_sb_add(&d, " MiB (pool quota, or the logical VRAM budget when the pool "
                            "is disabled)");
            break;
        case CUVS_ADMISSION_AR1:
        case CUVS_ADMISSION_AR2:
            cuvs_sb_add(&d, "needs "); cuvs_sb_mib(&d, f->n);
            cuvs_sb_add(&d, " MiB; "); cuvs_sb_mib(&d, f->f);
            cuvs_sb_add(&d, " MiB free (quota "); cuvs_sb_mib(&d, f->q);
            cuvs_sb_add(&d, " MiB); other CREATE INDEX builds hold "); cuvs_sb_mib(&d, f->p);
            cuvs_sb_add(&d, " MiB");
            break;
        case CUVS_ADMISSION_AR3:
        case CUVS_ADMISSION_AR4:
            cuvs_admission_core(&d, f);
            cuvs_sb_add(&d, "; "); cuvs_sb_mib(&d, f->e_mib);
            cuvs_sb_add(&d, " MiB in "); cuvs_sb_mib(&d, f->e_cnt);
            cuvs_sb_add(&d, load ? " indexes would be evictable even with protection time "
                                   "ignored (soft eviction), which is not enough"
                                 : " indexes would be evictable under scope lru");
            cuvs_admission_cands(&d, f, cat);
            break;
        case CUVS_ADMISSION_AR5:
            cuvs_admission_core(&d, f);
            if (load)
            {
                cuvs_sb_add(&d, "; nothing was evicted because evicting every unpinned "
                                "idle index would still not make room; ");
                cuvs_sb_mib(&d, f->e_mib);
                cuvs_sb_add(&d, " MiB in "); cuvs_sb_mib(&d, f->e_cnt);
                cuvs_sb_add(&d, " indexes are pinned or in use and would make room once "
                                "released");
                if (f->present & CUVS_ADM_F_W)
                    cuvs_sb_add(&d, f->w == CUVS_ADMISSION_WAIT_UNPIN
                                ? " (a pinned index is released only by pg_cuvs_unpin)"
                                : " (running searches release them when they finish)");
            }
            else
            {
                cuvs_sb_add(&d, "; nothing was evicted because the build eviction scope "
                                "does not allow it; ");
                cuvs_sb_mib(&d, f->e_mib);
                cuvs_sb_add(&d, " MiB in "); cuvs_sb_mib(&d, f->e_cnt);
                cuvs_sb_add(&d, " indexes would be evictable under scope lru");
                if ((f->present & CUVS_ADM_F_S) && f->s > 0)
                {
                    cuvs_sb_add(&d, "; this index is already resident with ");
                    cuvs_sb_mib(&d, f->s);
                    cuvs_sb_add(&d, " MiB");
                }
            }
            cuvs_admission_cands(&d, f, cat);
            break;
        case CUVS_ADMISSION_AR6:
        case CUVS_ADMISSION_AR7:
            cuvs_sb_add(&d, "large_workspace estimate "); cuvs_sb_mib(&d, f->l);
            cuvs_sb_add(&d, " MiB plus "); cuvs_sb_mib(&d, f->y);
            if (f->reason == CUVS_ADMISSION_AR6)
            {
                cuvs_sb_add(&d, " MiB held by other managed builds exceeds physical free "
                                "memory "); cuvs_sb_mib(&d, f->u);
                cuvs_sb_add(&d, " MiB; the pool part of the build needs "); cuvs_sb_mib(&d, f->n);
                cuvs_sb_add(&d, " MiB");
            }
            else
            {
                cuvs_sb_add(&d, " MiB held by other managed builds would spill beyond the "
                                "host memory guard "); cuvs_sb_mib(&d, f->g);
                cuvs_sb_add(&d, " MiB");
            }
            break;
        case CUVS_ADMISSION_OOM:
            cuvs_sb_add(&d, "needs "); cuvs_sb_mib(&d, f->n);
            cuvs_sb_add(&d, " MiB; "); cuvs_sb_mib(&d, f->f);
            cuvs_sb_add(&d, " MiB was free at admission; "); cuvs_sb_mib(&d, f->h_mib);
            cuvs_sb_add(&d, " MiB held by "); cuvs_sb_mib(&d, f->h_cnt);
            cuvs_sb_add(&d, " resident indexes; no resident index was evicted");
            break;
        case CUVS_ADMISSION_ATT:
            cuvs_sb_add(&d, "needs "); cuvs_sb_mib(&d, f->n);
            cuvs_sb_add(&d, " MiB");
            break;
        default:
            return NULL;
    }

    /* HINT */
    switch (f->reason)
    {
        case CUVS_ADMISSION_AR0:
            cuvs_sb_add(&h, "For a large table use pg_cuvs_hnsw with build_algo='auto' "
                            "(it spills to ACE on disk when over budget) or "
                            "build_algo='ace'.");
            if (obj != CUVS_ADMISSION_OBJ_IVFPQ)
                cuvs_sb_add(&h, " A lower graph_degree slightly lowers the admission "
                                "estimate.");
            cuvs_sb_add(&h, " Operators can review --rmm-pool-max-percent, --max-vram-mb "
                            "and --rmm-scratch-permille.");
            break;
        case CUVS_ADMISSION_AR1:
        case CUVS_ADMISSION_AR2:
            if (load)
            {
                cuvs_sb_add(&h, "A CREATE INDEX holds GPU memory (a CREATE INDEX may be "
                                "running); retry after it finishes. If the circuit breaker "
                                "opened, run SELECT pg_cuvs_reset_circuit('");
                cuvs_sb_literal(&h, load_index_name ? load_index_name : "");
                cuvs_sb_add(&h, "');");
            }
            else
                cuvs_sb_add(&h, "Retry after the other CREATE INDEX finishes.");
            break;
        case CUVS_ADMISSION_AR3:
        case CUVS_ADMISSION_AR4:
        case CUVS_ADMISSION_AR5:
        {
            int unpin = 0;
            if (load && f->reason == CUVS_ADMISSION_AR5 && (f->present & CUVS_ADM_F_W))
            {
                if (f->w == CUVS_ADMISSION_WAIT_QUERIES)
                    cuvs_sb_add(&h, "Retry once running searches finish, or free space: ");
                else
                    unpin = 1;
            }
            if (!load && f->reason == CUVS_ADMISSION_AR5 && (f->present & CUVS_ADM_F_S) &&
                f->s > 0 && f->k_cnt > 0)
                unpin = 1;
            cuvs_admission_unload_hint(&h, f, cat, load, &unload);
            if (unpin)
                cuvs_sb_add(&h, CUVS_ADM_UNPIN_CLAUSE);
            if (!load && f->reason == CUVS_ADMISSION_AR5)
                cuvs_sb_add(&h, " Operators can allow build eviction with "
                                "--build-evict-scope idle:<minutes> or lru.");
            if (obj == CUVS_ADMISSION_OBJ_SHARD)
                cuvs_sb_add(&h, " Or make room for one shard, or raise cuvs.shard_count "
                                "to make the shards smaller.");
            /* (3): only on the daemon's r=1 and never with zero candidates. */
            if (!load && unload && (f->present & CUVS_ADM_F_R) && f->r == 1)
                cuvs_sb_add(&h, "\nOr wait: background reclaim will free space for a build "
                                "of this size within the next reclaim tick.");
            break;
        }
        case CUVS_ADMISSION_AR6:
            if (f->y > 0)
                cuvs_sb_add(&h, "Retry after the other managed builds finish. ");
            cuvs_sb_add(&h, "If slower builds are acceptable, an operator can enable "
                            "--managed-overcommit on; or build smaller tables. "
                            "(--cagra-build-overflow error keeps large_workspace in the pool "
                            "and does not fix a physical memory shortage.)");
            break;
        case CUVS_ADMISSION_AR7:
            cuvs_sb_add(&h, "Free host memory, retry after the other managed builds finish, "
                            "or review --managed-lw-margin-permille.");
            break;
        case CUVS_ADMISSION_OOM:
            cuvs_sb_add(&h, "Raising --rmm-scratch-permille reserves more at admission and "
                            "avoids this (lowering it admits more builds that may run out of "
                            "memory). The pool part ran short; managed overflow covers only "
                            "large_workspace.");
            break;
        case CUVS_ADMISSION_ATT:
            cuvs_sb_add(&h, "Check the pool free memory and retry after other builds "
                            "finish. cuVS reports a failed dataset attach only as a "
                            "warning, so this build raised it as an error.");
            break;
        default:
            return NULL;
    }
    return state;
}

/* ----------------------------------------------------------------
 * I4 admission hook (cuvs_admission_contract.h). The I3-A tip has no managed
 * large_workspace policy, so AR6/AR7 are never emitted here (v6 1.4 (h)).
 * I4 owns this body and replaces it with the AR6/AR7 rule.
 * ---------------------------------------------------------------- */
CuvsAdmissionReason
cuvs_admission_workspace_check(const CuvsAdmissionWorkspaceInput *in)
{
    (void) in;
    return CUVS_ADMISSION_NONE;
}
