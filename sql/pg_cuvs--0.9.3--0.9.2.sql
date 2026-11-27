-- pg_cuvs downgrade: 0.9.3 -> 0.9.2
\echo Use "ALTER EXTENSION pg_cuvs UPDATE TO '0.9.2'" to load this file. \quit

-- Run this BEFORE the package is replaced with an older one. It needs the 0.9.3 library: step 1
-- calls a function an older library does not have, so running it too late fails at that call
-- and changes nothing (the whole ALTER EXTENSION is one transaction).

-- 1. No index may keep a guarantee mark. The function raises an error if one cannot be taken
--    off; that error aborts this script with SQL and library untouched. It also makes every
--    backend rebuild its index routine from the handler named below.
SELECT pg_cuvs_cover_invalidate_all();

-- 2. Hand the access method back to the old C symbol, which every library of 0.9.2 and earlier
--    exports.
CREATE OR REPLACE FUNCTION pg_cuvs_hnsw_handler(internal)
RETURNS index_am_handler
AS '$libdir/pg_cuvs', 'pg_cuvs_hnsw_handler'
LANGUAGE C;

-- 3. Remove what 0.9.3 added.
DROP FUNCTION pg_cuvs_cover_invalidate_all();
DROP FUNCTION pg_cuvs_hnsw_cover_status(regclass);
DROP FUNCTION pg_cuvs_cover_recertify(regclass);

DROP FUNCTION pg_cuvs_cover_breakglass();

DROP FUNCTION pg_cuvs_hnsw_layer_status();
DROP FUNCTION pg_cuvs_hnsw_layer_counts(regclass);
