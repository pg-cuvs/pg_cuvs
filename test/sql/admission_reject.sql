-- admission_reject.sql — I3 (v6 r18 C5, UC-2; CK-8, CK-29): a permanent
-- admission rejection (AR0) of a CAGRA and of an IVF-PQ build reaches the
-- user as SQLSTATE 54000 with the C5 errmsg, DETAIL and HINT, and is kept for
-- pg_cuvs_last_rejection(). AR0 is forced with the daemon's runtime budget
-- override (pg_cuvs_set_vram_budget), so the numbers are deterministic:
--   cagra 1000 x dim 8, gd 64: final 288,000 B, N = final + ceil(final x 7726
--   / 1000) = 2,513,088 B -> 2 MiB against a 1 MiB budget;
--   ivfpq 1000 x dim 8: 1000 x pq_dim 4 x 2 = 8,000 B against 4,096 B.
-- The budget is restored at the end.
\set ON_ERROR_STOP on
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
RESET client_min_messages;
SET cuvs.index_dir = '/tmp/cuvs_indexes';
SET max_parallel_maintenance_workers = 0;

-- No rejection yet in this session.
SELECT pg_cuvs_last_rejection() IS NULL AS no_rejection_yet;

CREATE TABLE ar_items (id int, embedding vector(8));
INSERT INTO ar_items
SELECT g, format('[%s,%s,0,0,0,0,0,1]', g, g % 7)::vector FROM generate_series(1, 1000) g;

SELECT pg_cuvs_set_vram_budget(1048576);
\set ON_ERROR_STOP off
CREATE INDEX ar_cagra ON ar_items USING cagra (embedding vector_l2_ops);
\echo :LAST_ERROR_SQLSTATE
\set ON_ERROR_STOP on
SELECT r->>'reason' AS reason, r->>'sqlstate' AS sqlstate, r->>'requester' AS requester,
       r->'fields' AS fields, r->'candidates' AS candidates, r->'shard' AS shard
FROM pg_cuvs_last_rejection() r;

SELECT pg_cuvs_set_vram_budget(4096);
\set ON_ERROR_STOP off
CREATE INDEX ar_ivfpq ON ar_items USING ivfpq (embedding vector_l2_ops);
\echo :LAST_ERROR_SQLSTATE
\set ON_ERROR_STOP on
SELECT r->>'reason' AS reason, r->>'sqlstate' AS sqlstate,
       (r->>'index_oid')::oid <> 0 AS has_index_oid
FROM pg_cuvs_last_rejection() r;

-- Sharded build (CK-36 (d')): 4000 rows in 2 shards of 2000, each
-- N = 576,000 + 4,450,176 = 5,026,176 B (4 MiB) against 3 MiB. The shard
-- rejection carries its shard number in DETAIL and in the jsonb.
CREATE TABLE ar_shard (id int, embedding vector(8));
INSERT INTO ar_shard
SELECT g, format('[%s,%s,0,0,0,0,1,0]', g, g % 11)::vector FROM generate_series(1, 4000) g;
SET cuvs.shard_count = 2;
SELECT pg_cuvs_set_vram_budget(3145728);
\set ON_ERROR_STOP off
CREATE INDEX ar_sharded ON ar_shard USING cagra (embedding vector_l2_ops);
\echo :LAST_ERROR_SQLSTATE
\set ON_ERROR_STOP on
SELECT r->>'reason' AS reason, r->>'sqlstate' AS sqlstate, r->>'requester' AS requester,
       r->'shard' AS shard
FROM pg_cuvs_last_rejection() r;
RESET cuvs.shard_count;

-- Restored budget: the same build is admitted.
SELECT pg_cuvs_set_vram_budget(0);
CREATE INDEX ar_cagra ON ar_items USING cagra (embedding vector_l2_ops);
SELECT pg_cuvs_last_rejection()->>'reason' AS last_reason_unchanged;

DROP TABLE ar_items;
DROP TABLE ar_shard;
