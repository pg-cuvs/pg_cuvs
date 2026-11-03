/*
 * CPU unit tests for src/cuvs_cache_policy.c (I3-B; v6 r18 CK-9, CK-23,
 * CK-24, CK-36 (e)). No daemon, no PG, no CUDA. Expected values are written
 * out by hand from the decision text, not recomputed with the code under test.
 *
 *   make test-cache-policy
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "cuvs_cache_policy.h"

static int g_pass, g_fail;

#define CHECK(cond, ...) do {                                         \
    if (cond) g_pass++;                                               \
    else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
           printf(__VA_ARGS__); printf("\n"); }                       \
} while (0)

#define GiB (1024ull * 1024 * 1024)
#define NOW 1000000

static CuvsEvictParams P = { 600, 30, 20 };

static CuvsEvictCand
cand(uint32_t idx, int64_t last_use, int64_t loaded_at, uint32_t hits,
     int pinned, int in_use, uint64_t bytes)
{
    CuvsEvictCand c = { 1, idx, last_use, loaded_at, hits, pinned, in_use, bytes };
    return c;
}

static CuvsEvictPick
pick(const CuvsEvictCand *c, int n, CuvsRequester rq, const char *scope,
     int soft, uint64_t need, int *victims, CuvsEvictReason *reasons)
{
    CuvsEvictRequest req;
    CuvsEvictPick out;
    memset(&req, 0, sizeof(req));
    req.rq = rq;
    cuvs_parse_evict_scope(scope, &req.scope);
    req.soft_evict = soft;
    req.need = need;
    req.now = NOW;
    cuvs_evict_pick(c, n, &P, &req, victims, reasons, &out);
    return out;
}

/* ---------------------------------------------------------------- CK-23 */
static void
test_protect_and_eligible(void)
{
    /* P = 600 + min(hits, 20) * 30 */
    CHECK(cuvs_evict_protect_s(0, 600, 30, 20) == 600, "P(0)");
    CHECK(cuvs_evict_protect_s(5, 600, 30, 20) == 750, "P(5)");
    CHECK(cuvs_evict_protect_s(20, 600, 30, 20) == 1200, "P(20)");
    CHECK(cuvs_evict_protect_s(500, 600, 30, 20) == 1200, "bonus cap");

    CuvsEvictCand c = cand(10, NOW - 600, NOW - 5000, 0, 0, 0, GiB);
    CHECK(cuvs_evict_eligible(&c, NOW, &P), "t - last_use == P is eligible");
    c.last_use = NOW - 599;
    CHECK(!cuvs_evict_eligible(&c, NOW, &P), "P - 1 is not eligible");
    c.last_use = NOW - 1199; c.hits = 1000;
    CHECK(!cuvs_evict_eligible(&c, NOW, &P), "hits above cap: P stays 1200");
    c.last_use = NOW - 1200;
    CHECK(cuvs_evict_eligible(&c, NOW, &P), "hits above cap: eligible at 1200");
    c = cand(10, 0, NOW - 1, 0, 0, 0, GiB);
    CHECK(cuvs_evict_eligible(&c, NOW, &P), "last_use 0 is eligible");
    c.pinned = 1;
    CHECK(!cuvs_evict_eligible(&c, NOW, &P), "pinned never eligible");
    c.pinned = 0; c.in_use = 1;
    CHECK(!cuvs_evict_eligible(&c, NOW, &P), "in use never eligible");
}

static void
test_order(void)
{
    /* Fresh index (last_use 0, loaded now-10) vs long-cold (last_use now-5000):
     * ord = max(last_use, loaded_at) -> cold first. */
    CuvsEvictCand c[3] = {
        cand(5, 0, NOW - 10, 0, 0, 0, GiB),
        cand(9, NOW - 5000, NOW - 9000, 0, 0, 0, GiB),
        cand(7, NOW - 5000, NOW - 9000, 0, 0, 0, GiB),   /* tie with 9 */
    };
    int order[3];
    int k = cuvs_reclaim_plan(c, 3, &P, NOW, order);
    CHECK(k == 3, "all eligible (%d)", k);
    CHECK(order[0] == 2 && order[1] == 1 && order[2] == 0,
          "ord asc, tie by index_oid: got %d %d %d", order[0], order[1], order[2]);
}

