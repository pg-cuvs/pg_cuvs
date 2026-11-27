-- compact_graph_degree.sql — #261: compaction rebuilds the kept rows with the
-- index's own graph_degree, not the cuVS default (64), and reloptions still
-- describe the graph afterwards. The width is read from the graph array of the
-- persisted .cagra file (its numpy shape is (rows, graph_degree)), which is what
-- a daemon restart reloads. GPU only (REGRESS_TIER2_ONLY): the CPU shim keeps no
-- graph and writes its own file format.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
RESET client_min_messages;
SET cuvs.index_dir = '/tmp/cuvs_indexes';

CREATE FUNCTION pg_temp.cgd_vec(g int) RETURNS vector LANGUAGE sql IMMUTABLE AS
$$ SELECT (SELECT array_agg(((hashtext(g::text || ':' || d::text) & 65535) / 65536.0)::real)
           FROM generate_series(1, 16) d)::vector $$;
-- {rows, graph_degree} of the graph stored in the index's .cagra file
CREATE FUNCTION pg_temp.cgd_graph(i regclass) RETURNS text LANGUAGE sql AS $$
  SELECT (regexp_match(encode(pg_read_binary_file(
      current_setting('cuvs.index_dir') || '/' ||
      (SELECT oid FROM pg_database WHERE datname = current_database()) || '_' || i::oid || '.cagra',
      0, 2048), 'escape'), '''shape'': \((\d+), (\d+)\)'))::text $$;

CREATE TABLE cgd32 (id int, v vector(16)) WITH (autovacuum_enabled = off);
INSERT INTO cgd32 SELECT g, pg_temp.cgd_vec(g) FROM generate_series(1, 3000) g;
CREATE INDEX cgd32_c ON cgd32 USING cagra (v vector_l2_ops) WITH (graph_degree = 32);
CREATE TABLE cgd128 (id int, v vector(16)) WITH (autovacuum_enabled = off);
INSERT INTO cgd128 SELECT g, pg_temp.cgd_vec(g) FROM generate_series(1, 3000) g;
CREATE INDEX cgd128_c ON cgd128 USING cagra (v vector_l2_ops) WITH (graph_degree = 128);
SELECT 'build' AS step, c.relname, pg_temp.cgd_graph(c.oid::regclass) AS graph, c.reloptions
  FROM pg_class c WHERE c.relname IN ('cgd32_c', 'cgd128_c') ORDER BY 2;

-- VACUUM after deleting 400 rows (13%) compacts.
DELETE FROM cgd32 WHERE id <= 400;
VACUUM cgd32;
DELETE FROM cgd128 WHERE id <= 400;
VACUUM cgd128;
SELECT index_oid::regclass AS index, compact_count FROM pg_stat_gpu_search
 WHERE index_oid IN ('cgd32_c'::regclass, 'cgd128_c'::regclass) ORDER BY 1;
SELECT 'vacuum' AS step, c.relname, pg_temp.cgd_graph(c.oid::regclass) AS graph, c.reloptions
  FROM pg_class c WHERE c.relname IN ('cgd32_c', 'cgd128_c') ORDER BY 2;

-- pg_cuvs_compact() sends the same request. VACUUM without a daemon socket
-- records the tombstones but cannot compact; the call below then does.
DELETE FROM cgd32 WHERE id <= 800;
DELETE FROM cgd128 WHERE id <= 800;
SET cuvs.socket_path = '';
VACUUM cgd32;
VACUUM cgd128;
RESET cuvs.socket_path;
SELECT pg_cuvs_compact('cgd32_c');
SELECT pg_cuvs_compact('cgd128_c');
SELECT index_oid::regclass AS index, compact_count FROM pg_stat_gpu_search
 WHERE index_oid IN ('cgd32_c'::regclass, 'cgd128_c'::regclass) ORDER BY 1;
SELECT 'pg_cuvs_compact' AS step, c.relname, pg_temp.cgd_graph(c.oid::regclass) AS graph, c.reloptions
  FROM pg_class c WHERE c.relname IN ('cgd32_c', 'cgd128_c') ORDER BY 2;

-- The compacted graph still answers: an exact match is its own nearest neighbour.
SET enable_seqscan = off;
SELECT id FROM cgd32 ORDER BY v <-> pg_temp.cgd_vec(1500) LIMIT 1;
SELECT id FROM cgd128 ORDER BY v <-> pg_temp.cgd_vec(1500) LIMIT 1;
RESET enable_seqscan;

DROP TABLE cgd32;
DROP TABLE cgd128;
