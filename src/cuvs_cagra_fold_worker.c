#include "postgres.h"

#ifdef CUVS_PAGE_FORMAT
#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "commands/defrem.h"
#include "cuvs_cagra_history.h"
#include "cuvs_cagra_pages.h"
#include "cuvs_cagra_pending_search.h"
#include "cuvs_ipc.h"
#include "fmgr.h"
#include "nodes/pg_list.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "port/atomics.h"
#include "storage/dsm.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "tcop/tcopprot.h"
#include "utils/snapmgr.h"
#ifdef CUVS_CAGRA_FOLD_ENABLED
#include "cuvs_page_recovery.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include <unistd.h>
#include "utils/memutils.h"
#include "utils/wait_event.h"
#endif

extern char *cuvs_socket_path;
PGDLLEXPORT void cuvs_cagra_fold_worker_main(Datum arg);

#ifdef CUVS_CAGRA_FOLD_ENABLED
typedef struct CuvsFoldWorkerScope
{
    Oid index_oid;
    uint64 index_nonce;
    dsm_handle report;
} CuvsFoldWorkerScope;

typedef enum CuvsFoldWorkerReport
{
    FOLD_WORKER_FAILED,
    FOLD_WORKER_IDLE,
    FOLD_WORKER_PENDING,
    FOLD_WORKER_DEFERRED,
    FOLD_WORKER_DISCARDED,
    FOLD_WORKER_PUBLISHED,
    FOLD_WORKER_ACK_PENDING,
    FOLD_WORKER_UNAVAILABLE,
    FOLD_WORKER_NO_GRAPH,
    FOLD_WORKER_HOST_BUDGET,
} CuvsFoldWorkerReport;

StaticAssertDecl(sizeof(CuvsFoldWorkerScope) <= BGW_EXTRALEN, "fold worker scope");

/* Supply only a contiguous, committed prefix, using the same page and heap
 * readers as a search. An in-progress or invisible TID stops the prefix. */
static CuvsFoldWorkerReport
fold_supply_index(Relation index, uint64 system_identifier, uint64 boot_id,
                   uint64 index_nonce)
{
    CuvsCagraMeta meta;
    CuvsPageOverlayRequest request = {.boot_id = boot_id};
    CuvsPageProgress progress;
    uint64 tail, from;
    BlockNumber blocks = RelationGetNumberOfBlocks(index);
    Oid index_oid = RelationGetRelid(index);
    Oid heap_oid = index->rd_index->indrelid;
    CuvsFoldWorkerReport result = FOLD_WORKER_IDLE;
    Relation heap;
    int rc;

    if (!RelationNeedsWAL(index) || !cuvs_cagra_read_meta(index, &meta) ||
        meta.index_nonce != index_nonce)
        return FOLD_WORKER_UNAVAILABLE;
    if (!meta.graph_nonce || !meta.n_slots)
        return FOLD_WORKER_NO_GRAPH;
    if (!cuvs_cagra_read_tail(index, &meta, blocks, &tail) || tail <= meta.folded)
        return result;

    request.base = (CuvsPageSearchRequest) {
        .system_identifier = system_identifier, .index_nonce = meta.index_nonce,
        .base_nonce = meta.base_nonce, .graph_nonce = meta.graph_nonce,
        .n_slots = meta.n_slots, .folded = meta.folded, .tail = meta.folded,
        .dead_slots = meta.dead_slots,
    };
    request.from = meta.folded;
    for (uint64 slot = 0; slot < meta.n_slots && slot < UINT32_MAX; ++slot)
    {
        ItemPointerData tid;
        CuvsCagraSlotResult state;

        CHECK_FOR_INTERRUPTS();
        state = cuvs_cagra_slot_tid(index, &meta, blocks, slot, &tid);
        if (state == CUVS_CAGRA_SLOT_BAD)
            break;
        if (state == CUVS_CAGRA_SLOT_LIVE)
        {
            request.base.reserved = (uint32) slot + 1;
            break;
        }
    }
    rc = cuvs_ipc_query_page_supply(cuvs_socket_path, MyDatabaseId, index_oid, &request,
                                     meta.dim, meta.metric, &progress);
    if (rc != CUVS_STATUS_OK || progress.covered_upto < meta.folded)
        return FOLD_WORKER_UNAVAILABLE;

    from = progress.covered_upto;
    if (from > meta.folded)
        result = FOLD_WORKER_PENDING;
    if (from >= tail)
        return result;
    if (!ConditionalLockRelationOid(heap_oid, AccessShareLock))
        return FOLD_WORKER_DEFERRED;
    heap = table_open(heap_oid, NoLock);
    /* A single invocation feeds at most one batch. Repeated calls can keep
     * supplying while the daemon's candidate is in flight. */
    CuvsPageSupplement supplement = {0};
    uint64 upto = from + Min(tail - from, (uint64) CUVS_PAGE_BATCH_MAX);

    if (cuvs_cagra_fold_supplement(index, heap, GetActiveSnapshot(), &meta,
                                   blocks, from, upto, &supplement) && supplement.positions)
    {
        request.from = from;
        request.base.tail = from + supplement.positions;
        rc = cuvs_ipc_supply_pages(cuvs_socket_path, MyDatabaseId, index_oid, &request,
                                    meta.dim, meta.metric, &supplement, &progress);
        if (rc == CUVS_STATUS_PAGE_OVERLAY_HOST_FULL)
            result = FOLD_WORKER_HOST_BUDGET;
        else
            result = rc == CUVS_STATUS_OK ? FOLD_WORKER_PENDING : FOLD_WORKER_DEFERRED;
    }
    cuvs_cagra_free_supplement(&supplement);
    table_close(heap, AccessShareLock);
    return result;
}
#endif

