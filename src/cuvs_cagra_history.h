#ifndef CUVS_CAGRA_HISTORY_H
#define CUVS_CAGRA_HISTORY_H

#ifdef CUVS_PAGE_FORMAT
#include "access/transam.h"
#include "cuvs_cagra_pages.h"
#include "cuvs_ipc.h"
#include "utils/snapshot.h"

extern bool cuvs_page_resident_search;

Size cuvs_cagra_history_shmem_memsize(void);
/* Call from the preload startup hook with AddinShmemInitLock held. */
void cuvs_cagra_history_shmem_startup(void);

/* The caller holds both relation locks and keeps the MVCC snapshot registered
 * or active through all requests and result resolution. Only the four graph
 * fields change. False means the caller must retry or use exact search. */
bool cuvs_cagra_history_select(Relation index, Relation heap, Snapshot snapshot,
                               const CuvsCagraMeta *current, CuvsPageSearchRequest *request);
/* The caller holds the heap relation lock and the same live snapshot used
 * for selection. Zero means no valid horizon. Use only for resident search. */
uint32 cuvs_cagra_history_horizon(Relation heap, Snapshot snapshot);

/* Publication owns the exclusive page-0 fence while reserving. */
bool cuvs_cagra_history_reserve(Relation index, const CuvsCagraMeta *parent,
                               const CuvsCagraFoldCandidate *candidate, FullTransactionId xid);
/* Call only after cuvs_cagra_publish_fold returned PUBLISHED and a flushed
 * LSN for this event. False leaves the resident ACK pending. */
bool cuvs_cagra_history_transition_lookup(Relation index,
                               const CuvsCagraFoldCandidate *candidate, uint32 *transition_xid);
void cuvs_cagra_history_acknowledged(Oid index_oid, const CuvsCagraFoldCandidate *candidate,
                                     uint32 transition_xid);
void cuvs_cagra_history_collect_database(Oid cagra_am, Snapshot snapshot);
#endif

#endif
