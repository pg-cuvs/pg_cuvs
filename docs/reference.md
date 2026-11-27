# pg_cuvs Reference

> **Current-state reference (SSOT)** for the user-facing surface: index access methods, search
> modes, GUCs, reloptions, SQL functions, and observability views. How the pieces fit together is
> in [ARCHITECTURE.md](../ARCHITECTURE.md); the rationale behind each is in
> [design/decisions.md](../design/decisions.md). Source-checked against `src/pg_cuvs.c` and
> `sql/pg_cuvs--0.9.0.sql` at HEAD `ae818ad` (extension version 0.9.0; source review only).
> The current install script `sql/pg_cuvs--0.9.3.sql` creates the catalog of `sql/pg_cuvs--0.9.1.sql`
> (the 0.9.1 → 0.9.2 and 0.9.2 → 0.9.3 scripts change no SQL object), which is the 0.9.0 catalog plus
> `pg_cuvs_hnsw_connectivity_status()` (`test/sql/upgrade_path.sql` compares the upgrade chain
> with a fresh install).

---

## 1. Index access methods

pg_cuvs registers four index AMs. All reuse pgvector's `vector` type and operators
(`<->` L2, `<=>` cosine, `<#>` inner product) via opclasses `vector_l2_ops` (default),
`vector_cosine_ops`, `vector_ip_ops`.

| AM | Create | Tuning (reloptions) | Served by | Storage / sidecars |
|----|--------|---------------------|-----------|---------------|
| `cagra` | `CREATE INDEX i ON t USING cagra (col vector_l2_ops)` | `graph_degree`, `intermediate_graph_degree`, `build_algo`, `index_dir` | GPU daemon | `.cagra`, `.tids`, (`.vectors`, `.delta`, `.tombstone`, `.stale`, `.shards`, `.sNNN.cagra`, `.relfilenode`) |
| `flat` | `CREATE INDEX i ON t USING flat (col vector_l2_ops) WITH (precision='float16')` | `precision`, `index_dir` | GPU daemon | `.tids`, `.vectors` (no `.cagra` graph) |
| `ivfpq` | `CREATE INDEX i ON t USING ivfpq (col vector_l2_ops)` | `n_lists`, `pq_bits`, `pq_dim`, `index_dir` | GPU daemon | `.ivfpq`, `.tids` |
| `pg_cuvs_hnsw` | `CREATE INDEX i ON t USING pg_cuvs_hnsw (col vector_l2_ops) WITH (source='my_cagra', mode='nsw')` | `source`, `mode`, `m`, `ef_construction`, `build_algo` | **pgvector** (CPU) | pgvector-format pages in this index's PostgreSQL relation; optional `.hnsw` hnswlib conversion sidecar, not the index storage |

`flat` is the first-class exact GPU brute-force AM (ADR-073, extension v0.4.0). It builds only a
`.tids` + `.vectors` store — no CAGRA graph — so the build is O(N) copy rather than graph
construction. The AM forces brute-force search over the stored corpus regardless of `cuvs.search_mode`. With `float16`, corpus and query quantization can change float32 top-k ordering; see the precision reloption below. Freshness uses the same delta/tombstone mechanism as `cagra`
(INSERTs append to `.delta`, merged at search time). The planner picks `flat` via its own cost
function: when the hardware profile is fully probed and `cuvs.enable_phys_cost` is on, cost is the
ADR-075 physical model (§ Cost model & hardware profile); otherwise it falls back to the legacy
constants (`CUVS_FLAT_STARTUP_COST=50`, vs `CUVS_STARTUP_COST=1000` for cagra). These are cost inputs, not a guarantee that small tables use `flat`; compare candidates with `EXPLAIN` for the target query and statistics. DROP removes `.tids` + `.vectors` and releases VRAM. The index is
restart-durable: `.vectors` is reloaded on daemon restart. Use `flat` for read-heavy, stable
corpora that require exhaustive search; use `cagra`/`ivfpq` for approximate search at large N.

> **Deprecated:** `SET cuvs.search_mode = 'brute_force'` on a `cagra` index was the previous way
> to get GPU exact BF. It still works (ADR-039) but is deprecated. `USING flat` is the
> first-class replacement: independent AM, honest build semantics, dedicated cost routing. The GUC
> path couples an exact-BF workload to an unnecessary graph build and cannot be independently cost-
> calibrated by the planner.

