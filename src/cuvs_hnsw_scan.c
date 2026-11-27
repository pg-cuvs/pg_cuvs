#include "postgres.h"
#include "access/xlog.h"
#include "access/amapi.h"
#include "access/relscan.h"
#include "utils/guc.h"
#include "utils/plancache.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "cuvs_hnsw_exact.h"
#include "cuvs_hnsw_scan.h"
#include "cuvs_cover_worker.h"
#include "cuvs_hnsw_rls.h"
#include "hnsw_export.h"
#include "cuvs_cover_scan.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

static bool force_exact = false;
static IndexAmRoutine delegate;

typedef struct CuvsHnswScanState {
    IndexAmRoutine original;
    void *original_opaque;
    CuvsHnswExact *exact;
    bool exact_mode;
    /* (only for an index of the new handler symbol) */
    bool cover;                 /* the index has a cover descriptor */
    bool merge_mode;            /* CERTIFIED: rank the ANN answers together with every ledger key, exactly */
    MemoryContext merge_cxt;    /* the candidate array of merge mode */
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
    state->cover = cuvs_hnsw_index_is_cover(index);
    scan->opaque = state;
    /* pgvector HNSW does not publish ORDER BY distances. Exact scans do, so
     * own initialized arrays rather than inspect its uninitialized fields. */
    scan->xs_orderbyvals = palloc0(sizeof(Datum) * orders);
    scan->xs_orderbynulls = palloc0(sizeof(bool) * orders);
    return scan;
}

/* The one statement shape that a scan of an index with a descriptor cannot answer: the index has an expression or a predicate,
 * which the exact module evaluates (as the role that reads, with no change of user) for every row it ranks, and the statement has
 * row-level security or a security barrier, which hide rows from the statement that those functions must not be run on. Planning
 * does not choose such an index for such a statement (cuvs_hnsw_rls.c) and PostgreSQL's sequential scan answers; this is the net
 * for a plan that kept it anyway. NULL when the scan can run. A plain-column index never refuses: the module computes the operator
 * class's own distance of a stored vector, which an index scan does for the rows that are hidden, too. */
static const char *
cover_unsafe_reason(IndexScanDesc scan)
{
    if (RelationGetIndexExpressions(scan->indexRelation) == NIL &&
        RelationGetIndexPredicate(scan->indexRelation) == NIL)
        return NULL;
    if (!cuvs_hnsw_rls_statement_known())
        return "the statement that runs the scan is not known (the scan does not run inside the executor)";
    if (cuvs_hnsw_rls_query_active())
        return "a security barrier or a plan that depends on the role applies to the statement";
    if (scan->heapRelation && RelationGetForm(scan->heapRelation)->relrowsecurity)
        return "row-level security is enabled on the table";
    /* a partition or an inheritance child whose parent has the policy: the child itself shows nothing */
    if (cuvs_hnsw_rls_statement_has_rls_relation())
        return "row-level security is enabled on a table of the statement";
    return NULL;
}

static void
cover_require_safe(IndexScanDesc scan)
{
    const char *why = cover_unsafe_reason(scan);

    if (why)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("pg_cuvs: index \"%s\" cannot answer this statement", RelationGetRelationName(scan->indexRelation)),
                        errdetail("Its expression or predicate would be evaluated on rows that the statement is not allowed to see: %s.", why),
                        errhint("Run the statement again, or SET cuvs.hnsw_exact_scan = on for it: PostgreSQL then reads the table itself and does not use this index.")));
}

bool
cuvs_hnsw_scan_exact_served(Relation index)
{
    CuvsCoverResult res;

    if (cuvs_hnsw_index_is_cover(index)) {
        cuvs_cover_scan_classify(index, &res);
        return res.cls != CUVS_COVER_LEGACY && res.cls != CUVS_COVER_OFF;
    }
    /* the older symbol: only while the break-glass marker exists, and then only an index that carries a descriptor */
    if (!cuvs_cover_breakglass_marked())
        return false;
    cuvs_cover_scan_classify(index, &res);
    return res.cls != CUVS_COVER_LEGACY && res.cls != CUVS_COVER_OFF && res.cls != CUVS_COVER_ERROR;
}

