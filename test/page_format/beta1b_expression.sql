\set ON_ERROR_STOP on
CREATE EXTENSION vector;
CREATE EXTENSION pg_cuvs;
CREATE EXTENSION pageinspect;
CREATE FUNCTION pending_first(regclass, bigint) RETURNS double precision
    AS 'pg_cuvs', 'cuvs_test_pending_first' LANGUAGE C STRICT;
CREATE FUNCTION pending_first_from(regclass, bigint, int, bigint) RETURNS double precision
    AS 'pg_cuvs', 'cuvs_test_pending_first' LANGUAGE C STRICT;

-- Inspect the metapage and pending payload independently of the AM reader.
-- The CPU matrix runs on little-endian PG16 with 8 KiB pages.
CREATE FUNCTION assert_pending(idx text, block int, kind int, width int, items int)
RETURNS void LANGUAGE plpgsql AS $$
DECLARE
    meta bytea := get_raw_page(idx, 0);
    pending bytea := get_raw_page(idx, block);
BEGIN
    IF encode(substr(meta, 29, 4), 'hex') <> '00000000'
       OR get_byte(meta, 32) <> (CASE WHEN kind = 4 THEN 2 ELSE 0 END)
       OR get_byte(pending, 8160) <> kind
       OR (SELECT lower FROM page_header(pending)) <> 24 + width * items THEN
        RAISE EXCEPTION '%: unexpected pending layout at block % (kind %, lower %, flags %)',
            idx, block, get_byte(pending, 8160), (SELECT lower FROM page_header(pending)),
            get_byte(meta, 32);
    END IF;
END $$;

CREATE TABLE page_expr(id int, v vector(3));
INSERT INTO page_expr VALUES (1,'[1,0,0]'), (2,'[2,0,0]');
CREATE INDEX page_expr_idx ON page_expr
    USING cagra (((v + '[1,0,0]'::vector)::vector(3)) vector_l2_ops);
INSERT INTO page_expr VALUES (3,'[3,0,0]'), (4,NULL);
SELECT assert_pending('page_expr_idx', 65, 4, 18, 1);
DO $$ BEGIN
    IF pending_first('page_expr_idx', 0) <> 4 THEN
        RAISE EXCEPTION 'pending reader lost the evaluated expression';
    END IF;
END $$;
DO $$
BEGIN
    IF encode(substr(get_raw_page('page_expr_idx', 65), 25, 18), 'hex')
        <> '000000000300000080400000000000000000' THEN
        RAISE EXCEPTION 'pending record did not store the heap TID and evaluated expression';
    END IF;
END $$;

SET enable_seqscan=off;
DO $$
BEGIN
    IF (SELECT id FROM page_expr ORDER BY ((v + '[1,0,0]'::vector)::vector(3)) <-> '[4,0,0]'
        LIMIT 1) IS DISTINCT FROM 3 OR pg_cuvs_last_search_k() IS NOT NULL THEN
        RAISE EXCEPTION 'a pending expression row did not use the exact heap fallback';
    END IF;
END $$;
RESET enable_seqscan;

INSERT INTO page_expr VALUES (5,'[5,0,0]'), (6,'[6,0,0]');
DELETE FROM page_expr WHERE id = 5;
VACUUM (INDEX_CLEANUP ON, TRUNCATE OFF) page_expr;
DO $$
DECLARE
    pending bytea := get_raw_page('page_expr_idx', 65);
BEGIN
    IF encode(substr(pending, 25, 18), 'hex') <>
           '000000000300000080400000000000000000' OR
       encode(substr(pending, 43, 18), 'hex') <>
           'ffffffff00000000c0400000000000000000' OR
       encode(substr(pending, 61, 18), 'hex') <>
           '0000000006000000e0400000000000000000' THEN
        RAISE EXCEPTION 'vector pending VACUUM changed a neighboring record';
    END IF;
END $$;

CREATE TABLE page_pending_plain(id int, v vector(3));
CREATE INDEX page_pending_plain_idx ON page_pending_plain USING cagra (v vector_l2_ops);
CREATE INDEX page_pending_partial_idx ON page_pending_plain
    USING cagra (v vector_l2_ops) WHERE id > 0;
