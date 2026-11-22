# build_identity_cross.spec — another session's rolled-back rebuild must not answer for this session
#
# s1 searches the index first, so its relcache holds "the build is current" (a rolled-back rebuild in another session sends this
# backend no invalidation). s2 then truncates, refills in reverse order, rebuilds and rolls back: the catalog's index is the old one
# again, while the daemon holds s2's rolled-back build, whose TIDs name other rows. s1's next search is planned with the index; the
# daemon must refuse it (55000, never the rolled-back build's rows), and the statement after that must answer like the heap.
# In the runs on the CPU reference build (PostgreSQL 16) s1's relcache entry of the index was rebuilt before its next plan, also
# when s2 only rolled back a REINDEX, so the planner read the record again and took the heap (last_reason not_current) before the
# daemon was asked; the expected output is that. What must never appear is a row of s2's rolled-back build. The daemon's own check is
# proven in test/unit/test_build_identity_daemon.c, since this spec does not reach it.

setup
{
    CREATE EXTENSION IF NOT EXISTS vector;
    CREATE EXTENSION IF NOT EXISTS pg_cuvs;
    SET cuvs.index_dir = '/tmp/cuvs_indexes';
    CREATE TABLE bic (id int, v vector(4));
    INSERT INTO bic SELECT g, ARRAY[g,0,0,0]::real[]::vector FROM generate_series(1, 30) g;
    CREATE INDEX bic_i ON bic USING cagra (v vector_l2_ops);
}

teardown
{
    DROP TABLE bic;
}

session s1
setup       { SET cuvs.index_dir = '/tmp/cuvs_indexes'; SET enable_seqscan = off; SET cuvs.k = 8; }
step s1_prepare { SET plan_cache_mode = force_generic_plan; PREPARE q AS SELECT string_agg(id::text, ',' ORDER BY d, id) AS ids FROM (SELECT id, v <-> '[1,0,0,0]' d FROM bic ORDER BY v <-> '[1,0,0,0]' LIMIT 5) s; }
step s1_execute { EXECUTE q; }
step s1_search { SELECT string_agg(id::text, ',' ORDER BY d, id) AS ids FROM (SELECT id, v <-> '[1,0,0,0]' d FROM bic ORDER BY v <-> '[1,0,0,0]' LIMIT 5) s; }
# which path answered the last search: the planner's (not_current) or none (the index, or the daemon's refusal raised above)
step s1_reason { SELECT coalesce((SELECT last_reason FROM pg_stat_gpu_fallback WHERE index_oid = 'bic_i'::regclass), 'none') AS last_reason; }

session s2
# the rebuild's NOTICE (old copy unloaded to make room) depends on the free memory other tests left
setup       { SET cuvs.index_dir = '/tmp/cuvs_indexes'; SET client_min_messages = warning; }
step s2_rebuild_rollback { BEGIN; TRUNCATE bic; INSERT INTO bic SELECT 1000+g, ARRAY[31-g,0,0,0]::real[]::vector FROM generate_series(1, 30) g; REINDEX INDEX bic_i; ROLLBACK; }
step s2_reindex_rollback { BEGIN; REINDEX INDEX bic_i; ROLLBACK; }

permutation s1_search s2_rebuild_rollback s1_search s1_reason s1_search s1_reason
permutation s1_search s2_reindex_rollback s1_search s1_reason s1_search s1_reason
# a generic plan prepared before the other session's rollback: must not fail on every EXECUTE
permutation s1_prepare s1_execute s2_rebuild_rollback s1_execute s1_execute s1_execute s1_reason
