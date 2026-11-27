/* cuvs_cover_worker.c - background recertification of a cover index whose ledger has grown past the threshold. */
#include "postgres.h"
#include "access/genam.h"
#include "access/xact.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/indexing.h"
#include "postmaster/interrupt.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "utils/fmgroids.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_opclass.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"
#include "access/table.h"
#include "libpq/pqsignal.h"
#include "tcop/tcopprot.h"
#include "access/htup_details.h"

#include "cuvs_cover_format.h"
#include "cuvs_cover_insert.h"
#include "cuvs_cover_recert.h"
#include "cuvs_cover_reach_pg.h"
#include "cuvs_cover_worker.h"
#include "cuvs_cover_testgate.h"

bool		cuvs_cover_auto_recertify = true;
int			cuvs_cover_recert_percent = CUVS_RECERT_GROWTH_PERCENT;
int			cuvs_cover_recert_min_interval = 10;
int			cuvs_cover_recert_max_workers = 2;
int			cuvs_cover_recert_max_age = 300;

PGDLLEXPORT void cuvs_cover_recert_worker_main(Datum main_arg);
PGDLLEXPORT void cuvs_cover_sweeper_main(Datum main_arg);
PGDLLEXPORT void cuvs_cover_sweep_db_main(Datum main_arg);

/*
 * The requests live in shared memory, so that every backend of the server sees one state per index (review 2f6789ed R7-1: a limit that
 * each backend keeps for itself is no limit when connections are short or one session touches many indexes). A slot says, for one
 * (database, index): whether a worker is starting or running, whether a request came in since the worker last looked ("want"), and
 * the time before which no new worker may start (set when a worker ended without having certified anything, so that a request that
 * a worker cannot satisfy does not start one worker after the other). Shared memory exists only when the library is in shared_preload_libraries; without
 * it nothing is requested and VACUUM and pg_cuvs_cover_recertify() remain.
 */
#define REQ_SLOTS 256
#define REQ_STARTING_GRACE_US (30 * USECS_PER_SEC)	/* a worker that has not started in this long is taken as never started */
#define REQ_RETRY_US (5 * USECS_PER_SEC)			/* after a worker could not even be registered (no free slot) */
#define WORKER_LINGER_MS 1000						/* a worker waits this long for another request before it ends */
#define WORKER_PACE_MS 50							/* least time between two looks of one worker */

typedef enum { REQ_IDLE = 0, REQ_STARTING, REQ_RUNNING } ReqState;

typedef struct ReqSlot
{
	Oid			db;				/* InvalidOid: free */
	Oid			index;
	uint8		state;
	bool		want;			/* a request came in after the worker last looked */
	bool		aged;			/* the request is a time-based one: recertify though the ledger is under the threshold */
	TimestampTz stamp;			/* when the worker was asked for (STARTING) */
	TimestampTz not_before;		/* no new worker before this */
	uint32		err_nblocks;	/* the index size when its last worker ended with an error (ERR_SIZE_UNKNOWN: before it read it); 0: none */
} ReqSlot;

#define ERR_SIZE_UNKNOWN UINT32_MAX

/*
 * The candidates of the sweeper: (database, index) pairs that may need a recertification. An index is written here by its first insert after
 * a certification (the one that marks the descriptor, cover_mark_inserted: once per certification, not once per page or insert) and when its
 * guarantee is taken off (a VACUUM, the growth monitor's unkeyed writer). A sweep keeps a candidate while anything was added since its
 * certification (a key, a page or the mark), so a certified index with something to recertify is in the table or, after a restart or an
 * overflow, found by the full sweep through its mark, keys or pages. The sweeper visits only the databases that have a candidate, and there only the
 * candidates: a database that does not use pg_cuvs costs nothing. A full sweep (every connectable database, every index of the access
 * method) runs once after the server starts (the table is empty then, and rows cut off before the restart are still outside the ledger)
 * and once after the table overflowed.
 */
#define CAND_SLOTS 1024

typedef struct CandSlot
{
	Oid			db;				/* InvalidOid: free */
	Oid			index;
	uint32		seq;			/* goes up at every note; a sweep takes the entry off only if nothing noted it since the sweep looked */
	int			vac_pid;		/* the VACUUM that took the guarantee off (its leader in a parallel VACUUM); 0: none recorded */
	BackendId	vac_backend;	/* with vac_lxid its virtual transaction id (the search merges only while that transaction runs) */
	LocalTransactionId vac_lxid;
} CandSlot;

typedef struct ReqShmem
{
	slock_t		mutex;
	ReqSlot		slots[REQ_SLOTS];
	bool		full_sweep;		/* the next sweep looks at everything */
	CandSlot	cand[CAND_SLOTS];
} ReqShmem;

/* The slots in use. Regression builds can make the table smaller (CUVS_RECERT_GATE_SLOTS) to test what happens when it is full. */
static int
req_nslots(void)
{
#ifdef CUVS_COVER_RECERT_TEST_GATE
	static int	n = 0;

	if (n == 0)
	{
		const char *e = getenv("CUVS_RECERT_GATE_SLOTS");

		n = e != NULL ? Max(1, Min(REQ_SLOTS, atoi(e))) : REQ_SLOTS;
	}
	return n;
#else
	return REQ_SLOTS;
#endif
}

static ReqShmem *g_req = NULL;
static shmem_request_hook_type prev_req_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_req_shmem_startup_hook = NULL;

static void
req_shmem_request(void)
{
	if (prev_req_shmem_request_hook)
		prev_req_shmem_request_hook();
	RequestAddinShmemSpace(sizeof(ReqShmem));
}

static void
req_shmem_startup(void)
{
	bool		found;

	if (prev_req_shmem_startup_hook)
		prev_req_shmem_startup_hook();
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	g_req = ShmemInitStruct("pg_cuvs cover recertification requests", sizeof(ReqShmem), &found);
	if (!found)
	{
		SpinLockInit(&g_req->mutex);
		memset(g_req->slots, 0, sizeof(g_req->slots));
		memset(g_req->cand, 0, sizeof(g_req->cand));
		g_req->full_sweep = true;
	}
	LWLockRelease(AddinShmemInitLock);
}

