#include "postgres.h"

#include "cuvs_cagra_history.h"

#ifdef CUVS_PAGE_FORMAT
#include "access/relation.h"
#include "access/xlog.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class_d.h"
#include "miscadmin.h"
#include "storage/bufpage.h"
#include "storage/lmgr.h"
#include "storage/lwlock.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "utils/snapmgr.h"

#define CUVS_CAGRA_HISTORY_RECORDS 1024

typedef struct CuvsCagraHistoryRecord
{
    Oid                 db_oid;
    Oid                 index_oid;
    uint64              index_nonce;
    uint64              base_nonce;
    uint64              child_graph_nonce;
    FullTransactionId   xid;
    uint64              parent_graph_nonce;
    uint64              parent_n_slots;
    uint64              parent_folded;
    uint64              parent_dead_slots;
    uint64              parent_last_xid;
    uint32              positions;
    bool                acknowledged;
} CuvsCagraHistoryRecord;

typedef struct CuvsCagraHistory
{
    LWLock              lock;
    CuvsCagraHistoryRecord records[CUVS_CAGRA_HISTORY_RECORDS];
} CuvsCagraHistory;

static CuvsCagraHistory *history;

Size
cuvs_cagra_history_shmem_memsize(void)
{
    return MAXALIGN(sizeof(CuvsCagraHistory));
}

void
cuvs_cagra_history_shmem_startup(void)
{
    bool found;

    history = ShmemInitStruct("pg_cuvs CAGRA fold history",
                              cuvs_cagra_history_shmem_memsize(), &found);
    if (!found)
    {
        memset(history, 0, sizeof(*history));
        LWLockInitialize(&history->lock, LWLockNewTrancheId());
    }
    LWLockRegisterTranche(history->lock.tranche, "PgCuvsCagraHistory");
}

static bool
has_live_snapshot(Snapshot snapshot)
{
    return snapshot && IsMVCCSnapshot(snapshot) &&
        (snapshot->regd_count || snapshot->active_count);
}

static bool
history_heap(Relation heap)
{
    return heap && heap->rd_rel->relam == HEAP_TABLE_AM_OID &&
        RelationNeedsWAL(heap) && !heap->rd_rel->relisshared;
}

/* ProcArray and snapshots expose low XIDs. Reconstruct them relative to a
 * later full XID only within the unambiguous half of the transaction ID ring. */
static bool
history_full_xid(FullTransactionId next, TransactionId xid, FullTransactionId *full)
{
    uint32 age = XidFromFullTransactionId(next) - xid;
    uint64 value = U64FromFullTransactionId(next);

    if (!TransactionIdIsNormal(xid) || age > PG_INT32_MAX || value < age)
        return false;
    *full = FullTransactionIdFromU64(value - age);
    return true;
}

uint32
cuvs_cagra_history_horizon(Relation heap, Snapshot snapshot)
{
    FullTransactionId next, oldest, xmin;
    TransactionId horizon;

    if (!cuvs_page_resident_search || !history || !history_heap(heap) ||
        !has_live_snapshot(snapshot) || RecoveryInProgress())
        return InvalidTransactionId;

    horizon = GetOldestNonRemovableTransactionId(heap);
    next = ReadNextFullTransactionId();
    if (!history_full_xid(next, horizon, &oldest) ||
        !history_full_xid(next, snapshot->xmin, &xmin))
        return InvalidTransactionId;

    if (FullTransactionIdPrecedes(xmin, oldest))
        return snapshot->xmin;
    return horizon;
}

/* Caller holds the history lock. The boot scope is the shared segment itself. */
static CuvsCagraHistoryRecord *
find_transition(Oid index_oid, uint64 index_nonce, uint64 base_nonce, uint64 child_graph_nonce)
{
    for (int i = 0; i < CUVS_CAGRA_HISTORY_RECORDS; ++i)
    {
        CuvsCagraHistoryRecord *record = &history->records[i];

        if (record->db_oid == MyDatabaseId && record->index_oid == index_oid &&
            record->index_nonce == index_nonce && record->base_nonce == base_nonce &&
            record->child_graph_nonce == child_graph_nonce)
            return record;
    }
    return NULL;
}

