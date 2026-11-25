#include "postgres.h"

#include "cuvs_cagra_resident_supply.h"

#if defined(CUVS_PAGE_FORMAT) && defined(CUVS_CAGRA_FOLD_ENABLED)
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/index.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "commands/defrem.h"
#include "cuvs_cagra_history.h"
#include "cuvs_cagra_pages.h"
#include "cuvs_cagra_pending_search.h"
#ifdef CUVS_TEST_HOOKS
#include "fmgr.h"
#include "utils/fmgrprotos.h"
#endif
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "port/atomics.h"
#include "postmaster/bgworker.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#define SUPPLY_INTERVAL_MS 250
#define SUPPLY_DB_REFRESH_MS 5000
#define SUPPLY_DB_LEASE_MS 1000
#define SUPPLY_STEP_TIMEOUT_MS 250
#define SUPPLY_BASE_PROBE_SLOTS 64U

typedef struct ResidentSupplyDb
{
    Oid oid;
    Oid last_index;
    TimestampTz next_visit;
} ResidentSupplyDb;

typedef struct ResidentSupplyVisit
{
    pg_atomic_uint32 last_index;
    pg_atomic_uint32 has_indexes;
} ResidentSupplyVisit;

static bool resident_supply_enabled;
static BackgroundWorkerHandle *resident_supply_child;

extern char *cuvs_socket_path;
PGDLLEXPORT void cuvs_cagra_resident_supply_launcher(Datum arg);
PGDLLEXPORT void cuvs_cagra_resident_supply_database(Datum arg);

/*
 * Certify only a physically dead line pointer or an aborted insertion at
 * the root itself. A committed xmin with an invisible xmax, a redirect or
 * an unrecognized line pointer supplies no proof for other snapshots.
 */
static bool
pending_root_dead(Relation heap, BlockNumber heap_blocks, ItemPointer tid)
{
    BlockNumber block;
    OffsetNumber offset;
    Buffer buffer;
    Page page;
    bool dead = false;

    if (!ItemPointerIsValid(tid))
        return true;
    block = ItemPointerGetBlockNumber(tid);
    if (block >= heap_blocks)
        return false;

    offset = ItemPointerGetOffsetNumber(tid);
    buffer = ReadBuffer(heap, block);
    LockBuffer(buffer, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buffer);
    if (offset <= PageGetMaxOffsetNumber(page))
    {
        ItemId line = PageGetItemId(page, offset);

        if (ItemIdIsDead(line))
            dead = true;
        else if (ItemIdIsNormal(line) && ItemIdGetLength(line) >= SizeofHeapTupleHeader)
        {
            HeapTupleHeader tuple = (HeapTupleHeader) PageGetItem(page, line);
            TransactionId xmin = HeapTupleHeaderGetRawXmin(tuple);

            dead = TransactionIdIsNormal(xmin) &&
                !(tuple->t_infomask & HEAP_MOVED) &&
                !(tuple->t_infomask2 & HEAP_HOT_UPDATED) &&
                !HeapTupleHeaderIsHeapOnly(tuple) && !HeapTupleHeaderIsSpeculative(tuple) &&
                !HeapTupleHeaderXminCommitted(tuple) &&
                (HeapTupleHeaderXminInvalid(tuple) || TransactionIdDidAbort(xmin));
        }
    }
    UnlockReleaseBuffer(buffer);
    return dead;
}

