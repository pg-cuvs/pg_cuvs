#include "cuvs_hnsw_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

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
    puts("NN versioned policy contract: PASS");
    return 0;
}
