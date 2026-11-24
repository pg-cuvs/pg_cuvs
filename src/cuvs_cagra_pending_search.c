#include "postgres.h"

#include "cuvs_cagra_pending_search.h"

#ifdef CUVS_PAGE_FORMAT
#include "access/tableam.h"
#include "catalog/pg_am_d.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/snapmgr.h"

static bool
copy_visible_vector(TupleTableSlot *slot, AttrNumber attribute, uint32 dim, float *out)
{
    bool isnull;
    Datum datum = slot_getattr(slot, attribute, &isnull);
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

bool
cuvs_cagra_local_supplement(Relation index, Relation heap, Snapshot snapshot,
                            const CuvsCagraMeta *meta, BlockNumber blocks,
                            uint64 from, uint64 upto, CuvsPageSupplement *supplement)
{
    bool expression = (meta->flags & CUVS_CAGRA_PENDING_VECTOR) != 0;
    AttrNumber attribute = index->rd_index->indkey.values[0];
    CuvsCagraPendingLocation *items;
    CuvsPagePosition *marks;
    float *vectors;
    uint32 packed = 0;
    uint32 count;

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

    if (ok)
    {
        IndexFetchTableData *fetch = table_index_fetch_begin(heap);
        TupleTableSlot *slot = table_slot_create(heap, NULL);

        for (uint32 i = 0; i < count; ++i)
        {
            bool again = false, dead = false;
            float *destination = vectors + (Size) packed * meta->dim;

            CHECK_FOR_INTERRUPTS();
            if (!ItemPointerIsValid(&items[i].tid))
                continue;
            table_index_fetch_reset(fetch);
            if (!table_index_fetch_tuple(fetch, &items[i].tid, snapshot, slot, &again, &dead))
            {
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