static bool
resident_supplement(Relation index, Relation heap, Snapshot snapshot, const CuvsCagraMeta *meta,
                    BlockNumber blocks, uint64 from, uint64 end, CuvsPageSupplement *supplement)
{
    CuvsCagraPendingLocation blocked;
    CuvsCagraPendingLocation *locations;
    CuvsPageSupplement local = {0};
    CuvsPagePosition *marks;
    BlockNumber heap_blocks;
    uint64 next;
    uint32 positions = 0, vectors = 0;

    if (!cuvs_cagra_fold_supplement(index, heap, snapshot, meta, blocks, from, end, supplement))
        return false;
    next = from + supplement->positions;
    if (next == end)
        return true;

    /* Keep the fold reader's normal path cheap, including a blocked writer. */
    heap_blocks = RelationGetNumberOfBlocks(heap);
    if (!cuvs_cagra_copy_pending(index, meta, blocks, next, next + 1, &blocked, NULL) ||
        !pending_root_dead(heap, heap_blocks, &blocked.tid))
        return true;

    /* Reuse the snapshot reader's HOT, TOAST and packed-vector handling.
     * Recheck every invisible mark before admitting it to the shared prefix. */
    locations = palloc((end - from) * sizeof(*locations));
    if (!cuvs_cagra_copy_pending(index, meta, blocks, from, end, locations, NULL) ||
        !cuvs_cagra_local_supplement(index, heap, snapshot, meta, blocks, from, end, &local))
    {
        pfree(locations);
        return true;
    }

    /* The local reader allocated these marks in this backend. */
    marks = (CuvsPagePosition *) local.marks;
    for (; positions < local.positions; ++positions)
    {
        CHECK_FOR_INTERRUPTS();
        if (marks[positions].state == CUVS_PAGE_POSITION_INVISIBLE)
        {
            if (!pending_root_dead(heap, heap_blocks, &locations[positions].tid))
                break;
            marks[positions] = (CuvsPagePosition) {.state = CUVS_PAGE_POSITION_DEAD};
        }
        if (marks[positions].state == CUVS_PAGE_POSITION_VECTOR)
            ++vectors;
    }
    pfree(locations);
    cuvs_cagra_free_supplement(supplement);
    local.positions = positions;
    local.vector_rows = vectors;
    *supplement = local;
    return true;
}

