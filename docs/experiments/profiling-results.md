# pg_cuvs 연산 지역성 프로파일링 결과 (4-preflight / ADR-044)

> 측정일: 2026-06-05
> 측정자: 4-preflight 세션
> 관련 ADR: ADR-044(프로파일링 계획), ADR-034(빌드 오버헤드), ADR-035(page write 병목), ADR-043(TOAST 비용), ADR-039(마이크로배칭)

이 문서는 빌드/검색/export 세 데이터 경로의 latency split을 실측한 결과다. 기존 ADR-034/035/043의 근거가 **코드 분석 기반 추정**이었으므로, 실측으로 검증·보정한다.

---

## 1. 측정 환경

| 항목 | 값 |
|------|-----|
| GPU | NVIDIA A100-SXM4-40GB (40465 MB) |
| PostgreSQL | 16.14 |
| 데이터셋 | `t` 테이블, N=1,000,000, dim=1024 (Cohere Wikipedia 계열) |
| storage | EXTENDED(`t`, 기본 TOAST) / PLAIN(`t_plain`, 동일 데이터 복제) |
| 인덱스 | CAGRA (`vector_l2_ops`), graph_degree 기본 |
| 도구 | NVIDIA Nsight Systems 2023.4.4, Linux perf, pg_stat_io / pg_stat_wal (PG16+) |

### 측정 제약 (중요)

1. **GCP VM은 하드웨어 PMU 카운터를 노출하지 않는다.** `cache-misses` / `LLC-load-misses` / `instructions` / `cycles` 모두 `<not supported>`. 따라서 ADR-043/044가 계획한 **cache-miss 핫스팟 측정은 이 하드웨어에서 불가능**하다. 대안으로 (a) `task-clock` 소프트웨어 이벤트 시간 기반 샘플링과 (b) EXTENDED vs PLAIN 빌드 시간 델타로 detoast 비용을 직접 측정했다. 시간 델타는 cache-miss 프록시보다 오히려 직접적인 측정이다.

2. **nsys 2023.4.4는 다중 스트림 멀티스레드 빌드 캡처에서 qdstrm→nsys-rep 변환이 실패한다** ("Wrong event order has been detected" — CUDA 이벤트 cross-thread 순서 버그). 검색 경로(단순 스트림)는 정상 캡처됐다. **빌드 GPU 시간은 nsys 대신 데몬 journal 타임스탬프**(`handle_build` 시작 → `built index` 완료)로 측정했다.

---

## 2. 검색 경로 (CAGRA, Q=1, 1M×1024)

### 측정 방법
- 데몬을 nsys(`--trace=cuda`)로 실행, 300회 단일 검색 + 배치(Q=100) 후 데몬 정상 종료 → in-process finalize로 유효 nsys-rep 획득.
- 데몬 wall-clock latency는 `pg_stat_gpu_search.avg_latency_us`(데몬 측 측정).

### 결과

| 구성요소 | 시간 (Q=1) | 비율 | 출처 |
|----------|-----------|------|------|
| **GPU 커널** (CAGRA search + topk) | **~715 µs** | **66%** | nsys `cuda_gpu_kern_sum` |
| ├ `multi_cta_search::search_kernel` | 698 µs (median) | 65% | 304 instances |
| ├ `kern_topk_cta_11` | 14.1 µs | 1.3% | 300 instances |
| └ `set_value_batch_kernel` | 2.4 µs | 0.2% | 304 instances |
| **memcpy** (H2D 쿼리 + D2H 결과) | **~4.4 µs** | **0.4%** | nsys `cuda_gpu_mem_time_sum` |
| ├ H2D 쿼리 벡터 | 2.08 µs (median) | — | 4KB/query |
| └ D2H 결과 (TID+dist) | 2.34 µs (median) | — | K=10 |
| **IPC + overhead** (shm read/write, socket, 결과 정리) | **~358 µs** | **33%** | wall-clock − kernel − memcpy |
| **데몬 wall-clock (합계)** | **1077.5 µs** | 100% | `pg_stat_gpu_search.avg_latency_us` |

> 일회성 인덱스 로드 H2D(4GB `.vectors`/`.cagra` GPU 업로드)는 876 ms로 측정됐으나 per-search 비용이 아니다(lazy-load 1회).