void
cuvs_cover_worker_init(void)
{
	cuvs_cover_reach_dbwait_hook = cuvs_cover_db_command_waits;
	DefineCustomBoolVariable("cuvs.cover_auto_recertify",
							 "Recertify a pg_cuvs_hnsw index in the background (through the sweeper) when it is past the threshold, past cuvs.cover_recertify_max_age, or lost (needs pg_cuvs in shared_preload_libraries).",
							 NULL, &cuvs_cover_auto_recertify, true, PGC_SIGHUP, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("cuvs.cover_recertify_threshold_percent",
							"Keys written (as a percent of the rows the last certification covered) or pages added (as a percent of the pages it had) since the last certification, beyond which VACUUM and the sweeper recertify and the status function recommends it.",
							NULL, &cuvs_cover_recert_percent, CUVS_RECERT_GROWTH_PERCENT, 1, 100, PGC_SIGHUP, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("cuvs.cover_recertify_min_interval",
							"Least number of seconds between the end of a background recertification of an index and the start of the next one for it.",
							NULL, &cuvs_cover_recert_min_interval, 10, 1, 86400, PGC_SIGHUP, GUC_UNIT_S, NULL, NULL, NULL);
	DefineCustomIntVariable("cuvs.cover_recertify_max_workers",
							"Most background recertification workers that run at once in the whole server (one index never has two).",
							NULL, &cuvs_cover_recert_max_workers, 2, 1, 16, PGC_SIGHUP, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("cuvs.cover_recertify_max_age",
							"A certified pg_cuvs_hnsw index with at least one key written or one page added since its certification is recertified in the background once the certification began this long ago, "
							"however few; a tenth of it is the sweep period. 0 turns this time rule off (the sweep, every 30 s then, still takes an index past the threshold or lost).",
							NULL, &cuvs_cover_recert_max_age, 300, 0, 604800, PGC_SIGHUP, GUC_UNIT_S, NULL, NULL, NULL);
	if (process_shared_preload_libraries_in_progress)
	{
		BackgroundWorker sweeper;

		memset(&sweeper, 0, sizeof(sweeper));
		snprintf(sweeper.bgw_name, BGW_MAXLEN, "pg_cuvs cover sweeper");
		snprintf(sweeper.bgw_type, BGW_MAXLEN, "pg_cuvs cover sweeper");
		sweeper.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
		sweeper.bgw_start_time = BgWorkerStart_RecoveryFinished;
		sweeper.bgw_restart_time = 10;
		snprintf(sweeper.bgw_library_name, BGW_MAXLEN, "pg_cuvs");
		snprintf(sweeper.bgw_function_name, BGW_MAXLEN, "cuvs_cover_sweeper_main");
		RegisterBackgroundWorker(&sweeper);
		prev_req_shmem_request_hook = shmem_request_hook;
		shmem_request_hook = req_shmem_request;
		prev_req_shmem_startup_hook = shmem_startup_hook;
		shmem_startup_hook = req_shmem_startup;
	}
}

/* True when a request can be made at all: the setting is on and the library holds shared memory. */
bool
cuvs_cover_auto_recertify_available(void)
{
	return cuvs_cover_auto_recertify && g_req != NULL;
}

/* The slot of (db, index), or a free one / the idle one that was used longest ago, or NULL. Called with the mutex held. */
static ReqSlot *
req_find(Oid db, Oid index, bool create)
{
	ReqSlot    *free_slot = NULL,
			   *oldest = NULL;
	int			i;

	for (i = 0; i < req_nslots(); i++)
	{
		ReqSlot    *sl = &g_req->slots[i];

		if (sl->db == db && sl->index == index)
			return sl;
		if (sl->db == InvalidOid)
		{
			if (free_slot == NULL)
				free_slot = sl;
		}
		else if (sl->state == REQ_IDLE && !sl->want && (oldest == NULL || sl->not_before < oldest->not_before))
			oldest = sl;
	}
	if (!create)
		return NULL;
	if (free_slot == NULL)
		free_slot = oldest;
	if (free_slot != NULL)
	{
		memset(free_slot, 0, sizeof(*free_slot));
		free_slot->db = db;
		free_slot->index = index;
	}
	return free_slot;
}

/* Workers running or being started right now, over the whole server. Called with the mutex held. */
static int
req_active(TimestampTz now)
{
	int			n = 0;
	int			i;

	for (i = 0; i < req_nslots(); i++)
	{
		ReqSlot    *sl = &g_req->slots[i];

		if (sl->db != InvalidOid && (sl->state == REQ_RUNNING || (sl->state == REQ_STARTING && now - sl->stamp < REQ_STARTING_GRACE_US)))
			n++;
	}
	return n;
}

static Oid	worker_db_for_next = InvalidOid;
static Oid	worker_index_for_next = InvalidOid;

/* Register the worker of (db, index): the slot is already STARTING. On failure (no free worker slot) the slot is idle again and tried after a pause. */
static bool
req_register(Oid db, Oid index_oid)
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;

	memset(&worker, 0, sizeof(worker));
	snprintf(worker.bgw_name, BGW_MAXLEN, "pg_cuvs recertification of index %u", index_oid);
	snprintf(worker.bgw_type, BGW_MAXLEN, "pg_cuvs recertification");
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(worker.bgw_library_name, BGW_MAXLEN, "pg_cuvs");
	snprintf(worker.bgw_function_name, BGW_MAXLEN, "cuvs_cover_recert_worker_main");
	worker.bgw_main_arg = ObjectIdGetDatum(index_oid);
	memcpy(worker.bgw_extra, &db, sizeof(Oid));
	worker.bgw_notify_pid = 0;
	if (RegisterDynamicBackgroundWorker(&worker, &handle))
		return true;
	{
		ReqSlot    *sl;

		TimestampTz now = GetCurrentTimestamp();	/* the clock is read before the spinlock, not under it */

		SpinLockAcquire(&g_req->mutex);
		sl = req_find(db, index_oid, false);
		if (sl != NULL)
		{
			sl->state = REQ_IDLE;
			sl->not_before = now + REQ_RETRY_US;
		}
		SpinLockRelease(&g_req->mutex);
	}
	return false;
}

/*
 * A request that found every worker place taken stays wanted, and nobody would wake it. A worker that ends starts one worker for an index
 * whose request is waiting (any database), so that a load that ends does not leave an index past the threshold only because other indexes were
 * served first.
 */
static void
worker_start_next(void)
{
	Oid			db = InvalidOid,
				idx = InvalidOid;
	TimestampTz now = GetCurrentTimestamp();
	int			i;

	if (g_req == NULL || !cuvs_cover_auto_recertify)
		return;
	SpinLockAcquire(&g_req->mutex);
	if (req_active(now) < cuvs_cover_recert_max_workers)
	{
		for (i = 0; i < req_nslots(); i++)
		{
			ReqSlot    *sl = &g_req->slots[i];

			if (sl->db != InvalidOid && sl->state == REQ_IDLE && sl->want && now >= sl->not_before &&
				!(sl->db == worker_db_for_next && sl->index == worker_index_for_next))
			{
				sl->state = REQ_STARTING;
				sl->stamp = now;
				db = sl->db;
				idx = sl->index;
				break;
			}
		}
	}
	SpinLockRelease(&g_req->mutex);
	if (OidIsValid(idx))
		(void) req_register(db, idx);
}

/*
 * A request for (db, index): the flag is set whatever happens, and a worker is started when none runs for the index, its interval is over and the
 * server runs fewer than cuvs.cover_recertify_max_workers. True if a worker was started.
 */
static bool
req_request(Oid db, Oid index_oid, bool aged, BlockNumber nblocks)
{
	TimestampTz now = GetCurrentTimestamp();
	ReqSlot    *sl;

	SpinLockAcquire(&g_req->mutex);
	sl = req_find(db, index_oid, true);
	if (sl == NULL)
	{
		static TimestampTz last_logged = 0;

		SpinLockRelease(&g_req->mutex);
		/* every slot is wanted or in use: the request is dropped, and made again by the next insert past the threshold or the next sweep */
		if (now - last_logged > 60 * USECS_PER_SEC)
		{
			last_logged = now;
			ereport(LOG, (errmsg("pg_cuvs: the request table for background recertifications is full (%d indexes wait or run); the request for index %u is made again by a later insert or sweep",
								 req_nslots(), index_oid)));
		}
		return false;
	}
	sl->want = true;
	if (aged)
		sl->aged = true;
	/* the index grew since its last worker ended with an error: what failed may not fail again, so the wait for it ends */
	if (sl->err_nblocks != 0 && sl->err_nblocks != ERR_SIZE_UNKNOWN && nblocks != sl->err_nblocks && sl->state == REQ_IDLE)
	{
		sl->err_nblocks = 0;
		sl->not_before = 0;
	}
	if (sl->state == REQ_RUNNING || (sl->state == REQ_STARTING && now - sl->stamp < REQ_STARTING_GRACE_US) ||
		now < sl->not_before || req_active(now) >= cuvs_cover_recert_max_workers)
	{
		SpinLockRelease(&g_req->mutex);
		return false;
	}
	sl->state = REQ_STARTING;
	sl->stamp = now;
	SpinLockRelease(&g_req->mutex);
	return req_register(db, index_oid);
}

void
cuvs_cover_note_candidate(Oid index_oid)
{
	int			i, free_at = -1;
	bool		present = false;

	if (g_req == NULL || !OidIsValid(MyDatabaseId))
		return;
	SpinLockAcquire(&g_req->mutex);
	for (i = 0; i < CAND_SLOTS; i++)
	{
		CandSlot   *c = &g_req->cand[i];

		if (c->db == MyDatabaseId && c->index == index_oid)
		{
			present = true;
			c->seq++;			/* a sweep that judged the index before this does not take it off */
			break;
		}
		if (c->db == InvalidOid && free_at < 0)
			free_at = i;
	}
	if (!present)
	{
		if (free_at >= 0)
		{
			g_req->cand[free_at].db = MyDatabaseId;
			g_req->cand[free_at].index = index_oid;
			g_req->cand[free_at].seq = 1;
			g_req->cand[free_at].vac_pid = 0;
		}
		else
			g_req->full_sweep = true;	/* the table is full: the next sweep looks at everything */
	}
	SpinLockRelease(&g_req->mutex);
}

/*
 * while a VACUUM really runs on an index whose guarantee it took off, a search merges the graph's answers
 * and the ledger; before and after, a LOST index is answered exactly from the heap. ambulkdelete records which VACUUM it is: the process and
 * the local transaction id of the leader (in a parallel VACUUM the index can be vacuumed by a worker that ends before the VACUUM does).
 */
void
cuvs_cover_note_vacuum(Oid index_oid)
{
	PGPROC	   *leader = MyProc->lockGroupLeader != NULL ? MyProc->lockGroupLeader : MyProc;
	int			pid = leader->pid;
	BackendId	backend = leader->backendId;
	LocalTransactionId lxid = leader->lxid;	/* the leader's transaction: it runs as long as this VACUUM does */
	int			i, at = -1, free_at = -1;

	if (g_req == NULL || !OidIsValid(MyDatabaseId))
		return;
	SpinLockAcquire(&g_req->mutex);
	for (i = 0; i < CAND_SLOTS; i++)
	{
		CandSlot   *c = &g_req->cand[i];

		if (c->db == MyDatabaseId && c->index == index_oid)
		{
			at = i;
			break;
		}
		if (c->db == InvalidOid && free_at < 0)
			free_at = i;
	}
	if (at < 0 && free_at >= 0)
	{
		at = free_at;
		g_req->cand[at].db = MyDatabaseId;
		g_req->cand[at].index = index_oid;
		g_req->cand[at].seq = 0;
	}
	if (at >= 0)
	{
		g_req->cand[at].seq++;
		g_req->cand[at].vac_pid = pid;
		g_req->cand[at].vac_backend = backend;
		g_req->cand[at].vac_lxid = lxid;
	}
	else
		g_req->full_sweep = true;	/* no record: the search answers from the heap (no merge without knowing the VACUUM runs) */
	SpinLockRelease(&g_req->mutex);
	elog(DEBUG1, "pg_cuvs: index %u taken off by a VACUUM (process %d, VACUUM leader %d)%s", index_oid, MyProcPid, pid,
		 at >= 0 ? "" : ": the candidate table is full, nothing recorded");
}

/*
 * The record is copied under the spinlock; the process is then looked up through the proc array (BackendPidGetProc takes ProcArrayLock), and
 * its backendId and lxid are read without a lock: 4-byte aligned fields that only that process changes, so each read gives the value before or
 * after a change, never a mix, and either one is a right answer for that instant. A pid that the VACUUM's process gave up and another process
 * took again is told apart by the virtual transaction id (backendId, lxid), which PostgreSQL never hands out twice while the server runs: the
 * local counter of a backend slot is kept for the next process of that slot. The VACUUM ends, is cancelled or crashes: its transaction is not
 * the one running any more (a crash also starts shared memory afresh, which leaves no record).
 */
bool
cuvs_cover_vacuum_running(Oid index_oid)
{
	int			i, pid = 0;
	BackendId	backend = InvalidBackendId;
	LocalTransactionId lxid = InvalidLocalTransactionId;
	PGPROC	   *proc;

	if (g_req == NULL || !OidIsValid(MyDatabaseId))
		return false;
	SpinLockAcquire(&g_req->mutex);
	for (i = 0; i < CAND_SLOTS; i++)
		if (g_req->cand[i].db == MyDatabaseId && g_req->cand[i].index == index_oid)
		{
			pid = g_req->cand[i].vac_pid;
			backend = g_req->cand[i].vac_backend;
			lxid = g_req->cand[i].vac_lxid;
			break;
		}
	SpinLockRelease(&g_req->mutex);
	if (pid == 0 || lxid == InvalidLocalTransactionId)
		return false;
	proc = BackendPidGetProc(pid);
	return proc != NULL && proc->backendId == backend && proc->lxid == lxid;
}

/*
 * DROP DATABASE, ALTER DATABASE ... SET TABLESPACE and CREATE DATABASE ... TEMPLATE wait up to 5 s for the other sessions
 * of the database to end and fail if any stays; PostgreSQL ends only autovacuum workers then. While they count, they hold a lock on the
 * database object (AccessExclusiveLock, or ShareLock on the template), which conflicts with RowExclusiveLock, the mode a session takes on its
 * database to connect: a worker that finds another backend holding such a lock leaves the walk and ends, and the command succeeds. The lock
 * table is asked (GetLockConflicts on a shared-object lock: no fast-path scan), not the query texts. Another command that holds such a lock
 * (ALTER DATABASE ... RENAME) makes the worker step aside as well: the recertification is made again at a later sweep.
 */
bool		cuvs_cover_stepped_aside = false;

bool
cuvs_cover_db_command_waits(void)
{
	static TimestampTz last = 0;
	static bool answer = false;
	TimestampTz now;
	LOCKTAG		tag;
	VirtualTransactionId *v;
	int			n = 0;

	if (!IsBackgroundWorker || !OidIsValid(MyDatabaseId))
		return false;
	now = GetCurrentTimestamp();
	if (answer || now - last < 100 * 1000)
		return answer;
	last = now;
	SET_LOCKTAG_OBJECT(tag, InvalidOid, DatabaseRelationId, MyDatabaseId, 0);
	v = GetLockConflicts(&tag, RowExclusiveLock, &n);
	if (v != NULL)
		pfree(v);
	answer = n > 0;
	if (answer)
		cuvs_cover_stepped_aside = true;
	return answer;
}

#ifdef CUVS_COVER_RECERT_TEST_GATE
/* a worker held at a test gate stands for a long walk: it leaves the gate when it would step aside in the walk (cuvs_cover_testgate.h) */
bool		(*cuvs_cover_test_gate_release) (void) = cuvs_cover_db_command_waits;
#endif

/* Take every candidate of a database off the table (the database is gone or does not accept connections). */
static void
cand_forget_db(Oid db)
{
	int			i;

	SpinLockAcquire(&g_req->mutex);
	for (i = 0; i < CAND_SLOTS; i++)
		if (g_req->cand[i].db == db)
			g_req->cand[i].db = InvalidOid;
	SpinLockRelease(&g_req->mutex);
}

/* The sequence number of a candidate (0 if it is not in the table). */
static uint32
cand_seq(Oid db, Oid index_oid)
{
	int			i;
	uint32		seq = 0;

	SpinLockAcquire(&g_req->mutex);
	for (i = 0; i < CAND_SLOTS; i++)
		if (g_req->cand[i].db == db && g_req->cand[i].index == index_oid)
		{
			seq = g_req->cand[i].seq;
			break;
		}
	SpinLockRelease(&g_req->mutex);
	return seq;
}

/*
 * Take a candidate off the table after a sweep found nothing pending for it, unless an insert noted it after the sweep read `seq`: that
 * insert found the entry present and wrote nothing new, and taking the entry off would lose it until the next growth.
 */
static void
cand_forget_if_unchanged(Oid db, Oid index_oid, uint32 seq)
{
	int			i;

	SpinLockAcquire(&g_req->mutex);
	for (i = 0; i < CAND_SLOTS; i++)
		if (g_req->cand[i].db == db && g_req->cand[i].index == index_oid && g_req->cand[i].seq == seq)
			g_req->cand[i].db = InvalidOid;
	SpinLockRelease(&g_req->mutex);
}

/* Take a candidate off the table (its index is gone, or is not one of this access method). */
static void
cand_forget(Oid db, Oid index_oid)
{
	int			i;

	SpinLockAcquire(&g_req->mutex);
	for (i = 0; i < CAND_SLOTS; i++)
		if (g_req->cand[i].db == db && g_req->cand[i].index == index_oid)
			g_req->cand[i].db = InvalidOid;
	SpinLockRelease(&g_req->mutex);
}

/* The indexes of the access method `am` in this database: through the dependencies on its operator classes (no scan of pg_class). */
static List *
am_indexes(Oid am)
{
	Relation	oprel, deprel;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	tup;
	List	   *opclasses = NIL, *out = NIL;
	ListCell   *lc;

	oprel = table_open(OperatorClassRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_opclass_opcmethod, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(am));
	scan = systable_beginscan(oprel, OpclassAmNameNspIndexId, true, NULL, 1, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		opclasses = lappend_oid(opclasses, ((Form_pg_opclass) GETSTRUCT(tup))->oid);
	systable_endscan(scan);
	table_close(oprel, AccessShareLock);
	deprel = table_open(DependRelationId, AccessShareLock);
	foreach(lc, opclasses)
	{
		ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(OperatorClassRelationId));
		ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(lfirst_oid(lc)));
		scan = systable_beginscan(deprel, DependReferenceIndexId, true, NULL, 2, key);
		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(tup);

			if (dep->classid == RelationRelationId && !list_member_oid(out, dep->objid))
				out = lappend_oid(out, dep->objid);
		}
		systable_endscan(scan);
	}
	table_close(deprel, AccessShareLock);
	list_free(opclasses);
	return out;
}

