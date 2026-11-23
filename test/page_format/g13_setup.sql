\set ON_ERROR_STOP on
CREATE EXTENSION vector;
CREATE EXTENSION pg_cuvs;
CREATE TABLE page_dirty(id int, v vector(8));
CREATE TABLE page_to_empty (LIKE page_dirty);
CREATE TABLE page_to_one (LIKE page_dirty);
INSERT INTO page_dirty SELECT i, array_fill(i::real, ARRAY[8])::vector FROM generate_series(1, 256) AS i;
INSERT INTO page_to_empty SELECT * FROM page_dirty WHERE id <= 2;
INSERT INTO page_to_one SELECT * FROM page_dirty WHERE id <= 2;
CREATE INDEX page_dirty_idx ON page_dirty USING cagra (v vector_l2_ops);
CREATE INDEX page_to_empty_idx ON page_to_empty USING cagra (v vector_l2_ops);
CREATE INDEX page_to_one_idx ON page_to_one USING cagra (v vector_l2_ops);
INSERT INTO page_dirty VALUES (0, '[0,0,0,0,0,0,0,0]');
DELETE FROM page_to_empty;
DELETE FROM page_to_one WHERE id = 2;
DO $$
BEGIN
    IF NOT EXISTS (
        SELECT FROM pg_cuvs_index_status()
        WHERE index_oid='page_dirty_idx'::regclass AND file_current IS FALSE AND extend_count = 1
    ) THEN
        RAISE EXCEPTION 'fixture did not leave the EXTEND solely in the resident legacy graph';
    END IF;
END $$;
