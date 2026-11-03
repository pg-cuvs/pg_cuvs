#ifndef CUVS_ROUTE_PREFLIGHT_H
#define CUVS_ROUTE_PREFLIGHT_H

/* I3-C (UC-5, ADR-087 decision 4, C4 "I3-C로 당긴 두 항목"): PG/CUDA-free
 * decisions the backend takes around its heap scan. Kept pure so CK-17
 * (12)-(14) exercise exactly what the backend executes. */
#include "cuvs_hnsw_build_common.h"
#include "cuvs_admission_contract.h" /* SQLSTATE and MiB saturation (I3-A) */
#include <math.h>

/* What the backend does with a pre-spool hnsw forecast (op 40). */
typedef enum CuvsHnswPrespoolAction {
    CUVS_HNSW_PRESPOOL_FALLBACK = 0,   /* no usable statistics/forecast: decide after the scan */
    CUVS_HNSW_PRESPOOL_PROCEED,        /* route announced as non-overflow; scan */
    CUVS_HNSW_PRESPOOL_NOTICE_OVERFLOW,/* PN-1: announce auto -> ACE, then scan */
    CUVS_HNSW_PRESPOOL_REJECT_BUDGET,  /* status 13 before the scan (SQLSTATE 54000) */
    CUVS_HNSW_PRESPOOL_REJECT          /* status 5 before the scan (route/disk) */
} CuvsHnswPrespoolAction;

/* reltuples is pg_class.reltuples: -1 (never analyzed, PG14+) or 0 means no
 * statistics. ipc_rc is cuvs_ipc_hnsw_route_preflight's return: only the
 * forecast statuses OK/13/5 carry a decision; any exchange failure
 * (unavailable daemon, protocol error) falls back to the post-scan path. */
static inline CuvsHnswPrespoolAction
cuvs_hnsw_prespool_action(double reltuples, int ipc_rc, uint32_t route,
                          uint32_t overflowed, uint32_t disk_ok)
{
    if (!(reltuples >= 1.0) || !isfinite(reltuples)) return CUVS_HNSW_PRESPOOL_FALLBACK;
    if (ipc_rc == CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED) return CUVS_HNSW_PRESPOOL_REJECT_BUDGET;
    if (ipc_rc == CUVS_STATUS_BUILD_FAILED) return CUVS_HNSW_PRESPOOL_REJECT;
    if (ipc_rc != CUVS_STATUS_OK || !disk_ok
        || route < CUVS_HNSW_ROUTE_TINY || route > CUVS_HNSW_ROUTE_ACE)
        return CUVS_HNSW_PRESPOOL_FALLBACK;
    return overflowed && route == CUVS_HNSW_ROUTE_ACE
        ? CUVS_HNSW_PRESPOOL_NOTICE_OVERFLOW : CUVS_HNSW_PRESPOOL_PROCEED;
}

/* reltuples -> the n_estimate sent to the daemon (0 = do not ask). */
static inline uint64_t
cuvs_prespool_rows(double reltuples)
{
    if (!(reltuples >= 1.0) || !isfinite(reltuples) || reltuples > (double)INT32_MAX)
        return 0;
    return (uint64_t)ceil(reltuples);
}

/* Receipt timing flag: PRESPOOL when the route was forecast before the scan,
 * STATS_FALLBACK otherwise (C4: "통계가 없거나 0이면 ... 영수증에 남긴다"). */
static inline uint32_t
cuvs_hnsw_timing_flag(CuvsHnswPrespoolAction action)
{
    return action == CUVS_HNSW_PRESPOOL_FALLBACK
        ? CUVS_HNSW_ROUTE_FLAG_STATS_FALLBACK : CUVS_HNSW_ROUTE_FLAG_PRESPOOL;
}

/* NOTICE lines owed after a successful build (CK-17 (13)): an auto -> ACE
 * receipt always yields the completion NOTICE and, unless the transition
 * NOTICE was already sent at decision time, the transition NOTICE too. An
 * announced overflow that did not happen gets a correction line. */
