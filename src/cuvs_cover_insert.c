/*
 * cuvs_cover_insert.c - the insert side of the cover ledger.
 *
 * For an index whose descriptor is CERTIFIED (or LOST with a chain still in place) every insert first appends the
 * heap TID to the ledger and only then lets pgvector insert it into the graph. A row that is in the graph is
 * therefore always in the ledger or reachable: the guarantee of the narrowed promise rests on this
 * order. A ledger key whose insert later rolls back stays (the safe direction, a recertification drops it).
 *
 * With cuvs.cover_verify_inserts (on by default) the order is the other way for a row that a search finds: pgvector inserts
 * first, the search runs, and a key is written afterwards only for a row the search does not find. The two records belong to one transaction,
 * so a crash between them leaves a row that never commits.
 *
 * Growth monitor: every key records the relation size at that moment in its ledger page. If the size
 * has grown by more than CUVS_COVER_GROWTH_PAGES since the previous event (a key, or a verified insert) while the index is CERTIFIED, somebody added
 * rows to the graph without writing keys (an older library, or the unsupported manual switch of the handler back to
 * the new symbol). The descriptor then moves to LOST (reason unkeyed_writer) with the persistent SUSPECT flag and
 * graph_epoch + 1, in the same WAL record as the key. Small unkeyed inserts are not caught; that limit is part of the
 * guarantee's statement.
 *
 * Locks: the cover lock is taken shared around key write and delegate insert, so inserts do not block each other
 * and a recertification (exclusive) cannot swap the ledger chain between the two. pgvector's own insert lock is NOT
 * taken here. Buffer order when a ledger page is added: tail page, new page, metapage.
 */
#include "postgres.h"
#include "access/amapi.h"
#include "access/generic_xlog.h"
#include "access/xact.h"
#include "catalog/pg_class.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "cuvs_cover_format.h"
#include "cuvs_cover_insert.h"
#include "cuvs_cover_ledger.h"
#include "cuvs_cover_testgate.h"
#include "cuvs_cover_verify.h"
#include "cuvs_cover_scan.h"
#include "cuvs_cover_worker.h"

#ifdef CUVS_COVER_RECERT_TEST_GATE
#define cover_growth_pages() cuvs_cover_test_growth_pages(CUVS_COVER_GROWTH_PAGES)
#else
#define cover_growth_pages() CUVS_COVER_GROWTH_PAGES
#endif

static bool (*pgv_aminsert) (Relation, Datum *, bool *, ItemPointer, Relation, IndexUniqueCheck, bool, struct IndexInfo *) = NULL;

/* Read the descriptor of `index` from a share-locked copy of block 0. Returns false if the index carries no
 * descriptor, or one that must not get keys (not CERTIFIED / LOST-with-chain, or a format this library may not write). */
static bool
cover_chain(Relation index, uint32 *tail, uint64 *generation, bool *certified, uint16 *flags)
{
	Buffer		buf = ReadBuffer(index, 0);
	uint8_t		meta[CUVS_COVER_PAGE_SIZE];
	CuvsCoverResult res;
	uint32_t	upper;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	memcpy(meta, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	upper = cuvs_cover_get16(meta, CUVS_COVER_OFF_UPPER);
	/* the two shapes of an index built without a descriptor: nothing to do, and no need to read block 1 */
	if (upper == CUVS_COVER_PAGE_SIZE - CUVS_COVER_SPECIAL_SIZE ||
		upper == CUVS_COVER_PAGE_SIZE - CUVS_COVER_SPECIAL_SIZE - CUVS_COVER_CONN_SIZE)
		return false;
	cuvs_cover_classify(meta, NULL, &res);
	if (!res.writable)
	{
		/* A descriptor this library may read but not write, or not even read (a newer format): the keys of this insert cannot be
		 * recorded, and a reader of that format would trust a ledger that lacks them. Refuse the insert rather than skip the key. */
		if (res.cls == CUVS_COVER_CERTIFIED || res.cls == CUVS_COVER_LOST ||
			(res.cls == CUVS_COVER_UNTRUSTED && res.reason == CUVS_COVER_UNTRUSTED_FUTURE_FORMAT))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("index \"%s\" carries a cover descriptor written by a newer pg_cuvs; this library cannot add keys to it",
							RelationGetRelationName(index)),
					 errhint("Install the pg_cuvs version that wrote it, or REINDEX the index with this one.")));
		return false;
	}
	if (res.cls == CUVS_COVER_CERTIFIED ||
		(res.cls == CUVS_COVER_LOST && res.ledger_tail != CUVS_COVER_INVALID_BLOCK && res.generation != 0))
	{
		*tail = res.ledger_tail;
		*generation = res.generation;
		*certified = (res.cls == CUVS_COVER_CERTIFIED);
		*flags = res.flags;
		return true;
	}
	return false;
}

