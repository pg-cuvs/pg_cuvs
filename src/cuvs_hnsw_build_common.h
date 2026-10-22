#ifndef CUVS_HNSW_BUILD_COMMON_H
#define CUVS_HNSW_BUILD_COMMON_H

#include "cuvs_ipc.h" /* CUVS_METRIC_*; PG/CUDA-free */

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
#ifdef __linux__
#include <sys/vfs.h>
#endif

typedef struct CuvsHnswCapacity {
    uint64_t raw_bytes;
    uint64_t serializer_floor;
    uint64_t host_peak;
    uint64_t gpu_peak;
} CuvsHnswCapacity;

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

#endif
