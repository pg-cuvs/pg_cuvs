/* cuvs_cover_scan.c - what a scan of a pg_cuvs_hnsw index reads from the cover descriptor and the ledger. */
#include "postgres.h"
#include "access/htup_details.h"
#include "access/itup.h"
#include "catalog/pg_am.h"
#include "catalog/pg_proc.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "cuvs_cover_format.h"
#include "cuvs_cover_ledger.h"
#include "cuvs_cover_scan.h"

static bool
scan_copy(Relation rel, BlockNumber blk, uint8_t *out)
{
	Buffer		buf;

	if (blk >= RelationGetNumberOfBlocks(rel))
		return false;
	buf = ReadBuffer(rel, blk);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	memcpy(out, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	return true;
}

#define CUVS_BREAKGLASS_MARKER "pg_cuvs_cover_breakglass_marker"

bool
cuvs_cover_breakglass_marked(void)
{
	Oid			am = get_am_oid("pg_cuvs_hnsw", true);
	HeapTuple	tup;
	Oid			handler, nsp;
	oidvector  *noargs;
	bool		marked;

	if (!OidIsValid(am))
		return false;
	tup = SearchSysCache1(AMOID, ObjectIdGetDatum(am));
	if (!HeapTupleIsValid(tup))
		return false;
	handler = ((Form_pg_am) GETSTRUCT(tup))->amhandler;
	ReleaseSysCache(tup);
	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(handler));
	if (!HeapTupleIsValid(tup))
		return false;
	nsp = ((Form_pg_proc) GETSTRUCT(tup))->pronamespace;
	ReleaseSysCache(tup);
	noargs = buildoidvector(NULL, 0);
	marked = SearchSysCacheExists3(PROCNAMEARGSNSP, CStringGetDatum(CUVS_BREAKGLASS_MARKER), PointerGetDatum(noargs),
								   ObjectIdGetDatum(nsp));
	pfree(noargs);
	return marked;
}

void
cuvs_cover_scan_classify(Relation index, CuvsCoverResult *res)
{
	uint8_t		meta[CUVS_COVER_PAGE_SIZE], head[CUVS_COVER_PAGE_SIZE];
	bool		have_head = scan_copy(index, 1, head);

	if (!scan_copy(index, 0, meta))
	{
		memset(res, 0, sizeof(*res));
		res->cls = CUVS_COVER_ERROR;
		return;
	}
	cuvs_cover_classify(meta, have_head ? head : NULL, res);
	if (res->cls == CUVS_COVER_CERTIFIED && cuvs_cover_breakglass_marked())
	{
		res->cls = CUVS_COVER_LOST;
		res->stored_reason = CUVS_COVER_REASON_BREAKGLASS;
	}
}

typedef struct LedgerRead
{
	Relation	rel;
	MemoryContext cxt;
	ItemPointerData *tids;
	Size		used, cap;
} LedgerRead;

static int
ledger_read_block(void *ctx, uint32_t blk, uint8_t *dst)
{
	LedgerRead *r = ctx;

	return scan_copy(r->rel, blk, dst) ? 0 : 1;
}

static int
ledger_key(void *ctx, const CuvsLedgerKey *k)
{
	LedgerRead *r = ctx;

	if (r->used == r->cap)
	{
		Size		ncap = r->cap ? r->cap * 2 : 1024;

		if (r->tids)
			r->tids = repalloc_huge(r->tids, ncap * sizeof(ItemPointerData));
		else
			r->tids = MemoryContextAllocHuge(r->cxt, ncap * sizeof(ItemPointerData));
		r->cap = ncap;
	}
	ItemPointerSet(&r->tids[r->used++], k->heap_block, k->heap_offset);
	return 0;
}

