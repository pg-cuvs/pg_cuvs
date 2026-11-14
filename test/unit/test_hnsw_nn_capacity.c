#include "cuvs_hnsw_nn_capacity.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
int main(void)
{
    CuvsHnswNnCapacity a, b;
    assert(cuvs_hnsw_nn_capacity(1000000, 768, 64, 128, 1, CUVS_HNSW_NN_FP16, &a) == 0);
    assert(cuvs_hnsw_nn_capacity(1000000, 768, 64, 128, 1, CUVS_HNSW_NN_FP32, &b) == 0);
    assert(a.build_host == UINT64_C(1000000) * 6548);
    assert(a.serialize_host == UINT64_C(1000000) * 21248);
    assert(a.sort_gpu == UINT64_C(1000000) * (4 * 768 + 4 * 128));
    assert(a.build_gpu == UINT64_C(1000000) * (2 * 768 + 280) + UINT64_C(100000) * 4 * 768);
    assert(b.build_gpu == a.build_gpu + UINT64_C(1000000) * 2 * 768);
    assert(b.gpu_peak >= b.build_gpu && b.gpu_peak >= b.sort_gpu);
    assert(b.host_peak >= b.build_host && b.host_peak >= b.serialize_host);
    assert(cuvs_hnsw_nn_capacity(0, 768, 64, 128, 1, CUVS_HNSW_NN_FP32, &a) != 0);
    assert(cuvs_hnsw_nn_capacity(INT32_MAX - 1, 768, 64, 128, 1, CUVS_HNSW_NN_FP32, &a) != 0);
    assert(cuvs_hnsw_nn_capacity(1000, 768, 64, 1025, 1, CUVS_HNSW_NN_FP32, &a) != 0);
    assert(cuvs_hnsw_nn_capacity(1000, 768, 64, 128, 1, 99, &a) != 0);
    uint64_t checked;
    assert(cuvs_hnsw_nn_mul(UINT64_MAX, 2, &checked) != 0);
    assert(cuvs_hnsw_nn_add(UINT64_MAX, 1, &checked) != 0);
    puts("NN resident capacity phase accounting: PASS");
    return 0;
}
