\set ON_ERROR_STOP on
-- U-13: run with psql -X -d DISPOSABLE_DB -f test/page_format/u13.sql.
-- Use a superuser on a page-format primary with vector, pg_cuvs and pageinspect in public.
-- The normal run rolls back its SQL objects. The Python runner uses setup-only
-- mode to commit the shared fixture before starting its independent sessions.
\if :{?u13_schema}
\else
SELECT 'u13_' || replace(gen_random_uuid()::text, '-', '') AS u13_schema \gset
\endif

BEGIN;
SET LOCAL search_path TO :"u13_schema", public;
SET LOCAL lock_timeout = '10s';
SET LOCAL statement_timeout = '60s';
CREATE SCHEMA :"u13_schema";

CREATE FUNCTION use_bf_oracle_settings(local_settings boolean DEFAULT true)
RETURNS void LANGUAGE plpgsql VOLATILE AS $$
BEGIN
    PERFORM set_config('enable_cuvs', 'on', local_settings);
    PERFORM set_config('cuvs.search_mode', 'brute_force', local_settings);
    PERFORM set_config('cuvs.bf_precision', 'float32', local_settings);
    PERFORM set_config('cuvs.cpu_hnsw_fallback', 'off', local_settings);
    PERFORM set_config('cuvs.filtered_knn_hook', 'off', local_settings);
    PERFORM set_config('enable_seqscan', 'on', local_settings);
    PERFORM set_config('enable_indexscan', 'off', local_settings);
    PERFORM set_config('enable_indexonlyscan', 'off', local_settings);
    PERFORM set_config('enable_bitmapscan', 'off', local_settings);
    PERFORM set_config('enable_tidscan', 'off', local_settings);
    PERFORM set_config('max_parallel_workers_per_gather', '0', local_settings);
    PERFORM set_config('row_security', 'on', local_settings);
END;
$$;

CREATE FUNCTION assert_page(idx regclass) RETURNS void LANGUAGE plpgsql STABLE AS $$
BEGIN
    IF NOT EXISTS (
        SELECT FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
        JOIN pg_am am ON am.oid = c.relam
        WHERE c.oid = idx AND am.amname = 'cagra' AND i.indisvalid
          AND pg_relation_size(idx, 'main') > 0
    ) THEN
        RAISE EXCEPTION 'expected a valid page-format CAGRA index: %', idx;
    END IF;
END;
$$;

-- The comparison SELECT binds both sides to one snapshot and one invoker.
-- heap_sql is a fixture-authored SELECT of (tid, key) from ONLY the heap.
CREATE FUNCTION assert_knn(
    stage text, api text, h regclass, idx regclass, heap_sql text,
    k integer, allowed tid[] DEFAULT NULL
) RETURNS void LANGUAGE plpgsql VOLATILE SECURITY INVOKER AS $$
DECLARE
    queries vector[] := ARRAY['[0,0,0,0,0,0,0,0]'::vector,
                              '[256.25,0,0,0,0,0,0,0]'::vector];
    heap_plan jsonb;
    actual_sql text;
    mismatch jsonb;
