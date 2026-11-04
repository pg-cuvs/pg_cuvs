/* I3-C CPU tests: CK-17 (12)-(14), UC-5 / ADR-087. Headers only; no PG,
 * daemon, CUDA or SDK. Each block names the CK item it covers. */
#include "cuvs_route_preflight.h"
#include <assert.h>
#include <stdio.h>

static unsigned checks;
#define CHECK(value) do { assert(value); checks++; } while (0)

static const uint64_t H = 32ULL << 30, G = 64ULL << 30;

static int forecast(uint64_t n, uint32_t algo, uint32_t overflow, uint64_t host,
                    uint64_t gpu, CuvsHnswForecast *f)
{
    char error[128];
    return cuvs_hnsw_forecast_route(n, 768, 64, 128, 1, algo, 64, overflow, host, gpu,
                                    f, error, sizeof(error));
}

/* (12) default macro: within budget resident, over budget ACE + overflow
 * record; knob error keeps status 13; explicit ivf_pq/ace ignore the knob. */
static void test_default_and_knob(void)
{
    CuvsHnswForecast f, e;
    CHECK(CUVS_HNSW_AUTO_OVERFLOW_DEFAULT == CUVS_HNSW_OVERFLOW_ACE);
    CHECK(!strcmp(cuvs_hnsw_overflow_name(CUVS_HNSW_AUTO_OVERFLOW_DEFAULT), "ace"));
    CHECK(cuvs_hnsw_overflow_from_name("ace") == CUVS_HNSW_OVERFLOW_ACE);
    CHECK(cuvs_hnsw_overflow_from_name("error") == CUVS_HNSW_OVERFLOW_ERROR);
    CHECK(cuvs_hnsw_overflow_from_name("ace_first") == UINT32_MAX); /* D-처음 not built */
    CHECK(cuvs_hnsw_overflow_from_name(NULL) == UINT32_MAX);

    /* within budget: resident under either knob */
    CHECK(forecast(1000000, CUVS_HNSW_ALGO_AUTO, CUVS_HNSW_AUTO_OVERFLOW_DEFAULT, H, G, &f) == CUVS_STATUS_OK);
    CHECK(f.decision.route == CUVS_HNSW_ROUTE_IVF_PQ && !f.decision.overflowed);
    /* The estimate is a function of the budgets (training sample), so a
     * host budget of 16 GiB is over the 1M x 768 estimate at that budget. */
    const uint64_t need = (16ULL << 30) + 1;
    CHECK(forecast(1000000, CUVS_HNSW_ALGO_AUTO, CUVS_HNSW_OVERFLOW_ACE, need - 1, G, &e) == CUVS_STATUS_OK
          && e.host_estimate > need - 1);
    /* over the host budget: default spills, error refuses */
    CHECK(forecast(1000000, CUVS_HNSW_ALGO_AUTO, CUVS_HNSW_AUTO_OVERFLOW_DEFAULT, need - 1, G, &f) == CUVS_STATUS_OK);
    CHECK(f.decision.route == CUVS_HNSW_ROUTE_ACE && f.decision.overflowed && f.decision.partitions > 0);
    CHECK(forecast(1000000, CUVS_HNSW_ALGO_AUTO, CUVS_HNSW_OVERFLOW_ERROR, need - 1, G, &e)
          == CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED);
    CHECK(e.decision.route == 0 && !e.decision.overflowed);
    /* GPU side overflows the same way */
    CHECK(forecast(1000000, CUVS_HNSW_ALGO_AUTO, CUVS_HNSW_OVERFLOW_ACE, H, 2ULL << 30, &f) == CUVS_STATUS_OK);
    CHECK(f.gpu_estimate > (2ULL << 30) && f.host_estimate <= H);
    CHECK(f.decision.route == CUVS_HNSW_ROUTE_ACE && f.decision.overflowed);
    /* explicit ivf_pq never overflows, under either knob */
    for (uint32_t k = 0; k <= 1; k++)
    {
        CHECK(forecast(1000000, CUVS_HNSW_ALGO_IVF_PQ, k, need - 1, G, &f)
              == CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED);
        CHECK(forecast(1000000, CUVS_HNSW_ALGO_ACE, k, H, G, &f) == CUVS_STATUS_OK);
        CHECK(f.decision.route == CUVS_HNSW_ROUTE_ACE && !f.decision.overflowed);
        CHECK(forecast(1000000, CUVS_HNSW_ALGO_IVF_PQ, k, H, G, &f) == CUVS_STATUS_OK);
        CHECK(f.decision.route == CUVS_HNSW_ROUTE_IVF_PQ && !f.decision.overflowed);
    }
    /* n < 256 over budget: ACE impossible, status 13 even with ace */
    CHECK(forecast(200, CUVS_HNSW_ALGO_AUTO, CUVS_HNSW_OVERFLOW_ACE, 1 << 20, G, &f)
          == CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED);
    CHECK(!f.decision.overflowed);
    /* tiny auto stays tiny; unknown knob value is refused, never defaulted */
    CHECK(forecast(10, CUVS_HNSW_ALGO_AUTO, CUVS_HNSW_OVERFLOW_ACE, H, G, &f) == CUVS_STATUS_OK);
    CHECK(f.decision.route == CUVS_HNSW_ROUTE_TINY);
    CHECK(forecast(1000000, CUVS_HNSW_ALGO_AUTO, 7, H, G, &f) == CUVS_STATUS_BUILD_FAILED);
    /* the knob's name appears in the error-mode refusal */
    char error[128];
    CHECK(cuvs_hnsw_forecast_route(1000000, 768, 64, 128, 1, CUVS_HNSW_ALGO_AUTO, 64,
            CUVS_HNSW_OVERFLOW_ERROR, need - 1, G, &e, error, sizeof(error))
          == CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED);
    CHECK(strstr(error, "--hnsw-auto-overflow=error") && strstr(error, "build_algo=ace"));
}