/* One notice per index and minute per backend: a heap-ranked answer is correct but slow, and says how to fix it. */
static void
cover_notice(Relation index, const char *why, bool reindex_only)
{
    static Oid last_index = InvalidOid;
    static TimestampTz last_at = 0;
    TimestampTz now = GetCurrentTimestamp();

    if (last_index == RelationGetRelid(index) && now - last_at < 60 * USECS_PER_SEC)
        return;
    last_index = RelationGetRelid(index);
    last_at = now;
    ereport(NOTICE,
            (errmsg("pg_cuvs: index \"%s\" answers from the heap, not from the graph", RelationGetRelationName(index)),
             errdetail("%s.", why),
             reindex_only ? errhint("REINDEX the index.") :
             errhint("Run pg_cuvs_cover_recertify('%s') now, or VACUUM the table. Autovacuum does it by itself only after enough rows of the table "
                     "changed (autovacuum_vacuum_threshold and _scale_factor, or the insert variants); a table that stays as it is waits for a call.",
                     RelationGetRelationName(index))));
}

/* Whether a search during a VACUUM merges the graph's answers and the ledger (a user decision): only while the VACUUM
 * that took the guarantee off really runs; before and after, and after a VACUUM that was cancelled, crashed or ended with its recertification
 * deferred, skipped or abandoned (the stored reason stays vacuum then), a LOST index is answered exactly from the heap, as the decision of
 * 2026-11-15 says for any index that cannot be trusted. The descriptor alone does not tell those apart: ambulkdelete records the VACUUM's
 * transaction in the sweeper's candidate table (cuvs_cover_vacuum_running). A standby cannot see the primary's VACUUM and merges while the
 * reason is vacuum (a user decision): until the primary's end-of-VACUUM recertification, or the next one of its sweeper,
 * reaches it through the WAL. false here gives the VACUUM state the heap ranking as well. */
#define CUVS_COVER_MERGE_DURING_VACUUM true

static bool
cover_vacuum_merge(Relation index, const CuvsCoverResult *res)
{
    /* after an emergency rollback the ledger is not kept on purpose, whatever the reason the descriptor stores */
    return CUVS_COVER_MERGE_DURING_VACUUM && res->cls == CUVS_COVER_LOST && res->stored_reason == CUVS_COVER_REASON_VACUUM &&
        res->ledger_head != CUVS_COVER_INVALID_BLOCK && res->writable && !cuvs_cover_breakglass_marked() &&
        (RecoveryInProgress() || cuvs_cover_vacuum_running(RelationGetRelid(index)));
}

/* The cover: an index of the new handler symbol is read by its descriptor.
 *   LEGACY / OFF   as before (approximate)
 *   CERTIFIED      the ANN answers and every ledger key, ranked exactly
 *   LOST (vacuum)  the same, while cover_vacuum_merge() says so
 *   LOST / other   the whole visible heap, ranked exactly, with a notice
 * The ranking evaluates the index's expression and predicate as the reading role. The one statement it cannot answer is the one
 * cover_unsafe_reason describes; a scan that cannot be answered exactly is never answered approximately. */
