-- prefilter_short_fill_guard.sql — #133 / ADR-083: the 3O short-fill fallback
-- guard must not misfire when a short answer is CORRECT (the filter set is
-- smaller than k), only when it signals a collapse (materially fewer results
-- than the filter set could supply).
--
-- The real collapse (CAGRA traversal failing to reach an anti-correlated
-- filter's passing rows) needs real GPU graph behavior: the Tier-1 CPU shim's
-- cuvs_cagra_search_filtered is an exact filtered brute-force search (see
-- design/ci-strategy.md), so it always fills every slot the filter set can
-- supply and can never reproduce the collapse. This file therefore covers
-- only the guard's other half — a filter set smaller than k is a legitimate
-- short answer, not a failure, and must NOT be retried on the GPU exact BF
-- prefilter (gpu_bf_prefilter -- not the D-wedge post-filter; different code
-- path, different cost model). The collapse-detection half is exercised on
-- real GPU via
-- bench/filter_recall/adr079_3o_recall.py --correlations anti (ADR-083 VM
-- verification).

\set ON_ERROR_STOP off
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';

DROP TABLE IF EXISTS psf CASCADE;
CREATE TABLE psf (row_id int NOT NULL, v vector(4));
SELECT setseed(0.13);
INSERT INTO psf
SELECT g, array_agg(round((random() * 0.9 + 0.05)::numeric, 4) ORDER BY d)::real[]::vector(4)
FROM generate_series(1, 200) g, generate_series(1, 4) d
GROUP BY g;

SET cuvs.search_mode = brute_force;   -- build the .vectors sidecar (main_bf_idx, needed
                                       -- both by D-wedge and by the gpu_bf_prefilter
                                       -- fallback this test exercises)
CREATE INDEX psf_cagra ON psf USING cagra (v vector_l2_ops);

-- Force every filtered call onto the 3O CAGRA-prefilter path.
SET cuvs.filter_auto_threshold = 1.0;

-- Filter set (3 rows) smaller than k (10): a correct 3O answer returns
-- exactly those 3 rows, not 10 — that must NOT be treated as a collapse.
SELECT count(*) AS n, min(row_id) AS min_id, max(row_id) AS max_id
FROM cuvs_filtered_knn(
    'psf_cagra'::regclass,
    '[0.5,0.3,0.7,0.2]'::vector(4),
    ARRAY(SELECT ctid FROM psf WHERE row_id IN (10, 50, 90)),
    10
) f
JOIN psf ON psf.ctid = f.ctid;

-- Must still show cagra_prefilter (mode 4), not a fallback to brute_force
-- (mode 3) or stream_bf.
SELECT search_mode AS mode_short_filter
FROM pg_stat_gpu_search WHERE index_oid = 'psf_cagra'::regclass;

-- The short-fill guard must not have fired: this is a correct short answer,
-- not a detected collapse.
SELECT prefilter_fallback_count AS fallback_count_after_short_filter
FROM pg_stat_gpu_search WHERE index_oid = 'psf_cagra'::regclass;

-- Sanity: a large-but-not-whole-corpus filter (190/200 rows, sel=0.95 < the
-- forced threshold of 1.0) still routes to 3O, fills all k, and does not
-- trip the guard. (A filter over the WHOLE corpus has sel=1.0, which is NOT
-- "< 1.0" and would route to D-wedge regardless of this GUC -- not what this
-- block means to exercise.)
SELECT count(*) AS n_full
FROM cuvs_filtered_knn(
    'psf_cagra'::regclass,
    '[0.5,0.3,0.7,0.2]'::vector(4),
    ARRAY(SELECT ctid FROM psf WHERE row_id <= 190),
    10
) f;

SELECT search_mode AS mode_full_filter,
       prefilter_fallback_count AS fallback_count_after_full_filter
FROM pg_stat_gpu_search WHERE index_oid = 'psf_cagra'::regclass;

RESET cuvs.filter_auto_threshold;
RESET cuvs.search_mode;
DROP TABLE psf CASCADE;

-- ----------------------------------------------------------------
-- #133 review F3: the short-fill guard is gated on e->main_bf_idx != NULL.
-- Without a resident BF index (no `.vectors` sidecar -- this table is never
-- built with cuvs.search_mode=brute_force), there is nothing to retry into.
-- Before this gate, a detected short fill would force pret=1 with no BF
-- fallback available, and the daemon's existing NO_VECTORS guard (above the
-- 3O block) would turn what used to be a degraded-but-approximate 3O answer
-- into a hard ERROR -- an availability regression, not just a recall one.
-- This asserts the query still returns OK (not an error) and stays on
-- cagra_prefilter, matching pre-#133 behavior for this configuration.
-- ----------------------------------------------------------------

