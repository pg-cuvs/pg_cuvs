#!/usr/bin/env python3
"""PR-E raw cuVS arm under nsys: same Pareto params as the pg_cuvs arm.

Build params gd=32 igd=128 (ADR-084 raw arm), K=200, 1M x 768. The raw arm's
wall-clock is the harness's own perf_counter; nsys supplies kernel and memcpy
totals for the same dispatches. Build happens before MARK_SEARCH_START so the
build's H2D corpus copy can be separated from the search memcpy.
"""
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.expanduser("~/pg_cuvs/bench/cuvs_bench_backend"))
from cuvs_engine import CuvsEngine  # noqa: E402
from pg_engine import read_fbin      # noqa: E402

DATA = os.path.expanduser("~/data")
N, DIM, K, KMAX = 1000000, 768, 200, 10
N_SINGLE, BATCH_Q = 300, 2000

eng = CuvsEngine()
t0 = time.perf_counter()
eng.load_corpus(os.path.join(DATA, "corpus.fbin"), N, DIM)
print(f"corpus_load_s={time.perf_counter()-t0:.2f}", flush=True)

bt, _, meta = eng.build("cuvs", N, build_cfg={"graph_degree": 32,
                                              "intermediate_graph_degree": 128})
print(f"RAW_BUILD_s={bt:.3f} meta={meta}", flush=True)

q = np.ascontiguousarray(read_fbin(os.path.join(DATA, "queries_10k.fbin")))

print("MARK_SEARCH_START", time.time(), flush=True)
ids, lat = eng.search("cuvs", q[:N_SINGLE], KMAX, K, warmup=50)
lat = np.array(lat) * 1e6
print(f"RAW_SINGLE n={N_SINGLE} mean_us={lat.mean():.1f} "
      f"p50={np.percentile(lat,50):.1f} p95={np.percentile(lat,95):.1f} "
      f"p99={np.percentile(lat,99):.1f}", flush=True)
print("MARK_SINGLE_END", time.time(), flush=True)

ids_b, elapsed = eng.search_batch(q[:BATCH_Q], KMAX, K, warmup=2, repeats=5)
print(f"RAW_BATCH Q={BATCH_Q} k={K} median_s={elapsed:.4f} "
      f"qps={BATCH_Q/elapsed:.1f}", flush=True)
print("MARK_BATCH_END", time.time(), flush=True)