static bool
transition_matches(const CuvsCagraHistoryRecord *record, const CuvsCagraFoldCandidate *candidate)
{
    return record && record->parent_graph_nonce == candidate->parent_graph_nonce &&
        record->parent_n_slots == candidate->parent_n_slots &&
        record->parent_folded == candidate->parent_folded &&
        record->positions == candidate->positions;
}

bool
cuvs_cagra_history_select(Relation index, Relation heap, Snapshot snapshot,
                          const CuvsCagraMeta *current, CuvsPageSearchRequest *request)
{
    CuvsPageSearchRequest selected = *request;
    FullTransactionId next, xmin;
    Oid index_oid = RelationGetRelid(index);
    uint64 xid = current->last_fold_xid;
    uint64 graph = current->graph_nonce;
    uint64 n_slots = current->n_slots;
    uint64 folded = current->folded;

    if (!cuvs_page_resident_search || !has_live_snapshot(snapshot) || RecoveryInProgress() ||
        !heap || !index->rd_index || index->rd_index->indrelid != RelationGetRelid(heap) ||
        request->system_identifier != GetSystemIdentifier() ||
        request->index_nonce != current->index_nonce || request->base_nonce != current->base_nonce ||
        request->graph_nonce != graph || request->n_slots != n_slots ||
        request->folded != folded || request->tail < folded || current->dead_slots > n_slots)
        return false;
    if (!xid)
        return folded == 0;
    if (!history || !history_heap(heap))
        return false;

    next = ReadNextFullTransactionId();
    if (!history_full_xid(next, snapshot->xmin, &xmin))
        return false;

    /* Even a visible transition can follow an older transition still present
     * in this snapshot. Walk until every earlier XID is below snapshot xmin. */
    while (xid >= U64FromFullTransactionId(xmin))
    {
        CuvsCagraHistoryRecord edge;
        CuvsCagraHistoryRecord *record;
        FullTransactionId transition = FullTransactionIdFromU64(xid);

        CHECK_FOR_INTERRUPTS();
        if (!FullTransactionIdPrecedes(transition, next) ||
            !TransactionIdIsNormal(XidFromFullTransactionId(transition)))
            return false;

        LWLockAcquire(&history->lock, LW_SHARED);
        record = find_transition(index_oid, current->index_nonce, current->base_nonce, graph);
        if (!record)
        {
            LWLockRelease(&history->lock);
            return false;
        }
        edge = *record;
        LWLockRelease(&history->lock);

        if (!FullTransactionIdEquals(edge.xid, transition) ||
            edge.parent_last_xid >= xid || !edge.positions ||
            edge.positions > n_slots || edge.parent_n_slots != n_slots - edge.positions ||
            edge.positions > folded || edge.parent_folded != folded - edge.positions ||
            !edge.parent_graph_nonce || edge.parent_graph_nonce == graph ||
            edge.parent_dead_slots > edge.parent_n_slots)
            return false;

        if (XidInMVCCSnapshot(XidFromFullTransactionId(transition), snapshot))
        {
            selected.graph_nonce = edge.parent_graph_nonce;
            selected.n_slots = edge.parent_n_slots;
            selected.folded = edge.parent_folded;
            selected.dead_slots = edge.parent_dead_slots;
        }

        graph = edge.parent_graph_nonce;
        n_slots = edge.parent_n_slots;
        folded = edge.parent_folded;
        xid = edge.parent_last_xid;
    }

    *request = selected;
    return true;
}

