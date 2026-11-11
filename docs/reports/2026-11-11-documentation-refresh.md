# 문서 최신화 검토 기록 — 2026-11-11

## 기준과 범위

- 소스 기준: `ae818ad03790a1b97542ca0b2b23d352f9fa4027`.
  `git rev-parse HEAD`와 `git ls-remote origin HEAD refs/heads/main`이 같은 값을 반환했다.
- 시작 작업 트리는 깨끗했다. 기존 추적 Markdown **93개**를 대상으로 현행 문장·명령·API·경로와 역사 기록의 상태를 대조했다.
- 확장 버전은 `pg_cuvs.control`, `Makefile`, `src/cuvs_version.h` 모두 **0.9.0**이다.
- 서브에이전트 5개는 `gpt-6-sol` / `low`를 사용했다. 부모가 핵심 문서를 직접 수정하고, 발견의 채택·현재 계약·역사 보존·검증 수준을 판단했다.
- 제품 C/CUDA 코드, SQL 설치 파일, 원본 CSV, frozen phase record는 변경하지 않았다.
  문서 검사 스크립트는 잘못된 버전·예제가 재유입되지 않도록 갱신했다.

| 영역 | 기존 문서 수 | 대조 담당 |
|---|---:|---|
| 핵심 문서 | 5 | 부모 |
| 제품 reference | 4 | `gpt-6-sol`, reasoning `low` |
| 운영 | 18 | `gpt-6-sol`, reasoning `low` |
| 설치·CI·SDK·개발 가이드 | 25 | `gpt-6-sol`, reasoning `low` |
| 벤치마크 | 20 | `gpt-6-sol`, reasoning `low` |
| 설계·역사·전략 | 21 | `gpt-6-sol`, reasoning `low` |

## 주요 수정과 근거

| 항목 | 수정 내용 | 현재 근거 |
|---|---|---|
| 설치와 SQL 예제 | `CREATE EXTENSION vector`, 양쪽 바이너리 설치, preload·재시작·daemon 경로, 실제 통계 컬럼, 실행 가능한 질의 | `pg_cuvs.control`, `Makefile`의 `install-server`, `sql/pg_cuvs--0.9.0.sql` |
| 호환성과 SDK | 기본 CUDA 12.4 개발 bootstrap과 별도 12.9/13.1 패치 SDK 레시피를 구분 | `infra/brev/bootstrap.sh`, `infra/cuvs-patches/build-patched-cuvs.sh` |
| HNSW | source-less heap build의 private op23 파일 경로, PG relation pages와 hnswlib sidecar 구분, global IPC v5와 op23 payload v5 구분 | `src/hnsw_export.c:cuvs_hnsw_ambuild`, `src/pg_cuvs.c:cuvs_build_hnsw_from_heap`, `src/cuvs_ipc.h` |
| 실패·쓰기·복구 | 실행 중 OOM/연결 실패/STALE은 ERROR이며 후속 CPU 재계획과 구별. CAGRA EXTEND 우선, flat delta, IVF-PQ INSERT no-op, marker 유실과 tombstone 잔여 설명 | `src/pg_cuvs.c:cuvs_gettuple`, `cuvs_aminsert`, `ivfpq_aminsert`, `cuvs_report_stale`, `test/stale_interim.py` |
| GCS 설정 | backend GUC는 standalone daemon 설정으로 전달되지 않음. 실제 daemon flags 안내 | `src/pg_cuvs.c:_PG_init`, `src/pg_cuvs_server.c`의 argument parser와 snapshot job 구성 |
| 메모리·용량 | host build budget과 GPU budget 분리, scratch 9593, RMM quota, AR8, physical-free 32 MiB preflight 및 비예약 한계 | `src/pg_cuvs_server.c`, `src/cuvs_admission_contract.h`, `src/cuvs_util.c:cuvs_auto_shard_count` |
| 검색 의미 | `cuvs.k`와 SQL LIMIT 구분, float16 exhaustive search가 원래 float32 순위를 보장하지 않음 | `src/pg_cuvs.c`, `src/cuvs_wrapper.cu` BF corpus/query conversion |
| CI·운영 명령 | 현행 runner·trigger·WIF 없는 경로, regression subset, 배포·업그레이드 0.9.0, 폐기된 명령과 잘못된 쉘 치환 수정 | `.github/workflows/ci.yml`, `gpu.yml`, `Makefile`, `infra/scripts/setup/` |
| 벤치마크 | 서로 다른 측정 셀의 잔차 비율을 시간적 IPC 증가로 해석하지 않음. projection과 실측 구별, 현재 runner/CSV/manifest 계약 반영 | `BENCHMARK.md`, `bench/protocol/run.sh`, `runner*.py`, `bench/legacy/anbench/observe.py`, versioned CSV |
| 로드맵 | #124 코드 해결, #88 측정 완료, #202 잔여 correctness를 구분. 완료된 구현을 미구현으로 나열한 부분 정정 | 원격 issue readback, 현재 dispatch guard·short-fill 경로·stale 경로 |
| 역사 링크 | ADR-085의 없는 local RMM 보고서 링크만 원본이 존재하는 고정 커밋 URL로 복구. 결정 본문과 수치는 유지 | GitHub contents API: commit `8688bf8b9feff9d6d7deaae6f5272deb69926a1c`, blob `26ef0fd6bd7cf0bbef0ff205029095f8534662d8` |

