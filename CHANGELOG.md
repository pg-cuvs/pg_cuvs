# Changelog

All notable changes to pg_cuvs are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Version numbers and the
release procedure are described in [CONTRIBUTING.md](CONTRIBUTING.md#branches-and-releases).

This file starts after commit `2afe8b2b`, the source baseline of the 0.9.0
documentation. Earlier changes are in the git history. No tag or GitHub release
exists for 0.9.0.

## [Unreleased]

### Added

- `scripts/check-sdk-patch-path.sh` checks the patched cuVS SDK build path without CUDA:
  the patch set applies to the pinned cuVS source with the manifest hashes, the planner
  source verifies, and the Makefile passes the planner's compile and link flags. The
  Unit tests (CPU) CI job runs it. Development tooling only.
- A pull request template (`.github/pull_request_template.md`) with the checks to make
  before opening a pull request; [CONTRIBUTING.md](CONTRIBUTING.md#development) lists them.
- `cuvs.hnsw_exact_scan` (bool, default `off`, USERSET): with `on`, an ordered scan of a
  `pg_cuvs_hnsw` index computes exact SQL distances over the MVCC-visible heap rows
  instead of reading the HNSW graph, so rows the graph cannot reach (#203) are returned
  and the result matches Seq Scan + Sort. With the default `off` the scan is unchanged
  and #203 stays open: nothing recovers unreachable rows without the setting. A scan runs
  exactly or stops with SQLSTATE `0A000`; it never continues approximately. Statements
  with row-level security, a security barrier, an index expression or a partial index are
  planned as Seq Scan + Sort instead, so that security quals run before any distance
  function; a statement whose final plan would keep a `pg_cuvs_hnsw` scan that cannot run
  exactly is planned a second time, and planning side effects such as the `NOTICE` of an
  `IMMUTABLE` function on constants then happen twice. Cost: the whole heap is read even
  with a small `LIMIT`; the best candidates that fit in `work_mem` (16 bytes each) are
  kept, and a caller that reads more makes a second pass through temporary files. On one
  CPU container (PostgreSQL 16.15, pgvector 0.8.7), `LIMIT 10`, warm: 11 ms over
  100,000 x 96 (Seq Scan + top-N Sort 11 ms) and 284 ms over 1,000,000 x 96 (143 ms).
  Tested on PostgreSQL 16 CPU only, not on GPU builds, other pgvector versions or in
  GitHub Actions at the time of writing.

### Changed

- A pg_cuvs built without `CUVS_SDK_SOURCE_INCLUDE` no longer refuses ACE
  (`build_algo='ace'`, and `auto` when it spills to ACE). It hands the requested
  partition count to the patched cuVS SDK and records it in the HNSW receipt as
  requested only: `pg_cuvs_hnsw_build_info()` reports `ace_partitions_origin` as
  `requested_effective_not_guaranteed`, `ace_local_nn_descent_iterations` as
  `not_guaranteed` for two requested partitions from 1e6 rows, and `CREATE INDEX`
  raises a NOTICE that says how to turn the guarantee on; a GPU build prints one
  line about it when it compiles `src/cuvs_wrapper.cu` without the variable. A pg_cuvs built with
  `CUVS_SDK_SOURCE_INCLUDE` is unchanged: the effective count is computed twice,
  an unstable plan refuses the build, and the origin stays
  `same_SDK_stable_explicit_plan`. The patched `libcuvs.so` is required either way;
  a plain one is still refused before the build. The mark is a bit of the receipt's
  `route_flags` (receipt size and version unchanged, no SQL change); an earlier
  version's `pg_cuvs_hnsw_build_info()` rejects an index carrying it as an invalid
  receipt.

### Fixed

- A brute_force search on an unsharded index whose `.vectors` sidecar cannot serve it
  (row count or `.tids` generation mismatch, as after an extend that the daemon saved at
  shutdown, or defective content) read the whole sidecar and checked its body crc under
  the daemon's index lock on every search, so every other search and extend of that
  daemon waited (#238). The daemon now remembers what that read found: defective content
  (magic, version, range, reserved field, body crc, or a file shorter than its header
  says), or the header's row count, dimension, metric and `.tids` generation, together
  with the file's inode, size and nanosecond mtime. While the file is the same, a search decides from that record against
  the index's current row count, dimension, metric and `.tids` crc without reading the
  file, so INSERTs (extend) no longer cause a re-read; a new sidecar (REINDEX) is read
  again. The record does not depend on `cuvs.bf_precision`. `cuvs_vectors_read` now
  reports a truncated sidecar (shorter than its header's row count times dimension) as
  defective content, so it is remembered too. Allocation failures, a short header and
  read errors are not remembered, so the next search retries as before. When the cache
  cannot fit in VRAM, the search now fails before reading the file (not remembered).
  The result of each search (`NO_VECTORS` or a brute_force answer) is the same as before.
  Not changed: the sharded brute_force path (`refresh_shard_bf_caches`) still reads the
  whole sidecar on every search in these states; a missing `.tids` with no earlier record
  still reads the sidecar on every search; and the first failing search after a daemon
  start or a reload after eviction still reads the whole file under the lock (moving the
  read out of the lock is not part of this change).
- Documentation: the README batch example, `docs/reference.md` and ADR-040 said that
  joining the results of `pg_cuvs_batch_search` and `cuvs_filtered_knn` on `ctid` gives
  the visible rows. That join silently drops HOT-updated rows, because the functions
  return the root TID of the HOT chain (#243); the documents now say so and show the
  `CROSS JOIN LATERAL ... ORDER BY ... LIMIT` form, which follows HOT chains.
  `docs/reference.md` also notes the brute_force cache defects #237 and #239. The
  functions are unchanged.
- A GPU build without `CUVS_SDK_SOURCE_INCLUDE` failed to link with
  `ld: .../librapids_logger.so: error adding symbols: DSO missing from command line`.
  The wrapper expands `RMM_LOG_*`, which call `rapids_logger::logger::log`, and
  `libcuvs.so` and `librmm.so` need `librapids_logger.so` only indirectly, which `ld`
  does not follow; the Makefile named `-lrapids_logger` only inside the
  `CUVS_SDK_SOURCE_INCLUDE` branch. It is now a separate `CUVS_LOGGER_LINK` on the
  extension and daemon link lines of every GPU build (not the CPU shim), and a
  hand-written `CUVS_SO_LINK` / `CUVS_SERVER_LINK` keeps it. Seen on a GPU host on
  2026-11-15 with the planner off; the same link succeeded once the library was named.
- Building with the patched cuVS SDK planner (`CUVS_SDK_SOURCE_INCLUDE` set, as the
  HNSW/ACE paths require) failed to compile `src/cuvs_wrapper.cu`: the planner pulls in
  the patched `cagra_build.cuh`, whose RAFT `map_offset` kernel calls a constexpr host
  function from device code, and nvcc rejects that without `--expt-relaxed-constexpr`.
  The planner flags now pass it, as the SDK's own build does. Builds without the
  planner are unchanged. 0.9.1 and 0.9.2 have the same defect.

## [0.9.2] - 2026-11-15

Micro-batched index scans no longer fail with 64 or more concurrent clients

The GPU regression workflow (`gpu.yml`) was not run on `release/v0.9.2`. The fix
below was run on one GPU host: 1x A100-SXM4-80GB, 24 vCPU, PostgreSQL 16.15, cuVS
26.04 built with SDK patches 0001-0004, 0006 and 0007, 999,000 x 768 float32
vectors, L2, `LIMIT 10`, pgbench in prepared mode, 30 s per run, one run per client
count unless noted. With a `cagra` index, `cuvs.k = 100` and
`cuvs.cagra_batch_wait_us = 200`, the source of 0.9.1 plus the version bump
(`dfdce616`) aborted 14 and 11 of 64 clients (two runs) and 461 of 512; the source
of this release aborted none at 64, 128, 256 and 2048 clients. With a `flat` index,
`cuvs.k = 10` and `cuvs.bf_batch_wait_us = 200`, `dfdce616` aborted 4 of 64 clients
and this release none at 64 and 256.

### Fixed

- With a CAGRA or brute-force micro-batch window (`cuvs.cagra_batch_wait_us` or
  `cuvs.bf_batch_wait_us` > 0) and 64 pgbench clients, some statements failed with
  `GPU daemon unavailable after planning` while the daemon was running. A batch
  releases all its waiters at once, every request opens a new connection, and the
  daemon's listen backlog of 32 filled up. A backend whose `connect()` waited for a
  slot and received a signal failed with `EINTR`, because the socket's send timeout
  keeps the kernel from restarting the call. PostgreSQL 15 and later backends can receive
  `SIGALRM` while busy: the idle stats-flush timeout (10 s) is armed when a backend
  goes idle and is not disarmed when the next query starts (`postgres.c`, "we don't
  disable the timeout below"). Every `connect()` now waits in 250 ms slices and
  retries after a signal or an expired slice within the call's timeout (30 s for a
  search), and checks for a pending cancel or termination after each attempt.
  `cuvs_ipc_search`, which CAGRA and flat index scans use, then reports a canceled
  query; the other calls (filtered, streaming, transient brute-force, IVF-PQ and
  batch search, builds) end the connect as before with their connect-failure status.
  A connect that finally fails is logged with its `errno` (`[cuvs_ipc] connect to
  ... failed`; a missing or stale socket stays at debug level). The daemon listens
  with a backlog of 1024, which shortens the wait but is not the fix; the kernel caps
  it at `net.core.somaxconn`, and the daemon logs a warning at startup when that cap
  is below one batch (256). The scan errors no longer say that the breaker is open
  (`retry will use CPU while breaker is open`): the per-backend breaker opens only
  after `cuvs.circuit_breaker_threshold` consecutive failures, and the errors now say
  so. The UNAVAILABLE error reads `GPU daemon unavailable after planning; connecting
  to the daemon socket failed`, and its HINT no longer says that the daemon crashed.
  The protocol-mismatch error, which non-scan paths raise too, drops the breaker
  clause. When `make_shm_key` fails, the plain, batch, filtered, streaming
  brute-force and IVF-PQ search calls no longer pass an uninitialized name to
  `shm_unlink`. The retry is covered by a Linux unit test against a full
  backlog. (#224)

### Upgrading

- Build the release from its tag, install the extension and the daemon together
  (`make install` and `make install-server`), restart PostgreSQL and then the
  daemon, and run `ALTER EXTENSION pg_cuvs UPDATE TO '0.9.2';` in each database
  (`SELECT extversion FROM pg_extension WHERE extname = 'pg_cuvs'` shows `0.9.2`
  afterwards). The upgrade script changes no SQL object, and the IPC protocol is
  the one of 0.9.1. The connect retry is in the extension and the larger listen
  backlog is in the daemon. The steps and the restart order are in
  `docs/playbooks/release-upgrade.md`.

## [0.9.1] - 2026-11-15

Faster HNSW import, resident NN-descent builds, a WAL fix and two CAGRA SDK patches

The GPU regression workflow (`gpu.yml`) was not run on `release/v0.9.1`; the GPU
measurements below are the ones named in each entry.

### Added

- `pg_cuvs_hnsw_connectivity_status(regclass)` and a `connectivity` object in
  `pg_cuvs_hnsw_build_info()`: a snapshot, taken when an HNSW index is built, of
  structural reachability from the entry point, level-0 reachability and the number
  of weakly connected components. It is opt-in: `SET cuvs.hnsw_connectivity_check =
  on` (bool, default `off`, USERSET) before `CREATE INDEX` or `REINDEX`. A build with
  the setting off records `{"status":"disabled",...}`, which differs from
  `not_recorded` of an index built before the snapshot existed. It is a build
  snapshot, not live graph integrity: INSERT, DELETE, VACUUM and entry-point changes
  do not update it, structural reachability does not guarantee recall at a finite
  `ef_search`, and rows that the check finds unreachable are not recovered by search
  (#203 stays open for that). The check changes no edge, seed or search result, and a
  check that cannot finish does not fail the build. One that needs more than
  `maintenance_work_mem` continues in an unlinked file under `base/pgsql_tmp` and is
  unavailable under a finite `temp_file_limit`. Cost on one A100-SXM4-80GB over a
  999,000 x 768 fp32 heap with `build_algo='ivf_pq'`: `CREATE INDEX` took 25.41 to
  25.95 s with the setting off (five runs, median 25.60 s) and 27.45 to 27.74 s with
  it on (four runs, mean 27.59 s, of which the check is 2.26 to 2.28 s); the
  unchanged `main` took 25.60 to 25.70 s (four runs). recall@10 at `ef_search` 40 and
  160 was 0.9667 and 0.9948 off and 0.9675 and 0.9947 on; two builds of the same
  index differ from each other, so this is not a claim of identical results. With
  `build_algo='nn_descent'` the same setting applies: 34.46 s off, 36.56 s on, 34.32 s
  on unchanged `main`, one run each, recall not measured. A graph file larger than the
  page cache was not measured. Native GPU runs on four inputs of 1.0 to 2.2 million
  rows passed, made with the check always on before the setting existed, and were not
  repeated on the final code. (#208)
- `build_algo='nn_descent'` for `CREATE INDEX ... USING pg_cuvs_hnsw` builds a
  resident NN-descent base graph and never falls back to another builder. It rejects
  fewer than 16 rows, an effective intermediate degree above 1024, non-finite input
  and input whose conservative FP32 distance bound overflows. The base dtype is FP32
  when the dimension is 16 or less, an element exceeds the half range, or the
  resident IVF-PQ norm bound reaches 65504, and FP16 otherwise. L2 builds feed
  NN-descent a column-centered copy (mean accumulated in float64) so that a common
  offset does not destroy the neighbour order; stored vectors and search distances use
  the original rows. The build receipt is policy v3 with an NN tail, and the HNSW
  op 23 payload is v6: deploy the extension and the daemon together. GPU check, made
  after the merge message was written, on one A100 80GB PCIe VM: `CREATE INDEX ...
  USING pg_cuvs_hnsw (embedding vector_l2_ops) WITH (build_algo='nn_descent',
  ef_construction=64, mode='hnsw')` over a 999000 x 768 fp32 heap took 40.8 s (one
  run) and gave recall@10 0.7905, 0.9850 and 0.9953 at `ef_search` 10, 80 and 160;
  with `ef_construction=128` (one run, a profiler attached) 49.4 s and 0.7584,
  0.9812 and 0.9949, a lower recall at small `ef_search` than at 64 that is not
  explained yet (#209). The binary was built from commit `09de6aef` with a cuVS SDK
  of patches 0001 to 0007 (`libcuvs.so` SHA-256 `ceda5af3...`). That commit has the
  source files of the merged code (the head of the merged branch has the tree of the
  merge commit and differs from `09de6aef` in tests and documentation only); the
  merged commit itself was not rebuilt. (#211)
- SDK patch 0005 (resident NN-descent upper levels) is tracked in
  `infra/cuvs-patches`. The runtime marker that the daemon requires now ends in the
  SHA-256 of the three patched files, so for `build_algo='nn_descent'` with a GPU
  hierarchy an SDK build with the old marker is rejected and must be rebuilt. The
  recipe builds against a copy of the installed RAFT patched by `raft/0001` and with
  `BUILD_MG_ALGOS=ON` (`infra/cuvs-patches/prepare-raft-prefix.sh`,
  `scripts/prepare-patched-cuvs-source.sh`). (#211)
- The Tier 1 CI job runs `make test-hnsw-converter` (fixtures of the HNSW page
  writers, the node-location table and the source-less import). No workflow ran it
  before. (#210)
- The Tier 1 CI job runs `make test-k3-digest`: `CREATE INDEX` on a live backend
  against a scripted fake daemon (daemon failure, daemon `CANCELED`, success, a
  held RPC with `statement_timeout`, a client lost during the RPC), checking the
  digest LOG line count, the digest against SHA-256 of the inserted rows and the
  build error. The unit-test step has a 20 minute limit and the digest step 10. (#214)
- Unit tests: `test-hnsw-digest` (in `test-unit`); the read windows of the
  source-less import against direct reads under injected EIO, EINTR and short
  reads; idle-work cases in `test-hnsw-build-ipc` that are ordered by flags instead
  of sleeps and fail after 10 s with a named condition instead of hanging.
  (#214, #215)
- The CPU CI job runs `make test-source-checks`: the HNSW route source checks and
  their mutation scripts, which no workflow ran before. Two of the checks had gone
  stale on `main` (one expected an order of calls that a later change had renamed,
  one read a commit that a depth-1 checkout does not have) and were repaired, each
  with a planted defect that must turn it red. (#218)
- Release process: `CHANGELOG.md`; the "Branches and releases" rules in
  `CONTRIBUTING.md`; CI runs on `release/**` branches; the `Release draft` workflow
  creates a draft GitHub release from a `vX.Y.Z` tag and refuses a tag that is not
  on `release/vX.Y.Z` or whose version files or changelog section do not match
  (`scripts/release-check.sh`, `scripts/release-notes.sh`, tested by
  `test/release/test_release_scripts.sh`).
- CI refuses a pull request that edits, renames or deletes an SQL script of a
  version below `default_version`, and any SQL script change into a release branch
  (`scripts/check-sql-frozen.sh`, tested by `test/release/test_sql_frozen.sh`).

### Changed

- Extension version 0.9.1. `sql/pg_cuvs--0.9.1.sql` is the 0.9.0 install script plus
  `pg_cuvs_hnsw_connectivity_status()`, which `sql/pg_cuvs--0.9.0--0.9.1.sql` adds
  too and which is the only SQL object it changes; the two 0.9.0 scripts are
  frozen. Object-store manifests are stamped with the
  new `PG_CUVS_VERSION`; a manifest with another pg_cuvs version logs a version
  drift warning and loads, and only a cuVS version mismatch rejects an artifact.
  `make docs-contract-check` reads the version from `pg_cuvs.control` and checks
  that every `sql/pg_cuvs--*.sql` is in the Makefile `DATA` list. (#219)
- Source-less HNSW import (`CREATE INDEX ... USING pg_cuvs_hnsw`), four changes
  that leave pages, page order and WAL records as they were. Timings are from one
  A100-SXM4-80GB VM (24 vCPU, 113 GiB RAM) with `build_algo='ivf_pq'`,
  `ef_construction=128`, `shared_buffers` 32GB, `maintenance_work_mem` 8GB and 11
  parallel maintenance workers, over a 999000 x 768 fp32 heap, with cuVS 26.04
  patches 0001 to 0004, 0006 and an earlier form of 0007 held fixed (the absolute
  times need that SDK). Medians of three runs with min to max in parentheses,
  except where one run is stated; the daemon and PostgreSQL were started fresh for
  every run. On the 10,000 official queries recall@10 stayed in the run-to-run range
  (0.966 to 0.968 at `ef_search` 40, 0.994 to 0.995 at 160).
  - (#210) Node locations that follow the regular page layout are computed instead of read
    back through a four-page cache. Replaying the access order of the import gave
    64,935,002 lookups with 56,497,735 cache misses, about 462 GB of 8 KB read
    requests against a table of about 8 MB. One run on a diagnostic build: import
    of the data pages 48.57 s to 11.25 s, `CREATE INDEX` 81.67 s to 44.27 s, the same
    3,588,907,769 bytes of WAL. Any row that does not match the layout turns the
    computation off for that table and the old path serves it; for graphs with
    hierarchy the computation switched off, so they take the old path and gain
    nothing from this change.
  - (#213) Data pages are added to the relation in groups of up to 32 buffers
    (`ExtendBufferedRelBy()`, PostgreSQL 16 and later) instead of one block per
    call. `CREATE INDEX` 33.150 s (32.77 to 33.36) to 30.004 s (29.94 to 30.08); the
    import after the daemon reply 14.105 s to 11.130 s. A batch holds up to 32 extra
    buffer pins. Not run on a PostgreSQL build with assertions, not through crash
    recovery.
  - (#214) The input digest (the SHA-256 LOG line that binds a build to its input) is
    computed from the vectors spool file while the backend waits for the daemon,
    not inside the heap scan. Against the code before the read windows below:
    `CREATE INDEX` 30.145 s (29.99 to 30.77) to 28.145 s (27.99 to 28.70); heap scan,
    spool and preflight 8.982 s to 6.882 s; the daemon job did not change. The
    value and the text of the LOG line are unchanged, but it is now printed after
    the daemon reply, an interrupt or an error before the RPC no longer prints it,
    and if rereading the spool fails after a successful daemon build `CREATE INDEX`
    aborts and the GPU build time is lost. The gain depends on the CPU (about 2 s
    here, on a VM with SHA instructions).
  - (#215) Level-0 fields, upper-level records and TIDs are read through fixed 1 MiB
    windows instead of one `pread` each. Against the same baseline: `CREATE INDEX`
    27.739 s (27.58 to 27.94), together with the digest change 25.608 s (25.58 to
    25.74); `pread` calls of the backend 9,991,134 to 7,537; bytes read by the backend
    3.64 GB to 6.73 GB (9.80 GB with the digest change).
- SDK patch 0007 (CAGRA prune, #216): at row degree 256 with 32-bit IDs the rank scan of
  `kern_prune` is replaced by a binary search on a sorted index in shared memory;
  every other degree keeps the old kernel. On the A100-SXM4-80GB, standalone
  `cagra::build` of a 999000 x 768 corpus, median of three interleaved runs: 15.60 s
  without the prune change, 8.92 s with this patch (9.15 s with an earlier form).
  Byte equality of the detour counts with the unpatched kernel in 52 cases and
  `compute-sanitizer` on a small suite were checked. An end-to-end `CREATE INDEX`
  with exactly this patch and an nsys trace of the new kernel were not.
- Project skills for coding agents moved from `.claude/skills` to `.agents/skills`;
  `.claude/skills/<name>` are relative symlinks. Development tooling only. (#217)

### Fixed

- The connectivity check of #208 answers PostgreSQL's ordinary interrupts (such as a
  memory-context log request) with `CHECK_FOR_INTERRUPTS()` instead of treating any
  pending interrupt as a cancel, which had discarded a valid observation as
  `unavailable`; a real cancel, timeout or backend termination returns PostgreSQL's
  error. `posix_fallocate` of its file workspace retries on `EINTR` with the backend's
  signals blocked, and the workspace is released on error paths. The earlier
  behaviour was in the revision of that change before it merged, not in a release.
  (#208)
- On macOS the backend closed its copy of the HNSW build owner descriptor right
  after sending it with `SCM_RIGHTS`; while the message was in flight the daemon
  could read EOF and treat a live build as cancelled (`test-hnsw-build-ipc` failed
  in about 5% of runs at that commit and 23% at `ae818ad0`; none in Linux 6.8 arm64
  runs). The backend now keeps the descriptor until the reply. (#211)
- The HNSW page writers (#212; `write_elem_page()`, `write_elem_pages_packed()`, both
  metapage writers and the source-less publisher) changed a shared buffer, called
  `MarkBufferDirty()` and wrote the WAL record outside one critical section, which
  PostgreSQL requires, and built page contents that can `ereport` directly in the
  locked buffer. Pages are now built in private memory and published through one
  helper that runs the copy, `MarkBufferDirty()` and `log_newpage_buffer()` inside
  `START_CRIT_SECTION()`/`END_CRIT_SECTION()`; the unit fixtures assert it. No
  corruption or crash caused by this was observed. The change was not run on a
  PostgreSQL build with assertions, and replaying the WAL it writes was not tested.
- SDK patch 0006 (#216): with refinement disabled, the CAGRA kNN-graph builder passed IVF-PQ
  search rows to the graph writer and the pruning step, which read each position as
  the neighbour's rank, but the select returns a different order on its warp-select
  path (K up to 256) than on its radix path (K above 256). A build with intermediate
  degree 256 (K = 257) consumed unordered rows. The patch sorts each row; the
  recipe also applies the RAFT `select_k` key bit width patch that the validated
  binaries used. On a 999000 x 768 fp32 corpus with intermediate degree 256,
  recall@10 at `ef_search` 10 over 10,000 queries went from 0.53653 to 0.84139 in
  one build configuration and from 0.62018 to 0.93995 in another.

### Upgrading

- Build the release from its tag, install the extension and the daemon together
  (`make install` and `make install-server`), restart PostgreSQL and then the
  daemon, and run `ALTER EXTENSION pg_cuvs UPDATE TO '0.9.1';` in each database
  (`SELECT extversion FROM pg_extension WHERE extname = 'pg_cuvs'` shows `0.9.1`
  afterwards). The upgrade script adds one function, `pg_cuvs_hnsw_connectivity_status(regclass)`. The steps and the
  restart order are in `docs/playbooks/release-upgrade.md`. The HNSW op 23 payload
  is v6, so an extension and a daemon of different builds do not work together.
- 0.9.0 was never tagged and its scripts changed in place until this release. A
  database that got `pg_cuvs` 0.9.0 from a development build may hold a catalog
  that differs from the final 0.9.0 scripts, and `ALTER EXTENSION ... UPDATE`
  does not repair it: a database installed from the first 0.9.0 commit
  (`4e5d25f3`) and updated to 0.9.1 still lacked 12 of the 35 `pg_cuvs*` functions
  that a fresh 0.9.1 database has. For such a database, note the definitions of its
  pg_cuvs indexes (`SELECT indexdef FROM pg_indexes WHERE indexdef ~ 'USING (cagra|flat|ivfpq|pg_cuvs_hnsw)'`),
  run `DROP EXTENSION pg_cuvs CASCADE` (it drops those indexes), `CREATE EXTENSION
  pg_cuvs`, and run the noted `CREATE INDEX` statements again, instead of updating
  in place. The same sequence on that database gave a function list equal to the
  fresh database's and a `cagra` index that the planner used (checked on the CPU
  reference build, PostgreSQL 16.15).
- The cuVS SDK is built from `infra/cuvs-patches`. With `build_algo='nn_descent'`
  and a GPU hierarchy (`mode='hnsw'`) the daemon refuses the build unless the
  library exports the marker of patch 0005 (`src/cuvs_wrapper.cu`, the
  `hnsw_sdk_marker_verified("pgcuvs_hnsw_resident_nn_upper_patch_id", ...)` check).
  Patch 0005 itself refuses an upper level only when it does not fit the base FP16
  precision (the dimension is 16 or less, or a value is non-finite or beyond the half
  range: `RAFT_EXPECTS(dim > 16 && !nn_input_needs_fp32(...))` in
  `infra/cuvs-patches/0005-hnsw-resident-nn-upper.patch`) or, for L2, when non-finite
  values prevent centering. Other build algorithms are unchanged. To get the CAGRA
  row-order fix (0006) and the faster prune (0007), rebuild the SDK with
  `infra/cuvs-patches/build-patched-cuvs.sh`.
