/*
 * cuvs_cover_admin.c - pg_cuvs_cover_invalidate_all(): take every guarantee mark off the
 * pg_cuvs_hnsw indexes of the current database before the library or the SQL goes back to a
 * version that does not know the cover descriptor.
 *
 * The downgrade script pg_cuvs--0.9.3--0.9.2.sql calls it first. It is fail-closed: it returns
 * only when no index is left that an older library could keep serving as guaranteed. Today the
 * CERTIFIED indexes are written LOST (reason downgrade) with graph_epoch + 1 under the heap
 * ShareUpdateExclusiveLock, so a recertification (which holds the same lock) cannot turn them back
 * into CERTIFIED between this function and the commit. An index this library may not write (a
 * newer format) aborts the ALTER EXTENSION with nothing changed.
 *
 * Every index also gets a relcache invalidation, so each backend rebuilds its index routine from
 * the handler symbol the catalog names after the script (the old one) instead of keeping the new
 * routine in memory.
 */
#include "postgres.h"
#include "fmgr.h"
#include "access/genam.h"
#include "access/generic_xlog.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "utils/syscache.h"
#include "catalog/namespace.h"
#include "storage/smgr.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "commands/defrem.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "utils/rel.h"
#include "utils/timestamp.h"
#include "utils/snapmgr.h"

#include "cuvs_cover_format.h"
#include "cuvs_cover_recert.h"
#include "cuvs_cover_scan.h"
#include "cuvs_cover_reach_pg.h"
#include "utils/acl.h"

PG_FUNCTION_INFO_V1(pg_cuvs_cover_invalidate_all);
PG_FUNCTION_INFO_V1(pg_cuvs_hnsw_cover_status);
PG_FUNCTION_INFO_V1(pg_cuvs_cover_recertify);

/* Copy block `blkno` of fork `fork` of `rel` into the 8192-byte `out`. Returns false if the block does not exist. */
static bool
copy_block_fork(Relation rel, ForkNumber fork, BlockNumber blkno, uint8_t *out)
{
	Buffer		buf;

	StaticAssertStmt(BLCKSZ == CUVS_COVER_PAGE_SIZE, "cover format assumes 8 kB pages");
	if (!smgrexists(RelationGetSmgr(rel), fork) || blkno >= smgrnblocks(RelationGetSmgr(rel), fork))
		return false;
	buf = ReadBufferExtended(rel, fork, blkno, RBM_NORMAL, NULL);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	memcpy(out, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	return true;
}

static bool
copy_block(Relation rel, BlockNumber blkno, uint8_t *out)
{
	return copy_block_fork(rel, MAIN_FORKNUM, blkno, out);
}

/* Classify the pages of one fork. */
static void
classify_fork(Relation rel, ForkNumber fork, uint8_t *meta, uint8_t *head, CuvsCoverResult *res)
{
	bool		have_head = copy_block_fork(rel, fork, 1, head);
	bool		have_meta = copy_block_fork(rel, fork, 0, meta);

	if (have_meta)
		cuvs_cover_classify(meta, have_head ? head : NULL, res);
	else
	{
		memset(res, 0, sizeof(*res));
		res->cls = CUVS_COVER_ERROR;
	}
}

/* Rewrite the descriptor in block 0 of fork `fork` of `index` as LOST with `reason`, graph_epoch + 1, under the
 * buffer lock. The main fork goes through Generic WAL (a delta; none for an UNLOGGED index). The init fork of an
 * UNLOGGED index is what a crash reset copies over the main fork, so it must say LOST too, and its page is logged
 * whole. The page is classified again under the lock: another backend may have changed it. */
static void
write_lost(Relation index, ForkNumber fork, uint8 reason)
{
	Buffer		buf = ReadBufferExtended(index, fork, 0, RBM_NORMAL, NULL);
	GenericXLogState *xlog = NULL;
	Page		page;
	Page		copy = NULL;
	CuvsCoverResult res;
	CuvsCoverDescriptor d;
	uint32_t	upper;

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	if (fork == MAIN_FORKNUM)
		page = GenericXLogRegisterBuffer((xlog = GenericXLogStart(index)), buf, 0);
	else
		page = copy = (Page) palloc(BLCKSZ), memcpy(copy, BufferGetPage(buf), BLCKSZ);
	cuvs_cover_classify((const uint8_t *) page, NULL, &res);
	if (res.cls != CUVS_COVER_CERTIFIED || !res.writable)
	{
		if (xlog != NULL)
			GenericXLogAbort(xlog);
		UnlockReleaseBuffer(buf);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot take the guarantee off index \"%s\"", RelationGetRelationName(index)),
				 errdetail("Its descriptor (%s fork) is not a CERTIFIED one this library may write; it was probably written by a newer pg_cuvs.",
						   fork == MAIN_FORKNUM ? "main" : "init"),
				 errhint("Take it off with the pg_cuvs version that wrote it, or DROP the index. REINDEX would build a new certified index.")));
	}
	upper = cuvs_cover_get16((const uint8_t *) page, CUVS_COVER_OFF_UPPER);
	memcpy(&d, (char *) page + upper, sizeof(d));
	d.state = CUVS_COVER_STATE_LOST;
	d.reason = reason;
	d.graph_epoch++;
	cuvs_cover_descriptor_seal(&d);
	memcpy((char *) page + upper, &d, sizeof(d));
	if (fork == MAIN_FORKNUM)
		GenericXLogFinish(xlog);
	else
	{
		START_CRIT_SECTION();
		memcpy(BufferGetPage(buf), copy, BLCKSZ);
		MarkBufferDirty(buf);
		log_newpage_buffer(buf, true);
		END_CRIT_SECTION();
		pfree(copy);
	}
	UnlockReleaseBuffer(buf);
}

