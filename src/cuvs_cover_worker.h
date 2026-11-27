#ifndef CUVS_COVER_WORKER_H
#define CUVS_COVER_WORKER_H
#include "postgres.h"
#include "utils/rel.h"

/*
 * Recertification without anyone asking for it.
 *
 * An insert into a CERTIFIED index knows, without any extra read, how many keys the ledger holds and how many rows the
 * certification covered. When the keys written since have passed cuvs.cover_recertify_threshold_percent of those rows, the insert
 * asks for a short-lived background worker (RegisterDynamicBackgroundWorker) that connects to the database and runs the
 * same recertification VACUUM and pg_cuvs_cover_recertify() run. Nothing is stored in shared memory: the request is made at
 * most once per index and cuvs.cover_recertify_min_interval seconds by each backend, and the worker takes a session lock on the
 * index first, so that two workers for one index end at once instead of working twice. A worker slot that is not there is not
 * an error: the insert goes on and VACUUM remains.
 */
extern bool cuvs_cover_auto_recertify;
extern int	cuvs_cover_recert_percent;
extern int	cuvs_cover_recert_min_interval;
extern int	cuvs_cover_recert_max_workers;
extern int	cuvs_cover_recert_max_age;

extern void cuvs_cover_worker_init(void);

/* True when requests can be made: the setting is on and the library holds shared memory (it is in shared_preload_libraries). */
extern bool cuvs_cover_auto_recertify_available(void);

/* Write (this database, `index_oid`) into the sweeper's table of candidates: its first insert after a certification, or its guarantee was taken
 * off. Nothing without shared memory. */
extern void cuvs_cover_note_candidate(Oid index_oid);

/* The same, from a VACUUM that takes the guarantee off (ambulkdelete): the candidate also remembers which VACUUM it was. */
extern void cuvs_cover_note_vacuum(Oid index_oid);

/* Is the VACUUM that took the guarantee of `index_oid` (in this database) off still running? False without shared memory, without a record
 * (the table was full, the server restarted), or once that VACUUM's transaction has ended in any way. */
extern bool cuvs_cover_vacuum_running(Oid index_oid);

/* in a background worker of pg_cuvs, does a command on this database wait for its other sessions to go (DROP DATABASE, ALTER DATABASE
 * ... SET TABLESPACE, CREATE DATABASE ... TEMPLATE)? Then the worker steps aside. Looked up at most every 100 ms; always false in a session. */
extern bool cuvs_cover_db_command_waits(void);
extern bool cuvs_cover_stepped_aside;		/* set once it answered true in this process */

#endif
