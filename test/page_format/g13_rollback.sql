\set ON_ERROR_STOP on
BEGIN;
REINDEX INDEX page_dirty_idx;
DO $$
BEGIN
    IF pg_relation_size('page_dirty_idx') = 0 THEN
        RAISE EXCEPTION 'REINDEX did not switch index storage';
    END IF;
    IF EXISTS (SELECT FROM pg_cuvs_index_status()
               WHERE index_oid='page_dirty_idx'::regclass AND kind <> 'page_cagra') OR
       NOT EXISTS (SELECT FROM pg_cuvs_index_status()
                   WHERE index_oid='page_dirty_idx'::regclass AND kind='page_cagra'
                         AND vram_bytes > 0 AND warmup_state LIKE 'incarnation:%') OR
       EXISTS (SELECT FROM pg_cuvs_gpu_search_stats() WHERE index_oid='page_dirty_idx'::regclass) THEN
        RAISE EXCEPTION 'page-format status did not identify the resident page graph';
    END IF;
    BEGIN
        PERFORM pg_cuvs_load('page_dirty_idx'::regclass);
        RAISE EXCEPTION 'manual LOAD opened the legacy graph through a page-format index';
    EXCEPTION WHEN feature_not_supported THEN
        NULL;
    END;
    BEGIN
        PERFORM pg_cuvs_unload('page_dirty_idx'::regclass);
        RAISE EXCEPTION 'UNLOAD was allowed before the page-format REINDEX committed';
    EXCEPTION WHEN object_not_in_prerequisite_state THEN
        NULL;
    END;
    BEGIN
        PERFORM pg_cuvs_unpin('page_dirty_idx'::regclass);
        RAISE EXCEPTION 'UNPIN was allowed before the page-format REINDEX committed';
    EXCEPTION WHEN object_not_in_prerequisite_state THEN
        NULL;
    END;
    BEGIN
        PERFORM pg_cuvs_compact('page_dirty_idx'::regclass);
        RAISE EXCEPTION 'manual COMPACT opened a legacy page-format index';
    EXCEPTION WHEN feature_not_supported THEN
        NULL;
    END;
END $$;
ROLLBACK;

SET enable_seqscan=off;
DO $$
BEGIN
    IF pg_relation_size('page_dirty_idx') <> 0 OR NOT EXISTS (
        SELECT FROM pg_cuvs_index_status()
        WHERE index_oid='page_dirty_idx'::regclass AND file_current IS FALSE AND extend_count = 1
    ) THEN
        RAISE EXCEPTION 'rollback displaced the dirty legacy graph';
    END IF;
    IF (SELECT id FROM page_dirty ORDER BY v <-> '[0,0,0,0,0,0,0,0]' LIMIT 1) IS DISTINCT FROM 0 OR
       pg_cuvs_last_search_k() IS NULL THEN
        RAISE EXCEPTION 'rollback did not return the EXTEND row from the legacy daemon graph';
    END IF;
END $$;