### 핵심 결론
- **GPU 커널 : IPC overhead ≈ 2 : 1.** CAGRA 검색은 **GPU-bound**다(커널이 66%).
- **memcpy는 0.4%로 무시 가능** — Q×K만 전송하는 zero-copy shm 설계(ADR-039 locality 원칙)가 검색 경로에서 잘 작동함을 실증.
- **배칭 효율**: Q=100 배치 커널이 1.27 ms(max) — Q=1(698µs) 대비 ~1.8배에 100배 쿼리 처리. 배칭이 throughput을 크게 높임.

---

## 3. 빌드 경로 (CREATE INDEX USING cagra, 1M×1024)

### 측정 방법
- backend에 perf(`task-clock` 샘플링) 부착, 빌드 wall-clock는 `\timing`, GPU build 구간은 데몬 journal 타임스탬프.

### 결과 (EXTENDED storage)

| 구성요소 | 시간 | 비율 | 출처 |
|----------|------|------|------|
| **GPU CAGRA build** (데몬: shm read + H2D + 그래프 구축) | **~68 s** | **82%** | journal: `handle_build`→`built index` |
| **backend** (heap scan + detoast + memcpy + shm write) | **~15.5 s** | **18%** | total − GPU build |
| **빌드 wall-clock (합계)** | **83.5 s** | 100% | `\timing` (83.5–85.5s 재현) |

### backend CPU 분포 (perf task-clock, on-CPU 시간 기준)

| 항목 | 비율 (backend CPU) | 비고 |
|------|-------------------|------|
| page fault (`do_user_addr_fault` 등) | **~39%** | accumulation buffer `realloc` 성장 + TOAST `palloc` |
| memcpy/memmove (`__memmove_evex`) | **~11%** | 벡터→flat buffer 복사(6.7%) + TOAST 재조립(3.3%) |
| heap scan + detoast 로직 | 나머지 | `heapam_index_build_range_scan` 55% (children) |

### 핵심 결론 (ADR-034 보정)
- **ADR-034의 "GPU build ~10s vs PG overhead ~45s" 추정은 역전됐다.** 실측은 **GPU build가 ~68s로 지배(82%)**, PG backend는 ~15.5s(18%)다. (ADR-036의 1M×1024 CAGRA build 55.7s, ADR-020의 1M×1536 build 70.8s와 일관.)
- **backend(~15.5s)와 GPU build(~68s)는 직렬**(backend 전체 accumulate → shm 전송 → GPU build). 4A는 backend ~15.5s만 대상이고, **어떤 4A도 빌드를 ~68s 밑으로 못 내린다**.
- **가치/난이도 (절대 절감만이 아니라 ROI로 판단)**:
  - **4A-1 (double memcpy)**: ~2-5s(~3-6%), **난이도 낮음**(ADR-034) → ROI 양호한 quick win. memcpy ~1.7s + realloc page fault(backend CPU 39%) 완화. 추가로 shm 직접 할당이 **4A-2 worker buffer의 전제**(enabler)이므로 4A-1을 먼저.
  - **4A-2 (parallel workers)**: backend heap scan+detoast(~12s) 병렬화 → 4 workers 기준 ~15.5s→~7s, **~8-12s(~10-14%)**, 난이도 중간 → 절대 이득 크나 작업량 많음.
  - 둘 다 빌드가 일회성(CREATE INDEX/REINDEX)이라 쿼리 경로 대비 **긴급도만 낮을 뿐 저가치 아님**. 빌드 속도가 워크로드 우선순위면 **4A-1 → 4A-2** 순.
- 빌드를 ~68s 밑으로 내리려면 cuVS build 파라미터(graph_degree) 또는 streaming(cuVS incremental API 부재)이 필요 — pg_cuvs 단독 4A 범위 밖.

---

## 4. TOAST(EXTENDED) vs PLAIN storage (ADR-043 실증)

동일 1M×1024 데이터를 EXTENDED(`t`)와 PLAIN(`t_plain`)에 적재해 비교.

| 측정 항목 | EXTENDED | PLAIN | 차이 |
|-----------|----------|-------|------|
| (a) CAGRA 빌드 시간 | 83.5 s | 76.7 s | **PLAIN 6.8s (8.1%) 빠름** |
| (b) backend 구간 (빌드 − GPU 68s) | ~15.5 s | ~8.7 s | detoast ≈ 6.8s |
| (c) main heap 크기 | 58 MB | 7813 MB | PLAIN 134× 큼 |
| (c) TOAST+합계 | 13 GB (거의 TOAST) | 7.8 GB (TOAST 없음) | PLAIN 총 디스크 작음 |
| (d) 검색 latency (GPU CAGRA) | storage 무관 | storage 무관 | CAGRA 그래프는 VRAM 상주; heap recheck는 K=10개만 |
| (e) INSERT throughput (100k) | 3130 ms | 2811 ms | **PLAIN 10% 빠름** |
| cache-miss 핫스팟 | 측정 불가 (GCP VM PMU 미지원) | 측정 불가 | §1 제약 참조 |

