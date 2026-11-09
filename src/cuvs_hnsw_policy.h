#ifndef CUVS_HNSW_POLICY_H
#define CUVS_HNSW_POLICY_H

/* HNSW-only policy. Not the device CAGRA AUTO policy. PG/CUDA-free. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define CUVS_HNSW_ALGO_AUTO 0u
#define CUVS_HNSW_ALGO_IVF_PQ 1u
#define CUVS_HNSW_ALGO_NN_DESCENT 2u /* reserved; no supported memory model */
#define CUVS_HNSW_ALGO_ACE 3u
#define CUVS_HNSW_MIN_ELEMENTS 16
#define CUVS_HNSW_ROUTE_TINY 1u
#define CUVS_HNSW_ROUTE_IVF_PQ 2u
#define CUVS_HNSW_ROUTE_ACE 3u
#define CUVS_HNSW_POLICY_MAGIC UINT32_C(0x43565031)
#define CUVS_HNSW_POLICY_VERSION 2u
#define CUVS_HNSW_POLICY_LEGACY_VERSION 1u

/* ADR-087 (UC-5, 2026-11-03): what build_algo=auto does when the resident
 * IVF-PQ estimate exceeds the host/GPU budget. Daemon flag
 * --hnsw-auto-overflow. ace = spill to ACE-disk with NOTICE + receipt;
 * error = status 13 + ace hint (the I1 behaviour). Explicit ivf_pq/ace never
 * read this value. Reverting the default is a one-line macro change. */
#define CUVS_HNSW_OVERFLOW_ERROR 0u
#define CUVS_HNSW_OVERFLOW_ACE 1u
#ifndef CUVS_HNSW_AUTO_OVERFLOW_DEFAULT
#define CUVS_HNSW_AUTO_OVERFLOW_DEFAULT CUVS_HNSW_OVERFLOW_ACE
#endif

/* CuvsHnswPolicy.route_flags (was the zero `reserved` word; layout unchanged).
 * AUTO_OVERFLOW: auto exceeded the resident budget and spilled to ACE. It is
 * set only by cuvs_hnsw_resolve_route's single overflow branch and is the
 * only way an auto receipt may name route ACE.
 * Timing and its cause are separate facts (I3CD-B1): PRESPOOL: the backend
 * forecast the route from reltuples/dim before the heap scan (PN-1).
 * AFTER_SCAN: the route was decided only after the scan. NO_STATISTICS: the
 * cause was missing/zero reltuples; without it AFTER_SCAN means statistics
 * existed but the pre-scan forecast was unavailable (daemon/IPC failure).
 * PRESPOOL and AFTER_SCAN are mutually exclusive; NO_STATISTICS needs
 * AFTER_SCAN.
 * IVFPQ_FP32: the resident IVF-PQ kNN search ran with fp32 LUT, internal and
 * coarse distances because fp16 could overflow for this data
 * (cuvs_hnsw_ivfpq_needs_fp32). Route IVF_PQ only.
 * GRAPH_FALLBACK: auto chose resident IVF-PQ, cuVS rejected its kNN graph
 * (CUVS_HNSW_GRAPH_KNN_REJECTED) and, with overflow ace, the build ran on
 * ACE-disk instead. Requested auto, route ACE, never with AUTO_OVERFLOW. */
#define CUVS_HNSW_ROUTE_FLAG_AUTO_OVERFLOW 0x1u
#define CUVS_HNSW_ROUTE_FLAG_PRESPOOL 0x2u
#define CUVS_HNSW_ROUTE_FLAG_AFTER_SCAN 0x4u
#define CUVS_HNSW_ROUTE_FLAG_NO_STATISTICS 0x8u
#define CUVS_HNSW_ROUTE_FLAG_IVFPQ_FP32 0x10u
#define CUVS_HNSW_ROUTE_FLAG_GRAPH_FALLBACK 0x20u
#define CUVS_HNSW_ROUTE_FLAGS_KNOWN 0x3fu

