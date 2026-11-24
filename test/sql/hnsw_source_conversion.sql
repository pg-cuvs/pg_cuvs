\set ECHO none
\set QUIET 1
\set ON_ERROR_STOP on
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';
SET enable_cuvs = off;
SET hnsw.ef_search = 400;

-- Probe an accepted conversion with its actual target scan and a sequential
-- oracle. A refusal only passes if it describes the source state and has a hint.
CREATE FUNCTION pg_temp.check_conversion(command text, target_name text,
                                         probe text, refusal text DEFAULT NULL)
RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE
    target regclass;
    exact_id integer;
    found_id integer;
    plan text;
    hint text;
    target_used boolean := false;
BEGIN
    BEGIN
        IF target_name IS NULL THEN
            EXECUTE command INTO target;
        ELSE
            EXECUTE command;
            target := target_name::regclass;
        END IF;
    EXCEPTION WHEN object_not_in_prerequisite_state OR invalid_table_definition THEN
        GET STACKED DIAGNOSTICS hint = PG_EXCEPTION_HINT;
        RETURN refusal IS NOT NULL AND position(refusal IN SQLERRM) > 0
               AND coalesce(hint, '') <> '';
    WHEN OTHERS THEN
        RAISE WARNING 'conversion failed for another reason (%): %', SQLSTATE, SQLERRM;
        RETURN false;
    END;

    PERFORM set_config('enable_seqscan', 'on', true);
    PERFORM set_config('enable_indexscan', 'off', true);
    PERFORM set_config('enable_bitmapscan', 'off', true);
    EXECUTE probe INTO STRICT exact_id;
    PERFORM set_config('enable_indexscan', 'on', true);
    PERFORM set_config('enable_seqscan', 'off', true);
    FOR plan IN EXECUTE 'EXPLAIN (COSTS OFF) ' || probe LOOP
        target_used := target_used OR position('Index Scan using ' || target::text || ' ' IN plan) > 0;
    END LOOP;
    IF NOT target_used THEN
        RAISE WARNING 'query did not scan the converted index %', target;
        RETURN false;
    END IF;
    EXECUTE probe INTO STRICT found_id;
    EXECUTE format('DROP INDEX %s', target);
    IF found_id <> exact_id THEN
        RAISE WARNING 'source conversion returned id %, exact id %', found_id, exact_id;
        RETURN false;
    END IF;
    RETURN refusal IS NULL;
END;
$$;

-- A. Leave a real pending row without stopping the shared test daemon.
CREATE TABLE hsc_delta (id int PRIMARY KEY, v vector(3)) WITH (autovacuum_enabled = off);
INSERT INTO hsc_delta
SELECT g, ARRAY[g, g % 7, g % 11]::real[]::vector FROM generate_series(1,100) g;
SET cuvs.cpu_hnsw_fallback = on;
CREATE INDEX hsc_delta_src ON hsc_delta USING cagra (v vector_l2_ops);
RESET cuvs.cpu_hnsw_fallback;
SET cuvs.socket_path = '';
INSERT INTO hsc_delta VALUES (1000, '[500,500,500]');
RESET cuvs.socket_path;

SELECT (pg_stat_file(format('%s/%s_%s.delta', current_setting('cuvs.index_dir'),
       (SELECT oid FROM pg_database WHERE datname = current_database()),
       'hsc_delta_src'::regclass::oid))).size > 32 AS pending_on_disk;
SELECT mode, pg_temp.check_conversion(
    format('CREATE INDEX hsc_delta_dst ON hsc_delta USING pg_cuvs_hnsw (v vector_l2_ops) WITH (source = hsc_delta_src, mode = %L)', mode),
    'hsc_delta_dst', 'SELECT id FROM hsc_delta ORDER BY v <-> ''[500,500,500]'' LIMIT 1',
    'pending inserts') AS ddl_rejected,
    pg_temp.check_conversion(format('SELECT pg_cuvs_build_hnsw(''hsc_delta_src'', %L)', mode),
    NULL, 'SELECT id FROM hsc_delta ORDER BY v <-> ''[500,500,500]'' LIMIT 1',
    'pending inserts') AS function_rejected
