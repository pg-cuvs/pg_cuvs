-- CAGRA tuning GUCs: defaults, validation, SET LOCAL rollback and IPC paths.
-- CPU shim verifies plumbing/results; real-GPU recall/latency is a separate gate.
\set ON_ERROR_STOP on
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';

SELECT current_setting('cuvs.itopk_size') = '0'
   AND current_setting('cuvs.search_width') = '0' AS legacy_defaults;
BEGIN;
SET LOCAL cuvs.itopk_size = 256;
SET LOCAL cuvs.search_width = 4;
ROLLBACK;
SELECT current_setting('cuvs.itopk_size') = '0'
   AND current_setting('cuvs.search_width') = '0' AS rollback_restores_defaults;

\set ON_ERROR_STOP off
SET cuvs.itopk_size = 33;
SET cuvs.search_width = -1;
\set ON_ERROR_STOP on

CREATE TABLE csp_test (id int, v vector(4));
INSERT INTO csp_test
SELECT g, format('[%s,%s,%s,%s]', g, g%7, g%11, g%13)::vector
FROM generate_series(1, 200) g;
CREATE INDEX csp_idx ON csp_test USING cagra (v vector_l2_ops)
  WITH (graph_degree = 32, intermediate_graph_degree = 64);
SET enable_seqscan = off;
SET cuvs.k = 5;

SELECT count(*) = 5 AND bool_or(id = 31) AS default_search
FROM (SELECT id FROM csp_test ORDER BY v <-> '[31,3,9,5]' LIMIT 5) s;
SET cuvs.itopk_size = 128;
SET cuvs.search_width = 2;
SELECT count(*) = 5 AND bool_or(id = 31) AS tuned_search
FROM (SELECT id FROM csp_test ORDER BY v <-> '[31,3,9,5]' LIMIT 5) s;

SET cuvs.cagra_batch_wait_us = 2000;
SELECT count(*) = 5 AND bool_or(id = 31) AS tuned_microbatch
FROM (SELECT id FROM csp_test ORDER BY v <-> '[31,3,9,5]' LIMIT 5) s;
SELECT count(*) = 10 AND count(DISTINCT query_idx) = 2 AS tuned_batch
FROM pg_cuvs_batch_search('csp_test',
  ARRAY['[31,3,9,5]', '[32,4,10,6]']::vector[], 5);

RESET cuvs.itopk_size;
RESET cuvs.search_width;
RESET cuvs.cagra_batch_wait_us;
RESET cuvs.k;
RESET enable_seqscan;
DROP TABLE csp_test;
