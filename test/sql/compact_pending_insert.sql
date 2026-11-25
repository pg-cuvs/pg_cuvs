-- Standalone test for an isolated CUVS_TEST_HOOKS daemon started with
-- CUVS_FAULT_SAVE_INDEX=1. Supply fault_socket and fault_index_dir to psql.
\set ON_ERROR_STOP on
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.socket_path = :'fault_socket';
SET cuvs.index_dir = :'fault_index_dir';
SET cuvs.shard_count = 1;
SET cuvs.max_delta_rows = 10000;

CREATE TABLE compact_pending_insert (id int, v vector(8)) WITH (autovacuum_enabled = false);
INSERT INTO compact_pending_insert
SELECT g, format('[%s,0,0,0,0,0,0,0]', g)::vector
FROM generate_series(1, 256) g;
CREATE INDEX compact_pending_insert_idx ON compact_pending_insert USING cagra (v vector_l2_ops);
DELETE FROM compact_pending_insert WHERE id = 256;
VACUUM (INDEX_CLEANUP ON) compact_pending_insert;

DO $$
BEGIN
    BEGIN
        PERFORM pg_cuvs_compact('compact_pending_insert_idx');
        RAISE EXCEPTION 'fixture did not reach a failing COMPACT save';
    EXCEPTION WHEN internal_error THEN
        IF SQLERRM <> 'pg_cuvs_compact: daemon returned status 6' THEN
            RAISE;
        END IF;
    END;

    BEGIN
        INSERT INTO compact_pending_insert VALUES (257, '[0,0,0,0,0,0,0,0]');
        RAISE EXCEPTION 'INSERT succeeded while compaction was pending';
    EXCEPTION WHEN serialization_failure THEN
        IF SQLERRM <> 'pg_cuvs: cannot insert while CAGRA compaction is pending' THEN
            RAISE;
        END IF;
    END;

    IF EXISTS (SELECT FROM compact_pending_insert WHERE id = 257) OR NOT EXISTS (
        SELECT FROM pg_stat_gpu_search WHERE index_oid = 'compact_pending_insert_idx'::regclass
          AND n_vecs = 256 AND extend_count = 0 AND compact_count = 0
    ) THEN
        RAISE EXCEPTION 'the rejected INSERT or failed COMPACT changed the heap or resident graph';
    END IF;
END;
$$;

DROP TABLE compact_pending_insert;
