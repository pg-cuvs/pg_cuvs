#ifndef CUVS_BF_VERDICT_H
#define CUVS_BF_VERDICT_H

/* #238: remembered "brute_force unavailable" verdict for the unsharded BF
 * cache, PostgreSQL-free so the CPU unit test runs it.
 *
 * refresh_main_bf_cache reads the whole `.vectors` sidecar and checks its body
 * crc under g_index_mutex. When the sidecar cannot serve the entry, nothing in
 * the file changes until it is rewritten, so the verdict records what the read
 * found: whether the content is defective (magic, version, range, reserved,
 * body crc, or a file shorter than its header says), and otherwise the
 * header's n_vecs, dim, metric and base_tids_crc32. It is tied to the file identity (inode, size, nanosecond
 * mtime). While the identity is the same, the remembered header is checked
 * against the entry's current row count, dim, metric and `.tids` crc with the
 * same test the read path applies, so an extend (row count change) or a new
 * `.tids` re-decides without reading the file. Precision is not an input:
 * none of these reasons depends on it. Resource and I/O failures of the read
 * (allocation, short header, read error) are not remembered: they can pass. */

#include <stdint.h>

#include "cuvs_util.h"

typedef struct CuvsBfVerdict {
    int      valid;
    uint64_t ino;              /* `.vectors` identity when the verdict was made */
    int64_t  size;
    int64_t  mtime_ns;
    int      content_bad;      /* cuvs_vectors_read returned CUVS_VECTORS_READ_BAD */
    int64_t  n_vecs;           /* header values (meaningful when !content_bad) */
    uint32_t dim;
    uint32_t metric;
    uint32_t base_tids_crc32;
} CuvsBfVerdict;

/* The test refresh_main_bf_cache applies to a sidecar whose content is valid:
 * its shape matches the entry and it belongs to the current `.tids`
 * (tids_ok: the `.tids` header was read and has the right magic). */
static inline int
cuvs_bf_sidecar_fits(int64_t file_n, uint32_t file_dim, uint32_t file_metric,
                     uint32_t file_base_tids_crc32,
                     int64_t n_vecs, uint32_t dim, uint32_t metric,
                     int tids_ok, uint32_t tids_crc)
{
    return file_dim == dim && file_metric == metric && file_n == n_vecs
        && tids_ok && tids_crc == file_base_tids_crc32;
}

/* 1: the remembered verdict answers "unavailable" for the current state, so
 * the sidecar need not be read. 0: read it (no verdict, another file, or the
 * remembered header would now fit). */
static inline int
cuvs_bf_verdict_unavailable(const CuvsBfVerdict *v,
                            uint64_t ino, int64_t size, int64_t mtime_ns,
                            int64_t n_vecs, uint32_t dim, uint32_t metric,
                            int tids_ok, uint32_t tids_crc)
{
    if (!v->valid || v->ino != ino || v->size != size || v->mtime_ns != mtime_ns)
        return 0;
    if (v->content_bad)
        return 1;
    return !cuvs_bf_sidecar_fits(v->n_vecs, v->dim, v->metric, v->base_tids_crc32,
                                 n_vecs, dim, metric, tids_ok, tids_crc);
}

/* Record the outcome of a cuvs_vectors_read that did not lead to a BF index.
 * read_rc is its return value; hdr is the header it filled (used when
 * read_rc == 0). Returns 1 if a verdict was recorded, 0 if not (resource or
 * I/O failure: the previous verdict, if any, is kept as it was). */
static inline int
cuvs_bf_verdict_note(CuvsBfVerdict *v, int read_rc, const CuvsVectorsHeader *hdr,
                     uint64_t ino, int64_t size, int64_t mtime_ns)
{
    if (read_rc != 0 && read_rc != CUVS_VECTORS_READ_BAD)
        return 0;
    v->valid       = 1;
    v->ino         = ino;
    v->size        = size;
    v->mtime_ns    = mtime_ns;
    v->content_bad = (read_rc == CUVS_VECTORS_READ_BAD);
    v->n_vecs          = v->content_bad ? 0 : hdr->n_vecs;
    v->dim             = v->content_bad ? 0 : hdr->dim;
    v->metric          = v->content_bad ? 0 : hdr->metric;
    v->base_tids_crc32 = v->content_bad ? 0 : hdr->base_tids_crc32;
    return 1;
}

#endif /* CUVS_BF_VERDICT_H */
