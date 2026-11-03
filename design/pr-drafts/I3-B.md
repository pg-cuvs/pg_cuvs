# PR I3-B: cache, reclaim, manual functions and circuit breaker (draft)

Stack: I1 → I2 (d058dec6) → I3-A → **I3-B** → I3-C(+D) → I4. This branch sits on
d058dec6 plus I3-A's shared commits cherry-picked unchanged: 36911f50 (admission
contract header), c8e6355 (estimate functions and the I4 workspace hook) and
1c601f9 (extension 0.8.2 → 0.9.0 with empty `-- I3-A begin/end` blocks). IPC version numbers follow the track-split decision
(`2026-11-02-i2b-track-split.md`): **I3 = 5, I2b = 6**. This PR does not bump
`CUVS_PROTO_VERSION`; I3-A does (once). New ops are appended in the I3-B range
**30–37** of 30–39.

Canonical inputs: `2026-11-03-set-code-task-briefs.md` (PR I3-B section and the
coordination appendix), the decision files `2026-11-03-uc1-cache-eviction.md`,
`-uc2-errors-circuit-breaker.md` (breaker part), `-uc4-13-recommended-values.md`
(UC-6 meter, UC-7, UC-12), `2026-11-02-i2b-track-split.md` (decision 4), plan v6 r18
C1/C6/C7/C8/C9 and conch rulings of 2026-11-03 (SQL EXECUTE rule 4e421f63, fault
tests owned by I3-A, DROP forgets F records).

## What changes for users

- **Builds never evict resident indexes** (default `--build-evict-scope none`),
  including the post-admission OOM retry: a build that does not fit is rejected
  instead of unloading other indexes. `idle:<min>` and `lru` restore eviction for
  bulk restores.
- **Reloads evict cold indexes first, then softly evict protected ones**
  (`reason=soft_evict`, `--load-soft-evict on`), also when another build's pending
  reservation is the cause; pinned and in-use indexes are never evicted.
- **REINDEX self-replace**: a resident CAGRA index whose rebuild fits only by
  counting its old copy unloads that copy (`self_replace=1`) and is admitted; never
  for pinned/in-use copies or with `--reindex-self-replace off`.
- **Background reclaim** thread (`--reclaim-mode off|usage|headroom`, default
  `headroom`): usage lines 85/75 % of `budget_eff`, plus the free target F from
  builds rejected for room in the last 30 min (cap 50 %, floor 10 % on the F side
  only, AR0/AR7 excluded, admitted or dropped indexes removed). Cold indexes only;
  CAGRA indexes with a delta/tombstone file and unsaved INSERTs are skipped
  (`skip_sidecar`).
- **Protection**: `P = 600 s + min(hits, 20) × 30 s`; a never-searched index is
  eligible; victims ordered by `max(last_use, loaded_at)`.
- **Usage stats file** `index_dir/.pg_cuvs_usage_stats` (atomic rewrite every tick,
  any mode); startup loads each pass hot-first from it, else OID order with the
  reason logged; startup never evicts.
- **Manual SQL functions**: `pg_cuvs_unload/load/pin/unpin(regclass)` (owner or
  superuser, EXECUTE PUBLIC), superuser-only `pg_cuvs_unload/unpin(oid, oid)`,
  `pg_cuvs_reclaim_set(name, value)` (superuser; allow-list incl. KB-10),
  `pg_cuvs_index_status()` (rows filtered by privilege), `pg_cuvs_memory_status()`
  (superuser/pg_monitor), `pg_cuvs_circuit_status()` (PUBLIC).
- **Circuit breaker**: transient reasons half-open after
  `cuvs.circuit_breaker_transient_ttl_s` (USERSET, default 30, 0–3600); permanent
  reasons (AR0, PROTO_MISMATCH, other counted statuses) keep manual reset; an open
  breaker at execution time (cached plan) raises **53200** instead of 0 rows.
- `n_extended`/compact counters reset on reload (defect 3); `file_current` (I-FC)
  makes eviction skip the save when the files are current.
- Always-on lock-free RMM pool peak meter (`peak_bytes` in memory status).

## Interfaces (append-only)

**IPC ops** (`src/cuvs_ipc.h`): 30 UNLOAD, 31 LOAD, 32 PIN, 33 UNPIN,
34 INDEX_STATUS, 35 MEMORY_STATUS, 36 RECLAIM_SET, 37 RECLAIM_STATUS. Same frame and
reply header as all ops (manual ops: result text in `error[]`; status ops:
length-prefixed text, header line checked by the backend).

