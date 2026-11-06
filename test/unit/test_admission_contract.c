/*
 * CPU unit tests for the I3 admission estimate in cuvs_admission_contract.h.
 * The same file is compiled as C11 and as C++17 because the header is shared
 * by the daemon (C), the backend (C) and the wrapper (C++/CUDA).
 *
 * Expected values come from v6 r18 C3 (the reservation formula, the rows of
 * the per-rho admission table, the L40S example) and CK-30 (dim%4 stride).
 */
#include "cuvs_admission_contract.h"
#include "cuvs_lw_policy.h"

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

/* AR8 (ADR-089) with the Q6 shape: n 999000, dim 768, igd 128, gd 32, pool
 * quota 6,807,502,080 B, rho 0, LW-8 margin 1100. */
static const uint64_t Q6_QUOTA = UINT64_C(6807502080);

static int
ar8(uint64_t quota, uint64_t pending, uint64_t needed, uint64_t lw_ub,
    uint32_t permille)
{
    uint64_t need = cuvs_admission_pool_need(needed, lw_ub);
    return cuvs_admission_pool_refuses(quota, pending, need,
                                       cuvs_admission_build_headroom(need, permille));
}

static void
test_ar8_pool_capacity(void)
{
    uint64_t final_b = cuvs_admission_estimate_final_bytes(999000, 768, 32);
    uint64_t n_m = cuvs_admission_needed_bytes(final_b, 0, 0, 0);
    uint64_t lw_ub = cuvs_lw_ub_bytes(cuvs_lw_shape_bytes(999000, 768, 128), 1100);
    uint64_t need = cuvs_admission_pool_need(n_m, lw_ub);
    uint64_t h150 = cuvs_admission_build_headroom(need, 150);

    CHECK_EQ("Q6 final", final_b, UINT64_C(3196800000));
    CHECK_EQ("Q6 L_shape", cuvs_lw_shape_bytes(999000, 768, 128), UINT64_C(3580416000));
    CHECK_EQ("Q6 lw_ub 1100", lw_ub, UINT64_C(3938457600));
    CHECK_EQ("Q6 pool_need", need, UINT64_C(7135257600));
    CHECK_EQ("Q6 headroom 150", h150, UINT64_C(1070288640));
    CHECK_EQ("Q6 need+headroom", need + h150, UINT64_C(8205546240));
    CHECK("Q6 default is 150", CUVS_RMM_BUILD_HEADROOM_PERMILLE_DEFAULT == 150u);

    /* The Q6 shape is refused with and without headroom. */
    CHECK("Q6 permille 150 -> AR8", ar8(Q6_QUOTA, 0, n_m, lw_ub, 150));
    CHECK("Q6 permille 0 -> AR8 (capacity)", ar8(Q6_QUOTA, 0, n_m, lw_ub, 0));
    CHECK_EQ("permille 0 headroom", cuvs_admission_build_headroom(need, 0), 0);

    /* A shape that fits is admitted at 150: half the Q6 rows. */
    {
        uint64_t f2 = cuvs_admission_estimate_final_bytes(499500, 768, 32);
        uint64_t l2 = cuvs_lw_ub_bytes(cuvs_lw_shape_bytes(499500, 768, 128), 1100);
        CHECK("half Q6 fits", !ar8(Q6_QUOTA, 0, f2, l2, 150));
        /* Pending pushes the same build into AR8. */
        CHECK("pending -> AR8", ar8(Q6_QUOTA, UINT64_C(3) << 30, f2, l2, 150));
    }
    /* Headroom alone refuses a build that the capacity rule admits. */
    CHECK("150-only refusal", !ar8(Q6_QUOTA, 0, Q6_QUOTA - (UINT64_C(512) << 20), 0, 0) &&
                              ar8(Q6_QUOTA, 0, Q6_QUOTA - (UINT64_C(512) << 20), 0, 150));
    /* Quota 0 (pool disabled) never refuses. */
    CHECK("quota 0", !ar8(0, 0, n_m, lw_ub, 150));
    CHECK("quota 0 saturated", !cuvs_admission_pool_refuses(0, SAT, SAT, SAT));

    /* Boundary: == quota is admitted, one byte more is refused (">"). */
    CHECK("== quota admitted", !cuvs_admission_pool_refuses(need + h150, 0, need, h150));
    CHECK("+1 refused", cuvs_admission_pool_refuses(need + h150 - 1, 0, need, h150));
    CHECK("pending boundary", !cuvs_admission_pool_refuses(need + h150 + 5, 5, need, h150) &&
                              cuvs_admission_pool_refuses(need + h150 + 5, 6, need, h150));

    /* Floor 256 MiB for small builds; ceil rounding; saturation refuses. */
    CHECK_EQ("floor", cuvs_admission_build_headroom(1000, 150), UINT64_C(256) << 20);
    CHECK_EQ("ceil", cuvs_admission_build_headroom(UINT64_C(10000000001), 150),
             UINT64_C(1500000001));
    CHECK_EQ("permille above max", cuvs_admission_build_headroom(1, 100001), SAT);
    CHECK_EQ("max permille", cuvs_admission_build_headroom(UINT64_C(1) << 30, 100000),
             UINT64_C(100) << 30);
    CHECK_EQ("need saturates", cuvs_admission_pool_need(SAT - 1, 2), SAT);
    CHECK("saturated sum refuses", cuvs_admission_pool_refuses(UINT64_MAX / 4, UINT64_MAX / 4,
                                                               UINT64_MAX / 4, 1));
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
    if (g_failures) {
        fprintf(stderr, "test_admission_contract: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_admission_contract: all checks passed\n");
    return 0;
}