/* Receipt JSON "route_decided" (pg_cuvs_hnsw_build_info). */
static inline const char *cuvs_hnsw_route_decided_name(uint32_t flags)
{
    if (flags & CUVS_HNSW_ROUTE_FLAG_PRESPOOL) return "before_scan_from_statistics";
    if (flags & CUVS_HNSW_ROUTE_FLAG_AFTER_SCAN)
        return (flags & CUVS_HNSW_ROUTE_FLAG_NO_STATISTICS)
            ? "after_scan_no_statistics" : "after_scan_preflight_unavailable";
    return "not_recorded";
}

static inline const char *cuvs_hnsw_overflow_name(uint32_t mode)
{
    return mode == CUVS_HNSW_OVERFLOW_ACE ? "ace"
         : mode == CUVS_HNSW_OVERFLOW_ERROR ? "error" : "unsupported";
}

static inline uint32_t cuvs_hnsw_overflow_from_name(const char *name)
{
    if (name && !strcmp(name, "ace")) return CUVS_HNSW_OVERFLOW_ACE;
    if (name && !strcmp(name, "error")) return CUVS_HNSW_OVERFLOW_ERROR;
    return UINT32_MAX;
}

/* Stored after the unchanged pgvector metapage prefix and returned by op23.
 * All estimates are route selection inputs, not allocator/RSS upper bounds.
 * A receipt is published only after the native build returns successfully. */
typedef struct CuvsHnswPolicy {
    uint32_t magic, version, bytes, route;
    uint32_t requested_algo, requested_ef, degree, intermediate;
    uint32_t hierarchy_ef, ace_ef, partitions, internal_builder;
    uint32_t n_lists, pq_dim, pq_bits, n_probes;
    uint32_t query_batch, guard_passed, hierarchy, dim;
    uint64_t n_rows;
    uint32_t metric, route_flags;
    uint64_t training_requested, training_actual;
    uint64_t host_estimate, gpu_estimate, host_budget, gpu_budget;
    uint64_t sdk_host_budget, sdk_gpu_budget;
} CuvsHnswPolicy;

/* HNSW options are not a numeric range: reserved NN-descent must stay closed. */
static inline int cuvs_hnsw_algo_supported(uint32_t algo)
{
    return algo == CUVS_HNSW_ALGO_AUTO || algo == CUVS_HNSW_ALGO_IVF_PQ
        || algo == CUVS_HNSW_ALGO_ACE;
}

static inline const char *cuvs_hnsw_algo_name(uint32_t algo)
{
    switch (algo) {
    case CUVS_HNSW_ALGO_AUTO: return "auto";
    case CUVS_HNSW_ALGO_IVF_PQ: return "ivf_pq";
    case CUVS_HNSW_ALGO_ACE: return "ace";
    default: return "unsupported";
    }
}

static inline uint32_t cuvs_hnsw_algo_from_name(const char *name)
{
    if (!name || !strcmp(name, "auto")) return CUVS_HNSW_ALGO_AUTO;
    if (!strcmp(name, "ivf_pq")) return CUVS_HNSW_ALGO_IVF_PQ;
    if (!strcmp(name, "ace")) return CUVS_HNSW_ALGO_ACE;
    if (!strcmp(name, "nn_descent")) return CUVS_HNSW_ALGO_NN_DESCENT;
    return UINT32_MAX;
}

static inline const char *cuvs_hnsw_route_name(uint32_t route)
{
    switch (route) {
    case CUVS_HNSW_ROUTE_TINY: return "tiny";
    case CUVS_HNSW_ROUTE_IVF_PQ: return "resident_ivf_pq";
    case CUVS_HNSW_ROUTE_ACE: return "ace_disk";
    default: return "unknown";
    }
}

/* Reloption default64 preserves I128 / hierarchy200 (ACE: cuvs_hnsw_ace_ef). These control
 * construction effort, not a promise of monotonic recall. Do not replace the
 * pinned IVF training/probe defaults with cuVS AUTO heuristics. */
