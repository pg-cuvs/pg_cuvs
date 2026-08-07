#!/usr/bin/env python3
"""#161 lever 1: index_bytes/build-time before-vs-after packing, dim=768.

'before' is not re-measured against old code (packing replaced the old
one-pair-per-page writer, so there is nothing left to build against) -- it
is the deterministic (N+1)*8192 formula that layout produced, unaffected by
any build parameter (#161's own finding). 'after' is a real build against
the current, packed code."""
import json
import os
import sys
import time

sys.path.insert(0, os.path.expanduser("~/pg_cuvs/bench/cuvs_bench_backend"))
from pg_engine import read_fbin

import psycopg
import pgvector.psycopg

N = 100_000
DIM = 768
CORPUS = os.path.expanduser("~/data/corpus.fbin")

admin = psycopg.connect(dbname="postgres", autocommit=True)
admin.execute('DROP DATABASE IF EXISTS "lever1_bench"')
admin.execute('CREATE DATABASE "lever1_bench"')
admin.close()
conn = psycopg.connect(dbname="lever1_bench", autocommit=True)
conn.execute("CREATE EXTENSION vector")
conn.execute("CREATE EXTENSION pg_cuvs")
pgvector.psycopg.register_vector(conn)

with conn.cursor() as cur:
    cur.execute("SET maintenance_work_mem = '8GB'")
    cur.execute(f"CREATE TABLE t (id bigint, embedding vector({DIM}))")
t0 = time.perf_counter()
with conn.cursor().copy("COPY t (id, embedding) FROM STDIN WITH (FORMAT BINARY)") as cp:
    cp.set_types(["int8", "vector"])
    batch = 20_000
    for s in range(0, N, batch):
        e = min(s + batch, N)
        chunk = read_fbin(CORPUS, count=e - s, offset=s)
        for i in range(e - s):
            cp.write_row((s + i, chunk[i]))
print(f"[load] {N} rows dim={DIM} in {time.perf_counter()-t0:.1f}s", flush=True)

with conn.cursor() as cur:
    cur.execute("SET cuvs.index_dir = '/tmp/cuvs_indexes'")
    cur.execute("SET maintenance_work_mem = '8GB'")
    t0 = time.perf_counter()
    cur.execute("CREATE INDEX t_cagra ON t USING cagra (embedding vector_l2_ops) "
                "WITH (graph_degree=64, intermediate_graph_degree=128, build_algo='ivf_pq')")
    t_cagra = time.perf_counter() - t0
    print(f"[build] cagra {t_cagra:.1f}s", flush=True)

    t0 = time.perf_counter()
    cur.execute("CREATE INDEX t_hnsw ON t USING pg_cuvs_hnsw (embedding vector_l2_ops) "
                "WITH (source='t_cagra', mode='nsw')")
    t_hnsw = time.perf_counter() - t0
    cur.execute("SELECT pg_relation_size('t_hnsw')")
    packed_bytes = cur.fetchone()[0]
    print(f"[build] hnsw (packed) {t_hnsw:.1f}s, index_bytes={packed_bytes}", flush=True)

old_bytes = (N + 1) * 8192
print(f"\nold (1 elem/page, computed): {old_bytes} bytes ({old_bytes/N:.1f} B/vector)")
print(f"new (packed, measured):      {packed_bytes} bytes ({packed_bytes/N:.1f} B/vector)")
print(f"ratio: {old_bytes/packed_bytes:.2f}x smaller")

# pgvector native for reference
with conn.cursor() as cur:
    t0 = time.perf_counter()
    cur.execute("CREATE INDEX t_hnsw_pgv ON t USING hnsw (embedding vector_l2_ops)")
    t_pgv = time.perf_counter() - t0
    cur.execute("SELECT pg_relation_size('t_hnsw_pgv')")
    pgv_bytes = cur.fetchone()[0]
    print(f"\npgvector native: {t_pgv:.1f}s build, index_bytes={pgv_bytes} ({pgv_bytes/N:.1f} B/vector)")
    print(f"pg_cuvs_hnsw (packed) vs pgvector native size ratio: {packed_bytes/pgv_bytes:.2f}x")

out = {
    "n": N, "dim": DIM,
    "build_s_cagra": round(t_cagra, 2),
    "build_s_hnsw_packed": round(t_hnsw, 2),
    "build_s_pgvector_native": round(t_pgv, 2),
    "index_bytes_old_computed": old_bytes,
    "index_bytes_packed_measured": packed_bytes,
    "index_bytes_pgvector_native": pgv_bytes,
    "ratio_old_over_packed": round(old_bytes / packed_bytes, 4),
    "ratio_packed_over_pgvector_native": round(packed_bytes / pgv_bytes, 4),
}
out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "results", "issue_161_packing_bench.json")
with open(out_path, "w") as f:
    json.dump(out, f, indent=2)
print(f"\n[done] -> {out_path}")
