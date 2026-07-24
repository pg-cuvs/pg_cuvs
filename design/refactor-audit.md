# 리팩토링 계획 — 복잡도 / 고아코드 정리

> 출처: 3-에이전트 병렬 코드 점검(고아코드 / 고아모듈 / 스파게티), 2026-06-12.
> 원칙: **Tidy First**(구조변경 <-> 행동변경 분리, 각자 커밋) · 안전한 것부터 · 매 단계 VM 회귀 게이트.
> 빌드/테스트는 전부 VM(PG16): `make installcheck` + `make installcheck-isolation`. 로컬 빌드 불가(daemon/CUDA).

---

## 1. 감사 요약 (2026-06-12)

### 1.1 고아코드 (dead symbols) — 극소
- `xlog_state` (`src/hnsw_export.c:320,420`) — 미사용 로컬 + `(void)` 억제자. 제거. [high]
- `PGV_HNSW_LAYER_M` (`src/hnsw_export.c:91`) — 미사용 매크로. 삭제. [med-high]
- `cuvs_last_dim` (`src/pg_cuvs.c:170,3009,3610`) — write-only로 보고됨. **제거 전 재확인**. [med]
- 클린: `PG_FUNCTION_INFO_V1` 22개 전부 SQL 매칭 · orphan SQL 바인딩 0 · `#if 0` 0.

