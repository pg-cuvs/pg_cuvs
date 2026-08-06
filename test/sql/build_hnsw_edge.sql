-- build_hnsw_edge.sql — pg_cuvs_build_hnsw edge cases.
--
-- Coverage:
--   1. Small N (< 16): 'nsw' and 'hnsw' modes succeed; hnswlib gracefully handles.
--   2. UNLOGGED table: HNSW inherits UNLOGGED persistence, WAL skipped.
--   3. VACUUM after build: index remains searchable.
--   4. REINDEX after build: pgvector CPU path rebuilds; index remains searchable.
--   5. Error: calling pg_cuvs_build_hnsw on non-existent CAGRA (NOT_FOUND).
--   6. #162: dim-ceiling pre-check rejects BEFORE the GPU/IPC conversion
--      round-trip, for both vector (fp32) and halfvec (fp16) targets.
--
-- NOTE: No \set ON_ERROR_STOP on for Cases 1b, 5, and 6 (expected errors).

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
RESET client_min_messages;
SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- ── Case 1: small index (4 vecs, n_vecs < 16) ───────────────────
-- 'nsw' works for any N (no hnswlib serialization guard).
CREATE TABLE ec_small (id bigint, embedding vector(4));
INSERT INTO ec_small VALUES (1,'[1,0,0,0]'),(2,'[0,1,0,0]'),
                             (3,'[0,0,1,0]'),(4,'[0,0,0,1]');
CREATE INDEX ec_small_cagra ON ec_small USING cagra (embedding vector_l2_ops);

SET client_min_messages = 'warning';
SELECT pg_cuvs_build_hnsw('ec_small_cagra'::regclass, 'nsw') IS NOT NULL AS small_nsw_ok;
SET client_min_messages = 'notice';

SET enable_cuvs = off; SET enable_seqscan = off;
SELECT count(*) AS small_rows
FROM (SELECT id FROM ec_small ORDER BY embedding <-> '[1,0,0,0]' LIMIT 4) s;
RESET enable_cuvs; RESET enable_seqscan;

-- cpu_hnsw_fallback still works for search on small CAGRA index.
SET cuvs.cpu_hnsw_fallback = on;
SET enable_seqscan = off;
SELECT search_mode FROM pg_stat_gpu_search WHERE index_name = 'ec_small_cagra';
RESET cuvs.cpu_hnsw_fallback;
RESET enable_seqscan;

DROP TABLE ec_small;

-- ── Case 2: UNLOGGED table → HNSW inherits UNLOGGED persistence ──
CREATE UNLOGGED TABLE ec_unlogged (id bigint, embedding vector(4));
INSERT INTO ec_unlogged SELECT i, ('[' || (i*0.05) || ',0,0,0]')::vector
                        FROM generate_series(1,20) i;
CREATE INDEX ec_ul_cagra ON ec_unlogged USING cagra (embedding vector_l2_ops);

SET client_min_messages = 'warning';
SELECT pg_cuvs_build_hnsw('ec_ul_cagra'::regclass) IS NOT NULL AS unlogged_ok;
SET client_min_messages = 'notice';

-- Verify the HNSW index is UNLOGGED (relpersistence = 'u').
SELECT relpersistence = 'u' AS is_unlogged
FROM pg_class
WHERE relname LIKE 'pg_cuvs_hnsw%'
  AND relnamespace = (SELECT oid FROM pg_namespace WHERE nspname = current_schema());

SET enable_cuvs = off; SET enable_seqscan = off;
SELECT count(*) AS ul_rows
FROM (SELECT id FROM ec_unlogged ORDER BY embedding <-> '[0.5,0,0,0]' LIMIT 5) s;
RESET enable_cuvs; RESET enable_seqscan;

DROP TABLE ec_unlogged;

-- ── Case 3: VACUUM on GPU-built HNSW ────────────────────────────
CREATE TABLE ec_vacuum (id bigint, embedding vector(4));
INSERT INTO ec_vacuum SELECT i, ('[' || (i*0.05) || ',0,0,0]')::vector
                      FROM generate_series(1,20) i;
CREATE INDEX ec_vac_cagra ON ec_vacuum USING cagra (embedding vector_l2_ops);

