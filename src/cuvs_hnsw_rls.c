/* The PostgreSQL planner may remove security quals implied by a partial
 * index. Once removed they cannot safely be reconstructed from IndexScan's
 * plan.qual: neither placing the partial predicate before nor after ordinary
 * quals preserves all security-barrier semantics. In explicit exact mode,
 * filter pg_cuvs_hnsw paths while the original Query still carries its
 * security quals and let PostgreSQL plan the normal executor path.
 *
 * The same holds for an index with a cover descriptor that has an expression
 * or a predicate, without the setting: its scan ranks rows by calling those
 * functions on every row it examines, so a statement with row-level security
 * or a security barrier must not use it, and PostgreSQL's sequential scan
 * answers instead. A plain-column index is not touched by this part. */
#include "postgres.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/skey.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/pg_class.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_index.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "cuvs_hnsw_scan.h"
#include "cuvs_hnsw_rls.h"
#include "cuvs_cover_scan.h"
#include "cuvs_hnsw_exact.h"
#include "hnsw_export.h"

static planner_hook_type previous_planner;
static set_rel_pathlist_hook_type previous_paths;
static ExecutorStart_hook_type previous_start;
static ExecutorRun_hook_type previous_run;
static ExecutorFinish_hook_type previous_finish;
static bool protected_planning;
static bool exact_planning;
static bool cover_planning;     /* pg_cuvs_hnsw exists: paths of an index with a descriptor and an expression or predicate may be removed */
static int executing_marker = -1;   /* the statement carries a marker: -1 not looked at yet, 0 no, 1 yes */
static bool statement_has_marker(PlannedStmt *statement);
static PlannedStmt *executing_statement;
static int executing_rls = -1;  /* -1: the range table was not looked at yet */

/* What the backend remembers about the indexes of the pg_cuvs_hnsw access method in its database: whether there is any, and whether one of
 * them has an expression or a predicate. Without an index that has an expression or a predicate, and without cuvs.hnsw_exact_scan, the
 * planning has nothing to remove or to look ahead for, and a statement passes through the planner and path hooks as if they were not
 * there; without any index of the access method no scan of one can start, and the executor hooks stand aside as well. The answer is
 * looked up through the dependencies on the operator classes of the access method (then pg_index for each index found) and thrown away by the invalidation of any
 * pg_index tuple, which every CREATE INDEX (also CONCURRENTLY, whose phases update the tuple), DROP INDEX, REINDEX and ATTACH PARTITION
 * that adds or removes an index sends, also for a change the backend made itself and when a transaction that made one rolls back, and by
 * the invalidation of any pg_am tuple (the extension is created or dropped). Changes of pg_class that nobody could use to turn a
 * table into an index of this access method (statistics, relfilenode) do not touch it: a callback on pg_class would run after every VACUUM
 * and ANALYZE. Invalidation messages are accepted when a statement takes the locks of its tables, which happens before the planner hook
 * runs, in the same batch that tells the planner about the index it plans with. A backend is tied to one database, whose catalogs it reads. */
static bool cover_known = false;
static bool cover_any = true;
static bool cover_exprpred = true;
static uint64 cover_generation = 0;

static void
cover_invalidate(Datum arg, int cacheid, uint32 hashvalue)
{
    cover_known = false;
    cover_generation++;
}

/* The indexes of the access method are found through the dependency every index records on the operator classes of its key columns: the
 * operator classes of the access method (a handful, by the index on pg_opclass), then the dependents of each (by the index on pg_depend).
 * The work does not grow with the number of tables and indexes of the database, which matters because any index created anywhere, a
 * temporary table's primary key included, makes every backend look again. */