/*
 * The ledger page the descriptor names does not read as a ledger page of its generation. An insert is not refused for that (one
 * damaged page would stop every write to the table until a rebuild): the chain is dropped from the descriptor in one WAL record
 * (LOST, reason ledger_damaged, graph_epoch + 1, no head or tail), and the insert goes on without a key. From then on nothing
 * trusts the ledger, scans rank the heap, and a recertification builds a new chain (it treats an index without a chain like a
 * new one). The check on the generation makes this idempotent when several sessions find the damage together.
 */
static void
cover_drop_chain(Relation index, uint64 generation, BlockNumber blk)
{
	Buffer		metabuf = ReadBuffer(index, 0);
	GenericXLogState *xlog;
	Page		metapage;
	CuvsCoverDescriptor d;
	uint32_t	upper;
	bool		dropped = false;

	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	xlog = GenericXLogStart(index);
	metapage = GenericXLogRegisterBuffer(xlog, metabuf, 0);
	upper = cuvs_cover_get16((const uint8_t *) metapage, CUVS_COVER_OFF_UPPER);
	memcpy(&d, (char *) metapage + upper, sizeof(d));
	if (d.magic == CUVS_COVER_D_MAGIC && d.generation == generation && d.ledger_head != CUVS_COVER_INVALID_BLOCK)
	{
		d.state = CUVS_COVER_STATE_LOST;
		d.reason = CUVS_COVER_REASON_LEDGER_DAMAGED;
		d.ledger_head = d.ledger_tail = CUVS_COVER_INVALID_BLOCK;
		d.graph_epoch++;
		cuvs_cover_descriptor_seal(&d);
		memcpy((char *) metapage + upper, &d, sizeof(d));
		GenericXLogFinish(xlog);
		dropped = true;
	}
	else
		GenericXLogAbort(xlog);
	UnlockReleaseBuffer(metabuf);
	if (dropped)
		ereport(WARNING,
				(errmsg("ledger page %u of index \"%s\" is not what its descriptor says; the guarantee is taken off", blk,
						RelationGetRelationName(index)),
				 errdetail("The insert goes on without a ledger key and scans answer from the heap until the index is recertified."),
				 errhint("Run pg_cuvs_cover_recertify() on the index; REINDEX also builds a new ledger.")));
}

static void
cover_damaged(Relation index, BlockNumber blk)
{
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("ledger page %u of index \"%s\" is not what its descriptor says", blk, RelationGetRelationName(index)),
			 errhint("REINDEX the index.")));
}

/*
 * Put the descriptor into LOST (reason unkeyed_writer) with the SUSPECT flag and graph_epoch + 1, in the metapage copy
 * `metapage` that is part of the caller's WAL record. Does nothing if another backend already left CERTIFIED.
 */
