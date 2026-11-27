-- build_hnsw_packing.sql — #161 lever 1: multiple elements packed per page.
--
-- mode='nsw' (the default/recommended mode) now packs a fixed k
-- element+neighbor pairs per page instead of one pair per page, since
-- every nsw element is level 0 and therefore uniform size. This is a pure
-- page-layout change: on-disk tuple format, graph structure, and neighbor
-- relationships are unchanged, so search correctness must be identical to
-- the pre-packing one-pair-per-page layout.
--
-- mode='nsw' now builds upper layers (see hnsw_insert_shift.sql), so
-- about 1/32 of the elements carry a larger neighbor tuple. Pairs are packed
-- in element order, each taking its own size: level-0 pairs still go k=9 to
-- a page here, and the page count grows by the room the upper-layer pairs
-- take. Levels come from the cagra_assign_level() draw seeded by (N, M), made
-- by the daemon's upper-layer builder (cuvs_hnsw_upper.c) or, when it builds
-- none, by the backend; elements keep row order. So the page count is a pure
-- function of N, M and dim whatever the builder's thread timing (measured: 59
-- pages for N=500 and for N=504, dim 100, M=32; 57 before 0.9.3).
--
-- Coverage:
--   1. index_bytes shrinks below the old one-page-per-element size — direct
--      evidence packing actually happened, not just that the build succeeded.
--   2. Full correctness sweep: every row is its own exact-match nearest
--      neighbor, including elements that land on different pages and
--      elements whose neighbor pointers cross a page boundary.
--   3. Rebuild determinism: same source, rebuilt from scratch, identical
--      index_bytes.
--   4. REINDEX preserves correctness on a packed index.
--   5. A regular INSERT after the packed build (pgvector's own aminsert
--      path, not ours) succeeds and the new row is searchable — exercises
--      metap->insertPage against a packed layout.
--   6. N an exact multiple of k: the last page is completely full (not the
--      partial-last-page case Case 1-5 exercise with N=500, k=9), still
--      correct.