Datum
pg_cuvs_cover_invalidate_all(PG_FUNCTION_ARGS)
{
	Oid			am_oid = get_am_oid("pg_cuvs_hnsw", true);
	Relation	pg_class_rel;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tup;
	List	   *index_oids = NIL;
	ListCell   *lc;
	int64		examined = 0;
	int64		n_legacy = 0, n_off = 0, n_lost = 0, n_untrusted = 0, n_error = 0, n_invalidated = 0;
	uint8_t	   *meta = palloc(CUVS_COVER_PAGE_SIZE);
	uint8_t	   *head = palloc(CUVS_COVER_PAGE_SIZE);

	if (!OidIsValid(am_oid))
		PG_RETURN_INT64(0);

	/* collect the index OIDs first; pg_class has no index on relam, so this is a sequential scan */
	pg_class_rel = table_open(RelationRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_class_relam, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(am_oid));
	scan = systable_beginscan(pg_class_rel, InvalidOid, false, NULL, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_class form = (Form_pg_class) GETSTRUCT(tup);

		if (form->relkind == RELKIND_INDEX)
			index_oids = lappend_oid(index_oids, form->oid);
	}
	systable_endscan(scan);
	table_close(pg_class_rel, AccessShareLock);

	foreach(lc, index_oids)
	{
		Oid			index_oid = lfirst_oid(lc);
		Oid			heap_oid = IndexGetRelation(index_oid, true);
		Relation	heap_rel;
		Relation	index_rel;
		CuvsCoverResult res;

		if (!OidIsValid(heap_oid))
			continue;			/* dropped while we were collecting */
		/* another session's temporary index cannot be opened; say which one instead of failing inside index_open */
		if (get_rel_persistence(index_oid) == RELPERSISTENCE_TEMP && isOtherTempNamespace(get_rel_namespace(index_oid)))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot examine index \"%s\": it belongs to a temporary table of another session",
							get_rel_name(index_oid)),
					 errhint("Retry when that session has ended.")));
		/* heap before index, and the heap lock a recertification also takes */
		heap_rel = table_open(heap_oid, ShareUpdateExclusiveLock);
		index_rel = index_open(index_oid, ShareUpdateExclusiveLock);
		classify_fork(index_rel, MAIN_FORKNUM, meta, head, &res);
		examined++;

		switch (res.cls)
		{
			case CUVS_COVER_LEGACY:
				n_legacy++;
				break;
			case CUVS_COVER_OFF:
				n_off++;		/* no guarantee was ever promised */
				break;
			case CUVS_COVER_LOST:
				n_lost++;		/* already answers from the heap; stays so until recertified */
				break;
			case CUVS_COVER_UNTRUSTED:
				n_untrusted++;	/* treated like LOST by every reader */
				break;
			case CUVS_COVER_ERROR:
				n_error++;		/* not a guarantee mark either; reported below */
				break;
			case CUVS_COVER_CERTIFIED:
				write_lost(index_rel, MAIN_FORKNUM, CUVS_COVER_REASON_DOWNGRADE);
				n_invalidated++;
				break;
		}

		/* An UNLOGGED index starts from its init fork after a crash: that copy must not bring a guarantee back. */
		if (index_rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED &&
			smgrexists(RelationGetSmgr(index_rel), INIT_FORKNUM))
		{
			CuvsCoverResult init_res;

			classify_fork(index_rel, INIT_FORKNUM, meta, head, &init_res);
			if (init_res.cls == CUVS_COVER_CERTIFIED)
			{
				write_lost(index_rel, INIT_FORKNUM, CUVS_COVER_REASON_DOWNGRADE);
				n_invalidated++;
			}
		}

		/* every backend rebuilds this index's routine from the catalog after this transaction */
		CacheInvalidateRelcacheByRelid(index_oid);
		index_close(index_rel, NoLock);
		table_close(heap_rel, NoLock);	/* locks are held to the end of the transaction */
	}

	if (n_error > 0)
		ereport(WARNING,
				(errmsg("%lld pg_cuvs_hnsw index(es) have an unusable metapage and were left as they are",
						(long long) n_error)));
	ereport(NOTICE,
			(errmsg("pg_cuvs: %lld pg_cuvs_hnsw index(es) examined: %lld legacy, %lld off, %lld lost, %lld untrusted, %lld unusable, %lld certified taken off; none is CERTIFIED now",
					(long long) examined, (long long) n_legacy, (long long) n_off, (long long) n_lost,
					(long long) n_untrusted, (long long) n_error, (long long) n_invalidated)));
	PG_RETURN_INT64(examined);
}