> For an unsharded legacy `cagra` index, an INSERT that extends the graph or a VACUUM
> that compacts it invalidates the resident brute-force cache (#237, #239). Before
> saving the changed graph, the daemon removes its old `.vectors` sidecar. Searches
> that require that sidecar report an error until it is rebuilt. This
> includes index scans and `pg_cuvs_batch_search` in `brute_force` mode, and
> `cuvs_filtered_knn` with a NULL filter. Use `REINDEX` to rebuild the sidecar.

> If a legacy CAGRA compaction fails to save or remove its tombstones, an
> INSERT that would extend that resident index reports SQLSTATE `40001`.
> Complete compaction, then retry the transaction.
> If the tombstones disappear before their rows are removed, or the daemon
> shuts down with those rows still in the resident graph, the index is marked
> stale. Use `REINDEX` to restore GPU searches and writes to the graph.

**NULL query vector.** A `cagra` / `flat` / `ivfpq` index scan cannot answer
`ORDER BY col <-> NULL` — there is no vector to search with — so it raises
`ERROR 0A000 (feature_not_supported)` rather than returning zero rows. A literal
`<-> NULL::vector` never reaches the scan (the operator is strict, so it const-folds
and the query is answered by the heap); a NULL bound to a parameter under a generic
plan does, and is where the error surfaces (#150).

### Transient no-index GPU brute-force (`cuvs.gpu_bruteforce`)

When `cuvs.gpu_bruteforce = on`, the planner routes a no-index single-table
`ORDER BY veccol <-> q LIMIT k` query to a transient GPU exact brute-force Custom Scan node named
`CuvsTransientBF` — no index required. This is the W1 counterpart to `USING flat` (W2 resident
index): use it when write velocity or always-fresh semantics make maintaining a flat index
impractical.

Key properties:
- **Exact** (recall@k = 1.0) and **always fresh** — scans the live heap under the query snapshot
  every call; no delta or tombstone logic needed.
- **Filter-first**: WHERE clauses are applied before the GPU handoff, not post-filtered.
- **Exec-time parameter binding**: `ORDER BY col <-> $1 LIMIT k` parametrized queries are NOT
  downgraded to approximate search. A NULL bound to that parameter raises
  `ERROR 0A000 (feature_not_supported)` rather than returning zero rows — there is no vector to
  search with, and B stands in for a seqscan, so a silent empty result would read as "every row
  was scanned and none matched" (#156). Same contract as the index scans above (#150).
- **Fires only for the bounded shape**: single base relation (no join), no OFFSET, no GROUP
  BY/DISTINCT/HAVING/window, a plan-time Const LIMIT in 1..100000, single distance ORDER BY key.
  Any other shape falls back to seqscan. The planner keeps `Limit → Sort → CustomScan` (no
  pathkeys claimed).
- **Host cap** (`cuvs.gpu_bruteforce_max_mb`, default 2048): if the per-query corpus materialized
  on the host exceeds this limit, the query fails closed with a clear ERROR rather than silently
  falling back.
- **Non-evicting VRAM admission**: the daemon never evicts a resident `flat` or `cagra` index to
  make room for a transient corpus. If the corpus exceeds the VRAM budget the daemon returns
  `OOM_FALLBACK` and the backend raises an ERROR; no crash.
- **Write cost zero** (no index to maintain). Read cost is paid per query: heap scan + detoast +
  H2D transfer every call. For read-heavy stable corpora, `USING flat` (W2) is more efficient.

Mental model: "flat index present → use it (A1/W2); no index + `gpu_bruteforce=on` → B (W1)."

Verified Tier-2 on A100/PG16 (2026-06-14): `make installcheck` 32/32 GREEN + isolation 3/3 GREEN.

```sql
SET cuvs.gpu_bruteforce = on;
-- No index needed; a plain ORDER BY query uses CuvsTransientBF:
SELECT ctid, embedding <-> '[1,2,3]' AS dist
FROM items
ORDER BY embedding <-> '[1,2,3]'
LIMIT 10;
```

`ivfpq` trades recall for 10–100× lower VRAM via product quantization. `pg_cuvs_hnsw` is the GPU
*build accelerator*: it builds a pgvector HNSW from a CAGRA graph without pgvector's CPU build,
then serves entirely through pgvector (its query path is pgvector's). See also
the deprecated function form `pg_cuvs_build_hnsw()` in §5.

> Cosine opclass note: for `pg_cuvs_hnsw` the cosine opclass mirrors pgvector exactly — proc 1 is
> the negative inner product and proc 2 is `vector_norm` (pgvector normalizes at build and ranks by
> inner product), not `cosine_distance`.

### Cost model & hardware profile (ADR-075)

The planner decides between a GPU index (`cagra`/`flat`) and a CPU seqscan by **cost**. Two regimes:

- **Physical (default, when probed)** — the daemon measures the deployment's hardware at boot
  (CPU↔GPU `link_bw`, `hbm_bw`, GPU brute-force throughput, IPC round-trip, CPU distance
  throughput, CAGRA per-query latency) and writes a CRC'd profile sidecar. The planner reads it
  cheaply (no CUDA/IPC) and costs GPU paths in real PG cost-units via the anchor
  `κ = cpu_operator_cost · cpu_dist_tput / dim`: `cagra ≈ κ·(ipc_rtt + cagra_latency)`,
  `flat ≈ κ·ipc_rtt + κ·(N·dim·4 / hbm_bw)`. The GPU-vs-seqscan crossover is therefore located by
  the deployment's hardware and the query's dimension, not a baked constant.
- **Legacy fallback (byte-identical to pre-0.5.0)** — when `cuvs.enable_phys_cost = off`, the
  profile is missing/partial/corrupt, or the build is the Tier-1 CPU shim (which leaves the GPU
  coefficients unprobed — only the host-side `ipc_rtt`/`cpu_dist` bits get set, so the required GPU
  probe bits are never satisfied), the planner uses the legacy heuristic constants (`CUVS_STARTUP_COST=1000`, `CUVS_FLAT_STARTUP_COST=50`,
  k-dominant). Routing is unchanged from prior versions in this regime.

Inspect the active profile with `pg_cuvs_hw_profile()` (§5): `source = measured` confirms the
physical regime; `probe_status` shows which coefficients were measured; `matches_running_daemon`
flags a stale profile after a GPU swap / migration. The transient-BF (`auto`) path is **not** yet
cost-driven — `cuvs.gpu_bruteforce = auto` stays off-behavior pending unified-memory hardware
(ADR-075 Phase 3).

---

## 2. Search modes

`cagra` indexes choose an execution path at query time. Set the desired path with
`cuvs.search_mode` / related GUCs; the path actually taken is reported as the **string** in
`pg_stat_gpu_search.search_mode`. The integer below is the internal mode code — no SQL function
returns it, but it appears in daemon logs and test assertions, so this table doubles as the
decoder.

| `pg_stat_gpu_search.search_mode` | Code | Meaning |
|---|---|---|
| `gpu_cagra` | 0 | GPU CAGRA approximate NN (default) |
| `cpu_hnsw` | 1 | CPU HNSW fallback via `.hnsw` sidecar (`cuvs.cpu_hnsw_fallback`) |
| `cpu_fallback` | 2 | Generic CPU path (seqscan / pgvector) after a daemon-side gate |
| `brute_force` | 3 | GPU exact BF over the `.vectors` sidecar (`cuvs.search_mode='brute_force'` on a `cagra` index). **Deprecated** — use `USING flat` instead (ADR-073) |
| `cagra_prefilter` | 4 | CAGRA with GPU BITSET prefilter (3O filtered search) |
| `ivfpq` | 5 | GPU IVF-PQ (`ivfpq` AM) |
| `stream_bf` | 6 | Out-of-core filtered BF streamed from `.vectors` (ADR-064) |

> **Cancellation / timeouts:** an in-flight GPU search is interruptible — `statement_timeout` or a
> query cancel aborts the daemon round-trip within ~0.5s instead of blocking indefinitely (ADR-053,
> `recv_all_interruptible`). The daemon ignores SIGPIPE so it survives a client mid-reply disconnect.

An `OOM_FALLBACK` returned during an already planned CAGRA or flat scan raises SQLSTATE `53200` for that statement; it does not replay the statement on CPU. A later attempt can choose a CPU plan after the breaker opens, subject to the planner gate and threshold.

---

## 3. GUC reference

Defaults and ranges are from source. "Set by" is the minimum role/scope: `USERSET` (any session),
`SUSET` (superuser), `SIGHUP` (config reload), `POSTMASTER` (server start).

### Core

| GUC | Type | Default | Range | Set by | Purpose |
|-----|------|---------|-------|--------|---------|
| `enable_cuvs` | bool | `on` | — | USERSET | Master switch for the GPU path; off routes everything to CPU |
| `cuvs.enable_phys_cost` | bool | `on` | — | USERSET | Use the ADR-075 physical (hardware-anchored) cost model for `cagra`/`flat` when the profile is fully probed. Off (or unprobed/Tier-1) → legacy heuristic constants, byte-identical routing. See § Cost model & hardware profile |
| `cuvs.debug` | bool | `off` | — | USERSET | Emit a per-search NOTICE with daemon latency + metric (for EXPLAIN VERBOSE) |
| `cuvs.socket_path` | string | `/tmp/.s.pg_cuvs` | — | SUSET | UDS path to the daemon |
| `cuvs.daemon_uid` | int | `-1` | -1– | SUSET | Expected owner uid of `cuvs.socket_path`; backend refuses to connect if the socket is owned by a different uid. `-1` disables the check. See playbooks/daemon-restart-recovery.md § Security / hardening |
| `cuvs.index_dir` | string | `$PGDATA/cuvs_indexes` | — | SUSET | Artifact directory (empty = resolved at runtime) |
| `cuvs.k` | int | `100` | 1–2000 | USERSET | GPU candidates fetched per CAGRA scan; the AM does not read SQL `LIMIT`, which the executor applies afterward. Set high enough for the desired LIMIT and recall |
| `cuvs.search_more` | bool | `off` | — | USERSET | Experimental. For eligible CAGRA L2 queries, try one additional candidate search before returning the first row when the first results have too few visible rows |
| `cuvs.search_more_max_components` | int | `8388608` | 0–8388608 | USERSET | Maximum row-by-dimension evaluations in the additional GPU pass. Zero permits only the initial CAGRA search |
| `cuvs.rmm_pool_mb` | int/status | daemon value | 0–INT_MAX, or `unavailable` | read-only (INTERNAL) | Applied initial RMM pool reservation per GPU, queried from the daemon. `0` = disabled; `unavailable` = no compatible daemon reply. Set using the daemon CLI and restart, not SQL SET. Record with each benchmark run |
| `cuvs.rmm_pool_max_percent` | int/status | daemon value | 0–100, or `unavailable` | read-only (INTERNAL) | Applied upstream pool quota as a percentage of total CUDA memory on each GPU. `0` when the pool is disabled |
| `cuvs.rmm_pool_status` | string/status | daemon value | JSON, or `unavailable` | read-only (INTERNAL) | Applied initial MiB, quota percentage, per-device maximum/reserved/used pool bytes, and `hnsw_auto_overflow` (`ace` or `error`, from `--hnsw-auto-overflow`). Counters cover RMM pool allocations; they are live snapshots, not process-wide GPU memory |
| `cuvs.circuit_breaker_threshold` | int | `3` | 1–100 | USERSET | Consecutive GPU errors before the (per-backend) breaker trips |
| `cuvs.circuit_breaker_transient_ttl_s` | int | `30` | 0–3600 | USERSET | Seconds before a breaker opened by a transient reason half-opens and allows one GPU attempt. `0` = half-open immediately. Breakers opened by a permanent reason ignore it. See § 5 `pg_cuvs_reset_circuit` |

`cuvs.search_more` applies to a forward L2 scan of a clean, resident, unsharded
legacy CAGRA index in the daemon's default index directory. Use a single heap
table, a non-null vector column, a constant query vector and a positive constant
`LIMIT <= cuvs.k <= 512`. Plans with filters, joins, or parallel scans keep their
existing route. Both passes use the same statement snapshot and compare the
daemon's graph and TID generation. The extra pass scores only a bounded prefix
of the resident dataset, up to 32,768 rows. The daemon scores at most 1,024
rows and 262,144 vector components per submission, releases the registry lock
between submissions and checks for cancellation before each new submission.
A submitted chunk can finish after a backend cancellation. The configured
limit counts scored vector components in the extra pass. The initial ANN,
GPU time and the number of visible rows found are separate.
`EXPLAIN (ANALYZE)` reports candidate counts,
additional requests, scored rows and the reason the node stopped.

**Known limitations.** Replacing the configured index directory after the
final BUILD identity check, while auxiliary sidecars are being published, can
leave the new files in the previous directory. A subsequent reload from the
configured path can find missing or older artifacts.

In the [#284 A100 trial](https://github.com/pg-cuvs/pg_cuvs/pull/284), the
additional search raised median total execution time from 0.575 ms with the
option off to 5.572 ms with it on. This was a single PostgreSQL 16.16 fixture on
an A100 80GB with 10,000 seed rows, 768 dimensions, 100 deletions, one additional
insert and `LIMIT 100`, using 24 `EXPLAIN (ANALYZE)` samples per mode.

Unlocked CAGRA builds reserve estimated RMM quota separately from logical index
residency under the registry mutex. Admission subtracts all in-flight reservations
from pool headroom and releases them on build success or failure. Since allocator
counters can already include part of a reservation, this is conservative and may
reject a concurrent build until the earlier build returns. Pending-quota pressure
never makes a build evict resident indexes; a search-side reload that is short
because of another build's reservation evicts cold (then, with
`--load-soft-evict on`, protected) indexes to proceed (see § 8). This remains estimate-based admission;
workspace allocations can still produce a genuine allocation failure.

### Build

| GUC | Type | Default | Range | Set by | Purpose |
|-----|------|---------|-------|--------|---------|
| `cuvs.max_build_mem_mb` | int | `0` (auto) | 0–INT_MAX | USERSET | Backend memory cap for accumulating a build corpus; 0 = `MemAvailable × safety_ratio` |
| `cuvs.build_mem_safety_ratio` | real | `0.5` | 0.01–0.95 | USERSET | `MemAvailable` fraction usable when `max_build_mem_mb=0` |

### Search / recall / write-path routing

| GUC | Type | Default | Range | Set by | Purpose |
|-----|------|---------|-------|--------|---------|
| `cuvs.search_mode` | enum | `cagra` | `cagra`, `brute_force` | USERSET | CAGRA ANN vs GPU exact BF on a `cagra` index. `brute_force` value is **deprecated** — use `USING flat` AM instead. A `flat` index always forces exact BF regardless of this GUC |
| `cuvs.gpu_bruteforce` | enum | `off` | `off`, `auto`, `on` | USERSET | **EXPERIMENTAL, hardware-dependent (ADR-074).** Route a no-index single-table vector top-k query to the transient GPU BF CustomScan (`CuvsTransientBF`). On a **PCIe-attached GPU it does NOT beat CPU pgvector** (memory-bound + per-query H2D) — not recommended; use `USING flat` (read-heavy) or plain pgvector no-index (write-heavy). It becomes a first-class W1 path only on **unified-memory hardware** (Grace Hopper / MI300A) where the H2D penalty collapses. `auto` behaves as `off` (reserved until hardware-portable cost auto-enables it where it wins); `on` forces the path for qualifying single-table top-k shapes (ADR-073/ADR-074) |
| `cuvs.gpu_bruteforce_max_mb` | int | `2048` | 1–INT_MAX | USERSET | Host-side hard cap (MiB) on the per-query corpus materialized before the GPU handoff. Queries whose corpus exceeds this limit fail closed with an ERROR |
| `cuvs.bf_precision` | enum | `float32` | `float32`, `float16` | USERSET | BF precision fallback; float16 halves corpus VRAM but quantizes vectors and queries |
| `cuvs.bf_batch_wait_us` | int | `0` (off) | 0–10000 | USERSET | Daemon BF micro-batch coalescing window (µs) |
| `cuvs.cagra_batch_wait_us` | int | `0` (off) | 0–10000 | USERSET | Daemon CAGRA micro-batch coalescing window (µs) |
| `cuvs.itopk_size` | int | `0` (auto) | 0–2147483616, multiples of 32 | USERSET | CAGRA intermediate search buffer floor. Auto preserves `max(64, ceil(actual_k/32)*32)`; an explicit value is raised to cover actual k, including shard/tombstone overfetch. Does not change the number of returned candidates |
| `cuvs.search_width` | int | `0` (SDK default) | 0–INT_MAX | USERSET | CAGRA vertices expanded per iteration. Applies to single, filtered, sharded and batched CAGRA search; ignored by exact BF |
| `cuvs.cpu_hnsw_fallback` | bool | `off` | — | USERSET | Serve from the `.hnsw` sidecar instead of GPU CAGRA |
| `cuvs.hnsw_connectivity_check` | bool | `off` | — | USERSET | After a `pg_cuvs_hnsw` build, run the structural connectivity check and record it in `pg_cuvs_hnsw_build_info(index)->'connectivity'` (see § Build connectivity snapshot). Off skips the check, its scratch allocation and its NOTICE and records `disabled`. Read per index build |
| `cuvs.hnsw_exact_scan` | bool | `off` | — | USERSET | When `on`, ordered scans of a `pg_cuvs_hnsw` index compute exact SQL distances over the MVCC-visible heap rows instead of reading the HNSW graph (see § Explicit exact scans for pg_cuvs_hnsw). A scan runs exactly or stops with an error; it never falls back to approximate results. Changing the value invalidates the saved plans and cached expressions of the current backend. Read at scan start and rescan. Does not recover unreachable rows in the default configuration (#203 stays open) |
| `cuvs.max_stale_fraction` | real | `0.10` | 0.0–1.0 | USERSET | Delete-drift fraction above which a CAGRA index reroutes to CPU; 1.0 disables |
| `cuvs.max_delta_rows` | int | `10000` | 0–INT_MAX | USERSET | Pending-insert rows merged before CPU reroute; 0 disables delta |
| `cuvs.delta_search` | enum | `auto` | `auto`, `cpu`, `gpu` | USERSET | Delta merge mode: GPU-with-CPU-fallback / always CPU (the daemon then neither refreshes nor searches its delta cache) / GPU-only. The `delta_search_mode` column of `pg_stat_gpu_search` is per index, not per session: `cpu` when the last CAGRA search of the index asked for `cpu`, otherwise `gpu` while a delta cache is resident, otherwise `none` |

The CAGRA tuning defaults preserve the previous search parameters. Micro-batches
only combine CAGRA requests with identical tuning, so concurrent sessions can
use different settings. The command frame changed to protocol version 4;
co-deploy the extension and daemon (`make install && make install-server`).

### Filtered search (3O / D-wedge)

| GUC | Type | Default | Range | Set by | Purpose |
|-----|------|---------|-------|--------|---------|
| `cuvs.filter_auto_threshold` | real | `0.0` | 0.0–1.0 | USERSET | Selectivity below which filtered BF uses the **approximate** GPU BITSET prefilter (3O) instead of the exact D-wedge post-filter. Defaults to off: 3O recall collapses on selective filters (0.28 at 1e-4, ADR-082) and the exact paths are faster there. **Selectivity does not predict the worst case (ADR-083, #133): the collapse point depends on filter *shape* (query correlation), not selectivity.** An anti-correlated filter measured recall 0.0 across the entire 0.0001–0.5 selectivity range — no threshold value here filters it out. The daemon mitigates by detecting a materially short fill and retrying on the GPU exact BF prefilter (`gpu_bf_prefilter`; **not** the D-wedge post-filter above — different cost model, see `pg_stat_gpu_search.prefilter_fallback_count`); this is a mitigation, not a guarantee for every filter shape |
| `cuvs.stream_bf_selectivity_threshold` | real | `0.004` | 0.0–1.0 | USERSET | Selectivity below which filtered BF streams out-of-core from `.vectors`; `0.004` is the canonical-host operational baseline, not a portable crossover. A second host measured the crossover below `0.002`; recalibrate for the deployment ([report](reports/2026-08-04-item2b-second-host-replication.md), ADR-064/082) |
| `cuvs.stream_bf_chunk_vectors` | int | `262144` | 1–INT_MAX | USERSET | Vectors per GPU chunk in streaming BF (footprint only; result is exact for any chunking) |
| `cuvs.filtered_knn_hook` | bool | `off` | — | USERSET | Enable the D-wedge Custom Scan hook (ADR-063 spike) |
| `cuvs.max_batch_queries` | int | `1024` | 1–4096 | USERSET | Max query vectors per `pg_cuvs_batch_search` call |

### Sharding (multi-GPU)

| GUC | Type | Default | Range | Set by | Purpose |
|-----|------|---------|-------|--------|---------|
| `cuvs.shard_count` | int | `0` (auto) | 0–256 | USERSET | 0 = auto from VRAM budget, 1 = unsharded, ≥2 = force N shards (set at build) |
| `cuvs.shard_overfetch` | int | `0` | 0–4096 | USERSET | Extra candidates per shard before the global top-k merge |
| `cuvs.parallel_fanout` | bool | `on` | — | USERSET | Dispatch per-shard searches concurrently (off = sequential) |

### IVF-PQ / streaming updates

| GUC | Type | Default | Range | Set by | Purpose |
|-----|------|---------|-------|--------|---------|
| `cuvs.ivfpq_n_probes` | int | `64` | 1–4096 | USERSET | IVF clusters probed per query (≤ `n_lists`); higher = better recall |
| `cuvs.extend_chunk_size` | int | `0` (auto) | 0–65536 | USERSET | CAGRA `extend` max chunk size (Phase 3Q) |
| `cuvs.compact_delete_ratio` | real | `0.10` | 0.0–1.0 | USERSET | Deleted-vector fraction that triggers auto-compact after VACUUM |

### GCS snapshot / warmup (multi-node)

These backend GUCs are registered but are not read by the backend build/search path or forwarded to the daemon. Configure the separate daemon flags below for active snapshot and warmup behavior.

| GUC | Type | Default | Range | Set by | Purpose |
|-----|------|---------|-------|--------|---------|
| `cuvs.snapshot_uri` | string | `` (off) | — | SUSET | Registered backend setting only; does not configure daemon GCS snapshots |
| `cuvs.cluster_id` | string | `` | — | SUSET | Registered backend setting only; does not configure daemon cluster ID |
| `cuvs.gcs_key_file` | string | `` | — | SUSET | Registered backend setting only; does not configure daemon credentials |
| `cuvs.warmup_threads` | int | `2` | 1–8 | SUSET | Registered backend setting only; does not configure daemon warmup threads |

CAGRA compaction (`pg_cuvs_compact` and VACUUM) rebuilds the kept rows with the
index's `graph_degree` and `intermediate_graph_degree` reloptions (before #261 it
used cuVS's defaults, 64 and 128) and logs
`[cuvs_cagra_compact] rebuild graph_degree=... intermediate_graph_degree=...`.
An index compacted to width 64 by an earlier version gets its `graph_degree`
back at its next compaction. The values are read at each compaction, so a value
set by `ALTER INDEX ... SET` applies from the next compaction, before any
`REINDEX`, and the session that ran the compaction gets a NOTICE
`pg_cuvs: compaction rebuilt index "..." at graph width N (was M)` when the width
moved to `graph_degree` (a graph that cuVS narrows because the index has
fewer rows than the degree gets no NOTICE). If the reloptions have `intermediate_graph_degree` below
`graph_degree`, which `CREATE INDEX` rejects, compaction keeps the current
graph width and raises a WARNING (`pg_cuvs: compaction of index "..." kept graph width
N: intermediate_graph_degree (...) is below graph_degree (...)`, with a hint)
instead of an error. Each message appears once per compaction and index, and
not when there was nothing to compact. Under autovacuum the WARNING goes to
the server log (the NOTICE does so only if `log_min_messages` is `notice` or
lower).

Compaction rebuilds the graph with cuVS's automatic builder choice whatever
the index's `build_algo` is; only `graph_degree` and
`intermediate_graph_degree` are taken from the index. Compaction runs inside
VACUUM, usually with nobody watching, so it uses the choice that falls back
to another builder when memory is short: on fp16-safe data cuVS takes
NN-descent when the device has enough memory for it and IVF-PQ otherwise
(on fp16-unsafe data the daemon makes the same choice before the merge and
uses fp32 IVF-PQ distances), and a memory failure leads to the explicit IVF-PQ
retry described below. Measured on one A100 (100,000 rows of 768-dimension
wiki embeddings, 20% deleted, then compacted; recall@10 over 500 queries; three
runs per setting): an index built with `build_algo = 'ivf_pq'` had a
recall@10 0.0002 to 0.0010 lower after compaction than after a `REINDEX` with
`ivf_pq` (`graph_degree` 64 and 128), and an index built with `nn_descent`
was within the run-to-run variation. All these values are 0.997 or higher; a
lower search budget was not measured. To get a graph built with the
index's `build_algo`, run `REINDEX`, which builds it with that method.
Compaction initially preserves the auto
builder choice. A memory failure from NN-descent or an unresolved auto choice
triggers one IVF-PQ merge retry after the first merge's allocations unwind.
The retry caps the IVF-PQ internal search batch at 4096 rows (the ordinary
IVF-PQ default, versus the CAGRA preset of 131072) to reduce scratch use.
It also reduces the training fraction to target at most max(65536, 32 ×
coarse-list count) rows without increasing the preset fraction. The SDK
rounds its integer sampling, so the actual training count may be higher.
This changes only the recovery graph builder and can affect recall and build
time; it does not change the fp16/fp32 distance rule.
It recomputes the row-norm bound: LUT, internal and coarse distances
are fp32 if the fp16 bound is reached; otherwise fp16 defaults remain.
An already pinned IVF-PQ failure, a non-memory exception, or a failed request
stream/host-free fence is not retried. This also recognizes the pinned SDK's
`Filtered merge isn't available on cpu memory` translation of `bad_alloc`.
The retry may still fail if even the merge's dataset copies do not fit.

The daemon logs `[cuvs_cagra_compact] retry=1 to=ivf_pq reason=oom begin`,
`[cuvs_cagra_compact_retry] knn precision` (including dtype),
`retry=1 ivfpq_max_internal_batch_size=4096`,
`retry=1 ivfpq_training_rows_target=... kmeans_trainset_fraction=...`, and
`retry=1 to=ivf_pq outcome=succeeded|failed`. Manual compaction and VACUUM
also receive a NOTICE with the retry dtype and result once the retry params
are selected. The handle swap increments `compact_count`; successful persistence removes
the tombstone file. A failed merge keeps the original index and tombstones.

### Auto-compaction (Phase 4C bgworker)

| GUC | Type | Default | Range | Set by | Purpose |
|-----|------|---------|-------|--------|---------|
| `cuvs.auto_compact` | bool | `off` | — | SIGHUP | Auto-`REINDEX CONCURRENTLY` when delta growth crosses the threshold |
| `cuvs.auto_compact_check_interval` | int | `60` | 10–3600 | SIGHUP | Seconds between checks |
| `cuvs.auto_compact_threshold` | real | `0.10` | 0.01–1.0 | SIGHUP | Trigger when `extend_count / n_vecs` exceeds this |
| `cuvs.auto_compact_database` | string | `` | — | POSTMASTER | Database the bgworker monitors; empty = disabled |

### Daemon CLI flags (not GUCs)

`pg_cuvs_server` flags (set in the systemd unit `ExecStart`). Most mirror a `cuvs.*` GUC; the
daemon-only ones have no session equivalent.

| Flag | GUC equivalent | Default | Purpose |
|------|----------------|---------|---------|
| `--socket PATH` | `cuvs.socket_path` | `/tmp/.s.pg_cuvs` | UDS path |
| `--index-dir DIR` | `cuvs.index_dir` | — | Artifact dir the daemon serves from; must match the backend's `index_dir` or searches fall back to seqscan |
| `--max-vram-mb N` | — (daemon-only) | 90% of total VRAM | Logical index-residency budget, enforced by self-accounting (ADR-065). Excludes unused pool reservation and temporary workspace; it is separate from the RMM quota |
| `--rmm-pool-mb N` | `cuvs.rmm_pool_mb` (read-only status) | `512` MiB | Initial RMM pool reservation per allowed GPU, installed before probes/load/warmup. Explicit `0` disables the pool and keeps the legacy allocator. CPU shim builds default to `0`. Init failure aborts startup |
| `--rmm-pool-max-percent N` | `cuvs.rmm_pool_max_percent` (read-only status) | `50` | Hard upstream quota: total CUDA memory × N%, rounded down to 256 bytes, on each allowed GPU. Range 1–100; initial reservation must fit. Preserves the pool's doubling/backoff growth policy |
| `--rmm-scratch-permille N` | — (daemon-only, `scratch_permille` in `cuvs.rmm_pool_status`) | `9593` (Q1 qualification on one NVIDIA A100-SXM4-80GB host with `--cagra-build-overflow managed`; recalibrate for other GPUs or overflow policies) | CAGRA build scratch coefficient ρ. Admission, reservation and GPU choice use `N = final + ceil(final × ρ / 1000)`; residency accounting keeps `final`. Range 0–100000. Restart to change |
| `--max-indexes N` | — (daemon-only) | `1024` | **Soft** LRU working-set cap (ADR-068), not a hard wall (was 64). At the cap, a reload or `pg_cuvs_load` frees a slot under the § 8 rules (cold first, then soft eviction; reason `slot`); a build over the cap does not evict (with the default `--build-evict-scope none`): its files are saved and the first query reloads it |
| `--page-overlay-host-bytes N` | — (daemon-only) | `0` | Host budget for page-overlay chunks during receive, residency and active searches, plus fold staging, mirrors and retained host graph buffers when folding is enabled. A zero budget disables chunk allocation. Overlay IPC returns `PAGE_OVERLAY_HOST_FULL` to the caller on budget exhaustion. The per-index row limit also applies. |
| `--build-evict-scope …` and the other cache flags | — (daemon-only) | see § 8 | Cache, eviction and reclaim knobs (§ 8.2) |
| `--hnsw-auto-overflow ace\|error` | `hnsw_auto_overflow` in `cuvs.rmm_pool_status` (read-only) | `ace` | What `pg_cuvs_hnsw` `build_algo=auto` does over the resident budget: `ace` spills to ACE-disk with a NOTICE, `error` refuses with SQLSTATE 54000. The same value decides an auto build whose resident IVF-PQ kNN graph cuVS rejects: `ace` rebuilds on ACE-disk with a NOTICE (receipt `graph_fallback`), `error` fails. Startup-only; logged at startup. Explicit `ivf_pq`/`ace` ignore it |
| `--hnsw-ace-build-quality auto\|Q` | — (daemon-only, restart) | `auto` | ACE partition build quality handed to cuVS (`ace_params.ef_construction` = A; the SDK uses q = A/16, NN-descent runs 5 + q iterations per partition under 1e6 rows). `auto`: q = (the SDK's default NN-descent iterations, 20) − 5 at `ef_construction` 64, i.e. A 240, scaled by `ef_construction`/64. `Q` (0–4095): A = 16·Q for every ACE build, ignoring `ef_construction`. Logged at startup; recorded per index in `pg_cuvs_hnsw_build_info` (`ace_build_quality_source`, `ace_build_quality`). Resident builds ignore it |
| `--cagra-build-overflow error\|managed` | — (daemon-only, restart) | `managed` | Where a CAGRA build's large_workspace lives (ADR-088). `managed`: a build-only managed-memory resource outside the RMM pool and quota that can spill to host RAM (slower, see below). `error`: inside the pool, as before. Applies to `cagra` single and parallel (multi-partial) builds only; sharded builds, startup warmup and `pg_cuvs_hnsw` always use `error` |
| `--managed-overcommit on\|off` | — (daemon-only, restart) | `on` | Policy `managed` only. `off`: refuse (SQLSTATE 53400, reason AR6) a build whose large_workspace estimate would not fit in physical GPU free memory. `on`: allow spill up to the host memory guard, refuse beyond it (53200, AR7) |
| `--managed-lw-margin-permille N` | — (daemon-only, restart) | see `SHOW cuvs.rmm_pool_status` (`lw_margin_permille`) | Policy `managed` only. Safety factor on the large_workspace estimate `L_shape`. Range 1000–100000; out of range refuses startup |
| `--rmm-build-headroom-permille N` | — (daemon-only, restart) | `150` | AR8 headroom for a CAGRA single or parallel build (ADR-089): when `pending + pool_used + N + headroom` exceeds the pool quota, the build eviction scope may evict for the shortfall, otherwise the build is refused (AR5 or AR8, see RMM quota and memory accounting), with `headroom = max(256 MiB, ceil(N × value / 1000))`. `0` removes the headroom but keeps the capacity check; it does not refuse the Q6 shape (see ADR-089). Range 0–100000; out of range refuses startup. The value is on the `CAGRA build pool headroom` startup log line |
| `--oom-score-adj N` | — (daemon-only, restart) | `500` | Written once to `/proc/self/oom_score_adj` at startup so the kernel OOM killer prefers the daemon over PostgreSQL under host RAM pressure. Range 0–1000; `0` leaves the file untouched; a higher current value is kept (never lowered); a write failure is logged, not fatal |
| `--gpu-devices LIST` | — (daemon-only) | all visible | CUDA devices the daemon uses (else honors `CUDA_VISIBLE_DEVICES`) |
| `--snapshot-uri URI` | separate daemon flag; backend GUC is not forwarded | `` | GCS snapshot root |
| `--cluster-id ID` | separate daemon flag; backend GUC is not forwarded | `` | Cluster id in the GCS artifact path |
| `--gcs-key-file PATH` | separate daemon flag; backend GUC is not forwarded | `` | Service-account JSON path |
| `--warmup-threads N` | separate daemon flag; backend GUC is not forwarded | `2` | Background GCS warmup pool size |

### RMM quota and memory accounting

The current device resource is installed before hardware probes, index reload,
warmup and worker threads, so the pool covers retained index data and builds as
well as search workspace. The quota is imposed by the upstream limiting resource;
the pool constructor's maximum is left unset to retain its doubling/backoff policy.
The initial reservation is not the maximum, and it does not shrink after a build.

Admission uses **unused pool bytes + min(reclaimable physical memory, remaining
upstream quota)**. An index estimate larger than an empty pool's quota is rejected
before evicting resident indexes. The logical index budget still applies separately.

A CAGRA build (single, parallel or per shard) is admitted on
`N = final + ceil(final × ρ / 1000)`, where `final = n × (4 × ld + 4 × graph_degree)`
and `ld` is the dimension rounded up to a multiple of 4; ρ is
`--rmm-scratch-permille`. The registry records `final`. A build that still runs
out of GPU memory after admission fails with SQLSTATE 53200 and evicts no resident
index (there is no evict-and-retry); raise `--rmm-scratch-permille` to reserve
more. IVF-PQ builds and index reloads keep their own estimates.

While a request holds a GPU stream, host buffers that cuVS frees are released only
after that stream's queued work has completed, including when a failed build unwinds.
If waiting for the stream reports a CUDA error, the buffer is kept allocated
instead of freed, because work still queued may write into it. The daemon then logs
`[host_free_fence] stream wait failed: err=... stream=... device=...` with the bytes
kept and discards that request's GPU resources instead of reusing them. An index
build, extend, load or save that hit such a failure is reported as failed. The kept
memory is not reused until the daemon restarts.

A CAGRA single or parallel build must also fit the pool next to what is already
there (ADR-089): `pending + pool_used + N + headroom ≤ quota`. `pool_used` is the
pool's used bytes before eviction (resident indexes), `pending` is what builds already
admitted on that GPU hold (their `N` plus their `lw_ub`), and `headroom = max(256 MiB,
ceil(N × --rmm-build-headroom-permille / 1000))`, or 0 when the flag is 0. This build's
own `lw_ub` is not a term: the managed large_workspace is not pool memory. When the pool
is short, the shortfall goes to the build's eviction scope (`--build-evict-scope`,
§ 8) together with the logical-budget and physical shortfalls, after the AR6/AR7
large_workspace check: the residents the scope allows are evicted and the build is
admitted; if the scope allows too few but evicting idle unpinned indexes would make
room, the build is refused with AR5 and nothing is evicted (the default scope `none`
always lands here). AR8 refuses when no eviction could free the pool shortfall (it
exceeds the resident pool use or every evictable index), with nothing evicted, or when
the pool, read again after an eviction, is still short. AR8 also refuses with nothing
evicted while another build or lock-free GPU job (a sharded search, an HNSW export) is
using the same GPU's pool: that work can grow the pool during the eviction, so the
eviction might free nothing usable. Nothing is reserved on a
refusal. A REINDEX whose pool is short does not unload its own old copy (§ 8.5
self-replace applies only when the pool fits), and an AR5 decided by the pool
shortfall carries no "wait for reclaim" hint. Without a pool quota AR8 never applies. Sharded builds, reloads and manual
loads do not run it.

`pg_stat_gpu_search.vram_bytes` estimates retained dataset/graph memory; it does
not include unused pool reservation or transient workspace. Pool-backed frees
become reusable without returning their physical allocation to other CUDA users.
The maximum row-norm reduction used for CAGRA build/compaction uses RAFT's
RMM Thrust policy on the request stream, so its temporary device storage is
included in the current device resource's pool usage and quota when enabled.
cuVS may also allocate outside the current RMM resource (for example managed or
CUDA async scratch); the pool quota does not bound those allocations, CUDA context
memory, or total `nvidia-smi memory.used`. Record both pool status and physical GPU
usage when assessing memory pressure:

```sql
SHOW cuvs.rmm_pool_mb;
SHOW cuvs.rmm_pool_max_percent;
SHOW cuvs.rmm_pool_status;
```

The JSON has `initial_mb`, `maximum_percent`, and a `devices` array containing
`device_id`, `maximum_bytes`, `reserved_bytes`, and `used_bytes`, followed by
`version` (3) and `scratch_permille`. New keys are only appended after these. A disabled pool
reports initial/percentage zero and an empty device array. An absent or incompatible
daemon reports `unavailable`, not a fabricated zero. These are read-only settings;
changing them with SQL SET cannot replace a live allocator. See ADR-085 for the
measured default rule and its single-GPU scope.

### Managed large_workspace for CAGRA builds (ADR-088)

Under `--cagra-build-overflow managed` (default) a `cagra` build's large_workspace
comes from a build-only CUDA managed-memory resource that is installed after the
build borrows its resources and restored before they return to the pool, on every
exit path. It is outside the RMM pool, the quota and the logical budget, so
`SHOW cuvs.rmm_pool_status` pool usage and `nvidia-smi` can disagree during such a
build. If GPU memory runs short, large_workspace pages can move to host RAM.
This does not guarantee progress when physical GPU memory is exhausted (see the
physical backing guard below). A spill can slow the build (not measured), and
the requester receives:

```
NOTICE:  pg_cuvs: build used managed memory spill (large_workspace peak <MiB>, physical free <MiB>); the build may be slower than usual
```

Only the large_workspace part can spill. The in-pool part of the build is still
admitted against the pool and can still fail with an out-of-memory error after
admission. Before a managed build starts, the daemon estimates
`lw_ub = ceil(L_shape × lw_margin_permille / 1000)` with
`L_shape = max(4·n·ld + 4·n·igd, n·(5·igd + 4))` bytes (`ld` = dim rounded up to a
multiple of 4, `igd` = intermediate_graph_degree) and refuses the build when
`max(0, N_m − (reserved − used)) + lw_ub + lw_pending` exceeds physical free GPU
memory (overcommit `off`: AR6, 53400) or exceeds it by more than the host guard
`min(MemAvailable/2, cgroup remaining/2)` (overcommit `on`: AR7, 53200; an
unreadable cgroup or cgroup v1 gives a guard of 0). `lw_pending` is the sum of
`lw_ub` over managed builds already admitted on the same GPU. The estimate is not
an upper bound.

**Physical backing guard.** Before CAGRA single, parallel or shard builds,
direct IVF-PQ builds, HNSW file builds and CAGRA compaction enter cuVS, the daemon
checks raw `cudaMemGetInfo` free memory on the selected GPU. Below **32 MiB**
(33,554,432 bytes), or if the query fails, it defers the operation with
SQLSTATE **53200**, reason `PHYS`, and a NOTICE naming physical memory pressure.
Retry after other GPU users release memory; a daemon restart is not needed for
this rejection. Idle RMM pool bytes and cached CUDA async allocations do not
count: they cannot back the SDK's HMM access to pageable host metadata or its
direct managed allocations. The default overflow policies remain `managed` and
overcommit `on`; ordinary builds keep their existing algorithm and allocator.

This is an exhaustion mitigation, **not a memory reservation or a bound on a
build's peak**. The 32 MiB floor leaves margin above the observed sub-MiB failure,
but does not prove that all shapes fit. Another process, a concurrent build or
later SDK allocations can consume the sampled free memory before an HMM fault.
Sticky CUDA errors and daemon abort remain possible in that race. Use GPU
isolation or coordinate physical memory use when that risk is unacceptable.
The guard does not change the SDK's host memset or direct managed allocations.

`SHOW cuvs.rmm_pool_status` includes `build_physical_floor_bytes` and
`physical_build_guard`, an array of `device_id`, `rejects`, and
`last_reject_free_bytes` (0 before any rejection; check `rejects`). Counters reset
on daemon restart and stay separate from `rejects_ar0` through `rejects_ar8`.
The requester can also inspect `pg_cuvs_last_rejection()` (`reason=PHYS`,
`need_mib=32`, `phys_free_mib`). Logs use `[physical_build_guard]` with the
operation, GPU, database/index OIDs, raw free bytes, floor and `retryable=1`.
The text payload adds `PHYS n=32 u=<MiB>` without changing the IPC frame size;
deploy the extension and daemon together so both recognize the new reason.

Status keys appended to `SHOW cuvs.rmm_pool_status`: `cagra_build_overflow`,
`managed_overcommit`, `lw_margin_permille`, `oom_score_adj`,
`oom_score_adj_effective` (-1 when not read), `oom_score_adj_result` (the startup
line), and `managed_lw`, an array of `device_id`, `lw_pending_bytes`,
`managed_spill_builds`, `managed_spill_bytes_max`. The daemon logs one
`[managed_lw]` line per cagra build (`policy`, `lw_ub`, `lw_peak`, `spill`,
`free_at_peak`, `lw_pending_after`, `applied`, `restored`, `live_end`).

**Host protection.** Spill uses host RAM, so the host OOM killer can still end the
daemon; the guard and the settings below lower that risk but do not remove it, and
their effect depends on kernel settings. In the daemon's systemd unit, raise
`OOMScoreAdjust=` (for example `500`, matching `--oom-score-adj`) so the kernel
kills the daemon before PostgreSQL, and set `MemoryMax=` to bound the daemon's host
memory. The startup log line reports `oom_score_adj=<n> applied`,
`oom_score_adj kept current <n>`, or `oom_score_adj not applied: <reason>`
(`value 0`, `permission denied`, `not Linux`).

---

## 4. Reloptions

### `flat`

| Reloption | Type | Default | Range / values | Notes |
|-----------|------|---------|----------------|-------|
| `precision` | enum | `float32` | `float32`, `float16` | Resident BF index precision. `float16` halves corpus VRAM (N·dim·2 B vs ·4 B) but quantizes corpus and query, so float32 top-k identity/recall is not guaranteed; falls back to `cuvs.bf_precision` GUC if not set |
| `index_dir` | string | (uses `cuvs.index_dir`) | path | Per-index artifact directory; same semantics as `cagra` (ADR-045) |

VRAM sizing: `float32` requires N·dim·4 bytes resident (e.g. 1M×1024 ≈ 4 GB); `float16` halves
it. If the corpus does not fit in free VRAM at build time the daemon returns a dedicated
no-fit error ("corpus N·dim·4B exceeds free VRAM; try precision=float16, shard, or use
cagra/ivfpq") rather than silently failing.

### `cagra`

| Reloption | Type | Default | Range / values | Notes |
|-----------|------|---------|----------------|-------|
| `graph_degree` | int | `64` | 8–512 | CAGRA output graph degree. Compaction rebuilds the graph at this degree |
| `intermediate_graph_degree` | int | `128` | 8–1024 | Must be ≥ `graph_degree` (fail-closed) |
| `build_algo` | enum | `auto` | `auto`, `ivf_pq`, `nn_descent` | CAGRA build algorithm. The IVF-PQ kNN search (`ivf_pq`, or `auto` when cuVS picks IVF-PQ) uses fp32 LUT, internal and coarse distances when 4 x max\|x\|^2 of the corpus (max\|x\|^2 for inner product and cosine) reaches 65504, the fp16 maximum; otherwise cuVS's fp16 defaults stay. The daemon logs `[cuvs_cagra_build_multi] knn precision` with the bound and the choice. Compaction (`pg_cuvs_compact`, VACUUM) rebuilds the kept rows with cuVS's `auto` choice under the same rule, whatever this option is, and logs `[cuvs_cagra_compact] knn precision`; `REINDEX` rebuilds with this option |
| `index_dir` | string | (uses `cuvs.index_dir`) | path | Per-index artifact directory (ADR-045); self-describes in `reloptions` so no-GUC sessions still find it |

**Experimental page format (`CUVS_PAGE_FORMAT=1`).** An expression CAGRA index
uses page storage only when its result type declares `vector(n)` with
`1 ≤ n ≤ 2030`; an expression without a declared dimension or above that limit
uses legacy storage, regardless of table contents, TRUNCATE, or REINDEX. Pending
page records hold the vector evaluated by PostgreSQL. Folded records use TID
slots. Rebuilding the graph from those slots requires reevaluating the expression.

**Experimental resident pending search.** With matching page-format builds of
PostgreSQL's extension and its daemon, set `cuvs.page_resident_search=on` in
`postgresql.conf` and restart the preloaded primary. This setting defaults to
`off`. A regular CAGRA Index Scan can reuse the daemon's committed pending
prefix and send at most 4,096 remaining positions. PostgreSQL scores larger
unsupplied ranges in 4,096-position chunks. The search admits up to 200,000
pending positions relative to the selected graph and verifies each returned
candidate against the scan snapshot. Retained graphs for old snapshots count
toward the daemon's VRAM budget.

With native page-format fold support, `cuvs.resident_supply=on` starts a
background supplier that visits page-format CAGRA indexes and releases expired
graph history. This postmaster setting also defaults to `off`. The worker uses
the daemon's `--page-overlay-host-bytes` budget.

Native page-format daemons can opt into GPU scoring for the resident prefix
with `--page-overlay-gpu-min-rows`. The default `0` keeps CPU scoring.
`--page-overlay-gpu-cache-mb` caps the mirrors and scoring workspace per GPU
(default `1024` MiB). GPU scoring requires an RMM pool. New allocations use
existing pool backing and defer while a fold is active. A fold that needs
backing can suspend GPU scoring and reclaim the device allocations and the
workspace. When a fold cannot reserve host staging, the daemon can reclaim
workspaces across all devices for its next attempt. If enough rows cannot use
the GPU, the daemon scores the prefix on the CPU.

Each cached device mirror follows its immutable host chunk. It is released
when the last graph or cursor releases that chunk. The host budget charges
packed vectors and mirror metadata together at chunk allocation, and charges
the pinned scoring workspace separately. The GPU cap and
daemon VRAM admission also account for the device allocations. Nonstandard
host floating-point modes use the CPU. The selected
device returns scores through pinned memory. The daemon resolves uncertain
scores with its existing ordered CPU calculation and passes all finite scores
to the resident cursor's visibility checks.

**Experimental page-format fold (`PGCUVS_BETA3A_GPU_FOLD=1`).** Build both the
extension and daemon with this option and `CUVS_PAGE_FORMAT=1`, using the
matching cuVS `cpp/src` tree as `CUVS_BETA3A_SDK_SOURCE`. The fold option defaults
to `0`. On a preloaded primary, `SELECT pg_cuvs_compact('index_name');` can
supply committed pending rows or publish one ready fold for the specified
page-format CAGRA index. The function uses the existing index-owner check and
reports the outcome as a `NOTICE`. Repeat the call to publish a candidate that
was still being prepared. Use a writable statement outside a transaction
block or subtransaction. Folding requires an existing base GPU graph. After a
daemon or PostgreSQL restart, PostgreSQL
retains the WAL-published page state and can use exact heap search if the
matching GPU graph is unavailable.

The daemon reads these environment variables at startup. Restart it to apply
a change.

| Variable | Default | Effect |
|----------|---------|--------|
| `CUVS_PAGE_FOLD_BATCH_ROWS` | `0` (or unset) | Uses the existing GPU fold algorithm, with up to 16,384 pending positions per fold. A graph becomes eligible at 100 pending positions or after 300 seconds with pending positions. An integer from `1` to `16384` enables optimized folding, caps the number of positions at that value and makes the graph eligible from the first pending position. Other values log a warning and use the default policy. |
| `CUVS_PAGE_FOLD_PROFILE` | `0` (or unset) | `1` enables diagnostic `[page_fold_phases]` lines in either fold mode. The lines report `staging_us`, `input_us`, `dataset_us`, `graph_setup_us`, `incoming_us`, `search_us`, `rank_us`, `reverse_us`, `scatter_us`, `final_graph_us` and `host_graph_cache_hit`. Timing can add GPU synchronization. Other values log a warning and leave profiling off. |

Set `--page-overlay-host-bytes` to cover the supplied overlay, packed fold
input and host mirrors of the expanded output. The existing path mirrors the
graph and dataset. Optimized folds can copy the dataset directly between GPU
buffers when the dataset row stride equals the vector dimension. That path
only needs a host graph mirror. Each fold producer can retain one host graph
buffer for the next fold of the same graph chain, with its allocated capacity
charged to the same budget. The producer frees it when switching chains,
after 30 seconds without a successful update, or on thread exit. The fold
can use an SDK-owned host graph when the cache allocation cannot fit.
Supply and fold requests under host-budget pressure reclaim idle buffers
across devices. Producers release borrowed buffers when their current fold
finishes if a request reported pressure in the meantime.

With the RMM pool enabled, a fold reuses the pool's existing backing and can
defer when that backing cannot satisfy its allocations. A fold cannot grow the
pool's backing, which could stay resident after the fold yields to a build.

The daemon preserves the parent and candidate under memory pressure once it
has selected a `FOLD_READY` reply for transmission. The graphs stay protected
until the matching `FOLD_COMMITTED` or a PostgreSQL lifetime change. A lost
reply or acknowledgment can therefore leave two graphs resident. Competing
builds in that interval can receive existing admission rejections, including
`AR8` when the RMM pool cannot provide the required capacity. The foreground
zero-new-rejections gate covers requests without memory pressure during the
GPU fold before READY. Zero new rejections under pressure, including the
READY-to-COMMITTED interval, will be revalidated in γ.

Known admission limits of the experimental flag (sizes in MiB):

- BUILD placement can choose a GPU without accounting for the RMM headroom
  that the actual admission check requires. For two GPUs with budget and
  quota 2,048 each, a BUILD needing 1,280 and headroom 256, GPU 0 can admit
  the build by evicting an eligible LRU of 1,280 out of its 1,536 used. GPU 1
  uses 1,537, including an unoffered candidate of 641 and eligible LRU of
  256. Together they cover the placement shortfall of 769. They cannot
  cover the actual pool shortfall of 1,025, so placement on GPU 1 can still
  return `AR8`.
- A legacy EXTEND can wait for a fold on GPU 0 while UNLOAD then LOAD moves
  its index to GPU 1. With 1,537 used on GPU 1, a 600 increase and a 2,048
  cap, the request is short by 89. An unoffered candidate of 513 on GPU 1
  stays in place because the request waited on GPU 0, so EXTEND returns the
  existing status 5 (`not enough VRAM for extend`).
- With RMM, discarding an unoffered candidate of at least 16 can reduce the
  live ledger while the pool keeps its backing. If raw free memory remains
  16 after the discard, the unchanged 32 MiB floor can still reject the
  build with `PHYS`.

Sustained inserts can still drive search p99 into seconds when pending rows
force exact heap search.

### `ivfpq`

| Reloption | Type | Default | Range | Notes |
|-----------|------|---------|-------|-------|
| `n_lists` | int | `1024` | 1–65536 | IVF cluster count |
| `pq_bits` | int | `8` | 4–8 | Bits per PQ code |
| `pq_dim` | int | `0` (auto → ~dim/2) | 0–65536 | PQ subspace count |
| `index_dir` | string | (uses `cuvs.index_dir`) | path | Per-index artifact directory; same semantics as `cagra` (ADR-045) |

### `pg_cuvs_hnsw`

| Reloption | Type | Default | Values | Notes |
|-----------|------|---------|--------|-------|
| `source` | string | `` | a legacy `cagra` index name on the same table | empty = private direct HNSW file build from the heap (op23), without a persistent CAGRA index |
| `mode` | string | `nsw` | `nsw`, `hnswlib`, `hnsw`, `hnswlib_file` | `nsw` and `hnswlib` are recommended. With `source`, `nsw` and `hnsw` give the same graph: level 0 from the CAGRA graph and upper layers built by the daemon on the host. Since 0.9.3 this path checks the pgvector version as the heap build does (outside the supported range is an error, not a warning) |
| `m` | int | `32` | 2–100 | Heap builds use `m` as the HNSW neighbor capacity, including the pgvector maintenance fallback of a source index. With `source`, the converted graph supplies M. A differing explicit value produces a warning. |
| `ef_construction` | int | `64` | 4–1000 | Sets construction effort for direct heap builds. The build records the configured value in the index metapage. A conversion from a legacy CAGRA index uses the option only for that metapage value during the build. pgvector maintenance fallback uses at least `2 * m`. |
| `build_algo` | enum | `auto` | `auto`, `ivf_pq`, `nn_descent`, `ace` | Direct heap build only; cannot be combined with explicit `source` |

A heap build reserves `m` neighbors per upper layer and 2 × `m` at level 0. The metapage records
`m`, which later inserts use. The GPU requests degree 2 × `m` and reduces it for small inputs.
`pg_cuvs_hnsw_build_info()` reports the constructed graph width as `graph_degree` and adds
`graph_degree_limited_by: "input_rows"` when that width was reduced. Tiny graphs may include
padding in their serialized width. The daemon removes that padding from the serialized neighbor counts before import.
Choose `m` above 3 for general use. Values 2 and 3 substantially lowered recall in a 5,000-row
test (128 dimensions), and quality depends on the data.

In source modes `nsw` and `hnsw`, the converter caps the CAGRA degree at 64 and rounds it down
to an even number, then divides by two. Modes `hnswlib` and `hnswlib_file` read M from the SDK
or file header. Omit `m` to inherit that value, including an existing header with M above 32.
A differing explicit `m` produces one warning per build and the build uses the graph's M.
The explicit value still applies when maintenance rebuilds the index from the heap through
pgvector, so the index then has that M.
Use `(pg_cuvs_hnsw_layer_counts('index_name')).m` to see the built value.
`ALTER INDEX ... SET (m = ...)` changes the stored option without rebuilding the index. The new
setting applies on the next `REINDEX` or other rebuild.
Use `ALTER INDEX ... RESET (m)` to restore the default for a future heap build or inherit M on the
next source conversion.

An `m` stored on an existing heap-built index by an earlier release starts
affecting its graph on the next rebuild, including a restore. A heap build without
an explicit `m` on a table with 1–64 rows now stores M 32 and reserves slots
for it. The graph itself can still use its smaller width from the row count.

The query below finds valid heap-built indexes whose explicit option differs
from the M they currently store. It reads index pages through
`pg_cuvs_hnsw_layer_counts` and requires SELECT on those tables.

```sql
WITH heap_indexes AS MATERIALIZED (
    SELECT c.oid::regclass AS index_name, opt.option_value::integer AS configured_m
    FROM pg_class c
    JOIN pg_am a ON a.oid = c.relam
    JOIN pg_index i ON i.indexrelid = c.oid
    CROSS JOIN LATERAL pg_options_to_table(c.reloptions) opt
    WHERE a.amname = 'pg_cuvs_hnsw' AND c.relkind = 'i' AND i.indisvalid AND i.indisready
      AND opt.option_name = 'm'
      AND NOT EXISTS (
          SELECT FROM pg_options_to_table(c.reloptions) src
          WHERE src.option_name = 'source' AND src.option_value <> ''
      )
), sizes AS MATERIALIZED (
    SELECT index_name, configured_m, (pg_cuvs_hnsw_layer_counts(index_name)).m AS built_m
    FROM heap_indexes
)
SELECT * FROM sizes WHERE configured_m IS DISTINCT FROM built_m;
```

For later inserts, pgvector reads `ef_construction` from the index reloption.
`ALTER INDEX … SET (ef_construction = …)` changes that option for subsequent
inserts. The value recorded in the metapage is refreshed by `REINDEX`.

A CAGRA source must be valid and free of pending inserts and deletes. A stale
source or pending sidecar records cause the conversion to fail with a hint to
rebuild the source or build the HNSW index from the heap. The target must use
the same index key and predicate as the source. A cast of the same vector key
to `halfvec` is supported. The deprecated `pg_cuvs_build_hnsw()` function creates
its target with the source key and predicate.

During a regular `REINDEX` or table maintenance that rebuilds an existing
index, pgvector builds the target from the current heap if the CAGRA source
has pending changes, belongs to a previous heap generation, or cannot supply
a matching file export. The stored `source` option is preserved. Explicit
source conversions still require a verified source. The deprecated function
holds a table lock that blocks writes until its transaction ends.

The heap build can take longer and uses `maintenance_work_mem`. Its graph
may differ from the source graph. It uses the stored `m` and raises
`ef_construction` to at least `2 * m` for the build if needed. When it raises
that value, the build runs serially and leaves the catalog options unchanged.

Source-based `CREATE INDEX CONCURRENTLY` and `REINDEX INDEX CONCURRENTLY`
require a source without pending changes. Rebuild that source before retrying,
or use an ordinary rebuild of the existing HNSW index. A heap tablespace move
changes the heap generation and requires a new source build for explicit
conversion.

The `hnswlib_file` mode opens files in the source index directory and
requires a build-time binding between the HNSW graph and the source CAGRA's
TID file. The binding records each file's device, inode, size and SHA-256.
Verification reads both files to compute their digests.
A failed CAGRA rebuild preserves the binding when it restores the same files
with unchanged contents. Earlier versions could import an older graph after the source
changed. A missing or invalid binding now causes conversion to fail with
SQLSTATE `55000`. Files written by earlier versions have no binding, so their
conversion fails until the files are regenerated. Choose `mode = 'nsw'` for a clean resident source, or set
`cuvs.cpu_hnsw_fallback = on` and run `REINDEX` on the source to regenerate
its files. If a failed `CREATE INDEX CONCURRENTLY` leaves its target in
`pg_index` with `indisvalid = false`, run `DROP INDEX` on that invalid
target before retrying.

The in-memory `hnswlib` mode exports the graph and TIDs together. Restart the
daemon after upgrading the extension so both support paired export. When an
export repeats a TID with the same vector, the target is built from the heap
using pgvector. An explicit conversion rejects repeated TIDs that carry
different vectors. Maintenance can rebuild the target from the heap in either
case.

For direct heap construction, `auto` uses tiny exact GPU construction below 16 rows and
otherwise the resident IVF-PQ route while its estimate fits the host and GPU budgets.
Over the budget, `auto` spills to ACE on disk by default and says so with a NOTICE
(`pg_cuvs: build_algo=auto exceeded the resident budget (need X, budget Y); building
with ACE on disk`, DETAIL with the disk requirement and directory) and, after the
build, the completion NOTICE `auto selected ACE-disk (<P> partitions)`; the receipt
records `requested_algo=auto`, `route=ace_disk`, `auto_overflow=true`. To refuse
instead (SQLSTATE 54000 with a `build_algo=ace` hint), start the daemon with
`--hnsw-auto-overflow error`. Below 256 rows an over-budget `auto` is refused either way.
ACE indexes can differ from resident IVF-PQ in recall and latency. Because the route
depends on the budgets at build time, a restore or `REINDEX` on a smaller host can
rebuild an `auto` index as ACE (or, with `error`, fail); set `build_algo` explicitly to
pin the route. Explicit `ivf_pq` and `ace` never read the flag.

When the table has statistics (`pg_class.reltuples` > 0, i.e. after `ANALYZE`), the
route and the disk precheck are decided from `reltuples` and the declared dimension
BEFORE the heap scan, so a refusal or the ACE NOTICE comes before any row is spooled;
the build re-decides from the spooled row count. Without statistics the decision comes
after the scan. The receipt field `route_decided` says which
(`before_scan_from_statistics`, `after_scan_no_statistics`, or
`after_scan_preflight_unavailable` when statistics existed but the pre-scan forecast could not be
obtained from the daemon). Stale statistics can
mislead the early decision; run `ANALYZE` first. The `cagra` AM likewise refuses a
certain AR0 (estimated final size over the per-GPU limit, unsharded) before its scan
when statistics exist.
Explicit `ace` consumes the matching patched SDK/planner and a private disk workdir.
`nn_descent` requests a resident NN-descent base graph and never falls back to another
builder. It rejects fewer than 16 rows, nonfinite inputs and inputs whose conservative
FP32 distance bound overflows. The build applies the SDK's intermediate width limit of
1024 and the row-count adjustment before the NN-descent capacity check. Its base dtype is FP32 when dim <= 16, any element exceeds half range, or the resident
IVF-PQ norm bound reaches 65504 (4 x max\|x\|^2 for L2, max\|x\|^2 for inner product and
cosine), because the SDK's FP16 path mixes FP32 norms with half dot products; otherwise FP16.
Because the SDK computes L2 as ||a||^2 + ||b||^2 - 2a.b in float32 on both paths, L2 builds
feed NN-descent a column-centered copy (mean accumulated in float64); since every mode of
the heap build builds the upper layers, the SDK patch 0005 does the same for each upper
level's point set (its own mean). L2 is
translation invariant; stored vectors and search distances use the original rows. The base
copy is counted in the host estimate (n x dim x 4, L2 only) and freed before serialization;
each upper-level copy is counted in that level's NN-descent host workspace check. IP and
cosine are not centered. Centering removes a common offset only: well-separated tight
clusters can still lose order to the float32 expansion (not detected; GPU validation pending).
Its admission estimate uses FP32 dataset storage even when the selected base dtype is FP16;
the estimate is not an allocator or RSS upper bound. GPU hierarchy requires the matching
resident-NN upper-precision SDK patch and receives the base dtype and budgets. Existing
receipts do not identify the upper precision contract, so configured upper dtype is null;
per-level observed dtype is logged, not stored.

An over-budget refusal (SQLSTATE 54000, naming the resident IVF-PQ or NN-descent
estimate) has the DETAIL `he=<MiB> hb=<MiB> ge=<MiB> gb=<MiB> rp=<MiB> rl=<MiB>`:
the host estimate and budget, the GPU estimate and budget, the RMM pool's used bytes
(0 without a pool) and the daemon's logical GPU use (`rl`; a reloaded index counts a
graph_degree-64 estimate, so it can differ from `rp`), each in MiB rounded down. The
exact bytes are on the daemon's `[hnsw_route]` line. Every route, resident ones
included, needs regular disk under the daemon's `--index-dir` for its private workdir,
about `4 × raw vector bytes + n × (graph_degree × 8 + 64)` bytes. Short of that, or on
tmpfs, the build fails before any GPU work with SQLSTATE XX000 and the message
`HNSW needs regular disk capacity: need=<bytes> avail=<bytes> fs=<tmpfs|regular>
route=<route>`; its HINT names the filesystem and adds that the PostgreSQL temporary
tablespace needs a further `n × dim × 4` bytes for the input spool. The `[hnsw_route]`
line then has `status=5` and still names the decided route (`route=none` only when no
route was decided, such as an over-budget refusal).

Direct `ef_construction` scales the planned intermediate width from 4 × `m` before the
row-count adjustment and the hnswlib serialization EF from 200 at the default 64.
The tiny route uses its own graph width. The
`hierarchy_ef_construction` receipt field reports the value read from that serialized
file. The GPU upper-graph builder takes its neighbors and budgets through the SDK.
Heap builds cap the intermediate width at the SDK limit of 1024. With the default `m = 32`,
a build that reaches that ceiling with a higher requested width issues a NOTICE.
Under `--hnsw-ace-build-quality auto`,
the ACE effort scales from 240 at the default 64 (20 NN-descent iterations per small partition with
cuVS 26.04). ACE indexes also record a partition-builder
readback computed from the pinned SDK policy (`ace_build_quality`, `ace_local_intermediate_degree` = 3 × (graph_degree / 2),
`ace_local_nn_descent_iterations`); the receipt's `intermediate_degree` is the resident width
and the ACE memory-plan input, not the width ACE partitions build and optimize with.
The readback reports a configured iteration limit. The SDK logs describe individual partitions.
`ace_partitions` is the effective partition count (`ace_partitions_origin` =
`same_SDK_stable_explicit_plan`) when pg_cuvs was built with `CUVS_SDK_SOURCE_INCLUDE`. Built
without it, an ACE build still runs on the patched SDK but records the requested count:
`ace_partitions_origin` is `requested_effective_not_guaranteed`, and
`ace_local_nn_descent_iterations` is `not_guaranteed` for two requested partitions from 1e6
rows; `CREATE INDEX` raises a NOTICE. Reusing a source CAGRA graph does not
rebuild it according to these options. See [HNSW build options](hnsw-build-options.md)
for limits, compatibility and estimate-vs-upper-bound caveats.

**Concurrent builds.** `CREATE INDEX CONCURRENTLY` and `REINDEX INDEX CONCURRENTLY` of a `pg_cuvs_hnsw` index built
from the heap (no `source`) write the index under the `RowExclusiveLock` PostgreSQL's concurrent build holds, so the
table's reads and writes go on while it runs. With `source = ...` the conversion still asks for `AccessExclusiveLock`
on the index it writes: during a concurrent build or reindex it waits for every running statement on the table and
for every open transaction that updated or deleted rows of it, and every query on the table that starts after it,
`SELECT` included, waits until the conversion has written the index. `REINDEX TABLE CONCURRENTLY` of a table with such
an index does the same. Build such an index without `CONCURRENTLY` at a quiet time, or build it from the heap.

---

## 5. SQL functions

| Function | Returns | Purpose |
|----------|---------|---------|
| `pg_cuvs_reset_circuit(index regclass)` | void | Close this backend's circuit breaker for the index. Breakers are per backend and per index: `cuvs.circuit_breaker_threshold` consecutive GPU errors open one. A breaker opened by a transient reason (admission AR1–AR5, post-admission OOM, ATT, or `OOM_FALLBACK` with no reason code) half-opens after `cuvs.circuit_breaker_transient_ttl_s`: the plan-time gate restores the GPU path and one GPU attempt decides. Success closes it, a transient failure re-opens it with a fresh TTL, and a permanent failure re-opens it as permanent. A breaker opened by a permanent reason (AR0, protocol mismatch, any other counted status) closes only through this function. When a scan executes against an open breaker (for example a cached plan), it raises SQLSTATE `53200` with a HINT to retry during an index build or run this function, then re-run the statement to replan |
| `pg_cuvs_circuit_status()` | SETOF (index_oid oid, state text, transient bool, opened_at timestamptz, half_open_count bigint, consecutive_errors int) | (0.9.0) This backend's breakers, one row per tracked index. No IPC. `state` = `closed` \| `open` \| `half_open`, evaluated with the current TTL. `transient` is the reason class of the last open. `opened_at` is NULL if the breaker never opened. `half_open_count` counts OPEN→HALF_OPEN transitions |
| `pg_cuvs_last_rejection()` | jsonb | This session's last GPU memory admission rejection (NULL if none); see [Admission rejections](#admission-rejections-and-recovery). Backend memory only, cleared at session end; callable by everyone |
| `pg_cuvs_unload(index regclass)` / `pg_cuvs_load(index regclass)` / `pg_cuvs_pin(index regclass)` / `pg_cuvs_unpin(index regclass)` | text | (0.9.0) Manual cache control; index owner or superuser. See § 8.4 |
| `pg_cuvs_unload(db_oid oid, index_oid oid)` / `pg_cuvs_unpin(db_oid oid, index_oid oid)` | text | (0.9.0) Superuser only; any database's index. See § 8.4 |
| `pg_cuvs_reclaim_set(name text, value text)` | text | (0.9.0) Superuser only; change a reclaim parameter at runtime. See § 8.2 |
| `pg_cuvs_index_status()` | SETOF record | (0.9.0) Per-index residency, protection, sidecar, pin and eviction history; rows filtered by privilege. See § 8.4 |
| `pg_cuvs_memory_status()` | SETOF record | (0.9.0) Superuser or `pg_monitor`; per-GPU accounting, reclaim state and eviction counters. See § 8.4 |
| `pg_cuvs_build_status()` | SETOF record | (0.9.0) In-flight CAGRA builds and their reservations; rows filtered by privilege. See § 8.4 |
| `pg_cuvs_hnsw_build_info(index regclass)` | jsonb | Read the stored construction route/settings and estimate-based admission. Version2 writer with historical version1 reader; absent legacy/source/empty receipts are not reconstructed from current GUCs. For ACE, `ace_partitions_origin` says whether `ace_partitions` is the effective count or only the requested one. Refuses an index that is not ready (`indisready` false: a concurrent build or reindex has not finished writing it, failed while writing it, or the index is being dropped concurrently; SQLSTATE 55000). A concurrent build that failed after it wrote the index leaves an index that is ready but not valid; that one is read. Requires SELECT on the source table; estimates are not memory upper-bound guarantees |
| `pg_cuvs_hnsw_layer_counts(index regclass)` | record (elements bigint, upper_elements bigint, m integer, entry_level integer, built_hierarchy integer, level1_unlinked bigint) | One `pg_cuvs_hnsw` index that is ready (`indisready`; an index a concurrent build or reindex has not finished writing, failed while writing, or that is being dropped concurrently is refused with SQLSTATE 55000): elements not marked deleted, how many are above level 0, the metapage `m` and entry level, the hierarchy flag its heap build recorded (NULL when none, e.g. a CAGRA-source build), and how many elements above level 0 have no level-1 link (0 when the upper layer is connected; a search descending through level 1 stops at such an element). Reads every page; requires SELECT on the table |
| `pg_cuvs_hnsw_layer_status()` | SETOF (index regclass, access_method name, current boolean, reason text) | Every `pg_cuvs_hnsw` index the caller can read that is ready (`indisready`; one a concurrent build or reindex is still writing is left out): whether it has upper layers, and why not. `mode='nsw'` before 0.9.3 wrote level 0 only, so rows inserted afterwards held the upper layers alone and searches could miss the built rows; such an index needs `REINDEX INDEX`. A heap build is judged from its recorded hierarchy flag; a CAGRA-source build from the share of elements above level 0 (about 1/m expected, under a quarter of that means none), and NULL below 16 × m elements. With two or more elements above level 0, any of them without a level-1 link makes an index not current, whatever built it (a single upper element, the entry point, has nobody to link to) |
| `pg_cuvs_build_hnsw(cagra regclass, mode text DEFAULT 'nsw')` | regclass | Build a pgvector HNSW from a CAGRA index without pgvector's CPU build. **Deprecated** in favor of `CREATE INDEX ... USING pg_cuvs_hnsw`; the older two-step `pg_cuvs_import_hnsw` form (empty `USING hnsw` target + import) is removed |
| `pg_cuvs_compact(index regclass)` | void | Remove tombstoned vectors via `cuvsCagraMerge`, rebuild `.cagra`/`.tids` at the index's `graph_degree` and `intermediate_graph_degree`, drop `.tombstone`. The rebuild uses fp32 IVF-PQ kNN distances under the `build_algo` fp16 rule. If the initial NN-descent or auto merge runs out of memory, retry once with explicit IVF-PQ under that same rule. A pinned IVF-PQ failure is not retried |
| `pg_cuvs_batch_search(rel regclass, queries vector[], k int)` | SETOF (query_idx int, ctid tid, distance real) | Batches Q queries in one GPU dispatch when eligible. Honors `search_mode` and `bf_precision` on the GPU path |
| `cuvs_filtered_knn(index regclass, query vector, filter_tids bigint[], k int)` | TABLE (ctid tid, distance float4) | Brute-force kNN over the TID set (`block<<16\|off`). NULL means unfiltered, and an empty array returns zero rows |
| `cuvs_filtered_knn(index regclass, query vector, filter_tids tid[], k int)` | TABLE (ctid tid, distance float4) | Type-safe overload that accepts `ctid` directly |
| `pg_cuvs_gc_orphans(do_delete bool DEFAULT false)` | SETOF (db_oid oid, index_oid oid, reason text, action text) | Reconcile `index_dir` vs catalog; dry-run by default (ADR-046) |
| `pg_cuvs_last_search_latency_us()` / `_n_results()` / `_k()` / `_index()` / `_metric()` | int / int / int / oid / text | Process-local stats for the most recent scan in this backend; NULL if none |
| `pg_cuvs_hw_profile()` | SETOF (gpu_name text, n_gpus int, total_vram_bytes bigint, link_bw_bytes_per_us float8, hbm_bw_bytes_per_us float8, gpu_bf_tput float8, ipc_rtt_us float8, measured_at_epoch bigint, probe_status int, source text, matches_running_daemon bool, cpu_dist_tput float8, gpu_cagra_lat_us float8) | The measured (or DEFAULT) hardware profile the daemon wrote at boot, consumed by the ADR-075 cost model. `source` = `measured` \| `default`; `probe_status` is a bitmask of which coefficients were measured; `matches_running_daemon` = false flags a stale profile (GPU swap / migration). Read-only, no CUDA/IPC |

> With experimental page-format CAGRA indexes, both functions return the
> `ctid` visible in the caller's snapshot. A `filter_tids` array matches those
> visible `ctid` values. The GPU path checks candidates in heap block order.
> The functions can use exact heap search, for example when the GPU shortlist
> contains fewer than `k` visible rows. Current standard builds supply no GPU
> brute-force dataset for page-format indexes, so `cuvs_filtered_knn` uses exact
> heap search on those indexes.
>
> In the `pg_cuvs_batch_search` visibility benchmark on an A100 80 GB PCIe,
> an FP32 brute-force test fixture held 100,000 vectors of 128 dimensions.
> For Q=32 and k=100, checking visibility by heap block raised p50 from
> 1.82 ms to 2.40 ms and from 1.74 ms to 2.36 ms in two repetitions. The
> prototype measured 2.02 ms to 2.69 ms under the same workload. These comparisons
> used a read-only workload with no HOT updates.
>
> With legacy CAGRA indexes (#243), both functions return the TID the index
> holds. For a row updated by HOT (an UPDATE that changes no indexed
> column and fits in the same heap page), that TID is the root of the HOT chain, not the
> current row version, so `JOIN ... ON t.ctid = b.ctid` finds no row: the row is silently
> missing and a query can return fewer than `k` rows. A `tid[]` or `bigint[]` filter built
> from current `ctid` values does not match the row either, and the next-ranked row in the
> filter takes its place. VACUUM, REINDEX and `pg_cuvs_compact` do not bring the row back;
> VACUUM FULL did in testing. An index scan (`ORDER BY embedding <-> q LIMIT k`, one per
> query, for example through `CROSS JOIN LATERAL` as in the README) follows HOT chains and is
> not affected. For `vector_l2_ops`, the `distance` these functions return is squared L2;
> `<->` returns L2.

### Internal / unsupported

The extension also defines fault-injection and test-harness functions (GPU VRAM pre-allocation, OOM
injection, and a non-persistent daemon budget override). They exist for the test suite only, are
**not a supported API**, and can OOM or crash the daemon — do not call them in production. The
supported way to cap VRAM is the daemon's `--max-vram-mb` flag.

### Admission rejections and recovery

When the daemon refuses GPU memory for a CAGRA, IVF-PQ or sharded build, or for
reloading an index on a query, the error names the reason and the numbers instead of
`BUILD failed (status 2)`. The SQLSTATE tells permanent from transient:

| Reason | SQLSTATE | Meaning | What to do |
|---|---|---|---|
| AR0 | 54000 | The build needs more than one build may ever get (pool quota, or the logical VRAM budget when the pool is disabled) | Use `pg_cuvs_hnsw` with `build_algo='auto'` or `'ace'`; lower `graph_degree`; operators review `--rmm-pool-max-percent`, `--max-vram-mb`, `--rmm-scratch-permille`. Retrying does not help |
| AR1, AR2 | 53200 | Another CREATE INDEX holds GPU memory | Retry after it finishes |
| PHYS | 53200 | Raw physical GPU free memory is below the 32 MiB backing floor, or the query failed; the build/compaction was deferred before cuVS | Retry after other GPU users release physical memory; inspect the NOTICE and `SHOW cuvs.rmm_pool_status` |
| AR3, AR4 | 53200 | Not enough even after evicting what may be evicted | Unload an idle index (HINT names one), then retry |
| AR8 | 53200 | A CAGRA build does not fit the GPU memory pool with headroom: `pending + pool_used + N + headroom` exceeds the pool quota (DETAIL `need … MiB + headroom … MiB > pool quota … MiB (pending … MiB, pool used … MiB)`) and no permitted eviction frees enough; nothing was reserved, and nothing was evicted unless the pool was still short after an eviction (`pool used` is then the value read after it) | Lower the index size or `graph_degree` (`ALTER INDEX … SET (graph_degree = …)` and `REINDEX`); an operator can raise `--rmm-pool-max-percent`. A retry helps when `pending` or the resident pool use goes down |
| OOM (after admission) | 53200 | The build ran out of GPU memory although admitted; nothing was evicted | Raise `--rmm-scratch-permille`, or retry when memory is free |
| reload (AR0–AR5 on a query) | 53200 | `GPU memory is unavailable to load index "<name>"` | As the HINT says; a query retried later may load |

AR5–AR7 and ATT are reserved for the eviction-scope, managed large_workspace and
dataset-attach checks. IVF-PQ admission rejections changed from XX000 to 54000/53200;
IVF-PQ failures after admission and non-admission failures stay XX000.

DETAIL lists `need`, `free`, the resident total and count, pinned amounts, and up to
three idle candidates, oldest first: `Idle candidates: public.idx_a 2048 MiB idle 60 min; (db 16385) oid 24576 512 MiB idle never`.
An index of the current database is named (`quote_qualified_identifier`); one of
another database appears only as `(db <oid>) oid <oid>`. Values at 99,999,999 MiB
print as `99999999+`. Shard rejections start with `shard <i>: `.

The HINT depends on what the caller may do: if the caller owns (or as superuser may
unload) a candidate, it gives `SELECT pg_cuvs_unload('<index>'); then retry CREATE INDEX.`
(superuser and another database: `SELECT pg_cuvs_unload(<db_oid>, <index_oid>); ...`),
otherwise `Ask a superuser to unload an idle index (see pg_cuvs_index_status()).`
With no candidates it says either that pinned indexes hold the space
(`pg_cuvs_unpin`) or that every resident index is in use. Unloading is not permanent:
the unloaded index reloads on its next query.

`pg_cuvs_last_rejection()` returns the same rejection for scripts:

```json
{"reason": "AR3", "sqlstate": "53200", "requester": "BUILD", "index_oid": 24601,
 "at": "2026-11-03 15:40:00+09", "shard": null,
 "fields": {"need_mib": 3049, "free_mib": 512, "resident_mib": 6000,
            "resident_count": 3, "evictable_mib": 0, "evictable_count": 0,
            "pinned_mib": 0, "pinned_count": 0, "reclaim_can_help": 0},
 "candidates": [{"db_oid": 16384, "index_oid": 24576, "mib": 2048, "idle_s": 3600}]}
```

`requester` is `BUILD`, `LOAD` or `SHARD`; `fields` holds only the values the daemon
reported for that reason (AR8: `need_mib` = N, `quota_mib`, `pending_mib`,
`headroom_mib`, `pool_used_mib`); `idle_s` is -1 for an index never searched. Daemon logs
carry one `[admit]` line per admission decision (ending with `pool_need`
(= `needed`), `headroom`, `pool_quota` and `pending`, the AR8 inputs, 0 when AR8 was not
evaluated; the AR8 `pool_used` is the line's existing `pool_used` field), `[release]` when a reservation is
returned and `[build_oom] ... action=fail_no_evict` for an out-of-memory build.
The eight-field `[build_oom]` record ends with `failed_bytes`: the final failing
pool-upstream growth request observed around the RMM limiter, which can differ
from the caller's logical suballocation and from `needed`. Unobserved sizes
(including synthetic OOM and failures outside this boundary) are `-1`, not zero.
A build that defers to reload because the GPU index registry is full now prints a
NOTICE instead of finishing silently.

---

## 6. Observability views

Four views back onto SRFs. All are **empty when the daemon is down** (except `pg_stat_gpu_fallback`,
which is backend-shmem sourced), so monitoring stays queryable. Counters reset on index
rebuild/reload or daemon restart.

### `pg_stat_gpu_search` — per-index search stats (daemon-sourced)

`database_oid, index_oid, index_name, dim, metric, n_vecs, vram_bytes, resident, search_count,
error_count, avg_latency_us, p50_latency_us, p95_latency_us, p99_latency_us, last_status, last_error,
last_search_at, requested_k, returned_k, stale, stale_since, delta_rows, delta_generation,
delta_vram_bytes, delta_merged_count, delta_search_mode, warmup_state, last_warmup_at,
warmup_duration_ms, download_count, cache_miss_count, gpu_device_id, shard_count, search_mode,
bf_batch_count, extend_count, compact_count, last_compact_at, prefilter_fallback_count,
cagra_batch_count`

The single most useful row for "is this index healthy": `resident`, `search_mode` (did it stay on
GPU?), `error_count`, p50/p95/p99 latency, `stale`, `delta_rows`.

`prefilter_fallback_count` (ADR-083, #133) counts searches where the 3O CAGRA prefilter
(`cagra_prefilter`) returned a materially short fill — a signal the traversal collapsed on an
anti-correlated filter — and were retried on the GPU exact BF prefilter (`gpu_bf_prefilter`,
mode 3) within the same query. This is a different code path from the D-wedge Custom Scan hook
above — its cost scales with corpus size (N), not with `1/selectivity` like D-wedge. Watch
`prefilter_fallback_count` against `search_count`: a nonzero, growing value means an index is
quietly paying `gpu_bf_prefilter` latency on some filter shapes even though
`cuvs.filter_auto_threshold` routes it to 3O.

`cagra_batch_count` (#160) counts coalesced CAGRA micro-batch dispatches — the cagra twin of
`bf_batch_count`. It stays 0 unless `cuvs.cagra_batch_wait_us > 0`, and is the only SQL-visible
confirmation that the window is actually routing single-query cagra scans through the daemon's
batch worker. One dispatch can serve many concurrent requests, so `search_count /
cagra_batch_count` is the average coalesced batch width. Coalesced requests run the multi-CTA
batch kernel rather than the single-query kernel; measured recall between the two differs by
around 0.003 in either direction depending on the graph (#144), the same order as a one-step
`ef_search` change or graph-build nondeterminism.

### `pg_stat_gpu_cache` — per-GPU VRAM cache counters (daemon-sourced)

`gpu_device_id, hits, misses, evictions, reloads, persist_failures, resident_count, vram_used_mb,
vram_budget_mb, bf_vram_mb, bf_precision`

Watch `vram_used_mb` vs `vram_budget_mb` for headroom, and `evictions`/`reloads` for thrashing.
**`persist_failures > 0` is serious** — eviction could not serialize an index to disk.

### `pg_stat_gpu_fallback` — per-index CPU fallback (backend-shmem sourced)

`index_oid, fallback_count, last_reason, last_fallback_at`

The only place plan-time CPU routing is visible. `last_reason` ∈ `disabled / circuit_breaker /
stale / delete_drift / daemon_down / no_artifact / delta_unusable / tombstone_unusable`. Watch the
trend against `pg_stat_gpu_search.search_count` to catch queries silently dropping to CPU. Counts
are a relative pressure signal (the cost hook can run more than once per query), not exact query
counts.

### `pg_stat_gpu_shards` — per-shard placement (daemon-sourced, sharded indexes only)

`database_oid, index_oid, index_name, shard_id, gpu_device_id, n_vecs, tid_offset, vram_used_mb,
search_count, error_count, resident, last_status`

One row per shard; empty for unsharded indexes.

---

## 7. On-disk artifacts

The sidecar file suffixes (`.cagra`, `.tids`, `.vectors`, `.delta`, `.tombstone`, `.stale`,
`.shards`, `.sNNN.cagra`, `.relfilenode`, `.hnsw`) and their formats/lifecycle are documented in
[ARCHITECTURE.md §5](../ARCHITECTURE.md#5-index-lifecycle-and-on-disk-artifacts). The `.hnsw` file is an optional hnswlib conversion/fallback artifact; `pg_cuvs_hnsw` stores its searchable pgvector pages in the PostgreSQL index relation.

---

## 8. GPU memory operations

In this chapter, *sidecar* means only the `.delta` and `.tombstone` files of an index
(not `.cagra`, `.tids` or `.vectors`).

### 8.1 Model: a shared cache and work memory

Resident indexes are a shared cache (like `shared_buffers`); build scratch is work
memory (like `maintenance_work_mem`). The files in `index_dir` are the source of truth: an index
that leaves the GPU is reloaded by its next query.

**When an index is evicted.** Only these paths unload a resident index:

| Path | Who may be evicted | Log `reason` |
|------|-------------------|--------------|
| A query reloads a non-resident index, or `pg_cuvs_load` / `pg_cuvs_pin` | Cold (eligible) indexes, oldest first. If they are not enough, protected indexes too (soft eviction, `--load-soft-evict on`); never pinned or in-use ones. Also applies when another build's reservation is what makes the reload short | `reload`, `soft_evict` |
| Background reclaim (§ 8.3) | Cold indexes only, oldest first | `reclaim` |
| `pg_cuvs_unload` | The named index (refused if pinned or in use) | `manual_unload` |
| Registry slot cap (`--max-indexes`) for a reload | Same rule as the reload row | `slot` |
| `REINDEX` of a resident index when only its old copy makes room | That index's own previous copy (§ 8.5) | `admit_build` (`self_replace=1`) |
| A build with `--build-evict-scope idle:<min>` or `lru` | Cold indexes idle that long, or any unpinned idle-or-not index | `admit_build` |

**When nothing is evicted.** A build (CREATE INDEX, REINDEX, sharded or IVF-PQ build)
with the default `--build-evict-scope none`, including a build that hits an
out-of-memory error after admission; daemon startup (indexes that do not fit stay
not resident); snapshot warmup never evicts protected indexes; pinned indexes and
indexes in use are never evicted by any path. Background reclaim does not unload a
CAGRA index that has a delta or tombstone file and received INSERTs since it was
last saved (`file_current = false`): saving it would invalidate those files (see
"Queries got slower"). The insert-durability track handles this (in design); no
release is promised here.

**Cold, protected, eligible.** An index is *eligible* when it is not pinned, not in
use, and `now - last_use >= protect_s`, where
`protect_s = --evict-min-idle-s + min(hits, --evict-hit-cap) x --evict-hit-bonus-s`
(defaults 600 s, 20, 30 s: 600-1,200 s). `last_use` is the last search; a freshly
built or loaded index that has never been searched has no `last_use` and is
eligible. Victims are taken in ascending `max(last_use, loaded_at)` order, so a new
index is not evicted ahead of one that has been cold for longer. Only searches
update `last_use`; INSERT (EXTEND) and compaction do not.

### 8.2 Knobs

Daemon flags are read at startup. The *change path* column separates accounting
knobs (restart) from reclaim parameters (changed at runtime with
`pg_cuvs_reclaim_set(name, value)`, superuser only; the new value applies from the
next reclaim tick). SIGHUP re-reading of these values is **not implemented**:
`pg_cuvs_reclaim_set` is the only runtime path, and a restart returns to the flags.

| Flag | Values | Default | Change path | Effect |
|------|--------|---------|-------------|--------|
| `--build-evict-scope` | `none` / `idle:<minutes>` / `lru` | `none` | restart | Which residents a build may evict (`idle:0` = eligible ones) |
| `--evict-min-idle-s` | 0-86400 | `600` | `pg_cuvs_reclaim_set` | Minimum protection after the last search |
| `--evict-hit-bonus-s` | 0-3600 | `30` | `pg_cuvs_reclaim_set` | Extra protection per search since load |
| `--evict-hit-cap` | 0-1000 | `20` | `pg_cuvs_reclaim_set` | Searches counted for the bonus |
| `--reclaim-mode` | `off` / `usage` / `headroom` | `headroom` | `pg_cuvs_reclaim_set` | Background reclaim mode (§ 8.3) |
| `--reclaim-free-bytes` | bytes, 0 = automatic | `0` | `pg_cuvs_reclaim_set` | Fixed free target F (still capped and floored) |
| `--reclaim-high-pct` | 1-100 (100 = usage lines off) | `85` | `pg_cuvs_reclaim_set` | Usage line: reclaim starts above this share of `budget_eff` |
| `--reclaim-low-pct` | 1-high | `75` | `pg_cuvs_reclaim_set` | Usage line: reclaim stops at this share |
| `--reclaim-tick-s` | 1-3600 | `10` | `pg_cuvs_reclaim_set` | Reclaim thread period (also the usage stats file period) |
| `--reclaim-free-max-pct` | 1-100 | `50` | restart | F cap, share of `budget_eff` |
| `--reclaim-free-window-min` | 1-1440 | `30` | restart | Rejected builds older than this stop driving F |
| `--reclaim-floor-pct` | 0-100 | `10` | restart | F reclaim never takes residency below this share of `budget_eff` |
| `--pin-max-pct` | 0-100 | `50` | restart | Pinned bytes per GPU, share of `budget_eff` |
| `--load-soft-evict` | `on` / `off` | `on` | restart | `off`: a reload that cold indexes cannot cover is refused instead of evicting protected ones |
| `--reindex-self-replace` | `on` / `off` | `on` | restart | `off`: REINDEX never unloads the old copy (§ 8.5) |
| `--usage-stats-file` | `on` / `off` | `on` | restart | `off`: no usage stats file; startup loads in OID order |
| `--rmm-peak-meter` | `on` / `off` | `on` | restart | Lock-free RMM pool peak (`peak_bytes` in memory status and in `SHOW cuvs.rmm_pool_status` devices, with `solo` = at most one build in flight). `off` (diagnostics) stops updating the peak; live bytes are still counted |

`budget_eff` is `min(--max-vram-mb budget, RMM pool quota)` per GPU. Out-of-range,
empty, negative or contradictory values (`--reclaim-low-pct` above
`--reclaim-high-pct`) refuse startup; at runtime they are refused and the old value
stays. `pg_cuvs_reclaim_set` accepts only the seven runtime names above plus
`reclaim-mode`; accounting knobs are refused. The startup log prints every effective
value on one `cache policy` line, and `SHOW cuvs.rmm_pool_status` shows them live.

Configuration layers:

| Layer | Examples | Takes effect |
|-------|----------|--------------|
| Daemon flag | `--max-vram-mb`, `--reclaim-free-max-pct`, `--build-evict-scope` | Daemon restart (reloads indexes, recreates the pool, fails in-flight requests) |
| Daemon runtime parameter | `--reclaim-mode`, `--evict-min-idle-s`, ... | `pg_cuvs_reclaim_set`, next reclaim tick |
| `PGC_USERSET` GUC | `cuvs.circuit_breaker_transient_ttl_s`, `cuvs.k` | `SET` in the session |
| `PGC_SUSET` / `PGC_SIGHUP` / `PGC_POSTMASTER` GUC | `cuvs.socket_path`, `cuvs.index_dir` | Superuser `SET`, reload, or server restart respectively |
| `PGC_INTERNAL` status | `cuvs.rmm_pool_status` | Read-only; each `SHOW` asks the daemon, so another session's change is visible at once |

### 8.3 Background reclaim

A daemon thread runs every `--reclaim-tick-s` per GPU:

- **`off`**: no reclaim (the usage stats file is still written).
- **`usage`**: when resident bytes exceed `--reclaim-high-pct` of `budget_eff`,
  unload eligible indexes oldest first until at or below `--reclaim-low-pct`.
- **`headroom`** (default): the usage lines, plus a free target **F** for the next
  build. When `budget_eff - resident < F`, unload eligible indexes oldest first
  until `budget_eff - resident >= F`. F is the largest size requested by a build
  that was rejected for room in the last `--reclaim-free-window-min` minutes on that
  GPU, capped at `--reclaim-free-max-pct` of `budget_eff`. Rejections that no
  amount of free VRAM can fix (larger than `budget_eff`, or the host RAM guard) do
  not count. A rejected build drops out of F once the same index is admitted, when
  the window passes, or when the index is dropped. F reclaim never takes residency
  below `--reclaim-floor-pct` of `budget_eff` (the usage lines have no floor). A
  `CREATE INDEX` retried after a rejection gets a new OID, so the old rejection
  keeps F at that level until the window passes (F is the maximum, not a sum).

Reclaim makes room for the *next* attempt: it cannot prevent the first rejection,
and it frees nothing when no index is eligible. Resident bytes exclude build
reservations, so an in-flight build never triggers reclaim by itself.

### 8.4 Manual control and observability

| Function | Who | Effect |
|----------|-----|--------|
| `pg_cuvs_unload(index regclass)` | index owner or superuser | Unload now. Refused if pinned (`pinned: run pg_cuvs_unpin first`, SQLSTATE 55006) or in use. Returns `unloaded`, `not resident`, or `unloaded; warning: delta or tombstone file present, GPU path switches to CPU replan until REINDEX` |
| `pg_cuvs_load(index regclass)` | index owner or superuser | Load now under the reload rules (`loaded`, `already resident`) |
| `pg_cuvs_pin(index regclass)` / `pg_cuvs_unpin(index regclass)` | index owner or superuser | Pin (loads first if needed) / unpin. A pin protects from every eviction path, lasts until the daemon restarts (it is recorded but not restored), survives `REINDEX` of the same index, and is not inherited by `REINDEX CONCURRENTLY` (new OID). Pinned bytes per GPU are capped by `--pin-max-pct` |
| `pg_cuvs_unload(db_oid oid, index_oid oid)` / `pg_cuvs_unpin(db_oid oid, index_oid oid)` | superuser | Same for an index of another database (it may need unpin first) |
| `pg_cuvs_reclaim_set(name text, value text)` | superuser | Change a reclaim parameter at runtime (§ 8.2) |
| `pg_cuvs_index_status()` | everyone (rows filtered) | One row per resident, cold (snapshot warmup) or not-resident index: `index_name` (NULL for another database), `state`, `kind`, `gpu`, `vram_bytes`, `last_use`, `loaded_at`, `hits`, `pinned`, `in_use`, `protect_until`, `eligible`, `reclaim_exempt` (sidecar kind: `none`/`delta`/`tombstone`/`both`, read from the daemon's `index_dir`; an index with its own `index_dir` reloption shows `none`), `file_current`, `extend_count`, `warmup_state`, `evict_count`, `last_evict_at`, `last_evict_reason`, `last_build_self_replace`, `last_build_unloaded`. Non-privileged callers see only indexes of the current database that they own; superusers and `pg_monitor` members see the whole daemon |
| `pg_cuvs_memory_status()` | superuser or `pg_monitor` | Per GPU: `budget_eff`, `resident_bytes`, `pending_bytes`, `pinned_bytes`, counts, `reclaim_mode`, `reclaim_f_bytes` and `reclaim_f_src` (`fixed`, `build:<db>:<idx>` or `none`), `last_reclaim_at`, `last_reclaim_trigger`, `reclaim_runs`, `reclaim_skip_sidecar`, `persist_failures`, evictions by reason and in total, `drops`, `history_dropped`, `peak_bytes` (pool peak) |

| `pg_cuvs_build_status()` | everyone (rows filtered) | One row per in-flight CAGRA build that runs outside the daemon lock: `index_name`, `gpu`, `db_oid`, `index_oid`, `algo`, `path` (`single` or `multi`), `started_at` (wall clock when the build was admitted and reserved), `n`, `dim`, `final` (resident footprint), `scratch` (`reserved_n - final`), `reserved_n` (the N held in `pending_bytes` until the build ends), `policy` (`--cagra-build-overflow` passed to this build), `lw_ub` (the large_workspace upper bound this build added to `lw_pending`), `lw_ub_src` (`admission`: the value of this build's admission check; `none`: no admission check ran, `lw_ub` is NULL), `phase` (`building`). Same row filter as `pg_cuvs_index_status()`. A `CREATE INDEX` build's index is not visible to other sessions until commit, so only superusers and `pg_monitor` members see it; a `REINDEX` build is visible to the owner. Sharded CAGRA builds and IVF-PQ builds hold the daemon lock for the whole build and never appear; a status call made during one waits until it ends |

`pg_cuvs_memory_status()` also has these columns per GPU: `budget_bytes`
(`--max-vram-mb`, 0 = unlimited), `quota_bytes` (the RMM pool quota, 0 = none),
`pool_used` and `pool_reserved` (RMM pool bytes; NULL when the GPU has no quota or
the pool cannot be read), `pending_rmm` (build reservations counted against the
quota), `lw_pending` (sum of `lw_ub` of admitted builds, § 3 managed
large_workspace), `builds_active` (builds holding a reservation), `rejects_ar0` …
`rejects_ar8` (admission rejections by reason, one per `[admit] verdict=reject`
line), `last_reject_reason` and `last_reject_at` (NULL before the first rejection),
`managed_spill_builds` and `managed_spill_bytes_max`. A rejection that found no GPU
(AR0 against every budget) has no GPU row; it is counted in
`rejects_no_gpu` of `SHOW cuvs.rmm_pool_status`. `pg_cuvs_index_status()` also has
`dataset_attached`, NULL in this release (no wrapper call reports it for a loaded
index yet; the `[build_result]` line carries it per build). While a build is in
flight, `builds_active` equals its row count in `pg_cuvs_build_status()`,
`pending_bytes` the sum of `reserved_n` and `lw_pending` the sum of `lw_ub` on
that GPU.

`SHOW cuvs.rmm_pool_status` appends these keys to its JSON: `build_evict_scope`,
`pin_max_pct`, `reclaim_free_max_pct`, `reclaim_free_window_min`,
`reclaim_floor_pct`, `load_soft_evict`, `reindex_self_replace`, `usage_stats_file`,
and `reclaim_params`, an object with `value`, `source` (`default`, `flag` or
`function`) and `changed_at` (epoch seconds or null) for each runtime parameter.
It then appends `builds_unlisted` (builds not listed because the table of
32 in-flight builds was full), `rejects_no_gpu` (`ar0` … `ar8`) and `builds`: for superusers and `pg_monitor`
members, every `pg_cuvs_build_status()` row (`started_at_us` in epoch
microseconds); for other roles, null.

Counters are cumulative and are not reset when read; only a daemon restart resets
them. Eviction history (`evict_count`, `last_evict_*`) outlives the resident entry,
is removed by `DROP INDEX`, and keeps at most 65,536 indexes (oldest dropped first,
counted in `history_dropped`). `DROP INDEX` cleanup counts as `drops`, not as an
eviction. The daemon hears of a drop of a `cagra`, `ivfpq` or `flat` index when the
dropping transaction commits (`DROP INDEX CONCURRENTLY`: at its first internal commit);
a drop inside a transaction that is prepared with `PREPARE TRANSACTION` is not passed
on, even after `COMMIT PREPARED`: its daemon entry and files stay. Every eviction writes one `[evict]` line (`op gpu db idx reason
requester_idx idle_s protect_s hits vram_bytes saved_ms mono_us self_replace`;
`idle_s = -1` for a never-searched index) and every reclaim pass that acts writes
one `[reclaim]` line (`op gpu tick resident_before resident_after hi_bytes lo_bytes
evicted skip_sidecar trigger f_bytes f_src mono_us`).

### 8.5 Procedures

**A build is rejected for lack of room.** Builds no longer evict resident indexes.
Check `pg_cuvs_index_status()` for cold indexes (`eligible`) and unload one, or wait:
in `headroom` mode the reclaim thread unloads cold indexes for a build of that size
within the next tick, if cold indexes exist. A superuser sees every database's
indexes and can use `pg_cuvs_unload(db_oid, index_oid)` (after
`pg_cuvs_unpin(db_oid, index_oid)` if pinned). Other users can unload only their own
indexes; ask the daemon administrator when the idle candidates belong to someone
else. The usage lines alone do not resolve a rejected build: they run only above
`--reclaim-high-pct`.

**Bulk creation or restore (pg_restore, REINDEX DATABASE).** Each build needs room
without evicting, so a series of builds can be rejected once enough indexes are
resident. Before the restore, unload cold indexes, or restart the daemon with
`--build-evict-scope idle:0` (new, never-searched indexes are eligible) or `lru`
and restore the default afterwards.

**Large index creation or REINDEX while serving.** REINDEX may unload the existing
copy of that index for a while, and queries on it may be slower meanwhile; to avoid
this, pin it with `pg_cuvs_pin` or keep enough free room for both copies. A pinned
index's REINDEX is refused when both copies do not fit. When the old copy is
unloaded, `pg_cuvs_index_status()` shows `last_build_self_replace = true` and
`last_build_unloaded`, and the daemon log has a `[self_replace]` line with the
NOTICE text (`previous resident copy of <index> unloaded from GPU to make room for
the rebuild`); in the 0.9.0 release the REINDEX session also receives that text as
a NOTICE. If the build then fails, the next query reloads the old copy. `REINDEX CONCURRENTLY` builds a new
OID beside the serving index: do not unload the old index for it; unload another
cold index instead.

Build and explicit load replies carry up to three NOTICE/DETAIL pairs in a
1024-byte reply field. Names of evicted indexes are resolved in the requester's
database; entries of another database retain their database and index OIDs.
When the field or pair limit is exceeded, `(+N more)` points to the full messages
in the daemon journal. The admission-error field remains 512 bytes. Co-deploy
the extension and daemon built from the same reply header.

**Queries got slower.** Causes: the index was evicted and is reloaded (see
`last_evict_reason`); an open circuit breaker (`pg_cuvs_circuit_status()`); or a
delta/tombstone invalidation. The last one: if the index has a delta or tombstone
file (`pg_cuvs_index_status().reclaim_exempt` is not `none`) and received INSERTs
since it was last loaded (in that database, `SELECT extend_count FROM
pg_cuvs_gpu_search_stats() WHERE index_oid = ...` is above 0; this function shows
only the current database and at most 64 rows), then saving it rewrites `.tids` and
its GPU path switches to CPU replan until REINDEX. That save happens when another
index needs the room, when a build makes room (non-default build scope), on manual
`pg_cuvs_unload`, and when the daemon stops. A query already running on the GPU plan
first fails with SQLSTATE 55000 (`delta sidecar unusable mid-scan`); re-running it
plans the CPU path. Compaction is separate: compacting a resident index that has a
tombstone file rewrites `.tids` even without INSERTs and invalidates its delta file;
a tombstone-only invalidation can clear at the next VACUUM compaction. Background
reclaim never saves such an index. When choosing what to unload, prefer indexes with
`reclaim_exempt = none`.

**After a daemon restart.** Startup never evicts. It loads `.cagra`, then
`.shards`, then `.vectors` indexes; within each pass the hottest indexes of the
previous run come first (last use, then searches), read from the usage stats file
`index_dir/.pg_cuvs_usage_stats` that the reclaim thread rewrites atomically every
tick (whatever the reclaim mode). Without the file, or if it is unreadable or
corrupt, the order is `(database OID, index OID)`; the startup log names the reason.
Indexes that do not fit stay not resident until their first query. Pins are not
restored.

### STALE response and retry contract (#202 interim fix)

A STALE daemon response raises SQLSTATE `55000`
(`object_not_in_prerequisite_state`) in CAGRA, flat, batch search,
and filtered index searches. IVF-PQ raises the same error from the backend's
pre-search marker check (`ivfpq_gettuple`); its daemon handler currently has no
stale gate. The error identifies the index and recommends
`REINDEX INDEX`. A genuine empty filter result remains an empty result.

Marking an index stale immediately invalidates cached plans for its parent
table, including when the writing transaction later rolls back. When marker IPC
succeeds, the marker also remains after rollback and new plans can choose CPU.
If marker IPC fails, notification still occurs but the marker may be absent;
the socket-file gate determines the next plan. If the socket file is absent,
new plans choose CPU. If a crash leaves the socket file behind, a new plan can
choose GPU and fail with UNAVAILABLE. Invalidation does not replace an executor
already running:
such a scan can fail with `55000`. Roll back the failed transaction (or recover
to an application-managed savepoint), then retry with a fresh plan. There is no
automatic replay of the SQL statement. REINDEX normally restores GPU eligibility.
For IVF-PQ, ordinary REINDEX can leave an old tombstone sidecar that still blocks
the reader; #202 observed recovery with REINDEX CONCURRENTLY. Check the resulting
plan and sidecar state rather than assuming ordinary REINDEX restored GPU use.

This is an interim correction. Marker IPC remains best-effort; if the daemon is
down when the marker is sent, its absence can still cause missing rows after
restart. Durable stale publication, the shared insertion/deletion cap, and insertion durability
recovery are separate unfinished work. Do not treat this change as fixing those
paths.

### Build connectivity snapshot (#203)

The check is opt-in. `cuvs.hnsw_connectivity_check` (bool, default `off`,
USERSET) is read when an index build imports its graph; set it with
`SET cuvs.hnsw_connectivity_check = on` before `CREATE INDEX` or `REINDEX`.
With it off the build allocates no scratch, maps no graph file, traverses
nothing and emits no NOTICE, and the build snapshot records
`{"status":"disabled","scope":"build_snapshot","setting":"cuvs.hnsw_connectivity_check"}`.
With it on, new `pg_cuvs_hnsw` imports record a CPU structural check in
`pg_cuvs_hnsw_build_info(index)->'connectivity'`, and a NOTICE reports the
observation. `pg_cuvs_hnsw_connectivity_status(index)` exposes the same object
in both cases (it reads `pg_cuvs_hnsw_build_info`, so it refuses an index that is not ready with SQLSTATE 55000). `status` is `measured`, `unavailable` (host memory/mapping/check
failure), `disabled` (the build ran with the setting off) or `not_recorded`
(historical metadata, built before this snapshot existed). An unavailable check
does not reject a build or change search policy. Anonymous workspace is bounded
by `maintenance_work_mem`; larger checks, or a failed heap allocation,
automatically use preallocated, unlinked file-backed scratch. `spilled` records
that path.

Why it is off by default: the check only reads the finished graph and changes
neither edges, seeds, search results nor search policy, but it costs time at
scale. Measured on one A100-SXM4-80GB host (PostgreSQL 16.15, `build_algo='ivf_pq'`, resident route): on 999,000 rows x 768 dimensions the check took 2.26-2.31 s (graph file 3.34 GB, page cache warm), about +2 s on a 25.6 s `CREATE INDEX` (mean of five builds with the check off; mean of four builds with it on: 27.6 s); on 100,000 rows it took 0.13-0.16 s. The explicit `build_algo='nn_descent'` route follows the same setting (999,000 rows: check 2.40 s, 34.4 s off, 36.6 s on). The time grows with the graph file size; a graph file larger than the page cache has not been measured.
The file uses `DataDir/base/pgsql_tmp` (the default PostgreSQL temp directory);
custom `temp_tablespaces` routing is not implemented for this diagnostic.
Because this mapping is not charged to PostgreSQL VFD temp-file accounting, a
finite `temp_file_limit` disables the file-backed path (the observation becomes
unavailable if heap scratch cannot be used); an explicit operator limit is
never bypassed. File-backed allocation does not imply measured physical disk I/O.
Failure to reserve or map scratch records unavailable, not an ANN build error.
Routine PostgreSQL interrupts (including memory-context logging requests) are
serviced without abandoning the observation. Actual cancellation, statement
timeout and backend termination retain PostgreSQL's normal error semantics;
the adapters release their heap/file scratch and input mappings on error.
File reservation temporarily blocks backend signals and retries `EINTR`, as
PostgreSQL does for shared-memory allocation. This prevents repeated SIGUSR1
from starving `posix_fallocate`; pending requests are serviced after reservation,
so cancellation can wait for that operating-system operation to finish.

`unreachable` follows same-level directed edges and downward transitions from
the actual entrypoint. It is structural possible-path reachability, not the
path a particular query's greedy descent will choose. `unreachable_l0` uses
only level zero. `wcc_l0` and `wcc_all` count **weak** components, not SCCs.
One WCC does not imply every node is reachable. Zero unreachable nodes does
not guarantee finite-ef recall. No repair, seed change, or error policy is
selected by this diagnostic.

The object has `scope='build_snapshot'`: INSERT, DELETE, VACUUM and entrypoint
updates do not refresh it. Rebuild to obtain a new observation; do not use it
as a live graph-integrity status. New receipts live outside the metapage WAL
free-space hole; pgvector's prefix and existing policy/ACE receipt bytes stay
unchanged. Existing binaries can search these pages without interpreting this
snapshot. The status convenience function was added in 0.9.1: `sql/pg_cuvs--0.9.1.sql` and every
later install script create it, `sql/pg_cuvs--0.9.0--0.9.1.sql` adds it on
`ALTER EXTENSION pg_cuvs UPDATE` from 0.9.0 (and from 0.8.2 through 0.9.0);
the two 0.9.0 scripts do not contain it. A database whose extension is already
at 0.9.1 from before this function existed can continue using the existing
`pg_cuvs_hnsw_build_info` JSON surface; registering the alias can be done by the
extension owner with `scripts/install-connectivity-status.sql` after loading the
new binary. The script adds extension membership and is idempotent;
release-version selection is separate.

### Explicit exact scans for pg_cuvs_hnsw (#203)

**Status: current; CPU PostgreSQL 16 with pgvector 0.8.7 only.** `cuvs.hnsw_exact_scan` defaults to `off`. Setting it to `on` makes ordered
`pg_cuvs_hnsw` scans use exact SQL distances over MVCC-visible heap rows; the
approximate graph is not read.

```sql
SET cuvs.hnsw_exact_scan = on;
SELECT * FROM items ORDER BY embedding <-> $1 LIMIT 10;
SET cuvs.hnsw_exact_scan = off;
```

With the setting on, a scan runs exactly or stops with an error; it never falls back to
approximate results. Changing the setting invalidates all saved plans and cached
expressions in the current backend, so prepared statements are replanned.

**Row-level security, security barriers, index expressions and partial indexes.** For
these shapes PostgreSQL plans its own filtering and sorting path (Seq Scan + Sort) using
the original quals, so security quals run before any distance function. Row-level security
on a partition or inheritance child counts: when the statement names a table with such
children the whole statement is planned without `pg_cuvs_hnsw` paths. A table with
row-level security that only appears while planning (inside an inlined SQL function, a
view, or a partitioned parent read through a function) loses its `pg_cuvs_hnsw` paths
where the planner can see it. After planning, the final plan is checked once with the same
three facts the scan uses (row-level security or a security barrier on the statement,
row-level security on the scan's table, an index expression or predicate). If the plan still
holds a `pg_cuvs_hnsw` scan that could not run exactly, for example an unprotected table
that shares a statement with such a function, the statement is planned a second time
without any `pg_cuvs_hnsw` path and runs as Seq Scan + Sort. Planning side effects, such as
the `NOTICE` of an `IMMUTABLE` function evaluated on constants, then happen twice in that
case only.

**Where an unsupported shape stops** (all with SQLSTATE `0A000`, feature not supported):

| When | Shape | Message |
|---|---|---|
| Planning | a `pg_cuvs_hnsw` path that could not run exactly survived the second planning (another planner hook added it) | `an exact query retained an unsupported HNSW index path` |
| Scan (re)start | the setting was turned on, or row-level security was enabled, after the statement was planned, and this scan is protected (security barrier, row-level security) | `cuvs.hnsw_exact_scan is on, but this scan cannot run exactly`, with the reason in `DETAIL`; run the statement again |
| Reading further | more rows were fetched than one pass keeps, and a STABLE ordering function returned different values in between (for example after `SET LOCAL` between two `FETCH`es) | `the ordering function returned different results during one exact scan` |
| Scan start (default setting) | a plan kept an expression or partial index that the exact module serves on a statement with row-level security or a security barrier | `pg_cuvs: index "..." cannot answer this statement`, with the cause in `DETAIL`; run the statement again or set `cuvs.hnsw_exact_scan = on` |
| First fetch | a direct caller of the exact scan on a table with row-level security enabled (the scan of an index with a descriptor is not a direct caller: it ranks a plain-column index on such a table, see below) | `RLS exact scans require the PostgreSQL executor path` |
| First fetch | the scan is not a heap MVCC scan with one ordering operator, or the ordering function is not a strict scalar `float8` | `HNSW exact routing requires ...` |

Turning the setting off while a statement runs returns to the delegate access method at
the next rescan.

**Rows the graph never holds.** Exact mode returns rows whose vector is NULL and, for
cosine, zero vectors, last, in the same position as `ORDER BY ... <=> ...` over a Seq Scan
(NULL distance last, NaN after numbers). An ordinary index scan omits them, so the row set
can differ.

**Cost.** Exact mode reads the whole heap even with a small `LIMIT`, evaluates one distance
per visible row, and keeps the best candidates that fit in `work_mem` (16 bytes each, 262,144 at the default 4MB). A `LIMIT` within
that needs no temporary file. A caller that reads more makes a second pass over the heap,
which goes through PostgreSQL temporary files (`temp_file_limit` applies) and reads the heap
and evaluates every distance a second time. On 100,000 x 96 at `work_mem = 64kB` (one pass
keeps 4,096 answers), `LIMIT 4000` took 14 to 16 ms and `LIMIT 5000` took 50 to 84 ms, about
what Seq Scan + top-N Sort took for `LIMIT 5000` (47 to 79 ms). On one CPU container, `LIMIT 10`, warm, 100,000 x 96 took
11 ms against 11 ms for Seq Scan + top-N Sort, and 1,000,000 x 96 took 284 ms against
143 ms; the graph scan took about 2 ms at 100,000. It uses the current role's function
privileges; evaluating an index expression or partial predicate can require EXECUTE even
when an index path did not call that function. Standard vector distance operators return
strict scalar `float8`.

**pgvector coupling.** `pg_cuvs_hnsw` wraps pgvector's scan callbacks and relies on its
`amrescan` copying the ordering operators into the scan descriptor and on its not using
`scan->opaque` or `xs_orderbyvals` outside its own state. This was checked against
pgvector 0.8.7 on PostgreSQL 16 only. If a pgvector version does not copy the ordering
operators, the exact scan stops with the first-fetch error above instead of returning
rows.

CPU PostgreSQL 16 regressions cover this explicit mode (`make test-hnsw-exact-pg`). The automatic
recovery described in the next section is a separate mechanism that works without this setting.

### Cover guarantee for pg_cuvs_hnsw (#203, extension 0.9.3)

**Status: current for CPU PostgreSQL 16 with pgvector 0.8.7 (regressions `test/unit/test_cover_*_pg.py`, see
"Not verified" below). This does not close #203: it bounds which rows a search can lose, it does not make an approximate
graph exact.**

A `pg_cuvs_hnsw` graph can contain rows that no path from the entry point reaches (#203: a clustered table can lose
recall almost entirely). An index created under 0.9.3 therefore carries a *cover descriptor* in its metapage and a *ledger*
of heap TIDs on pages outside the graph chain. A search of a certified index combines the graph's answers with every ledger
key and ranks the merged set by exact distance over MVCC-visible heap rows.

**What is guaranteed.** Rows the graph could not reach at the last certification, and every row inserted after it, are
always candidates of a search of that index. A row that was reachable at the last certification and that a later insert cuts
off from the graph is outside the guarantee until the next recertification. How many rows that is depends on the graph's degree and on the data (measured below:
none to 1.4 percent of the original rows on a strongly clustered test table); a plain pgvector index loses rows to its inserts in the same way, and has no ledger that would still return them. The next recertification is made by the
cleanup step of a `VACUUM` (once the keys since the certification pass 5 percent of the rows it covered, and after a `VACUUM` that removed rows took the index off), by `pg_cuvs_cover_recertify()`, or by `REINDEX`.
"Last certification" means the moment the recertification *started* (the instant it holds the cover lock exclusively and
no insert is in flight). The walk is not a snapshot: it reads the pages one after the other while inserts go on. Inserts that run while it walks the graph count as
later inserts, and a row such an insert cuts off from the graph, after the walk counted it as reached, is
outside the guarantee until the next recertification. `certified_at` in the status function is that instant.

**What this costs while no recertification runs.** Every insert adds one key to the ledger, and every search of a certified index reads the whole ledger and
ranks its rows exactly, so the time of a search grows with the number of inserts since the last certification. Measured (100,000 rows of 96 dimensions, warm cache, the
ledger at 0.03, 0.3 and 3 percent of the rows): the 95th percentile of a search is 1.03, 1.04 and 1.47 times that of an index with no ledger keys. `keys_since_certification`
and `recertify_recommended` (set once the keys pass 5 percent of the rows the certification covered) in the status function show it.
How often inserts cut a row off depends on the graph's degree m: a stress graph with m = 4 lost 1 to 85 rows per round of a few thousand inserts; graphs with m of 8 or more lost none in
89,282 inserts (an independent review, CPU-built graphs); and a 300,000-row table of random 96-dimensional vectors with m = 32 (the m of a graph converted from a CAGRA graph of degree 64) lost 8 rows (0.0027 percent)
when inserts doubled it, against 6 for the same table built and filled by pgvector alone. Random vectors are not clustered. On a strongly clustered table (20,000 rows of 8 dimensions in 50 clusters, noise 0.05, m = 32,
ef_construction = 64, four sessions inserting, counted from the state right after the build, without a recertification in between), the product (pgvector's bulk build, the index
switched to `pg_cuvs_hnsw`, the inserts made through it) and pgvector alone were each run six times with this repository's tool (`test/unit/measure_insert_disconnect.py --case
clusters:20000x8/50/0.05 --m 32 --ef-construction 64 --writers 4 --switch`, different seeds) and six times by an independent review with its own script (the same table shape and counting, noise from
`random()`). Twelve runs of each kind: the product cut off 0 to 278 of the 20,000 original rows (0 to 1.4 percent) in about 15,000 inserts and 0 to 278 in about 44,000; pgvector alone cut off 0 to 136 and 0 to 296.
The tool's six runs gave the product 0, 173, 0, 278, 0, 0 and 22, 173, 24, 278, 62, 0, and pgvector alone 2 to 136 and 14 to 296; the review's six gave the product 0 to 11 and 0 to 50, and pgvector alone 0 to 62 and 0 to 62
(0 in four of its six runs at 15,000 inserts, the tool's runs in none). The two measurements put the runs in different places of these ranges and why is not established, so no end of a range is a bound. In both, the ranges of
the two kinds overlap, and the measurements do not tell the two kinds apart: they neither show that the product cuts off as few rows as pgvector alone nor that it cuts off more. An independent review counted per recertification window on the product, same table shape and m, and found 14 cut-off rows in 15,443 inserts and 47 in 44,123 (about 0.1 percent of the inserted rows).

**Graphs built on the GPU, what the ledger costs, and strongly clustered tables (measured 2026-11-20; one A100-SXM4-80GB, the patched SDK 0008
with `build_algo` left on `auto`, which took the `resident_ivf_pq` route; PostgreSQL 16.16, pgvector 0.8.7; one client, simple protocol, three
runs of 8 s, median; "pgvector scan" is pgvector's own scan over the same index pages, with the access method switched for the measurement).**

| Table (768 dimensions) | Rows the graph did not reach at the build (`base_unreached`) | Single search of the certified index | pgvector scan | recall@10 (certified search; the pgvector scan where given) |
|---|---|---|---|---|
| wiki_all, 100,000 rows | 0 (one weak component) | 1.478 ms | 1.491 ms | 1.0 |
| wiki_all, 999,000 rows (`ef_construction` 64 and 128) | 0 | 2.241 and 2.214 ms | 2.171 and 2.166 ms | 1.0 |
| 100,000 random vectors | 0 | 3.059 ms | 3.068 ms | 0.335 (an approximate graph over random 768-dimensional vectors) |
| 100,000 vectors in 50 tight clusters | 98,000 (50 weak components) | 275.8 ms | 1.08 ms | 1.0 (pgvector scan 0.033) |

On the natural and the random table the graph the GPU builds reaches every row, the ledger is empty, and the certified search equals the pgvector scan
within the measurement limit (3 percent). On the clustered table the converted graph falls into one component per cluster and a search from the entry
point sees only its own cluster: the pgvector scan returns 3.3 percent of the true top ten, the certified search returns all of them because the ledger
holds the 98,000 unreached rows, and pays for that with 2.8 microseconds per key. No setting of `pg_cuvs_hnsw` lowers `base_unreached` today:
`guarantee_connectivity` of the SDK's CAGRA build (the MST optimization) ended the daemon in `cudaErrorIllegalAddress` on every table listed here.

The cost of a key is the same wherever it comes from. For 768-dimensional vectors, which are stored out of line, ranking one key means a heap page, the TOAST
chunk and the TOAST index (4.3 buffers per key): 2.5 to 2.9 microseconds, so a certified search takes about `pgvector time + 2.9 microseconds x ledger_keys`.
Measured on the 100,000-row wiki_all index after inserts that were not recertified: 100 inserted rows 1.87 ms (pgvector 1.41), 1,000 rows 4.25 ms (1.39),
5,000 rows 15.1 ms (1.32); on the 999,000-row index 1,000 rows 4.95 ms (1.98) and 10,000 rows 32.0 ms (1.93). A recertification empties the ledger of the
rows that are reachable (after 10,000 inserted rows the keys were 0 and the search equalled the pgvector scan again) and costs 0.18 to 0.21 s at 100,000 rows
and 1.96 to 1.99 s at 999,000 rows, almost all of it the walk, with the cover lock held exclusively for 6 to 620 microseconds in each of two short phases (the longer ones with 9,000 or more keys to carry over).

To see where an index stands: `pg_cuvs_hnsw_cover_status(index)` gives `base_unreached` (the rows the graph did not reach when the guarantee was last
established), `ledger_keys` and `keys_since_certification`, and with `cuvs.hnsw_connectivity_check = on` during the build
`pg_cuvs_hnsw_connectivity_status(index)` records `unreachable` and the number of weak components (`wcc_l0`). `base_unreached` divided by the row count is the share of rows
that a search of this index has to rank exactly every time. The new rows of a table (rows the table did not hold before, 100,001 to 200,000 of
wiki_all inserted into the 100,000-row index) were all reachable after the inserts (a recertification counted 0 unreached rows at 110,000, 120,000 and 200,000 rows).
Copies of existing rows are not a measurement of that: pgvector attaches the heap TID of an exact duplicate to the existing element and builds no new one.

Not measured: other data shapes, a GPU graph that was built with a different `graph_degree` or `m`, inserts from several sessions at once.

**What is not guaranteed.** Recall of the rows the graph does reach is still that of the finite-ef approximate search
(the merged candidates are ranked exactly, but the graph supplies only part of them). "Reached" means that a path of neighbour slots of any layer leads to the
row from the entry point, which is what pgvector's search does: it descends through the upper layers and starts layer 0 where the descent ends. Counting layer 0's slots alone would
list most of a clustered graph as unreachable (measured on 20,000 rows of 16 dimensions: 14,500 of them at m = 16, and an ordered scan by the row's own vector found all 200 sampled). The cost of the
wider definition: a row that only an upper-layer slot leads to is not in the ledger, and a search finds it only when its descent ends on it. Graphs of m = 8 or more built from random
vectors had no such row; at m = 4 there were 30 in 20,000 and an ordered scan by their own vectors found one of them (`test/unit/measure_upper_layer_reach.py`). Indexes built before 0.9.3 (class
`legacy`) keep the old behavior. Indexes whose recertification failed, that a VACUUM changed since, or whose ledger does
not read are answered from the heap (see the table), not from the graph.

**Row-level security, security barriers, index expressions and partial indexes, for an index with a descriptor** (the setting
`cuvs.hnsw_exact_scan` off). By default no such statement ends in an error because of the index. What answers depends on the shape,
and the answer is as exact as the state of the index allows:

| Index | Statement without row-level security | Statement with row-level security or a security barrier |
|---|---|---|
| plain column | the merge (certified) or the heap ranking (lost), as above | the same: the ranking applies the operator class's own distance function to a stored vector, as an index scan does for the rows that are hidden, and the executor applies the policy to the rows that come out, so a hidden row never comes back |
| expression or partial | the same; the expression and the predicate are evaluated by the scan as the role that reads (no change of user) | the planner does not choose the index, and PostgreSQL's Seq Scan + Sort answers (slower, exact) |

How many rows come back when the policy hides some depends on the state. The heap ranking (a lost or untrusted index) ranks every
visible row, so `LIMIT` is filled from the rows the policy shows. The merge of a certified index ranks the graph's own answers
(about `hnsw.ef_search` of them, as with pgvector alone) and the ledger keys, and the policy is applied to that set afterwards: when it
hides part of the set, fewer than `LIMIT` rows come back, exactly as with pgvector alone. The guarantee of the merge is the one in the
table above (rows that the graph cannot reach and rows inserted since are always candidates), not that the candidates are
sufficient for a given `LIMIT` once rows are hidden. A table where the policy shows half of 4,000 rows, a certified index and
`LIMIT 100` returned fewer than 100 rows in `test/unit/test_cover_scan_pg.py` (S9).

An index that has no descriptor, or an OFF one, is answered as before in every row of this table.

What runs on the rows. The expression of an index runs only when the statement wrote that expression: the planner makes an ordered
index path only when the statement's `ORDER BY` expression equals the index expression (`equal()` on the simplified forms, the same
simplification of both: a SQL-language wrapper function is replaced by its body in both), so the scan evaluates what the statement
already contains, with the function privileges of the reading role. A predicate is different: the planner chooses a partial index when the
statement's conditions imply the predicate, which does not make the predicate part of the statement (`id > 7` implies `id > 5 OR f(id)`).
The scan therefore evaluates a predicate only when every function, operator and type in it is one that the system created (OID below
`FirstNormalObjectId`; a domain, a user-defined type, a function or operator of an extension, `pgvector` included, and a node kind that is
not on the list of simple nodes do not count), because a built-in function can still run user code through the type it works on. A
partial index with a descriptor whose predicate is not built in only is never chosen by planning, whatever its state, and PostgreSQL's
Seq Scan + Sort answers. The merge of a certified index evaluates a built-in predicate as a check; the candidates are index entries
and satisfied it when they were inserted.

What still ends in an error (SQLSTATE `0A000`, "index ... cannot answer this statement", with the cause in `DETAIL` and the action in
`HINT`): a plan that kept an index of the cases above although planning removes it (another planner hook added the path again, a plan
cache entry made before the index definition changed), and a scan that is asked to evaluate an expression or a predicate while no
statement is known for it. The scan finds the statement through the executor hooks: `ExecutorStart` (an index scan that has no runtime
keys starts there), `ExecutorRun` and `ExecutorFinish` (a data-modifying CTE that the main query does not read runs there). A nested
statement (a function, SPI, a trigger, a cursor) replaces the remembered statement while it runs; a parallel worker has its own. The scan
recognizes a protected statement by the statement's marker (a security barrier or a plan that depends on the role), by row-level security on
its own table, or by row-level security on any table of the statement's range table, which includes a partitioned or inheritance parent
whose policy reaches its children. `test/unit/test_cover_scan_pg.py` (S5 to S5d, S8, S12, S14) found no statement under the default
settings that reaches the error without a second planner hook. A security barrier cannot enter a statement through an inlined scalar
function: PostgreSQL inlines a scalar SQL function only when its body has no FROM or WHERE (`inline_function` in `clauses.c`,
REL_16_STABLE), so it cannot read a table.

When the index is not used, and how to see it. Planning leaves an index with a descriptor out of a statement, and PostgreSQL's Seq Scan +
Sort answers (exactly, and slower), in two cases only:
1. the index has an expression or a predicate, and the statement reads a table with row-level security enabled or through a security barrier.
   The test is the table's flag, not whether a policy applies to the role or implies the predicate: the owner and a role that bypasses
   row-level security are included, and so is a policy that implies the index predicate. An index without an expression and without a
   predicate is never left out for this reason;
2. the index has a predicate that is not built in only (a user function, a user-defined operator or type, a domain, an enum, a type of an
   extension such as `vector`), whatever the statement.
An index without a descriptor (built before 0.9.3 and not rebuilt) or an OFF one is not left out in either case. There is no notice and no
counter for it: `EXPLAIN` shows the Seq Scan, and `pg_cuvs_hnsw_cover_status(index)` shows the state of the index but not that a statement left
it out. `REINDEX` does not change this; a predicate of built-in functions only, or a statement without row-level security, does.

Planning cost: every statement gets its index paths filtered as they are made. A statement that names a table with an index of the
access method that has an expression or a predicate, a table that has partitions or inheritance children, or a SQL-language function in `FROM`
(which may be inlined) is also checked after planning and planned again from a copy, which costs what the setting costs.
Functions of other languages (`generate_series`, `unnest`) do not. A table with row-level security loses these paths for the owner and
for a role that bypasses row-level security as well, because the check looks at the table's flag and not at the role.

**Index classes** (`pg_cuvs_hnsw_cover_status(index)->>'class'`) and what a scan does:

| Class | Meaning | Scan |
|---|---|---|
| `certified` | the descriptor is valid and its ledger reads | graph answers merged with all ledger keys, ranked exactly; a ledger with no key is scanned as the plain graph |
| `lost` | the guarantee was withdrawn (reason in `pending`: built without certification, a VACUUM changed the graph, rows inserted by an older library, a recertification gave up, a damaged ledger page, a downgrade, an emergency rollback) | exact ranking over the heap, with a NOTICE that names the reason and what certifies it again |
| `untrusted` | the descriptor does not read, or was written by a newer pg_cuvs whose ledger this library cannot judge | exact ranking over the heap |
| `off`, `legacy` | the descriptor says off, or the index was built before 0.9.3 and has none | as before 0.9.3 (approximate, no NOTICE) |
| `not_built` | the index has no pages (what a failed `CREATE INDEX CONCURRENTLY` leaves) | not used; `DROP INDEX` or `REINDEX INDEX` it |

**Writes.** An insert takes the index's cover lock in share mode, writes its ledger key (WAL-logged before the pgvector
insert), runs the pgvector insert, and releases the lock. `VACUUM` takes a certified index to `lost` (reason vacuum) before
pgvector changes the graph and recertifies it in the cleanup step. `CREATE INDEX` on an empty table starts `certified`; a build over
rows certifies at its end, and a certification that cannot be made at the end of a build does not fail the build (the
index stays `lost` with a NOTICE). `CREATE INDEX CONCURRENTLY` and `REINDEX INDEX CONCURRENTLY` on an empty table leave the index certified (tested on empty tables only).

**Recertification.** `SELECT pg_cuvs_cover_recertify('idx')` (owner of the table) walks the graph without blocking
inserts, writes a new ledger (rows the walk could not reach, then the keys of inserts that ran meanwhile) and publishes it
with one WAL record. It waits for the cover lock in the lock manager's queue for at most `min(lock_timeout, 2 s)` and
answers `deferred` if it cannot get the table and index locks without waiting. The walk gives way (`deferred`, nothing written) when another session starts to wait for a lock that conflicts with its table lock (`ALTER TABLE`, `DROP INDEX`, `REINDEX`), so that the statement, and every reader and writer queued behind it, does not wait for the whole walk. VACUUM does the same work, without giving way; a ledger that
does not read is rebuilt by it too. The walk's memory limit is taken from `maintenance_work_mem`; beyond it the walk may use an unlinked spill file (at most half of the
free disk space, none under a finite `temp_file_limit`). A walk that does not fit leaves a `last_recertification_failure` in the
status, and VACUUM does not retry it at the same relation size and limit. A recertification that is abandoned, deferred or cancelled after it wrote its new ledger pages leaves them as free pages for the next one (`reclaimed_pages` in its answer): repeated attempts do not grow the index. The answer of `pg_cuvs_cover_recertify` also has `reused_pages`, the phase times and the memory it used.

**Functions.** `pg_cuvs_hnsw_cover_status(index regclass) -> jsonb` (needs SELECT on the table): `class`, `writable`,
`guarantee_in_force`, `guarantee` (only while in force), `certified_at`, `generation`, `base_rows`, `base_unreached`,
`ledger_keys`, `ledger_ok`, `ledger_problem`, `keys_since_certification`, `recertify_recommended`, `pending`,
`last_recertification_failure`, `handler_symbol`, `stored_reason`, `breakglass_marker`. `pg_cuvs_cover_recertify(index regclass) -> jsonb`:
`result` is `done`, `not_applicable`, `deferred`, `failed`, `skipped` or `abandoned`, with `detail`. `pg_cuvs_cover_breakglass()`
is the emergency action when the package has to go back to the older library while the SQL stays at 0.9.3 (superuser; see
[rollback-and-cleanup](playbooks/rollback-and-cleanup.md)). `pg_cuvs_cover_invalidate_all()` is called by the
`0.9.2--0.9.3` and `0.9.3--0.9.2` scripts and is not for direct use.

**Measured cost** (CPU container, PostgreSQL 16.15, pgvector 0.8.7; one sample per cell unless stated):

| What | Value |
|---|---|
| Read latency, 100,000 x 96, warm, 200 queries x 7 repeats, same index file merged vs ANN alone, p95 ratio | 1.03 at 30 ledger keys (0.03%), 1.04 at 300 (0.3%), 1.47 at 3,000 (3%, +1.87 ms); an independent run on a loaded host gave 1.11 to 1.42 |
| One recertification (idle, `pg_cuvs_cover_recertify`) | 0.09 s at 100,000 rows; 0.71 s at 500,000 x 96, m = 32 (the walk is 0.70 s of it; the index file is 434 MiB) |
| Exclusive cover lock of one recertification, 500,000 x 96 (`phase1_lock_us`, `phase3_lock_us` in the answer) | phase 1 0.14 ms, phase 3 0.01 ms; with a ledger of 25,099 keys (5 percent) 0.89 ms and 0.57 ms, because phase 1 reads the whole chain under the lock; with two sessions inserting in a loop 0.03 and 0.05 ms, and the longest insert statement of that time was 186.6 ms against a median of 174.1 ms before it |
| Insert, 100,000 x 96, server CPU per row | within measurement noise of the plain pgvector index (ratio 0.97 to 1.00) |
| Rows cut off by later inserts (not covered until the next recertification), random vectors, 300,000 x 96, product graph (metapage m = 32), +100% inserted | 8 rows (0.0027%); the same size with stock m = 16 on 100,000 rows gave 0.24 to 0.29% |
| The same on a strongly clustered table (20,000 x 8, 50 clusters, noise 0.05, m = 32, ef_construction = 64, four inserting sessions, six runs of each kind), +75% and +220% inserted | twelve runs of each kind, two measurements (the repository's tool and the review): product 0 to 278 and 0 to 278 rows of the 20,000 (0 to 1.4%); pgvector alone 0 to 136 and 0 to 296; the two kinds are not told apart |

The metapage `m` of a pg_cuvs_hnsw index is 32 (CAGRA `graph_degree` 64 gives `M0` 64); the `m` reloption is informational.
Inserts prune neighbors with that `m` and with the `ef_construction` reloption (pgvector 0.8.7 `hnswinsert.c`).

**Not verified.** GPU-built indexes over existing rows (the CPU environment has no CAGRA daemon), including
`certify_after_build` failing and `CREATE/REINDEX INDEX CONCURRENTLY` over rows; read latency at 1M rows and above, at 768
dimensions and with a cold cache; the exposure to cut-off rows on strongly clustered data of other shapes than the one table measured, and on graphs converted from a CAGRA graph; one backend `SIGBUS` seen once on a table of identical vectors that three
attempts could not reproduce; a slow start of the server seen now and then under load (not reproduced, cause not established); recertification time above 500,000 rows (a 1,000,000-row build in the 5.9 GB test VM ended
when the server was shut down immediately during the index build, cause not established, so the walk time at that size is not measured; 0.11 s at 100,000 and 0.70 s at 500,000 rows says it grows about with the row count, which is a trend and not a measurement).