static void
cover_mark_suspect(Page metapage, uint64 generation)
{
	CuvsCoverDescriptor d;
	uint32_t	upper = cuvs_cover_get16((const uint8_t *) metapage, CUVS_COVER_OFF_UPPER);

	memcpy(&d, (char *) metapage + upper, sizeof(d));
	if (d.magic != CUVS_COVER_D_MAGIC || d.generation != generation || d.state != CUVS_COVER_STATE_CERTIFIED)
		return;
	d.state = CUVS_COVER_STATE_LOST;
	d.reason = CUVS_COVER_REASON_UNKEYED_WRITER;
	d.flags |= CUVS_COVER_FLAG_SUSPECT_UNKEYED_WRITER;
	d.graph_epoch++;
	cuvs_cover_descriptor_seal(&d);
	memcpy((char *) metapage + upper, &d, sizeof(d));
	ereport(LOG,
			(errmsg("pg_cuvs: the index grew by more than %d pages between two ledger keys; rows were inserted without keys, "
					"the guarantee is taken off until it is recertified", cover_growth_pages())));
}

/*
 * Append `proto` (its ordinal is assigned here) to the chain that ends at `tail`, following next pointers if another
 * inserter added pages. `certified`: the index is CERTIFIED (not just LOST with a chain), so growth since the last key
 * is checked. `update_meta`: a new page is also recorded as ledger_tail in the descriptor (an insert); a recertification
 * building the next chain before it is published passes false and tracks the tail itself. *tail_out receives the block
 * the key ended up in.
 */
