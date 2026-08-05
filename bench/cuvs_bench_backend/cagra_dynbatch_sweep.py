#!/usr/bin/env python3
"""CAGRA dynamic-batching sweep -- the #160 feature-validation measurement.

What this measures. `cuvs.cagra_batch_wait_us` opens a coalescing window in the
daemon so that concurrent single-query CAGRA searches merge into one multi-CTA
dispatch. This driver sweeps (N workers) x (window) against an already-resident
`t_cagra` index and reports, per cell, QPS / p50 / p95 / recall@10 / the
`pg_stat_gpu_search.cagra_batch_count` delta over the cell.

The counter delta is the load-bearing control: `wait_us=0` must produce a delta
of exactly 0 (no merging happened), and `total_queries / delta` is the average
coalesced batch width for the cells that did merge. A QPS gain without a
matching counter delta would mean something other than batching moved.

Query shape is deliberately the same as #98's conc arms -- closed loop, N
processes, inline vector literal + `LIMIT 10`, one sustained window per cell --
so the wait=0 column reproduces the daemon global-mutex serialization ceiling
that #98 documented, and the merged cells are read against it.

This is a **feature-validation sweep, not a canonical two-axis run.** It uses
its own driver rather than `run_pg_cuvsbench.py`, and the host it was taken on
(`massedcompute_A100_sxm4_80G`) is a different platform variant from the DGX
node behind `bench/results/pg_cuvsbench_98.csv`. Cite ratios measured **within**
one output file; absolute QPS here is not comparable to any other artifact.

Recorded run (2026-08-05, ext 0.7.0, `bench/results/cagra_dynbatch_160.csv`):
`t_cagra` = wiki_all_1M 1M x 768, `graph_degree=32`,
`intermediate_graph_degree=128`, `build_algo=ivf_pq`, freshly built; `cuvs.k=200`
(the #98 Pareto operating point); recall@10 over 2000 queries against exact GT.
Prerequisites: the daemon is up, the index is resident, and `~/data` holds
`queries_10k.fbin` and `gt_1000000_q2000.ibin`.

    gpu-run.sh run dynbatch160 -- \
        python3 bench/cuvs_bench_backend/cagra_dynbatch_sweep.py --db shadeform \
        > cagra_sweep.log

Narrative and the published table: `BENCHMARK.md` Section 2.1d.
"""
import argparse
import multiprocessing as mp
import os, struct, sys, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pg_engine import _vec_literal, read_fbin

WINDOW = 15.0
TOPK = 10
CUVS_K = 200   # #98 Pareto operating point

def read_ibin(path):
    with open(path, 'rb') as f:
        n, d = struct.unpack('<II', f.read(8))
        return np.frombuffer(f.read(n*d*4), dtype=np.int32).reshape(n, d)

def worker(wid, db, wait_us, gidx, qvecs, window, out):
    import psycopg
    conn = psycopg.connect(dbname=db, autocommit=True)
    cur = conn.cursor()
    for g in ('SET enable_seqscan = off',
              'SET cuvs.index_dir = \'/tmp/cuvs_indexes\'',
              'SET cuvs.k = %d' % CUVS_K,
              'SET cuvs.cagra_batch_wait_us = %d' % wait_us):
        cur.execute(g)
    lits = ['\'%s\'' % _vec_literal(v) for v in qvecs]
    for j in range(2):
        cur.execute('SELECT id FROM t ORDER BY embedding <-> %s::vector LIMIT %d' % (lits[j], TOPK))
        cur.fetchall()
    lats, ids_by_q, done = [], {}, 0
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < window:
        j = done % len(lits)
        s = time.perf_counter()
        cur.execute('SELECT id FROM t ORDER BY embedding <-> %s::vector LIMIT %d' % (lits[j], TOPK))
        rows = cur.fetchall()
        lats.append(time.perf_counter() - s)
        ids_by_q[int(gidx[j])] = [r[0] for r in rows]
        done += 1
    conn.close()
    out.put((wid, done, lats, ids_by_q))

def counter(db):
    import psycopg
    with psycopg.connect(dbname=db, autocommit=True) as c:
        r = c.execute("SELECT COALESCE(SUM(cagra_batch_count),0) FROM pg_stat_gpu_search").fetchone()
        return int(r[0])

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--db', default='shadeform', help='database holding the resident t_cagra index')
    ap.add_argument('--data-dir', default='~/data', help='directory with queries_10k.fbin and gt_1000000_q2000.ibin')
    args = ap.parse_args()
    data_dir = os.path.expanduser(args.data_dir)

    q = np.ascontiguousarray(read_fbin(os.path.join(data_dir, 'queries_10k.fbin')))[:2000]
    gt = read_ibin(os.path.join(data_dir, 'gt_1000000_q2000.ibin'))
    print('cell,N,wait_us,qps,p50_ms,p95_ms,recall_at10,covered_q,total_queries,cagra_batch_delta', flush=True)
    for N in (1, 8, 32):
        for wait_us in (0, 200, 1000):
            sl = np.array_split(np.arange(len(q)), N)
            b0 = counter(args.db)
            out = mp.Queue()
            procs = [mp.Process(target=worker,
                                args=(i, args.db, wait_us, sl[i], q[sl[i]], WINDOW, out))
                     for i in range(N)]
            [p.start() for p in procs]
            results = [out.get() for _ in range(N)]
            [p.join() for p in procs]
            b1 = counter(args.db)
            total = sum(r[1] for r in results)
            lats = np.array(sorted(sum((r[2] for r in results), [])))
            hits, covered = 0, 0
            for r in results:
                for qi, ids in r[3].items():
                    truth = set(gt[qi, :TOPK].tolist())
                    hits += len(truth & set(ids)); covered += 1
            rec = hits / (covered * TOPK) if covered else float('nan')
            print(f'RESULT,{N},{wait_us},{total/WINDOW:.0f},'
                  f'{np.percentile(lats,50)*1000:.2f},{np.percentile(lats,95)*1000:.2f},'
                  f'{rec:.4f},{covered},{total},{b1-b0}', flush=True)

if __name__ == '__main__':
    main()
