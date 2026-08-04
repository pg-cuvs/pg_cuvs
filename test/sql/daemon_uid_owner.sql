-- daemon_uid_owner.sql — #119 (#87 follow-up): cuvs.daemon_uid pre-connect
-- socket-owner check.
--
-- SO_PEERCRED (shm_check_peer_owner, #87) verifies the connecting peer's uid
-- AFTER a UDS connection is already made — it can't stop an attacker who
-- unlinks the real daemon socket and squats cuvs.socket_path with their own
-- listener before the backend connects. cuvs.daemon_uid closes that gap by
-- checking the socket file's owner uid before connect() (uds_connect_ex,
-- cuvs_ipc.c).
--
-- Discriminator: pg_stat_gpu_search reaches the daemon via cuvs_ipc_stats,
-- which goes through the same uds_connect_ex gate as every other cuvs_ipc_*
-- call. A daemon-down/UNAVAILABLE result is documented (cuvs_ipc.c) to leave
-- the view empty rather than erroring — so a wrong cuvs.daemon_uid is
-- observable as "0 rows" (fail-closed), not a crash and not attacker data.
--
--   1. discover the real daemon uid from cuvs.socket_path's owner,
--   2. SET cuvs.daemon_uid to that value -> build + search + stats all work
--      normally (backward-compat: matches the real owner),
--   3. SET cuvs.daemon_uid to a uid that cannot own the socket -> the stats
--      RPC comes back empty instead of returning (or crashing on) whatever
--      is actually listening on the path.
--
-- REQUIRES: pg_cuvs_server running; cuvs.index_dir writable; the connecting
-- role can COPY FROM PROGRAM (superuser in the regression harness). Runs in
-- Tier-1 CI too — the CPU-shim daemon owns its socket the same way.

\set ON_ERROR_STOP on

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
RESET client_min_messages;

SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- ----------------------------------------------------------------
-- Discover the real daemon uid (owner of cuvs.socket_path) so setup below
-- can run with the check enabled and matching, rather than disabled.
-- ----------------------------------------------------------------
CREATE TEMP TABLE daemon_uid_probe (uid int);
DO $$
DECLARE
    sock text := current_setting('cuvs.socket_path');
BEGIN
    EXECUTE format('COPY daemon_uid_probe FROM PROGRAM %L', 'stat -c %u ' || sock);
END $$;
SELECT uid AS real_daemon_uid FROM daemon_uid_probe \gset

SET cuvs.daemon_uid = :real_daemon_uid;

CREATE TABLE daemon_uid_owner_t (id int, v vector(8));
SELECT setseed(0.87);
INSERT INTO daemon_uid_owner_t
SELECT g, array_agg(round((random())::numeric, 5) ORDER BY d)::real[]::vector(8)
FROM generate_series(1, 200) g, generate_series(1, 8) d
GROUP BY g;

SET cuvs.search_mode = cagra;
SET max_parallel_workers_per_gather = 0;
CREATE INDEX daemon_uid_owner_idx ON daemon_uid_owner_t USING cagra (v vector_l2_ops);

-- ----------------------------------------------------------------
-- Test 1: matching daemon uid — search works, stats RPC reaches the daemon.
-- ----------------------------------------------------------------
SELECT count(*) FROM (
    SELECT id FROM daemon_uid_owner_t
    ORDER BY v <-> '[0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5]'::vector(8)
    LIMIT 5
) s;

SELECT count(*) >= 1 AS matching_uid_reaches_daemon
FROM pg_stat_gpu_search WHERE index_oid = 'daemon_uid_owner_idx'::regclass;

-- ----------------------------------------------------------------
-- Test 2: a uid that cannot own the socket -> uds_connect_ex refuses to
-- connect -> stats RPC comes back UNAVAILABLE -> empty view (fail-closed).
-- ----------------------------------------------------------------
SET cuvs.daemon_uid = 999999;

SELECT count(*) = 0 AS mismatched_uid_fails_closed
FROM pg_stat_gpu_search WHERE index_oid = 'daemon_uid_owner_idx'::regclass;

RESET cuvs.daemon_uid;

DROP TABLE daemon_uid_owner_t CASCADE;
