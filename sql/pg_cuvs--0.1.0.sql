-- pg_cuvs--0.1.0.sql
-- Loaded by CREATE EXTENSION pg_cuvs; (requires pgvector)
\echo Use "CREATE EXTENSION pg_cuvs" to load this file. \quit

-- ----------------------------------------------------------------
-- Index Access Method handler
-- ----------------------------------------------------------------
CREATE FUNCTION cuvsamhandler(internal)
RETURNS index_am_handler
AS '$libdir/pg_cuvs', 'cuvsamhandler'
LANGUAGE C;

CREATE ACCESS METHOD cagra
TYPE INDEX
HANDLER cuvsamhandler;

COMMENT ON ACCESS METHOD cagra IS
  'GPU-accelerated CAGRA index for pgvector vector type (pg_cuvs)';

-- ----------------------------------------------------------------
-- Operator classes — reuse pgvector operators via the cagra AM
-- ----------------------------------------------------------------
CREATE OPERATOR CLASS vector_l2_ops
DEFAULT FOR TYPE vector USING cagra AS
    OPERATOR 1 <-> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 vector_l2_squared_distance(vector, vector);

CREATE OPERATOR CLASS vector_cosine_ops
FOR TYPE vector USING cagra AS
    OPERATOR 1 <=> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 cosine_distance(vector, vector);

CREATE OPERATOR CLASS vector_ip_ops
FOR TYPE vector USING cagra AS
    OPERATOR 1 <#> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 vector_negative_inner_product(vector, vector);

-- ----------------------------------------------------------------
-- pg_cuvs_reset_circuit(index_name text)
-- Re-enables GPU routing after circuit breaker trips (FALLBACK-04).
-- ----------------------------------------------------------------
CREATE FUNCTION pg_cuvs_reset_circuit(index_oid regclass)
RETURNS void
AS '$libdir/pg_cuvs', 'pg_cuvs_reset_circuit'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_cuvs_reset_circuit(regclass) IS
  'Reset circuit breaker for a cagra index to re-enable GPU routing. '
  'Use after repeated GPU errors have been resolved. '
  'Example: SELECT pg_cuvs_reset_circuit(''my_schema.cagra_idx''::regclass);';

-- ----------------------------------------------------------------
-- Last-search stats (process-local). NULL if no scan in this session.
-- For EXPLAIN VERBOSE integration set cuvs.debug = on so the same
-- stats appear inline via NOTICE.
-- ----------------------------------------------------------------
CREATE FUNCTION pg_cuvs_last_search_latency_us()
RETURNS integer
AS '$libdir/pg_cuvs', 'pg_cuvs_last_search_latency_us'
LANGUAGE C;

CREATE FUNCTION pg_cuvs_last_search_n_results()
RETURNS integer
AS '$libdir/pg_cuvs', 'pg_cuvs_last_search_n_results'
LANGUAGE C;

CREATE FUNCTION pg_cuvs_last_search_k()
RETURNS integer
AS '$libdir/pg_cuvs', 'pg_cuvs_last_search_k'
LANGUAGE C;

CREATE FUNCTION pg_cuvs_last_search_index()
RETURNS oid
AS '$libdir/pg_cuvs', 'pg_cuvs_last_search_index'
LANGUAGE C;

CREATE FUNCTION pg_cuvs_last_search_metric()
RETURNS text
AS '$libdir/pg_cuvs', 'pg_cuvs_last_search_metric'
LANGUAGE C;

COMMENT ON FUNCTION pg_cuvs_last_search_latency_us() IS
  'Daemon-reported wall-clock latency in microseconds for the most '
  'recent successful cagra index scan in this backend. NULL if none.';
