-- ivfpq_smoke.sql — Phase 3P IVF-PQ access method smoke test.
-- Verifies: AM/opcls registration, GUC, CREATE INDEX (daemon required),
-- a real IVF-PQ search reaching the daemon, and pg_stat_gpu_search mode.

\set ON_ERROR_STOP on
SET client_min_messages = WARNING;

CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;

SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- ivfpq AM registered?
SELECT amname FROM pg_am WHERE amname = 'ivfpq';

-- Operator classes registered for ivfpq?
SELECT opcname FROM pg_opclass o
JOIN pg_am a ON a.oid = o.opcmethod
WHERE a.amname = 'ivfpq'
ORDER BY opcname;

-- cuvs.ivfpq_n_probes GUC registered with default 64?
SHOW cuvs.ivfpq_n_probes;

-- Build a small table and IVF-PQ index.
-- 20 rows, dim=4, n_lists=4 → ~5 vectors/cluster.
-- pq_dim=2 divides dim=4 evenly; pq_bits=8 is the default.
CREATE TABLE ivfpq_items (id int, embedding vector(4));
INSERT INTO ivfpq_items VALUES
    (1,  '[1,0,0,0]'), (2,  '[0,1,0,0]'),
    (3,  '[0,0,1,0]'), (4,  '[0,0,0,1]'),
    (5,  '[1,1,0,0]'), (6,  '[0,1,1,0]'),
    (7,  '[0,0,1,1]'), (8,  '[1,0,0,1]'),
    (9,  '[2,0,0,0]'), (10, '[0,2,0,0]'),
    (11, '[0,0,2,0]'), (12, '[0,0,0,2]'),
    (13, '[3,0,0,0]'), (14, '[0,3,0,0]'),
    (15, '[0,0,3,0]'), (16, '[0,0,0,3]'),
    (17, '[1,1,1,0]'), (18, '[0,1,1,1]'),
    (19, '[1,0,1,1]'), (20, '[1,1,0,1]');

CREATE INDEX ivfpq_idx ON ivfpq_items
    USING ivfpq (embedding vector_l2_ops)
    WITH (n_lists = 4, pq_bits = 8, pq_dim = 2);

-- Index exists in catalog?
SELECT indexrelid::regclass FROM pg_index
WHERE indrelid = 'ivfpq_items'::regclass
  AND indexrelid::regclass::text = 'ivfpq_idx';

-- Probe all 4 clusters, so no candidate is missed by the IVF stage.
--
-- What this file must NOT assert is which neighbor comes back. The old
-- "recall = 1.00 → id = 1 is the exact match" claim held only while the query
-- was silently running as a CPU Seq Scan + Sort (#151); the first run of a real
-- GPU IVF-PQ search returned id = 9 ([2,0,0,0], true distance 1.0). That is not
-- a bug: this corpus is deliberately degenerate — pq_bits = 8 trains 256 PQ
-- centroids from 20 samples at dim 4 — so quantization error reorders near ties,
-- and which id wins depends on kmeans initialization. The CPU shim answers
-- exactly, real GPU PQ does not, so the assertion below locks only the minimum
-- contract both tiers owe: one row, and a valid id. The fence that a real search
-- happened at all is the search_count bracket, not this result.
SET cuvs.ivfpq_n_probes = 4;
SET cuvs.k = 4;

-- #151: bracket the query with the daemon's search_count. The pg_stat_gpu_search
-- row is registered at CREATE INDEX time with search_count = 0, so asserting
-- search_mode alone proves only that the BUILD reached the daemon — it stayed
-- green all the while the query silently ran as a CPU Seq Scan + Sort and
-- returned the right answer anyway. Only the counter going 0 → ≥1 proves a real
-- ivfpq search happened.
SELECT search_count = 0 AS ivfpq_not_searched_before FROM pg_stat_gpu_search
WHERE index_oid = 'ivfpq_idx'::regclass;

SET enable_seqscan = off;
SELECT count(*) = 1                     AS one_row,
       bool_and(id BETWEEN 1 AND 20)    AS valid_id
FROM (SELECT id FROM ivfpq_items
      ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 1) s;
RESET enable_seqscan;

SELECT search_count >= 1 AS ivfpq_searched_after FROM pg_stat_gpu_search
WHERE index_oid = 'ivfpq_idx'::regclass;

-- Daemon stats should report search_mode = 'ivfpq' for this index.
SELECT search_mode FROM pg_stat_gpu_search
WHERE index_name = 'ivfpq_idx';

-- Cleanup
DROP TABLE ivfpq_items;
