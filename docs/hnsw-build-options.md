# over-the-vram HNSW 생성 옵션

이 정책은 over-the-vram 계열의 `USING pg_cuvs_hnsw` heap 생성에 적용한다.
기존 device CAGRA AUTO 경로의 정책은 바꾸지 않는다.

```sql
CREATE INDEX items_ann ON items USING pg_cuvs_hnsw (embedding vector_l2_ops)
  WITH (mode = 'hnsw', build_algo = 'auto', ef_construction = 64);
SELECT pg_cuvs_hnsw_build_info('items_ann'::regclass);
```

## 생성기와 예산

- `auto`(기본): 16행 미만은 tiny exact GPU 그래프다. 나머지는 bounded-training resident IVF-PQ를 사용하고, 메모리 추정이 host 또는 GPU 예산을 넘으면 ACE-disk를 선택하며 NOTICE를 출력한다.
- `ivf_pq`: resident IVF-PQ를 명시한다. 추정 예산 초과 또는 16행 미만이면 오류이며 다른 생성기로 대체하지 않는다.
- `nn_descent`: 예약된 미지원 값이다. 메모리 모델 부재와 `auto` 대안을 설명하는 오류로 거부한다. ACE 내부의 NN-descent 사용과는 별개다.

`cuvs.max_build_mem_mb`는 이 HNSW 경로에서 host 경로 선택 예산이다.
훈련 reserve는 실제 SDK 정수 샘플링 행 수를 사용한다. 추정치와 SDK guard는 모든 할당이나 RSS의 상한 보장이 아니다.
ACE 메타데이터에는 SDK 자체 planner가 산출하고 재소비해도 유지되는 조각 수를 기록한다.
이에 필요한 matching patched SDK의 `cpp/src` 경로를 빌드 시 `CUVS_SDK_SOURCE_INCLUDE`로 지정한다.
선택한 SDK의 CUTLASS/CCCL 등 include·정의 설정도 `NVCC_FLAGS`에 함께 제공한다.
빌드는 planner 소스 SHA를 검증하고 SDK와 같은 OpenMP 의미로 planner를 소비한다.
지원하지 않는 빌드는 ACE를 생성하기 전에 오류를 내며 추정 후보를 실제 조각 수로 기록하지 않는다.

## ef_construction

기본 64는 검증된 실제 설정을 유지한다: resident 중간 차수 128, hierarchy 200, ACE 120.
값을 바꾸면 각 기준값에 `ef_construction / 64`를 곱하고 올림한다.
중간 차수는 최종 차수 이상, 4096 이하 및 데이터 행 수 범위로 제한한다.
Hierarchy와 ACE 설정은 4–65535 범위다. 훈련 비율, probe 수, seed는 이 옵션으로 바꾸지 않는다.
이는 생성 노력의 전달 계약이며 recall의 단조 증가를 보장하지 않는다.
`tiny` 경로는 전체 이웃을 다루므로 이 옵션이 그래프 품질을 바꾸지 않는다.
`m`은 기존처럼 정보용이며 실제 graph degree가 레이아웃을 결정한다.

## 저장된 정보

0.8.1의 `pg_cuvs_hnsw_build_info(regclass)`는 생성 시 저장한 경로, 실제 설정,
resident 후보 훈련 형상, 추정치·실효 예산과 guard 반환 여부를 조회한다.
현재 세션 GUC로 과거 설정을 추정하지 않는다. 원 테이블 SELECT 권한이 필요하다.
기존 인덱스, 빈 heap, source 그래프 재사용에는 영수증이 없을 수 있으며 `not_recorded`로 표시한다.
명시적 `build_algo`와 `source` 그래프 재사용을 함께 지정할 수 없다.
pgvector metapage prefix는 유지하므로 기존 검색·INSERT·VACUUM 레이아웃을 바꾸지 않는다.

품질 합격은 새 질의의 exact top10 대비 recall@10이 직접 생성한 pgvector HNSW 이상인지
같은 데이터·같은 ef와 같은 질의 비용에서 각각 비교한다. self 도달성 진단과는 별개다.
같은 f32 벡터의 다른 ID는 동점 정답이다. 작은 생성 시험은 이 품질 기준의 통과 증거가 아니다.

extension과 daemon은 HNSW op23 payload v2를 함께 배포해야 한다. 글로벌 v3 명령 ABI는 유지된다.
이전 payload v1 요청은 새 필드를 기다리지 않고 버전 오류로 거부한다.
`internal_builder`는 base/local graph 생성기를 표시한다. 다른 계층의 미기록 과거 분기를 추정한 값이 아니다.