/* The forecast reproduces the wrapper's estimate whatever the SDK default
 * n_lists is (it is clamped to <= sample/32 there): training rows do not
 * depend on n_lists, so [hnsw_route] and the receipt agree. */
static void test_estimate_independent_of_lists(void)
{
    const uint64_t ns[] = {16, 255, 256, 10000, 1000000, 2500000};
    for (size_t i = 0; i < sizeof(ns) / sizeof(ns[0]); i++)
        for (uint64_t host = 1ULL << 30; host <= H; host <<= 2)
        {
            uint64_t n = ns[i], sample = cuvs_hnsw_sample_rows(n, 768, host, G);
            uint64_t base = cuvs_hnsw_training_rows(n, sample, 1);
            for (uint64_t lists = 1; lists <= sample / 32; lists = lists * 3 + 1)
                CHECK(cuvs_hnsw_training_rows(n, sample, (uint32_t)lists) == base);
        }
}

/* (13) invariant: the overflow flag exists only on the single resolver
 * branch, the receipt contract requires it on auto -> ACE, and every
 * auto -> ACE receipt yields NOTICE lines. */
static CuvsHnswPolicy receipt(uint32_t algo, uint32_t route, uint32_t flags)
{
    CuvsHnswPolicy p = {0};
    p.magic = CUVS_HNSW_POLICY_MAGIC; p.version = CUVS_HNSW_POLICY_VERSION;
    p.bytes = sizeof(p); p.route = route; p.requested_algo = algo;
    p.requested_ef = 64; p.guard_passed = 1; p.internal_builder = route;
    p.partitions = route == CUVS_HNSW_ROUTE_ACE ? 4 : 0; p.route_flags = flags;
    return p;
}

