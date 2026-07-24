# Document Map

pg_cuvs documentation splits into three layers. The distinction that matters: **current-state
SSOT** (what the product *is* now) vs **historical record** (how it got built and why) vs
**operational** (how to run it). When a fact and a historical doc disagree, the SSOT wins.

## Current-state SSOT — "what it is now"

Maintained to reflect the shipping product. Start here.

| Doc | Answers |
|-----|---------|
| [README.md](../README.md) | Overview, install, requirements, quickstart |
| [ARCHITECTURE.md](../ARCHITECTURE.md) | How it works: components, IPC, index lifecycle (incl. `flat` AM build/search/evict/restart), VRAM accounting, sharding, GCS, write path, key techniques, limitations |
| [docs/reference.md](reference.md) | The surface: index AMs (`cagra`, `flat`, `ivfpq`, `pg_cuvs_hnsw`), search modes, GUCs, reloptions, SQL functions, observability views |
| [docs/operational-guide.md](operational-guide.md) | Workload selection guide: flat vs cagra vs ivfpq, measured latency/throughput, crossovers, cost characteristics |
| [docs/best-practices.md](best-practices.md) | Build-time recommendations (TOAST/PLAIN, index_dir placement, version pinning) |
| [BENCHMARK.md](../BENCHMARK.md) | Published performance + overhead characterization |

## Historical record — "how it was built / why"

Preserved for provenance. **Not** kept in sync with the current product; read as history.

| Doc | Role |
|-----|------|
| [design/decisions.md](../design/decisions.md) | ADRs — every design decision, alternatives, rejection reasons. The "why" of record. Key recent: **ADR-073** (`flat` AM, supersedes ADR-071), ADR-072 (DiskANN direction), ADR-070 (resource governance) |
| [design/specs/phase-record.md](../design/specs/phase-record.md) | **Frozen.** Per-phase as-built spec + completion criteria + verification evidence. Its planning role ended when implementation completed |
| [design/specs/requirements.md](../design/specs/requirements.md), [design/strategy/positioning.md](../design/strategy/positioning.md) | Earlier requirements / positioning; the SSOT docs above supersede them for current state |
| [design/strategy/workload-notes.md](../design/strategy/workload-notes.md) | The analysis behind ADR-061 (workload-target repositioning); **cited by decisions.md §G/§H as the "why."** Not superseded — it is the record of that reasoning |
| [design/benchmarks/crossover-methodology.md](../design/benchmarks/crossover-methodology.md), [docs/experiments/profiling-results.md](experiments/profiling-results.md) | Measurement methodology + raw profiling that BENCHMARK.md cites |
| [design/spikes/](../design/spikes/), [docs/history/](history/) | Spike/decision records for specific phases (3B DiskANN go/no-go) + phase-2 audits |
| [docs/experiments/filter-threshold-experiment.md](experiments/filter-threshold-experiment.md), [docs/experiments/bruteforce-acceleration-lessons.md](experiments/bruteforce-acceleration-lessons.md), [docs/history/phase2-exit-criteria.md](history/phase2-exit-criteria.md), [docs/history/phase2-test-matrix.md](history/phase2-test-matrix.md), [docs/strategy/ecosystem-strategy.md](strategy/ecosystem-strategy.md), [docs/reports/](reports/) | Experiment results, lessons, phase-completion criteria, ecosystem strategy (ADR-062), prerelease reports |

## Active planning — "what's next"

| Doc | Role |
|-----|------|
| [ROADMAP.md](../ROADMAP.md) | Sequence of remaining work + trigger-based backlog. New sequencing goes here, not phase-record.md |
| [design/benchmarks/protocol.md](../design/benchmarks/protocol.md) | The rigorous benchmark + cost-model calibration protocol (ADR-069). New benchmark planning goes here |
| [design/benchmarks/competitive-baseline.md](../design/benchmarks/competitive-baseline.md) | Active competitive-baseline plan (pgvectorscale / VectorChord vs pg_cuvs Pareto), part of the rigorous benchmark track (#56) |
| [design/ci-strategy.md](../design/ci-strategy.md) | 2-tier CI design (ADR-067) |
| [design/refactor-audit.md](../design/refactor-audit.md) | Complexity / orphan-code audit + ordered refactor plan (2026-06-12 3-agent audit) |

## Operational — "how to run it"

| Doc | Role |
|-----|------|
| [design/ops-gpu-playbook.md](../design/ops-gpu-playbook.md) | Unified GPU operations reference: tuning, MIG, monitoring views, orphan cleanup |
| [docs/playbooks/](playbooks/) | Task-oriented runbooks (build/test, daemon recovery, VRAM OOM, sharding, GCS snapshot, persistence corruption, capacity planning, release upgrade, replica bootstrap, …) |
| [docs/ci-gpu-setup.md](ci-gpu-setup.md) | GPU CI runner setup (WIF keyless auth, self-hosted) — companion to design/ci-strategy.md |

---

### Where a new fact goes

- A capability changed (GUC, reloption, function, view, search mode) → **docs/reference.md** (+ code).
- How a subsystem behaves changed → **ARCHITECTURE.md**.
- A new design decision → a new ADR in **design/decisions.md**.
- A new step of remaining work → **ROADMAP.md** (sequence) or its trigger backlog.
- A benchmark result → **BENCHMARK.md**; new benchmark methodology → **design/benchmarks/protocol.md**.
- An operational procedure → **docs/playbooks/** (+ link it from ops-gpu-playbook).
- Never re-open **design/specs/phase-record.md**; it is frozen history.
