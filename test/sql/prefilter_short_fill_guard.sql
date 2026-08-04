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
-- short answer, not a failure, and must NOT be retried on D-wedge. The
-- collapse-detection half is exercised on real GPU via
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

SET cuvs.search_mode = brute_force;   -- build the .vectors sidecar (needed by D-wedge)
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

-- Sanity: an unrestricted (>= k members) filter on the same index still
-- fills all k and does not trip the guard either.
SELECT count(*) AS n_full
FROM cuvs_filtered_knn(
    'psf_cagra'::regclass,
    '[0.5,0.3,0.7,0.2]'::vector(4),
    ARRAY(SELECT ctid FROM psf),
    10
) f;

SELECT search_mode AS mode_full_filter,
       prefilter_fallback_count AS fallback_count_after_full_filter
FROM pg_stat_gpu_search WHERE index_oid = 'psf_cagra'::regclass;

RESET cuvs.filter_auto_threshold;
RESET cuvs.search_mode;
DROP TABLE psf CASCADE;
