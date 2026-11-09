/* hnsw route decisions through the actual daemon source (Linux, CPU shim).
 * The pre-spool forecast (op 40, handle_hnsw_route_preflight) and the build
 * op (op 23, handle_build_hnsw_file, reached through the real client
 * cuvs_ipc_build_hnsw_file) must report the bytes of the status-13 DETAIL
 * (v5 C5: he hb ge gb rp rl) and the disk refusal text (v5 C5 / v6 C2,
 * Q4-4). Fixtures: GPU free bytes, RMM pool used bytes, and the meminfo /
 * cgroup files the host budget reads (so the host budget is the request).
 * The disk refusal uses a tmpfs index directory (/dev/shm), which the
 * precheck refuses with fs=tmpfs whatever the free space. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
FILE *fixture_fopen(const char *path, const char *mode);
size_t fixture_free(int device_id);
#define cuvs_vram_free_bytes_on fixture_free
#define cuvs_vram_physical_free_bytes_on fixture_free
#define cuvs_rmm_pool_stats fixture_pool_stats
#define fopen fixture_fopen
#define main pg_cuvs_server_program_main
#include "../../src/pg_cuvs_server.c"
#undef main
#undef fopen
#undef cuvs_rmm_pool_stats
#undef cuvs_vram_physical_free_bytes_on
#undef cuvs_vram_free_bytes_on
#include "../../src/cuvs_route_preflight.h"
#include <assert.h>
#include <sys/un.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define MiB (UINT64_C(1) << 20)
#define GiB (UINT64_C(1) << 30)
static const uint64_t POOL_USED = UINT64_C(3196800000);   /* rp = 3048 MiB */
static const uint64_t RESIDENT = GiB;                       /* rl = 1024 MiB */
static const uint64_t GPU_FREE = 16 * GiB;
static const uint64_t HOST_REQ = 1536 * MiB;
/* auto over the resident budget at HOST_REQ: ACE with partitions (overflow
 * ace) or status 13 (overflow error). */
static const uint64_t N = 300000;
static const uint32_t DIM = 32;

static char fx_dir[256], fx_meminfo[320], fx_cgroup[320];

size_t fixture_free(int device_id) { (void)device_id; return (size_t)GPU_FREE; }
int
fixture_pool_stats(int device_id, CuvsRmmPoolStats *out)
{
    memset(out, 0, sizeof(*out));
    out->device_id = (uint32_t)device_id;
    out->used_bytes = POOL_USED;
    out->reserved_bytes = POOL_USED;
    return 0;
}
FILE *
fixture_fopen(const char *path, const char *mode)
{
    if (strcmp(path, "/proc/meminfo") == 0) path = fx_meminfo;
    else if (strcmp(path, "/proc/self/cgroup") == 0) path = fx_cgroup;
    else if (strncmp(path, "/sys/fs/cgroup", 14) == 0) path = "/nonexistent/pg_cuvs_cgroup";
    return fopen(path, mode);
}

static void
write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

