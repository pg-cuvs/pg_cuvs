/*
 * CPU unit tests for the user-visible rejection text built by the backend
 * from a decoded err_out payload (v6 r18 C5 DETAIL templates and HINT rules,
 * UC-2 decision 2; CK-8, CK-29): cuvs_admission_format in cuvs_util.c.
 */
#include "cuvs_util.h"
#include "cuvs_admission_contract.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(label, cond)                                                  \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s (line %d)\n", (label), __LINE__);      \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

#define HAS(label, hay, needle) CHECK(label, strstr((hay), (needle)) != NULL)
#define LACKS(label, hay, needle) CHECK(label, strstr((hay), (needle)) == NULL)

#define MY_DB 16384u

/* Fake catalog: same-DB OIDs 100..199 resolve; the caller owns 100 and 101. */
typedef struct FakeCatalog { int superuser; } FakeCatalog;

static int
fake_name(void *ctx, uint32_t index_oid, char *buf, size_t cap)
{
    (void)ctx;
    if (index_oid < 100 || index_oid > 199)
        return -1;
    if (index_oid == 150)
        snprintf(buf, cap, "\"My Schema\".\"odd\"\"name\"");
    else if (index_oid == 151)
        snprintf(buf, cap, "public.\"it's\"");
    else
        snprintf(buf, cap, "public.idx_%u", index_oid);
    return 0;
}

static int
fake_can_unload(void *ctx, uint32_t db_oid, uint32_t index_oid)
{
    const FakeCatalog *c = (const FakeCatalog *)ctx;
    if (c->superuser)
        return 1;
    return db_oid == MY_DB && (index_oid == 100 || index_oid == 101 || index_oid == 150 ||
                                index_oid == 151);
}

typedef struct Out {
    char msg[CUVS_ADMISSION_MSG_BYTES];
    char detail[CUVS_ADMISSION_MSG_BYTES];
    char hint[CUVS_ADMISSION_MSG_BYTES];
    const char *sqlstate;
} Out;

static Out
format(const CuvsAdmissionFields *f, CuvsAdmissionObject obj, int superuser,
       const char *load_name)
{
    Out o;
    FakeCatalog cat;
    CuvsAdmissionCatalog c;
    memset(&o, 0, sizeof(o));
    cat.superuser = superuser;
    c.my_db_oid = MY_DB;
    c.superuser = superuser;
    c.index_name = fake_name;
    c.can_unload = fake_can_unload;
    c.ctx = &cat;
    o.sqlstate = cuvs_admission_format(f, obj, load_name, &c, o.msg, sizeof(o.msg),
                                       o.detail, sizeof(o.detail), o.hint,
                                       sizeof(o.hint));
    return o;
}

static CuvsAdmissionFields
base(CuvsAdmissionReason r)
{
    CuvsAdmissionFields f;
    memset(&f, 0, sizeof(f));
    f.reason = r;
    return f;
}

static void
cand(CuvsAdmissionFields *f, uint32_t db, uint32_t idx, uint32_t mib, int32_t idle)
{
    CuvsAdmissionCandidate *c = &f->cand[f->n_cand++];
    c->db_oid = db;
    c->index_oid = idx;
    c->mib = mib;
    c->idle_s = idle;
}

static CuvsAdmissionFields
ar3(void)
{
    CuvsAdmissionFields f = base(CUVS_ADMISSION_AR3);
    f.present = CUVS_ADM_F_N | CUVS_ADM_F_F | CUVS_ADM_F_H | CUVS_ADM_F_E |
                CUVS_ADM_F_K | CUVS_ADM_F_R;
    f.n = 3049; f.f = 512; f.h_mib = 6000; f.h_cnt = 3; f.e_mib = 0; f.e_cnt = 0;
    f.k_mib = 1000; f.k_cnt = 1;
    return f;
}

