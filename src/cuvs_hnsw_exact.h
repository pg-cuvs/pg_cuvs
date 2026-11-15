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
 * The outer executor retains responsibility for other residual quals. */
CuvsHnswExact *cuvs_hnsw_exact_begin(IndexScanDesc scan);
bool cuvs_hnsw_exact_next(CuvsHnswExact *state, ItemPointer root,
                          Datum *distance, bool *isnull);
void cuvs_hnsw_exact_end(CuvsHnswExact *state);

#endif