## 검증

- `make docs-contract-check test-lw-docs test-i3e-sql`: PASS.
  문서 계약, I4 flag 기본값·범위, I3-E SQL/view·권한·문서 컬럼 대조가 모두 통과했다.
- `git diff --check`: PASS.
- 새 보고서를 포함한 94개 문서의 Markdown 상대 링크 276개와 로컬 heading anchor 12개: 결손 0.
- 현재 안내 문서의 `cuvs.*` 이름을 C 등록부와 대조: 등록 GUC 45개, 미등록 이름 0.
- 새 문서 검사에 잘못된 확장 이름, 통계 컬럼, SQL authority 버전,
  로드맵 버전, 재설치 버전을 각각 주입한 임시 복사본: **5/5 거부**.
  baseline과 복구된 복사본은 통과했다. 실제 작업 트리에는 변이를 넣지 않았다.
- 부모 작성 핵심 문서 4개의 변경 사실을 별도 서브에이전트가 정적으로 확인했다.
  최종 채택과 후속 수정 확인은 부모가 수행했다.

## 검증의 한계

PostgreSQL·daemon·GPU 실행, SDK 재빌드, GitHub Actions 재실행, VM 배포는
이번 문서 작업에서 **NOT RUN**이다. 정적 소스 대조를 새 runtime 결과로 표시하지
않았다. 과거 측정은 해당 날짜·데이터·호스트·버전의 증거로 보존했다. 외부 제품의
최신 시장 상태나 서비스 가용성은 이 작업의 현재 제품 계약에 사용하지 않았다.

`#202`의 marker 내구성·공유 상한·IVF-PQ tombstone 복구, 실제 standby 운영,
추가 GPU/SDK 조합의 품질·메모리 검증은 제품·실험의 잔여 항목이다. 문서 수정으로
해결됐다고 표시하지 않았다. GCP 레거시 bootstrap과 현재 remote environment
계약의 불일치도 코드 해결로 표시하지 않고 해당 가이드에 전제 미충족을 명시했다.

벤치마크 runner의 `infra/anbench` 참조와 실제 `bench/legacy/anbench` 위치도
불일치한다. `env -u PYTHONPATH python3 bench/protocol/runner.py --selfcheck`는
`runner.py:32`에서 `ModuleNotFoundError: No module named 'observe'`로 종료했다
(exit 1). 이 CPU-only 실행 실패는 현재 checkout의 벤치마크 전제 결함으로
기록했고, 문서 갱신으로 runner 코드가 해결됐다고 표시하지 않았다.

## 기존 문서 전수 목록

`수정`은 이 문서 갱신의 diff가 있는 파일, `보존`은 현행 대조 또는 역사·증거
상태 확인 후 본문을 유지한 파일이다. 보존은 모든 과거 수치의 재측정이라는 뜻이 아니다.

### 핵심 문서

- `ARCHITECTURE.md` — 수정
- `CLAUDE.md` — 보존
- `README.md` — 수정
- `ROADMAP.md` — 수정
- `docs/doc-map.md` — 수정

### 제품 reference

- `docs/best-practices.md` — 수정
- `docs/hnsw-build-options.md` — 수정
- `docs/operational-guide.md` — 수정
- `docs/reference.md` — 수정

### 운영

- `design/ops-gpu-playbook.md` — 수정
- `docs/playbooks/README.md` — 수정
- `docs/playbooks/benchmark-runbook.md` — 수정
- `docs/playbooks/capacity-planning.md` — 수정
- `docs/playbooks/create-index-failure-diagnosis.md` — 수정
- `docs/playbooks/daemon-restart-recovery.md` — 수정
- `docs/playbooks/drop-and-write-path-diagnosis.md` — 수정
- `docs/playbooks/gcs-snapshot-ops.md` — 수정
- `docs/playbooks/gpu-vm-build-and-test.md` — 수정
- `docs/playbooks/gpu-vm-lifecycle.md` — 수정
- `docs/playbooks/jit-threshold-sweep.md` — 수정
- `docs/playbooks/large-dataset-benchmark.md` — 수정
- `docs/playbooks/multi-gpu-sharding-ops.md` — 수정
- `docs/playbooks/persistence-corruption-recovery.md` — 수정
- `docs/playbooks/release-upgrade.md` — 수정
- `docs/playbooks/replica-bootstrap.md` — 수정
- `docs/playbooks/rollback-and-cleanup.md` — 수정
- `docs/playbooks/vram-oom-fallback.md` — 수정

