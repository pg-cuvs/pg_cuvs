# BENCHMARK archive — superseded and historical measurements

Status: **Superseded / Historical**. Preserved for provenance, not for headline
claims. Current results live in [`BENCHMARK.md`](../../BENCHMARK.md); nothing here
is a current default, a release gate, or a citable headline.

These sections were moved out of `BENCHMARK.md` verbatim — no number, table, or
sentence was changed in the move. Only link paths were rebased to this file's
location, and each section carries a header note naming where it came from and what
superseded it.

**Reading the section numbers:** they are the numbers these sections carried in
`BENCHMARK.md`. A bare `§x` cross-reference inside a moved section still points at
[`BENCHMARK.md`](../../BENCHMARK.md) — **unless that section itself was moved here**,
in which case it points within this file. So `§2.1` and `§2.1a` below resolve here,
while `§2.1b` resolves in `BENCHMARK.md`.

---

<!-- moved from BENCHMARK.md §2.1 / §2.1a (verbatim); superseded by BENCHMARK.md §2.1b (wiki_all_1M, canonical) and §2.1c (#98 two-axis run) -->

### 2.1 Real embeddings — Cohere Wikipedia 1M × 1024 (cosine) [legacy dataset]

> **The project's canonical real-embedding dataset is wiki_all_1M, not Cohere — see
> §2.1b.** This §2.1 subtree is retained because it is the only lineage that traces
> pg_cuvs's recall/k-wiring fixes end to end (legacy anbench → re-measured cuvs-bench),
> but Cohere is a secondary dataset going forward.

The canonical comparison **within the Cohere lineage** is §2.1a below (cuvs-bench, ext
0.5.0). The original run here was the earlier **anbench `run_cohere.sh`** harness, and it
carries two code-era defects that make its pg_cuvs rows not directly comparable to
pgvector's:

- **pg_cuvs searched k=100 regardless of the requested k** (`LIMIT` was not yet
  wired to the GPU top-k), so its recall@10 was read from a top-100 result while
  pgvector ran true k=10 sweeps — not iso-k. (CSV note: *"k hardcoded 100
  internally"*.)
- **`index_bytes` reported 0** for the GPU-resident CAGRA index (the VRAM-accounting
  gap later fixed in #73/#75).

The full legacy table is preserved with these defects annotated in
[Appendix A](#appendix-a--legacy-anbench-cohere-run-2026-06-superseded); raw:
[`cohere_N1000000_summary.csv`](../../bench/results/cohere_N1000000_summary.csv). Use
§2.1a/§2.1b for any headline claim.

#### 2.1a Re-measured via cuvs-bench (NVIDIA's tool, ext 0.5.0, 2026-07-16) — legacy dataset (Cohere)

A fresh run inside NVIDIA's own [cuvs-bench](https://docs.nvidia.com/cuvs/) on ext
0.5.0, through a first-of-its-kind Postgres backend
(`BenchmarkOrchestrator(backend_type="pg")` — see [ADR-080](../../design/decisions.md) and
[`bench/cuvs_bench_backend/`](../../bench/cuvs_bench_backend/)). This **supersedes §2.1**:
the k is wired to the GPU top-k and recall is computed against exact ground truth.
19-point Pareto in
[`bench/results/pg_cuvsbench_1m_legacy.csv`](../../bench/results/pg_cuvsbench_1m_legacy.csv).

> **Caveat on that CSV — `index_bytes = 0` for every `pgcuvs_cagra` row.** This run
> predates #73/#75: the CAGRA graph is daemon-resident, not a Postgres relation, so
> `pg_relation_size()` returned 0 while the pgvector rows report real sizes. Read
> naively the file says *the GPU index costs nothing* — the opposite of true. The
> `recall` / `qps` / `p50` / `build_time_s` columns are unaffected. Post-fix evidence:
> [`pg_cuvsbench_wiki1m.csv`](../../bench/results/pg_cuvsbench_wiki1m.csv) reports
> `index_bytes = 3328000000` (= `1M × (768×4 + 64×4)`, the corrected
> `estimate_vram_bytes`), reproduced byte-identically on a second host (§2.1b).
> Regenerating this Cohere sweep on post-#75 code is tracked separately.
> Per-artifact provenance: [`bench/results/README.md`](../../bench/results/README.md).

| index | serves on | recall@10 (best) | p50 | QPS | build |
|-------|-----------|----------:|----:|----:|------:|
| pg_cuvs CAGRA | GPU | 0.999 (cuvs.k=400) | 2.9 ms | 340 | **62 s** |
| pgvector HNSW (native) | CPU | 0.988 (ef=400) | 12.8 ms | 74 | 237 s |
| CAGRA build → pgvector HNSW conversion | CPU | 0.9994 (ef=512) | 31.2 ms | 31 | **120 s** |

Two honest results (all **end-to-end SQL** — parse/plan + shm IPC + GPU kernel +
heap fetch — the number a PostgreSQL application actually sees):

- **Search (GPU path)** → CAGRA index: at matched recall ≈ 0.99, **~4.5× faster**
  than pgvector HNSW (2.9 ms / 340 QPS vs 12.8 ms / 74 QPS — p50 4.4×, QPS 4.6×,
  from `pg_cuvsbench_1m_legacy.csv`). The CAGRA index itself builds in 62 s, but that is a
  *GPU* index — not a like-for-like
  substitute for a pgvector HNSW — so it is **not** a build-vs-pgvector comparison,
  just the setup cost for the GPU-search path.
- **Build the SAME index, faster (CPU path)** → have the GPU build a pgvector HNSW
  for you: **CAGRA build + conversion = 120 s vs pgvector native 237 s → ~2×**,
  after which queries run on ordinary pgvector HNSW. This is the apples-to-apples
  build comparison — identical output artifact (a pgvector HNSW index). It is
  slower than a bare CAGRA build because it also materializes that CPU-servable
  index. (The 120 s already includes the CAGRA build; it is not conversion-only.)

---

<!-- moved from BENCHMARK.md §2.2 (verbatim); superseded as a headline by BENCHMARK.md §2.1c; full methodology in design/benchmarks/crossover-methodology.md §11 -->

### 2.2 Synthetic crossover pilot — where the line is

Single A100, k=10, clustered synthetic, iso-recall target 0.95, concurrency=8.
Full table in [`crossover-methodology.md` §11](../../design/benchmarks/crossover-methodology.md).

| N | dim | engine | build (s) | p50 (µs) | QPS (c=8) | recall@10 |
|--:|----:|--------|----------:|---------:|----------:|----------:|
| 1,000 | 384 | HNSW | 0.25 | **224** | **24,513** | 0.988 |
| 1,000 | 384 | CAGRA | 0.14 | 871 | 1,864 | 1.000 |
| 100,000 | 384 | HNSW | 25.5 | 8,232 | 876 | 0.932 |
| 100,000 | 384 | CAGRA | **2.77** | **1,228** | **1,206** | 0.982 |
| 1,000,000 | 1536 | HNSW | 2,721 | 14,969 | 400 | 0.910 |
| 1,000,000 | 1536 | CAGRA | **75.3** | **1,702** | **893** | 0.995 |

**CAGRA latency is near-flat in N** (871 → 1,228 µs from 1K → 100K at dim 384), while
HNSW grows steeply (224 → 8,232 µs). The **latency crossover is ≈ N 10K–100K**; build
advantage widens with both N and dim (9× → 36× at 1M × 1536). Below ~10K, pgvector HNSW
wins on every axis — the IPC round-trip (§1.1) is not worth paying for a tiny search.

---

<!-- moved from BENCHMARK.md §3 (verbatim); superseded by the current routing contract, BENCHMARK.md §3 -->

## 3. Historical multi-tenant filtered search sweep — pre-#80

This is the original filtered brute-force sweep (D-wedge post-filter + GPU BITSET
pre-filter, ADR-048), recorded before #80 changed the D-wedge overfetch behavior. It is retained
as historical evidence; it is not a current default or release gate.
N=200K × 128, uniform random, k=10, overfetch=4, 5 reps/cell.
Full table: [`docs/experiments/filter-threshold-experiment.md`](filter-threshold-experiment.md).

| selectivity | random recall | mixed recall | spatial recall | med latency |
|------------:|:-------------:|:------------:|:--------------:|------------:|
| unfiltered  | — | — | 1.00 | 1.97 ms |
| 1%  (n=2k)  | 0.20 | 1.00 | 1.00 | **1.32 ms** |
| 5%  (n=10k) | 0.80 | 1.00 | 1.00 | 1.47 ms |
| 10% (n=20k) | 1.00 | 1.00 | 1.00 | 1.65 ms |
| 25% (n=50k) | 1.00 | 1.00 | 1.00 | 2.10 ms |
| 50% (n=100k)| 1.00 | 1.00 | 1.00 | 2.82 ms |

**Historical findings:**

1. **Filtered search at low selectivity is *faster* than unfiltered** (~33% at sel=1%) —
   the daemon searches a smaller candidate space. The crossover (filtered = unfiltered
   latency) is ≈ sel 15%; above that, the filter membership test costs more than it saves.
2. **D-wedge post-filter recall holds at sel ≥ 10% for all correlations.** It only drops
   on the worst-case *random* column below 5% (0.80 at 5%, 0.20 at 1%) — because the
   k×4 overfetch pool runs out of in-filter rows. Real multi-tenant workloads have
   spatial correlation (tenants query near their own data), where recall stays 1.00 even
   at 1%. This was the rationale for the pre-#80 `cuvs.filter_auto_threshold = 0.05`
   routing claim; it is not the current default.

---

<!-- moved from BENCHMARK.md Appendix A (verbatim); superseded by §2.1a above -->

## Appendix A — Legacy anbench Cohere run (2026-06, superseded)

> **Superseded by §2.1a.** Preserved for provenance, not for headline claims. This
> is the original `bench/legacy/run_cohere.sh` (anbench) output on Cohere 1M × 1024,
> A100-SXM4-40GB. The recall *method* is sound — exact brute-force ground truth,
> `table id == corpus row index`, standard set-intersection recall@k
> (`bench/legacy/anbench/anbench_common.py:53`) — but two **code-era defects** are baked
> into the pg_cuvs rows, so do not read them as iso-k vs pgvector:
>
> 1. **k=100 fixed** — the GPU path returned top-100 regardless of the requested k
>    (`LIMIT` not wired to GPU top-k), so pg_cuvs recall@10 comes from a top-100
>    result while pgvector ran true k=10/k=100 sweeps. The comparison is not iso-k.
> 2. **`index_bytes = 0`** for the CAGRA index — VRAM accounting gap, fixed in
>    #73/#75.
>
> Raw: [`cohere_N1000000_summary.csv`](../../bench/results/cohere_N1000000_summary.csv),
> [`cohere_N1000000.jsonl`](../../bench/results/cohere_N1000000.jsonl).

| System | recall@10 | QPS | p50 | Params | defect |
|--------|----------:|----:|----:|--------|--------|
| pg_cuvs CAGRA (GPU search) | 0.9912 | 227 | 4.4 ms | k=100 | recall from top-100, not k=10 |
| pgvector HNSW | 0.9891 | 45 | 22 ms | ef_search=400 | — |
| pgvector HNSW | 0.9392 | 130 | 7.6 ms | ef_search=80 | — |
| pgvector IVFFlat | 0.9766 | 8.6 | 115 ms | probes=128 | — |
| pg_cuvs `build_hnsw` (HNSW export path) | 0.9993 | 16.9 | 61 ms | ef=512 | — |

Build (1M × 1024): pgvector HNSW native **285 s** vs CAGRA build + `build_hnsw`
**142 s** (cagra_build 84.8 s + import 57.1 s). The build figures do not depend on the
k-wiring defect, but the canonical build comparison is §2.1a (identical output
artifact, measured end-to-end).
