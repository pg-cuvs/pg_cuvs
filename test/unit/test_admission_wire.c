/*
 * CPU unit tests for the err_out text payload codec (v6 r18 C5, CK-29):
 * cuvs_admission_encode / cuvs_admission_decode in cuvs_util.c.
 * Run under ASAN for the 511/512/513 B decoder boundary.
 */
#include "cuvs_util.h"
#include "cuvs_ipc.h"   /* CUVS_METRIC_*, CUVS_CAGRA_BUILD_* */
#include "cuvs_admission_contract.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(label, cond)                                                  \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s (line %d)\n", (label), __LINE__);      \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

static CuvsAdmissionFields
fields(CuvsAdmissionReason reason)
{
    CuvsAdmissionFields f;
    memset(&f, 0, sizeof(f));
    f.reason = reason;
    return f;
}

static void
add_cand(CuvsAdmissionFields *f, uint32_t db, uint32_t idx, uint32_t mib,
         int32_t idle)
{
    CuvsAdmissionCandidate c;
    c.db_oid = db;
    c.index_oid = idx;
    c.mib = mib;
    c.idle_s = idle;
    f->cand[f->n_cand++] = c;
}

/* Decode a payload from an exact-size heap buffer so ASAN sees overreads. */
static int
decode_exact(const char *bytes, size_t len, CuvsAdmissionFields *out)
{
    char *heap = (char *)malloc(len ? len : 1);
    int rc;
    memcpy(heap, bytes, len);
    rc = cuvs_admission_decode(heap, len, out);
    free(heap);
    return rc;
}

static void
test_mib_saturates(void)
{
    CHECK("mib 0", cuvs_admission_mib(0) == 0);
    CHECK("mib floor", cuvs_admission_mib((UINT64_C(1) << 20) * 3 - 1) == 2);
    CHECK("mib exact", cuvs_admission_mib(UINT64_C(1) << 20) == 1);
    CHECK("mib saturate",
          cuvs_admission_mib(UINT64_C(99999999) << 20) == CUVS_ADMISSION_SCALAR_MAX);
    CHECK("mib saturate +",
          cuvs_admission_mib(UINT64_C(100000000) << 20) == CUVS_ADMISSION_SCALAR_MAX);
    CHECK("mib max", cuvs_admission_mib(UINT64_MAX) == CUVS_ADMISSION_SCALAR_MAX);
    CHECK("count saturate", cuvs_admission_count(UINT64_C(1) << 40) ==
                                CUVS_ADMISSION_SCALAR_MAX);
}

static void
test_reason_names_and_sqlstate(void)
{
    CHECK("AR0 name", strcmp(cuvs_admission_reason_name(CUVS_ADMISSION_AR0), "AR0") == 0);
    CHECK("AR7 name", strcmp(cuvs_admission_reason_name(CUVS_ADMISSION_AR7), "AR7") == 0);
    CHECK("AR8 name", strcmp(cuvs_admission_reason_name(CUVS_ADMISSION_AR8), "AR8") == 0);
    CHECK("AR8 follows AR7", (int)CUVS_ADMISSION_AR8 == (int)CUVS_ADMISSION_AR7 + 1);
    CHECK("AR8 53200", strcmp(cuvs_admission_sqlstate(CUVS_ADMISSION_AR8), "53200") == 0);
    CHECK("AR8 transient", cuvs_admission_reason_transient(CUVS_ADMISSION_AR8));
    {
        /* AR0..AR7 spellings are unchanged by AR8. */
        static const char *const old[] = {"AR0", "AR1", "AR2", "AR3", "AR4", "AR5", "AR6", "AR7"};
        for (int r = 0; r < 8; r++)
            CHECK("AR0..AR7 names", strcmp(cuvs_admission_reason_name((CuvsAdmissionReason)r), old[r]) == 0);
    }
    CHECK("OOM name", strcmp(cuvs_admission_reason_name(CUVS_ADMISSION_OOM), "OOM") == 0);
    CHECK("ATT name", strcmp(cuvs_admission_reason_name(CUVS_ADMISSION_ATT), "ATT") == 0);
    CHECK("NONE name", cuvs_admission_reason_name(CUVS_ADMISSION_NONE) == NULL);
    /* Permanent AR0 must never be a retryable 53xxx (UC-2 decision 1). */
    CHECK("AR0 54000", strcmp(cuvs_admission_sqlstate(CUVS_ADMISSION_AR0), "54000") == 0);
    for (int r = CUVS_ADMISSION_AR1; r <= CUVS_ADMISSION_ATT; r++) {
        const char *want = r == CUVS_ADMISSION_AR6 ? "53400" : "53200";
        CHECK("sqlstate", strcmp(cuvs_admission_sqlstate((CuvsAdmissionReason)r), want) == 0);
    }
    CHECK("NONE sqlstate", cuvs_admission_sqlstate(CUVS_ADMISSION_NONE) == NULL);
    CHECK("AR0 permanent", !cuvs_admission_reason_transient(CUVS_ADMISSION_AR0));
    CHECK("AR6 not transient", !cuvs_admission_reason_transient(CUVS_ADMISSION_AR6));
    CHECK("AR5 transient", cuvs_admission_reason_transient(CUVS_ADMISSION_AR5));
    CHECK("OOM transient", cuvs_admission_reason_transient(CUVS_ADMISSION_OOM));
    CHECK("ATT transient", cuvs_admission_reason_transient(CUVS_ADMISSION_ATT));
}

