#!/usr/bin/env python3
"""
run_cuvs.py - raw cuvs CAGRA benchmark (Tier B, GPU library ceiling). Same
algorithm/params as pg_cuvs (graph_degree=64, intermediate=128, L2/sqeuclidean
on unit-norm vectors == cosine). No PostgreSQL, no IPC. Reports BOTH batched
QPS and single-query p50/p95/p99 so it can be compared with the SQL tiers.

Env: cuvs_py. Reads first N rows of the corpus + queries + GT.
"""
import argparse
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from anbench_common import (read_fbin, recall_at_k, percentiles_ms,  # noqa: E402
                            gpu_mem_used_mb, emit_result)


def log(m):
    print(f"[cuvs] {m}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--queries", required=True)
    ap.add_argument("--gt", required=True)
    ap.add_argument("--n", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--dataset", default="cohere-wiki-en-1024")
    ap.add_argument("--ks", default="10,100")
    ap.add_argument("--itopk", default="32,64,96,128,256,512")
    ap.add_argument("--search-width", default="1,2,4")
    ap.add_argument("--build-algo", default="ivf_pq", choices=["ivf_pq", "nn_descent"])
    ap.add_argument("--single-query-n", type=int, default=2000,
                    help="how many queries to time one-at-a-time for percentiles")
    args = ap.parse_args()

    import cupy as cp
    from cuvs.neighbors import cagra

    ks = [int(x) for x in args.ks.split(",")]
    kmax = max(ks)
    corpus = np.ascontiguousarray(read_fbin(args.corpus, count=args.n))
    queries = np.ascontiguousarray(read_fbin(args.queries))
    gt = np.load(args.gt)
    dim = corpus.shape[1]
    log(f"N={args.n} dim={dim} nq={len(queries)} build_algo={args.build_algo}")

    gpu_before = gpu_mem_used_mb()
    d_corpus = cp.asarray(corpus)
    bparams = cagra.IndexParams(graph_degree=64, intermediate_graph_degree=128,
                                metric="sqeuclidean", build_algo=args.build_algo)
    t0 = time.perf_counter()
    index = cagra.build(bparams, d_corpus)
    cp.cuda.runtime.deviceSynchronize()
    build_time = time.perf_counter() - t0
    gpu_after = gpu_mem_used_mb()
    log(f"build {build_time:.2f}s vram {gpu_before}->{gpu_after} MB")

    d_queries = cp.asarray(queries)
    nq = len(queries)
    sq_n = min(args.single_query_n, nq)

    for itopk in [int(x) for x in args.itopk.split(",")]:
        for sw in [int(x) for x in args.search_width.split(",")]:
            if itopk < kmax:
                continue  # itopk must be >= k
            sp = cagra.SearchParams(itopk_size=itopk, search_width=sw)
            # warm + batched timing
            cagra.search(sp, index, d_queries[:256], kmax)
            cp.cuda.runtime.deviceSynchronize()
            t0 = time.perf_counter()
            dists, nbrs = cagra.search(sp, index, d_queries, kmax)
            cp.cuda.runtime.deviceSynchronize()
            batched_s = time.perf_counter() - t0
            nbrs_np = cp.asnumpy(nbrs)
            qps = nq / batched_s
            # single-query latencies
            lat = []
            for i in range(sq_n):
                qi = d_queries[i:i + 1]
                t1 = time.perf_counter()
                cagra.search(sp, index, qi, kmax)
                cp.cuda.runtime.deviceSynchronize()
                lat.append(time.perf_counter() - t1)
            p50, p95, p99 = percentiles_ms(lat)
            for k in ks:
                rec = recall_at_k(nbrs_np[:, :k], gt[:, :k], k)
                emit_result(args.out, system="cuvs-cagra", dataset=args.dataset,
                            N=args.n, dim=dim, metric="cosine(L2-normed)", k=k,
                            param_set=f"itopk={itopk},sw={sw}", build_time_s=round(build_time, 3),
                            index_bytes=None, host_mem_mb=None,
                            gpu_mem_mb=round(gpu_after - gpu_before, 1),
                            recall=round(rec, 4), qps=round(qps, 1),
                            p50_ms=round(p50, 3), p95_ms=round(p95, 3), p99_ms=round(p99, 3),
                            n_queries=nq,
                            notes=("pg_cuvs-equivalent" if (itopk == 128 and sw == 1) else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
