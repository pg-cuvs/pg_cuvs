#ifndef CUVS_HNSW_BUILD_H
#define CUVS_HNSW_BUILD_H

/* PostgreSQL-only; include after postgres.h. Successful descriptors belong to
 * the converter, which must close both even if page conversion raises ERROR. */
#include "utils/relcache.h"
#include "nodes/execnodes.h"

typedef struct CuvsHnswBuildResult {
    int hnsw_fd;
    int tids_fd;             /* CuvsTidsHeader + original-row-order uint64 TIDs */
    uint64_t n_vecs;
    uint32_t dim;
    uint32_t metric;
    double reltuples;
} CuvsHnswBuildResult;

void cuvs_build_hnsw_from_heap(Relation heapRel, Relation indexRel,
    IndexInfo *indexInfo, const char *mode, CuvsHnswBuildResult *out);

#endif
