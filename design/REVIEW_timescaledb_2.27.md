# 설계 리뷰 — TimescaleDB 2.27 기법의 pg_cuvs 적용 가능성

> 출처: <https://www.tigerdata.com/blog/timescaledb-2-27> ("Broader Vectorized
> Execution, Up to 160x More Efficient UPDATE/DELETE, and Smarter UPSERT Pruning")
> 원본 리포 분석: `timescale/timescaledb` @ `2275ecd` (CHANGELOG 2.27.0~2.27.2),
> `tsl/src/compression/`, `tsl/src/nodes/{columnar_scan,vector_agg}/`.
> 작성: 2026-06-14. 본 문서는 **외부 설계의 평가 노트**이며 결정(ADR)이 아니다.

---

## 0. 한 줄 요약

2.27의 세 핵심 설계 중 **둘은 pg_cuvs에 비적용/이미흡수**, **하나(bloom sparse-index
배치 프루닝)만 진짜 이식 후보**다. 그 하나조차 **지금이 아니라** filtered/streaming-BF
트랙의 트리거 항목으로 붙이는 게 맞다. 구현 권고가 아니라 **백로그 등재 권고**.

이름 주의: 2.27의 "vectorized"는 **임베딩/벡터검색이 아니라 컬럼스토어 SIMD 배치 실행**을
가리킨다. 제목만 보고 벡터 DB 기능으로 오해하기 쉬우나 무관하다.

---

## 1. 2.27가 실제로 들여온 설계 3종

