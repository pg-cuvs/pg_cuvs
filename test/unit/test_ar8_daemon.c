/* ADR-089 AR8 through the actual daemon source (Linux, CPU shim): ensure_vram
 * refuses a CAGRA build when pending (build reservations + lw_pending) +
 * pool_used + N_m + headroom exceeds the pool quota, before AR6/AR7 and
 * before make_room (nothing evicted, nothing reserved), fills the AR8 err_out
 * subset, appends the [admit] fields, and parses --rmm-build-headroom-permille
 * strictly. Fixtures: physical free bytes, the host guard and the RMM pool
 * used bytes. Shapes are the Q6 condition (design 40c33a3b). */
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
size_t fixture_free(int device_id);
#define cuvs_vram_free_bytes_on fixture_free
#define cuvs_vram_physical_free_bytes_on fixture_free
#define cuvs_rmm_pool_stats fixture_pool_stats
#define main pg_cuvs_server_program_main
#include "../../src/pg_cuvs_server.c"
#undef main
#undef cuvs_rmm_pool_stats
#undef cuvs_vram_physical_free_bytes_on
#undef cuvs_vram_free_bytes_on
#include <assert.h>
#include <stdatomic.h>
#include <sys/wait.h>

static size_t g_free;
static uint64_t g_pool_used;
size_t fixture_free(int device_id) { (void)device_id; return g_free; }
int
fixture_pool_stats(int device_id, CuvsRmmPoolStats *out)
{
    memset(out, 0, sizeof(*out));
    out->device_id = (uint32_t)device_id;
    out->maximum_bytes = g_rmm_pool_limits[device_id];
    out->used_bytes = g_pool_used;
    out->reserved_bytes = g_pool_used;
    return 0;
}
static uint64_t fixture_guard(void) { return UINT64_C(1) << 40; }

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define GiB ((size_t)1024 * 1024 * 1024)
static const size_t Q6_QUOTA = (size_t)6807502080ull;
static const size_t Q6_N_M = (size_t)3196800000ull;           /* rho 0 */
static const uint64_t Q6_POOL_USED = UINT64_C(3196800000);    /* resident A_res */
static const uint64_t Q6_H150 = UINT64_C(479520000);
static const uint64_t Q6_H100 = UINT64_C(319680000);

static void
reset(void)
{
    cuvs_reclaim_params_default(&g_reclaim);
    memset(g_pending_build_vram, 0, sizeof(g_pending_build_vram));
    memset(g_pending_rmm_build_vram, 0, sizeof(g_pending_rmm_build_vram));
    memset(g_rmm_pool_limits, 0, sizeof(g_rmm_pool_limits));
    memset(g_max_vram_per_gpu, 0, sizeof(g_max_vram_per_gpu));
    memset(g_cache_evictions, 0, sizeof(g_cache_evictions));
    memset(g_lw_pending, 0, sizeof(g_lw_pending));
    g_n_indexes = 0;
    g_indexes = NULL;
    g_cagra_build_overflow = CUVS_LW_OVERFLOW_MANAGED;
    g_managed_overcommit = 1;
    g_managed_lw_margin_permille = 1100;
    g_rmm_build_headroom_permille = CUVS_RMM_BUILD_HEADROOM_PERMILLE_DEFAULT;
    g_lw_host_guard_fn = fixture_guard;
    g_rmm_pool_limits[0] = Q6_QUOTA;
    g_max_vram_per_gpu[0] = 80 * GiB;
    g_free = 80 * GiB;
    g_pool_used = 0;
}

static AdmitLine
line_for(int64_t n)
{
    AdmitLine a = {.op = "BUILD", .path = "single", .algo = "ivf_pq", .gpu = 0,
                   .db = 1, .idx = 2, .dim = 768, .gd = 32, .igd = 128, .n = n};
    a.final = a.needed = (size_t)cuvs_admission_estimate_final_bytes(n, 768, 32);
    return a;
}

/* ensure_vram for a CAGRA build; returns the reason (NONE when admitted). */
static int
admit(AdmitLine *a, CuvsAdmissionFields *rej)
{
    int rc = ensure_vram(a->needed, 0, CUVS_RQ_BUILD_CAGRA, rej, a);
    return rc == 0 ? CUVS_ADMISSION_NONE : g_last_admit_reason;
}

