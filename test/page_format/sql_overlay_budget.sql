\set ON_ERROR_STOP on
CREATE EXTENSION vector;
CREATE EXTENSION pg_cuvs;
CREATE TABLE overlay_budget(id integer PRIMARY KEY, v vector(3));
INSERT INTO overlay_budget
SELECT i, ARRAY[i::real, 0, 0]::vector FROM generate_series(1, 256) AS i;
CREATE INDEX overlay_budget_idx ON overlay_budget USING cagra (v vector_l2_ops);
SET enable_seqscan=off;
SET cuvs.k=2;
DO $$
BEGIN
    IF (SELECT id FROM overlay_budget ORDER BY v <-> '[0,0,0]' LIMIT 1) IS DISTINCT FROM 1
       OR pg_cuvs_last_search_k() IS DISTINCT FROM 2 THEN
        RAISE EXCEPTION 'base search did not use the daemon under the small host budget';
    END IF;
END $$;

INSERT INTO overlay_budget VALUES (0, '[0,0,0]');
DO $$
BEGIN
    IF (SELECT id FROM overlay_budget ORDER BY v <-> '[0,0,0]' LIMIT 1) IS DISTINCT FROM 0
       OR pg_cuvs_last_search_k() IS NOT NULL
       OR (SELECT last_reason FROM pg_stat_gpu_fallback
           WHERE index_oid = 'overlay_budget_idx'::regclass)
           IS DISTINCT FROM 'page_overlay_host_budget' THEN
        RAISE EXCEPTION 'the host budget did not send the SQL search to the exact heap';
    END IF;
END $$;

INSERT INTO overlay_budget VALUES (-1, '[0,1,0]');
UPDATE overlay_budget SET v = '[0,2,0]' WHERE id = -1;
DO $$
BEGIN
    IF (SELECT id FROM overlay_budget ORDER BY v <-> '[0,2,0]' LIMIT 1) IS DISTINCT FROM -1
       OR (SELECT v FROM overlay_budget WHERE id = -1) IS DISTINCT FROM '[0,2,0]'::vector
       OR pg_cuvs_last_search_k() IS NOT NULL
       OR (SELECT fallback_count FROM pg_stat_gpu_fallback
           WHERE index_oid = 'overlay_budget_idx'::regclass) IS DISTINCT FROM 2 THEN
        RAISE EXCEPTION 'writes or subsequent exact search failed under the host budget';
    END IF;
END $$;
