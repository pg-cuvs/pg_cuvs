-- gc_orphans.sql — ADR-046 orphan artifact GC (pg_cuvs_gc_orphans).
--
-- Coverage (deterministic, daemon-up):
--   1. pg_cuvs_gc_orphans() is callable and returns a well-formed set.
--   2. SAFETY: a live CAGRA index in this DB is never reported as an orphan.
--   3. SAFETY: do_delete=true never deletes a live index.
--   4. Dry-run + delete are non-destructive — the index still serves GPU search.
--   5. A daemon-up DROP cleans artifacts; GC then finds no missing_in_catalog orphan.
--
-- NOT covered here (inherently non-deterministic for pg_regress — needs the daemon
-- stopped mid-test): the daemon-DOWN DROP -> orphan -> GC reclaim -> no-zombie-reload
-- path. That is verified manually on the GPU VM; see docs/spec-audit / OPS_GPU_PLAYBOOK
-- and the manual procedure in DECISIONS.md ADR-046.

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- 1. Callable + well-formed result set.
SELECT count(*) >= 0 AS gc_callable FROM pg_cuvs_gc_orphans();

CREATE TABLE gc_test (id bigint, embedding vector(4));
INSERT INTO gc_test VALUES
    (1, '[1,0,0,0]'), (2, '[0,1,0,0]'),
    (3, '[0,0,1,0]'), (4, '[0,0,0,1]');
CREATE INDEX gc_live ON gc_test USING cagra (embedding vector_l2_ops);

-- 2. SAFETY: a live index in this database is never reported as an orphan.
SELECT count(*) AS live_reported
FROM pg_cuvs_gc_orphans()
WHERE index_oid = 'gc_live'::regclass;

-- 3. SAFETY: do_delete=true must not delete any live index. (Also reclaims any
-- pre-existing orphans in this DB, leaving a clean slate for step 5.)
SELECT count(*) AS live_deleted
FROM pg_cuvs_gc_orphans(true)
WHERE index_oid = 'gc_live'::regclass AND action = 'deleted';

-- 4. Non-destructive: the live index still serves GPU search after GC ran.
SET enable_seqscan = off;
SELECT id FROM gc_test ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 1;
RESET enable_seqscan;

-- 5. A daemon-up DROP unlinks artifacts; GC then sees no missing_in_catalog orphan.
DROP INDEX gc_live;
SELECT count(*) AS post_drop_orphans
FROM pg_cuvs_gc_orphans()
WHERE reason = 'missing_in_catalog';

DROP TABLE gc_test;
