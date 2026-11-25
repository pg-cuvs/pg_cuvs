-- hnsw_build_m.sql: a pg_cuvs_hnsw index built from the heap follows its m reloption. The GPU graph is built with
-- degree 2 * m (the build receipt's graph_degree), and the metapage records that m, which pgvector's inserts read.
-- Without m the layout uses M 32, including when a small table needs a narrower graph. GPU only.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SELECT setseed(0.42);
CREATE TABLE hbm (id int, v vector(8));
INSERT INTO hbm SELECT g, (SELECT array_agg(random()::real) FROM generate_series(1, 8) d WHERE g > 0)::vector
FROM generate_series(1, 2000) g;
CREATE INDEX hbm_default ON hbm USING pg_cuvs_hnsw (v vector_l2_ops);
CREATE INDEX hbm_m8 ON hbm USING pg_cuvs_hnsw (v vector_l2_ops) WITH (m = 8);
CREATE INDEX hbm_m24 ON hbm USING pg_cuvs_hnsw (v vector_l2_ops) WITH (m = 24);
SELECT i AS index, (pg_cuvs_hnsw_layer_counts(i)).m AS meta_m,
       pg_cuvs_hnsw_build_info(i) ->> 'graph_degree' AS graph_degree
FROM unnest(ARRAY['hbm_default', 'hbm_m8', 'hbm_m24']::regclass[]) i;
-- an empty table writes the metapage itself
CREATE TABLE hbm_empty (id int, v vector(8));
CREATE INDEX hbm_empty_m8 ON hbm_empty USING pg_cuvs_hnsw (v vector_l2_ops) WITH (m = 8);
SELECT (pg_cuvs_hnsw_layer_counts('hbm_empty_m8')).m AS meta_m;
-- REINDEX keeps the m
REINDEX INDEX hbm_m8;
SELECT (pg_cuvs_hnsw_layer_counts('hbm_m8')).m AS meta_m;
-- a row inserted after the build is found through the m 8 index
DROP INDEX hbm_default, hbm_m24;
INSERT INTO hbm VALUES (9001, '[9,9,9,9,9,9,9,9]');
SET enable_seqscan = off;
SELECT id FROM hbm ORDER BY v <-> '[9,9,9,9,9,9,9,9]' LIMIT 1;
RESET enable_seqscan;
\pset format unaligned
\pset tuples_only on
-- ALTER stores the next heap-build setting; the live index still has its old M.
ALTER INDEX hbm_m8 SET (m = 24);
DELETE FROM hbm WHERE id = 2;
VACUUM (INDEX_CLEANUP ON) hbm;
SELECT (pg_cuvs_hnsw_layer_counts('hbm_m8')).m = 8 AS before_reindex;
REINDEX INDEX hbm_m8;
SELECT (pg_cuvs_hnsw_layer_counts('hbm_m8')).m = 24 AS after_reindex;

-- The source graph sets M. Report differences from an explicit target m.
SET cuvs.index_dir = '/tmp/cuvs_indexes';
CREATE INDEX hbm_c32 ON hbm USING cagra (v vector_l2_ops) WITH (graph_degree = 32);
CREATE INDEX hbm_c80 ON hbm USING cagra (v vector_l2_ops)
    WITH (graph_degree = 80, intermediate_graph_degree = 128);
CREATE INDEX hbm_source ON hbm USING pg_cuvs_hnsw (v vector_l2_ops) WITH (source = 'hbm_c32');
CREATE INDEX hbm_source_match ON hbm USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = 'hbm_c32', m = 16, mode = 'hnsw');
CREATE INDEX hbm_source_cap ON hbm USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = 'hbm_c80', m = 32);
CREATE INDEX hbm_source_legacy ON hbm USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = 'hbm_c32', m = 16, mode = 'hnswlib');
SELECT bool_and((pg_cuvs_hnsw_layer_counts(idx)).m = m) AS source_m_matches
FROM (VALUES ('hbm_source'::regclass, 16), ('hbm_source_match', 16),
             ('hbm_source_cap', 32), ('hbm_source_legacy', 16)) AS expected(idx, m);

CREATE INDEX hbm_bad_default ON hbm USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = 'hbm_c32', m = 32);
CREATE INDEX hbm_bad_cap ON hbm USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = 'hbm_c80', m = 33);
CREATE INDEX hbm_bad_legacy ON hbm USING pg_cuvs_hnsw (v vector_l2_ops)
    WITH (source = 'hbm_c32', m = 32, mode = 'hnswlib');
ALTER INDEX hbm_source SET (m = 32);
REINDEX INDEX hbm_source;
SELECT (pg_cuvs_hnsw_layer_counts('hbm_source')).m = 16 AS after_reindex_notice;
ALTER INDEX hbm_source RESET (m);
REINDEX INDEX hbm_source;
SELECT (pg_cuvs_hnsw_layer_counts('hbm_source')).m = 16 AS after_reset;

-- Small builds keep the configured slot capacity even when the graph is narrower.
DO $$
DECLARE
    sample record;
    receipt jsonb;
    actual_m integer;
    nearest integer;
