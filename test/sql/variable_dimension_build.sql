-- Untyped vector columns infer the first dimension without silently dropping rows.
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';
SET max_parallel_maintenance_workers = 0;
CREATE TABLE vdim_seed (id integer, v vector);
INSERT INTO vdim_seed SELECT id, ARRAY[id::real, 0, 0, 0]::vector FROM generate_series(1,32) AS id ORDER BY id;
INSERT INTO vdim_seed VALUES (33, '[0,0,0,0,0]');
CREATE FUNCTION pg_temp.vdim_error(query text, table_name text, index_name text, building boolean) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE
    detail text;
    hint text;
BEGIN
    EXECUTE query;
    RETURN false;
EXCEPTION WHEN data_exception THEN
    GET STACKED DIAGNOSTICS detail = PG_EXCEPTION_DETAIL, hint = PG_EXCEPTION_HINT;
    RETURN SQLERRM LIKE '%expected 4 dimensions, not 5%'
       AND detail LIKE format('Index "%s" on table "%s"%%', index_name, table_name)
       AND CASE WHEN building THEN detail ~ 'index TID \([0-9]+,[0-9]+\)'
                ELSE detail LIKE '% recorded 4 dimensions.' END
       AND hint = 'Use a vector column with a fixed dimension.';
END;
$$;
-- A fresh empty generation must discard the previous build's dimension.
SET cuvs.max_stale_fraction = 1;
SET cuvs.k = 64;
CREATE FUNCTION pg_temp.vdim_empty_plan(table_name regclass, query_vector vector) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE
    plan json;
    hits int;
    stmt text;
BEGIN
    stmt := format('SELECT id FROM %s ORDER BY v <-> %L::vector LIMIT 40', table_name, query_vector);
    PERFORM set_config('enable_seqscan', 'off', false);
    EXECUTE 'EXPLAIN (FORMAT JSON) ' || stmt INTO plan;
    IF plan::text LIKE '%Seq Scan%' THEN
        EXECUTE 'SELECT count(*) FROM (' || stmt || ') AS q' INTO hits;
    END IF;
    PERFORM set_config('enable_seqscan', 'on', false);
    RETURN coalesce(hits = 0, false);
