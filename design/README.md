# design/ — pg_cuvs design records

Design-time records: the "why" and "how it was built," grouped by kind. For the current
product surface (what it *is* now) start at [ARCHITECTURE.md](../ARCHITECTURE.md) and
[docs/reference.md](../docs/reference.md); the full doc index is
[docs/doc-map.md](../docs/doc-map.md).

## Layout

| Path | What |
|------|------|
| [decisions.md](decisions.md) | **ADR log** — every design decision, alternatives, rejection reasons. The "why" of record |
| [specs/requirements.md](specs/requirements.md) | GEARS requirements spec (from the design interview) |
| [specs/phase-record.md](specs/phase-record.md) | **Frozen** per-phase as-built record + completion criteria + verification evidence |
| [strategy/positioning.md](strategy/positioning.md) | Engineering positioning / differentiation / limits |
| [strategy/workload-notes.md](strategy/workload-notes.md) | Workload-target analysis behind ADR-061 (cited by `decisions.md`) |
| [benchmarks/crossover-methodology.md](benchmarks/crossover-methodology.md) | Crossover benchmark design + methodology that BENCHMARK.md cites |
| [benchmarks/protocol.md](benchmarks/protocol.md) | Rigorous benchmark + cost-model calibration protocol (ADR-069) |
| [benchmarks/competitive-baseline.md](benchmarks/competitive-baseline.md) | pgvectorscale / VectorChord vs pg_cuvs Pareto plan (bench track #56) |
| [spikes/3b-diskann-decision.md](spikes/3b-diskann-decision.md) | Phase 3B NVMe/DiskANN go/no-go decision (NO-GO) |
| [spikes/3b-diskann-spike.md](spikes/3b-diskann-spike.md) | Phase 3B cuVS Vamana → MS DiskANN compatibility spike |
| [ci-strategy.md](ci-strategy.md) | 2-tier CI design (ADR-067) |
| [ops-gpu-playbook.md](ops-gpu-playbook.md) | GPU operations reference: tuning, MIG, monitoring, orphan cleanup |
| [refactor-audit.md](refactor-audit.md) | Complexity / orphan-code audit + refactor plan (2026-06-12) |

## Naming

Kebab-case, grouped by kind (`specs/`, `strategy/`, `benchmarks/`, `spikes/`). One-off
spike/review notes live under `spikes/` rather than at the top level next to the canonical
records. `decisions.md` stays at the root as the ADR spine.
