/* I4 / ADR-088 pure decisions (CK-9 parsing, CK-11 (h)(l), CK-28 AR6/AR7
 * boundaries, CK-3b lw_pending term, C8 NOTICE/log text). No PG, CUDA or
 * daemon state. Expected values are computed here from the plan's formulas,
 * never from the product helpers, and the margin is taken from the macro or
 * non-default values so the LW-8 one-line commit does not break the table. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "cuvs_lw_policy.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint64_t
ref_shape(uint64_t n, uint64_t dim, uint64_t igd)
{
    uint64_t ld = (dim + 3) / 4 * 4;
    uint64_t sort_term = 4 * n * ld + 4 * n * igd;
    uint64_t prune_term = n * (5 * igd + 4);
    return sort_term > prune_term ? sort_term : prune_term;
}

static uint64_t
ref_ub(uint64_t shape, uint64_t margin)
{
    return (shape * margin + 999) / 1000;
}

static void
test_shape_and_ub(void)
{
    const uint32_t dims[] = {96, 128, 768, 1536};
    const int igds[] = {64, 128, 256};
    const uint32_t margins[] = {CUVS_MANAGED_LW_MARGIN_PERMILLE_DEFAULT, 1000, 1251, 1300};
    const int64_t n = 250000;
    for (size_t d = 0; d < 4; d++)
        for (size_t g = 0; g < 3; g++)
        {
            uint64_t want = ref_shape((uint64_t)n, dims[d], (uint64_t)igds[g]);
            CHECK(cuvs_lw_shape_bytes(n, dims[d], igds[g]) == want);
            for (size_t m = 0; m < 4; m++)
                CHECK(cuvs_lw_ub_bytes(want, margins[m]) == ref_ub(want, margins[m]));
        }
    /* Plan C3 worked example (999,000 x 768 x igd 128). */
    CHECK(cuvs_lw_shape_bytes(999000, 768, 128) == 3580416000ull);
    /* Prune term dominates when igd + 4 > 4 ld (dim 4, igd 256). */
    CHECK(cuvs_lw_shape_bytes(1000, 4, 256) == 1000ull * (5 * 256 + 4));
    /* dim % 4: padded stride ld (safe direction). */
    CHECK(cuvs_lw_shape_bytes(1000, 97, 64) == 4ull * 1000 * 100 + 4ull * 1000 * 64);
    /* igd <= 0 means the wrapper default 128. */
    CHECK(cuvs_lw_shape_bytes(1000, 128, 0) == cuvs_lw_shape_bytes(1000, 128, 128));
    CHECK(cuvs_lw_shape_bytes(0, 128, 128) == 0);
    CHECK(cuvs_lw_shape_bytes(-5, 128, 128) == 0);
    /* Ceil boundary: 1001 B x 1250 = 1,251,250 -> 1252 (not 1251). */
    CHECK(cuvs_lw_ub_bytes(1001, 1250) == 1252);
    CHECK(cuvs_lw_ub_bytes(1000, 1250) == 1250);
    /* Saturation refuses rather than wraps. */
    CHECK(cuvs_lw_shape_bytes(INT64_MAX, 1536, 256) == CUVS_LW_SATURATED);
    CHECK(cuvs_lw_ub_bytes(CUVS_LW_SATURATED, 1250) == CUVS_LW_SATURATED);
    CHECK(cuvs_lw_ub_bytes(CUVS_LW_SATURATED / 1000, 100000) == CUVS_LW_SATURATED);
}

static CuvsLwAdmitInput
base_input(int overcommit)
{
    CuvsLwAdmitInput in;
    memset(&in, 0, sizeof(in));
    in.policy = CUVS_LW_OVERFLOW_MANAGED;
    in.overcommit = overcommit;
    in.n_m = 10000;
    in.reserved = 7000;   /* slack 3000 already held by the pool */
    in.pool_used = 4000;
    in.lw_ub = 5000;
    in.lw_pending = 2000;
    return in;
}

