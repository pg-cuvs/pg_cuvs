/* UC-5 ACE quality rule, daemon knob parser, and partition-builder readback
 * (cuvs_hnsw_policy.h). CPU-only; expected values are computed here from the
 * cuVS 26.04 contract, not by calling the helpers under test. */
#include "cuvs_hnsw_policy.h"
#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static void test_parse(void)
{
    uint32_t q = 12345;
    CHECK(cuvs_hnsw_ace_quality_parse("auto", &q) == 0 && q == CUVS_HNSW_ACE_QUALITY_AUTO);
    CHECK(cuvs_hnsw_ace_quality_parse("0", &q) == 0 && q == 0);
    CHECK(cuvs_hnsw_ace_quality_parse("7", &q) == 0 && q == 7);
    CHECK(cuvs_hnsw_ace_quality_parse("4095", &q) == 0 && q == 4095);
    const char *bad[] = {NULL, "", "4096", "9999", "00001", "-1", "+1", "1a", "a", " 7", "7 ", "AUTO", "1e3"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        q = 99;
        CHECK(cuvs_hnsw_ace_quality_parse(bad[i], &q) != 0 && q == 99);
    }
}

static void test_rule(void)
{
    /* auto: the linked SDK's ace_params default A at the reloption default,
     * ceil(A_sdk * E / 64) elsewhere, bounded to the SDK's [4, 65535] domain. */
    const uint32_t sdk_defaults[] = {120, 200, 64};
    for (size_t i = 0; i < 3; i++)
    {
        uint32_t base = sdk_defaults[i], last = 0;
        CHECK(cuvs_hnsw_ace_ef(CUVS_HNSW_ACE_QUALITY_AUTO, 64, base) == base);
        for (uint32_t ef = 4; ef <= 1000; ef++)
        {
            uint32_t a = cuvs_hnsw_ace_ef(CUVS_HNSW_ACE_QUALITY_AUTO, ef, base);
            uint64_t want = (uint64_t)base * ef;
            if (a > 4) CHECK((uint64_t)a * 64 >= want && (uint64_t)(a - 1) * 64 < want);
            else CHECK(want <= 4 * 64);
            CHECK(a >= last);
            last = a;
        }
    }
    /* cuVS 26.04 at the reloption default: A 120 -> q 7 -> 12 NN-descent
     * iterations, the value before the knob existed. */
    CHECK(cuvs_hnsw_ace_ef(CUVS_HNSW_ACE_QUALITY_AUTO, 64, 120) == 120);
    CHECK(cuvs_hnsw_ace_ef(CUVS_HNSW_ACE_QUALITY_AUTO, 64, 120) / 16 == 7);
    CHECK(cuvs_hnsw_ace_ef(CUVS_HNSW_ACE_QUALITY_AUTO, 128, 120) == 240);
    CHECK(cuvs_hnsw_ace_ef(CUVS_HNSW_ACE_QUALITY_AUTO, 1000, 120) == 1875);
    CHECK(cuvs_hnsw_ace_ef(CUVS_HNSW_ACE_QUALITY_AUTO, 4, 120) == 8);
    /* manual Q: exactly 16 Q whatever ef_construction is, so q == Q. */
    for (uint32_t qv = 0; qv <= CUVS_HNSW_ACE_QUALITY_MAX; qv++)
        for (uint32_t ef = 4; ef <= 1000; ef += 333)
        {
            uint32_t a = cuvs_hnsw_ace_ef(qv, ef, 120);
            CHECK(a == 16 * qv && a / 16 == qv && a <= 65535);
        }
}

static CuvsHnswPolicy ace_receipt(uint64_t rows, uint32_t partitions, uint32_t ace_ef)
{
    CuvsHnswPolicy p = {0};
    p.magic = CUVS_HNSW_POLICY_MAGIC; p.version = CUVS_HNSW_POLICY_VERSION; p.bytes = sizeof(p);
    p.route = CUVS_HNSW_ROUTE_ACE; p.internal_builder = CUVS_HNSW_ROUTE_ACE;
    p.requested_algo = CUVS_HNSW_ALGO_ACE; p.requested_ef = 64; p.guard_passed = 1;
    p.degree = 64; p.intermediate = 128; p.ace_ef = ace_ef; p.n_rows = rows; p.partitions = partitions;
    return p;
}

