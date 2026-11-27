/*
 * cuvs_cover_recert.c - recertification of a pg_cuvs_hnsw index.
 *
 * What it promises when it succeeds: every live row the search cannot reach from the entry point when the recertification began (phase 1, the
 * certification time) is a key of the new ledger, and so is every row inserted while the walk and the copy ran. The walk is not a snapshot: it
 * reads the pages one after the other while inserts go on, and an insert can cut a row off after the walk counted it as reached (it is
 * then in neither part of the ledger, and is outside the promise, which names the certification time and not the end of the walk). The inserts
 * of the walk are inserts since the certification: they count for the next recertification, also when the walk listed their rows themselves
 * and their keys were not carried over (cert_dropped_keys). Three phases:
 *
 *   1. Under the cover lock (exclusive, bounded wait) read the descriptor, make sure a ledger chain exists (so that rows
 *      inserted during the walk are keyed), and note T_start, the ordinal of the next key. Release.
 *   2. Without a lock: walk the graph (cuvs_cover_reach), write the unreached rows as the BASE part of a NEW chain with
 *      the next generation, and copy the keys with ordinal >= T_start from the old chain.
 *   3. Under the cover lock again (exclusive, bounded wait) check that nothing the walk depended on changed (generation,
 *      graph_epoch, flags, state), copy the keys written since, and publish: one WAL record rewrites the descriptor.
 *
 * Anything that goes wrong before the publication leaves the old chain and the old descriptor as they were. The pages of the new chain are
 * written before the walk of the inserts it copies ends; once written, the descriptor names the first of them (pending_head), and a
 * recertification that does not publish (abandoned, deferred, failed, cancelled) leaves them there: the next recertification turns them into
 * free pages before it does anything else, so repeated attempts do not grow the index. The pages are not in any chain meanwhile and nothing
 * reads them. The pages of the replaced chain become the free list at the publication and the next recertification takes them.
 * The WAL of these steps is flushed as it is written (flush_wal_so_far): the transaction does not commit for as long as the recertification runs and
 * the extension of the file is not WAL, so a process that dies would otherwise leave an index larger than its pages say.
 *
 * The blocks of a new chain that has to extend the index are added in small steps, each while the tail page of the old chain is locked,
 * and the size that page remembers is raised by what was added: the growth monitor of an insert (cuvs_cover_insert.c) takes a jump of the
 * index that nobody accounted for as rows written without ledger keys, and a chain of more pages than its limit would take the index off
 * the guarantee (LOST) at the first insert after the allocation, whether or not the recertification went on.
 */
#include "postgres.h"
#include "access/generic_xlog.h"
#include "access/genam.h"
#include "access/table.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/index.h"
#include "catalog/pg_class.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "cuvs_cover_format.h"
#include "cuvs_cover_insert.h"
#include "cuvs_cover_ledger.h"
#include "cuvs_cover_reach_pg.h"
#include "cuvs_cover_recert.h"
#include "cuvs_cover_scan.h"
#include "cuvs_cover_testgate.h"
#include "cuvs_cover_worker.h"

#define EXCLUSIVE_WAIT_MS 2000
#define EXCLUSIVE_POLL_MS 20

/* The queued request below is made inside a subtransaction: the lock it gets passes to the transaction's resource owner on commit,
 * which is not always the one that is current for the caller (a portal's, in a function call). The unlock names the owner. */
static bool cover_lock_owned_by_xact = false;

/*
 * Take the cover lock exclusively, waiting at most min(lock_timeout, 2 s).
 *
 * First a short poll without queueing: an index nobody writes to gives the lock at once. Writers take the cover lock in share mode
 * for the length of one insert, so under continuous inserts a conditional request meets a holder every time (review 2f6789ed R7-3:
 * twelve of twelve calls deferred with eight sessions inserting in a loop, and so would the worker and VACUUM). The poll therefore
 * ends in a request that enters the lock manager's queue: the holders ahead of it finish their inserts, the inserts that come
 * after it wait behind it for the few milliseconds the holders need, and the request is served. The queued wait is bounded by
 * lock_timeout (set for the length of the wait) in a subtransaction, whose rollback is how a timeout is taken back without an error
 * reaching the caller.
 */
static bool
cover_lock_exclusive(Oid relid)
{
	LOCKTAG		tag;
	int			bound = (LockTimeout > 0 && LockTimeout < EXCLUSIVE_WAIT_MS) ? LockTimeout : EXCLUSIVE_WAIT_MS;
	int			waited = 0;
	int			saved_timeout = LockTimeout;
	MemoryContext oldcxt = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;
	bool		got = false;

	cover_lock_owned_by_xact = false;
	SET_LOCKTAG_OBJECT(tag, MyDatabaseId, RelationRelationId, relid, CUVS_COVER_LOCKSUBID);
	while (waited < Min(bound, EXCLUSIVE_POLL_MS))
	{
		if (LockAcquire(&tag, ExclusiveLock, false, true) != LOCKACQUIRE_NOT_AVAIL)
			return true;
		if (cuvs_cover_db_command_waits())
			return false;		/* a background worker steps aside for a command on its database */
		CHECK_FOR_INTERRUPTS();
		pg_usleep(1000);
		waited += 1;
	}
	if (waited >= bound)
		return false;

	BeginInternalSubTransaction(NULL);
	PG_TRY();
	{
		LockAcquireResult acquired;

		LockTimeout = bound - waited;
		acquired = LockAcquire(&tag, ExclusiveLock, false, false);
		LockTimeout = saved_timeout;
		if (acquired == LOCKACQUIRE_NOT_AVAIL)
		{
			/* the wait ended by lock_timeout without an error: the lock is not ours (the first version took this for success and ran
			 * both exclusive phases without the lock: review-found by the walk test W3, a held insert) */
			RollbackAndReleaseCurrentSubTransaction();
		}
		else
		{
			ReleaseCurrentSubTransaction();
			got = true;
			cover_lock_owned_by_xact = true;
		}
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		LockTimeout = saved_timeout;
		MemoryContextSwitchTo(oldcxt);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		/* a timeout or a deadlock is "not now"; anything else (a cancel, a shutdown) goes on to the caller */
		if (edata->sqlerrcode != ERRCODE_LOCK_NOT_AVAILABLE && edata->sqlerrcode != ERRCODE_T_R_DEADLOCK_DETECTED)
			ReThrowError(edata);
		FreeErrorData(edata);
	}
	PG_END_TRY();
	MemoryContextSwitchTo(oldcxt);
	CurrentResourceOwner = oldowner;
	return got;
}

static void
cover_unlock_exclusive(Oid relid)
{
	ResourceOwner saved = CurrentResourceOwner;

	if (cover_lock_owned_by_xact)
		CurrentResourceOwner = CurTransactionResourceOwner;
	cover_lock_owned_by_xact = false;
	UnlockDatabaseObject(RelationRelationId, relid, CUVS_COVER_LOCKSUBID, ExclusiveLock);
	CurrentResourceOwner = saved;
}

