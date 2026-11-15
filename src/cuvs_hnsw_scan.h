#ifndef CUVS_HNSW_SCAN_H
#define CUVS_HNSW_SCAN_H

#include "access/amapi.h"

/* Explicit exact mode is off by default; automatic cover publication and
 * selective ANN/repair union have separate storage/maintenance contracts. */
void cuvs_hnsw_scan_init(void);
bool cuvs_hnsw_scan_exact_requested(void);
void cuvs_hnsw_scan_wrap(IndexAmRoutine *routine);

#endif
