# GPU 브루트포스 검색 가속 교훈

**작성일**: 2026-06-04  
**출처**: cuvs-silicon (Apple Silicon Metal GPU 구현) 실험 결과  
**대상**: Phase 3L `cuvs.search_mode='brute_force'` 구현 참고

---

## 1. 배경

cuvs-silicon 프로젝트에서 Apple M3 Max의 Metal GPU로 1M×1024d 벡터에 대한 브루트포스 검색을 구현하면서, GPU 브루트포스의 성능을 좌우하는 핵심 요인들을 실험적으로 확인했다. 이 중 GPU 아키텍처와 무관하게 CUDA에도 적용되는 교훈을 pg_cuvs Phase 3L 구현 참고 자료로 정리한다.

### 실험 환경 및 기준선

| 구현 | 하드웨어 | N=1M Q=100 QPS | N=1M Q=1 레이턴시 |
|---|---|---|---|
| AMX cblas_sgemm (CPU) | M3 Max | 13.8 | - |
| GPU single-pass kernel | M3 Max Metal | 156 | - |
| MPS matmul + CPU top-K | M3 Max Metal | 7.9 | - |
| **2-pass GPU (float32)** | M3 Max Metal | **749** | 19ms |
| **2-pass GPU (float16)** | M3 Max Metal | **1012** | 19ms |
| metalfaiss (MLX 최적화) | M3 Max | 351 | 82ms |
| CUVS brute_force (참고) | A100 | ~4000 (추정) | ~2ms |

---

## 2. 핵심 교훈

### 2-1. Q×N 중간 행렬을 CPU로 꺼내지 말 것

**가장 중요한 단일 원칙이다.**

Q=100, N=1M, float32 기준 Q×N 행렬의 크기는 100×1M×4 = 400MB다. 이를 GPU→CPU로 전송하면 CPU 메모리 대역폭(~40GB/s)에서 400MB를 읽는 데 ~10ms가 소요되고, GPU가 쓴 shared memory를 CPU가 읽을 때의 cache coherency 동기화 비용이 추가된다.

실험 결과: MPS matmul(GPU) + CPU top-K 조합은 오히려 순수 CPU AMX보다 7.9 QPS로 더 느렸다. 반면 matmul(GPU) + L2+top-K(GPU) 조합으로 중간 행렬을 GPU에 유지하고 Q×K = 4KB만 CPU로 꺼내자 749 QPS를 달성했다.

**pg_cuvs 적용 지점**: `cuvs::neighbors::brute_force::search`가 내부적으로 이 패턴을 사용하는지 확인한다. IPC shm reply 경로에서 Q×N 행렬이 절대 shm을 경유하지 않도록 보장해야 한다. reply payload는 Q×K indices + Q×K distances (= Q×K×8 bytes, K=10이면 800B)여야 한다.

```c
// IPC reply: Q×K만 전송 (절대 Q×N 중간 행렬 포함 금지)
// N=1M, Q=100, K=10 기준: 100 × 10 × 8B = 8KB
write_result_to_shm(out_indices, out_distances, Q, K);
```

---

### 2-2. Float16 `.vectors` sidecar 옵션

**배경**: 브루트포스 검색의 병목은 GPU가 dataset 전체를 읽는 메모리 대역폭이다. float16으로 변환하면 동일한 대역폭에서 2배 많은 벡터를 처리할 수 있다.

실험 결과: float16 전환으로 추가 연산 비용 없이 QPS가 749 → 1012로 35% 향상됐다. cuVS `brute_force`는 `cuda::std::array<half, dim>` 기반 half precision search를 지원한다.

**pg_cuvs 적용 지점**: ADR-039에서 `.vectors` sidecar에 float32를 가정하고 있다. `cuvs.bf_precision` GUC(기본값 `'float32'`, 옵션 `'float16'`)를 추가해 다음을 구현한다:

