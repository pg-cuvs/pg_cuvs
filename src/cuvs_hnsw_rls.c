/* The PostgreSQL planner may remove security quals implied by a partial
 * index. Once removed they cannot safely be reconstructed from IndexScan's
 * plan.qual: neither placing the partial predicate before nor after ordinary
 * quals preserves all security-barrier semantics. In explicit exact mode,
 * filter pg_cuvs_hnsw paths while the original Query still carries its
 * security quals and let PostgreSQL plan the normal executor path. */
#include "postgres.h"
#include "access/genam.h"
#include "access/relation.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "cuvs_hnsw_scan.h"
#include "cuvs_hnsw_rls.h"

static planner_hook_type previous_planner;
static set_rel_pathlist_hook_type previous_paths;
static ExecutorRun_hook_type previous_run;
static bool protected_planning;
static bool exact_planning;
static bool protected_execution;

bool
cuvs_hnsw_rls_query_active(void)
{
    return protected_execution;
}

static bool
relation_has_rls(Oid relid)
{
    HeapTuple tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
    bool enabled;

    if (!HeapTupleIsValid(tuple))
        return false;
    enabled = ((Form_pg_class) GETSTRUCT(tuple))->relrowsecurity;
    ReleaseSysCache(tuple);
    return enabled;
}

/* Partition and inheritance children enter the range table only while planning.
 * Look them up here, on the original Query, so one decision serves planning and
 * the later checks without planning twice. */
static bool
descendants_have_rls(Oid relid)
{
    List *all = find_all_inheritors(relid, NoLock, NULL);
    ListCell *cell;
    bool found = false;

    foreach(cell, all) {
        if (lfirst_oid(cell) != relid && relation_has_rls(lfirst_oid(cell))) {
            found = true;
            break;
        }
    }
    list_free(all);
    return found;
}

static bool
query_has_barrier(Node *node, void *context)
{
    if (!node)
        return false;
    if (IsA(node, Query)) {
        Query *query = (Query *) node;
        ListCell *cell;
        if (query->hasRowSecurity)
            return true;
        foreach(cell, query->rtable) {
            RangeTblEntry *rte = lfirst_node(RangeTblEntry, cell);
            if (rte->rtekind == RTE_SUBQUERY && rte->security_barrier)
                return true;
            if (rte->rtekind == RTE_RELATION &&
                (relation_has_rls(rte->relid) || (rte->inh && descendants_have_rls(rte->relid))))
                return true;
        }
        return query_tree_walker(query, query_has_barrier, context, 0);
    }
    return expression_tree_walker(node, query_has_barrier, context);
}

static List *
without_hnsw_paths(List *paths, Oid am, bool all_hnsw, bool *removed)
{
    List *kept = NIL;
    ListCell *cell;
    foreach(cell, paths) {
        Path *path = lfirst(cell);
        if (IsA(path, IndexPath)) {
            IndexOptInfo *index = ((IndexPath *) path)->indexinfo;
            if (index->relam == am &&
                (all_hnsw || index->indexprs != NIL || index->indpred != NIL)) {
                *removed = true;
                continue;
            }
        }
        kept = lappend(kept, path);
    }
    return kept;
}

static void
protected_paths(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
    Oid am;
    bool removed = false;
    bool protect;
    if (previous_paths)
        previous_paths(root, rel, rti, rte);
    if (!exact_planning || rte->rtekind != RTE_RELATION)
        return;
    am = get_am_oid("pg_cuvs_hnsw", true);
    if (!OidIsValid(am))
        return;
    /* A relation can enter the range table after the Query was checked: an inlined SQL
     * function, a rule or a view expanded while planning. Whatever the statement-level
     * decision, a relation with row-level security never keeps a pg_cuvs_hnsw path. */
    protect = protected_planning || rte->securityQuals != NIL || relation_has_rls(rte->relid);
    rel->pathlist = without_hnsw_paths(rel->pathlist, am, protect, &removed);
    rel->partial_pathlist = without_hnsw_paths(rel->partial_pathlist, am, protect, &removed);
    if (removed) {
        /* add_path may already have discarded the sequential alternative.
         * Recreate it with the relation's required lateral parameterization. */
        add_path(rel, create_seqscan_path(root, rel, rel->lateral_relids, 0));
    }
}

