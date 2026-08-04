#!/usr/bin/env python3
"""
conc_runner.py -- the #98 throughput axis's concurrency arm.

N client connections, each looping single-query index scans for a fixed
wall-clock window, is what a PostgreSQL application's throughput actually looks
like. This measures that: sustained QPS over the window, merged per-query
percentiles, and -- unlike the pgbench driver this replaces -- the returned
neighbour ids, so the arm reports a recall instead of asking the reader to
assume one.

Why not bench/protocol/runner_concurrency.py: it drives pgbench, which reports
latency only. A throughput number without a recall is not a point on a
recall/QPS curve, so it cannot enter this run's tables.

What the workers must replicate, and why each one matters:

  enable_seqscan=off      a worker that falls back to an exact seq scan reports
                          recall~=1.0 at huge latency -- a silently wrong row.
  cuvs.k / hnsw.ef_search the operating point. A GUC set only on the parent's
                          connection is not set on any worker's.
  enable_cuvs=off         pgvector's arm must be CPU, even with a GPU index
                          resident.
  cuvs.shard_count=1      sharding changes both the daemon's locking window and
                          VRAM; 0 ("auto") does not mean the same thing in every
                          code path, so it is pinned rather than defaulted.

Measured on the pg_cuvs side this arm is expected to be flat in N (the daemon
serialises non-sharded single searches under its global index mutex): the
pre-registered microbench on issue #98 measured QPS 757 -> 1296 -> 1307 for
N = 1, 4, 8 with p50 rising 1.31 -> 3.07 -> 6.11 ms, i.e. a hard ceiling with
pure queueing above it. That is an implementation-status fact about today's
daemon and the evidence for why pg_cuvs's throughput mechanism is the batch
arm -- it is not a claim about CAGRA.
"""
import multiprocessing as mp
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pg_engine import _vec_literal, percentiles_ms  # noqa: E402
from sidecar import RELATION_OF  # noqa: E402

#: Default concurrency ladder (plan rev.4). N=1 doubles as the consistency check
#: against the latency axis.
DEFAULT_N = (1, 8, 16, 32, 64)

WARMUP_QUERIES = 5


def worker_gucs(algo, param, index_dir):
    """The SET statements one worker connection needs for `algo`.

    Returned as a list rather than executed here so the exact set a worker will
    run is inspectable (and testable) without a database.
    """
    gucs = ["SET enable_seqscan = off"]
    if algo == "pgvector_hnsw":
        # pgvector's arm is the CPU baseline even while a GPU index is resident.
        gucs += ["SET enable_cuvs = off", f"SET hnsw.ef_search = {int(param)}"]
    elif algo == "pgcuvs_hnsw_import":
        gucs += ["SET enable_cuvs = off", f"SET hnsw.ef_search = {int(param)}"]
    elif algo == "pgcuvs_cagra":
        gucs += [f"SET cuvs.index_dir = '{index_dir}'",
                 "SET cuvs.shard_count = 1",
                 f"SET cuvs.k = {int(param)}"]
    else:
        raise ValueError(f"conc arm not defined for algo {algo}")
    return gucs


def _worker(wid, algo, param, index_dir, dbname, qidx, qvecs, top_k, window,
            barrier, out):
    """One connection, one disjoint query slice, one fixed time window."""
    import psycopg
    try:
        conn = psycopg.connect(dbname=dbname, autocommit=True)
        cur = conn.cursor()
        for g in worker_gucs(algo, param, index_dir):
            cur.execute(g)
        nq = len(qvecs)

        def one(j):
            # The literal is formatted INSIDE the timed region, exactly as the
            # latency axis does it (pg_engine.search). It costs ~0.45 ms/query
            # at dim=768, and it is a cost the application genuinely pays -- the
            # measurement boundary for both axes is the full round trip a
            # PostgreSQL client experiences. Precomputing it here would make
            # conc N=1 faster than the latency axis for a reason that has
            # nothing to do with concurrency (measured: ratio 1.33). The one
            # sanctioned departure from the same-statement-shape rule is the
            # batch arm's bind parameter, and that exception is pre-registered.
            cur.execute("SELECT id FROM t ORDER BY embedding <-> "
                        f"'{_vec_literal(qvecs[j])}'::vector LIMIT {top_k}")
            return [r[0] for r in cur.fetchall()]

        # Plan guard, per worker: the parent's EXPLAIN says nothing about a
        # connection that has its own GUCs.
        cur.execute("EXPLAIN (FORMAT TEXT) SELECT id FROM t ORDER BY embedding "
                    f"<-> '{_vec_literal(qvecs[0])}'::vector LIMIT {top_k}")
        plan = "\n".join(r[0] for r in cur.fetchall())
        seqscan = "Seq Scan" in plan
        index_used = plan_index(plan)

        for j in range(min(WARMUP_QUERIES, nq)):
            one(j)

        barrier.wait()
        t0 = time.perf_counter()
        lat, ids, n, i = [], {}, 0, 0
        while time.perf_counter() - t0 < window:
            j = i % nq
            t1 = time.perf_counter()
            got = one(j)
            lat.append(time.perf_counter() - t1)
            # Keep the FIRST result per query only: a repeated query must not
            # weight recall more heavily than one measured once.
            ids.setdefault(int(qidx[j]), got)
            i += 1
            n += 1
        wall = time.perf_counter() - t0
        conn.close()
        out.put({"wid": wid, "n": n, "wall": wall, "lat": lat, "ids": ids,
                 "seqscan": seqscan, "slice": nq, "index_used": index_used,
                 "passes": i / max(1, nq), "error": None})
    except Exception as e:  # noqa: BLE001 -- a dead worker must not hang the parent
        try:
            barrier.abort()
        except Exception:  # noqa: BLE001
            pass
        out.put({"wid": wid, "error": repr(e), "n": 0, "wall": 0.0, "lat": [],
                 "ids": {}, "seqscan": False, "slice": 0, "passes": 0.0,
                 "index_used": None})


