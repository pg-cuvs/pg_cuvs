# pg_cuvs Architecture

> **Current-state reference (SSOT), source-checked at `ae818ad0` (2026-11-11).** This document describes how pg_cuvs works *as built*.
> For the user-facing surface (GUCs, reloptions, SQL functions, views) see
> [docs/reference.md](docs/reference.md). For *why* each decision was made see
> [design/decisions.md](design/decisions.md) (ADRs). For the *history* of how each phase was
> built and verified see [design/specs/phase-record.md](design/specs/phase-record.md) (frozen). Document map:
> [docs/doc-map.md](docs/doc-map.md).

---

## 1. What pg_cuvs is

pg_cuvs keeps PostgreSQL as the control plane and offloads only the expensive, highly
parallel part — vector candidate generation — to a GPU sidecar. It is middleware, not a
standalone vector database.

Invariants that shape everything below:

- **PostgreSQL owns SQL and transactions.** The `vector` type, operators, and opclasses are
  reused from pgvector. MVCC visibility, ACL, joins, filters, and the final heap recheck all
  run in PostgreSQL.
- **The GPU never owns heap tuples.** The sidecar returns candidate TIDs + distances; the
  backend resolves those to visible rows.
- **Backends never create a CUDA context.** A sidecar daemon (`pg_cuvs_server`) owns per-device
  CUDA resources and the GPU index registry shared by backend sessions.
- **`CREATE INDEX USING cagra` is durable-or-nothing.** A successful build means the GPU build
  *and* the on-disk persistence both succeeded.

---

## 2. Components

| Component | Process | Source | Owns |
|-----------|---------|--------|------|
| Extension `.so` | each PostgreSQL backend | `src/pg_cuvs.c` | Index AM handlers (`cagra`, `flat`, `ivfpq`, `pg_cuvs_hnsw`), planner cost hook, `set_rel_pathlist_hook` (D-wedge + transient BF `CuvsTransientBF`), GUCs, reloptions, SQL functions, object-access (DROP) hook |
| Sidecar daemon | standalone `pg_cuvs_server` | `src/pg_cuvs_server.c` | Per-device CUDA resources, GPU index registry, build/search/evict/reload, warmup pool, GCS upload |
| Compaction bgworker | PostgreSQL background worker | `src/pg_cuvs_compaction.c` | Phase 4C auto-`REINDEX CONCURRENTLY` when delta growth crosses a threshold |
| IPC client | linked into the backend | `src/cuvs_ipc.c` / `.h` | UDS framing, shm/memfd payload handoff, interruptible waits |
| cuVS wrapper | linked into the daemon | `src/cuvs_wrapper.cu` | C++ bridge to cuVS: CAGRA build/search/serialize, IVF-PQ, HNSW `from_cagra()` |
| HNSW export | extension; SDK builder in daemon | `src/hnsw_export.c`, `src/cuvs_wrapper.cu` | Import private build files or an existing CAGRA graph into pgvector-compatible PostgreSQL index pages |
| Corpus builder | extension | `src/cuvs_build_corpus.c` | Tiered build corpus (memfd / shm / heap) + `SCM_RIGHTS` fd passing (ADR-057) |
| Object store | daemon | `src/cuvs_objstore.c` | GCS client (curl), manifest JSON, upload/download, SHA256 verification |
| Shared utilities | both | `src/cuvs_util.c` (PG/CUDA-free) | Circuit-breaker FSM, TID encode/decode, `.tids` header magic/CRC32, latency histogram |

The extension and daemon have separate address spaces and do not share PostgreSQL
internal memory or catalog access. They explicitly map shared payloads through named
shared memory or passed file descriptors (§4). This is the
root cause of several deliberate designs: orphan reconciliation (§11), fail-closed artifact
validation (§6), and the split between daemon-sourced and backend-sourced statistics
(see [docs/reference.md](docs/reference.md) observability views).

---

## 3. Data and control flow

### 3.1 Build (`CREATE INDEX ... USING cagra`)

