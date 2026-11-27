#include "postgres.h"
#include "access/heapam.h"
#include "access/genam.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/pg_am_d.h"
#include "access/transam.h"
#include "nodes/nodeFuncs.h"
#include "nodes/makefuncs.h"
#include "nodes/primnodes.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "parser/parse_oper.h"
#include "storage/bufmgr.h"
#include "common/hashfn.h"
#include "utils/float.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"
#include "utils/rel.h"
#include "cuvs_hnsw_exact.h"

/* One candidate answer. In-memory answers live in a binary min-heap ordered like the
 * SQL ORDER BY: distance ascending (NaN after every number), NULL distance last,
 * equal distances by root TID. Once they would exceed work_mem the answers move to
 * a tuplesort, which spills to PostgreSQL temporary files. */
typedef struct ExactEntry {
    float8 distance;
    ItemPointerData root;
    bool is_null;
} ExactEntry;

struct CuvsHnswExact {
    IndexScanDesc scan;
    TupleDesc sort_desc;
    BlockNumber root_block;
    bool root_cached;
    OffsetNumber root_offsets[MaxHeapTuplesPerPage];
    ExactEntry *heap;
    Size heap_used;
    Size heap_cap;
    Size limit;
    uint64 served;
    uint64 served_xor;
    uint64 served_sum;
    bool bounded;
    bool max_built;
    bool overflow;
    bool second_pass;
    MemoryContext context;
    EState *estate;
    ExprContext *expression;
    IndexInfo *index_info;
    ExprState *predicate;
    TupleTableSlot *heap_slot;
    TupleTableSlot *sort_in;
    TupleTableSlot *sort_out;
    Tuplesortstate *distances;
    TableScanDesc table_scan;
    /* Subset mode: only these index entry TIDs are examined (the ANN answers and the ledger keys of a certified
     * index), each followed through its HOT chain to the version the snapshot sees. */
    bool subset;
    const ItemPointerData *cand;
    Size ncand;
};

/*
 * A predicate that only PostgreSQL's own code can run. The exact module evaluates the predicate of a partial index for rows that the
 * statement may never have mentioned (the planner chooses the index when the statement's conditions IMPLY the predicate), so a
 * function that a user wrote must not run there. Every function, operator and type that the predicate uses must be one that the
 * system created (OID below FirstNormalObjectId), and a node kind that is not listed is refused. Types count because a built-in
 * function can call user code through its type: the operators of a user type, its input and output functions, a domain's CHECK.
 * Collations do not: a collation selects a locale of the C library or of ICU.
 */
static bool
exact_is_builtin_oid(Oid oid)
{
    return OidIsValid(oid) && oid < FirstNormalObjectId;
}