static FullTransactionId
index_horizon(Relation index, Snapshot snapshot)
{
    FullTransactionId oldest = InvalidFullTransactionId;
    Oid heap_oid = index->rd_index->indrelid;

    /* The fold worker already holds the index lock. Never wait for a heap
     * lock in that order. A busy or unavailable heap cannot authorize GC. */
    if (ConditionalLockRelationOid(heap_oid, AccessShareLock))
    {
        Relation heap = try_relation_open(heap_oid, NoLock);

        if (heap)
        {
            TransactionId horizon = cuvs_cagra_history_horizon(heap, snapshot);

            (void) history_full_xid(ReadNextFullTransactionId(), horizon, &oldest);
            relation_close(heap, NoLock);
        }
        UnlockRelationOid(heap_oid, AccessShareLock);
    }
    return oldest;
}

/* Caller holds the exclusive index page-0 fence and the history lock.
 * The current metapage is authoritative even if the publisher aborted. */
static bool
collect_index(Relation index, const CuvsCagraMeta *current, FullTransactionId oldest)
{
    Oid index_oid = RelationGetRelid(index);
    bool reclaimed = false;

    for (int i = 0; i < CUVS_CAGRA_HISTORY_RECORDS; ++i)
    {
        CuvsCagraHistoryRecord *record = &history->records[i];

        if (record->db_oid != MyDatabaseId || record->index_oid != index_oid)
            continue;

        if (record->index_nonce != current->index_nonce ||
            record->base_nonce != current->base_nonce ||
            record->parent_graph_nonce == current->graph_nonce ||
            (FullTransactionIdIsValid(oldest) && FullTransactionIdPrecedes(record->xid, oldest) &&
             (record->acknowledged ||
              U64FromFullTransactionId(record->xid) < current->last_fold_xid)))
        {
            /* A reservation descended from the current graph never published.
             * The local sweep keeps the unconfirmed current transition. */
            *record = (CuvsCagraHistoryRecord) {0};
            reclaimed = true;
        }
    }
    return reclaimed;
}

static FullTransactionId
cluster_history_horizon(void)
{
    FullTransactionId oldest = InvalidFullTransactionId;
    TransactionId horizon;

    /* A NULL relation requests PostgreSQL's shared-relation horizon,
     * including snapshots in every database. Sample outside history->lock. */
    horizon = GetOldestNonRemovableTransactionId(NULL);
    (void) history_full_xid(ReadNextFullTransactionId(), horizon, &oldest);
    return oldest;
}

/* Caller holds the history lock. An expired record is no longer needed for
 * snapshot selection. An ACK retry can recover its XID from the WAL pages. */
static CuvsCagraHistoryRecord *
find_available_record(FullTransactionId cluster_oldest)
{
    CuvsCagraHistoryRecord *expired = NULL;

    for (int i = 0; i < CUVS_CAGRA_HISTORY_RECORDS; ++i)
    {
        CuvsCagraHistoryRecord *record = &history->records[i];

        if (!OidIsValid(record->db_oid))
            return record;
        if (!expired && FullTransactionIdIsValid(cluster_oldest) &&
            FullTransactionIdPrecedes(record->xid, cluster_oldest))
            expired = record;
    }
    return expired;
}