static void test_readback(void)
{
    CuvsHnswAceReadback r;
    CuvsHnswPolicy p = ace_receipt(999000, 2, 240);
    CHECK(cuvs_hnsw_policy_valid(&p));
    /* Q3 shape: every partition holds 999,000 rows -> NN-descent at 5 + 15
     * iterations, kNN width 3 * (64 / 2) = 96 (not the receipt's 128). */
    cuvs_hnsw_ace_readback_fill(&r, CUVS_HNSW_ACE_QUALITY_AUTO, 240, 64, 999000, 2, 20);
    CHECK(r.magic == CUVS_HNSW_ACE_READBACK_MAGIC && r.version == 1 && r.bytes == 32 && sizeof(r) == 32);
    CHECK(r.quality_source == CUVS_HNSW_ACE_SOURCE_AUTO && r.quality == 15);
    CHECK(r.local_intermediate == 96 && r.local_iterations == 20 && r.reserved == 0);
    CHECK(cuvs_hnsw_ace_readback_valid(&r, &p));
    cuvs_hnsw_ace_readback_fill(&r, CUVS_HNSW_ACE_QUALITY_AUTO, 240, 32, 999000, 2, 20);
    CHECK(r.local_intermediate == 48);
    /* Manual Q7 -> 12 iterations below 1e6 rows. */
    p.ace_ef = 112;
    cuvs_hnsw_ace_readback_fill(&r, 7, 112, 64, 999000, 2, 20);
    CHECK(r.quality_source == CUVS_HNSW_ACE_SOURCE_MANUAL && r.quality == 7 && r.local_iterations == 12);
    CHECK(cuvs_hnsw_ace_readback_valid(&r, &p));
    /* 1e6 rows in two partitions: the positive-budget ledger's NN-descent at the
     * SDK default iterations, whatever the quality. */
    cuvs_hnsw_ace_readback_fill(&r, 7, 112, 64, 1000000, 2, 20);
    CHECK(r.local_iterations == 20);
    cuvs_hnsw_ace_readback_fill(&r, CUVS_HNSW_ACE_QUALITY_AUTO, 240, 64, 1000000, 2, 25);
    CHECK(r.local_iterations == 25);
    /* More partitions over 1e6 rows: per-partition values differ -> 0. */
    cuvs_hnsw_ace_readback_fill(&r, CUVS_HNSW_ACE_QUALITY_AUTO, 240, 64, 3000000, 4, 20);
    CHECK(r.local_iterations == 0);
    cuvs_hnsw_ace_readback_fill(&r, CUVS_HNSW_ACE_QUALITY_AUTO, 240, 64, 999000, 8, 20);
    CHECK(r.local_iterations == 20);

    /* Validity is bound to the receipt. */
    p = ace_receipt(999000, 2, 240);
    cuvs_hnsw_ace_readback_fill(&r, CUVS_HNSW_ACE_QUALITY_AUTO, 240, 64, 999000, 2, 20);
    CuvsHnswAceReadback bad = r; bad.magic ^= 1; CHECK(!cuvs_hnsw_ace_readback_valid(&bad, &p));
    bad = r; bad.bytes = 28; CHECK(!cuvs_hnsw_ace_readback_valid(&bad, &p));
    bad = r; bad.quality = 7; CHECK(!cuvs_hnsw_ace_readback_valid(&bad, &p));
    /* self-consistent q and iterations that are not the receipt's A */
    bad = r; bad.quality = 7; bad.local_iterations = 12; CHECK(!cuvs_hnsw_ace_readback_valid(&bad, &p));
    bad = r; bad.local_intermediate = 128; CHECK(!cuvs_hnsw_ace_readback_valid(&bad, &p));
    bad = r; bad.local_iterations = 12; CHECK(!cuvs_hnsw_ace_readback_valid(&bad, &p));
    bad = r; bad.quality_source = 2; CHECK(!cuvs_hnsw_ace_readback_valid(&bad, &p));
    bad = r; bad.reserved = 1; CHECK(!cuvs_hnsw_ace_readback_valid(&bad, &p));
    CuvsHnswPolicy resident = p; resident.route = CUVS_HNSW_ROUTE_IVF_PQ;
    CHECK(!cuvs_hnsw_ace_readback_valid(&r, &resident));
    CHECK(!cuvs_hnsw_ace_readback_valid(NULL, &p));

    /* Bounded tail reader: older ACE indexes have no tail; a short or
     * mismatched tail is corrupt; a future tail version is unsupported. */
    unsigned char page[64] = {0};
    CuvsHnswAceReadback out;
    CHECK(cuvs_hnsw_ace_readback_read(page, 0, &p, &out) == CUVS_HNSW_POLICY_NOT_RECORDED);
    CHECK(cuvs_hnsw_ace_readback_read(page, sizeof(page), &p, &out) == CUVS_HNSW_POLICY_NOT_RECORDED);
    memcpy(page, &r, sizeof(r));
    CHECK(cuvs_hnsw_ace_readback_read(page, sizeof(r), &p, &out) == CUVS_HNSW_POLICY_RECORDED
          && out.local_intermediate == 96);
    CHECK(cuvs_hnsw_ace_readback_read(page, sizeof(r) - 1, &p, &out) == CUVS_HNSW_POLICY_CORRUPT);
    bad = r; bad.version = 2; memcpy(page, &bad, sizeof(bad));
    CHECK(cuvs_hnsw_ace_readback_read(page, sizeof(bad), &p, &out) == CUVS_HNSW_POLICY_UNSUPPORTED);
    bad = r; bad.quality = 9; memcpy(page, &bad, sizeof(bad));
    CHECK(cuvs_hnsw_ace_readback_read(page, sizeof(bad), &p, &out) == CUVS_HNSW_POLICY_CORRUPT);
}

int main(void)
{
    test_parse();
    test_rule();
    test_readback();
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("PASS ACE quality knob parser, auto = SDK ace default scaled with ef_construction, manual 16Q, partition-builder readback and its bounded reader\n");
    return 0;
}