static void
test_pick_build_scope(void)
{
    int v[4]; CuvsEvictReason r[4];
    CuvsEvictCand c[2] = {
        cand(1, NOW - 5000, NOW - 9000, 0, 0, 0, 2 * GiB),  /* eligible */
        cand(2, NOW - 10, NOW - 9000, 0, 0, 0, 2 * GiB),    /* protected */
    };
    CuvsEvictPick p = pick(c, 2, CUVS_RQ_BUILD_CAGRA, "none", 1, GiB, v, r);
    CHECK(p.verdict == CUVS_EVICT_AR5 && p.n_victims == 0,
          "scope none: no victim, AR5 (lru set fits) v=%d n=%d", p.verdict, p.n_victims);
    CHECK(p.lru_total == 4 * GiB && p.elig_total == 2 * GiB, "totals");

    p = pick(c, 2, CUVS_RQ_BUILD_CAGRA, "idle:0", 1, GiB, v, r);
    CHECK(p.verdict == CUVS_EVICT_OK && p.n_victims == 1 && v[0] == 0 &&
          r[0] == CUVS_EVICT_ADMIT_BUILD, "idle:0 evicts the eligible one");

    p = pick(c, 2, CUVS_RQ_BUILD_CAGRA, "idle:120", 1, GiB, v, r);
    CHECK(p.verdict == CUVS_EVICT_AR5, "idle:120 (5000 s idle < 7200) -> AR5");

    p = pick(c, 2, CUVS_RQ_BUILD_CAGRA, "lru", 1, 3 * GiB, v, r);
    CHECK(p.verdict == CUVS_EVICT_OK && p.n_victims == 2, "lru ignores P");

    p = pick(c, 2, CUVS_RQ_BUILD_IVFPQ, "none", 1, GiB, v, r);
    CHECK(p.verdict == CUVS_EVICT_AR5 && p.n_victims == 0, "IVF-PQ build scope none");
    p = pick(c, 2, CUVS_RQ_BUILD_SHARD, "none", 1, GiB, v, r);
    CHECK(p.n_victims == 0, "shard build scope none");

    p = pick(c, 2, CUVS_RQ_BUILD_CAGRA, "lru", 1, 5 * GiB, v, r);
    CHECK(p.verdict == CUVS_EVICT_SHORT && p.n_victims == 0,
          "short even with lru: AR3/AR4, nothing evicted");
    p = pick(c, 2, CUVS_RQ_BUILD_CAGRA, "none", 1, 0, v, r);
    CHECK(p.verdict == CUVS_EVICT_OK && p.n_victims == 0, "need 0 fits");
}

static void
test_pick_load_soft(void)
{
    int v[4]; CuvsEvictReason r[4];
    /* A eligible 1 GiB, B protected-by-time 2 GiB, C pinned 4 GiB, D busy 1 GiB */
    CuvsEvictCand c[4] = {
        cand(1, NOW - 5000, NOW - 9000, 0, 0, 0, GiB),
        cand(2, NOW - 10, NOW - 9000, 3, 0, 0, 2 * GiB),
        cand(3, NOW - 9999, NOW - 99999, 0, 1, 0, 4 * GiB),
        cand(4, NOW - 9999, NOW - 99999, 0, 0, 1, GiB),
    };
    CuvsEvictPick p = pick(c, 4, CUVS_RQ_LOAD, "none", 1, GiB, v, r);
    CHECK(p.verdict == CUVS_EVICT_OK && p.n_victims == 1 && v[0] == 0 &&
          r[0] == CUVS_EVICT_RELOAD, "eligible suffices -> reload only");

    p = pick(c, 4, CUVS_RQ_LOAD, "none", 1, 2 * GiB, v, r);
    CHECK(p.verdict == CUVS_EVICT_OK && p.n_victims == 2 && v[0] == 0 &&
          r[0] == CUVS_EVICT_RELOAD && v[1] == 1 && r[1] == CUVS_EVICT_SOFT,
          "eligible short -> soft fills the rest (n=%d)", p.n_victims);

    p = pick(c, 4, CUVS_RQ_LOAD, "none", 1, 4 * GiB, v, r);
    CHECK(p.verdict == CUVS_EVICT_AR5 && p.wait == CUVS_ADMISSION_WAIT_QUERIES &&
          p.n_victims == 0, "busy release would fit -> AR5 w=0, nothing evicted");
    p = pick(c, 4, CUVS_RQ_LOAD, "none", 1, 7 * GiB, v, r);
    CHECK(p.verdict == CUVS_EVICT_AR5 && p.wait == CUVS_ADMISSION_WAIT_UNPIN,
          "needs unpin -> AR5 w=-1");
    p = pick(c, 4, CUVS_RQ_LOAD, "none", 1, 9 * GiB, v, r);
    CHECK(p.verdict == CUVS_EVICT_SHORT, "short even unpinned -> AR3/AR4");
    p = pick(c, 4, CUVS_RQ_LOAD_SHARD, "none", 1, 2 * GiB, v, r);
    CHECK(p.verdict == CUVS_EVICT_OK && r[1] == CUVS_EVICT_SOFT, "LOAD_SHARD soft");

    /* Same input, soft off / warmup: eligible only. */
    p = pick(c, 4, CUVS_RQ_LOAD, "none", 0, 2 * GiB, v, r);
    CHECK(p.verdict == CUVS_EVICT_AR5 && p.n_victims == 0, "soft off -> AR5");
    p = pick(c, 4, CUVS_RQ_LOAD_WARMUP, "none", 1, GiB, v, r);
    CHECK(p.verdict == CUVS_EVICT_OK && r[0] == CUVS_EVICT_RELOAD, "warmup eligible");
    p = pick(c, 4, CUVS_RQ_LOAD_WARMUP, "none", 1, 2 * GiB, v, r);
    CHECK(p.verdict == CUVS_EVICT_AR5 && p.n_victims == 0, "warmup: no soft");
    p = pick(c, 4, CUVS_RQ_BUILD_CAGRA, "idle:0", 1, 2 * GiB, v, r);
    CHECK(p.verdict == CUVS_EVICT_AR5 && p.n_victims == 0, "build: no soft");

    /* Startup never evicts and never says AR5. */
    p = pick(c, 4, CUVS_RQ_LOAD_STARTUP, "lru", 1, GiB, v, r);
    CHECK(p.verdict == CUVS_EVICT_SHORT && p.n_victims == 0, "startup no evict");

    /* All pinned: AR5 w=-1. */
    CuvsEvictCand allpin[1] = { cand(9, NOW - 9999, NOW - 99999, 0, 1, 0, 4 * GiB) };
    p = pick(allpin, 1, CUVS_RQ_LOAD, "none", 1, GiB, v, r);
    CHECK(p.verdict == CUVS_EVICT_AR5 && p.wait == -1, "all pinned -> AR5 w=-1");
    CuvsEvictCand allbusy[1] = { cand(9, NOW - 9999, NOW - 99999, 0, 0, 2, 4 * GiB) };
    p = pick(allbusy, 1, CUVS_RQ_LOAD, "none", 1, GiB, v, r);
    CHECK(p.verdict == CUVS_EVICT_AR5 && p.wait == 0, "all busy -> AR5 w=0");
}

