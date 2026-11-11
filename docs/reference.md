# pg_cuvs Reference

> **Current-state reference (SSOT)** for the user-facing surface: index access methods, search
> modes, GUCs, reloptions, SQL functions, and observability views. How the pieces fit together is
> in [ARCHITECTURE.md](../ARCHITECTURE.md); the rationale behind each is in
> [design/decisions.md](../design/decisions.md). Verified against `src/pg_cuvs.c` and
> `sql/pg_cuvs--0.5.0.sql` (extension version 0.5.0).

---

## 1. Index access methods

pg_cuvs registers four index AMs. All reuse pgvector's `vector` type and operators
(`<->` L2, `<=>` cosine, `<#>` inner product) via opclasses `vector_l2_ops` (default),
`vector_cosine_ops`, `vector_ip_ops`.

| AM | Create | Tuning (reloptions) | Served by | Sidecar files |
|----|--------|---------------------|-----------|---------------|
| `cagra` | `CREATE INDEX i ON t USING cagra (col vector_l2_ops)` | `graph_degree`, `intermediate_graph_degree`, `build_algo`, `index_dir` | GPU daemon | `.cagra`, `.tids`, (`.vectors`, `.delta`, `.tombstone`, `.stale`, `.shards`, `.sNNN.cagra`, `.relfilenode`) |
| `flat` | `CREATE INDEX i ON t USING flat (col vector_l2_ops) WITH (precision='float16')` | `precision`, `index_dir` | GPU daemon | `.tids`, `.vectors` (no `.cagra` graph) |
| `ivfpq` | `CREATE INDEX i ON t USING ivfpq (col vector_l2_ops)` | `n_lists`, `pq_bits`, `pq_dim`, `index_dir` | GPU daemon | `.ivfpq`, `.tids` |
| `pg_cuvs_hnsw` | `CREATE INDEX i ON t USING pg_cuvs_hnsw (col vector_l2_ops) WITH (source='my_cagra', mode='nsw')` | `source`, `mode`, `m`, `ef_construction`, `build_algo` | **pgvector** (CPU) | `.hnsw` (pgvector page format) |

`flat` is the first-class exact GPU brute-force AM (ADR-073, extension v0.4.0). It builds only a
`.tids` + `.vectors` store — no CAGRA graph — so the build is O(N) copy rather than graph
construction. Search is always exact (recall=1.0) regardless of `cuvs.search_mode`; the AM
forces brute-force internally. Freshness uses the same delta/tombstone mechanism as `cagra`
(INSERTs append to `.delta`, merged at search time). The planner picks `flat` via its own cost
function: when the hardware profile is fully probed and `cuvs.enable_phys_cost` is on, cost is the
ADR-075 physical model (§ Cost model & hardware profile); otherwise it falls back to the legacy
constants (`CUVS_FLAT_STARTUP_COST=50`, vs `CUVS_STARTUP_COST=1000` for cagra). Either way small-N
tables route to `flat` correctly. DROP removes `.tids` + `.vectors` and releases VRAM. The index is
restart-durable: `.vectors` is reloaded on daemon restart. Use `flat` for read-heavy, stable
corpora where recall=1.0 is required; use `cagra`/`ivfpq` for approximate search at large N.

