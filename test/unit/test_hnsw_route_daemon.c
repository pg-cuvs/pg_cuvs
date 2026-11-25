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
#ifdef CUVS_CAGRA_FOLD_ENABLED
#define cuvs_cagra_fold_shape fixture_fold_shape
#define cuvs_cagra_fold_reuse_with_yield fixture_fold_call
#define cuvs_cagra_extend fixture_extend
#define cuvs_vram_raw_free_bytes_on fixture_raw_free
#endif
#define fopen fixture_fopen
#define main pg_cuvs_server_program_main
#include "../../src/pg_cuvs_server.c"
#undef main
#undef fopen
#undef cuvs_rmm_pool_stats
#undef cuvs_vram_physical_free_bytes_on
#undef cuvs_vram_free_bytes_on
#undef cuvs_cagra_fold_shape
#undef cuvs_cagra_fold_reuse_with_yield
#undef cuvs_cagra_extend
#undef cuvs_vram_raw_free_bytes_on
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
#ifdef CUVS_CAGRA_FOLD_ENABLED
static bool fixture_pressure_pool;
static size_t fixture_raw_free_override;
size_t cuvs_vram_raw_free_bytes_on(int device_id);

size_t
fixture_raw_free(int device_id)
{
    return fixture_raw_free_override ? fixture_raw_free_override : cuvs_vram_raw_free_bytes_on(device_id);
}
#endif

