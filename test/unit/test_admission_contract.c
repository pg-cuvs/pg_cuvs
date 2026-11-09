/*
 * CPU unit tests for the I3 admission estimate in cuvs_admission_contract.h.
 * The same file is compiled as C11 and as C++17 because the header is shared
 * by the daemon (C), the backend (C) and the wrapper (C++/CUDA).
 *
 * Expected values come from v6 r18 C3 (the reservation formula, the rows of
 * the per-rho admission table, the L40S example) and CK-30 (dim%4 stride).
 */
#include "cuvs_admission_contract.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK_EQ(label, got, want)                                          \
    do {                                                                    \
        uint64_t got_ = (uint64_t)(got), want_ = (uint64_t)(want);          \
        if (got_ != want_) {                                                \
            fprintf(stderr, "FAIL %s: got %llu want %llu\n", (label),       \
                    (unsigned long long)got_, (unsigned long long)want_);   \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

#define CHECK(label, cond)                                                  \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s\n", (label));                          \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

static const uint64_t SAT = CUVS_ADMISSION_BYTES_SATURATED;

/* v5 estimate_vram_bytes for dim % 4 == 0: n * (dim*4 + gd*4). */
static uint64_t
v5_final(uint64_t n, uint64_t dim, uint64_t gd)
{
    return n * (dim * 4u + gd * 4u);
}

static uint64_t
reserve(int64_t n, int32_t dim, int32_t gd, uint32_t rho)
{
    return cuvs_admission_needed_bytes(
        cuvs_admission_estimate_final_bytes(n, dim, gd), 0, rho, 0);
}

static void
test_final_matches_v5_when_dim_is_aligned(void)
{
    /* Arrange / Act / Assert: the Q shape from C3. */
    CHECK_EQ("final 999000x768 gd32",
             cuvs_admission_estimate_final_bytes(999000, 768, 32),
             UINT64_C(3196800000));
    CHECK_EQ("final 1000x128 gd64",
             cuvs_admission_estimate_final_bytes(1000, 128, 64),
             v5_final(1000, 128, 64));
    CHECK_EQ("final 7x4 gd1", cuvs_admission_estimate_final_bytes(7, 4, 1),
             v5_final(7, 4, 1));
}

static void
test_final_uses_aligned_stride(void)
{
    /* CK-30: dim 766, 767 -> ld 768; dim 770 -> ld 772; dim 1..3 -> ld 4. */
    CHECK_EQ("dim 766", cuvs_admission_estimate_final_bytes(10, 766, 32),
             v5_final(10, 768, 32));
    CHECK_EQ("dim 767", cuvs_admission_estimate_final_bytes(10, 767, 32),
             v5_final(10, 768, 32));
    CHECK_EQ("dim 770", cuvs_admission_estimate_final_bytes(10, 770, 32),
             v5_final(10, 772, 32));
    CHECK_EQ("dim 1", cuvs_admission_estimate_final_bytes(3, 1, 8),
             v5_final(3, 4, 8));
    CHECK_EQ("dim 3", cuvs_admission_estimate_final_bytes(3, 3, 8),
             v5_final(3, 4, 8));
    CHECK_EQ("dim 5", cuvs_admission_estimate_final_bytes(3, 5, 8),
             v5_final(3, 8, 8));
}

static void
test_final_default_degree(void)
{
    /* graph_degree <= 0 means the cuVS default 64, as estimate_vram_bytes. */
    CHECK_EQ("gd 0", cuvs_admission_estimate_final_bytes(5, 128, 0),
             v5_final(5, 128, 64));
    CHECK_EQ("gd -1", cuvs_admission_estimate_final_bytes(5, 128, -1),
             v5_final(5, 128, 64));
}

static void
test_final_edges_fail_closed(void)
{
    CHECK_EQ("n 0", cuvs_admission_estimate_final_bytes(0, 768, 32), 0);
    CHECK_EQ("n -1", cuvs_admission_estimate_final_bytes(-1, 768, 32), SAT);
    CHECK_EQ("dim 0", cuvs_admission_estimate_final_bytes(10, 0, 32), SAT);
    CHECK_EQ("dim -4", cuvs_admission_estimate_final_bytes(10, -4, 32), SAT);
    CHECK_EQ("n huge", cuvs_admission_estimate_final_bytes(INT64_MAX, 768, 32),
             SAT);
    CHECK_EQ("dim max", cuvs_admission_estimate_final_bytes(1, INT32_MAX, 32),
             (uint64_t)4 * ((uint64_t)INT32_MAX + 1u) + 128u);
    /* Exactly at the saturation boundary and one row past it. */
    {
        uint64_t row = 4u * 4u + 4u * 64u; /* dim 4, gd 64 */
        int64_t fit = (int64_t)(SAT / row);
        CHECK_EQ("n at SAT", cuvs_admission_estimate_final_bytes(fit, 4, 64),
                 (uint64_t)fit * row);
        CHECK_EQ("n past SAT",
                 cuvs_admission_estimate_final_bytes(fit + 1, 4, 64), SAT);
    }
}

static void
test_needed_scratch_rounds_up(void)
{
    CHECK_EQ("rho 0", cuvs_admission_needed_bytes(12345, 0, 0, 0), 12345);
    CHECK_EQ("rho 1000", cuvs_admission_needed_bytes(12345, 0, 1000, 0), 24690);
    CHECK_EQ("ceil 1x1", cuvs_admission_needed_bytes(1, 0, 1, 0), 2);
    CHECK_EQ("ceil 999x1", cuvs_admission_needed_bytes(999, 0, 1, 0), 1000);
    CHECK_EQ("exact 1000x1", cuvs_admission_needed_bytes(1000, 0, 1, 0), 1001);
    CHECK_EQ("ceil 1001x1", cuvs_admission_needed_bytes(1001, 0, 1, 0), 1003);
    CHECK_EQ("final 0", cuvs_admission_needed_bytes(0, 0, 7700, 0), 0);
    CHECK_EQ("rho max", cuvs_admission_needed_bytes(1000, 0,
             CUVS_ADMISSION_RHO_PERMILLE_MAX, 0), 1000u + 100000u);
    CHECK_EQ("rho past max", cuvs_admission_needed_bytes(1000, 0,
             CUVS_ADMISSION_RHO_PERMILLE_MAX + 1u, 0), SAT);
}

static void
test_needed_adds_h2d_and_margin(void)
{
    CHECK_EQ("h2d", cuvs_admission_needed_bytes(1000, 70, 0, 0), 1070);
    CHECK_EQ("margin", cuvs_admission_needed_bytes(1000, 0, 0, 9), 1009);
    CHECK_EQ("all", cuvs_admission_needed_bytes(1000, 70, 500, 9), 1579);
    /* The scratch term scales final only, never corpus_h2d or margin. */
    CHECK_EQ("rho excludes h2d", cuvs_admission_needed_bytes(0, 1000, 1000, 0),
             1000);
}

static void
test_needed_saturates(void)
{
    CHECK_EQ("final SAT", cuvs_admission_needed_bytes(SAT, 0, 0, 0), SAT);
    CHECK_EQ("final SAT + h2d", cuvs_admission_needed_bytes(SAT, 1, 0, 0), SAT);
    CHECK_EQ("final SAT-1 + h2d 5",
             cuvs_admission_needed_bytes(SAT - 1u, 5, 0, 0), SAT);
    CHECK_EQ("final SAT-1 + margin 5",
             cuvs_admission_needed_bytes(SAT - 1u, 0, 0, 5), SAT);
    CHECK_EQ("final max", cuvs_admission_needed_bytes(UINT64_MAX, 0, 0, 0), SAT);
    CHECK_EQ("h2d max", cuvs_admission_needed_bytes(0, UINT64_MAX, 0, 0), SAT);
    CHECK_EQ("margin max", cuvs_admission_needed_bytes(0, 0, 0, UINT64_MAX), SAT);
    CHECK_EQ("scratch overflow",
             cuvs_admission_needed_bytes(SAT / 2u, 0, 100000u, 0), SAT);
    /* Large but representable: no false saturation from final*rho. */
    CHECK_EQ("no false SAT",
             cuvs_admission_needed_bytes(UINT64_C(1) << 50, 0, 7700u, 0),
             (UINT64_C(1) << 50) + ((UINT64_C(1) << 50) / 1000u) * 7700u +
                 (((UINT64_C(1) << 50) % 1000u) * 7700u + 999u) / 1000u);
}

static void
test_c3_admission_table(void)
{
    /* C3 table, U0 = 0, dim 768, gd 32: max n with N <= limit. */
    const uint64_t quota = UINT64_C(42547937280);
    const uint64_t logical = UINT64_C(76585893888);
    struct { uint32_t rho; int64_t q_rows; int64_t l_rows; } rows[] = {
        {0, 13296230, 23933091},     {2000, 4432076, 7977697},
        {3863, 2734162, 4921466},    {5000, 2216038, 3988848},
        {7700, 1528302, 2750930},    {10797, 1127085, 2028743},
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        char label[64];
        snprintf(label, sizeof(label), "rho %u quota fits", rows[i].rho);
        CHECK(label, reserve(rows[i].q_rows, 768, 32, rows[i].rho) <= quota);
        snprintf(label, sizeof(label), "rho %u quota +1", rows[i].rho);
        CHECK(label, reserve(rows[i].q_rows + 1, 768, 32, rows[i].rho) > quota);
        snprintf(label, sizeof(label), "rho %u logical fits", rows[i].rho);
        CHECK(label, reserve(rows[i].l_rows, 768, 32, rows[i].rho) <= logical);
        snprintf(label, sizeof(label), "rho %u logical +1", rows[i].rho);
        CHECK(label, reserve(rows[i].l_rows + 1, 768, 32, rows[i].rho) > logical);
    }
    /* L40S example: 1M x 768 gd32 at a high scratch coefficient and at 3863. */
    CHECK_EQ("L40S N rho 7700", reserve(999000, 768, 32, 7700),
             UINT64_C(27812160000));
    CHECK_EQ("L40S N rho 3863", reserve(999000, 768, 32, 3863),
             UINT64_C(15546038400));
}

static void
test_workspace_hook_is_inert_before_i4(void)
{
    /* I4 owns the AR6/AR7 rule (test_lw_policy.c). Here only: policy error
     * (managed = 0) and NULL never refuse (conch 2026-11-03 16:0x). */
    CuvsAdmissionWorkspaceInput spill;
    memset(&spill, 0, sizeof(spill));
    spill.needed = UINT64_C(1) << 40;
    spill.phys_free = 1;
    spill.lw_ub = UINT64_C(1) << 40;
    spill.managed = 0;
    CHECK("hook none: zero", cuvs_admission_workspace_check(&spill) ==
                                 CUVS_ADMISSION_NONE);
    spill.overcommit = 1;
    CHECK("hook none: overcommit", cuvs_admission_workspace_check(&spill) ==
                                       CUVS_ADMISSION_NONE);
    CHECK("hook none: NULL", cuvs_admission_workspace_check(NULL) ==
                                 CUVS_ADMISSION_NONE);
}

/* AR8 (ADR-089, design 40c33a3b) with the Q6 condition: n 999000, dim 768,
 * gd 32, rho 0, pool quota 6,807,502,080 B, resident A_res pool_used =
 * 3,196,800,000 B. */
static const uint64_t Q6_QUOTA = UINT64_C(6807502080);
static const uint64_t Q6_POOL_USED = UINT64_C(3196800000);

static int
ar8(uint64_t quota, uint64_t pending, uint64_t pool_used, uint64_t needed,
    uint32_t permille)
{
    return cuvs_admission_pool_refuses(quota, pending, pool_used, needed,
                                       cuvs_admission_build_headroom(needed, permille));
}

static void
test_ar8_pool_capacity(void)
{
    uint64_t final_b = cuvs_admission_estimate_final_bytes(999000, 768, 32);
    uint64_t n_m = cuvs_admission_needed_bytes(final_b, 0, 0, 0);

    CHECK_EQ("Q6 N_m", n_m, UINT64_C(3196800000));
    CHECK_EQ("Q6 headroom 150 on N_m", cuvs_admission_build_headroom(n_m, 150),
             UINT64_C(479520000));
    CHECK_EQ("Q6 headroom 100 on N_m", cuvs_admission_build_headroom(n_m, 100),
             UINT64_C(319680000));
    CHECK_EQ("Q6 used+N+150", Q6_POOL_USED + n_m + UINT64_C(479520000),
             UINT64_C(6873120000));
    CHECK("Q6 default is 150", CUVS_RMM_BUILD_HEADROOM_PERMILLE_DEFAULT == 150u);

    /* 150: 101.0% of quota -> AR8. 100: 98.6% and 0: 93.9% -> admitted
     * (permille 0 does not close the Q6 door). */
    CHECK("Q6 permille 150 -> AR8", ar8(Q6_QUOTA, 0, Q6_POOL_USED, n_m, 150));
    CHECK("Q6 permille 100 admitted", !ar8(Q6_QUOTA, 0, Q6_POOL_USED, n_m, 100));
    CHECK("Q6 permille 0 admitted", !ar8(Q6_QUOTA, 0, Q6_POOL_USED, n_m, 0));
    CHECK_EQ("permille 0 headroom", cuvs_admission_build_headroom(n_m, 0), 0);
    /* Without the resident bytes the request alone fits at 150. */
    CHECK("Q6 no resident admitted", !ar8(Q6_QUOTA, 0, 0, n_m, 150));

    /* Design concurrency example: quota 100, N_m 60, headroom 10. */
    CHECK("first 0+0+60+10 admitted", !cuvs_admission_pool_refuses(100, 0, 0, 60, 10));
    CHECK("second 60+0+60+10 -> AR8", cuvs_admission_pool_refuses(100, 60, 0, 60, 10));

    /* Quota 0 (pool disabled) never refuses. */
    CHECK("quota 0", !ar8(0, 0, Q6_POOL_USED, n_m, 150));
    CHECK("quota 0 saturated", !cuvs_admission_pool_refuses(0, SAT, SAT, SAT, SAT));

    /* Boundary with nonzero pool_used and pending: == quota admitted, +1 refused. */
    CHECK("== quota admitted", !cuvs_admission_pool_refuses(100, 7, 30, 50, 13));
    CHECK("+1 refused", cuvs_admission_pool_refuses(99, 7, 30, 50, 13));
    CHECK("each term counts", cuvs_admission_pool_refuses(100, 8, 30, 50, 13) &&
                              cuvs_admission_pool_refuses(100, 7, 31, 50, 13) &&
                              cuvs_admission_pool_refuses(100, 7, 30, 51, 13) &&
                              cuvs_admission_pool_refuses(100, 7, 30, 50, 14));

    /* Floor 256 MiB for small builds; ceil rounding; saturation refuses. */
    CHECK_EQ("floor", cuvs_admission_build_headroom(1000, 150), UINT64_C(256) << 20);
    CHECK_EQ("ceil", cuvs_admission_build_headroom(UINT64_C(10000000001), 150),
             UINT64_C(1500000001));
    CHECK_EQ("permille above max", cuvs_admission_build_headroom(1, 100001), SAT);
    CHECK_EQ("max permille", cuvs_admission_build_headroom(UINT64_C(1) << 30, 100000),
             UINT64_C(100) << 30);
    CHECK("saturated sum refuses", cuvs_admission_pool_refuses(UINT64_MAX / 4, UINT64_MAX / 4,
                                                               UINT64_MAX / 4, 1, 1));
}

/* Owner decision 2026-11-09: the pool shortfall make_room is asked to free
 * (pending + pool_used + N_m + headroom - quota), nonzero exactly when AR8's
 * sum refuses. Q6: 6,873,120,000 - 6,807,502,080 = 65,617,920 B. */
static void
test_ar8_pool_shortfall(void)
{
    uint64_t n_m = UINT64_C(3196800000), h = UINT64_C(479520000);
    CHECK_EQ("Q6 shortfall", cuvs_admission_pool_shortfall(Q6_QUOTA, 0, Q6_POOL_USED, n_m, h),
             UINT64_C(65617920));
    CHECK_EQ("Q6 headroom 0 fits", cuvs_admission_pool_shortfall(Q6_QUOTA, 0, Q6_POOL_USED, n_m, 0), 0);
    CHECK_EQ("== quota is 0", cuvs_admission_pool_shortfall(100, 7, 30, 50, 13), 0);
    CHECK_EQ("+1 is 1", cuvs_admission_pool_shortfall(99, 7, 30, 50, 13), 1);
    CHECK_EQ("each term", cuvs_admission_pool_shortfall(100, 8, 31, 52, 16), 7);
    CHECK_EQ("quota 0 never short", cuvs_admission_pool_shortfall(0, SAT, SAT, SAT, SAT), 0);
    CHECK_EQ("saturated sum", cuvs_admission_pool_shortfall(UINT64_MAX / 4, UINT64_MAX / 4,
                                                            UINT64_MAX / 4, 1, 1),
             SAT - UINT64_MAX / 4);
    /* nonzero iff AR8's sum refuses, over a small grid */
    for (uint64_t q = 0; q < 12; q++)
        for (uint64_t u = 0; u < 5; u++)
            CHECK("shortfall > 0 iff refuses",
                  (cuvs_admission_pool_shortfall(q, 1, u, 3, 2) > 0) ==
                  cuvs_admission_pool_refuses(q, 1, u, 3, 2));
}

int
main(void)
{
    test_workspace_hook_is_inert_before_i4();
    test_final_matches_v5_when_dim_is_aligned();
    test_final_uses_aligned_stride();
    test_final_default_degree();
    test_final_edges_fail_closed();
    test_needed_scratch_rounds_up();
    test_needed_adds_h2d_and_margin();
    test_needed_saturates();
    test_c3_admission_table();
    test_ar8_pool_capacity();
    test_ar8_pool_shortfall();
    if (g_failures) {
        fprintf(stderr, "test_admission_contract: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_admission_contract: all checks passed\n");
    return 0;
}