> **Deprecated:** `SET cuvs.search_mode = 'brute_force'` on a `cagra` index was the previous way
> to get GPU exact BF. It still works (ADR-039) but is deprecated. `USING flat` is the
> first-class replacement: independent AM, honest build semantics, dedicated cost routing. The GUC
> path couples an exact-BF workload to an unnecessary graph build and cannot be independently cost-
> calibrated by the planner.

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
| `cuvs.k` | int | `100` | 1–2000 | USERSET | GPU top-k candidates fetched per scan (cf. `hnsw.ef_search`) |
| `cuvs.rmm_pool_mb` | int/status | daemon value | 0–INT_MAX, or `unavailable` | read-only (INTERNAL) | Applied initial RMM pool reservation per GPU, queried from the daemon. `0` = disabled; `unavailable` = no compatible daemon reply. Set using the daemon CLI and restart, not SQL SET. Record with each benchmark run |
| `cuvs.rmm_pool_max_percent` | int/status | daemon value | 0–100, or `unavailable` | read-only (INTERNAL) | Applied upstream pool quota as a percentage of total CUDA memory on each GPU. `0` when the pool is disabled |
| `cuvs.rmm_pool_status` | string/status | daemon value | JSON, or `unavailable` | read-only (INTERNAL) | Applied initial MiB, quota percentage, per-device maximum/reserved/used pool bytes, and `hnsw_auto_overflow` (`ace` or `error`, from `--hnsw-auto-overflow`). Counters cover RMM pool allocations; they are live snapshots, not process-wide GPU memory |
| `cuvs.circuit_breaker_threshold` | int | `3` | 1–100 | USERSET | Consecutive GPU errors before the (per-backend) breaker trips |
| `cuvs.circuit_breaker_transient_ttl_s` | int | `30` | 0–3600 | USERSET | Seconds before a breaker opened by a transient reason half-opens and allows one GPU attempt. `0` = half-open immediately. Breakers opened by a permanent reason ignore it. See § 5 `pg_cuvs_reset_circuit` |

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
| `cuvs.bf_precision` | enum | `float32` | `float32`, `float16` | USERSET | Resident BF index precision; float16 halves VRAM |
| `cuvs.bf_batch_wait_us` | int | `0` (off) | 0–10000 | USERSET | Daemon BF micro-batch coalescing window (µs) |
| `cuvs.cagra_batch_wait_us` | int | `0` (off) | 0–10000 | USERSET | Daemon CAGRA micro-batch coalescing window (µs) |
| `cuvs.itopk_size` | int | `0` (auto) | 0–2147483616, multiples of 32 | USERSET | CAGRA intermediate search buffer floor. Auto preserves `max(64, ceil(actual_k/32)*32)`; an explicit value is raised to cover actual k, including shard/tombstone overfetch. Does not change the number of returned candidates |
| `cuvs.search_width` | int | `0` (SDK default) | 0–INT_MAX | USERSET | CAGRA vertices expanded per iteration. Applies to single, filtered, sharded and batched CAGRA search; ignored by exact BF |
| `cuvs.cpu_hnsw_fallback` | bool | `off` | — | USERSET | Serve from the `.hnsw` sidecar instead of GPU CAGRA |
| `cuvs.max_stale_fraction` | real | `0.10` | 0.0–1.0 | USERSET | Delete-drift fraction above which a CAGRA index reroutes to CPU; 1.0 disables |
| `cuvs.max_delta_rows` | int | `10000` | 0–INT_MAX | USERSET | Pending-insert rows merged before CPU reroute; 0 disables delta |
| `cuvs.delta_search` | enum | `auto` | `auto`, `cpu`, `gpu` | USERSET | Delta merge mode: GPU-with-CPU-fallback / always CPU / GPU-only |

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

| GUC | Type | Default | Range | Set by | Purpose |
|-----|------|---------|-------|--------|---------|
| `cuvs.snapshot_uri` | string | `` (off) | — | SUSET | GCS root URI for artifact snapshots, e.g. `gs://bucket/prefix` |
| `cuvs.cluster_id` | string | `` | — | SUSET | Cluster identifier in the GCS artifact path |
| `cuvs.gcs_key_file` | string | `` | — | SUSET | Service-account JSON path; empty = GCP instance metadata |
| `cuvs.warmup_threads` | int | `2` | 1–8 | SUSET | Background warmup (GCS download) thread pool size |