```
backend (cuvs_ambuild)                         daemon (handle_build)
  scan heap → (vector, TID) pairs
  serialize to corpus tier:                     
    memfd (SCM_RIGHTS) > named shm > heap copy
  IPC BUILD frame + index_dir ──────────────▶  mmap corpus
                                                resolve shard_count (auto/1/N)
                                                cuVS CAGRA build → VRAM
                                                write .tids (magic+CRC) tmp→fsync→rename
                                                write .cagra        tmp→fsync→rename
                                                fsync(index_dir)
                                                [sharded: per-shard .sNNN.cagra +
                                                 .shards manifest written LAST = commit]
                                                [optional .vectors sidecar]
                                                register in VRAM registry
                                                [async: GCS upload thread]
  reply OK / BUILD_FAILED / PERSIST_FAILED ◀──  reply
  on error: ereport(ERROR) → catalog rollback
```

Parallel builds (ADR-059, `handle_build_multi`) have the leader collect N worker
partial descriptors; the daemon streams their mapped corpora to the GPU. Admission
accounts for resident indexes and pending builds (§6). Measuring an index in isolation
is a benchmark methodology, not a runtime requirement.

### 3.1a Build (`CREATE INDEX ... USING flat`)

The `flat` AM uses a separate IPC op (`CUVS_OP_BUILD_FLAT`) and a dedicated daemon handler
(`handle_build_flat`). The flow is identical to cagra up through corpus delivery, then diverges:

```
backend (cuvs_build_flat_from_heap / flat_ambuild)   daemon (handle_build_flat)
  scan heap → (vector, TID) pairs
  serialize corpus (same tiered path as cagra)
  IPC BUILD_FLAT frame + index_dir ─────────────▶  mmap corpus
                                                    write .tids  tmp→fsync→rename
                                                    write .vectors tmp→fsync→rename
                                                    write commit marker
                                                    -- NO .cagra, NO cuvsCagraBuild --
                                                    refresh_main_bf_cache (graph-free)
                                                    register IndexEntry with is_flat=1,
                                                      handle=NULL, graph VRAM not reserved
  reply OK / BUILD_FAILED / PERSIST_FAILED ◀──────  reply
  on error: ereport(ERROR) → catalog rollback
```

Key differences from cagra:
- `finish_build_commit` (which serializes `.cagra`) is **not called**. The flat handler has its
  own tmp+rename+fsync sequence for `.tids` and `.vectors`.
- `IndexEntry.is_flat=1` signals a new daemon state: NULL cagra handle + unsharded, not the
  former "NULL = sharded" assumption. All cagra handle dereferences in `handle_search`,
  `handle_search_batch`, and `evict_lru` are guarded by `cagra_handle_required(e)` / `e->is_flat`
  checks to prevent NULL deref.
- On eviction: flat entries are freed without calling `save_index` (no handle to serialize);
  they reload from `.vectors` on next use.
- On daemon restart: `startup_load_indexes` detects `.vectors` present without `.cagra`/`.ivfpq`
  and registers the entry as `is_flat=1` (restart-durable).
- DROP: `cuvs_object_access` matches the flat index OID and calls `free_main_bf_cache` to release
  the resident BF VRAM before removing artifacts.

### 3.1b GPU-built CPU HNSW (`USING pg_cuvs_hnsw`)

A source-less build scans/spools the heap into private regular files and sends
`BUILD_HNSW_FILE` (op 23). The daemon produces a temporary hnswlib-format file;
`hnsw_export.c` imports it into the PostgreSQL index relation and records the
build-policy receipt. No persistent CAGRA registry entry is created for this
path. `build_algo=auto` selects a tiny/resident path or ACE on disk according to
its supported route and budgets; explicit `ivf_pq` does not spill to ACE.

With `source='existing_cagra'`, the exporter instead reuses that index's graph
on the same table. Explicit non-auto `build_algo` cannot be combined with source
reuse. The resulting index delegates search and maintenance to pgvector's HNSW
AM; serving it does not require the daemon. Its pages are PostgreSQL index
storage, not the optional `.hnsw` sidecar in §5. See
[HNSW build options](docs/hnsw-build-options.md) for page-size limits, modes,
precision/SDK guards, budgets, and `pg_cuvs_hnsw_build_info()`.

### 3.1c Transient no-index brute force (`cuvs.gpu_bruteforce`)

The extension's `set_rel_pathlist_hook` runs two independent sub-hooks for base
relations. `cuvs.gpu_bruteforce = on` enables the transient branch; it is not the
switch for D-wedge:

