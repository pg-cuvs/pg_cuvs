#include "cuvs_hnsw_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static CuvsHnswPolicy nn_policy(void)
{
    CuvsHnswPolicy p = {0};
    p.magic = CUVS_HNSW_POLICY_MAGIC; p.version = CUVS_HNSW_POLICY_NN_VERSION; p.bytes = sizeof(p);
    p.route = CUVS_HNSW_ROUTE_NN_DESCENT; p.requested_algo = CUVS_HNSW_ALGO_NN_DESCENT;
    p.internal_builder = CUVS_HNSW_ROUTE_NN_DESCENT; p.guard_passed = 1; p.requested_ef = 128;
    p.degree = 64; p.intermediate = 256; p.n_rows = 10000; p.dim = 768;
    return p;
}

/* v3 field checks; each assertion kills a mutant that survived before. */
static void test_v3_fields(void)
{
    CuvsHnswPolicy p = nn_policy();
    assert(cuvs_hnsw_policy_valid(&p));
    p.metric = 2; assert(cuvs_hnsw_policy_valid(&p));
    p.metric = 3; assert(!cuvs_hnsw_policy_valid(&p));
    p = nn_policy(); p.hierarchy = 1; assert(cuvs_hnsw_policy_valid(&p));
    p.hierarchy = 2; assert(!cuvs_hnsw_policy_valid(&p));
    p = nn_policy(); p.guard_passed = 0; assert(!cuvs_hnsw_policy_valid(&p));
    p.guard_passed = 2; assert(!cuvs_hnsw_policy_valid(&p));
    p = nn_policy(); p.n_rows = 256; assert(!cuvs_hnsw_policy_valid(&p)); /* intermediate == n_rows */
    p.n_rows = 257; assert(cuvs_hnsw_policy_valid(&p));
    p = nn_policy(); p.route_flags = CUVS_HNSW_ROUTE_FLAG_NO_STATISTICS; assert(!cuvs_hnsw_policy_valid(&p));
    p.route_flags |= CUVS_HNSW_ROUTE_FLAG_AFTER_SCAN; assert(cuvs_hnsw_policy_valid(&p));
}

static int read_nn(CuvsHnswPolicy p, CuvsHnswNnReadback nn)
{
    struct { CuvsHnswPolicy p; CuvsHnswNnReadback nn; } page;
    CuvsHnswPolicy out;
    memset(&page, 0, sizeof(page)); page.p = p; page.nn = nn;
    return cuvs_hnsw_policy_read(&page, sizeof(page), &out);
}

static void test_readback_rules(void)
{
    CuvsHnswPolicy p = nn_policy();
    CuvsHnswNnReadback nn = {0};
    nn.magic = CUVS_HNSW_NN_READBACK_MAGIC; nn.version = 1; nn.bytes = sizeof(nn);
    nn.graph_degree = 256; nn.internal_degree = 384; nn.sdk_default_iterations = 20;
    nn.max_iterations = 40; nn.dtype = CUVS_HNSW_NN_DTYPE_FP16; nn.termination_threshold = 0.0001f;
    assert(read_nn(p, nn) == CUVS_HNSW_POLICY_RECORDED);
    /* dim <= 16 must have recorded FP32 */
    p.dim = 16; assert(read_nn(p, nn) == CUVS_HNSW_POLICY_CORRUPT);
    nn.dtype = CUVS_HNSW_NN_DTYPE_FP32; assert(read_nn(p, nn) == CUVS_HNSW_POLICY_RECORDED);
    /* SDK default iterations are bounded */
    p = nn_policy(); p.requested_ef = 64;
    nn.sdk_default_iterations = 1024; nn.max_iterations = 1024; assert(read_nn(p, nn) == CUVS_HNSW_POLICY_RECORDED);
    nn.sdk_default_iterations = 1025; nn.max_iterations = 1025; assert(read_nn(p, nn) == CUVS_HNSW_POLICY_CORRUPT);
    /* internal degree clamps to n_rows - 1 */
    p = nn_policy(); p.n_rows = 300;
    nn.sdk_default_iterations = 20; nn.max_iterations = 40; nn.internal_degree = 299;
    assert(read_nn(p, nn) == CUVS_HNSW_POLICY_RECORDED);
    nn.internal_degree = 384; assert(read_nn(p, nn) == CUVS_HNSW_POLICY_CORRUPT);
}

/* Every (requested, route, overflowed) combination the daemon preflight may
 * report; R1 graph fallback is excluded because it happens after the build. */
static void test_forecast_matches(void)
{
    for (uint32_t a = 0; a <= 4; a++)
        for (uint32_t r = 0; r <= 5; r++)
            for (uint32_t o = 0; o <= 2; o++)
            {
                int expect = o <= 1 && (a == CUVS_HNSW_ALGO_AUTO
                    ? (r == CUVS_HNSW_ROUTE_ACE && o) || ((r == CUVS_HNSW_ROUTE_TINY || r == CUVS_HNSW_ROUTE_IVF_PQ) && !o)
                    : !o && ((a == CUVS_HNSW_ALGO_IVF_PQ && r == CUVS_HNSW_ROUTE_IVF_PQ)
                             || (a == CUVS_HNSW_ALGO_ACE && r == CUVS_HNSW_ROUTE_ACE)
                             || (a == CUVS_HNSW_ALGO_NN_DESCENT && r == CUVS_HNSW_ROUTE_NN_DESCENT)));
                assert(!!cuvs_hnsw_forecast_matches(a, r, o) == expect);
            }
    /* the explicit NN contract, spelled out */
    assert(cuvs_hnsw_forecast_matches(CUVS_HNSW_ALGO_NN_DESCENT, CUVS_HNSW_ROUTE_NN_DESCENT, 0));
    assert(!cuvs_hnsw_forecast_matches(CUVS_HNSW_ALGO_NN_DESCENT, CUVS_HNSW_ROUTE_IVF_PQ, 0));
    assert(!cuvs_hnsw_forecast_matches(CUVS_HNSW_ALGO_NN_DESCENT, CUVS_HNSW_ROUTE_ACE, 0));
    assert(!cuvs_hnsw_forecast_matches(CUVS_HNSW_ALGO_NN_DESCENT, CUVS_HNSW_ROUTE_TINY, 0));
    assert(!cuvs_hnsw_forecast_matches(CUVS_HNSW_ALGO_NN_DESCENT, CUVS_HNSW_ROUTE_NN_DESCENT, 1));
}