DROP TABLE IF EXISTS psf_novectors CASCADE;
CREATE TABLE psf_novectors (row_id int NOT NULL, v vector(4));
INSERT INTO psf_novectors
SELECT g, array_agg(round((random() * 0.9 + 0.05)::numeric, 4) ORDER BY d)::real[]::vector(4)
FROM generate_series(1, 200) g, generate_series(1, 4) d
GROUP BY g;

-- cuvs.search_mode stays at its default ('cagra') -- no .vectors sidecar,
-- so main_bf_idx never gets built for this index.
CREATE INDEX psf_novectors_cagra ON psf_novectors USING cagra (v vector_l2_ops);

SET cuvs.filter_auto_threshold = 1.0;

SELECT count(*) AS n
FROM cuvs_filtered_knn(
    'psf_novectors_cagra'::regclass,
    '[0.5,0.3,0.7,0.2]'::vector(4),
    ARRAY(SELECT ctid FROM psf_novectors WHERE row_id IN (10, 50, 90)),
    10
) f;

-- Must be cagra_prefilter, not an error and not a fallback mode (there is
-- no BF index to fall back to).
SELECT search_mode AS mode_no_vectors, last_status AS status_no_vectors
FROM pg_stat_gpu_search WHERE index_oid = 'psf_novectors_cagra'::regclass;

RESET cuvs.filter_auto_threshold;
DROP TABLE psf_novectors CASCADE;

