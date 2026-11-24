\set ON_ERROR_STOP on
CREATE EXTENSION vector;
CREATE EXTENSION pg_cuvs;
CREATE EXTENSION pageinspect;

CREATE TABLE page_cic (id int PRIMARY KEY, v vector(3));
INSERT INTO page_cic VALUES (1, '[1,0,0]'), (2, '[2,0,0]');
CREATE INDEX CONCURRENTLY page_cic_idx ON page_cic USING cagra (v vector_l2_ops);
DO $$
BEGIN
    IF get_byte(get_raw_page('page_cic_idx',0), 96) <> 1 THEN
        RAISE EXCEPTION 'concurrent validation did not advance G with an all-false callback';
    END IF;
END $$;

CREATE TABLE page_vac (id int PRIMARY KEY, v vector(3));
INSERT INTO page_vac SELECT i, array_fill(i::real, ARRAY[3])::vector FROM generate_series(1,10) i;
CREATE INDEX page_vac_idx ON page_vac USING cagra(v vector_l2_ops);
INSERT INTO page_vac VALUES (100,'[100,100,100]');
CREATE TEMP TABLE vacuum_target AS SELECT id,ctid AS old_tid FROM page_vac WHERE id IN (2,100);
DELETE FROM page_vac WHERE id IN (2,100);
VACUUM (INDEX_CLEANUP ON,TRUNCATE OFF) page_vac;
DO $$
DECLARE
    metadata bytea := get_raw_page('page_vac_idx', 0);
    tid_page bytea := get_raw_page('page_vac_idx', 1);
    pending bytea := get_raw_page('page_vac_idx',65);
BEGIN
    IF get_byte(metadata, 96) <> 1 OR get_byte(metadata,104) <> 1 THEN
        RAISE EXCEPTION 'vacuum did not publish G and the cumulative invalid slot count';
    END IF;
    IF get_byte(tid_page,8172) <> 1 OR
       encode(substr(tid_page,31,6),'hex') <> 'ffffffff0000' OR
       encode(substr(pending,25,6),'hex') <> 'ffffffff0000' THEN
        RAISE EXCEPTION 'vacuum did not invalidate both kinds of TID';
    END IF;
END $$;
INSERT INTO page_vac VALUES (11,'[0,0,0]');
SET enable_seqscan=off;
DO $$
BEGIN
    IF (SELECT ctid FROM page_vac WHERE id=11) NOT IN (SELECT old_tid FROM vacuum_target) THEN
        RAISE EXCEPTION 'fixture failed to reuse one of the vacuumed TIDs';
    END IF;
    IF (SELECT id FROM page_vac ORDER BY v <-> '[0,0,0]' LIMIT 1) IS DISTINCT FROM 11 THEN
        RAISE EXCEPTION 'vacuum and TID reuse changed the heap answer';
    END IF;
END $$;
