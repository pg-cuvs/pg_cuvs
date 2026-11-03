/* I4 / ADR-088 through the actual daemon source (Linux, CPU shim): KB-4/5/6/24
 * startup refusal (CK-9), oom_score_adj never-lower and failure-tolerant
 * startup (CK-9 r16/r18), lw_pending accounting under two concurrent managed
 * admissions (CK-3b), per-caller policy arguments (CK-28: handle_build and
 * handle_build_multi pass the active policy, build_sharded never does), spill
 * counters, completion NOTICE and pool-status fields (C8).
 * Only physical free and host_guard are fixtures; the registry, mutex, build
 * handlers and the CPU shim are production code. Run as a non-root user so the
 * permission-denied branch is real. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
static uint64_t g_fixture_phys_free;
static uint64_t g_fixture_host_guard;
size_t fixture_phys_free(int device_id);
#define cuvs_vram_physical_free_bytes_on fixture_phys_free
#define main pg_cuvs_server_program_main
#include "../../src/pg_cuvs_server.c"
#undef main
#undef cuvs_vram_physical_free_bytes_on
#include <assert.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>

size_t fixture_phys_free(int device_id) { (void)device_id; return (size_t)g_fixture_phys_free; }
/* Pinned guard for the CK-3b races; test_host_guard_production() restores
 * the production reader and checks it without any fixture. */
static uint64_t fixture_host_guard(void) { return g_fixture_host_guard; }

void     cuvs_shim_lw_reset(void);
uint32_t cuvs_shim_lw_calls(uint32_t policy);
uint32_t cuvs_shim_lw_last_policy(void);
void     cuvs_shim_lw_set_peak(uint64_t peak, uint64_t free_at_peak);

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static char tmpdir[256];

static void
reset_knobs(void)
{
    g_cagra_build_overflow = CUVS_CAGRA_BUILD_OVERFLOW_DEFAULT;
    g_managed_overcommit = CUVS_MANAGED_OVERCOMMIT_DEFAULT;
    g_managed_lw_margin_permille = CUVS_MANAGED_LW_MARGIN_PERMILLE_DEFAULT;
    g_oom_score_adj = CUVS_OOM_SCORE_ADJ_DEFAULT;
    memset(g_lw_pending, 0, sizeof(g_lw_pending));
    memset(g_managed_spill_builds, 0, sizeof(g_managed_spill_builds));
    memset(g_managed_spill_bytes_max, 0, sizeof(g_managed_spill_bytes_max));
}

