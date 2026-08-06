# cuvs-bench backend for PostgreSQL (pg_cuvs + pgvector)

A [cuvs-bench](https://docs.nvidia.com/cuvs/user-guide/benchmarking-guide/cu-vs-bench-tool.html)
**pluggable backend** that benchmarks PostgreSQL vector search — `pg_cuvs` (GPU,
via the sidecar) and `pgvector` (CPU) — inside NVIDIA's own benchmarking tool and
methodology (recall buckets, Pareto frontiers, matched-recall comparison).

cuvs-bench ships no PostgreSQL/pgvector backend. This adds one, so pg_cuvs and
pgvector are measured **apples-to-apples** on the same data, ground truth, and
methodology — the artifact NVIDIA invited (ecosystem-entry Stage 2).

## Design: one Postgres backend, several algos

cuvs-bench models a "backend" (how it runs) that exposes one or more `algo`s.
This backend exposes:

| algo | engine | build | search knob |
|------|--------|-------|-------------|
| `pgvector_hnsw` | pgvector CPU HNSW | `CREATE INDEX … USING hnsw` (m=16, ef_construction=64) | `hnsw.ef_search` |
| `pgvector_ivfflat` | pgvector CPU IVFFlat | `CREATE INDEX … USING ivfflat` (lists=4√N) | `ivfflat.probes` |
| `pgcuvs_cagra` | pg_cuvs GPU CAGRA (resident) | `CREATE INDEX … USING cagra` | `cuvs.k` (GPU candidate list) |
| `pgcuvs_hnsw_import` | pg_cuvs HNSW export path: GPU CAGRA build → pgvector HNSW export | CAGRA build + `pg_cuvs_build_hnsw(cagra,'nsw')` | `hnsw.ef_search` (CPU search) |
| `cuvs` | **raw cuVS CAGRA — no PostgreSQL** (`cuvs_engine.py`) | `cagra.build()` in-process, on the GPU | GPU candidate count (same values as `cuvs.k`) |

`cuvs` is the odd one out and is meant to be: it runs no SQL, holds its index in
the harness process's own VRAM (a second GPU tenant beside the daemon — every
row logs `nvidia-smi memory.used`), and is measured as **host wall-clock per
query, python dispatch included**. It exists so the integration tax can be read
off two rows of one CSV instead of two runs. It shares `pgcuvs_cagra`'s build
grid and sweep values exactly, derives `itopk_size` by the extension's own rule
(`src/cuvs_wrapper.cu:1122-1124`), and both arms pin `build_algo='ivf_pq'`
(the reloption default is `auto`, a size heuristic that may pick nn_descent —
so leaving it unset would let the two arms build their graphs with different
algorithms). The comparison is only about the integration if everything else
matches. It is **not** the same quantity as a
kernel-time profile (`BENCHMARK.md` §1.1) and does not replace one. See ADR-084
for what may and may not be said with these rows.

All use `vector_l2_ops` on L2-normalized vectors (L2-NN == cosine ranking). The
table is `t(id bigint, embedding vector(dim))` with **id == corpus row index**,
so returned ids land in the ground-truth id space directly (this is what
prevents the recall==0 id-space bug seen in an earlier 50M run).

## Files

- `pg_engine.py` — the engine: fbin load → COPY, per-algo `build()`,
  per-param `search()`, recall/percentiles. Factored from
  `bench/legacy/anbench/run_pg{,_3i}.py`. Runs standalone (see below) or under cuvs-bench.
- `cuvs_engine.py` — the raw arm (`cuvs`): the same `load_corpus`/`build`/
  `search`/`close` surface as `pg_engine`, backed by `cuvs.neighbors.cagra` on
  device arrays instead of SQL. `cupy`/`cuvs` are imported lazily inside the
  methods, so the module (and its tests) import on a CPU-only box. Also exposes
  `search_batch()` — one dispatch over the whole query set — for the throughput
  axis; nothing in the latency path calls it.
