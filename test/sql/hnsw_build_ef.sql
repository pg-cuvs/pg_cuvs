-- The SQL option is stored in the metapage. The GPU receipt records effective construction settings.
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
CREATE EXTENSION IF NOT EXISTS pageinspect;
SET cuvs.index_dir = '/tmp/cuvs_indexes';
SET max_parallel_maintenance_workers = 0;
SELECT setseed(0.453);
CREATE TABLE hef (id integer, v vector(8));
INSERT INTO hef SELECT g, (SELECT array_agg(random()::real) FROM generate_series(1, 8) d WHERE g > 0)::vector
FROM generate_series(1, 2000) g;
CREATE TABLE hef_tiny AS SELECT * FROM hef WHERE id <= 12;
CREATE TABLE hef_empty (v vector(8));
DO $$
DECLARE
    invalid_ef int;
BEGIN
    FOREACH invalid_ef IN ARRAY ARRAY[3,1001] LOOP
        BEGIN
            EXECUTE format('CREATE INDEX ON hef_empty USING pg_cuvs_hnsw (v) WITH (ef_construction = %s)',
                           invalid_ef);
            RAISE EXCEPTION 'ef_construction % was accepted', invalid_ef;
        EXCEPTION WHEN invalid_parameter_value THEN
            IF position('ef_construction' IN SQLERRM) = 0 THEN
                RAISE;
            END IF;
        END;
    END LOOP;
END;
$$;
CREATE FUNCTION pg_temp.hef_meta(idx text) RETURNS integer LANGUAGE sql AS $$
    SELECT get_byte(p, 38) + 256 * get_byte(p, 39) FROM (SELECT get_raw_page(idx, 0) AS p) raw
    WHERE get_byte(p, 24) = 83 AND get_byte(p, 25) = 169 AND get_byte(p, 26) = 83 AND get_byte(p, 27) = 169;
$$;
SET client_min_messages = notice;
CREATE INDEX hef_ivf ON hef USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (m = 32, ef_construction = 1000, build_algo = 'ivf_pq');
SET client_min_messages = warning;
SELECT pg_temp.hef_meta('hef_ivf');
SELECT pg_cuvs_hnsw_build_info('hef_ivf') ->> 'intermediate_degree';
CREATE INDEX hef_nn ON hef USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (m = 32, ef_construction = 1000, build_algo = 'nn_descent');
SELECT pg_temp.hef_meta('hef_nn');
SELECT pg_cuvs_hnsw_build_info('hef_nn') ->> 'intermediate_degree';
CREATE INDEX hef_ace ON hef USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (m = 32, ef_construction = 1000, build_algo = 'ace');
SELECT pg_temp.hef_meta('hef_ace');
SELECT concat_ws('|', info->>'ace_ef_construction', info->>'ace_build_quality',
                 info->>'ace_local_intermediate_degree', info->>'ace_local_nn_descent_iterations')
FROM (SELECT pg_cuvs_hnsw_build_info('hef_ace') AS info) AS receipt;
CREATE INDEX hef_low ON hef USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (m = 32, ef_construction = 4, build_algo = 'ivf_pq');
SELECT pg_temp.hef_meta('hef_low');
SELECT pg_cuvs_hnsw_build_info('hef_low') ->> 'intermediate_degree';
CREATE INDEX hef_tiny_i ON hef_tiny USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (ef_construction = 4);
SELECT pg_temp.hef_meta('hef_tiny_i');
CREATE INDEX hef_default_i ON hef_tiny USING pg_cuvs_hnsw (v vector_l2_ops);
SELECT pg_temp.hef_meta('hef_default_i');
CREATE INDEX hef_empty_i ON hef_empty USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (ef_construction = 1000);
SELECT pg_temp.hef_meta('hef_empty_i');
SET cuvs.cpu_hnsw_fallback = on;
CREATE INDEX hef_cagra ON hef USING cagra (v vector_l2_ops) WITH (graph_degree = 32);
SET cuvs.cpu_hnsw_fallback = off;
CREATE INDEX hef_nsw ON hef USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = 'hef_cagra', ef_construction = 4, mode = 'nsw');
SELECT pg_temp.hef_meta('hef_nsw');
CREATE INDEX hef_hnsw ON hef USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = 'hef_cagra', ef_construction = 1000, mode = 'hnsw');
SELECT pg_temp.hef_meta('hef_hnsw');
CREATE INDEX hef_shm ON hef USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = 'hef_cagra', ef_construction = 42, mode = 'hnswlib');
SELECT pg_temp.hef_meta('hef_shm');
CREATE INDEX hef_file ON hef USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = 'hef_cagra', ef_construction = 999, mode = 'hnswlib_file');
SELECT pg_temp.hef_meta('hef_file');
CREATE INDEX hef_source_default ON hef USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = 'hef_cagra');
SELECT pg_temp.hef_meta('hef_source_default');
ALTER INDEX hef_empty_i SET (ef_construction = 4);
SELECT pg_temp.hef_meta('hef_empty_i');
REINDEX INDEX hef_empty_i;
SELECT pg_temp.hef_meta('hef_empty_i');
DROP TABLE hef, hef_tiny, hef_empty;