### (A) Bloom sparse-index 배치 프루닝 — *쓰기 경로로 확장*
- **메커니즘**: 압축 배치(compressed batch)마다 equality-적격 컬럼에 대해 작은 bloom
  필터(`bloom1`)를 빌드 타임에 계산해 배치 메타데이터로 저장. 쿼리 타임에 equality
  술어의 상수를 해싱해 `bloom1_contains`로 검사 → **이 배치엔 매치가 없음**이 증명되면
  decompress를 건너뜀.
  - 구현 디테일(`batch_metadata_builder_bloom1.c`): 6 해시, FP ≈ 2.2%, 256-bit 블록
    지역성, ≤64-bit는 메인 테이블에 인라인(fits-in-row), composite(다중 컬럼) 지원,
    NULL 마커로 `IS NULL`도 커버.
  - composite 다중 필터는 **컬럼 수 내림차순(=가장 선택적 우선)**으로 평가(`bloom_filter_check_cmp`).
  - 해시는 **플랜 타임에 선계산**(#9475)해 실행 루프에서 재해싱 제거.
  - **fail-open**: false positive는 불필요한 decompress 한 번일 뿐 결과는 항상 정확.
- **2.27의 새로움**: 기존엔 `SELECT`만 쓰던 프루닝을 `UPDATE`/`DELETE`/`UPSERT`
  **쓰기 경로**로 확장(#9374/#9399). EXPLAIN에 "Compressed batches filtered" 등
  관측 카운터 추가. 일부 케이스 최대 160×.

### (B) Vectorized filter 실행 — 컬럼스토어 fast-path 확대
- Hypercore 엔진이 WHERE 필터를 **Arrow 포맷 배치 위에서 표준 PG 함수 경로로 인라인
  평가**해 validity 비트맵 생성(`columnar_scan/compressed_batch.c`). 더 많은 쿼리(CAgg
  refresh 포함)가 컬럼스토어 빠른 경로를 타게 됨. 30%~2× 향상.

### (C) Continuous Aggregate 자동 쿼리 재작성 — 실험적/opt-in
- 플래너가 쿼리의 집계가 CAgg 정의와 정확히 일치하면 투명하게 CAgg로 라우팅(#8967).

---

## 2. pg_cuvs 매핑

### (C) CAgg 재작성 → **비적용**
벡터 검색엔 머티리얼라이즈드 집계 개념이 없다. 유사물 없음. 제외.

### (B) Vectorized filter → **이미 흡수됨, 저가치**
GPU 자체가 궁극의 벡터화 실행기이고, 3O **BITSET prefilter가 곧 GPU validity 비트맵**의
등가물이다. CPU-스칼라로 남은 지점(데몬 rev-map 이진탐색, post-filter 루프)을 SIMD로
배치화할 여지는 있으나:
- ADR-044: 검색은 **GPU-bound**(GPU:IPC≈2:1).
- ADR-074: flat 읽기 병목은 **TOAST detoast(~535ms)**, 거리계산≈0(memory-bound).

즉 CPU 필터 평가는 핫패스가 아니다 → 한계가치 낮음. 추격할 이유 없음.

### (A) Bloom 배치 프루닝 → **유일한 진짜 이식 후보**

**구조적 일치**: "equality 컬럼에 대한 *값싼 블록 단위 skip 인덱스*로 *비싼 블록 단위
연산*을 통째로 회피한다." TSDB에서 비싼 연산 = decompress. pg_cuvs에서 (ADR-074로
실측된) 지배적 비싼 연산 = **TOAST detoast + H2D 복사** (거리계산 아님). 따라서 블록
단위 skip 인덱스는 **pg_cuvs의 실측 병목을 정조준**한다 — 단 **filtered 쿼리**(벡터
ORDER BY 옆에 equality 술어)에 한해서. 그게 정확히 STRATEGY_NOTES/ADR-061이 표적으로
잡은 **멀티테넌트 filtered-RAG** 세그먼트다.

**들어맞는 자리 두 곳**:

1. **멀티-GPU 샤딩 fanout** (`pg_cuvs_server.c` shard_count≥2)
   - 필터 컬럼(예: `tenant_id`)에 대한 **per-shard bloom/min-max sparse 통계** →
     매치 0임이 증명된 샤드를 fanout에서 제외 → 쿼리당 fanout 비용 절감.
   - **주의(검증함)**: 현재 샤드는 **행-범위 파티션을 GPU에 round-robin** 배치이지
     테넌트-정렬이 아니다(`shard_*` 경로는 row-range). 따라서 per-shard 필터의 선택도는
     **인입(ingest) 클러스터링에 의존** — 테넌트별 배치 적재면 잘 듣고, 인터리브면 약함.
     테넌트-정렬 배치(미래 항목)와 짝지을 때 최대 효과.

2. **Streaming-BF sidecar-gather** (ADR-064, `cuvs.stream_bf_chunk_vectors`)
   - 이미 고선택성 필터에서 통과 벡터만 `.vectors`에서 pread로 gather. 여기에
     **per-chunk bloom/min-max**를 얹으면 "통과 0" 청크는 gather pread + H2D **이전에**
     스킵(현재는 down-select만 하고 청크는 다 훑음).

**기존 prefilter와의 구분(중복 아님)**:
- 3O rev-map + BITSET = **정확·쿼리별·벡터 입도**의 선택.
- bloom = **값싼·블록/샤드 입도의 사전 패스**.
- 둘은 **합성**된다: bloom이 블록/샤드를 쳐내고, 생존 블록 안에서 rev-map/BITSET이
  정확 선택. bloom은 **fail-open**(FP=헛수고 1회, recall 불변)이라 pg_cuvs의
  fail-closed/regret-averse 문화와 충돌 없음 — **결과를 절대 바꾸지 않는다**.

**그대로 훔칠 만한 엔지니어링 디테일**(추진 시):
- 플랜 타임 해시 선계산(#9475) — pg_cuvs도 plan-time에서 필터 상수 알 수 있음.
- composite 필터 most-selective-first 정렬.
- ≤64-bit fits-in-row 풋프린트(사이드카 비대 회피 — `index_dir`/basebackup 가드 철학과 일관).
- EXPLAIN/`pg_stat_gpu_*` 프루닝 카운터(관측성 문화와 일관).

---

## 3. 판정 / 권고

| 설계 | 판정 | 근거 |
|------|------|------|
| (C) CAgg 재작성 | 비적용 | 벡터 DB에 집계 머티리얼라이즈 없음 |
| (B) Vectorized filter | 이미흡수·저가치 | GPU=벡터화 실행기, BITSET=validity 비트맵; 병목은 detoast(ADR-074) |
| (A) Bloom 배치 프루닝 | **이식 후보(트리거)** | 블록 skip이 실측 병목(detoast+H2D) 정조준; filtered 멀티테넌트 표적과 합치 |

**권고**: (A)를 **지금 구현하지 말 것**. 가치는 (i) filtered 멀티테넌트 + (ii) 그것이
확장할 streaming-BF/샤딩 기질 — 둘 다 트리거 백로그(VRAM 초과 스케일 / filtered 교차점
측정)다. 정직하게:
- **release-prep에 끼워넣지 않는다**(릴리스 준비는 문서·벤치·플레이북 순차 경로).
- **"filtered 교차점 + 런타임 적응 라우팅"** 및 **Streaming-BF sidecar-gather 후속**
  트랙에 *후보 레버*로 등재. 트리거(고선택성 filtered 워크로드 실측 / 테넌트-정렬 배치
  수요)가 서면 그때 ADR로 승격.

요컨대 이 블로그에서 pg_cuvs가 얻을 건 **단일 아이디어 한 줄**이다: *"filtered 검색에서
detoast+H2D를 치르기 전에, 필터 컬럼의 값싼 블록 단위 skip 인덱스로 샤드/청크를 먼저
쳐내라."* 신규 워크 스트림이 아니라 기존 filtered/streaming 트랙의 가속 레버로 다룬다.