- **`cuvs_dwedge_add_path`** (ADR-063) — adds the D-wedge filtered CustomScan path for indexed
  relations.
- **`cuvs_transient_bf_add_path`** — adds the `CuvsTransientBF` CustomScan path for the
  no-index transient corpus case.

`cuvs_transient_bf_add_path` inspects the query shape and only adds the path when all of the
following hold: single base relation (no join), no OFFSET, no GROUP BY / DISTINCT / HAVING /
window, a plan-time Const LIMIT in the range 1..100000, and exactly one distance ORDER BY key.
Any shape violation causes it to return without adding a path; the planner then picks seqscan
as usual. No pathkeys are claimed — the planner keeps `Limit → Sort → CustomScan`.

Execution flow:

```
executor (CuvsTransientBF ExecCustomScan)          daemon (handle_search_bf_transient)
  scan live heap under estate->es_snapshot
  apply WHERE quals (filter-first)
  detoast vectors into per-query corpus
  bind query vector at execution time
  ($1 parametrized queries: NOT downgraded)
  IPC SEARCH_BF_TRANSIENT (op 22) ────────────▶  non-evicting VRAM admission:
                                                    never evicts a resident flat/cagra index
                                                    corpus too large → OOM_FALLBACK → backend ERROR
                                                  cuvs_brute_force_search (exhaustive at selected precision)
                                                  no IndexEntry created
  reply: top-k (tid, dist) ◀──────────────────   reply
  heap recheck (MVCC, ACL)
  return ≤ k visible rows
```

Key differences from the `flat` AM (3.1a):
- **No on-disk artifacts and no IndexEntry.** The corpus is materialized per-query from the heap;
  nothing is written to `index_dir` and no daemon registry entry is created.
- **Non-evicting admission.** The daemon checks the VRAM budget before accepting the corpus, but
  will never evict a resident index to make room. On `OOM_FALLBACK` the backend raises an ERROR
  (fail-closed); no silent CPU fallback.
- **Host cap.** The backend enforces `cuvs.gpu_bruteforce_max_mb` before the IPC handoff: if the
  materialized corpus exceeds the limit the query fails with an ERROR before touching the daemon.
- **Write cost zero.** There is no index structure to update on INSERT/UPDATE/DELETE.
- **Read cost per query.** Every query pays heap scan + detoast + H2D transfer regardless of how
  recently the same query ran. For stable, read-heavy corpora `USING flat` (3.1a) amortizes this
  cost across queries.

Verified Tier-2 on A100/PG16 (2026-06-14): `make installcheck` 32/32 GREEN + isolation 3/3 GREEN
(new `transient_bf` test + regression fence).

### 3.2 Search

```
planner (cuvsamcostestimate)
  stat() the .stale sidecar — no CUDA, no IPC
  stale OR delete-drift > cuvs.max_stale_fraction → high cost → CPU path
  else → GPU cost: ADR-075 physical κ-model when the hw profile is probed
         (loads <index_dir>/cuvs_hw_profile sidecar — no CUDA/IPC), else legacy
         k-dominant constants (cuvs.enable_phys_cost gate / probe_status bits)

executor (cuvs_gettuple)            daemon (handle_search)
  query vector → shm                  find index (load on miss; cache hit/miss/reload counters)
  IPC SEARCH ─────────────────────▶   gate: stale / dim / metric mismatch
  (interruptible wait, Phase 3S)      [sharded] lock-free snapshot → parallel fanout → merge
                                      cuVS CAGRA / BF / IVF-PQ search
                                      [delta cache merge if resident + generation match]
                                      record stats
  reply: TIDs + distances ◀────────   reply
  delta merge (CPU side if needed)
  tombstone filter (snapshot-aware)
  heap recheck (MVCC, ACL)
  return ≤ k visible rows
```

New plans use cost gates to choose a CPU path when an index is stale or unavailable.
An already started scan receiving `STALE` raises SQLSTATE `55000`; it does not
switch executor nodes or return an empty success. Other daemon errors and query
cancellation follow their error/interrupt paths. Plan-time fallback is visible
in `pg_stat_gpu_fallback`; daemon replies are recorded in `pg_stat_gpu_search`.