FROM unnest(ARRAY['nsw', 'hnsw', 'hnswlib', 'hnswlib_file']) mode;
\set ON_ERROR_STOP off
CREATE INDEX CONCURRENTLY hsc_delta_cic ON hsc_delta USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = hsc_delta_src);
\set ON_ERROR_STOP on
SELECT indisvalid AS concurrent_index_valid FROM pg_index
WHERE indexrelid = 'hsc_delta_cic'::regclass;
DROP TABLE hsc_delta;

-- B. The function retains the source predicate; DDL must match it.
CREATE TABLE hsc_partial (id int PRIMARY KEY, v vector(3), flag bool)
    WITH (autovacuum_enabled = off);
INSERT INTO hsc_partial
SELECT g, ARRAY[g, g % 7, g % 11]::real[]::vector, g % 2 = 0 FROM generate_series(1,100) g;
CREATE INDEX hsc_partial_src ON hsc_partial USING cagra (v vector_l2_ops) WHERE flag;
SELECT pg_cuvs_build_hnsw('hsc_partial_src', 'nsw')::regclass AS partial_target \gset
SELECT indpred IS NOT NULL AS predicate_preserved FROM pg_index
WHERE indexrelid = :'partial_target'::regclass;
SET enable_seqscan = off;
SELECT id AS unqualified_id FROM hsc_partial ORDER BY v <-> '[51,2,7]' LIMIT 1;
RESET enable_seqscan;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT id AS exact_unqualified_id FROM hsc_partial ORDER BY v <-> '[51,2,7]' LIMIT 1;
RESET enable_indexscan;
RESET enable_bitmapscan;
DROP INDEX :partial_target;
SELECT pg_temp.check_conversion(
    'CREATE INDEX hsc_partial_dst ON hsc_partial USING pg_cuvs_hnsw (v vector_l2_ops) WITH (source = hsc_partial_src)',
    'hsc_partial_dst', 'SELECT id FROM hsc_partial ORDER BY v <-> ''[51,2,7]'' LIMIT 1',
    'predicate') AS mismatch_rejected;
SELECT pg_temp.check_conversion(
    'CREATE INDEX hsc_partial_dst ON hsc_partial USING pg_cuvs_hnsw (v vector_l2_ops) WITH (source = hsc_partial_src) WHERE flag',
    'hsc_partial_dst', 'SELECT id FROM hsc_partial WHERE flag ORDER BY v <-> ''[52,3,8]'' LIMIT 1')
    AS matching_predicate_accepted;
DROP TABLE hsc_partial;

-- The same-width vector columns are different source keys.
CREATE TABLE hsc_key (id int PRIMARY KEY, v vector(3), w vector(3))
    WITH (autovacuum_enabled = off);
INSERT INTO hsc_key
SELECT g, ARRAY[g,0,0]::real[]::vector, ARRAY[101-g,0,0]::real[]::vector
FROM generate_series(1,100) g;
CREATE INDEX hsc_key_src ON hsc_key USING cagra (v vector_l2_ops);
SELECT pg_temp.check_conversion(
    'CREATE INDEX hsc_key_dst ON hsc_key USING pg_cuvs_hnsw (w vector_l2_ops) WITH (source = hsc_key_src)',
    'hsc_key_dst', 'SELECT id FROM hsc_key ORDER BY w <-> ''[1,0,0]'' LIMIT 1',
    'index key') AS different_key_rejected;
DROP TABLE hsc_key;

-- Exercise an expression key in addition to the same-width column mismatch.
CREATE TABLE hsc_expr (id int PRIMARY KEY, v vector(3));
INSERT INTO hsc_expr SELECT g, ARRAY[g,0,0]::real[]::vector FROM generate_series(1,100) g;
CREATE INDEX hsc_expr_src ON hsc_expr USING cagra (((v + '[1,0,0]')::vector(3)) vector_l2_ops);
SELECT pg_temp.check_conversion(
    'SELECT pg_cuvs_build_hnsw(''hsc_expr_src'', ''nsw'')', NULL,
    'SELECT id FROM hsc_expr ORDER BY (v + ''[1,0,0]'')::vector(3) <-> ''[60,0,0]'' LIMIT 1')
    AS function_preserved_expression;
SELECT pg_temp.check_conversion(
    'CREATE INDEX hsc_expr_dst ON hsc_expr USING pg_cuvs_hnsw (((v + ''[2,0,0]'')::vector(3)) vector_l2_ops) WITH (source = hsc_expr_src)',
    'hsc_expr_dst', 'SELECT id FROM hsc_expr ORDER BY (v + ''[2,0,0]'')::vector(3) <-> ''[60,0,0]'' LIMIT 1',
    'index key') AS different_expression_rejected;
