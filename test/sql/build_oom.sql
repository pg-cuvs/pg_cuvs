-- build_oom.sql — ADR-069 Bug #3: a CAGRA build that OOMs (build-time scratch is
-- not covered by the pre-build VRAM estimate) must evict an LRU index and retry
-- once before failing, instead of returning BUILD_FAILED immediately.
--
-- Setup: a victim index (boom_a) stays resident so the retry has something to
-- evict. Arm exactly one synthetic build OOM, then CREATE INDEX on boom_b. With
-- the bug, the first (and only) cuvs_cagra_build returns NULL -> BUILD_FAILED ->
-- CREATE INDEX raises an error and ON_ERROR_STOP aborts the test. With the fix,
-- the daemon evicts boom_a and the retry succeeds, so CREATE INDEX completes.
--
-- REQUIRES: a CUVS_TEST_HOOKS pg_cuvs_server_test daemon on the test socket +
-- index dir below (`make installcheck-fault`, not `make installcheck` — #124:
-- CUVS_OP_INJECT_BUILD_OOM only exists in a CUVS_TEST_HOOKS build; the
-- production daemon rejects it). Runs in Tier-1 CI (CPU shim):
-- cuvs_set_inject_build_oom is implemented in the shim too.
--
-- ENVIRONMENT-DEPENDENT (#89 item 2): on the CPU shim (Tier-1 CI) the OOM
-- injection is deterministic, so this test is a stable pass. On real GPU —
-- especially a single-GPU dev VM — the evict-retry outcome is residency-
-- state-dependent: it depends on what is resident and evictable at run
-- time, so a full-suite GPU run may show this test flaking between runs.
-- A flake here on GPU is environment-dependent, NOT a code regression.

\set ON_ERROR_STOP on

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
RESET client_min_messages;

-- #124: points at the CUVS_TEST_HOOKS test daemon (make installcheck-fault),
-- not the production socket/index dir.
SET cuvs.socket_path = '/tmp/.s.pg_cuvs_test';
SET cuvs.index_dir = '/tmp/cuvs_indexes_test';

-- Victim index: resident LRU candidate for the retry to evict.
CREATE TABLE boom_a (id int, embedding vector(8));
INSERT INTO boom_a
SELECT g, format('[%s,0,0,0,0,0,0,0]', (g * 0.1)::numeric(8,3))::vector
FROM generate_series(1, 200) g;
CREATE INDEX boom_a_idx ON boom_a USING cagra (embedding vector_l2_ops);

-- Cumulative eviction baseline (counters are daemon-lifetime).
SELECT evictions AS e0 FROM pg_stat_gpu_cache WHERE gpu_device_id = 0 \gset

-- Arm one build OOM: the next CREATE INDEX's first build fails, the daemon
-- evicts the LRU (boom_a) and retries.
SELECT pg_cuvs_inject_build_oom(1);

CREATE TABLE boom_b (id int, embedding vector(8));
INSERT INTO boom_b
SELECT g, format('[0,%s,0,0,0,0,0,0]', (g * 0.1)::numeric(8,3))::vector
FROM generate_series(1, 200) g;
CREATE INDEX boom_b_idx ON boom_b USING cagra (embedding vector_l2_ops);

SELECT pg_cuvs_inject_build_oom(0);

-- The retried build is a working index: an exact (brute_force) self-query
-- returns the row itself as its own nearest neighbor.
SET cuvs.search_mode = 'brute_force';
SELECT id FROM boom_b
ORDER BY embedding <-> (SELECT embedding FROM boom_b WHERE id = 7)
LIMIT 1;

-- The retry path evicted the LRU index at least once.
SELECT evictions > :e0 AS bug3_evicted_and_retried
FROM pg_stat_gpu_cache WHERE gpu_device_id = 0;

DROP TABLE boom_a;
DROP TABLE boom_b;
