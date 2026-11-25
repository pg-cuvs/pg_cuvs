#pragma once

#include <cuda_runtime.h>

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "cuvs_ipc.h"

namespace cuvs_page_overlay_gpu_detail {

constexpr unsigned warp_width = 32;
constexpr unsigned rows_per_block = 4;
constexpr unsigned threads_per_block = warp_width * rows_per_block;

static __device__ __forceinline__ float needs_cpu_score()
{
    return __uint_as_float(0x7fc00000u);
}

static __device__ __forceinline__ bool finite_double(double value)
{
    constexpr uint64_t exponent = UINT64_C(0x7ff0000000000000);
    return (static_cast<uint64_t>(__double_as_longlong(value)) & exponent) != exponent;
}

static __device__ __forceinline__ double widen(float value)
{
    double result;
    // Omit .ftz explicitly to preserve binary32 subnormal inputs.
    asm("cvt.f64.f32 %0, %1;" : "=d"(result) : "f"(value));
    return result;
}

static __device__ __forceinline__ float narrow_rn(double value)
{
    float result;
    // Keep subnormal results and the sign on underflow to zero.
    asm("cvt.rn.f32.f64 %0, %1;" : "=f"(result) : "d"(value));
    return result;
}

static __device__ __forceinline__ double warp_sum(double value)
{
    for (unsigned offset = warp_width / 2; offset; offset /= 2)
        value = __dadd_rn(value, __shfl_down_sync(0xffffffffu, value, offset));
    return value;
}

static __device__ __forceinline__ float round_interval(double score, double error)
{
    const double lower = __dsub_rd(score, error);
    const double upper = __dadd_ru(score, error);
    // Leave any possible overflow of the CPU float cast to exact scoring.
    if (lower < -static_cast<double>(FLT_MAX) || upper > static_cast<double>(FLT_MAX))
        return needs_cpu_score();

    const float rounded = narrow_rn(lower);
    const uint32_t bits = __float_as_uint(rounded);

    if (bits != __float_as_uint(narrow_rn(upper)) || (bits & 0x7f800000u) == 0x7f800000u)
        return needs_cpu_score();
    // If zero is still possible, a negative zero cast cannot certify its sign.
    if (bits == 0x80000000u && upper >= 0.0)
        return needs_cpu_score();
    return rounded;
}

template <uint32_t metric>
static __device__ __forceinline__ float certify(double sum, double aux, double vnorm, uint32_t dim)
{
    // Nonfinite coordinates propagate into L2, the IP absolute mass, or a norm.
    // Check before the cosine zero-norm shortcut.
    if (!finite_double(sum) || !finite_double(aux) || !finite_double(vnorm))
        return needs_cpu_score();

    /*
     * As in the CPU page_score_distance_bound, use r = (dim + 8) * 2u,
     * where u is the binary64 unit roundoff. Float products are exact in
     * double. L2 also rounds subtraction and squaring (or the fused sum).
     * Each term meets at most dim + 8 rounding factors on either path,
     * including the five warp reduction steps on the GPU path. Nonzero
     * accumulations of finite binary32 inputs and nonzero products of
     * their norms stay in the binary64 normal range. The uint32 dimension
     * bounds r below 1e-6, so the usual gamma bound is at most r.
     */
    const double r = (static_cast<double>(dim) + 8.0) * DBL_EPSILON;
    if constexpr (metric == CUVS_METRIC_COSINE)
    {
        // A finite norm can be zero only when all its coordinates are zero.
        if (aux == 0.0 || vnorm == 0.0)
            return 1.0f;

        /*
         * Let D be the ideal norm denominator. Cauchy-Schwarz bounds the
         * dot and its absolute term mass by D. Each computed denominator
         * is within 2r relative of D. Dot, denominator, division and final
         * subtraction together give less than 5r absolute error per path.
         */
        const double denominator = __dsqrt_rn(__dmul_rn(aux, vnorm));
        const double score = __dsub_rn(1.0, __ddiv_rn(sum, denominator));
        return round_interval(score, 16.0 * r);
    }
    else
    {
        // For IP the absolute product mass bounds cancellation error.
        const double mass = metric == CUVS_METRIC_IP ? aux : sum;
        // The ordered sum starts at +0, so zero-only terms return +0.
        if (mass == 0.0)
            return 0.0f;

        // Both paths are within r of the ideal, scaled by ideal mass.
        // The measured mass is at least (1 - r) times ideal mass.
        return round_interval(sum, __dmul_ru(4.0 * r, mass));
    }
}

template <uint32_t metric>
static __global__ void score_rows(const float *query, const float *rows, uint32_t dim,
                                  uint32_t count, float *__restrict__ scores)
{
    const unsigned lane = threadIdx.x % warp_width;
    const uint32_t row_id = blockIdx.x * rows_per_block + threadIdx.x / warp_width;
    if (row_id >= count)
        return; // Entire warps exit, leaving full participation in each live warp.

    const float *row = rows + static_cast<size_t>(row_id) * dim;
    double sum = 0.0, aux = 0.0, vnorm = 0.0;

    // Adjacent lanes read adjacent coordinates, including for a partial tile.
    for (size_t col = lane; col < dim; col += warp_width)
    {
        const double q = widen(query[col]);
        const double v = widen(row[col]);
        if constexpr (metric == CUVS_METRIC_L2)
        {
            const double difference = __dsub_rn(q, v);
            sum = __fma_rn(difference, difference, sum);
        }
        else if constexpr (metric == CUVS_METRIC_IP)
        {
            const double product = __dmul_rn(q, v);
            sum = __dadd_rn(sum, product);
            aux = __dadd_rn(aux, fabs(product));
        }
        else
        {
            sum = __fma_rn(q, v, sum);
            aux = __fma_rn(q, q, aux);
            vnorm = __fma_rn(v, v, vnorm);
        }
    }
    sum = warp_sum(sum);
    if constexpr (metric != CUVS_METRIC_L2)
        aux = warp_sum(aux);
    if constexpr (metric == CUVS_METRIC_COSINE)
        vnorm = warp_sum(vnorm);

    if (lane == 0)
        scores[row_id] = certify<metric>(sum, aux, vnorm, dim);
}

} // namespace cuvs_page_overlay_gpu_detail