int
cuvs_cagra_resident_supply_step(Relation index, Relation heap, Snapshot snapshot,
                               uint64 limit, CuvsCagraResidentSupplyProgress *result)
{
    CuvsCagraMeta meta;
    CuvsPageOverlayRequest request = {.boot_id = cuvs_cagra_pg_boot_id()};
    CuvsPageProgress initial, supplied;
    CuvsPageSupplement supplement = {0};
    Oid index_oid = RelationGetRelid(index);
    BlockNumber blocks;
    uint64 tail, end, from, cap;
    int status;

    *result = (CuvsCagraResidentSupplyProgress) {0};
    /* An MVCC snapshot can see its own transaction's uncommitted writes. */
    if (TransactionIdIsValid(GetTopTransactionIdIfAny()))
        return CUVS_STATUS_ERROR;
    if (!request.boot_id || RecoveryInProgress() || !cuvs_socket_path || !cuvs_socket_path[0] ||
        !snapshot || !IsMVCCSnapshot(snapshot) ||
        index->rd_rel->relkind != RELKIND_INDEX ||
        index->rd_rel->relam != get_am_oid("cagra", true) ||
        !RelationNeedsWAL(index) ||
        !index->rd_index->indisvalid || !index->rd_index->indisready ||
        !index->rd_index->indislive || index->rd_index->indrelid != RelationGetRelid(heap) ||
        heap->rd_rel->relam != HEAP_TABLE_AM_OID || !RelationNeedsWAL(heap))
        return CUVS_STATUS_NOT_FOUND;

    blocks = RelationGetNumberOfBlocks(index);
    if (!blocks || !cuvs_cagra_read_meta(index, &meta) || !meta.dim ||
        !meta.graph_nonce || !meta.n_slots || meta.dead_slots > meta.n_slots ||
        (meta.last_fold_xid && XidInMVCCSnapshot((TransactionId) meta.last_fold_xid, snapshot)) ||
        !cuvs_cagra_read_tail(index, &meta, blocks, &tail) || tail < meta.folded)
        return CUVS_STATUS_NOT_FOUND;
    if (limit != UINT64_MAX && limit > tail)
        return CUVS_STATUS_ERROR;

    request.base = (CuvsPageSearchRequest) {
        .system_identifier = GetSystemIdentifier(),
        .index_nonce = meta.index_nonce, .base_nonce = meta.base_nonce,
        .graph_nonce = meta.graph_nonce, .n_slots = meta.n_slots,
        .folded = meta.folded, .tail = meta.folded, .dead_slots = meta.dead_slots,
    };
    request.from = meta.folded;
    if (cuvs_page_resident_search)
    {
        uint32 horizon = cuvs_cagra_history_horizon(heap, snapshot);

        if (horizon)
        {
            status = cuvs_ipc_page_history_prune(cuvs_socket_path, MyDatabaseId, index_oid,
                                                 &request, horizon,
                                                 cuvs_cagra_pg_boot_started_us());
            if (status == CUVS_STATUS_CANCELED)
                CHECK_FOR_INTERRUPTS();
        }
    }
    if (tail == meta.folded)
        return CUVS_STATUS_NOT_FOUND;
    /* A fold can use a live base vector to fill an all-dead group. Bound
     * this optional search so a mostly dead graph does not dominate a visit. */
    if (meta.dead_slots < meta.n_slots)
    {
        for (uint64 slot = 0; slot < Min(meta.n_slots, SUPPLY_BASE_PROBE_SLOTS); ++slot)
        {
            ItemPointerData tid;
            CuvsCagraSlotResult state = cuvs_cagra_slot_tid(index, &meta, blocks, slot, &tid);

            if (state == CUVS_CAGRA_SLOT_BAD)
                return CUVS_STATUS_NOT_FOUND;
            if (state == CUVS_CAGRA_SLOT_LIVE)
            {
                request.base.reserved = (uint32) slot + 1;
                break;
            }
        }
    }

    /* A backlog larger than the cap can still feed the next fold. */
    cap = meta.folded + CUVS_PAGE_OVERLAY_MAX;
    end = Min(Min(tail, cap), limit);
    status = cuvs_ipc_query_page_supply(cuvs_socket_path, MyDatabaseId, index_oid, &request,
                                       meta.dim, meta.metric, &initial);
    if (status == CUVS_STATUS_CANCELED)
        CHECK_FOR_INTERRUPTS();
    if (status != CUVS_STATUS_OK)
        return status;
    from = initial.covered_upto;
    if (!initial.daemon_id || from < meta.folded || from > cap)
        return CUVS_STATUS_ERROR;
    request.from = request.base.tail = from;
    result->request = request;
    result->progress = initial;
    if (from >= end)
        return CUVS_STATUS_OK;

    end = from + Min(end - from, (uint64) CUVS_PAGE_BATCH_MAX);
    if (!resident_supplement(index, heap, snapshot, &meta, blocks, from, end, &supplement))
        return CUVS_STATUS_ERROR;
    if (!supplement.positions)
    {
        cuvs_cagra_free_supplement(&supplement);
        return CUVS_STATUS_OK;
    }
    request.base.tail = from + supplement.positions;
    status = cuvs_ipc_supply_pages(cuvs_socket_path, MyDatabaseId, index_oid, &request,
                                 meta.dim, meta.metric, &supplement, &supplied);
    cuvs_cagra_free_supplement(&supplement);
    if (status == CUVS_STATUS_CANCELED)
        CHECK_FOR_INTERRUPTS();
    if (status != CUVS_STATUS_OK)
        return status;
    if (supplied.daemon_id != initial.daemon_id ||
        supplied.covered_upto < request.base.tail || supplied.covered_upto > cap)
        return CUVS_STATUS_ERROR;
    result->request = request;
    result->progress = supplied;
    return CUVS_STATUS_OK;
}

/*
 * Discovery gives us an OID only. Resolve the parent before locking and check
 * the pairing again under both locks to handle a concurrent DROP or REINDEX.
 */
static int
supply_locked_index(Oid index_oid, uint64 limit, CuvsCagraResidentSupplyProgress *result)
{
    Oid heap_oid = IndexGetRelation(index_oid, true);
    int status = CUVS_STATUS_NOT_FOUND;

    if (OidIsValid(heap_oid) &&
        ConditionalLockRelationOid(heap_oid, AccessShareLock) &&
        ConditionalLockRelationOid(index_oid, AccessShareLock))
    {
        Relation heap = try_relation_open(heap_oid, NoLock);
        Relation index = heap ? try_relation_open(index_oid, NoLock) : NULL;

        if (index)
        {
            status = cuvs_cagra_resident_supply_step(index, heap, GetActiveSnapshot(), limit, result);
            relation_close(index, NoLock);
        }
        if (heap)
            relation_close(heap, NoLock);
    }
    return status;
}

