#ifndef CUVS_HNSW_NN_CENTER_H
#define CUVS_HNSW_NN_CENTER_H

#include "cuvs_ipc.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* Explicit nn_descent + L2 builds the NN-descent graph from a column-centered
 * copy of the input.
 *
 * Why: the pinned SDK's NN-descent computes L2 as ||a||^2 + ||b||^2 - 2*a.b in
 * float32 (nn_descent.cuh calculate_metric), so a large common offset cancels
 * away the neighbor order. L2 distances are translation invariant, so the
 * exact neighbor sets of the centered copy equal those of the input.
 *
 * Only the graph input is centered. The stored vectors, HNSW serialization
 * and all search distances use the original rows. With mode='hnsw', SDK
 * patch 0005 centers each upper level's NN-descent input the same way.
 * IP and cosine are not translation invariant and are never centered. */
static inline int cuvs_hnsw_nn_centers(uint32_t metric)
{
    return metric == CUVS_METRIC_L2;
}

/* Column mean accumulated in float64; out[i] = (float)((double)x[i] - mean[j]).
 * out may not alias data. Returns 0, or -1 on bad arguments, allocation
 * failure or a nonfinite value. mean_sq (optional) receives ||mean||^2. */
static inline int
cuvs_hnsw_nn_center(const float *data, uint64_t rows, uint32_t dim,
                    float *out, double *mean_sq)
{
    double *mean;
    double norm = 0.0;
    if (!data || !out || !rows || !dim || rows > SIZE_MAX / sizeof(float) / dim)
        return -1;
    mean = (double *)calloc(dim, sizeof(double));
    if (!mean) return -1;
    for (uint64_t i = 0; i < rows; ++i)
        for (uint32_t j = 0; j < dim; ++j)
            mean[j] += (double)data[(size_t)i * dim + j];
    for (uint32_t j = 0; j < dim; ++j)
    {
        mean[j] /= (double)rows;
        if (!isfinite(mean[j])) { free(mean); return -1; }
        norm += mean[j] * mean[j];
    }
    for (uint64_t i = 0; i < rows; ++i)
        for (uint32_t j = 0; j < dim; ++j)
        {
            size_t k = (size_t)i * dim + j;
            out[k] = (float)((double)data[k] - mean[j]);
        }
    free(mean);
    if (mean_sq) *mean_sq = norm;
    return 0;
}
#endif