static void
test_q6(void)
{
    AdmitLine a;
    CuvsAdmissionFields rej;
    reset();
    g_pool_used = Q6_POOL_USED;
    pthread_mutex_lock(&g_index_mutex);

    /* 150: 3,196,800,000 + 3,196,800,000 + 479,520,000 = 101.0% -> AR8. */
    a = line_for(999000);
    CHECK(a.needed == Q6_N_M);
    CHECK(admit(&a, &rej) == CUVS_ADMISSION_AR8);
    CHECK(a.pool_valid && a.pool_need == Q6_N_M && a.pool_headroom == Q6_H150);
    CHECK(a.pool_used == Q6_POOL_USED && a.pool_pending == 0);
    CHECK(a.workspace.lw_ub > 0);                  /* lw_ub exists, is not a term */
    CHECK(rej.reason == CUVS_ADMISSION_AR8);
    CHECK(rej.present == (CUVS_ADM_F_N | CUVS_ADM_F_Q | CUVS_ADM_F_P | CUVS_ADM_F_D |
                          CUVS_ADM_F_O));
    CHECK(rej.n == 3048 && rej.q == 6492 && rej.p == 0 && rej.d == 457 && rej.o == 3048);
    CHECK(g_pending_rmm_build_vram[0] == 0 && g_pending_build_vram[0] == 0);
    CHECK(g_lw_pending[0] == 0 && g_cache_evictions[0] == 0);

    /* 100: 98.6% admitted. 0: 93.9% admitted (knob 0 does not close Q6). */
    g_rmm_build_headroom_permille = 100;
    a = line_for(999000);
    CHECK(admit(&a, NULL) == CUVS_ADMISSION_NONE && a.pool_headroom == Q6_H100);
    g_rmm_build_headroom_permille = 0;
    a = line_for(999000);
    CHECK(admit(&a, NULL) == CUVS_ADMISSION_NONE && a.pool_headroom == 0);
    g_rmm_build_headroom_permille = 150;

    /* Without the resident bytes the same request fits. */
    g_pool_used = 0;
    a = line_for(999000);
    CHECK(admit(&a, NULL) == CUVS_ADMISSION_NONE);

    /* Quota 0 (pool disabled): no AR8. */
    g_pool_used = Q6_POOL_USED;
    g_rmm_pool_limits[0] = 0;
    a = line_for(999000);
    CHECK(admit(&a, NULL) == CUVS_ADMISSION_NONE);
    g_rmm_pool_limits[0] = Q6_QUOTA;

    /* Without an AdmitLine (shard and manual callers) AR8 is never evaluated. */
    CHECK(ensure_vram(Q6_N_M, 0, CUVS_RQ_BUILD_CAGRA, NULL, NULL) == 0);
    CHECK(ensure_vram(Q6_N_M, 0, CUVS_RQ_BUILD_SHARD, NULL, NULL) == 0);
    pthread_mutex_unlock(&g_index_mutex);
}

/* Each term alone decides at the boundary: quota = sum admits, sum - 1
 * refuses. Half-Q6 shape N 1,598,400,000; 15% is 239,760,000, below the
 * 256 MiB floor, so headroom = 268,435,456. */
static void
test_terms_boundary(void)
{
    const uint64_t n = UINT64_C(1598400000), h = UINT64_C(268435456);
    const uint64_t one = (uint64_t)GiB;
    struct { uint64_t rmm, lw, used; } c[] = {
        {0, 0, 2 * one},          /* pool_used alone (nonzero) */
        {one, 0, 0},              /* build reservations alone */
        {0, 2 * one, 0},          /* lw_pending alone */
        {one, one, one},          /* all three */
    };
    AdmitLine a;
    reset();
    pthread_mutex_lock(&g_index_mutex);
    for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++)
    {
        uint64_t sum = c[i].rmm + c[i].lw + c[i].used + n + h;
        g_pending_rmm_build_vram[0] = (size_t)c[i].rmm;
        g_lw_pending[0] = c[i].lw;
        g_pool_used = c[i].used;
        g_rmm_pool_limits[0] = (size_t)sum;
        a = line_for(499500);
        CHECK(admit(&a, NULL) == CUVS_ADMISSION_NONE);
        CHECK(a.pool_pending == c[i].rmm + c[i].lw && a.pool_used == c[i].used);
        g_rmm_pool_limits[0] = (size_t)(sum - 1);
        a = line_for(499500);
        CHECK(admit(&a, NULL) == CUVS_ADMISSION_AR8);
    }
    g_pending_rmm_build_vram[0] = 0;
    g_lw_pending[0] = 0;
    pthread_mutex_unlock(&g_index_mutex);
}

/* Design concurrency (quota 100, N_m 60, headroom 10) scaled to real bytes:
 * two identical half-Q6 builds, quota 3.4e9, headroom 268,435,456 (floor).
 * The first sees 0 + 0 + N + h = 1,866,835,456 and is admitted; it reserves
 * N (and its lw_ub, managed policy). The second sees N + lw_ub + 0 + N + h
 * > 3.4e9 -> AR8. */