static volatile sig_atomic_t got_sigterm = false;
static Oid	worker_db = InvalidOid;
static Oid	worker_index = InvalidOid;
static bool worker_owns_slot = true;
static BlockNumber worker_nblocks = 0;	/* the index size at the worker's last look (0: not read yet) */

static void
worker_sigterm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	/* The usual terminate request (die): it is served at the next CHECK_FOR_INTERRUPTS, which the walk makes every few hundred pages, so that
	 * a fast shutdown does not wait for a recertification to end. The flag keeps the loops below from starting new work. */
	got_sigterm = true;
	die(postgres_signal_arg);
	errno = save_errno;
}

/*
 * The slot goes back to IDLE on every way out of the worker, an error included; a request that came in meanwhile stays wanted. A worker that
 * ends with an error (an ERROR is FATAL in a background worker) writes no failure into the descriptor, so the same error would come back at
 * every sweep. Its slot remembers the index size instead and starts no worker for it for cuvs.cover_recertify_max_age (300 s
 * when that is 0; at least cuvs.cover_recertify_min_interval), unless a sweep finds the index grown: the same rule as a failed recertification, which is not tried again
 * at the same relation size, with the time bound kept. A worker that was told to stop (SIGTERM: pg_terminate_backend, DROP DATABASE ... WITH
 * (FORCE), a shutdown) did not fail: it waits the usual cuvs.cover_recertify_min_interval, its request stays, and the log says it was stopped
 * (such a worker was held back for max_age without a word).
 */
