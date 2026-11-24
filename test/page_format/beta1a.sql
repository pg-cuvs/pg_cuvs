\set ON_ERROR_STOP on
CREATE EXTENSION vector;
CREATE EXTENSION pg_cuvs;

CREATE TABLE page_search(id int PRIMARY KEY, v vector(8), note text DEFAULT 'seed') WITH (fillfactor=60);
INSERT INTO page_search(id,v)
SELECT i, array_fill(i::real, ARRAY[8])::vector FROM generate_series(1,256) AS i;
CREATE INDEX page_search_idx ON page_search USING cagra (v vector_l2_ops);

SET enable_seqscan=off;
SET cuvs.k=100;
SELECT pg_stat_reset_single_table_counters('page_search_idx'::regclass);
DO $$
DECLARE
    answer int[];
BEGIN
    SELECT array_agg(id) INTO answer FROM
        (SELECT id FROM page_search ORDER BY v <-> '[1,1,1,1,1,1,1,1]' LIMIT 10) s;
    IF answer IS DISTINCT FROM ARRAY[1,2,3,4,5,6,7,8,9,10] THEN
        RAISE EXCEPTION 'unexpected slot search answer: %',answer;
    END IF;
    IF pg_cuvs_last_search_k() IS DISTINCT FROM 100 OR
       pg_cuvs_last_search_n_results() IS DISTINCT FROM 100 THEN
        RAISE EXCEPTION 'the page-format scan did not consume a daemon slot reply';
    END IF;
END $$;

SELECT pg_stat_force_next_flush();
DO $$
BEGIN
    IF (SELECT idx_blks_read+idx_blks_hit FROM pg_statio_user_indexes
         WHERE indexrelid='page_search_idx'::regclass) <> 12 THEN
        RAISE EXCEPTION 'LIMIT 10 did not read the metapage, tail and ten TID slots';
    END IF;
END $$;
/* Use the existing CustomScan with a whitelist that includes a HOT child. */
CREATE TEMP TABLE old_page_tid AS SELECT ctid AS root FROM page_search WHERE id = 2;
UPDATE page_search SET note='hot' WHERE id=2;
SELECT pg_stat_force_next_flush();
DO $$
BEGIN
    IF (SELECT n_tup_hot_upd FROM pg_stat_user_tables WHERE relname='page_search') <> 1 THEN
        RAISE EXCEPTION 'the HOT fixture did not produce a child TID';
    END IF;
END $$;
DO $$
DECLARE
    target tid := (SELECT ctid FROM page_search WHERE id = 2);
    other tid := (SELECT ctid FROM page_search WHERE id = 4);
    old tid := (SELECT root FROM old_page_tid);
    got tid[];
BEGIN
    SELECT array_agg(s.ctid ORDER BY s.query_idx) INTO got
    FROM pg_cuvs_batch_search('page_search',
        ARRAY['[2,2,2,2,2,2,2,2]'::vector, '[4,4,4,4,4,4,4,4]'::vector], 1) AS s;
    IF got IS DISTINCT FROM ARRAY[target, other] THEN
        RAISE EXCEPTION 'exact batch candidates %, expected (%, %)', got, target, other;
    END IF;

    SELECT array_agg(s.ctid ORDER BY s.distance) INTO got
    FROM cuvs_filtered_knn('page_search_idx', '[2,2,2,2,2,2,2,2]',
        ARRAY[other, old, target, target, NULL::tid], 2) AS s;
    IF got IS DISTINCT FROM ARRAY[target, other] THEN
        RAISE EXCEPTION 'exact filtered candidates %, expected (%, %)', got, target, other;
    END IF;

    IF EXISTS (SELECT FROM cuvs_filtered_knn('page_search_idx',
            '[2,2,2,2,2,2,2,2]', ARRAY[old], 1)) THEN
        RAISE EXCEPTION 'a non-visible filter CTID matched the HOT child';
    END IF;
END $$;
SET cuvs.filtered_knn_hook=on;
SET enable_indexscan=off;
EXPLAIN (ANALYZE,COSTS OFF,TIMING OFF)
    SELECT id FROM page_search WHERE id % 2 = 0
    ORDER BY v <-> '[0,0,0,0,0,0,0,0]'::vector(8) LIMIT 3;
