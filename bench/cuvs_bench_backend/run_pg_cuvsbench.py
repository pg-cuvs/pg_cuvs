#!/usr/bin/env python3
"""
run_pg_cuvsbench.py -- drive the pg_cuvs/pgvector cuvs-bench backend end to end.

This is the modern cuvs-bench entrypoint: register the pg backend, then use
BenchmarkOrchestrator(backend_type="pg").run_benchmark(...) -- the same
orchestrator, Dataset, IndexConfig, BuildResult/SearchResult, and compute_recall
that cuvs-bench's own backends use. The module-level cuvs_bench.run.run() is
deprecated upstream in favour of exactly this call.

It proves pg_cuvs runs *inside NVIDIA's cuvs-bench* and emits a native
Google-Benchmark-shaped CSV (items_per_second, Recall, real_time, p50/p95/p99)
for the Pareto plot -- the ecosystem-entry Stage-2 artifact.

#98 shape -- one process, one set of indexes, one CSV, two phases:

  Phase 1 (latency axis, through the orchestrator)
    run_benchmark() is called ONCE PER BUILD CONFIG, and each call sweeps that
    build's search params. A build and its search points therefore share a call,
    which is what makes the (algo, build_params) join in the CSV a fact rather
    than a reconstruction. Rows are appended and flushed after every call, so a
    crash costs one segment, not the run.

  Phase 2 (throughput axis, outside the orchestrator)   -- PR-C
    the same process picks each algo's Pareto point from the Phase-1 rows and
    calls the batch / concurrent / raw arms directly, appending to the same CSV.
    The orchestrator never learns those algo names (DEFAULT_SWEEPS[algo] would
    KeyError); see run_phase2() below.

Progress is a live stream, not a post-mortem: every meaningful event prints one
`[pg98] …` line with flush=True (gpu-run.sh redirects stdout to the run log, so
`gpu-run.sh log -f` follows it), and a heartbeat covers the long silent stretches
(big builds, COPY) so silence never has to be interpreted.

Usage (on the GPU VM, cuvs_bench env; daemon up; data in --data-dir):
    python bench/cuvs_bench_backend/run_pg_cuvsbench.py \
        --data-dir /home/ubuntu/anbench/data --n 1000000 \
        --algos pgcuvs_cagra,pgvector_hnsw --k 10 --max-queries 2000 \
        --out bench/results/pg_cuvsbench_98.csv [--resume]

Ground truth: gt_<n>.npy must exist in --data-dir (built by build_gt.py or
cuvs_bench.generate_groundtruth); the loader slices it to <max-queries> rows and
writes the .ibin cuvs-bench needs. Table t is (re)built to <n> rows on first use
and reused across algos/params.
"""
import argparse
import csv
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pg_engine import DEFAULT_SWEEPS  # noqa: E402
from sidecar import (  # noqa: E402
    BUILD_GRIDS,
    canonical_json,
    completed_keys,
    pareto_point,
    remaining_params,
)

# `reused` predates #98 -- build_params and axis are the new columns.
CSV_FIELDS = ["algo", "param", "k", "recall", "qps", "search_time_ms",
              "p50_ms", "p95_ms", "p99_ms", "build_time_s", "index_bytes",
              "n_queries", "build_params", "axis", "reused", "success", "error",
              "notes"]

RECALL_FLOOR = 0.95      # Pareto: the recall a throughput point must clear
RECALL_SANITY = (0.0, 1.0000001)   # anything outside is a broken measurement


# ── live stream ──────────────────────────────────────────────────────────────
def log(stage, event, *parts):
    """One event, one line, flushed. `[pg98] <stage> <event> …`"""
    msg = " ".join(str(p) for p in parts)
    print(f"[pg98] {stage} {event} {msg}".rstrip(), flush=True)
    Heartbeat.touch()


class Heartbeat:
    """Prints `[pg98] hb <what> elapsed=…s` every 30s while nothing else does.

    A long build or COPY otherwise leaves the log silent for minutes, and a
    silent log cannot be told apart from a dead job -- which is the exact
    ambiguity gpu-run.sh exists to remove.
    """

    _last = time.time()
    _lock = threading.Lock()

    def __init__(self, what, interval=30.0, quiet_after=60.0):
        self.what = what
        self.interval = interval
        self.quiet_after = quiet_after
        self._stop = threading.Event()
        self._t0 = time.time()
        self._thread = threading.Thread(target=self._run, daemon=True)

    @classmethod
    def touch(cls):
        with cls._lock:
            cls._last = time.time()

    def _run(self):
        while not self._stop.wait(self.interval):
            with Heartbeat._lock:
                silent = time.time() - Heartbeat._last
            if silent >= self.quiet_after:
                print(f"[pg98] hb {self.what} elapsed={time.time()-self._t0:.0f}s",
                      flush=True)

    def __enter__(self):
        self._thread.start()
        return self

    def __exit__(self, *exc):
        self._stop.set()
        return False


