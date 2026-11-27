#ifndef CUVS_COVER_REACH_PG_H
#define CUVS_COVER_REACH_PG_H
/* PostgreSQL side of the structural reachability walk (src/cuvs_cover_reach.h). */
#include "postgres.h"
#include "utils/rel.h"

typedef int (*CuvsReachEmitFn)(void *ctx, uint32 heap_blk, uint16 heap_off);

/* In the product library the worker module fills this hook. A standalone probe of the reach walk leaves it NULL;
 * the hook is false by definition outside a background worker. */
typedef bool (*CuvsCoverReachDbwaitHook)(void);
extern CuvsCoverReachDbwaitHook cuvs_cover_reach_dbwait_hook;

/* Result of a walk, plain integers so the caller needs no core header. */
typedef struct CuvsReachCounts {
	int			status;		/* 0 = ok; see CuvsReachStatus (2 over limit, 4 damaged) */
	uint32		nblocks;
	uint64		elements, live, reached, unreached, emitted;
	uint64		mem_bytes;	/* peak RAM the walk accounted */
	uint64		spilled_bytes;	/* peak bytes it placed in an unlinked temp file */
	uint64		needed_bytes;	/* over limit: roughly what RAM + spill it needs */
} CuvsReachCounts;

/*
 * The limits a recertification should use: RAM is autovacuum_work_mem inside an autovacuum worker when that is set,
 * else maintenance_work_mem; the spill limit is "as much as the disk gives" unless temp_file_limit is finite, in which
 * case there is no spill (the mapped file cannot be accounted against it).
 */
extern void cuvs_cover_reach_default_limits(uint64 *ram_limit, uint64 *spill_limit);

/*
 * Walk `index` (a pgvector hnsw or pg_cuvs_hnsw index the caller has locked) and count what the search cannot
 * reach from the entry point; `emit` (may be NULL) receives the heap TID of every unreached live element.
 * Every page is copied under a short buffer share lock (through a bulk-read ring) and released before the next
 * read; nothing else is held. CHECK_FOR_INTERRUPTS and vacuum_delay_point run every few hundred pages.
 * A status other than 0 means there is no result: never certify on it.
 */
extern void cuvs_cover_reach_index(Relation index, uint64 ram_limit, uint64 spill_limit, CuvsReachEmitFn emit,
				   void *emit_ctx, CuvsReachCounts *out);

/*
 * The same, for a caller that holds ShareUpdateExclusiveLock on `yield_heap`: when another session starts to wait for a lock that conflicts
 * with it (checked every few hundred pages; DDL on the table or on its indexes does) the walk stops with status 5 (CUVS_REACH_ABORTED) and no
 * result, so that the waiter, and everyone queued behind it, is not held for the length of the walk.
 */
extern void cuvs_cover_reach_index_yielding(Relation index, Relation yield_heap, uint64 ram_limit, uint64 spill_limit, CuvsReachEmitFn emit,
					    void *emit_ctx, CuvsReachCounts *out);

#endif
