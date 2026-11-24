\set ON_ERROR_STOP on
CREATE EXTENSION vector;
CREATE EXTENSION pg_cuvs;

CREATE TABLE page_dead(id int PRIMARY KEY, v vector(8), note text DEFAULT 'seed')
    WITH (fillfactor=60);
INSERT INTO page_dead(id, v)
SELECT i, array_fill(i::real, ARRAY[8])::vector FROM generate_series(1,256) AS i;
CREATE INDEX page_dead_idx ON page_dead USING cagra (v vector_l2_ops);
CREATE TEMP TABLE old_dead_tids AS SELECT ctid AS tid FROM page_dead WHERE id <= 3;
DELETE FROM page_dead WHERE id <= 3;
VACUUM (INDEX_CLEANUP ON, TRUNCATE OFF) page_dead;

-- A HOT child may reuse a heap offset whose old index slot is now dead.
-- It must still resolve through the live root of id=4.
UPDATE page_dead SET note='hot' WHERE id=4;
SELECT pg_stat_force_next_flush();
SET enable_seqscan=off;
SET cuvs.k=4;
DO $$
DECLARE
    rows int[];
BEGIN
    IF (SELECT n_tup_hot_upd FROM pg_stat_user_tables WHERE relname='page_dead') <> 1 OR
       (SELECT ctid FROM page_dead WHERE id=4) NOT IN (SELECT tid FROM old_dead_tids) THEN
        RAISE EXCEPTION 'fixture did not reuse a dead offset as a HOT child';
    END IF;
    SELECT ARRAY(SELECT id FROM page_dead ORDER BY v <-> '[0,0,0,0,0,0,0,0]' LIMIT 4) INTO rows;
    IF rows IS DISTINCT FROM ARRAY[4,5,6,7] OR
       pg_cuvs_last_search_k() IS DISTINCT FROM 7 OR
       pg_cuvs_last_search_n_results() IS DISTINCT FROM 7 THEN
        RAISE EXCEPTION 'wrong live rows or missing dead-slot overfetch: %', rows;
    END IF;
END $$;

SET cuvs.filtered_knn_hook=on;
SET enable_indexscan=off;
DO $$
DECLARE
    line record;
    used_daemon boolean := false;
BEGIN
    FOR line IN EXECUTE $plan$EXPLAIN (ANALYZE,COSTS OFF,TIMING OFF)
        SELECT id FROM page_dead WHERE id % 2 = 0
        ORDER BY v <-> '[0,0,0,0,0,0,0,0]'::vector(8) LIMIT 3$plan$
    LOOP
        used_daemon := used_daemon OR line."QUERY PLAN" LIKE '%GPU IPC latency%';
    END LOOP;
    IF NOT used_daemon OR ARRAY(
        SELECT id FROM page_dead WHERE id % 2 = 0
        ORDER BY v <-> '[0,0,0,0,0,0,0,0]'::vector(8) LIMIT 3
    ) IS DISTINCT FROM ARRAY[4,6,8] THEN
        RAISE EXCEPTION 'filtered page search mishandled invalid slots';
    END IF;
END $$;
