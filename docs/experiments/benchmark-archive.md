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
in which case it points within this file.

The Cohere lineage (§2.1, §2.1a, Appendix A) that #179 first moved here was later
dropped from this file as well: it is fully superseded by the wiki_all_1M re-runs,
and its numbers remain in the raw CSVs
([`pg_cuvsbench_1m_legacy.csv`](../../bench/results/pg_cuvsbench_1m_legacy.csv)),
the [results ledger](../../bench/results/README.md), and git history.

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
