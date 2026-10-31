
-- HNSW build-time route receipt (over-the-vram policy; estimates, not hard caps).
CREATE FUNCTION pg_cuvs_hnsw_build_info(regclass) RETURNS jsonb
AS 'MODULE_PATHNAME', 'pg_cuvs_hnsw_build_info'
LANGUAGE C STABLE STRICT PARALLEL RESTRICTED;
COMMENT ON FUNCTION pg_cuvs_hnsw_build_info(regclass) IS
  'Stored actual HNSW construction route/settings and estimate-based admission; not a memory upper-bound guarantee. Legacy/source/empty builds may have no receipt.';
