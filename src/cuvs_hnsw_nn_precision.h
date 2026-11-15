#ifndef CUVS_HNSW_NN_PRECISION_H
#define CUVS_HNSW_NN_PRECISION_H

#include "cuvs_ipc.h"
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* Candidate precision for resident NN, computed from the actual float spool.
 * This does not cover sort/optimizer numerical behavior or prove GPU success. */
#define CUVS_HNSW_NN_FP16 1u
#define CUVS_HNSW_NN_FP32 2u

/* Nonzero results of cuvs_hnsw_nn_precision. */
#define CUVS_HNSW_NN_PRECISION_INVALID (-1)       /* nonfinite, FP32 range, bad args */
#define CUVS_HNSW_NN_PRECISION_CANCELLATION (-2)  /* L2 norm-expansion cancellation */

/* L2 cancellation bound (r87 offset diagnosis). The pinned SDK's NN-descent
 * computes L2 as ||a||^2 + ||b||^2 - 2*a.b in float32 on both the FP16 and
 * FP32 paths (nn_descent.cuh calculate_metric). Its rounding error scales
 * with the squared norm, while neighbor distances scale with the spread V
 * (mean squared distance to the column mean). On the frozen corpus rows plus
 * a common offset, the k=64 graph overlap with exact neighbors was
 * 0.9997/0.998/0.979/0.83/0.18/0.012 at offset 1/3/10/30/100/1000, i.e. mean
 * squared norm / V = 160/1440/15965/1.4e5/1.6e6/1.6e8. Refuse when the ratio
 * exceeds 4096 (relative float32 error about 1e-3 of V). */
#define CUVS_HNSW_NN_L2_NORM_TO_SPREAD_MAX 4096.0

typedef struct CuvsHnswNnPrecision {
    uint32_t dtype;
    double max_abs, max_sq_norm;
    double mean_sq_norm, spread;   /* mean ||x||^2 and mean ||x - mean||^2 */
} CuvsHnswNnPrecision;

static inline int
cuvs_hnsw_nn_precision(const float *data, uint64_t rows, uint32_t dim,
                       uint32_t metric, CuvsHnswNnPrecision *out)
{
    double max_abs = 0.0, max_norm = 0.0, sum_norm = 0.0, mean_norm = 0.0;
    double *column;
    if (!data || !out || !rows || !dim || metric > CUVS_METRIC_IP
        || rows > SIZE_MAX / sizeof(float) / dim)
        return CUVS_HNSW_NN_PRECISION_INVALID;
    column = (double *)calloc(dim, sizeof(double));
    if (!column) return CUVS_HNSW_NN_PRECISION_INVALID;
    for (uint64_t i = 0; i < rows; ++i)
    {
        double norm = 0.0;
        for (uint32_t j = 0; j < dim; ++j)
        {
            double v = data[(size_t)i * dim + j];
            double a = fabs(v);
            if (!isfinite(v)) { free(column); return CUVS_HNSW_NN_PRECISION_INVALID; }
            if (a > max_abs) max_abs = a;
            norm += v * v;
            column[j] += v;
        }
        if (!isfinite(norm)) { free(column); return CUVS_HNSW_NN_PRECISION_INVALID; }
        if (norm > max_norm) max_norm = norm;
        sum_norm += norm;
    }
    for (uint32_t j = 0; j < dim; ++j)
    {
        double m = column[j] / (double)rows;
        mean_norm += m * m;
    }
    free(column);
    out->mean_sq_norm = sum_norm / (double)rows;
    out->spread = out->mean_sq_norm - mean_norm;
    if (out->spread < 0.0) out->spread = 0.0;
    /* L2 expanded distance contains two norms and -2*dot. Leave a factor
     * two margin beyond their mathematical absolute-sum bound. IP uses dot. */
    if (max_norm >= (double)FLT_MAX / (metric == CUVS_METRIC_L2 ? 8.0 : 2.0))
        return CUVS_HNSW_NN_PRECISION_INVALID;
    /* Identical rows (spread 0) have no neighbor order to lose. */
    if (metric == CUVS_METRIC_L2 && out->spread > 0.0
        && out->mean_sq_norm > CUVS_HNSW_NN_L2_NORM_TO_SPREAD_MAX * out->spread)
        return CUVS_HNSW_NN_PRECISION_CANCELLATION;
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
