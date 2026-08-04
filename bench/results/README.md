# Benchmark result artifacts — provenance ledger

Raw result files, each produced by a different harness at a different point in the
codebase's history. **They are not interchangeable.** A number is only meaningful with
its harness, code revision, GPU host, and dataset attached — this table supplies that.

Narrative and interpretation live in [`../../BENCHMARK.md`](../../BENCHMARK.md); this
file exists so a link that lands directly on a CSV does not lose the context.

## Status legend

- **canonical** — current best evidence for its question; safe to cite.
- **superseded** — a later artifact answers the same question with better code/method.
- **known defect** — usable, but a specific column or row is wrong; stated per row.

## Ledger

| Artifact | Date | Harness | Dataset | GPU host | Status |
|---|---|---|---|---|---|
| `pg_cuvsbench_wiki1m.csv` | 2026-07-23 | cuvs-bench (pg backend) | wiki_all_1M, 1M×768 | RunPod A100-40GB | **canonical** (search+build, post-#73/#74/#75) |
| `pg_cuvsbench_wiki1m_brev.csv` | 2026-07-23 | cuvs-bench (pg backend) | wiki_all_1M, 1M×768 | Brev A100-SXM4-80GB | **canonical** — cross-machine reproduction of the row above |
| `adr079_3o_recall.csv` | 2026-07-23 | `bench/filter_recall/adr079_3o_recall.py` | wiki_all_1M | Brev A100-SXM4-80GB | superseded by `_after80` (pre-#80 D-wedge) |
| `adr079_3o_recall_after80.csv` | 2026-07-23 | same | wiki_all_1M | Brev A100 | **canonical** — D-wedge after #80 |
| `adr079_3o_recall_tail.csv` | 2026-07-24 | same | wiki_all_1M | Brev A100 | **canonical** — low-selectivity tail (3O collapse) |
| `adr079_3o_recall_crossover.csv` | 2026-07-24 | same | wiki_all_1M | Brev A100 | **canonical** — D-wedge/stream_bf crossover (~0.004) |
| `adr079_3paths.csv` | 2026-07-24 | same | wiki_all_1M | Brev A100 | superseded by `_verified` |
| `adr079_3paths_verified.csv` | 2026-07-24 | same | wiki_all_1M | Brev A100 | **canonical** — per-query route attribution |
| `pg_cuvsbench_1m_legacy.csv` | 2026-07-16 | cuvs-bench (pg backend), ext 0.5.0 | Cohere wiki-en, 1M×1024 | A100-40GB | **legacy** (superseded by wiki_all_1M canonical rows above; `index_bytes=0` defect, fixed in code by #75/#79 — see below) |
| `cohere_N1000000_summary.csv`, `.jsonl` | 2026-06-01 | anbench `run_cohere.sh` | Cohere wiki-en, 1M×1024 | A100-SXM4-40GB | **superseded + known defect** — see below |
| `gpu_resources_bench.csv` | 2026-06-01 | `bench/legacy/test_gpu_resources.py` | synthetic 100K×384 | A100 | VRAM budget / shard / fanout matrix — not re-audited |
| `hnsw_import_bench.csv` | 2026-06-01 | 3I import harness | synthetic | A100 | CAGRA→HNSW import speedup — not re-audited |
| `adr083_133_anti_after_fix.csv` | 2026-08-04 | `bench/filter_recall/adr079_3o_recall.py --correlations anti` | wiki_all_1M | Brev A100-SXM4-80GB | superseded by `_after_review_fixes` (pre-F1/F2/F3/F5 review round) |
| `adr083_133_anti_after_review_fixes.csv` | 2026-08-04 | `bench/filter_recall/adr079_3o_recall.py --correlations anti` | wiki_all_1M | Brev A100-SXM4-80GB | **canonical** — #133/ADR-083 fix verification at the final commit (after the F1/F2/F3/F5 adversarial-review fixes): 3O recall 0.998/0.999 vs 0.0 before (`adr079_3o_correlation.csv`/`_hisel.csv`, same harness/host) |
| `pg_cuvsbench_98.csv` | 2026-08-05 | cuvs-bench (pg backend), 2-phase #98 harness | wiki_all_1M, 1M×768 | **massedcompute_A100_sxm4_80G_DGX** (Brev `pg-cuvs-item2b`) | **canonical** — the two-axis build-parameter sweep (#98). See below |
| `adr083_133_anti_per_query.csv` | 2026-08-04 | `bench/filter_recall/adr079_3o_recall.py --correlations anti --dump-per-query` | wiki_all_1M | Brev A100-SXM4-80GB | **canonical** — #133 review F7: per-query `returned` for every query behind `adr083_133_anti_after_review_fixes.csv`'s 3O rows. All 400 queries (200 x 2 selectivities) returned exactly k=10 -- not a bimodal mix that would indicate the guard under-triggers on some queries |

## `pg_cuvsbench_98.csv` — the #98 two-axis run

100 rows from **one process, one set of indexes, one file**: 88 `axis=latency` +
12 `axis=throughput`. Every row carries `axis` and `build_params`, and every row has
`success=True`.

**Relationship to `pg_cuvsbench_wiki1m_brev.csv` — neither supersedes the other.**
They answer different questions on the same dataset. The Brev file is a *fixed-config
cross-machine reproduction*: one build config per algo, run to show that the RunPod
numbers reproduce on a second host. This file is a *sweep*: four algos over a build
grid (pgvector `m`×`ef_construction`, CAGRA/raw `graph_degree`, 3I `mode`×`graph_degree`),
plus a throughput axis the older file has no rows for at all. Neither file's rows can
replace the other's, and they were taken on different hosts.

**Host caveat — this is the DGX variant.** `massedcompute_A100_sxm4_80G_DGX` is not the
same node type as the `A100-SXM4-80GB` behind the Brev canonical rows, and §2.1b of
`BENCHMARK.md` already documents ~3.5× QPS swings between nominally identical A100
hosts *including for the 0%-GPU pgvector baseline*. **Cite ratios measured within this
file only.** Absolute QPS, p50 and `build_time_s` here are not comparable to any other
artifact in this ledger.

Conditions recorded in every row's `notes`:

- **Daemon up for the entire run**, including the pgvector arms. pgvector search is pure
  CPU (`enable_cuvs=off`), so a resident GPU daemon does not compete with it, but its
  absolute QPS is nonetheless a "measured with the daemon resident" number.
- `max_connections=100` — above the top concurrency rung (N=64) with headroom.
- `cuvs.shard_count=1`, set explicitly rather than left at `0=auto`.
- **conc arms**: 30 s sustained window per rung, N ∈ {1, 8, 16, 32, 64}, workers drawing
  disjoint slices of the full 10k query pool; recall computed only over GT-covered
  queries. `index_used` and a fallback-counter delta are recorded per arm — **the delta
  is 0 on all ten conc rows**, so no arm was silently absorbed as a CPU exact search.
- **batch arms**: warmup 2 + 10 timed repeats, median reported, percentiles `n/a` by
  construction (one dispatch has no per-query distribution). Vectors are passed as
  psycopg bind parameters, not inline literals — the documented exception to the
  same-statement-shape rule, since 2000×768 inline would make parser time dominate.
- **raw `cuvs` / `cuvs_batch` rows**: a second GPU tenant alongside the daemon.
  `nvidia-smi` memory is logged at each raw build (deltas 120–486 MB); `index_bytes=0`
  for these rows because the index is process-resident, not a PostgreSQL relation —
  this is a not-applicable, not the `index_bytes=0` defect described below.

**One row carries a `GATE-VIOLATION` note that the current harness would no longer
emit.** `pgcuvs_cagra_batch` records `batch recall=0.9907 vs single=0.9928
delta=0.0021 (tol=0.002)`. That calibrated gate was falsified by follow-up measurement
on the resident graph (same-graph delta 0.00225 / 0.00240 / 0.00250 over three samples)
and has since been demoted to observe-and-record with an uncalibrated `|delta| > 0.01`
tripwire. The run was **not** repeated: all 100 rows are valid measurements, and the
batch row's recall of 0.9907 is reproducible to four decimals. The note is left as the
run emitted it rather than rewritten after the fact. The underlying kernel divergence is
tracked as [#144](https://github.com/pg-cuvs/pg_cuvs/issues/144); read the batch row's
recall as the batch path's own, never as the single-query path's.

## Known defects

### `pg_cuvsbench_1m_legacy.csv` — `index_bytes = 0` on every `pgcuvs_cagra` row (legacy, not re-run)

Produced **before** #73/#75 fixed VRAM accounting; `pg_relation_size()` returned 0 for
the daemon-resident CAGRA graph. `recall`, `qps`, `p50/p95/p99` and `build_time_s` are
**not** affected. **#92 (2026-08-04) withdrew the regenerate-this-CSV plan**: the defect
is already fixed in code (#75/#79), and `pg_cuvsbench_wiki1m.csv` /
`pg_cuvsbench_wiki1m_brev.csv` above are the canonical post-fix evidence
(`index_bytes = 3328000000`, byte-identical across hosts) — this file is retained as a
legacy artifact, not a regeneration target.

### `cohere_N1000000_summary.csv` / `.jsonl` — k not wired to GPU top-k

The `pg_cuvs` rows searched **k=100 regardless of the requested k** (`LIMIT` was not yet
wired to the GPU top-k, as the file's own `notes` column states), so their recall@10 is
read off a top-100 result while the pgvector rows ran true k sweeps — **not iso-k**.
`index_bytes` is also 0 for the same reason as above.

The recall *method* is sound (exact brute-force ground truth, `table id == corpus row
index`, standard set-intersection recall@k). The defects are in the extension of that
era, not the harness. Full annotation: [`BENCHMARK.md` Appendix A](../../BENCHMARK.md).

## Reading any of these

Two properties differ in kind, and mixing them is the most common error:

- **Deterministic** — `index_bytes` is a pure function of `(n_vecs, dim, graph_degree)`;
  it is byte-identical across hosts and is the one safely portable number.
- **Host-specific** — absolute `qps`, `p50` and `build_time_s` vary ~3× between an A100
  container pod and an A100 bare node, *including for the CPU-only pgvector baseline*.
  Cite iso-recall **ratios measured within one file**, never absolute throughput across
  files. See [`BENCHMARK.md` §2.1b](../../BENCHMARK.md).
