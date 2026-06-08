-- pg_cuvs upgrade: 0.2.0 -> 0.3.0
-- Phase 3Q: CAGRA streaming updates (in-place EXTEND + tombstone-based COMPACT)
--
-- Usage:
--   ALTER EXTENSION pg_cuvs UPDATE TO '0.3.0';
--
-- After upgrade:
--   SELECT pg_cuvs_compact('my_cagra_idx'::regclass);

\echo Use "ALTER EXTENSION pg_cuvs UPDATE TO ''0.3.0''" to load this file. \quit

-- Manual compact trigger for a CAGRA index.
-- Removes tombstoned vectors via cuvsCagraMerge; clears the .tombstone sidecar.
-- Auto-compact is triggered by cuvs.compact_delete_ratio during VACUUM.
CREATE FUNCTION pg_cuvs_compact(index_rel regclass)
RETURNS void
AS '$libdir/pg_cuvs', 'pg_cuvs_compact'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_cuvs_compact(regclass) IS
  'Compact a CAGRA index: remove tombstoned vectors via cuvsCagraMerge, '
  'rebuild the on-disk .cagra + .tids, and delete the .tombstone sidecar. '
  'Auto-triggered during VACUUM when cuvs.compact_delete_ratio is exceeded.';
