#ifndef CUVS_CAGRA_PAGES_H
#define CUVS_CAGRA_PAGES_H

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
void cuvs_cagra_init_pages(Relation index, ForkNumber fork, uint64 index_nonce,
                          uint64 graph_nonce, int dim, uint32 metric,
                          const uint64 *tids, int64 n);
void cuvs_cagra_append_pending(Relation index, CuvsCagraRelCache *cache,
                               ItemPointer tid, int dim);
void cuvs_cagra_check_dim(int expected, int actual);
#endif

#endif