static inline uint32_t cuvs_hnsw_scaled_quality(uint32_t base, uint32_t ef,
                                               uint32_t minimum, uint32_t maximum)
{
    uint64_t value = ((uint64_t)base * ef + 63) / 64;
    return value < minimum ? minimum : value > maximum ? maximum : (uint32_t)value;
}

/* ACE partition build quality, daemon --hnsw-ace-build-quality auto|Q.
 * cuVS 26.04 reads ace_params.ef_construction (A) only as q = A / 16
 * (cagra.cpp graph_params_heuristic): an ACE partition under 1e6 rows builds
 * its kNN graph with NN-descent for 5 + q iterations, a larger one with IVF-PQ
 * at round(2 + sqrt(n_lists) / 20 + q) probes.
 * auto: a partition's NN-descent runs the iteration budget that a whole-
 *   dataset NN-descent build runs by default (T, nn_descent::index_params
 *   max_iterations of the linked SDK, 20 in cuVS 26.04) at the reloption
 *   default ef_construction 64: q = T - 5, A = 16 * (T - 5). Other
 *   ef_construction values scale A like the intermediate degree and the
 *   hierarchy quality. (The SDK's own ace_params default A 120 gives 12
 *   iterations, which left ACE recall at low ef_search 0.01+ below resident
 *   IVF-PQ at ef_construction 64; UC-5.)
 *   The partition kNN width stays the SDK's 3 * (degree / 2), 96 at degree
 *   64. Handing the scaled intermediate degree (128) to the partition
 *   builder instead lowered recall at both 12 and 20 iterations (999,000 x
 *   768, degree 64), so it is recorded, not changed.
 * Q (0..4095): A = 16 * Q for every build whatever ef_construction is, for
 *   diagnosis or pinning. Q is the integer the SDK consumes, so distinct Q
 *   values never collapse to the same SDK input. */
#define CUVS_HNSW_ACE_QUALITY_AUTO UINT32_MAX
#define CUVS_HNSW_ACE_QUALITY_MAX 4095u
#define CUVS_HNSW_SDK_ACE_QUALITY_DIVISOR 16u

/* 0 and *out set on "auto" or a decimal 0..4095; -1 otherwise. */
static inline int cuvs_hnsw_ace_quality_parse(const char *name, uint32_t *out)
{
    uint32_t q = 0;
    if (!name || !*name) return -1;
    if (!strcmp(name, "auto")) { *out = CUVS_HNSW_ACE_QUALITY_AUTO; return 0; }
    for (const char *c = name; *c; c++)
    {
        if (*c < '0' || *c > '9' || c - name >= 4) return -1;
        q = q * 10 + (uint32_t)(*c - '0');
    }
    if (q > CUVS_HNSW_ACE_QUALITY_MAX) return -1;
    *out = q;
    return 0;
}

#define CUVS_HNSW_SDK_ACE_NN_ITERATION_OFFSET 5u

/* The A handed to the SDK. sdk_nn_iterations is the linked SDK's default
 * NN-descent max_iterations (nn_descent::index_params{}.max_iterations). */
static inline uint32_t cuvs_hnsw_ace_ef(uint32_t setting, uint32_t ef,
                                        uint32_t sdk_nn_iterations)
{
    if (setting == CUVS_HNSW_ACE_QUALITY_AUTO)
    {
        uint32_t q = sdk_nn_iterations > CUVS_HNSW_SDK_ACE_NN_ITERATION_OFFSET
            ? sdk_nn_iterations - CUVS_HNSW_SDK_ACE_NN_ITERATION_OFFSET : 0;
        if (q > CUVS_HNSW_ACE_QUALITY_MAX) q = CUVS_HNSW_ACE_QUALITY_MAX;
        return cuvs_hnsw_scaled_quality(q * CUVS_HNSW_SDK_ACE_QUALITY_DIVISOR, ef, 4, 65535);
    }
    return setting * CUVS_HNSW_SDK_ACE_QUALITY_DIVISOR;
}

