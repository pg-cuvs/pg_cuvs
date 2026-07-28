-- upgrade_path.sql — the ALTER EXTENSION UPDATE path must land on the same
-- catalog as a fresh CREATE EXTENSION.
--
-- Every other test in REGRESS installs the extension with CREATE EXTENSION,
-- i.e. it only ever exercises the *base* script for the current version. The
-- per-version upgrade scripts (sql/pg_cuvs--X--Y.sql) had no coverage at all,
-- which is how sql/pg_cuvs--0.2.0--0.3.0.sql came to omit six functions plus
-- pg_stat_gpu_fallback while base 0.3.0 kept accumulating them: the later
-- 0.4.0->0.5.0 REVOKEs then referenced functions that upgraded installs did
-- not have, and ALTER EXTENSION ... UPDATE TO '0.5.0' rolled back wholesale.
--
-- Coverage:
--   1. CREATE EXTENSION pg_cuvs VERSION '0.1.0' succeeds against the current
--      shared library (all C symbols the oldest script names still exist).
--   2. ALTER EXTENSION pg_cuvs UPDATE walks the whole chain to the control
--      file's default_version.
--   3. The resulting catalog is identical, attribute by attribute, to the one
--      a fresh CREATE EXTENSION produces: extension membership, function
--      signatures (argument list with defaults, result type, volatility,
--      STRICT, SECURITY DEFINER, parallel safety, language, C symbol) and
--      ACLs, view definitions and column types, access methods, operator
--      classes and their operator/support-function members, and COMMENTs.
--
-- Comparison method. Two instances of the same extension cannot coexist in one
-- database, and pg_regress gives each file a single session against a single
-- database, so the two catalogs cannot be built side by side. Instead each
-- install is rendered to sorted text rows in an ordinary (non-extension) table
-- that outlives DROP EXTENSION, and the two snapshots are diffed with
-- EXCEPT ALL in both directions. A failure prints the offending rows rather
-- than a bare `false`, so the regression diff names the missing object.
--
-- The version walk is deliberately version-agnostic: it starts at the fixed
-- oldest version and uses ALTER EXTENSION ... UPDATE with no TO clause, which
-- targets default_version from pg_cuvs.control, i.e. it walks 0.1.0 -> 0.2.0 ->
-- 0.3.0 -> 0.4.0 -> 0.5.0 today without naming any of them. A version bump
-- therefore needs no edit here, and no expected value below is version-specific.
--
-- Shape at the time of writing (0.5.0, measured on the GPU VM): 26 functions,
-- 12 operator classes, and the access methods cagra, flat, ivfpq,
-- pg_cuvs_hnsw. Recorded as orientation only — nothing here asserts it.
--
-- Attributes are assembled with format() rather than ||: provolatile, proacl
-- and friends are "char"/aclitem, and concatenating them with || fails with
-- `operator is not unique`.
--
-- No GPU and no daemon: this is pure DDL, so it belongs in Tier-1.

\set ON_ERROR_STOP on

-- Notices are suppressed for the whole install/drop dance so that this file's
-- expected output does not depend on whether an earlier test in REGRESS left
-- the extension installed.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
DROP EXTENSION IF EXISTS pg_cuvs CASCADE;
RESET client_min_messages;

CREATE TABLE upg_snap (phase text, kind text, obj text);

-- Renders the current pg_cuvs install into upg_snap. Owned by nobody, so it
-- survives the DROP EXTENSION between the two phases. prosrc and view
-- definitions are whitespace-normalized: re-indentation between a base script
-- and an upgrade script is not a catalog divergence.
CREATE FUNCTION upg_snapshot(p_phase text) RETURNS void LANGUAGE sql AS $fn$
INSERT INTO upg_snap (phase, kind, obj)

-- Extension membership + COMMENT, for every object of every kind. Catches an
-- upgrade script that simply forgot a CREATE or a COMMENT.
SELECT p_phase, 'member'::text,
       format('%s | comment=%s',
              pg_describe_object(d.classid, d.objid, d.objsubid),
              COALESCE(obj_description(d.objid, d.classid::regclass::text), '(none)'))
  FROM pg_depend d
  JOIN pg_extension e ON e.oid = d.refobjid
 WHERE d.refclassid = 'pg_extension'::regclass
   AND d.deptype = 'e'
   AND e.extname = 'pg_cuvs'