static void
cover_choose_mode(IndexScanDesc scan, CuvsHnswScanState *state)
{
    CuvsCoverResult res;

    state->merge_mode = false;
    if (state->merge_cxt) {
        MemoryContextDelete(state->merge_cxt);
        state->merge_cxt = NULL;
    }
    if (!state->cover) {
        /* The older symbol keeps the older meaning (an approximate answer) with one exception: while the break-glass marker
         * exists (the emergency action points the handler here), an index that carries a guarantee mark was written to by a
         * library that did not keep it, and an approximate answer would silently miss the rows the mark promised. */
        if (!cuvs_cover_breakglass_marked())
            return;
        cuvs_cover_scan_classify(scan->indexRelation, &res);
        if (res.cls == CUVS_COVER_LEGACY || res.cls == CUVS_COVER_OFF || res.cls == CUVS_COVER_ERROR)
            return;
        if (state->exact_mode)
            return;
        cover_require_safe(scan);
        state->exact_mode = true;
        cover_notice(scan->indexRelation, "an emergency rollback was made (pg_cuvs_cover_breakglass()): ALTER EXTENSION pg_cuvs UPDATE TO '0.9.2' and then TO '0.9.3' with the new library installed ends it", true);
        return;
    }
    cuvs_cover_scan_classify(scan->indexRelation, &res);
    switch (res.cls) {
    case CUVS_COVER_LEGACY:
    case CUVS_COVER_OFF:
        return;
    case CUVS_COVER_CERTIFIED:
        if (state->exact_mode)
            return;             /* cuvs.hnsw_exact_scan already ranks the whole heap */
        break;
    default:
        if (state->exact_mode)
            return;
        break;
    }
    if ((res.cls == CUVS_COVER_CERTIFIED || res.cls == CUVS_COVER_LOST) && !res.writable) {
        /* a descriptor of a newer format: this library reads the heap and says so (it cannot judge the ledger of that format) */
        cover_require_safe(scan);
        state->exact_mode = true;
        cover_notice(scan->indexRelation, "its descriptor was written by a newer pg_cuvs, whose ledger this library does not judge", true);
        return;
    }
    if (res.cls == CUVS_COVER_CERTIFIED || cover_vacuum_merge(scan->indexRelation, &res)) {
        if (cuvs_cover_ledger_is_empty(scan->indexRelation, &res)) {
            /* nothing to add to the graph's answers: the scan is the unwrapped one */
            elog(DEBUG1, "pg_cuvs: scan of \"%s\" delegated to the graph (the ledger holds no key)", RelationGetRelationName(scan->indexRelation));
            return;
        }
        cover_require_safe(scan);
        state->merge_mode = true;
        return;
    }
    cover_require_safe(scan);
    state->exact_mode = true;
    cover_notice(scan->indexRelation,
                 res.cls != CUVS_COVER_LOST ? "its descriptor cannot be trusted" :
                 res.stored_reason == CUVS_COVER_REASON_BREAKGLASS ? "an emergency rollback was made (pg_cuvs_cover_breakglass()): ALTER EXTENSION pg_cuvs UPDATE TO '0.9.2' and then TO '0.9.3' with the new library installed ends it" :
                 res.stored_reason == CUVS_COVER_REASON_LEDGER_DAMAGED ? "a ledger page was damaged and the ledger was dropped (pg_cuvs_cover_recertify() builds a new one)" :
                 "its guarantee was lost (a vacuum or an unkeyed writer changed the graph)",
                 res.cls != CUVS_COVER_LOST);
}

static int
cover_tid_cmp(const void *a, const void *b)
{
    return ItemPointerCompare((ItemPointer) a, (ItemPointer) b);
}

