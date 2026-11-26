#include "postgres.h"

#include "cuvs_cagra_pending_search.h"

#ifdef CUVS_PAGE_FORMAT
#include "access/tableam.h"
#include "catalog/pg_am_d.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/snapmgr.h"
#ifdef CUVS_CAGRA_FOLD_ENABLED
#include "access/htup_details.h"
#include "catalog/index.h"
#include "executor/executor.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#endif

static bool
copy_vector_datum(Datum datum, bool isnull, uint32 dim, float *out)
{
    struct varlena *full;
    int16 datum_dim;
    Size bytes = (Size) dim * sizeof(float);
    bool valid = false;

    if (isnull)
        return false;
    /* The tuple is visible to the scan snapshot, just as in a heap scan. */
    full = PG_DETOAST_DATUM(datum);
    if (VARSIZE(full) == VARHDRSZ + 2 * sizeof(int16) + bytes)
    {
        const char *data = VARDATA(full);

        memcpy(&datum_dim, data, sizeof(datum_dim));
        if (datum_dim == (int) dim)
        {
            memcpy(out, data + 2 * sizeof(int16), bytes);
            valid = true;
        }
    }
    if ((Pointer) full != DatumGetPointer(datum))
        pfree(full);
    return valid;
}

static bool
copy_visible_vector(TupleTableSlot *slot, AttrNumber attribute, uint32 dim, float *out)
{
    bool isnull;
    Datum datum = slot_getattr(slot, attribute, &isnull);

    return copy_vector_datum(datum, isnull, dim, out);
}

static bool
copy_supplement(Relation index, Relation heap, Snapshot snapshot,
                const CuvsCagraMeta *meta, BlockNumber blocks, uint64 from,
                uint64 upto, CuvsPageSupplement *supplement, bool shared, bool resident)
{
    bool expression = (meta->flags & CUVS_CAGRA_PENDING_VECTOR) != 0;
    AttrNumber attribute = index->rd_index->indkey.values[0];
    CuvsCagraPendingLocation *items;
    CuvsPagePosition *marks;
    float *vectors;
    uint32 packed = 0;
    uint32 count;
    CuvsCagraMeta canonical;

    if (!heap || RelationGetForm(heap)->relam != HEAP_TABLE_AM_OID ||
        !IsMVCCSnapshot(snapshot) || (!expression && attribute <= 0) ||
        !meta->dim || upto <= from || upto - from > CUVS_PAGE_BATCH_MAX)
        return false;

    count = (uint32) (upto - from);
    items = palloc(sizeof(*items) * count);
    marks = palloc0(sizeof(*marks) * count);
    vectors = palloc(sizeof(float) * (Size) count * meta->dim);
    bool ok = cuvs_cagra_copy_pending(index, meta, blocks, from, upto, items,
                                     expression ? vectors : NULL);
    if (ok && resident)
    {
        ok = cuvs_cagra_read_meta(index, &canonical) &&
            canonical.index_nonce == meta->index_nonce && canonical.base_nonce == meta->base_nonce &&
            canonical.base_slots == meta->base_slots && canonical.folded >= meta->folded;
        blocks = RelationGetNumberOfBlocks(index);
    }

    if (ok)
    {
        IndexFetchTableData *fetch = table_index_fetch_begin(heap);
        TupleTableSlot *slot = table_slot_create(heap, NULL);

        for (uint32 i = 0; i < count; ++i)
        {
            bool again = false, dead = false;
            float *destination = vectors + (Size) packed * meta->dim;

            CHECK_FOR_INTERRUPTS();
            if (resident && from + i < canonical.folded)
            {
                ItemPointerData root;
                CuvsCagraSlotResult state = cuvs_cagra_slot_tid(index, &canonical, blocks,
                                                               canonical.base_slots + from + i, &root);

                if (state == CUVS_CAGRA_SLOT_DEAD)
                    continue;
                if (state != CUVS_CAGRA_SLOT_LIVE || !ItemPointerIsValid(&items[i].tid) ||
                    !ItemPointerEquals(&root, &items[i].tid))
                {
                    ok = false;
                    break;
                }
            }
            if (!ItemPointerIsValid(&items[i].tid))
                continue;
            table_index_fetch_reset(fetch);
            if (!table_index_fetch_tuple(fetch, &items[i].tid, snapshot, slot, &again, &dead))
            {
                /* A different snapshot may still see this row. Only a
                 * physically invalid pending TID is a shared dead slot. */
                if (shared)
                {
                    count = i;
                    break;
                }
                marks[i].state = CUVS_PAGE_POSITION_INVISIBLE;
                continue;
            }
            if (expression)
                memmove(destination, vectors + (Size) i * meta->dim,
                        (Size) meta->dim * sizeof(float));
            else if (!copy_visible_vector(slot, attribute, meta->dim, destination))
            {
                ok = false;
                break;
            }
            marks[i] = (CuvsPagePosition) {.block = items[i].block,
                                         .state = CUVS_PAGE_POSITION_VECTOR};
            ++packed;
        }
        table_index_fetch_end(fetch);
        ExecDropSingleTupleTableSlot(slot);
    }
    pfree(items);
    if (!ok)
    {
        pfree(vectors);
        pfree(marks);
        return false;
    }
    *supplement = (CuvsPageSupplement) {.marks = marks, .vectors = vectors,
        .positions = count, .vector_rows = packed};
    return true;
}