static void
cover_lookup(bool *any, bool *exprpred)
{
    Oid am = get_am_oid("pg_cuvs_hnsw", true);
    Relation oprel, deprel;
    SysScanDesc scan;
    ScanKeyData key[2];
    HeapTuple tuple;
    List *opclasses = NIL;
    ListCell *cell;

    *any = false;
    *exprpred = false;
    if (!OidIsValid(am))
        return;
    oprel = table_open(OperatorClassRelationId, AccessShareLock);
    ScanKeyInit(&key[0], Anum_pg_opclass_opcmethod, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(am));
    scan = systable_beginscan(oprel, OpclassAmNameNspIndexId, true, NULL, 1, key);
    while (HeapTupleIsValid(tuple = systable_getnext(scan)))
        opclasses = lappend_oid(opclasses, ((Form_pg_opclass) GETSTRUCT(tuple))->oid);
    systable_endscan(scan);
    table_close(oprel, AccessShareLock);

    deprel = table_open(DependRelationId, AccessShareLock);
    foreach(cell, opclasses) {
        if (*exprpred)
            break;
        ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(OperatorClassRelationId));
        ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(lfirst_oid(cell)));
        scan = systable_beginscan(deprel, DependReferenceIndexId, true, NULL, 2, key);
        while (!*exprpred && HeapTupleIsValid(tuple = systable_getnext(scan))) {
            Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(tuple);
            HeapTuple index_tuple;

            if (dep->classid != RelationRelationId)
                continue;
            index_tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(dep->objid));
            if (!HeapTupleIsValid(index_tuple))
                continue;
            *any = true;
            *exprpred = !heap_attisnull(index_tuple, Anum_pg_index_indexprs, NULL) ||
                !heap_attisnull(index_tuple, Anum_pg_index_indpred, NULL);
            ReleaseSysCache(index_tuple);
        }
        systable_endscan(scan);
    }
    table_close(deprel, AccessShareLock);
    list_free(opclasses);
}

static void
cover_refresh(void)
{
    if (!cover_known) {
        uint64 generation = cover_generation;
        bool any, exprpred;

        cover_lookup(&any, &exprpred);
        cover_any = any;
        cover_exprpred = exprpred;
        /* an invalidation that arrived while the lookup ran (taking a lock accepts messages) leaves the answer unknown */
        cover_known = (generation == cover_generation);
    }
}

static bool
exprpred_cover_index_exists(void)
{
    cover_refresh();
    return cover_exprpred;
}

static bool
cover_index_exists(void)
{
    cover_refresh();
    return cover_any;
}

/* Which pg_cuvs_hnsw indexes a planning removes. Each scope contains the next. */
typedef enum HnswScope {
    SCOPE_NONE = 0,
    SCOPE_COVER_UNSAFE_PRED,    /* an index that the exact module serves and whose predicate is not built in only */
    SCOPE_COVER_EXPRPRED,       /* an index that the exact module serves and that has an expression or a predicate */
    SCOPE_EXPRPRED,             /* any index with an expression or a predicate */
    SCOPE_ALL                   /* every pg_cuvs_hnsw index */
} HnswScope;

static HnswScope
planning_scope(bool exact, bool cover, bool protect)
{
    if (exact)
        return protect ? SCOPE_ALL : SCOPE_EXPRPRED;
    if (cover)
        return protect ? SCOPE_COVER_EXPRPRED : SCOPE_COVER_UNSAFE_PRED;
    return SCOPE_NONE;
}

static bool
index_in_scope(Relation index, HnswScope scope)
{
    bool exprpred = RelationGetIndexExpressions(index) != NIL || RelationGetIndexPredicate(index) != NIL;

    switch (scope) {
    case SCOPE_ALL: return true;
    case SCOPE_EXPRPRED: return exprpred;
    case SCOPE_COVER_EXPRPRED: return exprpred && cuvs_hnsw_scan_exact_served(index);
    case SCOPE_COVER_UNSAFE_PRED:
        return RelationGetIndexPredicate(index) != NIL && cuvs_hnsw_scan_exact_served(index) &&
            !cuvs_hnsw_exact_predicate_builtin(index);
    default: return false;
    }
}

static bool
index_info_in_scope(IndexOptInfo *info, HnswScope scope)
{
    Relation index;
    bool in;

    if (scope == SCOPE_ALL)
        return true;
    if (scope == SCOPE_COVER_UNSAFE_PRED ? info->indpred == NIL : (info->indexprs == NIL && info->indpred == NIL))
        return false;
    if (scope == SCOPE_EXPRPRED)
        return true;
    /* the planner holds the lock on the index it plans with */
    index = index_open(info->indexoid, NoLock);
    in = index_in_scope(index, scope);
    index_close(index, NoLock);
    return in;
}

