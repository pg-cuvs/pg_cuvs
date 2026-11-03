-- build_oom_evict_to_fit.sql — the name is historical. ADR-070 made a build
-- OOM evict the LRU repeatedly until the build fit; ADR-086 (I3) removes that:
-- an out-of-memory build after admission fails with SQLSTATE 53200 and evicts
-- NOTHING, even when evicting would have made room. Two OOMs are armed with
-- two resident victims present, which is exactly the shape evict-to-fit used
-- to "fix"; the eviction counter must not move on either build path.
--
-- REQUIRES: a CUVS_TEST_HOOKS pg_cuvs_server_test daemon on the test socket +
-- index dir below (`make installcheck-fault`, not `make installcheck` — #124:
-- CUVS_OP_INJECT_BUILD_OOM only exists in a CUVS_TEST_HOOKS build; the
-- production daemon rejects it). Tier-1 CI (CPU shim).

\set ON_ERROR_STOP on

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
RESET client_min_messages;
-- #124: points at the CUVS_TEST_HOOKS test daemon (make installcheck-fault),
-- not the production socket/index dir.
SET cuvs.socket_path = '/tmp/.s.pg_cuvs_test';
SET cuvs.index_dir = '/tmp/cuvs_indexes_test';

-- ============================================================ single-process
SET max_parallel_maintenance_workers = 0;
CREATE TABLE sv1 (id int, embedding vector(8));
INSERT INTO sv1 SELECT g, format('[%s,0,0,0,0,0,0,0]', (g*0.1)::numeric(8,3))::vector FROM generate_series(1,1000) g;
CREATE INDEX sv1_idx ON sv1 USING cagra (embedding vector_l2_ops);
CREATE TABLE sv2 (id int, embedding vector(8));
INSERT INTO sv2 SELECT g, format('[0,%s,0,0,0,0,0,0]', (g*0.1)::numeric(8,3))::vector FROM generate_series(1,1000) g;
CREATE INDEX sv2_idx ON sv2 USING cagra (embedding vector_l2_ops);

SELECT evictions AS e_st FROM pg_stat_gpu_cache WHERE gpu_device_id = 0 \gset
SELECT pg_cuvs_inject_build_oom(2);   -- evict-to-fit would have evicted twice
CREATE TABLE st (id int, embedding vector(8));
INSERT INTO st SELECT g, format('[0,0,%s,0,0,0,0,0]', (g*0.1)::numeric(8,3))::vector FROM generate_series(1,1000) g;
\set ON_ERROR_STOP off
\set VERBOSITY terse
CREATE INDEX st_idx ON st USING cagra (embedding vector_l2_ops);   -- must FAIL
\echo :LAST_ERROR_SQLSTATE
\set VERBOSITY default
\set ON_ERROR_STOP on
SELECT pg_cuvs_inject_build_oom(0);
SELECT r->>'reason' AS reason, r->>'sqlstate' AS sqlstate
FROM pg_cuvs_last_rejection() r;
SELECT evictions = :e_st AS single_process_nothing_evicted
FROM pg_stat_gpu_cache WHERE gpu_device_id = 0;

-- ============================================================ parallel
SET max_parallel_maintenance_workers = 2;
SET min_parallel_table_scan_size = 0;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
CREATE TABLE pv1 (id int, embedding vector(8));
INSERT INTO pv1 SELECT g, format('[0,0,0,%s,0,0,0,0]', (g*0.1)::numeric(8,3))::vector FROM generate_series(1,1000) g;
CREATE INDEX pv1_idx ON pv1 USING cagra (embedding vector_l2_ops);
CREATE TABLE pv2 (id int, embedding vector(8));
INSERT INTO pv2 SELECT g, format('[0,0,0,0,%s,0,0,0]', (g*0.1)::numeric(8,3))::vector FROM generate_series(1,1000) g;
CREATE INDEX pv2_idx ON pv2 USING cagra (embedding vector_l2_ops);

SELECT evictions AS e_pt FROM pg_stat_gpu_cache WHERE gpu_device_id = 0 \gset
SELECT pg_cuvs_inject_build_oom(2);   -- evict-to-fit would have evicted twice
CREATE TABLE pt (id int, embedding vector(8));
INSERT INTO pt SELECT g, format('[0,0,0,0,0,%s,0,0]', (g*0.1)::numeric(8,3))::vector FROM generate_series(1,1000) g;
\set ON_ERROR_STOP off
\set VERBOSITY terse
CREATE INDEX pt_idx ON pt USING cagra (embedding vector_l2_ops);   -- must FAIL
\echo :LAST_ERROR_SQLSTATE
\set VERBOSITY default
\set ON_ERROR_STOP on
SELECT pg_cuvs_inject_build_oom(0);
SELECT r->>'reason' AS reason, r->>'sqlstate' AS sqlstate
FROM pg_cuvs_last_rejection() r;
SELECT evictions = :e_pt AS parallel_nothing_evicted
FROM pg_stat_gpu_cache WHERE gpu_device_id = 0;

DROP TABLE sv1; DROP TABLE sv2; DROP TABLE st;
DROP TABLE pv1; DROP TABLE pv2; DROP TABLE pt;