### 1.2 고아모듈 / 아티팩트
- **정정(에이전트 오류)**: `src/*.o`는 git-추적 안 됨(이미 gitignore) -> 조치 불요. `bench/legacy/anbench/observe.py`는 고아 아님 -> issue #56 bench-protocol 하네스로 통합 중인 WIP.
- **EXTVERSION 불일치**(확인됨): Makefile `EXTVERSION=0.1.0` vs `pg_cuvs.control default_version='0.3.0'` -> Makefile 0.3.0으로.
- 추적된 bench 산출물 4종(`bench/results/cohere_N1000000.jsonl`/`_summary.csv` · `gpu_resources_bench.csv` · `hnsw_import_bench.csv`) — BENCHMARK.md 증거 인용. 의도면 유지, 아니면 untrack.
- **정정 — 벤치 스크립트/문서는 고아 아님**: `bench/legacy/ef_recall_sweep.py` · `infra/scripts/recipes/pgbench-multigpu.sql` · `design/benchmarks/competitive-baseline.md`는 **진행 중인 엄밀 벤치마크 작업(ADR-069 / issue #56 web<->local)의 일부**. observe.py와 동일한 false-positive 클래스(감사 에이전트가 멀티-에이전트 벤치 맥락 부재). **삭제 금지.**
- doc-map 누락 6종(ci-gpu-setup · bruteforce-acceleration-lessons · ecosystem-strategy · filter-threshold-experiment · phase2-* · reports/2026-06-11-prerelease-ci-and-3o) -> doc-map에 historical 항목 추가 **(완료)**.

### 1.3 스파게티 (복잡도) — `pg_cuvs_server.c`에 집중
| 함수 | 위치 | ~줄 | 문제 | 심각도 |
|------|------|-----|------|--------|
| `handle_search` | server.c:2252 | 934 | 6-way 디스패치 + 깊은 중첩 + 2 mutex dance | CRITICAL |
| 핸들러 preamble 복붙 (find/reload/stale/metric/dim) | server.c (4핸들러) | 60x4 | 이미 divergence -> 버그 전파 | HIGH |
| `fill_hnsw_from_hnswlib` | hnsw_export.c:518 | 654 | god-fn 3관심사 | HIGH |
| `build_sharded` | server.c:3706 | 382 | god-fn + tangled cleanup | HIGH |
| `cuvs_ipc_*` 14개 | cuvs_ipc.c | — | 10-18 flat params, 공유 struct 없음 | MED |
| `_PG_init`(415) · `finish_build_commit`(291/11p) · `handle_search_batch`(304) · `cuvs_gettuple`(258) · `cuvs_filtered_knn`(270) | — | — | god-fn / long-params | MED |

`cuvs_wrapper.cu` · `cuvs_objstore.c` · `cuvs_util.c`는 건강(큰데 선형, 본질적 boilerplate).

---

## 2. Tier-1 즉시 정리 (저위험, 릴리스 준비에 흡수)
dead symbols 3건(§1.1) · Makefile `EXTVERSION` -> 0.3.0 · 고아 스크립트/문서 삭제·아카이브 · doc-map 6종 보강. 코드 변경은 dead-symbol 제거뿐 -> `installcheck`로 충분.

---

## 3. 리팩토링 순서

### Step 0 — 게이트 (선행 필수)
VM에서 **6 search_mode x 4 핸들러 preamble**의 `installcheck`/`isolation` 커버리지 확인. 미커버 경로는 해당 단계 전에 테스트 추가. ("회귀로 충분"의 성립 여부를 여기서 확정.)

### Phase A — 무위험 구조 추출 (회귀-안전 · 성능 무영향)
- **A0** (행동, 필요시): preamble **divergence 화해** — `handle_search_stream_bf`의 metric+dim 합친 체크 등 4핸들러를 동일 동작으로. 별도 커밋, 핸들러별 테스트.
- **A1** (구조): 동일해진 블록 -> `resolve_index_for_search()` 추출, 4핸들러에서 호출.
- **A2** (구조): `handle_search`에서 **비-mutex 모드만** 추출(CAGRA base / HNSW-sidecar / ivfpq / cagra_prefilter -> 헬퍼). **2개 mutex dance 블록은 그대로 격리** + 주석 보강.
- **A3** (구조, 독립): `fill_hnsw_from_hnswlib` 3분할(validate/parse/write) · `_PG_init` 분할(guc/hooks/bgworker) · `cuvs_build_cagra_from_heap` advisory 추출.

### Phase B — 파라미터 struct화 (구조 · 콜사이트 다수)
- **B1**: `cuvs_ipc.c` `CuvsSearchParams`/`CuvsBuildParams` 도입(14함수 10-18 params -> 4-5). 에이전트가 본 `delta_merged` 배선 불일치가 실재하면 **행동 서브스텝으로 선분리**.
- **B2**: `finish_build_commit` 11params -> `BuildCommitContext` · `build_sharded` 추출.

### Phase C — 안 함
2개 mutex dance(BF batch enqueue `server.c:2430/2479` · 샤딩 lock-free `2816/2840`): **리팩토링 X, 주석만.** Invariant: 락 순서 index->bf · `inflight` 가드(3G.4/ADR-022) · 재획득 후 `find_index` re-find + fail-closed.

---

## 4. 핵심 결정 / 근거
- **mutex dance는 버그 아님(audit-trap)**. invariant 3겹(lock order / inflight / re-find)이 정확하고 주석도 있음. 동작하는 동시성 코드를 마진 적은 가독성 위해 흔들면 **회귀가 못 잡는 race/UAF** 위험 -> 영구 보류, 주석만.
- **회귀 충분성**: 순수 추출(A1 · A2 비-mutex · A3)은 `installcheck`로 충분. **A0(divergence 화해)은 행동 결정**이라 핸들러별 체크 커버 필요. mutex 부분은 회귀로 불충분(동시성).
- **성능**: latency 무영향(IPC+GPU ~ms가 지배, 함수콜 오버헤드는 노이즈 — overhead-characterization과 일치). throughput은 **lock scope 불변 시** 무영향 -> A2가 dance 블록을 안 건드리는 이유.
- **게이트**: 매 단계 VM `installcheck` + `installcheck-isolation`. A2 직후 **동시 pgbench**(mutex 인접).

---

## 범위 권고
- **A0–A2 우선**: 가치 높고 안전(특히 A1이 버그-전파 위험을 실제 제거).
- **A3 / B**: 품질부채 — 해당 파일 손볼 때 또는 릴리스 후.
- **C**: 영구 보류.
