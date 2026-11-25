\set ON_ERROR_STOP on
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';
SET cuvs.shard_count = 1;
SET cuvs.max_delta_rows = 10000;
SET cuvs.bf_precision = 'float32';
SET cuvs.search_mode = 'brute_force';

CREATE TABLE bf_extend (id int, v vector(8)) WITH (autovacuum_enabled = false);
INSERT INTO bf_extend
SELECT g, format('[%s,0,0,0,0,0,0,0]', g)::vector
FROM generate_series(1, 256) g;
CREATE INDEX bf_extend_idx ON bf_extend USING cagra (v vector_l2_ops);

-- A successful exact query warms the resident BF cache before EXTEND.
SELECT count(*) = 1
FROM pg_cuvs_batch_search('bf_extend', ARRAY['[32,0,0,0,0,0,0,0]']::vector[], 1) b
JOIN bf_extend t ON b.ctid = t.ctid
WHERE t.id = 32 AND b.distance = 0;

INSERT INTO bf_extend VALUES (0, '[0,0,0,0,0,0,0,0]');
SELECT n_vecs = 257 AND extend_count = 1 AND compact_count = 0
FROM pg_stat_gpu_search WHERE index_oid = 'bf_extend_idx'::regclass;

-- The new vector is the unique exact match. The build sidecar lacks it.
DO $$
BEGIN
    PERFORM FROM pg_cuvs_batch_search('bf_extend', ARRAY['[0,0,0,0,0,0,0,0]']::vector[], 1);
    RAISE EXCEPTION 'brute-force search reused the pre-EXTEND cache';
EXCEPTION WHEN object_not_in_prerequisite_state THEN
    IF SQLERRM <> 'pg_cuvs_batch_search: brute_force requires a .vectors sidecar that is missing or stale for this index' THEN
        RAISE;
    END IF;
END;
$$;

REINDEX INDEX bf_extend_idx;
SELECT count(*) = 1
FROM pg_cuvs_batch_search('bf_extend', ARRAY['[0,0,0,0,0,0,0,0]']::vector[], 1) b
JOIN bf_extend t ON b.ctid = t.ctid
WHERE t.id = 0 AND b.distance = 0;

DROP TABLE bf_extend;
