#ifndef CUVS_HNSW_BUILD_H
#define CUVS_HNSW_BUILD_H

/* PostgreSQL-only; include after postgres.h. Successful descriptors belong to
 * the converter, which must close both even if page conversion raises ERROR. */
#include "utils/relcache.h"
#include "nodes/execnodes.h"
#include "cuvs_hnsw_policy.h"

typedef struct CuvsHnswBuildResult {
    int hnsw_fd;
    int tids_fd;             /* CuvsTidsHeader + original-row-order uint64 TIDs */
    uint64_t n_vecs;
    uint32_t dim;
    uint32_t metric;
    double reltuples;
    CuvsHnswPolicy policy;
} CuvsHnswBuildResult;

void cuvs_build_hnsw_from_heap(Relation heapRel, Relation indexRel,
    IndexInfo *indexInfo, const char *mode, uint32_t build_algo,
    uint32_t ef_construction, CuvsHnswBuildResult *out);

#endif
