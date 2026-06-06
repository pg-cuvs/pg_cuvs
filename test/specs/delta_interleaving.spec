# delta_interleaving.spec — cross-session pending-delta visibility (Phase 3A-1).
# The `.delta` append is NON-transactional (it is a derived sidecar, not WAL), so
# an INSERT's vector lands in `.delta` immediately — even before the inserting
# transaction commits. Correctness therefore relies on heap recheck / MVCC to
# hide the row from other snapshots until commit. edge_cases.sql checks the
# single-session ROLLBACK case; this spec checks the CONCURRENT case: an
# uncommitted INSERT in s1 must be invisible to s2, then visible once s1 commits.
#
# Determinism: id 200 sits at the unique extremum '[7,7,7,7]'; a probe equal to
# it is the unique nearest IFF visible, so the returned id (or empty) is
# deterministic regardless of CAGRA approximation.

setup
{
    SET cuvs.index_dir = '/tmp/cuvs_indexes';
    CREATE EXTENSION IF NOT EXISTS vector;
    CREATE EXTENSION IF NOT EXISTS pg_cuvs;
    CREATE TABLE iso2 (id bigint, embedding vector(4));
    INSERT INTO iso2 SELECT g, ('['||g||',0,0,0]')::vector FROM generate_series(1,20) g;
    CREATE INDEX iso2_cagra ON iso2 USING cagra (embedding vector_l2_ops);
}

teardown
{
    DROP TABLE iso2;
    DROP EXTENSION pg_cuvs;
}

session w
setup      { SET cuvs.index_dir = '/tmp/cuvs_indexes'; }
step w_begin   { BEGIN; }
# The INSERT appends to .delta immediately (non-transactional sidecar).
step w_insert  { INSERT INTO iso2 VALUES (200, '[7,7,7,7]'); }
step w_commit  { COMMIT; }

session r
setup      { SET cuvs.index_dir = '/tmp/cuvs_indexes'; SET enable_seqscan = off; }
# While w's INSERT is uncommitted: the probe must NOT return id 200 (MVCC hides it
# even though the vector is already in .delta).
step r_pre  { SELECT count(*) AS sees_200
              FROM (SELECT id FROM iso2 ORDER BY embedding <-> '[7,7,7,7]'::vector LIMIT 1) s
              WHERE id = 200; }
# After w commits: the probe must return id 200 via the merge.
step r_post { SELECT id FROM iso2 ORDER BY embedding <-> '[7,7,7,7]'::vector LIMIT 1; }

permutation w_begin w_insert r_pre w_commit r_post