static void
test_self_replace(void)
{
    CHECK(cuvs_self_replace_decide(0, 1, 0, GiB, 1) == CUVS_SELF_NOT_NEEDED, "fits");
    CHECK(cuvs_self_replace_decide(GiB, 1, 0, GiB, 1) == CUVS_SELF_REPLACE, "old copy covers");
    CHECK(cuvs_self_replace_decide(GiB, 1, 1, GiB, 1) == CUVS_SELF_UNAVAILABLE, "pinned");
    CHECK(cuvs_self_replace_decide(GiB, 1, 0, GiB, 0) == CUVS_SELF_UNAVAILABLE, "knob off");
    CHECK(cuvs_self_replace_decide(GiB, 0, 0, 0, 1) == CUVS_SELF_UNAVAILABLE, "no old");
    CHECK(cuvs_self_replace_decide(2 * GiB, 1, 0, GiB, 1) == CUVS_SELF_UNAVAILABLE,
          "old copy too small");

    /* NOTICE/DETAIL wording (decision UC-1 2a), exact strings and numbers. */
    char m[256], d[512];
    CHECK(cuvs_self_replace_notice(m, sizeof m, d, sizeof d, "\"public\".\"i1\"",
                                   10240000, 5734400) == 0, "notice fits");
    CHECK(strcmp(m, "pg_cuvs: previous resident copy of \"public\".\"i1\" unloaded from GPU "
                    "to make room for the rebuild") == 0, "notice text: %s", m);
    CHECK(strcmp(d, "GPU memory was insufficient for both copies (need 10240000 bytes, "
                    "free 5734400 bytes). Queries on this index use the CPU path or reload "
                    "the old copy until the new index is resident") == 0, "detail: %s", d);
    CHECK(cuvs_self_replace_fail_notice(m, sizeof m, d, sizeof d, "index 42") == 0 &&
          strcmp(m, "pg_cuvs: rebuild of index 42 failed after its previous resident copy "
                    "was unloaded from GPU") == 0 &&
          strcmp(d, "The next query on this index reloads the old copy") == 0,
          "failure notice: %s | %s", m, d);
    CHECK(cuvs_self_replace_notice(m, 10, d, sizeof d, "x", 1, 1) == -1, "truncation reported");
}

/* ---------------------------------------------------------------- CK-24 */
static CuvsAdmitRecord
rec(uint32_t idx, uint64_t needed, int reason, int64_t at)
{
    CuvsAdmitRecord r = { 1, idx, needed, reason, 0, at };
    return r;
}