UNION ALL
-- Functions, attribute by attribute. pg_get_function_result renders OUT
-- parameters as TABLE(...), so a set-returning function that drifted from 35
-- to 38 OUT columns shows up here even though its name never changed.
SELECT p_phase, 'function'::text,
       format('%s | args=%s | returns=%s | vol=%s strict=%s secdef=%s parallel=%s kind=%s retset=%s | lang=%s | bin=%s | src=%s | acl=%s',
              p.oid::regprocedure::text,
              pg_get_function_arguments(p.oid),
              pg_get_function_result(p.oid),
              p.provolatile, p.proisstrict, p.prosecdef, p.proparallel,
              p.prokind, p.proretset,
              l.lanname,
              COALESCE(p.probin, '(none)'),
              regexp_replace(btrim(p.prosrc), '\s+', ' ', 'g'),
              COALESCE((SELECT string_agg(a::text, ',' ORDER BY a::text)
                          FROM unnest(p.proacl) a), '(default)'))
  FROM pg_proc p
  JOIN pg_language l ON l.oid = p.prolang
  JOIN pg_depend d ON d.classid = 'pg_proc'::regclass
                  AND d.objid = p.oid AND d.deptype = 'e'
  JOIN pg_extension e ON e.oid = d.refobjid AND e.extname = 'pg_cuvs'

UNION ALL
-- View definitions. pg_get_viewdef re-deparses the stored rewrite rule, so a
-- `SELECT * FROM f()` view created against a stale function signature expands
-- to the column list it was frozen with.
SELECT p_phase, 'view'::text,
       format('%s | def=%s | acl=%s',
              c.relname,
              regexp_replace(btrim(pg_get_viewdef(c.oid, true)), '\s+', ' ', 'g'),
              COALESCE((SELECT string_agg(a::text, ',' ORDER BY a::text)
                          FROM unnest(c.relacl) a), '(default)'))
  FROM pg_class c
  JOIN pg_depend d ON d.classid = 'pg_class'::regclass
                  AND d.objid = c.oid AND d.deptype = 'e'
  JOIN pg_extension e ON e.oid = d.refobjid AND e.extname = 'pg_cuvs'
 WHERE c.relkind = 'v'

UNION ALL
SELECT p_phase, 'viewcol'::text,
       format('%s.%s %s %s', c.relname, a.attnum, a.attname,
              format_type(a.atttypid, a.atttypmod))
  FROM pg_class c
  JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped
  JOIN pg_depend d ON d.classid = 'pg_class'::regclass
                  AND d.objid = c.oid AND d.deptype = 'e'
  JOIN pg_extension e ON e.oid = d.refobjid AND e.extname = 'pg_cuvs'
 WHERE c.relkind = 'v'

UNION ALL
SELECT p_phase, 'am'::text,
       format('%s type=%s handler=%s',
              am.amname, am.amtype, am.amhandler::regprocedure::text)
  FROM pg_am am
  JOIN pg_depend d ON d.classid = 'pg_am'::regclass
                  AND d.objid = am.oid AND d.deptype = 'e'
  JOIN pg_extension e ON e.oid = d.refobjid AND e.extname = 'pg_cuvs'

UNION ALL
SELECT p_phase, 'opclass'::text,
       format('%s.%s family=%s intype=%s default=%s storage=%s',
              am.amname, oc.opcname, opf.opfname,
              format_type(oc.opcintype, NULL), oc.opcdefault,
              CASE WHEN oc.opckeytype = 0 THEN '-'
                   ELSE format_type(oc.opckeytype, NULL) END)
  FROM pg_opclass oc
  JOIN pg_am am ON am.oid = oc.opcmethod
  JOIN pg_opfamily opf ON opf.oid = oc.opcfamily
  JOIN pg_depend d ON d.classid = 'pg_opclass'::regclass
                  AND d.objid = oc.oid AND d.deptype = 'e'
  JOIN pg_extension e ON e.oid = d.refobjid AND e.extname = 'pg_cuvs'

