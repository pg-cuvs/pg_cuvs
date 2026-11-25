#pragma once

/*
 * pg_stat_progress_create_index subphases of the source-less pg_cuvs_hnsw
 * build (CREATE INDEX ... USING pg_cuvs_hnsw without source).  The view shows
 * them as "building index: <name>".  PostgreSQL itself reports subphase 1
 * (PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE) when the build starts.  The access
 * method numbers its own phases from 3, leaving pgvector's subphase 2
 * unassigned.
 *
 *   spooling heap rows               blocks_done/blocks_total from the heap scan
 *   building graph in the GPU daemon no counter: the backend waits for the reply
 *   checking graph files             no counter: preflight and location spill
 *   writing index pages              tuples_done/tuples_total count graph nodes
 *                                    whose element and neighbor pages are published
 */

#include <stddef.h>
#include <stdint.h>

#define CUVS_HNSW_SUBPHASE_INITIALIZE 1
#define CUVS_HNSW_SUBPHASE_SPOOL      3
#define CUVS_HNSW_SUBPHASE_DAEMON     4
#define CUVS_HNSW_SUBPHASE_CHECK      5
#define CUVS_HNSW_SUBPHASE_WRITE      6

static inline const char *
cuvs_hnsw_build_phase_name(int64_t phase)
{
    switch (phase)
    {
        case CUVS_HNSW_SUBPHASE_INITIALIZE:
            return "initializing";
        case CUVS_HNSW_SUBPHASE_SPOOL:
            return "spooling heap rows";
        case CUVS_HNSW_SUBPHASE_DAEMON:
            return "building graph in the GPU daemon";
        case CUVS_HNSW_SUBPHASE_CHECK:
            return "checking graph files";
        case CUVS_HNSW_SUBPHASE_WRITE:
            return "writing index pages";
        default:
            return NULL;
    }
}