static void
worker_release(int code, Datum arg)
{
	ReqSlot    *sl;
	int			wait_s = cuvs_cover_recert_min_interval;
	bool		failed = code != 0 && !got_sigterm;
	bool		logged = false;
	TimestampTz now;

	if (g_req == NULL || !worker_owns_slot)
		return;
	if (failed)
		wait_s = Max(cuvs_cover_recert_min_interval, cuvs_cover_recert_max_age > 0 ? cuvs_cover_recert_max_age : 300);	/* 300: the default, when the time rule is off */
	now = GetCurrentTimestamp();
	SpinLockAcquire(&g_req->mutex);
	sl = req_find(worker_db, worker_index, false);
	if (sl != NULL && sl->state != REQ_IDLE)
	{
		sl->state = REQ_IDLE;
		sl->not_before = now + (int64) wait_s * USECS_PER_SEC;
		if (code != 0)
		{
			sl->want = true;
			if (failed)
				sl->err_nblocks = worker_nblocks != 0 ? worker_nblocks : ERR_SIZE_UNKNOWN;
			logged = true;
		}
	}
	SpinLockRelease(&g_req->mutex);
	if (logged && failed)
		ereport(LOG,
				(errmsg("pg_cuvs: the background recertification of index %u ended with an error; the next one starts in %d s, or at the first sweep that finds the index grown",
						worker_index, wait_s)));
	else if (logged)
		ereport(LOG,
				(errmsg("pg_cuvs: the background recertification of index %u was stopped; a later sweep starts it again (in %d s at the earliest)",
						worker_index, wait_s)));
}

