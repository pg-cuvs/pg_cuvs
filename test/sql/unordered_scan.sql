-- unordered_scan.sql — #141: an ANN index must never answer an unordered scan.
--
-- count(*) needs ZERO columns, so the planner may legally satisfy it with an
-- Index Only Scan over a cagra / flat / ivfpq index. Those AMs produce tuples
-- only for an ORDER BY <-> scan, so such a plan delivered zero rows while the
-- cost estimate promised rows=N — `SELECT count(*) FROM t` silently returned 0
-- on any table carrying one of these indexes (issue #141).
--
-- Coverage, for each of the three AMs:
--   1. count(*) returns the true rowcount under default planner settings.
--   2. count(*) still returns the true rowcount under enable_seqscan = off —
--      the shape that reproduces #141, since a disabled seqscan (cost + 1e10)
--      loses to a normally-costed index path. amcostestimate now costs the
--      no-ORDER-BY path at 1e15, so the heap path wins even here.
--   3. The plan for count(*) does NOT reference the ANN index.
--   4. An ORDER BY <-> query still routes to the ANN index — the fix rejects
--      unordered paths only, it does not disable the index. (cagra and flat
--      only; see the NOTE on ivfpq coverage further down.)
--
-- REQUIRES: pg_cuvs_server running; cuvs.index_dir writable.
\set ON_ERROR_STOP on
-- Kept at warning for the whole file: CREATE INDEX notices differ per AM and
-- carry no signal for this test.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';
-- True when the chosen plan references the named index. Matching on the index
-- NAME (not the AM) is unambiguous: the plan shows the index name.
CREATE FUNCTION us_uses_index(q text, idx text) RETURNS boolean AS $$
DECLARE line text;
BEGIN
  FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    IF line LIKE '%' || idx || '%' THEN RETURN true; END IF;
  END LOOP;
  RETURN false;
END$$ LANGUAGE plpgsql;
-- Deterministic 200-vector, 8-dim corpus (same generator as brute_force.sql),
-- copied into one table per AM so each index is the only ANN path on its table.
CREATE TABLE us_base (id int, embedding vector(8));
INSERT INTO us_base
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
CREATE TABLE us_cagra_tbl (LIKE us_base);
INSERT INTO us_cagra_tbl SELECT * FROM us_base;
CREATE TABLE us_flat_tbl (LIKE us_base);
INSERT INTO us_flat_tbl SELECT * FROM us_base;
DROP TABLE us_base;
CREATE INDEX us_cagra_idx ON us_cagra_tbl USING cagra (embedding vector_l2_ops);
CREATE INDEX us_flat_idx ON us_flat_tbl USING flat (embedding vector_l2_ops);
ANALYZE us_cagra_tbl;
ANALYZE us_flat_tbl;
-- ivfpq corpus, in ivfpq_smoke.sql's shape (20 rows, dim 4, n_lists = 4,
-- pq_dim = 2). See the NOTE above the ivfpq assertions below.
CREATE TABLE us_ivfpq_tbl (id int, embedding vector(4));
INSERT INTO us_ivfpq_tbl VALUES
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
CREATE INDEX us_ivfpq_idx ON us_ivfpq_tbl USING ivfpq (embedding vector_l2_ops)
    WITH (n_lists = 4, pq_bits = 8, pq_dim = 2);
SET cuvs.ivfpq_n_probes = 4;
SET cuvs.k = 4;
-- ============================================================ cagra
SELECT count(*) = 200 AS cagra_count_default_ok FROM us_cagra_tbl;
SET enable_seqscan = off;
SELECT count(*) = 200 AS cagra_count_noseqscan_ok FROM us_cagra_tbl;
SELECT NOT us_uses_index('SELECT count(*) FROM us_cagra_tbl', 'us_cagra_idx')
    AS cagra_count_avoids_index;
SELECT us_uses_index('SELECT id FROM us_cagra_tbl ORDER BY embedding <-> ''[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]'' LIMIT 5', 'us_cagra_idx')
    AS cagra_orderby_uses_index;
RESET enable_seqscan;
-- ============================================================ flat
SELECT count(*) = 200 AS flat_count_default_ok FROM us_flat_tbl;
SET enable_seqscan = off;
SELECT count(*) = 200 AS flat_count_noseqscan_ok FROM us_flat_tbl;
SELECT NOT us_uses_index('SELECT count(*) FROM us_flat_tbl', 'us_flat_idx')
    AS flat_count_avoids_index;
SELECT us_uses_index('SELECT id FROM us_flat_tbl ORDER BY embedding <-> ''[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]'' LIMIT 5', 'us_flat_idx')
    AS flat_orderby_uses_index;
RESET enable_seqscan;
-- ============================================================ ivfpq
-- NOTE on ivfpq coverage, stated plainly: under the Tier-1 CPU shim the planner
-- did not choose this ivfpq index for a vector ORDER BY in this file's session
-- (tried with and without enable_seqscan = off, with and without an explicit
-- ::vector cast, and in ivfpq_smoke.sql's exact corpus shape). The three
-- assertions below are therefore probably VACUOUS here -- an index the planner
-- never considers trivially cannot answer count(*). They are kept because they
-- become real wherever ivfpq does route (Tier-2 / GPU).
--
-- The #141 fix is still covered for ivfpq: ivfpq shares cuvsamcostestimate with
-- cagra (see cuvsamhandler / ivfpqamhandler), so the cagra assertions above
-- exercise the very cost function that governs ivfpq. The "ivfpq still routes
-- for ORDER BY" fence is ivfpq_smoke.sql, which asserts a REAL ivfpq search
-- (pg_stat_gpu_search.search_mode = 'ivfpq') and passes with this fix applied --
-- stronger evidence than an EXPLAIN string match would be.
--
-- Why this file's ivfpq index is not a planner candidate is unrelated to #141
-- and is left undiagnosed here.
SELECT count(*) = 20 AS ivfpq_count_default_ok FROM us_ivfpq_tbl;
SET enable_seqscan = off;
SELECT count(*) = 20 AS ivfpq_count_noseqscan_ok FROM us_ivfpq_tbl;
SELECT NOT us_uses_index('SELECT count(*) FROM us_ivfpq_tbl', 'us_ivfpq_idx')
    AS ivfpq_count_avoids_index;
RESET enable_seqscan;
-- Cleanup
DROP TABLE us_cagra_tbl;
DROP TABLE us_flat_tbl;
DROP TABLE us_ivfpq_tbl;
DROP FUNCTION us_uses_index(text, text);