/* Runs fn with stderr captured into buf. */
static void
capture(void (*fn)(void *), void *arg, char *buf, size_t cap)
{
    char path[] = "/tmp/hnsw-route-log-XXXXXX";
    int fd = mkstemp(path), saved = dup(2);
    size_t n;
    FILE *f;
    assert(fd >= 0 && saved >= 0);
    fflush(stderr);
    dup2(fd, 2);
    fn(arg);
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

static const char *
route_line(const char *log, const char *phase)
{
    char key[64];
    snprintf(key, sizeof(key), "[hnsw_route] phase=%s ", phase);
    return strstr(log, key);
}

static IndexEntry g_resident;

static void
reset(uint32_t overflow, int tmpfs_dir)
{
    memset(&g_resident, 0, sizeof(g_resident));
    g_resident.valid = 1; g_resident.is_flat = 1; g_resident.gpu_device_id = 0;
    g_resident.db_oid = 1; g_resident.index_oid = 9; g_resident.main_bf_vram_bytes = RESIDENT;
    g_indexes = &g_resident; g_n_indexes = 1;
    memset(g_pending_build_vram, 0, sizeof(g_pending_build_vram));
    memset(g_pending_rmm_build_vram, 0, sizeof(g_pending_rmm_build_vram));
    memset(g_max_vram_per_gpu, 0, sizeof(g_max_vram_per_gpu));
    g_max_vram_per_gpu[0] = 32 * GiB;
    g_hnsw_auto_overflow = overflow;
    snprintf(g_index_dir, sizeof(g_index_dir), "%s", tmpfs_dir ? "/dev/shm/pg_cuvs_route_test" : fx_dir);
    if (tmpfs_dir) mkdir(g_index_dir, 0700);
}

/* ---- op 40 ---------------------------------------------------------- */
typedef struct PreflightCall { CuvsHnswPreflightRequest req; CuvsHnswPreflightReply reply; } PreflightCall;

static void
preflight_fn(void *arg)
{
    PreflightCall *c = arg;
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(send_all(sv[0], &c->req, sizeof(c->req)) == 0);
    handle_hnsw_route_preflight(sv[1]);
    assert(recv_all(sv[0], &c->reply, sizeof(c->reply)) == 0);
    close(sv[0]); close(sv[1]);
}

static void
preflight(PreflightCall *c, char *log, size_t cap)
{
    memset(&c->req, 0, sizeof(c->req));
    c->req.version = CUVS_HNSW_PREFLIGHT_VERSION;
    c->req.build_algo = CUVS_HNSW_ALGO_AUTO;
    c->req.ef_construction = 64;
    c->req.dim = DIM;
    c->req.graph_degree = 64;
    c->req.intermediate_degree = 128;
    c->req.n_estimate = N;
    c->req.host_budget = HOST_REQ;
    capture(preflight_fn, c, log, cap);
}

/* The forecast the daemon must have made: same pure function, same inputs. */
static CuvsHnswForecast
expected(uint32_t overflow)
{
    CuvsHnswForecast f;
    char why[160];
    (void)cuvs_hnsw_forecast_route(N, DIM, 64, 128, 0, CUVS_HNSW_ALGO_AUTO, 64, overflow,
                                   HOST_REQ, GPU_FREE, &f, why, sizeof(why));
    return f;
}

static void
test_preflight_status13_bytes(void)
{
    PreflightCall c;
    char log[8192], detail[CUVS_HNSW_BUDGET_DETAIL_MAX + 1], want[CUVS_HNSW_BUDGET_DETAIL_MAX + 1];
    CuvsHnswForecast f = expected(CUVS_HNSW_OVERFLOW_ERROR);
    CuvsHnswBudgetSnapshot b;
    reset(CUVS_HNSW_OVERFLOW_ERROR, 0);
    CHECK(f.status == CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED);
    preflight(&c, log, sizeof(log));
    CHECK(c.reply.header.status == CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED);
    CHECK(c.reply.version == CUVS_HNSW_PREFLIGHT_VERSION && !c.reply.gpu_static);
    CHECK(c.reply.host_budget == HOST_REQ && c.reply.gpu_budget == GPU_FREE);
    CHECK(c.reply.host_estimate == f.host_estimate && c.reply.gpu_estimate == f.gpu_estimate);
    CHECK(c.reply.pool_used == POOL_USED);       /* rp source: pool used_bytes */
    CHECK(c.reply.logical_used == RESIDENT);     /* rl source: total_vram_used */
    b = cuvs_hnsw_preflight_budget(&c.reply);
    (void)cuvs_hnsw_budget_detail(detail, sizeof(detail), &b);
    snprintf(want, sizeof(want), "he=%llu hb=1536 ge=%llu gb=16384 rp=3048 rl=1024",
             (unsigned long long)(f.host_estimate >> 20), (unsigned long long)(f.gpu_estimate >> 20));
    CHECK(strcmp(detail, want) == 0);
    CHECK(route_line(log, "prespool") != NULL);
}

static void
test_preflight_disk_refusal(void)
{
    PreflightCall c;
    char log[8192], want[160];
    CuvsHnswForecast f = expected(CUVS_HNSW_OVERFLOW_ACE);
    reset(CUVS_HNSW_OVERFLOW_ACE, 1);
    CHECK(f.status == CUVS_STATUS_OK && f.decision.route == CUVS_HNSW_ROUTE_ACE);
    preflight(&c, log, sizeof(log));
    CHECK(c.reply.header.status == CUVS_STATUS_BUILD_FAILED && !c.reply.disk_ok);
    CHECK(c.reply.disk_regular == 0);            /* /dev/shm is tmpfs */
    snprintf(want, sizeof(want), "HNSW needs regular disk capacity: need=%llu avail=%llu fs=tmpfs route=ace_disk",
             (unsigned long long)f.disk_need, (unsigned long long)c.reply.disk_avail);
    CHECK(strcmp(c.reply.header.error, want) == 0);
    CHECK(cuvs_hnsw_disk_refusal((int)c.reply.header.status, c.reply.header.error));
    CHECK(c.reply.pool_used == POOL_USED && c.reply.logical_used == RESIDENT);
    {
        const char *line = route_line(log, "prespool");
        CHECK(line && strstr(line, " status=5 ") && strstr(line, " error=HNSW needs regular disk capacity: "));
        /* Q4-4: the decided route survives the disk refusal on the journal */
        CHECK(line && strstr(line, " route=ace_disk ") && !strstr(line, " route=none "));
        if (line && !strstr(line, " route=ace_disk "))
            fprintf(stderr, "  journal: %.*s\n", (int)strcspn(line, "\n"), line);
    }
    rmdir(g_index_dir);
}

/* ---- op 23 through the real client -------------------------------------- */
typedef struct BuildCall {
    const char *sock;
    int vectors_fd, rc, result_fd;
    CuvsHnswPolicy policy;
    CuvsHnswAceReadback ace;
    CuvsHnswBudgetSnapshot budget;
    char error[128];
} BuildCall;

static void *
client_fn(void *arg)
{
    BuildCall *c = arg;
    c->rc = cuvs_ipc_build_hnsw_file(c->sock, c->vectors_fd, N, DIM, CUVS_METRIC_L2, 64, 128, 0,
                                     HOST_REQ, CUVS_HNSW_ALGO_AUTO, 64, &c->policy, &c->budget, &c->ace,
                                     &c->result_fd, c->error, sizeof(c->error));
    return NULL;
}

typedef struct ServeCall { int listener; } ServeCall;

static void
serve_fn(void *arg)
{
    ServeCall *s = arg;
    CuvsCmdFrame cmd;
    int fd = accept(s->listener, NULL, NULL);
    assert(fd >= 0);
    assert(recv_all(fd, &cmd, sizeof(cmd)) == 0 && cmd.op == CUVS_OP_BUILD_HNSW_FILE);
    g_peer_uid = getuid();                       /* the client is this process */
    handle_build_hnsw_file(fd, &cmd);
    close(fd);
}

static void
build(BuildCall *c, char *log, size_t cap)
{
    char sock[300], input[300];
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    ServeCall s;
    pthread_t t;
    snprintf(sock, sizeof(sock), "%s/s", fx_dir);
    snprintf(input, sizeof(input), "%s/vectors-XXXXXX", fx_dir);
    unlink(sock);
    s.listener = socket(AF_UNIX, SOCK_STREAM, 0);
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock);
    assert(s.listener >= 0 && bind(s.listener, (struct sockaddr *)&addr, sizeof(addr)) == 0
           && listen(s.listener, 1) == 0);
    memset(c, 0, sizeof(*c));
    c->sock = sock;
    c->result_fd = -1;
    c->vectors_fd = mkstemp(input);
    assert(c->vectors_fd >= 0 && ftruncate(c->vectors_fd, (off_t)(N * DIM * sizeof(float))) == 0);
    unlink(input);
    assert(pthread_create(&t, NULL, client_fn, c) == 0);
    capture(serve_fn, &s, log, cap);
    assert(pthread_join(t, NULL) == 0);
    close(c->vectors_fd);
    close(s.listener);
    unlink(sock);
}