BEGIN
    EXECUTE 'EXPLAIN (FORMAT JSON, COSTS OFF) ' || heap_sql INTO heap_plan;
    IF heap_plan #>> '{0,Plan,Node Type}' IS DISTINCT FROM 'Seq Scan' THEN
        RAISE EXCEPTION '%/%: oracle requires a heap Seq Scan: %', stage, api, heap_plan;
    END IF;

    CASE api
    WHEN 'batch' THEN
        IF allowed IS NOT NULL THEN
            RAISE EXCEPTION 'batch takes no TID filter';
        END IF;
        actual_sql := $actual$
            SELECT s.query_idx::bigint AS q,
                   row_number() OVER (PARTITION BY s.query_idx ORDER BY s.ord) AS pos,
                   s.ctid AS tid, s.distance::double precision AS distance
            FROM public.pg_cuvs_batch_search($2, $3, $4)
                WITH ORDINALITY AS s(query_idx, ctid, distance, ord)
        $actual$;
    WHEN 'tid' THEN
        queries := queries[1:1];
        actual_sql := $actual$
            SELECT 0::bigint AS q, s.ord AS pos, s.ctid AS tid,
                   s.distance::double precision AS distance
            FROM public.cuvs_filtered_knn($1, ($3)[1], $5, $4)
                WITH ORDINALITY AS s(ctid, distance, ord)
        $actual$;
    WHEN 'bigint' THEN
        queries := queries[1:1];
        actual_sql := $actual$
            SELECT 0::bigint AS q, s.ord AS pos, s.ctid AS tid,
                   s.distance::double precision AS distance
            FROM public.cuvs_filtered_knn(
                $1, ($3)[1], CASE WHEN $5 IS NULL THEN NULL::bigint[] ELSE
                    ARRAY(
                        SELECT split_part(btrim(t::text, '()'), ',', 1)::bigint * 65536
                             + split_part(btrim(t::text, '()'), ',', 2)::bigint
                        FROM unnest($5) AS f(t)
                        ORDER BY 1
                    ) END, $4
            ) WITH ORDINALITY AS s(ctid, distance, ord)
        $actual$;
    ELSE
        RAISE EXCEPTION 'unknown API: %', api;
    END CASE;

    -- The fixtures have no distance ties. <-> returns L2, while these APIs
    -- return squared L2. Ordinality records the raw SRF order before any join.
    EXECUTE format($compare$
        WITH heap_rows AS MATERIALIZED (%s),
        scored AS (
            SELECT q.ord - 1 AS q, h.tid, power(h.key <-> q.v, 2) AS distance
            FROM heap_rows h
            CROSS JOIN unnest($3) WITH ORDINALITY AS q(v, ord)
            WHERE h.key IS NOT NULL AND ($5 IS NULL OR h.tid = ANY($5))
        ), ranked AS (
            SELECT q, tid, distance, row_number() OVER (PARTITION BY q ORDER BY distance) AS pos
            FROM scored
        ), expected AS (
            SELECT * FROM ranked WHERE pos <= $4
        ), actual AS MATERIALIZED (%s)
        SELECT jsonb_build_object('expected', to_jsonb(e), 'actual', to_jsonb(a))
        FROM expected e FULL JOIN actual a USING (q, pos)
        WHERE e.pos IS NULL OR a.pos IS NULL OR e.tid IS DISTINCT FROM a.tid
           OR a.distance IS NULL
           OR abs(a.distance - e.distance) > 1e-5 + 1e-6 * abs(e.distance)
        ORDER BY coalesce(e.q, a.q), coalesce(e.pos, a.pos)
        LIMIT 1
    $compare$, heap_sql, actual_sql) INTO mismatch USING idx, h, queries, k, allowed;
    IF mismatch IS NOT NULL THEN
        RAISE EXCEPTION '%/%: raw top-k differs from snapshot heap SQL: %', stage, api, mismatch;
    END IF;
END;
$$;

CREATE TABLE base (
    id integer PRIMARY KEY, v vector(8) NOT NULL, note text NOT NULL DEFAULT 'seed'
) WITH (fillfactor = 60, autovacuum_enabled = false);
INSERT INTO base (id, v)
SELECT i, ARRAY[i::real, 0, 0, 0, 0, 0, 0, 0]::vector
FROM generate_series(1, 256) AS g(i);
CREATE INDEX base_idx ON base USING cagra (v vector_l2_ops);
SELECT assert_page('base_idx');

CREATE FUNCTION assert_base(stage text, api text, k integer, allowed tid[] DEFAULT NULL,
                            h regclass DEFAULT 'base', idx regclass DEFAULT 'base_idx')
RETURNS void LANGUAGE sql VOLATILE SECURITY INVOKER AS $$
    SELECT assert_knn(stage, api, h, idx,
                      format('SELECT ctid AS tid, v AS key FROM ONLY %s', h), k, allowed);
$$;

-- Require actual HOT bits and the root-to-child link before using this fixture.
CREATE FUNCTION assert_hot(h regclass, root tid, child tid) RETURNS void
LANGUAGE plpgsql STABLE AS $$
DECLARE
    root_block bigint := split_part(btrim(root::text, '()'), ',', 1)::bigint;
    child_block bigint := split_part(btrim(child::text, '()'), ',', 1)::bigint;
    root_lp integer := split_part(btrim(root::text, '()'), ',', 2)::integer;
    child_lp integer := split_part(btrim(child::text, '()'), ',', 2)::integer;
BEGIN
    IF root IS NULL OR child IS NULL OR root = child OR root_block <> child_block
       OR (SELECT count(*) FROM public.heap_page_items(public.get_raw_page(h::text, root_block)) p
           CROSS JOIN LATERAL public.heap_tuple_infomask_flags(p.t_infomask, p.t_infomask2) f
           WHERE (p.lp = root_lp AND p.t_ctid = child AND 'HEAP_HOT_UPDATED' = ANY(f.raw_flags))
              OR (p.lp = child_lp AND 'HEAP_ONLY_TUPLE' = ANY(f.raw_flags))) <> 2
    THEN
        RAISE EXCEPTION 'fixture requires a HOT root and child: % -> %', root, child;
    END IF;
END;
$$;

\if :{?u13_setup_only}
COMMIT;
\quit
\endif

SELECT use_bf_oracle_settings();
SELECT assert_base('base multi-query', 'batch', 3);
SELECT assert_base('NULL tid filter', 'tid', 3);
SELECT assert_base('NULL bigint filter', 'bigint', 3);
SELECT assert_base('empty tid filter', 'tid', 3, ARRAY[]::tid[]);
SELECT assert_base('empty bigint filter', 'bigint', 3, ARRAY[]::tid[]);