UNION ALL
-- Operator-family members. Reached through the extension-owned opclasses
-- rather than through pg_depend on the family, because an implicitly created
-- family's membership rows are auto-dependencies, not extension members.
-- DISTINCT because several opclasses may share one family.
SELECT DISTINCT p_phase, 'amop'::text,
       format('%s.%s strategy %s %s purpose=%s sortfamily=%s',
              am.amname, opf.opfname, ao.amopstrategy,
              ao.amopopr::regoperator::text, ao.amoppurpose,
              COALESCE(sf.opfname, '-'))
  FROM pg_amop ao
  JOIN pg_opfamily opf ON opf.oid = ao.amopfamily
  JOIN pg_am am ON am.oid = opf.opfmethod
  LEFT JOIN pg_opfamily sf ON sf.oid = ao.amopsortfamily
  JOIN pg_opclass oc ON oc.opcfamily = opf.oid
  JOIN pg_depend d ON d.classid = 'pg_opclass'::regclass
                  AND d.objid = oc.oid AND d.deptype = 'e'
  JOIN pg_extension e ON e.oid = d.refobjid AND e.extname = 'pg_cuvs'

UNION ALL
SELECT DISTINCT p_phase, 'amproc'::text,
       format('%s.%s proc %s left=%s right=%s %s',
              am.amname, opf.opfname, ap.amprocnum,
              format_type(ap.amproclefttype, NULL),
              format_type(ap.amprocrighttype, NULL),
              ap.amproc::regprocedure::text)
  FROM pg_amproc ap
  JOIN pg_opfamily opf ON opf.oid = ap.amprocfamily
  JOIN pg_am am ON am.oid = opf.opfmethod
  JOIN pg_opclass oc ON oc.opcfamily = opf.oid
  JOIN pg_depend d ON d.classid = 'pg_opclass'::regclass
                  AND d.objid = oc.oid AND d.deptype = 'e'
  JOIN pg_extension e ON e.oid = d.refobjid AND e.extname = 'pg_cuvs'
$fn$;

-- ── Phase 1: oldest version, then walk the upgrade chain ─────────
CREATE EXTENSION pg_cuvs VERSION '0.1.0';
SELECT extversion AS installed_version FROM pg_extension WHERE extname = 'pg_cuvs';

ALTER EXTENSION pg_cuvs UPDATE;

-- Asserted as a boolean rather than printing the version, so a version bump
-- does not churn this file's expected output.
SELECT e.extversion = a.default_version AS upgraded_to_default_version
  FROM pg_extension e, pg_available_extensions a
 WHERE e.extname = 'pg_cuvs' AND a.name = 'pg_cuvs';

SELECT upg_snapshot('upgraded');

-- ── Phase 2: fresh install of the same version ───────────────────
DROP EXTENSION pg_cuvs;
CREATE EXTENSION pg_cuvs;

SELECT e.extversion = a.default_version AS fresh_at_default_version
  FROM pg_extension e, pg_available_extensions a
 WHERE e.extname = 'pg_cuvs' AND a.name = 'pg_cuvs';

SELECT upg_snapshot('fresh');

-- ── The gate: both diffs must be empty ───────────────────────────
-- Present after a fresh CREATE EXTENSION, missing after the upgrade walk.
-- This is the direction that catches an upgrade script omitting an object.
SELECT kind, obj FROM upg_snap WHERE phase = 'fresh'
EXCEPT ALL
SELECT kind, obj FROM upg_snap WHERE phase = 'upgraded'
ORDER BY 1, 2;

-- Present after the upgrade walk, missing from a fresh install. This is the
-- direction that catches a base script that forgot to carry a change forward.
SELECT kind, obj FROM upg_snap WHERE phase = 'upgraded'
EXCEPT ALL
SELECT kind, obj FROM upg_snap WHERE phase = 'fresh'
ORDER BY 1, 2;

-- Sanity: the snapshots are non-empty, so an empty diff means "identical" and
-- not "both snapshot queries returned nothing".
SELECT phase, count(*) > 0 AS non_empty FROM upg_snap GROUP BY phase ORDER BY phase;

-- Cleanup. The extension is deliberately left installed at the current
-- version (see the DROP EXTENSION note in smoke.sql).
DROP FUNCTION upg_snapshot(text);
DROP TABLE upg_snap;