1. `CREATE INDEX USING cagra` 시 `.vectors` sidecar를 지정한 precision으로 직렬화
2. 데몬 startup 시 `CuvsBfIndex`를 해당 precision으로 로드
3. `pg_stat_gpu_cache`에 `bf_precision` 컬럼 노출

VRAM 절감 효과 (float16 기준):

| N | Dim | Float32 | Float16 | 절감 |
|---|---|---|---|---|
| 1M | 384 | 1.5 GB | 0.75 GB | 750 MB |
| 1M | 1024 | 4.0 GB | 2.0 GB | 2.0 GB |
| 1M | 1536 | 6.0 GB | 3.0 GB | 3.0 GB |

float16의 정밀도 손실이 recall에 미치는 영향은 내적 누적(accumulation) 정밀도에 달려있다. cuVS brute_force는 float16 입력이더라도 accumulation을 float32로 수행하는 옵션을 제공하므로 recall 저하 없이 bandwidth 이득을 얻을 수 있다.

---

### 2-3. 동시 Q=1 요청의 마이크로배칭

**배경**: GPU는 단일 쿼리(Q=1)에서 심각하게 저활용된다. 동일 시간에 GPU가 처리하는 데이터량은 Q=1이든 Q=100이든 비슷하지만, GPU dispatch 오버헤드는 Q에 무관하게 고정이다.

실험 결과:

| Q | QPS | 1쿼리당 시간 |
|---|---|---|
| 1 | 53 | 19ms |
| 10 | ~300 (추정) | 3.3ms |
| 100 | 1012 | 0.99ms |

N=1M 기준 Q=100이 Q=1 대비 19배 빠르다. GPU bandwidth 포화점(이론 최솟값 ~10ms/1M)이 분모가 커지면서 실제 throughput이 선형에 가깝게 증가한다.

**pg_cuvs 적용 지점**: 여러 PostgreSQL backend가 동시에 `cuvs.search_mode='brute_force'`로 검색할 때, 데몬이 짧은 대기 윈도우(예: 500μs) 동안 요청을 수집한 뒤 단일 Q=N GPU dispatch로 묶는 마이크로배칭을 구현한다.

```
client A: search(q_A) ─┐
client B: search(q_B) ─┤ 500μs window ─→ brute_force::search(Q=3) ─→ split results
client C: search(q_C) ─┘
```

구현 고려사항:
- batch 윈도우: `cuvs.bf_batch_wait_us` GUC (기본 0 = 배칭 비활성, 권장 범위 100-1000μs)
- 배칭이 활성화될 때만 `g_bf_mutex` 아래에서 request queue를 관리
- reply는 각 client의 shm에 개별 분배
- CAGRA search 경로와 코드를 공유하지 않고 BF 전용 배칭 루프로 분리
- `pg_stat_gpu_search`에 `bf_batch_size` 히스토그램 추가

특히 벤치마크 ground truth 생성(Q=1000 배치)과 같이 고정된 큰 Q 워크로드에서는 배칭 없이도 자연스럽게 효과를 얻는다.

---

### 2-4. d_norms 사전계산 및 캐시

**배경**: L2 거리 = ||q||² - 2(q·d) + ||d||²에서 ||d||²는 dataset이 바뀌지 않는 한 고정값이다.

cuvs-silicon에서 `cached_dataset_norms`를 dataset 로드 시 한 번만 계산하고 GPU buffer에 캐시해뒀다. 이를 통해 매 search마다 N번의 norm 계산을 건너뛸 수 있었다.

**pg_cuvs 적용 지점**: cuVS `brute_force::build`가 내부적으로 norm을 캐시하는지 확인한다. 만약 `CuvsBfIndex` 내부에 norm tensor가 보관된다면 별도 작업이 불필요하다. 그렇지 않다면 `IndexEntry`에 `bf_norms` GPU tensor를 추가하고 `load_index_bf` 시점에 한 번만 계산하는 것을 검토한다. N=1M×1024 float32 norm 계산은 ~10ms이므로 amortize 가치가 있다.

---

### 2-5. Q=1 단일 쿼리 레이턴시의 물리적 한계 인식