# ── CSV ──────────────────────────────────────────────────────────────────────
def read_existing(path):
    """Rows already in `path` (empty when it does not exist)."""
    if not os.path.exists(path):
        return []
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def open_csv(path, resume):
    """Open the CSV for append (resume) or fresh write. Returns (file, writer)."""
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    existed = os.path.exists(path) and os.path.getsize(path) > 0
    f = open(path, "a" if (resume and existed) else "w", newline="")
    w = csv.DictWriter(f, fieldnames=CSV_FIELDS, extrasaction="ignore")
    if not (resume and existed):
        w.writeheader()
        f.flush()
    return f, w


def _rows(results, axis="latency"):
    """Fold the orchestrator's flat [BuildResult|SearchResult, …] list into one
    CSV row per search point.

    The join key is (algo, build_params), NOT algo alone: with a build grid,
    "the algo's most recent build" would attach every sweep point to whichever
    build happened to run last.
    """
    from cuvs_bench.backends.base import BuildResult, SearchResult

    builds, rows = {}, []
    for r in results:
        if isinstance(r, BuildResult):
            builds[(r.algorithm, canonical_json(r.build_params))] = r
        elif isinstance(r, SearchResult):
            md = r.metadata or {}
            bp = md.get("build_params", "{}")
            b = builds.get((r.algorithm, bp))
            lp = r.latency_percentiles or {}
            sp = r.search_params[0] if r.search_params else {}
            rows.append({
                "algo": r.algorithm,
                "param": sp.get("param"),
                "k": sp.get("k"),
                "recall": round(r.recall, 4),
                "qps": round(r.queries_per_second, 1),
                "search_time_ms": round(r.search_time_ms, 3),
                "p50_ms": round(lp.get("p50_ms", float("nan")), 3),
                "p95_ms": round(lp.get("p95_ms", float("nan")), 3),
                "p99_ms": round(lp.get("p99_ms", float("nan")), 3),
                "build_time_s": round(b.build_time_seconds, 3) if b else "",
                "index_bytes": b.index_size_bytes if b else "",
                "n_queries": md.get("n_queries"),
                "build_params": bp,
                "axis": md.get("axis", axis),
                "reused": (b.metadata or {}).get("reused") if b else "",
                "success": r.success,
                "error": r.error_message or "",
                "notes": "",
            })
    return rows


# ── segment validation ───────────────────────────────────────────────────────
class SegmentError(RuntimeError):
    """A segment produced rows that must not enter the CSV's conclusions."""


def validate_segment(rows, expected_n, algo, build_params, recall_range=RECALL_SANITY):
    """Check one build_cfg's rows the moment they land, and abort on violation.

    Carrying on past a bad segment accumulates contamination that is only
    discovered at the end, when the run has already cost hours -- so this raises
    instead of warning. Returns a short human summary for the log line.
    """
    if len(rows) != expected_n:
        raise SegmentError(
            f"{algo} {build_params}: {len(rows)} rows, expected {expected_n}")
    bad = [r for r in rows if not r["success"]]
    if bad:
        raise SegmentError(
            f"{algo} {build_params}: {len(bad)} failed rows; first error: "
            + str(bad[0]["error"]))
    lo, hi = recall_range
    off = [r for r in rows if not (lo <= float(r["recall"]) <= hi)]
    if off:
        raise SegmentError(
            f"{algo} {build_params}: recall outside {recall_range}: "
            + ", ".join(f"param={r['param']} recall={r['recall']}" for r in off))
    # Exactly one build per segment: every row must name the same build, and
    # the reuse flag must be coherent across them (a segment that is half
    # reused and half rebuilt means the index changed mid-sweep).
    keys = {r["build_params"] for r in rows}
    if keys != {build_params}:
        raise SegmentError(
            f"{algo}: rows carry build_params {sorted(keys)}, expected {build_params}")
    reused = {str(r["reused"]) for r in rows}
    if len(reused) != 1:
        raise SegmentError(f"{algo} {build_params}: mixed reused flags {sorted(reused)}")
    return (f"rows={len(rows)} reused={reused.pop()} "
            f"recall=[{min(float(r['recall']) for r in rows):.4f},"
            f"{max(float(r['recall']) for r in rows):.4f}]")


# ── Phase 2 (PR-C) ───────────────────────────────────────────────────────────
def select_pareto(rows, algos, recall_floor=RECALL_FLOOR):
    """The throughput-axis operating point per algo, from the latency rows.

    Returns {algo: {"param":…, "build_params":…, "recall":…, "qps":…,
                    "fallback": bool}}. `fallback` marks an algo where no point
    cleared the floor and the highest-recall point was taken instead -- the
    report must say so rather than presenting it as a recall>=floor point.
    """
    out = {}
    for algo in algos:
        row, fallback = pareto_point(rows, algo, recall_floor)
        if row is None:
            continue
        out[algo] = {"param": row["param"], "build_params": row["build_params"],
                     "recall": float(row["recall"]), "qps": float(row["qps"]),
                     "fallback": fallback}
    return out


