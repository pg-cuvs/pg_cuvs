\set ON_ERROR_STOP on
DO $$
DECLARE
    pending bytea := get_raw_page('page_durable_idx', 1);
BEGIN
    IF (SELECT lower FROM page_header(pending)) <> 30 OR
       encode(substr(pending, 25, 6), 'hex') <> '000000000100' THEN
        RAISE EXCEPTION 'pending TID was lost after WAL replay';
    END IF;
END $$;
DO $$
DECLARE
    pending bytea := get_raw_page('page_durable_expr_idx', 1);
BEGIN
    IF get_byte(pending, 8160) <> 4 OR
       (SELECT lower FROM page_header(pending)) <> 42 OR
       encode(substr(pending, 25, 18), 'hex') <>
           '00000000010000000040000080400000c040' THEN
        RAISE EXCEPTION 'pending expression vector was lost after WAL replay';
    END IF;
END $$;
SET enable_seqscan = off;
DO $$
BEGIN
    IF (SELECT id FROM page_durable ORDER BY v <-> '[0,1,2]' LIMIT 1) IS DISTINCT FROM 1 THEN
        RAISE EXCEPTION 'WAL replay did not preserve the heap answer';
    END IF;
    BEGIN
        INSERT INTO page_durable VALUES (2, '[1,2]');
        RAISE EXCEPTION 'WAL replay lost the fixed dimension';
    EXCEPTION WHEN data_exception THEN
        NULL;
    END;
END $$;