/*
 * Enqueue one score per packed VECTOR row. A count of zero succeeds without
 * inspecting the other arguments. Otherwise dim must be positive and metric
 * must be a CUVS_METRIC_* value supported below.
 *
 * The caller supplies valid aligned device spans of dim, count * dim, and count
 * floats and keeps them alive through completion. Scores must be disjoint from
 * the input spans. Nonfinite inputs, possible float overflow, and ambiguous
 * endpoint casts produce a quiet NaN for CPU exact scoring of that row. The
 * ordered CPU reference uses round-to-nearest-even with gradual underflow.
 * The return value reports launch status. Completion is the caller's concern.
 */
static inline cudaError_t
cuvs_page_overlay_launch_scores(cudaStream_t stream, const float *query, const float *rows,
                                uint32_t dim, uint32_t count, uint32_t metric, float *scores)
{
    using namespace cuvs_page_overlay_gpu_detail;

    if (count == 0)
        return cudaSuccess;
    if (dim == 0 || !query || !rows || !scores || count > (SIZE_MAX / sizeof(float)) / dim)
        return cudaErrorInvalidValue;

    const unsigned blocks = (count - 1) / rows_per_block + 1;
    switch (metric)
    {
        case CUVS_METRIC_L2:
            score_rows<CUVS_METRIC_L2><<<blocks, threads_per_block, 0, stream>>>(query, rows, dim, count, scores);
            break;
        case CUVS_METRIC_IP:
            score_rows<CUVS_METRIC_IP><<<blocks, threads_per_block, 0, stream>>>(query, rows, dim, count, scores);
            break;
        case CUVS_METRIC_COSINE:
            score_rows<CUVS_METRIC_COSINE><<<blocks, threads_per_block, 0, stream>>>(query, rows, dim, count, scores);
            break;
        default:
            return cudaErrorInvalidValue;
    }
    return cudaGetLastError();
}
