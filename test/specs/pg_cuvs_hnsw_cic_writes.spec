# pg_cuvs_hnsw_cic_writes.spec: CREATE INDEX CONCURRENTLY ... USING pg_cuvs_hnsw (a heap build, no source) does not make
# the table's writers wait while it writes the index pages. GPU only: the CPU shim cannot build an HNSW graph from the heap.
#
# PostgreSQL's concurrent build opens the new index with RowExclusiveLock (index_concurrently_build), and every write
# statement on the table opens every index of the table, a not-yet-ready one too, with RowExclusiveLock (ExecOpenIndices).
# A plain INSERT releases those index locks when its statement ends (ExecCloseIndices; seen in pg_locks), so here it is a
# running INSERT that holds them. UPDATE, DELETE and INSERT ... ON CONFLICT keep the locks the planner took on the
# indexes to the end of their transaction (get_relation_info), and a SELECT keeps AccessShareLock on them the same way:
# an index build that takes AccessExclusiveLock on the new index while it writes the pages waits for all of these and
# queues every query that starts behind it. This spec checks the INSERT case only.
#
# How the steps meet at that moment:
#   l_lock     holds two advisory locks: 54541, which the index expression waits for, so the build stops in its heap scan
#              (phase 2, after the catalog row of phase 1 is committed), and 54542, which b's second row waits for;
#   l_phase    shows that the build waits in its heap scan (phase 2), not before it: a writer that began before phase 2
#              would only meet PostgreSQL's wait for writers before the build, and the page writing would find no writer;
#   b_insert   a write statement started while the build waits: it inserts its first row (opening the indexes) and waits
#              for 54542 while computing its second row, so it holds the new index's RowExclusiveLock until l_release;
#   l_unlock   lets the build go on; l_wait_cic waits until the build waits for a lock again and shows which: before the fix
#              the new index's AccessExclusiveLock (it waits for b), with the fix b's transaction itself (virtualxid), which is
#              PostgreSQL's own wait for writers before the validation phase;
#   c_insert   a new writer: before the fix it waits behind the build's AccessExclusiveLock (shown as <waiting ...>), with
#              the fix it finishes at once;
#   r_status_during, r_counts_during  while the build waits, the functions that read an index's pages leave the index that
#              is not ready alone: pg_cuvs_hnsw_layer_status() does not list it, pg_cuvs_hnsw_layer_counts() refuses it;
#   l_release  lets b finish;
#   r_found    after the build, the rows of b and c are found through the new index, which is valid.

setup
{
    SET cuvs.index_dir = '/tmp/cuvs_indexes';
    CREATE EXTENSION IF NOT EXISTS vector;
    CREATE EXTENSION IF NOT EXISTS pg_cuvs;
    CREATE TABLE cicw (id bigint, v vector(4));
    INSERT INTO cicw SELECT g, ('[' || g || ',' || (g % 7) || ',' || (g % 11) || ',1]')::vector FROM generate_series(1, 2000) g;
    CREATE FUNCTION cicw_gate(x vector) RETURNS vector LANGUAGE plpgsql IMMUTABLE
        AS $$ BEGIN PERFORM pg_advisory_xact_lock_shared(54541); RETURN x; END $$;
    CREATE FUNCTION cicw_hold(x vector) RETURNS vector LANGUAGE plpgsql VOLATILE
        AS $$ BEGIN PERFORM pg_advisory_xact_lock_shared(54542); RETURN x; END $$;
}

teardown
{
    DROP TABLE cicw;
    DROP FUNCTION cicw_gate(vector);
    DROP FUNCTION cicw_hold(vector);
    DROP EXTENSION pg_cuvs;
}

session l
setup       { SET cuvs.index_dir = '/tmp/cuvs_indexes'; }
step l_lock { SELECT pg_advisory_lock(54541), pg_advisory_lock(54542); }
step l_unlock { SELECT pg_advisory_unlock(54541) AS unlocked; }
step l_phase
{
    DO $$ BEGIN
        FOR i IN 1 .. 1200 LOOP
            EXIT WHEN EXISTS (SELECT 1 FROM pg_stat_progress_create_index WHERE relid = 'cicw'::regclass AND phase LIKE 'building index%');
            PERFORM pg_sleep(0.05);
        END LOOP;
    END $$;
    SELECT command, phase FROM pg_stat_progress_create_index WHERE relid = 'cicw'::regclass;
}
step l_release { SELECT pg_advisory_unlock(54542) AS released; }
step l_wait_cic
{
    DO $$ BEGIN
        FOR i IN 1 .. 1200 LOOP
            EXIT WHEN EXISTS (SELECT 1 FROM pg_locks k JOIN pg_stat_activity a USING (pid)
                              WHERE NOT k.granted AND k.locktype <> 'advisory' AND a.query LIKE 'CREATE INDEX CONCURRENTLY cicw_i%');
            PERFORM pg_sleep(0.05);
        END LOOP;
    END $$;
    SELECT k.locktype, k.mode FROM pg_locks k JOIN pg_stat_activity a USING (pid)
    WHERE NOT k.granted AND a.query LIKE 'CREATE INDEX CONCURRENTLY cicw_i%';
}

session a
setup       { SET cuvs.index_dir = '/tmp/cuvs_indexes'; SET client_min_messages = warning; }
step a_cic  { CREATE INDEX CONCURRENTLY cicw_i ON cicw USING pg_cuvs_hnsw ((cicw_gate(v)::vector(4)) vector_l2_ops); }

session b
setup       { SET cuvs.index_dir = '/tmp/cuvs_indexes'; }
step b_insert
{
    INSERT INTO cicw SELECT 5000 + 2 * g - 1, CASE WHEN g = 1 THEN '[5001,1,1,1]'::vector ELSE cicw_hold('[5003,3,3,3]') END
    FROM generate_series(1, 2) g;
}

session c
setup       { SET cuvs.index_dir = '/tmp/cuvs_indexes'; }
step c_insert { INSERT INTO cicw VALUES (5002, '[5002,2,2,2]'); }

session r
setup       { SET cuvs.index_dir = '/tmp/cuvs_indexes'; SET enable_seqscan = off; }
step r_status_during { SELECT count(*) AS listed FROM pg_cuvs_hnsw_layer_status() WHERE index = 'cicw_i'::regclass; }
step r_counts_during { SELECT elements FROM pg_cuvs_hnsw_layer_counts('cicw_i'); }
step r_found
{
    DO $$ BEGIN
        FOR i IN 1 .. 1200 LOOP
            EXIT WHEN (SELECT indisvalid FROM pg_index WHERE indexrelid = 'cicw_i'::regclass);
            PERFORM pg_sleep(0.05);
        END LOOP;
    END $$;
    SELECT (SELECT id FROM cicw ORDER BY cicw_gate(v)::vector(4) <-> '[5001,1,1,1]'::vector(4) LIMIT 1) = 5001 AND
           (SELECT id FROM cicw ORDER BY cicw_gate(v)::vector(4) <-> '[5003,3,3,3]'::vector(4) LIMIT 1) = 5003 AS b_found,
           (SELECT id FROM cicw ORDER BY cicw_gate(v)::vector(4) <-> '[5002,2,2,2]'::vector(4) LIMIT 1) = 5002 AS c_found,
           (SELECT indisvalid FROM pg_index WHERE indexrelid = 'cicw_i'::regclass) AS valid;
}

permutation l_lock a_cic l_phase b_insert r_status_during r_counts_during l_unlock l_wait_cic c_insert l_release r_found
