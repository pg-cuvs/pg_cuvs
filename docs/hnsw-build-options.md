# over-the-vram HNSW 생성 옵션

이 정책은 0.8.2 `USING pg_cuvs_hnsw` 직접 heap 생성에 적용한다.
기존 device CAGRA AUTO 경로의 정책은 바꾸지 않는다.

```sql
CREATE INDEX items_ann ON items USING pg_cuvs_hnsw (embedding vector_l2_ops)
  WITH (mode = 'hnsw', build_algo = 'auto', ef_construction = 64);
SELECT pg_cuvs_hnsw_build_info('items_ann'::regclass);
```

## 생성기와 예산

- `auto`(기본): 16행 미만은 tiny exact GPU 그래프다. 나머지는 추정이 host와 GPU 실효 예산 안이면 bounded-training resident IVF-PQ B 설정을 사용하고, 넘으면 기본값에서 ACE-disk로 넘친다(ADR-087, UC-5). 넘칠 때는 전환 NOTICE(`build_algo=auto exceeded the resident budget ...; building with ACE on disk`)와 빌드 뒤 완료 NOTICE(`auto selected ACE-disk`) 두 줄을 내고 영수증에 `requested_algo=auto`, `route=ace_disk`, `auto_overflow=true`를 남긴다. 데몬을 `--hnsw-auto-overflow error`로 띄우면 넘치지 않고 오류와 `build_algo=ace` 힌트를 낸다. 256행 미만은 넘칠 수 없어 오류다. ACE 인덱스의 recall과 지연은 resident와 다를 수 있다. 경로는 빌드 시점 예산으로 정해지므로 작은 호스트로 복원하거나 REINDEX하면 ACE로(또는 `error`에서는 오류로) 다시 만들어질 수 있다. 경로를 고정하려면 `build_algo`를 명시한다. resident IVF-PQ를 골랐는데 cuVS가 그 kNN 그래프를 거부하면(`Could not generate an intermediate CAGRA graph`) 같은 기본값에서 ACE-disk로 다시 빌드하고 NOTICE(`build_algo=auto built ACE-disk (N partitions) because cuVS rejected the resident IVF-PQ kNN graph`)를 내며 영수증에 `route=ace_disk`, `graph_fallback=true`를 남긴다. `--hnsw-auto-overflow error`이거나 256행 미만이면 이 대체 없이 오류다.
- 판정 시점: 테이블 통계(`reltuples` > 0)가 있으면 경로와 디스크 사전 점검을 heap 스캔 전에 `reltuples`와 선언 차원으로 판정한다. 거부와 ACE 전환 NOTICE가 spool 전에 나오고, 빌드는 spool한 행 수로 다시 판정한다. 통계가 없으면 스캔 뒤에 판정하며 영수증 `route_decided`가 어느 쪽인지 기록한다(`before_scan_from_statistics`, `after_scan_no_statistics`, 통계는 있었으나 데몬에서 스캔 전 예측을 받지 못한 `after_scan_preflight_unavailable`). 통계가 낡았으면 이른 판정이 틀릴 수 있으니 먼저 `ANALYZE`한다.
- `ivf_pq`: resident IVF-PQ를 명시한다. 추정 예산 초과 또는 16행 미만이면 오류이며 다른 생성기로 대체하지 않는다. cuVS가 kNN 그래프를 거부해도 대체하지 않고 오류다.
- resident IVF-PQ kNN 정밀도(`auto`, `ivf_pq` 공통): 제품은 원입력의 최대 제곱 노름에 따른 보수적 위험 분류를 사용한다. L2는 `4 * max_squared_norm`, 내적·cosine은 `max_squared_norm`이며, 상한이65504 이상이거나NaN/inf이면 coarse·internal·LUT를 모두fp32로 둔다. 정규화 데이터는 기존fp16 선택을 유지한다. 이 식은 모든PQ 중간항의 수치 범위를 증명하는 공식은 아니다. 영수증 `ivfpq_distance_dtype`은 이 **base graph**의 정밀도다.
- `mode='hnsw'`의 GPU 상위 계층도 고정SDK 패치0004가 같은 정책을 적용한다. 실제 상위 레벨의 `rows * neighbor_degree >= 10000000`이고 IVF-PQ를 고른 경우에만, 그 레벨에 전달한 원벡터의 노름을 검사해 세 dtype을 함께fp32로 선택한다. 그 미만의 NN-descent 및 예산으로 NN-descent를 고르는 경로는 생성기·정밀도 파라미터를 바꾸지 않는다. 상위 선택은 SDK의 `HNSW upper-level IVF-PQ precision` INFO 로그로 확인한다. base 영수증만으로 upper dtype을 판정하지 않는다.
- SDK0004는 메모리 상위 계층 복사도 수정한다. hnswlib의 `char*`를 `const T*`로 해석해 `dim`개 원소를 복사한다. 과거 잘못된 바이트 좌표로 만든 그래프와는 이웃이 달라질 수 있으며, 정규화/NN 경로의 **파라미터 정책 유지**와 그래프 바이트 동일성은 다르다. 기본 `mode='nsw'`는 이 계층 생성을 통과하지 않는다.
- ACE-disk 파티션은 이 upper 정책의 대상이 아니다. 제품은 ACE에 양수 host·GPU 예산을 넘기고, 메모리 가드0003은 예산이 있으면 파티션 kNN을 NN-descent로 선택한다. SDK 레시피·마커·소스 해시는 `infra/cuvs-patches`를 따른다.
- `ace`: disk 경로를 명시한다. resident 추정이 예산 안이어도 ACE를 요청한다. N<16은 tiny와 충돌하고 N16–255는 partition 후보가 없어 오류다. 그 밖도 SDK의 형상·고정점 P·private 경로·가드 검사를 통과해야 한다. 다른 생성기로 대체하지 않는다. 성공한 명시 ACE는 `explicit build_algo=ace selected ACE-disk` NOTICE를 출력한다.
- `nn_descent`: 예약된 미지원 값이다. 메모리 모델 부재와 `auto`(tiny/resident) 또는 명시 `ace` 대안을 설명하는 오류로 거부한다. ACE 내부의 NN-descent 사용과는 별개다.