static void
test_ar0_permanent(void)
{
    CuvsAdmissionFields f = base(CUVS_ADMISSION_AR0);
    Out o;
    f.present = CUVS_ADM_F_N | CUVS_ADM_F_Q;
    f.n = 27895; f.q = CUVS_ADMISSION_SCALAR_MAX;
    o = format(&f, CUVS_ADMISSION_OBJ_CAGRA, 0, NULL);
    CHECK("AR0 54000", o.sqlstate && strcmp(o.sqlstate, "54000") == 0);
    CHECK("AR0 msg", strcmp(o.msg, "pg_cuvs: GPU memory is insufficient for this CAGRA "
                                   "build (needs more than the configured limit); "
                                   "CREATE INDEX aborted") == 0);
    CHECK("AR0 detail", strcmp(o.detail, "needs 27895 MiB; the limit for one build is "
                                         "99999999+ MiB (pool quota, or the logical VRAM "
                                         "budget when the pool is disabled)") == 0);
    HAS("AR0 hint ace", o.hint, "build_algo='auto'");
    HAS("AR0 hint degree", o.hint, "graph_degree");
    LACKS("AR0 no pgvector", o.hint, "pgvector HNSW");
    f.present |= CUVS_ADM_F_I;
    f.i = 1;
    o = format(&f, CUVS_ADMISSION_OBJ_SHARD, 0, NULL);
    CHECK("AR0 shard prefix", strncmp(o.detail, "shard 1: needs 27895 MiB", 24) == 0);
    HAS("AR0 shard hint", o.hint, "cuvs.shard_count");
    f.present &= ~(uint32_t)CUVS_ADM_F_I;
    o = format(&f, CUVS_ADMISSION_OBJ_IVFPQ, 0, NULL);
    CHECK("AR0 ivfpq 54000", o.sqlstate && strcmp(o.sqlstate, "54000") == 0);
    HAS("AR0 ivfpq msg", o.msg, "this IVF-PQ build (needs more than");
    LACKS("AR0 ivfpq no degree", o.hint, "graph_degree");
}

static void
test_ar1_other_build(void)
{
    CuvsAdmissionFields f = base(CUVS_ADMISSION_AR2);
    Out o;
    f.present = CUVS_ADM_F_N | CUVS_ADM_F_Q | CUVS_ADM_F_P | CUVS_ADM_F_F;
    f.n = 10; f.f = 4; f.q = 20; f.p = 9;
    o = format(&f, CUVS_ADMISSION_OBJ_CAGRA, 0, NULL);
    CHECK("AR2 53200", strcmp(o.sqlstate, "53200") == 0);
    HAS("AR2 msg", o.msg, "while another CREATE INDEX holds GPU memory");
    CHECK("AR2 detail", strcmp(o.detail, "needs 10 MiB; 4 MiB free (quota 20 MiB); other "
                                         "CREATE INDEX builds hold 9 MiB") == 0);
}

static void
test_candidates_and_names(void)
{
    CuvsAdmissionFields f = ar3();
    Out o;
    cand(&f, 999, 7, 2048, 3600);        /* other DB: OID only */
    cand(&f, MY_DB, 150, 1024, 125);     /* quoted qualified name */
    cand(&f, MY_DB, 300, 10, -1);        /* same DB, name not found, never */
    o = format(&f, CUVS_ADMISSION_OBJ_CAGRA, 0, NULL);
    CHECK("AR3 53200", strcmp(o.sqlstate, "53200") == 0);
    CHECK("AR3 msg", strcmp(o.msg, "pg_cuvs: GPU memory is insufficient for this CAGRA "
                                   "build; CREATE INDEX aborted") == 0);
    CHECK("AR3 detail",
          strcmp(o.detail, "need 3049 MiB, free 512 MiB, resident 6000 MiB in 3 indexes "
                           "(pinned 1000 MiB in 1); 0 MiB in 0 indexes would be evictable "
                           "under scope lru. Idle candidates: (db 999) oid 7 2048 MiB idle "
                           "60 min; \"My Schema\".\"odd\"\"name\" 1024 MiB idle 2 min; oid 300 "
                           "10 MiB idle never") == 0);
    /* Owner of the second candidate: the first unloadable one is named. */
    HAS("AR3 owner hint cmd", o.hint,
        "SELECT pg_cuvs_unload('\"My Schema\".\"odd\"\"name\"'); then retry CREATE INDEX.");
    HAS("AR3 reload note", o.hint, "(the unloaded index reloads on its next query)");
    HAS("AR3 sidecar note", o.hint, "CPU replan until REINDEX");
    LACKS("AR3 no wait", o.hint, "Or wait");
    /* Superuser: the first candidate is in another DB -> OID overload. */
    o = format(&f, CUVS_ADMISSION_OBJ_CAGRA, 1, NULL);
    HAS("AR3 su overload", o.hint, "SELECT pg_cuvs_unload(999, 7); then retry CREATE INDEX.");
}

