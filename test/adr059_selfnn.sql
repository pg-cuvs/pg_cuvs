-- ADR-059 verification: self-NN equivalence between the single-corpus build
-- (max_parallel_maintenance_workers=0) and the multi-partial parallel build
-- (workers=4 -> handle_build_multi direct H2D). Distinct per-row vectors so
-- each row's nearest neighbor is itself (degenerate all-equal data is invalid
-- for recall -- see lessons). 50k x 128 -> heap large enough to recruit workers.
\timing off
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';
DROP TABLE IF EXISTS adr059 CASCADE;

-- Per-row randomness via cross join + GROUP BY (random() evaluated per (id,dim),
-- NOT an uncorrelated InitPlan -> genuinely distinct vectors).
CREATE TABLE adr059 AS
SELECT id, array_agg(random() ORDER BY d)::real[]::vector(128) AS v
FROM generate_series(1, 50000) AS id,
     generate_series(1, 128)   AS d
GROUP BY id;
ALTER TABLE adr059 ADD PRIMARY KEY (id);

-- ---- Single-corpus build ----
SET max_parallel_maintenance_workers = 0;
DROP INDEX IF EXISTS adr059_idx;
CREATE INDEX adr059_idx ON adr059 USING cagra (v vector_l2_ops);
SET cuvs.k = 1;
\echo '=== single (workers=0) self-NN: nn should equal id for all 5 ==='
SELECT id,
       (SELECT u2.id FROM adr059 u2 ORDER BY u2.v <-> u1.v LIMIT 1) AS nn,
       id = (SELECT u2.id FROM adr059 u2 ORDER BY u2.v <-> u1.v LIMIT 1) AS self_nn
FROM adr059 u1 WHERE id IN (1, 137, 5000, 25000, 50000) ORDER BY id;

-- ---- Parallel multi-partial build ----
SET max_parallel_maintenance_workers = 4;
SET min_parallel_table_scan_size = 0;     -- force worker recruitment
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
DROP INDEX adr059_idx;
CREATE INDEX adr059_idx ON adr059 USING cagra (v vector_l2_ops);
SET cuvs.k = 1;
\echo '=== parallel (workers=4, multi-partial) self-NN: nn should equal id for all 5 ==='
SELECT id,
       (SELECT u2.id FROM adr059 u2 ORDER BY u2.v <-> u1.v LIMIT 1) AS nn,
       id = (SELECT u2.id FROM adr059 u2 ORDER BY u2.v <-> u1.v LIMIT 1) AS self_nn
FROM adr059 u1 WHERE id IN (1, 137, 5000, 25000, 50000) ORDER BY id;

DROP TABLE adr059 CASCADE;