static bool
exact_pred_unsafe(Node *node, void *context)
{
    if (node == NULL)
        return false;
    switch (nodeTag(node)) {
    case T_List:
    case T_Var:
    case T_Const:
    case T_RelabelType:
    case T_NullTest:
    case T_BooleanTest:
    case T_BoolExpr:
    case T_CaseExpr:
    case T_CaseWhen:
    case T_CaseTestExpr:
    case T_CoalesceExpr:
        break;
    case T_FuncExpr: {
        FuncExpr *f = (FuncExpr *) node;
        ListCell *cell;

        if (!exact_is_builtin_oid(f->funcid) || !exact_is_builtin_oid(f->funcresulttype))
            return true;
        foreach(cell, f->args)
            if (!exact_is_builtin_oid(exprType((Node *) lfirst(cell))))
                return true;
        break;
    }
    case T_OpExpr:
    case T_DistinctExpr:
    case T_NullIfExpr: {
        OpExpr *o = (OpExpr *) node;      /* the three have the same leading fields */
        ListCell *cell;

        set_opfuncid(o);
        if (!exact_is_builtin_oid(o->opfuncid) || !exact_is_builtin_oid(o->opresulttype))
            return true;
        foreach(cell, o->args)
            if (!exact_is_builtin_oid(exprType((Node *) lfirst(cell))))
                return true;
        break;
    }
    case T_ScalarArrayOpExpr: {
        ScalarArrayOpExpr *a = (ScalarArrayOpExpr *) node;
        ListCell *cell;

        set_sa_opfuncid(a);
        if (!exact_is_builtin_oid(a->opfuncid))
            return true;
        foreach(cell, a->args)
            if (!exact_is_builtin_oid(exprType((Node *) lfirst(cell))))
                return true;
        break;
    }
    case T_CoerceViaIO: {
        CoerceViaIO *c = (CoerceViaIO *) node;

        if (!exact_is_builtin_oid(exprType((Node *) c->arg)) || !exact_is_builtin_oid(c->resulttype))
            return true;
        break;
    }
    case T_ArrayCoerceExpr: {
        ArrayCoerceExpr *c = (ArrayCoerceExpr *) node;

        if (!exact_is_builtin_oid(exprType((Node *) c->arg)) || !exact_is_builtin_oid(c->resulttype))
            return true;
        break;
    }
    case T_ArrayExpr: {
        ArrayExpr *a = (ArrayExpr *) node;

        if (!exact_is_builtin_oid(a->array_typeid) || !exact_is_builtin_oid(a->element_typeid))
            return true;
        break;
    }
    case T_MinMaxExpr: {
        MinMaxExpr *m = (MinMaxExpr *) node;

        if (!exact_is_builtin_oid(m->minmaxtype))
            return true;
        break;
    }
    default:
        /* CoerceToDomain, RowExpr, FieldSelect, SubLink, SubscriptingRef, XmlExpr, SQLValueFunction, a Param, an Aggref ...: refused */
        return true;
    }
    return expression_tree_walker(node, exact_pred_unsafe, context);
}

bool
cuvs_hnsw_exact_predicate_builtin(Relation index)
{
    List *pred = RelationGetIndexPredicate(index);

    return pred == NIL || !exact_pred_unsafe((Node *) pred, NULL);
}

/* Visible heap-only TIDs are not valid index answers. Resolve a root while
 * holding the page lock; release it before expressions, heap fetch or sorting. */
static bool
exact_heap_root(CuvsHnswExact *state, ItemPointer visible, ItemPointer root)
{
    Relation heap = state->scan->heapRelation;
    BlockNumber block = ItemPointerGetBlockNumber(visible);
    OffsetNumber offset = ItemPointerGetOffsetNumber(visible);
    OffsetNumber found;

    if (RelationGetForm(heap)->relam != HEAP_TABLE_AM_OID)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("HNSW exact heap routing requires the heap table access method")));
    if (!OffsetNumberIsValid(offset) || offset > MaxHeapTuplesPerPage)
        return false;
    /* The scan visits the tuples of a page together, so read the page's root map
     * once. A tuple that is visible to the snapshot keeps its chain root even if
     * the page is pruned afterwards. */
    if (!state->root_cached || state->root_block != block) {
        Buffer buffer = ReadBuffer(heap, block);

        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        memset(state->root_offsets, 0, sizeof(state->root_offsets));
        heap_get_root_tuples(BufferGetPage(buffer), state->root_offsets);
        UnlockReleaseBuffer(buffer);
        state->root_block = block;
        state->root_cached = true;
    }
    found = state->root_offsets[offset - 1];
    if (!OffsetNumberIsValid(found))
        return false;
    ItemPointerSet(root, block, found);
    return true;
}

static Tuplesortstate *
exact_sort(TupleDesc descriptor, bool distance_order)
{
    AttrNumber attributes[2] = {2, 1};
    Oid operators[2], collations[2] = {InvalidOid, InvalidOid};
    bool nulls_first[2] = {false, false};

    get_sort_group_operators(TIDOID, true, false, false,
                            &operators[1], NULL, NULL, NULL);
    if (distance_order)
        get_sort_group_operators(FLOAT8OID, true, false, false,
                                &operators[0], NULL, NULL, NULL);
    else {
        attributes[0] = 1;
        operators[0] = operators[1];
    }
    return tuplesort_begin_heap(descriptor, distance_order ? 2 : 1,
                                attributes, operators, collations, nulls_first,
                                work_mem, NULL, TUPLESORT_NONE);
}