bool
cuvs_cagra_history_reserve(Relation index, const CuvsCagraMeta *parent,
                           const CuvsCagraFoldCandidate *candidate, FullTransactionId xid)
{
    CuvsCagraHistoryRecord *record;
    FullTransactionId oldest;
    Oid index_oid = RelationGetRelid(index);
    Snapshot snapshot;
    bool reserved = false;

    if (!history || !OidIsValid(MyDatabaseId) || !index->rd_index ||
        parent->dead_slots > parent->n_slots ||
        U64FromFullTransactionId(xid) <= parent->last_fold_xid)
        return false;

    snapshot = ActiveSnapshotSet() ? GetActiveSnapshot() : NULL;
    oldest = snapshot ? index_horizon(index, snapshot) : InvalidFullTransactionId;
    LWLockAcquire(&history->lock, LW_EXCLUSIVE);
    (void) collect_index(index, parent, oldest);
    if (find_transition(index_oid, parent->index_nonce, parent->base_nonce, candidate->graph_nonce))
        goto done;

    record = find_available_record(InvalidFullTransactionId);
    if (!record && has_live_snapshot(snapshot) && !RecoveryInProgress())
    {
        FullTransactionId cluster_oldest;

        LWLockRelease(&history->lock);
        cluster_oldest = cluster_history_horizon();
        LWLockAcquire(&history->lock, LW_EXCLUSIVE);

        /* The page-0 fence still excludes other reservations for this
         * index. Recheck shared capacity before choosing a slot to reuse. */
        record = find_available_record(cluster_oldest);
    }
    if (record)
    {
        *record = (CuvsCagraHistoryRecord) {
            .db_oid = MyDatabaseId, .index_oid = index_oid,
            .index_nonce = parent->index_nonce, .base_nonce = parent->base_nonce,
            .child_graph_nonce = candidate->graph_nonce, .xid = xid,
            .parent_graph_nonce = parent->graph_nonce, .parent_n_slots = parent->n_slots,
            .parent_folded = parent->folded, .parent_dead_slots = parent->dead_slots,
            .parent_last_xid = parent->last_fold_xid, .positions = candidate->positions,
        };
        reserved = true;
    }

done:
    LWLockRelease(&history->lock);
    return reserved;
}

static bool
read_fold_log_page(Relation index, BlockNumber block, CuvsCagraFoldPage *page_tag,
                   CuvsCagraFoldRecord *last, CuvsCagraFoldRecord *previous,
                   CuvsCagraFoldRecord *all)
{
    Buffer buf = ReadBuffer(index, block);
    Page page;
    const PageHeaderData *header;
    bool valid = false;

    LockBuffer(buf, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buf);
    header = (const PageHeaderData *) page;
    if (!PageIsNew(page) &&
        header->pd_special == BLCKSZ - MAXALIGN(sizeof(CuvsCagraFoldPage)) &&
        header->pd_lower <= header->pd_upper && header->pd_upper <= header->pd_special)
    {
        const CuvsCagraFoldPage *tag =
            (const CuvsCagraFoldPage *) PageGetSpecialPointer(page);

        if (tag->kind == CUVS_PAGE_FOLDLOG && !tag->flags &&
            tag->count > 0 && tag->count <= CUVS_CAGRA_FOLD_RECORDS_PER_PAGE &&
            header->pd_lower == MAXALIGN(SizeOfPageHeaderData) +
                tag->count * sizeof(CuvsCagraFoldRecord))
        {
            const CuvsCagraFoldRecord *records =
                (const CuvsCagraFoldRecord *) PageGetContents(page);

            *page_tag = *tag;
            if (last)
                *last = records[tag->count - 1];
            if (previous && tag->count > 1)
                *previous = records[tag->count - 2];
            if (all)
                memcpy(all, records, tag->count * sizeof(*records));
            valid = true;
        }
    }
    UnlockReleaseBuffer(buf);
    return valid;
}

static bool
read_fold_log_end(Relation index, BlockNumber block, CuvsCagraFoldPage *page_tag,
                  CuvsCagraFoldRecord *last, CuvsCagraFoldRecord *previous)
{
    return read_fold_log_page(index, block, page_tag, last, previous, NULL);
}

#ifdef CUVS_CAGRA_FOLD_ENABLED
bool
cuvs_cagra_history_read_fold_log_page(Relation index, BlockNumber block, CuvsCagraFoldPage *tag,
                                      CuvsCagraFoldRecord *records)
{
    return read_fold_log_page(index, block, tag, NULL, NULL, records);
}
#endif