DROP TABLE hsc_expr;

-- A small delete batch recorded by VACUUM must be rejected before import.
CREATE TABLE hsc_tomb (id int PRIMARY KEY, v vector(3)) WITH (autovacuum_enabled = off);
INSERT INTO hsc_tomb SELECT g, ARRAY[g,0,0]::real[]::vector FROM generate_series(1,100) g;
CREATE INDEX hsc_tomb_src ON hsc_tomb USING cagra (v vector_l2_ops);
DELETE FROM hsc_tomb WHERE id = 50;
SET cuvs.socket_path = '';
VACUUM (INDEX_CLEANUP ON, TRUNCATE OFF) hsc_tomb;
RESET cuvs.socket_path;
SELECT (pg_stat_file(format('%s/%s_%s.tombstone', current_setting('cuvs.index_dir'),
       (SELECT oid FROM pg_database WHERE datname = current_database()),
       'hsc_tomb_src'::regclass::oid))).size > 32 AS tombstone_on_disk;
SELECT pg_temp.check_conversion(
    'CREATE INDEX hsc_tomb_dst ON hsc_tomb USING pg_cuvs_hnsw (v vector_l2_ops) WITH (source = hsc_tomb_src)',
    'hsc_tomb_dst', 'SELECT id FROM hsc_tomb ORDER BY v <-> ''[51,0,0]'' LIMIT 1',
    'pending deletes') AS tombstone_rejected;
DROP TABLE hsc_tomb;

-- C. Reuse dead heap slots while the source still has the old vectors.
SET cuvs.max_delta_rows = 10000;
CREATE TABLE hsc_stale (id int PRIMARY KEY, v vector(8)) WITH (autovacuum_enabled = off);
DO $$ BEGIN PERFORM setseed(0.1); END $$;
INSERT INTO hsc_stale
SELECT g, (SELECT array_agg(random()::real) FROM generate_series(1,8) WHERE g > 0)::vector(8)
FROM generate_series(1,12000) g;
CREATE INDEX hsc_stale_src ON hsc_stale USING cagra (v vector_l2_ops);
CREATE TEMP TABLE hsc_old AS SELECT id, ctid AS old_tid, v FROM hsc_stale WHERE id <= 10001;
DELETE FROM hsc_stale WHERE id <= 10001;
VACUUM (INDEX_CLEANUP ON, TRUNCATE OFF) hsc_stale;
INSERT INTO hsc_stale
SELECT g + 50000, (SELECT array_agg(random()::real) FROM generate_series(1,8) WHERE g > 0)::vector(8)
FROM generate_series(1,10001) g;
SELECT count(*) > 0 AS reused_heap_slot
FROM hsc_stale t JOIN hsc_old o ON t.ctid = o.old_tid WHERE t.id > 50000;
SELECT EXISTS (
    SELECT FROM pg_stat_file(format('%s/%s_%s.stale', current_setting('cuvs.index_dir'),
            (SELECT oid FROM pg_database WHERE datname = current_database()),
            'hsc_stale_src'::regclass::oid), true) WHERE size IS NOT NULL
) AS stale_on_disk;
SELECT format('SELECT id FROM hsc_stale ORDER BY v <-> %L LIMIT 1', o.v) AS stale_probe
FROM hsc_old o JOIN hsc_stale t ON t.ctid = o.old_tid WHERE t.id > 50000 ORDER BY o.id LIMIT 1 \gset
SELECT pg_temp.check_conversion(
    'CREATE INDEX hsc_stale_dst ON hsc_stale USING pg_cuvs_hnsw (v vector_l2_ops) WITH (source = hsc_stale_src)',
    'hsc_stale_dst', :'stale_probe', 'is stale') AS stale_ddl_rejected,
    pg_temp.check_conversion('SELECT pg_cuvs_build_hnsw(''hsc_stale_src'', ''nsw'')',
    NULL, :'stale_probe', 'is stale') AS stale_function_rejected;
\set ON_ERROR_STOP off
CREATE INDEX CONCURRENTLY hsc_stale_cic ON hsc_stale USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = hsc_stale_src);
\set ON_ERROR_STOP on
SELECT indisvalid AS concurrent_index_valid FROM pg_index
WHERE indexrelid = 'hsc_stale_cic'::regclass;
DROP TABLE hsc_stale;