static bool
exact_before(const ExactEntry *a, const ExactEntry *b)
{
    int order;

    if (a->is_null != b->is_null)
        return !a->is_null;
    if (!a->is_null) {
        order = float8_cmp_internal(a->distance, b->distance);
        if (order != 0)
            return order < 0;
    }
    return ItemPointerCompare((ItemPointer) &a->root, (ItemPointer) &b->root) < 0;
}

/* A min-heap puts the first answer in SQL order at the root; the bounded first
 * pass uses a max-heap so that the root is the worst answer it still keeps. */
static bool
exact_ahead(const ExactEntry *a, const ExactEntry *b, bool worst_first)
{
    return worst_first ? exact_before(b, a) : exact_before(a, b);
}

static void
exact_sift_down(ExactEntry *heap, Size used, Size at, bool worst_first)
{
    ExactEntry moving = heap[at];

    for (;;) {
        Size child = 2 * at + 1;

        if (child >= used)
            break;
        if (child + 1 < used && exact_ahead(&heap[child + 1], &heap[child], worst_first))
            child++;
        if (!exact_ahead(&heap[child], &moving, worst_first))
            break;
        heap[at] = heap[child];
        at = child;
    }
    heap[at] = moving;
}

static void
exact_put_sorted(CuvsHnswExact *state, const ExactEntry *entry)
{
    ExecClearTuple(state->sort_in);
    state->sort_in->tts_values[0] = PointerGetDatum(&entry->root);
    state->sort_in->tts_isnull[0] = false;
    state->sort_in->tts_values[1] = Float8GetDatum(entry->distance);
    state->sort_in->tts_isnull[1] = entry->is_null;
    ExecStoreVirtualTuple(state->sort_in);
    tuplesort_puttupleslot(state->distances, state->sort_in);
}

/* Keep answers in memory while they fit in work_mem; beyond that sort externally. */
static void
exact_emit(CuvsHnswExact *state, ItemPointer root, Datum distance, bool distance_null)
{
    ExactEntry entry;

    entry.distance = distance_null ? 0 : DatumGetFloat8(distance);
    entry.root = *root;
    entry.is_null = distance_null;
    if (state->bounded) {
        /* First pass: keep only the best `limit` answers. Most queries stop long before.
         * While everything fits the array is only appended to; it becomes a max-heap
         * (worst answer at the root) when the first answer has to be dropped. */
        if (state->heap_used < state->limit) {
            if (state->heap_used == state->heap_cap) {
                state->heap_cap = state->heap_cap ? state->heap_cap * 2 : 1024;
                if (state->heap_cap > state->limit)
                    state->heap_cap = state->limit;
                if (state->heap)
                    state->heap = repalloc_huge(state->heap, state->heap_cap * sizeof(ExactEntry));
                else
                    state->heap = MemoryContextAllocHuge(state->context, state->heap_cap * sizeof(ExactEntry));
            }
            state->heap[state->heap_used++] = entry;
        } else {
            if (!state->max_built) {
                Size at;

                for (at = state->heap_used / 2; at-- > 0;)
                    exact_sift_down(state->heap, state->heap_used, at, true);
                state->max_built = true;
            }
            state->overflow = true;
            if (exact_before(&entry, &state->heap[0])) {
                state->heap[0] = entry;
                exact_sift_down(state->heap, state->heap_used, 0, true);
            }
        }
        return;
    }
    /* Second pass: more answers than work_mem holds were asked for, so everything goes through
     * tuplesort, which spills to temporary files. */
    if (!state->distances)
        state->distances = exact_sort(state->sort_desc, true);
    exact_put_sorted(state, &entry);
}

static void
exact_finish(CuvsHnswExact *state)
{
    Size at;

    if (state->distances) {
        tuplesort_performsort(state->distances);
        return;
    }
    /* Floyd heap construction is linear; each answer is then taken on demand. */
    for (at = state->heap_used / 2; at-- > 0;)
        exact_sift_down(state->heap, state->heap_used, at, false);
}