bool
cuvs_hnsw_rls_query_active(void)
{
    /* looked at when a scan asks, not at the start of every statement: the range table of a generic plan holds every partition */
    if (executing_marker < 0)
        executing_marker = executing_statement && statement_has_marker(executing_statement);
    return executing_marker > 0;
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

/* A partition or an inheritance child enters the range table only while planning. Under the explicit setting (cuvs.hnsw_exact_scan = on) a
 * statement that reaches a child with row-level security through a parent without it is looked up here, on the original Query, so that
 * one decision serves planning and the later checks and the statement is planned once (a planning side effect, such as the NOTICE of an
 * IMMUTABLE function evaluated on constants, happens once). Without the setting the look-ahead does not open the children: a statement on
 * the parent applies the policies of the parent only, so the flag of a child decides nothing at the start of a statement, and finding it
 * there would walk the catalog entries of every descendant on every statement on the parent (a partitioned table with a thousand
 * partitions paid 0.23 ms for it, pruned to one partition or not). The flag of a child that is planned is seen where it matters: the paths
 * of that child are made under protected_paths, which checks the child's own table, and the final range table is checked after planning
 * (statement_has_rls_relation), which plans the statement again without the paths of the scope when a scan that must not run is left. */
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
                (relation_has_rls(rte->relid) ||
                 (context && *(bool *) context && rte->inh && has_subclass(rte->relid) && descendants_have_rls(rte->relid))))
                return true;
        }
        return query_tree_walker(query, query_has_barrier, context, 0);
    }
    return expression_tree_walker(node, query_has_barrier, context);
}

static List *
without_hnsw_paths(List *paths, Oid am, HnswScope scope, bool *removed)
{
    List *kept = NIL;
    ListCell *cell;
    foreach(cell, paths) {
        Path *path = lfirst(cell);
        if (IsA(path, IndexPath)) {
            IndexOptInfo *index = ((IndexPath *) path)->indexinfo;
            if (index->relam == am && index_info_in_scope(index, scope)) {
                *removed = true;
                continue;
            }
        }
        kept = lappend(kept, path);
    }
    return kept;
}

static bool
has_exprpred_path(List *paths, Oid am)
{
    ListCell *cell;

    foreach(cell, paths) {
        Path *path = lfirst(cell);

        if (IsA(path, IndexPath)) {
            IndexOptInfo *index = ((IndexPath *) path)->indexinfo;

            if (index->relam == am && (index->indexprs != NIL || index->indpred != NIL))
                return true;
        }
    }
    return false;
}

static void
protected_paths(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
    Oid am;
    bool removed = false;
    bool protect;
    HnswScope scope;
    if (previous_paths)
        previous_paths(root, rel, rti, rte);
    if (!(exact_planning || cover_planning) || rte->rtekind != RTE_RELATION)
        return;
    am = get_am_oid("pg_cuvs_hnsw", true);
    if (!OidIsValid(am))
        return;
    /* Without the setting only a relation that has such a path is looked at: the paths are the only thing to remove, and a relation
     * that enters the statement while it is planned (an inlined function) is seen here, when its own paths are made. */
    if (!exact_planning && !has_exprpred_path(rel->pathlist, am) && !has_exprpred_path(rel->partial_pathlist, am))
        return;
    /* A relation can enter the range table after the Query was checked: an inlined SQL
     * function, a rule or a view expanded while planning. Whatever the statement-level
     * decision, a relation with row-level security never keeps a pg_cuvs_hnsw path. */
    protect = protected_planning || rte->securityQuals != NIL || relation_has_rls(rte->relid);
    scope = planning_scope(exact_planning, cover_planning, protect);
    if (scope == SCOPE_NONE)
        return;
    rel->pathlist = without_hnsw_paths(rel->pathlist, am, scope, &removed);
    rel->partial_pathlist = without_hnsw_paths(rel->partial_pathlist, am, scope, &removed);
    if (removed) {
        /* add_path may already have discarded the sequential alternative.
         * Recreate it with the relation's required lateral parameterization. */
        add_path(rel, create_seqscan_path(root, rel, rel->lateral_relids, 0));
    }
}

static bool
plan_has_hnsw(Plan *plan, Oid am, HnswScope scope);

static bool
plan_list_has_hnsw(List *plans, Oid am, HnswScope scope)
{
    ListCell *cell;
    foreach(cell, plans)
        if (plan_has_hnsw(lfirst(cell), am, scope))
            return true;
    return false;
}