static inline int cuvs_hnsw_policy_valid(const CuvsHnswPolicy *p)
{
    return p && p->magic == CUVS_HNSW_POLICY_MAGIC
        && p->version == CUVS_HNSW_POLICY_VERSION && p->bytes == sizeof(*p)
        && p->route >= CUVS_HNSW_ROUTE_TINY && p->route <= CUVS_HNSW_ROUTE_ACE
        && cuvs_hnsw_algo_supported(p->requested_algo)
        && p->requested_ef >= 4 && p->requested_ef <= 1000
        && p->guard_passed == 1
        /* v2 records this pinned SDK's base/local builder, not upper-layer history. */
        && p->internal_builder == p->route
        && (p->route == CUVS_HNSW_ROUTE_ACE ? p->partitions > 0 : p->partitions == 0)
        && !(p->route_flags & ~CUVS_HNSW_ROUTE_FLAGS_KNOWN)
        && (p->route_flags & (CUVS_HNSW_ROUTE_FLAG_PRESPOOL | CUVS_HNSW_ROUTE_FLAG_AFTER_SCAN))
            != (CUVS_HNSW_ROUTE_FLAG_PRESPOOL | CUVS_HNSW_ROUTE_FLAG_AFTER_SCAN)
        && (!(p->route_flags & CUVS_HNSW_ROUTE_FLAG_NO_STATISTICS)
            || (p->route_flags & CUVS_HNSW_ROUTE_FLAG_AFTER_SCAN))
        /* auto may name ACE only through the recorded overflow branch or the
         * recorded kNN-graph fallback, exactly one of them. */
        && (!!(p->route_flags & CUVS_HNSW_ROUTE_FLAG_AUTO_OVERFLOW)
            + !!(p->route_flags & CUVS_HNSW_ROUTE_FLAG_GRAPH_FALLBACK)
            == (p->requested_algo == CUVS_HNSW_ALGO_AUTO && p->route == CUVS_HNSW_ROUTE_ACE))
        && (!(p->route_flags & CUVS_HNSW_ROUTE_FLAG_IVFPQ_FP32)
            || p->route == CUVS_HNSW_ROUTE_IVF_PQ)
        && ((p->requested_algo == CUVS_HNSW_ALGO_AUTO
                && (p->route == CUVS_HNSW_ROUTE_TINY || p->route == CUVS_HNSW_ROUTE_IVF_PQ
                    || p->route == CUVS_HNSW_ROUTE_ACE))
            || (p->requested_algo == CUVS_HNSW_ALGO_IVF_PQ && p->route == CUVS_HNSW_ROUTE_IVF_PQ)
            || (p->requested_algo == CUVS_HNSW_ALGO_ACE && p->route == CUVS_HNSW_ROUTE_ACE));
}

/* Historical v1 AUTO could select ACE. Never reinterpret it using v2 policy. */
static inline int cuvs_hnsw_policy_legacy_valid(const CuvsHnswPolicy *p)
{
    return p && p->magic == CUVS_HNSW_POLICY_MAGIC
        && p->version == CUVS_HNSW_POLICY_LEGACY_VERSION && p->bytes == sizeof(*p)
        && p->route >= CUVS_HNSW_ROUTE_TINY && p->route <= CUVS_HNSW_ROUTE_ACE
        && p->requested_algo <= CUVS_HNSW_ALGO_IVF_PQ
        && p->requested_ef >= 4 && p->requested_ef <= 1000 && p->guard_passed == 1
        && !(p->requested_algo == CUVS_HNSW_ALGO_IVF_PQ && p->route != CUVS_HNSW_ROUTE_IVF_PQ);
}

typedef enum CuvsHnswPolicyReadStatus {
    CUVS_HNSW_POLICY_NOT_RECORDED,
    CUVS_HNSW_POLICY_RECORDED,
    CUVS_HNSW_POLICY_UNSUPPORTED,
    CUVS_HNSW_POLICY_CORRUPT
} CuvsHnswPolicyReadStatus;

