\set ECHO none
\set QUIET 1
\set ON_ERROR_STOP on
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';
SET hnsw.ef_search = 400;

CREATE FUNCTION pg_temp.correct_neighbor(rel regclass, probe text)
RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE
    stmt text := format('SELECT id FROM %s ORDER BY v <-> %L LIMIT 1', rel, probe);
    idx text := rel::text || '_h';
    line text;
    idx_seen boolean := false;
    exact_id integer;
    actual_id integer;
BEGIN
    PERFORM set_config('enable_cuvs', 'off', true);
    PERFORM set_config('enable_indexscan', 'off', true);
    PERFORM set_config('enable_bitmapscan', 'off', true);
    EXECUTE stmt INTO STRICT exact_id;
    PERFORM set_config('enable_indexscan', 'on', true);
    PERFORM set_config('enable_seqscan', 'off', true);
    FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || stmt LOOP
        idx_seen := idx_seen OR position('Index Scan using ' || idx || ' ' IN line) > 0;
    END LOOP;
    EXECUTE stmt INTO STRICT actual_id;
    IF NOT idx_seen OR actual_id <> exact_id THEN
        RAISE WARNING 'maintained index % returned %, expected % (index scan %)',
            idx, actual_id, exact_id, idx_seen;
    END IF;
    RETURN idx_seen AND actual_id = exact_id;
END $$;

-- Reuse the original heap TIDs, reversing the ordering in the new heap.
CREATE TABLE hsm_trunc (id int PRIMARY KEY, v vector(3)) WITH (autovacuum_enabled = off);
INSERT INTO hsm_trunc SELECT g, ARRAY[g,0,0]::real[]::vector FROM generate_series(1,100) g;
CREATE INDEX hsm_trunc_c ON hsm_trunc USING cagra (v vector_l2_ops);
CREATE INDEX hsm_trunc_h ON hsm_trunc USING pg_cuvs_hnsw (v vector_l2_ops) WITH (source = hsm_trunc_c);
TRUNCATE hsm_trunc;
INSERT INTO hsm_trunc SELECT g+1000, ARRAY[2000-g,0,0]::real[]::vector FROM generate_series(1,100) g;
SELECT pg_temp.correct_neighbor('hsm_trunc', '[1,0,0]');
\set ON_ERROR_STOP off
CREATE INDEX hsm_trunc_again ON hsm_trunc USING pg_cuvs_hnsw (v vector_l2_ops) WITH (source = hsm_trunc_c);
\set ON_ERROR_STOP on
SELECT to_regclass('hsm_trunc_again') IS NULL;
DROP TABLE hsm_trunc;

-- Pending source inserts must not stop the table from being emptied.
CREATE TABLE hsm_dirty (id int PRIMARY KEY, v vector(3)) WITH (autovacuum_enabled = off);
INSERT INTO hsm_dirty SELECT g, ARRAY[g,0,0]::real[]::vector FROM generate_series(1,100) g;
CREATE INDEX hsm_dirty_c ON hsm_dirty USING cagra (v vector_l2_ops);
CREATE INDEX hsm_dirty_h ON hsm_dirty USING pg_cuvs_hnsw (v vector_l2_ops) WITH (source = hsm_dirty_c);
SET cuvs.socket_path = '';
INSERT INTO hsm_dirty VALUES (1000, '[1000,0,0]');
RESET cuvs.socket_path;
TRUNCATE hsm_dirty;
SELECT count(*) FROM hsm_dirty;
INSERT INTO hsm_dirty VALUES (2000, '[2000,0,0]');
SELECT pg_temp.correct_neighbor('hsm_dirty', '[2000,0,0]');
DROP TABLE hsm_dirty;

-- Reindex the CAGRA source concurrently so later maintenance sees the
-- dependent HNSW first. A heap rewrite must use the new heap TIDs.
CREATE TABLE hsm_order (id int PRIMARY KEY, v vector(3)) WITH (autovacuum_enabled = off);
INSERT INTO hsm_order SELECT g, ARRAY[g,0,0]::real[]::vector FROM generate_series(1,100) g;
CREATE INDEX hsm_order_c ON hsm_order USING cagra (v vector_l2_ops);
CREATE INDEX hsm_order_h ON hsm_order USING pg_cuvs_hnsw (v vector_l2_ops) WITH (source = hsm_order_c);
REINDEX INDEX CONCURRENTLY hsm_order_c;
SELECT 'hsm_order_c'::regclass::oid > 'hsm_order_h'::regclass::oid;
DELETE FROM hsm_order WHERE id <= 50;
VACUUM FULL hsm_order;
SELECT pg_temp.correct_neighbor('hsm_order', '[75,0,0]');
DELETE FROM hsm_order WHERE id <= 60;
CLUSTER hsm_order USING hsm_order_pkey;
SELECT pg_temp.correct_neighbor('hsm_order', '[75,0,0]');
SET cuvs.socket_path = '';
INSERT INTO hsm_order VALUES (2000, '[2000,0,0]');
RESET cuvs.socket_path;
REINDEX TABLE hsm_order;
SELECT pg_temp.correct_neighbor('hsm_order', '[2000,0,0]');
DROP TABLE hsm_order;
