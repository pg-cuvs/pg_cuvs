-- build_oom.sql — ADR-086 (I3, supersedes ADR-069/070 Bug #3's evict-and-retry):
-- a CAGRA build that runs out of GPU memory AFTER admission fails without
-- evicting any resident index. The reply is status 2 with an OOM err_out, the
-- backend raises SQLSTATE 53200 with the C5 message, the daemon logs
-- [build_oom] action=fail_no_evict, and the build is not retried.
--
-- Setup: a resident index (boom_a) that an evict-and-retry implementation would
-- evict. Arm exactly one synthetic build OOM, then CREATE INDEX on boom_b: it
-- must fail with 53200 and the eviction counter must not move. After disarming,
-- the same CREATE INDEX succeeds (the failure was the injected OOM only).
--
-- REQUIRES: a CUVS_TEST_HOOKS pg_cuvs_server_test daemon on the test socket +
-- index dir below (`make installcheck-fault`, not `make installcheck` — #124:
-- CUVS_OP_INJECT_BUILD_OOM only exists in a CUVS_TEST_HOOKS build; the
-- production daemon rejects it). Runs in Tier-1 CI (CPU shim):
-- cuvs_set_inject_build_oom is implemented in the shim too. DETAIL numbers
-- depend on daemon state, so the error is shown terse and its fields are
-- checked through pg_cuvs_last_rejection().

\set ON_ERROR_STOP on

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
RESET client_min_messages;

SET cuvs.socket_path = '/tmp/.s.pg_cuvs_test';
SET cuvs.index_dir = '/tmp/cuvs_indexes_test';

CREATE TABLE boom_a (id int, embedding vector(8));
INSERT INTO boom_a
SELECT g, format('[%s,0,0,0,0,0,0,0]', (g * 0.1)::numeric(8,3))::vector
FROM generate_series(1, 200) g;
CREATE INDEX boom_a_idx ON boom_a USING cagra (embedding vector_l2_ops);

SELECT evictions AS e0 FROM pg_stat_gpu_cache WHERE gpu_device_id = 0 \gset

SELECT pg_cuvs_inject_build_oom(1);

CREATE TABLE boom_b (id int, embedding vector(8));
INSERT INTO boom_b
SELECT g, format('[0,%s,0,0,0,0,0,0]', (g * 0.1)::numeric(8,3))::vector
FROM generate_series(1, 200) g;
\set ON_ERROR_STOP off
\set VERBOSITY terse
CREATE INDEX boom_b_idx ON boom_b USING cagra (embedding vector_l2_ops);
\echo :LAST_ERROR_SQLSTATE
\set VERBOSITY default
\set ON_ERROR_STOP on

SELECT r->>'reason' AS reason, r->>'sqlstate' AS sqlstate,
       r->>'requester' AS requester,
       (r->'fields') ?& ARRAY['need_mib', 'free_mib', 'resident_mib', 'resident_count']
         AS oom_fields
FROM pg_cuvs_last_rejection() r;

SELECT evictions = :e0 AS no_resident_index_evicted
FROM pg_stat_gpu_cache WHERE gpu_device_id = 0;

SELECT pg_cuvs_inject_build_oom(0);
CREATE INDEX boom_b_idx ON boom_b USING cagra (embedding vector_l2_ops);

SET cuvs.search_mode = 'brute_force';
SELECT id FROM boom_b
ORDER BY embedding <-> (SELECT embedding FROM boom_b WHERE id = 7)
LIMIT 1;

DROP TABLE boom_a;
DROP TABLE boom_b;