BEGIN
    FOR sample IN
        SELECT n, 24 AS m, greatest(4, n - n % 2) AS degree FROM generate_series(1, 15) n
        UNION ALL SELECT 15, 2, 4
        UNION ALL SELECT 1, 100, 4
        UNION ALL SELECT 32, 24, 30
    LOOP
        CREATE TABLE hbm_small (id integer, v vector(8));
        INSERT INTO hbm_small
        SELECT g, ARRAY[g::real, 0, 0, 0, 0, 0, 0, 0]::vector
        FROM generate_series(1, sample.n) g;
        EXECUTE format('CREATE INDEX hbm_small_i ON hbm_small USING pg_cuvs_hnsw (v vector_l2_ops) WITH (m = %s)', sample.m);
        SELECT (pg_cuvs_hnsw_layer_counts('hbm_small_i')).m,
               pg_cuvs_hnsw_build_info('hbm_small_i') INTO actual_m, receipt;
        IF actual_m IS DISTINCT FROM sample.m
           OR (receipt ->> 'graph_degree')::integer IS DISTINCT FROM sample.degree
           OR (receipt ->> 'graph_degree_limited_by') IS DISTINCT FROM
               (CASE WHEN sample.degree < 2 * sample.m THEN 'input_rows' END) THEN
            RAISE EXCEPTION 'n=%, m=%, metadata M=%, graph degree=%, limit=%',
                sample.n, sample.m, actual_m, receipt ->> 'graph_degree', receipt ->> 'graph_degree_limited_by';
        END IF;
        INSERT INTO hbm_small VALUES (100, '[100,0,0,0,0,0,0,0]');
        PERFORM set_config('enable_seqscan', 'off', true);
        SELECT id INTO nearest FROM hbm_small ORDER BY v <-> '[100,0,0,0,0,0,0,0]' LIMIT 1;
        IF nearest IS DISTINCT FROM 100 THEN
            RAISE EXCEPTION 'n=%, m=%: inserted row was not found', sample.n, sample.m;
        END IF;
        DROP TABLE hbm_small;
    END LOOP;
END $$;

-- Check physical neighbor counts and item lengths on the supported little-endian page format.
CREATE EXTENSION IF NOT EXISTS pageinspect;
CREATE FUNCTION pg_temp.hbm_u16(p bytea, pos integer) RETURNS integer LANGUAGE sql IMMUTABLE AS $$
    SELECT get_byte(p, pos) + 256 * get_byte(p, pos + 1);
$$;
CREATE FUNCTION pg_temp.hbm_itemid(p bytea, pos integer) RETURNS bigint LANGUAGE sql IMMUTABLE AS $$
    SELECT pg_temp.hbm_u16(p, pos)::bigint + (pg_temp.hbm_u16(p, pos + 2)::bigint << 16);
$$;
CREATE FUNCTION pg_temp.hbm_check_pages(idx regclass, m integer) RETURNS boolean LANGUAGE sql AS $$
    WITH pages AS MATERIALIZED (
        SELECT b, get_raw_page(idx::text, b) AS p
        FROM generate_series(1, pg_relation_size(idx) / current_setting('block_size')::integer - 1) b
    ), items AS MATERIALIZED (
        SELECT b, p, slot, (word & 32767)::integer AS off, (word >> 17)::integer AS size
        FROM pages CROSS JOIN LATERAL generate_series(1, ((page_header(p)).lower - 24) / 4) slot
        CROSS JOIN LATERAL (SELECT pg_temp.hbm_itemid(p, 24 + (slot - 1) * 4) AS word) link
        WHERE ((word >> 15) & 3) = 1
    ), elem AS (
        SELECT (pg_temp.hbm_u16(p, off + 64)::bigint << 16) + pg_temp.hbm_u16(p, off + 66) AS nblock,
               pg_temp.hbm_u16(p, off + 68) AS nslot, (get_byte(p, off + 1) + 2) * m AS slots
        FROM items WHERE get_byte(p, off) = 1
    )
    SELECT count(*) = (pg_cuvs_hnsw_layer_counts(idx)).elements
           AND coalesce(bool_and((get_byte(items.p, items.off) = 2
               AND pg_temp.hbm_u16(items.p, items.off + 2) = elem.slots
               AND items.size = ((4 + 6 * elem.slots + 7) / 8) * 8) IS TRUE), false)
    FROM elem LEFT JOIN items ON items.b = elem.nblock AND items.slot = elem.nslot;
$$;
DO $$
DECLARE
    sample_m integer;
    algo text;
BEGIN
    FOREACH sample_m IN ARRAY ARRAY[33, 100] LOOP
        FOREACH algo IN ARRAY ARRAY['auto', 'nn_descent', 'ace'] LOOP
            EXECUTE format('CREATE INDEX hbm_range_i ON hbm USING pg_cuvs_hnsw (v vector_l2_ops) WITH (m = %s, build_algo = %L)', sample_m, algo);
            IF (pg_cuvs_hnsw_layer_counts('hbm_range_i')).m IS DISTINCT FROM sample_m
               OR (pg_cuvs_hnsw_build_info('hbm_range_i') ->> 'graph_degree')::integer IS DISTINCT FROM 2 * sample_m
               OR pg_temp.hbm_check_pages('hbm_range_i', sample_m) IS DISTINCT FROM true THEN
                RAISE EXCEPTION 'm=%, %: metadata, graph width or page layout differs', sample_m, algo;
            END IF;
            DROP INDEX hbm_range_i;
        END LOOP;
    END LOOP;
END $$;
CREATE TABLE hbm_half AS SELECT id, v::halfvec(8) AS v FROM hbm;
CREATE INDEX hbm_half_i ON hbm_half USING pg_cuvs_hnsw (v halfvec_l2_ops) WITH (m = 33);
SELECT pg_temp.hbm_check_pages('hbm_half_i', 33);
DROP TABLE hbm_half;
CREATE TABLE hbm_default_small AS SELECT * FROM hbm WHERE id <= 40;
CREATE INDEX hbm_default_small_i ON hbm_default_small USING pg_cuvs_hnsw (v vector_l2_ops);
SELECT (pg_cuvs_hnsw_layer_counts('hbm_default_small_i')).m, pg_temp.hbm_check_pages('hbm_default_small_i', 32);
DROP TABLE hbm_default_small;
DROP TABLE hbm, hbm_empty;