static void
worker_set(bool running, bool want)
{
	ReqSlot    *sl;

	SpinLockAcquire(&g_req->mutex);
	sl = req_find(worker_db, worker_index, true);
	if (sl != NULL)
	{
		if (running)
			sl->state = REQ_RUNNING;
		sl->want = want;
	}
	SpinLockRelease(&g_req->mutex);
}

/* Under the mutex: has a request come in since the last look, and was it a time-based one? Clears both. */
static bool
worker_take_want(bool *aged)
{
	bool		w = false;
	ReqSlot    *sl;

	*aged = false;
	SpinLockAcquire(&g_req->mutex);
	sl = req_find(worker_db, worker_index, false);
	if (sl != NULL)
	{
		w = sl->want;
		*aged = sl->aged;
		sl->want = false;
		sl->aged = false;
	}
	SpinLockRelease(&g_req->mutex);
	return w;
}

/* The table and the index, without queueing behind a statement that conflicts: a request that waits holds everyone else behind it. */
static bool
worker_lock_pair(Oid heap_oid, Oid index_oid)
{
	int			waited = 0;

	while (!ConditionalLockRelationOid(heap_oid, ShareUpdateExclusiveLock) || !ConditionalLockRelationOid(index_oid, RowExclusiveLock))
	{
		/* a half taken pair is released before the next try */
		if (CheckRelationOidLockedByMe(heap_oid, ShareUpdateExclusiveLock, true))
			UnlockRelationOid(heap_oid, ShareUpdateExclusiveLock);
		if (waited++ >= 2000 || got_sigterm || cuvs_cover_db_command_waits())
			return false;
		CHECK_FOR_INTERRUPTS();
		pg_usleep(1000);
	}
	return true;
}