static bool
plan_has_hnsw(Plan *plan, Oid am, bool all_hnsw);

static bool
plan_list_has_hnsw(List *plans, Oid am, bool all_hnsw)
{
    ListCell *cell;
    foreach(cell, plans)
        if (plan_has_hnsw(lfirst(cell), am, all_hnsw))
            return true;
    return false;
}

static bool
plan_has_hnsw(Plan *plan, Oid am, bool all_hnsw)
{
    Oid index_oid = InvalidOid;
    Relation index;
    bool matched;
    if (!plan)
        return false;
    if (IsA(plan, IndexScan)) index_oid = ((IndexScan *) plan)->indexid;
    if (IsA(plan, IndexOnlyScan)) index_oid = ((IndexOnlyScan *) plan)->indexid;
    if (IsA(plan, BitmapIndexScan)) index_oid = ((BitmapIndexScan *) plan)->indexid;
    if (OidIsValid(index_oid)) {
        index = index_open(index_oid, AccessShareLock);
        matched = RelationGetForm(index)->relam == am &&
            (all_hnsw || RelationGetIndexExpressions(index) != NIL ||
             RelationGetIndexPredicate(index) != NIL);
        index_close(index, AccessShareLock);
        if (matched)
            return true;
    }
    if (plan_has_hnsw(outerPlan(plan), am, all_hnsw) || plan_has_hnsw(innerPlan(plan), am, all_hnsw))
        return true;
    switch (nodeTag(plan)) {
        case T_Append: return plan_list_has_hnsw(((Append *) plan)->appendplans, am, all_hnsw);
        case T_MergeAppend: return plan_list_has_hnsw(((MergeAppend *) plan)->mergeplans, am, all_hnsw);
        case T_BitmapAnd: return plan_list_has_hnsw(((BitmapAnd *) plan)->bitmapplans, am, all_hnsw);
        case T_BitmapOr: return plan_list_has_hnsw(((BitmapOr *) plan)->bitmapplans, am, all_hnsw);
        case T_SubqueryScan: return plan_has_hnsw(((SubqueryScan *) plan)->subplan, am, all_hnsw);
        case T_CustomScan: return plan_list_has_hnsw(((CustomScan *) plan)->custom_plans, am, all_hnsw);
        default: return false;
    }
}

static bool
statement_has_marker(PlannedStmt *statement)
{
    ListCell *cell;
    if (statement->dependsOnRole)
        return true;
    foreach(cell, statement->rtable) {
        RangeTblEntry *rte = lfirst_node(RangeTblEntry, cell);
        if (rte->rtekind == RTE_SUBQUERY && rte->security_barrier)
            return true;
    }
    return false;
}

static PlannedStmt *
plan_once(Query *query, const char *text, int options, ParamListInfo params, bool exact, bool security)
{
    bool saved = protected_planning;
    bool saved_exact = exact_planning;
    volatile PlannedStmt *result = NULL;
    PG_TRY();
    {
        protected_planning = security;
        exact_planning = exact;
        if (previous_planner) result = previous_planner(query, text, options, params);
        else result = standard_planner(query, text, options, params);
    }
    PG_FINALLY();
    {
        protected_planning = saved;
        exact_planning = saved_exact;
    }
    PG_END_TRY();
    return (PlannedStmt *) result;
}

/* Row-level security anywhere in the final range table. This includes a partitioned or
 * inheritance parent whose policy is not copied to its children: the scan of a child sees
 * neither a marker nor row-level security on its own table, so only the range table shows it. */
