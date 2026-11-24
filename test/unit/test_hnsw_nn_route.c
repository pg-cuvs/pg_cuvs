/* Forecast and admission for direct NN; no CUDA or PG execution. */
#include "cuvs_route_preflight.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int route(uint64_t n, uint32_t dim, uint32_t ef, uint64_t host, uint64_t gpu,
                 CuvsHnswForecast *f, char *error)
{
    return cuvs_hnsw_forecast_route(n, dim, 64, 128, 1, CUVS_HNSW_ALGO_NN_DESCENT,
        ef, CUVS_HNSW_OVERFLOW_ACE, host, gpu, f, error, 256);
}
int main(void)
{
    CuvsHnswForecast f;
    char error[256] = {0};
    const uint64_t budget = UINT64_C(32) << 30;
    assert(route(10000, 128, 128, budget, budget, &f, error) == CUVS_STATUS_OK);
    assert(f.decision.route == CUVS_HNSW_ROUTE_NN_DESCENT && !f.decision.overflowed);
    assert(f.decision.partitions == 0);
    assert(cuvs_hnsw_prespool_action(10000, CUVS_STATUS_OK, f.decision.route, 0, 1)
           == CUVS_HNSW_PRESPOOL_PROCEED);
    assert(route(10000, 128, 128, 1, 1, &f, error) == CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED);
    assert(f.decision.route == 0 && !f.decision.overflowed && strstr(error, "no fallback"));
    assert(route(0, 128, 64, budget, budget, &f, error) != CUVS_STATUS_OK);
    assert(route(15, 128, 64, budget, budget, &f, error) != CUVS_STATUS_OK);
    assert(route(16, 128, 64, budget, budget, &f, error) == CUVS_STATUS_OK);
    assert(f.decision.route == CUVS_HNSW_ROUTE_NN_DESCENT);
    CuvsHnswNnCapacity capped;
    assert(cuvs_hnsw_nn_capacity(10000, 128, 64, 1024, 1,
                                 CUVS_HNSW_NN_FP32, &capped) == 0);
    assert(route(10000, 128, 1000, budget, budget, &f, error) == CUVS_STATUS_OK);
    assert(f.decision.route == CUVS_HNSW_ROUTE_NN_DESCENT && !f.decision.overflowed);
    assert(f.host_estimate == capped.host_peak && f.gpu_estimate == capped.gpu_peak);
    assert(route(10000, 128, 1001, budget, budget, &f, error) != CUVS_STATUS_OK);
    assert(!cuvs_hnsw_graph_failure_to_ace(CUVS_HNSW_ALGO_NN_DESCENT,
        CUVS_HNSW_ROUTE_NN_DESCENT, CUVS_HNSW_OVERFLOW_ACE, 10000, CUVS_HNSW_GRAPH_KNN_REJECTED));
    puts("direct NN route, tiny, clamp, budget and no fallback: PASS");
    return 0;
}
