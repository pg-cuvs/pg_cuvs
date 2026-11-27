-- Standalone reproducer for the cold-cache availability failure.
-- Run with psql -X -v ON_ERROR_STOP=1 -f test/sql/filtered_knn_after_extend.sql.
-- Until a current vector source is available, the search fails with status 10.
\set ON_ERROR_STOP on
BEGIN;
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';
SET cuvs.shard_count = 1;
SET cuvs.max_delta_rows = 10000;
SET cuvs.bf_precision = 'float32';

CREATE TABLE filtered_extend (id int, v vector(8)) WITH (autovacuum_enabled = false);
INSERT INTO filtered_extend
SELECT g, format('[%s,0,0,0,0,0,0,0]', g)::vector
FROM generate_series(1, 256) g;
CREATE INDEX filtered_extend_idx ON filtered_extend USING cagra (v vector_l2_ops);
INSERT INTO filtered_extend VALUES (0, '[0,0,0,0,0,0,0,0]');

DO $$
BEGIN
    IF NOT EXISTS (SELECT FROM pg_stat_gpu_search
                   WHERE index_oid = 'filtered_extend_idx'::regclass
                     AND n_vecs = 257 AND extend_count = 1 AND compact_count = 0) THEN
        RAISE EXCEPTION 'fixture did not reach EXTEND';
    END IF;

    IF NOT EXISTS (
        SELECT FROM cuvs_filtered_knn('filtered_extend_idx', '[0,0,0,0,0,0,0,0]', NULL::bigint[], 1) f
        JOIN filtered_extend t ON f.ctid = t.ctid
        WHERE t.id = 0 AND f.distance = 0
    ) THEN
        RAISE EXCEPTION 'filtered kNN did not return the exact match after EXTEND';
    END IF;
END;
$$;
ROLLBACK;
