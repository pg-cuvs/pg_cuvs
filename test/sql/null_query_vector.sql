-- null_query_vector.sql — #150: a NULL query vector must never produce a
-- silent zero-row answer from an ANN index scan.
--
-- `embedding <-> NULL::vector` written as a LITERAL const-folds to a plain NULL
-- (the operator is strict), leaving no orderable index key, so #149's planner
-- exclusion routes it to the heap (covered in edge_cases.sql). A PARAMETERIZED
-- NULL query vector does not const-fold under a generic plan: the planner
-- cannot know the bind value, keeps the ordered index path, and the scan then
-- reaches the AM's gettuple with SK_ISNULL set. That guard used to
-- `return false` — a silent zero-row answer, the exact #141 failure mode.
--
-- The contract asserted here: the scan raises ERROR 0A000
-- (feature_not_supported) instead. This is a deliberate divergence from the
-- heap path, which returns LIMIT n rows in arbitrary order for an all-NULL sort
-- key; an ERROR is never a wrong answer, and a NULL query vector is in practice
-- always an unbound-parameter bug. See the PR for #150 for the full rationale
-- (including why pgvector's zero-distance emulation is not portable here).
--
-- The assertions below are self-distinguishing, not vacuous: if the planner had
-- NOT chosen the ANN index the probe would return 'rows=5' from the heap, so
-- 'error=0A000' is reachable only through the index scan under test.
--
-- REQUIRES: pg_cuvs_server running; cuvs.index_dir writable.

\set ON_ERROR_STOP on

-- CREATE INDEX notices differ per AM and carry no signal for this test.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- Deterministic 200-vector, 8-dim corpus (same generator as brute_force.sql),
-- copied into one table per AM so each index is the only ANN path on its table.
CREATE TABLE nqv_base (id int, embedding vector(8));
INSERT INTO nqv_base
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

CREATE TABLE nqv_cagra_tbl (LIKE nqv_base);
INSERT INTO nqv_cagra_tbl SELECT * FROM nqv_base;
CREATE TABLE nqv_flat_tbl (LIKE nqv_base);
INSERT INTO nqv_flat_tbl SELECT * FROM nqv_base;
DROP TABLE nqv_base;

CREATE INDEX nqv_cagra_idx ON nqv_cagra_tbl USING cagra (embedding vector_l2_ops);
CREATE INDEX nqv_flat_idx ON nqv_flat_tbl USING flat (embedding vector_l2_ops);
ANALYZE nqv_cagra_tbl;
ANALYZE nqv_flat_tbl;

-- The probes. A STATIC query inside plpgsql is planned through the plan cache,
-- so `plan_cache_mode = force_generic_plan` keeps `qv` a Param (a custom plan
-- would substitute the NULL and const-fold the sort key away, which is the
-- literal case already covered by edge_cases.sql). Dynamic EXECUTE ... USING is
-- deliberately NOT used: plpgsql marks those params const, which const-folds.
-- One function per AM — the duplication is the point (DUP-SIBLING symmetry).
CREATE FUNCTION nqv_cagra_probe(qv vector) RETURNS text AS $$
DECLARE n int;
BEGIN
  SELECT count(*) INTO n
    FROM (SELECT id FROM nqv_cagra_tbl ORDER BY embedding <-> qv LIMIT 5) s;
  RETURN 'rows=' || n;
EXCEPTION WHEN feature_not_supported THEN
  RETURN 'error=' || SQLSTATE;
END$$ LANGUAGE plpgsql;

CREATE FUNCTION nqv_flat_probe(qv vector) RETURNS text AS $$
DECLARE n int;
BEGIN
  SELECT count(*) INTO n
    FROM (SELECT id FROM nqv_flat_tbl ORDER BY embedding <-> qv LIMIT 5) s;
  RETURN 'rows=' || n;
EXCEPTION WHEN feature_not_supported THEN
  RETURN 'error=' || SQLSTATE;
END$$ LANGUAGE plpgsql;

SET plan_cache_mode = force_generic_plan;
SET enable_seqscan = off;
SET enable_bitmapscan = off;

-- ============================================================ cagra
SELECT nqv_cagra_probe(NULL) = 'error=0A000' AS cagra_null_param_errors;
-- The same generic plan with a non-NULL bind is unaffected.
SELECT nqv_cagra_probe('[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]') = 'rows=5'
    AS cagra_nonnull_param_ok;

-- ============================================================ flat
SELECT nqv_flat_probe(NULL) = 'error=0A000' AS flat_null_param_errors;
SELECT nqv_flat_probe('[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]') = 'rows=5'
    AS flat_nonnull_param_ok;

-- ============================================================ ivfpq
-- NOT asserted here, stated plainly: under the Tier-1 CPU shim the planner does
-- not choose an ivfpq index for a vector ORDER BY (established in #149, see the
-- NOTE in unordered_scan.sql). A probe would therefore return 'rows=5' from the
-- heap on Tier-1 and 'error=0A000' on Tier-2 — an environment-dependent
-- expected file. Rather than encode that, or leave a tolerant assertion that
-- passes in both worlds and proves nothing, ivfpq's guard is covered by:
--   * the DUP-SIBLING symmetry of the three gettuple guards (ADR-073), and
--   * ivfpq_smoke.sql, which asserts a real ivfpq search still routes and
--     returns rows with this fix applied.

RESET enable_bitmapscan;
RESET enable_seqscan;
RESET plan_cache_mode;

DROP FUNCTION nqv_cagra_probe(vector);
DROP FUNCTION nqv_flat_probe(vector);
DROP TABLE nqv_cagra_tbl;
DROP TABLE nqv_flat_tbl;