static void
test_admission_boundaries(void)
{
    /* phys_need = (10000 - 3000) + 5000 + 2000 = 14000 */
    CuvsLwAdmitInput in = base_input(0);
    CuvsLwAdmitResult r;
    in.phys_free = 14000 - 1;
    r = cuvs_lw_admit(&in);
    CHECK(r.phys_need == 14000 && r.spill_est == 1 && r.reason == CUVS_ADMISSION_AR6);
    in.phys_free = 14000;
    r = cuvs_lw_admit(&in);
    CHECK(r.spill_est == 0 && r.reason == CUVS_ADMISSION_NONE);
    in.phys_free = 14000 + 1;
    CHECK(cuvs_lw_admit(&in).reason == CUVS_ADMISSION_NONE);

    /* Overcommit on: spill is allowed up to the host guard, AR7 beyond it. */
    in = base_input(1);
    in.phys_free = 4000;            /* spill_est = 10000 */
    in.host_guard = 10000 - 1;
    r = cuvs_lw_admit(&in);
    CHECK(r.spill_est == 10000 && r.reason == CUVS_ADMISSION_AR7);
    in.host_guard = 10000;
    CHECK(cuvs_lw_admit(&in).reason == CUVS_ADMISSION_NONE);
    in.host_guard = 10000 + 1;
    CHECK(cuvs_lw_admit(&in).reason == CUVS_ADMISSION_NONE);
    /* Unreadable cgroup / cgroup v1: host_guard 0 makes any spill AR7. */
    in.host_guard = 0;
    in.phys_free = 14000 - 1;
    CHECK(cuvs_lw_admit(&in).reason == CUVS_ADMISSION_AR7);
    in.phys_free = 14000;
    CHECK(cuvs_lw_admit(&in).reason == CUVS_ADMISSION_NONE);

    /* Policy error never refuses for large_workspace. */
    in = base_input(0);
    in.policy = CUVS_LW_OVERFLOW_ERROR;
    in.phys_free = 0;
    r = cuvs_lw_admit(&in);
    CHECK(r.reason == CUVS_ADMISSION_NONE && r.phys_need == 0);

    /* Pool slack covering N_m leaves only the large_workspace terms. */
    in = base_input(0);
    in.reserved = 20000; in.pool_used = 5000;
    in.phys_free = 5000 + 2000 - 1;
    CHECK(cuvs_lw_admit(&in).reason == CUVS_ADMISSION_AR6);
    /* reserved < used (counter skew) is treated as zero slack, not a wrap. */
    in.reserved = 1; in.pool_used = 2;
    in.phys_free = 10000 + 5000 + 2000;
    CHECK(cuvs_lw_admit(&in).phys_need == 17000);

    /* CK-3b: the second build's phys_need contains the first build's lw_ub. */
    {
        uint64_t ub1 = 3000, ub2 = 4000;
        CuvsLwAdmitInput a = base_input(0), b;
        a.n_m = 0; a.reserved = a.pool_used = 0; a.lw_ub = ub1; a.lw_pending = 0;
        a.phys_free = ub1 + ub2 - 1;
        CHECK(cuvs_lw_admit(&a).reason == CUVS_ADMISSION_NONE);
        b = a; b.lw_ub = ub2; b.lw_pending = ub1;
        CHECK(cuvs_lw_admit(&b).reason == CUVS_ADMISSION_AR6);
        b.phys_free = ub1 + ub2;
        CHECK(cuvs_lw_admit(&b).reason == CUVS_ADMISSION_NONE);
        b.phys_free = ub1 + ub2 - 1; b.lw_pending = 0;   /* after release */
        CHECK(cuvs_lw_admit(&b).reason == CUVS_ADMISSION_NONE);
    }
    /* Saturated estimate always refuses (never wraps to a small need). */
    in = base_input(1);
    in.lw_ub = CUVS_LW_SATURATED; in.phys_free = UINT64_MAX / 4; in.host_guard = UINT64_MAX / 4;
    CHECK(cuvs_lw_admit(&in).reason == CUVS_ADMISSION_AR7);
}

static void
test_parsers(void)
{
    uint32_t policy = 99, margin = 7;
    int on = 9, adj = 9;
    CHECK(cuvs_lw_parse_overflow("error", &policy) == 0 && policy == CUVS_LW_OVERFLOW_ERROR);
    CHECK(cuvs_lw_parse_overflow("managed", &policy) == 0 && policy == CUVS_LW_OVERFLOW_MANAGED);
    CHECK(cuvs_lw_parse_overflow("Managed", &policy) < 0 && policy == CUVS_LW_OVERFLOW_MANAGED);
    CHECK(cuvs_lw_parse_overflow("", &policy) < 0 && cuvs_lw_parse_overflow(NULL, &policy) < 0);
    CHECK(cuvs_lw_parse_on_off("on", &on) == 0 && on == 1);
    CHECK(cuvs_lw_parse_on_off("off", &on) == 0 && on == 0);
    CHECK(cuvs_lw_parse_on_off("1", &on) < 0 && cuvs_lw_parse_on_off("yes", &on) < 0 && on == 0);
    CHECK(cuvs_lw_parse_margin("999", &margin) < 0 && margin == 7);
    CHECK(cuvs_lw_parse_margin("1000", &margin) == 0 && margin == 1000);
    CHECK(cuvs_lw_parse_margin("100000", &margin) == 0 && margin == 100000);
    CHECK(cuvs_lw_parse_margin("100001", &margin) < 0 && margin == 100000);
    {
        const char *bad[] = {"", "-1000", "+1000", " 1000", "1000x", "1e3", "18446744073709551617"};
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
            CHECK(cuvs_lw_parse_margin(bad[i], &margin) < 0);
        CHECK(cuvs_lw_parse_margin(NULL, &margin) < 0);
    }
    CHECK(cuvs_lw_parse_oom_score_adj("-1", &adj) < 0 && adj == 9);
    CHECK(cuvs_lw_parse_oom_score_adj("0", &adj) == 0 && adj == 0);
    CHECK(cuvs_lw_parse_oom_score_adj("1000", &adj) == 0 && adj == 1000);
    CHECK(cuvs_lw_parse_oom_score_adj("1001", &adj) < 0 && adj == 1000);
    CHECK(cuvs_lw_parse_oom_score_adj("", &adj) < 0);
    /* Defaults (UC-3): managed, overcommit on, oom_score_adj 500. */
    CHECK(CUVS_CAGRA_BUILD_OVERFLOW_DEFAULT == CUVS_LW_OVERFLOW_MANAGED);
    CHECK(CUVS_MANAGED_OVERCOMMIT_DEFAULT == 1);
    CHECK(CUVS_OOM_SCORE_ADJ_DEFAULT == 500);
    CHECK(CUVS_MANAGED_LW_MARGIN_PERMILLE_DEFAULT >= CUVS_MANAGED_LW_MARGIN_PERMILLE_MIN);
}

