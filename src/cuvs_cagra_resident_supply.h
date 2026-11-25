#ifndef CUVS_CAGRA_RESIDENT_SUPPLY_H
#define CUVS_CAGRA_RESIDENT_SUPPLY_H

#if defined(CUVS_PAGE_FORMAT) && defined(CUVS_CAGRA_FOLD_ENABLED)
#include "cuvs_ipc.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

typedef struct CuvsCagraResidentSupplyProgress
{
    CuvsPageOverlayRequest request; /* from is the daemon's initial coverage */
    CuvsPageProgress progress;    /* coverage confirmed by the last reply */
} CuvsCagraResidentSupplyProgress;

/* Register from _PG_init during shared preload. */
void cuvs_cagra_resident_supply_register(void);

/* In the same database, hold at least AccessShareLock on heap, then index.
 * Pass an active MVCC snapshot from a transaction without an assigned XID.
 * limit is an exclusive absolute pending position no greater than the
 * observed tail (UINT64_MAX for the worker).
 * OK can mean zero progress. Discard result on other statuses. */
int cuvs_cagra_resident_supply_step(Relation index, Relation heap, Snapshot snapshot,
                                   uint64 limit, CuvsCagraResidentSupplyProgress *result);
#endif

#endif
