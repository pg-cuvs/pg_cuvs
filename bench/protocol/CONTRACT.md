# bench/protocol — 스크립트 인터페이스 계약

> **Status: Current script interface at HEAD `ae818ad`.** 실행 환경의 VM 등록·
> 설치 상태는 별도로 확인한다. 구현 근거는 `run.sh`, `engines/_common.sh`,
> `runner*.py`, `bench/legacy/anbench/observe.py`, `.github/workflows/bench.yml`이다.
> **Open / Blocked in this checkout:** 이 인터페이스의 실행 전제가 현재 깨져 있다.
> `runner.py`, `runner_explain.py`, `runner_concurrency.py`,
> `runner_incremental.py`, `lib/shim_runner.py`는 `observe`를 top-level import하면서
> 존재하지 않는 `infra/anbench`를 `sys.path`에 넣는다. 실제 정본은
> `bench/legacy/anbench/{observe,anbench_common,build_gt}.py`에 있다.
> `env -u PYTHONPATH python3 bench/protocol/runner.py --selfcheck`는
> `runner.py:32`에서 `ModuleNotFoundError: No module named 'observe'`로 exit 1이다.
> 따라서 아래 계약은 코드가 의도한 입출력 형식이며, 이 checkout에서의
> 정상 실행을 검증한 절차가 아니다. 이 오류를 가리는 외부 `PYTHONPATH` 주입은
> 현재 스크립트 자체의 경로 정합성을 입증하지 않는다.

> 워크플로우 스켈레톤(로컬)과 하네스 스크립트(웹)가 **독립적으로 작업해도 맞물리도록** 고정한 계약.
> 실행 아키텍처는 [`README.md`](README.md), 실험 설계는
> [`design/benchmarks/protocol.md`](../../design/benchmarks/protocol.md)(ADR-069).

---

## 1. 디렉터리 / 진입점

```
bench/legacy/anbench/observe.py          # 자원 샘플러 + protocol-CSV 라이터 (import 모듈)
bench/protocol/
  run.sh                  # 단일 진입점. 워크플로우는 이것만 호출 (CSV 안 씀)
  lib/
    resolve_cells.sh      # env → cell 목록 전개
    shim_runner.py        # CPU-shim 자리표시 측정(observe 경유 1행 기록)
  engines/*.sh            # 현재 config 목록은 디렉터리와 _common.sh 참조
results/protocol/                     # repo root; PGCUVS_RESULTS_ROOT로 재지정 가능
  <stage>.csv                     # §6 스키마, append-only, run_id 컬럼 (observe.protocol_path)
  <stage>.<run_id>.progress       # 완료 cell|config 목록 (resume용)
  <stage>.<run_id>.manifest.json  # run.sh의 dispatch 메타데이터 (§3)
```

