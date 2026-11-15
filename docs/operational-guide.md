# pg_cuvs Operational Guide — Choosing a Vector-Search Path

> Historical single-host measurements below inform workload selection; current API defaults and limits are in [the reference](reference.md).
> All data: single **A100-40GB**, Cohere embeddings **dim=1024**, **k=10**,
> approximate paths held at **iso-recall@10 ≈ 0.95**, exact paths at **recall = 1.0**.
> Raw rows: [`docs/data/concurrency_consolidated.csv`](data/concurrency_consolidated.csv).
> Numbers are single-run on one GPU — treat as **directional (±~30% run-to-run)**, not spec.

## Search paths

| Path | What it is | Recall | Build | In the planner? |
|------|------------|--------|-------|-----------------|
| **cpu-seq** | CPU brute force (PostgreSQL seq scan) | exact | none | yes; verify the choice with `EXPLAIN` |
| **gpu-cagra** | GPU graph ANN (`USING cagra`) | approx | graph (GPU-fast) | yes (the cuvs index) |
| **gpu-bf (flat)** | GPU exact brute force via `USING flat` AM (ADR-073, first-class path, v0.4.0) | exact | `.tids`+`.vectors` only (no graph) | planner cost (independent AM) |
| **gpu-bf (legacy)** | GPU exact brute force via `cuvs.search_mode=brute_force` on a `cagra` index (**deprecated**) | exact | rides on a cagra build (graph is dead weight for BF) | runtime GUC |
| **gpu-bf+batch** | gpu-bf + daemon request **coalescing** (`cuvs.bf_batch_wait_us>0`) | exact | same | runtime mode (GUC) |
| **hnsw (cpu)** | pgvector HNSW / cagra→hnsw CPU export (`pg_cuvs_import_cagra`) or direct `USING pg_cuvs_hnsw` build | approx | graph | a **different deployment** (CPU-serve), not a per-query option vs cagra |

> **The hnsw rows below are a CPU-ANN benchmark reference.** PostgreSQL can hold multiple
> indexes on the same column, including CAGRA and HNSW; plan choice depends on the available
> indexes, costs, settings and query. Use `EXPLAIN` to see the path selected for a deployment.

## Quick decision guide

### W2 vs W1 vs approximate (choosing an index type)

| Need | Write pattern | Recommended path |
|------|--------------|-----------------|
| Exact (recall=1.0), read-heavy, stable corpus | Infrequent inserts/updates | **`USING flat`** with float32 precision — exhaustive BF over the stored corpus; validate top-k against the required baseline. First-class AM (ADR-073) |
| Exact (recall=1.0), write-heavy or always-fresh | Frequent inserts, ad-hoc filters | **transient BF** (`cuvs.gpu_bruteforce=on`, ADR-073) — experimental forced path; no index, every qualifying query scans live heap. Benchmark against CPU seqscan on the target hardware |
| Approximate OK, any N | Any | **`USING cagra`** — the historical single-host results below measured ~1.5 ms and ~1200 QPS; remeasure for the deployment |
| VRAM too small for cagra graph | Bulk-loaded, then rebuilt after writes | **`USING ivfpq`** — approximate PQ path; `aminsert` does not update it, so REINDEX after bulk loads; size and recall depend on corpus and settings |
| No GPU at query time | Any | **`USING pg_cuvs_hnsw`** — private direct HNSW build on GPU when no `source` is set; CPU serve |

`float16` BF is exhaustive over quantized vectors, so the historical exact-recall rows below apply only to their measured precision and workload. For float32 top-k identity, keep `float32` and validate against the intended baseline.

**`flat` cost characteristics:** write cost = delta append on INSERT/UPDATE (cheapest among all
GPU AMs: O(N) copy, no graph or PQ); periodic compaction merges delta into `.vectors`. Read cost
= O(N) per query (full `.vectors` scan). For approximate search or very large N where O(N) scan
exceeds your latency budget, use `cagra`/`ivfpq` instead.

**transient BF cost characteristics (`cuvs.gpu_bruteforce=on`):** write cost = zero (no index to
maintain). Read cost = per-query heap scan + detoast + H2D transfer — paid fresh every call.
Host corpus is capped by `cuvs.gpu_bruteforce_max_mb` (default 2048 MiB); exceeding it fails
closed with an ERROR. VRAM admission is non-evicting: a transient corpus never displaces a
resident `flat` or `cagra` index. Consider only after measuring against CPU seqscan on the target hardware, particularly for PCIe-attached GPUs. For stable, read-heavy corpora `USING flat` is more
efficient (corpus loaded once, served from resident VRAM).

**`flat` VRAM sizing:** N·dim·4 bytes for float32 resident BF (e.g. 1M×1024 ≈ 4 GB). Use
`WITH (precision='float16')` to halve corpus storage; this quantizes vectors and can change float32 top-k. If the corpus exceeds free VRAM at build time the
daemon emits a dedicated error; options: use `precision='float16'`, shard, or switch to
`cagra`/`ivfpq`.