static const char *
class_name(CuvsCoverClass c)
{
	switch (c)
	{
		case CUVS_COVER_LEGACY: return "legacy";
		case CUVS_COVER_OFF: return "off";
		case CUVS_COVER_CERTIFIED: return "certified";
		case CUVS_COVER_LOST: return "lost";
		case CUVS_COVER_UNTRUSTED: return "untrusted";
		default: return "error";
	}
}

static const char *
untrusted_name(CuvsCoverUntrustedReason r)
{
	switch (r)
	{
		case CUVS_COVER_UNTRUSTED_LAYOUT: return "layout";
		case CUVS_COVER_UNTRUSTED_DESCRIPTOR_CORRUPT: return "descriptor_corrupt";
		case CUVS_COVER_UNTRUSTED_DESCRIPTOR_LOST: return "descriptor_lost";
		case CUVS_COVER_UNTRUSTED_FUTURE_FORMAT: return "future_format";
		case CUVS_COVER_UNTRUSTED_HEAD_UNREADABLE: return "head_unreadable";
		case CUVS_COVER_UNTRUSTED_DESCRIPTOR_INCONSISTENT: return "descriptor_inconsistent";
		default: return NULL;
	}
}

/* What a LOST index waits for, by the reason it stores. */
static const char *
pending_text(uint8 reason)
{
	switch (reason)
	{
		case CUVS_COVER_REASON_UNCERTIFIED:
			return "built without a certification: pg_cuvs_cover_recertify() or the next VACUUM certifies it; autovacuum vacuums the table by itself only after enough rows changed (autovacuum_vacuum_threshold and _scale_factor, or the insert variants), an unchanged table waits for a call; until then scans rank the heap";
		case CUVS_COVER_REASON_VACUUM:
			return "a VACUUM changed the graph: the cleanup of that VACUUM, the next VACUUM or pg_cuvs_cover_recertify() certifies it again";
		case CUVS_COVER_REASON_UNKEYED_WRITER:
			return "rows were inserted without ledger keys (an older library?): pg_cuvs_cover_recertify() or the next VACUUM certifies it again";
		case CUVS_COVER_REASON_RECERT_FAILED:
			return "the last recertification gave up: see last_recertification_failure, then run pg_cuvs_cover_recertify() with more memory";
		case CUVS_COVER_REASON_LEDGER_DAMAGED:
			return "a ledger page did not read as the descriptor says and the chain was dropped: pg_cuvs_cover_recertify() or the next VACUUM builds a new one";
		case CUVS_COVER_REASON_DOWNGRADE:
			return "taken off before an older library or SQL: the next VACUUM or pg_cuvs_cover_recertify() certifies it again";
		default:
			return "no recertification is pending for this reason: REINDEX builds a certified index";
	}
}

