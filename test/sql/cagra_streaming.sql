-- cagra_streaming.sql — Phase 3Q: CAGRA Streaming Updates (EXTEND + COMPACT)
--
-- Tests:
--   1. INSERT after index build: new vector visible in search (EXTEND path).
--   2. pg_cuvs_compact() on a clean index: no-op, returns without error.
--   3. DELETE + VACUUM + pg_cuvs_compact(): dead vector removed from results.
--   4. Compact with no tombstone file: idempotent, returns OK.
--
-- Determinism: base cluster sits near origin (dist ~0.01); the inserted vector
-- sits at [10,0,0,0] — unambiguously the nearest neighbor to that query point.
-- After compact, the far point must not appear in top-1 for that query.
-- CAGRA is approximate but at this distance ratio (1000x) ordering is exact.
--
-- Requires a running pg_cuvs_server with a loaded CAGRA index.

CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- ----------------------------------------------------------------
-- Setup: small base cluster near origin, all within dist 0.05
-- ----------------------------------------------------------------
CREATE TABLE cs (id bigint, embedding vector(4));
INSERT INTO cs SELECT g, ('['||(g*0.01)||',0,0,0]')::vector
FROM generate_series(1, 100) g;
CREATE INDEX cs_cagra ON cs USING cagra (embedding vector_l2_ops);

-- ----------------------------------------------------------------
-- Test 1: INSERT a far vector — it must appear as top-1 near [10,0,0,0]
-- ----------------------------------------------------------------
INSERT INTO cs VALUES (1001, '[10,0,0,0]');

SET enable_seqscan = off;
-- Expect: id=1001 (the just-inserted far vector)
SELECT id FROM cs ORDER BY embedding <-> '[10,0,0,0]'::vector LIMIT 1;

-- ----------------------------------------------------------------
-- Test 2: pg_cuvs_compact() on a clean index (no tombstones) — must not error
-- ----------------------------------------------------------------
SELECT pg_cuvs_compact('cs_cagra'::regclass);

-- ----------------------------------------------------------------
-- Test 3: DELETE the far vector, VACUUM, compact, verify it's gone
-- ----------------------------------------------------------------
DELETE FROM cs WHERE id = 1001;
VACUUM cs;
SELECT pg_cuvs_compact('cs_cagra'::regclass);

-- After compact: top-1 near [10,0,0,0] must come from the base cluster (id <= 100)
SELECT id <= 100 AS from_base_cluster
FROM cs ORDER BY embedding <-> '[10,0,0,0]'::vector LIMIT 1;

-- ----------------------------------------------------------------
-- Test 4: second compact on already-clean index — idempotent
-- ----------------------------------------------------------------
SELECT pg_cuvs_compact('cs_cagra'::regclass);

-- ----------------------------------------------------------------
-- Test 5: VACUUM alone triggers compact (amvacuumcleanup path)
-- ----------------------------------------------------------------
INSERT INTO cs VALUES (2001, '[20,0,0,0]');
DELETE FROM cs WHERE id = 2001;
VACUUM cs;
-- After amvacuumcleanup fires COMPACT: top-1 near [20,0,0,0] must be from base cluster
SELECT id <= 100 AS from_base_cluster
FROM cs ORDER BY embedding <-> '[20,0,0,0]'::vector LIMIT 1;

RESET enable_seqscan;

DROP TABLE cs;
DROP EXTENSION pg_cuvs;