bool
cuvs_cover_ledger_is_empty(Relation index, const CuvsCoverResult *res)
{
	uint8_t		page[CUVS_COVER_PAGE_SIZE];
	CuvsLedgerHeader h;

	if (res->ledger_head == CUVS_COVER_INVALID_BLOCK || res->ledger_head != res->ledger_tail)
		return false;
	if (!scan_copy(index, res->ledger_tail, page) || cuvs_ledger_validate(page, &h) != CUVS_LEDGER_OK || h.generation != res->generation)
		return false;
	return h.first_ordinal == 0 && h.count == 0 && cuvs_ledger_next(page) == CUVS_LEDGER_NO_BLOCK;
}

static const char cover_ledger_stale_why[] = "a ledger page belongs to another generation";

/* Whether cuvs_cover_ledger_check failed because a page of the chain belongs to another generation: a reader without the cover lock
 * (a scan, the status function) sees that when a recertification published a new chain and a later one reused the old chain's pages
 * while it read; reading the descriptor again gives the chain in force. */
bool
cuvs_cover_ledger_stale(const char *why)
{
	return why == cover_ledger_stale_why;
}

bool
cuvs_cover_ledger_check(Relation index, const CuvsCoverResult *res, uint64 *nkeys, const char **why)
{
	LedgerRead	r = {index, CurrentMemoryContext, NULL, 0, 0};
	uint64_t	total = 0;
	CuvsLedgerWalkStatus st;

	*nkeys = 0;
	*why = "";
	if (res->ledger_head == CUVS_COVER_INVALID_BLOCK)
	{
		*why = "the index has no ledger chain";
		return false;
	}
	st = cuvs_ledger_walk(ledger_read_block, &r, res->ledger_head, res->generation, RelationGetNumberOfBlocks(index), 0,
						  res->ledger_tail, NULL, NULL, &total);
	if (st == CUVS_LEDGER_WALK_OK)
	{
		*nkeys = total;
		return true;
	}
	*why = st == CUVS_LEDGER_WALK_UNREADABLE ? "a ledger page cannot be read" :
		st == CUVS_LEDGER_WALK_INVALID ? "a ledger page is not a valid ledger page" :
		st == CUVS_LEDGER_WALK_STALE ? cover_ledger_stale_why :
		st == CUVS_LEDGER_WALK_GAP ? "the keys of two ledger pages do not follow each other" :
		st == CUVS_LEDGER_WALK_TOO_LONG ? "the ledger chain is longer than the relation (a loop)" :
		st == CUVS_LEDGER_WALK_BAD_KEY ? "a ledger key does not match its crc" :
		st == CUVS_LEDGER_WALK_TAIL_MISSING ? "the ledger chain ends before the tail the descriptor names" : "the ledger chain does not read";
	return false;
}

bool
cuvs_cover_scan_ledger(Relation index, uint32 head, uint32 tail, uint64 generation, MemoryContext cxt, ItemPointerData **tids, Size *ntids)
{
	int			attempt;

	for (attempt = 0; attempt < 3; attempt++)
	{
		LedgerRead	r = {index, cxt, NULL, 0, 0};
		uint64_t	total;
		CuvsLedgerWalkStatus st;

		CHECK_FOR_INTERRUPTS();
		st = cuvs_ledger_walk(ledger_read_block, &r, head, generation, RelationGetNumberOfBlocks(index), 0, tail, ledger_key, &r, &total);
		if (st == CUVS_LEDGER_WALK_OK)
		{
			*tids = r.tids;
			*ntids = r.used;
			return true;
		}
		if (r.tids)
			pfree(r.tids);
		if (st != CUVS_LEDGER_WALK_STALE)
			return false;
		/* a recertification replaced the chain while it was read: read the descriptor again */
		{
			CuvsCoverResult res;

			cuvs_cover_scan_classify(index, &res);
			if (res.cls != CUVS_COVER_CERTIFIED)
				return false;
			head = res.ledger_head;
			tail = res.ledger_tail;
			generation = res.generation;
		}
	}
	return false;
}