---

## 4. IPC

- **Transport:** Unix domain socket, default `/tmp/.s.pg_cuvs` (GUC `cuvs.socket_path`).
- **Command frame:** global protocol v5, a fixed-size `CuvsCmdFrame` (`src/cuvs_ipc.h`) carrying op code, db/index
  OID, `k`, metric, dim, `n_vecs`, a `shm_key` for the payload, plus per-feature fields
  (shard config, search mode, BF precision, build params, filter TIDs, IVF-PQ params, extend
  chunk size).
- **Payload:** zero-copy via `shm_open`/mmap (query vector, corpus) or `SCM_RIGHTS` memfd for
  build corpora; parallel builds pass N partial-shm descriptors after the frame.
- **Operations** (`CUVS_OP_*`): SEARCH, BUILD, STATUS, MARK_STALE, CACHE_STATS, SHARD_STATS,
  DROP_INDEX, EXPORT_ADJACENCY, EXPORT_HNSW_SHM, SEARCH_BATCH, BUILD_IVFPQ, SEARCH_IVFPQ,
  EXTEND, COMPACT, SEARCH_STREAM_BF, **BUILD_FLAT** (op 21, vectors-only build for the `flat`
  AM — no graph), **SEARCH_BF_TRANSIENT** (op 22, transient no-index corpus BF — no IndexEntry,
  non-evicting VRAM admission, handler `handle_search_bf_transient`), private
  **BUILD_HNSW_FILE** (23), server/pool settings (24/25), load/unload/pin and
  status/reclaim operations (30–37), and HNSW/CAGRA preflight (40/41).
  See `src/cuvs_ipc.h` for the full list. Synthetic build/extend OOM injection
  dispatch is compiled only with `CUVS_TEST_HOOKS`; use the separate test daemon.
- **Reply status** (`CUVS_STATUS_*`): OK, ERROR, OOM_FALLBACK, NOT_FOUND, UNAVAILABLE,
  BUILD_FAILED, PERSIST_FAILED, DIM_MISMATCH, METRIC_MISMATCH, STALE, NO_VECTORS,
  CANCELED, PROTO_MISMATCH, and HNSW_RESIDENT_BUDGET_EXCEEDED. Admission replies
  also carry structured rejection/NOTICE fields. The caller determines the reaction:
  scan-time STALE/OOM/connection failures raise errors, while future plans can
  choose CPU paths. Deploy both binaries together; HNSW op 23 additionally checks
  its own payload v5 and returns a stored build-policy/ACE readback receipt.
- **Interruptibility (Phase 3S):** the backend's reply wait is a `poll()` loop with a wait
  callback, so `statement_timeout` / query-cancel break a long GPU search (~sub-second) instead
  of blocking indefinitely. The daemon ignores `SIGPIPE` to survive mid-reply client disconnect.

---

## 5. Index lifecycle and on-disk artifacts

An index moves through: **build → serialize → load → evict-to-disk → reload**, plus the write-path
states (stale / delta / tombstone). All artifacts live under `index_dir` named
`<db_oid>_<index_oid><suffix>`.

| Suffix | Holds | Written by | Notes |
|--------|-------|-----------|-------|
| `.cagra` | CAGRA graph (cuVS binary) | build, eviction | the resident index |
| `.tids` | versioned header (`TIDS` magic, version, n_vecs, dim, metric, body CRC32) + `uint64_t[]` TID map | build | validated on every load; headerless files are rejected (REINDEX) |
| `.vectors` | full corpus `float32[n][dim]` (+ `VECS` header) | build (optional for `cagra`; **required** for `flat`) | required by `flat` AM, brute-force mode, and stream-BF; `flat` indexes reload from this file on daemon restart |
| `.delta` | pending inserts: `DELT` header + `(tid, vec[dim])` rows | `aminsert` (append) | lazily loaded into a GPU BF delta cache; generation tied to `.tids` CRC; cleared by REINDEX |
| `.tombstone` | deleted TIDs: `TOMB` header + records | `ambulkdelete` (append) | snapshot-aware filter at search; cleared by REINDEX |
| `.stale` | empty marker | `aminsert`/`ambulkdelete` fallback | read by the planner cost hook; cleared by REINDEX |
| `.shards` | shard manifest (`SHRS` header + per-shard records) | sharded build | written **last** = commit marker; absent ⇒ fail-closed |
| `.sNNN.cagra` | one CAGRA artifact per shard | sharded build | loaded together with the manifest |
| `.relfilenode` | heap relfilenode identity | build | GCS warmup heap-compat gate |
| `.hnsw` | serialized hnswlib CPU sidecar | optional CAGRA export | searched by the daemon when `cuvs.cpu_hnsw_fallback=on`; distinct from PostgreSQL index pages |
| `.ivfpq` | compressed IVF-PQ index | IVF-PQ build/eviction | used with its `.tids`; incremental INSERT is not supported |

