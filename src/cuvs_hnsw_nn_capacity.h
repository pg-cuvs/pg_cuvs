#ifndef CUVS_HNSW_NN_CAPACITY_H
#define CUVS_HNSW_NN_CAPACITY_H

#include "cuvs_hnsw_nn_precision.h"
#include <limits.h>

/* Estimate, not a hard allocation limit: unquantified SDK allocator and
 * hierarchy/serializer scratch remain outside a proven peak bound. */
typedef struct CuvsHnswNnCapacity {
    uint64_t raw_bytes, build_host, serialize_host, build_gpu, sort_gpu;
    uint64_t host_peak, gpu_peak;
} CuvsHnswNnCapacity;

static inline int cuvs_hnsw_nn_mul(uint64_t a, uint64_t b, uint64_t *out)
{
    if (b && a > UINT64_MAX / b) return -1;
    *out = a * b;
    return 0;
}
static inline int cuvs_hnsw_nn_add(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a > UINT64_MAX - b) return -1;
    *out = a + b;
    return 0;
}
static inline uint64_t cuvs_hnsw_nn_extended(uint64_t degree)
{
    uint64_t x = degree <= 32 ? degree : degree * 13 / 10;
    return (x + 31) / 32 * 32;
}
static inline int
cuvs_hnsw_nn_capacity(uint64_t n, uint32_t dim, uint32_t degree, uint32_t intermediate,
                      int hierarchy, uint32_t dtype, CuvsHnswNnCapacity *out)
{
    uint64_t e, q, internal, row, build_row, serial_row, staging, peak;
    const uint64_t reserve = UINT64_C(512) << 20;
    if (!out || n < CUVS_HNSW_MIN_ELEMENTS || n >= INT32_MAX - 1 || !dim || dim > 65535
        || degree < 2 || (degree & 1) || intermediate < degree || intermediate > 1024
        || intermediate >= n || (hierarchy != 0 && hierarchy != 1)
        || (dtype != CUVS_HNSW_NN_FP16 && dtype != CUVS_HNSW_NN_FP32)) return -1;
    internal = (uint64_t)intermediate * 3 / 2;
    if (internal >= n) internal = n - 1;
    e = cuvs_hnsw_nn_extended(intermediate);
    q = cuvs_hnsw_nn_extended(internal);
    row = (uint64_t)dim * 4;
    /* File mapping + external kNN graph + internal graph/dists + sample,
     * pinned arrays, list sizes, bloom, and random-sequence allowance. */
    build_row = row + (uint64_t)intermediate * 4 + 8 * e + 916 + 2 * q;
    /* Existing host serializer/hierarchy inputs: graph, records, dataset,
     * locks/maps and hierarchy scratch. Count mapped input pages. */
    serial_row = row * (hierarchy ? 6 : 4)
        + ((uint64_t)intermediate + 3 * (uint64_t)degree) * 8 + 256;
    if (cuvs_hnsw_nn_mul(n, row, &out->raw_bytes)
        || cuvs_hnsw_nn_mul(n, build_row, &out->build_host)
        || cuvs_hnsw_nn_mul(n, serial_row, &out->serialize_host)
        || cuvs_hnsw_nn_mul(n, (uint64_t)dim * (dtype == CUVS_HNSW_NN_FP32 ? 4 : 2) + 280,
                            &out->build_gpu)
        || cuvs_hnsw_nn_mul(n < 100000 ? n : 100000, row, &staging)
        || cuvs_hnsw_nn_add(out->build_gpu, staging, &out->build_gpu)
        || cuvs_hnsw_nn_mul(n, row + (uint64_t)intermediate * 4, &out->sort_gpu)) return -1;
    peak = out->build_host > out->serialize_host ? out->build_host : out->serialize_host;
    if (cuvs_hnsw_nn_add(peak, reserve, &out->host_peak)) return -1;
    peak = out->build_gpu > out->sort_gpu ? out->build_gpu : out->sort_gpu;
    return cuvs_hnsw_nn_add(peak, reserve, &out->gpu_peak);
}
#endif