/* Is another index waiting for a worker (a request came in, nobody is on it)? Mutex held. */
static bool
req_other_waiting(void)
{
	int			i;

	for (i = 0; i < req_nslots(); i++)
	{
		ReqSlot    *sl = &g_req->slots[i];

		if (sl->db != InvalidOid && sl->state == REQ_IDLE && sl->want && !(sl->db == worker_db && sl->index == worker_index))
			return true;
	}
	return false;
}

/* One look: lock, recertify when due, unlock, in a transaction of its own (no snapshot or lock is held while the worker waits). */
static CuvsRecertStatus
worker_cycle(Oid index_oid, bool aged, bool *due_again)
{
	Oid			heap_oid;
	Relation	heap, index;
	CuvsRecertOutcome out;
	uint64		ram, spill;
	CuvsRecertStatus status = CUVS_RECERT_NOT_APPLICABLE;

	*due_again = false;
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(index_oid)))
		goto out;				/* dropped meanwhile */
	heap_oid = IndexGetRelation(index_oid, true);
	if (!OidIsValid(heap_oid))
		goto out;
	if (!worker_lock_pair(heap_oid, index_oid))
	{
		status = CUVS_RECERT_DEFERRED;
		ereport(LOG, (errmsg("pg_cuvs: recertification of index \"%s\" by background worker: deferred (a conflicting statement holds the table)", get_rel_name(index_oid))));
		goto out;
	}
	/* a statement that dropped the table or the index committed while this waited: the locks are ours now, look again before opening */
	if (SearchSysCacheExists1(RELOID, ObjectIdGetDatum(index_oid)) && SearchSysCacheExists1(RELOID, ObjectIdGetDatum(heap_oid)))
	{
		heap = table_open(heap_oid, NoLock);
		index = index_open(index_oid, NoLock);
		worker_nblocks = RelationGetNumberOfBlocks(index);
		if (index->rd_rel->relam == get_am_oid("pg_cuvs_hnsw", true))
		{
			cuvs_cover_reach_default_limits(&ram, &spill);
			if (aged)
				cuvs_cover_recertify_aged(heap, index, ram, spill, &out);
			else
				cuvs_cover_recertify_yielding(heap, index, ram, spill, false, &out);
			status = out.status;
			ereport(out.status == CUVS_RECERT_NOT_APPLICABLE ? DEBUG1 : LOG,
					(errmsg("pg_cuvs: recertification of index \"%s\" by background worker: %s (%s)", RelationGetRelationName(index),
							out.status == CUVS_RECERT_DONE ? "done" : out.status == CUVS_RECERT_NOT_APPLICABLE ? "not applicable" :
							out.status == CUVS_RECERT_DEFERRED ? "deferred" : out.status == CUVS_RECERT_FAILED ? "failed" :
							out.status == CUVS_RECERT_SKIPPED ? "skipped" : "abandoned", out.detail ? out.detail : "")));
			/* the inserts that came in during the walk can have put the ledger past the threshold again */
			*due_again = (out.status == CUVS_RECERT_DONE && cuvs_cover_recertify_due(index));
		}
		index_close(index, NoLock);
		table_close(heap, NoLock);
	}
	UnlockRelationOid(index_oid, RowExclusiveLock);
	UnlockRelationOid(heap_oid, ShareUpdateExclusiveLock);
out:
	PopActiveSnapshot();
	CommitTransactionCommand();
	return status;
}

