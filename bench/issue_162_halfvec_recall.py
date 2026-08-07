#!/usr/bin/env python3
"""#162: paired fp32-vs-fp16 recall delta for pg_cuvs_hnsw export.

Both HNSW indexes are exported from ONE CAGRA graph, so the only thing that
differs between the two arms is the narrow-cast of the page payload to fp16.
The measured delta therefore isolates the cast, and nothing else.

The high-dimensional corpora are CONSTRUCTED, not real embeddings: each 768d
wiki_all vector is concatenated with itself m times and divided by sqrt(m)
(m=2 -> 1536d, m=4 -> 3072d). That transform preserves L2 norm and scales every
pairwise distance by the same constant, so the exact top-K neighbour ids are
identical to the 768d subset's -- which is why ground truth is brute-forced once
in 768d and reused for both dims. It also means these vectors carry no more
discriminating information than 768d ones do: the ABSOLUTE recall numbers here
are NOT a claim about real 1536d/3072d embedding recall. Only the fp32-fp16
delta is the measurement.

Usage (on the GPU VM, cuvs_bench conda env):
    python issue_162_halfvec_recall.py --n 100000 --nq 1000 --dims 1536 3072
"""
import argparse
import json
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "cuvs_bench_backend"))
from pg_engine import read_fbin, recall_at_k, _vec_literal  # noqa: E402

BASE_DIM = 768


def brute_force_gt(corpus, queries, k, chunk=20_000):
    """Exact top-k L2 neighbour ids (nq, k), ascending distance."""
    nq = queries.shape[0]
    best_d = np.full((nq, 0), 0.0, dtype=np.float32)
    best_i = np.zeros((nq, 0), dtype=np.int64)
    q = np.ascontiguousarray(queries, dtype=np.float32)
    qn = (q * q).sum(1, keepdims=True)
    for s in range(0, corpus.shape[0], chunk):
        c = np.ascontiguousarray(corpus[s:s + chunk], dtype=np.float32)
        d = qn - 2.0 * (q @ c.T) + (c * c).sum(1)[None, :]
        kk = min(k, d.shape[1])
        part = np.argpartition(d, kk - 1, axis=1)[:, :kk]
        pd = np.take_along_axis(d, part, axis=1)
        best_d = np.concatenate([best_d, pd], axis=1)
        best_i = np.concatenate([best_i, part.astype(np.int64) + s], axis=1)
        kk = min(k, best_d.shape[1])
        sel = np.argpartition(best_d, kk - 1, axis=1)[:, :kk]
        best_d = np.take_along_axis(best_d, sel, axis=1)
        best_i = np.take_along_axis(best_i, sel, axis=1)
    order = np.argsort(best_d, axis=1)
    return np.take_along_axis(best_i, order, axis=1)


def expand(v, mult):
    """concat(v, v, ... m times) / sqrt(m) -- norm- and rank-preserving."""
    return np.tile(v, (1, mult)).astype(np.float32) / np.sqrt(mult, dtype=np.float32)


def load_table(conn, corpus_path, n, dim, mult, batch=20_000):
    from pgvector import Vector
    with conn.cursor() as cur:
        cur.execute("DROP TABLE IF EXISTS t CASCADE")
        cur.execute(f"CREATE TABLE t (id bigint, embedding vector({dim}))")
    t0 = time.perf_counter()
    with conn.cursor().copy(
            "COPY t (id, embedding) FROM STDIN WITH (FORMAT BINARY)") as cp:
        cp.set_types(["int8", "vector"])
        for s in range(0, n, batch):
            e = min(s + batch, n)
            chunk = expand(read_fbin(corpus_path, count=e - s, offset=s), mult)
            for i in range(e - s):
                cp.write_row((s + i, Vector(chunk[i])))
    with conn.cursor() as cur:
        cur.execute("SELECT count(*) FROM t")
        got = cur.fetchone()[0]
    if got != n:
        raise RuntimeError(f"COPY landed {got} rows, expected {n}")
    print(f"[load] dim={dim} {n} rows in {time.perf_counter()-t0:.1f}s", flush=True)


