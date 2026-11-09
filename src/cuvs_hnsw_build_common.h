#ifndef CUVS_HNSW_BUILD_COMMON_H
#define CUVS_HNSW_BUILD_COMMON_H

#include "cuvs_ipc.h" /* CUVS_METRIC_*; PG/CUDA-free */
#include "cuvs_build_corpus.h" /* FD framing used by the shared request consumer */

/* PG/CUDA-free checks shared by backend, IPC and wrapper. These are admission
 * estimates, never a promise that cuVS max_* enforces an RSS ceiling. */
#include <stdint.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <stdio.h>
#ifdef __linux__
#include <sys/vfs.h>
#endif

typedef struct CuvsHnswCapacity {
    uint64_t raw_bytes;
    uint64_t serializer_floor;
    uint64_t host_peak;
    uint64_t gpu_peak;
} CuvsHnswCapacity;

/* Same daemon request consumer in CPU protocol tests. Check the fixed prefix
 * before waiting for the remaining fields/owner channel of a newer payload.
 * The caller retains ownership of any FDs received before an error. */
static inline int
cuvs_hnsw_receive_file_request(int socket_fd, CuvsHnswFileRequest *request,
    int *vectors_fd, int *owner_fd, char *error, size_t error_len)
{
    size_t prefix = offsetof(CuvsHnswFileRequest, build_algo), done = prefix;
    char token;
    memset(request, 0, sizeof(*request));
    *vectors_fd = *owner_fd = -1;
    if (cuvs_fd_recv(socket_fd, request, prefix, vectors_fd))
    {
        snprintf(error, error_len, "invalid HNSW file request prefix");
        return CUVS_STATUS_BUILD_FAILED;
    }
    if (!cuvs_hnsw_file_version_supported(request->version))
    {
        snprintf(error, error_len, "HNSW file payload version mismatch: expected v%u; co-deploy extension and daemon", CUVS_HNSW_FILE_VERSION);
        return CUVS_STATUS_PROTO_MISMATCH;
    }
    while (done < sizeof(*request))
    {
        ssize_t n = recv(socket_fd, (char *)request + done, sizeof(*request) - done, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0)
        {
            snprintf(error, error_len, "incomplete HNSW file request fields");
            return CUVS_STATUS_BUILD_FAILED;
        }
        done += (size_t)n;
    }
    if (cuvs_fd_recv(socket_fd, &token, 1, owner_fd)
        || !cuvs_hnsw_file_request_supported(request))
    {
        snprintf(error, error_len, "invalid HNSW file request settings/owner frame");
        return CUVS_STATUS_BUILD_FAILED;
    }
    return CUVS_STATUS_OK;
}

/* Exact pgvector families. In particular, halfvec cosine/IP must not silently
 * take the legacy unrecognized-family L2 fallback during a source-less build. */
static inline int
cuvs_hnsw_metric_from_family(const char *name)
{
    if (!name) return -1;
    if (!strcmp(name, "vector_l2_ops") || !strcmp(name, "halfvec_l2_ops"))
        return CUVS_METRIC_L2;
    if (!strcmp(name, "vector_cosine_ops") || !strcmp(name, "halfvec_cosine_ops"))
        return CUVS_METRIC_COSINE;
    if (!strcmp(name, "vector_ip_ops") || !strcmp(name, "halfvec_ip_ops"))
        return CUVS_METRIC_IP;
    return -1;
}

static inline uint64_t
cuvs_hnsw_monotonic_us(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

static inline int
cuvs_hnsw_regular_disk(int fd)
{
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) return 0;
#ifdef __linux__
    struct statfs fs;
    if (fstatfs(fd, &fs) || fs.f_type == 0x01021994 /* tmpfs */
        || fs.f_type == 0x858458f6 /* ramfs */) return 0;
#endif
    return 1;
}

/* Peeking preserves ACK/cancel bytes for the job owner. Normal request EOF is
 * never consulted: this is the separate owner socket, held through handoff. */
static inline int
cuvs_hnsw_owner_cancelled(int fd)
{
    char c;
    ssize_t n = recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n > 0) return c != 'A';
    if (n == 0) return 1;
    return errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR;
}

/* busy() runs under the registry lock. Return with the lock held only after
 * every previously admitted unlocked GPU scope has drained. A completing
 * query/build must be able to reacquire the lock while we wait. */