static void
test_literal_quoting(void)
{
    CuvsAdmissionFields f = ar3();
    Out o;
    cand(&f, MY_DB, 151, 10, 60);
    o = format(&f, CUVS_ADMISSION_OBJ_CAGRA, 0, NULL);
    HAS("literal doubled quote", o.hint,
        "SELECT pg_cuvs_unload('public.\"it''s\"'); then retry CREATE INDEX.");
    HAS("detail raw name", o.detail, "public.\"it's\" 10 MiB idle 1 min");
}

static void
test_hint_without_privilege(void)
{
    CuvsAdmissionFields f = ar3();
    Out o;
    cand(&f, 999, 7, 2048, 3600);
    cand(&f, MY_DB, 120, 1024, 60);      /* same DB, not owned */
    o = format(&f, CUVS_ADMISSION_OBJ_CAGRA, 0, NULL);
    HAS("ask superuser", o.hint,
        "Ask a superuser to unload an idle index (see pg_cuvs_index_status()).");
    LACKS("ask: no command", o.hint, "SELECT pg_cuvs_unload");
    HAS("ask: sidecar", o.hint, "CPU replan until REINDEX");
}

static void
test_zero_candidates(void)
{
    CuvsAdmissionFields f = ar3();
    Out o;
    f.r = 1;                              /* never a wait line without candidates */
    o = format(&f, CUVS_ADMISSION_OBJ_CAGRA, 0, NULL);
    HAS("none detail", o.detail,
        "Idle candidates: none (every other resident index is pinned or in use)");
    CHECK("pinned hint", strcmp(o.hint, "Pinned indexes hold 1000 MiB; release one with "
                                        "pg_cuvs_unpin(index) (owner or superuser), or "
                                        "retry after current queries finish") == 0);
    f.k_mib = 0; f.k_cnt = 0;
    o = format(&f, CUVS_ADMISSION_OBJ_CAGRA, 0, NULL);
    CHECK("in-use hint", strcmp(o.hint, "All resident indexes are in use; retry after "
                                        "current queries finish") == 0);
}

static void
test_wait_line(void)
{
    CuvsAdmissionFields f = ar3();
    Out o;
    cand(&f, MY_DB, 100, 64, 900);
    f.r = 1;
    o = format(&f, CUVS_ADMISSION_OBJ_CAGRA, 0, NULL);
    HAS("wait line", o.hint, "Or wait: background reclaim will free space for a build of "
                             "this size within the next reclaim tick.");
    f.present &= ~(uint32_t)CUVS_ADM_F_R; /* missing r never authorizes a wait */
    o = format(&f, CUVS_ADMISSION_OBJ_CAGRA, 0, NULL);
    LACKS("no r no wait", o.hint, "Or wait");
}