/* Caller holds the page-0 fence. Fold log pages only grow at EOF. */
static bool
fold_log_matches_transition(Relation index, const CuvsCagraMeta *meta,
                            const CuvsCagraFoldCandidate *candidate)
{
    BlockNumber blocks = RelationGetNumberOfBlocks(index);
    CuvsCagraFoldPage tag;
    CuvsCagraFoldRecord child, parent;

    if (!meta->foldlog_head || meta->foldlog_head > meta->foldlog_tail ||
        meta->foldlog_tail >= blocks ||
        !read_fold_log_end(index, meta->foldlog_tail, &tag, &child, &parent) ||
        BlockNumberIsValid(tag.next) || child.graph_nonce != meta->graph_nonce ||
        child.n_slots != meta->n_slots || child.folded != meta->folded)
        return false;

    if (tag.count == 1)
    {
        BlockNumber block = meta->foldlog_head;

        if (block == meta->foldlog_tail)
            return false;
        for (;;)
        {
            CHECK_FOR_INTERRUPTS();
            if (!read_fold_log_end(index, block, &tag, &parent, NULL) ||
                tag.count != CUVS_CAGRA_FOLD_RECORDS_PER_PAGE ||
                tag.next <= block || tag.next > meta->foldlog_tail)
                return false;
            if (tag.next == meta->foldlog_tail)
                break;
            block = tag.next;
        }
    }
    return parent.graph_nonce == candidate->parent_graph_nonce &&
        parent.n_slots == candidate->parent_n_slots && parent.folded == candidate->parent_folded;
}

bool
cuvs_cagra_history_transition_lookup(Relation index, const CuvsCagraFoldCandidate *candidate,
                                     uint32 *transition_xid)
{
    CuvsCagraMeta meta;
    FullTransactionId xid;
    CuvsCagraHistoryRecord *record;
    bool missing = false;
    bool found = false;

    *transition_xid = InvalidTransactionId;
    if (!history || !candidate->positions || !candidate->graph_nonce ||
        !candidate->parent_graph_nonce ||
        candidate->parent_graph_nonce == candidate->graph_nonce ||
        !ConditionalLockPage(index, 0, ShareLock))
        return false;

    if (!cuvs_cagra_read_meta(index, &meta) || meta.index_nonce != candidate->index_nonce ||
        meta.base_nonce != candidate->base_nonce || meta.graph_nonce != candidate->graph_nonce ||
        candidate->positions > meta.folded ||
        candidate->parent_folded != meta.folded - candidate->positions ||
        candidate->parent_n_slots != meta.n_slots - candidate->positions)
        goto done;

    xid = FullTransactionIdFromU64(meta.last_fold_xid);
    if (!TransactionIdIsNormal(XidFromFullTransactionId(xid)) ||
        !FullTransactionIdPrecedes(xid, ReadNextFullTransactionId()))
        goto done;

    LWLockAcquire(&history->lock, LW_SHARED);
    record = find_transition(RelationGetRelid(index), candidate->index_nonce,
                             candidate->base_nonce, candidate->graph_nonce);
    missing = record == NULL;
    found = transition_matches(record, candidate) && FullTransactionIdEquals(record->xid, xid);
    LWLockRelease(&history->lock);

    /* The caller already proved and flushed this publication's LSN. Use the
     * WAL-backed parent only if the in-memory edge is absent. */
    if (missing)
        found = fold_log_matches_transition(index, &meta, candidate);
    if (found)
        *transition_xid = XidFromFullTransactionId(xid);

done:
    UnlockPage(index, 0, ShareLock);
#ifdef CUVS_TEST_HOOKS
    if (found)
        elog(LOG, "[h6_history_ack_xid] db=%u index=%u graph_nonce=" UINT64_FORMAT
             " full_xid=" UINT64_FORMAT " xid=%u source=%s",
             MyDatabaseId, RelationGetRelid(index), meta.graph_nonce, meta.last_fold_xid,
             *transition_xid, missing ? "wal" : "history");
#endif
    return found;
}