/*
 * pg_cuvs_hnsw_cover_status(index regclass) -> jsonb: how the cover descriptor of a pg_cuvs_hnsw index reads.
 * It reads the two pages and classifies them; it does not verify the ledger and promises nothing about the
 * graph. "legacy" means an index built without a descriptor: it never had a guarantee.
 */
Datum
pg_cuvs_hnsw_cover_status(PG_FUNCTION_ARGS)
{
	Oid			index_oid = PG_GETARG_OID(0);
	Relation	index_rel = index_open(index_oid, AccessShareLock);
	Oid			am_oid = get_am_oid("pg_cuvs_hnsw", true);
	uint8_t	   *meta = palloc(CUVS_COVER_PAGE_SIZE);
	uint8_t	   *head = palloc(CUVS_COVER_PAGE_SIZE);
	CuvsCoverResult res;
	StringInfoData out;
	const char *why;
	const char *fn_name;
	bool		have_head, have_meta;
	bool		have_growth = false, recommended = false, have_ledger = false, ledger_ok = false, marked = false;
	uint64		ledger_keys = 0;
	const char *ledger_why = "";
	uint64		delta_keys = 0;
	CuvsCoverDescriptor dsc;

	if (index_rel->rd_rel->relam != am_oid || !OidIsValid(am_oid))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
						errmsg("\"%s\" is not a pg_cuvs_hnsw index", RelationGetRelationName(index_rel))));
	{
		/* What an index says about its guarantee is information about the table: the privilege to read the table is required */
		Oid			heap_oid = IndexGetRelation(RelationGetRelid(index_rel), false);
		AclResult	aclresult = pg_class_aclcheck(heap_oid, GetUserId(), ACL_SELECT);

		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TABLE, get_rel_name(heap_oid));
	}
	if (RelationGetNumberOfBlocks(index_rel) == 0)
	{
		/* what a failed or cancelled CREATE INDEX CONCURRENTLY leaves behind: an invalid index with an empty file */
		StringInfoData e;

		initStringInfo(&e);
		appendStringInfo(&e, "{\"class\": \"not_built\", \"writable\": false, \"valid\": %s, \"pending\": \"the index has no pages: a CREATE INDEX CONCURRENTLY "
						 "that failed or was cancelled leaves such an index (invalid, not used by any query); DROP INDEX it, or REINDEX INDEX it\"}",
						 index_rel->rd_index->indisvalid ? "true" : "false");
		index_close(index_rel, AccessShareLock);
		PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(e.data)));
	}
	have_head = copy_block(index_rel, 1, head);
	have_meta = copy_block(index_rel, 0, meta);
	if (have_meta)
		cuvs_cover_classify(meta, have_head ? head : NULL, &res);
	else
	{
		memset(&res, 0, sizeof(res));
		res.cls = CUVS_COVER_ERROR;
	}
	have_growth = res.writable && (res.cls == CUVS_COVER_CERTIFIED || res.cls == CUVS_COVER_LOST) &&
		cuvs_cover_growth_status(index_rel, &dsc, &delta_keys, &recommended);
	if (res.writable && (res.cls == CUVS_COVER_CERTIFIED || res.cls == CUVS_COVER_LOST) && res.ledger_head != CUVS_COVER_INVALID_BLOCK)
	{
		have_ledger = true;
		ledger_ok = cuvs_cover_ledger_check(index_rel, &res, &ledger_keys, &ledger_why);
	}
	marked = res.cls == CUVS_COVER_CERTIFIED && cuvs_cover_breakglass_marked();
	index_close(index_rel, AccessShareLock);

	/* Which C symbol the access method handler names right now. Only the new symbol makes the library honour the
	 * descriptor; while the SQL names the old one (an unplanned rollback fixed by hand) the descriptor may be stale. */
	{
		HeapTuple	amtup = SearchSysCache1(AMOID, ObjectIdGetDatum(am_oid));

		fn_name = "";
		if (HeapTupleIsValid(amtup))
		{
			Oid			handler = ((Form_pg_am) GETSTRUCT(amtup))->amhandler;
			char	   *mod = NULL, *fn = NULL;

			ReleaseSysCache(amtup);
			if (OidIsValid(handler))
			{
				fmgr_symbol(handler, &mod, &fn);
				fn_name = fn != NULL ? fn : "";
			}
		}
	}

	initStringInfo(&out);
	appendStringInfo(&out, "{\"class\": \"%s\", \"writable\": %s", class_name(res.cls), res.writable ? "true" : "false");
	/* The descriptor says CERTIFIED, this library may read it, the handler names the symbol that maintains the ledger, the break-glass
	 * marker is absent AND the whole ledger chain reads: only then is the promise of decision A in force. The promise: every row the
	 * graph could not reach when the index was last certified (the start of that recertification: the walk is not a snapshot), and every row
	 * inserted after that, is a candidate of every ordered scan. Rows an insert after the certification cut off from the graph, the inserts
	 * made during the walk included, are outside it until the next one. */
	appendStringInfo(&out, ", \"handler_symbol\": \"%s\", \"guarantee_in_force\": %s", fn_name,
					 (res.cls == CUVS_COVER_CERTIFIED && res.writable && strcmp(fn_name, "pg_cuvs_hnsw_handler_c1p") == 0 &&
					  !marked && have_ledger && ledger_ok) ? "true" : "false");
	if (res.cls == CUVS_COVER_CERTIFIED && res.writable && strcmp(fn_name, "pg_cuvs_hnsw_handler_c1p") == 0 && !marked && have_ledger && ledger_ok)
		appendStringInfoString(&out, ", \"guarantee\": \"rows the graph could not reach when the last certification began (certified_at: the start of the recertification, which is not a snapshot) and every row inserted since are always candidates; "
							   "a row that an insert since then cut off from the graph, the inserts made while that recertification ran included, is outside this until the next recertification "
							   "(the cleanup step of a VACUUM (once the keys since the certification pass 5 percent of the rows it covered, or after a VACUUM took the index off), pg_cuvs_cover_recertify(), or REINDEX); how many rows that is depends on the graph and the data (measured ranges are in docs/reference.md)\"");
	if (have_ledger)
	{
		appendStringInfo(&out, ", \"ledger_ok\": %s", ledger_ok ? "true" : "false");
		if (ledger_ok)
			appendStringInfo(&out, ", \"ledger_keys\": %llu", (unsigned long long) ledger_keys);
		else
			appendStringInfo(&out, ", \"ledger_problem\": \"%s\"", ledger_why);
	}
	if (marked)
		appendStringInfoString(&out, ", \"breakglass_marker\": true, \"pending\": \"an emergency rollback (pg_cuvs_cover_breakglass()) was made: "
							   "this index is read as lost until ALTER EXTENSION pg_cuvs UPDATE TO '0.9.2' and then TO '0.9.3' have run with the new library installed\"");
	why = untrusted_name(res.reason);
	if (res.cls == CUVS_COVER_UNTRUSTED && why != NULL)
		appendStringInfo(&out, ", \"untrusted_reason\": \"%s\"", why);
	if (res.cls == CUVS_COVER_OFF || res.cls == CUVS_COVER_LOST)
		appendStringInfo(&out, ", \"stored_reason\": %d", (int) res.stored_reason);
	if (res.cls == CUVS_COVER_CERTIFIED)
		appendStringInfo(&out, ", \"ledger_head\": %u, \"generation\": %llu", (unsigned) res.ledger_head,
						 (unsigned long long) res.generation);
	if (have_growth)
	{
		appendStringInfo(&out, ", \"base_rows\": %llu, \"base_unreached\": %llu, \"keys_since_certification\": %llu",
						 (unsigned long long) dsc.base_rows, (unsigned long long) dsc.base_missing, (unsigned long long) delta_keys);
		/* certified_at: the time the guarantee refers to (the recertification began: no insert was in flight then) */
		if (dsc.cert_time != 0)
			appendStringInfo(&out, ", \"certified_at\": \"%s\"", timestamptz_to_str((TimestampTz) dsc.cert_time));
		if (res.cls == CUVS_COVER_CERTIFIED)
		{
			appendStringInfo(&out, ", \"recertify_recommended\": %s", (recommended || (have_ledger && !ledger_ok)) ? "true" : "false");
			/* the emergency-rollback text above is the one pending text then: a second "pending" key would replace it */
			if (marked)
				;
			else if (have_ledger && !ledger_ok)
				appendStringInfoString(&out, ", \"pending\": \"the ledger does not read (see ledger_problem): pg_cuvs_cover_recertify() or the next VACUUM builds a new one; "
									   "until then scans rank the heap\"");
			else if (recommended)
				appendStringInfo(&out, ", \"pending\": \"the keys written since the certification passed %d percent of the rows it covered: "
								 "the next VACUUM, pg_cuvs_cover_recertify() or REINDEX recertifies it; until then every one of those keys is ranked exactly on each scan, so searches slow down as the keys grow\"",
								 CUVS_RECERT_GROWTH_PERCENT);
			else if (have_growth && dsc.last_fail_nblocks != 0)
				appendStringInfoString(&out, ", \"pending\": \"the last recertification of this index failed (see last_recertification_failure) and the guarantee in force is the one of the last certification; "
									   "a VACUUM tries again once the table or the memory limit has changed, and pg_cuvs_cover_recertify() tries at once\"");
		}
		if (res.cls == CUVS_COVER_LOST)
			appendStringInfo(&out, ", \"pending\": \"%s\"", pending_text(dsc.reason));
		if (dsc.last_fail_nblocks != 0)
			appendStringInfo(&out, ", \"last_recertification_failure\": {\"relation_blocks\": %u, \"cause\": \"%s\", \"limit_bytes\": %llu, \"needed_bytes\": %llu}",
							 (unsigned) dsc.last_fail_nblocks, dsc.last_fail_status == 2 ? "over_memory_limit" : dsc.last_fail_status == 4 ? "page_chain_damaged" : "no_result",
							 (unsigned long long) dsc.last_fail_limit, (unsigned long long) dsc.last_fail_needed);
	}
	appendStringInfoChar(&out, '}');
	PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(out.data)));
}

