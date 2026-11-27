/* cuvs_cover_verify.c - see cuvs_cover_verify.h. */
#include "postgres.h"
#include "access/amapi.h"
#include "access/genam.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "nodes/pg_list.h"
#include "storage/bufmgr.h"
#include "utils/array.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "cuvs_cover_format.h"
#include "cuvs_cover_reach.h"
#include "cuvs_cover_reach_pg.h"
#include "cuvs_cover_scan.h"
#include "cuvs_cover_verify.h"
#include "cuvs_hnsw_scan.h"

#define VERIFY_SEARCH_MAX_RESULTS 200	/* the rows a verification search reads at most (hnsw.ef_search is 40 unless a session raised it) */

/*
 * The search runs in a memory context of its own that is deleted at the end: the scan descriptor (pgvector's ambeginscan allocates it and
 * its order-by data), the detoasted copy of the query vector, and whatever the scan allocates are all freed, so that an INSERT ... SELECT
 * of millions of rows, which calls this once per row in one statement context, does not grow. Without an active snapshot nothing is
 * searched (pgvector's scan insists on an MVCC snapshot, and a caller that has none must not crash the backend): the answer is "not found" and
 * the caller writes the ledger key. An error raised by the scan is an error of the insert (it is not caught here).
 */
bool
cuvs_cover_insert_searchable(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid)
{
	const IndexAmRoutine *d = cuvs_hnsw_scan_delegate();
	MemoryContext cxt, old;
	IndexScanDesc scan;
	ScanKeyData order;
	bool		found = false;
	int			n = 0;

	if (isnull[0] || d->ambeginscan == NULL || !ActiveSnapshotSet())
		return false;
	cxt = AllocSetContextCreate(CurrentMemoryContext, "cover verify search", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	scan = d->ambeginscan(index, 0, 1);
	scan->xs_snapshot = GetActiveSnapshot();
	/* the value of an insert can still be a toasted datum of another table (INSERT ... SELECT); a scan key holds a plain one, as pgvector's insert does */
	ScanKeyEntryInitialize(&order, 0, 1, 1, InvalidOid, InvalidOid, InvalidOid, PointerGetDatum(PG_DETOAST_DATUM(values[0])));
	d->amrescan(scan, NULL, 0, &order, 1);
	while (n < VERIFY_SEARCH_MAX_RESULTS && d->amgettuple(scan, ForwardScanDirection))
	{
		n++;
		if (ItemPointerEquals(&scan->xs_heaptid, heap_tid))
		{
			found = true;
			break;
		}
	}
	d->amendscan(scan);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
	return found;
}

/*
 * Test function (a test creates the SQL wrapper itself): does the search that the verification runs for an inserted row find the row with this
 * heap TID when it is asked with `query`? pg_cuvs_cover_search_probe(index regclass, query vector, heap_tid tid) returns boolean.
 */
PG_FUNCTION_INFO_V1(pg_cuvs_cover_search_probe);

Datum
pg_cuvs_cover_search_probe(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	Datum		value = PG_GETARG_DATUM(1);
	bool		isnull = false;
	bool		found = cuvs_cover_insert_searchable(index, &value, &isnull, (ItemPointer) PG_GETARG_POINTER(2));

	index_close(index, AccessShareLock);
	PG_RETURN_BOOL(found);
}

/*
 * Test function (a test creates the SQL wrapper itself): the search without an active snapshot. Every active snapshot is taken off the stack
 * (and put back at the end), so that the search runs as a caller without one would; it must answer "not found" and not crash the backend.
 * pg_cuvs_cover_search_nosnapshot_probe(index regclass, query vector, heap_tid tid) returns boolean.
 */
PG_FUNCTION_INFO_V1(pg_cuvs_cover_search_nosnapshot_probe);

Datum
pg_cuvs_cover_search_nosnapshot_probe(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	Datum		value = PG_GETARG_DATUM(1);
	bool		isnull = false;
	List	   *saved = NIL;
	ListCell   *lc;
	bool		found;

	while (ActiveSnapshotSet())
	{
		saved = lcons(RegisterSnapshot(GetActiveSnapshot()), saved);	/* the list ends up bottom first */
		PopActiveSnapshot();
	}
	found = cuvs_cover_insert_searchable(index, &value, &isnull, (ItemPointer) PG_GETARG_POINTER(2));
	foreach(lc, saved)
	{
		Snapshot	snap = (Snapshot) lfirst(lc);

		PushActiveSnapshot(snap);
		UnregisterSnapshot(snap);
	}
	index_close(index, AccessShareLock);
	PG_RETURN_BOOL(found);
}

/*
 * Test function (a test creates the SQL wrapper itself): the memory the memory context of the caller holds after `n` searches in a row, minus
 * what it held before. The search runs in a context of its own that it deletes, so this stays near zero; without the deletion it grows with n.
 * pg_cuvs_cover_search_memory_probe(index regclass, query vector, heap_tid tid, n int) returns bigint.
 */
PG_FUNCTION_INFO_V1(pg_cuvs_cover_search_memory_probe);

Datum
pg_cuvs_cover_search_memory_probe(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	Datum		value = PG_GETARG_DATUM(1);
	bool		isnull = false;
	int			n = PG_GETARG_INT32(3), i;
	Size		before = MemoryContextMemAllocated(CurrentMemoryContext, true);

	for (i = 0; i < n; i++)
		(void) cuvs_cover_insert_searchable(index, &value, &isnull, (ItemPointer) PG_GETARG_POINTER(2));
	index_close(index, AccessShareLock);
	PG_RETURN_INT64((int64) MemoryContextMemAllocated(CurrentMemoryContext, true) - (int64) before);
}

/*
 * Test functions (a test creates the SQL wrappers itself).
 *   pg_cuvs_cover_probe_unreached(index regclass) returns tid[]: the heap TIDs of the live rows the graph does not reach from the entry point
 *   pg_cuvs_cover_probe_keys(index regclass) returns tid[]: the heap TIDs of the ledger keys
 */
PG_FUNCTION_INFO_V1(pg_cuvs_cover_probe_unreached);
PG_FUNCTION_INFO_V1(pg_cuvs_cover_probe_keys);

typedef struct ProbeTids
{
	ItemPointerData *tids;
	Size		n, cap;
} ProbeTids;

static int
probe_emit(void *ctx, uint32 heap_blk, uint16 heap_off)
{
	ProbeTids  *p = ctx;

	if (p->n == p->cap)
	{
		p->cap = p->cap ? p->cap * 2 : 256;
		p->tids = p->tids ? repalloc(p->tids, p->cap * sizeof(ItemPointerData)) : palloc(p->cap * sizeof(ItemPointerData));
	}
	ItemPointerSet(&p->tids[p->n++], heap_blk, heap_off);
	return 0;
}

static Datum
probe_array(ProbeTids *p)
{
	Datum	   *elems = palloc(sizeof(Datum) * (p->n ? p->n : 1));
	Size		i;

	for (i = 0; i < p->n; i++)
		elems[i] = ItemPointerGetDatum(&p->tids[i]);
	return PointerGetDatum(construct_array(elems, (int) p->n, TIDOID, sizeof(ItemPointerData), false, TYPALIGN_SHORT));
}

Datum
pg_cuvs_cover_probe_unreached(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	ProbeTids	p = {NULL, 0, 0};
	CuvsReachCounts counts;
	uint64		ram, spill;
	Datum		result;

	cuvs_cover_reach_default_limits(&ram, &spill);
	cuvs_cover_reach_index(index, ram, spill, probe_emit, &p, &counts);
	index_close(index, AccessShareLock);
	if (counts.status != 0)
		ereport(ERROR, (errmsg("the reachability walk did not complete (status %d)", counts.status)));
	result = probe_array(&p);
	PG_RETURN_DATUM(result);
}

Datum
pg_cuvs_cover_probe_keys(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	CuvsCoverResult res;
	ProbeTids	p = {NULL, 0, 0};
	Datum		result;

	cuvs_cover_scan_classify(index, &res);
	if (res.ledger_head != CUVS_COVER_INVALID_BLOCK &&
		!cuvs_cover_scan_ledger(index, res.ledger_head, res.ledger_tail, res.generation, CurrentMemoryContext, &p.tids, &p.n))
		ereport(ERROR, (errmsg("the ledger cannot be read")));
	result = probe_array(&p);
	index_close(index, AccessShareLock);
	PG_RETURN_DATUM(result);
}