void
cuvs_cover_recert_worker_main(Datum main_arg)
{
	Oid			index_oid = DatumGetObjectId(main_arg);
	Oid			dboid;
	LOCKTAG		tag;
	bool		productive = false;
	bool		left_over = false;
	int			cycles = 0;

	pqsignal(SIGTERM, worker_sigterm);
	BackgroundWorkerUnblockSignals();
	memcpy(&dboid, MyBgworkerEntry->bgw_extra, sizeof(Oid));
	worker_db = dboid;
	worker_index = index_oid;
	worker_db_for_next = dboid;
	worker_index_for_next = index_oid;
	before_shmem_exit(worker_release, (Datum) 0);
	BackgroundWorkerInitializeConnectionByOid(dboid, InvalidOid, 0);

	/* one worker per index: a second one finds the session lock taken and ends (it survives the transaction of each look) */
	SET_LOCKTAG_OBJECT(tag, MyDatabaseId, RelationRelationId, index_oid, CUVS_COVER_LOCKSUBID + 1);
	if (LockAcquire(&tag, ExclusiveLock, true, true) == LOCKACQUIRE_NOT_AVAIL)
	{
		worker_owns_slot = false;	/* the other worker's slot state is not ours to change */
		proc_exit(0);
	}
	worker_set(true, false);

	/*
	 * A look clears the request flag, then recertifies if the ledger is past the threshold. A request that comes in after the flag was
	 * cleared sets it again, and the worker does not end while it is set or while one may still come: after the last look that did
	 * nothing it waits WORKER_LINGER_MS for the flag. So a load that goes on is followed by one worker (no start per insert, no start
	 * per connection), and after a load stops the last look found the ledger within the threshold (review 2f6789ed R7-1, R7-2). Between
	 * looks the worker holds no lock, no snapshot, no transaction: a statement that needs the table gets it. When another index is
	 * waiting for a place, a worker that has looked twice hands its place over.
	 */
	for (;;)
	{
		bool		due_again;
		CuvsRecertStatus status;
		int			waited_ms = 0;
		bool		want = false;

		bool		aged;

		(void) worker_take_want(&aged);
		status = worker_cycle(index_oid, aged, &due_again);
		cycles++;
		if (cuvs_cover_stepped_aside)
		{
			left_over = true;	/* the request stays: a later sweep starts the worker again */
			break;
		}
		if (status == CUVS_RECERT_DONE)
			productive = true;
		else if (status != CUVS_RECERT_NOT_APPLICABLE)
		{
			left_over = true;	/* deferred, failed, skipped, abandoned: the request stays for a later worker */
			break;
		}
		if (got_sigterm)
			break;
		if (due_again)
			continue;
		/* wait for a request that comes in during the linger; the decision to end is taken under the mutex where requests set the flag */
		for (;;)
		{
			TimestampTz now = GetCurrentTimestamp();

			SpinLockAcquire(&g_req->mutex);
			{
				ReqSlot    *sl = req_find(worker_db, worker_index, false);

				want = sl != NULL && sl->want;
				if (want && cycles >= 2 && req_active(now) >= cuvs_cover_recert_max_workers && req_other_waiting())
				{
					/* hand the place over; the request stays wanted */
					sl->state = REQ_IDLE;
					SpinLockRelease(&g_req->mutex);
					goto ended;
				}
				if (!want && waited_ms >= WORKER_LINGER_MS && sl != NULL)
				{
					sl->state = REQ_IDLE;
					SpinLockRelease(&g_req->mutex);
					goto ended;
				}
			}
			SpinLockRelease(&g_req->mutex);
			if (want || got_sigterm || cuvs_cover_db_command_waits())
				break;
			(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, WORKER_PACE_MS, PG_WAIT_EXTENSION);
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
			waited_ms += WORKER_PACE_MS;
		}
		if (got_sigterm || cuvs_cover_stepped_aside)
			break;
		pg_usleep(WORKER_PACE_MS * 1000L);
	}
ended:
	{
		ReqSlot    *sl;
		TimestampTz now = GetCurrentTimestamp();

		/* a worker that certified nothing waits min_interval before the next start for this index; one that did the work does not */
		SpinLockAcquire(&g_req->mutex);
		sl = req_find(worker_db, worker_index, false);
		if (sl != NULL)
		{
			sl->state = REQ_IDLE;
			sl->not_before = productive ? 0 : now + (int64) cuvs_cover_recert_min_interval * USECS_PER_SEC;
			sl->err_nblocks = 0;	/* this worker ended without an error */
			if (left_over || cuvs_cover_stepped_aside)
				sl->want = true;
		}
		SpinLockRelease(&g_req->mutex);
	}
	LockRelease(&tag, ExclusiveLock, true);
	if (cuvs_cover_stepped_aside)
		ereport(LOG,
				(errmsg("pg_cuvs: the background recertification of index %u stepped aside for a command on its database (DROP DATABASE, ALTER DATABASE ... SET TABLESPACE, or a copy of it as a template); a later sweep starts it again",
						index_oid)));
	worker_start_next();
	proc_exit(0);
}

/* ---- the time-based trigger ---- */

/*
 * A certified index that nobody inserts into after its certification is never cut off; one that is inserted into below the threshold has no
 * request from any insert and, without this, would keep the rows those inserts cut off outside the ledger for good. The sweeper is a permanent worker
 * (registered with shared_preload_libraries, restarted by the postmaster, not run on a standby) that every sweep period asks, for each database
 * that has a candidate (the candidate table above; every connectable database after the start and after an overflow), a short-lived worker of
 * that database to look at its candidates. An index is due when the guarantee was withdrawn (lost), when the keys or the pages added since its
 * certification are past the threshold, or when at least one key or page was added and the certification began cuvs.cover_recertify_max_age
 * ago. A due index gets a request, which goes through the same slots, the same server-wide limit of workers and the same hand-over between
 * workers; no insert requests a worker in this version. An index with nothing added since its certification is never walked.
 */
static long
sweep_period_ms(void)
{
	long		ms = (long) cuvs_cover_recert_max_age * 1000 / 10;

	/* cuvs.cover_recertify_max_age = 0 turns the time rule off, not the sweeper: the threshold and lost indexes are still swept */
	if (cuvs_cover_recert_max_age == 0)
		return 30000L;
	return Max(10000L, Min(ms, 300000L));
}

void
cuvs_cover_sweep_db_main(Datum main_arg)
{
	Oid			dboid = DatumGetObjectId(main_arg);
	bool		full;
	List	   *indexes = NIL;
	ListCell   *lc;
	Oid			am;
	TimestampTz now;
	int64		max_age_us = (int64) cuvs_cover_recert_max_age * USECS_PER_SEC;
	int			i;

	memcpy(&full, MyBgworkerEntry->bgw_extra, sizeof(bool));
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnectionByOid(dboid, InvalidOid, 0);
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	am = get_am_oid("pg_cuvs_hnsw", true);
	if (OidIsValid(am))
	{
		if (full)
			indexes = am_indexes(am);
		else
		{
			SpinLockAcquire(&g_req->mutex);
			for (i = 0; i < CAND_SLOTS; i++)
				if (g_req->cand[i].db == dboid)
					indexes = lappend_oid(indexes, g_req->cand[i].index);
			SpinLockRelease(&g_req->mutex);
		}
	}
	now = GetCurrentTimestamp();
	foreach(lc, indexes)
	{
		Oid			ioid = lfirst_oid(lc);
		Relation	index;
		bool		due, pending;
		BlockNumber nblocks;
		uint32		seq;

		if (cuvs_cover_db_command_waits())
			break;				/* a command on this database waits for its sessions to go: the rest of the candidates stay for the next sweep */
		if (!ConditionalLockRelationOid(ioid, AccessShareLock))
			continue;			/* busy (DDL): stays a candidate, looked at next time */
		if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(ioid)))
		{
			UnlockRelationOid(ioid, AccessShareLock);
			cand_forget(dboid, ioid);
			continue;
		}
		index = index_open(ioid, NoLock);
		if (index->rd_rel->relkind != RELKIND_INDEX || index->rd_rel->relam != am || index->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		{
			index_close(index, AccessShareLock);
			cand_forget(dboid, ioid);
			continue;
		}
		seq = cand_seq(dboid, ioid);	/* read before the judgment: a note after it keeps the entry */
		due = index->rd_index->indisvalid && cuvs_cover_sweep_due(index, now, max_age_us);
		pending = cuvs_cover_sweep_pending(index);
		nblocks = RelationGetNumberOfBlocks(index);
		index_close(index, AccessShareLock);
#ifdef CUVS_COVER_RECERT_TEST_GATE
		(void) cuvs_cover_test_gate("sweep");	/* between the judgment and what the sweep does with it */
#endif
		if (due)
			(void) req_request(dboid, ioid, true, nblocks);
		/* an index with something since its certification stays a candidate (for the age, or until its recertification is published);
		 * one with nothing since leaves the table, and its next growth writes it there again */
		if (pending)
			cuvs_cover_note_candidate(ioid);
		else
			cand_forget_if_unchanged(dboid, ioid, seq);
	}
	PopActiveSnapshot();
	CommitTransactionCommand();
	proc_exit(0);
}

