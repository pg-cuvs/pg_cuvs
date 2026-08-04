# nsys profiling harness (#98 PR-E)

Scripts that produced the 2026-08 decomposition in
[`docs/experiments/profiling-results.md` §11](../../docs/experiments/profiling-results.md)
and BENCHMARK.md §1.1/§1.1a/§1.2. They run **on the GPU VM**, not locally.

| Script | Role |
|---|---|
| `nsys_search.sh <label> <none\|singles\|batch>` | Stop the systemd unit, relaunch the daemon under nsys with the unit's argv, run the workload, finalize with SIGTERM |
| `workload.py <none\|singles\|batch>` | The workload itself; also the unprofiled control |
| `nsys_build.sh <label> [extra nsys flags]` | Same, around one `CREATE INDEX ... USING cagra`, with epoch markers |
| `raw_arm.py` | Raw cuVS arm (build + singles + batch) for the cross-check |
| `analyze.py` | Baseline-subtracted kernel/memcpy totals for the three search captures |
| `build_aligned.py <sqlite> <epoch_before> <epoch_after>` | Build decomposition aligned to the CREATE INDEX window |
| `raw_window.py <search_start> <single_end> <batch_end>` | Raw-arm breakdown windowed by the harness's epoch marks |

## Things that will bite you

- **Finalize with SIGTERM to `pg_cuvs_server`.** `--duration` and SIGINT corrupt the
  report; in-process finalize on SIGTERM is the only path that has ever worked here.
- **No `LD_LIBRARY_PATH` needed.** The unit sets no `Environment=`, and the binary carries
  `RUNPATH=/opt/miniforge3/envs/cuvs_dev/lib`. Do not add conda lib dirs to ldconfig — it
  breaks sshd via an OpenSSL mismatch.
- **`chmod 666` the socket** after it appears; the unit's `ExecStartPost` normally does it
  and cross-uid shm access needs it.
- **Subtract the baseline.** The daemon loads resident indexes (GBs of H2D) and runs a
  warm-up build at startup; counting that as search cost inflates memcpy enormously.
- **Never compute the residual from a profiled wall.** nsys tracing inflates the daemon's
  single-search wall ~4.9× (4278.6 µs vs 882.1 µs). Take kernel/memcpy from nsys and the
  wall from an unprofiled run.
- **Drop profiling-only indexes while the daemon is UP,** or DROP-notify fails and leaves
  VRAM zombies (reclaim with `SELECT pg_cuvs_gc_orphans(true)`).
- Reports stay on the VM under `/tmp`; they are not committed.
