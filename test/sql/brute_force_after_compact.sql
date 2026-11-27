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

CREATE TABLE bf_compact (id int, v vector(8)) WITH (autovacuum_enabled = false);
INSERT INTO bf_compact
SELECT g, format('[%s,0,0,0,0,0,0,0]', g)::vector
FROM generate_series(1, 256) g;
CREATE INDEX bf_compact_idx ON bf_compact USING cagra (v vector_l2_ops);

-- Warm the cache and prove the query selects the row that will survive VACUUM.
SELECT count(*) = 1
FROM pg_cuvs_batch_search('bf_compact', ARRAY['[32,0,0,0,0,0,0,0]']::vector[], 1) b
JOIN bf_compact t ON b.ctid = t.ctid
WHERE t.id = 32 AND b.distance = 0;

DELETE FROM bf_compact WHERE id = 1;
VACUUM (INDEX_CLEANUP ON) bf_compact;
SELECT n_vecs = 255 AND extend_count = 0 AND compact_count = 1
FROM pg_stat_gpu_search WHERE index_oid = 'bf_compact_idx'::regclass;

-- COMPACT changed the item-id to TID mapping. The old cache must not serve it.
DO $$
BEGIN
    PERFORM FROM pg_cuvs_batch_search('bf_compact', ARRAY['[32,0,0,0,0,0,0,0]']::vector[], 1);
    RAISE EXCEPTION 'brute-force search reused the pre-COMPACT cache';
EXCEPTION WHEN object_not_in_prerequisite_state THEN
    IF SQLERRM <> 'pg_cuvs_batch_search: brute_force requires a .vectors sidecar that is missing or stale for this index' THEN
        RAISE;
    END IF;
END;
$$;

REINDEX INDEX bf_compact_idx;
SELECT count(*) = 1
FROM pg_cuvs_batch_search('bf_compact', ARRAY['[32,0,0,0,0,0,0,0]']::vector[], 1) b
JOIN bf_compact t ON b.ctid = t.ctid
WHERE t.id = 32 AND b.distance = 0;

DROP TABLE bf_compact;