bool
cuvs_cagra_local_supplement(Relation index, Relation heap, Snapshot snapshot,
                            const CuvsCagraMeta *meta, BlockNumber blocks,
                            uint64 from, uint64 upto, CuvsPageSupplement *supplement)
{
    return copy_supplement(index, heap, snapshot, meta, blocks, from, upto, supplement, false, false);
}

bool
cuvs_cagra_resident_supplement(Relation index, Relation heap, Snapshot snapshot,
                               const CuvsCagraMeta *meta, BlockNumber blocks, uint64 from,
                               uint64 upto, CuvsPageSupplement *supplement)
{
    /* The selected graph may predate the latest metapage. The append-only
     * pending pages still carry vectors, but their roots can be obsolete. */
    CuvsCagraMeta source = *meta;

    source.folded = Min(from, meta->folded);
    return copy_supplement(index, heap, snapshot, &source, blocks, from, upto, supplement, false, true);
}

#ifdef CUVS_CAGRA_FOLD_ENABLED
static CuvsCagraSlotResult
recovery_vector(Relation index, const CuvsCagraMeta *meta, BlockNumber blocks,
                uint64 slot_number, IndexFetchTableData *fetch, TupleTableSlot *slot,
                BlockNumber heap_blocks, Snapshot older, IndexInfo *info, EState *estate,
                float *out, bool *dead_in_heap)
{
    ItemPointerData tid, original;
    bool again = false;
    CuvsCagraSlotResult state = cuvs_cagra_slot_tid(index, meta, blocks, slot_number, &tid);

    *dead_in_heap = false;
    if (state != CUVS_CAGRA_SLOT_LIVE)
        return state;
    if (ItemPointerGetBlockNumber(&tid) >= heap_blocks ||
        ItemPointerGetOffsetNumber(&tid) > MaxHeapTuplesPerPage)
        return CUVS_CAGRA_SLOT_BAD;
    original = tid;
    table_index_fetch_reset(fetch);
    if (!table_index_fetch_tuple(fetch, &tid, GetActiveSnapshot(), slot, &again, NULL))
    {
        /* A delete invisible to the worker can still be visible to another
         * snapshot using this head. Keep its original vector if readable. */
        bool all_dead = false;

        tid = original;
        again = false;
        table_index_fetch_reset(fetch);
        if (!table_index_fetch_tuple(fetch, &tid, older, slot, &again, &all_dead))
        {
            *dead_in_heap = all_dead;
            return all_dead ? CUVS_CAGRA_SLOT_DEAD : CUVS_CAGRA_SLOT_BAD;
        }
    }
    if (info)
    {
        Datum values[INDEX_MAX_KEYS];
        bool nulls[INDEX_MAX_KEYS];
        ExprContext *expr = GetPerTupleExprContext(estate);
        MemoryContext previous;

        ResetExprContext(expr);
        expr->ecxt_scantuple = slot;
        previous = MemoryContextSwitchTo(expr->ecxt_per_tuple_memory);
        FormIndexDatum(info, slot, estate, values, nulls);
        bool valid = copy_vector_datum(values[0], nulls[0], meta->dim, out);
        MemoryContextSwitchTo(previous);
        return valid ? CUVS_CAGRA_SLOT_LIVE : CUVS_CAGRA_SLOT_BAD;
    }
    return copy_visible_vector(slot, index->rd_index->indkey.values[0], meta->dim, out) ?
        CUVS_CAGRA_SLOT_LIVE : CUVS_CAGRA_SLOT_BAD;
}

/* The caller guards this heap against VACUUM while spooling. A physical
 * invalid slot or a PG-confirmed globally dead HOT chain gets a placeholder.
 * A merely invisible row must be recovered for older snapshots. */
