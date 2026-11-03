#pragma once

/*
 * cuvs_util.h — shared, dependency-free helpers for pg_cuvs.
 *
 * This header is included by the PostgreSQL extension (pg_cuvs.c), the
 * standalone GPU daemon (pg_cuvs_server.c), the IPC client (cuvs_ipc.c),
 * and a standalone unit-test binary. It MUST NOT pull in PostgreSQL,
 * CUDA, or cuVS headers — only the C standard headers below.
 */

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>

#include "cuvs_admission_contract.h"

/* Strict daemon CLI parser: MiB, 0=disabled, bounded for size_t and the
 * read-only integer GUC reporting the applied setting. Returns 0 on success. */
#ifdef __cplusplus
extern "C" {
#endif
int cuvs_parse_rmm_pool_mb(const char *value, uint32_t *mb_out);
int cuvs_parse_rmm_pool_percent(const char *value, uint32_t *percent_out);
/* --rmm-scratch-permille: decimal 0..CUVS_ADMISSION_RHO_PERMILLE_MAX (C3 rho).
 * Returns 0, or -1 leaving *permille_out untouched. */
int cuvs_parse_scratch_permille(const char *value, uint32_t *permille_out);

/* Pool quota is aligned to RMM's 256-byte allocation boundary. A zero quota
 * in the headroom helper means the legacy, unpooled path. */
size_t cuvs_rmm_pool_max_bytes(size_t total_bytes, uint32_t percent);
size_t cuvs_rmm_pool_headroom(size_t physical_free, size_t async_reclaimable,
                              size_t quota, size_t reserved, size_t used);
#ifdef __cplusplus
}
#endif

/* ----------------------------------------------------------------
 * Leveled logging macros (PG-free; stderr only).
 *
 * ERROR/WARN/INFO are UNCONDITIONAL — they always print. Only DEBUG is
 * gated behind PG_CUVS_DEBUG (hot-path trace only). Each macro prepends a
 * level tag to the caller's printf format string. Callers must pass a
 * string literal as the first argument (the format), e.g.
 *     LOG_ERROR("save_index FAILED for %u/%u\n", db, idx);
 *
 * The "[TAG] " fmt concatenation requires a string-literal format and the
 * ##__VA_ARGS__ GNU extension (available under both -std=gnu11 for the
 * daemon and gcc for the PGXS .so build).
 * ---------------------------------------------------------------- */
#ifndef PG_CUVS_DEBUG
#define PG_CUVS_DEBUG 0
#endif