size_t fixture_free(int device_id) { (void)device_id; return (size_t)GPU_FREE; }
int
fixture_pool_stats(int device_id, CuvsRmmPoolStats *out)
{
    memset(out, 0, sizeof(*out));
    out->device_id = (uint32_t)device_id;
#ifdef CUVS_CAGRA_FOLD_ENABLED
    if (fixture_pressure_pool)
    {
        /* The pressure fixtures have no pending allocations. Map their live
         * ledger to used bytes while holding the backing fixed. */
        out->used_bytes = total_vram_used(device_id);
        out->reserved_bytes = out->maximum_bytes = g_rmm_pool_limits[device_id];
        return 0;
    }
#endif
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
    if (tmpfs_dir)
    {
        snprintf(g_index_dir, sizeof(g_index_dir), "/dev/shm/pg_cuvs_route_test-%ld", (long) getpid());
        mkdir(g_index_dir, 0700);
    }
    else
        snprintf(g_index_dir, sizeof(g_index_dir), "%s", fx_dir);
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
    uint64_t rows;
    uint32_t algo;
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
    c->rc = cuvs_ipc_build_hnsw_file(c->sock, c->vectors_fd, c->rows, DIM, CUVS_METRIC_L2, 64, 128, 0,
                                     HOST_REQ, c->algo, 64, &c->policy, &c->budget, &c->ace, NULL,
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
build(BuildCall *c, uint64_t rows, uint32_t algo, char *log, size_t cap)
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
    c->rows = rows;
    c->algo = algo;
    c->result_fd = -1;
    c->vectors_fd = mkstemp(input);
    assert(c->vectors_fd >= 0 && ftruncate(c->vectors_fd, (off_t)(rows * DIM * sizeof(float))) == 0);
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
    build(&c, N, CUVS_HNSW_ALGO_AUTO, log, sizeof(log));
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
    build(&c, N, CUVS_HNSW_ALGO_AUTO, log, sizeof(log));
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

#ifdef CUVS_CAGRA_FOLD_ENABLED
static unsigned fold_calls;
static size_t expected_host_charge;
static CuvsCagraFoldStatus fold_device_status;

#ifdef CUVS_PAGE_OVERLAY_GPU
static unsigned overlay_reclaims;

bool
cuvs_page_overlay_gpu_reclaim(int device)
{
    assert(device == 1 && pthread_mutex_trylock(&g_index_mutex) == 0);
    CHECK(g_page_gpu_host_fold_device == 0 && g_page_gpu_host_fold_order == 11 &&
          g_page_gpu_live[1] == MiB);
    g_page_gpu_live[1] = 0;
    g_max_vram_per_gpu[0] = 32 * MiB;  /* changed after the host-pressure hold */
    ++overlay_reclaims;
    pthread_mutex_unlock(&g_index_mutex);
    return true;
}

static void
test_gpu_host_retry_vram(bool other_ready)
{
    PageCagraGraph parent = {.n_slots = 512, .dim = 32, .index_oid = 31, .registered_order = 11,
        .overlay = {.boot_id = 9, .covered_upto = 100}};
    PageCagraGraph other = parent;

    reset(CUVS_HNSW_OVERFLOW_ERROR, 0);
    g_n_gpus = 2;
    parent.handle = &parent;
    if (other_ready)
    {
        parent.fold_retry_after = UINT64_MAX;
        parent.next = &other;
        other.handle = &other;
        other.index_oid = 32;
        other.registered_order = 12;
    }
    g_page_graphs = &parent;
    g_page_gpu_min_rows = 1;
    g_page_gpu_host_fold_device = 0;
    g_page_gpu_host_fold_order = parent.registered_order;
    g_page_gpu_live[1] = MiB;
    overlay_reclaims = 0;
    page_fold_run(0);
    CHECK(overlay_reclaims == 1 && g_page_gpu_live[1] == 0);
    if (other_ready)
    {
        CHECK(other.fold_retry_after > 0);
        CHECK(g_page_gpu_host_fold_device == 0);
        CHECK(g_page_gpu_host_fold_order == parent.registered_order);
        parent.fold_retry_after = 0;
        other.fold_retry_after = 0;
        other.last_search = time(NULL);
        other.hits = 100;  /* ordinary selection would pick this graph again */
        page_fold_run(0);
        CHECK(other.fold_retry_after == 0);
    }
    CHECK(parent.fold_retry_after > 0 && fold_calls == 0);
    CHECK(g_page_gpu_host_fold_device == -1);
    CHECK(!g_pending_build_vram[0] && !g_fold_active[0]);
    g_page_graphs = NULL;
}
#endif

int
fixture_fold_shape(CuvsCagraIndex handle, CuvsCagraFoldShape *shape)
{
    assert(handle);
    *shape = (CuvsCagraFoldShape) {.rows = 512, .dim = 32, .stride = 32, .degree = 64};
    return 0;
}

CuvsCagraFoldStatus
fixture_fold_call(CuvsCagraIndex source, const float *added, int64_t rows, int dim,
                  int device, CuvsCagraIndex *child, const CuvsCagraFoldControl *control)
{
    assert(source && added && rows == 100 && dim == 32 && device == 0 && !*child && control);
    CHECK(g_page_overlay_budget.used == expected_host_charge && g_fold_claim[0] > 0);
    ++fold_calls;
    if (fold_device_status == CUVS_CAGRA_FOLD_OK)
    {
        static float rows_fixture[612 * 32];

        *child = cuvs_cagra_build(rows_fixture, 612, 32, CUVS_METRIC_L2, 64, 128,
            CUVS_CAGRA_BUILD_AUTO, 0, CUVS_LW_OVERFLOW_ERROR, NULL);
        assert(*child);
    }
    return fold_device_status;
}

static void
test_fold_host_charge(void)
{
    CuvsPageOverlayRequest request = {.positions = 100};
    size_t vectors = request.positions * 32 * sizeof(float);
    size_t published_bytes = vectors + request.positions * sizeof(CuvsPagePosition);
    size_t staging_bytes = vectors + request.positions;
    size_t mirrors = (512 + request.positions) * (32 + 64) * sizeof(float);
    CuvsPageOverlayChunk *chunk;
    PageCagraGraph parent = {.n_slots = 512, .dim = 32, .db_oid = 2, .index_oid = 31,
        .system_identifier = 1, .index_nonce = 5, .base_nonce = 8, .graph_nonce = 8,
        .overlay = {.boot_id = 9, .covered_upto = request.positions}};

    reset(CUVS_HNSW_OVERFLOW_ERROR, 0);
    g_n_gpus = 1;
    expected_host_charge = sizeof(*chunk) * 2 + published_bytes + staging_bytes + mirrors;
    g_page_overlay_budget.limit = expected_host_charge - 1;
    assert(cuvs_page_overlay_alloc(&g_page_overlay_budget, &request, published_bytes, &chunk) == CUVS_STATUS_OK);
    memset(chunk->marks, 0, published_bytes);
    for (uint32_t i = 0; i < request.positions; ++i)
        chunk->marks[i] = (CuvsPagePosition) {.block = 1, .state = CUVS_PAGE_POSITION_VECTOR};
    parent.overlay.head = chunk;
    parent.handle = &parent;    /* the fixture never dereferences the handle */
    g_page_graphs = &parent;
    page_fold_run(0);
    CHECK(fold_calls == 0 && g_page_overlay_budget.used == chunk->charged_bytes);

    g_page_overlay_budget.limit = expected_host_charge;
    const CuvsCagraFoldStatus outcomes[] = {
        CUVS_CAGRA_FOLD_OOM, CUVS_CAGRA_FOLD_YIELDED, CUVS_CAGRA_FOLD_ERROR
    };
    for (unsigned i = 0; i < sizeof(outcomes) / sizeof(outcomes[0]); ++i)
    {
        parent.fold_retry_after = 0;
        fold_device_status = outcomes[i];
        page_fold_run(0);
        CHECK(fold_calls == i + 1 && g_page_overlay_budget.used == chunk->charged_bytes);
        CHECK(!g_fold_claim[0] && !g_pending_build_vram[0] && !g_fold_active[0]);
        CHECK(!parent.fold_candidate && !parent.fold_pins);
    }
    parent.fold_retry_after = 0;
    fold_device_status = CUVS_CAGRA_FOLD_OK;
    page_fold_run(0);
    CHECK(parent.fold_candidate && g_page_graph_bytes[0] == mirrors);
    CHECK(g_page_overlay_budget.used == chunk->charged_bytes && !g_fold_claim[0]);
    pthread_mutex_lock(&g_index_mutex);
    discard_page_fold_locked(&parent, "test");
    pthread_mutex_unlock(&g_index_mutex);
    release_retired_fold_graphs();
    CHECK(g_page_graph_bytes[0] == 0);
    g_page_graphs = NULL;
    cuvs_page_overlay_release(chunk);
    CHECK(g_page_overlay_budget.used == 0);
}

static void
start_fold_claim(int device, size_t bytes)
{
    pthread_mutex_lock(&g_index_mutex);
    reserve_build_vram(bytes, device);
    g_fold_claim[device] = bytes;
    __atomic_store_n(&g_fold_active[device], true, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&g_index_mutex);
}

static void
finish_fold_claim(int device)
{
    pthread_mutex_lock(&g_index_mutex);
    release_build_vram(g_fold_claim[device], device);
    g_fold_claim[device] = 0;
    __atomic_store_n(&g_fold_active[device], false, __ATOMIC_SEQ_CST);
    pthread_cond_broadcast(&g_fold_finished);
    pthread_mutex_unlock(&g_index_mutex);
}

static void
wait_for_fold_waiter(int device)
{
    uint64_t until = admission_mono_us() + UINT64_C(5000000);

    while (!page_fold_should_yield(&device) && admission_mono_us() < until)
        usleep(1000);
    assert(page_fold_should_yield(&device));
}

static void *
drain_fold_for_build(void *arg)
{
    int device = *(int *)arg;

    wait_for_fold_waiter(device);
    finish_fold_claim(device);
    return NULL;
}

static void
test_fold_preflight_and_readmission(void)
{
    PreflightCall p = {.req = {.version = CUVS_HNSW_PREFLIGHT_VERSION, .dim = DIM,
        .n_estimate = 512, .build_algo = CUVS_HNSW_ALGO_AUTO, .ef_construction = 64,
        .graph_degree = 64, .intermediate_degree = 128, .host_budget = HOST_REQ}};
    BuildCall b;
    pthread_t drain;
    int device = 0;
    char log[8192];

    reset(CUVS_HNSW_OVERFLOW_ERROR, 0);
    g_n_gpus = 1;
    start_fold_claim(device, 31 * GiB);
    preflight_fn(&p);
    CHECK(p.reply.header.status == CUVS_STATUS_UNAVAILABLE);
    CHECK(cuvs_hnsw_prespool_action(512, p.reply.header.status, p.reply.route,
                                  p.reply.overflowed, p.reply.disk_ok) == CUVS_HNSW_PRESPOOL_FALLBACK);

    assert(pthread_create(&drain, NULL, drain_fold_for_build, &device) == 0);
    build(&b, N, CUVS_HNSW_ALGO_AUTO, log, sizeof(log));
    assert(pthread_join(drain, NULL) == 0);
    CHECK(b.rc == CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED);
    CHECK(b.budget.logical_used == RESIDENT && b.budget.gpu_budget == GPU_FREE);
    CHECK(strstr(log, "op=HNSW") != NULL);
    preflight_fn(&p);
    CHECK(p.reply.header.status == CUVS_STATUS_OK);
    pthread_mutex_lock(&g_index_mutex);
    preflight_fn(&p);
    pthread_mutex_unlock(&g_index_mutex);
    CHECK(p.reply.header.status == CUVS_STATUS_UNAVAILABLE);
}

typedef enum FoldLruFixture
{
    FOLD_LRU_NONE, FOLD_LRU_ALLOWED, FOLD_LRU_PINNED
} FoldLruFixture;

static void
test_fold_candidate_pressure(bool ready_exposed, uint32_t algo, int requester, FoldLruFixture lru)
{
    BuildCall build_call;
    char log[8192];
    PageCagraGraph *parent = calloc(1, sizeof(*parent));
    PageGraphFoldRef pin = {0};
    CuvsPageFoldEvent event = {.length = sizeof(event), .db_oid = 2, .index_oid = 11,
        .positions = 1, .system_identifier = 7, .boot_id = 9, .daemon_id = 1,
        .index_nonce = 8, .base_nonce = 6, .parent_graph_nonce = 6,
        .graph_nonce = 12, .parent_n_slots = 512};
    static float rows[513 * 32];
    bool ace = algo == CUVS_HNSW_ALGO_ACE;
    uint64_t parent_bytes = ace ? 768 * MiB : GiB;
    uint64_t candidate_bytes = ace ? 768 * MiB + 76800 : 16 * GiB;
    bool admitted = !ready_exposed && lru != FOLD_LRU_PINNED;
    CuvsEvictScope prior_scope = g_reclaim.scope;

    assert(parent);
    reset(CUVS_HNSW_OVERFLOW_ERROR, 0);
    g_n_gpus = 1;
    g_max_vram_per_gpu[0] = ace ? 2086 * MiB : 18 * GiB;
    if (ace)
        g_n_indexes = 0;
    if (lru != FOLD_LRU_NONE)
    {
        g_max_vram_per_gpu[0] = 2048 * MiB;
        g_resident.main_bf_vram_bytes = 512 * MiB;
        g_resident.pinned = lru == FOLD_LRU_PINNED;
        g_reclaim.scope.kind = CUVS_SCOPE_LRU;
        parent_bytes = 512 * MiB;
        candidate_bytes = 513 * MiB;
    }
    g_page_daemon_id = event.daemon_id;
    *parent = (PageCagraGraph) {.db_oid = event.db_oid, .index_oid = event.index_oid,
        .system_identifier = event.system_identifier, .index_nonce = event.index_nonce,
        .base_nonce = event.base_nonce, .graph_nonce = event.parent_graph_nonce,
        .n_slots = event.parent_n_slots, .dim = 32, .bytes = parent_bytes,
        .overlay = {.boot_id = event.boot_id, .covered_upto = 1}};
    parent->handle = cuvs_cagra_build(rows, 512, 32, CUVS_METRIC_L2, 64, 128,
        CUVS_CAGRA_BUILD_AUTO, 0, CUVS_LW_OVERFLOW_ERROR, NULL);
    CuvsCagraIndex child = cuvs_cagra_build(rows, 513, 32, CUVS_METRIC_L2, 64, 128,
        CUVS_CAGRA_BUILD_AUTO, 0, CUVS_LW_OVERFLOW_ERROR, NULL);
    assert(parent->handle && child && !g_page_graphs);
    pthread_mutex_lock(&g_index_mutex);
    g_page_graphs = parent;
    g_n_page_graphs = 1;
    g_page_graph_bytes[0] = parent_bytes;
    assert(page_graph_fold_pin_locked(parent, &pin));
    assert(page_graph_register_fold_locked(&pin, &event, child, candidate_bytes));
    pthread_mutex_unlock(&g_index_mutex);
    page_graph_fold_unpin(&pin);

    {
        CuvsPageFoldQuery query = {.system_identifier = event.system_identifier, .boot_id = event.boot_id};
        CuvsPageFoldEvent offered;
        uint64_t protected_us;

        pthread_mutex_lock(&g_index_mutex);
        __atomic_store_n(&g_fold_active[0], true, __ATOMIC_SEQ_CST);
        CHECK(!page_fold_ready_locked(event.db_oid, event.index_oid, event.index_nonce, &query, &offered, NULL, NULL));
        __atomic_store_n(&g_fold_active[0], false, __ATOMIC_SEQ_CST);
        __atomic_fetch_add(&g_fold_waiters[0], 1, __ATOMIC_SEQ_CST);
        CHECK(!page_fold_ready_locked(event.db_oid, event.index_oid, event.index_nonce, &query, &offered, NULL, NULL));
        __atomic_fetch_sub(&g_fold_waiters[0], 1, __ATOMIC_SEQ_CST);
        CHECK(!parent->fold_candidate->fold_protected_us);
        if (ready_exposed)
        {
            assert(page_fold_ready_locked(event.db_oid, event.index_oid, event.index_nonce, &query, &offered,
                                          NULL, &protected_us));
            CHECK(offered.graph_nonce == event.graph_nonce && protected_us != 0);
            uint64_t redelivered_us;
            assert(page_fold_ready_locked(event.db_oid, event.index_oid, event.index_nonce, &query, &offered,
                                          NULL, &redelivered_us));
            CHECK(redelivered_us == protected_us);
        }
        pthread_mutex_unlock(&g_index_mutex);
    }

    if (ace)
    {
        CuvsHnswForecast f;
        char error[160];
        uint64_t gpu = 2086 * MiB - parent_bytes - candidate_bytes;

        CHECK(cuvs_hnsw_forecast_route(256, 32, 64, 128, 0, algo, 64, CUVS_HNSW_OVERFLOW_ERROR,
                                      HOST_REQ, gpu, &f, error, sizeof(error)) == CUVS_STATUS_BUILD_FAILED);
        CHECK(strcmp(error, CUVS_HNSW_BUDGET_NO_FIT_REASON) == 0 && f.gpu_estimate < gpu);
    }
    if (requester >= 0)
    {
        CuvsAdmissionFields rej;
        AdmitLine line = {.op = "BUILD", .n = 512, .dim = 32, .gd = 64, .igd = 128};

        pthread_mutex_lock(&g_index_mutex);
        if (requester == CUVS_RQ_BUILD_CAGRA && lru == FOLD_LRU_NONE)
        {
            g_n_gpus = 2;
            g_resident.gpu_device_id = 1;
            g_max_vram_per_gpu[0] = 17 * GiB;
            g_max_vram_per_gpu[1] = RESIDENT + MiB / 2;
            CHECK(pick_gpu_for_index(MiB) == 1);
            CHECK(pick_gpu_for_build(MiB, CUVS_RQ_BUILD_CAGRA) == (ready_exposed ? 1 : 0));
        }
        size_t needed = lru == FOLD_LRU_NONE ? MiB : 1280 * MiB;
        CHECK((ensure_vram(needed, 0, (CuvsRequester) requester, &rej, &line) == 0) == admitted);
        if (lru != FOLD_LRU_NONE)
            CHECK(g_n_indexes == (admitted ? 0 : 1));
        pthread_mutex_unlock(&g_index_mutex);
    }
    else
    {
        build(&build_call, ace ? 256 : 512, algo, log, sizeof(log));
        if (ready_exposed)
            CHECK(strstr(log, "phase=start") == NULL && strstr(log, "[page_fold_reclaim]") == NULL);
        else
        {
            CHECK(strstr(log, "[page_fold_reclaim]") != NULL && strstr(log, "phase=start") != NULL);
            CHECK(strstr(build_call.error, "CPU shim unsupported") != NULL);
        }
    }
    CHECK((parent->fold_candidate != NULL) == !admitted);
    CHECK(g_page_graph_bytes[0] == parent_bytes + (admitted ? 0 : candidate_bytes));
    pthread_mutex_lock(&g_index_mutex);
    drop_page_graphs(event.db_oid, event.index_oid);
    pthread_mutex_unlock(&g_index_mutex);
    release_retired_fold_graphs();
    CHECK(!g_page_graphs && !g_page_graph_bytes[0]);
    g_reclaim.scope = prior_scope;
}

typedef struct ShardCall
{
    int fd;
    CuvsCmdFrame cmd;
} ShardCall;

static void *
plain_build(void *arg)
{
    ShardCall *s = arg;

    g_peer_uid = getuid();
    if (s->cmd.op == CUVS_OP_BUILD_IVFPQ)
        handle_build_ivfpq(s->fd, &s->cmd);
    else
        handle_build(s->fd, &s->cmd);
    close(s->fd);
    return NULL;
}

static void
test_placement_after_yield(uint32_t op, uint32_t partials)
{
    const size_t rows = 64, dim = 32;
    ShardCall s = {.cmd = {.op = op, .db_oid = 2, .index_oid = 8,
        .n_vecs = rows, .dim = dim, .n_partials = partials, .shard_count = 1,
        .graph_degree = 8, .intermediate_graph_degree = 16, .n_lists = 4, .pq_bits = 8}};
    char dir[CUVS_PAGE_INDEX_DIR_SIZE] = {0};
    CuvsPartialDesc desc = {.n_vecs = rows};
    int pair[2];
    CuvsReplyHeader reply;
    pthread_t builder;
    const size_t vec_bytes = rows * dim * sizeof(float);
    const size_t bytes = vec_bytes + rows * sizeof(uint64_t);

    reset(CUVS_HNSW_OVERFLOW_ERROR, 0);
    g_n_gpus = 2;
    g_n_indexes = 0;
    g_max_vram_per_gpu[0] = 16 * GiB;
    g_max_vram_per_gpu[1] = 32 * GiB;
    start_fold_claim(1, 24 * GiB);
    pthread_mutex_lock(&g_index_mutex);
    CHECK(pick_gpu_for_index(MiB) == 0);
    pthread_mutex_unlock(&g_index_mutex);

    snprintf(desc.shm_name, sizeof(desc.shm_name), "/pg_cuvs_bld_placement_%ld", (long) getpid());
    memcpy(s.cmd.shm_key, desc.shm_name, sizeof(desc.shm_name));
    int shm = shm_open(desc.shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
    assert(shm >= 0 && ftruncate(shm, bytes) == 0);
    void *mem = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, shm, 0);
    assert(mem != MAP_FAILED);
    uint64_t *tids = (uint64_t *) ((char *) mem + vec_bytes);
    for (size_t i = 0; i < rows; ++i)
        tids[i] = i + 1;
    munmap(mem, bytes);
    close(shm);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    s.fd = pair[1];
    snprintf(dir, sizeof(dir), "%s", fx_dir);
    assert(cuvs_fd_send(pair[0], -1, dir, sizeof(dir)) == 0);
    if (partials)
        assert(send_all(pair[0], &desc, sizeof(desc)) == 0);
    assert(pthread_create(&builder, NULL, plain_build, &s) == 0);
    wait_for_fold_waiter(1);
    finish_fold_claim(1);
    assert(pthread_join(builder, NULL) == 0);
    CHECK(recv_all(pair[0], &reply, sizeof(reply)) == 0 && reply.status == CUVS_STATUS_OK);
    close(pair[0]);
    shm_unlink(desc.shm_name);
    IndexEntry *entry = find_index(s.cmd.db_oid, s.cmd.index_oid);
    CHECK(entry && entry->gpu_device_id == 1);
    if (entry && entry->ivfpq_handle)
    {
        cuvs_ivfpq_free(entry->ivfpq_handle, entry->gpu_device_id);
        entry->ivfpq_handle = NULL;
    }
    s.cmd.op = CUVS_OP_DROP_INDEX;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    handle_drop(pair[1], &s.cmd);
    close(pair[0]); close(pair[1]);
}

static void *
shard_build(void *arg)
{
    ShardCall *s = arg;
    size_t rows = (size_t)s->cmd.n_vecs;
    size_t vec_bytes = rows * s->cmd.dim * sizeof(float);
    size_t bytes = vec_bytes + rows * sizeof(uint64_t);
    void *mem = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    assert(mem != MAP_FAILED);
    uint64_t *tids = (uint64_t *)((char *)mem + vec_bytes);
    for (size_t i = 0; i < rows; ++i)
        tids[i] = i + 1;
    build_sharded(s->fd, &s->cmd, fx_dir, mem, tids, bytes, mem, 2);
    close(s->fd);
    return NULL;
}

static void
test_shard_fallback_wait(bool cancel)
{
    ShardCall s = {.cmd = {.db_oid = 2, .index_oid = cancel ? 7 : 8, .n_vecs = 64,
                          .dim = 8, .graph_degree = 8, .intermediate_graph_degree = 16}};
    int pair[2];
    pthread_t build_thread;
    CuvsReplyHeader reply;
    char final[512], temporary[576];

    reset(CUVS_HNSW_OVERFLOW_ERROR, 0);
    g_n_indexes = 0;
    g_n_gpus = 3;
    /* The first two devices are the round-robin homes. Only GPU 2 can
     * admit either shard, so it must be included before allocation starts. */
    g_max_vram_per_gpu[0] = g_max_vram_per_gpu[1] = 1;
    g_max_vram_per_gpu[2] = 32 * GiB;
    start_fold_claim(2, GiB);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    s.fd = pair[1];
    assert(pthread_create(&build_thread, NULL, shard_build, &s) == 0);
    wait_for_fold_waiter(2);
    pthread_mutex_lock(&g_index_mutex);
    CHECK(g_n_indexes == 0 && g_pending_build_vram[2] == GiB);
    shard_cagra_path(final, sizeof(final), fx_dir, s.cmd.db_oid, s.cmd.index_oid, 0);
    snprintf(temporary, sizeof(temporary), "%s.tmp", final);
    CHECK(access(temporary, F_OK) != 0 && errno == ENOENT);
    pthread_mutex_unlock(&g_index_mutex);
    if (cancel)
        close(pair[0]);
    else
        finish_fold_claim(2);
    assert(pthread_join(build_thread, NULL) == 0);
    CHECK(!g_fold_waiters[0] && !g_fold_waiters[1] && !g_fold_waiters[2]);
    if (cancel)
    {
        CHECK(g_n_indexes == 0 && g_pending_build_vram[2] == GiB);
        CHECK(access(temporary, F_OK) != 0 && errno == ENOENT);
        CHECK(access(final, F_OK) != 0 && errno == ENOENT);
        finish_fold_claim(2);
        return;
    }
    CHECK(recv_all(pair[0], &reply, sizeof(reply)) == 0 && reply.status == CUVS_STATUS_OK);
    IndexEntry *entry = find_index(s.cmd.db_oid, s.cmd.index_oid);
    CHECK(entry && entry->shard_count == 2 && entry->shards[0].gpu_device_id == 2 &&
          entry->shards[1].gpu_device_id == 2);
    s.cmd.op = CUVS_OP_DROP_INDEX;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    handle_drop(pair[1], &s.cmd);
    close(pair[0]);
    close(pair[1]);
}

static void
test_fold_dead_batch_without_base(void)
{
    CuvsPageOverlayBudget budget = {.limit = 4096};
    CuvsPageOverlayRequest request = {.positions = CUVS_CAGRA_FOLD_BATCH_ROWS};
    CuvsPageOverlayChunk *chunk;
    size_t bytes = request.positions * sizeof(CuvsPagePosition);
    float vectors[CUVS_CAGRA_FOLD_BATCH_ROWS];
    unsigned char live[CUVS_CAGRA_FOLD_BATCH_ROWS] = {0};

    assert(cuvs_page_overlay_alloc(&budget, &request, bytes, &chunk) == CUVS_STATUS_OK);
    memset(chunk->marks, 0, bytes);
    CHECK(page_fold_copy_rows(chunk, 0, request.positions, 1, vectors, live, false) == 0);
    cuvs_page_overlay_release(chunk);
    CHECK(budget.used == 0);
}

static PageCagraGraph *
register_pressure_fold(int device, bool exposed, size_t parent_bytes, size_t child_bytes)
{
    static float rows[5 * 32];
    PageCagraGraph *parent = calloc(1, sizeof(*parent));
    PageGraphFoldRef pin = {0};
    CuvsPageFoldEvent event = {.length = sizeof(event), .db_oid = 4, .index_oid = 5,
        .positions = 1, .system_identifier = 7, .boot_id = 9, .daemon_id = 1,
        .index_nonce = 8, .base_nonce = 6, .parent_graph_nonce = 6,
        .graph_nonce = 12, .parent_n_slots = 4};

    assert(parent && !g_page_graphs && !g_n_page_graphs);
    *parent = (PageCagraGraph) {.db_oid = event.db_oid, .index_oid = event.index_oid,
        .system_identifier = event.system_identifier, .index_nonce = event.index_nonce,
        .base_nonce = event.base_nonce, .graph_nonce = event.parent_graph_nonce,
        .n_slots = 4, .dim = 32, .gpu = device, .bytes = parent_bytes,
        .overlay = {.boot_id = event.boot_id, .covered_upto = 1}};
    parent->handle = cuvs_cagra_build(rows, 4, 32, CUVS_METRIC_L2, 8, 16,
        CUVS_CAGRA_BUILD_AUTO, device, CUVS_LW_OVERFLOW_ERROR, NULL);
    CuvsCagraIndex child = cuvs_cagra_build(rows, 5, 32, CUVS_METRIC_L2, 8, 16,
        CUVS_CAGRA_BUILD_AUTO, device, CUVS_LW_OVERFLOW_ERROR, NULL);
    assert(parent->handle && child);
    pthread_mutex_lock(&g_index_mutex);
    g_page_daemon_id = event.daemon_id;
    g_page_graphs = parent;
    g_n_page_graphs = 1;
    g_page_graph_bytes[device] = parent->bytes;
    assert(page_graph_fold_pin_locked(parent, &pin));
    assert(page_graph_register_fold_locked(&pin, &event, child, child_bytes));
    if (exposed)
    {
        CuvsPageFoldQuery query = {.system_identifier = event.system_identifier, .boot_id = event.boot_id};
        CuvsPageFoldEvent offered;
        assert(page_fold_ready_locked(event.db_oid, event.index_oid, event.index_nonce, &query,
                                      &offered, NULL, NULL));
        assert(offered.graph_nonce == event.graph_nonce);
    }
    pthread_mutex_unlock(&g_index_mutex);
    page_graph_fold_unpin(&pin);
    return parent;
}

static void
drop_pressure_fold(PageCagraGraph *parent)
{
    pthread_mutex_lock(&g_index_mutex);
    drop_page_graphs(parent->db_oid, parent->index_oid);
    pthread_mutex_unlock(&g_index_mutex);
    release_retired_fold_graphs();
}

static void
drop_pressure_index(CuvsCmdFrame cmd)
{
    int pair[2];
    cmd.op = CUVS_OP_DROP_INDEX;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    handle_drop(pair[1], &cmd);
    close(pair[0]); close(pair[1]);
}

static const CuvsCmdFrame pressure_build = {.op = CUVS_OP_BUILD, .db_oid = 2, .index_oid = 31,
    .n_vecs = 4096, .dim = 960, .graph_degree = 64, .intermediate_graph_degree = 128, .shard_count = 1};

static CuvsReplyHeader
build_pressure_request(void)
{
    const CuvsCmdFrame *cmd = &pressure_build;
    size_t vec_bytes = cmd->n_vecs * cmd->dim * sizeof(float);
    uint64_t tids[4096];
    char dir[CUVS_PAGE_INDEX_DIR_SIZE] = {0};
    CuvsReplyHeader reply;
    int pair[2];
    int fd = memfd_create("fold-build-choice", MFD_CLOEXEC);

    assert(fd >= 0 && ftruncate(fd, vec_bytes + sizeof(tids)) == 0);
    for (uint32_t i = 0; i < cmd->n_vecs; ++i)
        tids[i] = i + 1;
    assert(pwrite(fd, tids, sizeof(tids), vec_bytes) == (ssize_t) sizeof(tids));
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    snprintf(dir, sizeof(dir), "%s", fx_dir);
    assert(cuvs_fd_send(pair[0], fd, dir, sizeof(dir)) == 0);
    close(fd);
    handle_build(pair[1], cmd);
    assert(recv_all(pair[0], &reply, sizeof(reply)) == 0);
    close(pair[0]); close(pair[1]);
    return reply;
}

static void
test_build_combined_device(bool pinned, bool exposed)
{
    static IndexEntry registry[3];
    CuvsCmdFrame cmd = pressure_build;
    uint32_t saved_scratch = g_scratch_permille;
    CuvsEvictScope saved_scope = g_reclaim.scope;

    reset(CUVS_HNSW_OVERFLOW_ERROR, 0);
    memset(registry, 0, sizeof(registry));
    registry[0] = (IndexEntry) {.valid = 1, .is_flat = 1, .pinned = 1, .db_oid = 2,
        .index_oid = 32, .gpu_device_id = 0, .main_bf_vram_bytes = 1536 * MiB};
    registry[1] = (IndexEntry) {.valid = 1, .is_flat = 1, .pinned = pinned, .db_oid = 2,
        .index_oid = 33, .gpu_device_id = 1, .main_bf_vram_bytes = 512 * MiB};
    g_indexes = registry;
    g_n_indexes = 2;
    g_n_gpus = 2;
    g_max_vram_per_gpu[0] = g_max_vram_per_gpu[1] = 2048 * MiB;
    assert(!g_rmm_pool_limits[0] && !g_rmm_pool_limits[1]);
    g_reclaim.scope.kind = CUVS_SCOPE_LRU;
    /* A real 16 MiB corpus, with an explicit 79x scratch term to reach the
     * exact admission request without requiring GiB of test allocations. */
    g_scratch_permille = 79000;
    assert(admission_needed_bytes(estimate_vram_bytes(cmd.n_vecs, cmd.dim, cmd.graph_degree)) == 1280 * MiB);
    PageCagraGraph *parent = register_pressure_fold(1, exposed, 512 * MiB, 513 * MiB);
    assert(total_vram_used(0) == 1536 * MiB && total_vram_used(1) == 1537 * MiB);

    CuvsReplyHeader reply = build_pressure_request();

    fprintf(stderr, "BUILD_COMBINED pinned=%d exposed=%d status=%u error=%s err_out=%s\n",
            pinned, exposed, reply.status, reply.error, reply.err_out);
    IndexEntry *built = find_index(cmd.db_oid, cmd.index_oid);
    if (pinned || exposed)
    {
        CHECK(reply.status == CUVS_STATUS_OOM_FALLBACK && !built);
        CHECK(find_index(2, 33) && parent->fold_candidate && total_vram_used(1) == 1537 * MiB);
    }
    else
    {
        CHECK(reply.status == CUVS_STATUS_OK && built && built->gpu_device_id == 1);
        CHECK(!find_index(2, 33) && !parent->fold_candidate && total_vram_used(1) == 528 * MiB);
    }
    CHECK(find_index(2, 32) && total_vram_used(0) == 1536 * MiB);
    drop_pressure_index(cmd);
    drop_pressure_fold(parent);
    g_n_indexes = 0;
    g_scratch_permille = saved_scratch;
    g_reclaim.scope = saved_scope;
}

static void
test_build_rmm_headroom_limit(void)
{
    static IndexEntry registry[4];
    uint32_t saved_scratch = g_scratch_permille;
    uint32_t saved_headroom = g_rmm_build_headroom_permille;
    CuvsEvictScope saved_scope = g_reclaim.scope;
    CuvsAdmissionFields rejection;

    reset(CUVS_HNSW_OVERFLOW_ERROR, 0);
    memset(registry, 0, sizeof(registry));
    registry[0] = (IndexEntry) {.valid = 1, .is_flat = 1, .pinned = 1, .db_oid = 2,
        .index_oid = 32, .gpu_device_id = 0, .main_bf_vram_bytes = 256 * MiB};
    registry[1] = (IndexEntry) {.valid = 1, .is_flat = 1, .db_oid = 2,
        .index_oid = 33, .gpu_device_id = 0, .main_bf_vram_bytes = 1280 * MiB};
    registry[2] = (IndexEntry) {.valid = 1, .is_flat = 1, .db_oid = 2,
        .index_oid = 34, .gpu_device_id = 1, .main_bf_vram_bytes = 256 * MiB};
    g_indexes = registry;
    g_n_indexes = 3;
    g_n_gpus = 2;
    g_max_vram_per_gpu[0] = g_max_vram_per_gpu[1] = 2048 * MiB;
    g_rmm_pool_limits[0] = g_rmm_pool_limits[1] = 2048 * MiB;
    g_reclaim.scope.kind = CUVS_SCOPE_LRU;
    fixture_pressure_pool = true;
    g_scratch_permille = 79000;
    g_rmm_build_headroom_permille = 100;
    size_t needed = admission_needed_bytes(estimate_vram_bytes(pressure_build.n_vecs, pressure_build.dim,
                                                              pressure_build.graph_degree));
    size_t headroom = cuvs_admission_build_headroom(needed, g_rmm_build_headroom_permille);
    assert(needed == 1280 * MiB && headroom == 256 * MiB);
    PageCagraGraph *parent = register_pressure_fold(1, false, 640 * MiB, 641 * MiB);
    assert(total_vram_used(0) == 1536 * MiB && total_vram_used(1) == 1537 * MiB);
    CHECK(total_vram_used(1) + needed - g_max_vram_per_gpu[1] == 769 * MiB);
    CHECK(cuvs_admission_pool_shortfall(g_rmm_pool_limits[1], 0, total_vram_used(1),
                                       needed, headroom) == 1025 * MiB);

    /* Recheck zero new rejections under pressure in gamma. */
    CuvsReplyHeader reply = build_pressure_request();
    fprintf(stderr, "BUILD_RMM_HEADROOM status=%u error=%s err_out=%s\n", reply.status, reply.error, reply.err_out);
    CHECK(reply.status == CUVS_STATUS_OOM_FALLBACK &&
          strcmp(reply.error, "VRAM exhausted on GPU 1 after eviction") == 0);
    CHECK(cuvs_admission_decode(reply.err_out, sizeof(reply.err_out), &rejection) == 0 &&
          rejection.reason == CUVS_ADMISSION_AR8);
    CHECK(g_n_indexes == 3 && !find_index(pressure_build.db_oid, pressure_build.index_oid) &&
          parent->fold_candidate && !parent->fold_candidate->fold_protected_us);
    CHECK(total_vram_used(0) == 1536 * MiB && total_vram_used(1) == 1537 * MiB);

    /* Verify GPU 0 succeeds through actual admission, including eviction. */
    AdmitLine line = {.op = "BUILD", .n = pressure_build.n_vecs, .dim = pressure_build.dim,
                     .gd = pressure_build.graph_degree, .igd = pressure_build.intermediate_graph_degree};
    pthread_mutex_lock(&g_index_mutex);
    int admission = ensure_vram(needed, 0, CUVS_RQ_BUILD_CAGRA, &rejection, &line);
    CHECK(admission == 0 && !find_index(2, 33) && find_index(2, 32));
    CHECK(line.pool_used == 256 * MiB && total_vram_used(0) == 256 * MiB && total_vram_used(1) == 1537 * MiB);
    fprintf(stderr, "BUILD_RMM_HEADROOM_GPU0 admit_rc=%d used_mib=%zu\n", admission,
            total_vram_used(0) / (size_t)MiB);
    pthread_mutex_unlock(&g_index_mutex);
    drop_pressure_index(pressure_build);
    drop_pressure_fold(parent);
    g_n_indexes = 0;
    fixture_pressure_pool = false;
    g_rmm_pool_limits[0] = g_rmm_pool_limits[1] = 0;
    g_scratch_permille = saved_scratch;
    g_rmm_build_headroom_permille = saved_headroom;
    g_reclaim.scope = saved_scope;
}

static void
test_build_rmm_phys_limit(void)
{
    CuvsAdmissionFields rejection;
    CuvsRmmPoolStats pool_before, pool_after;
    uint64_t initial_rejects = g_physical_build_rejects[0];

    reset(CUVS_HNSW_OVERFLOW_ERROR, 0);
    g_n_gpus = 1;
    g_n_indexes = 0;
    g_max_vram_per_gpu[0] = g_rmm_pool_limits[0] = 2048 * MiB;
    fixture_pressure_pool = true;
    fixture_raw_free_override = 16 * MiB;
    PageCagraGraph *parent = register_pressure_fold(0, false, 512 * MiB, 16 * MiB);
    assert(fixture_pool_stats(0, &pool_before) == 0 && pool_before.used_bytes == 528 * MiB);
    assert(CUVS_BUILD_PHYSICAL_FLOOR_BYTES == 32 * MiB);

    /* Recheck zero new rejections under pressure in gamma. The retained
     * backing is modelled here; the real RMM case is test-rmm-fold-backing. */
    CuvsReplyHeader reply = build_pressure_request();
    assert(fixture_pool_stats(0, &pool_after) == 0);
    fprintf(stderr, "BUILD_RMM_PHYS status=%u error=%s err_out=%s used_mib=%llu backing_mib=%llu\n",
            reply.status, reply.error, reply.err_out, (unsigned long long)(pool_after.used_bytes / MiB),
            (unsigned long long)(pool_after.reserved_bytes / MiB));
    CHECK(reply.status == CUVS_STATUS_OOM_FALLBACK &&
          strcmp(reply.error, "GPU physical memory pressure; retry later") == 0);
    CHECK(cuvs_admission_decode(reply.err_out, sizeof(reply.err_out), &rejection) == 0 &&
          rejection.reason == CUVS_ADMISSION_PHYS && rejection.n == 32 && rejection.u == 16);
    CHECK(g_physical_build_rejects[0] == initial_rejects + 1 && g_physical_build_last_free[0] == 16 * MiB);
    CHECK(!find_index(pressure_build.db_oid, pressure_build.index_oid) && !parent->fold_candidate &&
          g_page_graphs == parent && total_vram_used(0) == 512 * MiB);
    CHECK(pool_after.used_bytes == 512 * MiB && pool_after.reserved_bytes == pool_before.reserved_bytes);
    drop_pressure_index(pressure_build);
    drop_pressure_fold(parent);
    fixture_pressure_pool = false;
    fixture_raw_free_override = 0;
    g_rmm_pool_limits[0] = 0;
}

/* The handler exercises its real corpus mapping, budget and TID mutation.
 * The fixture substitutes only graph extension, avoiding a 600 MiB write. */
static char extend_handle;
static unsigned fixture_extensions;
int cuvs_cagra_extend(CuvsCagraIndex, const float *, int64_t, int, uint32_t, int);

int
fixture_extend(CuvsCagraIndex handle, const float *vecs, int64_t rows, int dim, uint32_t chunk, int device)
{
    if (handle != &extend_handle)
        return cuvs_cagra_extend(handle, vecs, rows, dim, chunk, device);
    assert(vecs && rows == 153600 && dim == 960 && device == 0);
    CHECK(total_vram_used(0) == 1024 * MiB && g_page_graphs && !g_page_graphs->fold_candidate);
    ++fixture_extensions;
    return 0;
}

static void *
extend_request(void *arg)
{
    ShardCall *call = arg;
    g_peer_uid = getuid();
    handle_extend(call->fd, &call->cmd);
    close(call->fd);
    return NULL;
}

typedef struct ExtendPressure
{
    ShardCall request;
    CuvsReplyHeader reply;
    int client;
    bool relocate;
} ExtendPressure;

static void
run_waiting_extend(void *arg)
{
    ExtendPressure *call = arg;
    pthread_t worker;

    assert(pthread_create(&worker, NULL, extend_request, &call->request) == 0);
    wait_for_fold_waiter(0);
    pthread_mutex_lock(&g_index_mutex);
    CHECK(g_page_graphs->fold_candidate && !g_page_graphs->fold_candidate->fold_protected_us);
    CHECK(g_fold_active[0] && g_fold_waiters[0] > 0);
    if (call->relocate)
    {
        static IndexEntry reloaded;

        /* Model the placement an UNLOAD/LOAD would leave without running
         * the loader: the actual handler still waits and re-looks up the index. */
        reloaded = g_resident;
        reloaded.gpu_device_id = 1;
        memset(&g_resident, 0, sizeof(g_resident));
        g_indexes = &reloaded;
        CHECK(total_vram_used(1) == 1537 * MiB);
    }
    pthread_mutex_unlock(&g_index_mutex);
    usleep(1000);  /* The real wait must span more than a clock tick. */
    finish_fold_claim(0);
    assert(pthread_join(worker, NULL) == 0);
    assert(recv_all(call->client, &call->reply, sizeof(call->reply)) == 0);
}

static void
test_extend_after_wait(bool exposed, bool relocate)
{
    const size_t old_rows = 131072, new_rows = 153600;
    ExtendPressure call = {.request.cmd = {.op = CUVS_OP_EXTEND, .db_oid = 1, .index_oid = 9,
                                            .dim = 960, .n_vecs = new_rows}, .relocate = relocate};
    size_t vec_bytes = new_rows * call.request.cmd.dim * sizeof(float);
    size_t bytes = vec_bytes + new_rows * sizeof(uint64_t);
    char dir[CUVS_PAGE_INDEX_DIR_SIZE] = {0};
    char log[4096];
    int pair[2];

    reset(CUVS_HNSW_OVERFLOW_ERROR, 0);
    g_n_gpus = relocate ? 2 : 1;
    g_max_vram_per_gpu[0] = 2048 * MiB;
    if (relocate)
        g_max_vram_per_gpu[1] = 2048 * MiB;
    g_resident = (IndexEntry) {.valid = 1, .db_oid = 1, .index_oid = 9, .gpu_device_id = 0,
        .handle = &extend_handle, .n_vecs = old_rows, .dim = 960, .graph_degree = 64, .vram_bytes = 512 * MiB};
    g_resident.tids = malloc(old_rows * sizeof(uint64_t));
    assert(g_resident.tids);
    for (size_t i = 0; i < old_rows; ++i)
        g_resident.tids[i] = i + 1;
    assert(estimate_vram_bytes(old_rows + new_rows, 960, 64) - g_resident.vram_bytes == 600 * MiB);
    PageCagraGraph *parent = register_pressure_fold(relocate ? 1 : 0, exposed, 512 * MiB, 513 * MiB);
    assert(total_vram_used(0) == (relocate ? 512 : 1537) * MiB);
    fixture_extensions = 0;

    snprintf(call.request.cmd.shm_key, sizeof(call.request.cmd.shm_key), "/pg_cuvs_extend_wait_%ld", (long) getpid());
    int fd = shm_open(call.request.cmd.shm_key, O_RDWR | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && ftruncate(fd, bytes) == 0);
    /* Leave the vectors sparse: the storage stub never reads them. */
    uint64_t *tids = malloc(new_rows * sizeof(uint64_t));
    assert(tids);
    for (size_t i = 0; i < new_rows; ++i)
        tids[i] = old_rows + i + 1;
    assert(pwrite(fd, tids, new_rows * sizeof(*tids), vec_bytes) == (ssize_t)(new_rows * sizeof(*tids)));
    free(tids);
    close(fd);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    call.request.fd = pair[1];
    call.client = pair[0];
    snprintf(dir, sizeof(dir), "%s", fx_dir);
    assert(cuvs_fd_send(call.client, -1, dir, sizeof(dir)) == 0);
    if (!exposed)
    {
        start_fold_claim(0, MiB);
        capture(run_waiting_extend, &call, log, sizeof(log));
        const char *line = strstr(log, "[page_fold_admission] op=EXTEND db=1 index=9 gpu=0 wait_us=");
        unsigned long long wait_us = 0;
        int withdrawn = -1;
        CHECK(line && sscanf(line, "[page_fold_admission] op=EXTEND db=1 index=9 gpu=0 wait_us=%llu withdrawn=%d",
                             &wait_us, &withdrawn) == 2 && wait_us > 0 && withdrawn == 0);
        fprintf(stderr, "EXTEND_WAIT wait_us=%llu withdrawn=%d\n", wait_us, withdrawn);
    }
    else
    {
        extend_request(&call.request);
        assert(recv_all(call.client, &call.reply, sizeof(call.reply)) == 0);
    }
    close(call.client);
    shm_unlink(call.request.cmd.shm_key);
    fprintf(stderr, "EXTEND_FOLD exposed=%d status=%u error=%s\n", exposed, call.reply.status, call.reply.error);
    IndexEntry *entry = find_index(call.request.cmd.db_oid, call.request.cmd.index_oid);
    assert(entry);
    if (relocate)
    {
        /* Recheck zero new rejections under pressure in gamma. */
        fprintf(stderr, "EXTEND_RELOCATED status=%u final_gpu=%u used_mib=%zu error=%s\n",
                call.reply.status, entry->gpu_device_id, total_vram_used(1) / (size_t)MiB, call.reply.error);
        CHECK(call.reply.status == CUVS_STATUS_BUILD_FAILED && !fixture_extensions &&
              strcmp(call.reply.error, "not enough VRAM for extend") == 0);
        CHECK(entry != &g_resident && entry->gpu_device_id == 1 && total_vram_used(1) == 1537 * MiB &&
              parent->fold_candidate && !parent->fold_candidate->fold_protected_us);
        CHECK(entry->handle == &extend_handle && entry->n_vecs == (int64_t)old_rows &&
              entry->tids[old_rows - 1] == old_rows);
    }
    else if (exposed)
        CHECK(call.reply.status == CUVS_STATUS_BUILD_FAILED && !fixture_extensions && parent->fold_candidate);
    else
        CHECK(call.reply.status == CUVS_STATUS_OK && fixture_extensions == 1 && !parent->fold_candidate &&
              total_vram_used(0) == 1624 * MiB && g_resident.handle == &extend_handle &&
              g_resident.n_vecs == (int64_t)(old_rows + new_rows) && g_resident.tids[old_rows] == old_rows + 1);
    free(entry->tids);
    free(entry->rev_tids);
    free(entry->rev_item_ids);
    g_n_indexes = 0;
    drop_pressure_fold(parent);
}
#endif

int
main(int argc, char **argv)
{
    char dir[] = "/tmp/hnsw-route-daemon-XXXXXX";
    assert(mkdtemp(dir));
    snprintf(fx_dir, sizeof(fx_dir), "%s", dir);
    snprintf(fx_meminfo, sizeof(fx_meminfo), "%s/meminfo", dir);
    snprintf(fx_cgroup, sizeof(fx_cgroup), "%s/cgroup", dir);
    write_file(fx_meminfo, "MemTotal: 134217728 kB\nMemAvailable: 67108864 kB\n");
    write_file(fx_cgroup, "0::/\n");
#ifdef CUVS_CAGRA_FOLD_ENABLED
    if (argc == 2)
    {
        signal(SIGPIPE, SIG_IGN);
        if (strcmp(argv[1], "build-combined") == 0)
        {
            test_build_combined_device(false, false);
            test_build_combined_device(true, false);
            test_build_combined_device(false, true);
        }
        else if (strcmp(argv[1], "extend-after-wait") == 0)
        {
            test_extend_after_wait(false, false);
            test_extend_after_wait(true, false);
        }
        else if (strcmp(argv[1], "build-rmm-headroom-limit") == 0)
            test_build_rmm_headroom_limit();
        else if (strcmp(argv[1], "extend-relocation-limit") == 0)
            test_extend_after_wait(false, true);
        else if (strcmp(argv[1], "build-rmm-phys-limit") == 0)
            test_build_rmm_phys_limit();
#ifdef CUVS_PAGE_OVERLAY_GPU
        else if (strcmp(argv[1], "gpu-host-retry-vram") == 0)
        {
            test_gpu_host_retry_vram(false);
            test_gpu_host_retry_vram(true);
        }
#endif
        else
            return 2;
        goto done;
    }
#else
    (void) argv;
#endif
    if (argc != 1)
        return 2;
    test_preflight_status13_bytes();
    test_preflight_disk_refusal();
    test_build_status13_bytes();
    test_build_disk_refusal();
#ifdef CUVS_CAGRA_FOLD_ENABLED
    signal(SIGPIPE, SIG_IGN);
    test_fold_preflight_and_readmission();
    test_fold_candidate_pressure(false, CUVS_HNSW_ALGO_AUTO, -1, FOLD_LRU_NONE);
    test_fold_candidate_pressure(true, CUVS_HNSW_ALGO_AUTO, -1, FOLD_LRU_NONE);
    test_fold_candidate_pressure(false, CUVS_HNSW_ALGO_ACE, -1, FOLD_LRU_NONE);
    test_fold_candidate_pressure(false, CUVS_HNSW_ALGO_AUTO, CUVS_RQ_BUILD_CAGRA, FOLD_LRU_NONE);
    test_fold_candidate_pressure(false, CUVS_HNSW_ALGO_AUTO, CUVS_RQ_BUILD_IVFPQ, FOLD_LRU_NONE);
    test_fold_candidate_pressure(false, CUVS_HNSW_ALGO_AUTO, CUVS_RQ_BUILD_SHARD, FOLD_LRU_NONE);
    test_fold_candidate_pressure(true, CUVS_HNSW_ALGO_AUTO, CUVS_RQ_BUILD_CAGRA, FOLD_LRU_NONE);
    test_fold_candidate_pressure(false, CUVS_HNSW_ALGO_AUTO, CUVS_RQ_BUILD_CAGRA, FOLD_LRU_ALLOWED);
    test_fold_candidate_pressure(false, CUVS_HNSW_ALGO_AUTO, CUVS_RQ_BUILD_CAGRA, FOLD_LRU_PINNED);
    test_fold_candidate_pressure(true, CUVS_HNSW_ALGO_AUTO, CUVS_RQ_BUILD_CAGRA, FOLD_LRU_ALLOWED);
    test_placement_after_yield(CUVS_OP_BUILD, 0);
    test_placement_after_yield(CUVS_OP_BUILD, 1);
    test_placement_after_yield(CUVS_OP_BUILD_IVFPQ, 0);
    test_shard_fallback_wait(true);
    test_shard_fallback_wait(false);
    test_fold_dead_batch_without_base();
    test_fold_host_charge();
    test_build_combined_device(false, false);
    test_build_combined_device(true, false);
    test_build_combined_device(false, true);
    test_extend_after_wait(false, false);
    test_extend_after_wait(true, false);
    test_build_rmm_headroom_limit();
    test_extend_after_wait(false, true);
    test_build_rmm_phys_limit();
done:
#endif
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