int main(void)
{
    struct { CuvsHnswPolicy p; CuvsHnswNnReadback nn; } page = {0};
    CuvsHnswPolicy read;
    CuvsHnswPolicy p = {0};
    p.magic = CUVS_HNSW_POLICY_MAGIC;
    p.version = CUVS_HNSW_POLICY_NN_VERSION;
    p.bytes = sizeof(p);
    p.route = CUVS_HNSW_ROUTE_NN_DESCENT;
    p.requested_algo = CUVS_HNSW_ALGO_NN_DESCENT;
    p.internal_builder = CUVS_HNSW_ROUTE_NN_DESCENT;
    p.guard_passed = 1;
    p.requested_ef = 128;
    p.degree = 64; p.intermediate = 256; p.n_rows = 10000; p.dim = 768;
    assert(cuvs_hnsw_policy_valid(&p));
    page.p = p;
    page.nn.magic = CUVS_HNSW_NN_READBACK_MAGIC;
    page.nn.version = 1; page.nn.bytes = sizeof(page.nn);
    page.nn.graph_degree = 256; page.nn.internal_degree = 384;
    page.nn.sdk_default_iterations = 20; page.nn.max_iterations = 40; page.nn.dtype = CUVS_HNSW_NN_DTYPE_FP32;
    page.nn.termination_threshold = 0.0001f;
    assert(cuvs_hnsw_policy_read(&page, sizeof(page), &read) == CUVS_HNSW_POLICY_RECORDED);
    assert(cuvs_hnsw_policy_read(&page, sizeof(p), &read) == CUVS_HNSW_POLICY_CORRUPT);
    page.nn.graph_degree = 128;
    assert(cuvs_hnsw_policy_read(&page, sizeof(page), &read) == CUVS_HNSW_POLICY_CORRUPT);
    page.nn.graph_degree = 256; page.nn.internal_degree = 383;
    assert(cuvs_hnsw_policy_read(&page, sizeof(page), &read) == CUVS_HNSW_POLICY_CORRUPT);
    page.nn.internal_degree = 384; page.nn.dtype = 99;
    assert(cuvs_hnsw_policy_read(&page, sizeof(page), &read) == CUVS_HNSW_POLICY_CORRUPT);
    page.nn.dtype = CUVS_HNSW_NN_DTYPE_FP32; page.nn.max_iterations = 0;
    assert(cuvs_hnsw_policy_read(&page, sizeof(page), &read) == CUVS_HNSW_POLICY_CORRUPT);
    page.nn.max_iterations = 41;
    assert(cuvs_hnsw_policy_read(&page, sizeof(page), &read) == CUVS_HNSW_POLICY_CORRUPT);
    page.nn.max_iterations = 40; page.nn.version = 2;
    assert(cuvs_hnsw_policy_read(&page, sizeof(page), &read) == CUVS_HNSW_POLICY_UNSUPPORTED);
    page.nn.version = 1; page.nn.termination_threshold = 0.0f;
    assert(cuvs_hnsw_policy_read(&page, sizeof(page), &read) == CUVS_HNSW_POLICY_CORRUPT);
    p.route = CUVS_HNSW_ROUTE_IVF_PQ;
    assert(!cuvs_hnsw_policy_valid(&p));
    p.route = CUVS_HNSW_ROUTE_NN_DESCENT;
    p.internal_builder = CUVS_HNSW_ROUTE_IVF_PQ;
    assert(!cuvs_hnsw_policy_valid(&p));
    p.internal_builder = CUVS_HNSW_ROUTE_NN_DESCENT;
    p.training_actual = 1;
    assert(!cuvs_hnsw_policy_valid(&p));
    p.training_actual = 0; p.n_lists = 1;
    assert(!cuvs_hnsw_policy_valid(&p));
    p.n_lists = 0; p.n_rows = 15;
    assert(!cuvs_hnsw_policy_valid(&p));
    p.n_rows = 10000; p.intermediate = 1025;
    assert(!cuvs_hnsw_policy_valid(&p));
    p.intermediate = 256; p.version = CUVS_HNSW_POLICY_VERSION;
    assert(!cuvs_hnsw_policy_valid(&p));
    assert(cuvs_hnsw_policy_read(&p, sizeof(p), &read) == CUVS_HNSW_POLICY_CORRUPT);
    p.version = CUVS_HNSW_POLICY_NN_VERSION + 1;
    assert(cuvs_hnsw_policy_read(&p, sizeof(p), &read) == CUVS_HNSW_POLICY_UNSUPPORTED);
    test_v3_fields();
    test_readback_rules();
    test_forecast_matches();
    puts("NN versioned policy contract: PASS");
    return 0;
}
