# Benchmark Continuation — Handoff

> **Purpose**: hand off the pg_cuvs benchmark campaign to the next session/operator.
> Validation (cost model) is **done & merged**; this is the map for the remaining
> Stage D suite + the harness gaps each module needs. Read this, then
> [`design/BENCHMARK_PROTOCOL.md`](../../design/BENCHMARK_PROTOCOL.md) (v3, the design)
> and [`docs/cost-model-calibration.md`](../../docs/cost-model-calibration.md) (frozen result).

Last updated: 2026-06-15 (after #68 v3 protocol/calibration + #69 harness merged to main).

---

## 1. State snapshot

| Item | State |
|------|-------|
| **v3 protocol** (`design/BENCHMARK_PROTOCOL.md`) | merged (#68) — engines, axes, stages, P1/P2/P3 |
| **Cost-model validation** (Stage B + Stage A cross-check) | **DONE** — `docs/cost-model-calibration.md` (#68). `cost_model_version=v3-phys`, `hw_profile_version=v2` |
| **Harness** (`bench/protocol/`, `infra/anbench/observe.py`) | merged (#69). Engines: cagra, **flat**, **transient-bf**, seqscan, hnsw, bf+batch |
| **Operational guide** (`docs/operational-guide.md`) | v1, has the concurrency + single-client tables |
| **Measured data** (`docs/data/`) | `stageA_exact_v3.csv` (exact-tier), `concurrency_consolidated.csv` (150 rows) |
| **Stage A** (physics curves) | **partial** — exact-tier @10k/100k (TOAST, dim=1024) + concurrency @1k–1M done; full N×dim×storage sweep NOT done |
| **Stage C** (freeze) | done (the calibration report is the freeze) |
| **Stage D** (filter/incremental/Pareto/concurrency/storage) | **DONE 2026-06-16** (autonomous push, runs #30–#45): D1 Pareto ✅, D2 filter ✅, D3 incremental-v1 ✅, D4 concurrency ✅, D8 storage ✅, dim/auto ✅, D6 cite-only ✅. Only infra/follow-up left: D1 iso-$ CPU arm (separate instance), D3 v2 scenarios (FIFO/upsert/recall-drift/concurrent). See §5. |

**Engines implemented in main 0.5.0** (what the harness drives): `flat` AM (A1, resident exact GPU BF), `cagra` (GPU ANN), `ivfpq` (GPU PQ), transient-B (`cuvs.gpu_bruteforce=on`, indexless), pgvector HNSW, cpu-seqscan. Cost model = data-movement physics + `pg_cuvs_hw_profile()` probe (ADR-073/074/075).

---

## 2. How to run (dispatch interface)

Everything runs on the always-up A100 dev VM via the **`bench.yml`** workflow (`workflow_dispatch`), self-hosted runner. Dispatch from `main`, check out the harness from the `ref` input.

**Key inputs** (CONTRACT.md §2 → `PGCUVS_*` env):
- `ref` — harness branch (e.g. `main` now that #69 is merged). Builds extension from here if `build=true`.
- `stage` — A | B | C | D. `module` — `physics` (A), `explain` (B), `concurrency`/`filter`/`incremental`/`pareto`/`coldstart`/`ceiling` (D).
- `cells` — e.g. `N=10k,100k;dim=1024;k=10;recall=0.95`.
- `configs` — comma list: `forced-cuvs,forced-flat,forced-seqscan,forced-transient-bf,forced-hnsw,forced-cuvs-bf,forced-cuvs-bf-batch,auto`.
- `build` — `true` rebuilds+reinstalls the extension (needed when src changed or to guarantee the daemon/sidecar are current). `false` reuses installed 0.5.0.
- `reps`, `baseline` (`same-box`|`iso-$`), `dataset`, `stop_vm` (**always false** — keep warm).

**Engine → what it measures** (`runner.py` TABLES/build_index/knob_sweep):
- `forced-flat` → `USING flat` (A1), plan-guard = flat Index Scan, recall=1.0.
- `forced-cuvs` → `USING cagra`, plan-guard = cagra, iso-recall sweep on `cuvs.k`.
- `forced-seqscan` → CPU exact (no index), `enable_cuvs=off`.
- `forced-transient-bf` → `cuvs.gpu_bruteforce=on`, plan-guard = `CuvsTransientBF`.
- `forced-hnsw` → pgvector HNSW (Ring A competitor; **not** a planner option per §0 of the protocol).
- `auto` → planner-auto (NotImplementedError in runner.py — wire it for Stage D auto-envelope; the EXPLAIN runner already does auto-routing).

**Modules → runner**: `physics`→`runner.py`, `concurrency`→`runner_concurrency.py`, `explain`→`runner_explain.py`. Routing in `engines/_common.sh` via `PGCUVS_MODULE`.

**Example dispatches** (validated this session):
```
# Stage B physics cost validation (EXPLAIN-only, cheap):
stage=B module=explain ref=main configs=forced-cuvs build=false \
  cells="N=1k,10k,100k,1m;dim=1024;k=10;recall=0.95"

# Stage A exact-tier (slow paths capped):
stage=A module=physics ref=main build=false reps=1 \
  configs=forced-cuvs,forced-flat,forced-seqscan,forced-transient-bf \
  cells="N=10k,100k;dim=1024;k=10;recall=0.95"

# Concurrency (SLA-bounded QPS, pgbench):
stage=D module=concurrency ref=main build=false \
  configs=forced-cuvs,forced-cuvs-bf,forced-cuvs-bf-batch \
  cells="N=100k,1m;dim=1024;k=10;recall=0.95"
```

---

## 3. Gotchas & lessons (READ — these bit us)

1. **`ALTER EXTENSION pg_cuvs UPDATE` is required.** `build=true` reinstalls the .so/.sql but the `bench` DB keeps the old extension version; `CREATE EXTENSION IF NOT EXISTS` is a no-op → `access method "flat" does not exist`. The runners now do `ALTER EXTENSION pg_cuvs UPDATE`. Keep it for any new runner.
2. **gpu-singleton dispatch = 1 running + 1 pending MAX.** `bench.yml` concurrency group `gpu-singleton` + `cancel-in-progress:false` → dispatching a 3rd run **cancels the older pending one**. **Dispatch at most one-ahead.**
3. **Publish OVERWRITES per run.** Each run rewrites `results/protocol/*.csv` on the `bench-results/protocol` branch — cross-run data is lost. **Pull each run's CSV immediately and consolidate into `docs/data/`** (the durable record). This is why `docs/data/*.csv` exist.
4. **Slow exact paths need query caps.** cpu-seq/transient-B are ~0.1–1 s/query; `reps × 10k queries` = hours. `runner.py` caps them: measured phase `PGCUVS_SLOW_QCAP` (default 300) + warmup 20, **and** the iso-recall sweep (100 queries) — both were needed. cagra/flat (~ms) keep the full set.
5. **Shared dev VM restarts PG during multi-minute ops.** `AdminShutdown: terminating connection due to administrator command` hit cpu-seq@100k and transient-B@100k (each ran minutes) — fast GPU engines never hit it. **Environmental, not a measurement bug.** For robust large-N exact: shrink the cap further, or use a time-bounded pgbench path instead of the per-query loop.
6. **`build=false` is fine after the first `build=true`** of the session — the 0.5.0 binaries persist on the VM. Use `build=true` only when src changed or to refresh the daemon/sidecar.
7. **GT files** `gt_runner_<N>.npy` live on the VM (`~/anbench/data`), built by the physics runner on first use; concurrency runner requires them to pre-exist (run physics for that N first, or it self-builds via `build_gt.py`).
8. **`STORAGE PLAIN` axis is wired but not dispatchable** — `runner.py setup_table` honors `PGCUVS_STORAGE=plain`, but `bench.yml` has no input mapping it. Add a `storage` input → `PGCUVS_STORAGE` env to dispatch the TOAST/PLAIN contrast.

---

## 4. Validated results (don't re-derive)

- **Cost model PASS** (`docs/cost-model-calibration.md`): physics routes GPU from ~1k (legacy mis-routed cagra to seqscan until ~23k); `pg_cuvs_hw_profile()` source=measured/probe 6/6/daemon-match; exact-first holds; DEFAULT fallback safe.
- **ADR-074 reproduced** (Stage A @10k, TOAST, dim=1024, p50): flat 0.86ms(r=1.0) < cagra 1.26ms(r=0.998) ≪ cpu-seq 46.7ms(r=1.0) ≪ transient-B 129.6ms(r=1.0). flat 54× faster than seqscan; transient-B ≈/worse than cpu-seq on PCIe. cpu-seq@100k 697.8ms.
- **Concurrency** (`docs/data/concurrency_consolidated.csv`): single-stream cagra/bf ceiling ~900–1200 QPS; **bf+batch coalescing scales** (10k→5146, 100k→4226 QPS exact) but **SLA-dependent** (1M: cagra wins p99<20ms, bf+batch wins <50ms). **peak QPS is misleading → use SLA-bounded QPS.**

---

## 5. Next work — Stage D suite (RE-AUDITED 2026-06-16)

> **Re-audit correction.** The earlier "Stage D NOT started" framing was wrong. A
> full sweep of existing assets (`tools/`, `infra/anbench/`, `docs/`, `test/`) shows
> **most of Stage D already exists or is a small delta** — only **D3 (incremental
> perf)** is a genuinely new harness. Two parallel harnesses exist and are **not
> integrated**: **`bench/protocol/`** (this campaign — physics/concurrency/explain,
> writes the `observe.py` CSV) and **`infra/anbench/`** (an older competitor suite —
> `run_pg.py`/`run_cuvs.py`/`run_faiss.py`/`run_cagra_hnsw.py`, JSONL, `aggregate.py`
> Pareto plots, `run_all.sh`). **Reuse `infra/anbench/` for Ring A/B instead of rebuilding.**

### Status legend (audit evidence)

| Module | Status | Already exists (evidence) | Real remaining delta |
|--------|--------|---------------------------|----------------------|
| **D8 storage** | ✅ **DONE** | `docs/profiling-results.md §4`: TOAST vs PLAIN measured (PLAIN build +8%, CPU detoast wall 539→147ms). Run #40 (`engines/forced-flat-plain.sh`, dim=384): **flat is storage-independent — toast==plain** (build 1.2s, p99 1.0ms, qps ~1130 both) ✓, because flat serves from the resident `.vectors` sidecar, not the heap. The PLAIN benefit is the CPU-seq/transient-B detoast path (§4) | done — `storage` bench.yml input still blocked by the main-branch rule (wrapper used instead) |
| **D4 concurrency** | ✅ **DONE** | `runner_concurrency.py` now has `forced-flat`/`forced-transient-bf` + **`sla_bounded_qps` headline** (p99≤10ms, +5/25ms curve; was missing from `observe` — added). Run #38 @100k: **flat = 1432 sla-QPS** (c=4, p99 5.6ms; single-daemon ceiling), **transient-bf = 0** (reads TOAST heap → p99 1.5–18s, can't meet any SLA) — quantifies ADR-074 "transient-B redundant". Bug fixed: slow-detoast paths capped to 100 sweep queries | (optional) add forced-cuvs/forced-flat to the consolidated CSV for the full matrix |
| **D1 Pareto $** | ✅ **near-done** | `tools/d1_pareto.py` over a 4-engine cohere-100k cell (run #45, $3.67/hr A100): **flat on the frontier — recall 1.0 @ $1.21/1M**, cagra 0.991@$1.34, ivfpq 0.965@$1.76 (64 MB VRAM = the compression axis), **hnsw(CPU) 0.97@$4.20 (3.5× dearer, dominated)**. VRAM-budget axis covered (ivfpq 54–64MB vs ~410MB raw) | remaining: **iso-$ CPU arm** (a CPU-only instance at matched $/hr — separate infra dispatch) |
| **D2 filter** | ✅ **DONE** | pg_cuvs side: `filter-threshold-experiment.md` (D-wedge recall=1.0 @ ~1.3–2.8ms flat). **Competitor measured** (`tools/filter_competitor_spike.py`, run #44, pgvector 0.8.0): `off`=recall **cliff** (sel1% 0.093, 200/200 short) / `iterative_scan` recovers recall (0.85–0.98) but **p99 35–105ms** and never 1.0. **Headline: pg_cuvs 1.0@~3ms flat vs pgvector cliff-or-92ms-tail** | done (p99 + iterative_scan modes measured) |
| **Ring A competitors** | 🟡 partial | `infra/anbench/run_pg.py` (pgvector hnsw/ivfflat/exact) | add `run_pgvectorscale.py`/`run_vectorchord.py` on the `run_pg.py` skeleton; pgvector `iterative_scan` mode |
| **Ring B anchors** | ✅ exists | `run_cuvs.py` (raw CAGRA), `run_faiss.py` (gpu/cpu), `run_cagra_hnsw.py`, `aggregate.py`, `run_all.sh` | none new — just run + consolidate into `docs/data/` |
| **D3 incremental** | ✅ **v1 DONE** | `runner_incremental.py` (append ingest, run #43): **flat (W2) 431 rows/s, 2.28ms/row p50** vs **no-index (W1=pgvector) 1573 rows/s, 0.62ms/row** → no-index writes **3.6× faster** (write-heavy→no-index, read-heavy→flat crossover, ADR-074 confirmed; flat 2.28ms/row ≈ the 1.77ms/row claim) | follow-ups: FIFO window, upsert mix, recall drift, concurrent-query-during-ingest (v1 = append only) |
| **D6 ceiling — CAGRA 50M** | 🔴 cite-only | **50M×384 already measured (ADR-025, 2026-05-30): CAGRA shard=2 & shard=4 both OOM on A100-40GB×2** (73.24 GiB raw f32 > 80 GB VRAM); competitor numbers (HNSW p50=13ms/QPS=546, vchordrq recall=0.9991) recorded there | **50M CAGRA = cite ADR-025, do NOT re-run** (same OOM, A100-80GB×2 needed) |
| **D6 ceiling — IVF-PQ 50M** | 🟡 **runnable, UNMEASURED** | **`ivfpq` AM implemented (ADR-049, 20/20 PASS 2026-06-08) — but landed AFTER the 50M run, so ADR-025 never tested it.** Compressed codes ≈ pq_dim·pq_bits/8 per vec: 50M×(192 B) ≈ **9.6 GB → fits a single A100-40GB** | **the real large-scale arm.** Add `forced-ivfpq` to `runner.py` (gap — see below), then 50M head-to-head: IVF-PQ recall@n_probes vs vchordrq 0.9991 vs HNSW. Compression ANN race, not exact |
| **D6 multi-GPU** | 🔴 out / escalate | terraform `gpu_count>1` path ready | **multi-GPU sharding NOT implemented in the product** (no daemon shard routing) → engineering, not a benchmark; 10M CAGRA = high $ |
| **IVF-PQ engine (axis-wide)** | 🟢 iso-recall validated | `ivfpq` AM (ADR-049); `forced-ivfpq` wired (`b858656`) + build-knob sweep (`2b06b3a`). A100 runs: #30 default pq_dim/2 → recall **0.937** (54 MB, 7.6× vs raw); #31 build sweep climbs pq_dim {256→512→1024}, stops at **pq_dim=1024 → recall 0.9651 ≥ 0.95** (`iso_recall_met=true`, builds_tried=3, n_probes=64, p50 1.57ms/p99 4.52ms) | none for the harness — both the n_probes knob and the build-knob sweep work. Open cost question is RaBitQ (below): ivfpq needs 1024 B/vec to hit 0.95, RaBitQ projects ~136 B |

> **⚠ Engine-axis gap (added 2026-06-16)**: IVF-PQ was under-counted in the first
> re-audit — fixed only at 50M, then realised it's missing axis-wide. The protocol
> SPEC already treats ivfpq as a first-class engine, but neither the **harness**
> (`runner.py`) nor this handoff carried it. The headline ivfpq deliverable is **not**
> 50M — it's the **VRAM-budget cell in D1/D6**: at a fixed VRAM, ivfpq trades recall
> for 10–100× capacity, so it is the only engine that changes the *shape* of the
> resource/$ Pareto. Everything ivfpq is blocked on one Tier-0 item: `forced-ivfpq`
> in `runner.py` (build reloptions `n_lists/pq_bits/pq_dim` + `cuvs.ivfpq_n_probes` sweep).

> **Schema note**: `observe.PROTOCOL_FIELDS` already has first-class `selectivity`,
> `correlation`, `filter_mode`, `stream_op`, `ops_done`, `delta_rows`,
> `sla_bounded_qps`, `detoast_ms`, `build_kind` — it was designed for D2/D3/D4.
> **No schema gap.** The old D-prep "promote columns to first-class" item is already done.

### Priority order (value / effort)

**Tier 0 — enablers & small deltas (cheap, mostly no GPU)**
- **`PGCUVS_STORAGE` → `bench.yml` input** (~2 lines + env map). Unblocks D8 dispatch (§3.8).
- **`auto` engine in `runner.py`** — ✅ **DONE + validated** (run #36). Builds the cagra index but does NOT force the plan; the ADR-075 cost model routes per query, and the chosen path is recorded (`params_json.chosen_plan`, `notes: auto→X`), not asserted. `engines/auto.sh` added. At dim=1024 both N=1k and N=100k routed to **cagra** (recall 1.0 / 0.9913) — correct: high dim → GPU wins from small N (κ ∝ 1/dim). The seqscan side of the flip needs a low-dim cell (→ `dim` integration item below).
- **`forced-ivfpq` config in `runner.py`** — ✅ **DONE + smoke-validated** (`b858656`; build `USING ivfpq` `n_lists`=√N + AM-default `pq_bits`/`pq_dim`; recall knob `cuvs.ivfpq_n_probes` [16..512]; plan guard; `engines/forced-ivfpq.sh`). A100 run #30 (`bench.yml` dispatch, `ref=docs/benchmark-handoff`, N=100k·d1024): PASS — plan guard OK, build 5.4s, **resident 54 MB vs 410 MB raw (7.6×)**, recall@10=0.937 @ n_probes=512, qps=510, p50=1.88ms/p99=4.67ms. Row in `bench-results/protocol` `results/protocol/A.csv`.
- **`pq_dim` build-knob sweep — ✅ DONE + validated** (`2b06b3a`). Run #30 found ivfpq recall caps at 0.937 < 0.95 because the loss is PQ quantization (a BUILD param), not the query-time `n_probes` knob (which already probed all √N≈316 lists). `build_knob_sweep` now climbs an ascending pq_dim ladder {dim/4, dim/2, dim} and stops at the most-compressed build meeting target. Run #31 confirmed: pq_dim=1024 → **recall 0.9651 ≥ 0.95** (builds_tried=3, n_probes=64). Cost paid: 3 rebuilds + 2× storage (pq_dim 512→1024 = 1024 B/vec). That storage cost is exactly what motivates the RaBitQ track (below).
- **D4 configs** — ✅ **DONE + validated** (run #38). Added `forced-flat`/`forced-transient-bf` to `runner_concurrency.py` + `sla_bounded_qps` headline (p99≤10ms + 5/25ms curve; the column was missing from `observe` and silently dropped — now first-class). flat=1432 sla-QPS, transient-bf=0 (TOAST detoast wall). Fixed a hang: slow heap-detoast paths now cap the recall sweep to 100 queries.
- **`dim` synthetic integration** — ✅ **DONE + validated** (run #39). The runner now auto-generates+caches a synthetic clustered corpus when the cell dim ≠ the cohere-1024 corpus (GT keyed by dim). **auto flip demonstrated at dim=8**: N=2000→**seqscan**, N=10000→**cagra** (the ADR-075 discriminating flip — exactly as predicted). recall is a tie-artifact at dim=8 (low-dim clusters → ambiguous top-10), which is fine: these cells test ROUTING, not recall.
- **time-bounded exact path** — measure cpu-seq/transient-B at large N without the AdminShutdown flakiness (concurrency/pgbench `-T` path, or a hard per-cell wall-clock cap).

> **⚠ Synthetic-data recall caveat**: the dim-sweep cells (dim≠1024) use a synthetic clustered corpus. At high dim the distances concentrate (curse of dim) → even EXACT paths get near-zero recall@k vs GT (the top-k is tie-noise). This is fine for the cells using it (routing, storage, throughput don't depend on recall correctness) but **synthetic recall numbers are meaningless** — never quote them. For recall cells use cohere (dim=1024).

**Tier 1 — analysis / republish (reuse existing, no new measurement)**
- **D8** — ✅ **DONE** (run #40 + §4). flat toast==plain (storage-independent, resident sidecar); the PLAIN win is the CPU-detoast path (§4: 539→147ms). `engines/forced-flat-plain.sh` is the dispatch vehicle (no main-branch input needed).
- **D1 Pareto** — ✅ **near-done** (`tools/d1_pareto.py`, run #45). 4-engine recall-vs-$ frontier at cohere-100k: **flat 1.0@\$1.21/1M (frontier)**, cagra 0.991@\$1.34, ivfpq 0.965@\$1.76 (64 MB VRAM), hnsw(CPU) 0.97@\$4.20 (3.5× dearer, dominated). VRAM-budget axis covered. The post-hoc aggregator (known A100 \$/hr) is the workaround for the missing price input. **Remaining**: iso-\$ CPU arm (CPU instance at matched \$/hr — separate infra).

- **D2** — ✅ **DONE** (`tools/filter_competitor_spike.py`, run #44, pgvector 0.8.0). The competitor grid (sel × {off,strict_order,relaxed_order}, recall + p50/p99): `off` recall-cliffs (sel1% 0.093, all short), iterative_scan recovers to 0.85–0.98 but p99 35–105ms, never 1.0 — vs pg_cuvs D-wedge 1.0 @ ~3ms flat. **Follow-up**: the B filtered crossover (transient-B filter-first vs CPU exact-filtered, ADR-073) for the live-`auto` filter routing.
- **Ring A competitors** — `run_pgvectorscale.py` / `run_vectorchord.py` on the `run_pg.py` skeleton (in-PG; identical load/build/query pattern).

**Tier 3 — genuinely new harness (the only big build)**
- **D3** — ✅ **v1 DONE** (`runner_incremental.py`, run #43): append ingest measured — flat 431 rows/s (2.28ms/row) vs no-index 1573 rows/s (0.62ms/row), the W1/W2 crossover (ADR-074 confirmed). **Follow-ups** (scoped out of v1): FIFO window (head INSERT + tail DELETE), upsert mix, recall drift (window GT recompute), concurrent-query QPS during ingest.

**Out of scope / escalate**
- **D6 CAGRA / multi-GPU** — 10M CAGRA (high $); **50M CAGRA = already measured in ADR-025 (OOM on A100-40GB×2). Do NOT re-run — cite the table, record the cell as "N/A — VRAM ceiling, A100-80GB×2 needed".** Multi-GPU sharding (**product feature not implemented** — no shard routing in the daemon; engineering, not benchmarking). Escalate before spending.

> **NOTE — 50M IVF-PQ is NOT out of scope** (was missed in the first draft). ADR-049's `ivfpq` AM landed *after* the ADR-025 50M run, so the large-scale CAGRA-OOM verdict never had an IVF-PQ counterpoint. IVF-PQ compresses 50M×384 to ≈9.6 GB → **fits a single A100-40GB**, so it is genuinely runnable and is the *intended* answer for that scale (ADR-049 guide: IVF-PQ ⟶ 100M+). Once `forced-ivfpq` exists (Tier 0), this becomes a real Tier-2/3 benchmark cell, not an escalation: 50M IVF-PQ recall@n_probes vs vchordrq (0.9991) vs CPU HNSW. Recall is medium and `n_probes`-sensitive — that tradeoff IS the result.
- **Ring C** (Milvus/Qdrant/LanceDB) — separate system-level doc, deprioritized.

### Research track — native RaBitQ quantizer (spike GREEN on cohere, ADR candidate)

> Why: ivfpq needs **1024 B/vec** to reach iso-recall 0.95 on cohere-1024 (run #31).
> vchordrq hits 0.9991 at 50M because **RaBitQ** (Gao & Long, SIGMOD'24) reranks
> with a *theoretical error bound* — high recall at low memory, no full-f32 rerank
> penalty. The recall idea is reachable via cuVS `refine()` (option B, deferred —
> needs original vectors resident-or-streamed, partially undoing PQ's VRAM win), but
> the *full* RaBitQ win needs the quantizer itself.

- **numpy feasibility spike — ✅ GREEN** (`tools/rabitq_spike.py`, `268b05b`). On synthetic clustered dim=1024 (N=20k/50k, 2 seeds): unbiased (standardized std=**1.001** — theoretical variance form matches), error-bound coverage **0.9901**, recall@10 **0.966 @ 5% rerank** / 0.994 @ 10%, storage **136 B/vec = 30× vs raw, 3.8× smaller than ivfpq**. The math checks (unbiased + bound) are data-agnostic → estimator is correct.
- **cohere VM validation — ✅ GREEN, knee characterized** (runs #32→#33, `engines/spike-rabitq.sh`, real `corpus.fbin` 100k×1024). Math identical to synthetic (unbiased std=**1.000**, coverage **0.9901**). Recall grid (rows=n_probes of 316 lists, cols=rerank budget):

  | n_probes | 0.1% | 0.5% | 1% | 2% | 5% |
  |---|---|---|---|---|---|
  | 316 (all) | 0.9995 | 1.0000 | 1.0000 | 1.0000 | 1.0000 |
  | 64 | 0.8875 | **0.9675** | 0.9675 | 0.9675 | 0.9675 |
  | 32 | 0.6980 | 0.9165 | 0.9185 | 0.9185 | 0.9185 |
  | 16 | 0.6540 | 0.8310 | 0.8510 | 0.8510 | 0.8510 |
  | 8 | 0.6140 | 0.6770 | 0.7660 | 0.7660 | 0.7660 |

  Resolves the run #32 "suspicious 1.0": the **quantizer is genuinely excellent** (probe-all = 0.9995 at just 0.1% rerank → RaBitQ ranks cohere near-perfectly, not a bug). The per-row ceiling is **IVF miss** (fraction of true-NN clusters probed), not a RaBitQ fault — and a tiny 0.5% rerank already reaches it. Gate met at a realistic **n_probes=64 → 0.9675 ≥ 0.95**; raise n_probes to lift the ceiling (cheap — 136 B codes). Storage **136 B/vec = 30× vs raw, 3.8× < ivfpq's 1024 B needed for the same recall**.
- **If cohere holds** → write an ADR (like ADR-049 for ivfpq) + port to a CUDA kernel + `rabitq` AM. Effort: spike S (done) → CUDA encoder/estimator/bound M–L (first self-authored ANN numerics, correctness-sensitive) → AM integration M (flat/ivfpq template) → validation harness M (non-negotiable). Tractable *because the blocker is our own bounded numerics, not an unstable upstream API* (unlike DiskANN, ADR-026) — and it extends the hot-tier value prop (more vectors/GPU at high recall), which is in-segment.
- **Deferred / measured-out: option B (cuVS `refine()` for ivfpq)** — tested for real (cuVS 26.04 python spike `tools/ivfpq_refine_spike.py`, run #35, cohere 100k): refine **works**, lifts recall@10 0.9095→**0.9685** (ratio≥2, sub-ms). But dataset device-resident (variant A) → VRAM = full f32 (~419 MB/100k), and same-VRAM **flat is exact (1.0) → dominates variant A**; RaBitQ hits the same 0.968 at **136 B/vec (30× less)**. So variant A has no product value (not building it). The valuable B (PQ codes resident + originals streamed NVMe→VRAM via **GDS**) needs GDS hardware (NVMe + nvidia-fs + cuFile) we don't have → moved to the ADR-072 cold-tier track. Plumbing path recorded: `refine_ratio` via the `ivfpq_n_probes` GUC→IPC→wrapper chain.

---

## 6. Backlog / open items

- transient-B@100k+ measurement (env-flaky; predictable ~1.1s — low value).
- `cuvs.gpu_bruteforce=auto` is off (correct on PCIe); revisit on unified-memory HW (GH200/MI300A) — ADR-075 Phase 3.
- `bf+batch` window (`cuvs.bf_batch_wait_us`) tuning per workload.
- plot.py figures for the operational guide (latency-flat, SLA-bounded bars, concurrency scaling) — `uv run bench/protocol/plot.py`.
- #61/#62/#63 still open (superseded by #64; cleanup comments posted — local team to close).

---

## 7. References & coordination

- **Design**: `design/BENCHMARK_PROTOCOL.md` (v3) · ADR-069 (protocol) · ADR-073 (engines) · ADR-074 (characterization) · ADR-075 (cost model) · ADR-061 (strategy/segment).
- **Results**: `docs/cost-model-calibration.md` · `docs/operational-guide.md` · `docs/data/*.csv`.
- **Harness**: `bench/protocol/` (CONTRACT.md = interface SSOT, README.md = ownership) · `infra/anbench/observe.py`.
- **Coordination**: GitHub **issue #56** (web↔local benchmark channel). Diagnostic on the box: `SELECT * FROM pg_cuvs_hw_profile();`.
- **VM**: A100 `pg-cuvs-dev` (always up, PCIe). Never `stop_vm`. Shared with the local dev session → expect occasional PG restarts during long ops (§3.5).
