\set ON_ERROR_STOP on
CREATE EXTENSION vector;
CREATE EXTENSION pg_cuvs;
CREATE TABLE legacy_cagra (id int, v vector);
INSERT INTO legacy_cagra VALUES (1, '[0,0,0]'), (2, '[1,1,1]'), (3, '[2,2,2]');
CREATE INDEX legacy_cagra_idx ON legacy_cagra USING cagra (v vector_l2_ops);
SET enable_seqscan=off;
DO $$
BEGIN
    IF pg_relation_size('legacy_cagra_idx') <> 0 THEN
        RAISE EXCEPTION 'feature-off build produced a page-format index';
    END IF;
    IF (SELECT id FROM legacy_cagra ORDER BY v <-> '[0,0,0]' LIMIT 1) IS DISTINCT FROM 1 THEN
        RAISE EXCEPTION 'feature-off CAGRA returned the wrong row';
    END IF;
END $$;

SET cuvs.max_delta_rows=0;
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_settings
               WHERE name = 'debug_discard_caches' AND max_val::int >= 1) THEN
        PERFORM set_config('debug_discard_caches', '1', false);
    END IF;
END $$;
SHOW debug_assertions;
INSERT INTO legacy_cagra VALUES (4, '[4,4,4]'), (5, '[5,5,5]');
ANALYZE legacy_cagra;
INSERT INTO legacy_cagra VALUES (6, '[6,6,6]');
DO $$
BEGIN
    IF (SELECT id FROM legacy_cagra ORDER BY v <-> '[6,6,6]' LIMIT 1) IS DISTINCT FROM 6 THEN
        RAISE EXCEPTION 'legacy stale routing lost the inserted row';
    END IF;
END $$;