static void
test_encode_order_and_subset(void)
{
    char buf[CUVS_ADMISSION_ERR_OUT_BYTES];
    CuvsAdmissionFields f = fields(CUVS_ADMISSION_AR0);
    f.present = CUVS_ADM_F_N | CUVS_ADM_F_Q;
    f.n = 3049;
    f.q = 2048;
    CHECK("AR0 encode", cuvs_admission_encode(&f, buf, sizeof(buf)) > 0);
    CHECK("AR0 text", strcmp(buf, "AR0 n=3049 q=2048") == 0);

    f = fields(CUVS_ADMISSION_AR3);
    f.present = CUVS_ADM_F_N | CUVS_ADM_F_F | CUVS_ADM_F_H | CUVS_ADM_F_E |
                CUVS_ADM_F_K | CUVS_ADM_F_R;
    f.n = 10; f.f = 2; f.h_mib = 30; f.h_cnt = 3; f.e_mib = 5; f.e_cnt = 1;
    f.k_mib = 7; f.k_cnt = 2; f.r = 1;
    add_cand(&f, 16384, 24576, 5, -1);
    add_cand(&f, 1, 2, 3, 120);
    CHECK("AR3 encode", cuvs_admission_encode(&f, buf, sizeof(buf)) > 0);
    CHECK("AR3 text", strcmp(buf, "AR3 n=10 f=2 h=30/3 e=5/1 k=7/2 r=1 "
                                  "t=16384:24576:5:-1 t=1:2:3:120") == 0);

    f = fields(CUVS_ADMISSION_AR5);
    f.present = CUVS_ADM_F_N | CUVS_ADM_F_F | CUVS_ADM_F_E | CUVS_ADM_F_K |
                CUVS_ADM_F_W;
    f.w = CUVS_ADMISSION_WAIT_UNPIN;
    CHECK("AR5 reload encode", cuvs_admission_encode(&f, buf, sizeof(buf)) > 0);
    CHECK("AR5 reload text", strcmp(buf, "AR5 n=0 f=0 e=0/0 k=0/0 w=-1") == 0);

    f = fields(CUVS_ADMISSION_AR4);
    f.present = CUVS_ADM_F_N | CUVS_ADM_F_I;
    f.i = 1000;               /* shard numbers saturate at 999 */
    CHECK("shard encode", cuvs_admission_encode(&f, buf, sizeof(buf)) > 0);
    CHECK("shard text", strcmp(buf, "AR4 n=0 i=999") == 0);

    /* AR8 carries n (pool_need) q p d, d after every older key. */
    {
        CuvsAdmissionFields g;
        int len;
        f = fields(CUVS_ADMISSION_AR8);
        f.present = CUVS_ADM_F_N | CUVS_ADM_F_Q | CUVS_ADM_F_P | CUVS_ADM_F_D | CUVS_ADM_F_O;
        f.n = 3048; f.q = 6492; f.p = 0; f.d = 457; f.o = 3048;
        len = cuvs_admission_encode(&f, buf, sizeof(buf));
        CHECK("AR8 encode", len > 0);
        CHECK("AR8 text", strcmp(buf, "AR8 n=3048 q=6492 p=0 d=457 o=3048") == 0);
        CHECK("AR8 decode", decode_exact(buf, (size_t)len + 1, &g) == 0);
        CHECK("AR8 same", memcmp(&f, &g, sizeof(f)) == 0);
        f.present |= CUVS_ADM_F_I;
        f.i = 3;
        CHECK("d after i", cuvs_admission_encode(&f, buf, sizeof(buf)) > 0 &&
                           strcmp(buf, "AR8 n=3048 q=6492 p=0 i=3 d=457 o=3048") == 0);
    }
}