static atomic_int c_admitted, c_ar8, c_arrived, c_finish;
static void *
concurrent_build(void *unused)
{
    AdmitLine a = line_for(499500);
    int r;
    (void)unused;
    pthread_mutex_lock(&g_index_mutex);
    r = admit(&a, NULL);
    if (r == CUVS_ADMISSION_NONE)
    {
        reserve_build_vram(a.needed, 0);          /* what handle_build does */
        lw_pending_reserve(a.workspace.lw_ub, 0);
        atomic_fetch_add(&c_admitted, 1);
    }
    else if (r == CUVS_ADMISSION_AR8)
        atomic_fetch_add(&c_ar8, 1);
    pthread_mutex_unlock(&g_index_mutex);
    atomic_fetch_add(&c_arrived, 1);
    while (!atomic_load(&c_finish)) usleep(1000);
    if (r == CUVS_ADMISSION_NONE)
    {
        pthread_mutex_lock(&g_index_mutex);
        release_build_vram(a.needed, 0);
        lw_pending_release(a.workspace.lw_ub, 0);
        pthread_mutex_unlock(&g_index_mutex);
    }
    return NULL;
}

static void
test_concurrent(void)
{
    pthread_t t1, t2;
    AdmitLine a;
    CuvsAdmissionFields rej;
    reset();
    g_rmm_pool_limits[0] = (size_t)3400000000ull;
    atomic_store(&c_admitted, 0); atomic_store(&c_ar8, 0);
    atomic_store(&c_arrived, 0); atomic_store(&c_finish, 0);
    pthread_create(&t1, NULL, concurrent_build, NULL);
    pthread_create(&t2, NULL, concurrent_build, NULL);
    while (atomic_load(&c_arrived) < 2) usleep(1000);
    CHECK(atomic_load(&c_admitted) == 1 && atomic_load(&c_ar8) == 1);
    pthread_mutex_lock(&g_index_mutex);
    CHECK(g_pending_rmm_build_vram[0] == 1598400000u && g_lw_pending[0] > 0);
    /* The in-flight pending (N + lw_ub) is what the reply carries. */
    a = line_for(499500);
    CHECK(admit(&a, &rej) == CUVS_ADMISSION_AR8);
    CHECK(a.pool_pending == 1598400000u + g_lw_pending[0]);
    CHECK(rej.p == cuvs_admission_mib(a.pool_pending));
    pthread_mutex_unlock(&g_index_mutex);
    atomic_store(&c_finish, 1);
    pthread_join(t1, NULL); pthread_join(t2, NULL);
    CHECK(g_pending_rmm_build_vram[0] == 0 && g_lw_pending[0] == 0);
}

static void
test_order(void)
{
    AdmitLine a;
    IndexEntry resident = {.valid = 1, .is_flat = 1, .gpu_device_id = 0,
                           .db_oid = 1, .index_oid = 9, .main_bf_vram_bytes = GiB};
    reset();
    g_pool_used = Q6_POOL_USED;
    pthread_mutex_lock(&g_index_mutex);

    /* AR1 still comes first: pending beyond quota - N_m is AR1, not AR8. */
    g_pending_rmm_build_vram[0] = Q6_QUOTA - Q6_N_M + 1;
    a = line_for(999000);
    CHECK(admit(&a, NULL) == CUVS_ADMISSION_AR1);
    g_pending_rmm_build_vram[0] = 0;

    /* AR8 before AR6: overcommit off and no physical free would be AR6. */
    g_managed_overcommit = 0;
    g_free = 0;
    a = line_for(999000);
    CHECK(admit(&a, NULL) == CUVS_ADMISSION_AR8);
    /* AR6 itself is unchanged for a shape that fits the pool. */
    g_pool_used = 0;
    g_free = 2 * GiB;
    a = line_for(499500);
    CHECK(admit(&a, NULL) == CUVS_ADMISSION_AR6);
    g_managed_overcommit = 1;
    g_free = 80 * GiB;

    /* AR8 before make_room: with scope lru and the logical budget short, a
     * fitting build evicts the resident index (positive control) ... */
    g_reclaim.scope.kind = CUVS_SCOPE_LRU;
    g_indexes = &resident; g_n_indexes = 1;
    a = line_for(499500);
    g_max_vram_per_gpu[0] = a.needed + GiB / 2;
    CHECK(admit(&a, NULL) == CUVS_ADMISSION_NONE);
    CHECK(g_cache_evictions[0] == 1 && g_n_indexes == 0);
    /* ... while the refused Q6 build evicts nothing. Restore the entry the
     * eviction above cleared, and check the budget is short again, so the
     * no-eviction check is not vacuous. */
    resident = (IndexEntry){.valid = 1, .is_flat = 1, .gpu_device_id = 0,
                            .db_oid = 1, .index_oid = 9, .main_bf_vram_bytes = GiB};
    g_indexes = &resident; g_n_indexes = 1;
    CHECK(total_vram_used(0) == GiB);
    memset(g_cache_evictions, 0, sizeof(g_cache_evictions));
    g_pool_used = Q6_POOL_USED;
    a = line_for(999000);
    g_max_vram_per_gpu[0] = a.needed + GiB / 2;
    CHECK(total_vram_used(0) + a.needed > g_max_vram_per_gpu[0]);   /* make_room would run */
    CHECK(admit(&a, NULL) == CUVS_ADMISSION_AR8);
    CHECK(g_cache_evictions[0] == 0 && g_n_indexes == 1);
    CHECK(g_pending_rmm_build_vram[0] == 0);
    g_indexes = NULL; g_n_indexes = 0;
    pthread_mutex_unlock(&g_index_mutex);
}