def build_indexes(conn, dim, index_dir, graph_degree, igd):
    import psycopg
    f32_error = None
    with conn.cursor() as cur:
        cur.execute(f"SET cuvs.index_dir = '{index_dir}'")
        cur.execute("SET maintenance_work_mem = '8GB'")
        t0 = time.perf_counter()
        cur.execute("CREATE INDEX t_cagra ON t USING cagra "
                    "(embedding vector_l2_ops) "
                    f"WITH (graph_degree={graph_degree}, "
                    f"intermediate_graph_degree={igd}, build_algo='ivf_pq')")
        t_cagra = time.perf_counter() - t0
        print(f"[build] cagra dim={dim} {t_cagra:.1f}s", flush=True)

        # The fp32 export has a hard dimensional ceiling: one element tuple plus
        # its neighbour list must fit a single 8KB page. Above it the fp32 arm
        # does not exist to compare against -- that absence IS the #162 result,
        # so record it instead of aborting the halfvec arm with it.
        t0 = time.perf_counter()
        try:
            cur.execute("CREATE INDEX t_hnsw_f32 ON t USING pg_cuvs_hnsw "
                        "(embedding vector_l2_ops) "
                        "WITH (source='t_cagra', mode='nsw')")
            t_f32 = time.perf_counter() - t0
            print(f"[build] hnsw fp32 dim={dim} {t_f32:.1f}s", flush=True)
        except psycopg.Error as e:
            t_f32 = None
            f32_error = str(e).strip()
            print(f"[build] hnsw fp32 dim={dim} REFUSED: {f32_error}", flush=True)

        t0 = time.perf_counter()
        cur.execute("CREATE INDEX t_hnsw_hv ON t USING pg_cuvs_hnsw "
                    f"((embedding::halfvec({dim})) halfvec_l2_ops) "
                    "WITH (source='t_cagra', mode='nsw')")
        t_hv = time.perf_counter() - t0
        print(f"[build] hnsw halfvec dim={dim} {t_hv:.1f}s", flush=True)
    return t_cagra, t_f32, t_hv, f32_error