static void
exact_store(CuvsHnswExact *state, ItemPointer root)
{
    Datum values[INDEX_MAX_KEYS];
    bool nulls[INDEX_MAX_KEYS];
    ScanKey key = &state->scan->orderByData[0];
    Datum distance = (Datum)0;
    bool distance_null;
    MemoryContext previous;

    CHECK_FOR_INTERRUPTS();
    ResetExprContext(state->expression);
    state->expression->ecxt_scantuple = state->heap_slot;
    if (state->predicate && !ExecQual(state->predicate, state->expression))
        return;
    previous = MemoryContextSwitchTo(state->expression->ecxt_per_tuple_memory);
    FormIndexDatum(state->index_info, state->heap_slot, state->estate, values, nulls);
    distance_null = nulls[0] || (key->sk_flags & SK_ISNULL);
    if (!distance_null)
        distance = FunctionCall2Coll(&key->sk_func, key->sk_collation,
                                     values[0], key->sk_argument);
    MemoryContextSwitchTo(previous);

    exact_emit(state, root, distance, distance_null);
}

static void
exact_collect_subset(CuvsHnswExact *state)
{
    IndexFetchTableData *fetch = table_index_fetch_begin(state->scan->heapRelation);
    Size at;

    for (at = 0; at < state->ncand; at++) {
        ItemPointerData tid = state->cand[at];
        bool call_again = false, all_dead = false;

        CHECK_FOR_INTERRUPTS();
        /* A key may point at a tuple that was rolled back or vacuumed away: nothing visible, nothing to rank. */
        if (table_index_fetch_tuple(fetch, &tid, state->scan->xs_snapshot, state->heap_slot, &call_again, &all_dead))
            exact_store(state, &tid);
    }
    table_index_fetch_end(fetch);
}

static void
exact_collect_heap(CuvsHnswExact *state)
{
    if (state->subset) {
        exact_collect_subset(state);
        return;
    }
    state->root_cached = false;
    state->table_scan = table_beginscan(state->scan->heapRelation,
                                       state->scan->xs_snapshot, 0, NULL);
    while (table_scan_getnextslot(state->table_scan, ForwardScanDirection,
                                  state->heap_slot)) {
        ItemPointerData root;
        if (!exact_heap_root(state, &state->heap_slot->tts_tid, &root))
            ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                            errmsg("could not resolve a visible heap tuple to its HOT root")));
        exact_store(state, &root);
    }
    table_endscan(state->table_scan);
    state->table_scan = NULL;
}

static CuvsHnswExact *exact_begin_impl(IndexScanDesc scan, bool subset, const ItemPointerData *cand, Size ncand, bool cover);

CuvsHnswExact *
cuvs_hnsw_exact_begin(IndexScanDesc scan)
{
    return exact_begin_impl(scan, false, NULL, 0, false);
}

CuvsHnswExact *
cuvs_hnsw_exact_begin_cover(IndexScanDesc scan)
{
    return exact_begin_impl(scan, false, NULL, 0, true);
}

CuvsHnswExact *
cuvs_hnsw_exact_begin_tids(IndexScanDesc scan, const ItemPointerData *tids, Size ntids)
{
    return exact_begin_impl(scan, true, tids, ntids, true);
}

