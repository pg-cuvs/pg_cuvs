-- cagra_dynbatch.sql — #160: dynamic batching on the CAGRA single-query AM path.
--
-- Phase 3L-9 shipped a micro-batch worker for the brute-force path only
-- (cuvs.bf_batch_wait_us). #160 extends the same queue/worker to unsharded
-- CAGRA single searches behind its own GUC, cuvs.cagra_batch_wait_us.
--
-- Coverage (single connection, so every batch here has width 1 — the point is
-- the ROUTING and its observability, not the throughput win, which needs
-- concurrency and a real GPU):
--   1. cuvs.cagra_batch_wait_us > 0 routes a cagra scan through the batch
--      worker: pg_stat_gpu_search.cagra_batch_count advances by >= 1.
--   2. The default (0) does NOT route: the counter delta is exactly 0.
--   3. Results are identical with and without the batch window — a batch of
--      one must return exactly what the immediate dispatch returns.
--   4. brute_force requests are not stolen by the cagra window: with
--      cagra_batch_wait_us > 0 and bf_batch_wait_us = 0, a brute_force scan
--      leaves BOTH counters' deltas at 0 (the two gates stay independent).
--
-- Concurrency-driven coalescing (width > 1) and the recall delta of the
-- multi-CTA batch kernel (#144) are measured on the GPU VM, not here: pg_regress
-- gives one session, and the Tier-1 CPU shim's batch entry point is a per-query
-- exact search, so it cannot reproduce a kernel recall difference.
--
-- REQUIRES: pg_cuvs_server running; cuvs.index_dir writable.

\set ON_ERROR_STOP on

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
RESET client_min_messages;

SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- Deterministic 200-vector, 8-dim corpus (same generator as brute_force.sql).
CREATE TABLE cdb_test (id int, embedding vector(8));
INSERT INTO cdb_test
SELECT g,
       format('[%s,%s,%s,%s,%s,%s,%s,%s]',
              (g * 0.013)::numeric(12,6),
              (g * g * 0.0007)::numeric(12,6),
              sin(g * 0.10)::numeric(12,6),
              cos(g * 0.17)::numeric(12,6),
              ((g % 13) * 0.05)::numeric(12,6),
              ((g % 7) * 0.08)::numeric(12,6),
              sin(g * 0.30)::numeric(12,6),
              cos(g * 0.23)::numeric(12,6))::vector
FROM generate_series(1, 200) g;

SET client_min_messages = 'warning';
CREATE INDEX cdb_idx ON cdb_test USING cagra (embedding vector_l2_ops);
SET client_min_messages = 'notice';

SET enable_seqscan = off;

-- ── baseline: immediate dispatch (window off, the default) ───────
CREATE TEMP TABLE cdb_base AS
    SELECT id FROM cdb_test
    ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 10;

-- The counter exists and starts where the immediate path left it.
CREATE TEMP TABLE cdb_mark AS
    SELECT cagra_batch_count AS c, bf_batch_count AS b
    FROM pg_stat_gpu_search WHERE index_name = 'cdb_idx';

-- A second immediate scan must not touch the batch counter.
CREATE TEMP TABLE cdb_base2 AS
    SELECT id FROM cdb_test
    ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 10;

SELECT s.cagra_batch_count - m.c = 0 AS off_does_not_batch
FROM pg_stat_gpu_search s, cdb_mark m WHERE s.index_name = 'cdb_idx';

-- ── batched: window on ───────────────────────────────────────────
SET cuvs.cagra_batch_wait_us = 2000;
CREATE TEMP TABLE cdb_batched AS
    SELECT id FROM cdb_test
    ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 10;

-- The batch worker served at least one coalesced dispatch.
SELECT s.cagra_batch_count - m.c >= 1 AS on_routes_through_worker
FROM pg_stat_gpu_search s, cdb_mark m WHERE s.index_name = 'cdb_idx';

-- A batch of one returns exactly what the immediate path returned.
SELECT (SELECT array_agg(id ORDER BY id) FROM cdb_base)
     = (SELECT array_agg(id ORDER BY id) FROM cdb_batched) AS batched_matches_immediate;

-- ── the two gates are independent ────────────────────────────────
-- cagra window still on, BF window off: a brute_force scan takes neither
-- batch path.
CREATE TEMP TABLE cdb_mark2 AS
    SELECT cagra_batch_count AS c, bf_batch_count AS b
    FROM pg_stat_gpu_search WHERE index_name = 'cdb_idx';

SET cuvs.bf_batch_wait_us = 0;
SET cuvs.search_mode = 'brute_force';
CREATE TEMP TABLE cdb_bf AS
    SELECT id FROM cdb_test
    ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 10;
RESET cuvs.search_mode;

SELECT s.cagra_batch_count - m.c = 0 AS cagra_window_leaves_bf_alone,
       s.bf_batch_count    - m.b = 0 AS bf_window_off_stays_off
FROM pg_stat_gpu_search s, cdb_mark2 m WHERE s.index_name = 'cdb_idx';

RESET cuvs.cagra_batch_wait_us;
RESET cuvs.bf_batch_wait_us;
RESET enable_seqscan;

DROP TABLE cdb_test;