void
cuvs_cagra_history_acknowledged(Oid index_oid, const CuvsCagraFoldCandidate *candidate,
                               uint32 transition_xid)
{
    CuvsCagraHistoryRecord *record;

    if (!history)
        return;
    LWLockAcquire(&history->lock, LW_EXCLUSIVE);
    record = find_transition(index_oid, candidate->index_nonce, candidate->base_nonce,
                             candidate->graph_nonce);
    if (transition_matches(record, candidate) &&
        XidFromFullTransactionId(record->xid) == transition_xid)
        record->acknowledged = true;
    LWLockRelease(&history->lock);
}

static int
history_oid_cmp(const void *a, const void *b)
{
    Oid first = *(const Oid *) a;
    Oid second = *(const Oid *) b;

    return (first > second) - (first < second);
}

/* A full registry can reclaim other indexes in this database. Resolve each
 * OID under a relation lock before applying its own heap horizon. */
void
cuvs_cagra_history_collect_database(Oid cagra_am, Snapshot snapshot)
{
    Oid indexes[CUVS_CAGRA_HISTORY_RECORDS];
    int count = 0;

    if (!history || !OidIsValid(cagra_am) || !has_live_snapshot(snapshot))
        return;

    LWLockAcquire(&history->lock, LW_SHARED);
    for (int i = 0; i < CUVS_CAGRA_HISTORY_RECORDS; ++i)
    {
        const CuvsCagraHistoryRecord *record = &history->records[i];

        if (!OidIsValid(record->db_oid))
        {
            LWLockRelease(&history->lock);
            return;
        }
        if (record->db_oid == MyDatabaseId)
            indexes[count++] = record->index_oid;
    }
    LWLockRelease(&history->lock);

    qsort(indexes, count, sizeof(*indexes), history_oid_cmp);
    for (int i = 0; i < count; ++i)
    {
        Oid oid = indexes[i];
        Relation index;
        bool reclaimed = false;

        CHECK_FOR_INTERRUPTS();
        if ((i > 0 && oid == indexes[i - 1]) || !ConditionalLockRelationOid(oid, AccessShareLock))
            continue;
        index = try_relation_open(oid, NoLock);
        if (!index)
        {
            LWLockAcquire(&history->lock, LW_EXCLUSIVE);
            for (int j = 0; j < CUVS_CAGRA_HISTORY_RECORDS; ++j)
            {
                CuvsCagraHistoryRecord *record = &history->records[j];

                if (record->db_oid == MyDatabaseId && record->index_oid == oid)
                {
                    *record = (CuvsCagraHistoryRecord) {0};
                    reclaimed = true;
                }
            }
            LWLockRelease(&history->lock);
        }
        else
        {
            if (index->rd_rel->relkind == RELKIND_INDEX && index->rd_rel->relam == cagra_am &&
                index->rd_index && index->rd_index->indisvalid && index->rd_index->indisready &&
                index->rd_index->indislive && RelationNeedsWAL(index) &&
                ConditionalLockPage(index, 0, ExclusiveLock))
            {
                CuvsCagraMeta current;

                if (cuvs_cagra_has_pages(index) && cuvs_cagra_read_meta(index, &current))
                {
                    FullTransactionId oldest = index_horizon(index, snapshot);

                    LWLockAcquire(&history->lock, LW_EXCLUSIVE);
                    reclaimed = collect_index(index, &current, oldest);
                    LWLockRelease(&history->lock);
                }
                UnlockPage(index, 0, ExclusiveLock);
            }
            relation_close(index, NoLock);
        }
        UnlockRelationOid(oid, AccessShareLock);
        if (reclaimed)
            return;
    }
}


#ifdef CUVS_TEST_HOOKS
#include "access/genam.h"
#include "access/xact.h"
#include "commands/defrem.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/xid8.h"

