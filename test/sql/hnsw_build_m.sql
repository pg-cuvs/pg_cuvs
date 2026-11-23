-- hnsw_build_m.sql: a pg_cuvs_hnsw index built from the heap follows its m reloption. The GPU graph is built with
-- degree 2 * m (the build receipt's graph_degree), and the metapage records that m, which pgvector's inserts read.
-- Without m the build keeps the degree 64 (m 32) it always had. GPU only (the CPU shim cannot build an HNSW graph).
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SELECT setseed(0.42);
CREATE TABLE hbm (id int, v vector(8));
INSERT INTO hbm SELECT g, (SELECT array_agg(random()::real) FROM generate_series(1, 8) d WHERE g > 0)::vector
FROM generate_series(1, 2000) g;
CREATE INDEX hbm_default ON hbm USING pg_cuvs_hnsw (v vector_l2_ops);
CREATE INDEX hbm_m8 ON hbm USING pg_cuvs_hnsw (v vector_l2_ops) WITH (m = 8);
CREATE INDEX hbm_m24 ON hbm USING pg_cuvs_hnsw (v vector_l2_ops) WITH (m = 24);
SELECT i AS index, (pg_cuvs_hnsw_layer_counts(i)).m AS meta_m,
       pg_cuvs_hnsw_build_info(i) ->> 'graph_degree' AS graph_degree
FROM unnest(ARRAY['hbm_default', 'hbm_m8', 'hbm_m24']::regclass[]) i;
-- an empty table writes the metapage itself
CREATE TABLE hbm_empty (id int, v vector(8));
CREATE INDEX hbm_empty_m8 ON hbm_empty USING pg_cuvs_hnsw (v vector_l2_ops) WITH (m = 8);
SELECT (pg_cuvs_hnsw_layer_counts('hbm_empty_m8')).m AS meta_m;
-- REINDEX keeps the m
REINDEX INDEX hbm_m8;
SELECT (pg_cuvs_hnsw_layer_counts('hbm_m8')).m AS meta_m;
-- a row inserted after the build is found through the m 8 index
DROP INDEX hbm_default, hbm_m24;
INSERT INTO hbm VALUES (9001, '[9,9,9,9,9,9,9,9]');
SET enable_seqscan = off;
SELECT id FROM hbm ORDER BY v <-> '[9,9,9,9,9,9,9,9]' LIMIT 1;
RESET enable_seqscan;
DROP TABLE hbm, hbm_empty;
