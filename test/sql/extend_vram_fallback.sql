-- extend_vram_fallback.sql — VRAM budget 초과 시 EXTEND→delta fallback 검증
--
-- estimate_vram_bytes(50, 4) = 50 × (4×4 + 16×4) = 4,000 bytes.
-- budget = 4,000 bytes로 고정하면 10벡터 EXTEND delta (800 bytes) 가 예산을
-- 초과해 BUILD_FAILED → cuvs_aminsert delta fallback 경로가 활성화된다.
-- 이 테스트는 handle_extend의 budget 체크와 백엔드 delta fallback의 정합성을
-- end-to-end로 검증한다.

CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- ----------------------------------------------------------------
-- Setup: 50벡터 초기 build (id=999 는 far point로 검색 앵커 역할)
-- ----------------------------------------------------------------
CREATE TABLE vf (id bigint, v vector(4));
INSERT INTO vf VALUES (999, '[100,100,0,0]');
INSERT INTO vf SELECT g, ('['||g||',0,0,0]')::vector FROM generate_series(1,50) g;
CREATE INDEX vf_cagra ON vf USING cagra (v vector_l2_ops);

-- ----------------------------------------------------------------
-- VRAM budget을 현재 인덱스 사용량과 동일하게 설정 (extend delta 불가)
-- ----------------------------------------------------------------
SELECT pg_cuvs_set_vram_budget(vram_bytes)
FROM pg_stat_gpu_search
WHERE index_oid = 'vf_cagra'::regclass;

-- ----------------------------------------------------------------
-- Test 1: EXTEND 실패 → delta fallback
-- INSERT 10벡터 → extend_count=0 (budget 차단), delta_rows>0
-- ----------------------------------------------------------------
INSERT INTO vf SELECT g, ('['||g||',1,0,0]')::vector FROM generate_series(51,60) g;

SELECT extend_count = 0 AS extend_blocked,
       delta_rows > 0   AS fell_to_delta
FROM pg_stat_gpu_search
WHERE index_oid = 'vf_cagra'::regclass;

-- ----------------------------------------------------------------
-- Test 2: 검색 정합성 — delta 벡터가 검색 결과에 포함되는지 확인
-- id=55 ([55,1,0,0])가 쿼리 [55,1,0,0]의 nearest neighbor여야 함
-- ----------------------------------------------------------------
SELECT id FROM vf ORDER BY v <-> '[55,1,0,0]' LIMIT 1;

-- ----------------------------------------------------------------
-- Test 3: budget 복원 후 REINDEX → delta 흡수, extend_count 초기화
-- ----------------------------------------------------------------
SELECT pg_cuvs_set_vram_budget(0);
REINDEX INDEX CONCURRENTLY vf_cagra;

SELECT delta_rows = 0   AS no_delta,
       extend_count = 0 AS extend_clean
FROM pg_stat_gpu_search
WHERE index_oid = 'vf_cagra'::regclass;

-- ----------------------------------------------------------------
-- Teardown
-- ----------------------------------------------------------------
DROP TABLE vf;