/* CERTIFIED (and LOST while cover_vacuum_merge() says so): the ANN answers plus every ledger key, each once, handed to the exact module as a subset. */
static void
cover_build_merge(IndexScanDesc scan, CuvsHnswScanState *state)
{
    CuvsCoverResult res;
    ItemPointerData *ann = NULL, *ledger = NULL, *all;
    Size nann = 0, capann = 0, nledger = 0, nall, i, w;
    MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "HNSW cover merge", ALLOCSET_DEFAULT_SIZES);
    MemoryContext previous = MemoryContextSwitchTo(cxt);

    state->merge_cxt = cxt;
    cuvs_cover_scan_classify(scan->indexRelation, &res);
    if ((res.cls != CUVS_COVER_CERTIFIED && !cover_vacuum_merge(scan->indexRelation, &res)) ||
        !cuvs_cover_scan_ledger(scan->indexRelation, res.ledger_head, res.ledger_tail, res.generation, cxt, &ledger, &nledger)) {
        /* the descriptor changed or the ledger cannot be read: rank the whole heap, which is always right */
        MemoryContextSwitchTo(previous);
        cover_notice(scan->indexRelation, "its ledger could not be read (a damaged page or key)", false);
        state->exact = cuvs_hnsw_exact_begin_cover(scan);
        return;
    }
    /* the graph's own answers, in whatever order pgvector produces them */
    scan->opaque = state->original_opaque;
    PG_TRY();
    {
        while (state->original.amgettuple(scan, ForwardScanDirection)) {
            if (nann == capann) {
                capann = capann ? capann * 2 : 256;
                ann = ann ? repalloc_huge(ann, capann * sizeof(ItemPointerData)) : palloc(capann * sizeof(ItemPointerData));
            }
            ann[nann++] = scan->xs_heaptid;
        }
    }
    PG_FINALLY();
    {
        state->original_opaque = scan->opaque;
        scan->opaque = state;
    }
    PG_END_TRY();
    nall = nann + nledger;
    all = MemoryContextAllocHuge(cxt, (nall ? nall : 1) * sizeof(ItemPointerData));
    if (nann)
        memcpy(all, ann, nann * sizeof(ItemPointerData));
    if (nledger)
        memcpy(all + nann, ledger, nledger * sizeof(ItemPointerData));
    qsort(all, nall, sizeof(ItemPointerData), cover_tid_cmp);
    for (i = 0, w = 0; i < nall; i++)
        if (w == 0 || ItemPointerCompare(&all[w - 1], &all[i]) != 0)
            all[w++] = all[i];
    MemoryContextSwitchTo(previous);
    state->exact = cuvs_hnsw_exact_begin_tids(scan, all, w);
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
     * (security barrier, row-level security); it must not fall back to approximate
     * results without telling the caller. An index expression or predicate is no
     * reason to stop: the exact module evaluates it as the role that reads. */
    state->exact_mode = false;
    if (force_exact) {
        const char *why = NULL;

        if (cuvs_hnsw_rls_query_active())
            why = "row-level security or a security barrier applies to the statement";
        else if (scan->heapRelation && RelationGetForm(scan->heapRelation)->relrowsecurity)
            why = "row-level security is enabled on the table";
        else if ((RelationGetIndexExpressions(scan->indexRelation) != NIL ||
                  RelationGetIndexPredicate(scan->indexRelation) != NIL) &&
                 !cuvs_hnsw_rls_statement_known())
            why = "the statement that runs the scan is not known (the scan does not run inside the executor)";
        else if ((RelationGetIndexExpressions(scan->indexRelation) != NIL ||
                  RelationGetIndexPredicate(scan->indexRelation) != NIL) &&
                 cuvs_hnsw_rls_statement_has_rls_relation())
            why = "row-level security is enabled on a table of the statement";
        if (why)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("cuvs.hnsw_exact_scan is on, but this scan cannot run exactly"),
                            errdetail("%s.", why),
                            errhint("If cuvs.hnsw_exact_scan was turned on or row-level security was enabled while the statement was running, run the statement again.")));
        state->exact_mode = true;
    }
    cover_choose_mode(scan, state);
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

    if (state->exact_mode || state->merge_mode) {
        Datum distance;
        bool isnull;

        if (!ScanDirectionIsForward(direction))
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("HNSW exact scan requires forward direction")));
        if (!state->exact) {
            if (state->merge_mode)
                cover_build_merge(scan, state);
            else
                state->exact = cuvs_hnsw_exact_begin_cover(scan);
        }
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
    if (state->merge_cxt)
        MemoryContextDelete(state->merge_cxt);
    scan->opaque = state->original_opaque;
    state->original.amendscan(scan);
    pfree(scan->xs_orderbyvals);
    pfree(scan->xs_orderbynulls);
    pfree(state);
    scan->opaque = NULL;
}

/* pgvector's own scan routines, for a caller that wants the graph search without the cover merge (the insert verification). */
const IndexAmRoutine *
cuvs_hnsw_scan_delegate(void)
{
    return &delegate;
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