CAGRA compaction (`pg_cuvs_compact` and VACUUM) initially preserves the auto
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
| `--rmm-scratch-permille N` | — (daemon-only, `scratch_permille` in `cuvs.rmm_pool_status`) | see `SHOW cuvs.rmm_pool_status` (`scratch_permille`). The released default is the value the Q1 qualification stage derived on one NVIDIA A100-SXM4-80GB host with `--cagra-build-overflow managed`; other GPUs or overflow policies may need tuning | CAGRA build scratch coefficient ρ. Admission, reservation and GPU choice use `N = final + ceil(final × ρ / 1000)`; residency accounting keeps `final`. Range 0–100000. Restart to change |
| `--max-indexes N` | — (daemon-only) | `1024` | **Soft** LRU working-set cap (ADR-068), not a hard wall (was 64). At the cap, a reload or `pg_cuvs_load` frees a slot under the § 8 rules (cold first, then soft eviction; reason `slot`); a build over the cap does not evict (with the default `--build-evict-scope none`): its files are saved and the first query reloads it |
| `--build-evict-scope …` and the other cache flags | — (daemon-only) | see § 8 | Cache, eviction and reclaim knobs (§ 8.2) |
| `--hnsw-auto-overflow ace\|error` | `hnsw_auto_overflow` in `cuvs.rmm_pool_status` (read-only) | `ace` | What `pg_cuvs_hnsw` `build_algo=auto` does over the resident budget: `ace` spills to ACE-disk with a NOTICE, `error` refuses with SQLSTATE 54000. The same value decides an auto build whose resident IVF-PQ kNN graph cuVS rejects: `ace` rebuilds on ACE-disk with a NOTICE (receipt `graph_fallback`), `error` fails. Startup-only; logged at startup. Explicit `ivf_pq`/`ace` ignore it |
| `--hnsw-ace-build-quality auto\|Q` | — (daemon-only, restart) | `auto` | ACE partition build quality handed to cuVS (`ace_params.ef_construction` = A; the SDK uses q = A/16, NN-descent runs 5 + q iterations per partition under 1e6 rows). `auto`: q = (the SDK's default NN-descent iterations, 20) − 5 at `ef_construction` 64, i.e. A 240, scaled by `ef_construction`/64. `Q` (0–4095): A = 16·Q for every ACE build, ignoring `ef_construction`. Logged at startup; recorded per index in `pg_cuvs_hnsw_build_info` (`ace_build_quality_source`, `ace_build_quality`). Resident builds ignore it |
| `--cagra-build-overflow error\|managed` | — (daemon-only, restart) | `managed` | Where a CAGRA build's large_workspace lives (ADR-088). `managed`: a build-only managed-memory resource outside the RMM pool and quota that can spill to host RAM (slower, see below). `error`: inside the pool, as before. Applies to `cagra` single and parallel (multi-partial) builds only; sharded builds, startup warmup and `pg_cuvs_hnsw` always use `error` |
| `--managed-overcommit on\|off` | — (daemon-only, restart) | `on` | Policy `managed` only. `off`: refuse (SQLSTATE 53400, reason AR6) a build whose large_workspace estimate would not fit in physical GPU free memory. `on`: allow spill up to the host memory guard, refuse beyond it (53200, AR7) |
| `--managed-lw-margin-permille N` | — (daemon-only, restart) | see `SHOW cuvs.rmm_pool_status` (`lw_margin_permille`) | Policy `managed` only. Safety factor on the large_workspace estimate `L_shape`. Range 1000–100000; out of range refuses startup |
| `--rmm-build-headroom-permille N` | — (daemon-only, restart) | `150` | AR8 headroom for a CAGRA single or parallel build (ADR-089): when `pending + pool_used + N + headroom` exceeds the pool quota, the build eviction scope may evict for the shortfall, otherwise the build is refused (AR5 or AR8, see RMM quota and memory accounting), with `headroom = max(256 MiB, ceil(N × value / 1000))`. `0` removes the headroom but keeps the capacity check; it does not refuse the Q6 shape (see ADR-089). Range 0–100000; out of range refuses startup. The value is on the `CAGRA build pool headroom` startup log line |
| `--oom-score-adj N` | — (daemon-only, restart) | `500` | Written once to `/proc/self/oom_score_adj` at startup so the kernel OOM killer prefers the daemon over PostgreSQL under host RAM pressure. Range 0–1000; `0` leaves the file untouched; a higher current value is kept (never lowered); a write failure is logged, not fatal |
| `--gpu-devices LIST` | — (daemon-only) | all visible | CUDA devices the daemon uses (else honors `CUDA_VISIBLE_DEVICES`) |
| `--snapshot-uri URI` | `cuvs.snapshot_uri` | `` | GCS snapshot root |
| `--cluster-id ID` | `cuvs.cluster_id` | `` | Cluster id in the GCS artifact path |
| `--gcs-key-file PATH` | `cuvs.gcs_key_file` | `` | Service-account JSON path |
| `--warmup-threads N` | `cuvs.warmup_threads` | `2` | Background GCS warmup pool size |

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
| `precision` | enum | `float32` | `float32`, `float16` | Resident BF index precision. `float16` halves VRAM (N·dim·2 B vs ·4 B); falls back to `cuvs.bf_precision` GUC if not set |
| `index_dir` | string | (uses `cuvs.index_dir`) | path | Per-index artifact directory; same semantics as `cagra` (ADR-045) |

VRAM sizing: `float32` requires N·dim·4 bytes resident (e.g. 1M×1024 ≈ 4 GB); `float16` halves
it. If the corpus does not fit in free VRAM at build time the daemon returns a dedicated
no-fit error ("corpus N·dim·4B exceeds free VRAM; try precision=float16, shard, or use
cagra/ivfpq") rather than silently failing.

### `cagra`

| Reloption | Type | Default | Range / values | Notes |
|-----------|------|---------|----------------|-------|
| `graph_degree` | int | `64` | 8–512 | CAGRA output graph degree |
| `intermediate_graph_degree` | int | `128` | 8–1024 | Must be ≥ `graph_degree` (fail-closed) |
| `build_algo` | enum | `auto` | `auto`, `ivf_pq`, `nn_descent` | CAGRA build algorithm. The IVF-PQ kNN search (`ivf_pq`, or `auto` when cuVS picks IVF-PQ) uses fp32 LUT, internal and coarse distances when 4 x max\|x\|^2 of the corpus (max\|x\|^2 for inner product and cosine) reaches 65504, the fp16 maximum; otherwise cuVS's fp16 defaults stay. The daemon logs `[cuvs_cagra_build_multi] knn precision` with the bound and the choice. Compaction (`pg_cuvs_compact`, VACUUM) rebuilds the kept rows with cuVS's `auto` choice under the same rule and logs `[cuvs_cagra_compact] knn precision` |
| `index_dir` | string | (uses `cuvs.index_dir`) | path | Per-index artifact directory (ADR-045); self-describes in `reloptions` so no-GUC sessions still find it |

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
| `source` | string | `` | a `cagra` index name on the same table | empty = ephemeral CAGRA built from the heap |
| `mode` | string | `nsw` | `nsw`, `hnswlib`, `hnsw`, `hnswlib_file` | `nsw` and `hnswlib` are recommended |
| `m` | int | `16` | 2–100 | **informational** — the CAGRA graph degree drives the HNSW layout, not `m` |
| `ef_construction` | int | `64` | 4–1000 | Direct heap build: construction effort; source reuse: informational |
| `build_algo` | enum | `auto` | `auto`, `ivf_pq`, `ace` | Direct heap build only; cannot be combined with explicit `source` |

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
`nn_descent` is reserved for this direct HNSW surface and is rejected until a supported
memory model exists; the separate `cagra` AM has its own build-algorithm options.

An over-budget refusal (SQLSTATE 54000, `pg_cuvs: resident IVF-PQ estimate exceeds the
HNSW build budget`) has the DETAIL `he=<MiB> hb=<MiB> ge=<MiB> gb=<MiB> rp=<MiB> rl=<MiB>`:
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

`m` remains informational. Direct `ef_construction` scales intermediate degree from128,
hierarchy construction effort from200, and ACE effort from240 at the default64 (ACE: the
`--hnsw-ace-build-quality auto` rule, 20 NN-descent iterations per partition at 64);
receipts record the consumed/clamped values. ACE indexes also record a partition-builder
readback (`ace_build_quality`, `ace_local_intermediate_degree` = 3 × (graph_degree / 2),
`ace_local_nn_descent_iterations`); the receipt's `intermediate_degree` is the resident width
and the ACE memory-plan input, not the width ACE partitions build and optimize with. Reusing a source CAGRA graph does not
rebuild it according to these options. See [HNSW build options](hnsw-build-options.md)
for limits, compatibility and estimate-vs-upper-bound caveats.

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
| `pg_cuvs_hnsw_build_info(index regclass)` | jsonb | Read the stored construction route/settings and estimate-based admission. Version2 writer with historical version1 reader; absent legacy/source/empty receipts are not reconstructed from current GUCs. Requires SELECT on the source table; estimates are not memory upper-bound guarantees |
| `pg_cuvs_build_hnsw(cagra regclass, mode text DEFAULT 'nsw')` | regclass | Build a pgvector HNSW from a CAGRA index without pgvector's CPU build. **Deprecated** in favor of `CREATE INDEX ... USING pg_cuvs_hnsw`; the older two-step `pg_cuvs_import_hnsw` form (empty `USING hnsw` target + import) is removed |
| `pg_cuvs_compact(index regclass)` | void | Remove tombstoned vectors via `cuvsCagraMerge`, rebuild `.cagra`/`.tids`, drop `.tombstone`. The rebuild uses fp32 IVF-PQ kNN distances under the `build_algo` fp16 rule. If the initial NN-descent or auto merge runs out of memory, retry once with explicit IVF-PQ under that same rule. A pinned IVF-PQ failure is not retried |
| `pg_cuvs_batch_search(rel regclass, queries vector[], k int)` | SETOF (query_idx int, ctid tid, distance real) | Q queries in one IPC/GPU dispatch; JOIN on `ctid` for visible rows; honors `search_mode`/`bf_precision` |
| `cuvs_filtered_knn(index regclass, query vector, filter_tids bigint[], k int)` | TABLE (ctid tid, distance float4) | Exact GPU BF restricted to a sorted TID set (`block<<16\|off`); NULL = unfiltered |
| `cuvs_filtered_knn(index regclass, query vector, filter_tids tid[], k int)` | TABLE (ctid tid, distance float4) | Type-safe `tid[]` overload (accepts `ctid` directly) |
| `pg_cuvs_gc_orphans(do_delete bool DEFAULT false)` | SETOF (db_oid oid, index_oid oid, reason text, action text) | Reconcile `index_dir` vs catalog; dry-run by default (ADR-046) |
| `pg_cuvs_last_search_latency_us()` / `_n_results()` / `_k()` / `_index()` / `_metric()` | int / int / int / oid / text | Process-local stats for the most recent scan in this backend; NULL if none |
| `pg_cuvs_hw_profile()` | SETOF (gpu_name text, n_gpus int, total_vram_bytes bigint, link_bw_bytes_per_us float8, hbm_bw_bytes_per_us float8, gpu_bf_tput float8, ipc_rtt_us float8, measured_at_epoch bigint, probe_status int, source text, matches_running_daemon bool, cpu_dist_tput float8, gpu_cagra_lat_us float8) | The measured (or DEFAULT) hardware profile the daemon wrote at boot, consumed by the ADR-075 cost model. `source` = `measured` \| `default`; `probe_status` is a bitmask of which coefficients were measured; `matches_running_daemon` = false flags a stale profile (GPU swap / migration). Read-only, no CUDA/IPC |

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
[ARCHITECTURE.md §5](../ARCHITECTURE.md#5-index-lifecycle-and-on-disk-artifacts).

---

## 8. GPU memory operations

In this chapter, *sidecar* means only the `.delta` and `.tombstone` files of an index
(not `.cagra`, `.tids` or `.vectors`).

### 8.1 Model: a shared cache and work memory

Resident indexes are a shared cache (like `shared_buffers`); build scratch is work
memory (like `work_mem`). The files in `index_dir` are the source of truth: an index
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
eviction. Every eviction writes one `[evict]` line (`op gpu db idx reason
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
(`object_not_in_prerequisite_state`) in CAGRA, flat, IVF-PQ, batch search,
and filtered index searches. The error identifies the index and recommends
`REINDEX INDEX`. A genuine empty filter result remains an empty result.

Marking an index stale immediately invalidates cached plans for its parent
table, including when the writing transaction later rolls back. New plans can
choose the CPU path. Invalidation does not replace an executor already running:
such a scan can fail with `55000`. Roll back the failed transaction (or recover
to an application-managed savepoint), then retry with a fresh plan. There is no
automatic replay of the SQL statement. REINDEX restores GPU eligibility.

This is an interim correction. Marker IPC remains best-effort; if the daemon is
down when the marker is sent, its absence can still cause missing rows after
restart. Durable stale publication, the shared insertion/deletion cap, and insertion durability
recovery are separate unfinished work. Do not treat this change as fixing those
paths.