static void
test_encode_rejects(void)
{
    char buf[CUVS_ADMISSION_ERR_OUT_BYTES];
    char fit[8], tiny[7];     /* "AR3 n=0" is 7 chars + NUL */
    CuvsAdmissionFields f = fields(CUVS_ADMISSION_AR3);
    f.present = CUVS_ADM_F_N;
    f.n_cand = 4;             /* a fourth candidate is an error */
    CHECK("4 cands", cuvs_admission_encode(&f, buf, sizeof(buf)) < 0);
    f.n_cand = 0;
    CHECK("exact fit", cuvs_admission_encode(&f, fit, sizeof(fit)) == 7);
    CHECK("tiny buffer", cuvs_admission_encode(&f, tiny, sizeof(tiny)) < 0);
    CHECK("tiny buffer NUL", tiny[0] == '\0');
    f = fields(CUVS_ADMISSION_NONE);
    CHECK("NONE", cuvs_admission_encode(&f, buf, sizeof(buf)) < 0);
    f = fields(CUVS_ADMISSION_AR5);
    f.present = CUVS_ADM_F_W;
    f.w = 7;                  /* w is 0 or -1, never a countdown */
    CHECK("w 7", cuvs_admission_encode(&f, buf, sizeof(buf)) < 0);
    f.w = 0;
    f.present = CUVS_ADM_F_R;
    f.r = 2;
    CHECK("r 2", cuvs_admission_encode(&f, buf, sizeof(buf)) < 0);
    f = fields(CUVS_ADMISSION_AR3);
    add_cand(&f, 1, 2, 3, -2);
    CHECK("idle -2", cuvs_admission_encode(&f, buf, sizeof(buf)) < 0);
}

static void
test_round_trip_max(void)
{
    /* The longest legal payload: every field saturated, three candidates. */
    char buf[CUVS_ADMISSION_ERR_OUT_BYTES];
    CuvsAdmissionFields f = fields(CUVS_ADMISSION_AR3), g;
    int len;
    f.present = CUVS_ADM_F_ALL;
    f.n = f.f = f.q = f.p = f.l = f.y = f.u = f.g = f.s = f.d = f.o = CUVS_ADMISSION_SCALAR_MAX;
    f.h_mib = f.h_cnt = f.e_mib = f.e_cnt = f.k_mib = f.k_cnt = CUVS_ADMISSION_SCALAR_MAX;
    f.r = 1; f.w = CUVS_ADMISSION_WAIT_UNPIN; f.i = CUVS_ADMISSION_SHARD_MAX;
    for (int c = 0; c < 3; c++)
        add_cand(&f, UINT32_MAX, UINT32_MAX, CUVS_ADMISSION_SCALAR_MAX, 99999999);
    len = cuvs_admission_encode(&f, buf, sizeof(buf));
    CHECK("max encode", len > 0 && len < (int)CUVS_ADMISSION_ERR_OUT_BYTES);
    CHECK("max decode", decode_exact(buf, (size_t)len + 1, &g) == 0);
    CHECK("max same", memcmp(&f, &g, sizeof(f)) == 0);
    /* The r18 C5 longest real subset is AR3/AR4 at 215 B. */
    f.present = CUVS_ADM_F_N | CUVS_ADM_F_F | CUVS_ADM_F_H | CUVS_ADM_F_E |
                CUVS_ADM_F_K | CUVS_ADM_F_R;
    len = cuvs_admission_encode(&f, buf, sizeof(buf));
    CHECK("AR3 max 215 B", len == 215);
}

static void
test_round_trip_candidates(void)
{
    char buf[CUVS_ADMISSION_ERR_OUT_BYTES];
    for (uint32_t n = 0; n <= 3; n++) {
        CuvsAdmissionFields f = fields(CUVS_ADMISSION_AR5), g;
        int len;
        f.present = CUVS_ADM_F_N | CUVS_ADM_F_S | CUVS_ADM_F_R;
        f.n = 99; f.s = 4; f.r = 0;
        for (uint32_t c = 0; c < n; c++)
            add_cand(&f, 5 + c, 100 + c, c, c == 0 ? -1 : (int32_t)(60 * c));
        len = cuvs_admission_encode(&f, buf, sizeof(buf));
        CHECK("cands encode", len > 0);
        CHECK("cands decode", decode_exact(buf, (size_t)len + 1, &g) == 0);
        CHECK("cands count", g.n_cand == n);
        CHECK("cands same", memcmp(&f, &g, sizeof(f)) == 0);
    }
}

