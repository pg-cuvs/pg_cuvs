/*
 * cuvs_hnsw_digest.h - chunked SHA-256 of the HNSW vectors spool file (K3).
 *
 * No PostgreSQL dependency: the hash update is a callback, so the backend
 * plugs in pg_cryptohash_update and test/unit/test_hnsw_digest.c plugs in
 * OpenSSL. cuvs_hnsw_digest_step() is the CuvsIpcIdleWork run inside the
 * build-RPC wait: it never ereports, longjmps or checks interrupts.
 */
#ifndef CUVS_HNSW_DIGEST_H
#define CUVS_HNSW_DIGEST_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>

#define CUVS_HNSW_DIGEST_CHUNK (1u << 20)

/* Returns < 0 on failure (pg_cryptohash_update convention). */
typedef int (*CuvsHnswDigestUpdate)(void *ctx, const uint8_t *data, size_t len);

typedef struct CuvsHnswDigest {
    int fd;                    /* raw spool fd; pread only */
    void *ctx;                 /* hash context owned by the caller */
    CuvsHnswDigestUpdate update;
    char *buf;                 /* CUVS_HNSW_DIGEST_CHUNK bytes */
    uint64_t off, total;
    int read_errno;            /* read failure: errno, 0 = short file */
    bool read_failed, hash_failed;
} CuvsHnswDigest;

/* Bytes the scan wrote: n accepted rows of dim float32 values. */
static inline uint64_t
cuvs_hnsw_digest_total(uint64_t n, uint32_t dim)
{
    return n * (uint64_t)dim * sizeof(float);
}

/* One chunk. Returns nonzero while bytes remain and no error occurred. */
static inline int
cuvs_hnsw_digest_step(void *arg)
{
    CuvsHnswDigest *d = (CuvsHnswDigest *)arg;
    size_t want;
    ssize_t got;
    if (d->read_failed || d->hash_failed || d->off >= d->total)
        return 0;
    want = d->total - d->off > CUVS_HNSW_DIGEST_CHUNK
        ? CUVS_HNSW_DIGEST_CHUNK : (size_t)(d->total - d->off);
#ifdef CUVS_HNSW_DIGEST_TEST_DELAY_US
    usleep(CUVS_HNSW_DIGEST_TEST_DELAY_US);
#endif
#ifdef CUVS_HNSW_DIGEST_TEST_FAIL_READ
    got = -1;
    errno = EIO;
#else
    got = pread(d->fd, d->buf, want, (off_t)d->off);
#endif
    if (got < 0 && errno == EINTR)
        return 1;
    if (got <= 0)
    {
        d->read_failed = true;
        d->read_errno = got < 0 ? errno : 0;
        return 0;
    }
    if (d->update(d->ctx, (const uint8_t *)d->buf, (size_t)got) < 0)
    {
        d->hash_failed = true;
        return 0;
    }
    d->off += (uint64_t)got;
    return d->off < d->total;
}

#endif /* CUVS_HNSW_DIGEST_H */