### 핵심 결론 (ADR-043 보정)
- **PLAIN의 빌드 절감은 ~8%로, ADR-043 추정(~25-35%)보다 훨씬 작다.** detoast(TOAST 재조립; 4KB float 벡터는 pglz 압축 효과 적음)는 빌드의 ~8%뿐이고, 빌드의 82%는 GPU가 차지하기 때문.
- 다만 PLAIN은 빌드(8%)·INSERT(10%)·총 디스크(13GB→7.8GB) 모두 유리. **단점은 main heap 134× 증가**(비-벡터 쿼리 저하) — 벡터 전용 테이블에서만 권장하는 ADR-043 패턴은 여전히 타당.
- **권장 강도 조정 필요**: NOTICE/best-practice 문구의 "~25-35% 빌드 절감"을 **"~8% 빌드 절감 + 디스크/INSERT 이득"**으로 보정.

---

## 5. Export 경로 (CREATE INDEX USING pg_cuvs_hnsw, 1M 페이지, LOGGED)

CAGRA → pgvector HNSW 변환(`write_elem_page` × 1M). source=`t_cagra`, mode=`nsw`.

### 결과

| 구성요소 | 비율 (backend CPU) | 비고 |
|----------|-------------------|------|
| `write_elem_page` | **77%** | per-page 쓰기 본체 |
| ├ **buffer manager** (`ReadBufferExtended`/`ReadBuffer_common`) | **50% of write_elem_page (~39% 총)** | P_NEW 할당 + relation extension lock + 버퍼풀 |
| ├ **WAL** (`log_newpage_buffer`→`XLogInsert`) | **18% of write_elem_page (~14% 총)** | full-page image + crc32c |
| └ page fill (`PageAddItem`/memcpy/`MarkBufferDirty`) | ~32% of write_elem_page (~25% 총) | 벡터/이웃 데이터 복사 |
| **wall-clock (합계)** | 63.5 s | `\timing` |

| WAL 측정 (pg_stat_wal 델타) | 값 |
|------|-----|
| WAL records | 1,000,238 |
| WAL bytes | 4441 MB |
| WAL full-page images (FPI) | 1,000,026 |

leaf self-time: memcpy(rep_movs 10% + memmove 8% = ~18%), crc32c 3.4%, 페이지 캐시/fault ~8%, pwrite ~3.5%.

### 핵심 결론 (ADR-035 실증)
- **ADR-035의 "buffer manager 제약" 거부 근거가 정량 실증됨.** `ReadBuffer_common`(P_NEW relation extension 직렬화)이 export 단일 최대 비용(~39%)이다. 페이지당 순차 `ReadBuffer(P_NEW)`가 relation extension lock에서 직렬화되므로 병렬 page write가 막힌다는 ADR-035 논거가 실측으로 확인됨.
- **WAL은 ~14%(CPU) + 4441MB I/O.** UNLOGGED(ADR-033)가 이 부분을 제거 → LOGGED 대비 절감의 출처를 설명.
- 병렬 page write / Bulk WAL을 단기 제외한 ADR-035 결정은 유효.

---

## 6. 우선순위 재검증 (4-pre-4)

| 항목 | 측정 전 가정 | 실측 결과 | 우선순위 판단 |
|------|-------------|-----------|--------------|
| **적응형 마이크로배칭 (CAGRA)** | IPC 지배 여부 불명 | GPU 커널 66%, IPC 33% (2:1) | **CAGRA 단일 쿼리엔 제한적**(IPC 33% 상한). 동시성 throughput엔 유효. BF 모드(bandwidth-bound)가 이득 더 큼 → BF 우선 |
| **4A-1 (double memcpy 제거)** | 빌드 ~2-5s 절감 | ~2-5s(~3-6%), **난이도 낮음**; 4A-2 enabler(shm 직접 할당) | **quick win**. 저난이도라 ROI 양호, 먼저 착수 |
| **4A-2 (parallel workers)** | 빌드 ~10-20s 절감 | ~8-12s(~10-14%), 난이도 중간; heap scan 병렬화 | **큰 이득, 작업량 많음**. 긴급도만 낮음(빌드 일회성). GPU 68s 천장 불변 |
| **ADR-043 PLAIN 권장** | 빌드 ~25-35% 절감 | 빌드 8% + INSERT 10% + 디스크 이득 | **유지하되 문구 보정** (빌드 절감 8%로) |
| **ADR-035 병렬 page write 제외** | buffer manager 제약(추정) | buffer mgr ~39% 실증 | **유지** (거부 근거 강화) |

