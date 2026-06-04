-- pg_cuvs_hnsw.sql — Phase 3K (ADR-038): CREATE INDEX ... USING pg_cuvs_hnsw DDL.
--
-- Coverage:
--   1. AM + operator classes registered by the 0.2.0 migration.
--   2. DDL build from a CAGRA source (default mode 'nsw').
--   3. Index is a first-class pg_indexes entry, served via pgvector HNSW path.
--   4. Explicit mode + WITH options.
--   5. REINDEX re-runs ambuild from the stored source (natural rebuild).
--   6. DROP INDEX behaves naturally.
--   7. Missing 'source' option -> clear ERROR.
--   8. Non-cagra source -> clear ERROR.
--
-- REQUIRES: pg_cuvs_server running with GPU, cuvs.index_dir writable.

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;

-- AM + opclasses present (from the 0.1.0->0.2.0 migration).
SELECT amname FROM pg_am WHERE amname = 'pg_cuvs_hnsw';
SELECT opcname
FROM pg_opclass oc JOIN pg_am am ON am.oid = oc.opcmethod
WHERE am.amname = 'pg_cuvs_hnsw'
ORDER BY opcname;

SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- Setup: 20-vector 4-dim table. Query [1,0.5,0,0] -> unambiguous top-1 = id=20.
CREATE TABLE ph_test (id bigint, embedding vector(4));
INSERT INTO ph_test VALUES
    (1,'[1,0,0,0]'),(2,'[0,1,0,0]'),(3,'[0,0,1,0]'),(4,'[0,0,0,1]'),
    (5,'[0.1,0,0,0]'),(6,'[0,0.1,0,0]'),(7,'[0,0,0.1,0]'),(8,'[0,0,0,0.1]'),
    (9,'[0.5,0.5,0,0]'),(10,'[0,0.5,0.5,0]'),(11,'[0,0,0.5,0.5]'),
    (12,'[0.5,0,0.5,0]'),(13,'[0.5,0,0,0.5]'),(14,'[0,0.5,0,0.5]'),
    (15,'[0.3,0.3,0.3,0]'),(16,'[0.3,0.3,0,0.3]'),(17,'[0.3,0,0.3,0.3]'),
    (18,'[0,0.3,0.3,0.3]'),(19,'[0.25,0.25,0.25,0.25]'),(20,'[0.9,0.1,0,0]');

-- Source CAGRA index (daemon-resident; ambuild reads its graph over IPC).
CREATE INDEX ph_cagra ON ph_test USING cagra (embedding vector_l2_ops);

-- ── DDL build (default mode 'nsw') ───────────────────────────────
SET client_min_messages = 'warning';
CREATE INDEX ph_hnsw ON ph_test USING pg_cuvs_hnsw (embedding vector_l2_ops)
    WITH (source = 'ph_cagra');
SET client_min_messages = 'notice';

-- Catalog visibility: a first-class pg_indexes entry using the pg_cuvs_hnsw AM.
SELECT i.indexname, a.amname
FROM pg_indexes i
JOIN pg_class c ON c.relname = i.indexname
JOIN pg_am a ON a.oid = c.relam
WHERE i.tablename = 'ph_test' AND i.indexname = 'ph_hnsw';

-- Served by pgvector HNSW path with GPU off. Top-1 must be id=20.
SET enable_cuvs = off; SET enable_seqscan = off;
SELECT id FROM ph_test ORDER BY embedding <-> '[1,0.5,0,0]' LIMIT 1;
RESET enable_cuvs; RESET enable_seqscan;

-- ── explicit mode = 'hnsw' ───────────────────────────────────────
SET client_min_messages = 'warning';
CREATE INDEX ph_hnsw2 ON ph_test USING pg_cuvs_hnsw (embedding vector_l2_ops)
    WITH (source = 'ph_cagra', mode = 'hnsw');
SET client_min_messages = 'notice';
SELECT indexname FROM pg_indexes
WHERE tablename = 'ph_test' AND indexname = 'ph_hnsw2';

-- ── REINDEX re-runs ambuild from the stored 'source' relopt ──────
SET client_min_messages = 'warning';
REINDEX INDEX ph_hnsw;
SET client_min_messages = 'notice';
SET enable_cuvs = off; SET enable_seqscan = off;
SELECT id FROM ph_test ORDER BY embedding <-> '[1,0.5,0,0]' LIMIT 1;
RESET enable_cuvs; RESET enable_seqscan;

-- ── DROP INDEX behaves naturally ─────────────────────────────────
DROP INDEX ph_hnsw2;
SELECT count(*) FROM pg_indexes
WHERE tablename = 'ph_test' AND indexname = 'ph_hnsw2';

-- ── Error cases (do not abort the script) ────────────────────────
\set ON_ERROR_STOP off

-- Missing required 'source' option.
CREATE INDEX ph_bad ON ph_test USING pg_cuvs_hnsw (embedding vector_l2_ops);

-- Source exists but is not a cagra index (ph_hnsw uses pg_cuvs_hnsw AM).
CREATE INDEX ph_bad2 ON ph_test USING pg_cuvs_hnsw (embedding vector_l2_ops)
    WITH (source = 'ph_hnsw');

\set ON_ERROR_STOP on

-- Cleanup.
DROP TABLE ph_test CASCADE;
DROP EXTENSION pg_cuvs CASCADE;