/*
 * Process one event in the requested database. The daemon keeps it available
 * until its exact result is received, including after worker exit.
 */
void
cuvs_cagra_fold_worker_main(Datum arg)
{
    Oid database = DatumGetObjectId(arg);
    uint64 system_identifier;
    uint64 boot_id = cuvs_cagra_pg_boot_id();
    CuvsPageFoldEvent event;
    CuvsCagraFoldCandidate candidate;
    CuvsCagraFoldStatus status = CUVS_CAGRA_FOLD_DEFERRED;
    XLogRecPtr lsn = InvalidXLogRecPtr;
    uint32 transition_xid = InvalidTransactionId;
    Oid target = InvalidOid;
    bool ready;
#ifdef CUVS_CAGRA_FOLD_ENABLED
    CuvsFoldWorkerScope scope;
    pg_atomic_uint32 *report = NULL;
    CuvsFoldWorkerReport outcome = FOLD_WORKER_DEFERRED;
#endif

    if (!IsBackgroundWorker || !OidIsValid(database) || RecoveryInProgress() ||
        !boot_id || !cuvs_socket_path || !cuvs_socket_path[0])
        return;

    pqsignal(SIGTERM, die);
    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnectionByOid(database, InvalidOid, 0);

#ifdef CUVS_CAGRA_FOLD_ENABLED
    memcpy(&scope, MyBgworkerEntry->bgw_extra, sizeof(scope));
    target = scope.index_oid;
    if (OidIsValid(target))
    {
        dsm_segment *segment = dsm_attach(scope.report);

        if (!segment || !scope.index_nonce)
            return;
        report = dsm_segment_address(segment);
    }
#endif
    system_identifier = GetSystemIdentifier();
    int ready_rc;
#ifdef CUVS_CAGRA_FOLD_ENABLED
    if (report)
        ready_rc = cuvs_page_resident_search ?
            cuvs_ipc_page_fold_resident_ready(cuvs_socket_path, MyDatabaseId, target,
                                               scope.index_nonce, system_identifier, boot_id,
                                               cuvs_cagra_pg_boot_started_us(), &event) :
            cuvs_ipc_page_fold_ready_index(cuvs_socket_path, MyDatabaseId, target,
                                            scope.index_nonce, system_identifier, boot_id, &event);
    else
#endif
        ready_rc = cuvs_page_resident_search ?
            cuvs_ipc_page_fold_resident_ready(cuvs_socket_path, MyDatabaseId, 0, 0,
                                               system_identifier, boot_id,
                                               cuvs_cagra_pg_boot_started_us(), &event) :
            cuvs_ipc_page_fold_ready(cuvs_socket_path, MyDatabaseId, system_identifier, boot_id, &event);
    ready = ready_rc == CUVS_STATUS_OK;
#ifdef CUVS_CAGRA_FOLD_ENABLED
    if (report && ready_rc != CUVS_STATUS_OK && ready_rc != CUVS_STATUS_NOT_FOUND)
    {
        pg_atomic_write_u32(report, FOLD_WORKER_UNAVAILABLE);
        return;
    }
#endif
    if (!ready && !OidIsValid(target))
        return;
    CHECK_FOR_INTERRUPTS();

    if (ready)
        target = event.index_oid;
    if (ready)
        candidate = (CuvsCagraFoldCandidate) {
        .index_nonce = event.index_nonce,
        .base_nonce = event.base_nonce,
        .parent_graph_nonce = event.parent_graph_nonce,
        .parent_n_slots = event.parent_n_slots,
        .parent_folded = event.parent_folded,
        .graph_nonce = event.graph_nonce,
        .positions = event.positions,
    };

    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();
    PushActiveSnapshot(GetTransactionSnapshot());
    if (!XactReadOnly && ConditionalLockRelationOid(target, RowExclusiveLock))
    {
        Relation index = try_relation_open(target, NoLock);

        status = CUVS_CAGRA_FOLD_DISCARD;
        if (index)
        {
            if (index->rd_rel->relkind == RELKIND_INDEX &&
                index->rd_rel->relam == get_am_oid("cagra", true) &&
                index->rd_index->indisvalid && index->rd_index->indisready &&
                index->rd_index->indislive && cuvs_cagra_has_pages(index))
            {
                if (ready)
                {
                    status = cuvs_cagra_publish_fold(index, &candidate, &lsn);
                    if (cuvs_page_resident_search && status == CUVS_CAGRA_FOLD_PUBLISHED)
                        (void) cuvs_cagra_history_transition_lookup(index, &candidate,
                                                                   &transition_xid);
                }
#ifdef CUVS_CAGRA_FOLD_ENABLED
                else
                    outcome = fold_supply_index(index, system_identifier, boot_id, scope.index_nonce);
#endif
            }
            relation_close(index, NoLock);
        }
    }
    if (!XactReadOnly && ready && cuvs_page_resident_search && status == CUVS_CAGRA_FOLD_DEFERRED)
        cuvs_cagra_history_collect_database(get_am_oid("cagra", true), GetActiveSnapshot());
    PopActiveSnapshot();
    CommitTransactionCommand();

    CHECK_FOR_INTERRUPTS();
    if (ready && (status == CUVS_CAGRA_FOLD_PUBLISHED || status == CUVS_CAGRA_FOLD_DISCARD))
    {
        int rc = CUVS_STATUS_NOT_FOUND;

        if (cuvs_page_resident_search && status == CUVS_CAGRA_FOLD_PUBLISHED)
        {
            /* A published page is irreversible. If its original XID could
             * not be proved, leave READY outstanding for another attempt. */
            if (TransactionIdIsNormal(transition_xid))
            {
                rc = cuvs_ipc_page_fold_resident_result(cuvs_socket_path, &event, lsn,
                                                         transition_xid);
                if (rc == CUVS_STATUS_OK)
                    cuvs_cagra_history_acknowledged(target, &candidate, transition_xid);
            }
        }
        else
            rc = cuvs_ipc_page_fold_result(cuvs_socket_path, &event, lsn);

#ifdef CUVS_CAGRA_FOLD_ENABLED
        if (status == CUVS_CAGRA_FOLD_PUBLISHED)
            outcome = rc == CUVS_STATUS_OK ? FOLD_WORKER_PUBLISHED : FOLD_WORKER_ACK_PENDING;
        else
            outcome = FOLD_WORKER_DISCARDED;
#else
        (void) rc;
#endif
    }
#ifdef CUVS_CAGRA_FOLD_ENABLED
    if (report)
        pg_atomic_write_u32(report, outcome);
#endif
}