static void test_invariant_notices(void)
{
    CuvsHnswPolicy p;
    for (uint32_t a = 0; a <= 3; a++)
        for (uint32_t r = 1; r <= 3; r++)
            for (uint32_t flags = 0; flags <= 0xf; flags++)
                for (int sent = 0; sent <= 1; sent++)
                {
                    p = receipt(a, r, flags);
                    if (!cuvs_hnsw_policy_valid(&p)) continue;
                    uint32_t n = cuvs_hnsw_build_notices(&p, sent);
                    int auto_ace = a == CUVS_HNSW_ALGO_AUTO && r == CUVS_HNSW_ROUTE_ACE;
                    /* valid auto ACE receipts all carry the overflow record */
                    CHECK(!auto_ace || (flags & CUVS_HNSW_ROUTE_FLAG_AUTO_OVERFLOW));
                    /* every auto -> ACE gets the completion line, and the
                     * transition line exactly when not already sent */
                    CHECK(!!(n & CUVS_HNSW_NOTICE_AUTO_COMPLETION) == auto_ace);
                    CHECK(!!(n & CUVS_HNSW_NOTICE_TRANSITION) == (auto_ace && !sent));
                    CHECK(!!(n & CUVS_HNSW_NOTICE_EXPLICIT_ACE) == (a == CUVS_HNSW_ALGO_ACE));
                    CHECK(!!(n & CUVS_HNSW_NOTICE_NOT_OVERFLOWED) == (!auto_ace && a != CUVS_HNSW_ALGO_ACE && sent));
                }
    /* receipts that could hide a transition are rejected */
    p = receipt(CUVS_HNSW_ALGO_AUTO, CUVS_HNSW_ROUTE_ACE, 0);
    CHECK(!cuvs_hnsw_policy_valid(&p));
    p = receipt(CUVS_HNSW_ALGO_IVF_PQ, CUVS_HNSW_ROUTE_IVF_PQ, CUVS_HNSW_ROUTE_FLAG_AUTO_OVERFLOW);
    CHECK(!cuvs_hnsw_policy_valid(&p));
    p = receipt(CUVS_HNSW_ALGO_AUTO, CUVS_HNSW_ROUTE_ACE,
                CUVS_HNSW_ROUTE_FLAG_AUTO_OVERFLOW | CUVS_HNSW_ROUTE_FLAG_PRESPOOL
                | CUVS_HNSW_ROUTE_FLAG_AFTER_SCAN);
    CHECK(!cuvs_hnsw_policy_valid(&p)); /* timing bits are exclusive */
    p = receipt(CUVS_HNSW_ALGO_AUTO, CUVS_HNSW_ROUTE_ACE,
                CUVS_HNSW_ROUTE_FLAG_AUTO_OVERFLOW | CUVS_HNSW_ROUTE_FLAG_NO_STATISTICS);
    CHECK(!cuvs_hnsw_policy_valid(&p)); /* a cause without the after-scan timing */
    p = receipt(CUVS_HNSW_ALGO_AUTO, CUVS_HNSW_ROUTE_ACE, CUVS_HNSW_ROUTE_FLAG_AUTO_OVERFLOW | 0x10);
    CHECK(!cuvs_hnsw_policy_valid(&p)); /* unknown flag */
    CHECK(sizeof(CuvsHnswPolicy) == 160); /* reserved -> route_flags, same layout */
    /* "(need X, budget Y)" names the exceeded side */
    uint64_t need, budget; const char *side;
    cuvs_hnsw_overflow_need(10, 9, 1, 2, &need, &budget, &side);
    CHECK(need == 10 && budget == 9 && !strcmp(side, "host"));
    cuvs_hnsw_overflow_need(1, 9, 5, 2, &need, &budget, &side);
    CHECK(need == 5 && budget == 2 && !strcmp(side, "GPU"));
}

/* (14) statistics present -> decided before the scan, including the disk
 * precheck; statistics absent or zero -> post-scan path, recorded. */
