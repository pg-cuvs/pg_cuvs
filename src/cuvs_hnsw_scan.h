#ifndef CUVS_HNSW_SCAN_H
#define CUVS_HNSW_SCAN_H

#include "access/amapi.h"
#include "utils/rel.h"

/* Explicit exact mode is off by default; automatic cover publication and
 * selective ANN/repair union have separate storage/maintenance contracts. */
void cuvs_hnsw_scan_init(void);
bool cuvs_hnsw_scan_exact_requested(void);
void cuvs_hnsw_scan_wrap(IndexAmRoutine *routine);
extern const IndexAmRoutine *cuvs_hnsw_scan_delegate(void);

/* True when a scan of this index is answered by the exact module (the merge of a CERTIFIED index, the whole heap of a LOST or
 * untrusted one) and not by the graph alone. Decided from the descriptor class, which does not change between planning and
 * scanning except between CERTIFIED and LOST: an OFF or LEGACY index stays so until it is built again. */
bool cuvs_hnsw_scan_exact_served(Relation index);

#endif
