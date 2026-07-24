# pg_cuvs Cost-Model Calibration & Validation (Stage C freeze)

> **Status**: cost model = data-movement physics + hardware probe (ADR-075,
> implemented main 0.5.0). This report **freezes the validated state**: Stage B
> EXPLAIN-sweep + Stage A measured cross-check both confirm the model routes
> correctly. `cost_model_version = v3-phys`, `hw_profile_version = v2`.
>
> Evidence: this session's Stage A/B runs on the A100 dev VM via `bench.yml`
> (`ref=bench/protocol-v3`). Raw rows: [`docs/data/stageA_exact_v3.csv`](../data/stageA_exact_v3.csv).
> Design: ADR-075 (cost model) · ADR-074 (characterization) · ADR-073 (engines) ·
> ADR-069 (protocol) · [`design/benchmarks/protocol.md`](../../design/benchmarks/protocol.md) v3.

---

## 1. What the cost model is (ADR-075)

Pre-0.5.0 the cuvs cost was a row-count heuristic (`CUVS_STARTUP_COST=1000`,
`FLAT_STARTUP=50`, seqscan `~0.0226·N`). ADR-074 showed the real driver is **data
movement** (detoast, H2D, resident HBM compute) — vector kNN is **memory-bound**
(L2 ≈ 0.75 flop/byte). So the model is now a **physical decomposition**:

```
cost = scan(N) + detoast(m, storage) + move(m, link) + compute(m, engine) + topk(m,k) + fetch(k)
```

with coefficients in three layers:

- **hardware (measured, portable)** — `link_bw` (CPU↔GPU), `hbm_bw`, `gpu_cagra_lat_us`,
  `ipc_rtt`, `cpu_dist_tput`. The daemon **probes these once at boot** and writes a
  CRC'd, env-tagged sidecar `<index_dir>/cuvs_hw_profile`; the planner reads it cheaply
  (no CUDA/IPC). Crossovers move with the *box*, not a baked constant.
- **storage (catalog)** — `attstorage`/typmod → is this dim TOASTed → the detoast term.
- **workload (plan)** — `N`, selectivity `m`, `dim`, `k`.

The anchor that puts GPU µs into core cost-units: `κ = cpu_operator_cost · cpu_dist_tput / dim`
(κ ∝ 1/dim is correct — the core under-prices high-dim CPU work, so GPU should win
earlier as dim grows). Per-engine (measured regime):

- **cagra** (graph, N-independent): `startup = κ·(ipc_rtt + gpu_cagra_lat_us)`, `total = startup·(1 + cuvs_k/100)`.
- **flat** (resident BF): `startup = κ·ipc_rtt`, `total = startup + κ·bytes(N)/hbm_bw`.

**No-regression gating**: physics applies only when `cuvs.enable_phys_cost=on` (default)
∧ the hw_profile loads ∧ the required probe bits are set; otherwise the **legacy
constants** are used (byte-identical to pre-0.5.0). The CPU shim never probes GPU bits
→ stays legacy.

## 2. The decision surface (what the planner actually arbitrates)

`flat`, `cagra`, `ivfpq` are **different access methods — one per column**. Choosing
among them is a **build-time deployment decision** (recall / write-pattern), *not* a
per-query planner choice (same as HNSW vs CAGRA). The planner's **per-query** surface
is therefore two-way, per deployment:

| Deployment | Per-query planner choice |
|---|---|
| no index | `seqscan ↔ transient-B` (only if `cuvs.gpu_bruteforce=on`) |
| `cagra` index | `seqscan ↔ cagra` |
| `flat` index | `seqscan ↔ flat` |

`flat ↔ cagra` only competes if **both** are built (both VRAM-resident → unrealistic).
Calibration therefore measures each deployment separately. **exact-first rule**: when
exact (seqscan/flat/transient-B) costs ≤ cagra, take exact (cheaper *and* exact = free
win); recall is never traded for cost automatically.

## 3. Stage B — EXPLAIN-sweep validation (routing)

A100, `hw_profile: source=measured, probe_status=63 (0x3f, all 6 coeffs), matches_running_daemon=true`.
Bound KNN EXPLAIN per deployment, physics vs legacy regime:

| N | cagra physics→ | cagra legacy→ | flat physics→ | flat legacy→ |
|---|---|---|---|---|
| 1k | **cuvs** | seqscan | **flat** | seqscan |
| 10k | **cuvs** | seqscan | **flat** | flat |
| 100k | cuvs | cuvs | flat | flat |
| 1M | cuvs | cuvs | flat | flat |

Estimated costs (seqscan vs index `idx_total`): seqscan = `42 / 469 / 4179 / 22573`
(≈ 0.0226·N, the detoast+scan term); cagra `idx_total ≈ 1.4` flat across N (physics,
N-independent) vs `1001` (legacy, pinned); flat `idx_total ≈ 0.4–0.6` (physics) vs
`50.x` (legacy).

