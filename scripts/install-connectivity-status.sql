-- Development 0.9.0 installs: register the additive SQL status alias after
-- loading the new binary. Release-version selection remains separate.
\set ON_ERROR_STOP on
SELECT pg_catalog.current_setting('search_path') AS conn203_saved_path \gset
SELECT pg_catalog.set_config('search_path',pg_catalog.quote_ident(n.nspname)||',pg_catalog',false)
FROM pg_catalog.pg_extension e JOIN pg_catalog.pg_namespace n ON n.oid=e.extnamespace
WHERE e.extname='pg_cuvs';
CREATE OR REPLACE FUNCTION pg_cuvs_hnsw_connectivity_status(index regclass) RETURNS jsonb
LANGUAGE SQL STABLE STRICT AS $$
 SELECT COALESCE(pg_cuvs_hnsw_build_info($1)->'connectivity',
                 '{"status":"not_recorded","scope":"build_snapshot"}'::jsonb)
$$;
DO $do$
BEGIN
 IF NOT EXISTS (SELECT 1 FROM pg_catalog.pg_depend
    WHERE classid='pg_catalog.pg_proc'::regclass
      AND objid='pg_cuvs_hnsw_connectivity_status(regclass)'::regprocedure
      AND refclassid='pg_catalog.pg_extension'::regclass
      AND refobjid=(SELECT oid FROM pg_catalog.pg_extension WHERE extname='pg_cuvs')
      AND deptype='e') THEN
  ALTER EXTENSION pg_cuvs ADD FUNCTION pg_cuvs_hnsw_connectivity_status(regclass);
 END IF;
END
$do$;

SELECT pg_catalog.set_config('search_path', :'conn203_saved_path', false);