/* A copy of the descriptor and its classification, from a share-locked read of block 0. */
static bool
read_descriptor(Relation index, CuvsCoverDescriptor *d, CuvsCoverResult *res)
{
	Buffer		buf = ReadBuffer(index, 0);
	uint8_t		meta[CUVS_COVER_PAGE_SIZE];
	uint32_t	upper;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	memcpy(meta, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	cuvs_cover_classify(meta, NULL, res);
	if (res->cls != CUVS_COVER_CERTIFIED && res->cls != CUVS_COVER_LOST)
		return false;
	upper = cuvs_cover_get16(meta, CUVS_COVER_OFF_UPPER);
	memcpy(d, meta + upper, sizeof(*d));
	return true;
}

typedef struct Collect
{
	ItemPointerData *tids;
	Size		used, cap;
	MemoryContext cxt;
	Size		max_used;		/* the list stops here: it is held in memory next to the bitmaps the limit already counts (0 = no stop) */
	bool		over;
} Collect;

/*
 * What a row of the ledger costs while a recertification holds it: the TID list (6 bytes, up to twice that while it grows), the sorted copy
 * used to drop carried-over keys (8), and the page image of the new chain (about 24). The reachability walk accounts its own arrays against
 * the memory limit; these lists are accounted here against the same limit, so a recertification holds at most about twice
 * maintenance_work_mem.
 */
#define CUVS_RECERT_BYTES_PER_KEY 48

static int
collect_tid(void *ctx, uint32 blk, uint16 off)
{
	Collect    *c = ctx;

	if (c->max_used != 0 && c->used >= c->max_used)
	{
		c->over = true;
		return 1;
	}
	if (c->used == c->cap)
	{
		Size		ncap = c->cap ? c->cap * 2 : 4096;

		c->tids = c->tids ? repalloc_huge(c->tids, ncap * sizeof(ItemPointerData))
			: MemoryContextAllocHuge(c->cxt, ncap * sizeof(ItemPointerData));
		c->cap = ncap;
	}
	ItemPointerSet(&c->tids[c->used++], blk, off);
	return 0;
}

typedef struct KeyList
{
	CuvsLedgerKey *keys;
	Size		used, cap;
	uint64		from;			/* keep keys with ordinal >= from */
	MemoryContext cxt;
} KeyList;

static int
keylist_read(void *ctx, uint32_t blk, uint8_t *dst)
{
	Relation	rel = ctx;
	Buffer		buf;

	if (blk >= RelationGetNumberOfBlocks(rel))
		return 1;
	buf = ReadBuffer(rel, blk);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	memcpy(dst, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	return 0;
}

static int
keylist_add(void *ctx, const CuvsLedgerKey *k)
{
	KeyList    *l = ctx;

	if (k->ordinal < l->from)
		return 0;
	if (l->used == l->cap)
	{
		Size		ncap = l->cap ? l->cap * 2 : 1024;

		l->keys = l->keys ? repalloc_huge(l->keys, ncap * sizeof(CuvsLedgerKey))
			: MemoryContextAllocHuge(l->cxt, ncap * sizeof(CuvsLedgerKey));
		l->cap = ncap;
	}
	l->keys[l->used++] = *k;
	return 0;
}

/* Keys of the chain (head, generation) with ordinal >= from. False if the chain cannot be read as a whole. */
static bool
read_keys_from(Relation index, uint32 head, uint32 must_include, uint64 generation, uint64 from, MemoryContext cxt, KeyList *out)
{
	uint64_t	total;

	memset(out, 0, sizeof(*out));
	out->from = from;
	out->cxt = cxt;
	return cuvs_ledger_walk(keylist_read, index, head, generation, RelationGetNumberOfBlocks(index), 0, must_include, keylist_add, out, &total)
		== CUVS_LEDGER_WALK_OK;
}

/* Write one fully built ledger page image to block `blk` (an extended, so far unused block), as a full-page WAL image. */
static void
write_image(Relation index, BlockNumber blk, const uint8_t *image)
{
	Buffer		buf = ReadBuffer(index, blk);
	GenericXLogState *xlog;
	Page		page;

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	xlog = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(xlog, buf, GENERIC_XLOG_FULL_IMAGE);
	memcpy(page, image, BLCKSZ);
	GenericXLogFinish(xlog);
	UnlockReleaseBuffer(buf);
}

/* One new empty KEYS page as a chain by itself; returns its block. Used when an index has no chain yet. */
static BlockNumber
new_empty_chain(Relation index, uint64 generation, uint64 first_ordinal)
{
	uint8_t		image[CUVS_COVER_PAGE_SIZE];
	Buffer		buf;
	BlockNumber blk;

	LockRelationForExtension(index, ExclusiveLock);
	buf = ReadBuffer(index, P_NEW);
	blk = BufferGetBlockNumber(buf);
	ReleaseBuffer(buf);
	UnlockRelationForExtension(index, ExclusiveLock);
	cuvs_ledger_page_init(image, CUVS_LEDGER_KIND_KEYS, generation, first_ordinal, CUVS_LEDGER_NO_BLOCK, RelationGetNumberOfBlocks(index));
	write_image(index, blk, image);
	return blk;
}

/* Rewrite the descriptor of `index` in place with `edit` applied, as one WAL record; optionally also registers `extra`. */
typedef void (*DescriptorEdit) (CuvsCoverDescriptor *d, void *arg);

static void
edit_descriptor(Relation index, DescriptorEdit edit, void *arg)
{
	Buffer		buf = ReadBuffer(index, 0);
	GenericXLogState *xlog;
	Page		page;
	CuvsCoverDescriptor d;
	uint32_t	upper;

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	xlog = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(xlog, buf, 0);
	upper = cuvs_cover_get16((const uint8_t *) page, CUVS_COVER_OFF_UPPER);
	memcpy(&d, (char *) page + upper, sizeof(d));
	edit(&d, arg);
	cuvs_cover_descriptor_seal(&d);
	memcpy((char *) page + upper, &d, sizeof(d));
	GenericXLogFinish(xlog);
	UnlockReleaseBuffer(buf);
}

typedef struct FailArg
{
	uint32		nblocks, status;
	uint64		limit, needed;
} FailArg;

static void
edit_fail(CuvsCoverDescriptor *d, void *arg)
{
	FailArg    *f = arg;

	d->last_fail_nblocks = f->nblocks;
	d->last_fail_status = f->status;
	d->last_fail_limit = f->limit;
	d->last_fail_needed = f->needed;
	if (d->state == CUVS_COVER_STATE_LOST)
		d->reason = CUVS_COVER_REASON_RECERT_FAILED;
}

typedef struct ChainArg
{
	uint32		new_generation_chainless;	/* phase 1 for an index without a chain */
	uint32		head;
} ChainArg;

typedef struct PublishArg
{
	uint64		generation;
	uint32		head, tail;
	uint64		live, missing, kept;
	uint64		dropped;		/* carried-over keys not written, because the walk listed their rows (still inserts since the certification) */
	uint32		nblocks;
	uint32		free_head;		/* the replaced chain, in front of the pages that were not reused */
	TimestampTz cert_start;		/* when phase 1 held the cover lock: the certification time */
} PublishArg;

/*
 * Pages of the free list (the ledger pages of replaced chains, linked through their next pointers) for the new chain: up to
 * `want` of them, in `blks`. `*rest` is the block after the last one taken. The list is trusted only as far as it reads as
 * ledger pages of an older generation inside the relation, without a loop; from the first page that does not, it is ignored
 * (those pages are not reused, nothing else changes).
 */
static Size
take_free_pages(Relation index, uint32 free_head, uint64 new_generation, uint32 *blks, Size want, uint32 *rest)
{
	uint32		nblocks = RelationGetNumberOfBlocks(index);
	uint32		blk = free_head;
	Size		got = 0, steps = 0;
	uint8_t		page[CUVS_COVER_PAGE_SIZE];
	CuvsLedgerHeader h;

	while (blk != CUVS_LEDGER_NO_BLOCK && got < want)
	{
		if (blk < 2 || blk >= nblocks || steps++ > nblocks || keylist_read(index, blk, page) != 0 ||
			cuvs_ledger_validate(page, &h) != CUVS_LEDGER_OK || h.generation >= new_generation)
		{
			blk = CUVS_LEDGER_NO_BLOCK;
			break;
		}
		blks[got++] = blk;
		blk = cuvs_ledger_next(page);
	}
	*rest = blk;
	return got;
}

static void
edit_publish(CuvsCoverDescriptor *d, void *arg)
{
	PublishArg *p = arg;

	d->state = CUVS_COVER_STATE_CERTIFIED;
	d->reason = CUVS_COVER_REASON_NONE;
	d->flags &= ~CUVS_COVER_FLAG_SUSPECT_UNKEYED_WRITER;
	/* the new certification's cert_time is phase 1, and what was inserted since then is what phase 1 started to record */
	if ((d->flags & CUVS_COVER_FLAG_INSERTED_SINCE_START) != 0)
		d->flags |= CUVS_COVER_FLAG_INSERTED;
	else
		d->flags &= ~CUVS_COVER_FLAG_INSERTED;
	d->generation = p->generation;
	d->recert_generation = p->generation;
	d->ledger_head = p->head;
	d->ledger_tail = p->tail;
	d->free_head = p->free_head;
	d->base_rows = p->live;
	d->base_missing = p->missing;
	d->delta_count = p->kept;
	d->cert_dropped_keys = p->dropped;
	/* the bytes of the descriptor that belong to a certification and that this version does not use are the previous certification's: cleared */
	d->reserved2 = 0;
	memset(d->reserved3, 0, sizeof(d->reserved3));
	d->pending_head = 0;
	d->pending_generation = 0;
	d->graph_nblocks_at_cert = p->nblocks;
	d->cert_lsn = GetXLogInsertRecPtr();
	d->cert_time = p->cert_start != 0 ? p->cert_start : GetCurrentTimestamp();
	d->cert_published_time = GetCurrentTimestamp();
	d->last_fail_nblocks = 0;
	d->last_fail_status = 0;
	d->last_fail_limit = 0;
	d->last_fail_needed = 0;
}

#ifdef CUVS_COVER_RECERT_TEST_GATE
/* the first character of the "touch" file says which field to change: e = graph_epoch, g = generation, s = state (LOST) */
static void
edit_touch(CuvsCoverDescriptor *d, void *arg)
{
	switch (*(char *) arg)
	{
		case 'e':
			d->graph_epoch++;
			break;
		case 'g':
			d->generation++;
			break;
		case 's':
			d->state = CUVS_COVER_STATE_LOST;
			d->reason = CUVS_COVER_REASON_UNKEYED_WRITER;
			break;
	}
}
#endif

/* phase 1: from here on INSERTED_SINCE_START records the inserts after the cert_time this recertification will publish */
static void
edit_clear_since_start(CuvsCoverDescriptor *d, void *arg)
{
	d->flags &= ~CUVS_COVER_FLAG_INSERTED_SINCE_START;
}

static void
edit_free_head(CuvsCoverDescriptor *d, void *arg)
{
	d->free_head = *(uint32 *) arg;
}

typedef struct PendingArg
{
	uint32		head;			/* the first block of the new chain, or the head of the free list that takes the pages back */
	uint64		generation;
	uint32		expected;		/* edit_reclaim: the pending_head this was decided for */
} PendingArg;

/* the new chain is written: from here on an abandoned recertification leaves it where the next one finds it */
static void
edit_pending(CuvsCoverDescriptor *d, void *arg)
{
	PendingArg *a = arg;

	d->pending_head = a->head;
	d->pending_generation = a->generation;
}

/* the pending pages are free pages now (the head of the free list is the one the caller linked them to), or there is nothing to take back */
static void
edit_reclaim(CuvsCoverDescriptor *d, void *arg)
{
	PendingArg *a = arg;

	if (d->pending_head != a->expected)
		return;				/* changed meanwhile: leave it */
	if (a->head != CUVS_LEDGER_NO_BLOCK)
		d->free_head = a->head;
	d->pending_head = 0;
	d->pending_generation = 0;
}

/*
 * The pages of a new chain that a recertification wrote and did not publish (see the head of this file): they become the front of the free
 * list, in one descriptor change after their images say so. The pages are trusted as far as they read as ledger pages of the pending
 * generation inside the relation, in a chain without a loop; from the first that does not, the rest is not taken. Returns how many pages were taken.
 * Called before the first phase, with the table lock that serialises recertifications; no cover lock is needed, nothing else reads these pages.
 */
static Size
reclaim_pending(Relation index)
{
	CuvsCoverDescriptor d;
	CuvsCoverResult res;
	uint32		nblocks = RelationGetNumberOfBlocks(index);
	uint32		blk;
	uint32	   *blks = NULL;
	Size		n = 0, cap = 0, steps = 0, i;
	uint8_t		page[CUVS_COVER_PAGE_SIZE];
	CuvsLedgerHeader h;
	PendingArg	a;

	if (!read_descriptor(index, &d, &res) || !res.writable || d.pending_head == 0)
		return 0;
	a.expected = d.pending_head;
	a.generation = 0;
	a.head = CUVS_LEDGER_NO_BLOCK;
	/* not the live chain's, and not older than the live generation (stale: a library that did not know the field published since) */
	if (d.pending_generation > d.generation && d.pending_head != d.ledger_head)
	{
		blk = d.pending_head;
		while (blk != CUVS_LEDGER_NO_BLOCK && blk >= 2 && blk < nblocks && steps++ <= nblocks && keylist_read(index, blk, page) == 0 &&
			   cuvs_ledger_validate(page, &h) == CUVS_LEDGER_OK && h.generation == d.pending_generation)
		{
			if (n == cap)
			{
				cap = cap ? cap * 2 : 256;
				blks = blks ? repalloc(blks, cap * sizeof(uint32)) : palloc(cap * sizeof(uint32));
			}
			blks[n++] = blk;
			blk = cuvs_ledger_next(page);
		}
	}
	if (n > 0)
	{
		uint8_t		img[CUVS_COVER_PAGE_SIZE];

		/* last page first; a crash half way leaves a head that still reads as pending and pages behind it that no list reaches (REINDEX returns them) */
		for (i = n; i-- > 0;)
		{
			cuvs_ledger_page_init(img, CUVS_LEDGER_KIND_KEYS, 0, 0, i + 1 < n ? blks[i + 1] : d.free_head, 0);
			write_image(index, blks[i], img);
		}
		a.head = blks[0];
	}
	edit_descriptor(index, edit_reclaim, &a);
	if (blks)
		pfree(blks);
	return n;
}

/*
 * The WAL written so far is on disk when this returns. A recertification is one transaction that does not commit for as long as it runs, and
 * the extension of the file is not WAL: if the process dies (kill -9, the OOM killer, a crash of the server) the WAL records that say what
 * the extension did (the tail page that remembers the new size, the descriptor that names the new pages) can be the last bytes of the WAL
 * buffers and are not replayed, and recovery gives back an index that is larger than its pages say: the next insert takes the growth for rows
 * written without keys and the index leaves the guarantee. Called after each of those records.
 */
static void
flush_wal_so_far(Relation index)
{
	XLogRecPtr	end;

	if (!RelationNeedsWAL(index))
		return;
	/*
	 * The end of the last record this transaction inserted, not the insert position of the server: that position can be the first byte after
	 * the header of a WAL page that has no record yet, and XLogFlush cannot make it durable ("xlog flush request ... is not satisfied --- flushed
	 * only to ..." at that position minus 0x18, the size of the page header). It is 0 while this transaction has inserted nothing: nothing to flush.
	 */
	end = XactLastRecEnd;
	if (end != 0)
		XLogFlush(end);
}

/*
 * `want` new blocks for the new chain, in blks[]. They are added in steps of at most 64, and each step happens while the tail page of the old
 * ledger chain (the page an insert that keys a row writes) is locked exclusively: no insert can look at the size of the index between the
 * blocks being added and the page remembering them, and the page remembers `step` blocks more (cuvs_ledger_raise_seen). Without that the
 * first insert after the allocation would see the index grown by the whole new chain since its last key (see the head of this file).
 * A tail page that does not read as the chain says (the chain was replaced) is not touched and the blocks are added as they were.
 */
#define CUVS_RECERT_EXTEND_STEP 64

static void
extend_for_chain(Relation index, uint32 tail_hint, uint64 generation, Size want, uint32 *blks)
{
	Size		done = 0;

	while (done < want)
	{
		Size		step = Min(want - done, (Size) CUVS_RECERT_EXTEND_STEP), j;
		Buffer		tbuf = InvalidBuffer;
		uint32		blk = tail_hint;
		uint32		hops;

		for (hops = 0; hops < 1000000 && blk != CUVS_LEDGER_NO_BLOCK && blk >= 2; hops++)
		{
			CuvsLedgerHeader h;
			Buffer		b = ReadBuffer(index, blk);
			uint32		next;

			LockBuffer(b, BUFFER_LOCK_EXCLUSIVE);
			if (cuvs_ledger_validate((const uint8_t *) BufferGetPage(b), &h) != CUVS_LEDGER_OK || h.generation != generation ||
				h.kind != CUVS_LEDGER_KIND_KEYS)
			{
				UnlockReleaseBuffer(b);
				break;
			}
			next = cuvs_ledger_next((const uint8_t *) BufferGetPage(b));
			if (next != CUVS_LEDGER_NO_BLOCK)
			{
				UnlockReleaseBuffer(b);	/* an insert added a page after the one this looked at: the tail is further on */
				blk = next;
				continue;
			}
			tbuf = b;
			break;
		}
		LockRelationForExtension(index, ExclusiveLock);
		for (j = 0; j < step; j++)
		{
			Buffer		nb = ReadBuffer(index, P_NEW);

			blks[done + j] = BufferGetBlockNumber(nb);
			ReleaseBuffer(nb);
		}
		UnlockRelationForExtension(index, ExclusiveLock);
		if (tbuf != InvalidBuffer)
		{
			GenericXLogState *xlog = GenericXLogStart(index);
			Page		page = GenericXLogRegisterBuffer(xlog, tbuf, 0);

			if (cuvs_ledger_raise_seen((uint8_t *) page, (uint32) step) == CUVS_LEDGER_OK)
				GenericXLogFinish(xlog);
			else
				GenericXLogAbort(xlog);
			UnlockReleaseBuffer(tbuf);
		}
		flush_wal_so_far(index);
		done += step;
	}
}

/* edit_publish, plus the next pointer of the last page of the replaced chain, in the same WAL record: the replaced chain stops
 * being a chain and becomes the head of the free list in one step. The caller holds the cover lock exclusively. */
static void
publish_descriptor(Relation index, DescriptorEdit edit, void *arg, BlockNumber old_tail, uint32 old_tail_next)
{
	Buffer		buf = ReadBuffer(index, 0);
	Buffer		tailbuf = ReadBuffer(index, old_tail);
	GenericXLogState *xlog;
	Page		page, tailpage;
	CuvsCoverDescriptor d;
	uint32_t	upper;

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	LockBuffer(tailbuf, BUFFER_LOCK_EXCLUSIVE);
	xlog = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(xlog, buf, 0);
	tailpage = GenericXLogRegisterBuffer(xlog, tailbuf, 0);
	upper = cuvs_cover_get16((const uint8_t *) page, CUVS_COVER_OFF_UPPER);
	memcpy(&d, (char *) page + upper, sizeof(d));
	edit(&d, arg);
	cuvs_cover_descriptor_seal(&d);
	memcpy((char *) page + upper, &d, sizeof(d));
	cuvs_ledger_set_next((uint8_t *) tailpage, old_tail_next);
	GenericXLogFinish(xlog);
	UnlockReleaseBuffer(tailbuf);
	UnlockReleaseBuffer(buf);
}

/* A damaged chain is dropped and a fresh empty one takes its place, in one record: the index is LOST (ledger damaged) from the
 * moment its old keys stop counting, and graph_epoch goes up so that nothing that read the old chain publishes on it. */
static void
edit_chain_fresh(CuvsCoverDescriptor *d, void *arg)
{
	PublishArg *p = arg;

	d->state = CUVS_COVER_STATE_LOST;
	d->reason = CUVS_COVER_REASON_LEDGER_DAMAGED;
	d->graph_epoch++;
	d->generation = p->generation;
	d->ledger_head = p->head;
	d->ledger_tail = p->tail;
}

/* Set the chain of an index that has none (phase 1): the descriptor keeps its state, only the chain appears. */
static void
edit_chain(CuvsCoverDescriptor *d, void *arg)
{
	PublishArg *p = arg;

	d->generation = p->generation;
	d->ledger_head = p->head;
	d->ledger_tail = p->tail;
}

/* Keys written since the certification: those after the BASE ones, so the total of the tail page minus the BASE keys (base_missing less
 * the keys of rows the walk listed itself, which are inserts since the certification too); one page is read. False if the chain does not match the descriptor. */
static bool
growth_delta(Relation index, const CuvsCoverDescriptor *d, const CuvsCoverResult *res, uint64 *delta)
{
	uint8_t		page[CUVS_COVER_PAGE_SIZE];
	CuvsLedgerHeader h;
	uint64		total;

	*delta = 0;
	if (res->ledger_tail == CUVS_COVER_INVALID_BLOCK)
		return true;
	if (keylist_read(index, res->ledger_tail, page) != 0 || cuvs_ledger_validate(page, &h) != CUVS_LEDGER_OK ||
		h.generation != d->generation)
		return false;
	total = h.first_ordinal + h.count;
	*delta = total > cuvs_cover_base_keys(d) ? total - cuvs_cover_base_keys(d) : 0;
	return true;
}

/*
 * The pages the relation has grown by since the certification (graph_nblocks_at_cert is taken after the pages of the new ledger chain were allocated,
 * so a recertification's own pages are not counted). An insert that wrote a key and an insert that was verified and wrote none both grow the graph, which is
 * why this, and not the number of keys, tells whether rows were inserted since the certification: with the insert verification (cuvs.cover_verify_inserts) a row
 * that is reachable when it is inserted gets no key, and a later insert can still cut it off. A handful of inserts that fit in the free space of the last page
 * do not move this; the growth rule and the time trigger take the index once the inserts have filled a page.
 */
uint64
cuvs_cover_pages_since_cert(Relation index, const CuvsCoverDescriptor *d)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);

	return nblocks > d->graph_nblocks_at_cert ? nblocks - d->graph_nblocks_at_cert : 0;
}

