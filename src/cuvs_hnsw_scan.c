#include "postgres.h"
#include "access/amapi.h"
#include "access/relscan.h"
#include "utils/guc.h"
#include "utils/plancache.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "cuvs_hnsw_exact.h"
#include "cuvs_hnsw_scan.h"
#include "cuvs_hnsw_rls.h"

static bool force_exact = false;
static IndexAmRoutine delegate;

typedef struct CuvsHnswScanState {
    IndexAmRoutine original;
    void *original_opaque;
    CuvsHnswExact *exact;
    bool exact_mode;
} CuvsHnswScanState;

bool
cuvs_hnsw_scan_exact_requested(void)
{
    return force_exact;
}

static void
assign_exact(bool value, void *extra)
{
    (void) extra;
    /* A saved generic plan may have been prepared under the opposite mode.
     * The old plan remains pinned for an already-running portal. */
    if (value != force_exact)
        ResetPlanCache();
}

void
cuvs_hnsw_scan_init(void)
{
    DefineCustomBoolVariable("cuvs.hnsw_exact_scan",
                            "Use an exact heap scan for pg_cuvs_hnsw ordered scans.",
                            "Off by default. On scans the visible heap and sorts SQL distances; "
                            "cost grows with heap rows and may spill to PostgreSQL temporary files. "
                            "Policy is read at scan/rescan, including cached plans.",
                            &force_exact, false, PGC_USERSET, 0, NULL, assign_exact, NULL);
    cuvs_hnsw_rls_init();
}

static IndexScanDesc
cover_begin(Relation index, int keys, int orders)
{
    IndexScanDesc scan = delegate.ambeginscan(index, keys, orders);
    CuvsHnswScanState *state = palloc0(sizeof(*state));

    state->original = delegate;
    state->original_opaque = scan->opaque;
    scan->opaque = state;
    /* pgvector HNSW does not publish ORDER BY distances. Exact scans do, so
     * own initialized arrays rather than inspect its uninitialized fields. */
    scan->xs_orderbyvals = palloc0(sizeof(Datum) * orders);
    scan->xs_orderbynulls = palloc0(sizeof(bool) * orders);
    return scan;
}

static void
cover_rescan(IndexScanDesc scan, ScanKey keys, int nkeys,
             ScanKey orderbys, int norderbys)
{
    CuvsHnswScanState *state = scan->opaque;

    if (state->exact) {
        cuvs_hnsw_exact_end(state->exact);
        state->exact = NULL;
    }
    /* With the setting on, a scan either runs exactly or fails. A scan that was
     * planned before the setting changed may be one that exact mode never plans
     * (security barrier, row-level security, index expression or predicate); it
     * must not fall back to approximate results without telling the caller. */
    state->exact_mode = false;
    if (force_exact) {
        const char *why = NULL;

        if (cuvs_hnsw_rls_query_active())
            why = "row-level security or a security barrier applies to the statement";
        else if (scan->heapRelation && RelationGetForm(scan->heapRelation)->relrowsecurity)
            why = "row-level security is enabled on the table";
        else if (RelationGetIndexExpressions(scan->indexRelation) != NIL ||
                 RelationGetIndexPredicate(scan->indexRelation) != NIL)
            why = "the index has expressions or a predicate";
        if (why)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("cuvs.hnsw_exact_scan is on, but this scan cannot run exactly"),
                            errdetail("%s.", why),
                            errhint("If cuvs.hnsw_exact_scan was turned on or row-level security was enabled while the statement was running, run the statement again.")));
        state->exact_mode = true;
    }
    scan->opaque = state->original_opaque;
    PG_TRY();
    {
        state->original.amrescan(scan, keys, nkeys, orderbys, norderbys);
    }
    PG_FINALLY();
    {
        state->original_opaque = scan->opaque;
        scan->opaque = state;
    }
    PG_END_TRY();
}

static bool
cover_next(IndexScanDesc scan, ScanDirection direction)
{
    CuvsHnswScanState *state = scan->opaque;
    bool found;

    if (state->exact_mode) {
        Datum distance;
        bool isnull;

        if (!ScanDirectionIsForward(direction))
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("HNSW exact scan requires forward direction")));
        if (!state->exact)
            state->exact = cuvs_hnsw_exact_begin(scan);
        found = cuvs_hnsw_exact_next(state->exact, &scan->xs_heaptid, &distance, &isnull);
        if (found) {
            scan->xs_recheck = false;
            scan->xs_recheckorderby = false;
            scan->xs_orderbyvals[0] = distance;
            scan->xs_orderbynulls[0] = isnull;
        }
        return found;
    }

    scan->opaque = state->original_opaque;
    PG_TRY();
    {
        found = state->original.amgettuple(scan, direction);
    }
    PG_FINALLY();
    {
        state->original_opaque = scan->opaque;
        scan->opaque = state;
    }
    PG_END_TRY();
    return found;
}

static void
cover_end(IndexScanDesc scan)
{
    CuvsHnswScanState *state = scan->opaque;

    if (state->exact)
        cuvs_hnsw_exact_end(state->exact);
    scan->opaque = state->original_opaque;
    state->original.amendscan(scan);
    pfree(scan->xs_orderbyvals);
    pfree(scan->xs_orderbynulls);
    pfree(state);
    scan->opaque = NULL;
}

void
cuvs_hnsw_scan_wrap(IndexAmRoutine *routine)
{
    delegate = *routine;
    routine->ambeginscan = cover_begin;
    routine->amrescan = cover_rescan;
    routine->amgettuple = cover_next;
    routine->amendscan = cover_end;
}