PG_FUNCTION_INFO_V1(pg_cuvs_test_h6_history_fill);
PG_FUNCTION_INFO_V1(pg_cuvs_test_h6_history_fill_unacknowledged);
PG_FUNCTION_INFO_V1(pg_cuvs_test_h6_history_state);

static void
require_h6_history(void)
{
    if (!superuser())
        ereport(ERROR, (errmsg("H6 history fixture requires a superuser")));
    if (!history || !cuvs_page_resident_search || RecoveryInProgress() || !OidIsValid(MyDatabaseId))
        ereport(ERROR, (errmsg("H6 history fixture requires resident search on a preloaded primary")));
}

static FullTransactionId
h6_model_xid(FullTransactionId supplied, FullTransactionId next)
{
    FullTransactionId normal;

    if (!history_full_xid(next, XidFromFullTransactionId(supplied), &normal) ||
        !FullTransactionIdEquals(supplied, normal) || !FullTransactionIdPrecedes(normal, next))
        ereport(ERROR, (errmsg("H6 history fixture requires a normal, assigned model FullXID")));
    return normal;
}

static Datum
h6_history_fill(FunctionCallInfo fcinfo, bool acknowledged)
{
    CuvsCagraHistoryRecord model;
    CuvsCagraHistoryRecord *record;
    CuvsCagraMeta meta;
    FullTransactionId xid;
    Oid index_oid;
    Relation index;
    int used = 0;
    bool matched;

    require_h6_history();
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2) ||
        PG_GETARG_INT32(2) != CUVS_CAGRA_HISTORY_RECORDS)
        ereport(ERROR, (errmsg("H6 history fixture requires an index, a model FullXID and capacity %d",
                               CUVS_CAGRA_HISTORY_RECORDS)));
    if (XactReadOnly || IsTransactionBlock() || IsSubTransaction())
        ereport(ERROR, (errmsg("H6 history fill requires an autocommit write statement")));

    index_oid = PG_GETARG_OID(0);
    xid = h6_model_xid(PG_GETARG_FULLTRANSACTIONID(1), ReadNextFullTransactionId());
    index = index_open(index_oid, AccessShareLock);
    if (index->rd_rel->relkind != RELKIND_INDEX || index->rd_rel->relam != get_am_oid("cagra", true) ||
        !index->rd_index->indisvalid || !index->rd_index->indisready || !index->rd_index->indislive ||
        !RelationNeedsWAL(index) || !cuvs_cagra_has_pages(index))
        ereport(ERROR, (errmsg("H6 history fixture requires a valid WAL-logged CAGRA page index")));

    LockPage(index, 0, ShareLock);
    if (!cuvs_cagra_read_meta(index, &meta) || !meta.folded ||
        meta.last_fold_xid != U64FromFullTransactionId(xid))
        ereport(ERROR, (errmsg("H6 fixture FullXID does not match the physical fold metapage")));

    LWLockAcquire(&history->lock, LW_EXCLUSIVE);
    record = find_transition(index_oid, meta.index_nonce, meta.base_nonce, meta.graph_nonce);
    matched = record && record->acknowledged == acknowledged &&
        FullTransactionIdEquals(record->xid, xid) &&
        record->parent_graph_nonce && record->parent_graph_nonce != meta.graph_nonce &&
        record->positions > 0 && record->positions <= meta.folded &&
        record->parent_folded == meta.folded - record->positions &&
        record->parent_n_slots == meta.n_slots - record->positions;
    if (matched)
    {
        for (int i = 0; i < CUVS_CAGRA_HISTORY_RECORDS; ++i)
            used += OidIsValid(history->records[i].db_oid);

        if (used == 1)
        {
            model = *record;
            /* InvalidOid is an explicit synthetic marker. The owner DB and
             * the real committed FullXID remain intact, so ProcArray horizons
             * protect and expire these records exactly as they do the model. */
            model.index_oid = InvalidOid;
            for (int i = 0; i < CUVS_CAGRA_HISTORY_RECORDS; ++i)
                if (!OidIsValid(history->records[i].db_oid))
                    history->records[i] = model;
        }
    }
    LWLockRelease(&history->lock);
    UnlockPage(index, 0, ShareLock);
    index_close(index, AccessShareLock);

    if (!matched)
        ereport(ERROR, (errmsg("H6 fixture model must have a %s resident ACK for its physical transition",
                               acknowledged ? "confirmed" : "pending")));
    if (used != 1)
        ereport(ERROR, (errmsg("H6 history fill requires the model to be the only occupied slot")));

    PG_RETURN_INT32(CUVS_CAGRA_HISTORY_RECORDS - 1);
}

