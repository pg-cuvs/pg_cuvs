\set ECHO none
\set QUIET 1
\set ON_ERROR_STOP on
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';
SET cuvs.cpu_hnsw_fallback = on;
SET enable_cuvs = off;
SET hnsw.ef_search = 400;

CREATE TABLE hsc_file (id int PRIMARY KEY, v vector(3)) WITH (autovacuum_enabled = off);
INSERT INTO hsc_file
SELECT g, ARRAY[g,g % 7,g % 11]::real[]::vector FROM generate_series(1,100) g;
CREATE INDEX hsc_file_src ON hsc_file USING cagra (v vector_l2_ops);
CREATE INDEX hsc_file_dst ON hsc_file USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = hsc_file_src, mode = 'hnswlib_file');
SET enable_seqscan = off;
SELECT id FROM hsc_file ORDER BY v <-> '[53,4,9]' LIMIT 1;
DROP INDEX hsc_file_dst;
RESET enable_seqscan;

-- The on-disk .tids and .hnsw still have the same count after an in-memory
-- EXTEND. A count comparison between just those two files misses this case.
INSERT INTO hsc_file VALUES (1000, '[500,500,500]');
SELECT extend_count = 1 FROM pg_stat_gpu_search WHERE index_oid = 'hsc_file_src'::regclass;
\set ON_ERROR_STOP off
CREATE INDEX hsc_file_dst ON hsc_file USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = hsc_file_src, mode = 'hnswlib_file');
\set ON_ERROR_STOP on
SELECT to_regclass('hsc_file_dst') IS NULL;
DROP INDEX IF EXISTS hsc_file_dst;

-- The resident export sees that row. It remains a valid conversion source.
CREATE INDEX hsc_file_nsw ON hsc_file USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = hsc_file_src, mode = 'nsw');
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM hsc_file ORDER BY v <-> '[500,500,500]' LIMIT 1;
SELECT id FROM hsc_file ORDER BY v <-> '[500,500,500]' LIMIT 1;
RESET enable_seqscan;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT id FROM hsc_file ORDER BY v <-> '[500,500,500]' LIMIT 1;
RESET enable_indexscan;
RESET enable_bitmapscan;
DROP INDEX hsc_file_nsw;

-- REINDEX TABLE can rebuild the CAGRA source with fallback disabled, leaving
-- no current on-disk HNSW export for the dependent target to import.
REINDEX INDEX hsc_file_src;
CREATE INDEX hsc_file_dst ON hsc_file USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = hsc_file_src, mode = 'hnswlib_file');
RESET cuvs.cpu_hnsw_fallback;
REINDEX TABLE hsc_file;
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM hsc_file ORDER BY v <-> '[500,500,500]' LIMIT 1;
SELECT id FROM hsc_file ORDER BY v <-> '[500,500,500]' LIMIT 1;
DROP TABLE hsc_file;