**Serialization is atomic:** tmp file → `fsync` → `rename` → `fsync(dir)`. For sharded indexes the
`.shards` manifest is the commit marker, so a half-written shard set never loads.

**Startup load** scans `index_dir`, validates `.tids` headers and `.shards` geometry, and loads
only complete, valid pairs — everything else is skipped (fail-closed). Phase 3D registers
cold indexes (those with a `.relfilenode` marker but no local `.cagra`) for background GCS warmup.

---

## 6. Memory admission, accounting, and eviction

The daemon combines several budgets rather than treating a single free-memory
number as a complete capacity estimate:

- The logical index budget defaults to **90% of device VRAM** and can be set with
  `--max-vram-mb`; resident indexes and pending builds contribute to accounting.
- The RMM pool defaults to an initial **512 MiB per usable GPU**, with a maximum
  of **50% of device VRAM**. `--rmm-pool-mb=0` disables the pool. Pool quota and
  logical index budget are distinct controls.
- Build admission includes estimated final/scratch needs. The current
  `--rmm-scratch-permille` default is **9593**, from the recorded Q1 maximum;
  `--rmm-build-headroom-permille` defaults to **150**. AR8 refuses an insufficient
  pool-capacity premise before eviction while another pool allocator is active.
- A physical-free preflight runs before CAGRA single/multi/shard builds,
  IVF-PQ, HNSW-file builds, and compaction. It refuses when raw physical free
  memory is below **32 MiB**, including when idle pool memory still appears
  reusable. The rejection carries reason `PHYS`, a retryable SQL error/NOTICE,
  and daemon counters. The SDK can allocate memory outside the pool.

