-- delta_tail_merge.sql: rows appended to .delta one by one, across the threshold at which the daemon
-- moves them into its GPU delta index, are merged as the sequential scan ranks them: three metrics on a
-- sharded index (every INSERT goes to .delta), and the micro-batch path on an unsharded one.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';
SELECT pg_cuvs_set_vram_budget(0) \g /dev/null
CREATE TEMP TABLE dtm_result (name text, searches int, wrong int, merged boolean);
DO $$
DECLARE
  c record;
  i int;
  q int;
  vb bigint;
  ids_exact int[];
  ids_index int[];
  n int;
  bad int;
  merged bigint;
BEGIN
  PERFORM set_config('cuvs.itopk_size', '512', true);
  PERFORM set_config('enable_bitmapscan', 'off', true);
  FOR c IN SELECT * FROM (VALUES
      ('cosine',   'vector_cosine_ops', '<=>', 2, 0),
      ('ip',       'vector_ip_ops',     '<#>', 2, 0),
      ('l2',       'vector_l2_ops',     '<->', 2, 0),
      ('l2_batch', 'vector_l2_ops',     '<->', 1, 200)) AS x(name, ops, op, shards, wait_us)
  LOOP
    CREATE TABLE dtm (id int, v vector(8));
    CREATE TEMP TABLE dtm_src AS
      SELECT g AS id, ARRAY[sin(g), cos(g * 1.7), sin(g * 0.3), cos(g * 0.11),
                            g * 0.001, 1, sin(g * 2.9), cos(g * 0.77)]::float4[]::vector(8) AS v
      FROM generate_series(1, 242) g;
    INSERT INTO dtm SELECT id, v FROM dtm_src WHERE id <= 200;
    PERFORM set_config('cuvs.shard_count', c.shards::text, true);
    EXECUTE format('CREATE INDEX dtm_i ON dtm USING cagra (v %s)', c.ops);
    PERFORM set_config('cuvs.cagra_batch_wait_us', c.wait_us::text, true);
    SELECT vram_bytes INTO vb FROM pg_stat_gpu_search WHERE index_oid = 'dtm_i'::regclass;
    n := 0;
    bad := 0;
    FOR i IN 201..242 LOOP
      IF c.shards = 1 THEN PERFORM pg_cuvs_set_vram_budget(vb); END IF;
      INSERT INTO dtm SELECT id, v FROM dtm_src WHERE id = i;
      IF c.shards = 1 THEN PERFORM pg_cuvs_set_vram_budget(0); END IF;
      FOREACH q IN ARRAY ARRAY[i, i - 150] LOOP
        PERFORM set_config('enable_indexscan', 'off', true);
        PERFORM set_config('enable_seqscan', 'on', true);
        EXECUTE format('SELECT ARRAY(SELECT id FROM dtm ORDER BY v %s %L::vector LIMIT 3)',
                       c.op, (SELECT v::text FROM dtm_src WHERE id = q)) INTO ids_exact;
        PERFORM set_config('enable_indexscan', 'on', true);
        PERFORM set_config('enable_seqscan', 'off', true);
        EXECUTE format('SELECT ARRAY(SELECT id FROM dtm ORDER BY v %s %L::vector LIMIT 3)',
                       c.op, (SELECT v::text FROM dtm_src WHERE id = q)) INTO ids_index;
        n := n + 1;
        IF ids_index IS DISTINCT FROM ids_exact THEN bad := bad + 1; END IF;
      END LOOP;
    END LOOP;
    PERFORM set_config('enable_seqscan', 'on', true);
    SELECT delta_merged_count INTO merged FROM pg_stat_gpu_search WHERE index_oid = 'dtm_i'::regclass;
    INSERT INTO dtm_result VALUES (c.name, n, bad, merged >= n);
    DROP TABLE dtm;
    DROP TABLE dtm_src;
  END LOOP;
END
$$;
SELECT name, searches, wrong, merged AS daemon_merged FROM dtm_result ORDER BY name COLLATE "C";
DROP TABLE dtm_result;