\set ON_ERROR_STOP on
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
RESET client_min_messages;
SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- dim=100, graph_degree=64 (M=32): esize=MAXALIGN8(80+400)=480,
-- nsize(level=0,M=32)=MAXALIGN8(4+2*32*6)=392, needed=480+392+16=888,
-- k=floor(8160/888)=9 pairs/page. N=500 spans ceil(500/9)=56 pages — well
-- past 1, and past any single-digit edge case.
CREATE TABLE hp_test (id bigint, embedding vector(100));
-- The inner aggregate's argument must reference BOTH the outer loop
-- variable (i) and the inner one (j), or this silently produces 500
-- IDENTICAL vectors instead of 500 random ones (caught the hard way: an
-- earlier version of this file used an uncorrelated (no `i` reference)
-- scalar subquery, which becomes a single InitPlan evaluated once for the
-- whole INSERT rather than once per row -- LATERAL alone does not fix this
-- either, since with no reference to `i` inside it the planner still
-- treats it as unparameterized and materializes it once. Adding `i` fixes
-- the once-per-row problem but then trips a SEPARATE error ("column i.i
-- must appear in the GROUP BY clause") if the aggregate's argument touches
-- ONLY the outer `i` and never the inner level's own `j`: an aggregate
-- whose argument has no reference to its own query level's columns gets
-- bound to the OUTER level instead, colliding with `i` also being a plain
-- (non-aggregated) outer SELECT-list column. Referencing both `i` and `j`
-- resolves both issues at once -- verified empirically (500 distinct
-- vectors, 0 nearest-neighbor mismatches) before landing this file.
INSERT INTO hp_test
    SELECT i, (SELECT ('[' || string_agg((random() + i * 0.0 + j * 0.0)::text, ',') || ']')::vector
               FROM generate_series(1, 100) j)
    FROM generate_series(1, 500) i;
CREATE INDEX hp_cagra ON hp_test USING cagra
    (embedding vector_l2_ops) WITH (graph_degree = 64);

SET client_min_messages = 'warning';
CREATE INDEX hp_hnsw ON hp_test USING pg_cuvs_hnsw
    (embedding vector_l2_ops) WITH (source = 'hp_cagra', mode = 'nsw');
SET client_min_messages = 'notice';

-- ── Case 1: index_bytes shrinks below the old 1-pair-per-page size ──
-- Old layout: 1 (meta) + 500 (one per element) = 501 pages = 4,104,192 bytes.
-- Packed (k=9 for level-0 pairs, upper-layer pairs larger): 59 graph pages = 483,328 bytes, plus the pages of the cover (an index created under 0.9.3
-- carries them, see docs/reference.md, Cover guarantee).
-- The graph is exactly blocks 0..58: the last two bytes of a pgvector page are its page id (0xFF90, bytes 90 ff), block 58 has them and block 59,
-- the first block of the cover, does not. The relation ends with the ledger head.
CREATE EXTENSION IF NOT EXISTS pageinspect;
SELECT pg_relation_size('hp_hnsw') < 501 * 8192 AS packed_smaller_than_unpacked,
       substr(get_raw_page('hp_hnsw', 58), 8191, 2) = '\x90ff' AS last_graph_block_is_58,
       substr(get_raw_page('hp_hnsw', 59), 8191, 2) <> '\x90ff' AS block_59_is_not_graph,
       pg_relation_size('hp_hnsw') = ((pg_cuvs_hnsw_cover_status('hp_hnsw')->>'ledger_head')::int + 1) * 8192 AS ends_with_the_ledger_head;

-- ── Case 2: full correctness sweep ──
-- Every row must be its own exact-match (distance 0) nearest neighbor,
-- regardless of which page it and its neighbors landed on.
SET enable_cuvs = off; SET enable_seqscan = off;
SELECT count(*) AS mismatches
FROM hp_test t
WHERE t.id <> (
    SELECT t2.id FROM hp_test t2
    ORDER BY t2.embedding <-> t.embedding LIMIT 1
);
RESET enable_cuvs; RESET enable_seqscan;

-- ── Case 3: rebuild determinism ──
SELECT pg_relation_size('hp_hnsw') AS hp_bytes \gset
DROP INDEX hp_hnsw;
SET client_min_messages = 'warning';
CREATE INDEX hp_hnsw ON hp_test USING pg_cuvs_hnsw
    (embedding vector_l2_ops) WITH (source = 'hp_cagra', mode = 'nsw');
SET client_min_messages = 'notice';
SELECT pg_relation_size('hp_hnsw') = :'hp_bytes' AS rebuild_same_size;

-- ── Case 4: REINDEX preserves correctness on a packed index ──
SET client_min_messages = 'warning';
REINDEX INDEX hp_hnsw;
SET client_min_messages = 'notice';
SET enable_cuvs = off; SET enable_seqscan = off;
SELECT count(*) AS mismatches_after_reindex
FROM hp_test t
WHERE t.id <> (
    SELECT t2.id FROM hp_test t2
    ORDER BY t2.embedding <-> t.embedding LIMIT 1
);
RESET enable_cuvs; RESET enable_seqscan;

-- ── Case 5: regular INSERT after a packed build (pgvector's own aminsert,
-- exercising metap->insertPage against the packed layout) ──
INSERT INTO hp_test
    SELECT 501, (SELECT ('[' || string_agg(random()::text, ',') || ']')::vector
                 FROM generate_series(1, 100));
SET enable_cuvs = off; SET enable_seqscan = off;
SELECT id FROM hp_test
ORDER BY embedding <-> (SELECT embedding FROM hp_test WHERE id = 501) LIMIT 1;
RESET enable_cuvs; RESET enable_seqscan;

DROP TABLE hp_test CASCADE;

-- ── Case 6: N exactly divisible by k (504 = 9*56) — last page full ──
CREATE TABLE hp_test2 (id bigint, embedding vector(100));
INSERT INTO hp_test2
    SELECT i, (SELECT ('[' || string_agg((random() + i * 0.0 + j * 0.0)::text, ',') || ']')::vector
               FROM generate_series(1, 100) j)
    FROM generate_series(1, 504) i;
CREATE INDEX hp_cagra2 ON hp_test2 USING cagra
    (embedding vector_l2_ops) WITH (graph_degree = 64);

SET client_min_messages = 'warning';
CREATE INDEX hp_hnsw2 ON hp_test2 USING pg_cuvs_hnsw
    (embedding vector_l2_ops) WITH (source = 'hp_cagra2', mode = 'nsw');
SET client_min_messages = 'notice';

-- 504/9 = 56 exactly for level-0 pairs only; with the upper layers 1 (meta) + 58 pages, plus the pages of the cover (see Case 1).
SELECT substr(get_raw_page('hp_hnsw2', 58), 8191, 2) = '\x90ff' AS last_graph_block_is_58,
       substr(get_raw_page('hp_hnsw2', 59), 8191, 2) <> '\x90ff' AS block_59_is_not_graph,
       pg_relation_size('hp_hnsw2') = ((pg_cuvs_hnsw_cover_status('hp_hnsw2')->>'ledger_head')::int + 1) * 8192 AS ends_with_the_ledger_head;

SET enable_cuvs = off; SET enable_seqscan = off;
SELECT count(*) AS mismatches_exact_multiple
FROM hp_test2 t
WHERE t.id <> (
    SELECT t2.id FROM hp_test2 t2
    ORDER BY t2.embedding <-> t.embedding LIMIT 1
);
RESET enable_cuvs; RESET enable_seqscan;

-- The full-last-page case is the tightest-margin combination for a post-build
-- INSERT: the last page has ~240 bytes free (8192 - 24 header - 8 opaque -
-- 9*(480+392) tuples), too little for another pair's combinedSize, forcing
-- pgvector's own aminsert down its page-extend path (HnswInsertAppendPage)
-- rather than the fast in-page path Case 5's N=500 (partial last page,
-- ~3.7KB free) takes. metap->insertPage still has to point somewhere sane
-- for this to work at all.
INSERT INTO hp_test2
    SELECT 505, (SELECT ('[' || string_agg(random()::text, ',') || ']')::vector
                 FROM generate_series(1, 100));
SET enable_cuvs = off; SET enable_seqscan = off;
SELECT id FROM hp_test2
ORDER BY embedding <-> (SELECT embedding FROM hp_test2 WHERE id = 505) LIMIT 1;
RESET enable_cuvs; RESET enable_seqscan;

DROP TABLE hp_test2 CASCADE;
DROP EXTENSION pageinspect;
