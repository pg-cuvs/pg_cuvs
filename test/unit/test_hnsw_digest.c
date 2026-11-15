/*
 * test_hnsw_digest.c - src/cuvs_hnsw_digest.h (K3 spool digest), no PG.
 *
 * Rows are written to a real temp file at n*rowbytes like the HNSW spool
 * callback. The step loop, driven with cuvs_hnsw_digest_total(n, dim) as
 * the backend does, must give the SHA-256 of the rows computed independently
 * in memory, row by row (the synchronous digest). Also: chunk boundaries,
 * short file, bad fd, update failure, idempotent end.
 */
#include "cuvs_hnsw_digest.h"
#include <assert.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail_update;
static int evp_update(void *ctx, const uint8_t *data, size_t len)
{
    if (fail_update) return -1;
    return EVP_DigestUpdate((EVP_MD_CTX *)ctx, data, len) ? 0 : -1;
}

static int tmpfd(void)
{
    char path[] = "/tmp/k3-digest-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    unlink(path);
    return fd;
}

/* Writes n rows of dim floats; returns the synchronous per-row digest. */
static void write_rows(int fd, uint64_t n, uint32_t dim, unsigned char ref[32])
{
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    float *row = malloc((size_t)dim * sizeof(float));
    unsigned len = 0;
    uint32_t s = (uint32_t)(n * 2654435761u + dim);
    EVP_DigestInit_ex(c, EVP_sha256(), NULL);
    for (uint64_t i = 0; i < n; i++)
    {
        for (uint32_t j = 0; j < dim; j++)
        {
            s = s * 1103515245u + 12345u;
            row[j] = (float)((int32_t)(s >> 8) % 2001 - 1000) / 256.0f;
        }
        assert(pwrite(fd, row, (size_t)dim * sizeof(float), (off_t)(i * dim * sizeof(float)))
               == (ssize_t)(dim * sizeof(float)));
        EVP_DigestUpdate(c, row, (size_t)dim * sizeof(float));
    }
    EVP_DigestFinal_ex(c, ref, &len);
    EVP_MD_CTX_free(c);
    free(row);
}

static CuvsHnswDigest run(int fd, uint64_t n, uint32_t dim, unsigned char out[32], int *steps)
{
    static char buf[CUVS_HNSW_DIGEST_CHUNK];
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    unsigned len = 0;
    CuvsHnswDigest d;
    memset(&d, 0, sizeof(d));
    EVP_DigestInit_ex(c, EVP_sha256(), NULL);
    d.fd = fd; d.ctx = c; d.update = evp_update; d.buf = buf;
    d.total = cuvs_hnsw_digest_total(n, dim);
    *steps = 0;
    while (cuvs_hnsw_digest_step(&d)) (*steps)++;
    assert(cuvs_hnsw_digest_step(&d) == 0); /* idempotent at the end */
    EVP_DigestFinal_ex(c, out, &len);
    EVP_MD_CTX_free(c);
    return d;
}

int main(void)
{
    const uint64_t C = CUVS_HNSW_DIGEST_CHUNK / 4; /* dim-1 rows per chunk */
    struct { uint64_t n; uint32_t dim; } cases[] = {
        {1, 1}, {1, 3}, {C - 1, 1}, {C, 1}, {C + 1, 1}, {2 * C + 1, 1},
        {3000, 768}, {20000, 17}, {700, 2000},
    };
    int checked = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        unsigned char ref[32], got[32];
        int steps, fd = tmpfd();
        write_rows(fd, cases[i].n, cases[i].dim, ref);
        CuvsHnswDigest d = run(fd, cases[i].n, cases[i].dim, got, &steps);
        assert(!d.read_failed && !d.hash_failed);
        assert(d.off == (uint64_t)cases[i].n * cases[i].dim * 4);
        if (memcmp(ref, got, 32))
        {
            printf("FAIL digest mismatch n=%llu dim=%u\n", (unsigned long long)cases[i].n, cases[i].dim);
            return 1;
        }
        close(fd);
        checked++;
    }
    {   /* file shorter than the written length: read failure, errno 0 */
        unsigned char ref[32], got[32];
        int steps, fd = tmpfd();
        write_rows(fd, 1000, 8, ref);
        assert(ftruncate(fd, 1000) == 0);
        CuvsHnswDigest d = run(fd, 1000, 8, got, &steps);
        assert(d.read_failed && d.read_errno == 0 && !d.hash_failed);
        close(fd);
    }
    {   /* closed fd: read failure with errno */
        unsigned char got[32];
        int steps, fd = tmpfd();
        close(fd);
        CuvsHnswDigest d = run(fd, 10, 8, got, &steps);
        assert(d.read_failed && d.read_errno == EBADF);
    }
    {   /* update failure stops the walk */
        unsigned char ref[32], got[32];
        int steps, fd = tmpfd();
        write_rows(fd, 10, 8, ref);
        fail_update = 1;
        CuvsHnswDigest d = run(fd, 10, 8, got, &steps);
        fail_update = 0;
        assert(d.hash_failed && !d.read_failed && d.off == 0);
        close(fd);
    }
    {   /* zero rows: nothing to do */
        unsigned char got[32];
        int steps;
        CuvsHnswDigest d = run(-1, 0, 8, got, &steps);
        assert(steps == 0 && !d.read_failed && d.off == 0);
    }
    printf("PASS hnsw digest: %d size cases equal to the per-row digest; short file, bad fd, update failure, empty\n", checked);
    return 0;
}