static void test_prespool_and_fallback(void)
{
    const uint32_t OK = CUVS_STATUS_OK, B13 = CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED,
                   F5 = CUVS_STATUS_BUILD_FAILED;
    const double no_stats[] = {-1.0, 0.0, -0.5, 0.0 / 0.0};
    for (size_t i = 0; i < sizeof(no_stats) / sizeof(no_stats[0]); i++)
    {
        /* whatever the daemon would answer, no statistics never refuses */
        CHECK(cuvs_hnsw_prespool_action(no_stats[i], B13, 0, 0, 0) == CUVS_HNSW_PRESPOOL_FALLBACK);
        CHECK(cuvs_hnsw_prespool_action(no_stats[i], F5, 0, 0, 0) == CUVS_HNSW_PRESPOOL_FALLBACK);
        CHECK(cuvs_hnsw_prespool_action(no_stats[i], OK, CUVS_HNSW_ROUTE_ACE, 1, 1) == CUVS_HNSW_PRESPOOL_FALLBACK);
        CHECK(cuvs_prespool_rows(no_stats[i]) == 0);
    }
    for (size_t i = 0; i < sizeof(no_stats) / sizeof(no_stats[0]); i++)
        CHECK(cuvs_hnsw_timing_flags(no_stats[i], CUVS_HNSW_PRESPOOL_FALLBACK)
              == (CUVS_HNSW_ROUTE_FLAG_AFTER_SCAN | CUVS_HNSW_ROUTE_FLAG_NO_STATISTICS));
    /* statistics present but the pre-scan forecast failed: after scan, NOT no-statistics */
    CHECK(cuvs_hnsw_timing_flags(1e6, CUVS_HNSW_PRESPOOL_FALLBACK) == CUVS_HNSW_ROUTE_FLAG_AFTER_SCAN);
    for (int a = CUVS_HNSW_PRESPOOL_PROCEED; a <= CUVS_HNSW_PRESPOOL_REJECT; a++)
    {
        CHECK(cuvs_hnsw_timing_flags(1e6, (CuvsHnswPrespoolAction)a) == CUVS_HNSW_ROUTE_FLAG_PRESPOOL);
        CHECK(cuvs_hnsw_timing_flags(-1, (CuvsHnswPrespoolAction)a) == CUVS_HNSW_ROUTE_FLAG_PRESPOOL);
    }
    CHECK(!strcmp(cuvs_hnsw_route_decided_name(CUVS_HNSW_ROUTE_FLAG_PRESPOOL), "before_scan_from_statistics"));
    CHECK(!strcmp(cuvs_hnsw_route_decided_name(CUVS_HNSW_ROUTE_FLAG_AFTER_SCAN | CUVS_HNSW_ROUTE_FLAG_NO_STATISTICS),
                  "after_scan_no_statistics"));
    CHECK(!strcmp(cuvs_hnsw_route_decided_name(CUVS_HNSW_ROUTE_FLAG_AFTER_SCAN), "after_scan_preflight_unavailable"));
    CHECK(!strcmp(cuvs_hnsw_route_decided_name(0), "not_recorded"));
    /* with statistics */
    CHECK(cuvs_prespool_rows(1000000.0) == 1000000 && cuvs_prespool_rows(10.2) == 11);
    CHECK(cuvs_hnsw_prespool_action(1e6, OK, CUVS_HNSW_ROUTE_ACE, 1, 1) == CUVS_HNSW_PRESPOOL_NOTICE_OVERFLOW);
    CHECK(cuvs_hnsw_prespool_action(1e6, OK, CUVS_HNSW_ROUTE_ACE, 0, 1) == CUVS_HNSW_PRESPOOL_PROCEED); /* explicit ace */
    CHECK(cuvs_hnsw_prespool_action(1e6, OK, CUVS_HNSW_ROUTE_IVF_PQ, 0, 1) == CUVS_HNSW_PRESPOOL_PROCEED);
    CHECK(cuvs_hnsw_prespool_action(1e6, B13, 0, 0, 0) == CUVS_HNSW_PRESPOOL_REJECT_BUDGET);
    CHECK(cuvs_hnsw_prespool_action(1e6, F5, 0, 0, 0) == CUVS_HNSW_PRESPOOL_REJECT);
    /* exchange failures are not decisions */
    CHECK(cuvs_hnsw_prespool_action(1e6, CUVS_STATUS_UNAVAILABLE, 0, 0, 0) == CUVS_HNSW_PRESPOOL_FALLBACK);
    CHECK(cuvs_hnsw_prespool_action(1e6, CUVS_STATUS_ERROR, 0, 0, 0) == CUVS_HNSW_PRESPOOL_FALLBACK);
    CHECK(cuvs_hnsw_prespool_action(1e6, CUVS_STATUS_PROTO_MISMATCH, 0, 0, 0) == CUVS_HNSW_PRESPOOL_FALLBACK);
    CHECK(cuvs_hnsw_prespool_action(1e6, OK, CUVS_HNSW_ROUTE_ACE, 1, 0) == CUVS_HNSW_PRESPOOL_FALLBACK);

    /* The live 20k x128 / 512 MiB no-fit forecast is a configured budget
     * refusal. An unrelated build failure or completed disk probe is not. */
    {
        CuvsHnswForecast nofit;
        CuvsHnswPreflightReply reply = {0};
        char why[128];
        CHECK(cuvs_hnsw_forecast_route(20000, 128, 64, 128, 1,
            CUVS_HNSW_ALGO_AUTO, 128, CUVS_HNSW_OVERFLOW_ACE,
            512ULL << 20, 4ULL << 30, &nofit, why, sizeof(why)) == CUVS_STATUS_BUILD_FAILED);
        reply.header.status = nofit.status;
        reply.host_estimate = nofit.host_estimate; reply.host_budget = nofit.host_budget;
        reply.gpu_estimate = nofit.gpu_estimate; reply.gpu_budget = nofit.gpu_budget;
        reply.partitions = nofit.decision.partitions;
        CHECK(cuvs_hnsw_prespool_budget_refusal(&reply));
        reply.header.status = CUVS_STATUS_ERROR;
        CHECK(!cuvs_hnsw_prespool_budget_refusal(&reply));
        reply.header.status = CUVS_STATUS_BUILD_FAILED;
        reply.disk_regular = 1;
        CHECK(!cuvs_hnsw_prespool_budget_refusal(&reply));
        reply.disk_regular = 0; reply.partitions = 1;
        CHECK(!cuvs_hnsw_prespool_budget_refusal(&reply));
        reply.partitions = 0; reply.disk_avail = 1;
        CHECK(!cuvs_hnsw_prespool_budget_refusal(&reply));
        reply.disk_avail = 0; reply.host_estimate = reply.host_budget;
        reply.gpu_estimate = reply.gpu_budget;
        CHECK(!cuvs_hnsw_prespool_budget_refusal(&reply));
        reply.gpu_estimate++;
        CHECK(cuvs_hnsw_prespool_budget_refusal(&reply));
        memset(&reply, 0, sizeof(reply)); reply.header.status = CUVS_STATUS_BUILD_FAILED;
        CHECK(!cuvs_hnsw_prespool_budget_refusal(&reply));
    }

    /* disk precheck: same formula as the daemon (UC-5 sub-decision 1(a)) */
    CuvsHnswForecast f; char error[160];
    CHECK(forecast(1000000, CUVS_HNSW_ALGO_AUTO, CUVS_HNSW_OVERFLOW_ACE, 4ULL << 30, G, &f) == CUVS_STATUS_OK);
    CHECK(f.decision.route == CUVS_HNSW_ROUTE_ACE);
    CHECK(f.disk_need == 1000000ULL * 768 * 4 * 4 + 1000000ULL * (64 * 8 + 64));
    CHECK(f.disk_need == cuvs_hnsw_disk_need(1000000, 768, 64));
    CHECK(cuvs_hnsw_forecast_disk(&f, 1, f.disk_need, error, sizeof(error)) == CUVS_STATUS_OK && f.disk_ok);
    CHECK(cuvs_hnsw_forecast_disk(&f, 1, f.disk_need - 1, error, sizeof(error)) == CUVS_STATUS_BUILD_FAILED);
    CHECK(!f.disk_ok && strstr(error, "route=ace_disk") && strstr(error, "need="));
    CHECK(forecast(1000000, CUVS_HNSW_ALGO_AUTO, CUVS_HNSW_OVERFLOW_ACE, H, G, &f) == CUVS_STATUS_OK);
    CHECK(cuvs_hnsw_forecast_disk(&f, 0, UINT64_MAX, error, sizeof(error)) == CUVS_STATUS_BUILD_FAILED);
    CHECK(strstr(error, "fs=tmpfs") && strstr(error, "route=resident_ivf_pq"));
    /* a refused route is not turned into a disk verdict */
    CHECK(forecast(1000000, CUVS_HNSW_ALGO_IVF_PQ, 1, 1 << 20, G, &f) == CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED);
    CHECK(cuvs_hnsw_forecast_disk(&f, 1, UINT64_MAX, error, sizeof(error)) == CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED);
    CHECK(!f.disk_ok);
    CHECK(forecast(0, CUVS_HNSW_ALGO_AUTO, 1, H, G, &f) == CUVS_STATUS_BUILD_FAILED);
}