/* The same ratio as the keys: the pages added since the certification as a percent of the pages it covered (rows and pages grow together). */
static bool
pages_over_threshold(Relation index, const CuvsCoverDescriptor *d)
{
	uint64		pages = cuvs_cover_pages_since_cert(index, d);

	return pages >= 1 && pages * 100 >= (uint64) cuvs_cover_recert_percent * (d->graph_nblocks_at_cert ? d->graph_nblocks_at_cert : 1);
}

static bool
growth_over_threshold(const CuvsCoverDescriptor *d, uint64 delta)
{
	/* a ratio only: a small table is cheap to recertify, and one that is left uncertified reads slower at every rescan */
	return delta >= 1 &&
		delta * 100 >= (uint64) cuvs_cover_recert_percent * (d->base_rows ? d->base_rows : 1);
}

/* Is a recertification of this CERTIFIED index due: the keys written since the certification are past the threshold, or the ledger
 * does not read (a flipped bit in a key, a cut chain: a recertification builds a new one, REINDEX is not needed). The whole
 * ledger is read, which is far less than a vacuum reads of the table. */
static bool
growth_due(Relation index, const CuvsCoverDescriptor *d, const CuvsCoverResult *res)
{
	uint64		nkeys;
	const char *why;

	if (res->ledger_tail == CUVS_COVER_INVALID_BLOCK)
		return false;
	if (!cuvs_cover_ledger_check(index, res, &nkeys, &why))
		return true;
	return growth_over_threshold(d, nkeys > cuvs_cover_base_keys(d) ? nkeys - cuvs_cover_base_keys(d) : 0) || pages_over_threshold(index, d);
}