INSERT INTO page_pending_plain VALUES (1,'[1,0,0]'), (-1,'[-1,0,0]');
SELECT assert_pending('page_pending_plain_idx', 1, 2, 6, 2);
SELECT assert_pending('page_pending_partial_idx', 1, 2, 6, 1);
DO $$
BEGIN
    IF encode(substr(get_raw_page('page_pending_partial_idx', 1), 25, 6), 'hex')
        <> '000000000100' THEN
        RAISE EXCEPTION 'partial index did not store only the qualifying heap TID';
    END IF;
END $$;

-- An empty expression with typmod can use pages. At the maximum supported
-- dimension each pending page holds one item, so the next insert must extend.
CREATE TABLE page_expr_bound(id int, v vector);
CREATE INDEX page_expr_bound_idx ON page_expr_bound
    USING cagra (((v + v)::vector(2030)) vector_l2_ops);
INSERT INTO page_expr_bound VALUES
    (1,array_fill(1::real,ARRAY[2030])::vector),
    (2,array_fill(2::real,ARRAY[2030])::vector);
SELECT assert_pending('page_expr_bound_idx', 1, 4, 8126, 1);
SELECT assert_pending('page_expr_bound_idx', 3, 4, 8126, 1);
DO $$ BEGIN
    IF pending_first('page_expr_bound_idx', 0) <> 2 OR
       pending_first('page_expr_bound_idx', 1) <> 4 THEN
        RAISE EXCEPTION 'pending reader lost a vector across page rollover';
    END IF;
END $$;
DO $$
BEGIN
    IF encode(substr(get_raw_page('page_expr_bound_idx', 3), 8147, 4), 'hex')
        <> '00008040' THEN
        RAISE EXCEPTION 'last coordinate was not stored on the new pending page';
    END IF;
END $$;
INSERT INTO page_expr_bound VALUES (3,array_fill(3::real,ARRAY[2030])::vector);
DO $$
DECLARE
    message text;
    rejected bool := false;
BEGIN
    IF pending_first_from('page_expr_bound_idx', 1, 3, 1) <> 4 THEN
        RAISE EXCEPTION 'reader lost the first item after moving the pending head';
    END IF;
    BEGIN
        PERFORM pending_first_from('page_expr_bound_idx', 0, 3, 0);
    EXCEPTION WHEN others THEN
        GET STACKED DIAGNOSTICS message = MESSAGE_TEXT;
        rejected := message = 'could not read the CAGRA pending vector';
    END;
    IF NOT rejected THEN
        RAISE EXCEPTION 'reader accepted a position before the pending head';
    END IF;
END $$;
DELETE FROM page_expr_bound WHERE id = 1;
VACUUM (INDEX_CLEANUP ON, TRUNCATE OFF) page_expr_bound;
DO $$
BEGIN
    IF encode(substr(get_raw_page('page_expr_bound_idx', 1), 25, 6), 'hex')
        <> 'ffffffff0000'
       OR encode(substr(get_raw_page('page_expr_bound_idx', 3), 25, 6), 'hex')
        <> '000000000200' THEN
        RAISE EXCEPTION 'VACUUM did not distinguish dead and live pending records';
    END IF;
END $$;

CREATE TABLE page_expr_dim(v vector);
INSERT INTO page_expr_dim VALUES ('[1,2,3]'), ('[2,3,4]');
CREATE INDEX page_expr_dim_idx ON page_expr_dim USING cagra ((v + v) vector_l2_ops);
DO $$
BEGIN
    IF pg_relation_size('page_expr_dim_idx') <> 0 THEN
        RAISE EXCEPTION 'expression without declared dimension used page storage';
    END IF;
END $$;
TRUNCATE page_expr_dim;
INSERT INTO page_expr_dim VALUES ('[1,2,3]'), ('[2,3,4]');
REINDEX INDEX page_expr_dim_idx;
DO $$ BEGIN
    IF pg_relation_size('page_expr_dim_idx') <> 0 THEN
        RAISE EXCEPTION 'TRUNCATE/REINDEX changed undeclared expression storage';
    END IF;