**배경**: 브루트포스는 N개 벡터 전체를 읽어야 한다. 이는 하드웨어 bandwidth에 의해 결정되는 하한선이다.

실험으로 확인한 수치:

| 하드웨어 | Dataset | 이론 최솟값 | 실측 Q=1 레이턴시 |
|---|---|---|---|
| A100 HBM2e (2TB/s) | 1M×1024 float32 | ~2ms | ~2ms (cuVS) |
| M3 Max (400GB/s) | 1M×1024 float16 | ~5ms | 19ms |
| M3 Max MLX | 1M×1024 float32 | ~10ms | 82ms |

metalfaiss(Apple의 MLX 최적화 프레임워크)도 82ms였다는 점이 중요하다. 이는 19ms의 cuvs-silicon이 Apple Silicon에서 달성 가능한 최적에 근접한 구현임을 의미하며, 동시에 GPU Q=1 브루트포스는 하드웨어 bandwidth로 결정된다는 것을 보여준다.

**pg_cuvs cost model 함의**: `cuvsamcostestimate`에 `search_mode='brute_force'`에 대한 별도 cost estimate가 필요하다. A100 기준 Q=1 브루트포스는 N=1M에서 ~2ms이므로, CAGRA search(~2ms)와 비슷하지만 recall=1.0을 보장한다. cost 모델이 이 특성을 반영해야 한다:

```c
// brute_force mode cost estimate (A100 기준)
// bandwidth: ~2TB/s, N × dim × float32 bytes
double bf_latency_ms = (N * dim * 4.0) / (2e12 / 1e3);
double bf_startup = bf_latency_ms * 1000.0;  // cost unit으로 변환
```

ADR-039에서 "소규모 정확도 요구 workload (N < ~1M)" 조건이 적절한 이유는 N=1M 이상에서 VRAM 제약과 함께 latency도 선형으로 증가하기 때문이다.

---

## 3. Phase 3L 구현 체크리스트

이 교훈을 Phase 3L 구현에 반영하기 위한 체크리스트다.

### 필수 확인

- [ ] `cuvs::neighbors::brute_force::search`의 내부 동작 확인: Q×N 중간 행렬을 CPU 메모리에 쓰지 않는가?
- [ ] IPC shm reply payload 크기 확인: Q×K 결과만 포함하는가, 아니면 더 큰 버퍼를 사용하는가?
- [ ] `CuvsBfIndex` 로드 시 norm 캐시 여부 확인

### 권장 추가 기능

- [ ] `cuvs.bf_precision` GUC (`'float32'` / `'float16'`) 및 `.vectors` sidecar precision 지정
- [ ] float16 load path: `cuvs::neighbors::brute_force::build<half>` 또는 float32→half 변환 후 로드
- [ ] `pg_stat_gpu_cache`에 `bf_precision`, `bf_vram_bytes`, `bf_norms_cached` 컬럼
- [ ] `pg_stat_gpu_search`에 `search_mode` 컬럼 (ADR-039에 이미 포함)

### 성능 검증 기준

A100-40GB 기준 예상 성능 (N=1M×384):

| 측정 항목 | 목표값 | 비교 기준 |
|---|---|---|
| Q=1 레이턴시 | ≤ 2ms | bandwidth 이론값 |
| Q=100 QPS | ≥ 50,000 | Q×(1000/latency_ms) |
| IPC 오버헤드 | ≤ 0.5ms | UDS round-trip |
| VRAM (float32) | ≤ 1.6GB | N×384×4 bytes |
| VRAM (float16) | ≤ 0.8GB | N×384×2 bytes |

---

## 4. 참고 자료

- `cuvs-silicon/docs/apple_silicon_gpu_vector_search_report.md` — Apple Silicon 하드웨어 한계 분석
- `design/decisions.md` ADR-039 — Phase 3L 설계 결정
- `design/specs/phase-record.md` Phase 3L — GPU BF 검색 모드 사용자 노출 계획
- RAPIDS cuVS `cuvs::neighbors::brute_force` API 문서