static CuvsHnswExact *
exact_begin_impl(IndexScanDesc scan, bool subset, const ItemPointerData *cand, Size ncand, bool cover)
{
    CuvsHnswExact *state;
    MemoryContext previous;
    TupleDesc descriptor;

    if (!scan || !scan->heapRelation || scan->numberOfKeys != 0 ||
        scan->numberOfOrderBys != 1 || !scan->orderByData ||
        !scan->xs_snapshot || !IsMVCCSnapshot(scan->xs_snapshot) ||
        RelationGetForm(scan->heapRelation)->relam != HEAP_TABLE_AM_OID)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("HNSW exact routing requires a heap MVCC scan with no search keys and one ordering operator")));
    /* RLS quals must run before index expressions/predicates: this module ranks rows the statement may not see, and an
     * expression or a predicate is a function call on each of them. Those scans use the native executor plan, which
     * planning chooses (cuvs_hnsw_rls.c); a direct utility caller must not bypass that boundary. A plain column is not
     * a call of user code: the ordering function of the operator class is applied to a stored vector, as an index scan
     * does for the rows that are hidden, and the executor applies the row-level quals to the rows that come out. Only the
     * scan of an index with a cover descriptor (`cover`) relies on that; any other caller is refused on every such table. */
    if (RelationGetForm(scan->heapRelation)->relrowsecurity &&
        (!cover ||
         RelationGetIndexExpressions(scan->indexRelation) != NIL ||
         RelationGetIndexPredicate(scan->indexRelation) != NIL))
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("RLS exact scans require the PostgreSQL executor path")));
    if (!OidIsValid(scan->orderByData[0].sk_func.fn_oid) ||
        scan->orderByData[0].sk_func.fn_nargs != 2 ||
        scan->orderByData[0].sk_func.fn_retset ||
        !scan->orderByData[0].sk_func.fn_strict ||
        get_func_rettype(scan->orderByData[0].sk_func.fn_oid) != FLOAT8OID)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("HNSW exact routing requires a strict scalar float8 ordering function")));
    state = palloc0(sizeof(*state));
    state->scan = scan;
    state->subset = subset;
    state->cand = cand;
    state->ncand = ncand;
    state->context = AllocSetContextCreate(CurrentMemoryContext, "HNSW exact cover", ALLOCSET_DEFAULT_SIZES);
    previous = MemoryContextSwitchTo(state->context);
    PG_TRY();
    {
        state->estate = CreateExecutorState();
        state->estate->es_snapshot = scan->xs_snapshot;
        state->expression = GetPerTupleExprContext(state->estate);
        state->index_info = BuildIndexInfo(scan->indexRelation);
        if (state->index_info->ii_Predicate != NIL && !cuvs_hnsw_exact_predicate_builtin(scan->indexRelation))
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("pg_cuvs: index \"%s\" cannot answer this statement", RelationGetRelationName(scan->indexRelation)),
                            errdetail("The predicate of the index calls a function, operator or type that is not built in, and ranking rows by this index would run it."),
                            errhint("Planning does not choose such an index; run the statement again, or use a predicate of built-in functions only.")));
        state->predicate = ExecPrepareQual(state->index_info->ii_Predicate, state->estate);
        state->heap_slot = table_slot_create(scan->heapRelation, NULL);
        descriptor = CreateTemplateTupleDesc(2);
        TupleDescInitEntry(descriptor, 1, "root", TIDOID, -1, 0);
        TupleDescInitEntry(descriptor, 2, "distance", FLOAT8OID, -1, 0);
        state->sort_in = MakeSingleTupleTableSlot(descriptor, &TTSOpsVirtual);
        state->sort_out = MakeSingleTupleTableSlot(descriptor, &TTSOpsMinimalTuple);
        state->sort_desc = descriptor;
        state->bounded = true;
        state->limit = (Size) work_mem * 1024L / sizeof(ExactEntry);
        if (state->limit < 1)
            state->limit = 1;
        exact_collect_heap(state);
        exact_finish(state);
        MemoryContextSwitchTo(previous);
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(previous);
        cuvs_hnsw_exact_end(state);
        PG_RE_THROW();
    }
    PG_END_TRY();
    return state;
}

/* A position-independent digest of the roots handed out so far. The second pass uses it to
 * prove that the rows it skips are exactly the rows the first pass returned. */
static uint64
exact_root_hash(ItemPointer root)
{
    return hash_bytes_extended((const unsigned char *) root, sizeof(ItemPointerData), UINT64CONST(0x2033));
}

