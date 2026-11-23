\set ON_ERROR_STOP on
CREATE EXTENSION vector;
CREATE EXTENSION pg_cuvs;
CREATE EXTENSION pageinspect;

CREATE TABLE page_toast (id int PRIMARY KEY, v vector(768));
ALTER TABLE page_toast ALTER COLUMN v SET STORAGE EXTERNAL;
INSERT INTO page_toast VALUES (1, array_fill(1::real, ARRAY[768])::vector);
DO $$
BEGIN
    IF (SELECT octet_length(t_attrs[2]) FROM heap_page_item_attrs(
            get_raw_page('page_toast', 0), 'page_toast', false) WHERE lp = 1) IS DISTINCT FROM 18 THEN
        RAISE EXCEPTION 'fixture vector is not stored out of line';
    END IF;
END $$;

CREATE INDEX page_toast_idx ON page_toast USING cagra (v vector_l2_ops);
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_settings
               WHERE name = 'debug_discard_caches' AND max_val::int >= 1) THEN
        PERFORM set_config('debug_discard_caches', '1', false);
    END IF;
END $$;
SHOW debug_assertions;
INSERT INTO page_toast SELECT 2, v FROM page_toast WHERE id = 1;
UPDATE page_toast SET v = array_fill(4::real, ARRAY[768])::vector WHERE id = 2;
UPDATE page_toast AS target SET v = source.v FROM page_toast AS source
    WHERE target.id = 2 AND source.id = 1;
SET enable_seqscan=off;
DO $$
BEGIN
    IF (SELECT lower FROM page_header(get_raw_page('page_toast_idx', 65))) <> 42
       OR (SELECT count(*) FROM (SELECT id FROM page_toast
            ORDER BY v <-> array_fill(1::real, ARRAY[768])::vector LIMIT 2) AS results) <> 2 THEN
        RAISE EXCEPTION 'TOAST inserts and updates did not survive relcache handling';
    END IF;
END $$;