void
cuvs_cover_append_key(Relation index, uint32 tail, uint64 generation, bool certified, const CuvsLedgerKey *proto,
					  bool update_meta, uint32 *tail_out, uint64 *ordinal_out)
{
	CuvsLedgerKey key = *proto;
	uint32		hops;
	bool		took_off = false;	/* this call took the guarantee off (the growth monitor): a candidate of the sweeper */

	for (hops = 0; hops < 1000000; hops++)
	{
		Buffer		buf = ReadBuffer(index, tail);
		GenericXLogState *xlog;
		Page		page;
		CuvsLedgerHeader h;
		uint32		next;
		BlockNumber nblocks;
		bool		suspect;

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		xlog = GenericXLogStart(index);
		page = GenericXLogRegisterBuffer(xlog, buf, 0);
		if (cuvs_ledger_validate((const uint8_t *) page, &h) != CUVS_LEDGER_OK || h.generation != generation ||
			h.kind != CUVS_LEDGER_KIND_KEYS)
		{
			GenericXLogAbort(xlog);
			UnlockReleaseBuffer(buf);
			if (!update_meta)
				cover_damaged(index, tail);	/* a recertification building its own chain: a bug, not damage */
			cover_drop_chain(index, generation, tail);
			return;
		}
		next = cuvs_ledger_next((const uint8_t *) page);
		if (next != CUVS_LEDGER_NO_BLOCK)
		{
			/* someone added a page after the tail this backend read: walk on */
			GenericXLogAbort(xlog);
			UnlockReleaseBuffer(buf);
			tail = next;
			continue;
		}
		key.ordinal = h.first_ordinal + h.count;
		if (ordinal_out)
			*ordinal_out = key.ordinal;
		nblocks = RelationGetNumberOfBlocks(index);
		/* seen == 0 means "unknown" (a page written before the monitor existed): record it, do not judge */
		suspect = certified && h.graph_nblocks_seen != 0 && nblocks > h.graph_nblocks_seen &&
			nblocks - h.graph_nblocks_seen > cover_growth_pages();
		/*
		 * The sweeper's candidate of a certified index is written by its first insert after the certification (cover_mark_inserted), and stays
		 * while anything was added since; a page added later in the same certification is not written again (the scan of the
		 * candidate table under its spinlock ran once per page). What still writes one here is the move to LOST by the growth monitor.
		 */
		if (suspect)
			took_off = true;
		if (h.count < CUVS_LEDGER_CAPACITY)
		{
			if (cuvs_ledger_append_seen((uint8_t *) page, &key, nblocks) != CUVS_LEDGER_OK)
			{
				GenericXLogAbort(xlog);
				UnlockReleaseBuffer(buf);
				if (!update_meta)
					cover_damaged(index, tail);
				cover_drop_chain(index, generation, tail);
				return;
			}
			if (suspect)
			{
				Buffer		metabuf = ReadBuffer(index, 0);

				LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
				cover_mark_suspect(GenericXLogRegisterBuffer(xlog, metabuf, 0), generation);
				GenericXLogFinish(xlog);
				UnlockReleaseBuffer(metabuf);
			}
			else
				GenericXLogFinish(xlog);
			UnlockReleaseBuffer(buf);
			if (tail_out)
				*tail_out = tail;
			if (took_off)
				cuvs_cover_note_candidate(RelationGetRelid(index));
			return;
		}
		/* the tail is full: add a page. The extension lock is held only while the block is allocated and locked. */
		{
			Buffer		newbuf, metabuf = InvalidBuffer;
			Page		newpage;
			uint32		newblk;

			LockRelationForExtension(index, ExclusiveLock);
			newbuf = ReadBuffer(index, P_NEW);
			LockBuffer(newbuf, BUFFER_LOCK_EXCLUSIVE);
			UnlockRelationForExtension(index, ExclusiveLock);
			newblk = BufferGetBlockNumber(newbuf);
			newpage = GenericXLogRegisterBuffer(xlog, newbuf, GENERIC_XLOG_FULL_IMAGE);
			/* the size now includes the page this backend just added */
			cuvs_ledger_page_init((uint8_t *) newpage, CUVS_LEDGER_KIND_KEYS, generation, key.ordinal, CUVS_LEDGER_NO_BLOCK,
								  RelationGetNumberOfBlocks(index));
			if (cuvs_ledger_append((uint8_t *) newpage, &key) != CUVS_LEDGER_OK)
				elog(ERROR, "pg_cuvs: cannot start a ledger page");
			cuvs_ledger_set_next((uint8_t *) page, newblk);

			if (update_meta || suspect)
			{
				Page		metapage;
				CuvsCoverDescriptor d;
				uint32_t	upper;

				metabuf = ReadBuffer(index, 0);
				LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
				metapage = GenericXLogRegisterBuffer(xlog, metabuf, 0);
				upper = cuvs_cover_get16((const uint8_t *) metapage, CUVS_COVER_OFF_UPPER);
				if (update_meta)
				{
					memcpy(&d, (char *) metapage + upper, sizeof(d));
					if (d.magic != CUVS_COVER_D_MAGIC || d.generation != generation)
						elog(ERROR, "pg_cuvs: the descriptor changed while a ledger page was added");
					d.ledger_tail = newblk;
					cuvs_cover_descriptor_seal(&d);
					memcpy((char *) metapage + upper, &d, sizeof(d));
				}
				if (suspect)
					cover_mark_suspect(metapage, generation);
			}
			GenericXLogFinish(xlog);
			if (metabuf != InvalidBuffer)
				UnlockReleaseBuffer(metabuf);
			UnlockReleaseBuffer(newbuf);
			UnlockReleaseBuffer(buf);
			if (tail_out)
				*tail_out = newblk;
			if (took_off)
				cuvs_cover_note_candidate(RelationGetRelid(index));
			return;
		}
	}
	elog(ERROR, "pg_cuvs: the ledger chain is longer than expected");
}

static uint64
cover_append(Relation index, uint32 tail, uint64 generation, bool certified, ItemPointer htid)
{
	CuvsLedgerKey key;
	uint64		ordinal = UINT64_MAX;

	memset(&key, 0, sizeof(key));
	key.creator_fullxid = U64FromFullTransactionId(GetTopFullTransactionId());
	key.heap_block = ItemPointerGetBlockNumber(htid);
	key.heap_offset = ItemPointerGetOffsetNumber(htid);
	cuvs_cover_append_key(index, tail, generation, certified, &key, true, NULL, &ordinal);
	return ordinal;
}

bool cuvs_cover_verify_inserts = true;

/*
 * cuvs.cover_verify_inserts (on by default): a row that pgvector has inserted and that a search for its own vector finds
 * (cuvs_cover_insert_searchable) is reachable from the entry point at that moment and gets no ledger key; a row the search does not find gets
 * its key after the insert. Only a CERTIFIED index is verified. The search reads no ledger, so a long ledger does not stop it.
 */
