#pragma once

/*
 * hnsw_export.h — GPU HNSW index creation for pg_cuvs.
 *
 * Public API: pg_cuvs_build_hnsw(cagra_oid, mode)
 * Internal helpers: fill_hnsw_from_hnswlib / fill_hnsw_from_cagra_ipc
 *   (static, defined in hnsw_export.c, not callable from SQL)
 */

#include "postgres.h"
#include "fmgr.h"
#include "nodes/execnodes.h"   /* IndexInfo */
#include "utils/relcache.h"    /* Relation */

/*
 * pg_cuvs_build_hnsw(cagra_oid regclass, mode text DEFAULT 'nsw')
 *
 * GPU-accelerated HNSW creation without pgvector CPU build (285s eliminated).
 * Creates the HNSW index internally via INDEX_CREATE_SKIP_BUILD.
 *
 * Recommended modes:
 *   'nsw'     — level 0 from the CAGRA graph, upper layers built by the daemon
 *               (the same as 'hnsw'). Default. (117s / 2.4x were the old level-0-only path.)
 *   'hnswlib' — from_cagra() hierarchy via /dev/shm, 139s, 2.0x.
 *
 * Hidden modes (research only):
 *   'hnsw', 'hnswlib_file'
 *
 * Returns OID of newly created HNSW index (regclass).
 */
Datum pg_cuvs_build_hnsw(PG_FUNCTION_ARGS);

/*
 * Phase 3K (ADR-038): pg_cuvs_hnsw access method.
 *
 *   CREATE INDEX my_idx ON items USING pg_cuvs_hnsw (embedding vector_l2_ops)
 *     WITH (source = 'my_cagra', mode = 'nsw');
 *
 * cuvs_hnsw_init_reloptions() registers the WITH (source, mode, ...) options
 * and must be called once from _PG_init before any CREATE INDEX runs.
 */
void  cuvs_hnsw_init_reloptions(void);

/* Pre-build size of a source-less pg_cuvs_hnsw index (metapage + level-0
 * packed data pages); a lower bound for hierarchical graphs. 0 = unknown. */
uint64_t cuvs_hnsw_estimate_index_bytes(uint64_t n, int dim, int bytes_per_dim, int m);
Datum pg_cuvs_hnsw_handler(PG_FUNCTION_ARGS);
void  cuvs_report_proto_mismatch(int rc);
const char *cuvs_resolve_index_dir_rel(Relation indexRel);

/*
 * Shared heap-scan + CAGRA build (defined in pg_cuvs.c), used by cuvs_ambuild
 * (USING cagra). The source-less HNSW AM uses the separate disk-backed
 * cuvs_build_hnsw_from_heap() interface in cuvs_hnsw_build.h.
 */
void cuvs_build_cagra_from_heap(Relation heapRel, Relation indexRel,
                                IndexInfo *indexInfo,
                                uint32_t build_index_oid, uint32_t shard_count,
                                bool use_cpu_hnsw,
                                int graph_degree, int intermediate_graph_degree,
                                uint32_t build_algo,
                                int64_t *out_n_vecs, double *out_reltuples);