static void
supply_index(Oid index_oid)
{
    MemoryContext context = CurrentMemoryContext;

    PG_TRY();
    {
        CuvsCagraResidentSupplyProgress result;

        SetCurrentStatementStartTimestamp();
        StartTransactionCommand();
        XactIsoLevel = XACT_READ_COMMITTED;
        XactReadOnly = true;
        enable_timeout_after(STATEMENT_TIMEOUT, SUPPLY_STEP_TIMEOUT_MS);
        PushActiveSnapshot(GetTransactionSnapshot());
        (void) supply_locked_index(index_oid, UINT64_MAX, &result);
        PopActiveSnapshot();
        CHECK_FOR_INTERRUPTS();
        disable_timeout(STATEMENT_TIMEOUT, false);
        CommitTransactionCommand();
    }
    PG_CATCH();
    {
        if (geterrcode() != ERRCODE_QUERY_CANCELED)
            PG_RE_THROW();
        disable_timeout(STATEMENT_TIMEOUT, false);
        MemoryContextSwitchTo(context);
        AbortCurrentTransaction();
        FlushErrorState();
    }
    PG_END_TRY();
}

#ifdef CUVS_TEST_HOOKS
PG_FUNCTION_INFO_V1(pg_cuvs_test_page_supply_step);

Datum
pg_cuvs_test_page_supply_step(PG_FUNCTION_ARGS)
{
    int64 target = PG_GETARG_INT64(1);
    CuvsCagraResidentSupplyProgress result = {0};
    const char *status;
    int rc;
    char *json;

    if (!superuser())
        ereport(ERROR, (errmsg("the resident supply test requires a superuser")));
    if (target < 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("the resident supply target must be nonnegative")));
    rc = supply_locked_index(PG_GETARG_OID(0), (uint64) target, &result);
    switch (rc)
    {
        case CUVS_STATUS_OK: status = "ok"; break;
        case CUVS_STATUS_NOT_FOUND: status = "not_found"; break;
        case CUVS_STATUS_UNAVAILABLE: status = "unavailable"; break;
        case CUVS_STATUS_PAGE_NEED: status = "need"; break;
        case CUVS_STATUS_PAGE_OVERLAY_FULL: status = "full"; break;
        case CUVS_STATUS_PAGE_OVERLAY_HOST_FULL: status = "host_full"; break;
        default: status = "error"; break;
    }
    if (rc != CUVS_STATUS_OK)
        result = (CuvsCagraResidentSupplyProgress) {0};
    json = psprintf("{\"status\":\"%s\",\"index_nonce\":" UINT64_FORMAT
                    ",\"base_nonce\":" UINT64_FORMAT ",\"graph_nonce\":" UINT64_FORMAT
                    ",\"folded\":" UINT64_FORMAT ",\"from\":" UINT64_FORMAT
                    ",\"covered_upto\":" UINT64_FORMAT ",\"postmaster_boot_id\":" UINT64_FORMAT
                    ",\"daemon_id\":" UINT64_FORMAT "}",
                    status, result.request.base.index_nonce, result.request.base.base_nonce,
                    result.request.base.graph_nonce, result.request.base.folded, result.request.from,
                    result.progress.covered_upto, result.request.boot_id, result.progress.daemon_id);
    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(json)));
}
#endif

static List *
discover_indexes(void)
{
    List *indexes = NIL;
    MemoryContext context = CurrentMemoryContext;
    Relation catalog;
    TableScanDesc scan;
    HeapTuple tuple;
    Oid cagra_am;

    StartTransactionCommand();
    cagra_am = get_am_oid("cagra", true);
    if (OidIsValid(cagra_am))
    {
        catalog = table_open(RelationRelationId, AccessShareLock);
        scan = table_beginscan_catalog(catalog, 0, NULL);
        while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
        {
            Form_pg_class rel = (Form_pg_class) GETSTRUCT(tuple);

            CHECK_FOR_INTERRUPTS();
            if (rel->relam == cagra_am && rel->relkind == RELKIND_INDEX &&
                rel->relpersistence == RELPERSISTENCE_PERMANENT)
            {
                MemoryContext previous = MemoryContextSwitchTo(context);

                indexes = lappend_oid(indexes, rel->oid);
                MemoryContextSwitchTo(previous);
            }
        }
        table_endscan(scan);
        table_close(catalog, AccessShareLock);
    }
    CommitTransactionCommand();
    list_sort(indexes, list_oid_cmp);
    return indexes;
}

