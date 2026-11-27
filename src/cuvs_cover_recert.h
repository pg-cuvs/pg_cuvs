#ifndef CUVS_COVER_RECERT_H
#define CUVS_COVER_RECERT_H
#include "postgres.h"
#include "access/amapi.h"
#include "utils/rel.h"
#include "utils/timestamp.h"
#include "cuvs_cover_format.h"

typedef enum {
	CUVS_RECERT_DONE = 0,		/* the index is CERTIFIED with a new ledger */
	CUVS_RECERT_NOT_APPLICABLE,	/* LEGACY, OFF, untrusted, a newer format, or already certified with little to fold in */
	CUVS_RECERT_DEFERRED,		/* the cover lock could not be had within the bound; try again later */
	CUVS_RECERT_FAILED,		/* the reachability walk gave no result (over the memory limits, or a damaged chain) */
	CUVS_RECERT_SKIPPED,		/* it failed before and nothing that could change the outcome has changed */
	CUVS_RECERT_ABANDONED		/* the descriptor changed under the walk; nothing was published */
} CuvsRecertStatus;

typedef struct CuvsRecertOutcome {
	CuvsRecertStatus status;
	int			reach_status;	/* CuvsReachStatus of the walk, when FAILED */
	uint64		live, unreached;	/* from the walk */
	uint64		kept_keys;		/* ledger keys written after the walk started that were carried over */
	bool		chain_replaced;	/* the old chain did not read: a fresh empty chain took its place before the walk */
	uint64		reused_pages;	/* ledger pages of the new chain that came from the free list */
	uint64		reclaimed_pages;	/* pages of the new chain of an earlier recertification that did not publish, put on the free list before this one began */
	uint64		locked_keys;	/* of those, the ones copied while the cover lock was held exclusively */
	uint64		needed_bytes;		/* FAILED over limit: the walk's estimate */
	uint64		mem_bytes, spilled_bytes;
	uint64		phase1_lock_us, phase3_lock_us;	/* how long the cover lock was held exclusively in the first and the last phase (phase 3: 0 if it was not reached) */
	uint64		walk_us;	/* the reachability walk, without the lock */
	const char *detail;
} CuvsRecertOutcome;

/*
 * Recertify `index` (of `heap`): compute what the graph cannot reach, write it as the base of a new ledger chain together
 * with every key written since the walk started, and publish the chain with state CERTIFIED in one WAL record.
 * The caller holds ShareUpdateExclusiveLock on `heap` (this serialises against VACUUM, other recertifications and
 * REINDEX CONCURRENTLY) and at least RowExclusiveLock on `index`. ram_limit/spill_limit as in cuvs_cover_reach_index.
 * `force` recertifies a CERTIFIED index too (the manual function); otherwise only a LOST one with a reason that a
 * recertification fixes, or a CERTIFIED one whose ledger grew past the threshold, is taken.
 */
extern void cuvs_cover_recertify(Relation heap, Relation index, uint64 ram_limit, uint64 spill_limit, bool force,
				 CuvsRecertOutcome *out);

/* The same for a caller that is not a VACUUM (the manual function, the background worker): the walk gives way, with result DEFERRED and nothing
 * written, when another session waits for a lock that conflicts with the table's or the index's lock the caller holds. A VACUUM does not give
 * way: it holds the table's lock for its own whole run anyway. */
extern void cuvs_cover_recertify_yielding(Relation heap, Relation index, uint64 ram_limit, uint64 spill_limit, bool force,
					  CuvsRecertOutcome *out);

/* The same for the time-based recertification (cuvs.cover_recertify_max_age): a CERTIFIED index is taken when any key was written since its
 * certification, however few; the failure memory applies. Gives way like cuvs_cover_recertify_yielding. */
extern void cuvs_cover_recertify_aged(Relation heap, Relation index, uint64 ram_limit, uint64 spill_limit, CuvsRecertOutcome *out);

/* Is a time-based recertification due for `index` at `now` (see the definition). Reads the descriptor and the ledger tail only. */
extern bool cuvs_cover_sweep_due(Relation index, TimestampTz now, int64 max_age_us);

/* Has `index` anything since its certification that a later recertification must take (LOST, or a key or a page since)? The sweeper keeps
 * such an index as a candidate. */
extern bool cuvs_cover_sweep_pending(Relation index);

/* Keys left to copy under the exclusive lock at most (after up to COPY_PASSES unlocked passes). */
#define CUVS_RECERT_TAIL_KEYS 1000
#define CUVS_RECERT_COPY_PASSES 5

/* Default of cuvs.cover_recertify_threshold_percent: ledger growth since the last certification, as a percent of the rows it covered,
 * beyond which a CERTIFIED index is recertified by VACUUM or the background worker and shown as "recommended" by the status
 * function. A ratio only: there is no minimum number of keys. */
#define CUVS_RECERT_GROWTH_PERCENT 5

/* The descriptor, the keys written since the certification and whether that is past the recommendation threshold. */
extern bool cuvs_cover_growth_status(Relation index, CuvsCoverDescriptor *d, uint64 *delta_keys, bool *recommended);

/* The pages the relation has grown by since the certification the descriptor `d` describes (0 if it did not grow). */
extern uint64 cuvs_cover_pages_since_cert(Relation index, const CuvsCoverDescriptor *d);

/* Whether the growth rule (or an unreadable ledger) says a CERTIFIED index is due for a recertification. */
extern bool cuvs_cover_recertify_due(Relation index);

/* The tail of a build over rows: certify the new index now (it started LOST, uncertified). Never fails the build. */
extern void cuvs_cover_certify_after_build(Relation heap, Relation index);

/* Wrap ambulkdelete (CERTIFIED -> LOST before pgvector changes the graph) and amvacuumcleanup (automatic recertification). */
extern void cuvs_cover_wrap_vacuum(IndexAmRoutine *amroutine);

#endif