static void
test_decode_rejects(void)
{
    CuvsAdmissionFields g;
    const char *bad[] = {
        "",                                   /* no reason */
        "AR9 n=1",                            /* unknown reason */
        "AR8 d=1 d=2",                        /* duplicate headroom */
        "AR8 d=100000000",                    /* headroom above saturation */
        "AR8 o=1 o=2",                        /* duplicate pool used */
        "NONE n=1",                           /* NONE is never sent */
        "AR0 n=1 n=2",                        /* duplicate key */
        "AR0 z=1",                            /* unknown key */
        "AR0 n=100000000",                    /* above saturation */
        "AR0 n=-1",                           /* negative scalar */
        "AR0 n=",                             /* empty value */
        "AR0 n=1x",                           /* trailing junk */
        "AR0  n=1",                           /* double space */
        "AR0 n=1 ",                           /* trailing space */
        "AR3 h=1",                            /* pair without count */
        "AR3 h=1/2/3",                        /* pair with extra part */
        "AR5 w=5",                            /* w is 0 or -1 */
        "AR5 r=2",
        "AR4 i=1000",                         /* shard above 999 */
        "AR3 t=1:2:3",                        /* short candidate */
        "AR3 t=1:2:3:-2",                     /* idle below never */
        "AR3 t=4294967296:2:3:4",             /* OID above uint32 */
        "AR3 t=1:2:3:4 t=1:2:3:4 t=1:2:3:4 t=1:2:3:4", /* fourth candidate */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char label[96];
        snprintf(label, sizeof(label), "reject '%s'", bad[i]);
        CHECK(label, decode_exact(bad[i], strlen(bad[i]) + 1, &g) != 0);
    }
    CHECK("NULL", cuvs_admission_decode(NULL, 4, &g) != 0);
    /* Embedded NUL ends the text: what follows is never parsed. */
    {
        const char emb[] = "AR0 n=1\0 z=9";
        CHECK("embedded NUL", decode_exact(emb, sizeof(emb), &g) == 0 && g.n == 1);
    }
}

static void
test_decode_payload_boundary(void)
{
    /* CK-29: 511/512/513 B synthetic payloads (len counts the NUL). */
    char *p = (char *)malloc(CUVS_ADMISSION_ERR_OUT_BYTES + 1);
    CuvsAdmissionFields g;
    size_t sizes[] = {511, 512, 513};
    for (size_t k = 0; k < 3; k++) {
        size_t len = sizes[k];
        /* "AR0 n=1" padded with a long unknown-free run of candidates is not
         * legal, so build "AR0 n=1" followed by zero-padding digits. */
        memset(p, 0, CUVS_ADMISSION_ERR_OUT_BYTES + 1);
        memcpy(p, "AR0 n=", 6);
        memset(p + 6, '0', len - 1 - 7);
        p[len - 2] = '1';
        p[len - 1] = '\0';
        if (len <= CUVS_ADMISSION_ERR_OUT_BYTES)
            CHECK("boundary accepts", decode_exact(p, len, &g) == 0 && g.n == 1);
        else
            CHECK("513 B rejected", decode_exact(p, len, &g) != 0);
    }
    /* 512 bytes without a NUL inside the buffer: rejected, no overread. */
    memset(p, '1', CUVS_ADMISSION_ERR_OUT_BYTES);
    memcpy(p, "AR0 n=", 6);
    CHECK("512 no NUL", decode_exact(p, CUVS_ADMISSION_ERR_OUT_BYTES, &g) != 0);
    /* 511 bytes without a NUL: also rejected (the NUL must be inside). */
    CHECK("511 no NUL", decode_exact(p, CUVS_ADMISSION_ERR_OUT_BYTES - 1, &g) != 0);
    free(p);
}