#if defined(CUVS_TEST_HOOKS) || defined(CUVS_CAGRA_FOLD_ENABLED)
static BackgroundWorker
fold_worker_config(void)
{
    BackgroundWorker worker = {0};

    strlcpy(worker.bgw_name, "pg_cuvs fold worker", sizeof(worker.bgw_name));
    strlcpy(worker.bgw_type, "pg_cuvs fold worker", sizeof(worker.bgw_type));
    strlcpy(worker.bgw_library_name, "pg_cuvs", sizeof(worker.bgw_library_name));
    strlcpy(worker.bgw_function_name, "cuvs_cagra_fold_worker_main",
            sizeof(worker.bgw_function_name));
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    worker.bgw_main_arg = ObjectIdGetDatum(MyDatabaseId);
    worker.bgw_notify_pid = MyProcPid;
    return worker;
}
#endif

#ifdef CUVS_CAGRA_FOLD_ENABLED
PGDLLEXPORT void cuvs_cagra_recovery_launcher_main(Datum arg);
PGDLLEXPORT void cuvs_cagra_recovery_worker_main(Datum arg);

static bool
recovery_head_matches(Relation index, const CuvsCagraMeta *expected)
{
    CuvsCagraMeta current;

    return cuvs_cagra_read_meta(index, &current) &&
        current.index_nonce == expected->index_nonce && current.base_nonce == expected->base_nonce &&
        current.graph_nonce == expected->graph_nonce && current.n_slots == expected->n_slots &&
        current.folded == expected->folded;
}