/* Take the next answer in SQL order from whichever structure holds them. */
static bool
exact_take(CuvsHnswExact *state, ItemPointer root, Datum *distance, bool *isnull)
{
    Datum tid;
    bool root_null;

    if (!state->distances) {
        if (state->heap_used == 0)
            return false;
        *root = state->heap[0].root;
        *distance = Float8GetDatum(state->heap[0].distance);
        *isnull = state->heap[0].is_null;
        state->heap[0] = state->heap[--state->heap_used];
        exact_sift_down(state->heap, state->heap_used, 0, false);
        return true;
    }
    if (!tuplesort_gettupleslot(state->distances, true, false, state->sort_out, NULL))
        return false;
    tid = slot_getattr(state->sort_out, 1, &root_null);
    if (root_null)
        elog(ERROR, "NULL HNSW exact output root");
    *root = *((ItemPointer)DatumGetPointer(tid));
    *distance = slot_getattr(state->sort_out, 2, isnull);
    return true;
}

/* The caller asked for more answers than the bounded first pass kept. Walk the heap again
 * under the same snapshot, this time keeping everything through tuplesort (temporary files
 * when it does not fit work_mem), and skip the answers already returned. The skipped
 * answers must be exactly the rows the first pass handed out; a STABLE ordering function
 * whose value changed in between makes that false, and the scan stops with an error. */
static void
exact_second_pass(CuvsHnswExact *state)
{
    MemoryContext previous = MemoryContextSwitchTo(state->context);
    uint64 skip;

    if (state->heap)
        pfree(state->heap);
    state->heap = NULL;
    state->heap_used = state->heap_cap = 0;
    state->bounded = false;
    state->second_pass = true;
    exact_collect_heap(state);
    exact_finish(state);
    MemoryContextSwitchTo(previous);
    {
        uint64 skipped_xor = 0, skipped_sum = 0;

        for (skip = 0; skip < state->served; skip++) {
            ItemPointerData root;
            Datum distance;
            bool isnull;

            CHECK_FOR_INTERRUPTS();
#ifdef CUVS_HNSW_EXACT_TEST_SKIP_DELAY_US
            /* Regression builds only (test_hnsw_exact_pg.py compiles this file privately):
             * make the skipping slow so a test can tell whether a cancel is honoured here. */
            pg_usleep(CUVS_HNSW_EXACT_TEST_SKIP_DELAY_US);
#endif
            if (!exact_take(state, &root, &distance, &isnull))
                break;
            skipped_xor ^= exact_root_hash(&root);
            skipped_sum += exact_root_hash(&root);
        }
        /* The ordering function can be STABLE and change between two FETCHes of one cursor.
         * Then the first `served` answers of this pass are not the rows already returned, and
         * continuing would repeat some rows and lose others without any error. */
        if (skip != state->served || skipped_xor != state->served_xor || skipped_sum != state->served_sum)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("the ordering function returned different results during one exact scan"),
                            errdetail("The rows returned before the scan had to read the heap again are not the first rows of the new pass."),
                            errhint("Use an IMMUTABLE ordering function, or fetch fewer rows than work_mem allows.")));
    }
}

bool
cuvs_hnsw_exact_next(CuvsHnswExact *state, ItemPointer root, Datum *distance, bool *isnull)
{
    CHECK_FOR_INTERRUPTS();
    if (!exact_take(state, root, distance, isnull)) {
        if (!state->overflow || state->second_pass)
            return false;
        exact_second_pass(state);
        if (!exact_take(state, root, distance, isnull))
            return false;
    }
    state->served++;
    state->served_xor ^= exact_root_hash(root);
    state->served_sum += exact_root_hash(root);
    return true;
}

void
cuvs_hnsw_exact_end(CuvsHnswExact *state)
{
    if (!state)
        return;
    if (state->table_scan) table_endscan(state->table_scan);
    if (state->heap_slot) ExecDropSingleTupleTableSlot(state->heap_slot);
    if (state->sort_in) ExecDropSingleTupleTableSlot(state->sort_in);
    if (state->sort_out) ExecDropSingleTupleTableSlot(state->sort_out);
    if (state->distances) tuplesort_end(state->distances);
    if (state->estate) FreeExecutorState(state->estate);
    MemoryContextDelete(state->context);
    pfree(state);
}
