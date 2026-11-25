#include "postgres.h"

#ifdef CUVS_PAGE_FORMAT
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/pg_class_d.h"
#include "commands/defrem.h"
#include "cuvs_cagra_pages.h"
#include "cuvs_cagra_pending_search.h"
#include "cuvs_ipc.h"
#include "fmgr.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "port/atomics.h"
#include "storage/dsm.h"
#include "storage/lmgr.h"
#include "tcop/tcopprot.h"
#include "utils/snapmgr.h"

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
        ready_rc = cuvs_ipc_page_fold_ready_index(cuvs_socket_path, MyDatabaseId, target,
                                                  scope.index_nonce, system_identifier, boot_id, &event);
    else
#endif
        ready_rc = cuvs_ipc_page_fold_ready(cuvs_socket_path, MyDatabaseId, system_identifier, boot_id, &event);
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
                    status = cuvs_cagra_publish_fold(index, &candidate, &lsn);
#ifdef CUVS_CAGRA_FOLD_ENABLED
                else
                    outcome = fold_supply_index(index, system_identifier, boot_id, scope.index_nonce);
#endif
            }
            relation_close(index, NoLock);
        }
    }
    PopActiveSnapshot();
    CommitTransactionCommand();

    CHECK_FOR_INTERRUPTS();
    if (ready && (status == CUVS_CAGRA_FOLD_PUBLISHED || status == CUVS_CAGRA_FOLD_DISCARD))
    {
        int rc = cuvs_ipc_page_fold_result(cuvs_socket_path, &event, lsn);

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
