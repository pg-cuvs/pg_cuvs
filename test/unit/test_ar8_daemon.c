/* ADR-089 AR8 through the actual daemon source (Linux, CPU shim): ensure_vram
 * refuses a CAGRA build whose pending + N_m + lw_ub + headroom exceeds the
 * pool quota, before AR6/AR7 and before make_room (nothing evicted, nothing
 * reserved), fills the AR8 err_out subset, appends the four [admit] fields,
 * and parses --rmm-build-headroom-permille strictly. Only physical free
 * bytes and the host guard are fixtures. Shapes are the Q6 condition. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
size_t fixture_free(int device_id);
#define cuvs_vram_free_bytes_on fixture_free
#define cuvs_vram_physical_free_bytes_on fixture_free
#define main pg_cuvs_server_program_main
#include "../../src/pg_cuvs_server.c"
#undef main
#undef cuvs_vram_physical_free_bytes_on
#undef cuvs_vram_free_bytes_on
#include <assert.h>
#include <sys/wait.h>

static size_t g_free;
size_t fixture_free(int device_id) { (void)device_id; return g_free; }
static uint64_t fixture_guard(void) { return UINT64_C(1) << 40; }

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define GiB ((size_t)1024 * 1024 * 1024)
static const size_t Q6_QUOTA = (size_t)6807502080ull;
static const size_t Q6_N_M = (size_t)3196800000ull;           /* rho 0 */
static const uint64_t Q6_POOL_NEED = UINT64_C(7135257600);    /* margin 1100 */
static const uint64_t Q6_HEADROOM = UINT64_C(1070288640);     /* permille 150 */

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
test_q6_refused(void)
{
    AdmitLine a;
    CuvsAdmissionFields rej;
    reset();
    pthread_mutex_lock(&g_index_mutex);
    a = line_for(999000);
    CHECK(a.needed == Q6_N_M);
    CHECK(admit(&a, &rej) == CUVS_ADMISSION_AR8);
    CHECK(a.pool_valid && a.pool_need == Q6_POOL_NEED && a.pool_headroom == Q6_HEADROOM);
    CHECK(rej.reason == CUVS_ADMISSION_AR8);
    CHECK(rej.present == (CUVS_ADM_F_N | CUVS_ADM_F_Q | CUVS_ADM_F_P | CUVS_ADM_F_D));
    CHECK(rej.n == 6804 && rej.q == 6492 && rej.p == 0 && rej.d == 1020);
    /* No reservation, no lw_pending, no eviction. */
    CHECK(g_pending_rmm_build_vram[0] == 0 && g_pending_build_vram[0] == 0);
    CHECK(g_lw_pending[0] == 0 && g_cache_evictions[0] == 0);

    /* Permille 0: the capacity rule alone still refuses the Q6 shape. */
    g_rmm_build_headroom_permille = 0;
    a = line_for(999000);
    CHECK(admit(&a, &rej) == CUVS_ADMISSION_AR8);
    CHECK(a.pool_headroom == 0 && rej.d == 0);

    /* A fitting shape (half the rows) is admitted at 150. */
    g_rmm_build_headroom_permille = 150;
    a = line_for(499500);
    CHECK(admit(&a, NULL) == CUVS_ADMISSION_NONE);

    /* Pending (3 GiB, below the AR1 limit) pushes it into AR8. */
    g_pending_rmm_build_vram[0] = 3 * GiB;
    a = line_for(499500);
    CHECK(admit(&a, &rej) == CUVS_ADMISSION_AR8);
    CHECK(a.pool_pending == 3 * GiB && rej.p == 3072);
    g_pending_rmm_build_vram[0] = 0;

    /* Quota 0 (pool disabled): no AR8. */
    g_rmm_pool_limits[0] = 0;
    a = line_for(999000);
    CHECK(admit(&a, NULL) == CUVS_ADMISSION_NONE);

    /* Boundary: quota == pending + pool_need + headroom admits, one less refuses. */
    g_rmm_pool_limits[0] = (size_t)(Q6_POOL_NEED + Q6_HEADROOM);
    a = line_for(999000);
    CHECK(admit(&a, NULL) == CUVS_ADMISSION_NONE);
    g_rmm_pool_limits[0] = (size_t)(Q6_POOL_NEED + Q6_HEADROOM - 1);
    a = line_for(999000);
    CHECK(admit(&a, NULL) == CUVS_ADMISSION_AR8);

    /* Without an AdmitLine (shard and manual callers) AR8 is never evaluated. */
    g_rmm_pool_limits[0] = Q6_QUOTA;
    CHECK(ensure_vram(Q6_N_M, 0, CUVS_RQ_BUILD_CAGRA, NULL, NULL) == 0);
    CHECK(ensure_vram(Q6_N_M, 0, CUVS_RQ_BUILD_SHARD, NULL, NULL) == 0);
    pthread_mutex_unlock(&g_index_mutex);
}

static void
test_order(void)
{
    AdmitLine a;
    IndexEntry resident = {.valid = 1, .is_flat = 1, .gpu_device_id = 0,
                           .db_oid = 1, .index_oid = 9, .main_bf_vram_bytes = GiB};
    reset();
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
    /* ... while the refused Q6 build evicts nothing. */
    resident.valid = 1;
    g_indexes = &resident; g_n_indexes = 1;
    memset(g_cache_evictions, 0, sizeof(g_cache_evictions));
    a = line_for(999000);
    g_max_vram_per_gpu[0] = a.needed + GiB / 2;
    CHECK(admit(&a, NULL) == CUVS_ADMISSION_AR8);
    CHECK(g_cache_evictions[0] == 0 && g_n_indexes == 1);
    CHECK(g_pending_rmm_build_vram[0] == 0);
    g_indexes = NULL; g_n_indexes = 0;
    pthread_mutex_unlock(&g_index_mutex);
}

static void
test_admit_line(void)
{
    char path[] = "/tmp/ar8-admit-XXXXXX", buf[8192];
    int fd = mkstemp(path), saved = dup(2);
    AdmitLine a;
    size_t n;
    FILE *f;
    assert(fd >= 0 && saved >= 0);
    reset();
    pthread_mutex_lock(&g_index_mutex);
    a = line_for(999000);
    CHECK(admit(&a, NULL) == CUVS_ADMISSION_AR8);
    fflush(stderr);
    dup2(fd, 2);
    log_admit(&a, CUVS_ADMISSION_AR8);
    fflush(stderr);
    dup2(saved, 2);
    pthread_mutex_unlock(&g_index_mutex);
    f = fopen(path, "r");
    assert(f);
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    unlink(path);
    CHECK(strstr(buf, " verdict=reject reason=AR8 ") != NULL);
    CHECK(strstr(buf, " evicted=0 ") != NULL);
    /* Appended after mono_us, in this order. */
    CHECK(strstr(buf, " pool_need=7135257600 headroom=1070288640 pool_quota=6807502080 "
                      "pending=0\n") != NULL);
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
    test_q6_refused();
    test_order();
    test_admit_line();
    if (failures)
    {
        fprintf(stderr, "test_ar8_daemon: %d failure(s)\n", failures);
        return 1;
    }
    puts("PASS ar8 daemon: Q6 refused at 150 and 0, fitting/pending/quota 0/boundary, "
         "AR1 then AR8 then AR6, no eviction or reservation on AR8, err_out n q p d, "
         "[admit] appended fields, strict knob parsing");
    return 0;
}
