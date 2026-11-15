/* Explicit nn_descent + L2
 * builds the NN-descent graph from a column-centered copy. CPU only. */
#include "cuvs_hnsw_nn_center.h"
#include "cuvs_hnsw_nn_precision.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { N = 256, D = 64, K = 16 };
static float x[N * D], c[N * D];
static uint64_t state = 11;
static double unit(void)
{
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return ((state >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}

/* k nearest by the given float32 formula; expansion mimics the SDK's
 * ||a||^2 + ||b||^2 - 2 a.b with float32 accumulation. */
static void knn(const float *v, int expansion, int out[N][K])
{
    static float norm[N];
    for (int i = 0; i < N; i++)
    {
        float s = 0.0f;
        for (int j = 0; j < D; j++) s += v[i * D + j] * v[i * D + j];
        norm[i] = s;
    }
    for (int i = 0; i < N; i++)
    {
        float d[N];
        for (int r = 0; r < N; r++)
        {
            float s = 0.0f;
            if (expansion)
            {
                for (int j = 0; j < D; j++) s += v[i * D + j] * v[r * D + j];
                s = norm[i] + norm[r] - 2.0f * s;
                if (s < 0.0f) s = 0.0f;
            }
            else
                for (int j = 0; j < D; j++) { float t = v[i * D + j] - v[r * D + j]; s += t * t; }
            d[r] = r == i ? INFINITY : s;
        }
        for (int k = 0; k < K; k++)
        {
            int best = -1;
            for (int r = 0; r < N; r++)
                if (d[r] != INFINITY + 1.0f && (best < 0 || d[r] < d[best])) best = r;
            out[i][k] = best; d[best] = INFINITY + 1.0f;
        }
    }
}

static double overlap(int a[N][K], int b[N][K])
{
    int hit = 0;
    for (int i = 0; i < N; i++)
        for (int p = 0; p < K; p++)
            for (int q = 0; q < K; q++) hit += a[i][p] == b[i][q];
    return (double)hit / (N * K);
}

int main(void)
{
    static int exact[N][K], raw_exp[N][K], cen_exp[N][K];
    double mean_sq = 0.0;
    /* unit-scale rows plus a common offset of 1000 (the shape of the GPU offset check) */
    for (int i = 0; i < N * D; i++) x[i] = (float)(1000.0 + (unit() - 0.5) * 0.6);
    float before = x[17];

    assert(cuvs_hnsw_nn_centers(CUVS_METRIC_L2));
    assert(!cuvs_hnsw_nn_centers(CUVS_METRIC_IP));
    assert(!cuvs_hnsw_nn_centers(CUVS_METRIC_COSINE));

    assert(cuvs_hnsw_nn_center(x, N, D, c, &mean_sq) == 0);
    assert(x[17] == before);                 /* input untouched */
    assert(mean_sq > 0.99e6 * D && mean_sq < 1.01e6 * D);  /* ||mean||^2 ~ D * 1000^2 */
    for (int j = 0; j < D; j++)
    {
        double s = 0.0;
        for (int i = 0; i < N; i++) s += c[i * D + j];
        assert(fabs(s / N) < 1e-4);          /* centered columns */
    }

    knn(x, 0, exact);                        /* float32 direct difference, offset data */
    knn(x, 1, raw_exp);                      /* SDK formula on the raw offset rows */
    knn(c, 1, cen_exp);                      /* SDK formula on the centered copy */
    printf("k=%d overlap with direct: raw expansion %.3f, centered expansion %.3f\n",
           K, overlap(exact, raw_exp), overlap(exact, cen_exp));
    assert(overlap(exact, raw_exp) < 0.5);   /* the defect is reproduced */
    assert(overlap(exact, cen_exp) > 0.97);  /* and removed by centering */

    {   /* precision: guard skipped only when the graph input is centered;
         * dtype still comes from the original rows (FP32 here). */
        CuvsHnswNnPrecision p;
        assert(cuvs_hnsw_nn_precision_ex(x, N, D, CUVS_METRIC_L2, 0, &p) == CUVS_HNSW_NN_PRECISION_CANCELLATION);
        assert(cuvs_hnsw_nn_precision_ex(x, N, D, CUVS_METRIC_L2, 1, &p) == 0 && p.dtype == CUVS_HNSW_NN_FP32);
        assert(cuvs_hnsw_nn_precision(x, N, D, CUVS_METRIC_L2, &p) == CUVS_HNSW_NN_PRECISION_CANCELLATION);
    }
    {   /* arguments and nonfinite input */
        float bad[2 * 3] = {1, 2, 3, 4, NAN, 6}, out[6];
        assert(cuvs_hnsw_nn_center(bad, 2, 3, out, NULL) != 0);
        assert(cuvs_hnsw_nn_center(NULL, 2, 3, out, NULL) != 0);
        assert(cuvs_hnsw_nn_center(x, 0, D, c, NULL) != 0);
        assert(cuvs_hnsw_nn_center(x, N, 0, c, NULL) != 0);
    }
    puts("NN-descent L2 centering: PASS");
    return 0;
}
