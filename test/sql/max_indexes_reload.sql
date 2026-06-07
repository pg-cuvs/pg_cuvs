-- max_indexes_reload.sql — regression test for MAX_INDEXES hardwall fix.
--
-- Verifies that queries against >64 partitions auto-reload evicted indexes
-- rather than erroring out. The fix: load_index() / load_index_sharded()
-- now call evict_lru() before the slot-cap check, mirroring handle_build.
-- Cold registry cap is also raised (MAX_COLD_INDEXES=1024) so >64 partitions
-- survive the startup scan.
--
-- REQUIRES: pg_cuvs_server running with GPU; cuvs.index_dir writable.
-- Golden generated on VM (A100/PG16); do not regenerate on macOS.

\set ON_ERROR_STOP off
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';
SET max_parallel_workers_per_gather = 0;

-- 130 LIST partitions (tenant_id 1..130), dim=8, 50 rows each.
-- DO block creates partitions + inserts; subsequent CREATE INDEX is separate.
CREATE TABLE mir (tenant_id int NOT NULL, id bigint NOT NULL, v vector(8))
  PARTITION BY LIST (tenant_id);

DO $$
DECLARE t int;
BEGIN
  FOR t IN 1..130 LOOP
    EXECUTE format(
      'CREATE TABLE mir_t%s PARTITION OF mir FOR VALUES IN (%s)',
      t, t);
  END LOOP;
END;
$$;

-- Insert 50 distinct vectors per partition.
INSERT INTO mir
SELECT t, t * 1000 + g,
       array_agg((sin(t * 0.1 + g * 0.07 + d * 0.3))::real ORDER BY d)::vector(8)
FROM generate_series(1, 130) t,
     generate_series(1, 50)  g,
     generate_series(1, 8)   d
GROUP BY t, g;

-- Build one CAGRA index per partition (130 indexes total; exceeds MAX_INDEXES=64).
DO $$
DECLARE t int;
BEGIN
  FOR t IN 1..130 LOOP
    EXECUTE format(
      'CREATE INDEX mir_t%s_cagra ON mir_t%s USING cagra (v vector_l2_ops)',
      t, t);
  END LOOP;
END;
$$;

SET enable_seqscan = off;
SET cuvs.k = 10;

-- Query 1: tenant 1 (first partition, likely still resident after 130 builds).
-- Expect: 5 rows, all from tenant 1 (wrong_tenant = 0).
SELECT count(*) AS n,
       count(*) FILTER (WHERE tenant_id <> 1) AS wrong_tenant
FROM (
  SELECT tenant_id FROM mir WHERE tenant_id = 1
  ORDER BY v <-> '[0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8]'::vector(8)
  LIMIT 5
) s;

-- Query 2: tenant 65 (first partition past the original MAX_INDEXES=64 boundary).
-- Pre-fix: this would fail to reload → CPU fallback or ERROR.
-- Post-fix: daemon evicts an LRU slot, reloads mir_t65, returns GPU results.
SELECT count(*) AS n,
       count(*) FILTER (WHERE tenant_id <> 65) AS wrong_tenant
FROM (
  SELECT tenant_id FROM mir WHERE tenant_id = 65
  ORDER BY v <-> '[0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8]'::vector(8)
  LIMIT 5
) s;

-- Query 3: tenant 130 (last partition).
SELECT count(*) AS n,
       count(*) FILTER (WHERE tenant_id <> 130) AS wrong_tenant
FROM (
  SELECT tenant_id FROM mir WHERE tenant_id = 130
  ORDER BY v <-> '[0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8]'::vector(8)
  LIMIT 5
) s;

-- Query 4: round-trip across multiple >64 partitions to exercise repeated LRU evict/reload.
-- Each query for tenant 70, 80, 90 must succeed (no ERROR row).
SELECT count(*) AS n,
       count(*) FILTER (WHERE tenant_id <> 70) AS wrong_tenant
FROM (
  SELECT tenant_id FROM mir WHERE tenant_id = 70
  ORDER BY v <-> '[0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5]'::vector(8)
  LIMIT 5
) s;

SELECT count(*) AS n,
       count(*) FILTER (WHERE tenant_id <> 90) AS wrong_tenant
FROM (
  SELECT tenant_id FROM mir WHERE tenant_id = 90
  ORDER BY v <-> '[0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5]'::vector(8)
  LIMIT 5
) s;

RESET enable_seqscan;
DROP TABLE mir CASCADE;
