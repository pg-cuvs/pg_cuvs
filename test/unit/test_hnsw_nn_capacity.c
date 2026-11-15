#include "cuvs_hnsw_nn_capacity.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
int main(void)
{
    CuvsHnswNnCapacity a, b;
    assert(cuvs_hnsw_nn_capacity(1000000, 768, 64, 128, 1, CUVS_HNSW_NN_FP16, &a) == 0);
    assert(cuvs_hnsw_nn_capacity(1000000, 768, 64, 128, 1, CUVS_HNSW_NN_FP32, &b) == 0);
    /* 6548 before decision B + 4*768 for the centered NN-descent input copy */
    assert(a.build_host == UINT64_C(1000000) * (6548 + 4 * 768));
    assert(a.serialize_host == UINT64_C(1000000) * 22740);
    assert(a.sort_gpu == UINT64_C(1000000) * (4 * 768 + 4 * 128));
    assert(a.build_gpu == UINT64_C(1000000) * (2 * 768 + 280) + UINT64_C(100000) * 4 * 768);
    assert(b.build_gpu == a.build_gpu + UINT64_C(1000000) * 2 * 768);
    assert(b.hierarchy_gpu == UINT64_C(1000000) * (4 * 768 + 280 + 4 * 64) + UINT64_C(100000) * 4 * 768);
    assert(b.gpu_peak >= b.build_gpu && b.gpu_peak >= b.sort_gpu && b.gpu_peak >= b.hierarchy_gpu);
    assert(b.host_peak >= b.build_host && b.host_peak >= b.serialize_host);
    assert(cuvs_hnsw_nn_capacity(0, 768, 64, 128, 1, CUVS_HNSW_NN_FP32, &a) != 0);
    assert(cuvs_hnsw_nn_capacity(INT32_MAX - 1, 768, 64, 128, 1, CUVS_HNSW_NN_FP32, &a) != 0);
    assert(cuvs_hnsw_nn_capacity(1000, 768, 64, 1025, 1, CUVS_HNSW_NN_FP32, &a) != 0);
    assert(cuvs_hnsw_nn_capacity(1000, 768, 64, 128, 1, 99, &a) != 0);
    /* nn-diff-review-r87 D4: shape guards and hierarchy/dtype terms */
    assert(cuvs_hnsw_nn_capacity(1000, 768, 64, 128, 2, CUVS_HNSW_NN_FP32, &a) != 0);
    assert(cuvs_hnsw_nn_capacity(1000, 65535, 64, 128, 0, CUVS_HNSW_NN_FP32, &a) == 0);
    assert(cuvs_hnsw_nn_capacity(1000, 65536, 64, 128, 0, CUVS_HNSW_NN_FP32, &a) != 0);
    assert(cuvs_hnsw_nn_capacity(1000, 768, 63, 128, 0, CUVS_HNSW_NN_FP32, &a) != 0);
    assert(cuvs_hnsw_nn_capacity(1000, 768, 0, 128, 0, CUVS_HNSW_NN_FP32, &a) != 0);
    assert(cuvs_hnsw_nn_capacity(100, 768, 64, 100, 0, CUVS_HNSW_NN_FP32, &a) != 0);
    assert(cuvs_hnsw_nn_capacity(101, 768, 64, 100, 0, CUVS_HNSW_NN_FP32, &a) == 0);
    {
        CuvsHnswNnCapacity flat, tall;
        assert(cuvs_hnsw_nn_capacity(1000000, 768, 64, 128, 0, CUVS_HNSW_NN_FP32, &flat) == 0);
        assert(cuvs_hnsw_nn_capacity(1000000, 768, 64, 128, 1, CUVS_HNSW_NN_FP32, &tall) == 0);
        assert(flat.hierarchy_gpu == 0 && tall.hierarchy_gpu > 0);
        assert(tall.serialize_host - flat.serialize_host
               == UINT64_C(1000000) * (2 * 768 * 4 + 8 * 32 + 916 + 2 * 96 + 4 * 32));
        assert(flat.build_host == tall.build_host && flat.build_gpu == tall.build_gpu);
    }
    uint64_t checked;
    assert(cuvs_hnsw_nn_mul(UINT64_MAX, 2, &checked) != 0);
    assert(cuvs_hnsw_nn_add(UINT64_MAX, 1, &checked) != 0);
    puts("NN resident capacity phase accounting: PASS");
    return 0;
}