def run_phase2(args, writer, fout, latency_rows):
    """Throughput axis -- implemented in PR-C.

    The hook exists here in PR-A so the two-phase control flow (and its Pareto
    input) is settled before the arms land: Phase 2 must run in the SAME process
    with the Phase-1 indexes still resident, which is a property of where it is
    called from, not of what it does.
    """
    pareto = select_pareto(latency_rows, sorted({r["algo"] for r in latency_rows}))
    for algo, p in sorted(pareto.items()):
        log("P2", "pareto", algo, f"param={p['param']}",
            f"build={p['build_params']}", f"recall={p['recall']:.4f}",
            f"qps={p['qps']:.0f}", f"fallback={p['fallback']}")
    log("P2", "skipped", "throughput arms land in PR-C")
    return []


# ── main ─────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-dir", required=True)
    ap.add_argument("--n", type=int, required=True)
    ap.add_argument("--algos", default="pgcuvs_cagra,pgvector_hnsw")
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--max-queries", type=int, default=2000)
    ap.add_argument("--dataset", default="cohere-wiki-en-1024")
    ap.add_argument("--dbname", default="postgres")
    ap.add_argument("--index-dir", default="/tmp/cuvs_indexes")
    ap.add_argument("--out", required=True)
    ap.add_argument("--resume", action="store_true",
                    help="append to --out, skipping (algo, build_params, param) "
                         "points it already records as successful")
    ap.add_argument("--phase2", action="store_true",
                    help="run the throughput axis after Phase 1 (PR-C)")
    args = ap.parse_args()

    import backend as pg_backend       # imports cuvs_bench; keep it out of
    from cuvs_bench.orchestrator.orchestrator import (  # module import so the
        BenchmarkOrchestrator)         # pure helpers stay CPU-testable
    pg_backend.register("pg")

    algos = [a.strip() for a in args.algos.split(",") if a.strip()]
    prior = read_existing(args.out) if args.resume else []
    done = completed_keys(prior)
    if args.resume:
        log("S0", "resume", f"csv={args.out}", f"rows={len(prior)}",
            f"completed={len(done)}")
    else:
        # A fresh run must not inherit a previous run's indexes or build times.
        import tempfile
        try:
            os.remove(os.path.join(tempfile.gettempdir(), "pg_current_index.json"))
        except OSError:
            pass

    segments = []
    for algo in algos:
        for cell in BUILD_GRIDS[algo]:
            sweep = [p for p in DEFAULT_SWEEPS[algo]
                     if not (algo in ("pgvector_hnsw", "pgcuvs_hnsw_import")
                             and p < args.k)]
            todo = remaining_params(done, algo, cell, sweep)
            if todo:
                segments.append((algo, cell, todo))
            else:
                log("S1", "skip", algo, canonical_json(cell), "(already complete)")

    total = sum(len(t) for _, _, t in segments)
    log("S1", "plan", f"segments={len(segments)}", f"search-points={total}")

    fout, writer = open_csv(args.out, args.resume)
    all_rows = list(prior)
    done_pts = 0
    try:
        for algo, cell, todo in segments:
            cell_key = canonical_json(cell)
            log("S1", "segment", algo, cell_key, f"params={todo}")
            with Heartbeat(f"{algo} {cell_key}"):
                t0 = time.perf_counter()
                orch = BenchmarkOrchestrator(backend_type="pg")
                results = orch.run_benchmark(
                    mode="sweep", build=True, search=True, count=args.k,
                    dataset=args.dataset, dataset_path=args.data_dir,
                    algorithms=algo, n=args.n, dbname=args.dbname,
                    index_dir=args.index_dir, max_queries=args.max_queries,
                    build_cfg=cell, params=todo,
                )
            rows = _rows(results)
            summary = validate_segment(rows, len(todo), algo, cell_key)
            writer.writerows(rows)
            fout.flush()
            all_rows.extend(rows)
            for r in rows:
                done_pts += 1
                log("S1", "search", r["algo"], cell_key, f"param={r['param']}",
                    f"recall={float(r['recall']):.4f}", f"qps={float(r['qps']):.0f}",
                    f"p50={r['p50_ms']}ms", f"[{done_pts}/{total}]")
            log("S1", "gate", "segment-ok", algo, cell_key, summary,
                f"build={rows[0]['build_time_s']}s",
                f"{time.perf_counter()-t0:.1f}s")
    except SegmentError as e:
        log("S1", "ABORT", "gate-failed", str(e))
        fout.close()
        return 2
    except Exception as e:  # noqa: BLE001
        log("S1", "ABORT", "exception", repr(e))
        fout.close()
        raise

    if args.phase2:
        for r in run_phase2(args, writer, fout, all_rows):
            writer.writerow(r)
            fout.flush()
    fout.close()

    log("S1", "done", f"points={done_pts}", f"-> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
