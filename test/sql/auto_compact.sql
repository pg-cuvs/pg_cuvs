-- auto_compact.sql — Phase 4C: extend_count/compact_count/last_compact_at 갱신 e2e 검증
--
-- DELETE + VACUUM는 ambulkdelete(tombstone 생성) →
-- amvacuumcleanup(handle_compact 자동 호출) 경로를 통해
-- compact_count++ / n_extended=0 / last_compact_at 갱신을 트리거한다.
-- bgworker는 extend_count/n_vecs 임계값을 보고 REINDEX를 트리거하며,
-- REINDEX 후 새 OID에서 extend_count=0/compact_count=0을 별도로 검증한다.

CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- ----------------------------------------------------------------
-- Setup: id=999(far) + id 1-50 초기 build; 이후 EXTEND 검증용 행 추가
-- ----------------------------------------------------------------
CREATE TABLE ac (id bigint, v vector(4));
INSERT INTO ac VALUES (999, '[100,100,0,0]');
INSERT INTO ac SELECT g, ('['||g||',0,0,0]')::vector FROM generate_series(1,50) g;
CREATE INDEX ac_cagra ON ac USING cagra (v vector_l2_ops);

-- ----------------------------------------------------------------
-- Test 1: EXTEND 후 extend_count > 0
-- ----------------------------------------------------------------
INSERT INTO ac SELECT g, ('['||g||',1,0,0]')::vector FROM generate_series(51,60) g;
SELECT extend_count > 0 AS extend_positive
FROM pg_stat_gpu_search
WHERE index_oid = 'ac_cagra'::regclass;

-- ----------------------------------------------------------------
-- Test 2: DELETE + VACUUM → amvacuumcleanup auto-compact
-- compact_count = 1, extend_count = 0, last_compact_at IS NOT NULL
-- ----------------------------------------------------------------
DELETE FROM ac WHERE id = 999;
VACUUM ac;
SELECT compact_count = 1 AS compact_incremented,
       extend_count  = 0 AS extend_reset,
       last_compact_at IS NOT NULL AS last_compact_set
FROM pg_stat_gpu_search
WHERE index_oid = 'ac_cagra'::regclass;

-- ----------------------------------------------------------------
-- Test 3: 두 번째 DELETE + VACUUM → compact_count = 2
-- ----------------------------------------------------------------
DELETE FROM ac WHERE id = 1;
VACUUM ac;
SELECT compact_count = 2 AS compact_count_increments
FROM pg_stat_gpu_search
WHERE index_oid = 'ac_cagra'::regclass;

-- ----------------------------------------------------------------
-- Test 4: REINDEX INDEX CONCURRENTLY → 새 OID에서 extend_count = 0,
-- compact_count = 0 (fresh build)
-- bgworker가 REINDEX 후 새 OID의 임계값부터 다시 추적함을 확인.
-- ----------------------------------------------------------------
INSERT INTO ac SELECT g, ('['||g||',2,0,0]')::vector FROM generate_series(61,65) g;
REINDEX INDEX CONCURRENTLY ac_cagra;
SELECT extend_count = 0 AS extend_reset_after_reindex,
       compact_count = 0 AS fresh_compact_count_after_reindex
FROM pg_stat_gpu_search
WHERE index_oid = 'ac_cagra'::regclass;

-- ----------------------------------------------------------------
-- Teardown
-- ----------------------------------------------------------------
DROP TABLE ac;