static int
recovery_nonce_cmp(const void *a, const void *b)
{
    const CuvsPageRecoveryHead *left = a, *right = b;

    return (left->graph_nonce > right->graph_nonce) - (left->graph_nonce < right->graph_nonce);
}

static void
reconcile_index_checkpoints(Relation index, const CuvsCagraMeta *meta,
                            CuvsPageRecoveryRequest request, const char *root,
                            int degree, int intermediate, uint32 algo)
{
    Oid db = MyDatabaseId, oid = RelationGetRelid(index);
    CuvsPageRecoveryHead *lineage = NULL;
    uint32 lineage_count = 0;
    uint64 cursor = 0;

    for (;;)
    {
        CuvsPageCheckpointRef refs[CUVS_PAGE_RECOVERY_BATCH];
        uint32 listed, approved = 0, removed;
        int status;

        CHECK_FOR_INTERRUPTS();
        request.operation = CUVS_PAGE_RECOVERY_LIST;
        request.source.graph_nonce = cursor;
        request.count = 0;
        status = cuvs_ipc_page_reconcile(cuvs_socket_path, db, oid, meta->dim, meta->metric,
                                         degree, intermediate, algo, root, &request, refs, &listed);
        if (status != CUVS_STATUS_OK || !listed || !recovery_head_matches(index, meta))
            break;
        /* The listed files predate the authority check. Publication appends
         * a fresh nonce, so it cannot later adopt any of these rejected heads. */
        if (!lineage)
        {
            XLogRecPtr lsn;

            lineage = cuvs_cagra_read_lineage(index, meta, &lineage_count, &lsn);
            if (!lineage)
                break;
            XLogFlush(lsn);
            qsort(lineage, lineage_count, sizeof(*lineage), recovery_nonce_cmp);
            for (uint32 i = 1; i < lineage_count; ++i)
                if (lineage[i - 1].graph_nonce == lineage[i].graph_nonce)
                    goto done;
        }

        for (uint32 i = 0; i < listed; ++i)
        {
            if (refs[i].head.graph_nonce <= cursor || refs[i].reserved ||
                refs[i].action != CUVS_PAGE_CHECKPOINT_KEEP)
                goto done;
            cursor = refs[i].head.graph_nonce;
        }

        uint64 next = U64FromFullTransactionId(ReadNextFullTransactionId());

        for (uint32 i = 0; i < listed; ++i)
        {
            CuvsPageCheckpointRef *ref = &refs[i];
            const CuvsPageRecoveryHead *record = bsearch(&ref->head, lineage, lineage_count,
                                                          sizeof(*lineage), recovery_nonce_cmp);
            enum CuvsPageCheckpointAction action =
                cuvs_page_checkpoint_action(&ref->head, &request.target, record);

            if (action == CUVS_PAGE_CHECKPOINT_KEEP ||
                !cuvs_page_checkpoint_xid_in_window(ref->build_xid, next) ||
                TransactionIdIsInProgress(XidFromFullTransactionId(FullTransactionIdFromU64(ref->build_xid))))
                continue;
            ref->action = action;
            refs[approved++] = *ref;
        }
        if (approved)
        {
            if (!recovery_head_matches(index, meta))
                break;
            request.operation = CUVS_PAGE_RECOVERY_PRUNE;
            request.count = approved;
            status = cuvs_ipc_page_reconcile(cuvs_socket_path, db, oid, meta->dim, meta->metric,
                                             degree, intermediate, algo, root, &request, refs, &removed);
            if (status != CUVS_STATUS_OK)
                break;
        }
        if (listed < CUVS_PAGE_RECOVERY_BATCH)
            break;
    }

done:
    if (lineage)
        pfree(lineage);
}