static void
test_ar5_build_self(void)
{
    CuvsAdmissionFields f = base(CUVS_ADMISSION_AR5);
    Out o;
    f.present = CUVS_ADM_F_N | CUVS_ADM_F_F | CUVS_ADM_F_H | CUVS_ADM_F_E |
                CUVS_ADM_F_K | CUVS_ADM_F_S | CUVS_ADM_F_R;
    f.n = 100; f.f = 1; f.h_mib = 200; f.h_cnt = 2; f.e_mib = 150; f.e_cnt = 1;
    f.k_mib = 50; f.k_cnt = 1; f.s = 90;
    cand(&f, MY_DB, 101, 150, 7200);
    o = format(&f, CUVS_ADMISSION_OBJ_CAGRA, 0, NULL);
    HAS("AR5 msg", o.msg, "would require evicting resident indexes, which the build "
                          "eviction scope does not allow; CREATE INDEX aborted");
    CHECK("AR5 detail",
          strcmp(o.detail, "need 100 MiB, free 1 MiB, resident 200 MiB in 2 indexes (pinned "
                           "50 MiB in 1); nothing was evicted because the build eviction "
                           "scope does not allow it; 150 MiB in 1 indexes would be evictable "
                           "under scope lru; this index is already resident with 90 MiB. "
                           "Idle candidates: public.idx_101 150 MiB idle 120 min") == 0);
    HAS("AR5 unpin clause", o.hint,
        "(if the index itself is pinned, release it first with pg_cuvs_unpin(index))");
    HAS("AR5 scope knob", o.hint, "--build-evict-scope");
    f.s = 0;
    o = format(&f, CUVS_ADMISSION_OBJ_CAGRA, 0, NULL);
    LACKS("AR5 s=0 no self", o.detail, "already resident");
    LACKS("AR5 s=0 no unpin", o.hint, "release it first");
}

static void
test_shard_and_ivfpq(void)
{
    CuvsAdmissionFields f = ar3();
    Out o;
    f.present |= CUVS_ADM_F_I;
    f.i = 2;
    cand(&f, MY_DB, 100, 64, 900);
    o = format(&f, CUVS_ADMISSION_OBJ_SHARD, 0, NULL);
    HAS("shard msg", o.msg, "this shard of the CAGRA build");
    CHECK("shard detail prefix", strncmp(o.detail, "shard 2: need 3049 MiB", 22) == 0);
    HAS("shard hint", o.hint, "cuvs.shard_count");
    {
        /* A shard can meet another build's pending reservation (AR1/AR2). */
        CuvsAdmissionFields g = base(CUVS_ADMISSION_AR1);
        Out so;
        g.present = CUVS_ADM_F_N | CUVS_ADM_F_Q | CUVS_ADM_F_P | CUVS_ADM_F_F | CUVS_ADM_F_I;
        g.i = 4;
        so = format(&g, CUVS_ADMISSION_OBJ_SHARD, 0, NULL);
        CHECK("shard AR1 53200", so.sqlstate && strcmp(so.sqlstate, "53200") == 0);
        CHECK("shard AR1 prefix", strncmp(so.detail, "shard 4: needs 0 MiB", 20) == 0);
    }
    o = format(&f, CUVS_ADMISSION_OBJ_IVFPQ, 0, NULL);
    HAS("ivfpq msg", o.msg, "this IVF-PQ build; CREATE INDEX aborted");
    LACKS("ivfpq no shard prefix", o.detail, "shard 2:");
}