### Cost routing & the hardware profile (ADR-075)

The planner chooses GPU index vs CPU seqscan by **cost**. Since v0.5.0 there are two regimes:

- **Physical (default).** The daemon measures this deployment's hardware at boot (`link_bw`,
  `hbm_bw`, GPU BF throughput, IPC RTT, CPU distance throughput, CAGRA per-query latency) and writes
  a CRC'd profile to `<index_dir>/cuvs_hw_profile`. The planner reads it cheaply (no CUDA/IPC) and
  costs `cagra`/`flat` in real units, so the GPU-vs-seqscan crossover tracks your actual hardware
  and the query's vector dimension — not a baked constant. The earlier miscalibration
  (`CUVS_STARTUP_COST=1000` preferring seqscan up to ~23k rows) only applies to the legacy fallback.
- **Legacy fallback.** When `cuvs.enable_phys_cost=off`, the profile is missing/partial, or the
  build is the CPU shim, the planner uses the legacy constants (`CUVS_FLAT_STARTUP_COST=50` vs
  `CUVS_STARTUP_COST=1000`) — `flat` has a lower startup cost, but actual plan choice
  depends on competing paths and statistics. Inspect it with `EXPLAIN`.

**Diagnose with `pg_cuvs_hw_profile()`:**

```sql
SELECT gpu_name, source, probe_status, matches_running_daemon,
       link_bw_bytes_per_us, hbm_bw_bytes_per_us, gpu_cagra_lat_us, ipc_rtt_us
FROM pg_cuvs_hw_profile();
```

- `source = measured` → physical regime active; `source = default` → no usable sidecar (legacy).
- `matches_running_daemon = false` → the profile is **stale vs the running daemon** (GPU swapped,
  index_dir restored from another host, or daemon not yet rebooted after a GPU change). Fix:
  restart `pg-cuvs-server` so it re-probes and rewrites the sidecar. Until then the planner safely
  uses whatever the sidecar/DEFAULT says — never crashes, only mis-prices.
- `probe_status` is a bitmask of which coefficients were actually measured; a partial value means
  some coefficients fell back to DEFAULT and the affected path uses the legacy cost.
- To force legacy routing everywhere (e.g. to A/B a routing regression): `SET cuvs.enable_phys_cost
  = off`.