void
cuvs_cagra_resident_supply_database(Datum arg)
{
    Oid database = DatumGetObjectId(arg);
    dsm_handle handle;
    dsm_segment *segment;
    ResidentSupplyVisit *visit;
    TimestampTz stop_starting_at;
    List *indexes;
    int start = 0;
    int count;
    Oid last;

    pqsignal(SIGTERM, die);
    pqsignal(SIGINT, StatementCancelHandler);
    BackgroundWorkerUnblockSignals();
    if (!cuvs_cagra_pg_boot_id() || RecoveryInProgress())
        return;
    stop_starting_at = TimestampTzPlusMilliseconds(GetCurrentTimestamp(),
                                                   SUPPLY_DB_LEASE_MS - SUPPLY_STEP_TIMEOUT_MS);
    BackgroundWorkerInitializeConnectionByOid(database, InvalidOid, 0);
    memcpy(&handle, MyBgworkerEntry->bgw_extra, sizeof(handle));
    segment = dsm_attach(handle);
    if (!segment)
        return;
    visit = dsm_segment_address(segment);
    indexes = discover_indexes();
    count = list_length(indexes);
    if (count == 0)
        return;
    pg_atomic_write_u32(&visit->has_indexes, 1);

    last = pg_atomic_read_u32(&visit->last_index);
    while (start < count && list_nth_oid(indexes, start) <= last)
        ++start;
    if (start == count)
        start = 0;
    for (int i = 0; i < count; ++i)
    {
        Oid index_oid = list_nth_oid(indexes, start);

        CHECK_FOR_INTERRUPTS();
        if (GetCurrentTimestamp() >= stop_starting_at)
            break;
        /* Resume with the next OID even if this attempt times out. */
        pg_atomic_write_u32(&visit->last_index, index_oid);
        supply_index(index_oid);
        if (++start == count)
            start = 0;
    }
}

static ResidentSupplyDb *
discover_databases(ResidentSupplyDb *old, int old_count, int *count)
{
    List *oids = NIL;
    ResidentSupplyDb *databases = NULL;
    int i = 0, prior = 0;
    ListCell *cell;
    Relation catalog;
    TableScanDesc scan;
    HeapTuple tuple;

    StartTransactionCommand();
    catalog = table_open(DatabaseRelationId, AccessShareLock);
    scan = table_beginscan_catalog(catalog, 0, NULL);
    while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
    {
        Form_pg_database db = (Form_pg_database) GETSTRUCT(tuple);

        CHECK_FOR_INTERRUPTS();
        if (db->datallowconn && !db->datistemplate)
            oids = lappend_oid(oids, db->oid);
    }
    table_endscan(scan);
    table_close(catalog, AccessShareLock);

    list_sort(oids, list_oid_cmp);
    *count = list_length(oids);
    if (*count)
        databases = MemoryContextAllocZero(TopMemoryContext, *count * sizeof(*databases));
    foreach(cell, oids)
    {
        Oid oid = lfirst_oid(cell);

        while (prior < old_count && old[prior].oid < oid)
            ++prior;
        if (prior < old_count && old[prior].oid == oid)
            databases[i] = old[prior];
        else
            databases[i].oid = oid;
        ++i;
    }
    CommitTransactionCommand();
    if (old)
        pfree(old);
    return databases;
}

static BackgroundWorker
supply_worker(const char *name, const char *entry)
{
    BackgroundWorker worker = {0};

    strlcpy(worker.bgw_name, name, sizeof(worker.bgw_name));
    strlcpy(worker.bgw_type, name, sizeof(worker.bgw_type));
    strlcpy(worker.bgw_library_name, "pg_cuvs", sizeof(worker.bgw_library_name));
    strlcpy(worker.bgw_function_name, entry, sizeof(worker.bgw_function_name));
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    return worker;
}

static void
stop_supply_child(int code, Datum arg)
{
    if (resident_supply_child)
        TerminateBackgroundWorker(resident_supply_child);
}