static inline int
cuvs_hnsw_wait_for_idle(pthread_mutex_t *mutex, int owner_fd,
                        int (*busy)(void *), void *arg)
{
    for (;;)
    {
        int rc;
        if (cuvs_hnsw_owner_cancelled(owner_fd)) return -1;
        rc = pthread_mutex_trylock(mutex);
        if (rc == 0)
        {
            if (!busy(arg) && !cuvs_hnsw_owner_cancelled(owner_fd)) return 0;
            pthread_mutex_unlock(mutex);
        }
        else if (rc != EBUSY) return -1;
        struct pollfd owner;
        owner.fd = owner_fd; owner.events = POLLIN; owner.revents = 0;
        if (poll(&owner, 1, 100) < 0 && errno != EINTR) return -1;
    }
}

static inline int
cuvs_hnsw_capacity(uint64_t n, uint32_t dim, uint32_t degree,
                   uint32_t intermediate, int hierarchy, CuvsHnswCapacity *out)
{
    uint64_t row, host_row, gpu_row;
    if (!n || n > INT32_MAX || !dim || dim > 65535 || degree < 2
        || degree > 1024 || (degree & 1) || intermediate < degree
        || intermediate > 4096) return -1;
    row = (uint64_t)dim * 4;
    /* Host: file pages, IVF training copies, intermediate/optimized graphs,
     * hnswlib records/locks/maps, and hierarchy scratch. Deliberately count
     * input pages even though their mapping is file-backed and reclaimable. */
    host_row = row * (hierarchy ? 6 : 4)
        + (uint64_t)(intermediate + degree * 3) * 8 + 256;
    /* Compressed IVF-PQ index, IDs, search/refine batches, graph + optimizer.
     * Reserve bounded training/workspace separately; no raw N*D GPU upload. */
    gpu_row = (dim + 1) / 2 + 16 + (uint64_t)(intermediate + degree) * 8;
    if (n > (UINT64_MAX - (UINT64_C(512) << 20)) / host_row
        || n > (UINT64_MAX - (UINT64_C(512) << 20)) / gpu_row) return -1;
    out->raw_bytes = n * row;
    out->serializer_floor = n * 20 + (UINT64_C(65) << 20);
    out->host_peak = n * host_row + (UINT64_C(512) << 20);
    out->gpu_peak = n * gpu_row + (UINT64_C(512) << 20);
    return 0;
}

/* Reserve the shape consumed by the SDK's integer sampling, rather than the
 * requested fractional population. Kept shared with direct CPU consumers. */
static inline int
cuvs_hnsw_training_reserve(CuvsHnswCapacity *cap, uint64_t actual_rows, uint32_t dim)
{
    uint64_t bytes;
    if (!dim || actual_rows > UINT64_MAX / dim / sizeof(float)) return -1;
    bytes = actual_rows * dim * sizeof(float);
    if (bytes > (UINT64_MAX - cap->host_peak) / 2
        || bytes > (UINT64_MAX - cap->gpu_peak) / 4) return -1;
    cap->host_peak += bytes * 2;
    cap->gpu_peak += bytes * 4;
    return 0;
}

static inline uint64_t
cuvs_hnsw_sample_rows(uint64_t n, uint32_t dim, uint64_t host, uint64_t gpu)
{
    if (!n || !dim) return 0;
    uint64_t rows = (host < gpu ? host : gpu) / 64 / ((uint64_t)dim * sizeof(float));
    if (rows < 256) rows = 256;
    return n < rows ? n : rows;
}

/* Conservative candidate only. The patched SDK must recheck the ACTUAL
 * histogram, any internal P increase, live reorder buffers and serializer
 * upper-level workspace before allocation. A zero result admits no build. */
static inline uint32_t
cuvs_hnsw_ace_partitions(uint64_t n, uint32_t dim, uint32_t degree,
                         uint32_t intermediate, uint64_t host, uint64_t gpu)
{
    CuvsHnswCapacity full;
    if (cuvs_hnsw_capacity(n, dim, degree, intermediate, 0, &full)
        || full.serializer_floor >= host) return 0;
    for (uint32_t p = 2; p <= 65536 && p <= n / 128; p *= 2)
    {
        CuvsHnswCapacity partition;
        uint64_t rows = (n / p + (n % p != 0)) * 6; /* core+augmented, 3x skew */
        uint64_t row_bytes = (uint64_t)dim * 4;
        uint64_t buffer_bytes = host / 4 / p;
        if (buffer_bytes > (UINT64_C(64) << 20)) buffer_bytes = UINT64_C(64) << 20;
        uint64_t buffer_rows = buffer_bytes / row_bytes;
        if (buffer_rows < 64) buffer_rows = 64;
        uint64_t reorder = 2 * (uint64_t)p * buffer_rows * row_bytes;
        if (cuvs_hnsw_capacity(rows, dim, degree, intermediate, 0, &partition)) continue;
        if (reorder < host && full.serializer_floor < host - reorder
            && partition.host_peak < host - reorder - full.serializer_floor
            && partition.gpu_peak < gpu)
            return p;
    }
    return 0;
}

