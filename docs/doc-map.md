# Document Map

This file is the documentation routing contract for `pg_cuvs`. It separates
current product behavior, measured evidence, design history, planning, and
operations. A historical document must never override the current source or
the current-state documents listed below.

## Authority matrix

| Domain | Runtime/source authority | Canonical document | Rule |
|---|---|---|---|
| Product surface | `src/pg_cuvs.c`, `sql/pg_cuvs--0.5.0.sql`, `pg_cuvs.control` | [docs/reference.md](reference.md) | GUC defaults, AMs, SQL functions, views, and search modes are documented here. |
| Architecture | `src/pg_cuvs.c`, `src/pg_cuvs_server.c`, `src/cuvs_ipc.*` | [ARCHITECTURE.md](../ARCHITECTURE.md) | Process ownership, IPC, persistence, lifecycle, and failure boundaries are documented here. |
| Overview | Current-state documents above | [README.md](../README.md) | Overview and quickstart only; do not duplicate detailed defaults or benchmark caveats. |
| Workload guidance | Current reference plus measured evidence | [operational guide](operational-guide.md) and [best practices](best-practices.md) | Selection guidance and deployment recommendations must cite current measurements or source-backed defaults. |
| Measurements | Versioned files under `bench/results/` | [BENCHMARK.md](../BENCHMARK.md) and [results ledger](../bench/results/README.md) | Every published number needs dataset, host, code/version, method, and defect status. |
| Design rationale | ADR history | [design/decisions.md](../design/decisions.md) | Append decisions; record `supersedes` links. ADRs explain why, not current behavior. |
| Remaining work | GitHub issues and acceptance evidence | [ROADMAP.md](../ROADMAP.md) | Active entries are unfinished work only. Closed issues leave an evidence link, not an active task. |
| Operations | Tested commands and deployment assumptions | [design/ops-gpu-playbook.md](../design/ops-gpu-playbook.md) and [docs/playbooks/](playbooks/) | A procedure is current only when its version, paths, and verification command are current. |

## Document status vocabulary

Use one of these labels in a document header or section lead:

- **Current / Verified** — checked against a code commit, release, or reproducible run.
- **Proposed** — design under discussion; not a shipping contract.
- **Historical** — preserved for provenance; not synchronized with the product.
- **Superseded** — retained for history, with a link to the replacement.
- **Unverified** — implementation or claim exists, but the required runtime evidence is missing.
- **Open / Blocked** — tracked work that must not be described as complete.

`design/specs/requirements.md`, `design/specs/phase-record.md`, old experiment
reports, and ADRs are historical or decision records unless a current-state
document explicitly links a still-valid section.

## Where a change goes

- A capability changed (GUC, reloption, function, view, search mode) → code/SQL, tests, then `docs/reference.md`.
- A subsystem behavior changed → code/tests, then `ARCHITECTURE.md`; add an ADR when the decision or trade-off changed.
- A benchmark result changed → raw artifact, results ledger, then `BENCHMARK.md`; update README claims only when necessary.
- A remaining-work item changed → GitHub issue and `ROADMAP.md`; closure requires implementation and evidence references.
- An operational procedure changed → the relevant playbook and a verification command or test.
- Do not reopen `design/specs/phase-record.md`; it is frozen history.

## Supporting planning and evidence records

- [benchmark protocol](../design/benchmarks/protocol.md) — active experiment design; publish only through `BENCHMARK.md` and the results ledger.
- [CI strategy](../design/ci-strategy.md) — active CI scope and tier boundaries.
- [refactor audit](../design/refactor-audit.md) — historical audit/trigger list; stale findings do not override current source or checks.
- [reports and experiments](reports/) — evidence records; each claim must carry its status and link back to the current contract when still applicable.

## Issue and release evidence contract

Issues that affect release readiness should state:

1. the current SSOT document and source symbols;
2. the failure scenario and runtime or test evidence;
3. affected documentation and benchmark artifacts;
4. acceptance evidence required for closure; and
5. release impact: `blocker`, `open`, `environment-dependent`, or `deferred`.

An issue being closed is not, by itself, evidence that a runtime claim is
verified. Conversely, an open evidence issue must remain visible in the
release-readiness section of `ROADMAP.md`.

## Consistency check

Run the lightweight contract checks before submitting documentation or release
changes:

```bash
make docs-contract-check
```

The check covers the release version, current GUC defaults, stale operational
claims, benchmark caveat visibility, and release-blocker visibility. It is a
drift detector, not a substitute for GPU/runtime validation.
