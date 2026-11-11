# pg_cuvs

GPU-accelerated vector search for PostgreSQL via NVIDIA cuVS — a heterogeneous acceleration path that keeps Postgres as the control plane.

**Built on** [pgvector](https://github.com/pgvector/pgvector) and [RAPIDS cuVS](https://github.com/rapidsai/cuvs).

Current extension version: **0.9.0**. This overview was checked against source
commit `ae818ad0` (2026-11-11); historical GPU measurements below retain their
original versions and are not a validation run of this HEAD.

## What it is

pg_cuvs is **not** a replacement for pgvector. It is a GPU acceleration layer that sits on top of pgvector's interface. SQL syntax, transaction semantics, MVCC, and planner decisions remain entirely within PostgreSQL.

It supports two complementary modes:

- **GPU search tier**: use resident CAGRA, flat brute-force, or compressed IVF-PQ indexes as GPU candidate generators.
- **GPU build accelerator tier**: use nearby GPUs to build indexes quickly, then convert the result into a standard pgvector HNSW index (`USING pg_cuvs_hnsw`) and serve queries on CPU.

```sql
-- No query changes required. pg_cuvs accelerates this transparently.
SELECT * FROM items ORDER BY embedding <=> $1 LIMIT 10;
```

The GPU acts as a **candidate generator** — returning the top-K TID candidates and distances — and PostgreSQL handles heap access, visibility checks, joins, and filters as usual.

For on-prem or private RAG systems, this is useful even when search serving stays on CPU. These deployments often already run GPU servers for embedding models, rerankers, or batch embedding jobs. pg_cuvs can reuse that nearby GPU pool for slow index build/rebuild windows, while keeping the online serving path as ordinary PostgreSQL + pgvector HNSW.

CAGRA build and compaction norm checks use the daemon's RMM allocator for GPU
reduction scratch on the request stream. See [memory accounting](docs/reference.md#rmm-quota-and-memory-accounting)
for the pool quota and allocations outside it. If compaction runs out of memory
with NN-descent or an auto builder, it retries once with IVF-PQ, using fp32
distances when required by the data bound; the daemon log and SQL NOTICE
record the retry result.

## CI / testing

Two tiers (`design/ci-strategy.md`, ADR-067):

- **Tier 1 — CPU reference (every PR, hosted, free).** The single GPU boundary
  (`src/cuvs_wrapper.h`) is replaced by an exact-kNN **CPU shim** (`make
  PGCUVS_CPU_SHIM=1`, zero CUDA), and the daemon + IPC + fail-closed +
  Tier-1 correctness regression subset runs against it. This catches the bug class that
  actually bites — IPC/struct drift, `search_mode` labeling, fail-closed rejects,
  manifest contract, recall — without a GPU.
- **Tier 2 — real A100 (on demand).** GPU-kernel correctness, **approximate-recall
  regression**, and real VRAM/mempool behavior, run when explicitly triggered
  (not every PR).

> A green CI badge means the **CPU-reference** suite passed — it does **not** mean
> GPU kernels or approximate recall were verified. Those are the on-demand Tier 2 run.

## Architecture

> Full current-state architecture: [ARCHITECTURE.md](ARCHITECTURE.md). User-facing surface
> (GUCs, reloptions, functions, views): [docs/reference.md](docs/reference.md). How all the docs
> fit together (current vs historical): [docs/doc-map.md](docs/doc-map.md).

pg_cuvs uses a **tightly coupled sidecar model** (PG-Strom style), not an in-process CUDA context per backend:

```
┌─────────────────────────────────────────────┐
│  PostgreSQL                                  │
│  ┌──────────┐   UDS + shared payloads        │
│  │ Backend 1│ ──────────────────────────┐    │
│  │ Backend 2│ ──────────────────────────┤    │
│  │ Backend N│ ──────────────────────────┤    │
│  └──────────┘                           │    │
│                                         ▼    │
│  ┌──────────────────────────────────────┐    │
│  │  pg_cuvs_server (GPU Service Daemon) │    │
│  │  - Per-device CUDA resources         │    │
│  │  - CAGRA/IVF index in VRAM           │    │
│  │  - Task queue + thread pool          │    │
│  └──────────────────────────────────────┘    │
└─────────────────────────────────────────────┘
```

Each PostgreSQL backend is a separate process. The sidecar shares GPU resources
across sessions, with per-device resources for multi-GPU operation, instead of
creating a CUDA context in every backend.

New plans can choose a CPU index or SeqScan when the GPU path is unavailable.
If the daemon fails after a GPU scan has been planned, that statement raises an
error; it does not switch executor nodes in place. Roll back a failed transaction
before retrying. See [failure and recovery](docs/playbooks/daemon-restart-recovery.md).

## Index Types

| Index | Storage | Use Case |
|-------|---------|----------|
| `USING cagra` | GPU VRAM | Approximate NN, low latency (~2.9 ms p50 end-to-end at 1M×1024 real; ~1.5 ms at 384-dim), hot data, fits in GPU VRAM |
| `USING flat` | GPU VRAM (`.vectors` and TID map, no graph) | Exhaustive GPU brute-force for read-heavy corpora; float16 storage can change ranking relative to float32 |
| `USING ivfpq` | GPU VRAM (compressed) | Approximate NN, 10–100x VRAM savings vs cagra via product quantization |
| `USING pg_cuvs_hnsw` (built from heap, or reuses a `USING cagra` source) | GPU build / CPU serve | Faster GPU-accelerated build (~2× vs pgvector on real embeddings, higher on synthetic), then served by pgvector HNSW on CPU |

> **DiskANN/NVMe cold tier**: spike completed, no-go for now — cuVS PQFlash
> API is unstable in cuVS 26.04 and DiskANN at 50M×384 timed out at 2 GB cache.
> Revisit when demand for billion-scale is confirmed. See `design/spikes/3b-diskann-decision.md`.

All four AMs expose pgvector's `vector <->` / `<=>` / `<#>` operator interface;
`pg_cuvs_hnsw` also supports `halfvec`. The planner compares available paths using
a cost model that accounts for IPC overhead, GPU kernel cost, and data transfer:

```
Cost_total = Cost_IPC + Cost_GPU_kernel + Cost_CPU_recheck
```

Table size alone does not determine the route: query shape, filters, available
indexes, hardware profile, and freshness gates also matter. Check `EXPLAIN` for
the selected path. The index-scan candidate budget is `cuvs.k` (default 100),
independent of SQL `LIMIT`; size it for the requested result count and filtering.

## When to Use

| Scenario | Recommended path | Notes |
|----------|-----------------|-------|
| N < 10K, any dim | pgvector HNSW | IPC + daemon round-trip overhead exceeds GPU search benefit |
| N 100K–10M, VRAM fits index | pg_cuvs CAGRA (GPU search) | Hot tier, low latency; crossover vs pgvector appears around N≈50K (synthetic clustered data; verify with your embedding distribution) |
| Fast build/rebuild needed, CPU serving preferred | CAGRA build + `USING pg_cuvs_hnsw` | ~2× faster than pgvector native build on real embeddings (1M×1024, end-to-end); much higher on synthetic random data (1M×384, pgvector's HNSW worst case). Serves as standard pgvector HNSW afterward |
| On-prem RAG, embedding GPU pool already available | Reuse GPU for batch index build via the GPU Build Accelerator — the HNSW export path (GPU CAGRA build → pgvector-servable HNSW) | GPU pool not idle; online search stays on pgvector HNSW CPU path |
| Larger-than-VRAM / billion-scale / NVMe cold tier | pgvectorscale DiskANN or VectorChord | the pg_cuvs DiskANN path is a no-go in cuVS 26.04; revisit when demand is confirmed |
| Multi-GPU placement within one daemon | pg_cuvs CAGRA with `shard_count` | Measure recall and latency at the intended shard count; neither changes monotonically |

## Feature Support

| Feature | Status | Verified on |
|---------|--------|-------------|
| CAGRA GPU search | Implemented; historical GPU evidence | A100 VM, MIG, multi-GPU |
| Exhaustive GPU brute-force (`USING flat`) | Implemented; historical GPU evidence | A100 VM, installcheck 31/31 at introduction, restart test (ADR-073) |
| GPU Build Accelerator (`USING pg_cuvs_hnsw`) | Implemented; route-specific validation required | Historical 1M×384 export; current [build options and SDK requirements](docs/hnsw-build-options.md) |
| Multi-GPU sharding (`shard_count`) | Implemented; historical GPU evidence | 2×A100, shard_count=1/2 |
| MIG (Multi-Instance GPU) | Historical deployment evidence | `CUDA_VISIBLE_DEVICES=MIG-uuid` |
| GCS snapshot restore | Implemented; historical round-trip evidence | [Snapshot operations](docs/playbooks/gcs-snapshot-ops.md) |
| CPU HNSW sidecar (`cuvs.cpu_hnsw_fallback`) | Implemented; requires a live daemon and sidecar | Distinct from the daemon-independent `pg_cuvs_hnsw` index |
| DiskANN / NVMe cold tier | Deferred (no-go) | cuVS 26.04 PQFlash API unstable; see `design/spikes/3b-diskann-decision.md` |

## Current Status

The capabilities below are implemented. Original VM checks used GCP
(NVIDIA A100-40GB × 2, PostgreSQL 16); current release gaps are tracked in
[ROADMAP.md](ROADMAP.md), and those old checks do not certify every later change:

- [x] PostgreSQL C extension with C++ wrapper (resolves `float4` type collision between PG and CUDA headers)
- [x] CAGRA index build + GPU search via cuVS `cuvs::neighbors::cagra`
- [x] pgvector `vector` type as native input; operator classes for L2 (`<->`), Cosine (`<=>`), Inner Product (`<#>`)
- [x] Index Access Method handler (`cuvsamhandler`) registered with PostgreSQL planner
- [x] Hardware-anchored cost model (ADR-075): the daemon probes the deployment's GPU/CPU/IPC at boot and the planner costs GPU vs seqscan in real units (`κ = cpu_operator_cost·cpu_dist_tput/dim`); falls back to the legacy `startup_cost=1000` constants when unprobed or `cuvs.enable_phys_cost=off`
- [x] `enable_cuvs`, `cuvs.cpu_hnsw_fallback` GUCs for runtime GPU toggle and CPU fallback
- [x] `pg_stat_gpu_search`: per-index search/error counts, latency histograms, freshness, and mode; recall and build time require separate measurements
- [x] `CREATE INDEX ... USING pg_cuvs_hnsw`: heap builds use private disk-backed build/export files, or reuse a `USING cagra` source; the resulting PostgreSQL index serves through pgvector on CPU
- [x] Admission diagnostics and memory controls: `pg_cuvs_last_rejection()`, `pg_cuvs_memory_status()`, `pg_cuvs_build_status()`, and index load/unload/pin controls; see the [reference](docs/reference.md)
- [x] Multi-GPU sharding (`shard_count`), GCS snapshot restore
- [x] MIG verified (no code changes needed)

Benchmark results — canonical dataset is **wiki_all_1M (1M×768, the cuvs-bench standard
dataset)**, end-to-end SQL, measured inside NVIDIA's cuvs-bench. In the four-algorithm
build-parameter sweep (one process, one index set, one file), at matched recall≥0.95
CAGRA is **~5.7× faster** than pgvector HNSW (580.5 vs 102.1 QPS) and builds its index
**~4.9× faster**; pgvector clears no point at recall 0.99 at any `m`×`ef_construction`
tested (see [`BENCHMARK.md` §2.1c](BENCHMARK.md); raw:
[`bench/results/pg_cuvsbench_98.csv`](bench/results/pg_cuvsbench_98.csv)). The earlier
~5.4× figure is a different run on a different host
([`pg_cuvsbench_wiki1m_brev.csv`](bench/results/pg_cuvsbench_wiki1m_brev.csv), §2.1b);
absolute QPS is host-specific, so the two are not a before/after of one measurement.

The table below is the **legacy** real-embedding run — **Cohere Wikipedia 1M×1024**
(A100-40GB, ext 0.5.0; raw:
[`bench/results/pg_cuvsbench_1m_legacy.csv`](bench/results/pg_cuvsbench_1m_legacy.csv)).
Its ratios (search ~4.5×, build ~2×) remain valid as a same-run comparison; it is kept
for provenance, not as the primary citation. **Known artifact caveat:** this CSV predates
#73/#75 and reports `index_bytes=0` for CAGRA; recall/QPS/latency/build-time fields are
unaffected, and the result must be read with the
[benchmark caveat](docs/experiments/benchmark-archive.md) and
[provenance ledger](bench/results/README.md).

| Path | Build | p50 @ recall≈0.99 | QPS | vs pgvector |
|------|------:|------------------:|----:|-------------|
| pgvector HNSW (CPU, native) | 237s | 12.8ms | 74 | baseline |
| pg_cuvs CAGRA (GPU search) | 62s† | 2.9ms | 340 | **search ~4.5×** |
| CAGRA build → pgvector HNSW (GPU build, CPU serve) | 120s | served as pgvector HNSW | — | **build ~2×** |

† The 62s CAGRA build produces a *GPU* index (a different artifact from a pgvector HNSW), so it is the setup cost of the GPU-search path — **not** a build-vs-pgvector claim. The apples-to-apples build win is the ~2× row: CAGRA build + conversion = 120s vs pgvector native 237s, both producing a pgvector HNSW index.

<details><summary>Synthetic worst-case (1M×384 random) — pgvector's HNSW build stresses here; NOT representative of real embeddings</summary>

| Method | Build time | p50 latency | Recall@10 |
|--------|-----------|-------------|-----------|
| pgvector HNSW (native) | 918s | — | baseline |
| CAGRA build + HNSW conversion | 66s | 1.65ms | 1.0000* |
| pg_cuvs CAGRA search | 27s build | 1.65ms | 0.978** |

\* recall=1.0000 on synthetic random data (uniform distribution).  
\*\* 0.978 on synthetic clustered data (20 clusters, sigma=0.05).  
On synthetic **random** data pgvector's native HNSW build is pathologically slow (918s), which inflates the build ratio to ~13.9× — that is pgvector's worst case, not the real-embedding figure (~2×). The measured ratio therefore describes that synthetic workload; it does not establish data-independent GPU build cost.
</details>

See [`BENCHMARK.md`](BENCHMARK.md) for the latency decomposition and the current wiki_all_1M results, [`docs/experiments/benchmark-archive.md`](docs/experiments/benchmark-archive.md) for the full Cohere 1M×1024 Pareto and the pre-#80 filtered-search selectivity sweep, and `design/benchmarks/crossover-methodology.md` for crossover methodology.

## Roadmap

See [ROADMAP.md](ROADMAP.md) for remaining work and the trigger-based backlog. The full per-phase
build history (now frozen) is in [design/specs/phase-record.md](design/specs/phase-record.md). Some committed test-hardening work used its own "Phase 2/3/4/5" task labels; the table below refers only to the product roadmap.

| Milestone | Goal | Status |
|-----------|------|--------|
| Proof of Mechanism | PostgreSQL pipeline + sidecar CAGRA search | Done |
| Test & Ops Hardening | DDL durability, large-data benchmarks, GPU e2e, playbooks | Done |
| Single-node feature milestone | `pg_stat_gpu_search`, candidate-k/metric, write/staleness, large-build memory, tiered cache | Historical milestone; current stale and memory limitations below |
| Scale Out | pending-delta, snapshots, replicas, multi-GPU sharding, query optimization | Done (the DiskANN path is a **no-go** — see 3b-diskann-decision.md) |
| GPU Build Accelerator — HNSW export | CAGRA build → pgvector HNSW export (~2× faster build on real embeddings 1M×1024; 13× on synthetic 1M×384) | Done (VM E2E verified, MIG tested) |
| HNSW DDL export | `CREATE INDEX ... USING pg_cuvs_hnsw`: standard DDL for the build accelerator; `source` optional (private heap-build files) + metric pre-check | Done (VM E2E, installcheck 8/8) |
| Ops Playbooks | sizing guide, when-to-use, runbooks | Implemented procedures; deployment-specific verification remains |
| Release Hardening | compat matrix, known limitations, README, upgrade path | Active; see current release-readiness evidence in ROADMAP |

## Compatibility and requirements

The repository targets **Linux and PostgreSQL 16**. Its automated development
bootstrap uses Ubuntu 22.04, PG16 development headers, pgvector, Conda
`libcuvs=26.04`, and `cuda-version=12.4`; see
[the bootstrap and its assumptions](infra/brev/README.md). These are recipe
inputs, not a tested minimum-version matrix for every GPU and driver.

- Install pgvector **0.7.0 or newer**: current extension SQL creates `halfvec`
  opclasses, so the type is required even when queries only use `vector`.
- The HNSW exporter depends on pgvector's HNSW page format and opclass support
  functions. Validate upgrades against the [HNSW build contract](docs/hnsw-build-options.md).
- HNSW/ACE paths that require SDK patches use the separately pinned
  [patched cuVS recipe](infra/cuvs-patches/README.md), including patches 0001–0004.
  That recipe requires CMake 3.30.4+, Ninja, and nvcc 12.9 or 13.1 with matching
  RAPIDS dependencies. The CUDA 12.4 development bootstrap does not install that
  patched SDK. Follow the recipe's `CUVS_SDK_SOURCE_INCLUDE` and link settings.
- Deploy the extension and daemon from the same revision: global IPC protocol
  v5 and HNSW op-23 payload v5 are separate compatibility checks.

## Known limitations

| Limitation | Operational consequence |
|------------|-------------------------|
| GPU scans can fail after planning | Daemon loss, OOM, and stale replies raise errors; retry after rollback and verify the new plan. `enable_cuvs=off` disables the GPU planner path. |
| Stale delivery remains best-effort (#202) | A daemon outage can lose the stale marker. Delta and tombstone files share the `cuvs.max_delta_rows` limit. A normal VACUUM can hit it; increasing the limit does not clear an existing stale marker. See [write-path recovery](docs/playbooks/drop-and-write-path-diagnosis.md). |
| IVF-PQ inserts are not indexed incrementally | `ivfpq_aminsert` is a no-op. Rebuild after loading or changing vectors; ordinary REINDEX can leave an old tombstone sidecar, so verify recovery. |
| GPU sidecars are outside PostgreSQL WAL | Validate backup/restore and physical-replica procedures; do not infer artifact replication from catalog replication. |
| Memory admission is a preflight check | RMM quota, build estimates, and physical-free checks do not reserve memory against other processes or bound every SDK allocation. See [memory accounting](docs/reference.md#rmm-quota-and-memory-accounting). |
| HNSW layout and build resources | Export must fit PostgreSQL pages; disk space, host budget, SDK patches, and the selected build route matter. See [build options](docs/hnsw-build-options.md). |
| DiskANN/NVMe cold tier | Deferred; the historical spike does not provide a shipping DiskANN AM. |
| Multi-GPU and replica coverage | Historical runs cover particular hardware and workloads; validate the intended deployment and shard count. |

## Install

For a disposable Ubuntu GPU development VM, follow
[the bootstrap guide](infra/brev/README.md). It installs PostgreSQL, the extension,
the daemon, and a systemd unit. It changes host packages and PostgreSQL settings;
use the manual path below for an existing installation.

The following manual example assumes PG16 and its development headers,
pgvector, the selected CUDA/cuVS SDK, and compiler dependencies are already
installed, with the SDK environment active and PG16's `pg_config` on `PATH`.
For a patched SDK, use its documented build flags instead of the plain `make`.

```bash
# Install BOTH artifacts. `make install` alone does not install the daemon.
make
make server
sudo make install install-server

# Example persistent directory shared by the postgres backend and daemon.
sudo install -d -o postgres -g postgres -m 0750 /var/lib/postgresql/cuvs_indexes
```

Add `pg_cuvs` to the existing `shared_preload_libraries` list in
`postgresql.conf` and set matching paths. Preserve other preload entries:

```conf
shared_preload_libraries = 'pg_cuvs'  # append to existing entries, if any
cuvs.socket_path = '/var/run/postgresql/.s.pg_cuvs'
cuvs.index_dir = '/var/lib/postgresql/cuvs_indexes'
```

Restart PostgreSQL using the installation's service manager (on the example
Debian/Ubuntu PG16 cluster: `sudo systemctl restart postgresql@16-main`).
Preload is required before loading the extension because it registers startup
settings and the compaction worker. In a separate terminal, start a daemon
under the same OS user for this example:

```bash
sudo -u postgres "$(pg_config --bindir)/pg_cuvs_server" \
    --socket /var/run/postgresql/.s.pg_cuvs \
    --index-dir /var/lib/postgresql/cuvs_indexes
```

The service environment must resolve the same SDK shared libraries used at
build time. Wait for the daemon's `listening on` message. A systemd unit is
created by the development bootstrap, not by `make install`; for a persistent
service and a separate daemon user, follow [deployment operations](design/ops-gpu-playbook.md),
including socket/directory access and `cuvs.daemon_uid`.

In the target database, as an extension-installing role:

```sql
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION pg_cuvs;
SELECT extversion FROM pg_extension WHERE extname = 'pg_cuvs'; -- 0.9.0
```

Existing installations use the [upgrade playbook](docs/playbooks/release-upgrade.md)
to co-deploy both binaries and run `ALTER EXTENSION pg_cuvs UPDATE`.

### GPU Build Accelerator (HNSW export) Quick Start

```sql
-- The table must already contain vectors; see the runnable Usage example below.
-- A heap build spools a private corpus, asks the daemon for a temporary HNSW
-- file, and imports it into PostgreSQL index pages. No persistent CAGRA source
-- index is created. `auto` can select a resident or ACE-disk build route.
CREATE INDEX my_hnsw ON items USING pg_cuvs_hnsw (embedding vector_l2_ops);

-- Serve queries via standard pgvector HNSW on CPU (GPU not required).
SELECT * FROM items ORDER BY embedding <-> $1 LIMIT 10;

-- To ALSO keep a CAGRA index for GPU search, build it first and reuse it as the
-- source — one GPU build then powers both indexes (no second build):
CREATE INDEX my_cagra  ON items USING cagra (embedding vector_l2_ops);
CREATE INDEX my_hnsw2  ON items USING pg_cuvs_hnsw (embedding vector_l2_ops)
    WITH (source = 'my_cagra', mode = 'nsw');

-- Inspect the stored route/settings of the heap-built index.
SELECT pg_cuvs_hnsw_build_info('my_hnsw'::regclass);
```

See [HNSW build options](docs/hnsw-build-options.md) for `build_algo`,
`ef_construction`, disk and host budgets, precision guards, and SDK requirements.

> The older `SELECT pg_cuvs_import_hnsw(cagra, hnsw)` two-step form (create an
> empty `USING hnsw` target, then import) is removed. `pg_cuvs_build_hnsw(cagra,
> mode)` still works but is deprecated in favor of the DDL above.

## Usage

Run this in a fresh test schema after installation. The small corpus demonstrates
syntax and result visibility; it is not a performance or GPU-routing benchmark.

```sql
CREATE TABLE items (id bigint PRIMARY KEY, embedding vector(3));
INSERT INTO items VALUES (1, '[1,0,0]'), (2, '[0,1,0]'), (3, '[0,0,1]');

CREATE INDEX items_cagra ON items USING cagra (embedding vector_l2_ops);
SELECT id FROM items ORDER BY embedding <-> '[1,0,0]'::vector LIMIT 2;
EXPLAIN SELECT id FROM items ORDER BY embedding <-> '[1,0,0]'::vector LIMIT 2;

-- Alternative index: exhaustive BF, with no graph build.
-- float16 is an optional space/precision tradeoff, not a recall=1 promise
-- against the original float32 vectors.
CREATE INDEX items_flat ON items USING flat (embedding vector_l2_ops)
    WITH (precision='float32');

-- Batch API takes the TABLE, returns candidate ctids, and needs a heap join
-- for MVCC-visible rows. Explicit k is independent of cuvs.k here.
SELECT b.query_idx, t.id, b.distance
FROM pg_cuvs_batch_search(
    'items'::regclass,
    ARRAY['[1,0,0]'::vector, '[0,1,0]'::vector], 2
) b
JOIN items t ON t.ctid = b.ctid
ORDER BY b.query_idx, b.distance;

SELECT index_name, search_count, avg_latency_us,
       p50_latency_us, p95_latency_us, search_mode
FROM pg_stat_gpu_search;
```

`pg_stat_gpu_search` can be empty while the daemon is unavailable. Histogram
latencies describe daemon searches, not complete SQL statement latency.
`pg_cuvs_memory_status()` and `pg_cuvs_build_status()` expose memory/admission
and in-flight build information; administrative functions require the privileges
listed in the [reference](docs/reference.md).

For GPU index scans, tune the candidate budget before a query:

```sql
SET cuvs.k = 200;
-- CPU serving via an existing CPU index or SeqScan:
SET enable_cuvs = off;
RESET enable_cuvs;
```

`cuvs.cpu_hnsw_fallback=on` selects a CPU **sidecar** through the running daemon
when that sidecar exists. To serve without the daemon, build a
`USING pg_cuvs_hnsw` index instead. Choose sharding at CAGRA build time using the
[reference](docs/reference.md); changing `cuvs.shard_count` does not repartition
an already built index.

## Contributing

pg_cuvs is built by **Team pg-cuvs**, an independent open-source project. Contributions and collaboration inquiries are welcome — open an issue or pull request, or reach out at <ysys143@gmail.com>. See [CONTRIBUTING.md](CONTRIBUTING.md) to get started.

## License

PostgreSQL License. Copyright (c) 2026, JAESOL SHIN. See [LICENSE](LICENSE) for details.

## Related Work

- [pgvector](https://github.com/pgvector/pgvector) — PostgreSQL vector type and CPU indexes (HNSW, IVFFlat)
- [pgvectorscale](https://github.com/timescale/pgvectorscale) — DiskANN for PostgreSQL (CPU/SSD; benchmark comparison baseline)
- [RAPIDS cuVS](https://github.com/rapidsai/cuvs) — GPU ANN library (CAGRA, IVF-Flat, IVF-PQ, brute force)

## Acknowledgments

Thanks to the NVIDIA cuVS team for their feedback and encouragement.
- [PG-Strom](https://github.com/heterodb/pg-strom) — GPU-accelerated SQL for PostgreSQL (architectural inspiration)