static void
test_free_target_fences(void)
{
    CuvsAdmitLog log; CuvsFreeTarget t; char src[64];
    uint64_t B = 100 * GiB;   /* budget_eff */
    memset(&log, 0, sizeof(log));

    /* F-1: AR0 and AR7 never drive F. */
    cuvs_admit_log_add(&log, &(CuvsAdmitRecord){1, 1, 30 * GiB, CUVS_ADMISSION_AR0, 0, NOW});
    cuvs_admit_log_add(&log, &(CuvsAdmitRecord){1, 2, 20 * GiB, CUVS_ADMISSION_AR7, 0, NOW});
    cuvs_reclaim_free_target(&log, 0, NOW, 1800, B, 50, 0, &t);
    CHECK(t.f_bytes == 0 && t.src == CUVS_FSRC_NONE, "F-1: AR0/AR7 only -> F=0");

    /* AR1..AR6 count. */
    for (int rsn = CUVS_ADMISSION_AR1; rsn <= CUVS_ADMISSION_AR6; rsn++)
    {
        CuvsAdmitLog l2; memset(&l2, 0, sizeof(l2));
        CuvsAdmitRecord x = rec(3, 10 * GiB, rsn, NOW);
        cuvs_admit_log_add(&l2, &x);
        cuvs_reclaim_free_target(&l2, 0, NOW, 1800, B, 50, 0, &t);
        CHECK(t.f_bytes == 10 * GiB, "AR%d counts", rsn);
    }

    /* F-2: a 100% request is capped at 50% of budget_eff. */
    CuvsAdmitRecord big = rec(4, B, CUVS_ADMISSION_AR5, NOW);
    cuvs_admit_log_add(&log, &big);
    cuvs_reclaim_free_target(&log, 0, NOW, 1800, B, 50, 0, &t);
    CHECK(t.f_bytes == 50 * GiB && t.src == CUVS_FSRC_BUILD && t.src_idx == 4,
          "F-2 cap 50%%: %llu", (unsigned long long) t.f_bytes);
    cuvs_format_free_src(&t, src, sizeof(src));
    CHECK(strcmp(src, "build:1:4") == 0, "src '%s'", src);
    cuvs_reclaim_free_target(&log, 0, NOW, 1800, 60 * GiB, 50, 0, &t);
    CHECK(t.f_bytes == 30 * GiB, "F-2 base is budget_eff (logical < quota)");

    /* F-3: window edges and admission removal. */
    memset(&log, 0, sizeof(log));
    CuvsAdmitRecord in = rec(5, 8 * GiB, CUVS_ADMISSION_AR5, NOW - 1799);
    CuvsAdmitRecord out = rec(6, 9 * GiB, CUVS_ADMISSION_AR5, NOW - 1801);
    cuvs_admit_log_add(&log, &in);
    cuvs_admit_log_add(&log, &out);
    cuvs_reclaim_free_target(&log, 0, NOW, 1800, B, 50, 0, &t);
    CHECK(t.f_bytes == 8 * GiB, "F-3: window-1 in, window+1 out (%llu)",
          (unsigned long long) t.f_bytes);
    CuvsAdmitRecord admitted = rec(5, 8 * GiB, CUVS_ADMISSION_NONE, NOW);
    cuvs_admit_log_add(&log, &admitted);
    cuvs_reclaim_free_target(&log, 0, NOW, 1800, B, 50, 0, &t);
    CHECK(t.f_bytes == 0, "F-3: later admission removes the reject");
    cuvs_admit_log_add(&log, &in);
    cuvs_reclaim_free_target(&log, 0, NOW + 4000, 1800, B, 50, 0, &t);
    CHECK(t.f_bytes == 0, "F-3: one reject does not outlive the window");
    cuvs_admit_log_expire(&log, NOW + 4000, 1800);
    CHECK(log.n == 0, "expire drops old records");
    cuvs_admit_log_add(&log, &in);
    cuvs_admit_log_forget(&log, 1, 5);
    cuvs_reclaim_free_target(&log, 0, NOW, 1800, B, 50, 0, &t);
    CHECK(t.f_bytes == 0 && log.n == 0, "DROP INDEX forgets its reject at once");

    /* F is the MAXIMUM of the window's eligible requests (not sum, not
     * min, not last), whatever their order: 8, 12, 5 GiB -> 12 GiB. */
    memset(&log, 0, sizeof(log));
    CuvsAdmitRecord r8 = rec(21, 8 * GiB, CUVS_ADMISSION_AR5, NOW - 30);
    CuvsAdmitRecord r12 = rec(22, 12 * GiB, CUVS_ADMISSION_AR3, NOW - 20);
    CuvsAdmitRecord r5 = rec(23, 5 * GiB, CUVS_ADMISSION_AR1, NOW - 10);
    cuvs_admit_log_add(&log, &r8);
    cuvs_admit_log_add(&log, &r12);
    cuvs_admit_log_add(&log, &r5);
    cuvs_reclaim_free_target(&log, 0, NOW, 1800, B, 50, 0, &t);
    CHECK(t.f_bytes == 12 * GiB && t.src_idx == 22,
          "F = max of 8/12/5 GiB = 12 GiB (got %llu, src %u)",
          (unsigned long long) (t.f_bytes / GiB), t.src_idx);
    /* Window edge: age == window_s counts, window_s + 1 does not. */
    memset(&log, 0, sizeof(log));
    CuvsAdmitRecord at_edge = rec(24, 7 * GiB, CUVS_ADMISSION_AR5, NOW - 1800);
    cuvs_admit_log_add(&log, &at_edge);
    cuvs_reclaim_free_target(&log, 0, NOW, 1800, B, 50, 0, &t);
    CHECK(t.f_bytes == 7 * GiB, "age == window_s is inside the window");
    cuvs_reclaim_free_target(&log, 0, NOW + 1, 1800, B, 50, 0, &t);
    CHECK(t.f_bytes == 0, "age == window_s + 1 is outside the window");
    memset(&log, 0, sizeof(log));

    /* Fixed F keeps the cap. */
    cuvs_reclaim_free_target(&log, 0, NOW, 1800, B, 50, 70 * GiB, &t);
    CHECK(t.f_bytes == 50 * GiB && t.src == CUVS_FSRC_FIXED, "fixed capped");
    cuvs_reclaim_free_target(&log, 0, NOW, 1800, B, 50, 5 * GiB, &t);
    CHECK(t.f_bytes == 5 * GiB, "fixed under cap");

    /* Per-GPU: a record on gpu 1 does not drive gpu 0. */
    memset(&log, 0, sizeof(log));
    CuvsAdmitRecord g1 = { 1, 7, 9 * GiB, CUVS_ADMISSION_AR5, 1, NOW };
    cuvs_admit_log_add(&log, &g1);
    cuvs_reclaim_free_target(&log, 0, NOW, 1800, B, 50, 0, &t);
    CHECK(t.f_bytes == 0, "other gpu ignored");
}

