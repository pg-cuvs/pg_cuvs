\set ON_ERROR_STOP on
CREATE TEMP TABLE original_page_index AS
SELECT substr(get_raw_page('g14_idx',0),49,24) AS identity;
SET enable_seqscan=off;

BEGIN;
TRUNCATE g14_pages;
DO $$
BEGIN
    IF (SELECT count(*) FROM g14_pages) <> 0 OR
       substr(get_raw_page('g14_idx',0),49,24) =
           (SELECT identity FROM original_page_index) THEN
        RAISE EXCEPTION 'TRUNCATE did not create a distinct empty index';
    END IF;
END $$;
ROLLBACK;

SET cuvs.k=3;
DO $$
BEGIN
    IF substr(get_raw_page('g14_idx',0),49,24) <>
           (SELECT identity FROM original_page_index) OR
       (SELECT id FROM g14_pages ORDER BY v <-> '[1,1,1,1,1,1,1,1]' LIMIT 1)
           IS DISTINCT FROM 1 OR pg_cuvs_last_search_k() IS DISTINCT FROM 3 THEN
        RAISE EXCEPTION 'TRUNCATE rollback did not restore the original daemon graph';
    END IF;
END $$;

BEGIN;
REINDEX INDEX g14_idx;
DO $$
BEGIN
    IF substr(get_raw_page('g14_idx',0),49,24) =
           (SELECT identity FROM original_page_index) THEN
        RAISE EXCEPTION 'REINDEX retained the original graph identity';
    END IF;
END $$;
ROLLBACK;

SET cuvs.k=5;
DO $$
BEGIN
    IF substr(get_raw_page('g14_idx',0),49,24) <>
           (SELECT identity FROM original_page_index) OR
       (SELECT id FROM g14_pages ORDER BY v <-> '[1,1,1,1,1,1,1,1]' LIMIT 1)
           IS DISTINCT FROM 1 OR pg_cuvs_last_search_k() IS DISTINCT FROM 5 THEN
        RAISE EXCEPTION 'REINDEX rollback did not restore the original daemon graph';
    END IF;
END $$;
