# RMM quota production implementation validation

## Source and scope

This record follows the original allocator study in `../index-accel-20261021/`.
Those historical measurements used the unbounded pool implementation at `fded8ce`.
The original study's raw files and manifest are unchanged. Its 512 MiB / 50% quota
prototype does not verify this new production status transport and admission code.

The new implementation defaults to initial 512 MiB and a per-device 50% upstream
quota, preserves doubling/backoff, bounds admission, and adds applied device status.
CPU shim defaults to disabled and rejects an explicit nonzero initial pool.

## CPU verification (2026-10-21 KST)

- macOS unit: util 276, corpus 27 passed; Linux-only memfd/reaper cases skipped.
- Linux/aarch64, PostgreSQL 16.15, pgvector 0.8.0: util 276, corpus 60 passed.
- CPU shim extension, production/fault daemon: full build/install passed.
- SQL `rmm_pool`, `smoke`: 2/2 passed; isolation: 6/6 passed.
- Invalid quota 0/101/-1/suffix/overflow/missing value rejected. Explicit nonzero
  CPU-shim pool fails startup. No GPU allocator is fabricated in shim status.
- Shell syntax and docs contract checks passed.
- Red unit output and initial SQL golden mismatch are retained with passing logs.
  The SQL assertions all returned true; expected output was updated and rerun.

Native CUDA compilation, positive GPU device status and production quota admission
have not yet been run for this exact implementation. The assigned L40S VM remains
stopped pending coordination. This is not a new timing or cross-engine comparison.

## Reproducing measured snapshots after branch history reordering

`measured-sources.bundle` preserves GUC `0caa6483df57fa6d53310e5aecdddc8b7d559688`
and RMM `fded8ce8fb289112349fbce0d36e2e85f47e9eab` before the approved policy-first
GUC history rewrite and subsequent RMM rebase. Prerequisite commit is
`8b73d65fb8a3d6f66cdfe69eec4969c1f2fc753c` (already in origin main history).
Run `git bundle verify measured-sources.bundle`, then fetch into separate local
refs if needed. The GUC policy-only history rewrite preserves the measured code
exactly: current GUC `5f910b2` vs `0caa648`, excluding `.claude/pr-policy.json`.