static CuvsReclaimGoal
goal(CuvsReclaimMode m, uint64_t f)
{
    CuvsReclaimGoal g = { m, 100 * GiB, 85, 75, 10, f };
    return g;
}

/* Drive the loop the way the daemon does: victims of `each` bytes. */
static int
run_loop(const CuvsReclaimGoal *g, uint64_t resident, uint64_t each, int max,
         uint64_t *after, CuvsReclaimTrigger *trig)
{
    int k = 0;
    *trig = cuvs_reclaim_trigger(g, resident);
    for (; k < max && cuvs_reclaim_need_more(g, *trig, resident); )
    {
        if (!cuvs_reclaim_take(g, *trig, resident, each))
            break;
        resident -= each;
        k++;
    }
    *after = resident;
    return k;
}

static void
test_reclaim_goal(void)
{
    uint64_t after; CuvsReclaimTrigger tr; int k;
    CuvsReclaimGoal g;

    g = goal(CUVS_RECLAIM_OFF, 50 * GiB);
    k = run_loop(&g, 99 * GiB, GiB, 100, &after, &tr);
    CHECK(k == 0 && tr == CUVS_TRIG_NONE, "off: nothing");

    g = goal(CUVS_RECLAIM_USAGE, 50 * GiB);
    k = run_loop(&g, 90 * GiB, GiB, 100, &after, &tr);
    CHECK(tr == CUVS_TRIG_USAGE && after == 75 * GiB && k == 15,
          "usage: hi 85 -> lo 75 ignores F (k=%d after=%llu)", k,
          (unsigned long long) (after / GiB));
    k = run_loop(&g, 80 * GiB, GiB, 100, &after, &tr);
    CHECK(k == 0, "usage: below hi no action");

    g = goal(CUVS_RECLAIM_HEADROOM, 30 * GiB);
    k = run_loop(&g, 80 * GiB, GiB, 100, &after, &tr);
    CHECK(tr == CUVS_TRIG_HEADROOM && after == 70 * GiB && k == 10,
          "headroom: free >= F (after=%llu)", (unsigned long long) (after / GiB));
    k = run_loop(&g, 90 * GiB, GiB, 100, &after, &tr);
    CHECK(tr == CUVS_TRIG_BOTH && after == 70 * GiB, "both: max of lo and F");
    g = goal(CUVS_RECLAIM_HEADROOM, 0);
    k = run_loop(&g, 80 * GiB, GiB, 100, &after, &tr);
    CHECK(k == 0, "headroom with F=0 == usage below hi");

    /* F-4 floor: F of 95% would empty the GPU; stops at floor 10 GiB. */
    g = goal(CUVS_RECLAIM_HEADROOM, 95 * GiB);
    k = run_loop(&g, 40 * GiB, GiB, 100, &after, &tr);
    CHECK(after == 10 * GiB, "floor stops F reclaim (after=%llu)",
          (unsigned long long) (after / GiB));
    k = run_loop(&g, 10 * GiB, GiB, 100, &after, &tr);
    CHECK(k == 0 && tr == CUVS_TRIG_NONE, "at floor: F reclaim idle");
    /* A victim that would cross below the floor is not taken. */
    k = run_loop(&g, 14 * GiB, 5 * GiB, 100, &after, &tr);
    CHECK(k == 0 && after == 14 * GiB, "no crossing below floor");

    /* r18 A-1: usage above hi while resident <= floor (floor 90%): usage
     * runs to lo, F stops. */
    g = goal(CUVS_RECLAIM_HEADROOM, 50 * GiB);
    g.floor_pct = 90;
    k = run_loop(&g, 89 * GiB, GiB, 100, &after, &tr);
    CHECK(tr == CUVS_TRIG_USAGE && after == 75 * GiB,
          "usage ignores floor (tr=%d after=%llu)", tr,
          (unsigned long long) (after / GiB));

    /* hi = 100 disables usage lines. */
    g = goal(CUVS_RECLAIM_USAGE, 0);
    g.hi_pct = 100;
    k = run_loop(&g, 100 * GiB, GiB, 100, &after, &tr);
    CHECK(k == 0, "hi 100 off");
}