- `backend.py` — the cuvs-bench integration: `PgBackend(BenchmarkBackend)` +
  `PgConfigLoader(ConfigLoader)` + `register()`. `build()`/`search()` delegate to
  `pg_engine` and return cuvs-bench `BuildResult`/`SearchResult` carrying **real
  neighbor ids**, so the orchestrator recomputes recall itself (`compute_recall`)
  against the ground truth. One `BenchmarkConfig` per (algo, sweep-param); an
  index-dir-independent state sidecar lets `build()` reuse an index across a
  param sweep instead of rebuilding per point.
- `run_pg_cuvsbench.py` — the driver: registers the backend and runs
  `BenchmarkOrchestrator(backend_type="pg").run_benchmark(...)`, then writes a
  native (Google-Benchmark-shaped) CSV.
- `report_recall_tables.py` — the #98 reporter: reads one two-axis run CSV
  (`CSV_FIELDS` + `build_params` + `axis`) and renders per axis × algo ×
  build_params the highest-QPS point clearing recall 0.95 (plus 0.90/0.99
  secondaries, with explicit "no point clears" rows) as markdown. Groups are
  axis-homogeneous by construction, so the tool cannot express a latency ÷
  throughput ratio; caveats (batch bind parameters, undefined batch
  percentiles, daemon-mutex serialisation on `pgcuvs_*_conc{N}`) are emitted
  from the rows themselves and tagged per row.

## MEASUREMENT BOUNDARY (read this before comparing to cuVS's own rows)

Search latency here is the **full psql round-trip per query**:

```
client → PG backend → [pg_cuvs: shm IPC + GPU kernel] → heap fetch → client
```

i.e. **what a PostgreSQL application actually experiences**. This is deliberately
NOT the in-process C++ kernel time that cuVS's native cuvs-bench backends (e.g.
`cuvs_cagra`) report. Consequences:

- Numbers ARE apples-to-apples **across these Postgres algos** and **vs pgvector**
  (identical statement shape, same round-trip boundary).
- Numbers are **NOT 1:1** comparable to cuVS's own C++ `cuvs_*` rows — those
  exclude IPC + PG heap fetch. When plotting alongside cuVS C++ rows, label the
  Postgres rows as end-to-end SQL latency.

**The end-to-end number IS the honest number — don't strip the SQL cost out.**
The reported p50 includes a fixed per-query SQL cost — parse/plan + shm IPC +
heap fetch + client round-trip — that is **common to every algo** (empirically
~2.3 ms; pgvector at `ef_search=10` already sits at ~2.3 ms). That cost is
exactly what a PostgreSQL application pays, so the ~5× search / ~2× build
advantage at matched recall on real embeddings is the figure we report — it is
NOT re-scaled to a raw-kernel ratio by subtracting the shared floor. Two honest
caveats when reading it: (1) searches are single-query and **serial**, so QPS is
latency-bound; batch/concurrent clients would raise throughput (the GPU's
strength, not measured here). (2) The build advantage is **data-dependent**: on
real embeddings (Cohere 1M×1024, legacy dataset — see
`docs/experiments/benchmark-archive.md` §2.1a) it is ~2×
(pgvector native ~237 s vs the GPU-accelerated CAGRA-build+conversion ~120 s, both from
`bench/results/pg_cuvsbench_1m_legacy.csv`); much larger build ratios seen elsewhere come from
**synthetic random data**, where pgvector's HNSW build hits its worst case, and
are not representative. **Headline numbers are end-to-end Postgres numbers** —
a raw library timing is never the pg_cuvs figure and is never re-framed as one.

