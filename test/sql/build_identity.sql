-- build_identity.sql — a build is used only while the catalog's index is the one it was built for.
--
-- The daemon holds a build per index OID, and the build's TIDs name rows of the heap
-- it was built from. Several sequences leave the daemon (and the files) with a build
-- of another heap or of a rolled-back state while the catalog's index is the same OID;
-- an index scan or a function then answered with TIDs that name other rows. The rule:
-- a build records the index relfilenode it was built for, and the planner and the
-- daemon use it only while the catalog's index still has that relfilenode.
--
-- Old rows: id g, v [g,0,0,0], g = 1..200. New rows are inserted in reverse: id 1000+g,
-- v [201-g,0,0,0], so a build of the other heap names other rows at the same TIDs.
-- Query [1,0,0,0], k 5. Each case prints whether the index was used (EXPLAIN), the
-- index path's answer (or the SQLSTATE it raised) and whether it equals the heap's.

\set ON_ERROR_STOP on
SET client_min_messages = WARNING;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';
SET cuvs.ivfpq_n_probes = 4;
SET cuvs.k = 8;
SELECT oid AS dboid FROM pg_database WHERE datname = current_database() \gset
CREATE TEMP TABLE bi_sink (x text);
-- the index_dir of case 4, writable by the daemon's user
COPY bi_sink FROM PROGRAM 'mkdir -p /tmp/cuvs_bi4; chmod 0777 /tmp/cuvs_bi4 2>/dev/null; echo made';

