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

/* Statistics presence is a fact about pg_class.reltuples alone (I3CD-B1 r2):
 * -1 (never analyzed, PG14+), 0, negative or non-finite means no statistics;
 * any positive value, however small or large, means statistics exist. It is
 * deliberately separate from the row-count conversions below, which may
 * decline to forecast (0) for reasons that are not "no statistics". */
static inline int
cuvs_prespool_has_statistics(double reltuples)
{
    return isfinite(reltuples) && reltuples > 0.0;
}

/* Rows implied by positive statistics: ceil, saturated at INT64_MAX; 0 only
 * without statistics. Used where a large count is still meaningful (CAGRA AR0). */
static inline uint64_t
cuvs_prespool_count(double reltuples)
{
    if (!cuvs_prespool_has_statistics(reltuples)) return 0;
    if (reltuples >= 9.2e18) return (uint64_t)INT64_MAX;
    return (uint64_t)ceil(reltuples);
}

/* n_estimate sent to the daemon's hnsw forecast (op 40). 0 = do not ask:
 * no statistics, or a count beyond the HNSW shape limit (INT32_MAX rows),
 * which the resolver cannot forecast; the build then decides after the scan. */
static inline uint64_t
cuvs_prespool_rows(double reltuples)
{
    uint64_t n = cuvs_prespool_count(reltuples);
    return n > (uint64_t)INT32_MAX ? 0 : n;
}

/* ipc_rc is cuvs_ipc_hnsw_route_preflight's return: only the forecast
 * statuses OK/13/5 carry a decision; any exchange failure (unavailable
 * daemon, protocol error) falls back to the post-scan path. */
static inline CuvsHnswPrespoolAction
cuvs_hnsw_prespool_action(double reltuples, int ipc_rc, uint32_t route,
                          uint32_t overflowed, uint32_t disk_ok)
{
    if (!cuvs_prespool_has_statistics(reltuples)) return CUVS_HNSW_PRESPOOL_FALLBACK;
    if (ipc_rc == CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED) return CUVS_HNSW_PRESPOOL_REJECT_BUDGET;
    if (ipc_rc == CUVS_STATUS_BUILD_FAILED) return CUVS_HNSW_PRESPOOL_REJECT;
    if (ipc_rc != CUVS_STATUS_OK || !disk_ok
        || route < CUVS_HNSW_ROUTE_TINY || route > CUVS_HNSW_ROUTE_ACE)
        return CUVS_HNSW_PRESPOOL_FALLBACK;
    return overflowed && route == CUVS_HNSW_ROUTE_ACE
        ? CUVS_HNSW_PRESPOOL_NOTICE_OVERFLOW : CUVS_HNSW_PRESPOOL_PROCEED;
}

/* A no-fit route before any disk probe is a configured budget refusal,
 * not an engine error. Keep unrelated status-5 failures internal. */
static inline int
cuvs_hnsw_prespool_budget_refusal(const CuvsHnswPreflightReply *reply)
{
    if (reply->header.status != CUVS_STATUS_BUILD_FAILED || reply->partitions
        || reply->disk_regular || reply->disk_avail
        || strncmp(reply->header.error, CUVS_HNSW_BUDGET_NO_FIT_REASON,
                   sizeof(reply->header.error)) != 0)
        return 0;
    return (reply->host_budget && reply->host_estimate > reply->host_budget)
        || (reply->gpu_budget && reply->gpu_estimate > reply->gpu_budget);
}

/* hnsw budget DETAIL (v5 C5 "hnsw status 13", v6 C5): exactly six fields,
 * in this order, each in MiB rounded DOWN:
 *   he=<MiB> hb=<MiB> ge=<MiB> gb=<MiB> rp=<MiB> rl=<MiB>
 * he/hb = host estimate/budget, ge/gb = GPU estimate/budget, rp = RMM pool
 * used_bytes (0 without a pool), rl = total_vram_used (logical use; a
 * reloaded index counts its graph_degree-64 estimate, so rl can differ from
 * rp). Exact bytes are on the daemon's [hnsw_route] line. All six at
 * UINT64_MAX give 107 bytes; CK-8 bounds the DETAIL at 127. */
#define CUVS_HNSW_BUDGET_DETAIL_MAX 127u

static inline uint64_t
cuvs_hnsw_floor_mib(uint64_t bytes)
{
    return bytes >> 20;
}

static inline int
cuvs_hnsw_budget_detail(char *out, size_t len, const CuvsHnswBudgetSnapshot *s)
{
    return snprintf(out, len, "he=%llu hb=%llu ge=%llu gb=%llu rp=%llu rl=%llu",
                    (unsigned long long)cuvs_hnsw_floor_mib(s->host_estimate),
                    (unsigned long long)cuvs_hnsw_floor_mib(s->host_budget),
                    (unsigned long long)cuvs_hnsw_floor_mib(s->gpu_estimate),
                    (unsigned long long)cuvs_hnsw_floor_mib(s->gpu_budget),
                    (unsigned long long)cuvs_hnsw_floor_mib(s->pool_used),
                    (unsigned long long)cuvs_hnsw_floor_mib(s->logical_used));
}

