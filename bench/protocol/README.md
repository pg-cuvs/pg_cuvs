# bench/protocol — 실행 아키텍처

> **Status: Historical workflow design.** 아래의 `pg-cuvs-dev` 상시 VM,
> self-hosted runner 등록, 웹 에이전트 dispatch 및 results 브랜치 push는 이
> 프로토콜의 당시 운영 가정이다. 현재 인프라가 이 상태인지 이 문서만으로
> 확인할 수 없다. 새 실행은 [`run.sh`](run.sh)의 입력 계약과 실제
> [워크플로우](../../.github/workflows/bench.yml), 실행 환경을 각각 확인한다.

> **Open / Blocked at HEAD `ae818ad`:** `bench/protocol/runner.py`,
> `runner_explain.py`, `runner_concurrency.py`, `runner_incremental.py`,
> `lib/shim_runner.py`가 없는 `infra/anbench`를 import 경로로 사용한다.
> 실제 `observe.py`·`anbench_common.py`·`build_gt.py`는
> `bench/legacy/anbench/`에 있다. CPU 전용 재현
> (`env -u PYTHONPATH python3 bench/protocol/runner.py --selfcheck`)은
> `runner.py:32`의 `ModuleNotFoundError: No module named 'observe'`로 exit 1이다.
> 이 때문에 아래 dispatch 흐름은 이 checkout에서 실행 검증된 절차가 아니다.
> `PGCUVS_DRY_RUN=1`은 import를 건너뛰며, CPU shim도 같은 경로 오류에 걸린다.

> 벤치마크를 **누가 운전하고, 누가 실행하며, 결과가 어떻게 도는가**의 큰 틀.
> 실험 설계(무엇을 재는가)는 [`design/benchmarks/protocol.md`](../../design/benchmarks/protocol.md)(ADR-069),
> 스크립트 인터페이스(어떻게 호출하는가)는 [`CONTRACT.md`](CONTRACT.md).

---

## 1. 세 주체

| 주체 | 성격 | 역할 |
|------|------|------|
| **웹 에이전트** | always-on, GPU·자격증명 없음, ephemeral 컨테이너 | **운전자.** 워크플로우 dispatch · 결과 해석 · 코스트모델 수정 · 다음 run 트리거 |
| **GPU VM** (`pg-cuvs-dev`, A100-40GB) | always-up, 상시 가동 | **실행자.** self-hosted runner가 `run.sh` 실행 · 측정 · 결과 파일 생성 |
| **로컬 세션/사람** | 비상시 | **인프라.** runner 등록 · 워크플로우 스켈레톤 · VM/데이터 준비 · 머지 |

핵심: **웹은 붙어서 지켜보지 않는다.** VM이 독립적으로 일하고, 웹은 킥오프 + 체크인만 한다.
웹 컨테이너가 죽어도 VM의 run은 계속 돌고 결과는 git에 쌓인다.

---

## 2. 채널 — self-hosted GitHub runner

```
웹 에이전트                    GitHub                     GPU VM (always-up)
─────────                     ──────                     ──────────────────
actions_run_trigger  ───────► workflow_dispatch ───────► self-hosted runner
 (GitHub MCP)                  (bench.yml)                 └ bench/protocol/run.sh
                                                              └ 측정 → results/protocol/*.csv
get_file_contents    ◄─────── results 브랜치 push ◄──────── 워크플로우가 commit&push
 (결과 읽기)
웹훅 (실패 시 깨움)  ◄─────── CI 이벤트
```

**왜 이 형태인가**:
- 웹은 `workflow_dispatch` 트리거 + run 로그/상태/결과커밋을 GitHub MCP로 전부 다룰 수 있다.
- **자격증명이 runner에만** 있고 웹 컨테이너엔 안 들어온다(보안).
- 상시 가동 VM에 runner 상주 → GHA spin-up 0, GPU가 바로 옆.
- 설계의 "물리는 VM에서, 웹은 킥오프+체크인"이 그대로 구현된다.

---

## 3. 실행 루프 (적응적)

```
1. 웹: dispatch (Stage A 거친 격자)         → runner 실행 → 결과 push
2. 웹: 결과 CSV 읽기 → ratio 부호전환 구간 식별 (protocol §5.2)
3. 웹: dispatch (refine: 이분점 셀만)        → runner 실행 → 결과 push
4. 반복 → Stage A 물리 완성
5. 웹: Stage B EXPLAIN 스윕 (오프라인, 싸다) → regret 셀 → 코스트모델 수정 → 재스윕
6. 웹: 동결(Stage C) → 이후 Stage D 스위트 dispatch
```

단일 run은 12h가 아니라 **Stage/셀-배치 단위로 종료·체크포인트**한다(CONTRACT §3의 progress 기반 resume).
적응 이분탐색이 곧 "거친 run → 결과 보고 → refine run"의 자연스러운 단위.

---

## 4. 역할 분담 (소유권)

| | 로컬(인프라) | 웹(하네스·운전) |
|---|---|---|
| self-hosted runner 등록 (`pg-cuvs-dev`) | ✅ | |
| 워크플로우 스켈레톤 `.github/workflows/bench.yml` — dispatch inputs · 결과 push 배선 · `concurrency: gpu-singleton` · env 매핑(CONTRACT 표) | ✅ | |
| VM/데이터 준비 (PG·경쟁자·10M Cohere fetch) | ✅ | |
| `bench/protocol/run.sh` + `lib/` + `engines/` (resumable·self-checkpointing) | | ✅ |
| cell 전개 · 자원 수집기 · CSV 기록 · resume 로직 | | ✅ |
| CPU-shim 단위 테스트(plumbing) | | ✅ |
| run 트리거 · 결과 해석 · 코스트모델 수정 | | ✅ |
| 설계 문서 머지 | ✅ | |

**경계**: 워크플로우는 **얇게**(env 주입 + 결과 push), 로직은 **레포 안 스크립트에**(버전관리·테스트 가능).
스크립트는 `results/protocol/` 밑 파일만 쓴다 — **git push는 워크플로우만**(자격증명 격리).

---

## 5. 안전 / 동시성

- **`concurrency: gpu-singleton`** — 단일 A100 더블부킹 방지(한 번에 한 run).
- **progress 기반 resume** — `PGCUVS_RESUME=1`은 해당 run_id의 progress에 기록된 완료 셀만 건너뛴다. CSV 기록 뒤 progress 기록 전 종료하면 중복행이 생길 수 있다.
- **dry-run** — `PGCUVS_DRY_RUN=1`로 GPU 쓰기 전 cell 전개 검증.
- **종료 마커** — `run.sh` stdout 마지막 줄 `PGCUVS_RESULT: status=OK|FAIL` → 웹 진단.

---

## 6. 결과 흐름

`results/protocol/**` (results 브랜치) → 웹이 MCP로 읽음 → 동결(Stage C) 후
[`BENCHMARK.md`](../../BENCHMARK.md) 갱신 + 보정 보고서 `docs/experiments/cost-model-calibration.md`.
원시 CSV는 append-only·불변, 재실측은 새 `run_id`.