static bool
statement_has_rls_relation(PlannedStmt *statement)
{
    ListCell *cell;
    foreach(cell, statement->rtable) {
        RangeTblEntry *rte = lfirst_node(RangeTblEntry, cell);
        if (rte->rtekind == RTE_RELATION && relation_has_rls(rte->relid))
            return true;
    }
    return false;
}

/* True when the final plan keeps a pg_cuvs_hnsw scan that must not run in exact mode: the
 * statement is protected (a security barrier, a plan that depends on the role, row-level
 * security on any relation of the range table) or the scan's index has an expression or
 * predicate. The scan refuses to run in the same cases, so a plan that passes this check does
 * not stop with an error at run time unless something changed after planning. */
static bool
plan_keeps_unsafe_scan(PlannedStmt *statement, Oid am, bool all_hnsw)
{
    all_hnsw = all_hnsw || statement_has_marker(statement) || statement_has_rls_relation(statement);
    return plan_has_hnsw(statement->planTree, am, all_hnsw) ||
        plan_list_has_hnsw(statement->subplans, am, all_hnsw);
}

static PlannedStmt *
protected_planner(Query *query, const char *text, int options, ParamListInfo params)
{
    bool exact = cuvs_hnsw_scan_exact_requested();
    /* Optimization only: a protected shape visible before planning is planned once, without
     * any pg_cuvs_hnsw path. What this misses is caught by the check after planning. */
    bool security = exact && query_has_barrier((Node *) query, NULL);
    /* The planner scribbles on its input (it pulls subqueries up and replaces function
     * calls in place), and planning an already planned Query again is not something
     * PostgreSQL promises to support. A pristine copy is kept for the one case that needs
     * a second planning: see below. No test tells the copy from the planned Query
     * (planning the planned Query again gave the same plans for every shape tried), so this
     * rests on the contract, not on an observed difference. */
    Query *pristine = (exact && !security) ? copyObject(query) : NULL;
    PlannedStmt *result = plan_once(query, text, options, params, exact, security);

    if (exact) {
        Oid am = get_am_oid("pg_cuvs_hnsw", true);

        if (OidIsValid(am)) {
            /* Safety net. Row-level security and security barriers that only appear while
             * planning (an inlined SQL function, a view, a partitioned parent whose policy is
             * not copied to its partitions) are removed path by path where possible
             * (protected_paths), but that cannot see everything and a security barrier
             * subquery was planned before anything could be removed inside it. Whatever the
             * earlier steps did, a final plan that keeps a scan that must not run exactly
             * must not leave planning: plan the statement again with every pg_cuvs_hnsw path
             * removed. Planning side effects of that statement, such as the NOTICE of an
             * IMMUTABLE function evaluated on constants, then happen twice. */
            if (pristine && plan_keeps_unsafe_scan(result, am, security)) {
                security = true;
                result = plan_once(pristine, text, options, params, exact, security);
            }
            if (plan_keeps_unsafe_scan(result, am, security))
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                                errmsg("an exact query retained an unsupported HNSW index path")));
        }
    }
    return result;
}

static void
protected_run(QueryDesc *desc, ScanDirection direction, uint64 count, bool once)
{
    bool saved = protected_execution;
    PG_TRY();
    {
        /* Cheap statement-level marker (security barrier subquery or a plan that
         * depends on the role). Row-level security is judged by the scan itself
         * on its own heap relation, so no relation is opened here. */
        protected_execution = statement_has_marker(desc->plannedstmt);
        if (previous_run) previous_run(desc, direction, count, once);
        else standard_ExecutorRun(desc, direction, count, once);
    }
    PG_FINALLY();
    {
        protected_execution = saved;
    }
    PG_END_TRY();
}

void
cuvs_hnsw_rls_init(void)
{
    previous_planner = planner_hook;
    planner_hook = protected_planner;
    previous_paths = set_rel_pathlist_hook;
    set_rel_pathlist_hook = protected_paths;
    previous_run = ExecutorRun_hook;
    ExecutorRun_hook = protected_run;
}