static int
authorize_index_recovery(Relation index, const CuvsCagraMeta *meta,
                       CuvsPageRecoveryRequest *request, const char *root,
                       int degree, int intermediate, uint32 algo, File corpus,
                       XLogRecPtr lsn, uint32 physical_dead_slots)
{
    RelFileLocator locator = index->rd_locator;
    CuvsPageRecoveryAuthority authority = {
        .wal_lsn = lsn, .tablespace_oid = locator.spcOid,
        .database_oid = locator.dbOid, .rel_number = locator.relNumber,
        .physical_dead_slots = physical_dead_slots,
    };
    CuvsPageRecoveryOffer offer;
    int connection;
    int status = cuvs_ipc_page_install_begin(cuvs_socket_path, MyDatabaseId, RelationGetRelid(index),
                       meta->dim, meta->metric, degree, intermediate, algo, root, request,
                       corpus < 0 ? -1 : FileGetRawDesc(corpus), &authority, &offer, &connection);

    if (connection < 0)
        return status;
    status = CUVS_STATUS_NOT_FOUND;
    PG_TRY();
    {
        CHECK_FOR_INTERRUPTS();
        if (cuvs_page_recovery_offer_matches(&offer, request, &authority,
                                               MyDatabaseId, RelationGetRelid(index)) &&
            ConditionalLockPage(index, 0, ShareLock))
        {
            /* FCL keeps the WAL head fixed through the registry decision.
             * Destructors, file I/O and GPU preparation run outside this grant. */
            if (!RecoveryInProgress() && cuvs_cagra_pg_boot_id() == offer.boot_id &&
                index->rd_locator.spcOid == locator.spcOid && index->rd_locator.dbOid == locator.dbOid &&
                index->rd_locator.relNumber == locator.relNumber && recovery_head_matches(index, meta))
                status = cuvs_ipc_page_install_confirm(connection, &offer);
            UnlockPage(index, 0, ShareLock);
        }
    }
    PG_FINALLY();
    {
        close(connection);
    }
    PG_END_TRY();
    return status;
}

/* One attempt on the committed metapage. Use the same current head for
 * selecting an artifact, reading its missing rows, and registering it. */
static void
recover_index(Relation index)
{
    CuvsCagraMeta meta;
    CuvsPageRecoveryRequest request = {
        .length = sizeof(request), .operation = CUVS_PAGE_RECOVERY_CHECK,
        .boot_id = cuvs_cagra_pg_boot_id(), .system_identifier = GetSystemIdentifier(),
    };
    CuvsPageRecoveryHead *lineage;
    uint32 remaining, algo;
    XLogRecPtr lsn;
    int degree, intermediate, status;
    Oid db = MyDatabaseId, oid = RelationGetRelid(index);
    const char *root;
    File corpus = -1;
    Relation heap = NULL;
    bool rehydrate = false, reconcile_due = false;

    if (!RelationNeedsWAL(index) || !cuvs_cagra_has_pages(index) ||
        !cuvs_cagra_read_meta(index, &meta) || !meta.graph_nonce || meta.n_slots < 2)
        return;
    request.index_nonce = meta.index_nonce;
    request.base_nonce = meta.base_nonce;
    request.target = (CuvsPageRecoveryHead) {
        .graph_nonce = meta.graph_nonce, .n_slots = meta.n_slots, .folded = meta.folded,
    };
    root = cuvs_cagra_recovery_options(index, &degree, &intermediate, &algo);
    status = cuvs_ipc_page_recover(cuvs_socket_path, db, oid, meta.dim, meta.metric, degree,
                                   intermediate, algo, root, &request, NULL, -1, NULL, &reconcile_due);
    if (reconcile_due)
        reconcile_index_checkpoints(index, &meta, request, root, degree, intermediate, algo);
    if (status != CUVS_STATUS_NOT_FOUND)
        return;
    lineage = cuvs_cagra_read_lineage(index, &meta, &remaining, &lsn);
    if (!lineage)
        return;
    XLogFlush(lsn);
    /* A previous PG life may hold the only copy that fits the VRAM budget.
     * Confirm its retirement before staging a replacement or selecting a file. */
    request.operation = CUVS_PAGE_RECOVERY_RETIRE_AUTHORIZED;
    status = authorize_index_recovery(index, &meta, &request, root, degree, intermediate,
                                      algo, -1, lsn, 0);
    if (status != CUVS_STATUS_OK)
    {
        pfree(lineage);
        return;
    }

    for (;;)
    {
        uint64 from = 0;
        uint32 physical_dead_slots = 0;

        CHECK_FOR_INTERRUPTS();
        if (remaining)
        {
            uint32 count = Min(remaining, CUVS_PAGE_RECOVERY_BATCH);
            uint32 first = remaining - count;
            CuvsPageRecoveryHead found;
            uint32 i;

            request.operation = CUVS_PAGE_RECOVERY_FIND;
            request.count = count;
            request.placeholder_rows = 0;
            status = cuvs_ipc_page_recover(cuvs_socket_path, db, oid, meta.dim, meta.metric,
                                           degree, intermediate, algo, root, &request,
                                           lineage + first, -1, &found, NULL);
            if (status == CUVS_STATUS_NOT_FOUND)
            {
                if (meta.n_slots - lineage[first].n_slots > CUVS_PAGE_OVERLAY_MAX)
                    remaining = 0;
                else
                    remaining = first;
                continue;
            }
            if (status != CUVS_STATUS_OK)
                break;
            for (i = first; i < remaining; ++i)
                if (cuvs_page_recovery_head_matches(&lineage[i], &found))
                    break;
            if (i == remaining || meta.n_slots - lineage[i].n_slots > CUVS_PAGE_OVERLAY_MAX)
                break;
            request.source = lineage[i];
            remaining = i;
            from = request.source.n_slots;
        }
        else
        {
            request.source = (CuvsPageRecoveryHead) {0};
            request.build_xid = U64FromFullTransactionId(GetTopFullTransactionId());
            rehydrate = true;
        }
        request.operation = CUVS_PAGE_RECOVERY_INSTALL_AUTHORIZED;
        request.count = 0;
        request.placeholder_rows = 0;
        if (from < meta.n_slots)
        {
            Oid heap_oid = index->rd_index->indrelid;
            bool read;

            if (!ConditionalLockRelationOid(heap_oid, ShareUpdateExclusiveLock))
                break;
            heap = table_open(heap_oid, NoLock);
            corpus = OpenTemporaryFile(false);
            read = recovery_head_matches(index, &meta) &&
                cuvs_cagra_spool_recovery(index, heap, &meta, from, corpus, &request.placeholder_rows,
                                          &physical_dead_slots);
            table_close(heap, ShareUpdateExclusiveLock);
            if (!read)
                break;
        }
        if (!recovery_head_matches(index, &meta))
            break;
        status = authorize_index_recovery(index, &meta, &request, root, degree, intermediate,
                                        algo, corpus, lsn, physical_dead_slots);
        if (corpus >= 0)
        {
            FileClose(corpus);
            corpus = -1;
        }
        if (rehydrate || (status != CUVS_STATUS_NOT_FOUND && status != CUVS_STATUS_PAGE_OVERLAY_HOST_FULL))
            break;
    }
    if (corpus >= 0)
        FileClose(corpus);
    pfree(lineage);
}