static bool
plan_has_hnsw(Plan *plan, Oid am, HnswScope scope)
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
        matched = RelationGetForm(index)->relam == am && index_in_scope(index, scope);
        index_close(index, AccessShareLock);
        if (matched)
            return true;
    }
    if (plan_has_hnsw(outerPlan(plan), am, scope) || plan_has_hnsw(innerPlan(plan), am, scope))
        return true;
    switch (nodeTag(plan)) {
        case T_Append: return plan_list_has_hnsw(((Append *) plan)->appendplans, am, scope);
        case T_MergeAppend: return plan_list_has_hnsw(((MergeAppend *) plan)->mergeplans, am, scope);
        case T_BitmapAnd: return plan_list_has_hnsw(((BitmapAnd *) plan)->bitmapplans, am, scope);
        case T_BitmapOr: return plan_list_has_hnsw(((BitmapOr *) plan)->bitmapplans, am, scope);
        case T_SubqueryScan: return plan_has_hnsw(((SubqueryScan *) plan)->subplan, am, scope);
        case T_CustomScan: return plan_list_has_hnsw(((CustomScan *) plan)->custom_plans, am, scope);
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
plan_once(Query *query, const char *text, int options, ParamListInfo params, bool exact, bool cover, bool security)
{
    bool saved = protected_planning;
    bool saved_exact = exact_planning;
    bool saved_cover = cover_planning;
    volatile PlannedStmt *result = NULL;
    PG_TRY();
    {
        protected_planning = security;
        exact_planning = exact;
        cover_planning = cover;
        if (previous_planner) result = previous_planner(query, text, options, params);
        else result = standard_planner(query, text, options, params);
    }
    PG_FINALLY();
    {
        protected_planning = saved;
        exact_planning = saved_exact;
        cover_planning = saved_cover;
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

/* True when the final plan keeps a pg_cuvs_hnsw scan that must not run: in exact mode, a protected statement (a security
 * barrier, a plan that depends on the role, row-level security on any relation of the range table) with any pg_cuvs_hnsw scan,
 * or any scan of an index with an expression or predicate; without the setting, a protected statement with a scan of an index
 * that has an expression or predicate and that the exact module serves. The scan refuses to run in the same cases, so a plan
 * that passes this check does not stop with an error at run time unless something changed after planning. */
static bool
plan_keeps_unsafe_scan(PlannedStmt *statement, Oid am, bool exact, bool cover, bool security)
{
    bool protect = security || statement_has_marker(statement) || statement_has_rls_relation(statement);
    HnswScope scope = planning_scope(exact, cover, protect);

    if (scope == SCOPE_NONE)
        return false;
    return plan_has_hnsw(statement->planTree, am, scope) ||
        plan_list_has_hnsw(statement->subplans, am, scope);
}

/* Whether the relation has an index of the pg_cuvs_hnsw access method with an expression or a predicate. The relation is one of the
 * statement's own, which parse analysis or the plan cache has locked; its indexes are not locked yet, so they are looked at in the
 * catalog cache only (never opened). It decides only whether the statement gets the extra planning work, so it may say yes too often. */
static bool
rel_has_hnsw_exprpred_index(Oid relid, Oid am)
{
    Relation rel = relation_open(relid, NoLock);
    List *indexes;
    ListCell *cell;
    bool found = false;

    if (!rel->rd_rel->relhasindex) {
        relation_close(rel, NoLock);
        return false;
    }
    indexes = RelationGetIndexList(rel);
    foreach(cell, indexes) {
        HeapTuple tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(lfirst_oid(cell)));
        bool ours;

        if (!HeapTupleIsValid(tuple))
            continue;
        ours = ((Form_pg_class) GETSTRUCT(tuple))->relam == am;
        ReleaseSysCache(tuple);
        if (!ours)
            continue;
        tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(lfirst_oid(cell)));
        if (HeapTupleIsValid(tuple)) {
            found = !heap_attisnull(tuple, Anum_pg_index_indexprs, NULL) ||
                !heap_attisnull(tuple, Anum_pg_index_indpred, NULL);
            ReleaseSysCache(tuple);
        }
        if (found)
            break;
    }
    list_free(indexes);
    relation_close(rel, NoLock);
    return found;
}

/* A function in FROM that PostgreSQL may inline into the statement: a SQL-language function (its body is a query, which can read a
 * table that the statement does not name). Functions in other languages, generate_series and unnest among them, are never inlined. */
static bool
rte_function_may_inline(RangeTblEntry *rte)
{
    ListCell *cell;

    foreach(cell, rte->functions) {
        RangeTblFunction *rtfunc = lfirst_node(RangeTblFunction, cell);
        HeapTuple tuple;
        bool sql;

        if (!IsA(rtfunc->funcexpr, FuncExpr))
            return true;
        tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(((FuncExpr *) rtfunc->funcexpr)->funcid));
        if (!HeapTupleIsValid(tuple))
            return true;
        sql = ((Form_pg_proc) GETSTRUCT(tuple))->prolang == SQLlanguageId;
        ReleaseSysCache(tuple);
        if (sql)
            return true;
    }
    return false;
}