static void
capture_admit(AdmitLine *a, char *buf, size_t cap)
{
    char path[] = "/tmp/ar8-admit-XXXXXX";
    int fd = mkstemp(path), saved = dup(2);
    size_t n;
    FILE *f;
    assert(fd >= 0 && saved >= 0);
    fflush(stderr);
    dup2(fd, 2);
    log_admit(a, CUVS_ADMISSION_AR8);
    fflush(stderr);
    dup2(saved, 2);
    close(fd);
    close(saved);
    f = fopen(path, "r");
    assert(f);
    n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    fclose(f);
    unlink(path);
}

static void
test_admit_line(void)
{
    char buf[8192];
    AdmitLine a;
    reset();
    g_pool_used = Q6_POOL_USED;
    pthread_mutex_lock(&g_index_mutex);
    g_pending_rmm_build_vram[0] = 1000;
    g_lw_pending[0] = 2000;
    a = line_for(999000);
    CHECK(admit(&a, NULL) == CUVS_ADMISSION_AR8);
    capture_admit(&a, buf, sizeof(buf));
    g_pending_rmm_build_vram[0] = 0;
    g_lw_pending[0] = 0;
    pthread_mutex_unlock(&g_index_mutex);
    CHECK(strstr(buf, " verdict=reject reason=AR8 ") != NULL);
    CHECK(strstr(buf, " evicted=0 ") != NULL);
    CHECK(strstr(buf, " pool_used=3196800000 ") != NULL);   /* existing field */
    /* Appended after mono_us, in this order; pending = N + lw reservations. */
    CHECK(strstr(buf, " pool_need=3196800000 headroom=479520000 pool_quota=6807502080 "
                      "pending=3000\n") != NULL);
    {
        const char *m = strstr(buf, " mono_us=");
        CHECK(m && strstr(m, " pool_need=") != NULL);
    }
}

static int
run_main(const char *flag, const char *value)
{
    char *argv[3] = {(char *)"pg_cuvs_server", (char *)flag, (char *)value};
    pid_t pid = fork();
    if (pid == 0)
    {
        alarm(5);
        _exit(pg_cuvs_server_program_main(value ? 3 : 2, argv) == 1 ? 1 : 0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void
test_knob(void)
{
    static const char *const bad[] = {"100001", "-1", "", "150x", " 150", "+150"};
    uint32_t v = 7;
    CHECK(CUVS_RMM_BUILD_HEADROOM_PERMILLE_DEFAULT == 150u);
    CHECK(cuvs_lw_parse_build_headroom("0", &v) == 0 && v == 0);
    CHECK(cuvs_lw_parse_build_headroom("100000", &v) == 0 && v == 100000);
    CHECK(cuvs_lw_parse_build_headroom("150", &v) == 0 && v == 150);
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        v = 7;
        CHECK(cuvs_lw_parse_build_headroom(bad[i], &v) == -1 && v == 7);
        CHECK(run_main("--rmm-build-headroom-permille", bad[i]) == 1);
    }
    CHECK(cuvs_lw_parse_build_headroom(NULL, &v) == -1);
    CHECK(run_main("--rmm-build-headroom-permille", NULL) == 1);
}

int
main(void)
{
    test_knob();
    test_q6();
    test_terms_boundary();
    test_concurrent();
    test_order();
    test_admit_line();
    if (failures)
    {
        fprintf(stderr, "test_ar8_daemon: %d failure(s)\n", failures);
        return 1;
    }
    puts("PASS ar8 daemon: Q6 (pool_used A_res) AR8 at 150, admitted at 100 and 0, "
         "each of pool_used/build pending/lw_pending at the == / -1 boundary, two "
         "concurrent builds, AR1 then AR8 then AR6, no eviction or reservation on AR8, "
         "err_out n q p d o, [admit] fields, strict knob parsing");
    return 0;
}
