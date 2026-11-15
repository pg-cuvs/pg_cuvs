-- pg_cuvs upgrade: 0.9.0 -> 0.9.1
\echo Use "ALTER EXTENSION pg_cuvs UPDATE TO '0.9.1'" to load this file. \quit

-- 0.9.1 adds one SQL object to the catalog 0.9.0 creates: the #203 status
-- function pg_cuvs_hnsw_connectivity_status(regclass), appended below.
-- A SQL change that 0.9.1 needs is appended here and, identically, to
-- pg_cuvs--0.9.1.sql. The two 0.9.0 scripts are never edited again.

-- CONN203 begin
-- #203: immutable build observation, not a live graph-integrity assertion.
CREATE FUNCTION pg_cuvs_hnsw_connectivity_status(index regclass) RETURNS jsonb
LANGUAGE SQL STABLE STRICT AS $$
 SELECT COALESCE(pg_cuvs_hnsw_build_info($1)->'connectivity',
                 '{"status":"not_recorded","scope":"build_snapshot"}'::jsonb)
$$;
COMMENT ON FUNCTION pg_cuvs_hnsw_connectivity_status(regclass) IS
 'Last build structural connectivity snapshot; does not track INSERT/DELETE/VACUUM and does not guarantee finite-ef recall. The check runs only when cuvs.hnsw_connectivity_check was on for the build: status is disabled when it was off and not_recorded for indexes built before the snapshot existed. A check that needs more than maintenance_work_mem continues in file-backed scratch, and is unavailable under a finite temp_file_limit.';
-- CONN203 end