def plan_index(plan):
    """The relation an EXPLAIN plan actually scans, or None.

    Checking only for "Seq Scan" is not enough: with two ANN indexes on one
    column, a plan can be perfectly index-driven and still be scanning the OTHER
    algo's index -- and the row would carry this algo's label over the other
    one's numbers. This is what makes a conc row self-evidencing about which
    index served it (see the arm's notes).
    """
    for line in plan.splitlines():
        line = line.strip()
        if line.startswith("->"):
            line = line[2:].strip()
        if line.startswith("Index Scan using ") or line.startswith("Index Only Scan using "):
            return line.split(" using ", 1)[1].split(" on ", 1)[0].strip()
        if line.startswith("Seq Scan"):
            return "SEQSCAN"
    return None


def slices_for(n_queries, n_workers):
    """Disjoint, near-equal index slices of the FULL query set.

    The full 10k queries -- not the 2000 the latency axis scores -- because with
    2000 fixed, 64 workers would each replay ~120x within a 30 s window and the
    arm would measure a cache-hot workload, asymmetrically in pgvector's favour.
    """
    return [s for s in np.array_split(np.arange(n_queries), n_workers) if len(s)]


def run_arm(algo, n_workers, queries, gt, param, index_dir, dbname="postgres",
            window=30.0, top_k=10, mp_ctx=None):
    """Run one `{algo}_conc{N}` arm. Returns a dict of measured facts.

    Recall is computed only over queries whose ground truth exists (`gt` may
    cover fewer queries than the 10k pool the slices are drawn from), and only
    over queries actually reached inside the window.
    """
    ctx = mp_ctx or mp.get_context("spawn")
    sl = slices_for(len(queries), n_workers)
    barrier = ctx.Barrier(len(sl))
    out = ctx.Queue()
    procs = []
    for wid, idx in enumerate(sl):
        p = ctx.Process(target=_worker,
                        args=(wid, algo, param, index_dir, dbname, idx,
                              np.ascontiguousarray(queries[idx]), top_k, window,
                              barrier, out))
        p.start()
        procs.append(p)
    res = [out.get() for _ in procs]
    for p in procs:
        p.join()

    errs = [r["error"] for r in res if r["error"]]
    if errs:
        raise RuntimeError(f"{algo}_conc{n_workers}: {len(errs)} worker(s) "
                           f"failed; first: {errs[0]}")

    total = sum(r["n"] for r in res)
    wall = max(r["wall"] for r in res)
    lat = [x for r in res for x in r["lat"]]
    p50, p95, p99 = percentiles_ms(lat)

    merged = {}
    for r in res:
        merged.update(r["ids"])
    covered = [q for q in merged if q < len(gt)]
    if covered:
        got = np.array([(merged[q] + [-1] * top_k)[:top_k] for q in covered])
        recall = _recall(got, np.asarray(gt)[covered], top_k)
    else:
        recall = float("nan")

    passes = max(r["passes"] for r in res)
    notes = []
    if passes > 1.0:
        notes.append(f"slice repeated {passes:.1f}x within the window "
                     "(cache-hot to that degree)")
    if any(r["seqscan"] for r in res):
        notes.append("WARN seqscan detected in worker plan")
    used = sorted({r["index_used"] for r in res if r["index_used"]})
    return {"algo": algo, "n_workers": n_workers, "qps": total / wall,
            "queries": total, "wall": wall, "recall": recall,
            "p50_ms": p50, "p95_ms": p95, "p99_ms": p99,
            "recall_queries": len(covered), "passes": passes,
            "seqscan": any(r["seqscan"] for r in res), "notes": notes,
            "index_used": used, "expected_index": RELATION_OF.get(algo)}


def _recall(got, gt, k):
    """recall@k of `got` against `gt`, both in the corpus-row-index id space."""
    kk = min(k, gt.shape[1])
    hits = 0
    for a, b in zip(got[:, :k], gt[:, :kk]):
        hits += len(set(int(x) for x in a) & set(int(x) for x in b))
    return hits / float(len(got) * kk)