static void
test_wait_hint(void)
{
    /* r = 1 only in headroom with eligible bytes, free+elig >= need,
     * need <= Fcap, post-eviction resident >= floor. */
    CHECK(cuvs_reclaim_wait_hint(CUVS_RECLAIM_HEADROOM, 5, 10, 12, 50, 60, 10) == 1, "r=1");
    CHECK(cuvs_reclaim_wait_hint(CUVS_RECLAIM_USAGE, 5, 10, 12, 50, 60, 10) == 0, "usage r=0");
    CHECK(cuvs_reclaim_wait_hint(CUVS_RECLAIM_HEADROOM, 5, 0, 4, 50, 60, 10) == 0, "elig 0");
    CHECK(cuvs_reclaim_wait_hint(CUVS_RECLAIM_HEADROOM, 5, 10, 16, 50, 60, 10) == 0, "short");
    CHECK(cuvs_reclaim_wait_hint(CUVS_RECLAIM_HEADROOM, 5, 50, 51, 50, 60, 10) == 0, "need>cap");
    CHECK(cuvs_reclaim_wait_hint(CUVS_RECLAIM_HEADROOM, 5, 10, 12, 50, 15, 10) == 0, "floor");
}

/* ----------------------------------------------------------------- CK-9 */
static void
test_params(void)
{
    CuvsReclaimParams p; char err[256];
    cuvs_reclaim_params_default(&p);
    CHECK(p.mode == CUVS_RECLAIM_HEADROOM && p.t_min_s == 600 && p.bonus_s == 30 &&
          p.hits_cap == 20 && p.hi_pct == 85 && p.lo_pct == 75 && p.tick_s == 10 &&
          p.scope.kind == CUVS_SCOPE_NONE && p.pin_max_pct == 50 &&
          p.f_cap_pct == 50 && p.window_min == 30 && p.floor_pct == 10 &&
          p.soft_evict == 1 && p.self_replace == 1 && p.usage_file == 1 &&
          p.f_fixed == 0, "defaults (decision UC-1, C6)");

    CHECK(cuvs_reclaim_flag_set(&p, "reclaim-mode", "bogus", err, sizeof err) != 0, "bad mode");
    CHECK(cuvs_reclaim_flag_set(&p, "reclaim-free-max-pct", "0", err, sizeof err) != 0, "cap 0");
    CHECK(cuvs_reclaim_flag_set(&p, "reclaim-free-max-pct", "101", err, sizeof err) != 0, "cap 101");
    CHECK(cuvs_reclaim_flag_set(&p, "reclaim-free-window-min", "1441", err, sizeof err) != 0, "window");
    CHECK(cuvs_reclaim_flag_set(&p, "reclaim-floor-pct", "-1", err, sizeof err) != 0, "floor neg");
    CHECK(cuvs_reclaim_flag_set(&p, "reclaim-tick-s", "0", err, sizeof err) != 0, "tick 0");
    CHECK(cuvs_reclaim_flag_set(&p, "evict-min-idle-s", "", err, sizeof err) != 0, "empty");
    CHECK(cuvs_reclaim_flag_set(&p, "evict-hit-cap", "1001", err, sizeof err) != 0, "cap>1000");
    CHECK(cuvs_reclaim_flag_set(&p, "build-evict-scope", "idle:-1", err, sizeof err) != 0, "idle neg");
    CHECK(cuvs_reclaim_flag_set(&p, "build-evict-scope", "idle:1.5", err, sizeof err) != 0, "idle frac");
    CHECK(cuvs_reclaim_flag_set(&p, "build-evict-scope", "idle:", err, sizeof err) != 0, "idle empty");
    CHECK(cuvs_reclaim_flag_set(&p, "load-soft-evict", "yes", err, sizeof err) != 0, "onoff");
    CHECK(cuvs_reclaim_flag_set(&p, "build-evict-scope", "idle:15", err, sizeof err) == 0 &&
          p.scope.kind == CUVS_SCOPE_IDLE && p.scope.idle_min == 15, "idle:15");
    CHECK(cuvs_reclaim_flag_set(&p, "load-soft-evict", "off", err, sizeof err) == 0 &&
          p.soft_evict == 0, "soft off");
    CHECK(p.peak_meter == 1 &&
          cuvs_reclaim_flag_set(&p, "rmm-peak-meter", "off", err, sizeof err) == 0 &&
          p.peak_meter == 0 && cuvs_reclaim_flag_known("rmm-peak-meter") &&
          !cuvs_reclaim_name_is_runtime("rmm-peak-meter") &&
          cuvs_reclaim_flag_set(&p, "rmm-peak-meter", "maybe", err, sizeof err) != 0,
          "KB-17 --rmm-peak-meter on|off, restart-only");
    p.lo_pct = 90;
    CHECK(cuvs_reclaim_params_check(&p, err, sizeof err) != 0, "low > high rejected");

    /* Runtime allow-list. */
    cuvs_reclaim_params_default(&p);
    CHECK(cuvs_reclaim_runtime_set(&p, "reclaim-mode", "usage", err, sizeof err) == 0 &&
          p.mode == CUVS_RECLAIM_USAGE, "runtime mode");
    CHECK(cuvs_reclaim_runtime_set(&p, "evict-hit-cap", "5", err, sizeof err) == 0 &&
          p.hits_cap == 5, "runtime KB-10");
    CHECK(cuvs_reclaim_runtime_set(&p, "reclaim-low-pct", "90", err, sizeof err) != 0 &&
          p.lo_pct == 75, "runtime low>high rejected, value kept");
    CHECK(cuvs_reclaim_runtime_set(&p, "reclaim-high-pct", "101", err, sizeof err) != 0 &&
          p.hi_pct == 85, "runtime out of range kept");
    CHECK(cuvs_reclaim_runtime_set(&p, "reclaim-free-max-pct", "40", err, sizeof err) != 0 &&
          p.f_cap_pct == 50, "accounting knob not runtime");
    CHECK(cuvs_reclaim_runtime_set(&p, "reclaim-floor-pct", "5", err, sizeof err) != 0,
          "floor not runtime");
    CHECK(cuvs_reclaim_runtime_set(&p, "no-such", "1", err, sizeof err) != 0, "unknown");
    const char *rt[] = { "evict-min-idle-s", "evict-hit-bonus-s", "evict-hit-cap",
                         "reclaim-high-pct", "reclaim-low-pct", "reclaim-tick-s",
                         "reclaim-mode", "reclaim-free-bytes" };
    for (size_t i = 0; i < sizeof(rt) / sizeof(rt[0]); i++)
        CHECK(cuvs_reclaim_name_is_runtime(rt[i]), "runtime name %s", rt[i]);
    const char *nrt[] = { "reclaim-free-max-pct", "reclaim-free-window-min",
                          "reclaim-floor-pct", "pin-max-pct", "build-evict-scope",
                          "load-soft-evict", "reindex-self-replace", "usage-stats-file" };
    for (size_t i = 0; i < sizeof(nrt) / sizeof(nrt[0]); i++)
        CHECK(!cuvs_reclaim_name_is_runtime(nrt[i]), "restart-only %s", nrt[i]);
}

