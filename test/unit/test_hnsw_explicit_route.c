#include "cuvs_hnsw_build_common.h"
#include <assert.h>
#include <stdio.h>

static unsigned checks;
#define CHECK(value) do { assert(value); checks++; } while (0)

static CuvsHnswPolicy policy(uint32_t algo, uint32_t route)
{
    CuvsHnswPolicy p = {0};
    p.magic = CUVS_HNSW_POLICY_MAGIC;
    p.version = CUVS_HNSW_POLICY_VERSION;
    p.bytes = sizeof(p); p.route = route; p.requested_algo = algo;
    p.requested_ef = 64; p.guard_passed = 1;
    p.internal_builder = route;
    p.partitions = route == CUVS_HNSW_ROUTE_ACE ? 2 : 0;
    return p;
}

static void test_receipts(void)
{
    CuvsHnswPolicy p, out;
    CHECK(sizeof(CuvsHnswPolicy) == 160);
    CHECK(offsetof(CuvsHnswPolicy, training_requested) == 96);
    CHECK(offsetof(CuvsHnswPolicy, sdk_host_budget) == 144);
    for (uint32_t a = 0; a <= 4; a++)
        for (uint32_t r = 0; r <= 4; r++)
        {
            p = policy(a, r);
            int valid = (a == CUVS_HNSW_ALGO_AUTO && (r == CUVS_HNSW_ROUTE_TINY || r == CUVS_HNSW_ROUTE_IVF_PQ))
                || (a == CUVS_HNSW_ALGO_IVF_PQ && r == CUVS_HNSW_ROUTE_IVF_PQ)
                || (a == CUVS_HNSW_ALGO_ACE && r == CUVS_HNSW_ROUTE_ACE);
            CHECK(!!cuvs_hnsw_policy_valid(&p) == valid);
            /* ADR-087 (UC-5): the overflow flag is valid exactly on auto -> ACE. */
            p.route_flags = CUVS_HNSW_ROUTE_FLAG_AUTO_OVERFLOW;
            CHECK(!!cuvs_hnsw_policy_valid(&p)
                  == (a == CUVS_HNSW_ALGO_AUTO && r == CUVS_HNSW_ROUTE_ACE));
        }
    p = policy(CUVS_HNSW_ALGO_AUTO, CUVS_HNSW_ROUTE_ACE);
    CHECK(!cuvs_hnsw_policy_valid(&p)); /* auto ACE without the overflow record */
    CHECK(cuvs_hnsw_policy_read(&p, sizeof(p), &out) == CUVS_HNSW_POLICY_CORRUPT);
    p.route_flags = CUVS_HNSW_ROUTE_FLAG_AUTO_OVERFLOW;
    CHECK(cuvs_hnsw_policy_read(&p, sizeof(p), &out) == CUVS_HNSW_POLICY_RECORDED);
    p.route_flags = 0;
    p.version = CUVS_HNSW_POLICY_LEGACY_VERSION;
    CuvsHnswPolicy original = p;
    CHECK(cuvs_hnsw_policy_read(&p, sizeof(p), &out) == CUVS_HNSW_POLICY_RECORDED);
    CHECK(!memcmp(&p, &original, sizeof(p)) && !memcmp(&out, &original, sizeof(p)));
    CHECK(!cuvs_hnsw_policy_valid(&p)); /* historical bytes are not a new writer receipt */
    p = policy(CUVS_HNSW_ALGO_ACE, CUVS_HNSW_ROUTE_ACE);
    CHECK(cuvs_hnsw_policy_read(&p, sizeof(p), &out) == CUVS_HNSW_POLICY_RECORDED);
    CHECK(cuvs_hnsw_policy_read(NULL, 0, &out) == CUVS_HNSW_POLICY_NOT_RECORDED);
    uint32_t none = 0;
    CHECK(cuvs_hnsw_policy_read(&none, sizeof(none), &out) == CUVS_HNSW_POLICY_NOT_RECORDED);
    for (size_t n = 1; n < sizeof(p); n++)
        CHECK(cuvs_hnsw_policy_read(&p, n, &out) == CUVS_HNSW_POLICY_CORRUPT);
    p.bytes--;
    CHECK(cuvs_hnsw_policy_read(&p, sizeof(p), &out) == CUVS_HNSW_POLICY_CORRUPT);
    p.bytes++; p.version = 99;
    CHECK(cuvs_hnsw_policy_read(&p, sizeof(p), &out) == CUVS_HNSW_POLICY_UNSUPPORTED);
    p = policy(CUVS_HNSW_ALGO_IVF_PQ, CUVS_HNSW_ROUTE_IVF_PQ); p.guard_passed = 0;
    CHECK(!cuvs_hnsw_policy_valid(&p));
    p = policy(CUVS_HNSW_ALGO_ACE, CUVS_HNSW_ROUTE_ACE); p.internal_builder = 2;
    CHECK(!cuvs_hnsw_policy_valid(&p));
    p.internal_builder = 3; p.partitions = 0;
    CHECK(!cuvs_hnsw_policy_valid(&p));
    CHECK(cuvs_hnsw_algo_from_name("ace") == CUVS_HNSW_ALGO_ACE);
    CHECK(cuvs_hnsw_algo_from_name("auto") == CUVS_HNSW_ALGO_AUTO);
    CHECK(cuvs_hnsw_algo_from_name("ivf_pq") == CUVS_HNSW_ALGO_IVF_PQ);
    CHECK(!cuvs_hnsw_algo_supported(cuvs_hnsw_algo_from_name("nn_descent")));
    CHECK(!cuvs_hnsw_algo_supported(cuvs_hnsw_algo_from_name("typo")));
    CHECK(!strcmp(cuvs_hnsw_algo_name(CUVS_HNSW_ALGO_ACE), "ace"));
}