/* I3CD-B1 direct regression, as the backend sequences it: positive
 * statistics, the first op 40 fails (daemon/IPC), the scan runs, the second
 * op 40 succeeds and announces the overflow, op 23 returns an auto -> ACE
 * receipt. The stored metadata must say the forecast was unavailable, never
 * "no statistics"; the no-statistics path keeps its value. */
static const char *backend_receipt_route_decided(double reltuples, int first_rc, int second_rc,
                                                 int *transition_sent, int *valid)
{
    static CuvsHnswPolicy stored;
    CuvsHnswPolicy p, read;
    /* pre-scan: cuvs_hnsw_preflight(stats_rows, ...) */
    CuvsHnswPrespoolAction pre = cuvs_prespool_rows(reltuples)
        ? cuvs_hnsw_prespool_action(reltuples, first_rc, CUVS_HNSW_ROUTE_ACE, 1, 1)
        : CUVS_HNSW_PRESPOOL_FALLBACK;
    *transition_sent = pre == CUVS_HNSW_PRESPOOL_NOTICE_OVERFLOW;
    /* after the scan, only on FALLBACK: cuvs_hnsw_preflight(spool->n, ..., may_reject=false) */
    if (pre == CUVS_HNSW_PRESPOOL_FALLBACK)
        *transition_sent = cuvs_hnsw_prespool_action(400000, second_rc, CUVS_HNSW_ROUTE_ACE, 1, 1)
            == CUVS_HNSW_PRESPOOL_NOTICE_OVERFLOW;
    /* op 23 success: wrapper receipt, then the backend ORs the timing flags */
    p = receipt(CUVS_HNSW_ALGO_AUTO, CUVS_HNSW_ROUTE_ACE, CUVS_HNSW_ROUTE_FLAG_AUTO_OVERFLOW);
    p.route_flags |= cuvs_hnsw_timing_flags(reltuples, pre);
    /* metapage round trip, as pg_cuvs_hnsw_build_info reads it */
    stored = p;
    *valid = cuvs_hnsw_policy_read(&stored, sizeof(stored), &read) == CUVS_HNSW_POLICY_RECORDED;
    return cuvs_hnsw_route_decided_name(read.route_flags);
}

