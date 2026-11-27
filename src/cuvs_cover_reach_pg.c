/* PostgreSQL side of the structural reachability walk: page access, interrupts, cost-based delay and spill memory. */
#include "postgres.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include "miscadmin.h"
#include "access/genam.h"
#include "commands/vacuum.h"
#include "postmaster/autovacuum.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "cuvs_cover_testgate.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#define CUVS_REACH_MALLOC(n) palloc_extended((n), MCXT_ALLOC_HUGE | MCXT_ALLOC_NO_OOM)
#define CUVS_REACH_CALLOC(n, sz) palloc_extended((Size) (n) * (sz), MCXT_ALLOC_HUGE | MCXT_ALLOC_NO_OOM | MCXT_ALLOC_ZERO)
#define CUVS_REACH_FREE(p) do { if ((p) != NULL) pfree(p); } while (0)
#include "cuvs_cover_reach.h"
#include "cuvs_cover_reach_pg.h"

CuvsCoverReachDbwaitHook cuvs_cover_reach_dbwait_hook = NULL;

#define MAX_SPILL_MAPS 8

typedef struct PgReach {
	Relation	rel;
	Relation	yield_heap;	/* not NULL: the walk gives way when another session waits for a lock on this table or on the index (the caller holds both) */
	bool		gated;		/* regression builds: the "walk" gate was passed */
	BufferAccessStrategy strategy;
	CuvsReachEmitFn emit;
	void	   *emit_ctx;
	struct { void *p; size_t bytes; } maps[MAX_SPILL_MAPS];	/* spill mappings, released on error too */
} PgReach;

static uint32_t
pg_reach_nblocks(void *c)
{
#ifdef CUVS_COVER_RECERT_TEST_GATE
	/* the first thing a walk does: inserts that a test lets through here are in the graph the walk reads, whole */
	if (!((PgReach *) c)->gated)
	{
		((PgReach *) c)->gated = true;
		(void) cuvs_cover_test_gate("walk");
	}
#endif
	return (uint32_t) RelationGetNumberOfBlocks(((PgReach *) c)->rel);
}

/*
 * Does another session wait for a lock that conflicts with the table lock (ShareUpdateExclusive) the caller holds? Only the table's lock is
 * asked about: LockHasWaiters reads the shared lock table, which has no entry for a lock taken on the fast path (the RowExclusive lock on the
 * index is one), and a statement that needs more than the caller's lock on the index (DROP INDEX, REINDEX, ALTER INDEX) takes a lock on the
 * table that conflicts with this one first.
 */
static bool
pg_reach_others_wait(Relation heap)
{
	return CheckRelationLockedByMe(heap, ShareUpdateExclusiveLock, false) && LockHasWaitersRelation(heap, ShareUpdateExclusiveLock);
}

