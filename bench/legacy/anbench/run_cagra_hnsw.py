#!/usr/bin/env python3
"""
run_cagra_hnsw.py - the "GPU build, CPU serve" axis. Build a CAGRA graph on the
GPU (same params as pg_cuvs), convert it to an HNSW index with
cuvs.neighbors.hnsw.from_cagra, then search ON THE CPU (no GPU, no daemon at
query time). This answers: can we use the GPU to build the graph fast and then
serve queries cheaply on CPU like pgvector HNSW -- but with a GPU-built graph?

Env: cuvs_py. Reports batched QPS (multi-thread) + single-query p50/95/99
(num_threads=1, comparable to a single SQL backend).
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
    print(f"[cagra-hnsw] {m}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--queries", required=True)
    ap.add_argument("--gt", required=True)
    ap.add_argument("--n", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--dataset", default="cohere-wiki-en-1024")
    ap.add_argument("--ks", default="10,100")
    ap.add_argument("--ef", default="16,32,64,128,256,512")
    ap.add_argument("--threads", type=int, default=0, help="0 = all cpus for batched QPS")
    ap.add_argument("--single-query-n", type=int, default=2000)
    args = ap.parse_args()

    import cupy as cp
    from cuvs.neighbors import cagra, hnsw

    ks = [int(x) for x in args.ks.split(",")]
    kmax = max(ks)
    corpus = np.ascontiguousarray(read_fbin(args.corpus, count=args.n))
    queries = np.ascontiguousarray(read_fbin(args.queries))
    gt = np.load(args.gt)
    dim = corpus.shape[1]
    nq = len(queries)
    ncpu = args.threads or (os.cpu_count() or 8)
    sq_n = min(args.single_query_n, nq)
    log(f"N={args.n} dim={dim} nq={nq} cpus={ncpu}")

    gpu_before = gpu_mem_used_mb()
    d_corpus = cp.asarray(corpus)
    t0 = time.perf_counter()
    cagra_idx = cagra.build(cagra.IndexParams(graph_degree=64,
                            intermediate_graph_degree=128, metric="sqeuclidean"), d_corpus)
    cp.cuda.runtime.deviceSynchronize()
    t_build = time.perf_counter() - t0
    gpu_after = gpu_mem_used_mb()
    # convert CAGRA graph -> CPU HNSW
    t0 = time.perf_counter()
    hidx = hnsw.from_cagra(hnsw.IndexParams(), cagra_idx)
    t_conv = time.perf_counter() - t0
    del d_corpus, cagra_idx
    cp.get_default_memory_pool().free_all_blocks()
    build_time = t_build + t_conv
    log(f"cagra build {t_build:.2f}s + hnsw convert {t_conv:.2f}s = {build_time:.2f}s; "
        f"gpu {gpu_before}->{gpu_after}MB (freed after build)")

    for ef in [int(x) for x in args.ef.split(",")]:
        if ef < kmax:
            continue
        # batched (multi-thread) for QPS
        spb = hnsw.SearchParams(ef=ef, num_threads=ncpu)
        hnsw.search(spb, hidx, queries[:256], kmax)  # warm
        t0 = time.perf_counter()
        _, nbrs = hnsw.search(spb, hidx, queries, kmax)
        batched = time.perf_counter() - t0
        nbrs = np.asarray(nbrs)
        qps = nq / batched
        # single-query (1 thread) for latency
        sp1 = hnsw.SearchParams(ef=ef, num_threads=1)
        lat = []
        for i in range(sq_n):
            t1 = time.perf_counter()
            hnsw.search(sp1, hidx, queries[i:i + 1], kmax)
            lat.append(time.perf_counter() - t1)
        p50, p95, p99 = percentiles_ms(lat)
        for k in ks:
            rec = recall_at_k(nbrs[:, :k], gt[:, :k], k)
            emit_result(args.out, system="cagra-hnsw-cpu", dataset=args.dataset,
                        N=args.n, dim=dim, metric="cosine(L2-normed)", k=k,
                        param_set=f"ef={ef}", build_time_s=round(build_time, 3),
                        index_bytes=None, host_mem_mb=None,
                        gpu_mem_mb=round(gpu_after - gpu_before, 1),
                        recall=round(rec, 4), qps=round(qps, 1), p50_ms=round(p50, 3),
                        p95_ms=round(p95, 3), p99_ms=round(p99, 3), n_queries=nq,
                        notes="GPU CAGRA build -> CPU HNSW search")
    return 0


if __name__ == "__main__":
    sys.exit(main())