static void test_i3cd_b1_preflight_failure_is_not_no_statistics(void)
{
    int sent, valid;
    const int fails[] = {CUVS_STATUS_UNAVAILABLE, CUVS_STATUS_ERROR, CUVS_STATUS_PROTO_MISMATCH};
    for (size_t i = 0; i < sizeof(fails) / sizeof(fails[0]); i++)
    {
        const char *decided = backend_receipt_route_decided(400000, fails[i], CUVS_STATUS_OK, &sent, &valid);
        CHECK(valid && sent);
        CHECK(!strcmp(decided, "after_scan_preflight_unavailable"));
        CHECK(!strstr(decided, "no_statistics"));
    }
    /* no statistics keeps after_scan_no_statistics */
    CHECK(!strcmp(backend_receipt_route_decided(-1, CUVS_STATUS_OK, CUVS_STATUS_OK, &sent, &valid),
                  "after_scan_no_statistics") && valid && sent);
    CHECK(!strcmp(backend_receipt_route_decided(0, CUVS_STATUS_OK, CUVS_STATUS_OK, &sent, &valid),
                  "after_scan_no_statistics") && valid);
    /* statistics and a working pre-scan forecast */
    CHECK(!strcmp(backend_receipt_route_decided(400000, CUVS_STATUS_OK, CUVS_STATUS_OK, &sent, &valid),
                  "before_scan_from_statistics") && valid && sent);
}

