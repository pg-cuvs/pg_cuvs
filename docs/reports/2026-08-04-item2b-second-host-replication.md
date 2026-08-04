# 2026-08-04 — #89 item 2: second-host replication (A100-SXM4-80GB DGX)

`#89`이 지적한 두 evidence gap(단일 호스트·단일 run 근거)을 두 번째 GPU 호스트에서
재측정해 해소한다.

## 환경

| 항목 | 값 |
|------|----|
| 2nd host | `massedcompute_A100_sxm4_80G_DGX` (Brev, sm_80, 80GB) |
| canonical | massedcompute A100 SXM4 80G (`BENCHMARK.md` §2.1b, `bench/results/adr079_3o_recall_crossover.csv`) |
| dataset | wiki_all_1M (1M×768), k=10, 100 queries, selectivity {0.008, 0.005, 0.003, 0.002} |

## item 2-a — OOM-injection flake는 environment-dependent (실증)

`build_oom` / `build_multi_oom` / `build_oom_evict_to_fit`:
- 단독 5회 + full-suite(38 SQL + 6 isolation) 3회 = **8/8 green**.
- 옛 dev VM(A100-**40GB**) full-suite에서 관찰된 1~3 failures flake가 **80GB에서 재현되지 않음**.

→ flake는 코드 회귀가 아니라 **VRAM 용량/residency 압박에 의존**하는 환경 특성. 다른
하드웨어(80GB)에서 flake가 사라지는 것이 environment-dependence의 직접 증거다. 세 테스트에
추가한 마킹(main, `test/sql/build_oom*.sql` + `Makefile`)이 정당함이 실측으로 확인됐다.

## item 2-b (1) — recall은 호스트 간 재현됨

| sel | path | 2nd-host recall | canonical recall |
|-----|------|-----------------|------------------|
| 0.008 | 3O | 0.992 | 0.994 |
| 0.005 | 3O | 0.996 | 0.997 |
| 0.003 | 3O | 0.990 | 0.993 |
| 0.002 | 3O | 0.997 | 0.993 |

D-wedge / stream_bf는 양쪽 모두 ~1.0(exact). recall Δ ≤ 0.005 → **recall은 하드웨어
독립**(알고리즘·데이터 속성). single-run 우려 해소, recall 수치는 신뢰 가능.

## item 2-b (2) — latency crossover는 호스트 간 이동함

| sel | stream_bf 2nd-host | stream_bf canonical | D-wedge 2nd-host |
|-----|--------------------|---------------------|------------------|
| 0.005 | 13.4 ms | 6.5 ms | 3.8 ms |
| 0.003 | 8.7 ms | 3.1 ms | 3.8 ms |
| 0.002 | 5.7 ms | 2.6 ms | 4.0 ms |

- **canonical crossover**(stream_bf가 D-wedge보다 빨라지는 지점) ≈ **0.003-0.004**.
- **2nd-host**: 측정 구간(0.002-0.008) 전체에서 stream_bf가 D-wedge를 못 이김 → **crossover가
  <0.002로 이동**.
- 원인: stream_bf는 `.vectors` sidecar에서 `pread`로 gather하는 **디스크 IO 의존** 경로라
  호스트 스토리지 특성에 민감하다. 3O/D-wedge는 VRAM 상주라 IO 무관 → 재현된다.

→ `#89`의 예측(*"the D-wedge/stream_bf crossover (~0.004) ... should be expected to move on
different data"*)이 **같은 데이터·다른 호스트**에서도 성립함을 실증. **crossover ~0.004는
하드웨어별 단일 상수가 아니라 호스트 의존 값**이며, 절대값을 고정 상수로 인용하면 안 된다.

## 결론

| gap | 판정 |
|-----|------|
| OOM flake가 코드 회귀인가 | 아니오 — VRAM 용량 의존 (80GB 8/8 green, 40GB flake). 마킹 정당. |
| recall 수치 신뢰 가능한가 | 예 — 호스트 간 재현(±0.005). |
| latency crossover ~0.004 고정 상수인가 | 아니오 — 호스트 이동(stream_bf IO 의존). 고정 인용 금지. |

데이터: `bench/results/adr079_3o_recall_2ndhost_dgx80.csv` (canonical:
`bench/results/adr079_3o_recall_crossover.csv`).