static int
pg_reach_read(void *c, uint32_t blk, uint8_t *dst)
{
	PgReach    *r = (PgReach *) c;
	Buffer		buf = ReadBufferExtended(r->rel, MAIN_FORKNUM, blk, RBM_NORMAL, r->strategy);

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	memcpy(dst, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	return 0;
}

static int
pg_reach_delay(void *c)
{
	PgReach    *r = c;

	CHECK_FOR_INTERRUPTS();
	vacuum_delay_point();
	/* A statement that needs more than we hold (ALTER TABLE, DROP INDEX, TRUNCATE) queues behind this walk, and every reader and writer that
	 * arrives after it queues behind that statement: stop, the walk is only repeated later. */
	if (r->yield_heap != NULL && pg_reach_others_wait(r->yield_heap))
		return 1;
	/* a background worker steps aside for DROP DATABASE and the like on its database; every 256 pages, looked up at most every 100 ms */
	if (cuvs_cover_reach_dbwait_hook != NULL && cuvs_cover_reach_dbwait_hook())
		return 1;
	return 0;
}

static int
pg_reach_emit(void *c, uint32_t blk, uint16_t off)
{
	PgReach    *p = (PgReach *) c;

	return p->emit(p->emit_ctx, blk, off);
}

/*
 * The directory spill files go to, and what the volume under it can still give: the walk never reserves more than half of that, so
 * that WAL, other temp files and other backends keep room. An unlinked file is not counted by anything else, and a full volume
 * is a stop for the whole server.
 */
static bool
spill_dir(char *dir, size_t len)
{
	if (DataDir)
	{
		if (snprintf(dir, len, "%s/base/pgsql_tmp", DataDir) >= (int) len)
			return false;
		if (mkdir(dir, 0700) && errno != EEXIST)
			return false;
	}
	else
		snprintf(dir, len, "/tmp");
	return true;
}

static uint64
spill_available(void)
{
	char		dir[MAXPGPATH];
	struct statvfs sv;

	if (!spill_dir(dir, sizeof(dir)) || statvfs(dir, &sv) != 0)
		return 0;
	return ((uint64) sv.f_bavail * (uint64) sv.f_frsize) / 2;
}

/* The reservation is made in steps, each one short enough to restart when a signal interrupts it, and the backend's own
 * interrupts (a cancel, a timeout) are served between the steps: an index of hundreds of gigabytes of bitmaps takes long. */
#define SPILL_STEP ((size_t) 32 * 1024 * 1024)

/*
 * An unlinked, preallocated temp file mapped into memory: spill memory for the big arrays. The same method as the
 * build-time connectivity workspace (hnsw_export.c). It is not charged to PostgreSQL's temp file accounting, so it
 * is never used when temp_file_limit is finite (the caller passes spill_limit 0 then). Space is reserved before the
 * mapping is touched, so a full disk is an allocation failure here and not a SIGBUS later. Zero filled.
 */
static void *
spill_map(size_t bytes)
{
	char		path[MAXPGPATH], dir[MAXPGPATH];
	volatile int fd;
	volatile int rc = 0;
	void	   *p;

	if (temp_file_limit >= 0)
		return NULL;
	if (bytes > spill_available() || !spill_dir(dir, sizeof(dir)))
		return NULL;
	if (snprintf(path, sizeof(path), "%s/cover-reach-XXXXXX", dir) >= (int) sizeof(path))
		return NULL;
	fd = mkstemp(path);
	if (fd < 0)
		return NULL;
	unlink(path);
	PG_TRY();
	{
		size_t		done;

		for (done = 0; done < bytes && rc == 0; done += SPILL_STEP)
		{
			size_t		len = bytes - done < SPILL_STEP ? bytes - done : SPILL_STEP;

			do
				rc = posix_fallocate(fd, (off_t) done, (off_t) len);
			while (rc == EINTR);
			CHECK_FOR_INTERRUPTS();
		}
	}
	PG_CATCH();
	{
		close(fd);
		PG_RE_THROW();
	}
	PG_END_TRY();
	if (rc != 0)
	{
		close(fd);
		return NULL;
	}
	p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	return p == MAP_FAILED ? NULL : p;
}

static void *
pg_reach_big_alloc(void *c, size_t bytes, int spill)
{
	PgReach    *r = (PgReach *) c;
	int			i;
	void	   *p;

	if (!spill)
		return palloc_extended(bytes, MCXT_ALLOC_HUGE | MCXT_ALLOC_NO_OOM | MCXT_ALLOC_ZERO);
	for (i = 0; i < MAX_SPILL_MAPS && r->maps[i].p != NULL; i++)
		;
	if (i == MAX_SPILL_MAPS)
		return NULL;
	p = spill_map(bytes);
	if (p != NULL)
	{
		r->maps[i].p = p;
		r->maps[i].bytes = bytes;
	}
	return p;
}

static void
pg_reach_big_free(void *c, void *p, size_t bytes, int spill)
{
	PgReach    *r = (PgReach *) c;
	int			i;

	if (!spill)
	{
		pfree(p);
		return;
	}
	for (i = 0; i < MAX_SPILL_MAPS; i++)
	{
		if (r->maps[i].p == p)
		{
			munmap(p, bytes);
			r->maps[i].p = NULL;
			return;
		}
	}
}

void
cuvs_cover_reach_default_limits(uint64 *ram_limit, uint64 *spill_limit)
{
	int			kb = maintenance_work_mem;

	if (IsAutoVacuumWorkerProcess() && autovacuum_work_mem != -1)
		kb = autovacuum_work_mem;
	*ram_limit = (uint64) kb * 1024u;
#ifdef CUVS_COVER_RECERT_TEST_GATE
	/* Regression builds only: a RAM limit in bytes (and no spill) that a test can make smaller than any real setting allows */
	{
		const char *forced = getenv("CUVS_RECERT_GATE_RAM_LIMIT");

		if (forced != NULL)
		{
			*ram_limit = (uint64) strtoull(forced, NULL, 10);
			*spill_limit = 0;
			return;
		}
	}
#endif
	*spill_limit = temp_file_limit >= 0 ? 0 : Min((uint64) 1 << 46, spill_available());
	/* Rounded down to a power of two: the limit is remembered with a failed recertification and compared with the next attempt's,
	 * and the free space of a volume moves with every write; only a halving or doubling of it is worth another attempt. */
	if (*spill_limit > 0)
		*spill_limit = (uint64) 1 << (63 - __builtin_clzll(*spill_limit));
}

void
cuvs_cover_reach_index(Relation index, uint64 ram_limit, uint64 spill_limit, CuvsReachEmitFn emit, void *emit_ctx,
		       CuvsReachCounts *out)
{
	cuvs_cover_reach_index_yielding(index, NULL, ram_limit, spill_limit, emit, emit_ctx, out);
}

void
cuvs_cover_reach_index_yielding(Relation index, Relation yield_heap, uint64 ram_limit, uint64 spill_limit, CuvsReachEmitFn emit, void *emit_ctx,
				CuvsReachCounts *out)
{
	PgReach		ctx;
	CuvsReachSource src;
	CuvsReachResult r;
	int			i;

	StaticAssertStmt(BLCKSZ == CUVS_REACH_PAGE_SIZE, "reachability walk assumes 8 kB pages");
	memset(&ctx, 0, sizeof(ctx));
	ctx.rel = index;
	ctx.yield_heap = yield_heap;
	ctx.strategy = GetAccessStrategy(BAS_BULKREAD);
	ctx.emit = emit;
	ctx.emit_ctx = emit_ctx;
	memset(&src, 0, sizeof(src));
	src.ctx = &ctx;
	src.nblocks = pg_reach_nblocks;
	src.read_block = pg_reach_read;
	src.delay = pg_reach_delay;
	src.emit = emit != NULL ? pg_reach_emit : NULL;
	src.big_alloc = pg_reach_big_alloc;
	src.big_free = pg_reach_big_free;

	PG_TRY();
	{
		cuvs_reach_run(&src, ram_limit, spill_limit, &r);
	}
	PG_CATCH();
	{
		/* an error (query cancel from the delay hook, a read error) must not leave mapped temp files behind */
		for (i = 0; i < MAX_SPILL_MAPS; i++)
			if (ctx.maps[i].p != NULL)
				munmap(ctx.maps[i].p, ctx.maps[i].bytes);
		FreeAccessStrategy(ctx.strategy);
		PG_RE_THROW();
	}
	PG_END_TRY();
	FreeAccessStrategy(ctx.strategy);
	out->status = (int) r.status;
	out->nblocks = r.nblocks;
	out->elements = r.elements;
	out->live = r.live;
	out->reached = r.reached;
	out->unreached = r.unreached;
	out->emitted = r.emitted;
	out->mem_bytes = r.mem_bytes;
	out->spilled_bytes = r.spilled_bytes;
	out->needed_bytes = r.needed_bytes;
}