void
cuvs_cagra_recovery_worker_main(Datum arg)
{
    Oid database = DatumGetObjectId(arg);
    MemoryContext sweep;
    List *indexes = NIL;
    Relation catalog;
    SysScanDesc scan;
    HeapTuple tuple;
    Oid am;

    if (!IsBackgroundWorker || !OidIsValid(database) || RecoveryInProgress() ||
        !cuvs_cagra_pg_boot_id() || !cuvs_socket_path || !cuvs_socket_path[0])
        return;
    pqsignal(SIGTERM, die);
    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnectionByOid(database, InvalidOid, 0);
    sweep = AllocSetContextCreate(TopMemoryContext, "CAGRA recovery sweep", ALLOCSET_SMALL_SIZES);

    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();
    am = get_am_oid("cagra", true);
    if (OidIsValid(am))
    {
        catalog = table_open(RelationRelationId, AccessShareLock);
        scan = systable_beginscan(catalog, InvalidOid, false, NULL, 0, NULL);
        while (HeapTupleIsValid(tuple = systable_getnext(scan)))
        {
            Form_pg_class row = (Form_pg_class) GETSTRUCT(tuple);

            if (row->relkind == RELKIND_INDEX && row->relam == am)
            {
                MemoryContext previous = MemoryContextSwitchTo(sweep);
                indexes = lappend_oid(indexes, row->oid);
                MemoryContextSwitchTo(previous);
            }
        }
        systable_endscan(scan);
        table_close(catalog, AccessShareLock);
    }
    CommitTransactionCommand();

    ListCell *cell;
    foreach(cell, indexes)
    {
        Oid oid = lfirst_oid(cell);

        CHECK_FOR_INTERRUPTS();
        PG_TRY();
        {
            SetCurrentStatementStartTimestamp();
            StartTransactionCommand();
            PushActiveSnapshot(GetTransactionSnapshot());
            if (ConditionalLockRelationOid(oid, AccessShareLock))
            {
                Relation index = try_relation_open(oid, NoLock);

                if (index && index->rd_rel->relkind == RELKIND_INDEX && index->rd_rel->relam == am &&
                    index->rd_index->indisvalid && index->rd_index->indisready && index->rd_index->indislive)
                    recover_index(index);
                if (index)
                    relation_close(index, NoLock);
            }
            PopActiveSnapshot();
            CommitTransactionCommand();
        }
        PG_CATCH();
        {
            MemoryContextSwitchTo(TopMemoryContext);
            FlushErrorState();
            AbortCurrentTransaction();
            elog(DEBUG1, "CAGRA recovery deferred for index %u", oid);
        }
        PG_END_TRY();
    }
    MemoryContextDelete(sweep);
}