static bool
cover_try_verified(Relation index, uint32 tail, uint64 generation, Datum *values, bool *isnull, ItemPointer htid)
{
	CuvsCoverResult res;

	cuvs_cover_scan_classify(index, &res);
	if (res.cls != CUVS_COVER_CERTIFIED || res.ledger_tail != tail || res.generation != generation)
		return false;
	return cuvs_cover_insert_searchable(index, values, isnull, htid);
}

/*
 * A verified insert is an event of the ledger the way a key is, for the growth monitor (which finds rows inserted by a writer that does not
 * know the ledger): the monitor compares the relation size with the size recorded at the last event, and takes growth of more than
 * cover_growth_pages() between two events for rows inserted without keys. An insert that writes a key records the size and judges the growth
 * in cuvs_cover_append_key; a verified insert does the same here, with the same threshold. The tail page is read under a share lock first,
 * and locked exclusively (with one WAL record) only when the relation has grown since the recorded size, which is once in the few inserts
 * that fill a page, so the verified inserts do not queue on the tail page the way key writers do.
 */
static void
cover_note_verified(Relation index, uint32 tail, uint64 generation)
{
	uint32		hops;
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);
	bool		took_off = false;

	for (hops = 0; hops < 1000000; hops++)
	{
		Buffer		buf = ReadBuffer(index, tail);
		uint8_t		img[CUVS_COVER_PAGE_SIZE];
		CuvsLedgerHeader h;
		uint32		next;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		memcpy(img, BufferGetPage(buf), BLCKSZ);
		UnlockReleaseBuffer(buf);
		if (cuvs_ledger_validate(img, &h) != CUVS_LEDGER_OK || h.generation != generation || h.kind != CUVS_LEDGER_KIND_KEYS)
			return;				/* damage is found and handled by the next key */
		next = cuvs_ledger_next(img);
		if (next != CUVS_LEDGER_NO_BLOCK)
		{
			tail = next;
			continue;
		}
		/* seen == 0 means "unknown": nothing to judge or record; no growth since the recorded size: nothing to do */
		if (h.graph_nblocks_seen == 0 || nblocks <= h.graph_nblocks_seen)
			return;
		{
			GenericXLogState *xlog;
			Page		page;
			CuvsLedgerHeader h2;
			bool		suspect;

			buf = ReadBuffer(index, tail);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			xlog = GenericXLogStart(index);
			page = GenericXLogRegisterBuffer(xlog, buf, 0);
			if (cuvs_ledger_validate((const uint8_t *) page, &h2) != CUVS_LEDGER_OK || h2.generation != generation ||
				cuvs_ledger_next((const uint8_t *) page) != CUVS_LEDGER_NO_BLOCK || nblocks <= h2.graph_nblocks_seen)
			{
				GenericXLogAbort(xlog);		/* another backend moved on: it recorded the size, or added a page */
				UnlockReleaseBuffer(buf);
				return;
			}
			suspect = h2.graph_nblocks_seen != 0 && nblocks - h2.graph_nblocks_seen > cover_growth_pages();
			took_off = suspect;	/* the candidate of a growth is the first insert's (cover_mark_inserted); a move to LOST writes one here */
			h2.graph_nblocks_seen = nblocks;
			cuvs_ledger_write_header((uint8_t *) page, &h2);
			if (suspect)
			{
				Buffer		metabuf = ReadBuffer(index, 0);

				LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
				cover_mark_suspect(GenericXLogRegisterBuffer(xlog, metabuf, 0), generation);
				GenericXLogFinish(xlog);
				UnlockReleaseBuffer(metabuf);
			}
			else
				GenericXLogFinish(xlog);
			UnlockReleaseBuffer(buf);
		}
		if (took_off)
			cuvs_cover_note_candidate(RelationGetRelid(index));
		return;
	}
}