```sql
-- Disk build is an explicit choice, not a fallback from AUTO.
CREATE INDEX items_disk_ann ON items USING pg_cuvs_hnsw (embedding vector_l2_ops)
  WITH (mode = 'hnsw', build_algo = 'ace', ef_construction = 64);
```

빈 heap은 GPU 생성/영수증이 없고, source graph 재사용은 재생성 정책을 적용하지 않는다.
resident 예산 초과 전용 status13과 ACE 힌트는 tiny/ACE 자체 실패·불명 입력·실제 OOM에 쓰지 않는다.
빌드 중 메모리 할당 실패(`std::bad_alloc`, RMM `out_of_memory`)는 status 2이고 SQLSTATE 53200(`memory exhausted during the HNSW build`)으로 끝나며 상주 인덱스를 내리지 않는다. HINT는 요청 algo별로 다르다(auto: 명시 `ace`, ivf_pq: 예산·정리·`ace`).

`cuvs.max_build_mem_mb`는 이 HNSW 경로에서 host 경로 선택 예산이다.
훈련 reserve는 실제 SDK 정수 샘플링 행 수를 사용한다. 추정치와 SDK guard는 모든 할당이나 RSS의 상한 보장이 아니다.
ACE 메타데이터에는 SDK 자체 planner가 산출하고 재소비해도 유지되는 조각 수를 기록한다.
이에 필요한 matching patched SDK의 `cpp/src` 경로를 빌드 시 `CUVS_SDK_SOURCE_INCLUDE`로 지정한다.
선택한 SDK의 CUTLASS/CCCL 등 include·정의 설정도 `NVCC_FLAGS`에 함께 제공한다.
빌드는 planner 소스 SHA를 검증하고 SDK와 같은 OpenMP 의미로 planner를 소비한다.
지원하지 않는 빌드는 ACE를 생성하기 전에 오류를 내며 추정 후보를 실제 조각 수로 기록하지 않는다.

## ef_construction

기본 64는 검증된 실제 설정을 유지한다: resident 중간 차수 128, hierarchy 200. ACE 품질은 아래 절의 규칙으로 240이다.
값을 바꾸면 각 기준값에 `ef_construction / 64`를 곱하고 올림한다.
중간 차수는 최종 차수 이상, 4096 이하 및 데이터 행 수 범위로 제한한다.
Hierarchy와 ACE 요청 설정은 4–65535 범위다. hnswlib는 native ef를 M 이상으로 제한하며 영수증에는 직렬화된 실제 값을 기록한다. 훈련 비율, probe 수, seed는 이 옵션으로 바꾸지 않는다.
이는 생성 노력의 전달 계약이며 recall의 단조 증가를 보장하지 않는다.
`tiny` 경로는 전체 이웃을 다루므로 이 옵션이 그래프 품질을 바꾸지 않는다.
`m`은 기존처럼 정보용이며 실제 graph degree가 레이아웃을 결정한다.

## ACE 조각 그래프 품질

ACE는 데이터를 겹치는 조각으로 나눠 조각마다 kNN 그래프를 만든 뒤 합친다. cuVS 26.04는 ACE 품질 값 A(영수증 `ace_ef_construction`)를 q = A / 16으로만 쓰며, 100만 행 미만 조각은 NN-descent를 최대 5 + q회 반복한다.

