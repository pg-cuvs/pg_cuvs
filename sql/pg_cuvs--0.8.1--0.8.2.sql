-- Co-deploy the 0.8.2 extension and daemon (HNSW op23 payload v3).
-- No index rewrite: v1 stored policy receipts remain historical read-only data.
COMMENT ON FUNCTION pg_cuvs_hnsw_build_info(regclass) IS
  'Stored HNSW route/settings: policy v2 writer and v1 historical reader. AUTO is tiny/resident IVF-PQ; disk requires explicit ace. Estimates are not memory upper bounds. Older 0.8.1 readers cannot read v2 receipts.';