#define CUVS_RECOVERY_WORKERS 4

void
cuvs_cagra_recovery_launcher_main(Datum arg)
{
    BackgroundWorkerHandle *running[CUVS_RECOVERY_WORKERS] = {0};
    Oid active[CUVS_RECOVERY_WORKERS] = {0};
    Oid last = InvalidOid;

    (void) arg;
    if (!IsBackgroundWorker || RecoveryInProgress() || !cuvs_cagra_pg_boot_id())
        return;
    pqsignal(SIGTERM, die);
    BackgroundWorkerUnblockSignals();
    /* No database connection: only the shared database catalog is read. */
    BackgroundWorkerInitializeConnection(NULL, NULL, 0);
    for (;;)
    {
        Oid candidates[CUVS_RECOVERY_WORKERS] = {0};
        int available = 0;

        CHECK_FOR_INTERRUPTS();
        for (int i = 0; i < CUVS_RECOVERY_WORKERS; ++i)
        {
            pid_t pid;
            if (running[i] && GetBackgroundWorkerPid(running[i], &pid) == BGWH_STOPPED)
            {
                pfree(running[i]);
                running[i] = NULL;
                active[i] = InvalidOid;
            }
            if (!running[i])
                ++available;
        }
        if (available)
        {
            Relation catalog;
            SysScanDesc scan;
            HeapTuple tuple;

            SetCurrentStatementStartTimestamp();
            StartTransactionCommand();
            catalog = table_open(DatabaseRelationId, AccessShareLock);
            scan = systable_beginscan(catalog, InvalidOid, false, NULL, 0, NULL);
            while (HeapTupleIsValid(tuple = systable_getnext(scan)))
            {
                Form_pg_database row = (Form_pg_database) GETSTRUCT(tuple);
                bool busy = false;

                if (!row->datallowconn)
                    continue;
                for (int i = 0; i < CUVS_RECOVERY_WORKERS; ++i)
                    busy |= active[i] == row->oid;
                if (busy)
                    continue;
                /* Round-robin by OID, wrapping after the last database. */
                Oid next = row->oid;
                for (int i = 0; i < available; ++i)
                    if (!OidIsValid(candidates[i]) ||
                        (next > last && candidates[i] <= last) ||
                        ((next > last) == (candidates[i] > last) && next < candidates[i]))
                    {
                        Oid displaced = candidates[i];
                        candidates[i] = next;
                        if (!OidIsValid(displaced))
                            break;
                        next = displaced;
                    }
            }
            systable_endscan(scan);
            table_close(catalog, AccessShareLock);
            CommitTransactionCommand();

            int next = 0;
            for (int i = 0; i < CUVS_RECOVERY_WORKERS && next < available && OidIsValid(candidates[next]); ++i)
            {
                if (running[i])
                    continue;
                BackgroundWorker worker = fold_worker_config();

                strlcpy(worker.bgw_name, "pg_cuvs CAGRA recovery", sizeof(worker.bgw_name));
                strlcpy(worker.bgw_type, "pg_cuvs CAGRA recovery", sizeof(worker.bgw_type));
                strlcpy(worker.bgw_function_name, "cuvs_cagra_recovery_worker_main",
                        sizeof(worker.bgw_function_name));
                worker.bgw_main_arg = ObjectIdGetDatum(candidates[next]);
                MemoryContext previous = MemoryContextSwitchTo(TopMemoryContext);
                bool registered = RegisterDynamicBackgroundWorker(&worker, &running[i]);

                MemoryContextSwitchTo(previous);
                if (!registered)
                    break;
                active[i] = last = candidates[next++];
            }
        }
        ResetLatch(MyLatch);
        (void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                         1500L, PG_WAIT_EXTENSION);
    }
}

void
cuvs_cagra_register_recovery_launcher(void)
{
    BackgroundWorker worker = fold_worker_config();

    strlcpy(worker.bgw_name, "pg_cuvs CAGRA recovery launcher", sizeof(worker.bgw_name));
    strlcpy(worker.bgw_type, "pg_cuvs CAGRA recovery launcher", sizeof(worker.bgw_type));
    strlcpy(worker.bgw_function_name, "cuvs_cagra_recovery_launcher_main",
            sizeof(worker.bgw_function_name));
    worker.bgw_main_arg = (Datum) 0;
    worker.bgw_notify_pid = 0;
    worker.bgw_restart_time = 5;
    RegisterBackgroundWorker(&worker);
}