/*
 * pg_cuvs_cover_recertify(index regclass) -> jsonb: walk the graph, write what it cannot reach (and every key written since)
 * as a new ledger and certify the index. For the owner of the table. Takes ShareUpdateExclusiveLock on the table until the end of
 * the transaction, so it does not run together with VACUUM, another recertification or REINDEX CONCURRENTLY.
 */
/* Take a relation lock for the rest of the transaction, polling instead of queueing; false if it was not free in time. */
static bool
lock_polled(Oid relid, LOCKMODE mode)
{
	int			bound = LockTimeout > 0 ? LockTimeout : 2000;
	int			waited = 0;

	for (;;)
	{
		if (ConditionalLockRelationOid(relid, mode))
			return true;
		if (waited >= bound)
			return false;
		CHECK_FOR_INTERRUPTS();
		pg_usleep(1000);
		waited++;
	}
}

Datum
pg_cuvs_cover_recertify(PG_FUNCTION_ARGS)
{
	Oid			index_oid = PG_GETARG_OID(0);
	Oid			heap_oid = IndexGetRelation(index_oid, false);
	Oid			am_oid = get_am_oid("pg_cuvs_hnsw", true);
	Relation	heap, index;
	CuvsRecertOutcome out;
	StringInfoData js;
	uint64		ram, spill;
	static const char *const names[] = {"done", "not_applicable", "deferred", "failed", "skipped", "abandoned"};

	if (!object_ownercheck(RelationRelationId, heap_oid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, get_rel_name(heap_oid));
	/*
	 * The locks are asked for without queueing behind a statement that conflicts, and given up after lock_timeout (2 s if it is
	 * not set): a request that waits makes every later reader and writer of the table wait behind it (review af8d48e1 R6-9), and
	 * the walk that follows holds them for seconds. The caller is told, and may call again.
	 */
	if (!lock_polled(heap_oid, ShareUpdateExclusiveLock) || !lock_polled(index_oid, RowExclusiveLock))
	{
		StringInfoData d;

		initStringInfo(&d);
		appendStringInfoString(&d, "{\"result\": \"deferred\", \"detail\": \"the table or the index is locked by a statement that conflicts (VACUUM, DDL, another recertification); "
							   "nothing was done, call again\"}");
		PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(d.data)));
	}
	heap = table_open(heap_oid, NoLock);
	index = index_open(index_oid, NoLock);
	if (!OidIsValid(am_oid) || index->rd_rel->relam != am_oid)
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
						errmsg("\"%s\" is not a pg_cuvs_hnsw index", RelationGetRelationName(index))));
	if (RelationGetNumberOfBlocks(index) == 0)
	{
		StringInfoData d;

		index_close(index, NoLock);
		table_close(heap, NoLock);
		initStringInfo(&d);
		appendStringInfoString(&d, "{\"result\": \"not_applicable\", \"detail\": \"the index has no pages: a CREATE INDEX CONCURRENTLY that failed or was cancelled leaves such an index "
							   "(invalid, not used by any query); DROP INDEX it, or REINDEX INDEX it\"}");
		PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(d.data)));
	}
	cuvs_cover_reach_default_limits(&ram, &spill);
	cuvs_cover_recertify_yielding(heap, index, ram, spill, true, &out);
	index_close(index, NoLock);
	table_close(heap, NoLock);

	initStringInfo(&js);
	appendStringInfo(&js, "{\"result\": \"%s\", \"detail\": \"%s\", \"live\": %llu, \"unreached\": %llu, \"keys_carried_over\": %llu, \"keys_copied_locked\": %llu, \"reused_pages\": %llu, \"reclaimed_pages\": %llu, \"chain_replaced\": %s, "
					 "\"mem_bytes\": %llu, \"spilled_bytes\": %llu, \"phase1_lock_us\": %llu, \"phase3_lock_us\": %llu, \"walk_us\": %llu",
					 names[out.status], out.detail ? out.detail : "", (unsigned long long) out.live, (unsigned long long) out.unreached,
					 (unsigned long long) out.kept_keys, (unsigned long long) out.locked_keys, (unsigned long long) out.reused_pages, (unsigned long long) out.reclaimed_pages, out.chain_replaced ? "true" : "false", (unsigned long long) out.mem_bytes, (unsigned long long) out.spilled_bytes,
					 (unsigned long long) out.phase1_lock_us, (unsigned long long) out.phase3_lock_us, (unsigned long long) out.walk_us);
	if (out.status == CUVS_RECERT_FAILED || out.status == CUVS_RECERT_SKIPPED)
		appendStringInfo(&js, ", \"needed_bytes\": %llu", (unsigned long long) out.needed_bytes);
	appendStringInfoChar(&js, '}');
	PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(js.data)));
}
