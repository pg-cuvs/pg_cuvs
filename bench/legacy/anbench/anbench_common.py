#!/usr/bin/env python3
"""Shared helpers for the ANN benchmark: .fbin I/O, recall@k, result rows."""
import json
import os
import struct
import time

import numpy as np

RESULT_FIELDS = [
    "system", "dataset", "N", "dim", "metric", "k", "param_set",
    "build_time_s", "index_bytes", "host_mem_mb", "gpu_mem_mb",
    "recall", "qps", "p50_ms", "p95_ms", "p99_ms", "n_queries", "notes",
]


def setup_pg_session(conn, build_mem_gb=16, parallel_workers=7):
    """Apply required session-level settings for pg_cuvs / pgvector benchmarks.

    Must be called on every new psycopg connection before issuing
    CREATE INDEX or other build-heavy operations.  These settings are
    session-scoped and reset to postgresql.conf defaults on reconnect —
    never assume a prior connection's SET survives.

    Args:
        conn: open psycopg connection (autocommit=True recommended)
        build_mem_gb: maintenance_work_mem in GB (default 16)
        parallel_workers: max_parallel_maintenance_workers (default 7)
    """
    conn.execute(f"SET maintenance_work_mem = '{build_mem_gb}GB'")
    conn.execute(f"SET max_parallel_maintenance_workers = {parallel_workers}")


def read_fbin(path, count=None, offset=0):
    """Read a big-ann .fbin: int32 n, int32 dim, then n*dim float32 row-major.
    Returns a (count, dim) float32 array (mmap-backed view sliced to a copy-free
    ndarray). offset/count select a row range."""
    with open(path, "rb") as f:
        n, dim = struct.unpack("<ii", f.read(8))
    if count is None:
        count = n - offset
    arr = np.memmap(path, dtype=np.float32, mode="r", offset=8 + offset * dim * 4,
                    shape=(count, dim))
    return arr


def fbin_meta(path):
    with open(path, "rb") as f:
        n, dim = struct.unpack("<ii", f.read(8))
    return n, dim


def recall_at_k(returned_ids, gt_ids, k):
    """Mean |returned[:k] ∩ gt[:k]| / k over all queries. Both are int arrays
    shaped (n_queries, >=k) in the SAME corpus-row id space."""
    returned = np.asarray(returned_ids)[:, :k]
    gt = np.asarray(gt_ids)[:, :k]
    hits = 0
    for r, g in zip(returned, gt):
        hits += len(set(r.tolist()) & set(g.tolist()))
    return hits / (returned.shape[0] * k)


def percentiles_ms(latencies_s):
    a = np.asarray(latencies_s, dtype=np.float64) * 1000.0
    if a.size == 0:
        return (float("nan"),) * 3
    return (float(np.percentile(a, 50)),
            float(np.percentile(a, 95)),
            float(np.percentile(a, 99)))


def host_mem_mb():
    try:
        import resource
        # ru_maxrss is KB on Linux
        return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024.0
    except Exception:
        return float("nan")


def gpu_mem_used_mb():
    """Best-effort current GPU memory.used via nvidia-smi (whole device)."""
    import subprocess
    try:
        out = subprocess.check_output(
            ["nvidia-smi", "--query-gpu=memory.used", "--format=csv,noheader,nounits"],
            text=True)
        return float(out.strip().splitlines()[0])
    except Exception:
        return float("nan")


def emit_result(out_path, **row):
    r = {f: row.get(f) for f in RESULT_FIELDS}
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "a") as f:
        f.write(json.dumps(r) + "\n")
    print(f"[result] {r['system']} N={r['N']} k={r['k']} param={r['param_set']} "
          f"recall={r['recall']} qps={r['qps']} p50={r['p50_ms']}ms", flush=True)


class Timer:
    def __enter__(self):
        self.t0 = time.perf_counter()
        return self

    def __exit__(self, *a):
        self.dt = time.perf_counter() - self.t0
