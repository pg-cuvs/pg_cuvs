#!/usr/bin/env python3
"""
adr079_3o_recall.py -- #76 / ADR-079: measure the 3O recall ceiling.

3O = global CAGRA graph + GPU BITSET prefilter. VecFlow (arXiv:2506.00812,
SIGMOD 2026, by cuVS engineers) reports that this architecture plateaus near
80% recall and collapses under highly selective filters. pg_cuvs ships 3O but
had never measured that failure mode on its own implementation.

This sweeps filter selectivity and records recall@k for both filtered paths:

  3O       cuvs.filter_auto_threshold = 1.0  -> GPU BITSET prefilter over the
           CAGRA graph (daemon search_mode 4 = cagra_prefilter)
  D-wedge  cuvs.filter_auto_threshold = 0    -> exact BF over the whole corpus
           with 4x overfetch, then post-filter (ADR-063)

MEASUREMENT HONESTY
  Ground truth is computed here in numpy -- exact top-k over the *filtered*
  subset -- not taken from pg_cuvs. A systematic engine error therefore cannot
  hide inside the reference. The daemon's own search_mode is read back from
  pg_stat_gpu_search and reported, so a run that silently fell back to the
  exact BF prefilter (mode 3) or to D-wedge is visible as such rather than
  being reported as a 3O result.

Usage (GPU VM, cuvs_bench env, daemon up):
    python bench/adr079_3o_recall.py --data-dir ~/data --n 1000000 \
        --queries 200 --k 10 --out bench/results/adr079_3o_recall.csv
"""
import argparse
import csv
import struct
import sys
import time

import numpy as np

SELECTIVITIES = (0.5, 0.1, 0.05, 0.01, 0.005, 0.001)
HASH_MOD = 1_000_000          # cat = (id * KNUTH) % HASH_MOD -> filter is cat < s*HASH_MOD
KNUTH = 2654435761


def read_fbin(path, count=None, offset=0):
    with open(path, "rb") as f:
        n, dim = struct.unpack("<ii", f.read(8))
    if count is None:
        count = n - offset
    return np.memmap(path, dtype=np.float32, mode="r",
                     offset=8 + offset * dim * 4, shape=(count, dim))


def exact_topk_in_subset(base, subset_idx, queries, k, chunk=64):
    """Exact top-k row indices (into the full corpus) restricted to subset_idx.

    L2 ranking only needs ||b||^2 - 2 q.b; the ||q||^2 term is constant per
    query and cannot change the ordering, so it is dropped.
    """
    sub = np.ascontiguousarray(base[subset_idx])
    bn = (sub * sub).sum(1)
    out = np.empty((len(queries), k), dtype=np.int64)
    for s in range(0, len(queries), chunk):
        e = min(s + chunk, len(queries))
        d = bn[None, :] - 2.0 * (queries[s:e] @ sub.T)
        part = np.argpartition(d, k, axis=1)[:, :k]
        order = np.argsort(np.take_along_axis(d, part, axis=1), axis=1)
        out[s:e] = subset_idx[np.take_along_axis(part, order, axis=1)]
    return out


def recall_at_k(got, gt, k):
    """Mean over queries of |returned_topk ∩ gt_topk| / k."""
    tot = 0.0
    for g, t in zip(got, gt):
        tot += len(set(g[:k]) & set(t[:k].tolist())) / float(k)
    return tot / len(gt)