static void
test_build_status13_bytes(void)
{
    BuildCall c;
    char log[8192];
    CuvsHnswForecast f = expected(CUVS_HNSW_OVERFLOW_ERROR);
    reset(CUVS_HNSW_OVERFLOW_ERROR, 0);
    build(&c, log, sizeof(log));
    CHECK(c.rc == CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED && c.result_fd == -1);
    CHECK(c.budget.host_estimate == f.host_estimate && c.budget.host_budget == HOST_REQ);
    CHECK(c.budget.gpu_estimate == f.gpu_estimate && c.budget.gpu_budget == GPU_FREE);
    CHECK(c.budget.pool_used == POOL_USED && c.budget.logical_used == RESIDENT);
    CHECK(route_line(log, "build") != NULL);
    CHECK(strstr(log, "phase=start") == NULL);  /* no build started */
}

static void
test_build_disk_refusal(void)
{
    BuildCall c;
    char log[8192];
    CuvsHnswForecast f = expected(CUVS_HNSW_OVERFLOW_ACE);
    reset(CUVS_HNSW_OVERFLOW_ACE, 1);
    build(&c, log, sizeof(log));
    CHECK(c.rc == CUVS_STATUS_BUILD_FAILED && c.result_fd == -1);
    CHECK(cuvs_hnsw_disk_refusal(c.rc, c.error));
    {
        char want[160];
        snprintf(want, sizeof(want), "HNSW needs regular disk capacity: need=%llu avail=",
                 (unsigned long long)f.disk_need);
        CHECK(strncmp(c.error, want, strlen(want)) == 0 && strstr(c.error, " fs=tmpfs route=ace_disk"));
    }
    CHECK(strstr(log, "phase=start") == NULL);  /* refused before the build */
    {
        const char *line = route_line(log, "build");
        CHECK(line && strstr(line, " route=ace_disk "));
    }
    rmdir(g_index_dir);
}

int
main(void)
{
    char dir[] = "/tmp/hnsw-route-daemon-XXXXXX";
    assert(mkdtemp(dir));
    snprintf(fx_dir, sizeof(fx_dir), "%s", dir);
    snprintf(fx_meminfo, sizeof(fx_meminfo), "%s/meminfo", dir);
    snprintf(fx_cgroup, sizeof(fx_cgroup), "%s/cgroup", dir);
    write_file(fx_meminfo, "MemTotal: 134217728 kB\nMemAvailable: 67108864 kB\n");
    write_file(fx_cgroup, "0::/\n");
    test_preflight_status13_bytes();
    test_preflight_disk_refusal();
    test_build_status13_bytes();
    test_build_disk_refusal();
    unlink(fx_meminfo); unlink(fx_cgroup); rmdir(dir);
    if (failures)
    {
        fprintf(stderr, "test_hnsw_route_daemon: %d failure(s)\n", failures);
        return 1;
    }
    puts("PASS hnsw route daemon: op 40 and op 23 (real client) send he hb ge gb rp rl bytes "
         "(rp = pool used_bytes, rl = total_vram_used) on status 13 and on a disk refusal; "
         "the disk refusal text is the plan's, fs=tmpfs route=ace_disk, nothing built");
    return 0;
}
