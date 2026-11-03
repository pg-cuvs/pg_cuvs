/* I3-E / C8 observation through the actual daemon source (Linux, CPU shim):
 * rejection counters (one count per emitted [admit] rejection, AR0..AR7 only,
 * gpu -1 kept apart), the appended memory_status columns (values equal the
 * daemon variables, pool NULL when unreadable, a read never resets), the
 * index_status dataset_attached column (NULL until a wrapper API exists), and
 * pg_cuvs_build_status's rows: registered while an unlocked CAGRA build runs
 * (handle_build, handle_build_multi), removed on success and failure, never
 * for build_sharded, and table-full builds counted as unlisted. The client's
 * bound on n_builds is checked against a fake daemon.
 * Fixtures: the CAGRA build entry points (to hold a build inside the
 * wrapper) and the RMM pool reader (the shim has none). Everything else is
 * production code. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#define cuvs_cagra_build        fixture_cagra_build
#define cuvs_cagra_build_multi  fixture_cagra_build_multi
#define cuvs_rmm_pool_stats     fixture_rmm_pool_stats
#define main pg_cuvs_server_program_main
#include "../../src/pg_cuvs_server.c"
#undef main
#undef cuvs_cagra_build
#undef cuvs_cagra_build_multi
#undef cuvs_rmm_pool_stats
#include <assert.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "cuvs_status_text.h"

CuvsCagraIndex cuvs_cagra_build(const float *, int64_t, int, uint32_t, int, int, uint32_t,
                                int, uint32_t, CuvsLwBuildStats *);
CuvsCagraIndex cuvs_cagra_build_multi(const float **, const int64_t *, int, int64_t, int,
                                      uint32_t, int, int, uint32_t, int, uint32_t,
                                      CuvsLwBuildStats *);
int cuvs_rmm_pool_stats(int device_id, CuvsRmmPoolStats *out);

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* ---- fixtures ---- */
static int   fx_hold;             /* 1: block inside the build until released */
static int   fx_fail;             /* 1: the build returns NULL */
static int   fx_used_at_entry;    /* registered rows seen when the wrapper ran */
static sem_t fx_entered, fx_release;
static int   fx_pool_ok;
static CuvsRmmPoolStats fx_pool;

static int
count_used(void)
{
    int n = 0;
    for (int s = 0; s < CUVS_MAX_ACTIVE_BUILDS; s++)
        n += g_builds_used[s] != 0;
    return n;
}

static void
fx_enter(void)
{
    fx_used_at_entry = count_used();
    if (fx_hold)
    {
        sem_post(&fx_entered);
        sem_wait(&fx_release);
    }
}

CuvsCagraIndex
fixture_cagra_build(const float *v, int64_t n, int dim, uint32_t metric, int gd, int igd,
                    uint32_t algo, int dev, uint32_t lw, CuvsLwBuildStats *out)
{
    fx_enter();
    if (fx_fail)
        return NULL;
    return cuvs_cagra_build(v, n, dim, metric, gd, igd, algo, dev, lw, out);
}

CuvsCagraIndex
fixture_cagra_build_multi(const float **v, const int64_t *each, int parts, int64_t total,
                          int dim, uint32_t metric, int gd, int igd, uint32_t algo, int dev,
                          uint32_t lw, CuvsLwBuildStats *out)
{
    fx_enter();
    if (fx_fail)
        return NULL;
    return cuvs_cagra_build_multi(v, each, parts, total, dim, metric, gd, igd, algo, dev, lw,
                                  out);
}

int
fixture_rmm_pool_stats(int device_id, CuvsRmmPoolStats *out)
{
    if (!fx_pool_ok)
        return cuvs_rmm_pool_stats(device_id, out);
    *out = fx_pool;
    return 0;
}

static uint64_t fixture_host_guard(void) { return (uint64_t)64 << 30; }

/* ---- status text helpers ---- */
static char *
read_text(void (*handler)(int))
{
    int sv[2];
    CuvsReplyHeader hdr;
    char *t;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    handler(sv[1]);
    assert(recv(sv[0], &hdr, sizeof(hdr), MSG_WAITALL) == (ssize_t)sizeof(hdr));
    assert(hdr.status == CUVS_STATUS_OK);
    t = calloc(1, (size_t)hdr.n_results + 1);
    if (hdr.n_results)
        assert(recv(sv[0], t, hdr.n_results, MSG_WAITALL) == (ssize_t)hdr.n_results);
    close(sv[0]);
    close(sv[1]);
    return t;
}