SET client_min_messages = 'warning';
SELECT pg_cuvs_build_hnsw('ec_vac_cagra'::regclass) IS NOT NULL AS vac_build_ok;
SET client_min_messages = 'notice';

VACUUM ec_vacuum;

SET enable_cuvs = off; SET enable_seqscan = off;
SELECT count(*) AS post_vacuum_rows
FROM (SELECT id FROM ec_vacuum ORDER BY embedding <-> '[0.5,0,0,0]' LIMIT 5) s;
RESET enable_cuvs; RESET enable_seqscan;

-- ── Case 4: REINDEX rebuilds via pgvector CPU path ───────────────
REINDEX TABLE ec_vacuum;

SET enable_cuvs = off; SET enable_seqscan = off;
SELECT count(*) AS post_reindex_rows
FROM (SELECT id FROM ec_vacuum ORDER BY embedding <-> '[0.5,0,0,0]' LIMIT 5) s;
RESET enable_cuvs; RESET enable_seqscan;

DROP TABLE ec_vacuum;

-- ── Case 5: Error when CAGRA index not loaded in daemon ──────────
-- Create CAGRA, then drop it — pg_cuvs_build_hnsw should fail gracefully.
CREATE TABLE ec_dropped (id bigint, embedding vector(4));
INSERT INTO ec_dropped VALUES (1,'[1,0,0,0]'),(2,'[0,1,0,0]');
CREATE INDEX ec_drop_cagra ON ec_dropped USING cagra (embedding vector_l2_ops);
DROP INDEX ec_drop_cagra;  -- daemon no longer has it loaded

BEGIN;
SAVEPOINT sp_dropped;
-- Expect: ERROR (CAGRA not found / OID lookup fails)
SELECT pg_cuvs_build_hnsw('ec_drop_cagra'::regclass);
ROLLBACK TO SAVEPOINT sp_dropped;
COMMIT;

-- Cleanup.
DROP TABLE ec_dropped;

-- ── Case 6: #162 dim-ceiling pre-check (safe_maxlevel=5) ─────────
-- fp32, M=32 (graph_degree=64): ceiling is 1678 per the issue's table.
-- dim=1900 must be rejected — and rejected BEFORE the HNSW-conversion
-- IPC round-trip (only the source CAGRA build itself pays GPU time).
\set ON_ERROR_STOP off
CREATE TABLE ec_ceiling_f32 (id bigint, embedding vector(1900));
INSERT INTO ec_ceiling_f32
    SELECT i, (SELECT ('[' || string_agg(random()::text, ',') || ']')::vector
               FROM generate_series(1,1900))
    FROM generate_series(1,5) i;
SET client_min_messages = 'warning';
CREATE INDEX ec_ceiling_f32_cagra ON ec_ceiling_f32 USING cagra
    (embedding vector_l2_ops) WITH (graph_degree = 64);
SET client_min_messages = 'notice';
CREATE INDEX ec_ceiling_f32_hnsw ON ec_ceiling_f32 USING pg_cuvs_hnsw
    (embedding vector_l2_ops) WITH (source = 'ec_ceiling_f32_cagra', mode = 'nsw');
DROP TABLE ec_ceiling_f32;

-- halfvec, M=32 (graph_degree=64): ceiling is 3356 per the issue's table.
-- dim=3600 must be rejected the same way.
CREATE TABLE ec_ceiling_hv (id bigint, embedding vector(3600));
INSERT INTO ec_ceiling_hv
    SELECT i, (SELECT ('[' || string_agg(random()::text, ',') || ']')::vector
               FROM generate_series(1,3600))
    FROM generate_series(1,5) i;
SET client_min_messages = 'warning';
CREATE INDEX ec_ceiling_hv_cagra ON ec_ceiling_hv USING cagra
    (embedding vector_l2_ops) WITH (graph_degree = 64);
SET client_min_messages = 'notice';
CREATE INDEX ec_ceiling_hv_hnsw ON ec_ceiling_hv USING pg_cuvs_hnsw
    ((embedding::halfvec(3600)) halfvec_l2_ops)
    WITH (source = 'ec_ceiling_hv_cagra', mode = 'nsw');
DROP TABLE ec_ceiling_hv;
\set ON_ERROR_STOP on
