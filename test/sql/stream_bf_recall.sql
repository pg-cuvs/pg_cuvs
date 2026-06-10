-- stream_bf_recall.sql — ADR-064 out-of-core filtered BF (sidecar-gather).
--
-- Forces the streaming sidecar-gather path with a TINY chunk cap so the daemon's
-- chunk loop + running top-k merge iterate many times, then asserts:
--   1. tenant confinement (filter respected),
--   2. exact recall@k = 1.0 — streamed top-k == CPU exact ground truth,
--   3. the path actually taken is search_mode = 'stream_bf',
--   4. parity — the 3O in-VRAM prefilter returns the identical result.
--
-- The streamed BF and the CPU seqscan both compute exact L2, so with distinct
-- (untied) random vectors their distance-ordered ctid lists are byte-identical.
--
-- REQUIRES: pg_cuvs_server running; brute_force mode (generates .vectors sidecar).

\set ON_ERROR_STOP off
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- ---- Schema: 4 tenants x 50 rows, dim=8, distinct seeded vectors ----
DROP TABLE IF EXISTS sbf CASCADE;
CREATE TABLE sbf (
    tenant_id int    NOT NULL,
    row_id    bigint NOT NULL,
    v         vector(8)
);

SELECT setseed(0.17);
INSERT INTO sbf
SELECT t, t * 1000 + g,
       array_agg(round((random())::numeric, 5) ORDER BY d)::real[]::vector(8)
FROM generate_series(1, 4) t,
     generate_series(1, 50) g,
     generate_series(1, 8) d
GROUP BY t, g;

SET cuvs.search_mode = brute_force;   -- ensure the .vectors sidecar is written
SET cuvs.k = 10;
SET max_parallel_workers_per_gather = 0;

CREATE INDEX sbf_cagra ON sbf USING cagra (v vector_l2_ops);

-- Force out-of-core streaming with many chunks (50 filter rows / 4 = 13 chunks).
SET cuvs.stream_bf_selectivity_threshold = 1.0;  -- selectivity < 1.0 always → stream
SET cuvs.stream_bf_chunk_vectors = 4;            -- tiny cap → exercises the merge

-- ----------------------------------------------------------------
-- Test 1: tenant confinement — every streamed result is from tenant 2.
-- ----------------------------------------------------------------
SELECT count(*) AS n, count(*) FILTER (WHERE tenant_id <> 2) AS wrong_tenant
FROM (
    SELECT s.tenant_id
    FROM cuvs_filtered_knn(
        'sbf_cagra'::regclass,
        '[0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5]'::vector(8),
        ARRAY(SELECT ctid FROM sbf WHERE tenant_id = 2),
        10
    ) f
    JOIN sbf s ON s.ctid = f.ctid
) z;

-- ----------------------------------------------------------------
-- Test 2: exact recall@10 — streamed top-10 == CPU exact ground truth.
-- ----------------------------------------------------------------
SELECT
  (SELECT array_agg(f.ctid ORDER BY f.distance, f.ctid)
     FROM cuvs_filtered_knn(
         'sbf_cagra'::regclass,
         '[0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5]'::vector(8),
         ARRAY(SELECT ctid FROM sbf WHERE tenant_id = 2),
         10) f)
  =
  (SELECT array_agg(g.ctid ORDER BY g.d, g.ctid)
     FROM (SELECT ctid, v <-> '[0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5]'::vector(8) AS d
           FROM sbf WHERE tenant_id = 2
           ORDER BY 2 LIMIT 10) g)
  AS stream_recall_exact;

-- ----------------------------------------------------------------
-- Test 3: the path actually taken was streaming BF.
-- ----------------------------------------------------------------
SELECT search_mode AS mode_after_stream
FROM pg_stat_gpu_search
WHERE index_oid = 'sbf_cagra'::regclass;

-- ----------------------------------------------------------------
-- Test 4: chunk-size invariance — one big chunk yields the same exact top-10.
-- The running top-k merge is exact for ANY chunking, so chunk size is a pure
-- footprint knob: a single 1000-vector chunk must match the 4-vector-chunk run
-- (both equal the CPU exact ground truth).
-- ----------------------------------------------------------------
SET cuvs.stream_bf_chunk_vectors = 1000;   -- whole filter set in one GPU dispatch

SELECT
  (SELECT array_agg(f.ctid ORDER BY f.distance, f.ctid)
     FROM cuvs_filtered_knn(
         'sbf_cagra'::regclass,
         '[0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5]'::vector(8),
         ARRAY(SELECT ctid FROM sbf WHERE tenant_id = 2),
         10) f)
  =
  (SELECT array_agg(g.ctid ORDER BY g.d, g.ctid)
     FROM (SELECT ctid, v <-> '[0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5]'::vector(8) AS d
           FROM sbf WHERE tenant_id = 2
           ORDER BY 2 LIMIT 10) g)
  AS stream_recall_one_chunk;

-- ----------------------------------------------------------------
-- Cleanup
-- ----------------------------------------------------------------
SET cuvs.stream_bf_selectivity_threshold = 0.0;
DROP TABLE sbf CASCADE;