- 데몬 기본 `--hnsw-ace-build-quality auto`: `ef_construction` 64에서 조각의 NN-descent가 전체 데이터 NN-descent의 기본 반복 수(링크한 SDK의 `nn_descent::index_params` 기본값, cuVS 26.04에서 20)만큼 돌도록 q = 20 - 5 = 15, A = 240으로 정한다. 다른 `ef_construction`은 A를 `ef_construction / 64` 비율로 올림한다(128이면 480). 4–65535 범위로 제한한다.
- 이전 규칙은 SDK `ace_params` 기본값 A = 120(12회)을 썼다. 999,000×768, `ef_construction` 64에서 이 값은 낮은 `hnsw.ef_search`의 recall@10이 같은 데이터의 resident IVF-PQ보다 0.01 넘게 낮았다(UC-5 기준 미달).
- `--hnsw-ace-build-quality Q`(0–4095)는 모든 ACE 빌드에 A = 16 × Q를 쓴다. `ef_construction`과 무관한 진단·고정용 값이다. 시작할 때 로그에 남고, 재시작해야 바뀐다.
- 조각의 kNN 폭은 SDK 그대로 3 × (graph_degree / 2), degree 64에서 96이다. 영수증의 `intermediate_degree`(128)는 resident 경로의 폭이자 ACE 메모리 계획에 넘기는 값이며, ACE 조각이 kNN 생성과 최적화에 실제로 쓰는 폭이 아니다. 조각 폭을 128로 넓히는 SDK 수정은 같은 데이터에서 12회와 20회 반복 모두 recall을 낮춰 채택하지 않았다.
- `build_algo=auto`의 ACE 넘침 기본값, 예산 가드, NOTICE는 바뀌지 않는다. ACE와 resident의 recall·지연 차이는 데이터와 `ef_search`에 따라 다르다.

## 저장된 정보

0.8.2의 `pg_cuvs_hnsw_build_info(regclass)`는 생성 시 저장한 경로, 실제 설정,
resident 후보 훈련 형상, 추정치·실효 예산과 guard 반환 여부를 조회한다.
현재 세션 GUC로 과거 설정을 추정하지 않는다. 원 테이블 SELECT 권한이 필요하다.
ACE 인덱스는 영수증 바로 뒤에 조각 빌더 readback을 저장한다: `ace_build_quality_source`(`auto`/`manual`), `ace_build_quality`(q), `ace_local_intermediate_degree`(조각 kNN 폭), `ace_local_nn_descent_iterations`(조각 NN-descent 반복 상한; 100만 행 이상을 셋 이상의 조각으로 나누면 조각마다 달라 `varies_by_partition`). ACE 빌드는 검증된 고정 SDK(memory-guard marker `@60b19a0f`)에서만 돌므로 이 값은 그 SDK의 동작에서 계산한 값이다. 조각별 실제 값은 데몬 로그의 SDK 줄로 확인한다. 이 기능 이전에 만든 ACE 인덱스는 `"ace_readback":"not_recorded"`, resident·tiny는 `"not_applicable"`이다. 영수증 형식(policy v2)은 그대로이며 readback은 별도 magic의 꼬리 레코드다.
기존 인덱스, 빈 heap, source 그래프 재사용에는 영수증이 없을 수 있으며 `not_recorded`로 표시한다.
명시적 `build_algo`와 `source` 그래프 재사용을 함께 지정할 수 없다.
pgvector metapage prefix와 영수증 크기는 유지한다. 새 writer는 policy v2만 기록하며 새 reader는 역사적 v1 auto→ACE도 당시 값 그대로 읽는다. 손상·잘린 알려진 영수증은 오류, 알 수 없는 미래 버전은 unsupported 오류다. 현재 설정으로 과거 메타를 덮어쓰지 않는다.
실제 재시작·INSERT/DELETE/VACUUM 통합은 별도 검증 대상이며, CPU 페이지 검사는 실행 중인 PostgreSQL의 증거가 아니다.

품질 합격은 새 질의의 exact top10 대비 recall@10이 직접 생성한 pgvector HNSW 이상인지
같은 데이터·같은 ef와 같은 질의 비용에서 각각 비교한다. self 도달성 진단과는 별개다.
같은 f32 벡터의 다른 ID는 동점 정답이다. 작은 생성 시험은 이 품질 기준의 통과 증거가 아니다.

extension과 daemon은 HNSW op23 payload v5를 함께 배포해야 한다(v4는 응답 뒤에 결정 바이트 6개, 즉 status-13 DETAIL의 원값을 덧붙이고, v5는 그 뒤에 ACE partition builder readback 32바이트를 덧붙인다). 통합 기준 main의 글로벌 v4 명령 ABI는 유지되며 HNSW op23 payload v5와 구분한다.
이전 payload v1/v2/v3/v4 요청은 prefix의 버전을 먼저 검사해 나머지 필드를 기다리지 않고 거부한다.
기존 0.8.1 단독 reader는 새 policy v2를 읽을 수 없다. 0.8.2 바이너리를 함께 배포한 뒤 `ALTER EXTENSION pg_cuvs UPDATE TO '0.8.2'`를 사용한다. upgrade SQL은 영수증을 이주/덮어쓰기하지 않는다. v2가 있는 인덱스를 예전 reader로 downgrade하는 것은 지원하지 않는다.

이 구현의 CPU 검사·코드 리뷰와 별개로 작은 실제 CUDA 생성·1M 동일성/비용/품질은 별도 실행 승인과 검증이 필요하다. 같은 native 파일의 변환 보존과 서로 다른 GPU build 간 SHA 불일치는 구분하며, 후자의 비결정성을 성공 판정으로 덮지 않는다.
`internal_builder`는 base/local graph 생성기를 표시한다. 다른 계층의 미기록 과거 분기를 추정한 값이 아니다.