### 설치·CI·SDK·개발 가이드

- `.claude/skills/gpu-vm-provision/SKILL.md` — 보존
- `.claude/skills/gpu-vm-provision/references/cuvs-26x-quirks.md` — 수정
- `.claude/skills/gpu-vm-provision/references/failures/cuvs-build-errors.md` — 보존
- `.claude/skills/gpu-vm-provision/references/failures/cuvs-package-not-found.md` — 수정
- `.claude/skills/gpu-vm-provision/references/failures/cuvs-serialize-sigsegv.md` — 보존
- `.claude/skills/gpu-vm-provision/references/failures/disk-recovery.md` — 보존
- `.claude/skills/gpu-vm-provision/references/failures/ldconfig-disaster.md` — 보존
- `.claude/skills/gpu-vm-provision/references/failures/libstdcpp-version.md` — 보존
- `.claude/skills/gpu-vm-provision/references/failures/runtime-gotchas.md` — 보존
- `.claude/skills/gpu-vm-provision/references/failures/stockout.md` — 보존
- `.claude/skills/gpu-vm-provision/references/quick-start.md` — 수정
- `.claude/skills/gpu-vm-provision/references/troubleshooting.md` — 보존
- `.claude/skills/pg-cuvs-dev/SKILL.md` — 보존
- `.claude/skills/quality-loop/PATTERNS.md` — 수정
- `.claude/skills/quality-loop/SKILL.md` — 수정
- `CONTRIBUTING.md` — 수정
- `design/ci-strategy.md` — 수정
- `docs/ci-gpu-setup.md` — 수정
- `infra/README.md` — 수정
- `infra/brev/README.md` — 보존
- `infra/cuvs-patches/0003-hnsw-memory-guards.md` — 보존
- `infra/cuvs-patches/0004-hnsw-upper-precision.md` — 보존
- `infra/cuvs-patches/README.md` — 보존
- `scratch/README.md` — 보존
- `test/native/hnsw-sdk/ace-ip/README.md` — 수정

### 벤치마크

- `BENCHMARK.md` — 수정
- `bench/README.md` — 보존
- `bench/cuvs_bench_backend/README.md` — 수정
- `bench/legacy/README.md` — 보존
- `bench/profiling/README.md` — 보존
- `bench/protocol/CONTRACT.md` — 수정
- `bench/protocol/HANDOFF.md` — 수정
- `bench/protocol/README.md` — 수정
- `bench/results/README.md` — 수정
- `bench/results/connection-preflight-20261021/README.md` — 보존
- `design/benchmarks/competitive-baseline.md` — 수정
- `design/benchmarks/crossover-methodology.md` — 수정
- `design/benchmarks/protocol.md` — 수정
- `docs/experiments/benchmark-archive.md` — 보존
- `docs/experiments/bruteforce-acceleration-lessons.md` — 수정
- `docs/experiments/concurrency-comparison.md` — 수정
- `docs/experiments/cost-model-calibration.md` — 수정
- `docs/experiments/filter-threshold-experiment.md` — 보존
- `docs/experiments/pgstrom-comparison.md` — 수정
- `docs/experiments/profiling-results.md` — 수정

### 설계·역사·전략

- `design/README.md` — 수정
- `design/decisions.md` — 수정
- `design/pr-drafts/I3-B.md` — 수정
- `design/refactor-audit.md` — 수정
- `design/specs/phase-record.md` — 보존
- `design/specs/requirements.md` — 보존
- `design/spikes/3b-diskann-decision.md` — 보존
- `design/spikes/3b-diskann-spike.md` — 보존
- `design/strategy/positioning.md` — 수정
- `design/strategy/workload-notes.md` — 수정
- `docs/history/phase2-exit-criteria.md` — 보존
- `docs/history/phase2-test-matrix.md` — 보존
- `docs/history/spec-audit-2026-06-05.md` — 보존
- `docs/reports/2026-06-11-prerelease-ci-and-3o.md` — 보존
- `docs/reports/2026-06-11-resource-governance-audit.md` — 보존
- `docs/reports/2026-06-17-stage-d3-concurrent.md` — 보존
- `docs/reports/2026-08-04-3o-correlation-axis.md` — 수정
- `docs/reports/2026-08-04-aisaq-gds-viability.md` — 보존
- `docs/reports/2026-08-04-item2b-second-host-replication.md` — 보존
- `docs/strategy/ecosystem-strategy.md` — 수정
- `docs/strategy/levers-and-governance.md` — 수정
