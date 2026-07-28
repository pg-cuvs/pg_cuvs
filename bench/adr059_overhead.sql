-- ADR-059 overhead measurement: parallel multi-partial build backend overhead.
-- Degenerate (all-equal) vectors are valid for TIMING (invalid for recall).
-- backend overhead = CREATE INDEX wall-clock - daemon GPU build time (journal).
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';
DROP TABLE IF EXISTS bench_500000 CASCADE;

-- 500k x 1024, degenerate (fast gen; identical vectors -> timing only).
CREATE TABLE bench_500000 AS
SELECT id, (SELECT array_agg(random())::real[] FROM generate_series(1,1024))::vector(1024) AS v
FROM generate_series(1, 500000) id;
ALTER TABLE bench_500000 ADD PRIMARY KEY (id);

\timing on
-- Single-corpus (memfd) baseline.
SET max_parallel_maintenance_workers = 0;
\echo '=== build: single (workers=0, memfd) ==='
CREATE INDEX bench_single ON bench_500000 USING cagra (v vector_l2_ops);
DROP INDEX bench_single;

-- Parallel multi-partial (ADR-059 direct multi-H2D).
SET max_parallel_maintenance_workers = 4;
SET min_parallel_table_scan_size = 0;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
\echo '=== build: parallel (workers=4, multi-partial direct H2D) ==='
CREATE INDEX bench_par ON bench_500000 USING cagra (v vector_l2_ops);
DROP INDEX bench_par;
\timing off

DROP TABLE bench_500000 CASCADE;
