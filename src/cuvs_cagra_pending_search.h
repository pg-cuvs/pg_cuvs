#ifndef CUVS_CAGRA_PENDING_SEARCH_H
#define CUVS_CAGRA_PENDING_SEARCH_H

#ifdef CUVS_PAGE_FORMAT
#include "cuvs_cagra_pages.h"
#include "cuvs_ipc.h"
#include "utils/snapshot.h"

bool cuvs_cagra_local_supplement(Relation index, Relation heap, Snapshot snapshot,
                                 const CuvsCagraMeta *meta, BlockNumber blocks,
                                 uint64 from, uint64 upto, CuvsPageSupplement *supplement);
void cuvs_cagra_free_supplement(CuvsPageSupplement *supplement);
#endif

#endif
