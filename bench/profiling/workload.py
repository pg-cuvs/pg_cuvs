#!/usr/bin/env python3
"""PR-E nsys search workload. argv[1] = singles | batch | none.

Shapes match the #98 1M run's Pareto point (graph_degree=32,
intermediate_graph_degree=128, K=200) so the nsys decomposition describes the
same operation the benchmark reports.

Each mode runs in its own daemon/nsys session so the report's kernel and memcpy
totals belong to exactly one search shape. A separate `none` (startup-only)
baseline captures the 3.2 GB startup H2D index load, which is subtracted out.
"""
import os
import sys
import time

import numpy as np
import psycopg
import pgvector.psycopg
from pgvector import Vector

sys.path.insert(0, os.path.expanduser("~/pg_cuvs/bench/cuvs_bench_backend"))
from pg_engine import read_fbin, _vec_literal  # noqa: E402

DATA = os.path.expanduser("~/data")
K = 200
N_SINGLE = 300
BATCH_Q = 2000
INDEX_DIR = "/tmp/cuvs_indexes"

BATCH_SQL = ("SELECT b.query_idx, t.id, b.distance "
             "FROM pg_cuvs_batch_search('t'::regclass, %s::vector[], %s) b "
             "JOIN t ON t.ctid = b.ctid "
             "ORDER BY b.query_idx, b.distance")


def gpu_stat(cur):
    cur.execute(
        "SELECT index_name, search_count, avg_latency_us, p50_latency_us, "
        "p95_latency_us, p99_latency_us, error_count "
        "FROM pg_stat_gpu_search WHERE index_name = 't_cagra'")
    return cur.fetchall()


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "singles"
    if mode == "none":
        print("baseline: startup only, no workload", flush=True)
        return

    q = np.ascontiguousarray(read_fbin(os.path.join(DATA, "queries_10k.fbin")))
    print(f"mode={mode} queries={q.shape}", flush=True)

    with psycopg.connect("dbname=postgres", autocommit=True) as conn:
        pgvector.psycopg.register_vector(conn)
        with conn.cursor() as cur:
            cur.execute("SET enable_seqscan = off")
            cur.execute(f"SET cuvs.index_dir = '{INDEX_DIR}'")
            cur.execute(f"SET cuvs.k = {K}")
            print("stat_before:", gpu_stat(cur), flush=True)

            if mode == "singles":
                probe = ("SELECT id FROM t ORDER BY embedding <-> "
                         f"'{_vec_literal(q[0])}'::vector LIMIT {K}")
                cur.execute("EXPLAIN (FORMAT TEXT) " + probe)
                plan = "\n".join(r[0] for r in cur.fetchall())
                if "Seq Scan" in plan:
                    raise SystemExit("ABORT: planner chose Seq Scan\n" + plan)
                print("plan_ok:", plan.splitlines()[0], flush=True)

                for i in range(5):                    # warmup, not timed
                    cur.execute("SELECT id FROM t ORDER BY embedding <-> "
                                f"'{_vec_literal(q[i])}'::vector LIMIT {K}")
                    cur.fetchall()
                print("stat_after_warmup:", gpu_stat(cur), flush=True)
                print("MARK_WORKLOAD_START", time.time(), flush=True)

                lat = []
                for i in range(N_SINGLE):
                    t1 = time.perf_counter()
                    cur.execute("SELECT id FROM t ORDER BY embedding <-> "
                                f"'{_vec_literal(q[i])}'::vector LIMIT {K}")
                    cur.fetchall()
                    lat.append(time.perf_counter() - t1)
                print("MARK_WORKLOAD_END", time.time(), flush=True)
                lat = np.array(lat) * 1e6
                print(f"SINGLE n={N_SINGLE} client_mean_us={lat.mean():.1f} "
                      f"p50={np.percentile(lat,50):.1f} p95={np.percentile(lat,95):.1f} "
                      f"p99={np.percentile(lat,99):.1f}", flush=True)
                print("stat_after:", gpu_stat(cur), flush=True)

            elif mode == "batch":
                cur.execute("SET enable_hashjoin = off")
                cur.execute("SET cuvs.max_batch_queries = 4096")
                qb = [Vector(row) for row in q[:BATCH_Q]]

                cur.execute("EXPLAIN (FORMAT TEXT) " + BATCH_SQL, (qb, K))
                plan = "\n".join(r[0] for r in cur.fetchall())
                print("batch_plan_seqscan:", "Seq Scan" in plan, flush=True)

                cur.execute(BATCH_SQL, (qb, K))       # warmup dispatch
                cur.fetchall()
                print("stat_after_warmup:", gpu_stat(cur), flush=True)
                print("MARK_WORKLOAD_START", time.time(), flush=True)

                per = []
                for _ in range(5):
                    t1 = time.perf_counter()
                    cur.execute(BATCH_SQL, (qb, K))
                    rows = cur.fetchall()
                    per.append(time.perf_counter() - t1)
                print("MARK_WORKLOAD_END", time.time(), flush=True)
                per = np.array(per)
                print(f"BATCH Q={BATCH_Q} k={K} repeats=5 rows={len(rows)} "
                      f"median_s={np.median(per):.4f} min_s={per.min():.4f} "
                      f"max_s={per.max():.4f} "
                      f"qps_median={BATCH_Q/np.median(per):.1f}", flush=True)
                print("stat_after:", gpu_stat(cur), flush=True)


if __name__ == "__main__":
    main()
