#include "cuvs_hnsw_build_common.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    CuvsHnswCapacity cap;
    assert(cuvs_hnsw_scaled_quality(128, 64, 64, 4096) == 128);
    assert(cuvs_hnsw_scaled_quality(200, 64, 4, 65535) == 200);
    assert(cuvs_hnsw_scaled_quality(120, 64, 4, 65535) == 120);
    uint32_t last = 0;
    for (uint32_t ef = 4; ef <= 1000; ef++) {
        uint32_t actual = cuvs_hnsw_scaled_quality(128, ef, 64, 4096);
        assert(actual >= last); last = actual;
    }
    assert(cuvs_hnsw_training_rows(1000000, 174762, 500) == 200000);
    assert(cuvs_hnsw_training_rows(1000, 1, 10) == 10);
    assert(cuvs_hnsw_training_rows(1000, 1000, 1) == 1000);
    assert(!cuvs_hnsw_capacity(1000000, 768, 64, 128, 1, &cap));
    assert(!cuvs_hnsw_training_reserve(&cap, 200000, 768));
    assert(cap.host_peak == UINT64_C(23013670912));
    assert(cap.gpu_peak == UINT64_C(4930470912));
    assert(cap.host_peak < (UINT64_C(32) << 30));
    assert(cap.host_peak > (UINT64_C(16) << 30));
    cap.host_peak = UINT64_MAX;
    assert(cuvs_hnsw_training_reserve(&cap, 1, 768) == -1);
    assert(cuvs_hnsw_training_reserve(&cap, UINT64_MAX, 65535) == -1);
    assert(!cuvs_hnsw_capacity(65536, 1536, 64, 128, 1, &cap));
    uint64_t h = UINT64_C(2) << 30, g = UINT64_C(64) << 30;
    assert(cap.host_peak > h);
    unsigned p = cuvs_hnsw_ace_partitions(65536, 1536, 64, 128,
        h - (UINT64_C(256) << 20), g - (UINT64_C(512) << 20));
    assert(p > 0);
    CuvsHnswPolicy receipt = {0};
    assert(!cuvs_hnsw_policy_valid(&receipt));
    receipt.magic = CUVS_HNSW_POLICY_MAGIC;
    receipt.version = CUVS_HNSW_POLICY_VERSION;
    receipt.bytes = sizeof(receipt);
    receipt.route = CUVS_HNSW_ROUTE_IVF_PQ;
    receipt.internal_builder = 2;
    receipt.requested_ef = 64;
    receipt.guard_passed = 1;
    assert(cuvs_hnsw_policy_valid(&receipt));
    receipt.requested_algo = CUVS_HNSW_ALGO_NN_DESCENT;
    assert(!cuvs_hnsw_policy_valid(&receipt));
    /* Resident IVF-PQ kNN precision: fp16 holds the L2 bound 4 x max|x|^2 only
     * below 65504 (the fp16 maximum); IP and cosine use max|x|^2. */
    assert(!cuvs_hnsw_ivfpq_needs_fp32(16375.0, CUVS_METRIC_L2));   /* 65500 */
    assert(cuvs_hnsw_ivfpq_needs_fp32(16376.0, CUVS_METRIC_L2));    /* 65504 */
    assert(!cuvs_hnsw_ivfpq_needs_fp32(65503.0, CUVS_METRIC_IP));
    assert(cuvs_hnsw_ivfpq_needs_fp32(65504.0, CUVS_METRIC_IP));
    assert(!cuvs_hnsw_ivfpq_needs_fp32(1.0, CUVS_METRIC_COSINE));     /* normalized */
    assert(cuvs_hnsw_ivfpq_needs_fp32(NAN, CUVS_METRIC_L2));
    assert(cuvs_hnsw_ivfpq_needs_fp32(INFINITY, CUVS_METRIC_IP));
    {
        const float rows[6] = {3, 4, 0, 1, 0, 2};          /* |x|^2 = 25, 1, 4 */
        assert(cuvs_hnsw_max_sq_norm(rows, 3, 2) == 25.0);
        assert(cuvs_hnsw_max_sq_norm(rows, 0, 2) == 0.0);
        const float bad[4] = {1, 2, NAN, 0};
        assert(isinf(cuvs_hnsw_max_sq_norm(bad, 2, 2)));
        float sift[128], unit[128];                          /* SIFT-scale and a unit row */
        for (int j = 0; j < 128; j++) { sift[j] = 45.0f; unit[j] = 0.0f; }
        unit[7] = 1.0f;
        assert(cuvs_hnsw_ivfpq_needs_fp32(cuvs_hnsw_max_sq_norm(sift, 1, 128), CUVS_METRIC_L2));
        assert(!cuvs_hnsw_ivfpq_needs_fp32(cuvs_hnsw_max_sq_norm(unit, 1, 128), CUVS_METRIC_L2));
    }
    /* kNN-graph rejection -> ACE only for auto, resident IVF-PQ, overflow ace, n >= 256 */
    {
        const char *why = "RAFT failure at file=graph_core.cuh line=1383: " CUVS_HNSW_GRAPH_KNN_REJECTED
                          " because the initial kNN graph contains too many invalid or duplicated neighbor nodes";
        assert(cuvs_hnsw_graph_failure_to_ace(CUVS_HNSW_ALGO_AUTO, CUVS_HNSW_ROUTE_IVF_PQ,
                                              CUVS_HNSW_OVERFLOW_ACE, 999000, why));
        assert(!cuvs_hnsw_graph_failure_to_ace(CUVS_HNSW_ALGO_IVF_PQ, CUVS_HNSW_ROUTE_IVF_PQ,
                                               CUVS_HNSW_OVERFLOW_ACE, 999000, why));
        assert(!cuvs_hnsw_graph_failure_to_ace(CUVS_HNSW_ALGO_AUTO, CUVS_HNSW_ROUTE_IVF_PQ,
                                               CUVS_HNSW_OVERFLOW_ERROR, 999000, why));
        assert(!cuvs_hnsw_graph_failure_to_ace(CUVS_HNSW_ALGO_AUTO, CUVS_HNSW_ROUTE_ACE,
                                               CUVS_HNSW_OVERFLOW_ACE, 999000, why));
        assert(!cuvs_hnsw_graph_failure_to_ace(CUVS_HNSW_ALGO_AUTO, CUVS_HNSW_ROUTE_TINY,
                                               CUVS_HNSW_OVERFLOW_ACE, 999000, why));
        assert(!cuvs_hnsw_graph_failure_to_ace(CUVS_HNSW_ALGO_AUTO, CUVS_HNSW_ROUTE_IVF_PQ,
                                               CUVS_HNSW_OVERFLOW_ACE, 255, why));
        assert(!cuvs_hnsw_graph_failure_to_ace(CUVS_HNSW_ALGO_AUTO, CUVS_HNSW_ROUTE_IVF_PQ,
                                               CUVS_HNSW_OVERFLOW_ACE, 999000, "std::bad_alloc"));
        assert(!cuvs_hnsw_graph_failure_to_ace(CUVS_HNSW_ALGO_AUTO, CUVS_HNSW_ROUTE_IVF_PQ,
                                               CUVS_HNSW_OVERFLOW_ACE, 999000, NULL));
    }
    printf("PASS default preservation, monotonic construction effort, SDK integer training shape, 1M reserve, overflow, forced ACE candidate P%u, receipt validation, fp16 kNN bound, kNN-graph fallback\n", p);
    return 0;
}