static void test_requests(void)
{
    CuvsHnswFileRequest r = {CUVS_HNSW_FILE_VERSION, 1, 1ULL << 30, CUVS_HNSW_ALGO_ACE, 64};
    CHECK(CUVS_HNSW_FILE_VERSION == 3);
    CHECK(cuvs_hnsw_file_request_supported(&r));
    for (uint32_t v = 0; v <= 4; v++)
    {
        r.version = v;
        CHECK(!!cuvs_hnsw_file_request_supported(&r) == (v == 3));
    }
    r.version = 3;
    for (uint32_t a = 0; a <= 4; a++)
    {
        r.build_algo = a;
        CHECK(!!cuvs_hnsw_file_request_supported(&r) == (a == 0 || a == 1 || a == 3));
    }
    r.build_algo = 3; r.hierarchy = 2; CHECK(!cuvs_hnsw_file_request_supported(&r));
    r.hierarchy = 1; r.host_budget = 0; CHECK(!cuvs_hnsw_file_request_supported(&r));
    r.host_budget = 1; r.ef_construction = 3; CHECK(!cuvs_hnsw_file_request_supported(&r));
    r.ef_construction = 1001; CHECK(!cuvs_hnsw_file_request_supported(&r));
}

static void test_routes(void)
{
    const uint64_t h = 32ULL << 30, g = 64ULL << 30;
    CuvsHnswCapacity cap; CuvsHnswRouteDecision d; char error[128];
    CHECK(cuvs_hnsw_sample_rows(1000000, 768, h, g) == 174762);
    CHECK(cuvs_hnsw_sample_rows(1000000, 768, g, h) == 174762);
    CHECK(cuvs_hnsw_sample_rows(1000000, 768, 16ULL << 30, g) == 87381);
    CHECK(cuvs_hnsw_sample_rows(15, 768, h, g) == 15);
    CHECK(cuvs_hnsw_sample_rows(1000, 768, 1, 1) == 256);
    CHECK(cuvs_hnsw_sample_rows(1000, 0, h, g) == 0);
    CHECK(!cuvs_hnsw_capacity(1000000, 768, 64, 128, 1, &cap));
    CHECK(!cuvs_hnsw_training_reserve(&cap, cuvs_hnsw_training_rows(1000000, 174762, 500), 768));
    for (uint32_t a = 0; a <= 1; a++)
    {
        CHECK(cuvs_hnsw_resolve_route(1000000, 768, 64, 128, a, CUVS_HNSW_OVERFLOW_ERROR, &cap, h, g, &d, error, sizeof(error)) == CUVS_STATUS_OK);
        CHECK(d.route == CUVS_HNSW_ROUTE_IVF_PQ && d.partitions == 0);
        CHECK(d.sdk_host_budget == h - (256ULL << 20) && d.sdk_gpu_budget == g - (512ULL << 20));
        CHECK(cuvs_hnsw_resolve_route(1000000, 768, 64, 128, a, CUVS_HNSW_OVERFLOW_ERROR, &cap, cap.host_peak, cap.gpu_peak, &d, error, sizeof(error)) == CUVS_STATUS_OK);
        CHECK(cuvs_hnsw_resolve_route(1000000, 768, 64, 128, a, CUVS_HNSW_OVERFLOW_ERROR, &cap, cap.host_peak - 1, g, &d, error, sizeof(error)) == CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED);
        CHECK(d.route == 0 && strstr(error, "build_algo=ace"));
        CHECK(cuvs_hnsw_resolve_route(1000000, 768, 64, 128, a, CUVS_HNSW_OVERFLOW_ERROR, &cap, h, cap.gpu_peak - 1, &d, error, sizeof(error)) == CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED);
    }
    CHECK(cuvs_hnsw_resolve_route(1000000, 768, 64, 128, CUVS_HNSW_ALGO_ACE, CUVS_HNSW_OVERFLOW_ERROR, &cap, h, g, &d, error, sizeof(error)) == CUVS_STATUS_OK);
    CHECK(d.route == CUVS_HNSW_ROUTE_ACE && d.partitions >= 2); /* candidate, not real SDK support */
    CHECK(cuvs_hnsw_resolve_route(1000000, 768, 64, 128, CUVS_HNSW_ALGO_ACE, CUVS_HNSW_OVERFLOW_ERROR, &cap, 1, 1, &d, error, sizeof(error)) == CUVS_STATUS_BUILD_FAILED);
    CHECK(d.route == 0 && !strstr(error, "use build_algo=ace"));
    CHECK(cuvs_hnsw_resolve_route(1000000, 768, 64, 128, 2, CUVS_HNSW_OVERFLOW_ERROR, &cap, h, g, &d, error, sizeof(error)) == CUVS_STATUS_BUILD_FAILED);
    CHECK(cuvs_hnsw_resolve_route(1000000, 768, 64, 128, UINT32_MAX, CUVS_HNSW_OVERFLOW_ERROR, &cap, h, g, &d, error, sizeof(error)) == CUVS_STATUS_BUILD_FAILED);
    CHECK(cuvs_hnsw_resolve_route(1000000, 768, 64, 128, 0, CUVS_HNSW_OVERFLOW_ERROR, &cap, 0, g, &d, error, sizeof(error)) == CUVS_STATUS_BUILD_FAILED);
    CHECK(cuvs_hnsw_resolve_route(UINT64_MAX, 768, 64, 128, 0, CUVS_HNSW_OVERFLOW_ERROR, &cap, h, g, &d, error, sizeof(error)) == CUVS_STATUS_BUILD_FAILED);
    for (uint64_t n = 1; n <= 16; n++)
    {
        uint32_t degree = n < 16 ? (n & ~1ULL) : 14;
        if (degree < 4) degree = 4;
        CHECK(!cuvs_hnsw_capacity(n, 768, degree, degree, 0, &cap));
        CHECK(cuvs_hnsw_resolve_route(n, 768, degree, degree, 0, CUVS_HNSW_OVERFLOW_ERROR, &cap, h, g, &d, error, sizeof(error)) == CUVS_STATUS_OK);
        CHECK(d.route == (n < 16 ? CUVS_HNSW_ROUTE_TINY : CUVS_HNSW_ROUTE_IVF_PQ));
        if (n < 16)
        {
            CHECK(cuvs_hnsw_resolve_route(n, 768, degree, degree, 1, CUVS_HNSW_OVERFLOW_ERROR, &cap, h, g, &d, error, sizeof(error)) == CUVS_STATUS_BUILD_FAILED);
            CHECK(cuvs_hnsw_resolve_route(n, 768, degree, degree, 3, CUVS_HNSW_OVERFLOW_ERROR, &cap, h, g, &d, error, sizeof(error)) == CUVS_STATUS_BUILD_FAILED);
            CHECK(cuvs_hnsw_resolve_route(n, 768, degree, degree, 0, CUVS_HNSW_OVERFLOW_ERROR, &cap, 1, 1, &d, error, sizeof(error)) == CUVS_STATUS_BUILD_FAILED);
        }
    }
    for (uint64_t n = 16; n <= 256; n += n == 16 ? 239 : 1)
    {
        CHECK(!cuvs_hnsw_capacity(n, 768, 64, 128, 0, &cap));
        int rc = cuvs_hnsw_resolve_route(n, 768, 64, 128, 3, CUVS_HNSW_OVERFLOW_ERROR, &cap, h, g, &d, error, sizeof(error));
        CHECK(rc == (n < 256 ? CUVS_STATUS_BUILD_FAILED : CUVS_STATUS_OK));
    }
    CHECK(!cuvs_hnsw_capacity(14200000, 1536, 64, 128, 1, &cap));
    CHECK(cuvs_hnsw_resolve_route(14200000, 1536, 64, 128, 0, CUVS_HNSW_OVERFLOW_ERROR, &cap, 16ULL << 30, g, &d, error, sizeof(error)) == CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED);
    CHECK(cuvs_hnsw_resolve_route(14200000, 1536, 64, 128, 3, CUVS_HNSW_OVERFLOW_ERROR, &cap, 16ULL << 30, g, &d, error, sizeof(error)) == CUVS_STATUS_OK);
    CHECK(d.route == CUVS_HNSW_ROUTE_ACE && d.partitions > 0);
}

int main(void)
{
    test_receipts(); test_requests(); test_routes();
    printf("PASS %u CPU policy/resolver/request/legacy-reader checks; SDK/GPU not run\n", checks);
    return 0;
}