/* Is a CERTIFIED index due for a recertification by the growth rule (or because its ledger does not read)? For the background worker,
 * which runs again while inserts that arrived during its run have put the ledger past the threshold once more. */
/* A CERTIFIED index with at least one key, one page or one insert (CUVS_COVER_FLAG_INSERTED) since its certification, or a ledger that does not read: what a time-based recertification takes. */
static bool
growth_nonzero(Relation index, const CuvsCoverDescriptor *d, const CuvsCoverResult *res)
{
	uint64		nkeys;
	const char *why;

	if (res->ledger_tail == CUVS_COVER_INVALID_BLOCK)
		return false;
	if ((d->flags & CUVS_COVER_FLAG_INSERTED) != 0)
		return true;			/* an insert since the certification, keyed or not */
	if (!cuvs_cover_ledger_check(index, res, &nkeys, &why))
		return true;
	return nkeys > cuvs_cover_base_keys(d) || cuvs_cover_pages_since_cert(index, d) > 0;
}

/*
 * Is a time-based recertification due: the index is LOST (a recertification fixes it), or CERTIFIED and past the growth threshold, or CERTIFIED with at
 * least one key or one page added since its certification, which began (cert_time) at least `max_age_us` ago, or CERTIFIED with a ledger that does not read. An index nobody inserted into since is
 * never due: nothing could have cut a row off. The pages and the insert flag count because an insert that the verification found reachable writes no key
 * and can still be cut off by a later one, in a page that was already there. Reads two or three pages; no lock beyond the caller's.
 */