/* Bounded metapage tail reader; v1/v2 have the same layout. Unknown future
 * versions are unsupported, not proof of index corruption or absent history. */
static inline CuvsHnswPolicyReadStatus
cuvs_hnsw_policy_read(const void *data, size_t available, CuvsHnswPolicy *out)
{
    uint32_t header[3];
    memset(out, 0, sizeof(*out));
    if (available == 0) return CUVS_HNSW_POLICY_NOT_RECORDED;
    if (available < sizeof(header[0])) return CUVS_HNSW_POLICY_CORRUPT;
    memcpy(header, data, sizeof(header[0]));
    if (header[0] != CUVS_HNSW_POLICY_MAGIC) return CUVS_HNSW_POLICY_NOT_RECORDED;
    if (available < sizeof(header)) return CUVS_HNSW_POLICY_CORRUPT;
    memcpy(header, data, sizeof(header));
    if (header[1] != CUVS_HNSW_POLICY_VERSION && header[1] != CUVS_HNSW_POLICY_LEGACY_VERSION)
        return CUVS_HNSW_POLICY_UNSUPPORTED;
    if (header[2] != sizeof(*out) || available < sizeof(*out)) return CUVS_HNSW_POLICY_CORRUPT;
    memcpy(out, data, sizeof(*out));
    if (cuvs_hnsw_policy_valid(out) || cuvs_hnsw_policy_legacy_valid(out))
        return CUVS_HNSW_POLICY_RECORDED;
    return CUVS_HNSW_POLICY_CORRUPT;
}

/* ACE partition-builder readback. Stored on the metapage right after the
 * receipt of an ACE index and appended to the op-23 reply (payload v5); the
 * receipt layout and version are unchanged, so an older ACE index simply has
 * no readback. ACE builds run only on the verified pinned SDK (memory-guard
 * marker @60b19a0f), whose partition builders do this with A and degree:
 *   local_intermediate: from_hnsw_params(degree / 2, SAME_GRAPH_FOOTPRINT)
 *     gives each partition builder a kNN width of 3 * (degree / 2), not the
 *     receipt's intermediate degree (the resident width and the ACE memory
 *     plan input).
 *   local_iterations: NN-descent iterations of every partition. A partition
 *     under 1e6 rows runs 5 + q; one of 1e6 rows or more is moved from IVF-PQ
 *     to NN-descent at the SDK default iterations by the positive-budget
 *     partition ledger (patch 0003). Two partitions each hold every row (core
 *     plus augmented), so the value is exact; with more partitions and at
 *     least 1e6 rows it differs between partitions and is recorded as 0. */
#define CUVS_HNSW_ACE_READBACK_MAGIC UINT32_C(0x43564152)
#define CUVS_HNSW_ACE_READBACK_VERSION 1u
#define CUVS_HNSW_SDK_ACE_NN_MAX_ROWS UINT64_C(1000000)
#define CUVS_HNSW_ACE_SOURCE_AUTO 0u
#define CUVS_HNSW_ACE_SOURCE_MANUAL 1u
typedef struct CuvsHnswAceReadback {
    uint32_t magic, version, bytes;
    uint32_t quality_source;     /* CUVS_HNSW_ACE_SOURCE_*: --hnsw-ace-build-quality */
    uint32_t quality;            /* q = ace_ef / 16, the integer the SDK consumes */
    uint32_t local_intermediate; /* partition kNN width */
    uint32_t local_iterations;   /* 0: differs between partitions */
    uint32_t reserved;           /* 0 */
} CuvsHnswAceReadback;

static inline uint32_t cuvs_hnsw_sdk_ace_local_width(uint32_t degree)
{
    return 3u * (degree / 2u);
}