#define LOG_ERROR(fmt, ...) \
    do { fprintf(stderr, "[ERROR] " fmt, ##__VA_ARGS__); } while (0)
#define LOG_WARN(fmt, ...) \
    do { fprintf(stderr, "[WARN] " fmt, ##__VA_ARGS__); } while (0)
#define LOG_INFO(fmt, ...) \
    do { fprintf(stderr, "[INFO] " fmt, ##__VA_ARGS__); } while (0)
#define LOG_DEBUG(fmt, ...) \
    do { if (PG_CUVS_DEBUG) fprintf(stderr, "[DEBUG] " fmt, ##__VA_ARGS__); } while (0)

/* ----------------------------------------------------------------
 * TID encode/decode (heap TID <-> uint64 block<<16|offset)
 * ---------------------------------------------------------------- */
static inline uint64_t
cuvs_tid_encode(uint32_t block, uint16_t offset)
{
    return ((uint64_t)block << 16) | (uint64_t)offset;
}

static inline void
cuvs_tid_decode(uint64_t tid, uint32_t *block, uint16_t *offset)
{
    *block  = (uint32_t)(tid >> 16);
    *offset = (uint16_t)(tid & 0xFFFF);
}

/* ----------------------------------------------------------------
 * Fail-closed filter-frame gate (ADR-063 tenant isolation): a search
 * request that names a TID whitelist (n_filter_tids > 0) but for which the
 * whitelist itself is unavailable (bad/empty shm key, failed mmap, ...)
 * must be refused rather than silently answered with unfiltered rows.
 * Pure decision, no I/O -- callers keep their own status code for the
 * refusal.
 * ---------------------------------------------------------------- */
bool cuvs_filter_frame_refuses(uint32_t n_filter_tids, bool filter_available);

/* ----------------------------------------------------------------
 * Index filename parsing: "<db_oid>_<index_oid>.cagra"
 * Returns 0 on success, -1 otherwise.
 * ---------------------------------------------------------------- */
int cuvs_parse_index_filename(const char *name, uint32_t *db_oid, uint32_t *index_oid);

/* ----------------------------------------------------------------
 * IPC status code -> human-readable string (see cuvs_ipc.h CUVS_STATUS_*)
 * ---------------------------------------------------------------- */
const char *cuvs_status_str(int status);

/* Opclass name -> CUVS_METRIC_* (cuvs_ipc.h); -1 if unrecognized. */
int cuvs_metric_from_opclass_name(const char *name);
void cuvs_u64_sort(uint64_t *values, size_t n);
int  cuvs_u64_contains(const uint64_t *sorted_values, size_t n, uint64_t target);

/* ----------------------------------------------------------------
 * Latency histogram (per-index, daemon-side). Log2-spaced fixed buckets:
 * bucket 0 = {0 us}; bucket k (k>=1) covers [2^(k-1), 2^k) us. 32 buckets
 * span up to ~2^31 us (~35 min), which comfortably bounds any GPU search.
 * Percentiles are approximate (bucket upper edge) — adequate for
 * monitoring, not for precise SLA accounting. Pure, PG/GPU-free.
 * ---------------------------------------------------------------- */
#define CUVS_LAT_BUCKETS 32

/* Bucket index for a latency in microseconds (clamped to [0, CUVS_LAT_BUCKETS-1]). */
uint32_t cuvs_lat_bucket_index(uint32_t us);

/* q-quantile (q in [0,1]) over a CUVS_LAT_BUCKETS-wide histogram, returned as
 * the containing bucket's upper-edge latency in us. Returns 0 for an empty
 * histogram. nbuckets lets the unit test pass smaller arrays. */
uint32_t cuvs_lat_percentile(const uint32_t *buckets, int nbuckets, double q);

/* ----------------------------------------------------------------
 * Versioned, checksummed .tids on-disk sidecar format.
 *
 * Layout: [CuvsTidsHeader (32 bytes)] [n_vecs * uint64_t TID body].
 * LITTLE-ENDIAN ONLY: the daemon is x86-64 and the Makefile sets
 * RAFT_SYSTEM_LITTLE_ENDIAN=1; no byte-swap is performed. Legacy
 * headerless .tids files are intentionally rejected by the magic check
 * (pre-1.0, no shipped users -> reREINDEX such indexes).
 * ---------------------------------------------------------------- */
#define CUVS_TIDS_MAGIC    0x53444954u   /* 'TIDS' little-endian */
#define CUVS_TIDS_VERSION  1u
#define CUVS_TIDS_MAX_VECS 1000000000LL  /* sanity cap on n_vecs */

typedef struct CuvsTidsHeader {
    uint32_t magic;
    uint32_t version;
    int64_t  n_vecs;
    uint32_t dim;
    uint32_t metric;
    uint32_t body_crc32;   /* crc32 over n_vecs*8 bytes of TID body */
    uint32_t reserved;     /* must be 0 */
} CuvsTidsHeader;          /* 32 bytes, LE-only, x86-64 daemon */

/* Standard table-based CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320). */
uint32_t cuvs_crc32(const void *data, size_t len);

/* Streaming CRC-32 for data that does not fit in one buffer (e.g. a large
 * .cagra artifact read in chunks). Usage:
 *     uint32_t s = cuvs_crc32_stream_begin();
 *     s = cuvs_crc32_stream_update(s, buf, n);   // repeat per chunk
 *     uint32_t crc = cuvs_crc32_stream_end(s);
 * cuvs_crc32(d,n) == end(update(begin(), d, n)). */
uint32_t cuvs_crc32_stream_begin(void);
uint32_t cuvs_crc32_stream_update(uint32_t state, const void *data, size_t len);
uint32_t cuvs_crc32_stream_end(uint32_t state);

/* Write header + TID body to an open FILE*. Returns 0 on success, -1 on a
 * short/failed fwrite. Caller is responsible for fflush/fsync/rename. */
int cuvs_tids_write(FILE *f, int64_t n_vecs, uint32_t dim, uint32_t metric,
                    const uint64_t *tids);

/* Read + validate a .tids file from an open FILE*. On success returns 0,
 * fills *hdr_out, and allocates *tids_out via malloc (caller frees).
 * Validates magic, version, n_vecs range (0 < n_vecs <= CUVS_TIDS_MAX_VECS),
 * full body read, and body crc32. On any failure returns -1 and frees only
 * what it allocated (leaving *tids_out NULL). */
int cuvs_tids_read(FILE *f, CuvsTidsHeader *hdr_out, uint64_t **tids_out);

/* ----------------------------------------------------------------
 * Versioned, checksummed .vectors sidecar (Phase 3L brute-force mode).
 *
 * Holds the raw row-major vector matrix so the daemon can build a resident
 * GPU brute-force index (CuvsBfIndex) for exact search, independent of the
 * CAGRA graph. Mirrors the .tids format: same header shape plus a crc32 over
 * the body, little-endian only. base_tids_crc32 ties this sidecar to its
 * build's .tids body_crc32 (generation token, like .delta/.shards): a REINDEX
 * rewrites the .tids and changes that crc, so a torn build (new .tids, stale
 * .vectors) is detected at load and brute_force fails closed until REINDEX.
 *
 * Layout: [CuvsVectorsHeader (36 bytes)] [n_vecs * dim * float32 body].
 * LITTLE-ENDIAN ONLY, x86-64 daemon.
 * ---------------------------------------------------------------- */
#define CUVS_VECTORS_MAGIC   0x53434556u   /* 'VECS' little-endian */
#define CUVS_VECTORS_VERSION 1u
#define CUVS_VECTORS_MAX_DIM 65535u        /* sanity cap on dim (corrupt header) */

typedef struct CuvsVectorsHeader {
    uint32_t magic;
    uint32_t version;
    int64_t  n_vecs;
    uint32_t dim;
    uint32_t metric;
    uint32_t body_crc32;      /* crc32 over n_vecs*dim*4 bytes of float body */
    uint32_t base_tids_crc32; /* sibling .tids body_crc32 @ build (generation token) */
    uint32_t reserved;        /* must be 0 */
} CuvsVectorsHeader;          /* 36 bytes, LE-only, x86-64 daemon */

/* Write header + float body to an open FILE*. base_tids_crc32 is the build's
 * .tids body_crc32 (generation token). Returns 0 on success, -1 on a
 * short/failed fwrite. Caller is responsible for fflush/fsync/rename. */
int cuvs_vectors_write(FILE *f, int64_t n_vecs, uint32_t dim, uint32_t metric,
                       uint32_t base_tids_crc32, const float *vecs);

/* ADR-059: write the .vectors sidecar from N partitions (part_vecs[i] is
 * [n_each[i]][dim], concatenated in order) without a contiguous host copy.
 * Body crc32 is streamed over the partitions. Output is byte-identical to
 * cuvs_vectors_write over the concatenated corpus. Returns 0 / -1. */
int cuvs_vectors_write_multi(FILE *f, const int64_t *n_each, int n_parts,
                             uint32_t dim, uint32_t metric,
                             uint32_t base_tids_crc32, const float *const *part_vecs);

/* Read + validate a .vectors file from an open FILE*. On success returns 0,
 * fills *hdr_out, and allocates *vecs_out via malloc (caller frees). Validates
 * magic, version, n_vecs range (0 < n_vecs <= CUVS_TIDS_MAX_VECS), dim range
 * (0 < dim <= CUVS_VECTORS_MAX_DIM), reserved==0, full body read, and body
 * crc32. On any failure returns -1 and leaves *vecs_out NULL. */
int cuvs_vectors_read(FILE *f, CuvsVectorsHeader *hdr_out, float **vecs_out);

/* ----------------------------------------------------------------
 * Versioned, checksummed hardware profile (ADR-075 cost-model v2, Phase 1).
 *
 * GLOBAL (one per deployment, not per-index) sidecar `<index_dir>/cuvs_hw_profile`
 * written once by the daemon at boot after GPU detection. Holds measured PHYSICAL
 * constants (bandwidths/latencies) that the cost model will consume per deployment
 * so coefficients are not baked to one machine. Self-contained: all data is in the
 * struct (no separate body), so body_crc32 covers all bytes AFTER it (probe_status
 * onward). LITTLE-ENDIAN ONLY, x86-64 daemon. The planner reads + validates this
 * cheaply at plan time (no CUDA/IPC); any mismatch -> caller uses compiled DEFAULTs.
 * ---------------------------------------------------------------- */
#define CUVS_HWPROFILE_MAGIC   0x46505748u   /* 'HWPF' little-endian */
/* v1: link_bw/hbm_bw/gpu_bf_tput/ipc_rtt (136 bytes).
 * v2: + cpu_dist_tput + gpu_cagra_lat_us (152 bytes) for the physical cost model
 *     (ADR-075 Phase 2). The reader accepts v1 (older daemon) and zero-fills the
 *     v2 tail with DEFAULTs + clear bits, so the planner falls back to legacy. */
#define CUVS_HWPROFILE_VERSION 2u

/* probe_status bits: set = field measured on this hardware; clear = compiled DEFAULT. */
#define CUVS_HWPROBE_LINK_BW   0x1u
#define CUVS_HWPROBE_HBM_BW    0x2u
#define CUVS_HWPROBE_BF_TPUT   0x4u
#define CUVS_HWPROBE_IPC_RTT   0x8u
#define CUVS_HWPROBE_CPU_DIST  0x10u  /* v2: CPU L2-distance throughput measured */
#define CUVS_HWPROBE_CAGRA_LAT 0x20u  /* v2: CAGRA per-query graph-search latency measured */

typedef struct CuvsHwProfile {
    uint32_t magic;
    uint32_t version;
    uint32_t body_crc32;       /* crc32 over all bytes AFTER this field (probe_status..end) */
    /* --- crc-covered body starts here --- */
    uint32_t probe_status;     /* CUVS_HWPROBE_* bits */
    char     gpu_name[64];     /* primary GPU name (CuvsGpuDeviceInfo.name); env/identity tag */
    uint32_t n_gpus;
    uint32_t reserved;         /* must be 0 */
    int64_t  total_vram_bytes; /* primary GPU total VRAM */
    int64_t  measured_at;      /* epoch seconds at probe time */
    double   link_bw_bpus;     /* CPU<->GPU H2D bandwidth, bytes per microsecond */
    double   hbm_bw_bpus;      /* GPU device memory bandwidth, bytes per microsecond */
    double   gpu_bf_tput;      /* brute-force throughput, (vectors*dim) per microsecond */
    double   ipc_rtt_us;       /* loopback IPC round-trip, microseconds */
    /* --- v2 tail (ADR-075 Phase 2); v1 readers/files treat these as DEFAULT --- */
    double   cpu_dist_tput;    /* CPU L2-distance throughput, (vectors*dim) per microsecond */
    double   gpu_cagra_lat_us; /* CAGRA per-query graph-search latency floor, microseconds */
} CuvsHwProfile;               /* fixed-size POD, naturally aligned, LE-only */

/* Byte sizes of each on-disk version (for the version-conditional read). */
#define CUVS_HWPROFILE_SZ_V1   (offsetof(CuvsHwProfile, cpu_dist_tput))
#define CUVS_HWPROFILE_SZ_V2   (sizeof(CuvsHwProfile))

/* Stamp magic/version + body_crc32 on *p (body fields already filled) and fwrite
 * it. Returns 0 / -1. Caller does fflush/fsync/rename. */
int cuvs_hw_profile_write(FILE *f, CuvsHwProfile *p);

/* Read + validate a cuvs_hw_profile from an open FILE*. Validates magic, version,
 * reserved==0, and body crc32. On success returns 0 and fills *out; on any
 * failure returns -1 (caller falls back to compiled DEFAULT coefficients). */
int cuvs_hw_profile_read(FILE *f, CuvsHwProfile *out);

/* Conservative DEFAULT coefficients used when no measured profile is present.
 * Shared by the daemon (pre-probe seed) and the planner (fallback) so 'default'
 * behavior is consistent. bytes-per-microsecond unless noted. */
#define CUVS_HWP_DEFAULT_LINK_BW   12000.0      /* ~12 GB/s (PCIe floor) */
#define CUVS_HWP_DEFAULT_HBM_BW    1000000.0    /* ~1 TB/s floor */
#define CUVS_HWP_DEFAULT_BF_TPUT   1000.0       /* (vectors*dim) per microsecond */
#define CUVS_HWP_DEFAULT_IPC_RTT   500.0        /* microseconds */
#define CUVS_HWP_DEFAULT_CPU_DIST  200.0        /* CPU (vectors*dim) per microsecond */
#define CUVS_HWP_DEFAULT_CAGRA_LAT 1000.0       /* CAGRA graph-search latency, microseconds */

/* ----------------------------------------------------------------
 * Versioned, checksummed .shards manifest (Phase 3F multi-GPU sharding).
 *
 * Marks a logical CAGRA index as split into N standalone CAGRA shard
 * artifacts (`<db>_<idx>.s%03u.cagra`), each a contiguous build-order range
 * of the global `.tids`. The manifest is the commit marker: a build renames
 * all shard `.cagra` + global `.tids` first, then renames `.shards` last.
 * Reload refuses a logical sharded index unless the manifest validates AND
 * its base_tids_crc32 matches the current `.tids` body_crc32 (generation).
 *
 * Layout: [CuvsShardsHeader (40 bytes)] [shard_count * CuvsShardRecord (40)].
 * LITTLE-ENDIAN ONLY, like .tids. The record body has its own crc32 in the
 * header. cuvs_shards_read also enforces two semantic invariants so a
 * structurally-valid-but-incoherent manifest fails closed: shard offsets are
 * contiguous from 0 and the per-shard n_vecs sum to the header n_vecs.
 * ---------------------------------------------------------------- */
#define CUVS_SHARDS_MAGIC   0x53524853u   /* 'SHRS' little-endian */
#define CUVS_SHARDS_VERSION 1u
#define CUVS_SHARDS_MAX     256           /* sanity cap on shard_count */

typedef struct CuvsShardsHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t shard_count;
    uint32_t base_tids_crc32;  /* global .tids body_crc32 — generation token */
    int64_t  n_vecs;           /* total vectors across all shards (== .tids n_vecs) */
    uint32_t dim;
    uint32_t metric;
    uint32_t body_crc32;       /* crc32 over shard_count * CuvsShardRecord */
    uint32_t reserved;         /* must be 0 */
} CuvsShardsHeader;            /* 40 bytes, LE-only */

typedef struct CuvsShardRecord {
    uint32_t shard_id;
    uint32_t gpu_device_id;    /* GPU assigned at build (advisory; reload re-places) */
    int64_t  tid_offset;       /* global TID start offset of this shard's [start,end) */
    int64_t  n_vecs;           /* vectors in this shard */
    uint32_t dim;
    uint32_t metric;
    uint32_t artifact_crc32;   /* crc32 of the shard's .cagra file bytes */
    uint32_t reserved;         /* must be 0 */
} CuvsShardRecord;             /* 40 bytes, LE-only */

/* Write manifest header + record body to an open FILE*. Sets magic/version/
 * body_crc32/reserved internally from (shard_count, n_vecs, dim, metric,
 * base_tids_crc32, recs). Returns 0 on success, -1 on a short/failed fwrite.
 * Caller fflush/fsync/renames. */
int cuvs_shards_write(FILE *f, uint32_t shard_count, int64_t n_vecs,
                      uint32_t dim, uint32_t metric, uint32_t base_tids_crc32,
                      const CuvsShardRecord *recs);

/* Read + validate a .shards manifest. On success returns 0, fills *hdr_out,
 * and allocates *recs_out via malloc (caller frees). Validates magic, version,
 * reserved==0, shard_count in [1,CUVS_SHARDS_MAX], n_vecs/dim ranges, full
 * body read, body crc32, AND the semantic invariants (contiguous offsets from
 * 0, per-shard n_vecs sum == header n_vecs, each record reserved==0). On any
 * failure returns -1 and leaves *recs_out NULL. */
int cuvs_shards_read(FILE *f, CuvsShardsHeader *hdr_out, CuvsShardRecord **recs_out);

/* Decide how many shards a logical CAGRA index needs (Phase 3G auto count).
 * Pure arithmetic over the daemon's runtime inputs; no CUDA. Returns 1 for
 * unsharded (fits one GPU / unknown budget / too small), 2..max_shards for a
 * split, or 0 when it cannot fit even maximally sharded (caller fails closed).
 * `needed` bytes mirror estimate_vram_bytes(): n_vecs*(dim*4 + 64). */
int cuvs_auto_shard_count(int64_t n_vecs, int dim, size_t per_gpu_budget_bytes,
                          int n_gpus, int max_shards);

/* ----------------------------------------------------------------
 * Versioned .delta pending-insert sidecar (Phase 3A).
 *
 * Layout: [CuvsDeltaHeader (32 bytes)] [n_rows * record], each record being
 * { uint64_t tid; float vec[dim]; } (fixed width = 8 + dim*4 bytes). Holds
 * vectors inserted/updated since the base CAGRA build so a query can merge GPU
 * base candidates with CPU-exact delta candidates without a rebuild.
 *
 * LITTLE-ENDIAN ONLY, like .tids. Corruption (e.g. a truncated file) is caught
 * by a file-size check: the body must be exactly n_rows*record_bytes. There is
 * deliberately no whole-body CRC — appends must stay O(1), and a CRC recomputed
 * over the growing body on every insert would be O(n^2). base_tids_crc32 ties a
 * delta to its base build's .tids body_crc32; a REINDEX rewrites the base and
 * changes that CRC, so a leftover delta is detected as a generation mismatch.
 * ---------------------------------------------------------------- */
#define CUVS_DELTA_MAGIC   0x544c4544u   /* 'DELT' little-endian */
#define CUVS_DELTA_VERSION 1u

typedef struct CuvsDeltaHeader {
    uint32_t magic;
    uint32_t version;
    int64_t  n_rows;
    uint32_t dim;
    uint32_t metric;
    uint32_t base_tids_crc32;  /* .tids body_crc32 at delta creation (generation) */
    uint32_t reserved;         /* must be 0 */
} CuvsDeltaHeader;             /* 32 bytes, LE-only, x86-64 daemon */

/* Bytes per delta record for a given dim: TID (uint64) + dim float32s. */
static inline size_t
cuvs_delta_record_bytes(uint32_t dim)
{
    return sizeof(uint64_t) + (size_t) dim * sizeof(float);
}

/* Initialize a fresh (empty) delta header. */
void cuvs_delta_header_init(CuvsDeltaHeader *h, uint32_t dim, uint32_t metric,
                            uint32_t base_tids_crc32);

/* Validate a delta header against the actual body byte count (file size minus
 * sizeof(CuvsDeltaHeader)). Checks magic/version/reserved/dim, n_rows range,
 * and that body_bytes == n_rows * record_bytes exactly. Returns 0 if valid,
 * -1 otherwise. Pure (no I/O). */
int cuvs_delta_validate(const CuvsDeltaHeader *h, int64_t body_bytes);

/* Read + validate just the delta header from an open FILE* (does not read the
 * body). Returns 0 and fills *out on magic/version/reserved success; -1 on a
 * short read or bad fields. The body-size check is the caller's job (it has
 * the file size) via cuvs_delta_validate. */
int cuvs_delta_read_header(FILE *f, CuvsDeltaHeader *out);

/* ----------------------------------------------------------------
 * Versioned .tombstone sidecar (Phase 3A-4).
 *
 * Layout: [CuvsTombstoneHeader (32 bytes)] [n_entries * CuvsTombstoneRecord].
 * Records dead TIDs from DELETE/UPDATE-old so base CAGRA results can be
 * filtered before merge. The backend does snapshot-aware filtering using
 * delete_xid (the daemon has no MVCC knowledge). Tied to a base build via
 * base_tids_crc32, like .delta.
 * ---------------------------------------------------------------- */
#define CUVS_TOMBSTONE_MAGIC   0x424D4F54u  /* 'TOMB' little-endian */
#define CUVS_TOMBSTONE_VERSION 1u

typedef struct CuvsTombstoneHeader {
    uint32_t magic;
    uint32_t version;
    int64_t  n_entries;
    uint32_t base_tids_crc32;
    uint32_t reserved;          /* must be 0 */
    uint64_t _pad0;             /* pad to 32 bytes */
} CuvsTombstoneHeader;          /* 32 bytes */

typedef struct CuvsTombstoneRecord {
    uint64_t tid;               /* heap TID of the dead tuple */
    uint64_t delete_xid;        /* xact ID that deleted/updated this tuple */
} CuvsTombstoneRecord;          /* 16 bytes */

void cuvs_tombstone_header_init(CuvsTombstoneHeader *h, uint32_t base_tids_crc32);
int  cuvs_tombstone_validate(const CuvsTombstoneHeader *h, int64_t body_bytes);
int  cuvs_tombstone_read_header(FILE *f, CuvsTombstoneHeader *out);

/* ----------------------------------------------------------------
 * I3 admission rejection payload codec (v6 r18 C5, UC-2; CK-29).
 *
 * Text grammar and key meanings live in cuvs_admission_contract.h. The codec
 * is format-only: which keys a reason carries is the caller's choice (C5
 * subsets). Keys are emitted in the fixed order n f q p h e k l y u g s r t*
 * w i. Scalars (MiB, counts) saturate at CUVS_ADMISSION_SCALAR_MAX on encode
 * and are rejected above it on decode; shard i saturates at 999.
 * ---------------------------------------------------------------- */
enum {
    CUVS_ADM_F_N = 1u << 0,  CUVS_ADM_F_F = 1u << 1,  CUVS_ADM_F_Q = 1u << 2,
    CUVS_ADM_F_P = 1u << 3,  CUVS_ADM_F_H = 1u << 4,  CUVS_ADM_F_E = 1u << 5,
    CUVS_ADM_F_K = 1u << 6,  CUVS_ADM_F_L = 1u << 7,  CUVS_ADM_F_Y = 1u << 8,
    CUVS_ADM_F_U = 1u << 9,  CUVS_ADM_F_G = 1u << 10, CUVS_ADM_F_S = 1u << 11,
    CUVS_ADM_F_R = 1u << 12, CUVS_ADM_F_W = 1u << 13, CUVS_ADM_F_I = 1u << 14,
    CUVS_ADM_F_ALL = (1u << 15) - 1u
};

typedef struct CuvsAdmissionFields {
    CuvsAdmissionReason reason;
    uint32_t present;            /* CUVS_ADM_F_* of the scalar keys sent */
    uint32_t n, f, q, p;         /* MiB */
    uint32_t h_mib, h_cnt, e_mib, e_cnt, k_mib, k_cnt;
    uint32_t l, y, u, g, s;      /* MiB */
    uint32_t r;                  /* 0 or 1, daemon-computed */
    int32_t  w;                  /* CUVS_ADMISSION_WAIT_QUERIES or _UNPIN */
    uint32_t i;                  /* shard number */
    uint32_t n_cand;             /* 0..CUVS_ADMISSION_MAX_CANDIDATES */
    CuvsAdmissionCandidate cand[CUVS_ADMISSION_MAX_CANDIDATES];
} CuvsAdmissionFields;

/* NOTICE transport: N1:msg<US>detail<RS>..., at most three pairs.
 * Plain legacy messages decode as one pair. len is the accessible byte count.
 * append returns0,1 for (+N more), or-1 for malformed arguments.
 * The daemon logs every full pair before append, including overflowed ones. */
typedef struct CuvsNoticePair {
    char message[CUVS_NOTICE_OUT_BYTES];
    char detail[CUVS_NOTICE_OUT_BYTES];
} CuvsNoticePair;
typedef struct CuvsNoticeBatch {
    uint32_t count;
    CuvsNoticePair pairs[CUVS_NOTICE_MAX_PAIRS];
} CuvsNoticeBatch;
int cuvs_notice_decode(const char *wire, size_t len, CuvsNoticeBatch *out);
int cuvs_notice_append(char *wire, size_t cap, const char *message, const char *detail);

/* [admit] shape_class: "checked", "bracketed" or "extrapolated" (v5 C3;
 * gd/igd 0 mean the defaults 64/128; float32 is implied by the cagra AM). */
const char *cuvs_build_shape_class(int64_t n, uint32_t dim, uint32_t metric,
                                   uint32_t gd, uint32_t igd, uint32_t build_algo);
/* bytes -> MiB, floored, saturated. */
uint32_t cuvs_admission_mib(uint64_t bytes);
/* count, saturated. */
uint32_t cuvs_admission_count(uint64_t count);
/* "AR0".."AR7", "OOM", "ATT"; NULL for NONE or out of range. */
const char *cuvs_admission_reason_name(CuvsAdmissionReason reason);
/* 54000 / 53200 / 53400 (C5); NULL for NONE or out of range. */
const char *cuvs_admission_sqlstate(CuvsAdmissionReason reason);
/* 1 for reasons a later retry may clear without a config change and that
 * count as transient for the circuit breaker (UC-2 3): AR1..AR5, AR7, OOM,
 * ATT. 0 for AR0 (permanent), AR6 (configuration) and NONE. */
int cuvs_admission_reason_transient(CuvsAdmissionReason reason);
/* Write the payload into buf (NUL-terminated). Returns its length, or -1
 * (buf[0] = '\0' when cap > 0) for NONE, more than three candidates,
 * w not in {0,-1}, r not in {0,1}, idle_s < -1, or a too-small buffer. */
int cuvs_admission_encode(const CuvsAdmissionFields *in, char *buf, size_t cap);
/* Parse a payload read from a buffer of len bytes. The NUL must lie within
 * the first min(len, CUVS_ADMISSION_ERR_OUT_BYTES) bytes; the parser never
 * reads past it. Returns 0, or -1 for any malformed, unknown, duplicate or
 * out-of-range token (out is then zeroed). */
int cuvs_admission_decode(const char *buf, size_t len, CuvsAdmissionFields *out);

/* User-visible rejection text (C5 errmsg, DETAIL templates, HINT rules).
 * PG-free: the backend supplies name lookup and the unload privilege test.
 * index_name writes the quote_qualified_identifier() form of a same-DB index
 * and returns 0, or -1 when the OID does not resolve. can_unload returns 1
 * when the caller owns the index or is superuser (C7). Each output buffer
 * gets a NUL-terminated (possibly truncated) string. Returns the SQLSTATE,
 * or NULL (outputs empty) when the reason does not belong to obj or the
 * fields are invalid; the caller then keeps its legacy message. */
#define CUVS_ADMISSION_MSG_BYTES 2048

typedef enum CuvsAdmissionObject {
    CUVS_ADMISSION_OBJ_CAGRA,   /* cagra AM build: AR0..AR7, OOM, ATT */
    CUVS_ADMISSION_OBJ_IVFPQ,   /* IVF-PQ AM build: AR0..AR5 */
    CUVS_ADMISSION_OBJ_SHARD,   /* sharded CAGRA build: AR0..AR5 */
    CUVS_ADMISSION_OBJ_LOAD     /* search-path reload: AR0..AR5 */
} CuvsAdmissionObject;

typedef struct CuvsAdmissionCatalog {
    uint32_t my_db_oid;
    int      superuser;
    int    (*index_name)(void *ctx, uint32_t index_oid, char *buf, size_t cap);
    int    (*can_unload)(void *ctx, uint32_t db_oid, uint32_t index_oid);
    void    *ctx;
} CuvsAdmissionCatalog;

const char *cuvs_admission_format(const CuvsAdmissionFields *f,
                                  CuvsAdmissionObject obj,
                                  const char *load_index_name,
                                  const CuvsAdmissionCatalog *cat,
                                  char *msg, size_t msg_cap,
                                  char *detail, size_t detail_cap,
                                  char *hint, size_t hint_cap);

#ifdef CUVS_TEST_HOOKS
/* Test-only fault injection: returns 1 if env var `name` is set, else 0.
 * Compiled in ONLY under CUVS_TEST_HOOKS; absent from production builds. */
int cuvs_fault(const char *name);
#endif

/* ----------------------------------------------------------------
 * Circuit breaker state (per index, process-local). Moved here from
 * cuvs_ipc.h so it can be linked by daemon + extension + tests.
 * ---------------------------------------------------------------- */
#define CUVS_MAX_TRACKED_INDEXES 64

typedef struct CuvsCircuitBreaker {
    uint32_t index_oid;
    int      consecutive_errors;
    int      open;              /* 1 = tripped, routing to CPU */
    int      transient;         /* 1 = last opened by a transient reason (TTL-eligible) */
    int      half_open;         /* 1 = TTL elapsed; the next GPU attempt decides */
    int64_t  opened_at;         /* seconds of the last open; 0 = never opened */
    int64_t  half_open_count;   /* OPEN -> HALF_OPEN transitions */
} CuvsCircuitBreaker;

/* UC-2: a breaker opened by a transient reason (VRAM pressure, reload, ATT)
 * half-opens once cuvs.circuit_breaker_transient_ttl_s has elapsed; one opened
 * by a permanent reason stays open until pg_cuvs_reset_circuit(). */
typedef enum CuvsCircuitState {
    CUVS_CIRCUIT_CLOSED = 0,
    CUVS_CIRCUIT_OPEN,
    CUVS_CIRCUIT_HALF_OPEN
} CuvsCircuitState;

/* What a scan does with the breaker state it observes at execution time. */
typedef enum CuvsCircuitExecAction {
    CUVS_CIRCUIT_EXEC_TRY_GPU = 0,  /* closed or half-open: run the GPU search */
    CUVS_CIRCUIT_EXEC_ERROR         /* open: raise 53200 instead of 0 rows */
} CuvsCircuitExecAction;

/* Defined in cuvs_util.c */
extern CuvsCircuitBreaker cuvs_circuit_breakers[CUVS_MAX_TRACKED_INDEXES];
extern int                cuvs_n_circuit_breakers;

/* Pure, clock-injected core. `now` is seconds; `reason` is a
 * CuvsAdmissionReason (cuvs_admission_contract.h), CUVS_ADMISSION_NONE when
 * the reply carried no reason code. */
int  cuvs_circuit_reason_is_transient(int status, int reason);
void cuvs_circuit_record_failure(uint32_t index_oid, int threshold,
                                 int transient, int64_t now);
void cuvs_circuit_record_status_reason(uint32_t index_oid, int threshold,
                                       int status, int reason, int64_t now);
CuvsCircuitState cuvs_circuit_state(uint32_t index_oid, int64_t now, int ttl_s);
CuvsCircuitExecAction cuvs_circuit_exec_action(CuvsCircuitState state);
const char *cuvs_circuit_state_name(CuvsCircuitState state);

/* Legacy entry points (thin wrappers: wall clock, no reason code). */
void cuvs_circuit_record_error(uint32_t index_oid, int threshold); /* permanent */
void cuvs_circuit_record_status(uint32_t index_oid, int threshold, int status);
void cuvs_circuit_record_success(uint32_t index_oid); /* reset errors; closes a half-open breaker */
void cuvs_circuit_reset(uint32_t index_oid);          /* also clears open flag */
int  cuvs_circuit_is_open(uint32_t index_oid);        /* stored state; no TTL check */
void cuvs_circuit_reset_all(void);                    /* zeroes all breaker state */

/* CAGRA's intermediate buffer must cover the actual dispatch k (including
 * shard/tombstone overfetch). Zero preserves the legacy minimum of 64; an
 * explicit setting is a floor, rounded to the kernel's 32-entry granularity.
 * Inputs come from bounded GUCs: configured <= INT_MAX-31, k <= INT_MAX-31. */
static inline uint32_t
cuvs_cagra_itopk_size(uint32_t configured, uint32_t k)
{
    uint32_t n = configured ? configured : 64;
    if (n < k) n = k;
    return ((n + 31) / 32) * 32;
}

/* ----------------------------------------------------------------
 * Phase 3L-9 / #160: micro-batch grouping (pure, daemon + test).
 *
 * The daemon's batch worker collects concurrent single-query searches and
 * coalesces those targeting the SAME (db_oid, index_oid, precision, dim, mode)
 * into a single batched dispatch — cuvs_bf_search_batch for mode=1
 * (brute_force, Phase 3L-9), cuvs_cagra_search_batch for mode=0 (cagra, #160).
 * cuvs_batch_group is the pure indexing core (no threads, no CUDA): it assigns
 * each request a group id so requests with an identical key share a group.
 * Group ids are dense and assigned in first-seen order. O(n^2) but n is bounded
 * by the batch cap.
 * ---------------------------------------------------------------- */
typedef struct CuvsBatchKey {
    uint32_t db_oid;
    uint32_t index_oid;
    uint32_t precision;   /* 0=float32, 1=float16 (brute_force only) */
    uint32_t dim;
    /* #160: same values as CuvsCmdFrame.search_mode, so cagra and brute_force
     * requests for one index never land in the same dispatch. Appended last so
     * a 4-field positional initializer still means mode=0 (cagra). */
    uint32_t mode;        /* 0=cagra, 1=brute_force */
    uint32_t itopk_size;  /* CAGRA only; 0 = derive from dispatch k */
    uint32_t search_width;/* CAGRA only; 0 = SDK default */
} CuvsBatchKey;

/* group_id_out[i] = group of request i (requests with equal keys share it);
 * *n_groups_out = number of distinct keys. group_id_out must hold n ints. */
void cuvs_batch_group(const CuvsBatchKey *keys, int n,
                         int *group_id_out, int *n_groups_out);
