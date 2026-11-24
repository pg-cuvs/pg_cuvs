\set ON_ERROR_STOP on
CREATE EXTENSION vector;
CREATE EXTENSION pg_cuvs;
CREATE TABLE page_gpu_build(id int, v vector(8));
INSERT INTO page_gpu_build SELECT i,array_fill(i::real,ARRAY[8])::vector FROM generate_series(1,256) i;
CREATE INDEX page_gpu_build_idx ON page_gpu_build USING cagra (v vector_l2_ops);
SELECT pg_relation_size('page_gpu_build_idx') > 0;
SET enable_seqscan=off;
DO $$
BEGIN
    IF (SELECT id FROM page_gpu_build ORDER BY v <-> '[1,1,1,1,1,1,1,1]' LIMIT 1)
           IS DISTINCT FROM 1 THEN
        RAISE EXCEPTION 'the page-format index did not serve the expected heap row';
    END IF;
END $$;