static void
sweep_all(void)
{
	List	   *dbs = NIL;
	ListCell   *lc;
	bool		full;
	int			i;

	SpinLockAcquire(&g_req->mutex);
	full = g_req->full_sweep;
	g_req->full_sweep = false;
	if (!full)
		for (i = 0; i < CAND_SLOTS; i++)
			if (g_req->cand[i].db != InvalidOid && !list_member_oid(dbs, g_req->cand[i].db))
				dbs = lappend_oid(dbs, g_req->cand[i].db);
	SpinLockRelease(&g_req->mutex);
	if (!full && dbs != NIL)
	{
		/*
		 * A database of a candidate may have been dropped, or may no longer accept connections, since its candidate was written: a worker of it
		 * would end at the connection, at every sweep, and its candidates would never leave the table. Such a database
		 * loses its candidates here. Only a sweep that has candidates reads pg_database; an idle one still opens no transaction.
		 */
		MemoryContext keep = CurrentMemoryContext;	/* the list outlives the transaction that reads pg_database */
		List	   *usable = NIL;

		StartTransactionCommand();
		foreach(lc, dbs)
		{
			Oid			db = lfirst_oid(lc);
			HeapTuple	tup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(db));
			bool		ok = false;

			if (HeapTupleIsValid(tup))
			{
				Form_pg_database d = (Form_pg_database) GETSTRUCT(tup);

				ok = d->datallowconn && !d->datistemplate;
#ifdef DATCONNLIMIT_INVALID_DB
				ok = ok && d->datconnlimit != DATCONNLIMIT_INVALID_DB;	/* a DROP DATABASE that was interrupted (16.2 and later) */
#endif
				ReleaseSysCache(tup);
			}
			if (ok)
			{
				MemoryContext txn = MemoryContextSwitchTo(keep);

				usable = lappend_oid(usable, db);
				MemoryContextSwitchTo(txn);
			}
			else
				cand_forget_db(db);
		}
		CommitTransactionCommand();
		list_free(dbs);
		dbs = usable;
	}
	if (full)
	{
		MemoryContext keep = CurrentMemoryContext;	/* the list outlives the transaction that reads pg_database */

		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());
		{
			Relation	rel = table_open(DatabaseRelationId, AccessShareLock);
			SysScanDesc scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
			HeapTuple	tup;

			while ((tup = systable_getnext(scan)) != NULL)
			{
				Form_pg_database d = (Form_pg_database) GETSTRUCT(tup);

				if (d->datallowconn && !d->datistemplate)
				{
					MemoryContext txn = MemoryContextSwitchTo(keep);

					dbs = lappend_oid(dbs, d->oid);
					MemoryContextSwitchTo(txn);
				}
			}
			systable_endscan(scan);
			table_close(rel, AccessShareLock);
		}
		PopActiveSnapshot();
		CommitTransactionCommand();
	}
	/* nothing to look at: no worker, no connection */
	foreach(lc, dbs)
	{
		BackgroundWorker worker;
		BackgroundWorkerHandle *handle;
		Oid			db = lfirst_oid(lc);

		memset(&worker, 0, sizeof(worker));
		snprintf(worker.bgw_name, BGW_MAXLEN, "pg_cuvs cover sweep of database %u", db);
		snprintf(worker.bgw_type, BGW_MAXLEN, "pg_cuvs cover sweep");
		worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
		worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
		worker.bgw_restart_time = BGW_NEVER_RESTART;
		snprintf(worker.bgw_library_name, BGW_MAXLEN, "pg_cuvs");
		snprintf(worker.bgw_function_name, BGW_MAXLEN, "cuvs_cover_sweep_db_main");
		worker.bgw_main_arg = ObjectIdGetDatum(db);
		memcpy(worker.bgw_extra, &full, sizeof(bool));
		worker.bgw_notify_pid = MyProcPid;
		/* no free worker place: this database is looked at in the next sweep (a full sweep is asked for again so nothing is skipped) */
		if (RegisterDynamicBackgroundWorker(&worker, &handle))
			(void) WaitForBackgroundWorkerShutdown(handle);
		else if (full)
		{
			SpinLockAcquire(&g_req->mutex);
			g_req->full_sweep = true;
			SpinLockRelease(&g_req->mutex);
		}
		if (ShutdownRequestPending)
			break;
	}
	list_free(dbs);
}

void
cuvs_cover_sweeper_main(Datum main_arg)
{
	long		wait_ms = 10000;	/* the first sweep soon after the server is up */

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnection(NULL, NULL, 0);
	for (;;)
	{
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, wait_ms, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		/* a sweep that has nothing to do opens no transaction, so the barriers other backends wait for (DROP DATABASE waits for every
		 * process to release its file handles) and the interrupts are taken here, at every wake */
		CHECK_FOR_INTERRUPTS();
		if (ShutdownRequestPending)
			proc_exit(0);
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
		wait_ms = sweep_period_ms();
		if (cuvs_cover_auto_recertify && g_req != NULL)
			sweep_all();
	}
}
