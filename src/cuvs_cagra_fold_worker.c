#include "postgres.h"

#ifdef CUVS_PAGE_FORMAT
#include "access/relation.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/pg_class_d.h"
#include "commands/defrem.h"
#include "cuvs_cagra_pages.h"
#include "cuvs_ipc.h"
#include "fmgr.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/lmgr.h"
#include "tcop/tcopprot.h"
#include "utils/snapmgr.h"

extern char *cuvs_socket_path;
PGDLLEXPORT void cuvs_cagra_fold_worker_main(Datum arg);

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

    if (!IsBackgroundWorker || !OidIsValid(database) || RecoveryInProgress() ||
        !boot_id || !cuvs_socket_path || !cuvs_socket_path[0])
        return;

    pqsignal(SIGTERM, die);
    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnectionByOid(database, InvalidOid, 0);

    system_identifier = GetSystemIdentifier();
    if (cuvs_ipc_page_fold_ready(cuvs_socket_path, MyDatabaseId,
                                 system_identifier, boot_id, &event) != CUVS_STATUS_OK)
        return;
    CHECK_FOR_INTERRUPTS();

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
    if (!XactReadOnly && ConditionalLockRelationOid(event.index_oid, RowExclusiveLock))
    {
        Relation index = try_relation_open(event.index_oid, NoLock);

        status = CUVS_CAGRA_FOLD_DISCARD;
        if (index)
        {
            if (index->rd_rel->relkind == RELKIND_INDEX &&
                index->rd_rel->relam == get_am_oid("cagra", true) &&
                index->rd_index->indisvalid && index->rd_index->indisready &&
                index->rd_index->indislive && cuvs_cagra_has_pages(index))
                status = cuvs_cagra_publish_fold(index, &candidate, &lsn);
            relation_close(index, NoLock);
        }
    }
    PopActiveSnapshot();
    CommitTransactionCommand();

    CHECK_FOR_INTERRUPTS();
    if (status == CUVS_CAGRA_FOLD_PUBLISHED || status == CUVS_CAGRA_FOLD_DISCARD)
        (void) cuvs_ipc_page_fold_result(cuvs_socket_path, &event, lsn);
}

#ifdef CUVS_TEST_HOOKS
PG_FUNCTION_INFO_V1(cuvs_test_launch_fold_worker);

Datum
cuvs_test_launch_fold_worker(PG_FUNCTION_ARGS)
{
    BackgroundWorker worker = {0};
    BackgroundWorkerHandle *handle;

    if (!superuser())
        ereport(ERROR, (errmsg("the fold worker test requires a superuser")));
    if (!cuvs_cagra_pg_boot_id() || RecoveryInProgress())
        ereport(ERROR, (errmsg("the fold worker test requires a preloaded primary")));

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
    if (!RegisterDynamicBackgroundWorker(&worker, &handle))
        ereport(ERROR, (errmsg("could not start the fold worker test")));
    if (WaitForBackgroundWorkerShutdown(handle) != BGWH_STOPPED)
        ereport(ERROR, (errmsg("the fold worker test did not stop")));
    pfree(handle);
    PG_RETURN_VOID();
}
#endif
#endif