**`SHOW cuvs.rmm_pool_status` keys appended by I3-B** (after I3-A's keys):
`build_evict_scope`, `pin_max_pct`, `reclaim_free_max_pct`,
`reclaim_free_window_min`, `reclaim_floor_pct`, `load_soft_evict`,
`reindex_self_replace`, `usage_stats_file`, `reclaim_params` (object per runtime
parameter: `value`, `source` default/flag/function, `changed_at`).

**Daemon flags** (KB-7–10, 12–15, 19–23, 25–27): `--build-evict-scope`,
`--evict-min-idle-s`, `--evict-hit-bonus-s`, `--evict-hit-cap`,
`--reclaim-high-pct`, `--reclaim-low-pct`, `--reclaim-tick-s`, `--pin-max-pct`,
`--reclaim-mode`, `--reclaim-free-bytes`, `--reclaim-free-max-pct`,
`--reclaim-free-window-min`, `--reclaim-floor-pct`, `--load-soft-evict`,
`--reindex-self-replace`, `--usage-stats-file`. Runtime path: `pg_cuvs_reclaim_set`
only. **SIGHUP re-reading is not implemented** (the daemon reads no config file).

**GUC**: `cuvs.circuit_breaker_transient_ttl_s` (KB-18).

**SQL**: one `-- I3-B begin/end` block appended after I3-A's block, identical in
`sql/pg_cuvs--0.9.0.sql` and `sql/pg_cuvs--0.8.2--0.9.0.sql` (checked by
`check_i3b_sql_perms.py`; `upgrade_path` passes). EXECUTE: unload/load/pin/unpin
PUBLIC with the owner check in C; the `(oid, oid)` overloads, `pg_cuvs_reclaim_set`
and `pg_cuvs_memory_status` revoked from PUBLIC (`pg_monitor` granted the last);
`pg_cuvs_index_status`/`pg_cuvs_circuit_status` PUBLIC.

**Logs**: `[evict]` and `[reclaim]` in the ADR-086 13-field schemas; `[self_replace]`
with the NOTICE text; `[startup]` load order and summary; `cache policy` startup
line with every effective knob.

## Integration points with I3-A (03:00 stack checklist)

1. **NOTICE delivery**: the self-replace NOTICE/DETAIL (and the build-failure
   variant) text is produced in `g_build_notice` and logged; it must be copied into
   I3-A's `notice_out` of the build reply.
2. **`[build_result]` fields** `self_replace`, `unloaded`: values exist
   (`note_build_admission` → eviction history → `pg_cuvs_index_status`
   `last_build_self_replace/unloaded`); append to I3-A's `[build_result]` line.
3. **Reason codes**: `ensure_vram` sets a provisional `g_last_admit_reason`
   (AR0 too big, AR1 quota/pending, AR2 pending physical, AR5 from the pick,
   AR3/AR4 short) used for the F log and manual-load messages. Replace with I3-A's
   classifier; keep feeding `note_build_admission`.
4. **`r` flag**: `cuvs_reclaim_wait_hint()` (pure) implements the contract header's
   rule; I3-A's err_out encoder should call it with the pick's eligible total.
5. **Breaker reasons**: the three record sites in `pg_cuvs.c` pass
   `CUVS_ADMISSION_NONE /* I3-A: pass decoded err_out reason */`; until then only
   `OOM_FALLBACK` is classified transient.
6. **`[admit] path=load`** lines for manual load/pin are I3-A's line; not written.
7. **`[build_peak]`**: switch its source to `cuvs_rmm_pool_peak_bytes/reset`.
8. **Fault suite**: `build_oom`, `build_multi_oom`, `build_oom_evict_to_fit` are RED
   on this tip by design (they expect evict-and-retry on build OOM); I3-A rewrites
   them as post-admission OOM = no eviction + status 2 + `[build_oom]` (conch ruling).
9. `test/expected/rmm_pool.out` may need the appended keys once both land (Tier-1
   passes on this tip: the shim reports the pool disabled).

## Tests (CPU only; commands and outputs in `I3-B-evidence/`)

| Area | Command | Result |
|------|---------|--------|
| Unit (all, macOS) | `make test-unit` | rc=0 — admission contract (C11 + C++17), policy 141, status text 80, util 357, corpus 27, circuit/sql/server/docs scans OK (`make-test-unit.txt`) |
| Unit (Linux image) | `make test-unit` in pgcuvs-i1-cpu | rc=0 (`make-test-unit-linux.txt`) |
| Shim integration (PG16 + shim daemon) | `sh test/i3b/run_i3b_shim.sh` in the image | 74 PASS, 0 FAIL (`shim-scenario.txt`) |
| Tier-1 regression | `sh test/i3b/run_tier1.sh` in the image | All 37 tests passed (`tier1.txt`) |
| max-indexes soft LRU | `run_tier1.sh maxidx` | ALL PASS (`max-indexes-scale.txt`) |
| Fault suite | `run_tier1.sh fault` | extend_cuda_oom ok; 3 RED by design (item 8) (`fault-suite.txt`) |

CK coverage on this tip: CK-9 (flags, runtime allow-list, knobs off), CK-10 (I3-B
lines: field order), CK-19/CK-27 (I3-B chapter wording and defaults vs source),
CK-23, CK-24 (pure plan, fences, floor, modes, sidecar kind, usage file),
CK-25/CK-26 (manual ops, permissions, visibility, history, counters, cross-session
`reclaim_params`), CK-32 (breaker TTL, 53200), CK-36 (a)(b)(e), CK-37 (a)(b)(c).

### Mutations (defect injected → RED)

| Suite | Command | Result |
|-------|---------|--------|
| Policy module | `python3 test/unit/mutate_cache_policy.py` | 38/38 RED (`mutate-cache-policy.txt`) |
| Status text / SQL permissions | `python3 test/unit/mutate_status_text.py` | 21/21 RED (incl. install/upgrade block divergence) |
| Server source scan (CK-36 (a), CK-37 (a)) | `python3 test/unit/mutate_i3b_server_source.py` | 9/9 RED |
| Docs (CK-19/27 parts) | `sh test/unit/mutate_i3b_docs.sh` | 6/6 RED |
| Daemon/backend via shim scenario | `python3 test/i3b/mutate_i3b_shim.py -j 6` | 18/18 RED on the final tree (`mutate-shim.txt`; an earlier run had 2 MISS, both checks were strengthened: `mutate-shim-first-run.txt`) |
| Circuit breaker | unit + `make test-circuit-source` | 7/7 RED (subagent run, commands in the breaker commits) |

Brief's mutation list mapping: four fences removed (policy F-1..F-4), floor blocking
usage reclaim (policy "floor applied to usage lines"), self-replace NOTICE missing
(shim), soft-evict reason missing (policy "soft victim labeled reload"), startup file
corruption fallback (policy parse + shim "startup ignores the usage file"), knobs off
not reproducing old behavior (shim, three knobs), stale `reclaim_params` in another
session (shim "SHOW caches reclaim_params per session"), no half-open after TTL
(breaker), cached plan 0 rows (breaker source scan).

## Self review (reference only; the official review is conch's Fable reviewer)

An independent code-reviewer subagent read the daemon diff: BLOCK 0, MAJOR 5,
MINOR 9. Fixed in `fix(daemon): address the I3-B correctness review`: partial
eviction before a later reject in `ensure_vram` (one pick for the larger
shortfall), eviction continuing after a failed save, self-replace re-admission
failure unreported, new registry slots inheriting `hnsw_idx`/`rev_tids` from the
shifted copy (wrong results, use-after-free), export paths using handle/TIDs
unlocked while the reclaim thread could free them; plus stale reject reason,
sharded admission not recorded for F, zero-byte victims, reclaim thread not joined
at shutdown, OOM-loop `requester_idx`, per-index `index_dir` treated as
file-current. Left as reported: `pg_cuvs_pin` that loads and then hits the pin cap
keeps the index loaded; heavy work under `g_index_mutex` (status enumeration,
reclaim saves) is a latency risk, not a semantic one; AR5 `w = -1` for time
protection is a contract question for I3-A. No CPU test reproduces the two
eviction-order races (needs fault injection between the checks) or the stale
`hnsw_idx` (needs a CPU-HNSW sidecar).

## CUDA compile

NOT_RUN in this worker (no GPU toolchain). The dev VM is announced; when access
arrives the bundle is built there with nvcc and the result recorded here.

## Not done / risks (explicit)

- **CUDA build of `PeakUsage` (cuvs_wrapper.cu) not compiled** in this worker (no GPU
  toolchain, no VM allowed). Written against RMM 26.04 `limiting_resource_adaptor`
  (fetched source). First GPU build must confirm.
- **`pg_cuvs_build_status()`** (plan C8) is not implemented: conch ruled it outside
  I3-B (an I4 integration item, handled at the stack integration).
- **SIGHUP re-read** not implemented (optional per UC-7 ruling; documented).
- **Pending-build soft reload** (r18 B-9) is implemented in `ensure_vram` but not
  exercised by a CPU test (needs a held build reservation).
- **Repeat suppression of all-sidecar `[reclaim]` lines** (CK-24 r10) is
  implemented coarsely (first tick, every 12th, set-size change) and not tested.
- **CK-13** (Q runner mock) lives in the bench tooling, not in this repo: not done here.
- **Warmup path** (LOAD_WARMUP eligible-only) is covered by the pure pick tests only;
  no GCS snapshot run.
- The F window keyed on `(db_oid, index_oid)`: a retried `CREATE INDEX` gets a new
  OID, so the old rejection keeps F at that level until the window passes (conch:
  within intent; documented).