CREATE FUNCTION bi_uses_index(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
  FOR r IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    IF r LIKE '%Index Scan%' THEN RETURN true; END IF;
  END LOOP;
  RETURN false;
END $$;
-- The answer of q (a query returning one text), or 'ERROR <sqlstate>'.
CREATE FUNCTION bi_try(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
  EXECUTE q INTO r;
  RETURN r;
EXCEPTION WHEN OTHERS THEN
  RETURN 'ERROR ' || SQLSTATE;
END $$;
-- Index path answer, heap answer, and the verdict for table t.
CREATE FUNCTION bi_case(t text) RETURNS TABLE (index_used boolean, index_answer text, like_heap boolean) LANGUAGE plpgsql AS $$
DECLARE q text := format('SELECT string_agg(id::text, '','' ORDER BY d, id) FROM (SELECT id, v <-> ''[1,0,0,0]'' d FROM %I ORDER BY v <-> ''[1,0,0,0]'' LIMIT 5) s', t);
        h text;
BEGIN
  SET LOCAL enable_seqscan = off;
  index_used := bi_uses_index(q);
  index_answer := bi_try(q);
  SET LOCAL enable_seqscan = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off;
  EXECUTE q INTO h;
  SET LOCAL enable_indexscan = on; SET LOCAL enable_bitmapscan = on;
  like_heap := index_answer = h;
  RETURN NEXT;
END $$;
CREATE FUNCTION bi_fill_old(t text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN EXECUTE format('INSERT INTO %I SELECT g, ARRAY[g,0,0,0]::real[]::vector FROM generate_series(1,200) g', t); END $$;
CREATE FUNCTION bi_fill_new(t text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN EXECUTE format('INSERT INTO %I SELECT 1000+g, ARRAY[201-g,0,0,0]::real[]::vector FROM generate_series(1,200) g', t); END $$;

-- 1. The function paths in the transaction that truncated and refilled the table.
--    No exact path exists in them: refused with 55000, never the old build's rows.
CREATE TABLE bi1 (id int, v vector(4));
SELECT bi_fill_old('bi1');
CREATE INDEX bi1_i ON bi1 USING cagra (v vector_l2_ops);
BEGIN;
TRUNCATE bi1;
SELECT bi_fill_new('bi1');
SELECT bi_try($q$SELECT string_agg(x.id::text, ',' ORDER BY b.distance, x.id) FROM pg_cuvs_batch_search('bi1', ARRAY['[1,0,0,0]'::vector], 5) b JOIN bi1 x ON x.ctid = b.ctid$q$) AS batch_search;
SELECT bi_try($q$SELECT string_agg(x.id::text, ',' ORDER BY f.distance, x.id) FROM cuvs_filtered_knn('bi1_i'::regclass, '[1,0,0,0]'::vector, NULL::tid[], 5) f JOIN bi1 x ON x.ctid = f.ctid$q$) AS filtered_knn;
SELECT * FROM bi_case('bi1');
COMMIT;
-- After REINDEX the index is used again and answers like the heap, and so do the functions.
REINDEX INDEX bi1_i;
SELECT * FROM bi_case('bi1');
SELECT bi_try($q$SELECT string_agg(x.id::text, ',' ORDER BY b.distance, x.id) FROM pg_cuvs_batch_search('bi1', ARRAY['[1,0,0,0]'::vector], 5) b JOIN bi1 x ON x.ctid = b.ctid$q$) AS batch_search;
DROP TABLE bi1;

-- 2. A rolled-back REINDEX of the refilled table: the daemon holds the rolled-back build.
CREATE TABLE bi2 (id int, v vector(4));
SELECT bi_fill_old('bi2');
CREATE INDEX bi2_i ON bi2 USING cagra (v vector_l2_ops);
BEGIN;
TRUNCATE bi2;
SELECT bi_fill_new('bi2');
REINDEX INDEX bi2_i;
ROLLBACK;
SELECT * FROM bi_case('bi2');
DROP TABLE bi2;

-- 3. The same with a savepoint: the TRUNCATE commits, the REINDEX and its rows roll back.
CREATE TABLE bi3 (id int, v vector(4));
SELECT bi_fill_old('bi3');
CREATE INDEX bi3_i ON bi3 USING cagra (v vector_l2_ops);
BEGIN;
TRUNCATE bi3;
SAVEPOINT s;
SELECT bi_fill_new('bi3');
REINDEX INDEX bi3_i;
ROLLBACK TO s;
COMMIT;
SELECT bi_fill_new('bi3');
SELECT * FROM bi_case('bi3');
DROP TABLE bi3;

-- 4. An index in its own index_dir (reloption), TRUNCATE committed: the heap answers, no error.
CREATE TABLE bi4 (id int, v vector(4));
SELECT bi_fill_old('bi4');
CREATE INDEX bi4_i ON bi4 USING cagra (v vector_l2_ops) WITH (index_dir = '/tmp/cuvs_bi4');
TRUNCATE bi4;
SELECT bi_fill_new('bi4');
SELECT * FROM bi_case('bi4');
REINDEX INDEX bi4_i;
SELECT * FROM bi_case('bi4');
DROP TABLE bi4;

-- 5. flat and ivfpq: a rolled-back REINDEX of the refilled table.
CREATE TABLE bi5 (id int, v vector(4));
SELECT bi_fill_old('bi5');
CREATE INDEX bi5_i ON bi5 USING flat (v vector_l2_ops);
BEGIN; TRUNCATE bi5; SELECT bi_fill_new('bi5'); REINDEX INDEX bi5_i; ROLLBACK;
SELECT * FROM bi_case('bi5');
DROP TABLE bi5;
CREATE TABLE bi6 (id int, v vector(4));
SELECT bi_fill_old('bi6');
CREATE INDEX bi6_i ON bi6 USING ivfpq (v vector_l2_ops) WITH (n_lists = 4, pq_bits = 8, pq_dim = 2);
BEGIN; TRUNCATE bi6; SELECT bi_fill_new('bi6'); REINDEX INDEX bi6_i; ROLLBACK;
SELECT * FROM bi_case('bi6');
DROP TABLE bi6;

-- 6. A build with no record (an ivfpq index built before the record existed): the heap answers until REINDEX.
CREATE TABLE bi7 (id int, v vector(4));
SELECT bi_fill_old('bi7');
CREATE INDEX bi7_i ON bi7 USING ivfpq (v vector_l2_ops) WITH (n_lists = 4, pq_bits = 8, pq_dim = 2);
SELECT format('%s/%s_%s.relfilenode', current_setting('cuvs.index_dir'), :dboid, 'bi7_i'::regclass::oid) AS rec \gset
SELECT format('COPY bi_sink FROM PROGRAM %L', 'rm -f ' || :'rec' || '; echo removed') AS cmd \gset
:cmd;
\c
SET client_min_messages = WARNING;
SET cuvs.index_dir = '/tmp/cuvs_indexes';
SET cuvs.ivfpq_n_probes = 4;
SET cuvs.k = 8;
CREATE TEMP TABLE bi_sink (x text);
SELECT * FROM bi_case('bi7');
REINDEX INDEX bi7_i;
SELECT * FROM bi_case('bi7');
DROP TABLE bi7;

-- 7. Reclaim: after a committed TRUNCATE the daemon holds no entry for the index (the drop at commit).
CREATE TABLE bi8 (id int, v vector(4));
SELECT bi_fill_old('bi8');
CREATE INDEX bi8_i ON bi8 USING cagra (v vector_l2_ops);
SELECT 'bi8_i'::regclass::oid AS bi8 \gset
SELECT count(*) AS entries_before FROM pg_cuvs_index_status() WHERE index_oid = :bi8;
TRUNCATE bi8;
SELECT count(*) AS entries_after_truncate FROM pg_cuvs_index_status() WHERE index_oid = :bi8;
DROP TABLE bi8;

-- 8. A rolled-back REINDEX alone (same rows): the index's relfilenode goes back, the daemon holds the rolled-back build. The heap
--    answers until REINDEX, and pg_stat_gpu_fallback says why.
CREATE TABLE bi9 (id int, v vector(4));
SELECT bi_fill_old('bi9');
CREATE INDEX bi9_i ON bi9 USING cagra (v vector_l2_ops);
BEGIN; REINDEX INDEX bi9_i; ROLLBACK;
SELECT * FROM bi_case('bi9');
SELECT last_reason FROM pg_stat_gpu_fallback WHERE index_oid = 'bi9_i'::regclass;
REINDEX INDEX bi9_i;
SELECT * FROM bi_case('bi9');
DROP TABLE bi9;

-- 9. ALTER INDEX ... SET TABLESPACE gives the index a new relfilenode without a build: the heap answers until REINDEX.
COPY bi_sink FROM PROGRAM 'mkdir -p /tmp/bi_ts && echo made';
CREATE TABLESPACE bi_ts LOCATION '/tmp/bi_ts';
CREATE TABLE bi10 (id int, v vector(4));
SELECT bi_fill_old('bi10');
CREATE INDEX bi10_i ON bi10 USING cagra (v vector_l2_ops);
ALTER INDEX bi10_i SET TABLESPACE bi_ts;
SELECT * FROM bi_case('bi10');
SELECT last_reason FROM pg_stat_gpu_fallback WHERE index_oid = 'bi10_i'::regclass;
REINDEX INDEX bi10_i;
SELECT * FROM bi_case('bi10');
DROP TABLE bi10;
DROP TABLESPACE bi_ts;

-- 10. An in-place TRUNCATE: a table created in this transaction keeps its index relfilenode when emptied, so
--     the record must say "empty" (index relfilenode 0). In the transaction and after the commit the heap answers.
BEGIN;
CREATE TABLE bi11 (id int, v vector(4));
SELECT bi_fill_old('bi11');
CREATE INDEX bi11_i ON bi11 USING cagra (v vector_l2_ops);
SELECT relfilenode AS bi11_rfn FROM pg_class WHERE relname = 'bi11_i' \gset
TRUNCATE bi11;
SELECT relfilenode = :bi11_rfn AS same_relfilenode FROM pg_class WHERE relname = 'bi11_i';
SELECT bi_fill_new('bi11');
SELECT * FROM bi_case('bi11');
SELECT bi_try($q$SELECT string_agg(x.id::text, ',' ORDER BY b.distance, x.id) FROM pg_cuvs_batch_search('bi11', ARRAY['[1,0,0,0]'::vector], 5) b JOIN bi11 x ON x.ctid = b.ctid$q$) AS batch_search;
COMMIT;
SELECT * FROM bi_case('bi11');
REINDEX INDEX bi11_i;
SELECT * FROM bi_case('bi11');
DROP TABLE bi11;

-- 11. The same after a REINDEX in the transaction (the table's relfilenode is new in it, so the second TRUNCATE is in place).
CREATE TABLE bi12 (id int, v vector(4));
SELECT bi_fill_old('bi12');
CREATE INDEX bi12_i ON bi12 USING cagra (v vector_l2_ops);
BEGIN;
TRUNCATE bi12;
INSERT INTO bi12 SELECT 1000+g, ARRAY[g,0,0,0]::real[]::vector FROM generate_series(1,200) g;
REINDEX INDEX bi12_i;
TRUNCATE bi12;
INSERT INTO bi12 SELECT 2000+g, ARRAY[201-g,0,0,0]::real[]::vector FROM generate_series(1,200) g;
SELECT * FROM bi_case('bi12');
COMMIT;
SELECT * FROM bi_case('bi12');
DROP TABLE bi12;

-- 12. An in-place TRUNCATE that rolls back: the table goes with it (created in the transaction); nothing remains to answer.
BEGIN;
CREATE TABLE bi13 (id int, v vector(4));
SELECT bi_fill_old('bi13');
CREATE INDEX bi13_i ON bi13 USING cagra (v vector_l2_ops);
TRUNCATE bi13;
ROLLBACK;
SELECT count(*) AS bi13_left FROM pg_class WHERE relname IN ('bi13', 'bi13_i');

-- 13. The table stays and only the TRUNCATEs roll back: the second TRUNCATE is in place (the first gave the
--     table a new relfilenode in this transaction), so it rewrites the record only if the record names the index's relfilenode now,
--     which it does not. After the rollback the old relfilenode and its record are back: the index answers like the heap.
CREATE TABLE bi14 (id int, v vector(4));
SELECT bi_fill_old('bi14');
CREATE INDEX bi14_i ON bi14 USING cagra (v vector_l2_ops);
BEGIN;
TRUNCATE bi14;
TRUNCATE bi14;
ROLLBACK;
SELECT * FROM bi_case('bi14');
DROP TABLE bi14;

-- 14. A rollback candidate: ALTER TABLE SET TABLESPACE gives the heap a new relfilenode in this transaction and
--     leaves the index's, so the TRUNCATE after it empties the index in place and the record is rewritten as empty; the ROLLBACK does
--     not bring the record back. What this prints is what happens now: a known limit (docs/reference.md), the expected output fixes
--     today's behavior, not a wanted one.
CREATE TABLESPACE bi_ts14 LOCATION '/tmp/bi_ts';
CREATE TABLE bi15 (id int, v vector(4));
SELECT bi_fill_old('bi15');
CREATE INDEX bi15_i ON bi15 USING cagra (v vector_l2_ops);
BEGIN;
ALTER TABLE bi15 SET TABLESPACE bi_ts14;
TRUNCATE bi15;
ROLLBACK;
SELECT * FROM bi_case('bi15');
REINDEX INDEX bi15_i;
SELECT * FROM bi_case('bi15');
DROP TABLE bi15;
DROP TABLESPACE bi_ts14;

-- 15. A committed ALTER TABLE SET TABLESPACE: the table gets a new relfilenode with the same rows and TIDs and its
--     indexes are not rebuilt; the build stays in use for searches and inserts, and answers like the heap.
CREATE TABLESPACE bi_ts15 LOCATION '/tmp/bi_ts';
CREATE TABLE bi16 (id int, v vector(4));
SELECT bi_fill_old('bi16');
CREATE INDEX bi16_i ON bi16 USING cagra (v vector_l2_ops);
ALTER TABLE bi16 SET TABLESPACE bi_ts15;
SELECT * FROM bi_case('bi16');
INSERT INTO bi16 VALUES (5000, '[0.5,0,0,0]');
SELECT * FROM bi_case('bi16');
DROP TABLE bi16;
DROP TABLESPACE bi_ts15;

DROP FUNCTION bi_case(text), bi_try(text), bi_uses_index(text), bi_fill_old(text), bi_fill_new(text);
