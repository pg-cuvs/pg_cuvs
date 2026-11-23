/*
 * cuvs_hnsw_upper.h — an hnswlib file from a CAGRA graph, with the
 * upper layers built on the host (daemon, CUVS_OP_EXPORT_HNSW_UPPER).
 *
 * Level 0 is the given graph (entries out of range, self links and
 * duplicates dropped). Levels are drawn per row from a seed fixed by (n, M),
 * the same draw the backend's own conversion makes (cagra_assign_level in
 * hnsw_export.c), and elements keep row order, so the same input always
 * gives the same levels, element order and pages. The upper layers are built
 * by HNSW insertion (greedy descent, ef-bounded layer search, heuristic
 * neighbor selection, mutual links pruned to M) of the elements above level
 * 0, on several threads; only the links themselves depend on thread timing.
 * No GPU memory is used.
 */
#ifndef CUVS_HNSW_UPPER_H
#define CUVS_HNSW_UPPER_H

#include <stddef.h>
#include <stdint.h>

#define CUVS_HNSW_UPPER_OK        0
#define CUVS_HNSW_UPPER_CANCELED  1
#define CUVS_HNSW_UPPER_ERROR    (-1)

/* Asked now and then from the building threads; nonzero stops the build. */
typedef int (*CuvsHnswUpperCancel)(void *arg);

typedef struct CuvsHnswUpperStats
{
    uint64_t upper_nodes;   /* elements above level 0 */
    int      max_level;
    int      threads;
    uint64_t build_us;      /* upper layers */
    uint64_t write_us;      /* the file */
} CuvsHnswUpperStats;

/*
 * graph: n rows of graph_stride uint32 ids; the first `degree` of each row
 * are level 0 (degree even, 4 <= degree <= graph_stride, so M >= 2). vecs: n x dim
 * floats, unit rows when inner_product is set for a cosine index. The file
 * (hnswlib saveIndex layout, M = degree / 2, efConstruction as given) is
 * written to out_fd from its current offset. threads <= 0 picks
 * min(online CPUs, 8).
 */
int cuvs_hnsw_upper_write(int out_fd, const uint32_t *graph, int graph_stride, int degree,
                          const float *vecs, uint64_t n, int dim, int inner_product,
                          int ef_construction, int threads,
                          CuvsHnswUpperCancel cancel, void *cancel_arg,
                          CuvsHnswUpperStats *stats, char *error, size_t error_len);

/* The level the conversion gives row draws in order: exposed for tests. */
int cuvs_hnsw_upper_level(int M, unsigned int *seed);

#endif