def encode_ctid(ctid):
    """'(block,off)' -> block<<16 | off, pg_cuvs's TID encoding."""
    block, off = ctid.strip("()").split(",")
    return (int(block) << 16) | int(off)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-dir", required=True)
    ap.add_argument("--n", type=int, default=1_000_000)
    ap.add_argument("--queries", type=int, default=200)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--dbname", default="postgres")
    ap.add_argument("--index-dir", default="/tmp/cuvs_indexes")
    ap.add_argument("--out", required=True)
    ap.add_argument("--reuse-table", action="store_true",
                    help="skip COPY/CREATE INDEX if f3o is already loaded")
    args = ap.parse_args()

    import psycopg
    import pgvector.psycopg
    from pgvector import Vector

    corpus = f"{args.data_dir}/corpus.fbin"
    base = read_fbin(corpus, count=args.n)
    queries = np.ascontiguousarray(
        read_fbin(f"{args.data_dir}/queries_10k.fbin", count=args.queries))
    dim = base.shape[1]

    conn = psycopg.connect(dbname=args.dbname, autocommit=True)
    conn.execute("CREATE EXTENSION IF NOT EXISTS vector")
    conn.execute("CREATE EXTENSION IF NOT EXISTS pg_cuvs")
    pgvector.psycopg.register_vector(conn)
    conn.execute(f"SET cuvs.index_dir = '{args.index_dir}'")

    loaded = conn.execute(
        "SELECT count(*) FROM pg_class WHERE relname='f3o'").fetchone()[0]
    if not (args.reuse_table and loaded):
        conn.execute("DROP TABLE IF EXISTS f3o CASCADE")
        conn.execute(f"CREATE TABLE f3o (id bigint, cat int, embedding vector({dim}))")
        t0 = time.perf_counter()
        with conn.cursor().copy(
                "COPY f3o (id, cat, embedding) FROM STDIN WITH (FORMAT BINARY)") as cp:
            cp.set_types(["int8", "int4", "vector"])
            for s in range(0, args.n, 50_000):
                e = min(s + 50_000, args.n)
                chunk = np.ascontiguousarray(base[s:e])
                for i in range(e - s):
                    rid = s + i
                    cp.write_row((rid, (rid * KNUTH) % HASH_MOD, Vector(chunk[i])))
        print(f"[setup] COPY {args.n} rows in {time.perf_counter()-t0:.1f}s", flush=True)
        # brute_force build mode also emits the .vectors sidecar + reverse map the
        # filtered paths need; without it the daemon cannot build the BITSET.
        conn.execute("SET cuvs.search_mode = 'brute_force'")
        t0 = time.perf_counter()
        conn.execute("CREATE INDEX f3o_cagra ON f3o USING cagra (embedding vector_l2_ops)")
        print(f"[setup] CREATE INDEX in {time.perf_counter()-t0:.1f}s", flush=True)
    conn.execute("ANALYZE f3o")   # reltuples drives the selectivity routing gate

    cat = (np.arange(args.n, dtype=np.int64) * KNUTH) % HASH_MOD
    ctid_of_id = {}
    for rid, ct in conn.execute("SELECT id, ctid::text FROM f3o").fetchall():
        ctid_of_id[encode_ctid(ct)] = rid

    # The TID whitelist is up to n*0.5 bigints. Re-serialising it from the client
    # on every query would swamp the latency being measured, so it is materialised
    # server-side once per selectivity and referenced by a scalar subquery.
    conn.execute("DROP TABLE IF EXISTS filterset")
    conn.execute("CREATE TABLE filterset (sel float8 PRIMARY KEY, tids bigint[])")

    rows = []
    for sel in SELECTIVITIES:
        cut = int(sel * HASH_MOD)
        subset_idx = np.nonzero(cat < cut)[0]
        actual_sel = len(subset_idx) / args.n
        conn.execute(
            "INSERT INTO filterset "
            "SELECT %s, array_agg(t ORDER BY t) FROM ("
            "  SELECT (((ctid::text::point)[0])::bigint << 16)"
            "       | ((ctid::text::point)[1])::bigint AS t"
            "  FROM f3o WHERE cat < %s) s", (sel, cut))
        t0 = time.perf_counter()
        gt = exact_topk_in_subset(base, subset_idx, queries, args.k)
        print(f"[gt] sel={actual_sel:.4f} |S|={len(subset_idx)} "
              f"exact top-{args.k} in {time.perf_counter()-t0:.1f}s", flush=True)

        for path, threshold in (("3O", 1.0), ("D-wedge", 0.0)):
            conn.execute("SET cuvs.stream_bf_selectivity_threshold = 0")
            conn.execute(f"SET cuvs.filter_auto_threshold = {threshold}")
            got, lat = [], []
            for qi in range(args.queries):
                q0 = time.perf_counter()
                res = conn.execute(
                    "SELECT ctid::text FROM cuvs_filtered_knn("
                    "  'f3o_cagra'::regclass, %s::vector,"
                    "  (SELECT tids FROM filterset WHERE sel = %s), %s)",
                    (Vector(queries[qi]), sel, args.k)).fetchall()
                lat.append((time.perf_counter() - q0) * 1000.0)
                got.append([ctid_of_id.get(encode_ctid(r[0]), -1) for r in res])
            mode = conn.execute(
                "SELECT search_mode FROM pg_stat_gpu_search "
                "WHERE index_oid = 'f3o_cagra'::regclass").fetchone()
            r = recall_at_k(got, gt, args.k)
            returned = np.mean([len(g) for g in got])
            rows.append(dict(path=path, selectivity=round(actual_sel, 6),
                             n_filter=len(subset_idx), k=args.k,
                             recall=round(r, 4),
                             mean_returned=round(float(returned), 2),
                             p50_ms=round(float(np.percentile(lat, 50)), 3),
                             daemon_search_mode=(mode[0] if mode else None),
                             n_queries=args.queries))
            print(f"  {path:8s} sel={actual_sel:.4f} recall@{args.k}={r:.4f} "
                  f"returned={returned:.2f} mode={rows[-1]['daemon_search_mode']}",
                  flush=True)

    with open(args.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"[done] wrote {args.out} ({len(rows)} rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
