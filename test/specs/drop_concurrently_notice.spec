# drop_concurrently_notice.spec — DROP INDEX CONCURRENTLY must not reach the daemon before the index is gone
#
# DROP INDEX CONCURRENTLY commits several times. As PostgreSQL 16 does it (index_drop with concurrent = true):
#   tx 1  the object-access hook (OAT_DROP) runs for the index (deleteObjectsInList calls the drop hooks before the deletions);
#         index_drop clears indisvalid, invalidates the table's relcache and commits (CommitTransactionCommand), so new plans stop
#         using the index;
#   tx 2  WaitForLockers on the table: waits for every transaction that holds a lock on it, then clears indisready and commits;
#   tx 3  WaitForLockers again, then the index's storage and its pg_class/pg_index rows are deleted; this transaction commits at the
#         end of the statement.
# pg_cuvs queued the daemon's DROP at the OAT_DROP hook and sent it at the first COMMIT, the end of tx 1. While DIC waits in tx 2 for
# an older transaction that may still be scanning the index, the daemon had already dropped its entry and files (this spec fails that
# way without the fix). The notice must go only at the commit that deletes the index's pg_class row.
#
# s1 holds a lock on the table in an open transaction after an index scan; s2 runs DROP INDEX CONCURRENTLY and waits for s1; s3 looks
# at the daemon's entry (pg_cuvs_index_status) and the index's files in cuvs.index_dir while s2 waits (both must still be there), and
# again after s1 commits and s2 completes (both gone). One permutation per access method (cagra, flat, ivfpq).
# The cagra and flat tables have 30 rows: installcheck runs this after installcheck-tier1 on the same daemon, whose admission_reject
# test leaves a build limit of about 140 kB (a 300-row CAGRA build needs about 860 kB and was refused AR0 in setup).

setup
{
    CREATE EXTENSION IF NOT EXISTS vector;
    CREATE EXTENSION IF NOT EXISTS pg_cuvs;
    SET cuvs.index_dir = '/tmp/cuvs_indexes';
    CREATE TABLE dic_c (id int, v vector(4));
    CREATE TABLE dic_f (id int, v vector(4));
    CREATE TABLE dic_q (id int, v vector(4));
    INSERT INTO dic_c SELECT g, ARRAY[g % 17, g % 13, g % 11, g % 7]::real[]::vector FROM generate_series(1, 30) g;
    INSERT INTO dic_f SELECT * FROM dic_c;
    INSERT INTO dic_q SELECT g, ARRAY[g % 17, g % 13, g % 11, g % 7]::real[]::vector FROM generate_series(1, 300) g;
    CREATE INDEX dic_c_i ON dic_c USING cagra (v vector_l2_ops);
    CREATE INDEX dic_f_i ON dic_f USING flat (v vector_l2_ops);
    CREATE INDEX dic_q_i ON dic_q USING ivfpq (v vector_l2_ops) WITH (n_lists = 4, pq_bits = 8, pq_dim = 2);
    CREATE TABLE dic_oid AS SELECT relname::text AS name, oid FROM pg_class WHERE relname IN ('dic_c_i', 'dic_f_i', 'dic_q_i');
}

teardown
{
    DROP TABLE dic_c, dic_f, dic_q, dic_oid;
}

session s1
setup       { SET cuvs.index_dir = '/tmp/cuvs_indexes'; SET enable_seqscan = off; SET cuvs.ivfpq_n_probes = 4; }
step s1_c   { BEGIN; SELECT count(*) = 1 AS searched FROM (SELECT id FROM dic_c ORDER BY v <-> '[1,0,0,0]'::vector LIMIT 1) s; }
step s1_f   { BEGIN; SELECT count(*) = 1 AS searched FROM (SELECT id FROM dic_f ORDER BY v <-> '[1,0,0,0]'::vector LIMIT 1) s; }
step s1_q   { BEGIN; SELECT count(*) = 1 AS searched FROM (SELECT id FROM dic_q ORDER BY v <-> '[1,0,0,0]'::vector LIMIT 1) s; }
step s1_commit { COMMIT; }

session s2
setup       { SET cuvs.index_dir = '/tmp/cuvs_indexes'; }
step s2_c   { DROP INDEX CONCURRENTLY dic_c_i; }
step s2_f   { DROP INDEX CONCURRENTLY dic_f_i; }
step s2_q   { DROP INDEX CONCURRENTLY dic_q_i; }

session s3
setup       { SET cuvs.index_dir = '/tmp/cuvs_indexes'; }
step s3_c   { SELECT (SELECT count(*) FROM pg_cuvs_index_status() WHERE index_oid = o.oid) AS status_rows,
                     (SELECT count(*) FROM pg_ls_dir(current_setting('cuvs.index_dir')) f
                       WHERE starts_with(f, format('%s_%s.', (SELECT oid FROM pg_database WHERE datname = current_database()), o.oid))) > 0 AS has_files
              FROM dic_oid o WHERE name = 'dic_c_i'; }
step s3_f   { SELECT (SELECT count(*) FROM pg_cuvs_index_status() WHERE index_oid = o.oid) AS status_rows,
                     (SELECT count(*) FROM pg_ls_dir(current_setting('cuvs.index_dir')) f
                       WHERE starts_with(f, format('%s_%s.', (SELECT oid FROM pg_database WHERE datname = current_database()), o.oid))) > 0 AS has_files
              FROM dic_oid o WHERE name = 'dic_f_i'; }
step s3_q   { SELECT (SELECT count(*) FROM pg_cuvs_index_status() WHERE index_oid = o.oid) AS status_rows,
                     (SELECT count(*) FROM pg_ls_dir(current_setting('cuvs.index_dir')) f
                       WHERE starts_with(f, format('%s_%s.', (SELECT oid FROM pg_database WHERE datname = current_database()), o.oid))) > 0 AS has_files
              FROM dic_oid o WHERE name = 'dic_q_i'; }

permutation s1_c s2_c s3_c s1_commit s3_c
permutation s1_f s2_f s3_f s1_commit s3_f
permutation s1_q s2_q s3_q s1_commit s3_q