bool
cuvs_cover_sweep_due(Relation index, TimestampTz now, int64 max_age_us)
{
	CuvsCoverDescriptor d;
	CuvsCoverResult res;

	if (cuvs_cover_breakglass_marked() || !read_descriptor(index, &d, &res) || !res.writable)
		return false;
	/* a LOST index is due, except while the VACUUM that took it off runs: that VACUUM recertifies it at its end, and a worker now would only be
	 * deferred by the VACUUM's lock at every sweep */
	if (res.cls == CUVS_COVER_LOST)
		return !(res.stored_reason == CUVS_COVER_REASON_VACUUM && cuvs_cover_vacuum_running(RelationGetRelid(index)));
	if (res.cls != CUVS_COVER_CERTIFIED || !growth_nonzero(index, &d, &res))
		return false;
	/* past the growth threshold: at the next sweep, whatever the age (no insert asks for a worker in this version) */
	if (growth_due(index, &d, &res))
		return true;
	if (max_age_us <= 0)
		return false;			/* cuvs.cover_recertify_max_age = 0: no time rule */
	return d.cert_time == 0 || now - d.cert_time >= max_age_us;
}

bool
cuvs_cover_sweep_pending(Relation index)
{
	CuvsCoverDescriptor d;
	CuvsCoverResult res;

	if (cuvs_cover_breakglass_marked() || !read_descriptor(index, &d, &res) || !res.writable)
		return false;
	if (res.cls == CUVS_COVER_LOST)
		return true;
	return res.cls == CUVS_COVER_CERTIFIED && growth_nonzero(index, &d, &res);
}

bool
cuvs_cover_recertify_due(Relation index)
{
	CuvsCoverDescriptor d;
	CuvsCoverResult res;

	return read_descriptor(index, &d, &res) && res.cls == CUVS_COVER_CERTIFIED && res.writable && growth_due(index, &d, &res);
}

/* For the status function: the descriptor of `index`, the keys written since the certification and whether that is past the
 * threshold. False if the index has no descriptor this library reads (or the chain does not match it). */
bool
cuvs_cover_growth_status(Relation index, CuvsCoverDescriptor *d, uint64 *delta_keys, bool *recommended)
{
	CuvsCoverResult res;

	if (!read_descriptor(index, d, &res) || !growth_delta(index, d, &res, delta_keys))
		return false;
	*recommended = res.cls == CUVS_COVER_CERTIFIED && (growth_over_threshold(d, *delta_keys) || pages_over_threshold(index, d));
	return true;
}

static int
cover_u64_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y ? 1 : 0;
}

static void recertify_impl(Relation heap, Relation index, uint64 ram_limit, uint64 spill_limit, bool force, bool aged, bool yield, CuvsRecertOutcome *out);

void
cuvs_cover_recertify(Relation heap, Relation index, uint64 ram_limit, uint64 spill_limit, bool force, CuvsRecertOutcome *out)
{
	recertify_impl(heap, index, ram_limit, spill_limit, force, false, false, out);
}

/* The same for a caller that is not a VACUUM: the walk gives way to a session that waits for a lock on the table or the index. */
void
cuvs_cover_recertify_yielding(Relation heap, Relation index, uint64 ram_limit, uint64 spill_limit, bool force, CuvsRecertOutcome *out)
{
	recertify_impl(heap, index, ram_limit, spill_limit, force, false, true, out);
}

/* The time-based recertification: a CERTIFIED index is taken when any key was written since its certification, however few. The failure memory applies. */
void
cuvs_cover_recertify_aged(Relation heap, Relation index, uint64 ram_limit, uint64 spill_limit, CuvsRecertOutcome *out)
{
	recertify_impl(heap, index, ram_limit, spill_limit, false, true, true, out);
}