static void
test_oom_score_adj(void)
{
    char buf[CUVS_LW_OOM_RESULT_LEN];
    CHECK(cuvs_oom_adj_action(0, 900) == CUVS_OOM_ADJ_SKIP_ZERO);
    CHECK(cuvs_oom_adj_action(0, -1000) == CUVS_OOM_ADJ_SKIP_ZERO);
    CHECK(cuvs_oom_adj_action(500, 800) == CUVS_OOM_ADJ_KEEP);   /* never lower */
    CHECK(cuvs_oom_adj_action(500, 500) == CUVS_OOM_ADJ_KEEP);
    CHECK(cuvs_oom_adj_action(500, 499) == CUVS_OOM_ADJ_WRITE);
    CHECK(cuvs_oom_adj_action(500, -1000) == CUVS_OOM_ADJ_WRITE);

    cuvs_oom_adj_describe(CUVS_OOM_ADJ_WRITE, 500, 0, 0, 1, buf, sizeof(buf));
    CHECK(strcmp(buf, "oom_score_adj=500 applied") == 0);
    cuvs_oom_adj_describe(CUVS_OOM_ADJ_KEEP, 500, 800, 0, 1, buf, sizeof(buf));
    CHECK(strcmp(buf, "oom_score_adj kept current 800") == 0);
    cuvs_oom_adj_describe(CUVS_OOM_ADJ_SKIP_ZERO, 0, -1, 0, 1, buf, sizeof(buf));
    CHECK(strcmp(buf, "oom_score_adj not applied: value 0") == 0);
    cuvs_oom_adj_describe(CUVS_OOM_ADJ_WRITE, 500, 0, EACCES, 1, buf, sizeof(buf));
    CHECK(strcmp(buf, "oom_score_adj not applied: permission denied") == 0);
    cuvs_oom_adj_describe(CUVS_OOM_ADJ_WRITE, 500, 0, EPERM, 1, buf, sizeof(buf));
    CHECK(strcmp(buf, "oom_score_adj not applied: permission denied") == 0);
    cuvs_oom_adj_describe(CUVS_OOM_ADJ_WRITE, 500, 0, 0, 0, buf, sizeof(buf));
    CHECK(strcmp(buf, "oom_score_adj not applied: not Linux") == 0);
    cuvs_oom_adj_describe(CUVS_OOM_ADJ_WRITE, 500, 0, ENOENT, 1, buf, sizeof(buf));
    CHECK(strncmp(buf, "oom_score_adj not applied: ", 27) == 0 && strlen(buf) > 27);
}

static void
test_notice_and_spill(void)
{
    char buf[CUVS_ADMISSION_ERR_OUT_BYTES];
    const uint64_t MiB = 1024u * 1024u;
    CHECK(cuvs_lw_spill_flag(CUVS_LW_OVERFLOW_MANAGED, 3000 * MiB, 2999 * MiB) == 1);
    CHECK(cuvs_lw_spill_flag(CUVS_LW_OVERFLOW_MANAGED, 3000 * MiB, 3000 * MiB) == 0);
    CHECK(cuvs_lw_spill_flag(CUVS_LW_OVERFLOW_MANAGED, 0, 0) == 0);
    CHECK(cuvs_lw_spill_flag(CUVS_LW_OVERFLOW_ERROR, 3000 * MiB, 0) == 0);
    CHECK(cuvs_lw_spill_notice(1, 3000 * MiB + 5, 2999 * MiB, buf, sizeof(buf)) == 1);
    CHECK(strcmp(buf, "pg_cuvs: build used managed memory spill (large_workspace peak 3000 MiB, "
                      "physical free 2999 MiB); the build may be slower than usual") == 0);
    strcpy(buf, "stale");
    CHECK(cuvs_lw_spill_notice(0, 3000 * MiB, 0, buf, sizeof(buf)) == 0 && buf[0] == '\0');
    CHECK(cuvs_lw_mib(UINT64_MAX) == CUVS_ADMISSION_SCALAR_MAX);
}