static inline void cuvs_hnsw_ace_readback_fill(CuvsHnswAceReadback *r,
    uint32_t setting, uint32_t ace_ef, uint32_t degree, uint64_t n_rows,
    uint32_t partitions, uint32_t sdk_nn_iterations)
{
    uint32_t q = ace_ef / CUVS_HNSW_SDK_ACE_QUALITY_DIVISOR;
    memset(r, 0, sizeof(*r));
    r->magic = CUVS_HNSW_ACE_READBACK_MAGIC;
    r->version = CUVS_HNSW_ACE_READBACK_VERSION;
    r->bytes = sizeof(*r);
    r->quality_source = setting == CUVS_HNSW_ACE_QUALITY_AUTO
        ? CUVS_HNSW_ACE_SOURCE_AUTO : CUVS_HNSW_ACE_SOURCE_MANUAL;
    r->quality = q;
    r->local_intermediate = cuvs_hnsw_sdk_ace_local_width(degree);
    if (n_rows < CUVS_HNSW_SDK_ACE_NN_MAX_ROWS)
        r->local_iterations = CUVS_HNSW_SDK_ACE_NN_ITERATION_OFFSET + q;
    else if (partitions == 2)
        r->local_iterations = sdk_nn_iterations;
    else
        r->local_iterations = 0;
}

/* Shape check of a stored or received readback against its receipt. */
static inline int cuvs_hnsw_ace_readback_valid(const CuvsHnswAceReadback *r,
                                               const CuvsHnswPolicy *p)
{
    return r && p && r->magic == CUVS_HNSW_ACE_READBACK_MAGIC
        && r->version == CUVS_HNSW_ACE_READBACK_VERSION && r->bytes == sizeof(*r)
        && p->route == CUVS_HNSW_ROUTE_ACE
        && r->quality_source <= CUVS_HNSW_ACE_SOURCE_MANUAL
        && r->quality == p->ace_ef / CUVS_HNSW_SDK_ACE_QUALITY_DIVISOR
        && r->local_intermediate == cuvs_hnsw_sdk_ace_local_width(p->degree)
        && (r->local_iterations == 0
            || p->n_rows >= CUVS_HNSW_SDK_ACE_NN_MAX_ROWS
            || r->local_iterations == CUVS_HNSW_SDK_ACE_NN_ITERATION_OFFSET + r->quality)
        && r->reserved == 0;
}

/* Bounded reader of the tail after a receipt: absent, valid, or corrupt. */
static inline CuvsHnswPolicyReadStatus
cuvs_hnsw_ace_readback_read(const void *data, size_t available,
                            const CuvsHnswPolicy *p, CuvsHnswAceReadback *out)
{
    uint32_t magic;
    memset(out, 0, sizeof(*out));
    if (available < sizeof(magic)) return CUVS_HNSW_POLICY_NOT_RECORDED;
    memcpy(&magic, data, sizeof(magic));
    if (magic != CUVS_HNSW_ACE_READBACK_MAGIC) return CUVS_HNSW_POLICY_NOT_RECORDED;
    if (available < sizeof(*out)) return CUVS_HNSW_POLICY_CORRUPT;
    memcpy(out, data, sizeof(*out));
    if (out->version != CUVS_HNSW_ACE_READBACK_VERSION) return CUVS_HNSW_POLICY_UNSUPPORTED;
    return cuvs_hnsw_ace_readback_valid(out, p) ? CUVS_HNSW_POLICY_RECORDED
                                                : CUVS_HNSW_POLICY_CORRUPT;
}

/* Match SDK integer sampling: ratio=max(1,N/max(floor(fraction*N),lists));
 * actual=N/ratio. fraction is the SAME double passed to the SDK. */
static inline uint64_t cuvs_hnsw_training_rows(uint64_t n, uint64_t requested,
                                               uint32_t lists)
{
    double fraction = (double)requested / (double)n;
    uint64_t rows = (uint64_t)(fraction * (double)n);
    if (rows < lists) rows = lists;
    uint64_t ratio = rows ? n / rows : n;
    return n / (ratio ? ratio : 1);
}

#endif
