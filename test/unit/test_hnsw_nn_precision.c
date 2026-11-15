#include "cuvs_hnsw_nn_precision.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

/* FP16-cancellation emulation inputs: dim 128, n 4000, N(0, sd) + offset.
 * With FP16 dot + FP32 norm a CPU emulation measured recall@10 1.000/0.899/0.017/
 * 0.001 for offset 0/100/1000/5000; every offset >= 100 must select FP32. */
static uint64_t rng_state = 1;
static double uniform01(void)
{
    rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return ((rng_state >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}
#define REFUSED_CANCELLATION 99u
static uint32_t dtype_for(double offset, double sd, uint32_t metric)
{
    enum { N = 4000, D = 128 };
    static float x[N * D];
    CuvsHnswNnPrecision r;
    int rc;
    rng_state = 1;
    for (int i = 0; i < N * D; i++)
        x[i] = (float)(offset + sd * sqrt(-2.0 * log(uniform01())) * cos(6.283185307179586 * uniform01()));
    rc = cuvs_hnsw_nn_precision(x, N, D, metric, &r);
    if (rc == CUVS_HNSW_NN_PRECISION_CANCELLATION) return REFUSED_CANCELLATION;
    assert(rc == 0);
    assert(r.max_abs < 65504.0);  /* the old element-only rule chose FP16 for all of these */
    return r.dtype;
}
static void test_review_offsets(void)
{
    /* Never FP16 for these. This exercises cuvs_hnsw_nn_precision, i.e.
     * UNCENTERED input, where offsets whose mean squared norm exceeds 4096 x
     * spread are refused. The product centers L2 input and does not take this
     * path (see test_hnsw_nn_center.c). */
    assert(dtype_for(0.0, 1.0, CUVS_METRIC_L2) == CUVS_HNSW_NN_FP16);
    assert(dtype_for(100.0, 5.0, CUVS_METRIC_L2) == CUVS_HNSW_NN_FP32);         /* ratio ~400 */
    assert(dtype_for(1000.0, 5.0, CUVS_METRIC_L2) == REFUSED_CANCELLATION);      /* ~4e4 */
    assert(dtype_for(5000.0, 5.0, CUVS_METRIC_L2) == REFUSED_CANCELLATION);
    assert(dtype_for(100.0, 5.0, CUVS_METRIC_IP) == CUVS_HNSW_NN_FP32);
    /* unit-scale 768-d rows (a 768-d embedding corpus with max_abs 0.79) stay FP16 */
    {
        static float u[64 * 768];
        CuvsHnswNnPrecision r;
        for (int i = 0; i < 64 * 768; i++) u[i] = (float)((uniform01() - 0.5) * 0.072);
        assert(cuvs_hnsw_nn_precision(u, 64, 768, CUVS_METRIC_L2, &r) == 0 && r.dtype == CUVS_HNSW_NN_FP16);
    }
}

/* Offset diagnosis: 768-d unit-scale rows (spread ~4.8, like a real 768-d
 * embedding corpus) plus a common offset. On GPU, recall@10 at offset
 * 1000 was 0.40/0.79/0.90 even with FP32; the CPU emulation of the SDK's
 * float32 norm expansion gave k=64 graph overlap 0.998 at offset 3, 0.979 at
 * 10 and 0.012 at 1000. The uncentered L2 rule refuses from offset 10; IP is not affected. */
static int offset_rc(double offset, uint32_t metric, int outlier)
{
    enum { N = 256, D = 768 };
    static float x[N * D];
    CuvsHnswNnPrecision r;
    rng_state = 7;
    for (int i = 0; i < N * D; i++) x[i] = (float)(offset + (uniform01() - 0.5) * 0.276);
    if (outlier) x[5 * D] = 65505.0f;  /* one large row: spread grows with it */
    return cuvs_hnsw_nn_precision(x, N, D, metric, &r);
}
static void test_offset_cancellation(void)
{
    assert(offset_rc(0.0, CUVS_METRIC_L2, 0) == 0);
    assert(offset_rc(3.0, CUVS_METRIC_L2, 0) == 0);
    assert(offset_rc(10.0, CUVS_METRIC_L2, 0) == CUVS_HNSW_NN_PRECISION_CANCELLATION);
    assert(offset_rc(1000.0, CUVS_METRIC_L2, 0) == CUVS_HNSW_NN_PRECISION_CANCELLATION);
    assert(offset_rc(1000.0, CUVS_METRIC_IP, 0) == 0);
    assert(offset_rc(0.0, CUVS_METRIC_L2, 1) == 0);  /* one row with an element > 65504 built normally on GPU */
    {
        float same[4 * 17];
        CuvsHnswNnPrecision r;
        for (int i = 0; i < 4 * 17; i++) same[i] = 1000.0f;  /* spread 0: nothing to order */
        assert(cuvs_hnsw_nn_precision(same, 4, 17, CUVS_METRIC_L2, &r) == 0);
    }
}

/* The FP32 distance-range margins are 8x (L2) and 2x
 * (IP/cosine) below FLT_MAX; metrics above IP are rejected. */
static void test_distance_margins(void)
{
    CuvsHnswNnPrecision r;
    float v[17] = {0};
    v[0] = (float)sqrt((double)FLT_MAX / 4.0);   /* norm = FLT_MAX/4 */
    assert(cuvs_hnsw_nn_precision(v, 1, 17, CUVS_METRIC_L2, &r) != 0);
    assert(cuvs_hnsw_nn_precision(v, 1, 17, CUVS_METRIC_IP, &r) == 0);
    v[0] = (float)sqrt((double)FLT_MAX / 16.0);  /* norm = FLT_MAX/16 */
    assert(cuvs_hnsw_nn_precision(v, 1, 17, CUVS_METRIC_L2, &r) == 0);
    v[0] = (float)sqrt((double)FLT_MAX * 0.75);  /* norm = 0.75 FLT_MAX */
    assert(cuvs_hnsw_nn_precision(v, 1, 17, CUVS_METRIC_IP, &r) != 0);
    v[0] = 1.0f;
    assert(cuvs_hnsw_nn_precision(v, 1, 17, CUVS_METRIC_IP + 1, &r) != 0);
}

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
    /* Every element fits half, but the expanded-L2 cancellation bound does
     * not: FP32, matching cuvs_hnsw_ivfpq_needs_fp32. */
    safe[0] = 65504.0f;
    assert(cuvs_hnsw_nn_precision(safe, 1, 17, CUVS_METRIC_L2, &result) == 0);
    assert(result.dtype == CUVS_HNSW_NN_FP32 && result.max_sq_norm > 65504.0);
    /* Bound edges: L2 uses 4*max_sq_norm < 65504, IP uses max_sq_norm < 65504. */
    safe[0] = 127.0f;  /* 16129 * 4 = 64516 */
    assert(cuvs_hnsw_nn_precision(safe, 1, 17, CUVS_METRIC_L2, &result) == 0 && result.dtype == CUVS_HNSW_NN_FP16);
    safe[0] = 128.0f;  /* 16384 * 4 = 65536 */
    assert(cuvs_hnsw_nn_precision(safe, 1, 17, CUVS_METRIC_L2, &result) == 0 && result.dtype == CUVS_HNSW_NN_FP32);
    assert(cuvs_hnsw_nn_precision(safe, 1, 17, CUVS_METRIC_IP, &result) == 0 && result.dtype == CUVS_HNSW_NN_FP16);
    safe[0] = 256.0f;  /* 65536 */
    assert(cuvs_hnsw_nn_precision(safe, 1, 17, CUVS_METRIC_IP, &result) == 0 && result.dtype == CUVS_HNSW_NN_FP32);
    safe[0] = 1.0f;
    safe[0] = INFINITY;
    assert(cuvs_hnsw_nn_precision(safe, 1, 17, CUVS_METRIC_L2, &result) != 0);
    safe[0] = FLT_MAX;
    assert(cuvs_hnsw_nn_precision(safe, 1, 17, CUVS_METRIC_L2, &result) != 0);
    safe[0] = NAN;
    assert(cuvs_hnsw_nn_precision(safe, 1, 17, CUVS_METRIC_L2, &result) != 0);
    assert(cuvs_hnsw_nn_precision(safe, UINT64_MAX, 17, CUVS_METRIC_L2, &result) != 0);
    assert(cuvs_hnsw_nn_precision(safe, 0, 17, CUVS_METRIC_L2, &result) != 0);
    assert(cuvs_hnsw_nn_precision(safe, 1, 17, UINT32_MAX, &result) != 0);
    test_review_offsets();
    test_distance_margins();
    test_offset_cancellation();
    puts("NN precision input guard: PASS");
    return 0;
}