static void
test_reject_fields(void)
{
    const uint64_t MiB = 1024u * 1024u;
    char buf[CUVS_ADMISSION_ERR_OUT_BYTES];
    CuvsAdmissionWorkspaceInput in;
    int len;
    memset(&in, 0, sizeof(in));
    in.needed = 2048 * MiB + 1; in.lw_ub = 4096 * MiB; in.lw_pending = 512 * MiB;
    in.phys_free = 1000 * MiB; in.host_guard = 7 * MiB;
    len = cuvs_lw_reject_fields(&in, CUVS_ADMISSION_AR6, buf, sizeof(buf));
    CHECK(len > 0 && strcmp(buf, "AR6 n=2048 l=4096 y=512 u=1000") == 0);
    len = cuvs_lw_reject_fields(&in, CUVS_ADMISSION_AR7, buf, sizeof(buf));
    CHECK(len > 0 && strcmp(buf, "AR7 n=2048 l=4096 y=512 g=7") == 0);
    CHECK(cuvs_lw_reject_fields(&in, CUVS_ADMISSION_AR1, buf, sizeof(buf)) < 0);
    /* Saturated maximum stays far inside the 512 B payload. */
    in.needed = in.lw_ub = in.lw_pending = in.phys_free = in.host_guard = UINT64_MAX;
    len = cuvs_lw_reject_fields(&in, CUVS_ADMISSION_AR6, buf, sizeof(buf));
    CHECK(strcmp(buf, "AR6 n=99999999 l=99999999 y=99999999 u=99999999") == 0);
    CHECK(len > 0 && (size_t)len < CUVS_ADMISSION_ERR_OUT_BYTES);
}

static void
test_shape_class(void)
{
    CHECK(cuvs_lw_shape_checked(999000, 768, 128));
    CHECK(cuvs_lw_shape_checked(250000, 128, 256));
    CHECK(cuvs_lw_shape_checked(1000, 128, 0));          /* igd default 128 */
    CHECK(!cuvs_lw_shape_checked(999001, 768, 128));
    CHECK(!cuvs_lw_shape_checked(1000, 127, 128));
    CHECK(!cuvs_lw_shape_checked(1000, 769, 128));
    CHECK(!cuvs_lw_shape_checked(1000, 768, 127));
    CHECK(!cuvs_lw_shape_checked(1000, 768, 257));
    CHECK(!cuvs_lw_shape_checked(1000, 1536, 128));
}

/* The shared I3-A hook delegates to the same rule (cuvs_util.c). */
static void
test_hook(void)
{
    CuvsAdmissionWorkspaceInput ws;
    memset(&ws, 0, sizeof(ws));
    ws.managed = 1; ws.needed = 10000; ws.reserved = 7000; ws.pool_used = 4000;
    ws.lw_ub = 5000; ws.lw_pending = 2000; ws.phys_free = 14000 - 1;
    CHECK(cuvs_admission_workspace_check(&ws) == CUVS_ADMISSION_AR6);
    ws.phys_free = 14000;
    CHECK(cuvs_admission_workspace_check(&ws) == CUVS_ADMISSION_NONE);
    ws.overcommit = 1; ws.phys_free = 4000; ws.host_guard = 10000 - 1;
    CHECK(cuvs_admission_workspace_check(&ws) == CUVS_ADMISSION_AR7);
    ws.host_guard = 10000;
    CHECK(cuvs_admission_workspace_check(&ws) == CUVS_ADMISSION_NONE);
    ws.managed = 0; ws.phys_free = 0; ws.host_guard = 0; ws.overcommit = 0;
    CHECK(cuvs_admission_workspace_check(&ws) == CUVS_ADMISSION_NONE);
    CHECK(cuvs_admission_workspace_check(NULL) == CUVS_ADMISSION_NONE);
}

int
main(void)
{
    test_hook();
    test_shape_class();
    test_shape_and_ub();
    test_admission_boundaries();
    test_parsers();
    test_oom_score_adj();
    test_notice_and_spill();
    test_reject_fields();
    if (failures)
    {
        fprintf(stderr, "test_lw_policy: %d failure(s)\n", failures);
        return 1;
    }
    printf("PASS lw policy: L_shape/lw_ub table, AR6/AR7 boundaries, lw_pending term, "
           "knob parsers, oom_score_adj never-lower, spill NOTICE, AR6/AR7 err_out fields\n");
    return 0;
}