static void
test_reload_variants(void)
{
    CuvsAdmissionFields f = base(CUVS_ADMISSION_AR5);
    Out o;
    f.present = CUVS_ADM_F_N | CUVS_ADM_F_F | CUVS_ADM_F_E | CUVS_ADM_F_K | CUVS_ADM_F_W;
    f.n = 80; f.f = 2; f.e_mib = 90; f.e_cnt = 1; f.k_mib = 0; f.k_cnt = 0;
    f.w = CUVS_ADMISSION_WAIT_QUERIES;
    cand(&f, MY_DB, 100, 30, 10);
    o = format(&f, CUVS_ADMISSION_OBJ_LOAD, 0, "public.items_idx");
    CHECK("load 53200", strcmp(o.sqlstate, "53200") == 0);
    CHECK("load msg", strcmp(o.msg, "pg_cuvs: GPU memory is unavailable to load index "
                                    "\"public.items_idx\"") == 0);
    HAS("load AR5 detail", o.detail, "nothing was evicted because evicting every unpinned "
                                     "idle index would still not make room; 90 MiB in 1 "
                                     "indexes are pinned or in use and would make room once "
                                     "released (running searches release them when they "
                                     "finish). Idle candidates: public.idx_100 30 MiB idle 0 min");
    CHECK("load w=0 hint prefix", strncmp(o.hint, "Retry once running searches finish, or ", 39) == 0);
    LACKS("load absent h is not zero", o.detail, "resident 0 MiB");
    HAS("load retries the query", o.hint, "then retry the query.");
    LACKS("load never says CREATE INDEX", o.hint, "retry CREATE INDEX");
    LACKS("load w=0 no unpin", o.hint, "pg_cuvs_unpin");
    f.w = CUVS_ADMISSION_WAIT_UNPIN;
    o = format(&f, CUVS_ADMISSION_OBJ_LOAD, 0, "public.items_idx");
    HAS("load w=-1 detail", o.detail, "(a pinned index is released only by pg_cuvs_unpin)");
    LACKS("load w=-1 no retry prefix", o.hint, "Retry once running searches finish");
    HAS("load w=-1 unpin", o.hint, "pg_cuvs_unpin(index)");
    /* CK-8: every search-path reload rejection stays 53200, AR0 included,
     * with load wording rather than build advice. */
    f = base(CUVS_ADMISSION_AR0);
    f.present = CUVS_ADM_F_N | CUVS_ADM_F_Q;
    f.n = 300; f.q = 200;
    o = format(&f, CUVS_ADMISSION_OBJ_LOAD, 0, "public.items_idx");
    CHECK("load AR0 53200", o.sqlstate && strcmp(o.sqlstate, "53200") == 0);
    CHECK("load AR0 detail", strcmp(o.detail, "needs 300 MiB; the limit for one index is "
                                              "200 MiB (pool quota, or the logical VRAM "
                                              "budget when the pool is disabled)") == 0);
    LACKS("load AR0 no build advice", o.hint, "pg_cuvs_hnsw");
    HAS("load AR0 hint", o.hint, "--max-vram-mb");
    f = base(CUVS_ADMISSION_AR1);
    f.present = CUVS_ADM_F_N | CUVS_ADM_F_F | CUVS_ADM_F_Q | CUVS_ADM_F_P;
    o = format(&f, CUVS_ADMISSION_OBJ_LOAD, 0, "public.items_idx");
    HAS("load AR1 running", o.hint, "a CREATE INDEX may be running");
    HAS("load AR1 reset", o.hint, "pg_cuvs_reset_circuit");
    f = base(CUVS_ADMISSION_AR4);
    f.present = CUVS_ADM_F_N | CUVS_ADM_F_F | CUVS_ADM_F_H | CUVS_ADM_F_E | CUVS_ADM_F_K;
    cand(&f, MY_DB, 100, 30, 10);
    o = format(&f, CUVS_ADMISSION_OBJ_LOAD, 0, "public.items_idx");
    HAS("load AR4 soft", o.detail, "even with protection time ignored (soft eviction)");
    LACKS("load AR4 no running", o.hint, "a CREATE INDEX may be running");
}

static void
test_after_admission_and_workspace(void)
{
    CuvsAdmissionFields f = base(CUVS_ADMISSION_OOM);
    Out o;
    f.present = CUVS_ADM_F_N | CUVS_ADM_F_F | CUVS_ADM_F_H;
    f.n = 5; f.f = 6; f.h_mib = 7; f.h_cnt = 1;
    o = format(&f, CUVS_ADMISSION_OBJ_CAGRA, 0, NULL);
    CHECK("OOM msg", strcmp(o.msg, "pg_cuvs: GPU memory exhausted during CAGRA build; "
                                   "CREATE INDEX aborted. No resident index was evicted") == 0);
    CHECK("OOM detail", strcmp(o.detail, "needs 5 MiB; 6 MiB was free at admission; 7 MiB "
                                         "held by 1 resident indexes; no resident index was "
                                         "evicted") == 0);
    HAS("OOM hint", o.hint, "--rmm-scratch-permille");
    f = base(CUVS_ADMISSION_ATT);
    f.present = CUVS_ADM_F_N;
    f.n = 5;
    o = format(&f, CUVS_ADMISSION_OBJ_CAGRA, 0, NULL);
    CHECK("ATT detail", strcmp(o.detail, "needs 5 MiB") == 0);
    HAS("ATT msg", o.msg, "could not attach the dataset");
    f = base(CUVS_ADMISSION_AR6);
    f.present = CUVS_ADM_F_N | CUVS_ADM_F_L | CUVS_ADM_F_Y | CUVS_ADM_F_U;
    f.n = 1; f.l = 2; f.y = 3; f.u = 4;
    o = format(&f, CUVS_ADMISSION_OBJ_CAGRA, 0, NULL);
    CHECK("AR6 53400", strcmp(o.sqlstate, "53400") == 0);
    CHECK("AR6 detail", strcmp(o.detail, "large_workspace estimate 2 MiB plus 3 MiB held by "
                                         "other managed builds exceeds physical free memory 4 "
                                         "MiB; the pool part of the build needs 1 MiB") == 0);
    f = base(CUVS_ADMISSION_AR7);
    f.present = CUVS_ADM_F_N | CUVS_ADM_F_L | CUVS_ADM_F_Y | CUVS_ADM_F_G;
    f.l = 2; f.y = 0; f.g = 8;
    o = format(&f, CUVS_ADMISSION_OBJ_CAGRA, 0, NULL);
    CHECK("AR7 53200", strcmp(o.sqlstate, "53200") == 0);
    HAS("AR7 detail", o.detail, "would spill beyond the host memory guard 8 MiB");
}