#define MEM_HDR   CUVS_MEMORY_STATUS_COLS CUVS_MEMORY_STATUS_COLS_I3E
#define MEM_KINDS CUVS_MEMORY_STATUS_KINDS CUVS_MEMORY_STATUS_KINDS_I3E
#define MEM_N     (CUVS_MEMORY_STATUS_NCOLS + CUVS_MEMORY_STATUS_NCOLS_I3E)
#define IDX_HDR   CUVS_INDEX_STATUS_COLS CUVS_INDEX_STATUS_COLS_I3E
#define IDX_KINDS CUVS_INDEX_STATUS_KINDS CUVS_INDEX_STATUS_KINDS_I3E
#define IDX_N     (CUVS_INDEX_STATUS_NCOLS + CUVS_INDEX_STATUS_NCOLS_I3E)

typedef struct Row {
    char  *f[64];
    char   buf[4096];
} Row;

static int
col_index(const char *header, const char *name)
{
    char h[2048];
    int i = 0;
    snprintf(h, sizeof(h), "%s", header);
    for (char *tok = strtok(h, "\t"); tok; tok = strtok(NULL, "\t"), i++)
        if (strcmp(tok, name) == 0)
            return i;
    return -1;
}

/* Row of the given gpu (column 0) from a memory_status reply; every field
 * must parse with its declared kind. Returns 1 when found. */
static int
mem_row(int gpu, Row *out)
{
    char *t = read_text(handle_memory_status);
    size_t len = strlen(t), pos = 0, ll;
    const char *line;
    int found = 0;
    CHECK(cuvs_status_check_header(t, len, MEM_HDR, &pos) == 1);
    while (cuvs_status_next_line(t, len, &pos, &line, &ll) == 1)
    {
        Row r;
        assert(ll < sizeof(r.buf));
        memcpy(r.buf, line, ll);
        r.buf[ll] = '\0';
        CHECK(cuvs_status_split(r.buf, ll, r.f, MEM_N) == MEM_N);
        for (int c = 0; c < MEM_N; c++)
        {
            int64_t v;
            int isnull;
            CHECK(cuvs_status_parse_field(MEM_KINDS[c], r.f[c], &v, &isnull) == 0);
        }
        if (atoi(r.f[0]) == gpu)
        {
            *out = r;
            for (int c = 0; c < MEM_N; c++)    /* rebase into the copy */
                out->f[c] = out->buf + (r.f[c] - r.buf);
            found = 1;
        }
    }
    free(t);
    return found;
}

static long long
mcol(const Row *r, const char *name)
{
    int c = col_index(MEM_HDR, name);
    assert(c >= 0);
    return atoll(r->f[c]);
}

static const char *
mtext(const Row *r, const char *name)
{
    int c = col_index(MEM_HDR, name);
    assert(c >= 0);
    return r->f[c];
}

