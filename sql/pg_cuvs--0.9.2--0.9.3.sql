-- pg_cuvs upgrade: 0.9.2 -> 0.9.3
\echo Use "ALTER EXTENSION pg_cuvs UPDATE TO '0.9.3'" to load this file. \quit

-- A SQL change that 0.9.3 needs is appended here and, identically, to
-- pg_cuvs--0.9.3.sql. The scripts of 0.9.2 and earlier are never edited again.

-- mode='nsw' before 0.9.3 wrote every element of a pg_cuvs_hnsw index at level 0. pgvector's insert then gives the upper
-- layers to inserted rows only, and a search that descends through them can miss the built rows. REINDEX rebuilds the index
-- with upper layers. pg_cuvs_hnsw_layer_counts() reads one index (every page); pg_cuvs_hnsw_layer_status() judges each one.
CREATE FUNCTION pg_cuvs_hnsw_layer_counts(
    index regclass,
    OUT elements bigint,
    OUT upper_elements bigint,
    OUT m integer,
    OUT entry_level integer,
    OUT built_hierarchy integer,
    OUT level1_unlinked bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_cuvs_hnsw_layer_counts'
LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION pg_cuvs_hnsw_layer_counts(regclass) IS
    'Elements of a pg_cuvs_hnsw index (not marked deleted), how many are above level 0, the metapage m and entry level, the hierarchy flag the heap build recorded (NULL if none), and how many elements above level 0 have no level-1 link (0 when the upper layer is connected). Reads every page; needs SELECT on the table.';

CREATE FUNCTION pg_cuvs_hnsw_layer_status()
RETURNS TABLE (index regclass, access_method name, current boolean, reason text)
LANGUAGE sql STABLE
AS $$
SELECT c.oid::regclass, a.amname,
       CASE WHEN k.level1_unlinked > 0 AND k.upper_elements > 1 THEN false
            WHEN k.built_hierarchy IS NOT NULL THEN k.built_hierarchy = 1
            WHEN k.m IS NULL OR k.elements < 16 * k.m THEN NULL
            ELSE k.upper_elements * k.m * 4 >= k.elements END,
       CASE WHEN k.level1_unlinked > 0 AND k.upper_elements > 1
                THEN 'elements above level 0 without a level-1 link among two or more such elements: searches can miss rows; REINDEX INDEX'
            WHEN k.built_hierarchy = 0
                THEN 'built without upper layers (mode nsw before 0.9.3): rows inserted since can hide the built rows; REINDEX INDEX'
            WHEN k.built_hierarchy = 1 THEN NULL
            WHEN k.m IS NULL OR k.elements < 16 * k.m
                THEN 'too few elements to tell from the layers (fewer than 16 x m)'
            WHEN k.upper_elements * k.m * 4 < k.elements
                THEN 'built without upper layers (mode nsw before 0.9.3; judged from the share of elements above level 0): rows inserted since can hide the built rows; REINDEX INDEX'
       END
FROM pg_class c
JOIN pg_am a ON a.oid = c.relam
JOIN pg_index i ON i.indexrelid = c.oid
CROSS JOIN LATERAL pg_cuvs_hnsw_layer_counts(c.oid::regclass) k
WHERE a.amname = 'pg_cuvs_hnsw' AND i.indisready AND has_table_privilege(i.indrelid, 'SELECT')
ORDER BY c.oid
$$;
COMMENT ON FUNCTION pg_cuvs_hnsw_layer_status() IS
    'Every pg_cuvs_hnsw index the caller can read that is ready (one a concurrent build is writing is left out): whether it has the upper layers mode nsw lacked before 0.9.3, and why not. A heap build answers from its recorded hierarchy flag; a CAGRA-source build from the share of elements above level 0 (expected about 1/m; fewer than a quarter of that means no upper layers). Reads every page of every such index.';
-- The handler now names the C symbol pg_cuvs_hnsw_handler_c1p (see pg_cuvs--0.9.3.sql).
CREATE OR REPLACE FUNCTION pg_cuvs_hnsw_handler(internal)
RETURNS index_am_handler
AS '$libdir/pg_cuvs', 'pg_cuvs_hnsw_handler_c1p'
LANGUAGE C;

CREATE FUNCTION pg_cuvs_cover_invalidate_all()
RETURNS bigint
AS '$libdir/pg_cuvs', 'pg_cuvs_cover_invalidate_all'
LANGUAGE C;

REVOKE ALL ON FUNCTION pg_cuvs_cover_invalidate_all() FROM PUBLIC;

-- This update is the way out of an emergency rollback too (pg_cuvs_cover_breakglass()): whatever carries a guarantee mark is
-- taken off first, and only then does the marker go. An index built before this update has no mark, so for a plain update
-- this changes nothing.
SELECT pg_cuvs_cover_invalidate_all();
DROP FUNCTION IF EXISTS pg_cuvs_cover_breakglass_marker();

-- pg_cuvs_hnsw_cover_status(index): how the cover descriptor of a pg_cuvs_hnsw index reads (class, writable, ...).
CREATE FUNCTION pg_cuvs_hnsw_cover_status(index regclass)
RETURNS jsonb
AS '$libdir/pg_cuvs', 'pg_cuvs_hnsw_cover_status'
LANGUAGE C STABLE STRICT;

-- pg_cuvs_cover_recertify(index): recertify a pg_cuvs_hnsw index now (owner of the table). Returns what happened.
CREATE FUNCTION pg_cuvs_cover_recertify(index regclass)
RETURNS jsonb
AS '$libdir/pg_cuvs', 'pg_cuvs_cover_recertify'
LANGUAGE C STRICT;

-- pg_cuvs_cover_breakglass(): the one emergency action when the package has to go back to the older library before the
-- database could be brought back to 0.9.2 by ALTER EXTENSION UPDATE (the older library cannot run that script). It is plain SQL,
-- so it runs with either library installed. It points the access method handler back at the older C symbol, so that the
-- older library can open the indexes again, and leaves a marker: while pg_cuvs_cover_breakglass_marker() exists the new
-- library reads every CERTIFIED index as lost, because the older library inserts and vacuums without recording anything. The
-- marker is dropped only by the update script to 0.9.3 (pg_cuvs--0.9.2--0.9.3.sql), after the new library has taken every
-- guarantee off: ALTER EXTENSION UPDATE TO '0.9.2' and then TO '0.9.3', with the new library installed. Pointing the handler
-- back by hand (CREATE OR REPLACE FUNCTION) leaves no marker and is not supported.
CREATE FUNCTION pg_cuvs_cover_breakglass()
RETURNS void
LANGUAGE plpgsql
AS $breakglass$
DECLARE
	ns text;
BEGIN
	SELECT quote_ident(n.nspname) INTO ns
	FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace
	WHERE e.extname = 'pg_cuvs';
	IF to_regprocedure(ns || '.pg_cuvs_cover_breakglass_marker()') IS NULL THEN
		EXECUTE 'CREATE FUNCTION ' || ns || '.pg_cuvs_cover_breakglass_marker() RETURNS void LANGUAGE sql AS ''SELECT 1''';
		EXECUTE 'ALTER EXTENSION pg_cuvs ADD FUNCTION ' || ns || '.pg_cuvs_cover_breakglass_marker()';
	END IF;
	EXECUTE 'CREATE OR REPLACE FUNCTION ' || ns || '.pg_cuvs_hnsw_handler(internal) RETURNS index_am_handler '
		|| 'AS ''$libdir/pg_cuvs'', ''pg_cuvs_hnsw_handler'' LANGUAGE C';
	RAISE NOTICE 'pg_cuvs: the access method handler names the older C symbol and the break-glass marker exists; install the new package again, restart, then ALTER EXTENSION pg_cuvs UPDATE TO ''0.9.2'' and TO ''0.9.3''';
END
$breakglass$;

REVOKE ALL ON FUNCTION pg_cuvs_cover_breakglass() FROM PUBLIC;
