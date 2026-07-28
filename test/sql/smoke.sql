-- smoke.sql — Phase 1 load and registration test.
-- Verifies extension install, AM, operator classes, GUCs, functions, and a
-- durable CREATE INDEX. The CREATE INDEX path REQUIRES pg_cuvs_server to be
-- running and cuvs.index_dir to point at a daemon-writable directory.

\set ON_ERROR_STOP on

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
RESET client_min_messages;

-- Point at the daemon's index dir so build artifacts persist where the
-- pg_cuvs_server process (running as its own user) can write them.
SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- Access method registered?
SELECT amname FROM pg_am WHERE amname = 'cagra';

-- Operator classes registered?
SELECT opcname FROM pg_opclass o
JOIN pg_am a ON a.oid = o.opcmethod
WHERE a.amname = 'cagra'
ORDER BY opcname;

-- GUCs registered?
SHOW enable_cuvs;
SHOW cuvs.socket_path;
SHOW cuvs.circuit_breaker_threshold;

-- pg_cuvs_reset_circuit function registered?
SELECT proname FROM pg_proc WHERE proname = 'pg_cuvs_reset_circuit';

-- CREATE INDEX USING cagra on a small table.
-- With no daemon running, cuvs_ambuild now ereport(ERROR)s for DDL
-- durability, so this CREATE INDEX fails unless a daemon is reachable.
CREATE TABLE smoke_items (id bigint, embedding vector(4));
INSERT INTO smoke_items VALUES
    (1, '[1,0,0,0]'), (2, '[0,1,0,0]'),
    (3, '[0,0,1,0]'), (4, '[0,0,0,1]');

CREATE INDEX cagra_idx ON smoke_items
    USING cagra (embedding vector_l2_ops);

-- Index entry exists in pg_index?
SELECT indexrelid::regclass FROM pg_index
WHERE indrelid = 'smoke_items'::regclass
  AND indexrelid::regclass::text = 'cagra_idx';

-- CPU fallback trigger 2: enable_cuvs = off routes to pgvector
CREATE INDEX hnsw_idx ON smoke_items
    USING hnsw (embedding vector_l2_ops);

SET enable_cuvs = off;
-- [1,0.5,0,0] has an unambiguous top-2 (id 1 then id 2), so LIMIT 2 output
-- is deterministic (no distance tie in the first two rows).
SELECT id FROM smoke_items
ORDER BY embedding <-> '[1,0.5,0,0]'::vector LIMIT 2;
SET enable_cuvs = on;

-- Cleanup
DROP TABLE smoke_items;
DROP EXTENSION pg_cuvs;
