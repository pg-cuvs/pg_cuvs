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
\pset format unaligned
\pset tuples_only on
-- ALTER stores the next heap-build setting; the live index still has its old M.
ALTER INDEX hbm_m8 SET (m = 24);
SELECT (pg_cuvs_hnsw_layer_counts('hbm_m8')).m = 8 AS before_reindex;
REINDEX INDEX hbm_m8;
SELECT (pg_cuvs_hnsw_layer_counts('hbm_m8')).m = 24 AS after_reindex;

-- Reuse the source's actual graph degree when m is omitted; an explicit m
-- (including 32, the heap default) has to match the converted graph.
CREATE INDEX hbm_c32 ON hbm USING cagra (v vector_l2_ops) WITH (graph_degree = 32);
CREATE INDEX hbm_c80 ON hbm USING cagra (v vector_l2_ops)
    WITH (graph_degree = 80, intermediate_graph_degree = 128);
CREATE INDEX hbm_source ON hbm USING pg_cuvs_hnsw (v vector_l2_ops) WITH (source = 'hbm_c32');
CREATE INDEX hbm_source_match ON hbm USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = 'hbm_c32', m = 16, mode = 'hnsw');
CREATE INDEX hbm_source_cap ON hbm USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = 'hbm_c80', m = 32);
CREATE INDEX hbm_source_legacy ON hbm USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = 'hbm_c32', m = 16, mode = 'hnswlib');
SELECT bool_and((pg_cuvs_hnsw_layer_counts(idx)).m = m) AS source_m_matches
FROM (VALUES ('hbm_source'::regclass, 16), ('hbm_source_match', 16),
             ('hbm_source_cap', 32), ('hbm_source_legacy', 16)) AS expected(idx, m);

\set VERBOSITY terse
CREATE INDEX hbm_bad_default ON hbm USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = 'hbm_c32', m = 32);
CREATE INDEX hbm_bad_cap ON hbm USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = 'hbm_c80', m = 33);
CREATE INDEX hbm_bad_legacy ON hbm USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = 'hbm_c32', m = 32, mode = 'hnswlib');
ALTER INDEX hbm_source SET (m = 32);
REINDEX INDEX hbm_source;
\set VERBOSITY default
SELECT (pg_cuvs_hnsw_layer_counts('hbm_source')).m = 16 AS after_failed_reindex;
ALTER INDEX hbm_source RESET (m);
REINDEX INDEX hbm_source;
SELECT (pg_cuvs_hnsw_layer_counts('hbm_source')).m = 16 AS after_reset;

-- Small builds keep the configured slot capacity even when the graph is narrower.
DO $$
DECLARE
    sample record;
    receipt jsonb;
    actual_m integer;
    nearest integer;
BEGIN
    FOR sample IN
        SELECT n, 24 AS m, greatest(4, n - n % 2) AS degree FROM generate_series(1, 15) n
        UNION ALL SELECT 15, 2, 4
        UNION ALL SELECT 32, 24, 30
    LOOP
        CREATE TABLE hbm_small (id integer, v vector(8));
        INSERT INTO hbm_small
        SELECT g, ARRAY[g::real, 0, 0, 0, 0, 0, 0, 0]::vector
        FROM generate_series(1, sample.n) g;
        EXECUTE format('CREATE INDEX hbm_small_i ON hbm_small USING pg_cuvs_hnsw (v vector_l2_ops) WITH (m = %s)', sample.m);
        SELECT (pg_cuvs_hnsw_layer_counts('hbm_small_i')).m,
               pg_cuvs_hnsw_build_info('hbm_small_i') INTO actual_m, receipt;
        IF actual_m IS DISTINCT FROM sample.m
           OR (receipt ->> 'graph_degree')::integer IS DISTINCT FROM sample.degree
           OR (receipt ->> 'graph_degree_limited_by') IS DISTINCT FROM
               (CASE WHEN sample.degree < 2 * sample.m THEN 'input_rows' END) THEN
            RAISE EXCEPTION 'n=%, m=%, metadata M=%, graph degree=%, limit=%',
                sample.n, sample.m, actual_m, receipt ->> 'graph_degree', receipt ->> 'graph_degree_limited_by';
        END IF;

        INSERT INTO hbm_small VALUES (100, '[100,0,0,0,0,0,0,0]');
        PERFORM set_config('enable_seqscan', 'off', true);
        SELECT id INTO nearest FROM hbm_small ORDER BY v <-> '[100,0,0,0,0,0,0,0]' LIMIT 1;
        IF nearest IS DISTINCT FROM 100 THEN
            RAISE EXCEPTION 'n=%, m=%: inserted row was not found', sample.n, sample.m;
        END IF;
        DROP TABLE hbm_small;
    END LOOP;
END $$;
DROP TABLE hbm, hbm_empty;
