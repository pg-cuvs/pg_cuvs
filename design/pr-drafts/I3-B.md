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

## Boundary exception (hnsw export functions)

Cited from the brief's coordination appendix (conch commit 9c6fb4c1, ruling on
review BLOCK 1): "**경계 예외(2026-11-03 16:4x, I3-B 리뷰 BLOCK 1 판정):** I3-B는
I3-C 소유인 hnsw 내보내기 함수 두 곳(`handle_export_adjacency`,
`handle_export_hnsw_shm`)에 "in-use 표시(inflight++/--·재조회)와 `e->tids`를 락
안에서 복사"하는 안전 수정만 더할 수 있다(회수 스레드 해제와의 use-after-free
방지). 그 밖의 변경은 BLOCK. I3-B PR 본문이 이 문장을 인용한다. 03:00 스택에서
I3-C가 같은 함수를 고쳤으면 I3-B의 hunk를 기준으로 병합한다."

The I3-B diff in those two functions is limited to exactly that:
`handle_export_hnsw_shm` re-finds the entry under the lock, checks it is the same
handle, marks it in use (`inflight++`) across the unlocked serialize and clears it
after; `handle_export_adjacency` copies `e->tids` into a private buffer under the
lock (and frees it on every exit). Nothing else in either function changes.

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

Added from the independent review (REVIEW-I3-B-FABLE.md § 6, items 10-18):

10. `ensure_vram` signature: I3-A WIP has `(needed, dev, CuvsAdmissionFields *rej)`,
    I3-B has `(needed, dev, CuvsRequester rq)`. Merge to `(needed, dev, rq, rej)` and
    set `ensure_vram` to 4 in the `arity` table of `test/unit/check_i3b_server_source.py`;
    re-run CK-36 (a) and all I3-B mutations on the merged binary. Trial merge
    conflicts: server.c (~15 hunks), Makefile, reference.md, header/SQL.
11. KB-17 `--rmm-peak-meter on|off` is not implemented (the brief says the meter is
    always on); the startup INFO line of I3-A ("build admission model") should carry
    the I3-B knobs; this PR prints a separate `cache policy` line.
12. `pg_cuvs_memory_status()` columns owned by I3-A/I4 (`rejects_ar0..7`,
    `last_reject_*`, `quota`, `pool_used`, `pool_reserved`, `pending_rmm`,
    `builds_active`, `lw_pending`, `managed_*`) are appended by them: header string,
    NCOLS, SQL signature and the status-text test change together.
13. Resolved in r2: the I3-B block is in both 0.9.0 scripts.
14. Resolved: `pg_cuvs_build_status()` is outside I3-B (conch).
15. NOTICE (b) eviction notice text (`pg_cuvs: evicted index ... to make room`) is not
    produced by I3-B; the registry-full fallback NOTICE (d) is in I3-A WIP.
16. Item 1 detail: the NOTICE label is `index <oid>` in the daemon; the final text
    uses `quote_qualified_identifier(schema, name)` (B-7) and I3-A's notice_out
    encoding. Item 6 detail: `[admit] path=startup` / `path=warmup` lines are I3-A's.
17. AR5 `w`: the contract defines -1/0; KB-25 off's AR5 "seconds until the first
    index becomes eligible" is not computed (non-soft AR5 returns -1).
18. LOAD under quota pending: `ensure_vram` returns AR1 before soft eviction when
    `needed > quota - pending`; the classifier swap decides AR1 vs AR3/AR4.
19. `test/unit/test_admission_contract_mutations.py` (and the other files of I3-A's
    c8e6355/1c601f9, cherry-picked here unchanged) meet I3-A's own copies as add/add
    at the stack rebase: take I3-A's version. The upgrade script itself stays
    I3-A's file with its `-- I3-A begin/end` block followed by the I3-B block
    (only the earlier I3-B-only version of that file was dropped in r2).

Implementation-stage conditions from the review (not changed in this PR unless
noted): (1) KB-17 flag: implemented in r4 (`--rmm-peak-meter`, `peak_bytes`/`solo` in
`CuvsRmmPoolStats`, CK-34 test with 8 threads and three mutations; GPU: on gives
`peak_bytes` 15,360,000 after probes, off gives 0); r5 raises the peak only after a successful
allocation: on the VM, `test/i3b/gpu_peak_oom.c` (pool quota 1%, a 614 MB BF build
refused by the quota, then a 10 MB one) reports peak 0 after the refusal and
10,280,192 after the success on 864a649 (CUDA build 0 errors), while the same smoke
on c0d8993 (r4) fails with peak 614,400,000 after the refusal; the source scan
requires the commit after `upstream_.allocate` (mutation RED); (2) manual-op result strings differ from the C7
table (`not resident`/`already resident`/`not pinned`/`name = value` vs
`not_resident`/`already_resident`/`not_pinned`/`applied`): conch to fix the spelling;
(3) `pg_cuvs_unload` on a non-daemon AM (e.g. hnsw) answers "not resident" instead
of 22023; (4) r18 B-8 second channel (startup order source field, breaker
open/half-open/close log line) missing; (5) user-visible change:
`pg_cuvs_gpu_search_stats().last_search_at` is NULL for a never-searched index
(was the registration time), not yet in reference.md; (6) reference.md says the
REINDEX session receives a NOTICE, which needs I3-A's notice_out (0.9.0 stack);
(7) shim pending is always 0, so the r18 B-9 quota-pending branch, `budget_eff` with
a quota, DROP→F cleanup and in-use unload refusal are covered by unit/scan tests
only; scenario 7b covers the logical reservation path; (8) breaker mutation
evidence lives in the breaker commits (the reviewer re-ran 13: 12 RED, 1
equivalent); (9) isolation suite: reviewer ran 6/6 ok; (10) self-replace unloads
before re-admission (re-admission failure is reported, see self review);
(11) status enumeration under `g_index_mutex` and the 128-entry admit log are
follow-up items; (12) a pin that loads and then exceeds the cap stays loaded;
(13) resolved in r2; (14) CUDA build: see "CUDA compile".

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

Dev VM (RTX A6000, CUDA/cuVS 26.04 env `/work/_setup/env.sh`, lane `/work/i3-b`,
`/work/build-lane i3-b` = `make -j2 all server`):

- 396e1212 and 3ad5c86: **build OK**, 0 errors; `cuvs_wrapper.cu` (with `PeakUsage`)
  compiles; warnings are pre-existing (cuVS deprecations, one `%zu` at line 1914).
  Logs `/work/i3-b/.cuda-build-396e1212.log`, `.cuda-build-3ad5c86.log`.
- GPU daemon smoke (3ad5c86, private socket/dir, `.gpu-smoke.sh`): real RMM pool,
  `budget_eff` 25,448,644,608, `peak_bytes` 15,360,000 after the startup probes,
  ops 30–37 answer (status text, manual ops, runtime allow-list refusal), usage
  stats file written, SIGTERM exit 0 (reclaim thread joined).
- ASAN (`EXTRA_SERVER_CFLAGS=-fsanitize=address -fno-omit-frame-pointer`,
  `ASAN_OPTIONS=protect_shadow_gap=0:detect_leaks=0`): a clean rebuild **with `-g`**
  runs the same smoke with 0 ASAN reports and exit 0. **Open:** the same clean
  rebuild **without `-g`** loops `AddressSanitizer:DEADLYSIGNAL` before the first
  log line (reproduced twice), while d058dec6 built the same way starts normally.
  Root cause not found (no gdb on the VM, package installs not allowed); the
  difference lies between d058dec6 and 3ad5c86 and needs a bisect on the VM.

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
