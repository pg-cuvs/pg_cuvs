\set ON_ERROR_STOP on
CREATE EXTENSION vector;
CREATE EXTENSION pg_cuvs;

-- Use a dimension that exercises a padded device dataset and values that
-- remain exact in both precisions. Run with the preseeded BF fixture off.
CREATE TABLE page_bf_rows(id int PRIMARY KEY, v vector(7), note text)
    WITH (fillfactor=60);
INSERT INTO page_bf_rows
SELECT i, ARRAY[i::real, 0, 0, 0, 0, 0, 0]::vector, 'seed'
FROM generate_series(1,256) AS i;
CREATE INDEX page_bf_idx ON page_bf_rows USING cagra (v vector_l2_ops);
CREATE TEMP TABLE old_page_bf_tid AS SELECT ctid AS root FROM page_bf_rows WHERE id=1;
UPDATE page_bf_rows SET note='hot' WHERE id=1;
SELECT pg_stat_force_next_flush();

BEGIN ISOLATION LEVEL REPEATABLE READ;
SET LOCAL enable_indexscan = off;
SET LOCAL enable_bitmapscan = off;
DO $$
DECLARE
    q vector := '[0,0,0,0,0,0,0]';
    root tid := (SELECT root FROM old_page_bf_tid);
    child tid := (SELECT ctid FROM page_bf_rows WHERE id=1);
    filter tid[];
    expected_tids tid[];
    expected_dists real[];
    actual_tids tid[];
    actual_dists real[];
    precision text;
    before_hits bigint;
    after_hits bigint;
    before_bytes bigint;
    after_bytes bigint;
    before_scans bigint;
    after_scans bigint;
    target_bytes bigint;
    deadline timestamptz;
    round_no int;
BEGIN
    IF (SELECT n_tup_hot_upd FROM pg_stat_user_tables WHERE relid='page_bf_rows'::regclass)
            IS DISTINCT FROM 1 OR child IS NOT DISTINCT FROM root THEN
        RAISE EXCEPTION 'expected a HOT child';
    END IF;
    SELECT array_agg(ctid ORDER BY ctid) INTO filter
    FROM ONLY page_bf_rows WHERE id IN (1,4,7,8);
    filter := filter || ARRAY[root, child, NULL::tid];
    SELECT array_agg(tid ORDER BY distance), array_agg(distance ORDER BY distance)
    INTO expected_tids, expected_dists
    FROM (
        SELECT ctid AS tid, power(v <-> q, 2)::real AS distance
        FROM ONLY page_bf_rows
        WHERE ctid = ANY(filter)
        ORDER BY distance LIMIT 2
    ) AS oracle;
    IF expected_tids[1] IS DISTINCT FROM child OR expected_dists <> ARRAY[1,16]::real[] THEN
        RAISE EXCEPTION 'unexpected heap oracle: %, %', expected_tids, expected_dists;
    END IF;

    FOR round_no IN 1..3 LOOP
        precision := CASE WHEN round_no = 2 THEN 'float16' ELSE 'float32' END;
        PERFORM set_config('cuvs.bf_precision', precision, true);
        SELECT hits, vram_bytes INTO STRICT before_hits, before_bytes
        FROM pg_cuvs_index_status()
        WHERE index_oid='page_bf_idx'::regclass AND kind='page_cagra' AND state='resident';
        IF round_no <= 2 THEN
            SELECT seq_scan INTO STRICT before_scans FROM pg_stat_xact_user_tables
            WHERE relid='page_bf_rows'::regclass;
            SELECT array_agg(s.ctid ORDER BY ordinality), array_agg(distance ORDER BY ordinality)
            INTO actual_tids, actual_dists
            FROM cuvs_filtered_knn('page_bf_idx', q, filter, 2) WITH ORDINALITY AS s;
            SELECT hits INTO STRICT after_hits FROM pg_cuvs_index_status()
            WHERE index_oid='page_bf_idx'::regclass AND kind='page_cagra' AND state='resident';
            SELECT seq_scan INTO STRICT after_scans FROM pg_stat_xact_user_tables
            WHERE relid='page_bf_rows'::regclass;
            IF after_hits <> before_hits OR after_scans <= before_scans OR
                actual_tids IS DISTINCT FROM expected_tids OR actual_dists IS DISTINCT FROM expected_dists THEN
                RAISE EXCEPTION '% cold request did not return the heap oracle', precision;
            END IF;

            target_bytes := before_bytes + 256 * (7 * CASE WHEN round_no=1 THEN 4 ELSE 2 END + 4);
            deadline := clock_timestamp() + interval '10 seconds';
            LOOP
                SELECT vram_bytes INTO STRICT after_bytes FROM pg_cuvs_index_status()
                WHERE index_oid='page_bf_idx'::regclass AND kind='page_cagra' AND state='resident';
                EXIT WHEN after_bytes = target_bytes;
                IF clock_timestamp() >= deadline THEN
                    RAISE EXCEPTION '% BF reserve did not become resident: got %, expected %',
                        precision, after_bytes, target_bytes;
                END IF;
                -- A busy daemon may defer the first admission attempt.
                PERFORM count(*) FROM cuvs_filtered_knn('page_bf_idx', q, filter, 2);
                PERFORM pg_sleep(0.02);
            END LOOP;
        END IF;

        SELECT hits INTO STRICT before_hits FROM pg_cuvs_index_status()
        WHERE index_oid='page_bf_idx'::regclass AND kind='page_cagra' AND state='resident';
        SELECT seq_scan INTO STRICT before_scans FROM pg_stat_xact_user_tables
        WHERE relid='page_bf_rows'::regclass;
        SELECT array_agg(s.ctid ORDER BY ordinality), array_agg(distance ORDER BY ordinality)
        INTO actual_tids, actual_dists
        FROM cuvs_filtered_knn('page_bf_idx', q, filter, 2) WITH ORDINALITY AS s;
        SELECT hits INTO STRICT after_hits FROM pg_cuvs_index_status()
        WHERE index_oid='page_bf_idx'::regclass AND kind='page_cagra' AND state='resident';
        SELECT seq_scan INTO STRICT after_scans FROM pg_stat_xact_user_tables
        WHERE relid='page_bf_rows'::regclass;
        IF after_hits <> before_hits + 1 OR after_scans <> before_scans OR
            actual_tids IS DISTINCT FROM expected_tids OR actual_dists IS DISTINCT FROM expected_dists THEN
            RAISE EXCEPTION '% warm request did not return the heap oracle via BF', precision;
        END IF;
    END LOOP;
    IF (SELECT pending_bytes FROM pg_cuvs_memory_status() WHERE gpu=0) IS DISTINCT FROM 0 THEN
        RAISE EXCEPTION 'a completed page BF copy left a pending reservation';
    END IF;
END $$;
COMMIT;
