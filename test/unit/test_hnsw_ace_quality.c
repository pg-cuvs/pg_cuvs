/* UC-5 ACE quality rule, daemon knob parser, and partition-builder readback
 * (cuvs_hnsw_policy.h). CPU-only; expected values are computed here from the
 * cuVS 26.04 contract, not by calling the helpers under test. */
#include "cuvs_hnsw_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    /* auto: q = T - 5 at the reloption default (T = the SDK's default NN-descent
     * iterations), A = ceil(16 (T - 5) E / 64) elsewhere, bounded to [4, 65535]. */
    const uint32_t sdk_iterations[] = {20, 25, 6};
    for (size_t i = 0; i < 3; i++)
    {
        uint32_t t = sdk_iterations[i], base = 16 * (t - 5), last = 0;
        CHECK(cuvs_hnsw_ace_ef(CUVS_HNSW_ACE_QUALITY_AUTO, 64, t) == base);
        CHECK(cuvs_hnsw_ace_ef(CUVS_HNSW_ACE_QUALITY_AUTO, 64, t) / 16 + 5 == t);
        for (uint32_t ef = 4; ef <= 1000; ef++)
        {
            uint32_t a = cuvs_hnsw_ace_ef(CUVS_HNSW_ACE_QUALITY_AUTO, ef, t);
            uint64_t want = (uint64_t)base * ef;
            if (a > 4) CHECK((uint64_t)a * 64 >= want && (uint64_t)(a - 1) * 64 < want);
            else CHECK(want <= 4 * 64);
            CHECK(a >= last);
            last = a;
        }
    }
    /* cuVS 26.04 (T 20) at the reloption default: A 240 -> q 15 -> 20 iterations. */
    CHECK(cuvs_hnsw_ace_ef(CUVS_HNSW_ACE_QUALITY_AUTO, 64, 20) == 240);
    CHECK(cuvs_hnsw_ace_ef(CUVS_HNSW_ACE_QUALITY_AUTO, 128, 20) == 480);
    /* An SDK without a usable iteration budget never underflows. */
    CHECK(cuvs_hnsw_ace_ef(CUVS_HNSW_ACE_QUALITY_AUTO, 64, 5) == 4);
    CHECK(cuvs_hnsw_ace_ef(CUVS_HNSW_ACE_QUALITY_AUTO, 64, 0) == 4);
    CHECK(cuvs_hnsw_ace_ef(CUVS_HNSW_ACE_QUALITY_AUTO, 1000, UINT32_MAX) == 65535);
    /* manual Q: exactly 16 Q whatever ef_construction is, so q == Q. */
    for (uint32_t qv = 0; qv <= CUVS_HNSW_ACE_QUALITY_MAX; qv++)
        for (uint32_t ef = 4; ef <= 1000; ef += 333)
        {
            uint32_t a = cuvs_hnsw_ace_ef(qv, ef, 20);
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

/* The effective ACE partition count is optional (CUVS_HNSW_HAS_SDK_PLAN == 0): the receipt then
 * carries the requested count and says so with one bit of the existing route_flags word. Expected
 * values are written out here, not derived from the helpers under test. */
static CuvsHnswPolicy nn_receipt(void)
{
    CuvsHnswPolicy p = {0};
    p.magic = CUVS_HNSW_POLICY_MAGIC; p.version = CUVS_HNSW_POLICY_NN_VERSION; p.bytes = sizeof(p);
    p.route = CUVS_HNSW_ROUTE_NN_DESCENT; p.internal_builder = CUVS_HNSW_ROUTE_NN_DESCENT;
    p.requested_algo = CUVS_HNSW_ALGO_NN_DESCENT; p.requested_ef = 64; p.guard_passed = 1;
    p.n_rows = 10000; p.dim = 8; p.degree = 64; p.intermediate = 128;
    return p;
}

static void test_ace_partitions_requested(void)
{
    const uint32_t F = 0x40u;
    CuvsHnswPolicy p = ace_receipt(999000, 2, 240), q, out;
    unsigned char page[sizeof(CuvsHnswPolicy) + 16] = {0};

    /* Stored layout: 160 bytes, route_flags at byte 92, magic and version as before; the mark adds
     * no field and no version. */
    CHECK(sizeof(CuvsHnswPolicy) == 160 && offsetof(CuvsHnswPolicy, route_flags) == 92);
    CHECK(CUVS_HNSW_POLICY_MAGIC == UINT32_C(0x43565031) && CUVS_HNSW_POLICY_VERSION == 2u);
    CHECK(CUVS_HNSW_ROUTE_FLAG_ACE_PARTITIONS_REQUESTED == F && (F & 0x3fu) == 0);
    CHECK(CUVS_HNSW_ROUTE_FLAGS_KNOWN == 0x7fu);

    /* A receipt written before the bit existed has it clear and keeps its meaning: the effective
     * count of the SDK source planner. */
    CHECK(cuvs_hnsw_policy_valid(&p) && !cuvs_hnsw_ace_partitions_unverified(&p));
    CHECK(strcmp(cuvs_hnsw_ace_partitions_origin(&p), "same_SDK_stable_explicit_plan") == 0);

    /* With the bit the receipt is still valid, partitions stay required, and the count is named as
     * requested. */
    q = p; q.route_flags |= F;
    CHECK(cuvs_hnsw_policy_valid(&q) && cuvs_hnsw_ace_partitions_unverified(&q));
    CHECK(strcmp(cuvs_hnsw_ace_partitions_origin(&q), "requested_effective_not_guaranteed") == 0);
    q.partitions = 0; CHECK(!cuvs_hnsw_policy_valid(&q));
    /* Every ACE route may be requested-only: explicit, auto overflow, kNN-graph fallback. */
    q = p; q.requested_algo = CUVS_HNSW_ALGO_AUTO;
    q.route_flags = CUVS_HNSW_ROUTE_FLAG_AUTO_OVERFLOW | F; CHECK(cuvs_hnsw_policy_valid(&q));
    q.route_flags = CUVS_HNSW_ROUTE_FLAG_GRAPH_FALLBACK | F; CHECK(cuvs_hnsw_policy_valid(&q));
    q.route_flags = F; CHECK(!cuvs_hnsw_policy_valid(&q)); /* auto -> ACE still needs its own record */
    /* The bit means nothing off the ACE route, and no other new bit is accepted. */
    q = p; q.route = q.internal_builder = CUVS_HNSW_ROUTE_IVF_PQ; q.requested_algo = CUVS_HNSW_ALGO_IVF_PQ;
    q.partitions = 0; CHECK(cuvs_hnsw_policy_valid(&q));
    q.route_flags |= F; CHECK(!cuvs_hnsw_policy_valid(&q));
    CHECK(strcmp(cuvs_hnsw_ace_partitions_origin(&q), "not_applicable") == 0);
    CHECK(!cuvs_hnsw_ace_partitions_unverified(&q)); /* a stray bit off the ACE route claims nothing */
    q = p; q.route = q.internal_builder = CUVS_HNSW_ROUTE_TINY; q.requested_algo = CUVS_HNSW_ALGO_AUTO;
    q.partitions = 0; CHECK(cuvs_hnsw_policy_valid(&q));
    q.route_flags |= F; CHECK(!cuvs_hnsw_policy_valid(&q));
    q = nn_receipt(); CHECK(cuvs_hnsw_policy_valid(&q));
    q.route_flags |= F; CHECK(!cuvs_hnsw_policy_valid(&q));
    q = p; q.route_flags |= 0x80u; CHECK(!cuvs_hnsw_policy_valid(&q));

    /* The stored bytes carry the mark through the bounded reader unchanged. */
    q = p; q.route_flags |= F;
    memcpy(page, &q, sizeof(q));
    CHECK(cuvs_hnsw_policy_read(page, sizeof(q), &out) == CUVS_HNSW_POLICY_RECORDED
          && out.route_flags == F && out.partitions == 2 && out.version == CUVS_HNSW_POLICY_VERSION);
    memcpy(page, &p, sizeof(p));
    CHECK(cuvs_hnsw_policy_read(page, sizeof(p), &out) == CUVS_HNSW_POLICY_RECORDED
          && out.route_flags == 0 && !cuvs_hnsw_ace_partitions_unverified(&out));
}

static void test_ace_iterations_guarantee(void)
{
    CuvsHnswAceReadback r;
    CuvsHnswPolicy p;
    const uint32_t F = CUVS_HNSW_ROUTE_FLAG_ACE_PARTITIONS_REQUESTED;

    /* Under 1e6 rows a partition runs 5 + q iterations whatever the partition count: exact either way. */
    p = ace_receipt(999000, 2, 240); p.route_flags = F;
    cuvs_hnsw_ace_readback_fill(&r, CUVS_HNSW_ACE_QUALITY_AUTO, 240, 64, 999000, 2, 20);
    CHECK(r.local_iterations == 20 && cuvs_hnsw_ace_iterations_guaranteed(&p, &r));
    /* From 1e6 rows the exact value exists only for two partitions, and a requested-only count of 2
     * does not guarantee that the SDK built two. */
    p = ace_receipt(1000000, 2, 240);
    cuvs_hnsw_ace_readback_fill(&r, CUVS_HNSW_ACE_QUALITY_AUTO, 240, 64, 1000000, 2, 20);
    CHECK(r.local_iterations == 20 && cuvs_hnsw_ace_iterations_guaranteed(&p, &r));
    p.route_flags = F;
    CHECK(cuvs_hnsw_policy_valid(&p) && cuvs_hnsw_ace_readback_valid(&r, &p));
    CHECK(!cuvs_hnsw_ace_iterations_guaranteed(&p, &r));
    /* With more partitions the readback already claims no exact value (0). */
    p = ace_receipt(3000000, 4, 240); p.route_flags = F;
    cuvs_hnsw_ace_readback_fill(&r, CUVS_HNSW_ACE_QUALITY_AUTO, 240, 64, 3000000, 4, 20);
    CHECK(r.local_iterations == 0 && cuvs_hnsw_ace_iterations_guaranteed(&p, &r));
    /* The stored readback is the same with and without the mark: only the reading differs. */
    {
        CuvsHnswAceReadback plain, marked;
        cuvs_hnsw_ace_readback_fill(&plain, CUVS_HNSW_ACE_QUALITY_AUTO, 240, 64, 1000000, 2, 20);
        marked = plain;
        CHECK(memcmp(&plain, &marked, sizeof(plain)) == 0 && sizeof(plain) == 32 && plain.reserved == 0);
    }
}

int main(void)
{
    test_parse();
    test_rule();
    test_readback();
    test_ace_partitions_requested();
    test_ace_iterations_guarantee();
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("PASS ACE quality knob parser, auto = 16 (SDK NN-descent iterations - 5) scaled with ef_construction, manual 16Q, partition-builder readback and its bounded reader, requested-only partition count mark (receipt validation, origin, readback exactness)\n");
    return 0;
}