static void
test_rejects_bad_input(void)
{
    CuvsAdmissionFields f = base(CUVS_ADMISSION_NONE);
    Out o = format(&f, CUVS_ADMISSION_OBJ_CAGRA, 0, NULL);
    CHECK("NONE", o.sqlstate == NULL);
    f = ar3();
    f.n_cand = 4;
    o = format(&f, CUVS_ADMISSION_OBJ_CAGRA, 0, NULL);
    CHECK("4 cands", o.sqlstate == NULL);
    f = base(CUVS_ADMISSION_AR6);
    o = format(&f, CUVS_ADMISSION_OBJ_LOAD, 0, "x");
    CHECK("AR6 on load", o.sqlstate == NULL);
}

static void
test_ar8_pool_capacity(void)
{
    CuvsAdmissionFields f = base(CUVS_ADMISSION_AR8);
    Out o;
    f.present = CUVS_ADM_F_N | CUVS_ADM_F_Q | CUVS_ADM_F_P | CUVS_ADM_F_D | CUVS_ADM_F_O;
    f.n = 3048; f.q = 6492; f.p = 0; f.d = 457; f.o = 3048;
    o = format(&f, CUVS_ADMISSION_OBJ_CAGRA, 1, NULL);
    CHECK("AR8 53200", o.sqlstate && strcmp(o.sqlstate, "53200") == 0);
    CHECK("AR8 msg", strcmp(o.msg, "pg_cuvs: this CAGRA build does not fit the GPU memory "
                                   "pool with headroom; CREATE INDEX aborted") == 0);
    CHECK("AR8 detail", strcmp(o.detail, "need 3048 MiB + headroom 457 MiB > pool quota "
                                         "6492 MiB (pending 0 MiB, pool used 3048 MiB)") == 0);
    CHECK("AR8 hint", strcmp(o.hint, "Lower the index size or graph_degree (for example "
                                     "ALTER INDEX ... SET (graph_degree = 32) and REINDEX), "
                                     "or an operator can raise --rmm-pool-max-percent.") == 0);
    LACKS("AR8 no unpin", o.hint, "unpin");
    LACKS("AR8 no evict", o.hint, "evict");
    LACKS("AR8 no unload", o.hint, "pg_cuvs_unload");
    f.p = 512;
    o = format(&f, CUVS_ADMISSION_OBJ_CAGRA, 0, NULL);
    HAS("AR8 pending", o.detail, "(pending 512 MiB, pool used 3048 MiB)");
    /* AR8 is a CAGRA single/multi build reason only (shard path unchanged). */
    CHECK("AR8 not shard", format(&f, CUVS_ADMISSION_OBJ_SHARD, 0, NULL).sqlstate == NULL);
    CHECK("AR8 not ivfpq", format(&f, CUVS_ADMISSION_OBJ_IVFPQ, 0, NULL).sqlstate == NULL);
    CHECK("AR8 not load", format(&f, CUVS_ADMISSION_OBJ_LOAD, 0, "x").sqlstate == NULL);
}