static int
run_main(int n, const char *a0, const char *a1, const char *a2)
{
    char *argv[4] = {(char *)"pg_cuvs_server", (char *)a0, (char *)a1, (char *)a2};
    pid_t pid = fork();
    if (pid == 0)
    {
        alarm(5);   /* an accepted bad value proceeds to serve: count as failure */
        _exit(pg_cuvs_server_program_main(n + 1, argv) == 1 ? 1 : 0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void
test_flags(void)
{
    /* Out-of-range, empty or missing values refuse startup (rc 1) before
     * any socket, GPU or registry work. Each runs in a child. */
    struct { const char *f, *v; } bad[] = {
        {"--cagra-build-overflow", "Managed"}, {"--cagra-build-overflow", ""},
        {"--managed-overcommit", "1"}, {"--managed-overcommit", "yes"},
        {"--managed-lw-margin-permille", "999"}, {"--managed-lw-margin-permille", "100001"},
        {"--managed-lw-margin-permille", "-1250"}, {"--managed-lw-margin-permille", "1250x"},
        {"--oom-score-adj", "-1"}, {"--oom-score-adj", "1001"}, {"--oom-score-adj", ""},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(run_main(2, bad[i].f, bad[i].v, NULL) == 1);
    CHECK(run_main(1, "--oom-score-adj", NULL, NULL) == 1);          /* missing value */
    CHECK(run_main(1, "--managed-lw-margin-permille", NULL, NULL) == 1);

    /* In-range values reach the globals through the same parser main uses. */
    {
        char *argv[] = {(char *)"x", (char *)"--cagra-build-overflow", (char *)"error",
                        (char *)"--managed-overcommit", (char *)"off",
                        (char *)"--managed-lw-margin-permille", (char *)"1000",
                        (char *)"--oom-score-adj", (char *)"0", (char *)"--socket"};
        int argc = 10, i = 1, seen = 0;
        reset_knobs();
        while (i < argc)
        {
            int r = parse_lw_flag(argc, argv, &i);
            if (r == 1) seen++;
            CHECK(r >= 0);
            i++;
        }
        CHECK(seen == 4);
        CHECK(g_cagra_build_overflow == CUVS_LW_OVERFLOW_ERROR && g_managed_overcommit == 0);
        CHECK(g_managed_lw_margin_permille == 1000 && g_oom_score_adj == 0);
        i = 1;
        CHECK(parse_lw_flag(3, (char *[]){(char *)"x", (char *)"--oom-score-adj", (char *)"1000"}, &i) == 1);
        CHECK(g_oom_score_adj == 1000 && i == 2);
        reset_knobs();
    }
}

static void
write_file(const char *path, const char *text, mode_t mode)
{
    FILE *f;
    chmod(path, 0600);
    f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
    chmod(path, mode);
}

static void
read_file(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    size_t n;
    assert(f);
    n = fread(buf, 1, len - 1, f);
    buf[n] = '\0';
    fclose(f);
}

static void
test_oom_score_adj(void)
{
    char path[320], buf[64];
    snprintf(path, sizeof(path), "%s/oom_score_adj", tmpdir);

    g_oom_score_adj = 0;                               /* 0: never write */
    write_file(path, "123", 0600);
    apply_oom_score_adj(path);
    read_file(path, buf, sizeof(buf));
    CHECK(strcmp(buf, "123") == 0);
    CHECK(strcmp(g_oom_score_adj_result, "oom_score_adj not applied: value 0") == 0);

    g_oom_score_adj = 500;                             /* higher current: keep */
    write_file(path, "800\n", 0600);
    apply_oom_score_adj(path);
    read_file(path, buf, sizeof(buf));
    CHECK(strcmp(buf, "800\n") == 0);
    CHECK(strcmp(g_oom_score_adj_result, "oom_score_adj kept current 800") == 0);
    CHECK(g_oom_score_adj_effective == 800);

    write_file(path, "0\n", 0600);                     /* lower current: raise */
    apply_oom_score_adj(path);
    read_file(path, buf, sizeof(buf));
    CHECK(strcmp(buf, "500") == 0);
    CHECK(strcmp(g_oom_score_adj_result, "oom_score_adj=500 applied") == 0);
    CHECK(g_oom_score_adj_effective == 500);

    if (geteuid() != 0)                                /* EACCES on write */
    {
        write_file(path, "0\n", 0444);
        apply_oom_score_adj(path);
        CHECK(strcmp(g_oom_score_adj_result, "oom_score_adj not applied: permission denied") == 0);
        read_file(path, buf, sizeof(buf));
        CHECK(strcmp(buf, "0\n") == 0);
        chmod(path, 0600);
    }
    else
        fprintf(stderr, "NOTE running as root: permission-denied branch not exercised\n");

    snprintf(path, sizeof(path), "%s/missing/oom_score_adj", tmpdir);
    apply_oom_score_adj(path);                         /* returns; daemon keeps going */
    CHECK(strncmp(g_oom_score_adj_result, "oom_score_adj not applied: ", 27) == 0);
    g_oom_score_adj = CUVS_OOM_SCORE_ADJ_DEFAULT;
}

/* ---- CK-3b: two concurrent managed admissions on one GPU ---- */
static atomic_int admitted, refused, arrived, finish;
static atomic_int last_reason;
static const int64_t SHAPE_N = 250000;
static const uint32_t SHAPE_DIM = 128;
static const int SHAPE_IGD = 128;

static void *
managed_build(void *unused)
{
    CuvsAdmissionWorkspaceInput ws;
    CuvsAdmissionReason r;
    (void)unused;
    pthread_mutex_lock(&g_index_mutex);
    r = lw_admission_check(0, 0, SHAPE_N, SHAPE_DIM, SHAPE_IGD, &ws);
    if (r == CUVS_ADMISSION_NONE) { lw_pending_reserve(ws.lw_ub, 0); atomic_fetch_add(&admitted, 1); }
    else { atomic_store(&last_reason, (int)r); atomic_fetch_add(&refused, 1); }
    pthread_mutex_unlock(&g_index_mutex);
    atomic_fetch_add(&arrived, 1);
    while (!atomic_load(&finish)) usleep(1000);
    if (r == CUVS_ADMISSION_NONE)
    {
        pthread_mutex_lock(&g_index_mutex);
        lw_pending_release(ws.lw_ub, 0);
        pthread_mutex_unlock(&g_index_mutex);
    }
    return NULL;
}

static void
race(int overcommit, uint64_t phys_free, uint64_t host_guard, int want_admitted, int want_reason)
{
    pthread_t a, b;
    reset_knobs();
    g_managed_overcommit = overcommit;
    g_fixture_phys_free = phys_free;
    g_fixture_host_guard = host_guard;
    g_lw_host_guard_fn = fixture_host_guard;
    atomic_store(&admitted, 0); atomic_store(&refused, 0);
    atomic_store(&arrived, 0); atomic_store(&finish, 0); atomic_store(&last_reason, -1);
    pthread_create(&a, NULL, managed_build, NULL);
    pthread_create(&b, NULL, managed_build, NULL);
    while (atomic_load(&arrived) < 2) usleep(1000);
    CHECK(atomic_load(&admitted) == want_admitted);
    CHECK(atomic_load(&refused) == 2 - want_admitted);
    if (want_admitted < 2) CHECK(atomic_load(&last_reason) == want_reason);
    atomic_store(&finish, 1);
    pthread_join(a, NULL); pthread_join(b, NULL);
    CHECK(g_lw_pending[0] == 0);   /* every admitted build returned its lw_ub */
    g_lw_host_guard_fn = lw_host_guard_bytes;
}

static void
test_lw_pending(void)
{
    uint64_t ub;
    CuvsAdmissionWorkspaceInput ws;
    reset_knobs();
    ub = lw_ub_for_build(SHAPE_N, SHAPE_DIM, SHAPE_IGD);
    CHECK(ub == cuvs_lw_ub_bytes(cuvs_lw_shape_bytes(SHAPE_N, SHAPE_DIM, SHAPE_IGD),
                                 CUVS_MANAGED_LW_MARGIN_PERMILLE_DEFAULT));
    CHECK(ub > 0);
    /* Off: phys_free in {2 ub - 1, 2 ub, 2 ub + 1}. */
    race(0, 2 * ub - 1, 0, 1, CUVS_ADMISSION_AR6);
    race(0, 2 * ub, 0, 2, -1);
    race(0, 2 * ub + 1, 0, 2, -1);
    /* On: the second spills ub beyond phys_free = ub; host_guard in {ub-1, ub, ub+1}. */
    race(1, ub, ub - 1, 1, CUVS_ADMISSION_AR7);
    race(1, ub, ub, 2, -1);
    race(1, ub, ub + 1, 2, -1);

    /* After release the refused shape is admitted at the single-build boundary. */
    reset_knobs();
    g_managed_overcommit = 0;
    g_fixture_phys_free = ub;
    g_lw_host_guard_fn = fixture_host_guard;
    pthread_mutex_lock(&g_index_mutex);
    CHECK(lw_admission_check(0, 0, SHAPE_N, SHAPE_DIM, SHAPE_IGD, &ws) == CUVS_ADMISSION_NONE);
    lw_pending_reserve(ws.lw_ub, 0);
    CHECK(lw_admission_check(0, 0, SHAPE_N, SHAPE_DIM, SHAPE_IGD, &ws) == CUVS_ADMISSION_AR6);
    CHECK(ws.lw_pending == ub && ws.lw_ub == ub && ws.phys_free == ub);
    lw_pending_release(ub, 0);
    CHECK(lw_admission_check(0, 0, SHAPE_N, SHAPE_DIM, SHAPE_IGD, &ws) == CUVS_ADMISSION_NONE);
    /* GPU isolation: another device's pending does not count. */
    lw_pending_reserve(ub, 1);
    CHECK(lw_admission_check(0, 0, SHAPE_N, SHAPE_DIM, SHAPE_IGD, &ws) == CUVS_ADMISSION_NONE);
    lw_pending_release(ub, 1);
    /* Policy error: no lw_ub, no pending, never AR6/AR7. */
    g_cagra_build_overflow = CUVS_LW_OVERFLOW_ERROR;
    g_fixture_phys_free = 0;
    CHECK(lw_ub_for_build(SHAPE_N, SHAPE_DIM, SHAPE_IGD) == 0);
    CHECK(lw_admission_check(0, 0, SHAPE_N, SHAPE_DIM, SHAPE_IGD, &ws) == CUVS_ADMISSION_NONE);
    CHECK(ws.managed == 0 && ws.lw_ub == 0);
    /* Release never underflows. */
    lw_pending_release(5, 0);
    CHECK(g_lw_pending[0] == 0);
    /* Pool status reports the live per-GPU sum (C8). */
    {
        CuvsRmmPoolStatus st;
        memset(&st, 0, sizeof(st));
        lw_pending_reserve(ub, 0);
        pthread_mutex_unlock(&g_index_mutex);
        g_n_gpus = 1;
        lw_fill_pool_status(&st);
        CHECK(st.lw_n_devices == 1 && st.lw_devices[0].lw_pending_bytes == ub);
        pthread_mutex_lock(&g_index_mutex);
        lw_pending_release(ub, 0);
        g_n_gpus = 0;
    }
    pthread_mutex_unlock(&g_index_mutex);
    g_lw_host_guard_fn = lw_host_guard_bytes;
    reset_knobs();
}

/* ---- review B-2 (a): the production host guard, no fixture ---- */
static uint64_t
mem_available_half(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    unsigned long long kb = 0;
    if (!f) return 0;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) break;
    fclose(f);
    return (uint64_t)kb * 1024 / 2;
}

static int
cgroup_is_v2_only(void)
{
    FILE *f = fopen("/proc/self/cgroup", "r");
    char line[512];
    int v2 = 0, other = 0;
    if (!f) return 0;
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, "0::", 3) == 0) v2 = 1; else other = 1;
    fclose(f);
    return v2 && !other;
}

static void
test_host_guard_production(void)
{
    CuvsAdmissionWorkspaceInput ws;
    uint64_t before = mem_available_half(), direct, after;
    reset_knobs();
    CHECK(g_lw_host_guard_fn == lw_host_guard_bytes);
    direct = lw_host_guard_bytes();
    pthread_mutex_lock(&g_index_mutex);
    (void)lw_admission_check(0, 0, 1000, 128, 128, &ws);   /* AR7 input path */
    pthread_mutex_unlock(&g_index_mutex);
    after = mem_available_half();
    if (cgroup_is_v2_only())
    {
        uint64_t cap = before > after ? before : after;
        CHECK(direct > 0 && direct <= cap + cap / 20);   /* MemAvailable drifts */
        CHECK(ws.host_guard > 0 && ws.host_guard <= cap + cap / 20);
    }
    else
    {
        fprintf(stderr, "NOTE /proc/self/cgroup is not v2-only: production guard must be 0\n");
        CHECK(direct == 0 && ws.host_guard == 0);
    }
}

/* ---- review C-6: the shared reader on a fake tree (CK-28 v1/failure -> 0) ---- */
static void
put(const char *root, const char *rel, const char *text)
{
    char path[512], dir[512];
    snprintf(path, sizeof(path), "%s%s", root, rel);
    snprintf(dir, sizeof(dir), "%s", path);
    for (char *p = dir + strlen(root) + 1; *p; p++)
        if (*p == '/') { *p = '\0'; mkdir(dir, 0700); *p = '/'; }
    write_file(path, text, 0600);
}

static uint64_t
headroom_tree(const char *root, uint64_t requested, char *err, size_t len)
{
    char meminfo[512], cg[512], cgroot[512];
    HostHeadroomPaths p = {meminfo, cg, cgroot};
    snprintf(meminfo, sizeof(meminfo), "%s/proc/meminfo", root);
    snprintf(cg, sizeof(cg), "%s/proc/self/cgroup", root);
    snprintf(cgroot, sizeof(cgroot), "%s/sys/fs/cgroup", root);
    err[0] = '\0';
    return host_headroom_at(&p, requested, err, len);
}

static void
test_host_headroom_tree(void)
{
    char root[320], err[160];
    snprintf(root, sizeof(root), "%s/hr", tmpdir);
    mkdir(root, 0700);
    put(root, "/proc/meminfo", "MemTotal: 16000000 kB\nMemAvailable: 8000000 kB\n");
    put(root, "/proc/self/cgroup", "0::/a/b\n");
    put(root, "/sys/fs/cgroup/a/b/memory.max", "max\n");
    put(root, "/sys/fs/cgroup/a/b/memory.current", "100\n");
    put(root, "/sys/fs/cgroup/a/memory.max", "3000000000\n");
    put(root, "/sys/fs/cgroup/a/memory.current", "1000000000\n");
    /* MemAvailable/2 = 4,096,000,000; ancestor remaining/2 = 1,000,000,000 */
    CHECK(headroom_tree(root, UINT64_MAX, err, sizeof(err)) == 1000000000ull);
    CHECK(headroom_tree(root, 5, err, sizeof(err)) == 5);
    put(root, "/sys/fs/cgroup/a/b/memory.max", "500000000\n");   /* leaf tighter */
    put(root, "/sys/fs/cgroup/a/b/memory.current", "100000000\n");
    CHECK(headroom_tree(root, UINT64_MAX, err, sizeof(err)) == 200000000ull);
    put(root, "/sys/fs/cgroup/a/b/memory.current", "600000000\n");  /* over limit */
    CHECK(headroom_tree(root, UINT64_MAX, err, sizeof(err)) == 0);
    put(root, "/sys/fs/cgroup/a/b/memory.current", "100000000\n");
    /* cgroup v1 memory controller: refused with a reason. */
    put(root, "/proc/self/cgroup", "12:cpu,memory:/x\n0::/a/b\n");
    CHECK(headroom_tree(root, UINT64_MAX, err, sizeof(err)) == 0 && strstr(err, "cgroup v1") != NULL);
    put(root, "/proc/self/cgroup", "0::/a/../b\n");                  /* traversal */
    CHECK(headroom_tree(root, UINT64_MAX, err, sizeof(err)) == 0);
    put(root, "/proc/self/cgroup", "0::/a/b\n");
    {   /* numeric limit without a readable current: read failure */
        char p[512];
        snprintf(p, sizeof(p), "%s/sys/fs/cgroup/a/b/memory.current", root);
        unlink(p);
        CHECK(headroom_tree(root, UINT64_MAX, err, sizeof(err)) == 0);
        snprintf(p, sizeof(p), "%s/proc/self/cgroup", root);
        unlink(p);                                               /* unreadable cgroup */
        put(root, "/sys/fs/cgroup/a/b/memory.current", "100000000\n");
        CHECK(headroom_tree(root, UINT64_MAX, err, sizeof(err)) == 0);
        put(root, "/proc/self/cgroup", "0::/a/b\n");
        snprintf(p, sizeof(p), "%s/proc/meminfo", root);
        unlink(p);                                               /* unreadable meminfo */
        CHECK(headroom_tree(root, UINT64_MAX, err, sizeof(err)) == 0);
    }
}

/* ---- CK-28 per-caller policy, end to end through the build handlers ---- */
static const int64_t N = 64;
static const uint32_t DIM = 8;

static void
fill_corpus(char *mem)
{
    float *v = (float *)mem;
    uint64_t *t = (uint64_t *)(mem + (size_t)N * DIM * sizeof(float));
    for (int64_t i = 0; i < N * (int64_t)DIM; i++) v[i] = (float)((i * 2654435761u) % 997) / 997.0f;
    for (int64_t i = 0; i < N; i++) t[i] = (uint64_t)i + 1;
}

static CuvsCmdFrame
build_cmd(uint32_t index_oid, uint32_t shard_count)
{
    CuvsCmdFrame cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = CUVS_OP_BUILD;
    cmd.db_oid = 1;
    cmd.index_oid = index_oid;
    cmd.n_vecs = N;
    cmd.dim = DIM;
    cmd.metric = CUVS_METRIC_L2;
    cmd.graph_degree = 4;
    cmd.intermediate_graph_degree = 8;
    cmd.shard_count = shard_count;
    return cmd;
}

static int
reply_status(int fd)
{
    CuvsReplyHeader hdr;
    if (recv(fd, &hdr, sizeof(hdr), MSG_WAITALL) != (ssize_t)sizeof(hdr)) return -1;
    return (int)hdr.status;
}

static int
run_handle_build(uint32_t index_oid, uint32_t shard_count)
{
    int sv[2], st;
    size_t total = (size_t)N * DIM * sizeof(float) + (size_t)N * sizeof(uint64_t);
    char dir[256];
    CuvsCmdFrame cmd = build_cmd(index_oid, shard_count);
    int mfd = memfd_create("lw_test", MFD_CLOEXEC);
    assert(mfd >= 0 && ftruncate(mfd, (off_t)total) == 0);
    char *mem = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    assert(mem != MAP_FAILED);
    fill_corpus(mem);
    munmap(mem, total);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    memset(dir, 0, sizeof(dir));
    snprintf(dir, sizeof(dir), "%s/idx", tmpdir);
    assert(cuvs_fd_send(sv[0], mfd, dir, sizeof(dir)) == 0);
    close(mfd);
    handle_build(sv[1], &cmd);
    st = reply_status(sv[0]);
    close(sv[0]); close(sv[1]);
    return st;
}

static int
run_handle_build_multi(uint32_t index_oid)
{
    int sv[2], st;
    char dir[256];
    CuvsPartialDesc desc[2];
    CuvsCmdFrame cmd = build_cmd(index_oid, 1);
    const int64_t half = N / 2;
    size_t per_vec = DIM * sizeof(float);
    char *whole = malloc((size_t)N * (per_vec + sizeof(uint64_t)));
    fill_corpus(whole);
    cmd.n_partials = 2;
    for (int p = 0; p < 2; p++)
    {
        size_t len = (size_t)half * (per_vec + sizeof(uint64_t));
        snprintf(desc[p].shm_name, sizeof(desc[p].shm_name), "/pg_cuvs_bld_lwtest_%d_%d", (int)getpid(), p);
        desc[p].n_vecs = half;
        shm_unlink(desc[p].shm_name);
        int fd = shm_open(desc[p].shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
        assert(fd >= 0 && ftruncate(fd, (off_t)len) == 0);
        char *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        assert(m != MAP_FAILED);
        memcpy(m, whole + (size_t)p * half * per_vec, (size_t)half * per_vec);
        memcpy(m + (size_t)half * per_vec,
               whole + (size_t)N * per_vec + (size_t)p * half * sizeof(uint64_t),
               (size_t)half * sizeof(uint64_t));
        munmap(m, len);
        close(fd);
    }
    free(whole);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(send(sv[0], desc, sizeof(desc), 0) == (ssize_t)sizeof(desc));
    memset(dir, 0, sizeof(dir));
    snprintf(dir, sizeof(dir), "%s/idx", tmpdir);
    g_peer_uid = getuid();
    handle_build_multi(sv[1], &cmd, dir);
    st = reply_status(sv[0]);
    close(sv[0]); close(sv[1]);
    for (int p = 0; p < 2; p++) shm_unlink(desc[p].shm_name);
    return st;
}

static int
run_build_sharded(uint32_t index_oid)
{
    int sv[2], st;
    size_t total = (size_t)N * DIM * sizeof(float) + (size_t)N * sizeof(uint64_t);
    char dir[256];
    CuvsCmdFrame cmd = build_cmd(index_oid, 2);
    char *mem = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mem != MAP_FAILED);
    fill_corpus(mem);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    snprintf(dir, sizeof(dir), "%s/idx", tmpdir);
    build_sharded(sv[1], &cmd, dir, (const float *)mem,
                  (const uint64_t *)(mem + (size_t)N * DIM * sizeof(float)), total, mem, 2);
    st = reply_status(sv[0]);
    close(sv[0]); close(sv[1]);
    return st;
}

static void
test_callers(void)
{
    const uint64_t MiB = 1024u * 1024u;
    CuvsRmmPoolStatus status;
    char notice[CUVS_ADMISSION_ERR_OUT_BYTES];
    CuvsLwBuildStats st;

    g_n_gpus = 2;
    g_max_vram_per_gpu[0] = g_max_vram_per_gpu[1] = (size_t)64 << 30;
    g_indexes = calloc((size_t)g_max_indexes, sizeof(IndexEntry));
    g_cold_indexes = calloc((size_t)g_max_indexes, sizeof(ColdIndexEntry));
    assert(g_indexes && g_cold_indexes);

    /* handle_build: the active policy reaches the wrapper; lw_pending returns. */
    reset_knobs();
    cuvs_shim_lw_reset();
    cuvs_shim_lw_set_peak(300 * MiB, 200 * MiB);           /* spill */
    CHECK(run_handle_build(101, 1) == CUVS_STATUS_OK);
    CHECK(cuvs_shim_lw_calls(CUVS_LW_OVERFLOW_MANAGED) == 1);
    CHECK(cuvs_shim_lw_calls(CUVS_LW_OVERFLOW_ERROR) == 0);
    CHECK(g_lw_pending[0] == 0 && g_lw_pending[1] == 0);
    CHECK(g_managed_spill_builds[0] + g_managed_spill_builds[1] == 1);
    CHECK(g_managed_spill_bytes_max[0] + g_managed_spill_bytes_max[1] == 300 * MiB);

    /* No spill: counters unchanged. */
    cuvs_shim_lw_set_peak(300 * MiB, 400 * MiB);
    CHECK(run_handle_build(102, 1) == CUVS_STATUS_OK);
    CHECK(g_managed_spill_builds[0] + g_managed_spill_builds[1] == 1);

    /* Policy error: the wrapper receives error. */
    g_cagra_build_overflow = CUVS_LW_OVERFLOW_ERROR;
    cuvs_shim_lw_reset();
    CHECK(run_handle_build(103, 1) == CUVS_STATUS_OK);
    CHECK(cuvs_shim_lw_calls(CUVS_LW_OVERFLOW_MANAGED) == 0 &&
          cuvs_shim_lw_calls(CUVS_LW_OVERFLOW_ERROR) == 1);
    g_cagra_build_overflow = CUVS_LW_OVERFLOW_MANAGED;

    /* handle_build_multi: active policy too. */
    cuvs_shim_lw_reset();
    CHECK(run_handle_build_multi(104) == CUVS_STATUS_OK);
    CHECK(cuvs_shim_lw_calls(CUVS_LW_OVERFLOW_MANAGED) == 1 &&
          cuvs_shim_lw_last_policy() == CUVS_LW_OVERFLOW_MANAGED);
    CHECK(g_lw_pending[0] == 0 && g_lw_pending[1] == 0);

    /* build_sharded: never managed, whatever the active policy. */
    cuvs_shim_lw_reset();
    CHECK(run_build_sharded(105) == CUVS_STATUS_OK);
    CHECK(cuvs_shim_lw_calls(CUVS_LW_OVERFLOW_MANAGED) == 0);
    CHECK(cuvs_shim_lw_calls(CUVS_LW_OVERFLOW_ERROR) == 2);
    /* handle_build's auto/forced shard dispatch goes through build_sharded too. */
    cuvs_shim_lw_reset();
    CHECK(run_handle_build(106, 2) == CUVS_STATUS_OK);
    CHECK(cuvs_shim_lw_calls(CUVS_LW_OVERFLOW_MANAGED) == 0);

    /* Completion NOTICE text is produced exactly when the build spilled. */
    memset(&st, 0, sizeof(st));
    st.applied = st.restored = 1; st.lw_peak = 300 * MiB; st.free_at_peak = 200 * MiB;
    pthread_mutex_lock(&g_index_mutex);
    CHECK(lw_build_finish("build", 0, 1, 107, CUVS_LW_OVERFLOW_MANAGED, 1, &st,
                          notice, sizeof(notice)) == 1);
    CHECK(strcmp(notice, "pg_cuvs: build used managed memory spill (large_workspace peak 300 MiB, "
                         "physical free 200 MiB); the build may be slower than usual") == 0);
    st.free_at_peak = 300 * MiB;
    CHECK(lw_build_finish("build", 0, 1, 108, CUVS_LW_OVERFLOW_MANAGED, 1, &st,
                          notice, sizeof(notice)) == 0);
    CHECK(notice[0] == '\0');
    pthread_mutex_unlock(&g_index_mutex);

    /* Pool status carries the I4 keys (second channel for oom_score_adj). */
    memset(&status, 0, sizeof(status));
    snprintf(g_oom_score_adj_result, sizeof(g_oom_score_adj_result), "oom_score_adj=500 applied");
    g_oom_score_adj_effective = 500;
    lw_fill_pool_status(&status);
    CHECK(status.cagra_build_overflow == CUVS_LW_OVERFLOW_MANAGED);
    CHECK(status.managed_overcommit == 1);
    CHECK(status.lw_margin_permille == CUVS_MANAGED_LW_MARGIN_PERMILLE_DEFAULT);
    CHECK(status.oom_score_adj == 500 && status.oom_score_adj_effective == 500);
    CHECK(strcmp(status.oom_score_adj_result, "oom_score_adj=500 applied") == 0);
    CHECK(status.lw_n_devices == 2 && status.lw_devices[1].device_id == 1);
    CHECK(status.lw_devices[0].managed_spill_builds + status.lw_devices[1].managed_spill_builds == 2);
    CHECK(status.lw_devices[0].managed_spill_bytes_max + status.lw_devices[1].managed_spill_bytes_max
          >= 300 * MiB);
    CHECK(status.lw_devices[0].lw_pending_bytes == 0 && status.lw_devices[1].lw_pending_bytes == 0);
}

int
main(void)
{
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/lw_daemon_%d", (int)getpid());
    mkdir(tmpdir, 0700);
    test_flags();
    test_oom_score_adj();
    test_lw_pending();
    test_host_guard_production();
    test_host_headroom_tree();
    test_callers();
    if (failures)
    {
        fprintf(stderr, "test_lw_daemon: %d failure(s)\n", failures);
        return 1;
    }
    printf("PASS lw daemon: KB-4/5/6/24 startup refusal, oom_score_adj 0/keep/raise/EACCES/missing, "
           "CK-3b concurrent managed admissions (off/on boundaries, release, GPU isolation, policy error), "
           "production host guard, cgroup v2 min/v1/read-failure tree, "
           "per-caller policy (build, build_multi active; sharded error), spill counters, NOTICE, pool status\n");
    return 0;
}