/* Exact SDK LP64 hnswlib header, checked only for the <=15-row GPU graph
 * route. cuVS writes the padded graph width as each base link count. Remove
 * self/duplicate padding so delegated pgvector INSERT sees valid neighbors.
 * No vector payload or whole index is reconstructed. */
static inline int
cuvs_hnsw_fix_tiny_counts(int fd, uint64_t expected)
{
    uint64_t header[12];
    struct stat st;
    if (!expected || expected > 15 || sizeof(size_t) != 8
        || pread(fd, header, sizeof(header), 0) != sizeof(header)
        || header[2] != expected || header[3] > (UINT64_C(1) << 20)
        || header[8] < 4 || header[8] > 16 || header[9] < 2 || header[9] > 8
        || header[8] != header[9] * 2
        || header[5] != 4 + header[8] * 4 || header[4] < header[5]
        || header[4] > header[3] || header[3] - header[4] != 8
        || fstat(fd, &st) || st.st_size < 0
        || (uint64_t)st.st_size < 96 + expected * header[3]) return -1;
    for (uint64_t row = 0; row < expected; row++)
    {
        uint32_t links[16], count, kept = 0;
        uint64_t label;
        off_t offset = (off_t)(96 + row * header[3]);
        if (pread(fd, &count, sizeof(count), offset) != sizeof(count) || count > header[8]
            || pread(fd, links, (size_t)header[8] * 4, offset + 4) != (ssize_t)header[8] * 4
            || pread(fd, &label, 8, offset + (off_t)header[4]) != 8 || label != row) return -1;
        for (uint32_t j = 0; j < count; j++)
        {
            uint32_t id = links[j], k;
            if (id >= expected) return -1;
            if (id == row) continue;
            for (k = 0; k < kept && links[k] != id; k++) {}
            if (k == kept) links[kept++] = id;
        }
        if (kept != expected - 1) return -1;
        for (uint32_t j = kept; j < header[8]; j++) links[j] = 0;
        if (pwrite(fd, &kept, 4, offset) != 4
            || pwrite(fd, links, (size_t)header[8] * 4, offset + 4) != (ssize_t)header[8] * 4) return -1;
    }
    return 0;
}

/* Disk precheck formula kept as-is (UC-5 sub-decision 1(a)): the bound for
 * input copy + reorder + graph + serialization, all routes. */
static inline uint64_t cuvs_hnsw_disk_need(uint64_t n, uint32_t dim, uint32_t degree)
{
    uint64_t raw = n * dim * sizeof(float);
    return raw * 4 + n * (degree * 8ULL + 64);
}

typedef struct CuvsHnswRouteDecision {
    uint32_t route;
    uint32_t partitions; /* candidate only; SDK fixed-point plan records effective P */
    uint32_t overflowed; /* 1 only via the auto -> ACE overflow branch */
    uint32_t pad;
    uint64_t sdk_host_budget;
    uint64_t sdk_gpu_budget;
} CuvsHnswRouteDecision;

/* Canonical preflight no-fit reason. Shared by producer and backend so
 * generic BUILD_FAILED estimates never imply a configured-budget refusal.
 * The existing wire status/layout and diagnostic text remain unchanged. */
#define CUVS_HNSW_BUDGET_NO_FIT_REASON \
    "no ACE partition count fits metadata/reorder/serializer and GPU budgets"

/* Settings/training reserve must already have been prepared using the SAME
 * budgets. This decision allocates nothing and never silently changes builder. */
