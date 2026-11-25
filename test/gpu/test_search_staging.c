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

static int
cagra_distances_match(const float corpus[ROWS][DIM], const float *query,
                      const CuvsSearchResult *results)
{
    for (int i = 0; i < K; ++i) {
        const int64_t row = results[i].item_id;
        if (row < 0 || row >= ROWS || !isfinite(results[i].distance))
            return 0;
        double distance = 0;
        for (int col = 0; col < DIM; ++col) {
            const double delta = (double)corpus[row][col] - query[col];
            distance += delta * delta;
        }
        if (fabs(distance - results[i].distance) >= 0.01)
            return 0;
    }
    return 1;
}

static void
report(const char *kind, const char *route, const CuvsSearchResult *results, int count)
{
    printf("%s %s", kind, route);
    for (int i = 0; i < count; ++i)
        printf(" %lld:%a", (long long)results[i].item_id, (double)results[i].distance);
    putchar('\n');
}

int
main(int argc, char **argv)
{
    float corpus[ROWS][DIM], queries[2][DIM];
    CuvsSearchResult ranked[2 * K];
    uint32_t exclude[ROWS / 32] = {0};
    uint32_t state = 12345;
    CuvsIvfPqIndex pq = NULL;

    CHECK(argc == 2);
    printf("staging mode %s\n", argv[1]);
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
    report("exact", "brute-force", ranked, K);

    for (uint32_t precision = 0; precision <= 1; ++precision) {
        CuvsBfIndex bf = cuvs_bf_build(&corpus[0][0], ROWS, DIM, CUVS_METRIC_L2, precision, 0);
        CHECK(bf != NULL);
        CHECK(cuvs_bf_search(bf, queries[0], DIM, K, ranked, 0) == 0);
        CHECK(self_hit(ranked, QUERY_ROW));
        report("exact", precision ? "bf-fp16" : "bf-fp32", ranked, K);
        CHECK(cuvs_bf_search_batch(bf, &queries[0][0], 2, DIM, K, ranked, 0) == 0);
        CHECK(self_hit(ranked, QUERY_ROW) && self_hit(ranked + K, SECOND_ROW));
        report("exact", precision ? "bf-batch-fp16" : "bf-batch-fp32", ranked, 2 * K);
        CHECK(cuvs_bf_search_filtered(bf, queries[0], DIM, K, exclude, ROWS, ranked, 0) == 0);
        CHECK(self_hit(ranked, QUERY_ROW));
        for (int i = 0; i < K; ++i)
            CHECK(ranked[i].item_id >= 0 && ranked[i].item_id < ROWS && ranked[i].item_id % 2 == 1);
        report("exact", precision ? "bf-filter-fp16" : "bf-filter-fp32", ranked, K);
        if (precision == 0) {
            enum { BATCH = 32 };
            float repeated[BATCH][DIM];
            CuvsSearchResult *wide = malloc(sizeof(*wide) * BATCH * ROWS);
            CHECK(wide != NULL);
            for (int row = 0; row < BATCH; ++row)
                for (int col = 0; col < DIM; ++col)
                    repeated[row][col] = queries[row % 2][col];
            CHECK(cuvs_bf_search_batch(bf, &repeated[0][0], BATCH, DIM, ROWS, wide, 0) == 0);
            for (int row = 0; row < BATCH; ++row) {
                const CuvsSearchResult *result = wide + row * ROWS;
                CHECK(self_hit(result, row % 2 ? SECOND_ROW : QUERY_ROW));
                report("exact", "bf-wide-self", result, 1);
            }
            free(wide);
        }
        cuvs_bf_free(bf, 0);
    }

    CuvsCagraIndex cagra = cuvs_cagra_build(&corpus[0][0], ROWS, DIM, CUVS_METRIC_L2,
                                          32, 64, CUVS_CAGRA_BUILD_NN_DESCENT, 0,
                                          CUVS_LW_OVERFLOW_ERROR, NULL);
    CHECK(cagra != NULL);
    CHECK(cuvs_cagra_search(cagra, queries[0], DIM, K, ranked, 0, 128, 4) == 0);
    CHECK(self_hit(ranked, QUERY_ROW));
    CHECK(cagra_distances_match(corpus, queries[0], ranked));
    report("anchor", "cagra-self", ranked, 1);
    report("approximate", "cagra", ranked, K);
    CHECK(cuvs_cagra_search_batch(cagra, &queries[0][0], 2, DIM, K, ranked, 0, 128, 4) == 0);
    CHECK(self_hit(ranked, QUERY_ROW) && self_hit(ranked + K, SECOND_ROW));
    CHECK(cagra_distances_match(corpus, queries[0], ranked));
    CHECK(cagra_distances_match(corpus, queries[1], ranked + K));
    report("anchor", "cagra-batch-self-first", ranked, 1);
    report("anchor", "cagra-batch-self-second", ranked + K, 1);
    report("approximate", "cagra-batch", ranked, 2 * K);
    CHECK(cuvs_cagra_search_filtered(cagra, queries[0], DIM, K, exclude, ROWS, ranked, 0, 128, 4) == 0);
    CHECK(self_hit(ranked, QUERY_ROW));
    for (int i = 0; i < K; ++i)
        CHECK(ranked[i].item_id >= 0 && ranked[i].item_id < ROWS && ranked[i].item_id % 2 == 1);
    CHECK(cagra_distances_match(corpus, queries[0], ranked));
    report("anchor", "cagra-filter-self", ranked, 1);
    report("approximate", "cagra-filter", ranked, K);
    cuvs_cagra_free(cagra, 0);

    CHECK(cuvs_ivfpq_build(&corpus[0][0], ROWS, DIM, CUVS_METRIC_L2, 4, 4, 8, 0, &pq) == 0);
    CHECK(pq != NULL);
    CHECK(cuvs_ivfpq_search(pq, queries[0], DIM, K, 4, ranked, 0) == 0);
    for (int i = 0; i < K; ++i)
        CHECK(ranked[i].item_id >= 0 && ranked[i].item_id < ROWS && isfinite(ranked[i].distance));
    report("approximate", "ivfpq", ranked, K);
    cuvs_ivfpq_free(pq, 0);

    puts("test_search_staging: all GPU search routes passed");
    return EXIT_SUCCESS;
}
