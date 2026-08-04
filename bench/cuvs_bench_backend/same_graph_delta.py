#!/usr/bin/env python3
"""Batch-vs-single recall delta ON THE SAME GRAPH -- the #98 gate calibration.

Why this exists as a separate tool rather than a number in the harness.

`cuvs.k` feeds an AUTO kernel selection (cuvs_wrapper.cu:1116-1130 vs
:1211-1220), so a batch dispatch and a single-query scan can pick different
kernels and need not return identical neighbors. #98 therefore refuses to
hardcode a batch-vs-single recall tolerance and requires it measured.

Phase 2's own gate cannot supply that number. It compares its batch recall
against the Phase-1 latency row's recall, and Phase 2 generally REBUILDS to the
Pareto cell first (`P2 rebuild ... reason=not the Pareto cell`) -- so its delta
mixes "batch vs single" with "graph A vs graph B". Measured at N=100k, the
mixed delta was 0.0030 while the same-graph delta was 0.00065: the
graph-to-graph term is the larger of the two. A tolerance calibrated from the
mixed number would be measuring build nondeterminism, not kernel divergence.

Stage-1 same-graph measurements, both at N=100k / gd=32 / K=64, on two
different builds of that same cell:

    0.000650   (batch 0.981100 < single 0.981750)
    0.001000   (batch 0.982400 > single 0.981400)

The sign flips, so the gate compares |delta| (gate_batch_recall does). One
sample would have under-called it; the tolerance below is taken from the
LARGER delta, giving --batch-recall-tol = 0.002.

This script runs BOTH paths against whatever index is resident right now, so
the difference is only the thing being calibrated. Run it immediately after
run_pg_cuvsbench.py finishes, before anything rebuilds, and pass the Pareto
`cuvs.k` as --K (the batch row's `param` is `batch_k<K>`).

    gpu-run.sh run pg98delta -- python3 bench/cuvs_bench_backend/same_graph_delta.py \
        --data-dir ~/data --n 100000 --K 64

It prints the resident index's DDL alongside the delta: a calibration is only
meaningful with the graph it was taken on named next to it.
"""
import argparse
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pg_engine import PgEngine, read_fbin  # noqa: E402


def read_ibin(path):
    """big-ann .ibin (uint32 n, uint32 dim, int32 data) -> (n, dim) array."""
    with open(path, "rb") as f:
        n, d = struct.unpack("<II", f.read(8))
        return np.frombuffer(f.read(n * d * 4), dtype=np.int32).reshape(n, d)


def recall_at_k(nbr, gt, k):
    """recall@k against ground truth, both in corpus-row-index id space."""
    nq = min(len(nbr), len(gt))
    hits = sum(len(set(nbr[i][:k].tolist()) & set(gt[i][:k].tolist()))
               for i in range(nq))
    return hits / float(nq * k)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-dir", required=True)
    ap.add_argument("--n", type=int, required=True)
    ap.add_argument("--K", type=int, required=True,
                    help="Pareto cuvs.k -- the K the batch arm was measured at")
    ap.add_argument("--k", type=int, default=10, help="recall@k")
    ap.add_argument("--max-queries", type=int, default=2000)
    ap.add_argument("--dbname", default="postgres")
    ap.add_argument("--index-dir", default="/tmp/cuvs_indexes")
    args = ap.parse_args()

    q = np.ascontiguousarray(read_fbin(
        os.path.join(args.data_dir, "queries_10k.fbin")))[:args.max_queries]
    gt = read_ibin(os.path.join(
        args.data_dir, f"gt_{args.n}_q{args.max_queries}.ibin"))

    eng = PgEngine(dbname=args.dbname, index_dir=args.index_dir)
    try:
        with eng.conn.cursor() as cur:
            # Name the graph the number was taken on; without it the delta is
            # unattributable and cannot be defended as "same graph".
            cur.execute("SELECT pg_get_indexdef(oid) FROM pg_class "
                        "WHERE relname = 't_cagra'")
            row = cur.fetchone()
        if row is None:
            raise SystemExit("[delta] t_cagra is not resident -- nothing to "
                             "calibrate against. Run this straight after "
                             "run_pg_cuvsbench.py, before anything rebuilds.")
        print(f"[delta] resident graph: {row[0]}", flush=True)

        # Single-query path under the same knob (cuvs.k = K) the batch call uses.
        ids, lat = eng.search("pgcuvs_cagra", q, args.k, args.K)
        r_single = recall_at_k(np.asarray(ids[:, :args.k]), gt, args.k)
        print(f"[delta] single  K={args.K} recall@{args.k}={r_single:.6f} "
              f"qps={len(q)/sum(lat):.0f}", flush=True)

        bids, per_repeat, notes = eng.batch_search(
            q, args.K, top_k=args.k, warmup=2, repeats=3)
        r_batch = recall_at_k(np.asarray(bids), gt, args.k)
        med = sorted(per_repeat)[len(per_repeat) // 2]
        print(f"[delta] batch   K={args.K} recall@{args.k}={r_batch:.6f} "
              f"qps={len(q)/med:.0f} notes={notes}", flush=True)

        d = abs(r_batch - r_single)
        # x2 is headroom for run-to-run noise around a delta this small, and is
        # the rule #98 fixed in advance so the tolerance is not chosen after
        # seeing which value would pass.
        print(f"[delta] SAME-GRAPH DELTA = {d:.6f}  "
              f"proposed --batch-recall-tol = {d * 2:.6f}", flush=True)
    finally:
        eng.close()


if __name__ == "__main__":
    sys.exit(main())
