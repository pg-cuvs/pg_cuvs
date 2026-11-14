#include "cuvs_hnsw_nn_precision.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    CuvsHnswNnPrecision result;
    float safe[17] = {1.0f};
    assert(cuvs_hnsw_nn_precision(safe, 1, 17, CUVS_METRIC_L2, &result) == 0);
    assert(result.dtype == CUVS_HNSW_NN_FP16 && result.max_abs == 1.0);
    assert(cuvs_hnsw_nn_precision(safe, 1, 16, CUVS_METRIC_IP, &result) == 0);
    assert(result.dtype == CUVS_HNSW_NN_FP32);
    safe[0] = 65505.0f;
    assert(cuvs_hnsw_nn_precision(safe, 1, 17, CUVS_METRIC_L2, &result) == 0);
    assert(result.dtype == CUVS_HNSW_NN_FP32 && result.max_abs == 65505.0);
    safe[0] = 65504.0f;
    assert(cuvs_hnsw_nn_precision(safe, 1, 17, CUVS_METRIC_L2, &result) == 0);
    assert(result.dtype == CUVS_HNSW_NN_FP16);
    /* A half-representable input may have squared distances > 65504;
     * this is not an element conversion overflow. */
    assert(result.max_sq_norm > 65504.0);
    safe[0] = INFINITY;
    assert(cuvs_hnsw_nn_precision(safe, 1, 17, CUVS_METRIC_L2, &result) != 0);
    safe[0] = FLT_MAX;
    assert(cuvs_hnsw_nn_precision(safe, 1, 17, CUVS_METRIC_L2, &result) != 0);
    safe[0] = NAN;
    assert(cuvs_hnsw_nn_precision(safe, 1, 17, CUVS_METRIC_L2, &result) != 0);
    assert(cuvs_hnsw_nn_precision(safe, UINT64_MAX, 17, CUVS_METRIC_L2, &result) != 0);
    assert(cuvs_hnsw_nn_precision(safe, 0, 17, CUVS_METRIC_L2, &result) != 0);
    assert(cuvs_hnsw_nn_precision(safe, 1, 17, UINT32_MAX, &result) != 0);
    puts("NN precision input guard: PASS");
    return 0;
}