These checks are samples and estimates, **not reservations or a full peak-memory
bound**. Other processes, concurrency, and later SDK allocations can still exhaust
memory. See [memory accounting and admission](docs/reference.md#rmm-quota-and-memory-accounting)
for controls, reason codes, exclusions, and status fields.

When the applicable admission policy permits it, LRU reclamation serializes an
eligible resident index, frees its handle, and leaves artifacts for reload.
Pinned, in-flight, and unsupported sharded candidates are not ordinary eviction
victims. A persistence failure aborts eviction and increments `persist_failures`.
`pg_cuvs_unload/load/pin/unpin`, reclaim controls, `pg_cuvs_index_status()`,
`pg_cuvs_memory_status()`, and `pg_cuvs_build_status()` expose the current state;
see the reference for privileges and request-specific policy.

An `OOM_FALLBACK` reply does not restart an executing SELECT on the CPU. The
backend reports an error and records the failure for later planner/circuit-breaker
routing. Build rejection also aborts the DDL. HNSW `auto` has a separate,
explicitly reported resident-to-ACE-disk policy (§3.1b).

Hit/miss/eviction/reload/persist-failure counters remain available per GPU in
`pg_stat_gpu_cache`. Load validation checks `.tids` CRC, shard geometry, GCS
checksums, heap identity, and manifest versions before accepting artifacts.

---

## 7. Multi-GPU sharding (Phase 3E–3G)

A logical CAGRA index can be split into N shards, one per GPU. A global `.tids` is the TID source
of truth; each shard is an independent `.sNNN.cagra` with a TID offset; the `.shards` manifest
ties them together. `cuvs.shard_count` is `0` (auto from VRAM budget), `1` (unsharded), or `N`
(forced). Auto resolves a small index to unsharded, so single-GPU behavior is byte-identical to
the pre-sharding path.

Search uses a **lock-free window** (ADR-022, safe-by-construction): under the registry mutex the
daemon snapshots the shard descriptors + tids + metric, releases the mutex, dispatches the
per-shard searches (concurrently when `cuvs.parallel_fanout` is on), then re-acquires the mutex to
re-find the entry, merge results with a metric-aware comparator, and update stats. Safety rests on
three invariants: sharded entries are non-evictable (marked `gpu_device_id=0xFFFFFFFF`), shard
memory is freed only by DROP (serialized by `AccessExclusiveLock`), and an in-flight counter blocks
eviction during the window. `cuvs.shard_overfetch` requests `k + overfetch` per shard to preserve
recall through the global merge. Per-shard placement and stats are in `pg_stat_gpu_shards`.

---

## 8. GCS snapshot and multi-node warmup (Phase 3C/3D)

When the daemon is started with `--snapshot-uri`, a successful build spawns a detached thread that uploads the
artifacts and writes `manifest.json` **last** (commit marker). The manifest records pg_cuvs and
cuVS versions, OIDs, `relfilenode`, base generation, metric, dim, vector count, and per-file
SHA256.

A daemon serving a different node downloads on cold-miss: a fixed warmup thread pool
(`--warmup-threads`) pulls from GCS, verifies SHA256, checks that the manifest `relfilenode`
matches the local heap (a restored/rebuilt heap changes it → REINDEX required), checks cuVS-version
compatibility, then loads. Background GCS hydration runs asynchronously; socket
readiness still follows startup initialization and local loading. A cold index
can be avoided at plan time; a scan that already selected it can fail and require
a new query. Authentication
uses the GCP instance metadata service, falling back to a service-account key
(`--gcs-key-file`). The similarly named PostgreSQL GUCs are not automatically
forwarded to this standalone process: configure `--snapshot-uri`, `--cluster-id`,
`--gcs-key-file`, and `--warmup-threads` in its launch command/service. See the
[GCS playbook](docs/playbooks/gcs-snapshot-ops.md).

---

## 9. Write path and correctness

Write behavior depends on the access method:

- **CAGRA INSERT/UPDATE** first attempts daemon `EXTEND`. If that fails, the
  backend appends `(tid, vector)` to `.delta`. A scan merges base candidates with
  exact delta search on the GPU when a generation-matching cache is available,
  otherwise on the CPU, according to `cuvs.delta_search`.
- **Flat INSERT/UPDATE** uses the delta append/merge path; it does not extend a
  CAGRA graph. **IVF-PQ INSERT is a no-op** and requires rebuilding after loads
  or vector changes. It does not inherit the CAGRA/flat freshness mechanism.
- **VACUUM** appends dead TIDs to `.tombstone`; scan-side filtering observes the
  snapshot and over-fetches candidates. The same `cuvs.max_delta_rows` setting
  caps each delta and tombstone file. Reaching the tombstone cap can mark an
  index stale before compaction, even during normal autovacuum. `ANALYZE` alone
  does not run tombstone compaction.
- **Fallback marker** — if the delta/tombstone append fails, the path marks the index `.stale`,
  and the planner cost hook routes new plans to CPU until a REINDEX clears it.
  The backend immediately invalidates cached plans depending on the parent table;
  this notification survives transaction rollback. The marker also survives rollback
  when the marker IPC succeeds; an unavailable daemon can lose the marker.
  An already executing scan that receives STALE raises SQLSTATE `55000` with the
  index name and REINDEX guidance; it never treats that reply as an empty result.
  The application must roll back the failed transaction before retrying the query.
  Marker delivery remains best-effort: a daemon outage can lose the marker (#202).
  This interim fix does not add marker durability or change the shared delta/tombstone cap.
  A delete-drift backstop (`cuvs.max_stale_fraction`) reroutes to CPU when too many rows were deleted since build.

REINDEX normally rebuilds the index and clears `.stale` / `.delta` / `.tombstone`.
IVF-PQ is an exception: `ivfpq_ambuild` does not remove an old tombstone sidecar,
so ordinary REINDEX can leave the reader blocked. #202 observed recovery with
REINDEX CONCURRENTLY; verify the resulting plan and sidecar state.

---

## 10. Key engineering techniques

Non-obvious choices a reader will otherwise rediscover the hard way. Rationale in the cited ADRs.

- **BITSET polarity inversion (ADR-048, fixed in PR #49).** cuVS BITSET semantics are
  `bit=1 = INCLUDE`; the 3O prefilter wrapper inverts to pg_cuvs's `bit=1 = EXCLUDE` convention.
  Getting this backwards silently flips the filter. A Tier-1 shim test now asserts the polarity.
- **Reverse TID→item map (3O, ADR-048).** A sorted `rev_tids[] → rev_item_ids[]` map, built in
  *both* build paths, converts WHERE-clause heap TIDs to GPU item indices by binary search.
  It was originally built only on the load path, so freshly built indexes silently degraded to
  post-filter (a false-done caught by ADR-064 work); the new mode is now asserted in tests.
- **Fail-closed everywhere (ADR-016/019/024/047/066).** Delta cap, `.tids` CRC, `.shards`
  geometry, GCS SHA256, heap relfilenode, and cuVS-version gates all reject rather than degrade.
- **Memory accounting and physical preflight.** Logical index usage, pool quota,
  pending reservations, and raw device free memory answer different questions (§6).
  None alone guarantees that a later SDK allocation will succeed.
- **CPU-reference shim CI (ADR-067).** `src/cuvs_wrapper_shim_cpu.c` lets the extension build and
  pass `make installcheck` with no GPU/CUDA, so every PR runs Tier-1 in CI; real-GPU Tier-2 runs
  on demand.
- **False-done prevention (bidirectional).** Both "claimed done but unimplemented" (ADR-009) and
  "claimed undone but actually wired" (3A/ADR-047, 3C/ADR-066) are real failure modes. Completion
  requires verification of the *normal* path, not just a grep that the code exists.
- **Soft LRU index cap (ADR-068).** The daemon registry is a working-set cap (`--max-indexes`,
  default 1024), not a hard wall: `load_index` evicts the LRU index to reserve a slot and reloads it
  on next use; a build over the cap defers gracefully. This is what makes online multi-tenant
  scaling work without a fixed `MAX_INDEXES` ceiling.
- **Build-overhead minimization (4A -- ADR-057/058/059).** memfd + `SCM_RIGHTS` corpus passing
  (leak-safe, zero-copy, peak RSS -32%); parallel maintenance workers for the build scan; daemon
  multi-partial direct H2D (drops the leader-side merge copy). Backend overhead ~6.3s -> ~3.7s; the
  GPU build floor still dominates wall-clock, so the value is the *backend-removal* north-star, not
  raw speedup.
- **Interruptible IPC (ADR-053).** The backend waits on the daemon reply via `recv_all_interruptible`
  (poll + `CHECK_FOR_INTERRUPTS`), so `statement_timeout` / cancel abort an in-flight GPU search
  (~0.5s) instead of hanging; the daemon ignores SIGPIPE to survive client mid-reply disconnects.

---

## 11. Boundaries and honest limitations

- **GPU-serving sidecars are outside PostgreSQL WAL.** CAGRA, flat, and IVF-PQ
  artifacts have their own persistence/freshness paths. `pg_cuvs_hnsw` instead
  imports PostgreSQL index pages and WAL-logs page writes for logged relations;
  its CPU search/maintenance delegates to pgvector. Do not apply the sidecar
  backup limitations to the HNSW relation's page storage.
- **The circuit breaker is process-local**, not server-wide (ADR-011 correction): each backend
  tracks and resets its own breaker; `pg_cuvs_reset_circuit` affects the calling session's view.
- **The default `index_dir` (`$PGDATA/cuvs_indexes`) sits inside the data directory**, so
  `pg_basebackup` copies the artifacts. A build-time WARNING and the ops playbooks recommend a
  sibling directory; the default is kept to avoid breaking existing setups.
- **The daemon has no catalog access**, so orphan artifacts (daemon-down DROP, DROP DATABASE) are
  reconciled by `pg_cuvs_gc_orphans()` / startup checks (ADR-046) rather than prevented.
- **Fallback is not a daemon-visible counter.** Plan-time CPU routing shows up only in
  `pg_stat_gpu_fallback`; treat it as a relative pressure signal (the cost hook can run more than
  once per query), not an exact query count.