SELECT ctid AS hot_root FROM base WHERE id = 1 \gset
UPDATE base SET note = 'hot child' WHERE id = 1 RETURNING ctid AS hot_child \gset
SELECT assert_hot('base', :'hot_root', :'hot_child');
SELECT assert_base('HOT', 'batch', 3);
SELECT assert_base('HOT', 'bigint', 3);
SELECT assert_base('HOT old root only', 'tid', 3, ARRAY[:'hot_root'::tid]);
SELECT ctid AS other_tid FROM base WHERE id = 4 \gset
SELECT assert_base('HOT duplicate whitelist and short result', api, 5,
       ARRAY[:'other_tid'::tid, :'hot_root'::tid, :'hot_child'::tid, :'hot_child'::tid, NULL::tid])
FROM (VALUES ('tid'), ('bigint')) AS apis(api);

SAVEPOINT before_delete;
DELETE FROM base WHERE id = 1;
SELECT assert_base('deleted nearest; refill top-k', api, 3)
FROM (VALUES ('batch'), ('tid'), ('bigint')) AS apis(api);
ROLLBACK TO SAVEPOINT before_delete;

INSERT INTO base (id, v) VALUES (0, '[0,0,0,0,0,0,0,0]') RETURNING ctid AS pending_tid \gset
SELECT assert_base('own pending INSERT', api, 3)
FROM (VALUES ('batch'), ('tid'), ('bigint')) AS apis(api);
SELECT assert_base('own pending INSERT with explicit filter', 'tid', 1, ARRAY[:'pending_tid'::tid]);

CREATE TABLE indexed_shapes (
    id integer NOT NULL, v vector(8), eligible boolean NOT NULL
);
INSERT INTO indexed_shapes
SELECT i, CASE WHEN i = -1 THEN NULL ELSE ARRAY[i::real, 0, 0, 0, 0, 0, 0, 0]::vector END,
       i % 2 = 1 OR i = -1
FROM generate_series(-1, 256) AS g(i);
CREATE INDEX shape_idx ON indexed_shapes USING cagra (v vector_l2_ops) WHERE eligible;
SELECT assert_page('shape_idx');
SELECT assert_knn('partial index', api, 'indexed_shapes', 'shape_idx',
                  'SELECT ctid AS tid, v AS key FROM ONLY indexed_shapes WHERE eligible', 3)
FROM (VALUES ('batch'), ('tid'), ('bigint')) AS apis(api);

DROP INDEX shape_idx;
CREATE INDEX shape_idx ON indexed_shapes
    USING cagra (((v + '[10.5,0,0,0,0,0,0,0]'::vector)::vector(8)) vector_l2_ops);
SELECT assert_page('shape_idx');
SELECT assert_knn('expression index', api, 'indexed_shapes', 'shape_idx',
                  $$SELECT ctid AS tid, (v + '[10.5,0,0,0,0,0,0,0]'::vector) AS key
                    FROM ONLY indexed_shapes$$, 3)
FROM (VALUES ('batch'), ('tid'), ('bigint')) AS apis(api);

DROP INDEX shape_idx;
CREATE INDEX shape_idx ON indexed_shapes
    USING cagra (((v + '[10.5,0,0,0,0,0,0,0]'::vector)::vector(8)) vector_l2_ops)
    WHERE eligible;
SELECT assert_page('shape_idx');
SELECT assert_knn('partial expression index', api, 'indexed_shapes', 'shape_idx',
                  $$SELECT ctid AS tid, (v + '[10.5,0,0,0,0,0,0,0]'::vector) AS key
                    FROM ONLY indexed_shapes WHERE eligible$$, 3,
                  CASE WHEN api <> 'batch' THEN ARRAY(SELECT ctid FROM indexed_shapes WHERE id <= 5)
                      ELSE NULL::tid[] END)
FROM (VALUES ('batch'), ('tid'), ('bigint')) AS apis(api);

CREATE ROLE :"u13_schema" NOLOGIN NOSUPERUSER NOBYPASSRLS;
GRANT USAGE ON SCHEMA :"u13_schema" TO :"u13_schema";
GRANT SELECT ON base TO :"u13_schema";
ALTER TABLE base ENABLE ROW LEVEL SECURITY;
CREATE POLICY even_ids ON base FOR SELECT TO :"u13_schema" USING (id % 2 = 0);
SET LOCAL ROLE :"u13_schema";
DO $$
BEGIN
    IF NOT row_security_active('base'::regclass)
       OR (SELECT array_agg(id ORDER BY id) FROM base WHERE id <= 4)
          IS DISTINCT FROM ARRAY[0, 2, 4]
    THEN
        RAISE EXCEPTION 'expected the even-ID SELECT policy to be active';
    END IF;
END;
$$;
SELECT assert_base('RLS even IDs', api, 3)
FROM (VALUES ('batch'), ('tid'), ('bigint')) AS apis(api);
RESET ROLE;
ROLLBACK;
\echo U-13 single-session SQL correctness PASS
