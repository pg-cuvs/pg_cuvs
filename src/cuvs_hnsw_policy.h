#ifndef CUVS_HNSW_POLICY_H
#define CUVS_HNSW_POLICY_H

/* HNSW-only policy. Not the device CAGRA AUTO policy. PG/CUDA-free. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define CUVS_HNSW_ALGO_AUTO 0u
#define CUVS_HNSW_ALGO_IVF_PQ 1u
#define CUVS_HNSW_ALGO_NN_DESCENT 2u /* reserved; no supported memory model */
#define CUVS_HNSW_ROUTE_TINY 1u
#define CUVS_HNSW_ROUTE_IVF_PQ 2u
#define CUVS_HNSW_ROUTE_ACE 3u
#define CUVS_HNSW_POLICY_MAGIC UINT32_C(0x43565031)
#define CUVS_HNSW_POLICY_VERSION 1u

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
    uint32_t metric, reserved;
    uint64_t training_requested, training_actual;
    uint64_t host_estimate, gpu_estimate, host_budget, gpu_budget;
    uint64_t sdk_host_budget, sdk_gpu_budget;
} CuvsHnswPolicy;

static inline const char *cuvs_hnsw_route_name(uint32_t route)
{
    switch (route) {
    case CUVS_HNSW_ROUTE_TINY: return "tiny";
    case CUVS_HNSW_ROUTE_IVF_PQ: return "resident_ivf_pq";
    case CUVS_HNSW_ROUTE_ACE: return "ace_disk";
    default: return "unknown";
    }
}

/* Reloption default64 preserves I128 / hierarchy200 / ACE120. These control
 * construction effort, not a promise of monotonic recall. Do not replace the
 * pinned IVF training/probe defaults with cuVS AUTO heuristics. */
static inline uint32_t cuvs_hnsw_scaled_quality(uint32_t base, uint32_t ef,
                                               uint32_t minimum, uint32_t maximum)
{
    uint64_t value = ((uint64_t)base * ef + 63) / 64;
    return value < minimum ? minimum : value > maximum ? maximum : (uint32_t)value;
}

static inline int cuvs_hnsw_policy_valid(const CuvsHnswPolicy *p)
{
    return p && p->magic == CUVS_HNSW_POLICY_MAGIC
        && p->version == CUVS_HNSW_POLICY_VERSION && p->bytes == sizeof(*p)
        && p->route >= CUVS_HNSW_ROUTE_TINY && p->route <= CUVS_HNSW_ROUTE_ACE
        && p->requested_algo <= CUVS_HNSW_ALGO_IVF_PQ
        && p->requested_ef >= 4 && p->requested_ef <= 1000
        && p->guard_passed == 1
        && !(p->requested_algo == CUVS_HNSW_ALGO_IVF_PQ && p->route != CUVS_HNSW_ROUTE_IVF_PQ);
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
