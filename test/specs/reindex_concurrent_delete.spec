# reindex_concurrent_delete.spec — REINDEX INDEX CONCURRENTLY + concurrent DELETE 정합성 (ADR-050)
#
# 검증 속성: REINDEX CONCURRENTLY 전/후에 DELETE+VACUUM+compact가 발생해도
# 삭제된 벡터가 검색 결과에 노출(ghost)되지 않아야 한다.
#
# REINDEX CONCURRENTLY는 ShareUpdateExclusiveLock만 요구해 DML을 블록하지 않는다.
# 따라서 두 permutation으로 각 경로를 검증한다:
#   perm A: DELETE+compact 후 REINDEX → 새 인덱스 빌드 시 dead row 제외
#   perm B: REINDEX 후 DELETE+compact → tombstone 필터 + heap recheck 경로
#
# 결정론: id=99는 '[99,0,0,0]'에 위치해 base cluster(거리 ~1-20)와 충분히 멀다.
# REINDEX 후 최근접 이웃 쿼리는 id=99를 제외하면 항상 base cluster를 반환한다.

setup
{
    SET cuvs.index_dir = '/tmp/cuvs_indexes';
    CREATE EXTENSION IF NOT EXISTS vector;
    CREATE EXTENSION IF NOT EXISTS pg_cuvs;
    CREATE TABLE iso3 (id bigint, embedding vector(4));
    INSERT INTO iso3 SELECT g, ('['||g||',0,0,0]')::vector FROM generate_series(1,20) g;
    INSERT INTO iso3 VALUES (99, '[99,0,0,0]');
    CREATE INDEX iso3_cagra ON iso3 USING cagra (embedding vector_l2_ops);
}

teardown
{
    DROP TABLE iso3;
    DROP EXTENSION pg_cuvs;
}

session s1
setup     { SET cuvs.index_dir = '/tmp/cuvs_indexes'; }
step s1_del     { DELETE FROM iso3 WHERE id = 99; }
step s1_vac     { VACUUM iso3; }
step s1_compact { SELECT pg_cuvs_compact('iso3_cagra'::regclass); }
step s1_reindex { REINDEX INDEX CONCURRENTLY iso3_cagra; }

session s3
setup     { SET cuvs.index_dir = '/tmp/cuvs_indexes'; SET enable_seqscan = off; }
# id=99가 삭제된 후 쿼리 → base cluster(id 1-20) 중 최근접 반환; id=99 ghost이면 실패
step s3_read { SELECT id <> 99 AS no_ghost
               FROM iso3 ORDER BY embedding <-> '[99,0,0,0]'::vector LIMIT 1; }

# perm A: DELETE+compact 선행 → REINDEX가 dead row를 빌드에서 제외
permutation s1_del s1_vac s1_compact s1_reindex s3_read

# perm B: REINDEX 선행 → DELETE+compact 후 tombstone 필터 경로
permutation s1_reindex s1_del s1_vac s1_compact s3_read