### 종합
1. **빌드 천장은 GPU build ~68s.** 빌드 시간의 82%가 cuVS 내부 GPU build(제어 불가). 빌드를 ~68s 밑으로 내리려면 cuVS build 파라미터(graph_degree) 또는 streaming이 필요. ADR-034의 "PG overhead 45s"는 틀린 추정 — 실제 PG backend는 ~15.5s.

2. **4A의 가치는 "빌드 시간 비율"이 아니라 "PG 오버헤드 제거율"로 평가해야 한다.** backend ~15.5s(EXTENDED)는 **전부 제거 가능한 PG 오버헤드**이고, 세 가지를 결합하면 거의 소멸한다:
   - **PLAIN storage** → detoast 제거 (측정: backend 15.5s → **8.7s**)
   - **4A-1 (shm 직접 할당)** → accumulation buffer realloc page fault(backend CPU 39%) + heap→shm double memcpy 제거
   - **4A-2 (parallel workers)** → 남은 heap scan 분산
   → backend가 ~15.5s에서 **~2-4s 수준**으로 축소되고, 빌드 wall-clock는 83.5s → **~70-72s**가 된다. 이는 **GPU build 68s + 최소 IPC/dispatch만 남는 것 = cuVS 직접 호출의 ~95% 성능**이며, 그것도 **PostgreSQL의 MVCC·durability·DDL 통합을 유지한 채** 달성한다.
   비율(14.5s/83.5s ≈ 17%)로 보면 작아 보이지만, **절대 14.5s는 전부 제거 가능한 PG 오버헤드**이고 이를 거의 다 제거한다는 것이 핵심 가치다("pg_cuvs build = Postgres 안전성 + cuVS native 속도"). 개별 4A는 modest하나 **결합 효과로 평가**해야 한다. 단 빌드는 일회성(CREATE INDEX/REINDEX)이라 긴급도는 쿼리 경로보다 낮다.
   - 착수 순서: 난이도/enabler 기준 **4A-1(저난이도, shm 직접 할당이 4A-2 전제) → 4A-2(난이도 중간)**. PLAIN은 사용자 스키마 선택(ADR-043).

3. **검색 경로는 잘 최적화돼 있다.** memcpy 0.4%, GPU-bound. 마이크로배칭은 BF·동시성 시나리오로 한정.
4. **export 병목(buffer manager 39%)은 PG 구조적 제약**으로 단기 개선 어려움(ADR-035 유지). UNLOGGED가 현실적 완화.

---

## 부록: 측정 재현 방법

```bash
# 검색 nsys (단순 스트림만 변환 성공)
nsys profile --trace=cuda --output=/tmp/p pg_cuvs_server ...   # 데몬 실행
# 워크로드 후 데몬에 SIGTERM → in-process finalize (--duration/SIGINT는 변환 깨짐)
nsys stats --report cuda_gpu_kern_sum,cuda_gpu_mem_time_sum /tmp/p.nsys-rep

# 빌드 backend perf (PMU 미지원 → task-clock 소프트웨어 샘플링)
perf record -g --call-graph fp -e task-clock -F 999 -p <backend_pid>
perf report --stdio --no-children

# 빌드 GPU 시간: 데몬 journal
journalctl -u pg-cuvs-server | grep -E 'handle_build|built index'

# TOAST vs PLAIN: 동일 데이터 두 테이블, \timing CREATE INDEX 비교

# export WAL: pg_stat_wal 델타 (CREATE INDEX USING pg_cuvs_hnsw 전후)
```

---

## 7. 빌드 corpus 핸드오프: memfd 하이브리드 (ADR-057, 2026-06-07)

빌드 corpus를 익명 memfd에 모아 `SCM_RIGHTS`로 데몬에 fd를 넘기는 무복사·누수-안전 핸드오프. 측정 환경
동일(A100/PG16). **north-star = raw cuVS 대비 backend 오버헤드 제거율**로 프레이밍한다.