static inline int
cuvs_hnsw_resolve_route(uint64_t n, uint32_t dim, uint32_t degree,
    uint32_t intermediate, uint32_t algo, uint32_t overflow,
    const CuvsHnswCapacity *cap,
    uint64_t host, uint64_t gpu, CuvsHnswRouteDecision *out,
    char *error, size_t error_len)
{
    CuvsHnswCapacity base;
    memset(out, 0, sizeof(*out));
    if (!cap || !n || !host || !gpu || !cuvs_hnsw_algo_supported(algo)
        || (overflow != CUVS_HNSW_OVERFLOW_ACE && overflow != CUVS_HNSW_OVERFLOW_ERROR))
    {
        snprintf(error, error_len, "unsupported HNSW route or unknown capacity budget");
        return CUVS_STATUS_BUILD_FAILED;
    }
    if (cuvs_hnsw_capacity(n, dim, degree, intermediate, 0, &base)
        || cap->raw_bytes != base.raw_bytes || cap->serializer_floor != base.serializer_floor
        || cap->host_peak < base.host_peak || cap->gpu_peak < base.gpu_peak)
    {
        snprintf(error, error_len, "invalid prepared HNSW shape or capacity");
        return CUVS_STATUS_BUILD_FAILED;
    }
    int tiny = n < CUVS_HNSW_MIN_ELEMENTS;
    int exceeded = cap->host_peak > host || cap->gpu_peak > gpu;
    if (tiny && algo != CUVS_HNSW_ALGO_AUTO)
    {
        snprintf(error, error_len, "explicit %s requires non-tiny input; use build_algo=auto for tiny exact",
                 cuvs_hnsw_algo_name(algo));
        return CUVS_STATUS_BUILD_FAILED;
    }
    if (tiny && exceeded)
    {
        snprintf(error, error_len, "tiny exact route estimate exceeds host/GPU budget");
        return CUVS_STATUS_BUILD_FAILED;
    }
    /* ADR-087: the ONLY auto -> ACE transition. Explicit ivf_pq never
     * overflows; auto overflows only when the daemon flag says ace. */
    int overflow_to_ace = !tiny && exceeded && algo == CUVS_HNSW_ALGO_AUTO
        && overflow == CUVS_HNSW_OVERFLOW_ACE;
    if (overflow_to_ace && n < 256)
    {
        snprintf(error, error_len, "resident IVF-PQ estimate exceeds host/GPU budget and ACE needs at least 256 rows");
        return CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED;
    }
    if (!tiny && algo != CUVS_HNSW_ALGO_ACE && exceeded && !overflow_to_ace)
    {
        snprintf(error, error_len, algo == CUVS_HNSW_ALGO_AUTO
            ? "resident IVF-PQ estimate exceeds host/GPU budget; --hnsw-auto-overflow=error; use build_algo=ace explicitly"
            : "resident IVF-PQ estimate exceeds host/GPU budget; no fallback; use build_algo=ace explicitly");
        return CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED;
    }
    out->sdk_host_budget = host - (host / 8 < (UINT64_C(256) << 20) ? host / 8 : (UINT64_C(256) << 20));
    out->sdk_gpu_budget = gpu - (gpu / 8 < (UINT64_C(512) << 20) ? gpu / 8 : (UINT64_C(512) << 20));
    if (algo == CUVS_HNSW_ALGO_ACE || overflow_to_ace)
    {
        if (n < 256)
        {
            snprintf(error, error_len, "explicit ace needs at least 256 rows for a partition candidate; no resident fallback");
            return CUVS_STATUS_BUILD_FAILED;
        }
        out->partitions = cuvs_hnsw_ace_partitions(n, dim, degree, intermediate,
                                                  out->sdk_host_budget, out->sdk_gpu_budget);
        if (!out->partitions)
        {
            snprintf(error, error_len, "%s", CUVS_HNSW_BUDGET_NO_FIT_REASON);
            return CUVS_STATUS_BUILD_FAILED;
        }
        out->route = CUVS_HNSW_ROUTE_ACE;
        out->overflowed = (uint32_t)overflow_to_ace;
    }
    else out->route = tiny ? CUVS_HNSW_ROUTE_TINY : CUVS_HNSW_ROUTE_IVF_PQ;
    return CUVS_STATUS_OK;
}

/* Shared route preparation (UC-5: I5-j/I5-l moved into I3-C). Reproduces
 * the wrapper's estimate preparation (degree clamps, scaled intermediate,
 * training reserve) from (n, dim) alone and then calls the SAME resolver,
 * so neither the pre-spool forecast nor the daemon's pre-mkdtemp decision
 * can take a second auto -> ACE branch. n_lists only bounds the training
 * population from below and the wrapper's n_lists never exceeds
 * sample_rows/32 < sample_rows, so the training rows (hence the estimate)
 * do not depend on the SDK default n_lists; test_hnsw_auto_overflow pins
 * that. disk_need uses the requested graph_degree like the daemon check. */
typedef struct CuvsHnswForecast {
    uint32_t status;        /* CUVS_STATUS_* from the resolver or disk check */
    uint32_t disk_ok;       /* 0 until cuvs_hnsw_forecast_disk() passes */
    CuvsHnswRouteDecision decision;
    uint64_t n, host_estimate, gpu_estimate, host_budget, gpu_budget;
    uint64_t disk_need, disk_avail;
} CuvsHnswForecast;

