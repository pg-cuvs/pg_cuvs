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