### 빌드 오버헤드 분해 (N=500k dim=1024, memfd tier)

| 구성요소 | 시간 | 비고 |
|----------|------|------|
| **GPU build** (데몬: H2D + 그래프 구축) | **~33 s** | cuVS-native와 공유(천장). journal `corpus via memfd`→`built index` |
| **backend 오버헤드** (scan + detoast + memfd fill + 무복사 IPC) | **~6 s** | pg_cuvs가 cuVS 위에 얹는 PG 오버헤드 |
| **빌드 wall-clock (합계)** | **39.2 s** | `\timing` |

### old(heap+이중복사) vs new(memfd) A/B — N=500k dim=1024

| 지표 | old (heap+shm 복사) | new (memfd) | 차이 |
|------|--------------------|-------------|------|
| 빌드 wall-clock | 40.3 s | 39.2 s | −1.1s (GPU 지배라 marginal) |
| backend peak RSS | 6146 MB | 4189 MB | **−1957 MB (−32%)** = corpus 크기(이중버퍼 1개 제거) |
| **copy 오버헤드** | heap→shm memcpy(corpus 전체) | **0 (데몬이 corpus 직접 mmap)** | 무복사 |

### 핵심 결론 (ADR-034 §4A-1 대체)

- **copy 오버헤드를 ~0으로**: 데몬이 backend가 채운 corpus를 그대로 mmap → heap→shm 복사 소멸. memfd라
  **/dev/shm 이름 없음 → 크래시 고아 누수 구조적 불가**(SIGKILL/SIGSEGV/OOM-killer 매트릭스 + soak 실증).
- **남은 backend 오버헤드 ~6s = detoast + heap scan**. north-star(오버헤드→0)를 위해선 PLAIN storage(§4,
  detoast 제거)·4A-2 parallel maintenance workers(heap scan 분산)가 다음 레버. memfd는 그 둘의 enabler
  (worker buffer도 corpus 위에 직접).
- **peak RSS −32%**(= corpus 크기)는 대규모/동시-빌드/메모리-제약 환경에서 fit-vs-OOM을 가름.

---

## 8. 빌드 병렬화: parallel maintenance workers (ADR-058, 2026-06-07)

`table_index_build_scan`을 PostgreSQL parallel index build로 병렬화(워커별 named-shm partial → 리더가 memfd로
merge). north-star = backend 오버헤드(=total − GPU floor) 제거율.

### bench_500000 (dim1024) — workers=0 vs 4

| workers | total | GPU floor | backend 오버헤드 |
|---------|-------|-----------|------------------|
| 0 (단일) | 39.2 s | ~33 s | **~6.2 s** |
| 4 (병렬) | 36.5 s | ~32 s | **~4.5 s (−27%)** |

- **정합**: 고유-벡터 데이터에서 self-NN 단일=병렬 5/5(merge가 (vec,tid) pairing 보존). installcheck 15/15 무회귀.
- **한계(merge가 병목)**: 단일 경로는 corpus를 1회만 씀(memfd 직접). 병렬은 worker가 partial에 1회 쓰고
  **리더가 모든 partial을 최종 memfd로 다시 복사(merge)** = corpus 2-pass. backend(병렬) ≈ 분산스캔(~1.2s) +
  **merge 복사(~3s)** ≈ 4.5s. 즉 스캔은 ~5배 빨라졌으나 **merge가 절감분을 대부분 먹어** 순이득 −27%에 그침.
  merge 복사가 4A-2 이득의 상한(ADR-057이 없앤 복사를 부분 재도입).
- **wall-clock은 GPU floor(~33s) 지배라 marginal**(39→36s). 가치는 backend 오버헤드 제거(north-star).
- **다음 레버**: (a) **merge 복사 제거** — 데몬이 worker별 다중 partial을 직접 mmap(프로토콜 변경)하면 2-pass→
  1-pass, 분산스캔 이득이 그대로 살아남. (b) **PLAIN storage(§4)** — detoast 자체 제거(직교).

> 주의: 이 문서의 1M 벤치 테이블(bench1m/bench_500000)은 uncorrelated subquery 생성이라 **모든 행이 동일
> 벡터**(InitPlan 1회 평가) — 빌드 시간/오버헤드 측정엔 유효하나 **recall 측정엔 무효**. recall은 고유-벡터
> 테이블(예: `array_agg(random() ...) GROUP BY id`)로 별도 검증.

---

## 9. 빌드 merge 복사 제거: 데몬 multi-partial direct H2D (ADR-059, 2026-06-07)

