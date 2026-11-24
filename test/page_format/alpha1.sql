\set ON_ERROR_STOP on
CREATE EXTENSION vector;
CREATE EXTENSION pg_cuvs;

CREATE TABLE page_empty (id int PRIMARY KEY, v vector);
CREATE INDEX page_empty_idx ON page_empty USING cagra (v vector_l2_ops);
DO $$
BEGIN
    IF pg_relation_size('page_empty_idx') = 0 THEN
        RAISE EXCEPTION 'empty CAGRA build did not initialize page storage';
    END IF;
END $$;

SET enable_seqscan = off;
SELECT id FROM page_empty ORDER BY v <-> '[0,0,0]' LIMIT 2;
BEGIN;
INSERT INTO page_empty VALUES (1, '[1,2,3]');
ROLLBACK;
DO $$
BEGIN
    BEGIN
        INSERT INTO page_empty VALUES (1, '[1,2]');
        RAISE EXCEPTION 'dimension changed after the first INSERT rolled back';
    EXCEPTION WHEN data_exception THEN
        NULL;
    END;
END $$;
INSERT INTO page_empty VALUES (2, '[1,2,3]');

CREATE TABLE page_one (id int PRIMARY KEY, v vector(3));
INSERT INTO page_one VALUES (1, '[0,0,0]');
CREATE INDEX page_one_idx ON page_one USING cagra (v vector_l2_ops);
DO $$
BEGIN
    IF pg_relation_size('page_one_idx') = 0 OR
       (SELECT id FROM page_one ORDER BY v <-> '[0,0,0]' LIMIT 1) <> 1 THEN
        RAISE EXCEPTION 'one-row CAGRA build did not serve its only row';
    END IF;
END $$;

CREATE TABLE page_vectors (id int PRIMARY KEY, v vector(3), note text) WITH (fillfactor=40);
INSERT INTO page_vectors VALUES (3,'[2,2,2]','a'),(1,'[0,0,0]','a'),(2,'[1,1,1]','a');
CREATE INDEX page_vectors_idx ON page_vectors USING cagra (v vector_l2_ops);
UPDATE page_vectors SET note='hot' WHERE id=1;
SELECT pg_stat_force_next_flush();
DO $$
DECLARE
    line record;
    found_scan boolean := false;
BEGIN
    FOR line IN EXECUTE $plan$EXPLAIN (COSTS OFF)
        SELECT id FROM page_vectors ORDER BY v <-> '[0,0,0]' LIMIT 3$plan$
    LOOP
        IF line."QUERY PLAN" LIKE '%Index Scan using page_vectors_idx%' THEN
            found_scan := true;
        END IF;
    END LOOP;
    IF NOT found_scan OR
       (SELECT n_tup_hot_upd FROM pg_stat_user_tables WHERE relname='page_vectors') <> 1 THEN
        RAISE EXCEPTION 'the page-format scan or HOT test path was not exercised';
    END IF;
END $$;
DO $$
DECLARE
    ordered int[];
    visible_tid tid;
    filtered_tid tid;
    fetched_tid tid;
    encoded bigint;
BEGIN
    SELECT array_agg(id) INTO ordered FROM
        (SELECT id FROM page_vectors ORDER BY v <-> '[0,0,0]' LIMIT 3) xs;
    IF ordered IS DISTINCT FROM ARRAY[1,2,3] THEN
        RAISE EXCEPTION 'unexpected heap scan result: %', ordered;
    END IF;
    SELECT ctid INTO visible_tid FROM page_vectors WHERE id=1;
    SELECT ctid INTO fetched_tid FROM pg_cuvs_batch_search('page_vectors',
            ARRAY['[0,0,0]'::vector], 1);
    IF fetched_tid IS DISTINCT FROM visible_tid THEN
        RAISE EXCEPTION 'batch did not return the visible TID';
    END IF;
    SELECT ((((visible_tid::text)::point)[0])::bigint << 16) |
           (((visible_tid::text)::point)[1])::bigint INTO encoded;
    SELECT ctid INTO filtered_tid FROM cuvs_filtered_knn('page_vectors_idx',
             '[0,0,0]', ARRAY[encoded], 1);
    IF filtered_tid IS DISTINCT FROM visible_tid THEN
        RAISE EXCEPTION 'filtered scan did not return the specified visible TID';
    END IF;
END $$;

CREATE TABLE page_mixed (v vector);
INSERT INTO page_mixed VALUES ('[1,2,3]'),('[1,2]');
DO $$
BEGIN
    BEGIN
        CREATE INDEX page_mixed_idx ON page_mixed USING cagra (v vector_l2_ops);
        RAISE EXCEPTION 'build silently skipped a different dimension';
    EXCEPTION WHEN data_exception THEN
        NULL;
    END;
END $$;

CREATE UNLOGGED TABLE page_unlogged (id int, v vector(3));
CREATE INDEX page_unlogged_idx ON page_unlogged USING cagra (v vector_l2_ops);
DO $$
BEGIN
    IF pg_relation_size('page_unlogged_idx', 'init') = 0 THEN
        RAISE EXCEPTION 'unlogged CAGRA init fork has no pages';
    END IF;
END $$;
INSERT INTO page_unlogged VALUES (1, '[1,2,3]');
