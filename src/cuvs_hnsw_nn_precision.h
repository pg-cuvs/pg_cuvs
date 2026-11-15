#ifndef CUVS_HNSW_NN_PRECISION_H
#define CUVS_HNSW_NN_PRECISION_H

#include "cuvs_ipc.h"
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* Candidate precision for resident NN, computed from the actual float spool.
 * This does not cover sort/optimizer numerical behavior or prove GPU success. */
#define CUVS_HNSW_NN_FP16 1u
#define CUVS_HNSW_NN_FP32 2u

typedef struct CuvsHnswNnPrecision {
    uint32_t dtype;
    double max_abs, max_sq_norm;
} CuvsHnswNnPrecision;

static inline int
cuvs_hnsw_nn_precision(const float *data, uint64_t rows, uint32_t dim,
                       uint32_t metric, CuvsHnswNnPrecision *out)
{
    double max_abs = 0.0, max_norm = 0.0;
    if (!data || !out || !rows || !dim || metric > CUVS_METRIC_IP
        || rows > SIZE_MAX / sizeof(float) / dim)
        return -1;
    for (uint64_t i = 0; i < rows; ++i)
    {
        double norm = 0.0;
        for (uint32_t j = 0; j < dim; ++j)
        {
            double v = data[(size_t)i * dim + j];
            double a = fabs(v);
            if (!isfinite(v)) return -1;
            if (a > max_abs) max_abs = a;
            norm += v * v;
        }
        if (!isfinite(norm)) return -1;
        if (norm > max_norm) max_norm = norm;
    }
    /* L2 expanded distance contains two norms and -2*dot. Leave a factor
     * two margin beyond their mathematical absolute-sum bound. IP uses dot. */
    if (max_norm >= (double)FLT_MAX / (metric == CUVS_METRIC_L2 ? 8.0 : 2.0))
        return -1;
    /* The pinned SDK's FP16 path takes norms from the FP32 input but the dot
     * product from half-converted data (nn_descent.cuh preprocess_data_kernel
     * and the norm_a + norm_b - 2*dot epilogue). With a large common offset the
     * dot's rounding error exceeds the true distance differences and neighbors
     * collapse even though every element fits half (nn-diff-review-r87 D2).
     * Use the same bound as cuvs_hnsw_ivfpq_needs_fp32: FP16 only when
     * 4*max_sq_norm (L2) or max_sq_norm (IP/cosine) stays below 65504. */
    double half_bound = metric == CUVS_METRIC_L2 ? 4.0 * max_norm : max_norm;
    out->dtype = dim <= 16 || max_abs > 65504.0 || !(half_bound < 65504.0)
        ? CUVS_HNSW_NN_FP32 : CUVS_HNSW_NN_FP16;
    out->max_abs = max_abs;
    out->max_sq_norm = max_norm;
    return 0;
}
#endif