bool
cuvs_cagra_spool_recovery(Relation index, Relation heap, const CuvsCagraMeta *meta,
                         uint64 from, File file, uint32 *placeholder_rows, uint32 *physical_dead_slots)
{
    const uint32 batch_rows = 64;
    Size row_bytes = (Size) meta->dim * sizeof(float);
    BlockNumber blocks = RelationGetNumberOfBlocks(index);
    BlockNumber heap_blocks = RelationGetNumberOfBlocks(heap);
    float *sample, *batch;
    uint64 sample_slot;
    bool expression = index->rd_index->indkey.values[0] == 0;
    bool ok = false;
    Oid saved_user;
    int saved_sec, nest_level = 0;
    IndexInfo *info = NULL;
    EState *estate = NULL;
    SnapshotData older = {0};

    if (heap->rd_rel->relam != HEAP_TABLE_AM_OID || !row_bytes ||
        from >= meta->n_slots || meta->dead_slots == meta->n_slots)
        return false;
    *placeholder_rows = 0;
    *physical_dead_slots = 0;
    sample = palloc(row_bytes);
    batch = palloc(batch_rows * row_bytes);
    TupleTableSlot *slot = table_slot_create(heap, NULL);
    IndexFetchTableData *fetch = table_index_fetch_begin(heap);
    InitNonVacuumableSnapshot(older, GlobalVisTestFor(heap));

    GetUserIdAndSecContext(&saved_user, &saved_sec);
    if (expression)
    {
        SetUserIdAndSecContext(heap->rd_rel->relowner, saved_sec | SECURITY_RESTRICTED_OPERATION);
        nest_level = NewGUCNestLevel();
        estate = CreateExecutorState();
        estate->es_snapshot = GetActiveSnapshot();
        info = BuildIndexInfo(index);
    }

    for (sample_slot = 0; sample_slot < meta->n_slots; ++sample_slot)
    {
        bool dead_in_heap;

        CHECK_FOR_INTERRUPTS();
        if (recovery_vector(index, meta, blocks, sample_slot, fetch, slot,
                            heap_blocks, &older, info, estate, sample, &dead_in_heap) == CUVS_CAGRA_SLOT_LIVE)
            break;
    }
    if (sample_slot == meta->n_slots)
        goto done;

    for (uint64 start = from; start < meta->n_slots; start += batch_rows)
    {
        uint32 count = Min((uint64) batch_rows, meta->n_slots - start);
        int bytes = count * row_bytes;

        for (uint32 i = 0; i < count; ++i)
        {
            uint64 slot_number = start + i;
            float *out = batch + (Size) i * meta->dim;
            CuvsCagraSlotResult state;
            bool dead_in_heap;

            CHECK_FOR_INTERRUPTS();
            if (slot_number == sample_slot)
            {
                memcpy(out, sample, row_bytes);
                continue;
            }
            state = recovery_vector(index, meta, blocks, slot_number, fetch, slot,
                                    heap_blocks, &older, info, estate, out, &dead_in_heap);
            if (state == CUVS_CAGRA_SLOT_BAD)
                goto done;
            if (state == CUVS_CAGRA_SLOT_DEAD)
            {
                memcpy(out, sample, row_bytes);
                ++*placeholder_rows;
                if (!dead_in_heap)
                    ++*physical_dead_slots;
            }
        }
        if (FileWrite(file, (char *) batch, bytes, (off_t) (start - from) * row_bytes, 0) != bytes)
            goto done;
    }
    ok = true;

done:
    if (expression)
    {
        FreeExecutorState(estate);
        AtEOXact_GUC(false, nest_level);
        SetUserIdAndSecContext(saved_user, saved_sec);
    }
    table_index_fetch_end(fetch);
    ExecDropSingleTupleTableSlot(slot);
    pfree(batch);
    pfree(sample);
    return ok;
}

bool
cuvs_cagra_fold_supplement(Relation index, Relation heap, Snapshot snapshot,
                           const CuvsCagraMeta *meta, BlockNumber blocks,
                           uint64 from, uint64 upto, CuvsPageSupplement *supplement)
{
    return copy_supplement(index, heap, snapshot, meta, blocks, from, upto, supplement, true, false);
}
#endif

void
cuvs_cagra_free_supplement(CuvsPageSupplement *supplement)
{
    if (supplement->marks)
        pfree((void *) supplement->marks);
    if (supplement->vectors)
        pfree((void *) supplement->vectors);
    memset(supplement, 0, sizeof(*supplement));
}
#endif