**Findings:**
- **Legacy miscalibration reproduced + fixed.** Legacy pins cagra at 1001, so it keeps
  cagra on **seqscan until ~23k** (where seqscan crosses 1001, between 10k=469 and
  100k=4179). **Physics routes cagra to GPU from ~1k** — matching the measured ~500×
  resident-GPU advantage (§4, ADR-074).
- **Physically correct shape**: index cost N-independent; seqscan cost N-scaling.
  Crossover = where N-scaling seqscan exceeds the GPU floor.
- **exact-first holds**: flat (~0.5) < cagra (~1.4); both beat seqscan from ~1k.
- **DEFAULT fallback safe**: `enable_phys_cost=off` / missing profile / CPU shim →
  legacy routing, un-probed bytes identical (separately verified, installcheck 35/35).
- **Discriminating flip** (ADR-075 fence): `dim=8, N=10000` → legacy picks seqscan
  (startup 1000), physics picks cagra (real per-query latency already beats a 10k CPU
  scan). `dim=8, N=2000` → both seqscan (anti-flip, no unfair GPU forcing).

## 4. Stage A — measured cross-check (does physics match reality?)

Single-client (c=1), TOAST, dim=1024, k=10, p50 latency / recall:

| N | flat (GPU exact) | cagra (GPU approx) | cpu-seq (CPU exact) | transient-B (GPU exact, no index) |
|---|---|---|---|---|
| 10k | **0.86 ms** / 1.0 | 1.26 ms / 0.998 | 46.7 ms / 1.0 | 129.6 ms / 1.0 |
| 100k | **1.22 ms** / 1.0 | 1.32 ms / 0.991 | 697.8 ms / 1.0 | — *(env, see §6)* |

- **Physics crossover confirmed**: the model routes GPU over seqscan from ~1k; measured
  at 10k, **flat is 54× faster than seqscan** (0.86 vs 46.7 ms) — the GPU advantage the
  physics cost predicts is real and large.
- **ADR-074 reproduced**: resident GPU (flat) wins by 54–570×; cpu-seq and transient-B
  are **data-movement (detoast) bound**; transient-B (per-query H2D) is **≈ or worse than
  cpu-seq** on PCIe (130 ms vs 47 ms @10k) — redundant, as ADR-074 found.
- **flat (exact) ≈/beats cagra (approx)** at these N and is exact — at small/mid N the
  resident exact path is the right default; cagra's value is large-N where O(N) flat
  scan exceeds budget.

## 5. Versioning (frozen)

| field | value |
|---|---|
| `cost_model_version` | `v3-phys` |
| `hw_profile_version` | `v2` (adds `cpu_dist_tput`, `gpu_cagra_lat_us`; `ipc_rtt` promoted to measured) |
| extension | `0.5.0` |
| regime witness | `pg_cuvs_hw_profile()` → `source`, `probe_status`, `matches_running_daemon` |

Routing-affecting changes bump `cost_model_version`; probe-schema changes bump
`hw_profile_version`. Result rows carry both.

## 6. Honest limits / provenance

- **Single A100 (PCIe), dim=1024, TOAST so far.** The physics formula is hardware-portable
  by design (probed bandwidth/latency constants), but only the A100/PCIe point is measured.
  The `dim` discriminating flip is validated by the unit fence (`dim=8`), not yet by a
  benchmark dim-sweep (synthetic small-dim = backlog). STORAGE PLAIN axis is wired
  (`PGCUVS_STORAGE=plain`) but needs a `bench.yml` input to dispatch.
- **Slow exact paths at large N are environmentally flaky on the shared dev VM**:
  `AdminShutdown` (PG SIGTERM mid-query) hit both cpu-seq@100k and transient-B@100k during
  the multi-minute runs (fast GPU engines never hit it). cpu-seq@100k landed on retry;
  transient-B@100k omitted (predictable ~1.1 s; the 10k point + cpu-seq@100k already make
  the point). Robust large-N exact measurement → smaller query cap or a time-bounded
  pgbench path (backlog).
- **transient-B `auto` is off** (not cost-driven) — its win needs unified-memory hardware
  (GH200/MI300A); on PCIe it is correctly never auto-routed (ADR-074/075 Phase 3).
- Single-run numbers, ~30% run-to-run variation observed elsewhere — treat as directional.

## 7. Reproduce

```
# Stage B (EXPLAIN, cheap): physics-vs-legacy routing + hw_profile
bench.yml: stage=B module=explain ref=bench/protocol-v3 \
           cells="N=1k,10k,100k,1m;dim=1024;k=10;recall=0.95" configs=forced-cuvs

# Stage A (measured cross-check): exact-tier latencies
bench.yml: stage=A module=physics ref=bench/protocol-v3 \
           cells="N=10k,100k;dim=1024;k=10;recall=0.95" \
           configs=forced-cuvs,forced-flat,forced-seqscan,forced-transient-bf

# Regime check on the box:
SELECT source, probe_status, matches_running_daemon FROM pg_cuvs_hw_profile();
# A/B legacy: SET cuvs.enable_phys_cost = off;
```