/* I3CD-B1 r2: statistics presence is reltuples > 0, independent of the op 40
 * row conversion. 0 < reltuples < 1 is a (tiny) forecast; reltuples above
 * INT32_MAX has statistics but no hnsw forecast. Neither may read as
 * "no statistics" in the receipt or the NOTICE wording. */
static void test_i3cd_b1_r2_statistics_boundaries(void)
{
    int sent, valid;
    /* 0 < reltuples < 1 */
    CHECK(cuvs_prespool_has_statistics(0.5) && cuvs_prespool_rows(0.5) == 1 && cuvs_prespool_count(0.5) == 1);
    CHECK(cuvs_hnsw_timing_flags(0.5, CUVS_HNSW_PRESPOOL_FALLBACK) == CUVS_HNSW_ROUTE_FLAG_AFTER_SCAN);
    CHECK(!strcmp(cuvs_hnsw_after_scan_reason(0.5), "pre-scan forecast unavailable"));
    CHECK(!strcmp(backend_receipt_route_decided(0.5, CUVS_STATUS_OK, CUVS_STATUS_OK, &sent, &valid),
                  "before_scan_from_statistics") && valid);
    CHECK(!strcmp(backend_receipt_route_decided(0.5, CUVS_STATUS_UNAVAILABLE, CUVS_STATUS_OK, &sent, &valid),
                  "after_scan_preflight_unavailable") && valid);
    /* reltuples > INT32_MAX: no op 40 request, still statistics */
    CHECK(cuvs_prespool_has_statistics(3e9) && cuvs_prespool_rows(3e9) == 0
          && cuvs_prespool_count(3e9) == 3000000000ULL);
    CHECK(cuvs_prespool_rows((double)INT32_MAX) == (uint64_t)INT32_MAX
          && cuvs_prespool_rows((double)INT32_MAX + 1.0) == 0);
    CHECK(cuvs_hnsw_timing_flags(3e9, CUVS_HNSW_PRESPOOL_FALLBACK) == CUVS_HNSW_ROUTE_FLAG_AFTER_SCAN);
    CHECK(!strcmp(cuvs_hnsw_after_scan_reason(3e9), "pre-scan forecast unavailable"));
    CHECK(!strcmp(backend_receipt_route_decided(3e9, CUVS_STATUS_OK, CUVS_STATUS_OK, &sent, &valid),
                  "after_scan_preflight_unavailable") && valid);
    CHECK(cuvs_prespool_count(1e30) == (uint64_t)INT64_MAX);
    /* the no-statistics side keeps its wording */
    const double none[] = {-1.0, 0.0, -0.5, 0.0 / 0.0, 1.0 / 0.0};
    for (size_t i = 0; i < sizeof(none) / sizeof(none[0]); i++)
    {
        CHECK(!cuvs_prespool_has_statistics(none[i]) && cuvs_prespool_count(none[i]) == 0);
        CHECK(!strcmp(cuvs_hnsw_after_scan_reason(none[i]), "no table statistics"));
        CHECK(!strcmp(backend_receipt_route_decided(none[i], CUVS_STATUS_OK, CUVS_STATUS_OK, &sent, &valid),
                      "after_scan_no_statistics") && valid);
    }
}

