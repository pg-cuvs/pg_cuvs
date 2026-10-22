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
    receipt.requested_ef = 64;
    receipt.guard_passed = 1;
    assert(cuvs_hnsw_policy_valid(&receipt));
    receipt.requested_algo = CUVS_HNSW_ALGO_NN_DESCENT;
    assert(!cuvs_hnsw_policy_valid(&receipt));
    printf("PASS default preservation, monotonic construction effort, SDK integer training shape, 1M reserve, overflow, forced ACE candidate P%u, receipt validation\n", p);
    return 0;
}