/* The pre-spool forecast reply (v2) carries the same six values. */
static inline CuvsHnswBudgetSnapshot
cuvs_hnsw_preflight_budget(const CuvsHnswPreflightReply *r)
{
    CuvsHnswBudgetSnapshot s;
    s.host_estimate = r->host_estimate;
    s.host_budget = r->host_budget;
    s.gpu_estimate = r->gpu_estimate;
    s.gpu_budget = r->gpu_budget;
    s.pool_used = r->pool_used;
    s.logical_used = r->logical_used;
    return s;
}

/* The daemon's disk precheck refusal (status 5, CUVS_HNSW_DISK_REFUSAL_PREFIX).
 * v5 C5 / Q4-4: the backend sends that text as the message itself (no
 * "temporary HNSW build failed" prefix) and attaches CUVS_HNSW_DISK_HINT. */
#define CUVS_HNSW_DISK_HINT \
    "Free space on the filesystem holding pg_cuvs_server --index-dir; the job needs about " \
    "4 x raw vector bytes + n x (graph_degree x 8 + 64) bytes. The PostgreSQL temporary " \
    "tablespace needs a further n x dim x 4 bytes."

static inline int
cuvs_hnsw_disk_refusal(int status, const char *error)
{
    return status == CUVS_STATUS_BUILD_FAILED && error != NULL
        && strncmp(error, CUVS_HNSW_DISK_REFUSAL_PREFIX,
                   sizeof(CUVS_HNSW_DISK_REFUSAL_PREFIX) - 1) == 0;
}

/* Receipt timing flags from the PRE-scan action and the statistics that
 * action saw: PRESPOOL when the route was forecast before the scan; otherwise
 * AFTER_SCAN, plus NO_STATISTICS only when reltuples was absent or zero
 * (C4: "통계가 없거나 0이면 ... 영수증에 남긴다"). A failed pre-scan forecast
 * on a table WITH statistics is not "no statistics" (I3CD-B1). */
static inline uint32_t
cuvs_hnsw_timing_flags(double reltuples, CuvsHnswPrespoolAction action)
{
    if (action != CUVS_HNSW_PRESPOOL_FALLBACK) return CUVS_HNSW_ROUTE_FLAG_PRESPOOL;
    return CUVS_HNSW_ROUTE_FLAG_AFTER_SCAN
        | (cuvs_prespool_has_statistics(reltuples) ? 0 : CUVS_HNSW_ROUTE_FLAG_NO_STATISTICS);
}

/* Wording for the post-scan decision point (NOTICE DETAIL), same fact. */
static inline const char *
cuvs_hnsw_after_scan_reason(double reltuples)
{
    return cuvs_prespool_has_statistics(reltuples)
        ? "pre-scan forecast unavailable" : "no table statistics";
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

/* CAGRA reltuples AR0 guard (C4 (iii), CK-17 (14)). The estimate is I3-A's
 * shared cuvs_admission_estimate_final_bytes (aligned dim stride, saturating).
 * The guard compares the FINAL size, a lower bound of the admission value
 * needed = final + scratch: rho is a daemon setting the backend does not know,
 * and a pre-scan refusal must be certain. */
/* AR0 is permanent, so it fires only when it is certain: statistics exist,
 * every GPU has a configured limit (max_limit > 0), the build cannot be
 * auto-sharded across GPUs (shard_count 1, or 0 with one GPU), and the
 * estimate exceeds the largest per-GPU limit. Anything else scans. */
static inline int
cuvs_cagra_prespool_ar0(double reltuples, uint32_t dim, int graph_degree,
                        uint32_t shard_count, uint32_t n_devices,
                        uint64_t max_limit, uint64_t *estimate)
{
    uint64_t n = cuvs_prespool_count(reltuples);
    *estimate = 0;
    if (!n || !dim || dim > INT32_MAX || !max_limit || !n_devices) return 0;
    if (!(shard_count == 1 || (shard_count == 0 && n_devices == 1))) return 0;
    *estimate = cuvs_admission_estimate_final_bytes((int64_t)n, (int32_t)dim, graph_degree);
    return *estimate > max_limit;
}

/* HINT for an HNSW allocation failure after admission (status 2, v6 C2:
 * auto -> explicit ace; ivf_pq -> budget, cleanup, ace; ace -> as is). */
static inline const char *
cuvs_hnsw_oom_hint(uint32_t build_algo)
{
    switch (build_algo) {
    case CUVS_HNSW_ALGO_AUTO:
        return "The resident estimate was admitted but memory ran out; retry with build_algo='ace' explicitly, or free host/GPU memory.";
    case CUVS_HNSW_ALGO_IVF_PQ:
        return "Raise the build budget (cuvs.max_build_mem_mb), drop or unload unused GPU indexes, or use build_algo='ace'.";
    default:
        return "Free host/GPU memory and retry; ACE has no smaller route.";
    }
}

static inline uint32_t
cuvs_prespool_mib(uint64_t bytes)
{
    uint64_t mib = (bytes + (UINT64_C(1) << 20) - 1) >> 20;
    return mib > CUVS_ADMISSION_SCALAR_MAX ? CUVS_ADMISSION_SCALAR_MAX : (uint32_t)mib;
}

#endif