**경계 (observe.py 정합, issue #56):**
- **`observe.py`**(bench/legacy/anbench) = 자원 샘플링(`ResourceSampler`) + protocol-CSV 라이터
  (`write_protocol_row`). `PROTOCOL_FIELDS`가 **CSV 단일 진실**. 별도 `collect.sh` 없음.
- **per-engine 러너**(`runner.py`/`runner_concurrency.py`/`runner_explain.py`/
  `runner_incremental.py`, `engines/<config>.sh`가 호출) = observe를 import,
  `with ResourceSampler(...)`로 실제 build/query를 감싸고 `write_protocol_row(...)` 호출.
  **CSV 쓰기는 러너 안**, run.sh가 아니다.
- **`run.sh`** = cell 전개 · 디스패치 · resume(`.progress`) · dry-run · `engines/<config>.sh` 호출 ·
  `PGCUVS_RESULT` 마커. **CSV를 직접 쓰지 않는다.**

`run.sh`의 결과 루트 기본값은 repo-root `results/protocol/`이다. 러너가
GT·합성 corpus도 `PGCUVS_GT_DIR` 아래 생성할 수 있다. 결과 branch push는
워크플로우가 수행한다(§8).

---

## 2. 입력 (워크플로우 → 스크립트, 전부 env)

워크플로우가 제공하는 입력은 `.github/workflows/bench.yml`에서 env로 매핑한다.
직접 호출 시 `run.sh`가 읽는 값도 아래와 같다.

| env | 값 | 비고 |
|-----|----|----|
| `PGCUVS_STAGE` | `A`\|`B`\|`C`\|`D` | |
| `PGCUVS_MODULE` | 기본 `physics`; `_common.sh`는 `concurrency`→`runner_concurrency.py`, `explain`→`runner_explain.py`, `incremental`→`runner_incremental.py` | 그 외 값은 `runner.py`로 전달; 이름만으로 별도 모듈 실행을 보장하지 않음 |
| `PGCUVS_CELLS` | `N=1k,10k,100k,1m;dim=1024;k=10;recall=0.95,0.99` | `resolve_cells.sh`가 전개 |
| `PGCUVS_CONFIGS` | 기본 `forced-hnsw,forced-cuvs,auto`; 실제 config는 `engines/*.sh`와 해당 runner의 구현 범위 | `forced-bf`는 현재 엔진명이 아님 |
| `PGCUVS_BASELINE` | `same-box`\|`iso-$` | §protocol 2 |
| `PGCUVS_DATASET` | `cohere-1m`\|`cohere-10m`\|`synth-clustered`\|`synth-random` | |
| `PGCUVS_REPS` | 기본 `5` | latency 셀 반복 |
| `PGCUVS_RUN_ID` | 워크플로우: `gha-<run_id>-<run_attempt>`; 직접 호출: UTC 시각 기반 `local-...` | 출력 경로·CSV 행에 박힘 |
| `PGCUVS_DRY_RUN` | `1`=cell 전개만 출력하고 exit 0; CSV·progress·manifest는 쓰지 않음 | `results/protocol/` 디렉터리는 `mkdir -p`로 생성될 수 있음 |
| `PGCUVS_RESUME` | `1`=해당 run_id의 `.progress`에 기록된 cell×config만 스킵 | CSV 중복 제거·원자적 재실행 보장은 아님 |
| `PGCUVS_COST_MODEL_VERSION` | 문자열 | 모든 행에 스탬프 (§protocol 6.4) |
| `PGCUVS_RUNTIME_ROUTING_VERSION` | 문자열 | 동일 |
| `PGHOST/PGPORT/PGDATABASE/PGUSER` | 접속 | conda 활성화·`CUVS_INDEX_DIR`은 runner가 |
| `PGCUVS_CPU_SHIM` | `1`=GPU 없이 plumbing 검증 | §7 |

---

## 3. 출력 계약

- **셀×config 1개 = dispatch/checkpoint 단위**. 러너가
  `observe.write_protocol_row`로 CSV 행을 append한 뒤, 성공 반환 시 `run.sh`가
  `.progress`에 `cell|config`를 append하고 그 파일을 `sync -f`한다(실패 시
  `sync` 폴백). 두 append는 별개 작업이다. CSV 기록 후 progress 기록 전
  종료되면 재실행에서 같은 셀을 다시 측정해 CSV 중복행이 생길 수 있다.
  `PGCUVS_RESUME=1`은 **해당 run_id의 progress에 이미 있는 셀만** 건너뛴다.
  CSV 중복 검사나 두 파일의 원자적 커밋은 구현되어 있지 않다.
  러너 비호출(shim 제외) 시 `run.sh`는 CSV를 직접 쓰지 않는다.
- **종료 마커**를 stdout 마지막 줄에:
  `PGCUVS_RESULT: status=OK|FAIL cells_done=N/M` (`dry-run`은 `status=OK` 뒤에 `(dry-run)`) → 로그 진단용. `run.sh`는 OOM을 별도 분류하지 않는다.
- **manifest** (`<stage>.<run_id>.manifest.json`): `run.sh`가 기록하는 값은
  run_id/stage/module/baseline/dataset/reps/cpu_shim, 비용·라우팅 버전 문자열,
  CSV 경로, 시작 시각, 총 셀 수다. `pg_settings`, 바이너리/드라이버 버전,
  `gt_method`, 종료 상태는 이 파일의 현재 기록 계약에 포함되지 않는다.
  protocol §12의 전체 재현성 항목은 별도 확인이 필요하다.

---

## 4. per-engine 테이블 분리 (플랜 강제)

`runner.py`는 config별 테이블에 같은 corpus를 적재한다. 각 테이블의
인덱스·무인덱스 경로는 다음과 같다(`runner.py:TABLES`, `build_index`):

| 테이블 | 인덱스 | config |
|--------|--------|--------|
| `t_hnsw` | pgvector HNSW만 | `forced-hnsw` |
| `t_cuvs` | pg_cuvs cagra만 | `forced-cuvs` |
| `t_flat` | pg_cuvs flat만 | `forced-flat` |
| `t_ivfpq` | pg_cuvs ivfpq만 | `forced-ivfpq` |
| `t_tbf` | 없음 | `forced-transient-bf` |
| `t_seq` | 없음 | `forced-seqscan` |
| `t_auto` | pg_cuvs cagra만 | `auto`(seqscan↔cagra planner 선택) |

- **같은 PG 인스턴스 · 같은 데이터 · 같은 데몬 1개.** 별도 PG를 띄우는 게 **아니다**.
  pgvector·pg_cuvs는 한 PG의 두 확장으로 공존.
- 빌드 phase는 어차피 엔진별로 따로 재므로 자연스럽다.
- 데몬 PID는 GPU config에서 `_common.sh`가 조회해 sampler에 전달한다.
  `forced-hnsw`의 idle daemon footprint를 manifest에 기록하거나
  `PGCUVS_DAEMON_DOWN_FOR_CPU`로 데몬을 내리는 기능은 구현되어 있지 않다.
- `PGCUVS_BASELINE=iso-$`는 행의 baseline 값이다. 이 입력만으로 별도 CPU
  인스턴스가 생성·측정되지는 않는다.

---

## 5. iso-recall 등화 (config별 노브 sweep)

엔진 러너(`engines/<config>.sh`→python)가 각 (N, recall) 셀에서 노브를 sweep해 target 만족
**최소값**을 채택한다(`ef_search`/`probes`/`cuvs.k`). 상한 미달이면 달성 recall 병기 + 미달 표기.
BF 모드는 recall=1.0 — 등화 없이 별도 행. 상세 protocol §3.3.

---

## 6. 결과 CSV 스키마

**단일 진실 = `bench/legacy/anbench/observe.py`의 `PROTOCOL_FIELDS`** (protocol §4와 동일).
헤더/순서는 그 리스트가 결정하며 `observe.write_protocol_row`가 RFC-4180(CRLF)로 기록한다.
아래는 참조용 복제:

```
run_id, date, stage, phase, cell_id, config,
system, system_version, system_commit, index_type,
N, dim, k, recall_target, dataset, query_set_id, seed, clients, warm_state,
build_s, qps, p50_us, p95_us, p99_us, p999_us, avg_latency_us, sla_bounded_qps, recall_at_k,
peak_vram_mb, peak_rss_mb, cpu_core_s, gpu_s, energy_j,
disk_bytes_written, wal_bytes, index_bytes_vram, index_bytes_host, index_bytes_disk,
instance_type, price_usd_hr, usd_per_1m_queries,
reps, agg_method, dispersion, gt_method,
cost_model_version, runtime_routing_version,
selectivity, correlation, filter_mode,        # M1 (PGCUVS_MODULE=filter)
stream_op, ops_done, delta_rows,              # M2 (PGCUVS_MODULE=incremental)
params_json, notes
```

`PGCUVS_MODULE=explain`은 `runner_explain.py`가 같은 stage CSV에
`phase=explain` 행을 쓴다. `params_json`에 engine, phys_cost, hw profile,
chosen path와 seq/index startup·total cost를 담으며, 별도의
`planner_est/<run_id>.csv`를 만들지 않는다.

---

## 7. CPU-shim 테스트 가능성

`PGCUVS_CPU_SHIM=1`에서 GPU 없이 돌아야 한다(Tier-1 shim 재사용). 측정값은 무의미하지만
**배선(cell 열거 · CSV 행 형식 · `.progress` 기반 완료 셀 건너뛰기 · manifest · 종료 마커)은 검증 가능**해야
한다. 현재 `lib/shim_runner.py:17-18`도 없는 `infra/anbench`를 참조하므로
`observe` import에서 막힌다. `run.sh`의 dry-run은 shim을 호출하지 않아 이
결함을 드러내지 못한다. `lib/selftest.sh`는 별도로
`bench/legacy/anbench`를 import 경로에 넣지만, 그 경로가 맞는 것만으로
shim이나 real runner의 실행을 입증하지 못한다. 이 경로의 CPU-shim 성공은
이 checkout에서 확인되지 않았다.

---

## 8. 현행 워크플로우 (`.github/workflows/bench.yml`)

1. `runs-on: [self-hosted, gpu, a100]` — 이 라벨의 runner가 필요하다. 실제 등록·가동 여부는 별도 확인.
2. `workflow_dispatch` inputs → §2 env 매핑(이름 그대로).
3. `bench-results/protocol` 브랜치에 `results/protocol/**`를 commit & push한다.
   publish 단계는 원격 `results/protocol`을 지우고 이번 run의 디렉터리로
   교체한다. 따라서 원격 branch를 run 간 append-only 저장소로 보지 않는다.
   `dry_run`/`cpu_shim`이면 publish하지 않는다.
4. `concurrency: { group: gpu-singleton, cancel-in-progress: false }`.
5. 워크플로우 파일은 **default 브랜치(main)에 존재**해야 웹이 API로 dispatch 가능.
6. conda 활성화 + `CUVS_INDEX_DIR` 등 runner 환경은 step에서 세팅 후 `run.sh` 호출.
   `build=true`이면 확장과 daemon을 설치·재시작한다.

---

## 9. VM 환경 (engines) — `_common.sh` 기본값

`engines/<config>.sh`는 conda 활성화 후 §2에 적은 module별 runner를 exec한다. VM 사실(issue #56)을
기본값으로 두고 bench.yml env로 override 가능:

| env | 기본값 (pg-cuvs-dev) | 용도 |
|-----|----------------------|------|
| `PGCUVS_CONDA` / `PGCUVS_ENV` | `~/miniforge3` / `cuvs_py` | numpy+psycopg+pgvector(+GPU) |
| `PGCUVS_CORPUS` | `~/anbench/data/corpus.fbin` | 적재 corpus |
| `PGCUVS_QUERIES` | `~/anbench/data/queries_10k.fbin` | 쿼리셋 |
| `PGCUVS_GT_DIR` | `~/anbench/data` | `gt_<N>.npy` 조회 |
| `CUVS_INDEX_DIR` | `/tmp/cuvs_indexes` | **데몬 `--index-dir`와 일치 필수** (§아래) |
| `PGDATABASE` | `bench` | 작업 DB |
| `PGCUVS_INSTANCE_TYPE` / `PGCUVS_PRICE_USD_HR` | (없음) | CSV 경제 컬럼 |

데몬 pid는 `systemctl show -p MainPID --value pg-cuvs-server`로 발견 → `ResourceSampler(daemon_pid=)`에
주입(host RSS 합산).

### cuvs 플랜 가드 (issue #56 — 치명적)
`forced-cuvs`/`auto`는 세션마다 `SET cuvs.index_dir='<daemon --index-dir>'` **필수**. 불일치 시
backend가 `$PGDATA/cuvs_indexes`를 보고 아티팩트를 못 찾아 **조용히 Seq Scan 폴백**(GPU 없음·에러
없음) → pg_cuvs를 처참히 느린 것으로 오측정. `runner.py`는 `forced-cuvs`에서 빌드 후 **EXPLAIN으로 cagra 경로를
확인**하고, 불일치하면 셀을 실패 처리한다. `auto`는 계획을 강제하지
않으며 `cagra`·`seqscan`·`other` 선택을 `params_json.chosen_plan`에 기록한다.

### GT 가용성
`runner.py`는 corpus·query·N에 맞춘 `gt_runner_<N>[_d<dim>].npy`를
`PGCUVS_GT_DIR`에서 찾고, 없으면 `ANBENCH/build_gt.py`를 호출하도록 작성됐다.
하지만 `runner.py:30`의 `ANBENCH`는 없는 `infra/anbench`로 계산된다.
실제 `build_gt.py`는 `bench/legacy/anbench/build_gt.py`에 있으므로,
현재 checkout에서는 import 문제를 넘겨도 이 자동생성 경로가 성립하지 않는다.
`runner_concurrency.py`는 `gt_runner_<N>.npy`가 없으면 종료하므로 해당 N의
physics runner를 먼저 실행하도록 설계됐다. 다만 두 runner 모두 위
`observe` import에서 먼저 막히므로 이 선행 절차는 현재 실행 불가다.