#define CUVS_HNSW_NOTICE_TRANSITION      0x1u
#define CUVS_HNSW_NOTICE_AUTO_COMPLETION 0x2u
#define CUVS_HNSW_NOTICE_EXPLICIT_ACE    0x4u
#define CUVS_HNSW_NOTICE_NOT_OVERFLOWED  0x8u

static inline uint32_t
cuvs_hnsw_build_notices(const CuvsHnswPolicy *p, int transition_sent)
{
    if (p->requested_algo == CUVS_HNSW_ALGO_ACE && p->route == CUVS_HNSW_ROUTE_ACE)
        return CUVS_HNSW_NOTICE_EXPLICIT_ACE;
    if (p->requested_algo == CUVS_HNSW_ALGO_AUTO && p->route == CUVS_HNSW_ROUTE_ACE
        && (p->route_flags & CUVS_HNSW_ROUTE_FLAG_AUTO_OVERFLOW))
        return CUVS_HNSW_NOTICE_AUTO_COMPLETION
            | (transition_sent ? 0 : CUVS_HNSW_NOTICE_TRANSITION);
    return transition_sent ? CUVS_HNSW_NOTICE_NOT_OVERFLOWED : 0;
}

/* The exceeded side for "(need X, budget Y)": host first, as the resolver. */
static inline void
cuvs_hnsw_overflow_need(uint64_t host_est, uint64_t host_budget,
                        uint64_t gpu_est, uint64_t gpu_budget,
                        uint64_t *need, uint64_t *budget, const char **side)
{
    int host = host_est > host_budget || !(gpu_est > gpu_budget);
    *need = host ? host_est : gpu_est;
    *budget = host ? host_budget : gpu_budget;
    *side = host ? "host" : "GPU";
}

/* CAGRA reltuples AR0 guard (C4 (iii), CK-17 (14)).
 * TEMPORARY estimate: the final resident size the daemon currently admits
 * (estimate_vram_bytes: n x (dim x 4 + graph_degree x 4)). I3-A owns the
 * admission estimate and will expose it as a pure function in
 * cuvs_admission_contract.h; replace this body with that call then. */
static inline uint64_t
cuvs_cagra_prespool_final_estimate(uint64_t n, uint32_t dim, int graph_degree)
{
    uint64_t gd = graph_degree > 0 ? (uint64_t)graph_degree : 64;
    if (!n || !dim || n > UINT64_MAX / ((uint64_t)dim * 4 + gd * 4)) return UINT64_MAX;
    return n * ((uint64_t)dim * 4 + gd * 4);
}

/* AR0 is permanent, so it fires only when it is certain: statistics exist,
 * every GPU has a configured limit (max_limit > 0), the build cannot be
 * auto-sharded across GPUs (shard_count 1, or 0 with one GPU), and the
 * estimate exceeds the largest per-GPU limit. Anything else scans. */
static inline int
cuvs_cagra_prespool_ar0(double reltuples, uint32_t dim, int graph_degree,
                        uint32_t shard_count, uint32_t n_devices,
                        uint64_t max_limit, uint64_t *estimate)
{
    uint64_t n = cuvs_prespool_rows(reltuples);
    *estimate = 0;
    if (!n || !dim || !max_limit || !n_devices) return 0;
    if (!(shard_count == 1 || (shard_count == 0 && n_devices == 1))) return 0;
    *estimate = cuvs_cagra_prespool_final_estimate(n, dim, graph_degree);
    return *estimate > max_limit;
}

static inline uint32_t
cuvs_prespool_mib(uint64_t bytes)
{
    uint64_t mib = (bytes + (UINT64_C(1) << 20) - 1) >> 20;
    return mib > CUVS_ADMISSION_SCALAR_MAX ? CUVS_ADMISSION_SCALAR_MAX : (uint32_t)mib;
}

#endif