static inline int
cuvs_hnsw_forecast_route(uint64_t n, uint32_t dim, uint32_t graph_degree,
    uint32_t intermediate_degree, uint32_t hierarchy, uint32_t algo,
    uint32_t ef_construction, uint32_t overflow, uint64_t host, uint64_t gpu,
    CuvsHnswForecast *out, char *error, size_t error_len)
{
    CuvsHnswCapacity cap;
    uint32_t degree = graph_degree, intermediate;
    memset(out, 0, sizeof(*out));
    out->n = n; out->host_budget = host; out->gpu_budget = gpu;
    out->status = CUVS_STATUS_BUILD_FAILED;
    if (!n || !dim || ef_construction < 4 || ef_construction > 1000)
    {
        snprintf(error, error_len, "HNSW route decision needs positive row count, dimension and ef_construction");
        return (int)out->status;
    }
    intermediate = cuvs_hnsw_scaled_quality(intermediate_degree, ef_construction, degree, 4096);
    if (n < CUVS_HNSW_MIN_ELEMENTS)
    {
        degree = (uint32_t)(n & ~UINT64_C(1));
        if (degree < 4) degree = 4;
        intermediate = degree;
    }
    else
    {
        if (degree >= n) degree = (uint32_t)((n - 1) & ~UINT64_C(1));
        if (intermediate >= n) intermediate = (uint32_t)(n - 1);
    }
    out->disk_need = cuvs_hnsw_disk_need(n, dim, graph_degree);
    if (cuvs_hnsw_capacity(n, dim, degree, intermediate, hierarchy != 0, &cap))
    {
        snprintf(error, error_len, "invalid HNSW capacity/dimension/degree");
        return (int)out->status;
    }
    if (n >= CUVS_HNSW_MIN_ELEMENTS)
    {
        uint64_t sample = cuvs_hnsw_sample_rows(n, dim, host, gpu);
        uint64_t lists = sample / 32 ? sample / 32 : 1;
        if (lists > UINT32_MAX) lists = UINT32_MAX;
        if (cuvs_hnsw_training_reserve(&cap,
                cuvs_hnsw_training_rows(n, sample, (uint32_t)lists), dim))
        {
            snprintf(error, error_len, "HNSW training estimate overflow");
            return (int)out->status;
        }
    }
    out->host_estimate = cap.host_peak;
    out->gpu_estimate = cap.gpu_peak;
    out->status = (uint32_t)cuvs_hnsw_resolve_route(n, dim, degree, intermediate,
        algo, overflow, &cap, host, gpu, &out->decision, error, error_len);
    return (int)out->status;
}

/* Disk precheck for a decided route. The pre-spool forecast calls it on the
 * daemon's index directory so an insufficient disk fails BEFORE the backend
 * spools (JI-21); the daemon repeats the same check on the private workdir.
 * Refusal text (v5 C5, v6 C2 "hnsw 전 route 공통"):
 *   HNSW needs regular disk capacity: need=<u> avail=<u> fs=<tmpfs|regular> route=<s>
 * need/avail are bytes; fs tells a tmpfs refusal (avail may be >= need) from
 * a capacity refusal. The backend matches the prefix to attach its HINT. */
#define CUVS_HNSW_DISK_REFUSAL_PREFIX "HNSW needs regular disk capacity: "
static inline int
cuvs_hnsw_forecast_disk(CuvsHnswForecast *f, int disk_regular,
    uint64_t disk_avail, char *error, size_t error_len)
{
    f->disk_avail = disk_avail;
    f->disk_ok = f->status == CUVS_STATUS_OK && disk_regular && disk_avail >= f->disk_need;
    if (f->status == CUVS_STATUS_OK && !f->disk_ok)
    {
        snprintf(error, error_len, CUVS_HNSW_DISK_REFUSAL_PREFIX "need=%llu avail=%llu fs=%s route=%s",
                 (unsigned long long)f->disk_need, (unsigned long long)disk_avail,
                 disk_regular ? "regular" : "tmpfs", cuvs_hnsw_route_name(f->decision.route));
        f->status = CUVS_STATUS_BUILD_FAILED;
    }
    return (int)f->status;
}

/* The route field of [hnsw_route] (v5 Q4-4; v6 [hnsw_route] schema): the
 * route the resolver decided, also when the disk precheck refused it after
 * (status 5 with route=ace_disk); "none" only when no route was decided
 * (status 13, no ACE partition fits, invalid input). The resolver sets
 * decision.route only on success, so it alone says whether one was. */
static inline const char *
cuvs_hnsw_forecast_route_name(const CuvsHnswForecast *f)
{
    return f->decision.route ? cuvs_hnsw_route_name(f->decision.route) : "none";
}

#endif