static void
recertify_impl(Relation heap, Relation index, uint64 ram_limit, uint64 spill_limit, bool force, bool aged, bool yield, CuvsRecertOutcome *out)
{
	Oid			relid = RelationGetRelid(index);
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "HNSW cover recert", ALLOCSET_DEFAULT_SIZES);
	MemoryContext previous = MemoryContextSwitchTo(cxt);
	CuvsCoverDescriptor d0, d1;
	CuvsCoverResult res;
	CuvsReachCounts counts;
	Collect		unreached = {NULL, 0, 0, cxt, Max((Size) 1, (Size) (ram_limit / CUVS_RECERT_BYTES_PER_KEY)), false};
	KeyList		delta;
	uint64		t_start, new_generation, copied_upto;
	uint64		dropped = 0;
	uint64	   *listed = NULL;
	TimestampTz cert_start = 0, lock3_at = 0, walk_at;
	Size		listed_n = 0, k0;
	uint32		old_head, old_tail;
	bool		locked = false;

	memset(out, 0, sizeof(*out));
	out->status = CUVS_RECERT_NOT_APPLICABLE;

	/* After an emergency rollback nothing is certified again until the update script has run: a certification now would be
	 * read as lost anyway, and the older library's writes since the rollback are not in any ledger. */
	if (cuvs_cover_breakglass_marked())
	{
		out->detail = "the break-glass marker exists: ALTER EXTENSION pg_cuvs UPDATE TO '0.9.2' and then TO '0.9.3' with the new library installed ends it";
		goto done;
	}

	/* a CERTIFIED index is only taken when its ledger has grown enough: look first, without the lock, so that the many vacuums
	 * that find nothing to do neither block inserts nor read the ledger */
	if (!force && read_descriptor(index, &d0, &res) && res.cls == CUVS_COVER_CERTIFIED && !(aged ? growth_nonzero(index, &d0, &res) : growth_due(index, &d0, &res)))
	{
		out->detail = "certified, and not enough keys since";
		goto done;
	}

	/* the pages a recertification that did not publish left behind are free pages before this one allocates its own */
	out->reclaimed_pages = reclaim_pending(index);

	/* ---- phase 1 ---- */
	if (!cover_lock_exclusive(relid))
	{
		out->status = CUVS_RECERT_DEFERRED;
		out->detail = "the cover lock could not be taken within the bound";
		goto done;
	}
	locked = true;
	cert_start = GetCurrentTimestamp();
	if (!read_descriptor(index, &d0, &res) || !res.writable)
	{
		out->detail = "the index has no descriptor this library may rewrite";
		goto done;
	}
	if (res.cls == CUVS_COVER_CERTIFIED && !force && !(aged ? growth_nonzero(index, &d0, &res) : growth_due(index, &d0, &res)))
	{
		out->detail = "certified, and not enough keys since";
		goto done;
	}
	/* failure memory: the same walk in the same conditions would fail the same way */
	if (d0.last_fail_nblocks != 0 && !force && RelationGetNumberOfBlocks(index) <= d0.last_fail_nblocks &&
		ram_limit + spill_limit <= d0.last_fail_limit)
	{
		out->status = CUVS_RECERT_SKIPPED;
		out->needed_bytes = d0.last_fail_needed;
		out->reach_status = (int) d0.last_fail_status;
		out->detail = "an earlier recertification gave up at this size and limit";
		goto done;
	}
	old_head = res.ledger_head;
	old_tail = res.ledger_tail;
	if (old_head != CUVS_COVER_INVALID_BLOCK)
	{
		/* The whole chain is read once: every page, every key crc, the chain ends in the tail the descriptor names. What it
		 * holds is only needed from T_start on, but a chain that does not read cannot be copied from either, and one that is
		 * left as it is would fail every later attempt the same way. */
		KeyList		probe;

		if (!read_keys_from(index, old_head, old_tail, d0.generation, UINT64_MAX, cxt, &probe))
		{
			PublishArg a;

			memset(&a, 0, sizeof(a));
			a.generation = d0.generation + 1;
			a.head = a.tail = new_empty_chain(index, a.generation, 0);
			edit_descriptor(index, edit_chain_fresh, &a);
			old_head = old_tail = a.head;
			t_start = 0;
			if (!read_descriptor(index, &d0, &res))
			{
				out->detail = "the descriptor disappeared";
				goto done;
			}
			old_head = res.ledger_head;
			old_tail = res.ledger_tail;
			out->chain_replaced = true;
		}
	}
	if (old_head == CUVS_COVER_INVALID_BLOCK)
	{
		/* no chain yet: make an empty one now, so that rows inserted while the walk runs are keyed */
		PublishArg a;

		memset(&a, 0, sizeof(a));
		a.generation = d0.generation + 1;
		a.head = a.tail = new_empty_chain(index, a.generation, 0);
		edit_descriptor(index, edit_chain, &a);
		old_head = old_tail = a.head;
		t_start = 0;
		if (!read_descriptor(index, &d0, &res))
		{
			out->detail = "the descriptor disappeared";
			goto done;
		}
	}
	else if (!out->chain_replaced)
	{
		uint8_t		tailpage[CUVS_COVER_PAGE_SIZE];
		CuvsLedgerHeader h;

		if (keylist_read(index, old_tail, tailpage) != 0 || cuvs_ledger_validate(tailpage, &h) != CUVS_LEDGER_OK ||
			h.generation != d0.generation)
		{
			out->detail = "the ledger chain does not match the descriptor";
			out->status = CUVS_RECERT_FAILED;
			out->reach_status = 4;
			goto done;
		}
		t_start = h.first_ordinal + h.count;
	}
	/* no insert runs now (the cover lock is ours): an insert after this sets the bit again, and an abandoned walk leaves INSERTED as it was */
	if ((d0.flags & CUVS_COVER_FLAG_INSERTED_SINCE_START) != 0)
		edit_descriptor(index, edit_clear_since_start, NULL);
	new_generation = d0.generation + 1;
	out->phase1_lock_us = (uint64) (GetCurrentTimestamp() - cert_start);
	cover_unlock_exclusive(relid);
	locked = false;

	/* ---- phase 2: no cover lock; inserts go on and write their keys to the old chain ---- */
	walk_at = GetCurrentTimestamp();
	cuvs_cover_reach_index_yielding(index, yield ? heap : NULL, ram_limit, spill_limit, collect_tid, &unreached, &counts);
	out->walk_us = (uint64) (GetCurrentTimestamp() - walk_at);
	out->mem_bytes = counts.mem_bytes;
	out->spilled_bytes = counts.spilled_bytes;
	if (unreached.over)
	{
		/* the list of rows the walk cannot reach does not fit next to its bitmaps: the same as the walk's own limit, with a guess of what it needs */
		FailArg f = {RelationGetNumberOfBlocks(index), 2, ram_limit + spill_limit, ram_limit * 2};

		out->status = CUVS_RECERT_FAILED;
		out->reach_status = 2;
		out->needed_bytes = ram_limit * 2;
		out->detail = "over the memory limits (the list of rows the walk cannot reach)";
		edit_descriptor(index, edit_fail, &f);
		goto done;
	}
	if (counts.status == 5)
	{
		/* another session waits for a lock on the table or the index: nothing was written, the request stays for a later look */
		out->status = CUVS_RECERT_DEFERRED;
		out->detail = "the walk gave way: another session waits for a lock on the table or the index; nothing was done, call again";
		goto done;
	}
	if (counts.status != 0)
	{
		FailArg f = {RelationGetNumberOfBlocks(index), (uint32) counts.status, ram_limit + spill_limit, counts.needed_bytes};

		out->status = CUVS_RECERT_FAILED;
		out->reach_status = counts.status;
		out->needed_bytes = counts.needed_bytes;
		out->detail = counts.status == 2 ? "over the memory limits" : counts.status == 4 ? "the page chain is damaged" : "the walk gave no result";
		edit_descriptor(index, edit_fail, &f);
		goto done;
	}
	out->live = counts.live;
	out->unreached = counts.unreached;
	if (!read_keys_from(index, old_head, old_tail, d0.generation, t_start, cxt, &delta))
	{
		out->status = CUVS_RECERT_ABANDONED;
		out->detail = "the old ledger chain changed during the walk";
		goto done;
	}
	copied_upto = t_start + delta.used;
	if ((Size) (unreached.used + delta.used) > (Size) (ram_limit / CUVS_RECERT_BYTES_PER_KEY))
	{
		FailArg f = {RelationGetNumberOfBlocks(index), 2, ram_limit + spill_limit, ram_limit * 2};

		out->status = CUVS_RECERT_FAILED;
		out->reach_status = 2;
		out->needed_bytes = (uint64) (unreached.used + delta.used) * CUVS_RECERT_BYTES_PER_KEY;
		out->detail = "over the memory limits (the keys of the new ledger)";
		edit_descriptor(index, edit_fail, &f);
		goto done;
	}
	/* the TIDs the walk listed, ascending: a key carried over for one of them is not carried (the row is a candidate through the listed key) */
	listed_n = unreached.used;
	listed = MemoryContextAllocHuge(cxt, (listed_n ? listed_n : 1) * sizeof(uint64));
	for (k0 = 0; k0 < listed_n; k0++)
		listed[k0] = cuvs_ledger_tid_code(ItemPointerGetBlockNumber(&unreached.tids[k0]), ItemPointerGetOffsetNumber(&unreached.tids[k0]));
	qsort(listed, listed_n, sizeof(uint64), cover_u64_cmp);
	{
		/* a TID the walk lists twice would make the binary search ambiguous, not wrong; the ledger of the walk does not repeat one */
		Size		w0 = 0;

		for (k0 = 0; k0 < listed_n; k0++)
			if (w0 == 0 || listed[w0 - 1] != listed[k0])
				listed[w0++] = listed[k0];
		listed_n = w0;
	}
	{
		Size		before = delta.used;

		delta.used = cuvs_ledger_drop_listed(delta.keys, delta.used, listed, listed_n);
		dropped += before - delta.used;
	}

	/* ---- the new chain: BASE pages for the unreached rows, then KEYS pages for the carried-over keys ---- */
	{
		Size		nbase = unreached.used, ndelta = delta.used, npages, i, k;
		Size		base_pages = (nbase + CUVS_LEDGER_CAPACITY - 1) / CUVS_LEDGER_CAPACITY;
		Size		delta_pages = ndelta / CUVS_LEDGER_CAPACITY + 1;	/* always one KEYS page with room at the end */
		uint8_t    *images;
		BlockNumber *blks;
		uint32		seen = RelationGetNumberOfBlocks(index);
		uint64		ordinal = 0;
		Size		at_base = 0, at_delta = 0;
		uint32		blks_tail, d1_tail;
		PublishArg	p;

		npages = base_pages + delta_pages;
		images = MemoryContextAllocHuge(cxt, npages * CUVS_COVER_PAGE_SIZE);
		blks = MemoryContextAllocHuge(cxt, npages * sizeof(BlockNumber));
		/* blocks: the free list first (the pages are unlinked from it before they are overwritten, so that a recertification
		 * that gives up leaves a list that is still whole), then new ones (extend_for_chain) */
		{
			uint32		rest_free;
			Size		reused = take_free_pages(index, d0.free_head, new_generation, blks, npages, &rest_free);

			if (reused > 0)
				edit_descriptor(index, edit_free_head, &rest_free);
			if (reused < npages)
				extend_for_chain(index, old_tail, d0.generation, npages - reused, blks + reused);
			out->reused_pages = reused;
		}
#ifdef CUVS_COVER_RECERT_TEST_GATE
		(void) cuvs_cover_test_gate("extended");	/* the index is extended and remembers it; the pages hold nothing and no list names them yet */
#endif
		seen = RelationGetNumberOfBlocks(index);	/* after the allocation: the growth monitor must not take our own pages for foreign growth */
		for (i = 0; i < npages; i++)
		{
			uint8_t    *img = images + i * CUVS_COVER_PAGE_SIZE;
			bool		is_base = i < base_pages;
			Size		room = CUVS_LEDGER_CAPACITY;

			cuvs_ledger_page_init(img, is_base ? CUVS_LEDGER_KIND_BASE : CUVS_LEDGER_KIND_KEYS, new_generation, ordinal,
								  i + 1 < npages ? blks[i + 1] : CUVS_LEDGER_NO_BLOCK, seen);
			for (k = 0; k < room; k++)
			{
				CuvsLedgerKey key;

				if (is_base)
				{
					if (at_base >= nbase)
						break;
					memset(&key, 0, sizeof(key));
					key.ordinal = ordinal;
					key.heap_block = ItemPointerGetBlockNumber(&unreached.tids[at_base]);
					key.heap_offset = ItemPointerGetOffsetNumber(&unreached.tids[at_base]);
					key.flags = CUVS_LEDGER_KEY_UNKNOWN_CREATOR;
					at_base++;
				}
				else
				{
					if (at_delta >= ndelta)
						break;
					key = delta.keys[at_delta++];
					key.ordinal = ordinal;
				}
				if (cuvs_ledger_append(img, &key) != CUVS_LEDGER_OK)
					elog(ERROR, "pg_cuvs: cannot build a ledger page");
				ordinal++;
			}
		}
		for (i = 0; i < npages; i++)
			write_image(index, blks[i], images + i * CUVS_COVER_PAGE_SIZE);
		{
			/* the pages are written: a recertification that does not publish leaves them where the next one finds them */
			PendingArg	pa;

			pa.head = blks[0];
			pa.generation = new_generation;
			pa.expected = 0;
			edit_descriptor(index, edit_pending, &pa);
			flush_wal_so_far(index);	/* and with it the pages written before it */
		}

		{
			uint32		tail_priv = blks[npages - 1];
			int			pass;

			/* ---- phase 2b: keys written meanwhile are copied to the (still private) new chain without the lock, until what is
			 * left is small, so that the exclusive phase has little to copy ---- */
#ifdef CUVS_COVER_RECERT_TEST_GATE
			{
				char		kind = cuvs_cover_test_gate("recert");

				if (kind != 0)
					edit_descriptor(index, edit_touch, &kind);
			}
#endif
			for (pass = 0; pass < CUVS_RECERT_COPY_PASSES; pass++)
			{
				KeyList		more;
				Size		raw_more;

				if (cuvs_cover_db_command_waits())
				{
					out->status = CUVS_RECERT_ABANDONED;
					out->detail = "a command on the database waits for its sessions: the background worker stepped aside";
					goto done;
				}

				if (!read_keys_from(index, old_head, old_tail, d0.generation, copied_upto, cxt, &more))
				{
					out->status = CUVS_RECERT_ABANDONED;
					out->detail = "the old ledger chain changed during the walk";
					goto done;
				}
				if (more.used <= CUVS_RECERT_TAIL_KEYS)
					break;
				/* The listed TIDs are the walk's. A key written after the walk ended is of a row whose element is inserted after the key, so after
				 * the walk too: the walk cannot have listed it, and here the filter keeps every key (a heap TID is freed by VACUUM only, which
				 * the table lock of a recertification keeps out). The pass is kept the same as the first one for the case that does not hold, and
				 * how far it has read is the filter's own answer (cuvs_ledger_carry), which a unit test fixes: no scenario of the running
				 * server drops a key here, so none can show the difference. */
				more.used = cuvs_ledger_carry(more.keys, more.used, listed, listed_n, &raw_more);
				dropped += raw_more - more.used;
				for (k = 0; k < more.used; k++)
				{
					uint32		t2 = tail_priv;

					cuvs_cover_append_key(index, tail_priv, new_generation, false, &more.keys[k], false, &t2, NULL);
					tail_priv = t2;
				}
				copied_upto += raw_more;
				ndelta += more.used;
			}
			blks_tail = tail_priv;
		}

		/* ---- phase 3 ---- */
		if (!cover_lock_exclusive(relid))
		{
			out->status = CUVS_RECERT_DEFERRED;
			out->detail = "the cover lock could not be taken to publish; the new chain was not used";
			goto done;
		}
		locked = true;
		lock3_at = GetCurrentTimestamp();
		/* the two insert bits change during the walk by design (phase 1 cleared one, inserts set them): not a change of the graph */
		if (!read_descriptor(index, &d1, &res) || d1.generation != d0.generation || d1.graph_epoch != d0.graph_epoch ||
			(d1.flags & ~CUVS_COVER_INSERT_FLAGS) != (d0.flags & ~CUVS_COVER_INSERT_FLAGS) || d1.state != d0.state)
		{
			out->status = CUVS_RECERT_ABANDONED;
			out->detail = "the descriptor changed during the walk (a vacuum, a flag, another recertification)";
			goto done;
		}
		d1_tail = res.ledger_tail;	/* the old chain ends here now: inserts may have extended it */
		/* the keys written to the old chain since phase 2 read it: append them to the new tail */
		{
			KeyList		rest;
			uint32		tail = blks_tail;

			if (!read_keys_from(index, old_head, old_tail, d0.generation, copied_upto, cxt, &rest))
			{
				out->status = CUVS_RECERT_ABANDONED;
				out->detail = "the old ledger chain changed while publishing";
				goto done;
			}
			{
				Size		before = rest.used;

				rest.used = cuvs_ledger_drop_listed(rest.keys, rest.used, listed, listed_n);
				dropped += before - rest.used;
			}
			for (k = 0; k < rest.used; k++)
			{
				uint32		t2 = tail;

				cuvs_cover_append_key(index, tail, new_generation, false, &rest.keys[k], false, &t2, NULL);
				tail = t2;
			}
			memset(&p, 0, sizeof(p));
			p.cert_start = cert_start;
			p.generation = new_generation;
			p.head = blks[0];
			p.tail = tail;
			p.live = counts.live;
			p.missing = counts.unreached;
			p.kept = ndelta + rest.used;
			p.dropped = dropped;
			p.nblocks = RelationGetNumberOfBlocks(index);
			out->kept_keys = p.kept;
			out->locked_keys = rest.used;
		}
		p.free_head = old_head;
		publish_descriptor(index, edit_publish, &p, d1_tail, d1.free_head);
	}
	out->status = CUVS_RECERT_DONE;
	out->detail = "recertified";