/* ------------------------------------------------------ CK-36 (e) file */
static void
test_usage_file(void)
{
    CuvsUsageStat s[3] = {
        { 1, 100, 5000, 7, 0 }, { 1, 200, 9000, 1, 1 }, { 2, 50, 0, 0, 0 },
    };
    char buf[4096], why[128];
    CuvsUsageStat got[8]; int n;
    long len = cuvs_usage_stats_format(s, 3, buf, sizeof buf);
    CHECK(len > 0, "format");
    CHECK(cuvs_usage_stats_parse(buf, (size_t) len, got, 8, &n, why, sizeof why) == 0 &&
          n == 3 && got[1].index_oid == 200 && got[1].pinned == 1 &&
          got[0].last_use == 5000 && got[0].hits == 7, "round trip (%s)", why);

    /* Corruptions: each must fail closed with n = 0. */
    struct { const char *name; long cut; } cuts[] = {
        { "empty", 0 }, { "header only", 23 }, { "mid entry", len / 2 },
        { "no trailer newline", len - 1 },
    };
    for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++)
    {
        n = 99;
        CHECK(cuvs_usage_stats_parse(buf, (size_t) cuts[i].cut, got, 8, &n, why,
                                     sizeof why) != 0 && n == 0,
              "corrupt: %s", cuts[i].name);
    }
    char bad[4096];
    memcpy(bad, buf, (size_t) len);
    /* Overwrite a prefix in place keeping the format valid ("5000" ->
     * "6000"): only the crc can catch it. */
    char *at = strstr(bad, " 5000 ");
    if (at) at[1] = '6';
    CHECK(cuvs_usage_stats_parse(bad, (size_t) len, got, 8, &n, why, sizeof why) != 0,
          "crc catches overwritten prefix");
    memcpy(bad, buf, (size_t) len);
    bad[20] = '2';                             /* "v1" -> "v2" */
    CHECK(cuvs_usage_stats_parse(bad, (size_t) len, got, 8, &n, why, sizeof why) != 0,
          "unknown version");
    const char *huge = "pg_cuvs-usage-stats v1\ncount 9999999\n";
    CHECK(cuvs_usage_stats_parse(huge, strlen(huge), got, 8, &n, why, sizeof why) != 0,
          "absurd count");
    CHECK(cuvs_usage_stats_parse(buf, (size_t) len, got, 2, &n, why, sizeof why) != 0,
          "count above caller max");
    char tail[4200];
    memcpy(tail, buf, (size_t) len); memcpy(tail + len, "x\n", 2);
    CHECK(cuvs_usage_stats_parse(tail, (size_t) len + 2, got, 8, &n, why, sizeof why) != 0,
          "trailing bytes");
}