Datum
pg_cuvs_test_h6_history_fill(PG_FUNCTION_ARGS)
{
    return h6_history_fill(fcinfo, true);
}

Datum
pg_cuvs_test_h6_history_fill_unacknowledged(PG_FUNCTION_ARGS)
{
    return h6_history_fill(fcinfo, false);
}

Datum
pg_cuvs_test_h6_history_state(PG_FUNCTION_ARGS)
{
    Oid owner;
    Relation heap;
    FullTransactionId xid, next, shared, local;
    TransactionId shared_xid, local_xid;
    int used = 0;
    int source_real = 0, source_synthetic = 0, source_acknowledged = 0;
    int target = 0, target_acknowledged = 0;

    require_h6_history();
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2) ||
        !OidIsValid(PG_GETARG_OID(1)) || PG_GETARG_OID(1) == MyDatabaseId)
        ereport(ERROR, (errmsg("H6 history state requires a local heap and a different source DB and FullXID")));

    owner = PG_GETARG_OID(1);
    heap = relation_open(PG_GETARG_OID(0), AccessShareLock);
    if (!history_heap(heap))
        ereport(ERROR, (errmsg("H6 history state requires a WAL-logged heap")));
    shared_xid = GetOldestNonRemovableTransactionId(NULL);
    local_xid = GetOldestNonRemovableTransactionId(heap);
    next = ReadNextFullTransactionId();
    xid = h6_model_xid(PG_GETARG_FULLTRANSACTIONID(2), next);
    if (!history_full_xid(next, shared_xid, &shared) || !history_full_xid(next, local_xid, &local))
        ereport(ERROR, (errmsg("H6 history state could not identify both horizons")));
    relation_close(heap, AccessShareLock);

    LWLockAcquire(&history->lock, LW_SHARED);
    for (int i = 0; i < CUVS_CAGRA_HISTORY_RECORDS; ++i)
    {
        const CuvsCagraHistoryRecord *record = &history->records[i];

        if (!OidIsValid(record->db_oid))
            continue;
        ++used;
        if (record->db_oid == owner && FullTransactionIdEquals(record->xid, xid))
        {
            if (OidIsValid(record->index_oid))
                ++source_real;
            else
                ++source_synthetic;
            source_acknowledged += record->acknowledged;
        }
        if (record->db_oid == MyDatabaseId)
        {
            ++target;
            target_acknowledged += record->acknowledged;
        }
    }
    LWLockRelease(&history->lock);

    PG_RETURN_TEXT_P(cstring_to_text(psprintf(
        "{\"capacity\":%d,\"used\":%d,\"source_real\":%d,\"source_synthetic\":%d,"
        "\"source_acknowledged\":%d,\"target\":%d,\"target_acknowledged\":%d,"
        "\"shared_oldest\":" UINT64_FORMAT ",\"local_oldest\":" UINT64_FORMAT "}",
        CUVS_CAGRA_HISTORY_RECORDS, used, source_real, source_synthetic, source_acknowledged,
        target, target_acknowledged, U64FromFullTransactionId(shared), U64FromFullTransactionId(local))));
}
#endif

#endif
