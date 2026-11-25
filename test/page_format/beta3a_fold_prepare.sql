\set ON_ERROR_STOP on
CREATE EXTENSION vector;
CREATE EXTENSION pg_cuvs;
CREATE EXTENSION pageinspect;

CREATE TABLE fold_prepare(id integer PRIMARY KEY, v vector(3));
INSERT INTO fold_prepare
SELECT i, ARRAY[i::real,0,0]::vector FROM generate_series(1,256) i;
CREATE INDEX fold_prepare_idx ON fold_prepare USING cagra(v vector_l2_ops);

CREATE FUNCTION pg_temp.prepare_fold(regclass, integer, integer) RETURNS text
AS '$libdir/pg_cuvs', 'cuvs_test_fold_prepare'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

INSERT INTO fold_prepare VALUES (1001,'[1,0,0]'), (1002,'[2,0,0]'), (1003,'[3,0,0]');
DELETE FROM fold_prepare WHERE id=1002;
VACUUM (INDEX_CLEANUP ON, TRUNCATE OFF) fold_prepare;

CREATE TEMP TABLE old_meta AS SELECT get_raw_page('fold_prepare_idx',0) AS image;
DO $$
DECLARE
    expected text;
BEGIN
    expected := format('prepared %s null %s',
                       (SELECT ctid FROM fold_prepare WHERE id=1001),
                       (SELECT ctid FROM fold_prepare WHERE id=1003));
    IF pg_temp.prepare_fold('fold_prepare_idx', 3, 0) IS DISTINCT FROM expected THEN
        RAISE EXCEPTION 'fold preparation did not use the current TIDs and dead position';
    END IF;
    IF pg_temp.prepare_fold('fold_prepare_idx', 4, 0) <> 'discard' OR
       pg_temp.prepare_fold('fold_prepare_idx', 0, 0) <> 'discard' THEN
        RAISE EXCEPTION 'fold preparation accepted an invalid prefix length';
    END IF;
    FOR mismatch IN 1..6 LOOP
        IF pg_temp.prepare_fold('fold_prepare_idx', 3, mismatch) <> 'discard' THEN
            RAISE EXCEPTION 'fold preparation accepted mismatched identity %', mismatch;
        END IF;
    END LOOP;
END $$;

BEGIN;
SELECT pg_temp.prepare_fold('fold_prepare_idx', 3, 0);
INSERT INTO fold_prepare VALUES (1004,'[4,0,0]');
ROLLBACK;

DO $$
BEGIN
    IF EXISTS (SELECT FROM fold_prepare WHERE id=1004) OR
       pg_temp.prepare_fold('fold_prepare_idx', 4, 0) NOT LIKE 'prepared %' THEN
        RAISE EXCEPTION 'rolled-back INSERT did not leave its pending position';
    END IF;
    IF get_raw_page('fold_prepare_idx',0) IS DISTINCT FROM (SELECT image FROM old_meta) THEN
        RAISE EXCEPTION 'preparation changed the published metapage';
    END IF;
END $$;