static void
test_startup_order(void)
{
    /* keys given in reverse and shuffled order: result must not depend on it */
    CuvsLoadKey k1[4] = { {1, 400}, {1, 300}, {1, 200}, {1, 100} };
    CuvsLoadKey k2[4] = { {1, 200}, {1, 400}, {1, 100}, {1, 300} };
    CuvsUsageStat st[3] = {
        { 1, 300, 9000, 1, 0 },   /* hottest */
        { 1, 100, 5000, 9, 0 },
        { 1, 999, 9999, 9, 0 },   /* no such index: ignored */
    };
    int p1[4], p2[4];
    cuvs_startup_order(k1, 4, st, 3, p1);
    cuvs_startup_order(k2, 4, st, 3, p2);
    uint32_t e[4] = { 300, 100, 200, 400 };
    int ok1 = 1, ok2 = 1;
    for (int i = 0; i < 4; i++)
    {
        ok1 &= k1[p1[i]].index_oid == e[i];
        ok2 &= k2[p2[i]].index_oid == e[i];
    }
    CHECK(ok1 && ok2, "hot first then OID, independent of enumeration");

    cuvs_startup_order(k1, 4, NULL, -1, p1);
    uint32_t e2[4] = { 100, 200, 300, 400 };
    int ok = 1;
    for (int i = 0; i < 4; i++)
        ok &= k1[p1[i]].index_oid == e2[i];
    CHECK(ok, "no/corrupt file -> OID order");

    /* Tie on last_use -> hits desc. */
    CuvsUsageStat tie[2] = { { 1, 400, 7000, 2, 0 }, { 1, 200, 7000, 5, 0 } };
    cuvs_startup_order(k1, 4, tie, 2, p1);
    CHECK(k1[p1[0]].index_oid == 200 && k1[p1[1]].index_oid == 400, "hits tie-break");
}

/* ------------------------------------------------------------ sidecars */
static void
touch(const char *dir, const char *name)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (f) fclose(f);
}

static void
test_sidecar(void)
{
    char dir[] = "/tmp/cache_policy_XXXXXX";
    if (!mkdtemp(dir)) { CHECK(0, "mkdtemp"); return; }
    touch(dir, "1_12.delta");        /* only .delta, zero bytes */
    touch(dir, "1_13.tombstone");
    touch(dir, "1_14.delta");
    touch(dir, "1_14.tombstone");
    touch(dir, "1_15.stale");
    touch(dir, "1_123.delta");       /* prefix trap for idx 12 / 1_23 */
    touch(dir, "2_16.delta");        /* other db */
    CHECK(cuvs_entry_sidecar_kind(dir, 1, 12) == CUVS_SIDECAR_DELTA, "delta only");
    CHECK(cuvs_entry_sidecar_kind(dir, 1, 13) == CUVS_SIDECAR_TOMBSTONE, "tombstone only");
    CHECK(cuvs_entry_sidecar_kind(dir, 1, 14) == CUVS_SIDECAR_BOTH, "both");
    CHECK(cuvs_entry_sidecar_kind(dir, 1, 15) == CUVS_SIDECAR_NONE, ".stale is none");
    CHECK(cuvs_entry_sidecar_kind(dir, 1, 1) == CUVS_SIDECAR_NONE, "prefix 1_1 vs 1_123");
    CHECK(cuvs_entry_sidecar_kind(dir, 12, 3) == CUVS_SIDECAR_NONE, "12_3 vs 1_23");
    CHECK(cuvs_entry_sidecar_kind(dir, 1, 16) == CUVS_SIDECAR_NONE, "other db");
    CHECK(cuvs_entry_sidecar_kind(dir, 1, 99) == CUVS_SIDECAR_NONE, "none");

    CHECK(cuvs_reclaim_skip_sidecar(CUVS_SIDECAR_DELTA, 1, 0), "skip: cagra unsaved");
    CHECK(!cuvs_reclaim_skip_sidecar(CUVS_SIDECAR_DELTA, 1, 1), "no skip: file_current");
    CHECK(!cuvs_reclaim_skip_sidecar(CUVS_SIDECAR_BOTH, 0, 0), "no skip: non-cagra");
    CHECK(!cuvs_reclaim_skip_sidecar(CUVS_SIDECAR_NONE, 1, 0), "no skip: no sidecar");

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0) { /* best effort */ }
}

int
main(void)
{
    test_protect_and_eligible();
    test_order();
    test_pick_build_scope();
    test_pick_load_soft();
    test_self_replace();
    test_free_target_fences();
    test_reclaim_goal();
    test_wait_hint();
    test_params();
    test_usage_file();
    test_startup_order();
    test_sidecar();
    printf("test_cache_policy: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
