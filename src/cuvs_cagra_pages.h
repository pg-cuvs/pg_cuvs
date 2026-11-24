#ifndef CUVS_CAGRA_PAGES_H
#define CUVS_CAGRA_PAGES_H

#include "access/amapi.h"
#include "access/xlogdefs.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"

static inline bool
cuvs_cagra_has_pages(Relation index)
{
    return RelationGetNumberOfBlocks(index) != 0;
}

typedef struct CuvsCagraRelCache
{
    bool        page_format;
    bool        stale_sent;
#ifdef CUVS_PAGE_FORMAT
    uint64      index_nonce;
    BlockNumber tail;
    int         dim;
    bool        pending_vectors;
#endif
} CuvsCagraRelCache;

CuvsCagraRelCache *cuvs_cagra_relation_cache(Relation index);
void cuvs_cagra_require_legacy(Relation index);

#ifdef CUVS_PAGE_FORMAT
#define CUVS_CAGRA_PAGE_MAGIC 0x43565047U
#define CUVS_CAGRA_PAGE_MAJOR 0   /* development format, never accepted by a release */
#define CUVS_CAGRA_PAGE_EXTENT 64
#define CUVS_CAGRA_PAGE_DIR 1950
#define CUVS_CAGRA_TIDS_PER_PAGE 1357
#define CUVS_CAGRA_PENDING_PER_PAGE 1356
#define CUVS_CAGRA_PENDING_VECTOR 2U

typedef struct CuvsCagraMeta
{
    uint32      magic;
    uint16      major;
    uint16      minor;
    uint32      flags;
    uint32      dim;
    uint32      metric;
    uint64      index_nonce;
    uint64      base_nonce;
    uint64      graph_nonce;
    uint64      n_slots;
    uint64      folded;
    uint64      base_slots;
    uint64      delete_generation;
    uint64      dead_slots;
    BlockNumber pending_head;
    BlockNumber pending_tail;
    BlockNumber foldlog_head;
    BlockNumber foldlog_tail;
    BlockNumber foldlog_spare;
    uint32      n_extents;
    BlockNumber extent_overflow;
    uint64      last_fold_xid;
    int64       last_fold_time;
    BlockNumber extents[CUVS_CAGRA_PAGE_DIR];
} CuvsCagraMeta;

uint64 cuvs_cagra_nonce(void);
bool cuvs_cagra_read_meta(Relation index, CuvsCagraMeta *meta);

typedef enum CuvsCagraSlotResult
{
    CUVS_CAGRA_SLOT_BAD,
    CUVS_CAGRA_SLOT_DEAD,
    CUVS_CAGRA_SLOT_LIVE
} CuvsCagraSlotResult;

bool cuvs_cagra_read_tail(Relation index, const CuvsCagraMeta *meta,
                          BlockNumber nblocks, uint64 *tail);
typedef struct CuvsCagraPendingLocation
{
    BlockNumber block;
    ItemPointerData tid;
} CuvsCagraPendingLocation;

bool cuvs_cagra_copy_pending(Relation index, const CuvsCagraMeta *meta,
                             BlockNumber nblocks, uint64 from, uint64 upto,
                             CuvsCagraPendingLocation *locations, float *vectors);

/* Local PG storage input. The caller has already authenticated the worker's
 * database, relation and postmaster lifetime before preparing this candidate. */
typedef struct CuvsCagraFoldCandidate
{
    uint64 index_nonce;
    uint64 base_nonce;
    uint64 parent_graph_nonce;
    uint64 parent_n_slots;
    uint64 parent_folded;
    uint64 graph_nonce;
    uint32 positions;
} CuvsCagraFoldCandidate;

typedef enum CuvsCagraFoldStatus
{
    CUVS_CAGRA_FOLD_PREPARED,
    CUVS_CAGRA_FOLD_PUBLISHED,
    CUVS_CAGRA_FOLD_DEFERRED,
    CUVS_CAGRA_FOLD_DISCARD
} CuvsCagraFoldStatus;

/* Caller holds the relation lock and supplies storage for candidate->positions
 * entries. A successful preparation retains the exclusive FCL (page lock 0).
 * Use the copied positions only under that lock. Failure retains no FCL.
 * Preparation does not change any published page. */
CuvsCagraFoldStatus cuvs_cagra_prepare_fold(Relation index,
                        const CuvsCagraFoldCandidate *candidate,
                        CuvsCagraMeta *base, CuvsCagraPendingLocation *locations);

/* Standalone writable background worker operation on a WAL-logged index. The
 * caller holds RowExclusiveLock and has verified the candidate's origin and PG life.
 * PUBLISHED returns a flushed physical transition LSN and no FCL. That is
 * the first point at which the caller may send FOLD_COMMITTED. */
CuvsCagraFoldStatus cuvs_cagra_publish_fold(Relation index,
                        const CuvsCagraFoldCandidate *candidate, XLogRecPtr *commit_lsn);
CuvsCagraSlotResult cuvs_cagra_pending_tid(Relation index, const CuvsCagraMeta *meta,
                                         BlockNumber nblocks, BlockNumber block,
                                         uint64 position, ItemPointer tid);
CuvsCagraSlotResult cuvs_cagra_slot_tid(Relation index, const CuvsCagraMeta *meta,
                                      BlockNumber nblocks, uint64 slot, ItemPointer tid);
bool cuvs_cagra_slot_filter(Relation index, const CuvsCagraMeta *meta,
                            BlockNumber nblocks, const uint64 *roots, int nroots,
                            uint32 *excluded);
void cuvs_cagra_init_pages(Relation index, ForkNumber fork, uint64 index_nonce,
                          uint64 graph_nonce, int dim, uint32 metric,
                          const uint64 *tids, int64 n, bool pending_vectors);
bool cuvs_cagra_can_store_pending_vector(int dim);
void cuvs_cagra_append_pending(Relation index, ItemPointer tid, int dim,
                               const float *vector);
void cuvs_cagra_check_dim(int expected, int actual);
void cuvs_cagra_check_query_dim(Relation index, int dim);
IndexBulkDeleteResult *cuvs_cagra_page_bulkdelete(IndexVacuumInfo *info,
    IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback, void *callback_state);
IndexBulkDeleteResult *cuvs_cagra_page_vacuumcleanup(IndexVacuumInfo *info,
    IndexBulkDeleteResult *stats);
#ifdef CUVS_TEST_HOOKS
void cuvs_cagra_vacuum_init_test_hook(void);
#endif
#endif

#endif