/* AR0..AR7 errmsg, SQLSTATE and HINT text for a CAGRA build are pinned
 * byte for byte (AR8 must not change them). */
static void
test_ar0_to_ar7_text_pinned(void)
{
    static const struct { CuvsAdmissionReason r; const char *state, *msg, *hint; } pin[] = {
        {CUVS_ADMISSION_AR0, "54000",
         "pg_cuvs: GPU memory is insufficient for this CAGRA build (needs more than the "
         "configured limit); CREATE INDEX aborted",
         "For a large table use pg_cuvs_hnsw with build_algo='auto' (it spills to ACE on "
         "disk when over budget) or build_algo='ace'. A lower graph_degree slightly lowers "
         "the admission estimate. Operators can review --rmm-pool-max-percent, --max-vram-mb "
         "and --rmm-scratch-permille."},
        {CUVS_ADMISSION_AR1, "53200",
         "pg_cuvs: GPU memory is insufficient while another CREATE INDEX holds GPU memory; "
         "CREATE INDEX aborted",
         "Retry after the other CREATE INDEX finishes."},
        {CUVS_ADMISSION_AR2, "53200",
         "pg_cuvs: GPU memory is insufficient while another CREATE INDEX holds GPU memory; "
         "CREATE INDEX aborted",
         "Retry after the other CREATE INDEX finishes."},
        {CUVS_ADMISSION_AR3, "53200",
         "pg_cuvs: GPU memory is insufficient for this CAGRA build; CREATE INDEX aborted",
         NULL},
        {CUVS_ADMISSION_AR4, "53200",
         "pg_cuvs: GPU memory is insufficient for this CAGRA build; CREATE INDEX aborted",
         NULL},
        {CUVS_ADMISSION_AR5, "53200",
         "pg_cuvs: this CAGRA build would require evicting resident indexes, which the build "
         "eviction scope does not allow; CREATE INDEX aborted",
         NULL},
        {CUVS_ADMISSION_AR6, "53400",
         "pg_cuvs: large_workspace would not fit in GPU memory and managed overcommit is "
         "disabled; CREATE INDEX aborted",
         "If slower builds are acceptable, an operator can enable --managed-overcommit on; "
         "or build smaller tables. (--cagra-build-overflow error keeps large_workspace in "
         "the pool and does not fix a physical memory shortage.)"},
        {CUVS_ADMISSION_AR7, "53200",
         "pg_cuvs: large_workspace would spill beyond the host memory guard; CREATE INDEX "
         "aborted",
         "Free host memory, retry after the other managed builds finish, or review "
         "--managed-lw-margin-permille."},
    };
    for (size_t i = 0; i < sizeof(pin) / sizeof(pin[0]); i++)
    {
        CuvsAdmissionFields f = base(pin[i].r);
        Out o;
        char label[64];
        f.present = CUVS_ADM_F_N;
        o = format(&f, CUVS_ADMISSION_OBJ_CAGRA, 0, NULL);
        snprintf(label, sizeof(label), "AR%d pinned state", (int)pin[i].r);
        CHECK(label, o.sqlstate && strcmp(o.sqlstate, pin[i].state) == 0);
        snprintf(label, sizeof(label), "AR%d pinned msg", (int)pin[i].r);
        CHECK(label, strcmp(o.msg, pin[i].msg) == 0);
        if (pin[i].hint)
        {
            snprintf(label, sizeof(label), "AR%d pinned hint", (int)pin[i].r);
            CHECK(label, strcmp(o.hint, pin[i].hint) == 0);
        }
    }
}

int
main(void)
{
    test_ar0_permanent();
    test_ar1_other_build();
    test_candidates_and_names();
    test_literal_quoting();
    test_hint_without_privilege();
    test_zero_candidates();
    test_wait_line();
    test_ar5_build_self();
    test_shard_and_ivfpq();
    test_reload_variants();
    test_after_admission_and_workspace();
    test_rejects_bad_input();
    test_ar8_pool_capacity();
    test_ar0_to_ar7_text_pinned();
    if (g_failures) {
        fprintf(stderr, "test_admission_message: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_admission_message: all checks passed\n");
    return 0;
}
