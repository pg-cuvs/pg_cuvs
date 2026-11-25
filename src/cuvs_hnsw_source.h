/*
 * Content-bound identity for a legacy CAGRA HNSW sidecar and its TID file.
 * Both callers hash their open descriptors. Link and rename bookkeeping
 * must not invalidate a record for an unchanged pair.
 */
#ifndef CUVS_HNSW_SOURCE_H
#define CUVS_HNSW_SOURCE_H

#include <errno.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Binding created with an optional on-disk HNSW export. The record
 * binds each file's identity and SHA-256. A zero magic is durably written
 * before the first in-memory mutation of that generation. */
#define CUVS_HNSW_SOURCE_MAGIC 0x53574e48u
#define CUVS_HNSW_SOURCE_VERSION 2u
#define CUVS_HNSW_SOURCE_SUFFIX ".source"

typedef struct CuvsHnswFileIdentity {
    uint64_t device;
    uint64_t inode;
    uint64_t bytes;
    unsigned char sha256[32];
} CuvsHnswFileIdentity;

typedef struct CuvsHnswSourceBinding {
    uint32_t magic;
    uint32_t version;
    CuvsHnswFileIdentity graph;
    CuvsHnswFileIdentity tids;
} CuvsHnswSourceBinding;

#ifdef __APPLE__
#define SOURCE_MTIME st_mtimespec
#define SOURCE_CTIME st_ctimespec
#else
#define SOURCE_MTIME st_mtim
#define SOURCE_CTIME st_ctim
#endif

#define SOURCE_HASH_CHUNK_BYTES (64 * 1024)

static inline bool
source_file_unchanged(int fd, const struct stat *before)
{
    struct stat after;

    return fstat(fd, &after) == 0 &&
           before->st_size == after.st_size &&
           before->SOURCE_MTIME.tv_sec == after.SOURCE_MTIME.tv_sec &&
           before->SOURCE_MTIME.tv_nsec == after.SOURCE_MTIME.tv_nsec &&
           before->SOURCE_CTIME.tv_sec == after.SOURCE_CTIME.tv_sec &&
           before->SOURCE_CTIME.tv_nsec == after.SOURCE_CTIME.tv_nsec;
}

static inline int
hash_file_identity(int fd, CuvsHnswFileIdentity *out,
                   const CuvsHnswFileIdentity *expected,
                   struct stat *verified, bool (*cancel_pending)(void))
{
    struct stat before;
    EVP_MD_CTX *hash;
    unsigned char chunk[SOURCE_HASH_CHUNK_BYTES];
    unsigned int length = 0;
    off_t offset = 0;
    int result = -1;

    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 0)
        return -1;
    out->device = (uint64_t) before.st_dev;
    out->inode = (uint64_t) before.st_ino;
    out->bytes = (uint64_t) before.st_size;
    if (expected && (out->device != expected->device ||
                     out->inode != expected->inode ||
                     out->bytes != expected->bytes))
        return -1;

    hash = EVP_MD_CTX_new();
    if (!hash)
        return -1;
    if (EVP_DigestInit_ex(hash, EVP_sha256(), NULL) != 1)
        goto done;
    while (offset < before.st_size)
    {
        off_t remaining = before.st_size - offset;
        size_t request = remaining < (off_t) sizeof(chunk) ?
            (size_t) remaining : sizeof(chunk);
        ssize_t n;

        if (cancel_pending && cancel_pending())
            goto done;
        n = pread(fd, chunk, request, offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0 || EVP_DigestUpdate(hash, chunk, (size_t) n) != 1)
            goto done;
        offset += n;
    }

    /* Only require stable timestamps during this read. The persisted
     * identity does not contain either timestamp. */
    if (!source_file_unchanged(fd, &before))
        goto done;
    if (EVP_DigestFinal_ex(hash, out->sha256, &length) == 1 &&
        length == sizeof(out->sha256))
    {
        if (verified)
            *verified = before;
        result = 0;
    }
done:
    EVP_MD_CTX_free(hash);
    return result;
}

static inline int
cuvs_hnsw_file_identity(int fd, CuvsHnswFileIdentity *out)
{
    return hash_file_identity(fd, out, NULL, NULL, NULL);
}

static inline bool
read_binding_record(int fd, CuvsHnswSourceBinding *out)
{
    struct stat st;
    ssize_t n;

    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size != (off_t) sizeof(*out))
        return false;
    do {
        n = pread(fd, out, sizeof(*out), 0);
    } while (n < 0 && errno == EINTR);

    return n == (ssize_t) sizeof(*out) &&
           out->magic == CUVS_HNSW_SOURCE_MAGIC &&
           out->version == CUVS_HNSW_SOURCE_VERSION;
}

static inline bool
cuvs_hnsw_source_binding_read(FILE *binding_file, int graph_fd, int tids_fd,
                             CuvsHnswSourceBinding *out,
                             bool (*cancel_pending)(void))
{
    CuvsHnswSourceBinding current;
    CuvsHnswFileIdentity graph, tids;
    struct stat verified_graph, verified_tids;
    int binding_fd = fileno(binding_file);

    if (!read_binding_record(binding_fd, out) ||
        hash_file_identity(graph_fd, &graph, &out->graph, &verified_graph, cancel_pending) != 0 ||
        memcmp(graph.sha256, out->graph.sha256, sizeof(graph.sha256)) != 0 ||
        hash_file_identity(tids_fd, &tids, &out->tids, &verified_tids, cancel_pending) != 0 ||
        memcmp(tids.sha256, out->tids.sha256, sizeof(tids.sha256)) != 0 ||
        (cancel_pending && cancel_pending()))
        return false;

    /* EXTEND and COMPACT clear the binding magic in place before mutating
     * the resident source. Observe any invalidation during the file reads. */
    return source_file_unchanged(graph_fd, &verified_graph) &&
           source_file_unchanged(tids_fd, &verified_tids) &&
           read_binding_record(binding_fd, &current) &&
           memcmp(&current, out, sizeof(current)) == 0;
}

#undef SOURCE_MTIME
#undef SOURCE_CTIME
#undef SOURCE_HASH_CHUNK_BYTES

#endif /* CUVS_HNSW_SOURCE_H */