def search(conn, queries, dim, k, ef, arm):
    """arm: 'f32' -> vector operator; 'halfvec' -> halfvec cast operator."""
    if arm == "f32":
        def stmt(lit):
            return (f"SELECT id FROM t ORDER BY embedding <-> "
                    f"'{lit}'::vector LIMIT {k}")
    else:
        def stmt(lit):
            return (f"SELECT id FROM t ORDER BY embedding::halfvec({dim}) <-> "
                    f"'{lit}'::halfvec({dim}) LIMIT {k}")

    ids = np.full((len(queries), k), -1, dtype=np.int64)
    with conn.cursor() as cur:
        cur.execute("SET enable_cuvs = off")
        cur.execute("SET enable_seqscan = off")
        cur.execute(f"SET hnsw.ef_search = {ef}")
        probe = stmt(_vec_literal(queries[0]))
        cur.execute("EXPLAIN (FORMAT TEXT) " + probe)
        plan = "\n".join(r[0] for r in cur.fetchall())
        if "Seq Scan" in plan:
            raise RuntimeError(f"{arm} ef={ef}: planner chose Seq Scan -- "
                               f"refusing to report an exact-scan result.\n{plan}")
        want = "t_hnsw_hv" if arm == "halfvec" else "t_hnsw_f32"
        if want not in plan:
            raise RuntimeError(f"{arm} ef={ef}: plan does not use {want}.\n{plan}")
        for i, q in enumerate(queries):
            cur.execute(stmt(_vec_literal(q)))
            rows = [r[0] for r in cur.fetchall()]
            ids[i, :len(rows)] = rows
    return ids


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", default=os.path.expanduser("~/data/corpus.fbin"))
    ap.add_argument("--queries", default=os.path.expanduser("~/data/queries_10k.fbin"))
    ap.add_argument("--scratch", default=os.path.expanduser("~/scratch/issue162"))
    ap.add_argument("--dbname", default="halfvec_recall")
    ap.add_argument("--index-dir", default="/tmp/cuvs_indexes")
    ap.add_argument("--n", type=int, default=100_000)
    ap.add_argument("--nq", type=int, default=1000)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--gt-k", type=int, default=100)
    ap.add_argument("--ef", type=int, nargs="+", default=[40, 100])
    ap.add_argument("--dims", type=int, nargs="+", default=[1536, 3072])
    ap.add_argument("--graph-degree", type=int, default=64)
    ap.add_argument("--intermediate-graph-degree", type=int, default=128)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    os.makedirs(args.scratch, exist_ok=True)
    gt_path = os.path.join(args.scratch, f"gt_{args.n}_{args.nq}_{args.gt_k}.npy")

    queries768 = np.ascontiguousarray(read_fbin(args.queries, count=args.nq))
    if os.path.exists(gt_path):
        gt = np.load(gt_path)
        print(f"[gt] reusing {gt_path}", flush=True)
    else:
        t0 = time.perf_counter()
        corpus768 = read_fbin(args.corpus, count=args.n)
        gt = brute_force_gt(corpus768, queries768, args.gt_k)
        np.save(gt_path, gt)
        print(f"[gt] brute force {args.n}x{BASE_DIM} in "
              f"{time.perf_counter()-t0:.1f}s -> {gt_path}", flush=True)

    import psycopg
    import pgvector.psycopg
    admin = psycopg.connect(dbname="postgres", autocommit=True)
    admin.execute(f'DROP DATABASE IF EXISTS "{args.dbname}"')
    admin.execute(f'CREATE DATABASE "{args.dbname}"')
    admin.close()
    conn = psycopg.connect(dbname=args.dbname, autocommit=True)
    conn.execute("CREATE EXTENSION vector")
    conn.execute("CREATE EXTENSION pg_cuvs")
    pgvector.psycopg.register_vector(conn)
    ver = conn.execute("SELECT extversion FROM pg_extension "
                       "WHERE extname='pg_cuvs'").fetchone()[0]
    print(f"[env] pg_cuvs {ver}", flush=True)

    results = []
    for dim in args.dims:
        if dim % BASE_DIM:
            raise SystemExit(f"dim {dim} is not a multiple of {BASE_DIM}")
        mult = dim // BASE_DIM
        load_table(conn, args.corpus, args.n, dim, mult)
        t_cagra, t_f32, t_hv, f32_error = build_indexes(
            conn, dim, args.index_dir, args.graph_degree,
            args.intermediate_graph_degree)
        queries = expand(queries768, mult)
        for ef in args.ef:
            row = {"dim": dim, "n": args.n, "nq": args.nq, "k": args.k,
                   "ef_search": ef, "build_s_cagra": round(t_cagra, 2),
                   "build_s_hnsw_fp32": None if t_f32 is None else round(t_f32, 2),
                   "build_s_hnsw_halfvec": round(t_hv, 2),
                   "fp32_build_error": f32_error}
            arms = ("halfvec",) if f32_error else ("f32", "halfvec")
            for arm in arms:
                t0 = time.perf_counter()
                ids = search(conn, queries, dim, args.k, ef, arm)
                row[f"recall_{arm}"] = recall_at_k(ids, gt, args.k)
                row[f"qps_{arm}"] = round(args.nq / (time.perf_counter() - t0), 1)
            row["delta"] = (None if f32_error
                            else row["recall_f32"] - row["recall_halfvec"])
            print("[result] " + json.dumps(row), flush=True)
            results.append(row)

    out = args.out or os.path.join(args.scratch, "results.json")
    with open(out, "w") as f:
        json.dump(results, f, indent=2)
    print(f"[done] -> {out}", flush=True)

    print("\ndim   ef   recall@10 fp32   recall@10 halfvec   delta")
    for r in results:
        f32 = "n/a (unbuildable)" if r["delta"] is None else f"{r['recall_f32']:.4f}"
        d = "n/a" if r["delta"] is None else f"{r['delta']:+.4f}"
        print(f"{r['dim']:<5} {r['ef_search']:<4} {f32:<15} "
              f"{r['recall_halfvec']:<19.4f} {d}")


if __name__ == "__main__":
    main()
