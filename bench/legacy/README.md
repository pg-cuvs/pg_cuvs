# bench/legacy/ — 2026-06 anbench-generation harnesses

**Archive. Superseded — do not cite for new numbers.** These are the pre-cuvs-bench
harnesses. They are kept because `design/` and `BENCHMARK.md` still cite results produced
by them, and because the pgvectorscale / VectorChord competitor runners here have no
replacement yet.

Current-generation harnesses live one level up:
- `../cuvs_bench_backend/` — search/build via NVIDIA cuvs-bench (canonical)
- `../filter_recall/` — filtered-search measurement (ADR-082)
- `../protocol/` — the rigorous benchmark protocol + runners

## Why the numbers here are demoted

Two defects of that era are baked into the pg_cuvs rows (see `../../BENCHMARK.md`
Appendix A):

- **k was not wired to the GPU top-k** — pg_cuvs searched k=100 regardless of the
  requested k, so recall@10 came off a top-100 result while pgvector ran true k sweeps.
  Not iso-k.
- **`index_bytes` reported 0** for GPU-resident indexes (fixed in #73/#75).

The recall *method* in `common.py` / `anbench/anbench_common.py` is sound (exact
brute-force ground truth, `table id == corpus row index`, set-intersection recall@k). The
defects were in the extension of that era, not the harness.

## What is here

| Group | Files |
|-------|-------|
| Pilot orchestration | `run_pilot.sh`, `gen_dataset.py`, `gt.py`, `pctl.py`, `recall.py`, `common.py` |
| 50M / large-N | `bench_50m.sh`, `bench_1m1536.sh`, `gt_faiss.py`, `load_binary.py` |
| Competitor runners | `run_pgvectorscale.sh`, `run_vectorchord.sh` |
| Cohere real-embedding | `run_cohere.sh` (drives `anbench/`) |
| 3I / resource / MIG | `test_3i_bench.py`, `test_3i_restart.sh`, `test_gpu_resources.py`, `test_mig.sh` |
| Concurrency | `bf_microbatch_concurrency.sh`, `ef_recall_sweep.py` |
| `anbench/` | The Cohere pipeline (`run_all.sh`, `run_pg.py`, `run_cuvs.py`, `run_faiss.py`, `build_gt.py`, …), consolidated here from `infra/anbench/` |

## Why this is not split into subdirectories

The scripts call each other by `bench/legacy/<name>` path (`bench_50m.sh` → run_pilot /
run_pgvectorscale / run_vectorchord + helpers; `run_pilot.sh` → gen_dataset / gt / pctl /
recall; `run_cohere.sh` → `anbench/*`). Splitting them would mean rewriting dozens of
inter-script paths in dead archive code — pure regression risk with no upside. They stay
flat, quarantined under `legacy/`.