/*
 * say in the descriptor that a row was inserted since the certification (CUVS_COVER_FLAG_INSERTED) and since phase 1 of a
 * recertification that may run now (INSERTED_SINCE_START), so that the time rule takes the index although the insert writes no key and adds no
 * page, and a sweep after a restart knows it too. Only an insert that finds one of the two missing in its share-locked copy of block 0 comes here:
 * the bits are looked at again under the exclusive lock, so that of several inserts that come together one writes the WAL record. The caller holds
 * the cover lock in share mode (no recertification phase runs) and has not called pgvector's insert yet, which locks block 0 itself. The insert
 * that writes the bits writes the candidate of the sweeper too. True if this insert wrote them.
 */
static bool
cover_mark_inserted(Relation index, uint64 generation)
{
	const uint16 both = CUVS_COVER_FLAG_INSERTED | CUVS_COVER_FLAG_INSERTED_SINCE_START;
	Buffer		metabuf = ReadBuffer(index, 0);
	GenericXLogState *xlog;
	Page		metapage;
	CuvsCoverDescriptor d;
	uint32_t	upper;
	bool		wrote = false;

	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	xlog = GenericXLogStart(index);
	metapage = GenericXLogRegisterBuffer(xlog, metabuf, 0);
	upper = cuvs_cover_get16((const uint8_t *) metapage, CUVS_COVER_OFF_UPPER);
	memcpy(&d, (char *) metapage + upper, sizeof(d));
	if (d.magic == CUVS_COVER_D_MAGIC && d.generation == generation && d.state == CUVS_COVER_STATE_CERTIFIED && (d.flags & both) != both)
	{
		d.flags |= both;
		cuvs_cover_descriptor_seal(&d);
		memcpy((char *) metapage + upper, &d, sizeof(d));
		GenericXLogFinish(xlog);
		wrote = true;
	}
	else
		GenericXLogAbort(xlog);
	UnlockReleaseBuffer(metabuf);
	if (wrote)
	{
		elog(DEBUG1, "pg_cuvs: index \"%s\" marked as inserted into since its certification", RelationGetRelationName(index));
		cuvs_cover_note_candidate(RelationGetRelid(index));
	}
	return wrote;
}

static bool
cuvs_cover_aminsert(Relation index, Datum *values, bool *isnull, ItemPointer htid, Relation heap,
					IndexUniqueCheck check, bool unchanged, struct IndexInfo *info)
{
	uint32		tail;
	uint64		generation;
	bool		keyed;
	bool		certified = false;
	bool		verify;
	bool		result;
	uint16		flags = 0;

	/* the cover lock is a transaction lock in the lock manager; it is released as soon as the delegate insert is done */
	LockDatabaseObject(RelationRelationId, RelationGetRelid(index), CUVS_COVER_LOCKSUBID, ShareLock);
	keyed = cover_chain(index, &tail, &generation, &certified, &flags);
	if (keyed && certified && (flags & (CUVS_COVER_FLAG_INSERTED | CUVS_COVER_FLAG_INSERTED_SINCE_START)) !=
		(CUVS_COVER_FLAG_INSERTED | CUVS_COVER_FLAG_INSERTED_SINCE_START))
		(void) cover_mark_inserted(index, generation);
	verify = keyed && certified && cuvs_cover_verify_inserts;
	if (keyed && !verify)
		(void) cover_append(index, tail, generation, certified, htid);
#ifdef CUVS_COVER_RECERT_TEST_GATE
	(void) cuvs_cover_test_gate("insert");
#endif
	result = pgv_aminsert(index, values, isnull, htid, heap, check, unchanged, info);
	if (verify)
	{
		if (cover_try_verified(index, tail, generation, values, isnull, htid))
			cover_note_verified(index, tail, generation);
		else
			(void) cover_append(index, tail, generation, certified, htid);
	}
	UnlockDatabaseObject(RelationRelationId, RelationGetRelid(index), CUVS_COVER_LOCKSUBID, ShareLock);
	return result;
}

void
cuvs_cover_wrap_insert(IndexAmRoutine *amroutine)
{
	pgv_aminsert = amroutine->aminsert;
	amroutine->aminsert = cuvs_cover_aminsert;
}
