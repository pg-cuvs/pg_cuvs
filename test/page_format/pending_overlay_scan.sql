\set ON_ERROR_STOP on
CREATE EXTENSION vector;
CREATE EXTENSION pg_cuvs;
CREATE TABLE pending_scan(id integer, v vector(3), note text) WITH (fillfactor=70);
INSERT INTO pending_scan VALUES (1, '[1,0,0]', 'base'), (2, '[2,0,0]', 'base');
CREATE INDEX pending_scan_idx ON pending_scan USING cagra (v vector_l2_ops);
INSERT INTO pending_scan VALUES (0, '[0,0,0]', 'pending');
UPDATE pending_scan SET note='hot' WHERE id=0;
SELECT pg_stat_force_next_flush();
DO $$
BEGIN
    IF (SELECT n_tup_hot_upd FROM pg_stat_user_tables WHERE relid = 'pending_scan'::regclass)
        IS DISTINCT FROM 1 THEN
        RAISE EXCEPTION 'the pending row update did not use HOT';
    END IF;
END $$;

SET enable_seqscan=off;
SET cuvs.k=1;
DO $$
BEGIN
    IF (SELECT id FROM pending_scan ORDER BY v <-> '[0,0,0]' LIMIT 1) IS DISTINCT FROM 0 OR
       pg_cuvs_last_search_k() IS DISTINCT FROM 1 THEN
        RAISE EXCEPTION 'the page index scan did not use the pending overlay';
    END IF;
END $$;

CREATE TABLE pending_mvcc(id integer, v vector(3));
INSERT INTO pending_mvcc VALUES (1, '[0,0,0]'), (2, '[100,0,0]');
CREATE INDEX pending_mvcc_idx ON pending_mvcc USING cagra (v vector_l2_ops);
INSERT INTO pending_mvcc VALUES (3, '[1,0,0]');
DELETE FROM pending_mvcc WHERE id = 1;
SET cuvs.k=1;
DO $$
BEGIN
    IF (SELECT id FROM pending_mvcc ORDER BY v <-> '[0,0,0]' LIMIT 1) IS DISTINCT FROM 3 OR
       pg_cuvs_last_search_k() IS NOT NULL THEN
        RAISE EXCEPTION 'an invisible base candidate displaced the visible pending neighbor';
    END IF;
END $$;
SET cuvs.k=2;
DO $$
BEGIN
    IF ARRAY(SELECT id FROM pending_mvcc ORDER BY v <-> '[0,0,0]' LIMIT 2)
        IS DISTINCT FROM ARRAY[3,2] OR pg_cuvs_last_search_k() IS NOT NULL THEN
        RAISE EXCEPTION 'the scan did not replace an incomplete overlay shortlist before returning rows';
    END IF;
END $$;

CREATE TABLE pending_expression(id integer, v vector(3));
INSERT INTO pending_expression VALUES (1, '[1,0,0]'), (2, '[2,0,0]');
CREATE INDEX pending_expression_idx ON pending_expression USING cagra
    (((v + '[10,10,10]'::vector)::vector(3)) vector_l2_ops);
BEGIN;
INSERT INTO pending_expression VALUES (0, '[0,0,0]');
SET cuvs.k=2;
DO $$
BEGIN
    IF (SELECT id FROM pending_expression
        ORDER BY ((v + '[10,10,10]'::vector)::vector(3)) <-> '[10,10,10]' LIMIT 1)
        IS DISTINCT FROM 0 OR pg_cuvs_last_search_k() IS DISTINCT FROM 2 THEN
        RAISE EXCEPTION 'the page index scan did not use the stored expression vector';
    END IF;
END $$;
ROLLBACK;

SET cuvs.k=3;
DO $$
BEGIN
    IF (SELECT id FROM pending_expression
        ORDER BY ((v + '[10,10,10]'::vector)::vector(3)) <-> '[10,10,10]' LIMIT 1)
       IS DISTINCT FROM 1 OR pg_cuvs_last_search_k() IS DISTINCT FROM 3 THEN
        RAISE EXCEPTION 'the request-local overlay did not exclude an aborted expression entry';
    END IF;
END $$;
