# delta_tombstone_snapshot.spec — concurrent DELETE visibility through the
# GPU+delta path (Phase 3A-4). pg_regress is single-session and cannot express
# this; the isolation tester runs the snapshots concurrently against one live
# daemon.
#
# Property under test: a DELETE that commits in one session must be filtered for
# a NEW snapshot (heap recheck / tombstone correction drops the dead TID) but
# must remain visible to an OLDER REPEATABLE READ snapshot that began before the
# delete committed. This locks MVCC correctness through the base-CAGRA + delta +
# tombstone candidate pipeline under real concurrency.
#
# Determinism: id 42 sits at the unique far extremum '[9,9,9,9]'; a probe equal
# to it has distance 0, so it is the unique nearest IFF still visible — the
# returned id is deterministic regardless of CAGRA approximation. enable_seqscan
# is off so the GPU index path (not a CPU seqscan) serves the probe.
#
# Note: the snapshot-PROTECTIVE branch of the tombstone delete_xid compare
# (XidInMVCCSnapshot == true) is unreachable here by construction — VACUUM only
# writes a tombstone once OldestXmin has passed the delete, so no older snapshot
# can coexist with the tombstone. Heap recheck is the load-bearing filter while a
# pre-delete snapshot is open; this spec verifies that pipeline.

setup
{
    SET cuvs.index_dir = '/tmp/cuvs_indexes';
    CREATE EXTENSION IF NOT EXISTS vector;
    CREATE EXTENSION IF NOT EXISTS pg_cuvs;
    CREATE TABLE iso (id bigint, embedding vector(4));
    INSERT INTO iso SELECT g, ('['||g||',0,0,0]')::vector FROM generate_series(1,20) g;
    INSERT INTO iso VALUES (42, '[9,9,9,9]');
    CREATE INDEX iso_cagra ON iso USING cagra (embedding vector_l2_ops);
}

teardown
{
    DROP TABLE iso;
    DROP EXTENSION pg_cuvs;
}

session s1
setup     { SET cuvs.index_dir = '/tmp/cuvs_indexes'; SET enable_seqscan = off;
            BEGIN ISOLATION LEVEL REPEATABLE READ; }
# First query fixes s1's snapshot (before s2's DELETE commits): sees id 42.
step s1_snap  { SELECT id FROM iso ORDER BY embedding <-> '[9,9,9,9]'::vector LIMIT 1; }
# Same snapshot after the concurrent DELETE+VACUUM: id 42 must still be visible.
step s1_again { SELECT id FROM iso ORDER BY embedding <-> '[9,9,9,9]'::vector LIMIT 1; }
step s1_commit { COMMIT; }

session s2
setup     { SET cuvs.index_dir = '/tmp/cuvs_indexes'; }
step s2_del { DELETE FROM iso WHERE id = 42; }
step s2_vac { VACUUM iso; }

session s3
setup     { SET cuvs.index_dir = '/tmp/cuvs_indexes'; SET enable_seqscan = off; }
# Fresh snapshot taken after the DELETE committed: id 42 must NOT come back.
step s3_read { SELECT id FROM iso ORDER BY embedding <-> '[9,9,9,9]'::vector LIMIT 1; }

permutation s1_snap s2_del s2_vac s1_again s1_commit s3_read