static void
visit_database(ResidentSupplyDb *database, dsm_segment *segment)
{
    ResidentSupplyVisit *visit = dsm_segment_address(segment);
    BackgroundWorker worker = supply_worker("pg_cuvs resident supply",
                                          "cuvs_cagra_resident_supply_database");
    dsm_handle handle = dsm_segment_handle(segment);
    TimestampTz deadline;
    bool stopping = false;

    pg_atomic_write_u32(&visit->last_index, database->last_index);
    pg_atomic_write_u32(&visit->has_indexes, 0);
    worker.bgw_main_arg = ObjectIdGetDatum(database->oid);
    worker.bgw_notify_pid = MyProcPid;
    memcpy(worker.bgw_extra, &handle, sizeof(handle));
    if (!RegisterDynamicBackgroundWorker(&worker, &resident_supply_child))
        return;

    deadline = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), SUPPLY_DB_LEASE_MS);
    for (;;)
    {
        pid_t pid;

        ResetLatch(MyLatch);
        CHECK_FOR_INTERRUPTS();
        if (GetBackgroundWorkerPid(resident_supply_child, &pid) == BGWH_STOPPED)
            break;
        if (!stopping && GetCurrentTimestamp() >= deadline)
        {
            TerminateBackgroundWorker(resident_supply_child);
            stopping = true;
        }
        (void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                         50, PG_WAIT_EXTENSION);
    }
    pfree(resident_supply_child);
    resident_supply_child = NULL;
    database->last_index = pg_atomic_read_u32(&visit->last_index);
    if (!pg_atomic_read_u32(&visit->has_indexes))
        database->next_visit = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), SUPPLY_DB_REFRESH_MS);
}

void
cuvs_cagra_resident_supply_launcher(Datum arg)
{
    ResidentSupplyDb *databases = NULL;
    int count = 0;
    TimestampTz refresh_at = 0;
    ResidentSupplyVisit *visit;
    dsm_segment *segment;

    pqsignal(SIGTERM, die);
    BackgroundWorkerUnblockSignals();
    if (!resident_supply_enabled || !cuvs_cagra_pg_boot_id() || RecoveryInProgress())
        return;
    /* Initialize system catalogs without joining any target database. */
    BackgroundWorkerInitializeConnectionByOid(InvalidOid, InvalidOid, 0);
    before_shmem_exit(stop_supply_child, (Datum) 0);
    segment = dsm_create(sizeof(*visit), DSM_CREATE_NULL_IF_MAXSEGMENTS);
    if (!segment)
        ereport(ERROR, (errmsg("could not allocate pg_cuvs resident supply worker state")));
    visit = dsm_segment_address(segment);
    pg_atomic_init_u32(&visit->last_index, InvalidOid);
    pg_atomic_init_u32(&visit->has_indexes, 0);

    for (;;)
    {
        TimestampTz now = GetCurrentTimestamp();

        CHECK_FOR_INTERRUPTS();
        if (now >= refresh_at)
        {
            databases = discover_databases(databases, count, &count);
            refresh_at = TimestampTzPlusMilliseconds(now, SUPPLY_DB_REFRESH_MS);
        }
        for (int i = 0; i < count; ++i)
            if (databases[i].next_visit <= now)
                visit_database(&databases[i], segment);

        ResetLatch(MyLatch);
        CHECK_FOR_INTERRUPTS();
        (void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                         SUPPLY_INTERVAL_MS, PG_WAIT_EXTENSION);
    }
}

void
cuvs_cagra_resident_supply_register(void)
{
    BackgroundWorker worker;

    if (!process_shared_preload_libraries_in_progress)
        return;
    DefineCustomBoolVariable("cuvs.resident_supply",
        "Periodically supply committed pending rows of page-format CAGRA indexes.",
        NULL, &resident_supply_enabled, false, PGC_POSTMASTER, 0, NULL, NULL, NULL);
    if (!resident_supply_enabled)
        return;

    worker = supply_worker("pg_cuvs resident supply launcher",
                           "cuvs_cagra_resident_supply_launcher");
    worker.bgw_restart_time = 10;
    RegisterBackgroundWorker(&worker);
}
#endif
