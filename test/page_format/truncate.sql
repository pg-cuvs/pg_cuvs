\set ON_ERROR_STOP on
CREATE EXTENSION vector;
CREATE EXTENSION pg_cuvs;
CREATE EXTENSION pageinspect;

CREATE TABLE page_truncate (id int, v vector);
INSERT INTO page_truncate VALUES (1, '[1,0,0]'), (2, '[2,0,0]');
CREATE INDEX page_truncate_idx ON page_truncate USING cagra (v vector_l2_ops);
CREATE TEMP TABLE original_nonce AS
    SELECT substr(get_raw_page('page_truncate_idx', 0), 49, 8) AS nonce;
SET enable_seqscan=off;

BEGIN;
TRUNCATE page_truncate;
INSERT INTO page_truncate VALUES (3, '[3,4]');
DO $$
BEGIN
    IF substr(get_raw_page('page_truncate_idx', 0), 49, 8) = (SELECT nonce FROM original_nonce)
       OR ARRAY(SELECT id FROM page_truncate ORDER BY v <-> '[3,4]' LIMIT 2) <> ARRAY[3] THEN
        RAISE EXCEPTION 'same-transaction TRUNCATE reused the old index state';
    END IF;
END $$;
ROLLBACK;

DO $$
BEGIN
    IF substr(get_raw_page('page_truncate_idx', 0), 49, 8) <> (SELECT nonce FROM original_nonce)
       OR ARRAY(SELECT id FROM page_truncate ORDER BY v <-> '[1,0,0]' LIMIT 2) <> ARRAY[1,2] THEN
        RAISE EXCEPTION 'TRUNCATE rollback did not restore the old index state';
    END IF;
END $$;

TRUNCATE page_truncate;
INSERT INTO page_truncate VALUES (4, '[4,5]');
DO $$
BEGIN
    IF substr(get_raw_page('page_truncate_idx', 0), 49, 8) = (SELECT nonce FROM original_nonce)
       OR ARRAY(SELECT id FROM page_truncate ORDER BY v <-> '[4,5]' LIMIT 2) <> ARRAY[4] THEN
        RAISE EXCEPTION 'committed TRUNCATE reused the old index state';
    END IF;
END $$;

BEGIN;
CREATE TABLE page_new (id int, v vector);
INSERT INTO page_new VALUES (1, '[1,0,0]');
CREATE INDEX page_new_idx ON page_new USING cagra (v vector_l2_ops);
CREATE TEMP TABLE original_new_layout ON COMMIT DROP AS
    SELECT pg_relation_filenode('page_new') AS filenode,
           substr(get_raw_page('page_new_idx', 0), 49, 8) AS nonce;
TRUNCATE page_new;
INSERT INTO page_new VALUES (2, '[3,4]');
DO $$
BEGIN
    IF pg_relation_filenode('page_new') <> (SELECT filenode FROM original_new_layout) THEN
        RAISE EXCEPTION 'same-transaction fixture did not take the in-place TRUNCATE path';
    END IF;
    IF substr(get_raw_page('page_new_idx', 0), 49, 8) = (SELECT nonce FROM original_new_layout)
       OR ARRAY(SELECT id FROM page_new ORDER BY v <-> '[3,4]' LIMIT 2) <> ARRAY[2] THEN
        RAISE EXCEPTION 'in-place TRUNCATE reused the old index state';
    END IF;
END $$;
COMMIT;