void
cuvs_cagra_run_fold_worker(Relation index)
{
    CuvsCagraMeta meta;
    BackgroundWorker worker = fold_worker_config();
    BackgroundWorkerHandle *handle;
    dsm_segment *segment;
    pg_atomic_uint32 *report;
    CuvsFoldWorkerScope scope;
    CuvsFoldWorkerReport outcome;

    if (!cuvs_cagra_pg_boot_id() || RecoveryInProgress())
        ereport(ERROR, (errmsg("CAGRA fold requires a preloaded primary")));
    if (!cuvs_cagra_read_meta(index, &meta))
        ereport(ERROR, (errmsg("could not read the CAGRA metapage")));

    segment = dsm_create(sizeof(*report), DSM_CREATE_NULL_IF_MAXSEGMENTS);
    if (!segment)
    {
        ereport(NOTICE, (errmsg("CAGRA fold deferred because shared memory is busy")));
        return;
    }
    report = dsm_segment_address(segment);
    pg_atomic_init_u32(report, FOLD_WORKER_FAILED);
    scope = (CuvsFoldWorkerScope) {.index_oid = RelationGetRelid(index),
                                 .index_nonce = meta.index_nonce,
                                 .report = dsm_segment_handle(segment)};
    memcpy(worker.bgw_extra, &scope, sizeof(scope));
    if (!RegisterDynamicBackgroundWorker(&worker, &handle))
    {
        dsm_detach(segment);
        ereport(NOTICE, (errmsg("CAGRA fold deferred because no worker slot is available")));
        return;
    }
    PG_TRY();
    {
        if (WaitForBackgroundWorkerShutdown(handle) != BGWH_STOPPED)
            ereport(ERROR, (errmsg("could not wait for the CAGRA fold worker")));
    }
    PG_CATCH();
    {
        TerminateBackgroundWorker(handle);
        PG_RE_THROW();
    }
    PG_END_TRY();
    outcome = pg_atomic_read_u32(report);
    dsm_detach(segment);
    pfree(handle);

    switch (outcome)
    {
        case FOLD_WORKER_IDLE:
            ereport(NOTICE, (errmsg("no eligible CAGRA pending rows to fold")));
            break;
        case FOLD_WORKER_PENDING:
            ereport(NOTICE, (errmsg("CAGRA pending rows supplied or already queued")));
            break;
        case FOLD_WORKER_DEFERRED:
            ereport(NOTICE, (errmsg("CAGRA fold deferred")));
            break;
        case FOLD_WORKER_DISCARDED:
            ereport(NOTICE, (errmsg("CAGRA fold candidate no longer matches the index")));
            break;
        case FOLD_WORKER_PUBLISHED:
            ereport(NOTICE, (errmsg("CAGRA fold published")));
            break;
        case FOLD_WORKER_ACK_PENDING:
            ereport(NOTICE, (errmsg("CAGRA fold published with daemon acknowledgment pending")));
            break;
        case FOLD_WORKER_UNAVAILABLE:
            ereport(NOTICE, (errmsg("CAGRA fold daemon or graph unavailable")));
            break;
        case FOLD_WORKER_NO_GRAPH:
            ereport(NOTICE, (errmsg("CAGRA fold requires an existing base GPU graph")));
            break;
        case FOLD_WORKER_HOST_BUDGET:
            ereport(NOTICE, (errmsg("CAGRA fold supply deferred because the overlay host budget is full")));
            break;
        default:
            ereport(ERROR, (errmsg("CAGRA fold worker exited before reporting an outcome")));
    }
}
#endif

#ifdef CUVS_TEST_HOOKS
PG_FUNCTION_INFO_V1(cuvs_test_launch_fold_worker);

Datum
cuvs_test_launch_fold_worker(PG_FUNCTION_ARGS)
{
    BackgroundWorker worker = fold_worker_config();
    BackgroundWorkerHandle *handle;

    if (!superuser())
        ereport(ERROR, (errmsg("the fold worker test requires a superuser")));
    if (!cuvs_cagra_pg_boot_id() || RecoveryInProgress())
        ereport(ERROR, (errmsg("the fold worker test requires a preloaded primary")));
    if (!RegisterDynamicBackgroundWorker(&worker, &handle))
        ereport(ERROR, (errmsg("could not start the fold worker test")));
    if (WaitForBackgroundWorkerShutdown(handle) != BGWH_STOPPED)
        ereport(ERROR, (errmsg("the fold worker test did not stop")));
    pfree(handle);
    PG_RETURN_VOID();
}
#endif
#endif