END $$;

CREATE TABLE page_expr_stable(v vector);
INSERT INTO page_expr_stable VALUES ('[1,2,3]');
CREATE INDEX page_expr_stable_idx ON page_expr_stable
    USING cagra (((v + v)::vector(3)) vector_l2_ops);
TRUNCATE page_expr_stable;
DO $$ BEGIN
    IF pg_relation_size('page_expr_stable_idx') = 0 THEN
        RAISE EXCEPTION 'TRUNCATE changed declared expression storage';
    END IF;
END $$;
INSERT INTO page_expr_stable VALUES ('[1,2,3]');
REINDEX INDEX page_expr_stable_idx;
DO $$ BEGIN
    IF pg_relation_size('page_expr_stable_idx') = 0 THEN
        RAISE EXCEPTION 'REINDEX changed declared expression storage';
    END IF;
END $$;

CREATE TABLE page_expr_mixed_a(v vector);
INSERT INTO page_expr_mixed_a VALUES ('[1,2,3]'), ('[1,2]');
CREATE TABLE page_expr_mixed_b(v vector);
INSERT INTO page_expr_mixed_b VALUES ('[1,2]'), ('[1,2,3]');
CREATE TABLE page_expr_mixed_c(v vector);
INSERT INTO page_expr_mixed_c VALUES ('[1,2,3]'),
    (array_fill(1::real,ARRAY[2031])::vector);
CREATE TABLE page_expr_mixed_d(v vector);
INSERT INTO page_expr_mixed_d VALUES
    (array_fill(1::real,ARRAY[2031])::vector), ('[1,2,3]');
DO $$
DECLARE
    tbl text;
    message text;
    first_message text;
    state text;
BEGIN
    FOREACH tbl IN ARRAY ARRAY['page_expr_mixed_a', 'page_expr_mixed_b',
                               'page_expr_mixed_c', 'page_expr_mixed_d'] LOOP
        BEGIN
            EXECUTE format('CREATE INDEX ON %I USING cagra ((v + v) vector_l2_ops)', tbl);
            RAISE EXCEPTION 'mixed dimensions built an expression index on %', tbl;
        EXCEPTION WHEN data_exception THEN
            GET STACKED DIAGNOSTICS message = MESSAGE_TEXT, state = RETURNED_SQLSTATE;
            IF state <> '22000' OR (first_message IS NOT NULL AND message <> first_message) THEN
                RAISE EXCEPTION 'mixed dimension error differs on %: % %', tbl, state, message;
            END IF;
            first_message := message;
        END;
    END LOOP;
END $$;

CREATE TABLE page_expr_unknown(v vector);
CREATE INDEX page_expr_unknown_idx ON page_expr_unknown USING cagra ((v + v) vector_l2_ops);
INSERT INTO page_expr_unknown VALUES (NULL);
CREATE INDEX page_expr_null_idx ON page_expr_unknown USING cagra ((v + v) vector_l2_ops);
CREATE INDEX page_expr_typed_wide_idx ON page_expr_unknown
    USING cagra (((v + v)::vector(2031)) vector_l2_ops);
CREATE TABLE page_expr_wide(v vector);
INSERT INTO page_expr_wide SELECT array_fill(i::real,ARRAY[2031])::vector
    FROM generate_series(1,2) AS i;
CREATE INDEX page_expr_wide_idx ON page_expr_wide USING cagra ((v + v) vector_l2_ops);
DO $$
BEGIN
    IF pg_relation_size('page_expr_unknown_idx') <> 0 OR
       pg_relation_size('page_expr_null_idx') <> 0 OR
       pg_relation_size('page_expr_typed_wide_idx') <> 0 OR
       pg_relation_size('page_expr_wide_idx') <> 0 THEN
        RAISE EXCEPTION 'unknown or oversized expression did not retain the legacy layout';
    END IF;
END $$;