done:
	if (locked)
	{
		if (lock3_at != 0)
			out->phase3_lock_us = (uint64) (GetCurrentTimestamp() - lock3_at);
		cover_unlock_exclusive(relid);
	}
	MemoryContextSwitchTo(previous);
	MemoryContextDelete(cxt);
}

/*
 * ---- F6b: VACUUM ----
 *
 * ambulkdelete: pgvector removes dead elements and repairs the neighbour lists of the elements that pointed at them, and a
 * repair can leave an element that was reachable through a removed one without any way in. So a CERTIFIED index is taken off
 * (LOST, reason vacuum, graph_epoch + 1) and the change is WAL-logged BEFORE pgvector changes a page: no crash and no
 * concurrent reader sees changed graph pages together with a descriptor that still promises the old graph. A scan that began
 * earlier keeps the guarantee it started with; classification happens at every rescan.
 *
 * amvacuumcleanup: after pgvector's own work, a LOST index whose reason a recertification fixes, or a CERTIFIED one whose ledger
 * grew past the threshold, is recertified, inside the vacuum that holds the table's ShareUpdateExclusiveLock. ANALYZE also calls
 * amvacuumcleanup (analyze_only); it does nothing then.
 */
static IndexBulkDeleteResult *(*pgv_ambulkdelete) (IndexVacuumInfo *, IndexBulkDeleteResult *, IndexBulkDeleteCallback, void *) = NULL;
static IndexBulkDeleteResult *(*pgv_amvacuumcleanup) (IndexVacuumInfo *, IndexBulkDeleteResult *) = NULL;

