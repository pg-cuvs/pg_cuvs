/* Private regression AM, never installed by the extension. Build/insert use
 * pgvector HNSW; only scans exercise the exact utility in this checkout. */
#include "postgres.h"
#include "fmgr.h"
#include "access/amapi.h"
#include "access/genam.h"
#include "catalog/pg_am.h"
#include "commands/defrem.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "cuvs_hnsw_exact.h"

PG_MODULE_MAGIC;

typedef struct ExactProbeState {
    CuvsHnswExact *exact;
} ExactProbeState;

static IndexScanDesc
probe_begin(Relation relation, int keys, int orders)
{
    IndexScanDesc scan = RelationGetIndexScan(relation, keys, orders);
    scan->opaque = palloc0(sizeof(ExactProbeState));
    /* RelationGetIndexScan leaves these fields to the AM. A NULL check in
     * gettuple cannot make an uninitialized pointer safe. */
#ifndef EXACT_PROBE_MUTATE_ORDERBY
    scan->xs_orderbyvals = palloc0(sizeof(Datum) * orders);
    scan->xs_orderbynulls = palloc0(sizeof(bool) * orders);
#endif
    return scan;
}

static void
probe_rescan(IndexScanDesc scan, ScanKey keys, int nkeys,
             ScanKey orders, int norders)
{
    ExactProbeState *state = scan->opaque;
    if (state->exact)
        cuvs_hnsw_exact_end(state->exact);
    state->exact = NULL;
    if (keys && nkeys)
        memmove(scan->keyData, keys, nkeys * sizeof(ScanKeyData));
    if (orders && norders)
        memmove(scan->orderByData, orders, norders * sizeof(ScanKeyData));
}

static bool
probe_next(IndexScanDesc scan, ScanDirection direction)
{
    ExactProbeState *state = scan->opaque;
    Datum distance;
    bool isnull;

    if (!ScanDirectionIsForward(direction))
        elog(ERROR, "exact regression AM requires forward scan");
    if (!state->exact)
        state->exact = cuvs_hnsw_exact_begin(scan);
    if (!cuvs_hnsw_exact_next(state->exact, &scan->xs_heaptid, &distance, &isnull))
        return false;
    scan->xs_recheck = false;
    scan->xs_recheckorderby = false;
    if (scan->xs_orderbyvals) {
        scan->xs_orderbyvals[0] = distance;
        scan->xs_orderbynulls[0] = isnull;
    }
    return true;
}

static void
probe_end(IndexScanDesc scan)
{
    ExactProbeState *state = scan->opaque;
    if (state->exact)
        cuvs_hnsw_exact_end(state->exact);
#ifndef EXACT_PROBE_MUTATE_ORDERBY
    pfree(scan->xs_orderbyvals);
    pfree(scan->xs_orderbynulls);
#endif
    pfree(state);
}

PG_FUNCTION_INFO_V1(hnsw_exact_probe_handler);
Datum
hnsw_exact_probe_handler(PG_FUNCTION_ARGS)
{
    Oid am = get_am_oid("hnsw", false);
    HeapTuple tuple = SearchSysCache1(AMOID, ObjectIdGetDatum(am));
    Oid handler = ((Form_pg_am) GETSTRUCT(tuple))->amhandler;
    IndexAmRoutine *routine;

    ReleaseSysCache(tuple);
    routine = (IndexAmRoutine *) DatumGetPointer(OidFunctionCall1(handler, (Datum) 0));
    routine->ambeginscan = probe_begin;
    routine->amrescan = probe_rescan;
    routine->amgettuple = probe_next;
    routine->amendscan = probe_end;
    PG_RETURN_POINTER(routine);
}
