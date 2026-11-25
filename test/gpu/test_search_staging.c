#include "cuvs_wrapper.h"
#include "cuvs_ipc.h"
#include "cuvs_lw_policy.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { ROWS = 512, DIM = 16, K = 4, QUERY_ROW = 17, SECOND_ROW = 319 };

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "test_search_staging: failed at line %d: %s\n", __LINE__, #expr); \
        return EXIT_FAILURE; \
    } \
} while (0)

static int
self_hit(const CuvsSearchResult *results, int row)
{
    return results[0].item_id == row && fabsf(results[0].distance) < 0.01f;
}

int
main(void)
{
    float corpus[ROWS][DIM], queries[2][DIM];
    CuvsSearchResult ranked[2 * K];
    uint32_t exclude[ROWS / 32] = {0};
    uint32_t state = 12345;
    CuvsIvfPqIndex pq = NULL;

    CHECK(cuvs_gpu_available());
    for (int row = 0; row < ROWS; ++row)
        for (int col = 0; col < DIM; ++col) {
            state = state * UINT32_C(1664525) + UINT32_C(1013904223);
            corpus[row][col] = (float)(state >> 22) / 1024.0f;
        }
    for (int col = 0; col < DIM; ++col) {
        queries[0][col] = corpus[QUERY_ROW][col];
        queries[1][col] = corpus[SECOND_ROW][col];
    }
    /* Exclude all even rows. */
    for (size_t i = 0; i < sizeof(exclude) / sizeof(exclude[0]); ++i)
        exclude[i] = UINT32_C(0x55555555);

    cuvs_warmup_device(0);
    CHECK(cuvs_brute_force_search(&corpus[0][0], queries[0], ROWS, DIM, K,
                                 CUVS_METRIC_L2, ranked, 0) == 0);
    CHECK(self_hit(ranked, QUERY_ROW));

    for (uint32_t precision = 0; precision <= 1; ++precision) {
        CuvsBfIndex bf = cuvs_bf_build(&corpus[0][0], ROWS, DIM, CUVS_METRIC_L2, precision, 0);
        CHECK(bf != NULL);
        CHECK(cuvs_bf_search(bf, queries[0], DIM, K, ranked, 0) == 0);
        CHECK(self_hit(ranked, QUERY_ROW));
        CHECK(cuvs_bf_search_batch(bf, &queries[0][0], 2, DIM, K, ranked, 0) == 0);
        CHECK(self_hit(ranked, QUERY_ROW) && self_hit(ranked + K, SECOND_ROW));
        CHECK(cuvs_bf_search_filtered(bf, queries[0], DIM, K, exclude, ROWS, ranked, 0) == 0);
        CHECK(self_hit(ranked, QUERY_ROW));
        for (int i = 0; i < K; ++i)
            CHECK(ranked[i].item_id >= 0 && ranked[i].item_id < ROWS && ranked[i].item_id % 2 == 1);
        cuvs_bf_free(bf, 0);
    }

    CuvsCagraIndex cagra = cuvs_cagra_build(&corpus[0][0], ROWS, DIM, CUVS_METRIC_L2,
                                          32, 64, CUVS_CAGRA_BUILD_NN_DESCENT, 0,
                                          CUVS_LW_OVERFLOW_ERROR, NULL);
    CHECK(cagra != NULL);
    CHECK(cuvs_cagra_search(cagra, queries[0], DIM, K, ranked, 0, 128, 4) == 0);
    CHECK(self_hit(ranked, QUERY_ROW));
    CHECK(cuvs_cagra_search_batch(cagra, &queries[0][0], 2, DIM, K, ranked, 0, 128, 4) == 0);
    CHECK(self_hit(ranked, QUERY_ROW) && self_hit(ranked + K, SECOND_ROW));
    CHECK(cuvs_cagra_search_filtered(cagra, queries[0], DIM, K, exclude, ROWS, ranked, 0, 128, 4) == 0);
    CHECK(self_hit(ranked, QUERY_ROW));
    for (int i = 0; i < K; ++i)
        CHECK(ranked[i].item_id >= 0 && ranked[i].item_id < ROWS && ranked[i].item_id % 2 == 1);
    cuvs_cagra_free(cagra, 0);

    CHECK(cuvs_ivfpq_build(&corpus[0][0], ROWS, DIM, CUVS_METRIC_L2, 4, 4, 8, 0, &pq) == 0);
    CHECK(pq != NULL);
    CHECK(cuvs_ivfpq_search(pq, queries[0], DIM, K, 4, ranked, 0) == 0);
    for (int i = 0; i < K; ++i)
        CHECK(ranked[i].item_id >= 0 && ranked[i].item_id < ROWS && isfinite(ranked[i].distance));
    cuvs_ivfpq_free(pq, 0);

    puts("test_search_staging: all GPU search routes passed");
    return EXIT_SUCCESS;
}