/* ---- build drivers (corpus as in test_lw_daemon.c) ---- */
static const int64_t N = 64;
static const uint32_t DIM = 8;
static char tmpdir[256];

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
run_handle_build(uint32_t index_oid)
{
    int sv[2], st;
    size_t total = (size_t)N * DIM * sizeof(float) + (size_t)N * sizeof(uint64_t);
    char dir[256];
    CuvsCmdFrame cmd = build_cmd(index_oid, 1);
    int mfd = memfd_create("i3e_test", MFD_CLOEXEC);
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
        snprintf(desc[p].shm_name, sizeof(desc[p].shm_name), "/pg_cuvs_bld_i3etest_%d_%d",
                 (int)getpid(), p);
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

typedef struct BuildJob {
    uint32_t idx;
    int      multi;
    int      status;
} BuildJob;

static void *
build_thread(void *arg)
{
    BuildJob *j = arg;
    j->status = j->multi ? run_handle_build_multi(j->idx) : run_handle_build(j->idx);
    return NULL;
}

static int64_t
wall_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ---- tests ---- */
static void
test_rejects(void)
{
    AdmitLine al = {.op = "BUILD", .path = "single", .algo = "cagra", .gpu = 1,
                    .db = 1, .idx = 7, .dim = 8, .n = 64, .final = 100, .needed = 150};
    Row r1, r2;
    time_t t0 = time(NULL), t1;

    log_admit(&al, CUVS_ADMISSION_AR3);
    log_admit(&al, CUVS_ADMISSION_AR3);
    log_admit(&al, CUVS_ADMISSION_AR6);
    log_admit(&al, CUVS_ADMISSION_NONE);    /* an admission is not a rejection */
    log_admit(&al, CUVS_ADMISSION_OOM);     /* not an admission rejection */
    log_admit(&al, CUVS_ADMISSION_ATT);
    al.gpu = -1;
    log_admit(&al, CUVS_ADMISSION_AR0);     /* no GPU chosen */
    t1 = time(NULL);
    /* admission_fill formats replies and rechecks: it must not count. */
    {
        CuvsAdmissionFields f;
        admission_fill(&f, CUVS_ADMISSION_AR4, 150, 1);
    }

    CHECK(g_rejects[1][CUVS_ADMISSION_AR3] == 2);
    CHECK(g_rejects[1][CUVS_ADMISSION_AR6] == 1);
    CHECK(g_rejects[1][CUVS_ADMISSION_AR4] == 0);
    CHECK(g_rejects_no_gpu[CUVS_ADMISSION_AR0] == 1);
    CHECK(g_rejects[0][CUVS_ADMISSION_AR3] == 0);   /* other GPU untouched */
    CHECK(mem_row(1, &r1));
    CHECK(mcol(&r1, "rejects_ar3") == 2);
    CHECK(mcol(&r1, "rejects_ar6") == 1);
    CHECK(mcol(&r1, "rejects_ar0") == 0 && mcol(&r1, "rejects_ar7") == 0);
    CHECK(strcmp(mtext(&r1, "last_reject_reason"), "AR6") == 0);
    CHECK(mcol(&r1, "last_reject_at") >= (long long)t0 && mcol(&r1, "last_reject_at") <= (long long)t1);
    CHECK(mem_row(0, &r2));
    CHECK(strcmp(mtext(&r2, "last_reject_reason"), "") == 0);   /* NULL: none yet */
    CHECK(mcol(&r2, "last_reject_at") == 0);                     /* NULL time */
    /* Reading does not reset (C8). */
    CHECK(mem_row(1, &r2));
    CHECK(mcol(&r2, "rejects_ar3") == 2 && mcol(&r2, "rejects_ar6") == 1);
    /* The pool status reply carries the gpu -1 counts. */
    {
        CuvsRmmPoolStatus st;
        memset(&st, 0, sizeof(st));
        i3e_fill_pool_status(&st);
        CHECK(st.rejects_no_gpu[CUVS_ADMISSION_AR0] == 1);
        CHECK(st.rejects_no_gpu[CUVS_ADMISSION_AR3] == 0);
    }
}

static void
test_memory_columns(void)
{
    Row r;
    g_max_vram_per_gpu[1] = (size_t)3 << 30;
    g_rmm_pool_limits[1] = (size_t)2 << 30;
    g_pending_rmm_build_vram[1] = 4444;
    g_lw_pending[1] = 5555;
    g_pending_build_count[1] = 2;
    g_managed_spill_builds[1] = 3;
    g_managed_spill_bytes_max[1] = 7777;
    fx_pool_ok = 0;                  /* shim: the pool cannot be read */
    CHECK(mem_row(1, &r));
    CHECK(mcol(&r, "budget_bytes") == (long long)((size_t)3 << 30));
    CHECK(mcol(&r, "quota_bytes") == (long long)((size_t)2 << 30));
    CHECK(mcol(&r, "pool_used") == -1 && mcol(&r, "pool_reserved") == -1);   /* NULL */
    CHECK(mcol(&r, "pending_rmm") == 4444);
    CHECK(mcol(&r, "lw_pending") == 5555);
    CHECK(mcol(&r, "builds_active") == 2);
    CHECK(mcol(&r, "managed_spill_builds") == 3);
    CHECK(mcol(&r, "managed_spill_bytes_max") == 7777);
    fx_pool_ok = 1;
    fx_pool.used_bytes = 1111;
    fx_pool.reserved_bytes = 2222;
    CHECK(mem_row(1, &r));
    CHECK(mcol(&r, "pool_used") == 1111 && mcol(&r, "pool_reserved") == 2222);
    g_rmm_pool_limits[1] = 0;        /* no quota: pool columns NULL even if readable */
    CHECK(mem_row(1, &r));
    CHECK(mcol(&r, "pool_used") == -1 && mcol(&r, "quota_bytes") == 0);
    fx_pool_ok = 0;
    g_max_vram_per_gpu[1] = (size_t)64 << 30;
    g_pending_rmm_build_vram[1] = 0;
    g_lw_pending[1] = 0;
    g_pending_build_count[1] = 0;
    g_managed_spill_builds[1] = 0;
    g_managed_spill_bytes_max[1] = 0;
}

static void
check_inflight(int multi, uint32_t idx)
{
    BuildJob job = {.idx = idx, .multi = multi};
    pthread_t th;
    CuvsRmmPoolStatus st;
    Row r;
    int64_t t0 = wall_us(), t1;
    int gpu;

    fx_hold = 1;
    assert(pthread_create(&th, NULL, build_thread, &job) == 0);
    sem_wait(&fx_entered);
    t1 = wall_us();
    memset(&st, 0, sizeof(st));
    i3e_fill_pool_status(&st);
    CHECK(fx_used_at_entry == 1);
    CHECK(st.n_builds == 1);
    if (st.n_builds == 1)
    {
        const CuvsBuildStatusRow *b = &st.builds[0];
        size_t final = estimate_vram_bytes(N, (int)DIM, 4);
        gpu = b->gpu;
        CHECK(gpu >= 0 && gpu < 2);
        CHECK(b->db_oid == 1 && b->index_oid == idx);
        CHECK(b->n == N && b->dim == DIM);
        CHECK(strcmp(b->algo, admission_algo_name(CUVS_CAGRA_BUILD_AUTO)) == 0);
        CHECK(strcmp(b->path, multi ? "multi" : "single") == 0);
        CHECK(strcmp(b->phase, "building") == 0);
        CHECK(b->final_bytes == final);
        CHECK(b->reserved_n_bytes == admission_needed_bytes(final));
        CHECK(b->scratch_bytes == b->reserved_n_bytes - b->final_bytes);
        /* Same numbers the accounting holds while the build is in flight. */
        CHECK(b->reserved_n_bytes == g_pending_build_vram[gpu]);
        CHECK(b->lw_ub_valid == 1 && b->lw_ub_bytes == g_lw_pending[gpu]);
        CHECK(b->lw_ub_bytes == lw_ub_for_build(N, DIM, 8));
        CHECK(b->policy == g_cagra_build_overflow);
        CHECK(b->started_at_us >= t0 && b->started_at_us <= t1);
        CHECK(mem_row(gpu, &r));
        CHECK(mcol(&r, "builds_active") == 1);
        CHECK(mcol(&r, "pending_bytes") == (long long)b->reserved_n_bytes);
        CHECK(mcol(&r, "lw_pending") == (long long)b->lw_ub_bytes);
    }
    sem_post(&fx_release);
    pthread_join(th, NULL);
    fx_hold = 0;
    memset(&st, 0, sizeof(st));
    i3e_fill_pool_status(&st);
    CHECK(st.n_builds == 0);
    CHECK(count_used() == 0);
}

static void
test_build_status(void)
{
    CuvsRmmPoolStatus st;
    uint64_t unlisted;

    check_inflight(0, 201);
    check_inflight(1, 202);
    /* Policy error is reported as the build's policy. */
    g_cagra_build_overflow = CUVS_LW_OVERFLOW_ERROR;
    check_inflight(0, 203);
    g_cagra_build_overflow = CUVS_CAGRA_BUILD_OVERFLOW_DEFAULT;

    /* A failed build leaves no row. */
    fx_fail = 1;
    CHECK(run_handle_build(204) != CUVS_STATUS_OK);
    CHECK(fx_used_at_entry == 1 && count_used() == 0);
    CHECK(run_handle_build_multi(205) != CUVS_STATUS_OK);
    CHECK(fx_used_at_entry == 1 && count_used() == 0);
    fx_fail = 0;

    /* build_sharded holds g_index_mutex for the build and never registers. */
    CHECK(run_build_sharded(206) == CUVS_STATUS_OK);
    CHECK(fx_used_at_entry == 0 && count_used() == 0);

    /* Full table: the build runs, is not listed and is counted. */
    unlisted = g_builds_unlisted;
    for (int s = 0; s < CUVS_MAX_ACTIVE_BUILDS; s++)
        g_builds_used[s] = 1;
    CHECK(run_handle_build(207) == CUVS_STATUS_OK);
    CHECK(g_builds_unlisted == unlisted + 1);
    memset(&st, 0, sizeof(st));
    i3e_fill_pool_status(&st);
    CHECK(st.builds_unlisted == unlisted + 1);
    CHECK(st.n_builds == CUVS_MAX_ACTIVE_BUILDS);
    memset(g_builds_used, 0, sizeof(g_builds_used));
}

static void
test_index_status(void)
{
    char *t = read_text(handle_index_status);
    size_t len = strlen(t), pos = 0, ll;
    const char *line;
    int rows = 0;
    CHECK(cuvs_status_check_header(t, len, IDX_HDR, &pos) == 1);
    while (cuvs_status_next_line(t, len, &pos, &line, &ll) == 1)
    {
        char buf[4096];
        char *f[64];
        int64_t v;
        int isnull;
        memcpy(buf, line, ll);
        buf[ll] = '\0';
        CHECK(cuvs_status_split(buf, ll, f, IDX_N) == IDX_N);
        CHECK(cuvs_status_parse_field(IDX_KINDS[IDX_N - 1], f[IDX_N - 1], &v, &isnull) == 0);
        CHECK(isnull == 1);            /* dataset_attached NULL in this release */
        rows++;
    }
    CHECK(rows >= 1);                  /* the indexes built above are resident */
    free(t);
}

/* Fake daemon replying to one pool-status request with n_builds = nb. */
typedef struct FakeArgs { int lfd; uint32_t nb; } FakeArgs;
static void *
fake_daemon(void *arg)
{
    FakeArgs *a = arg;
    int c = accept(a->lfd, NULL, NULL);
    CuvsCmdFrame cmd;
    CuvsReplyHeader hdr = {.status = CUVS_STATUS_OK, .n_results = 1};
    static CuvsRmmPoolStatus st;
    if (c < 0) return NULL;
    (void)!recv(c, &cmd, sizeof(cmd), MSG_WAITALL);
    memset(&st, 0, sizeof(st));
    st.version = CUVS_RMM_POOL_STATUS_VERSION;
    st.n_builds = a->nb;
    (void)!send(c, &hdr, sizeof(hdr), MSG_NOSIGNAL);
    (void)!send(c, &st, sizeof(st), MSG_NOSIGNAL);
    close(c);
    return NULL;
}

static int
client_with_nbuilds(uint32_t nb)
{
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    char path[200];
    FakeArgs a = {.nb = nb};
    pthread_t th;
    static CuvsRmmPoolStatus out;
    int rc;
    snprintf(path, sizeof(path), "%s/fake.sock", tmpdir);
    unlink(path);
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    a.lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(a.lfd >= 0 && bind(a.lfd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    assert(listen(a.lfd, 1) == 0);
    assert(pthread_create(&th, NULL, fake_daemon, &a) == 0);
    rc = cuvs_ipc_rmm_pool_status(path, &out);
    pthread_join(th, NULL);
    close(a.lfd);
    unlink(path);
    return rc;
}

static void
test_client_bound(void)
{
    CHECK(client_with_nbuilds(CUVS_MAX_ACTIVE_BUILDS) == CUVS_STATUS_OK);
    CHECK(client_with_nbuilds(CUVS_MAX_ACTIVE_BUILDS + 1) == CUVS_STATUS_ERROR);
}

int
main(void)
{
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/i3e_obs_%d", (int)getpid());
    mkdir(tmpdir, 0700);
    sem_init(&fx_entered, 0, 0);
    sem_init(&fx_release, 0, 0);
    g_n_gpus = 2;
    g_max_vram_per_gpu[0] = g_max_vram_per_gpu[1] = (size_t)64 << 30;
    g_indexes = calloc((size_t)g_max_indexes, sizeof(IndexEntry));
    g_cold_indexes = calloc((size_t)g_max_indexes, sizeof(ColdIndexEntry));
    assert(g_indexes && g_cold_indexes);
    g_lw_host_guard_fn = fixture_host_guard;

    test_rejects();
    test_memory_columns();
    test_build_status();
    test_index_status();
    test_client_bound();
    if (failures)
    {
        fprintf(stderr, "test_i3e_observation: %d failure(s)\n", failures);
        return 1;
    }
    printf("PASS i3e observation: rejects per emitted [admit] (AR0..AR7, gpu -1 apart, "
           "admission_fill not counted, read does not reset), memory_status I3-E columns "
           "equal the daemon variables (pool NULL when unreadable or no quota), build_status "
           "rows for handle_build/handle_build_multi in flight (N, lw_ub, policy, wall start) "
           "and removed on success/failure, none for build_sharded, unlisted when full, "
           "index_status dataset_attached NULL, client n_builds bound\n");
    return 0;
}
