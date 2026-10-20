-- Applied daemon pool setting is observable and cannot be changed with SQL SET.
\set ON_ERROR_STOP on
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SELECT current_setting('cuvs.rmm_pool_mb') AS applied_mb,
       current_setting('cuvs.rmm_pool_max_percent') AS applied_percent \gset
SELECT (current_setting('cuvs.rmm_pool_status')::jsonb->>'initial_mb') = :'applied_mb'
   AND (current_setting('cuvs.rmm_pool_status')::jsonb->>'maximum_percent') = :'applied_percent'
   AS applied_status_matches;
SELECT CASE WHEN :'applied_mb' = '0'
  THEN :'applied_percent' = '0'
       AND current_setting('cuvs.rmm_pool_status')::jsonb->'devices' = '[]'::jsonb
  ELSE :'applied_percent'::int BETWEEN 1 AND 100
       AND jsonb_array_length(current_setting('cuvs.rmm_pool_status')::jsonb->'devices') > 0
  END AS device_status_matches;
SELECT count(*) = 3 AND bool_and(context = 'internal') AS read_only FROM pg_settings
WHERE name IN ('cuvs.rmm_pool_mb', 'cuvs.rmm_pool_max_percent', 'cuvs.rmm_pool_status');
\set ON_ERROR_STOP off
SET cuvs.rmm_pool_mb = 1024;
SET cuvs.rmm_pool_max_percent = 100;
SET cuvs.rmm_pool_status = '{}';
\set ON_ERROR_STOP on
BEGIN;
SET LOCAL cuvs.socket_path = '/tmp/pgcuvs-rmm-test-no-daemon';
SELECT current_setting('cuvs.rmm_pool_mb') = 'unavailable'
   AND current_setting('cuvs.rmm_pool_max_percent') = 'unavailable'
   AND current_setting('cuvs.rmm_pool_status') = 'unavailable' AS not_reported_as_zero;
ROLLBACK;
SELECT current_setting('cuvs.rmm_pool_mb') = :'applied_mb'
   AND current_setting('cuvs.rmm_pool_max_percent') = :'applied_percent' AS applied_setting_restored;