static void
test_parse_scratch_permille(void)
{
    uint32_t v = 1234;
    CHECK("0", cuvs_parse_scratch_permille("0", &v) == 0 && v == 0);
    CHECK("1109", cuvs_parse_scratch_permille("1109", &v) == 0 && v == 1109);
    CHECK("max", cuvs_parse_scratch_permille("100000", &v) == 0 && v == 100000);
    v = 5;
    CHECK("max+1", cuvs_parse_scratch_permille("100001", &v) != 0 && v == 5);
    CHECK("negative", cuvs_parse_scratch_permille("-1", &v) != 0);
    CHECK("plus", cuvs_parse_scratch_permille("+1", &v) != 0);
    CHECK("junk", cuvs_parse_scratch_permille("12x", &v) != 0);
    CHECK("empty", cuvs_parse_scratch_permille("", &v) != 0);
    CHECK("NULL", cuvs_parse_scratch_permille(NULL, &v) != 0);
    CHECK("huge", cuvs_parse_scratch_permille("99999999999999999999", &v) != 0);
}

static void
test_shape_class(void)
{
    /* v5 C3 (carried by v6 C3): checked points, bracketed region, rest. */
    const uint32_t L2 = CUVS_METRIC_L2, IVF = CUVS_CAGRA_BUILD_IVF_PQ,
                   AUTO = CUVS_CAGRA_BUILD_AUTO, NND = CUVS_CAGRA_BUILD_NN_DESCENT;
#define SC(n, dim, m, gd, igd, a) cuvs_build_shape_class((n), (dim), (m), (gd), (igd), (a))
    CHECK("checked 32/128", strcmp(SC(999000, 768, L2, 32, 128, IVF), "checked") == 0);
    CHECK("checked 64/256", strcmp(SC(999000, 768, L2, 64, 256, IVF), "checked") == 0);
    CHECK("checked 32/64", strcmp(SC(999000, 768, L2, 32, 64, IVF), "checked") == 0);
    CHECK("checked 64/128 auto", strcmp(SC(999000, 768, L2, 64, 128, AUTO), "checked") == 0);
    CHECK("checked defaults", strcmp(SC(999000, 768, L2, 0, 0, AUTO), "checked") == 0);
    CHECK("checked 250k", strcmp(SC(250000, 768, L2, 32, 128, IVF), "checked") == 0);
    CHECK("checked 500k", strcmp(SC(500000, 768, L2, 32, 128, IVF), "checked") == 0);
    CHECK("bracketed 300k", strcmp(SC(300000, 768, L2, 32, 128, IVF), "bracketed") == 0);
    CHECK("bracketed 64/64", strcmp(SC(999000, 768, L2, 64, 64, IVF), "bracketed") == 0);
    CHECK("bracketed auto", strcmp(SC(250000, 768, L2, 48, 200, AUTO), "bracketed") == 0);
    CHECK("64/128 ivf is bracketed", strcmp(SC(999000, 768, L2, 64, 128, IVF), "bracketed") == 0);
    CHECK("igd < gd", strcmp(SC(999000, 768, L2, 64, 32, IVF), "extrapolated") == 0);
    CHECK("nn_descent", strcmp(SC(999000, 768, L2, 32, 128, NND), "extrapolated") == 0);
    CHECK("dim 767", strcmp(SC(999000, 767, L2, 32, 128, IVF), "extrapolated") == 0);
    CHECK("cosine", strcmp(SC(999000, 768, CUVS_METRIC_COSINE, 32, 128, IVF), "extrapolated") == 0);
    CHECK("gd 31", strcmp(SC(999000, 768, L2, 31, 128, IVF), "extrapolated") == 0);
    CHECK("gd 65", strcmp(SC(999000, 768, L2, 65, 128, IVF), "extrapolated") == 0);
    CHECK("igd 257", strcmp(SC(999000, 768, L2, 64, 257, IVF), "extrapolated") == 0);
    CHECK("n 249999", strcmp(SC(249999, 768, L2, 32, 128, IVF), "extrapolated") == 0);
    CHECK("n 999001", strcmp(SC(999001, 768, L2, 32, 128, IVF), "extrapolated") == 0);
#undef SC
}

int
main(void)
{
    test_shape_class();
    test_parse_scratch_permille();
    test_mib_saturates();
    test_reason_names_and_sqlstate();
    test_encode_order_and_subset();
    test_encode_rejects();
    test_round_trip_max();
    test_round_trip_candidates();
    test_decode_rejects();
    test_decode_payload_boundary();
    if (g_failures) {
        fprintf(stderr, "test_admission_wire: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_admission_wire: all checks passed\n");
    return 0;
}