static bool
query_touches_cover_exprpred(Node *node, void *context)
{
    Oid am = *(Oid *) context;

    if (!node)
        return false;
    if (IsA(node, Query)) {
        Query *query = (Query *) node;
        ListCell *cell;

        foreach(cell, query->rtable) {
            RangeTblEntry *rte = lfirst_node(RangeTblEntry, cell);

            if (rte->rtekind == RTE_FUNCTION && rte_function_may_inline(rte))
                return true;    /* the inlined body may read a table that the statement does not name */
            if (rte->rtekind != RTE_RELATION)
                continue;
            /* Partitions and inheritance children enter the range table only while planning, and opening them here would lock
             * every one of them before partitions are pruned: a statement on a table that has children always gets the extra work
             * (rte->inh is set for every table named without ONLY, has_subclass for the ones that have children). */
            if ((rte->inh && has_subclass(rte->relid)) || rel_has_hnsw_exprpred_index(rte->relid, am))
                return true;
        }
        return query_tree_walker(query, query_touches_cover_exprpred, context, 0);
    }
    return expression_tree_walker(node, query_touches_cover_exprpred, context);
}

bool
cuvs_hnsw_rls_statement_has_rls_relation(void)
{
    if (executing_rls < 0)
        executing_rls = executing_statement && statement_has_rls_relation(executing_statement);
    return executing_rls > 0;
}

static PlannedStmt *
protected_plan(Query *query, const char *text, int options, ParamListInfo params)
{
    bool exact = cuvs_hnsw_scan_exact_requested();
    Oid am = get_am_oid("pg_cuvs_hnsw", true);
    bool cover = !exact && OidIsValid(am);
    /* Without the setting only a statement that reads such a table, or a function that may read one, is planned with the
     * barrier look-ahead and a copy to plan again from. Every other statement is cheap: its paths are filtered while they are made,
     * which also sees a relation that enters the statement through an inlined function. What this leaves (a security barrier that
     * enters through an inlined scalar function) is stopped by the scan itself. */
    bool active = exact || (cover && query_touches_cover_exprpred((Node *) query, &am));
    /* Optimization only: a protected shape visible before planning is planned once, without
     * any pg_cuvs_hnsw path. What this misses is caught by the check after planning. */
    bool security = active && query_has_barrier((Node *) query, &exact);
    /* The planner scribbles on its input (it pulls subqueries up and replaces function
     * calls in place), and planning an already planned Query again is not something
     * PostgreSQL promises to support. A pristine copy is kept for the one case that needs
     * a second planning: see below. No test tells the copy from the planned Query
     * (planning the planned Query again gave the same plans for every shape tried), so this
     * rests on the contract, not on an observed difference. */
    Query *pristine = (active && !security) ? copyObject(query) : NULL;
    PlannedStmt *result = plan_once(query, text, options, params, exact, cover, security);

    if (active && OidIsValid(am)) {
        /* Safety net. Row-level security and security barriers that only appear while
         * planning (an inlined SQL function, a view, a partitioned parent whose policy is
         * not copied to its partitions) are removed path by path where possible
         * (protected_paths), but that cannot see everything and a security barrier
         * subquery was planned before anything could be removed inside it. Whatever the
         * earlier steps did, a final plan that keeps a scan that must not run
         * must not leave planning: plan the statement again with every pg_cuvs_hnsw path
         * of the scope removed. Planning side effects of that statement, such as the NOTICE of an
         * IMMUTABLE function evaluated on constants, then happen twice. */
        if (pristine && plan_keeps_unsafe_scan(result, am, exact, cover, security)) {
            security = true;
            result = plan_once(pristine, text, options, params, exact, cover, security);
        }
        /* Explicit exact mode stops here. Without the setting the scan itself stops a plan that kept the index, with a
         * message that names the cause (cuvs_hnsw_scan.c), so that EXPLAIN of the statement still works. */
        if (exact && plan_keeps_unsafe_scan(result, am, exact, cover, security))
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("an exact query retained an unsupported HNSW index path")));
    }
    return result;
}

