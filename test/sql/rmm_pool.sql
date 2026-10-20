-- Applied daemon pool setting is observable and cannot be changed with SQL SET.
\set ON_ERROR_STOP on
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SELECT current_setting('cuvs.rmm_pool_mb') = '0' AS pool_disabled;
SELECT context = 'internal' AS read_only FROM pg_settings
WHERE name = 'cuvs.rmm_pool_mb';
\set ON_ERROR_STOP off
SET cuvs.rmm_pool_mb = 1024;
\set ON_ERROR_STOP on
BEGIN;
SET LOCAL cuvs.socket_path = '/tmp/pgcuvs-rmm-test-no-daemon';
SELECT current_setting('cuvs.rmm_pool_mb') = 'unavailable' AS not_reported_as_zero;
ROLLBACK;
SELECT current_setting('cuvs.rmm_pool_mb') = '0' AS applied_setting_restored;