Raw library timings ARE reported, under four conditions (ADR-084, which
conditionally supersedes ADR-080 decision 2's blanket prohibition): they come
from the **same process, same build parameters, and same CSV** as the SQL rows
(the `cuvs` algo below); every row carries an `axis` label; they are cited
**only** as an integration-tax anchor within the latency axis, or as the library
mechanism within the throughput axis; and **no ratio crosses the two axes**. The
prohibition existed because the raw number in circulation came from a different
run, day and host than the SQL numbers — on this hardware even a 0%-GPU pgvector
shows ~3.5× QPS spread between two same-model A100 hosts — so it could be
neither subtracted nor divided. Producing both in one run is what removes that
defect, not an exception to it.

Queries are issued one statement at a time with the query vector as an **inline
literal** (not a bind parameter) so every algo runs an identical statement shape
— matching `bench/legacy/anbench/run_pg.py`.

`index_bytes` for `pgcuvs_cagra` is the PG heap relation size only (≈0) — the
CAGRA graph lives off-heap in `cuvs.index_dir` / GPU VRAM and isn't counted, so
don't read cagra's `index_bytes` as "storage-free."

## Daemon toggling (VRAM-fair baselines)

The standalone `pg_engine.py` runner offers `--toggle-daemon`: it restarts the
`pg-cuvs-server` daemon for `pg_cuvs` algos and stops it for `pgvector` algos,
so the CPU baselines aren't starved of VRAM by a resident GPU index (mirroring
`bench/legacy/run_cohere.sh`'s `restart_daemon`/`stop_daemon`). The **cuvs-bench
backend keeps the daemon up throughout** — it does not toggle it — because
`pgvector` is CPU-only and unaffected by a resident GPU index. The daemon's
`--index-dir` (e.g. `/tmp/cuvs_indexes`) must match `cuvs.index_dir`, or pg_cuvs
silently seq-scan-falls-back and mis-measures as slow.

## Usage

### Standalone (validate the engine without cuvs-bench)

```bash
# On the GPU VM, in an env with numpy + psycopg + pgvector (+ pg_cuvs installed,
# daemon reachable). Data + gt built by bench/legacy/run_cohere.sh Steps 0-1 (or cuvs-bench).
python bench/cuvs_bench_backend/pg_engine.py \
  --corpus ~/anbench/data/corpus.fbin \
  --queries ~/anbench/data/queries_10k.fbin \
  --gt ~/anbench/data/gt_1000000.npy \
  --n 1000000 --algos pgcuvs_cagra,pgvector_hnsw \
  --out bench/results/pg_engine_1m.csv --toggle-daemon
```

### Via cuvs-bench (the modern orchestrator path)

`run_pg_cuvsbench.py` registers the backend and drives
`BenchmarkOrchestrator(backend_type="pg").run_benchmark(...)` — the same
orchestrator, `Dataset`, `IndexConfig`, `BuildResult`/`SearchResult`, and
`compute_recall` cuvs-bench's own backends use (the module-level
`cuvs_bench.run.run()` is deprecated upstream in favour of this call):

```bash
# GPU VM, cuvs_bench env (needs psycopg + pgvector), daemon up, ext >= 0.5.0.
python bench/cuvs_bench_backend/run_pg_cuvsbench.py \
  --data-dir ~/anbench/data --n 1000000 \
  --algos pgcuvs_cagra,pgvector_hnsw,pgcuvs_hnsw_import \
  --k 10 --max-queries 2000 --out bench/results/pg_cuvsbench_1m_legacy.csv
```

Requires the pg_cuvs extension at **0.5.0** (a fresh `CREATE EXTENSION pg_cuvs`,
or `ALTER EXTENSION pg_cuvs UPDATE TO '0.5.0'` on an older database): the
`pgcuvs_hnsw_import` algo
calls the unified `pg_cuvs_build_hnsw(cagra, 'nsw')` (the older two-step
`pg_cuvs_import_hnsw(cagra, hnsw)` was removed). The gt is read as a big-ann
`.ibin`; the loader converts `gt_<n>.npy` → `gt_<n>_q<nq>.ibin` sliced to the
searched query count so `compute_recall` gets matching shapes.

## Ground truth

Exact k-NN by GPU brute force over the first N corpus rows (unit-norm → dot
product == L2 ranking), id-space-aligned to `t.id`. Produced by
`cuvs_bench.generate_groundtruth` or `bench/legacy/anbench/build_gt.py` (identical exact
result). **Always rebuild GT for the exact N tested**; never reuse a different-N GT.