static PlannedStmt *
protected_planner(Query *query, const char *text, int options, ParamListInfo params)
{
    /* Nothing to remove and nothing to look ahead for: plan as if this module were not loaded (see cover_known). */
    if (!cuvs_hnsw_scan_exact_requested() && !exprpred_cover_index_exists())
        return previous_planner ? previous_planner(query, text, options, params) : standard_planner(query, text, options, params);
    return protected_plan(query, text, options, params);
}

/* What the scan needs to know about the statement it runs for: the statement, whether it carries a marker, and (looked at when asked) whether
 * a table of its range table has row-level security. Every phase of the executor in which an index scan can begin or restart is hooked:
 * ExecutorStart (an index scan without runtime keys starts in ExecInitIndexScan), ExecutorRun, and ExecutorFinish (a data-modifying
 * CTE that the main query does not read runs to its end there). A nested statement (a function, SPI, a trigger, a cursor) has its own
 * QueryDesc and replaces the remembered one for as long as it runs; a parallel worker runs its own QueryDesc of the same plan. */
typedef struct StatementMark {
    int marker;
    PlannedStmt *statement;
    int rls;
} StatementMark;

static void
mark_enter(QueryDesc *desc, StatementMark *saved)
{
    saved->marker = executing_marker;
    saved->statement = executing_statement;
    saved->rls = executing_rls;
    executing_statement = desc->plannedstmt;
    executing_rls = -1;
    executing_marker = -1;
}

static void
mark_leave(const StatementMark *saved)
{
    executing_marker = saved->marker;
    executing_statement = saved->statement;
    executing_rls = saved->rls;
}

bool
cuvs_hnsw_rls_statement_known(void)
{
    return executing_statement != NULL;
}

static void
protected_start(QueryDesc *desc, int eflags)
{
    StatementMark saved;

    /* without an index of the access method no scan of one can start in this statement */
    if (!cover_index_exists()) {
        if (previous_start) previous_start(desc, eflags);
        else standard_ExecutorStart(desc, eflags);
        return;
    }
    mark_enter(desc, &saved);
    PG_TRY();
    {
        if (previous_start) previous_start(desc, eflags);
        else standard_ExecutorStart(desc, eflags);
    }
    PG_FINALLY();
    {
        mark_leave(&saved);
    }
    PG_END_TRY();
}

static void
protected_run(QueryDesc *desc, ScanDirection direction, uint64 count, bool once)
{
    StatementMark saved;

    if (!cover_index_exists()) {
        if (previous_run) previous_run(desc, direction, count, once);
        else standard_ExecutorRun(desc, direction, count, once);
        return;
    }
    mark_enter(desc, &saved);
    PG_TRY();
    {
        if (previous_run) previous_run(desc, direction, count, once);
        else standard_ExecutorRun(desc, direction, count, once);
    }
    PG_FINALLY();
    {
        mark_leave(&saved);
    }
    PG_END_TRY();
}

static void
protected_finish(QueryDesc *desc)
{
    StatementMark saved;

    if (!cover_index_exists()) {
        if (previous_finish) previous_finish(desc);
        else standard_ExecutorFinish(desc);
        return;
    }
    mark_enter(desc, &saved);
    PG_TRY();
    {
        if (previous_finish) previous_finish(desc);
        else standard_ExecutorFinish(desc);
    }
    PG_FINALLY();
    {
        mark_leave(&saved);
    }
    PG_END_TRY();
}

void
cuvs_hnsw_rls_init(void)
{
    CacheRegisterSyscacheCallback(INDEXRELID, cover_invalidate, (Datum) 0);
    CacheRegisterSyscacheCallback(AMOID, cover_invalidate, (Datum) 0);
    previous_planner = planner_hook;
    planner_hook = protected_planner;
    previous_paths = set_rel_pathlist_hook;
    set_rel_pathlist_hook = protected_paths;
    previous_start = ExecutorStart_hook;
    ExecutorStart_hook = protected_start;
    previous_run = ExecutorRun_hook;
    ExecutorRun_hook = protected_run;
    previous_finish = ExecutorFinish_hook;
    ExecutorFinish_hook = protected_finish;
}
