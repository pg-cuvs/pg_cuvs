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
| `pg_cuvsbench_1m.csv` | 2026-07-16 | cuvs-bench (pg backend), ext 0.5.0 | Cohere wiki-en, 1M×1024 | A100-40GB | **known defect** — see below |
| `cohere_N1000000_summary.csv`, `.jsonl` | 2026-06-01 | anbench `run_cohere.sh` | Cohere wiki-en, 1M×1024 | A100-SXM4-40GB | **superseded + known defect** — see below |
| `gpu_resources_bench.csv` | 2026-06-01 | `bench/legacy/test_gpu_resources.py` | synthetic 100K×384 | A100 | VRAM budget / shard / fanout matrix — not re-audited |
| `hnsw_import_bench.csv` | 2026-06-01 | 3I import harness | synthetic | A100 | CAGRA→HNSW import speedup — not re-audited |

## Known defects

### `pg_cuvsbench_1m.csv` — `index_bytes = 0` on every `pgcuvs_cagra` row

Produced **before** #73/#75 fixed VRAM accounting. The CAGRA graph is daemon-resident,
not a Postgres relation, so the backend's `pg_relation_size()` returned 0 — while the
pgvector rows in the same file report real sizes (7.62 GiB). Read naively, the file says
*"the GPU index costs nothing"*, which is the opposite of true on the axis that matters
most for GPU deployment.

`recall`, `qps`, `p50/p95/p99` and `build_time_s` in this file are **not** affected.

Corrected evidence for the same measurement, on post-fix code:
`pg_cuvsbench_wiki1m.csv` reports `index_bytes = 3328000000` for CAGRA
(= `1M × (768×4 + 64×4)`, exactly the fixed `estimate_vram_bytes` formula), and
`pg_cuvsbench_wiki1m_brev.csv` reproduces it byte-identically on a second host.
It is a different dataset (wiki_all_1M 768d vs Cohere 1024d), so it is a replacement for
the *defect*, not a drop-in replacement for the row. Regenerating the Cohere sweep on
post-#75 code is tracked separately.

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
