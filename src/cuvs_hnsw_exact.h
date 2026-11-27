#ifndef CUVS_HNSW_EXACT_H
#define CUVS_HNSW_EXACT_H

#include "access/relscan.h"

typedef struct CuvsHnswExact CuvsHnswExact;

/* Internal AM utility, not a caller-visible SQL function. It walks the heap
 * under the scan's snapshot and returns every visible row's HOT root TID.
 * Distance uses the SQL ordering function, not cuVS's squared/raw scale.
 * Requires heap table AM, MVCC snapshot, zero index search keys and one
 * strict scalar float8 ordering function. Unsupported contracts fail explicitly.
 * Direct utility use on an RLS relation is rejected. Product RLS statements
 * use a native executor scan/sort so security quals precede expressions.
 * The scan wrapper (cuvs_hnsw_scan.c) calls the _cover variant for every
 * exact scan, an explicit-mode scan of an index without a descriptor included,
 * and the _tids variant for the merge. Both accept an RLS relation for a
 * plain-column index (the operator class's own distance function on a stored
 * vector, as an index scan does for the rows that are hidden) and still refuse
 * one for an index with an expression or a predicate. The scan wrapper has
 * already stopped an explicit-mode scan on an RLS table (cover_rescan), so only
 * the descriptor path reaches that case. cuvs_hnsw_exact_begin, the strict
 * variant that refuses every RLS relation, has no caller in src: the private
 * exact test (hnsw_exact_pg_probe.c) keeps it covered.
 * The outer executor retains responsibility for other residual quals. */
/* True when the predicate of the (partial) index uses only objects that the system created: see exact_pred_unsafe. An index
 * without a predicate is true. The exact module refuses an index for which this is false. */
bool cuvs_hnsw_exact_predicate_builtin(Relation index);
CuvsHnswExact *cuvs_hnsw_exact_begin(IndexScanDesc scan);
CuvsHnswExact *cuvs_hnsw_exact_begin_cover(IndexScanDesc scan);
/* Same, but only over the given index entry TIDs (each followed through its HOT chain to the version the snapshot
 * sees): the answers are the exact ranking of that set. The array must stay valid until cuvs_hnsw_exact_end. */
CuvsHnswExact *cuvs_hnsw_exact_begin_tids(IndexScanDesc scan, const ItemPointerData *tids, Size ntids);
bool cuvs_hnsw_exact_next(CuvsHnswExact *state, ItemPointer root,
                          Datum *distance, bool *isnull);
void cuvs_hnsw_exact_end(CuvsHnswExact *state);

#endif