-- ----------------------------------------------------------------
-- #133 review F2: `expect` must be sized from n_included (bits the bitset-
-- building loop actually set), not cmd->n_filter_tids (what the backend
-- sent). A TID the loop's rev_tids binary search can't find -- a post-build
-- delta row, a tombstoned row, or (as constructed determinstically here)
-- an encoded TID that was never a real ctid of this table -- is silently
-- skipped, so the bitset can legitimately include fewer positions than the
-- filter array's length. cuvs_filtered_knn's bigint[] overload takes
-- encoded TIDs directly from the client, so this gap is constructible
-- without relying on delta/tombstone timing (which the #133 review noted
-- is nondeterministic and not worth chasing -- see the PR discussion).
--
-- This is deliberately real == 3 (< k) so the query only reaches the F2
-- codepath if F3's k-vs-|filter| guard has already let it through: a
-- pre-F2 (cmd->n_filter_tids based) `expect` of min(10,23)=10 exceeds what
-- 3 real matches can ever fill (filled=3 < 10), so it would retry on
-- gpu_bf_prefilter -- misjudging a CORRECT 3-row answer as a collapse.
-- Post-F2, n_included=3 (only the 3 real TIDs resolve), so expect=3 and
-- filled(3) < expect(3) is false: no retry.
--
-- Verified manually that this test FAILS without the F2 fix: reverting
-- `expect = (int)n_included` back to `expect = (int)cmd->n_filter_tids`
-- (i.e. sizing `expect` from the filter array length instead of the
-- bitset-loop's actual hit count) flips mode_f2_gap from cagra_prefilter to
-- brute_force and fallback_count_after_f2_gap from 0 to 1 -- confirmed by
-- temporarily reverting expect's source, rebuilding the daemon, and
-- re-running this file on pg-cuvs-item2b before restoring the fix.
-- ----------------------------------------------------------------

DROP TABLE IF EXISTS psf_f2 CASCADE;
CREATE TABLE psf_f2 (row_id int NOT NULL, v vector(4));
INSERT INTO psf_f2
SELECT g, array_agg(round((random() * 0.9 + 0.05)::numeric, 4) ORDER BY d)::real[]::vector(4)
FROM generate_series(1, 200) g, generate_series(1, 4) d
GROUP BY g;

-- Give this index a resident BF index (main_bf_idx != NULL) so the F3 gate
-- alone can't explain a lack of retry -- this test isolates F1+F2's expect
-- computation specifically.
SET cuvs.search_mode = brute_force;
CREATE INDEX psf_f2_cagra ON psf_f2 USING cagra (v vector_l2_ops);
SET cuvs.filter_auto_threshold = 1.0;

SELECT prefilter_fallback_count AS fallback_count_before_f2_gap
FROM pg_stat_gpu_search WHERE index_oid = 'psf_f2_cagra'::regclass;

-- 3 real encoded ctids + 20 encoded TIDs that were never real ctids of this
-- table (block 999999, offsets 1..20 -- this table has nowhere near that
-- many heap blocks). n_filter_tids=23, n_included=3.
SELECT count(*) AS n_f2_gap
FROM cuvs_filtered_knn(
    'psf_f2_cagra'::regclass,
    '[0.5,0.3,0.7,0.2]'::vector(4),
    (SELECT array_agg((((ctid::text::point)[0])::bigint << 16)
                     | (((ctid::text::point)[1])::bigint))
       FROM psf_f2 WHERE row_id IN (10, 50, 90))
    || ARRAY(SELECT (999999::bigint << 16) | g::bigint
              FROM generate_series(1, 20) g),
    10
) f;

SELECT search_mode AS mode_f2_gap,
       prefilter_fallback_count AS fallback_count_after_f2_gap
FROM pg_stat_gpu_search WHERE index_oid = 'psf_f2_cagra'::regclass;

RESET cuvs.filter_auto_threshold;
RESET cuvs.search_mode;
DROP TABLE psf_f2 CASCADE;

-- ----------------------------------------------------------------
-- #133 review, round 3: the psf_f2 case above proves F2 is correct against
-- a FABRICATED gap (encoded TIDs that were never real ctids). It does not
-- prove the causal claim in the code comment and ADR-083 -- that a
-- post-build DELTA ROW produces this same gap. This section reproduces
-- that specific cause.
--
-- Mechanism (found by reading cuvs_aminsert, src/pg_cuvs.c): a `cagra`-AM
-- INSERT does NOT go straight to the async .delta file. When the daemon
-- socket is reachable, aminsert synchronously calls cuvs_ipc_extend() per
-- row, which grows the resident CAGRA graph (and rebuilds rev_tids) in
-- place immediately -- so in normal operation a delta row is usually
-- absorbed into the base graph before any query ever sees it pending,
-- which is exactly why chasing this race non-deterministically failed
-- twice before. Only when the EXTEND call cannot reach the daemon does
-- aminsert fall through to cuvs_delta_append() (a direct file write, not
-- an RPC) -- and THAT path is what leaves rev_tids permanently unaware of
-- the new row until a REINDEX. `cuvs.socket_path=''` (already used by the
-- fc_anti case in filter_comparison.sql for VACUUM) makes aminsert take
-- that fallback deterministically, without racing anything.
--
-- Confirmed the gap is real, not merely inferred: with 5 rows inserted
-- this way, n_vecs stayed unchanged (checked via pg_stat_gpu_search) and
-- the daemon log showed "delta cache ... built (5 rows...)" but no
-- "extend" activity -- the base graph never saw them. A direct probe
-- (filter = ONLY those 5 pending-delta TIDs) returned all 5 rows from
-- cuvs_filtered_knn's SQL-visible output, which looked at first like the
-- gap does NOT exist -- but that visible row count comes from a SEPARATE
-- backend-side step (cuvs_merge_delta_filtered in src/pg_cuvs.c, called
-- after the daemon replies) that CPU-merges matching .delta rows into
-- filtered results independent of the daemon's own search. It runs after
-- the guard's decision, not as part of it. The daemon-side observation
-- point -- search_mode / prefilter_fallback_count, exactly what this
-- guard controls -- showed cagra_prefilter / unchanged throughout,
-- confirming the daemon's own n_included correctly excluded the pending
-- rows and did not misfire.
-- ----------------------------------------------------------------

DROP TABLE IF EXISTS psf_delta CASCADE;
CREATE TABLE psf_delta (row_id int NOT NULL, v vector(4));
INSERT INTO psf_delta
SELECT g, array_agg(round((random() * 0.9 + 0.05)::numeric, 4) ORDER BY d)::real[]::vector(4)
FROM generate_series(1, 200) g, generate_series(1, 4) d
GROUP BY g;

SET cuvs.search_mode = brute_force;   -- main_bf_idx present (isolates F1+F2)
CREATE INDEX psf_delta_cagra ON psf_delta USING cagra (v vector_l2_ops);
RESET cuvs.search_mode;               -- back to 'cagra' -- brute_force mode
                                       -- skips refresh_delta_cache() below

-- Force aminsert to skip the synchronous EXTEND RPC so these 5 rows go
-- through cuvs_delta_append() (a direct file write) instead of growing the
-- resident CAGRA graph -- see the mechanism note above.
SET cuvs.socket_path = '';
INSERT INTO psf_delta
SELECT g, array_agg(round((random() * 0.9 + 0.05)::numeric, 4) ORDER BY d)::real[]::vector(4)
FROM generate_series(201, 205) g, generate_series(1, 4) d
GROUP BY g;
RESET cuvs.socket_path;

-- Step 1 (review instruction): confirm the rows are genuinely pending
-- BEFORE checking the gap. A plain (non-prefiltered) CAGRA search is the
-- path that calls refresh_delta_cache() and picks up the .delta file.
SET enable_seqscan = off;
SELECT count(*) FROM (
    SELECT ctid FROM psf_delta
    ORDER BY v <-> '[0.5,0.3,0.7,0.2]'::vector(4) LIMIT 5
) s;
RESET enable_seqscan;

SELECT delta_rows AS pending_delta_confirmed
FROM pg_stat_gpu_search WHERE index_oid = 'psf_delta_cagra'::regclass;

-- Step 2: filter on 3 real base rows + all 5 pending-delta rows, force 3O.
-- n_filter_tids=8; n_included should be 3 (rev_tids never absorbed the
-- delta rows). k=5 so a pre-F2 expect=min(5,8)=5 would exceed what the
-- daemon can ever find (3), misjudging a correct answer as a collapse.
SET cuvs.filter_auto_threshold = 1.0;

SELECT prefilter_fallback_count AS fallback_before_real_delta_gap
FROM pg_stat_gpu_search WHERE index_oid = 'psf_delta_cagra'::regclass;

SELECT count(*) AS n_real_delta_gap  -- SQL-visible count; see note above on
                                      -- why this isn't the assertion point
FROM cuvs_filtered_knn(
    'psf_delta_cagra'::regclass,
    '[0.5,0.3,0.7,0.2]'::vector(4),
    (SELECT array_agg(ctid) FROM psf_delta WHERE row_id IN (10, 50, 90))
    || (SELECT array_agg(ctid) FROM psf_delta WHERE row_id > 200),
    5
) f;

-- The actual assertion: the daemon's own decision, unaffected by the
-- backend's separate delta merge above.
SELECT search_mode AS mode_real_delta_gap,
       prefilter_fallback_count AS fallback_after_real_delta_gap
FROM pg_stat_gpu_search WHERE index_oid = 'psf_delta_cagra'::regclass;

RESET cuvs.filter_auto_threshold;
DROP TABLE psf_delta CASCADE;