DO $$
DECLARE
    line record;
    seen boolean := false;
    gpu boolean := false;
BEGIN
    FOR line IN EXECUTE $query$EXPLAIN (ANALYZE,COSTS OFF,TIMING OFF)
        SELECT id FROM page_search WHERE id % 2 = 0
        ORDER BY v <-> '[0,0,0,0,0,0,0,0]'::vector(8) LIMIT 3$query$
    LOOP
        seen := seen OR line."QUERY PLAN" LIKE '%Custom Scan%';
        gpu := gpu OR line."QUERY PLAN" LIKE '%GPU IPC latency%';
    END LOOP;
    IF NOT seen OR gpu THEN
        RAISE EXCEPTION 'Custom Scan did not use exact fallback without a BF copy (plan %, IPC %)', seen, gpu;
    END IF;
    IF (SELECT array_agg(id) FROM
         (SELECT id FROM page_search WHERE id % 2 = 0
          ORDER BY v <-> '[0,0,0,0,0,0,0,0]' LIMIT 3) s)
          IS DISTINCT FROM ARRAY[2,4,6] THEN
        RAISE EXCEPTION 'filtered slot search lost a visible HOT result';
    END IF;
END $$;
RESET enable_indexscan;
RESET cuvs.filtered_knn_hook;
SET cuvs.search_mode=brute_force;
SET cuvs.k=9;
DO $$
BEGIN
    IF (SELECT id FROM page_search ORDER BY v <-> '[2,2,2,2,2,2,2,2]' LIMIT 1)
        IS DISTINCT FROM 2 OR pg_cuvs_last_search_k() IS NOT DISTINCT FROM 9 THEN
        RAISE EXCEPTION 'BF without a materialized copy did not return the exact row';
    END IF;
END $$;
RESET cuvs.search_mode;
BEGIN;
DELETE FROM page_search WHERE id = 1;
DO $$
BEGIN
    IF (SELECT s.ctid FROM pg_cuvs_batch_search('page_search',
            ARRAY['[1,1,1,1,1,1,1,1]'::vector], 1) AS s)
       IS DISTINCT FROM (SELECT ctid FROM page_search WHERE id = 2) THEN
        RAISE EXCEPTION 'the exact batch did not find the next visible neighbor after DELETE';
    END IF;
END $$;
ROLLBACK;
INSERT INTO page_search(id,v) VALUES (0, '[0,0,0,0,0,0,0,0]');
DO $$
BEGIN
    IF (SELECT id FROM page_search ORDER BY v <-> '[0,0,0,0,0,0,0,0]' LIMIT 1)
       IS DISTINCT FROM 0 THEN
        RAISE EXCEPTION 'pending rows were missed by the heap fallback';
    END IF;
    IF (SELECT s.ctid FROM pg_cuvs_batch_search('page_search',
            ARRAY['[0,0,0,0,0,0,0,0]'::vector], 1) AS s)
       IS DISTINCT FROM (SELECT ctid FROM page_search WHERE id = 0) THEN
        RAISE EXCEPTION 'the SQL batch did not use the heap for pending rows';
    END IF;
END $$;

RESET enable_seqscan;
CREATE TABLE g14_pages AS SELECT id,v FROM page_search WHERE id > 0 ORDER BY id;
CREATE INDEX g14_idx ON g14_pages USING cagra (v vector_l2_ops);

-- The sidecar is required to exercise the CPU HNSW 1-dot distance contract.
CREATE TABLE page_ip(id int, v vector(8));
INSERT INTO page_ip
SELECT i, array_fill((CASE i WHEN 1 THEN 0.9 WHEN 2 THEN 0.45 ELSE 0.1 END)::real,
                     ARRAY[8])::vector
FROM generate_series(1,256) AS i;
SET cuvs.cpu_hnsw_fallback=on;
CREATE INDEX page_ip_idx ON page_ip USING cagra (v vector_ip_ops);
RESET cuvs.cpu_hnsw_fallback;
