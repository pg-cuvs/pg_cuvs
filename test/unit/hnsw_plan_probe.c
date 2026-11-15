/* Test-only planner probe, never installed by the extension. Loaded with LOAD
 * after pg_cuvs, it re-adds the pg_cuvs_hnsw index paths that the exact-mode
 * path filter removed, so the post-planning safety check can be exercised.
 *   probe.readd                 every planning of the statement (the second one too)
 *   probe.readd_first_planning  only the first planning of the statement: the path-level
 *                               removal is switched off for it, the planning again is not */
#include "postgres.h"
#include "fmgr.h"
#include "commands/defrem.h"
#include "nodes/pathnodes.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

static set_rel_pathlist_hook_type previous_hook;
static planner_hook_type previous_planner;
static bool readd_paths = false;
static bool readd_first_planning = false;
static PlannerGlobal *first_glob = NULL;

/* The planning that pg_cuvs repeats starts from standard_planner, not from this hook,
 * so only the first planning of a statement reaches here: its PlannerGlobal is the one
 * whose paths this probe puts back. */
static PlannedStmt *
probe_planner(Query *parse, const char *query_string, int cursorOptions, ParamListInfo boundParams)
{
    first_glob = NULL;
    if (previous_planner)
        return previous_planner(parse, query_string, cursorOptions, boundParams);
    return standard_planner(parse, query_string, cursorOptions, boundParams);
}

static void
probe_paths(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
    List *saved = NIL;
    ListCell *cell;
    Oid am;

    if (readd_first_planning && first_glob == NULL)
        first_glob = root->glob;
    if ((readd_paths || (readd_first_planning && root->glob == first_glob)) && rte->rtekind == RTE_RELATION)
        saved = list_copy(rel->pathlist);
    if (previous_hook)
        previous_hook(root, rel, rti, rte);
    if (!saved)
        return;
    am = get_am_oid("pg_cuvs_hnsw", true);
    foreach(cell, saved) {
        Path *path = lfirst(cell);
        if (IsA(path, IndexPath) && ((IndexPath *) path)->indexinfo->relam == am &&
            !list_member_ptr(rel->pathlist, path))
            add_path(rel, path);
    }
}

void _PG_init(void);

void
_PG_init(void)
{
    DefineCustomBoolVariable("probe.readd", "Re-add pg_cuvs_hnsw index paths after the exact filter.",
                             NULL, &readd_paths, false, PGC_USERSET, 0, NULL, NULL, NULL);
    DefineCustomBoolVariable("probe.readd_first_planning", "Re-add the paths in the first planning of a statement only.",
                             NULL, &readd_first_planning, false, PGC_USERSET, 0, NULL, NULL, NULL);
    previous_planner = planner_hook;
    planner_hook = probe_planner;
    previous_hook = set_rel_pathlist_hook;
    set_rel_pathlist_hook = probe_paths;
}