END;
$$;
CREATE FUNCTION pg_temp.vdim_reload(table_name regclass, dim int) RETURNS boolean LANGUAGE plpgsql AS $$
BEGIN
    EXECUTE format('INSERT INTO %s SELECT id, array_append(array_fill(0::real, ARRAY[%s]), id::real)::vector
                    FROM generate_series(1, 32) AS id', table_name, dim - 1);
    RETURN true;
EXCEPTION WHEN data_exception THEN
    RETURN false;
END;
$$;
CREATE FUNCTION pg_temp.vdim_same_rows(table_name regclass, index_name regclass,
                                       query_vector vector, predicate text DEFAULT 'true')
RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE
    stmt text;
    exact_ids int[];
    index_ids int[];
    searches_before bigint;
    searches_after bigint;
BEGIN
    stmt := format('SELECT array_agg(id ORDER BY id) FROM
                    (SELECT id FROM %s WHERE %s ORDER BY v <-> %L::vector LIMIT 40) AS q',
                   table_name, predicate, query_vector);
    PERFORM set_config('enable_cuvs', 'off', false);
    EXECUTE stmt INTO exact_ids;
    PERFORM set_config('enable_cuvs', 'on', false);
    PERFORM set_config('enable_seqscan', 'off', false);
    SELECT search_count INTO searches_before FROM pg_stat_gpu_search WHERE index_oid = index_name;
    EXECUTE stmt INTO index_ids;
    SELECT search_count INTO searches_after FROM pg_stat_gpu_search WHERE index_oid = index_name;
    PERFORM set_config('enable_seqscan', 'on', false);
    RETURN index_ids = exact_ids AND searches_after > searches_before;
END;
$$;
CREATE TABLE vdim_cagra_mixed AS SELECT * FROM vdim_seed ORDER BY id;
SELECT pg_temp.vdim_error('CREATE INDEX vdim_cagra_mixed_i ON vdim_cagra_mixed USING cagra (v vector_l2_ops)', 'vdim_cagra_mixed', 'vdim_cagra_mixed_i', true);
CREATE TABLE vdim_cagra_valid AS SELECT * FROM vdim_seed WHERE id <= 32 ORDER BY id;
CREATE INDEX vdim_cagra_i ON vdim_cagra_valid USING cagra (v vector_l2_ops);
SELECT n_vecs = 32 FROM pg_stat_gpu_search WHERE index_oid = 'vdim_cagra_i'::regclass;
SELECT pg_temp.vdim_error('INSERT INTO vdim_cagra_valid VALUES (33, ''[0,0,0,0,0]'')', 'vdim_cagra_valid', 'vdim_cagra_i', false);
SELECT count(*) = 32 FROM vdim_cagra_valid;
SELECT pg_temp.vdim_same_rows('vdim_cagra_valid', 'vdim_cagra_i', '[0,0,0,0]');
INSERT INTO vdim_cagra_valid VALUES (34, '[0,0,0,0]');
SELECT count(*) = 33 FROM vdim_cagra_valid;
SELECT pg_temp.vdim_same_rows('vdim_cagra_valid', 'vdim_cagra_i', '[0,0,0,0]');
BEGIN;
SAVEPOINT before_truncate;
TRUNCATE vdim_cagra_valid;
SELECT pg_temp.vdim_empty_plan('vdim_cagra_valid', '[0,0,0,0]');
ROLLBACK TO before_truncate;
SELECT pg_temp.vdim_error('INSERT INTO vdim_cagra_valid VALUES (35, ''[0,0,0,0,0]'')', 'vdim_cagra_valid', 'vdim_cagra_i', false);
COMMIT;
TRUNCATE vdim_cagra_valid;
SELECT pg_temp.vdim_empty_plan('vdim_cagra_valid', '[0,0,0,0]');
SELECT pg_temp.vdim_reload('vdim_cagra_valid', 6);
REINDEX INDEX vdim_cagra_i;
SELECT pg_temp.vdim_same_rows('vdim_cagra_valid', 'vdim_cagra_i', '[0,0,0,0,0,0]');
SELECT n_vecs = 32 FROM pg_stat_gpu_search WHERE index_oid = 'vdim_cagra_i'::regclass;
DELETE FROM vdim_cagra_valid;
REINDEX INDEX vdim_cagra_i;
SELECT pg_temp.vdim_empty_plan('vdim_cagra_valid', '[1,2,3,4,5,6]');
SELECT pg_temp.vdim_reload('vdim_cagra_valid', 4);
REINDEX INDEX vdim_cagra_i;
SELECT pg_temp.vdim_same_rows('vdim_cagra_valid', 'vdim_cagra_i', '[0,0,0,0]');
SELECT n_vecs = 32 FROM pg_stat_gpu_search WHERE index_oid = 'vdim_cagra_i'::regclass;
INSERT INTO vdim_cagra_mixed VALUES (0, NULL);
CREATE INDEX vdim_cagra_mixed_partial ON vdim_cagra_mixed USING cagra (v vector_l2_ops)
    WHERE id <= 32 AND v IS NOT NULL;
SELECT pg_temp.vdim_same_rows('vdim_cagra_mixed', 'vdim_cagra_mixed_partial', '[0,0,0,0]', 'id <= 32 AND v IS NOT NULL');
DROP TABLE vdim_cagra_mixed, vdim_cagra_valid;
CREATE TABLE vdim_flat_mixed AS SELECT * FROM vdim_seed ORDER BY id;
SELECT pg_temp.vdim_error('CREATE INDEX vdim_flat_mixed_i ON vdim_flat_mixed USING flat (v vector_l2_ops)', 'vdim_flat_mixed', 'vdim_flat_mixed_i', true);
CREATE TABLE vdim_flat_valid AS SELECT * FROM vdim_seed WHERE id <= 32 ORDER BY id;
CREATE INDEX vdim_flat_i ON vdim_flat_valid USING flat (v vector_l2_ops);
SELECT n_vecs = 32 FROM pg_stat_gpu_search WHERE index_oid = 'vdim_flat_i'::regclass;
SELECT pg_temp.vdim_error('INSERT INTO vdim_flat_valid VALUES (33, ''[0,0,0,0,0]'')', 'vdim_flat_valid', 'vdim_flat_i', false);
SELECT count(*) = 32 FROM vdim_flat_valid;
SELECT pg_temp.vdim_same_rows('vdim_flat_valid', 'vdim_flat_i', '[0,0,0,0]');
INSERT INTO vdim_flat_valid VALUES (34, '[0,0,0,0]');
SELECT count(*) = 33 FROM vdim_flat_valid;
SELECT pg_temp.vdim_same_rows('vdim_flat_valid', 'vdim_flat_i', '[0,0,0,0]');
BEGIN;
SAVEPOINT before_truncate;
TRUNCATE vdim_flat_valid;
SELECT pg_temp.vdim_empty_plan('vdim_flat_valid', '[0,0,0,0]');
ROLLBACK TO before_truncate;
SELECT pg_temp.vdim_error('INSERT INTO vdim_flat_valid VALUES (35, ''[0,0,0,0,0]'')', 'vdim_flat_valid', 'vdim_flat_i', false);
COMMIT;
TRUNCATE vdim_flat_valid;
SELECT pg_temp.vdim_empty_plan('vdim_flat_valid', '[0,0,0,0]');
SELECT pg_temp.vdim_reload('vdim_flat_valid', 6);
REINDEX INDEX vdim_flat_i;
SELECT pg_temp.vdim_same_rows('vdim_flat_valid', 'vdim_flat_i', '[0,0,0,0,0,0]');
SELECT n_vecs = 32 FROM pg_stat_gpu_search WHERE index_oid = 'vdim_flat_i'::regclass;
DELETE FROM vdim_flat_valid;
REINDEX INDEX vdim_flat_i;
SELECT pg_temp.vdim_empty_plan('vdim_flat_valid', '[1,2,3,4,5,6]');
SELECT pg_temp.vdim_reload('vdim_flat_valid', 4);
REINDEX INDEX vdim_flat_i;
SELECT pg_temp.vdim_same_rows('vdim_flat_valid', 'vdim_flat_i', '[0,0,0,0]');
SELECT n_vecs = 32 FROM pg_stat_gpu_search WHERE index_oid = 'vdim_flat_i'::regclass;
INSERT INTO vdim_flat_mixed VALUES (0, NULL);
CREATE INDEX vdim_flat_mixed_partial ON vdim_flat_mixed USING flat (v vector_l2_ops)
    WHERE id <= 32 AND v IS NOT NULL;
SELECT pg_temp.vdim_same_rows('vdim_flat_mixed', 'vdim_flat_mixed_partial', '[0,0,0,0]', 'id <= 32 AND v IS NOT NULL');
DROP TABLE vdim_flat_mixed, vdim_flat_valid;
CREATE TABLE vdim_ivfpq_mixed AS SELECT * FROM vdim_seed ORDER BY id;
SELECT pg_temp.vdim_error('CREATE INDEX vdim_ivfpq_mixed_i ON vdim_ivfpq_mixed USING ivfpq (v vector_l2_ops) WITH (n_lists=1, pq_bits=4, pq_dim=2)', 'vdim_ivfpq_mixed', 'vdim_ivfpq_mixed_i', true);
CREATE TABLE vdim_ivfpq_valid AS SELECT * FROM vdim_seed WHERE id <= 32 ORDER BY id;
CREATE INDEX vdim_ivfpq_i ON vdim_ivfpq_valid USING ivfpq (v vector_l2_ops) WITH (n_lists=1, pq_bits=4, pq_dim=2);
SELECT n_vecs = 32 FROM pg_stat_gpu_search WHERE index_oid = 'vdim_ivfpq_i'::regclass;
SELECT pg_temp.vdim_error('INSERT INTO vdim_ivfpq_valid VALUES (33, ''[0,0,0,0,0]'')', 'vdim_ivfpq_valid', 'vdim_ivfpq_i', false);
SELECT count(*) = 32 FROM vdim_ivfpq_valid;
SELECT pg_temp.vdim_same_rows('vdim_ivfpq_valid', 'vdim_ivfpq_i', '[0,0,0,0]');
INSERT INTO vdim_ivfpq_valid VALUES (34, '[0,0,0,0]');
REINDEX INDEX vdim_ivfpq_i;
SELECT count(*) = 33 FROM vdim_ivfpq_valid;
SELECT pg_temp.vdim_same_rows('vdim_ivfpq_valid', 'vdim_ivfpq_i', '[0,0,0,0]');
BEGIN;
SAVEPOINT before_truncate;
TRUNCATE vdim_ivfpq_valid;
SELECT pg_temp.vdim_empty_plan('vdim_ivfpq_valid', '[0,0,0,0]');
ROLLBACK TO before_truncate;
SELECT pg_temp.vdim_error('INSERT INTO vdim_ivfpq_valid VALUES (35, ''[0,0,0,0,0]'')', 'vdim_ivfpq_valid', 'vdim_ivfpq_i', false);
COMMIT;
TRUNCATE vdim_ivfpq_valid;
SELECT pg_temp.vdim_empty_plan('vdim_ivfpq_valid', '[0,0,0,0]');
SELECT pg_temp.vdim_reload('vdim_ivfpq_valid', 6);
REINDEX INDEX vdim_ivfpq_i;
SELECT pg_temp.vdim_same_rows('vdim_ivfpq_valid', 'vdim_ivfpq_i', '[0,0,0,0,0,0]');
SELECT n_vecs = 32 FROM pg_stat_gpu_search WHERE index_oid = 'vdim_ivfpq_i'::regclass;
DELETE FROM vdim_ivfpq_valid;
REINDEX INDEX vdim_ivfpq_i;
SELECT pg_temp.vdim_empty_plan('vdim_ivfpq_valid', '[1,2,3,4,5,6]');
SELECT pg_temp.vdim_reload('vdim_ivfpq_valid', 4);
REINDEX INDEX vdim_ivfpq_i;
SELECT pg_temp.vdim_same_rows('vdim_ivfpq_valid', 'vdim_ivfpq_i', '[0,0,0,0]');
SELECT n_vecs = 32 FROM pg_stat_gpu_search WHERE index_oid = 'vdim_ivfpq_i'::regclass;
INSERT INTO vdim_ivfpq_mixed VALUES (0, NULL);
CREATE INDEX vdim_ivfpq_mixed_partial ON vdim_ivfpq_mixed USING ivfpq (v vector_l2_ops) WITH (n_lists=1, pq_bits=4, pq_dim=2)
    WHERE id <= 32 AND v IS NOT NULL;
SELECT pg_temp.vdim_same_rows('vdim_ivfpq_mixed', 'vdim_ivfpq_mixed_partial', '[0,0,0,0]', 'id <= 32 AND v IS NOT NULL');
DROP TABLE vdim_ivfpq_mixed, vdim_ivfpq_valid;
DROP TABLE vdim_seed;