/* (14) CAGRA reltuples AR0 guard: refuse before the scan only when certain. */
static void test_cagra_ar0(void)
{
    uint64_t est;
    const uint64_t limit = 10ULL << 30, fit = 3000000, over = 4000000; /* 768-d */
    /* I3-A's shared estimate (aligned stride ld = 4 * ceil(dim / 4)) */
    CHECK(cuvs_admission_estimate_final_bytes(1000, 768, 64) == 1000ULL * (768 * 4 + 64 * 4));
    CHECK(cuvs_admission_estimate_final_bytes(1000, 770, 0) == 1000ULL * (772 * 4 + 64 * 4));
    CHECK(cuvs_cagra_prespool_ar0((double)over, 770, 64, 1, 1, limit, &est)
          && est == cuvs_admission_estimate_final_bytes(over, 770, 64));
    /* statistics beyond the HNSW forecast range still prove a CAGRA AR0 */
    CHECK(cuvs_cagra_prespool_ar0(3e9, 768, 64, 1, 1, limit, &est)
          && est == cuvs_admission_estimate_final_bytes(3000000000LL, 768, 64));
    CHECK(cuvs_cagra_prespool_ar0((double)over, 768, 64, 1, 1, limit, &est) && est > limit);
    CHECK(cuvs_cagra_prespool_ar0((double)over, 768, 64, 0, 1, limit, &est)); /* auto, one GPU */
    CHECK(!cuvs_cagra_prespool_ar0((double)fit, 768, 64, 1, 1, limit, &est) && est <= limit);
    /* could be auto-sharded across GPUs, or explicitly sharded: scan */
    CHECK(!cuvs_cagra_prespool_ar0((double)over, 768, 64, 0, 2, limit, &est));
    CHECK(!cuvs_cagra_prespool_ar0((double)over, 768, 64, 4, 1, limit, &est));
    /* no statistics, no limit, no devices: current path */
    CHECK(!cuvs_cagra_prespool_ar0(-1, 768, 64, 1, 1, limit, &est));
    CHECK(!cuvs_cagra_prespool_ar0(0, 768, 64, 1, 1, limit, &est));
    CHECK(!cuvs_cagra_prespool_ar0((double)over, 768, 64, 1, 1, 0, &est));
    CHECK(!cuvs_cagra_prespool_ar0((double)over, 768, 64, 1, 0, limit, &est));
    CHECK(!strcmp(CUVS_ADMISSION_SQLSTATE_LIMIT, "54000"));
    CHECK(cuvs_prespool_mib(1) == 1 && cuvs_prespool_mib(1ULL << 20) == 1);
    CHECK(cuvs_prespool_mib(UINT64_MAX / 2) == CUVS_ADMISSION_SCALAR_MAX);
}

/* v6 C2: HNSW allocation failure after admission -> status 2 -> 53200 hints. */
static void test_hnsw_oom_hint(void)
{
    CHECK(strstr(cuvs_hnsw_oom_hint(CUVS_HNSW_ALGO_AUTO), "build_algo='ace'"));
    CHECK(strstr(cuvs_hnsw_oom_hint(CUVS_HNSW_ALGO_IVF_PQ), "cuvs.max_build_mem_mb")
          && strstr(cuvs_hnsw_oom_hint(CUVS_HNSW_ALGO_IVF_PQ), "build_algo='ace'"));
    CHECK(!strstr(cuvs_hnsw_oom_hint(CUVS_HNSW_ALGO_ACE), "build_algo='ace'"));
    CHECK(CUVS_STATUS_OOM_FALLBACK == 2);
}

int main(void)
{
    /* First, so its mutants are attributed to the direct regression itself. */
    test_i3cd_b1_preflight_failure_is_not_no_statistics();
    test_i3cd_b1_r2_statistics_boundaries();
    test_default_and_knob();
    test_estimate_independent_of_lists();
    test_invariant_notices();
    test_prespool_and_fallback();
    test_cagra_ar0();
    test_hnsw_oom_hint();
    printf("PASS %u CK-17 (12)-(14) CPU checks: default ace + knob, single overflow branch receipts/NOTICEs, pre-spool/fallback, disk precheck, CAGRA reltuples AR0; SDK/GPU/PG not run\n", checks);
    return 0;
}
