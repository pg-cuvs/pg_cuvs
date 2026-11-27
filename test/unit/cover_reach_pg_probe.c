/* Test module for test_cover_reach_pg.py: runs the product reachability walk (src/cuvs_cover_reach_pg.c) on a
 * pgvector hnsw index and returns the counts and the heap TIDs of the unreached live elements. */
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "access/genam.h"
#include "access/relation.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"
#include "cuvs_cover_reach_pg.h"

PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(cover_reach_probe);
PG_FUNCTION_INFO_V1(cover_reach_unreached);
PG_FUNCTION_INFO_V1(cover_reach_limits);

/* cover_reach_probe(index regclass, mem_limit bigint, spill_limit bigint) ->
 * (status, nblocks, elements, live, reached, unreached, emitted, mem_bytes, spilled_bytes, needed_bytes) */
Datum
cover_reach_probe(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	CuvsReachCounts c;
	TupleDesc	tupdesc;
	Datum		v[10];
	bool		n[10] = {false};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	cuvs_cover_reach_index(index, (uint64) PG_GETARG_INT64(1), (uint64) PG_GETARG_INT64(2), NULL, NULL, &c);
	index_close(index, AccessShareLock);
	v[0] = Int32GetDatum(c.status);
	v[1] = Int64GetDatum(c.nblocks);
	v[2] = Int64GetDatum((int64) c.elements);
	v[3] = Int64GetDatum((int64) c.live);
	v[4] = Int64GetDatum((int64) c.reached);
	v[5] = Int64GetDatum((int64) c.unreached);
	v[6] = Int64GetDatum((int64) c.emitted);
	v[7] = Int64GetDatum((int64) c.mem_bytes);
	v[8] = Int64GetDatum((int64) c.spilled_bytes);
	v[9] = Int64GetDatum((int64) c.needed_bytes);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, v, n)));
}

typedef struct Collect { Tuplestorestate *ts; TupleDesc desc; } Collect;

static int
collect_tid(void *ctx, uint32 blk, uint16 off)
{
	Collect    *c = ctx;
	Datum		v[1] = {Int64GetDatum(((int64) blk << 16) | off)};
	bool		n[1] = {false};

	tuplestore_putvalues(c->ts, c->desc, v, n);
	return 0;
}

/* cover_reach_unreached(index regclass, mem_limit bigint, spill_limit bigint) -> setof bigint (heap block << 16 | offset) */
Datum
cover_reach_unreached(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index;
	CuvsReachCounts c;
	Collect		col;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	col.ts = rsinfo->setResult;
	col.desc = rsinfo->setDesc;
	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	cuvs_cover_reach_index(index, (uint64) PG_GETARG_INT64(1), (uint64) PG_GETARG_INT64(2), collect_tid, &col, &c);
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* cover_reach_limits() -> (ram bigint, spill bigint): the limits cuvs_cover_reach_default_limits gives this session */
Datum
cover_reach_limits(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		v[2];
	bool		n[2] = {false, false};
	uint64		ram, spill;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	cuvs_cover_reach_default_limits(&ram, &spill);
	v[0] = Int64GetDatum((int64) ram);
	v[1] = Int64GetDatum((int64) spill);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, v, n)));
}
