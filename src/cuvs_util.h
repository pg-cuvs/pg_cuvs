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
} CuvsCircuitBreaker;

/* Defined in cuvs_util.c */
extern CuvsCircuitBreaker cuvs_circuit_breakers[CUVS_MAX_TRACKED_INDEXES];
extern int                cuvs_n_circuit_breakers;

void cuvs_circuit_record_error(uint32_t index_oid, int threshold);
void cuvs_circuit_record_success(uint32_t index_oid); /* reset consecutive_errors */
void cuvs_circuit_reset(uint32_t index_oid);          /* also clears open flag */
int  cuvs_circuit_is_open(uint32_t index_oid);
void cuvs_circuit_reset_all(void);                    /* zeroes all breaker state */