/* CERTIFIED becomes LOST (vacuum); a LOST index keeps its reason. Either way graph_epoch goes up: ambulkdelete can be called
 * several times in one VACUUM and every call may repair the graph again. The transition is idempotent. */
static void
edit_vacuum_lost(CuvsCoverDescriptor *d, void *arg)
{
	/* the classification above was made without the buffer lock: look again */
	if (d->state == CUVS_COVER_STATE_CERTIFIED)
	{
		d->state = CUVS_COVER_STATE_LOST;
		d->reason = CUVS_COVER_REASON_VACUUM;
	}
	d->graph_epoch++;
}

/* ambulkdelete on an index that is not valid yet: remember whether the callback reported any tuple dead */
typedef struct DeadSeen
{
	IndexBulkDeleteCallback callback;
	void	   *state;
	bool		any;
} DeadSeen;

static bool
dead_seen_callback(ItemPointer tid, void *state)
{
	DeadSeen   *ds = (DeadSeen *) state;
	bool		dead = ds->callback(tid, ds->state);

	if (dead)
		ds->any = true;
	return dead;
}

static IndexBulkDeleteResult *
cuvs_cover_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback, void *callback_state)
{
	CuvsCoverDescriptor d;
	CuvsCoverResult res;

	/*
	 * An index that is not valid yet is being built concurrently (CREATE INDEX CONCURRENTLY, REINDEX CONCURRENTLY): the call is
	 * validate_index collecting the tuples of the index, whose callback never reports a tuple as dead, so the graph does not change.
	 * Taking the index off here would leave every concurrently built index LOST until a VACUUM that may never come. Nothing reads an
	 * invalid index, and it becomes valid only after that build. If a callback does report tuples dead (a VACUUM of an invalid
	 * index left by a failed build, or a catalog edit that made a valid index invalid), the graph does change, and the index is taken
	 * off after pgvector has done it (review 32af2a8c R8-1: nobody reads it meanwhile, and the descriptor must not keep promising).
	 */
	if (!info->index->rd_index->indisvalid)
	{
		DeadSeen	ds;
		IndexBulkDeleteResult *r;

		ds.callback = callback;
		ds.state = callback_state;
		ds.any = false;
		r = pgv_ambulkdelete(info, stats, dead_seen_callback, &ds);
		if (ds.any && read_descriptor(info->index, &d, &res) && res.writable)
			edit_descriptor(info->index, edit_vacuum_lost, NULL);
		return r;
	}
	if (read_descriptor(info->index, &d, &res) && res.writable)
	{
		edit_descriptor(info->index, edit_vacuum_lost, NULL);
		cuvs_cover_note_vacuum(RelationGetRelid(info->index));	/* a candidate that remembers this VACUUM */
	}
#ifdef CUVS_COVER_RECERT_TEST_GATE
	(void) cuvs_cover_test_gate("vacuum");
#endif
	return pgv_ambulkdelete(info, stats, callback, callback_state);
}

static IndexBulkDeleteResult *
cuvs_cover_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	Oid			heap_oid;
	Relation	heap;
	CuvsRecertOutcome out;
	uint64		ram, spill;
	int			elevel;

	stats = pgv_amvacuumcleanup(info, stats);
	if (info->analyze_only)
		return stats;
	heap_oid = IndexGetRelation(RelationGetRelid(info->index), true);
	if (!OidIsValid(heap_oid))
		return stats;
	heap = table_open(heap_oid, ShareUpdateExclusiveLock);	/* held by the vacuum already */
	cuvs_cover_reach_default_limits(&ram, &spill);
	cuvs_cover_recertify(heap, info->index, ram, spill, false, &out);
	table_close(heap, NoLock);

	/* nothing to do is routine; a result or a reason to look is worth a line in the server log */
	elevel = (out.status == CUVS_RECERT_NOT_APPLICABLE || out.status == CUVS_RECERT_SKIPPED) ? DEBUG1 : LOG;
	ereport(elevel,
			(errmsg("pg_cuvs: recertification of index \"%s\" by vacuum: %s (%s)", RelationGetRelationName(info->index),
					out.status == CUVS_RECERT_DONE ? "done" : out.status == CUVS_RECERT_NOT_APPLICABLE ? "not applicable" :
					out.status == CUVS_RECERT_DEFERRED ? "deferred" : out.status == CUVS_RECERT_FAILED ? "failed" :
					out.status == CUVS_RECERT_SKIPPED ? "skipped" : "abandoned", out.detail ? out.detail : ""),
			 out.status == CUVS_RECERT_FAILED && out.needed_bytes > 0 ?
			 errhint("The walk needs about %llu bytes; raise autovacuum_work_mem or maintenance_work_mem, or run pg_cuvs_cover_recertify() in a session that has more.",
					 (unsigned long long) out.needed_bytes) : 0));
	return stats;
}

void
cuvs_cover_wrap_vacuum(IndexAmRoutine *amroutine)
{
	pgv_ambulkdelete = amroutine->ambulkdelete;
	pgv_amvacuumcleanup = amroutine->amvacuumcleanup;
	amroutine->ambulkdelete = cuvs_cover_ambulkdelete;
	amroutine->amvacuumcleanup = cuvs_cover_amvacuumcleanup;
}

/*
 * The tail every build of rows shares: the index was just written, so it starts LOST (uncertified) and is
 * certified here, by the same recertification VACUUM and pg_cuvs_cover_recertify() run. A recertification that cannot run
 * (the memory limits, a damaged chain) does not fail the build, which may have taken hours on the GPU: the index stays LOST
 * (uncertified), the first VACUUM or pg_cuvs_cover_recertify() certifies it, and the notice says so.
 */
void
cuvs_cover_certify_after_build(Relation heap, Relation index)
{
	CuvsRecertOutcome out;
	uint64		ram, spill;

	cuvs_cover_reach_default_limits(&ram, &spill);
	cuvs_cover_recertify(heap, index, ram, spill, false, &out);
	if (out.status == CUVS_RECERT_DONE || out.status == CUVS_RECERT_NOT_APPLICABLE)
		return;
	ereport(NOTICE,
			(errmsg("pg_cuvs: index \"%s\" is built but not certified yet: %s", RelationGetRelationName(index), out.detail ? out.detail : ""),
			 errhint("The next VACUUM of the table, or pg_cuvs_cover_recertify('%s'), certifies it; until then scans rank the heap.",
					 RelationGetRelationName(index))));
}