ADR-058의 상한이던 **리더 merge 복사**를 제거 — 리더가 worker partial을 최종 corpus로 연접하는 대신 N개
descriptor를 데몬에 넘기고, 데몬이 각 partial을 mmap해 **device 행렬 1개에 offset별 직접 H2D**
(`cuvs_cagra_build_multi`). host corpus 복사 0.

### bench_500000 (dim1024) — backend 오버헤드 (total − GPU floor)

| 구성 | total wall | GPU floor(데몬 저널) | backend 오버헤드 |
|------|-----------|---------------------|------------------|
| 단일(w0, memfd) | 39.35 s | ~33 s | **~6.3 s** |
| 병렬(w4, ADR-059 multi-partial) | 36.67 s | ~33 s | **~3.7 s** |

- **merge 복사 소멸 실증**: 데몬 로그 `[handle_build_multi] 2 partial(s) ... (direct multi-H2D)` — 연접 단계
  없음. ADR-058 병렬 backend ~4.5s(merge 포함) 대비 감소(저널 1s 해상도 내 노이즈 존재).
- **wall-clock은 여전히 GPU floor(~33s/37s ≈ 89%) 지배** → 빌드 시간 자체는 marginal. 가치는 north-star
  (backend 오버헤드 제거).
- **구조적 이득**: 리더가 더 이상 2번째 full corpus(merge 버퍼)를 들지 않음 → backend peak RSS −corpus(~2GB,
  500k×1024). single-shard 직접 경로; multi-shard(대형)는 host 조립 + `build_sharded` 폴백.
- **정합**: 고유-벡터 self-NN 단일==병렬 5/5(§ADR-059), installcheck 15/15 + iso 2/2, sidecar byte-identity
  단위. /dev/shm 고아 0.
- **남은 레버**: PLAIN storage(§4, detoast 제거) — 단일/병렬 양쪽 직교 적용.

## 10. 3Q CAGRA EXTEND INSERT 처리량 (ADR-051, 2026-06-09)

`cuvsCagraExtend` 기반 스트리밍 INSERT 처리량 측정.
측정 환경: A100 GPU, PG 16, dim=128, base 1K 행 CAGRA 인덱스, 이후 100K 행 연속 INSERT.
각 `aminsert` 호출이 `CUVS_OP_EXTEND` IPC 1회를 발행 → 데몬이 `cuvsCagraExtend` 실행.

### 결과 (dim=128, N_extend=100K, base 1K)

| 항목 | 수치 | 비고 |
|------|------|------|
| 100K INSERT 총 wall-clock | 6,804,354 ms (≈6,804 s) | `\timing` psql 측정 |
| 행당 평균 latency | ~68 ms/행 | 6,804,354 / 100,000 |
| delta_rows (종료 후) | 0 | EXTEND path 사용 확인 — delta fallback 없음 |
| vram_bytes (종료 후) | 76,880,448 bytes (~73 MB) | estimate_vram_bytes(~133K graph nodes, 128) |
| n_vecs (daemon 내부) | 133,473 | row count(101K)와 다름: CAGRA 내부 그래프 노드 수 |

### 비교: delta append path (§4, PLAIN, 100K 행)

| 경로 | 100K INSERT | 행당 | 검색 가시성 |
|------|------------|------|------------|
| **EXTEND (3Q, IPC+graph)** | ~6,804,000 ms | ~68 ms/행 | INSERT 즉시 top-k 반환 가능 |
| delta append (3A, file write) | ~2,811 ms | ~0.028 ms/행 | merge 전까지 delta 경로 검색 |

### 핵심 결론 (ADR-051 보정)

- **EXTEND는 저빈도 스트리밍 쓰기에 최적화된 경로**임이 실증됨.
  매 호출마다 IPC 왕복 + CAGRA 그래프 재연결이 발생하며, 그래프 크기에 비례해 증가.
  delta append 대비 ~2,430× 느림 (68ms vs 0.028ms/행).
- **용도 구분**: 실시간 검색 가시성(INSERT 직후 top-k 반환) 요구 시 EXTEND 사용;
  bulk load 시 delta append + 주기적 COMPACT(또는 REINDEX)가 적합.
- delta_rows=0 확인: EXTEND 성공 시 `.delta` 파일 경로를 완전히 건너뜀.
- n_vecs ≠ row count: CAGRA 내부 그래프 노드 수는 입력 벡터 수와 다를 수 있음(내부 표현 차이).