Note: `cuvs.gpu_bruteforce = auto` currently behaves as `off`; `on` forces the
qualifying transient BF query shape. See [the reference](reference.md#transient-no-index-gpu-brute-force-cuvsgpu_bruteforce).

### Within gpu-bf: single-query latency guide

1. **Need exact (recall = 1.0)?**
   - Trivially small table (≲ a few hundred rows): **cpu-seq**.
   - Otherwise, low/moderate concurrency: **gpu-bf via `USING flat`** (~1–2 ms up to ~100k).
   - Sustained **high** concurrency + can tolerate a few ms extra latency: **gpu-bf+batch**
     (`cuvs.bf_batch_wait_us` on a `flat` or `cagra` index in brute_force mode).
2. **Approximate OK (recall target < 1.0)?** → **gpu-cagra**: ~1.5 ms latency at *any* N,
   ~1200 QPS, SLA-robust. The GPU workhorse.
3. **No GPU at query time?** → hnsw (build on GPU via cagra, export to CPU).

## Single-client latency (c=1) — p50 / p99 in ms

| N | cpu-seq | hnsw(cpu) | gpu-cagra | gpu-bf | gpu-bf+batch |
|---|---|---|---|---|---|
| 1k | – | 0.52 / 0.65 | 1.25 / 1.42 | – | – |
| 10k | 73.2 / 75.6 | 1.12 / 1.52 | 1.60 / 4.13 | 1.14 / 1.30 | 2.29 / 2.49 |
| 32k | – | 4.07 / 6.14 | 1.66 / 4.31 | – | – |
| 100k | 994 / 1032 | 6.13 / 8.93 | 1.43 / 1.62 | 1.90 / 2.10 | 3.34 / 3.60 |
| 200k | – | 6.92 / 9.99 | 1.68 / 4.22 | – | – |
| 500k | – | 10.69 / 14.94 | 1.70 / 4.33 | – | – |
| 1M | – | 11.18 / 16.05 | 1.50 / 1.69 | 6.90 / 11.10 | 8.08 / 13.47 |

**Reading it:**
- **gpu-cagra latency is ~flat (~1.5 ms) from 1k→1M.** hnsw climbs 0.5→11 ms; cpu-seq is hopeless past ~1k.
- **gpu-bf is exact and flat-fast up to ~100k**, then its per-query cost grows with N (1M ≈ 7 ms) — exact brute force genuinely scans more.
- **gpu-bf+batch is *slower* single-client** (the coalescing window adds latency with nothing to batch). It is a throughput tool, not a latency tool.

## Throughput — peak QPS (best across c = 1…64)

| N | cpu-seq | hnsw(cpu) | gpu-cagra | gpu-bf | gpu-bf+batch |
|---|---|---|---|---|---|
| 1k | – | 16,652 | 1,356 | – | – |
| 10k | 82 | 8,438 | 930 | 1,892 | **5,146** |
| 32k | – | 1,911 | 921 | – | – |
| 100k | 8 | 1,201 | 1,206 | 776 | **4,226** |
| 200k | – | 1,049 | 914 | – | – |
| 500k | – | 679 | 910 | – | – |
| 1M | – | 647 | 1,203 | 160 | **1,660** |

## Throughput — SLA-bounded (the *fair* comparison)

Peak QPS flatters `gpu-bf+batch`, which buys throughput by **trading latency** (coalescing).
Holding a p99 ceiling is the honest comparison:

**max QPS with p99 < 20 ms**

| N | hnsw(cpu) | gpu-cagra | gpu-bf | gpu-bf+batch |
|---|---|---|---|---|
| 100k | 1,012 | 1,206 | 776 | **3,811** |
| 1M | 342 | **1,203** | 140 | 890 |

**max QPS with p99 < 50 ms**

| N | hnsw(cpu) | gpu-cagra | gpu-bf | gpu-bf+batch |
|---|---|---|---|---|
| 100k | 1,201 | 1,206 | 776 | **4,226** |
| 1M | 556 | 1,203 | 160 | **1,441** |

**Reading it:**
- **gpu-cagra is SLA-robust**: its ~1200 QPS survives even a tight 20 ms p99 (peak is at low c, low latency).
- **gpu-bf+batch wins big at 100k** under either SLA (exact, 3–4× cagra).
- **At 1M the SLA decides it**: under 20 ms p99, cagra (1203) **beats** bf+batch (890); loosen to 50 ms and bf+batch (1441) wins. This is the crux of "bf+batch is regime-specific."

## Key crossovers (single A100)

| Comparison | Crossover | Note |
|---|---|---|
| cpu-seq ↔ gpu (any) | ~ a few hundred rows | GPU wins almost immediately; cpu-seq 13 QPS @10k vs gpu-bf 862 |
| gpu-bf (exact) ↔ gpu-cagra (approx), single-stream | ~50k | below: exact BF is *faster*; above: cagra (approx) wins |
| hnsw ↔ gpu-cagra, single-client latency | ~20–30k | cagra flat ~1.5 ms overtakes climbing hnsw |
| hnsw ↔ gpu-cagra, peak throughput | ~270k | hnsw per-core QPS falls below cagra's ~1200 floor |
| **planner seqscan → cuvs (modeled)** | **~23k** | **miscalibrated**: cuvs actually wins from ~1k → `CUVS_STARTUP_COST=1000` ~20× too high |

## gpu-bf+batch — when the coalescing window pays off

`cuvs.bf_batch_wait_us` makes the daemon hold incoming brute-force requests for a short
window and fuse the concurrent ones into a single GPU pass. It is the **only** path whose
throughput *rises* with concurrency (others plateau):

```
N=100k   c1    c4    c8   c16   c32   c64
gpu-cagra 696  1206  1194  1183  1184  1181   ← flat (single-stream ceiling)
gpu-bf    524   776   772   771   769   767   ← flat
bf+batch  298  1144  2188  3094  3811  4226   ← scales with load
```

Use it when **all** hold: exact required, **sustained high concurrency** (requests actually
overlap in the window), and the SLA tolerates the added queueing latency. At low concurrency
it is a net loss (the c=1 penalty above). It is **not** the default — it is a high-throughput
specialization. (Tune the window per workload; `cuvs.bf_precision=float16` halves corpus VRAM but can change float32 top-k.)

## Caveats & provenance

- **Single A100-40GB, single GPU.** Crossovers move with CPU cores (hnsw/seq scaling), GPU
  model/count (the ~900–1200 single-stream GPU ceiling), and memory bandwidth. Re-measure per spec.
- **`USING flat` is the first-class exact BF path (v0.4.0, ADR-073).** It builds only `.tids`+
  `.vectors` — no graph — so you pay only the O(N) corpus copy, not a CAGRA graph build.
  The legacy path (`cuvs.search_mode='brute_force'` on a `cagra` index) still works but is
  **deprecated**: it couples exact-BF to an unnecessary graph build and cannot be independently
  cost-calibrated. Use `USING flat` for new deployments.
- **`.vectors` sidecar = full raw corpus** (N·dim·4 B; 1M·1024 ≈ 4 GB), resident on top of the
  graph. This is the float32 corpus cost of GPU BF; `cuvs.bf_precision=float16` halves corpus VRAM but changes precision.
- **Numbers are single-run on one GPU**; ~30% run-to-run variation observed (e.g. cagra@100k
  peak 914 vs 1206 across runs). Use ranges, not the last digit.
- **Results branch overwrites per run** — this file + the consolidated CSV are the durable record.
