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

Usage (on the GPU VM, cuvs_bench env; daemon up; data in --data-dir):
    python bench/cuvs_bench_backend/run_pg_cuvsbench.py \
        --data-dir /home/ubuntu/anbench/data --n 1000000 \
        --algos pgcuvs_cagra,pgvector_hnsw --k 10 --max-queries 2000 \
        --out bench/results/pg_cuvsbench_1m.csv

Ground truth: gt_<n>.npy must exist in --data-dir (built by build_gt.py or
cuvs_bench.generate_groundtruth); the loader slices it to <max-queries> rows and
writes the .ibin cuvs-bench needs. Table t is (re)built to <n> rows on first use
and reused across algos/params.
"""
import argparse
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import backend as pg_backend  # noqa: E402  (registers PgBackend + PgConfigLoader)

from cuvs_bench.backends.base import BuildResult, SearchResult  # noqa: E402
from cuvs_bench.orchestrator.orchestrator import BenchmarkOrchestrator  # noqa: E402


CSV_FIELDS = ["algo", "param", "k", "recall", "qps", "search_time_ms",
              "p50_ms", "p95_ms", "p99_ms", "build_time_s", "index_bytes",
              "n_queries", "reused", "success", "error"]


def _rows(results):
    """Fold the orchestrator's flat [BuildResult|SearchResult, ...] list into one
    CSV row per search point, joining each search to its algo's most recent build."""
    build_by_algo = {}
    rows = []
    for r in results:
        if isinstance(r, BuildResult):
            build_by_algo[r.algorithm] = r
        elif isinstance(r, SearchResult):
            b = build_by_algo.get(r.algorithm)
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
                "n_queries": (r.metadata or {}).get("n_queries"),
                "reused": (b.metadata or {}).get("reused") if b else "",
                "success": r.success,
                "error": r.error_message or "",
            })
    return rows


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
    args = ap.parse_args()

    pg_backend.register("pg")

    # Reset build-reuse state so this run never inherits a prior run's index /
    # stale build_time (the sidecar persists in the temp dir across processes).
    import tempfile
    _sidecar = os.path.join(tempfile.gettempdir(), "pg_current_index.json")
    try:
        os.remove(_sidecar)
    except OSError:
        pass

    orch = BenchmarkOrchestrator(backend_type="pg")
    results = orch.run_benchmark(
        mode="sweep", build=True, search=True, count=args.k,
        dataset=args.dataset, dataset_path=args.data_dir, algorithms=args.algos,
        n=args.n, dbname=args.dbname, index_dir=args.index_dir,
        max_queries=args.max_queries,
    )

    rows = _rows(results)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        w.writeheader()
        w.writerows(rows)

    print(f"\n[cuvs-bench] {len(rows)} search points -> {args.out}")
    for r in rows:
        flag = "" if r["success"] else "  FAILED: " + r["error"]
        print(f"  {r['algo']:<20} param={r['param']:<5} k={r['k']} "
              f"recall={r['recall']:.4f} qps={r['qps']:.0f} "
              f"p50={r['p50_ms']}ms build={r['build_time_s']}s{flag}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
