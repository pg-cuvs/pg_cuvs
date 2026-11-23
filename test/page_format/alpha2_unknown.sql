\set ON_ERROR_STOP on
CREATE EXTENSION vector;
CREATE EXTENSION pg_cuvs;
CREATE TABLE unknown_layout(id int, v vector(3));
INSERT INTO unknown_layout VALUES (1,'[1,2,3]'), (2,'[2,2,2]');
CREATE INDEX unknown_layout_idx ON unknown_layout USING cagra(v vector_l2_ops);
SET enable_seqscan=off;
DO $$
BEGIN
    IF (SELECT id FROM unknown_layout ORDER BY v <-> '[1,2,3]' LIMIT 1) IS DISTINCT FROM 1 THEN
        RAISE EXCEPTION 'unknown format did not fall back to the exact heap result';
    END IF;
    BEGIN
        INSERT INTO unknown_layout VALUES (3,'[0,0,0]');
        RAISE EXCEPTION 'unknown format accepted a write';
    EXCEPTION WHEN data_corrupted THEN
        NULL;
    END;
END $$;
