#!/usr/bin/env python3
"""
adr079_3o_recall.py -- #76 / ADR-079: measure the 3O recall ceiling.

3O = global CAGRA graph + GPU BITSET prefilter. VecFlow (arXiv:2506.00812,
SIGMOD 2026, by cuVS engineers) reports that this architecture plateaus near
80% recall and collapses under highly selective filters. pg_cuvs ships 3O but
had never measured that failure mode on its own implementation.

This sweeps filter selectivity and records recall@k for three filtered paths:

  3O       cuvs.filter_auto_threshold = 1.0  -> GPU BITSET prefilter over the
           CAGRA graph (daemon search_mode 4 = cagra_prefilter)
  D-wedge  cuvs.filter_auto_threshold = 0    -> BF candidate prefix followed by
           whitelist post-filtering (ADR-063)
  stream   cuvs.stream_bf_selectivity_threshold = 1.0 -> exact BF over only
           whitelist members (ADR-064)

MEASUREMENT HONESTY
  Ground truth is computed here in numpy -- exact top-k over the *filtered*
  subset -- not taken from pg_cuvs. A systematic engine error therefore cannot
  hide inside the reference. The daemon's own search_mode is read back from
  pg_stat_gpu_search and reported, so a run that silently fell back to the
  BF prefilter (mode 3), D-wedge, or stream BF is visible rather than being
  reported as a different route.

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
from numpy.typing import NDArray

from adr079_reuse import HASH_MOD, KNUTH, corpus_fingerprint

SELECTIVITIES = (0.5, 0.1, 0.05, 0.01, 0.005, 0.001)


def read_fbin(path, count=None, offset=0):
    with open(path, "rb") as f:
        n, dim = struct.unpack("<ii", f.read(8))
    if count is None:
        count = n - offset
    return np.memmap(path, dtype=np.float32, mode="r",
                     offset=8 + offset * dim * 4, shape=(count, dim))


def exact_topk_in_subset(
        base: NDArray[np.float32],
        subset_idx: NDArray[np.int64],
        queries: NDArray[np.float32],
        k: int,
        chunk: int = 64) -> NDArray[np.int64]:
    """Exact top-k row indices (into the full corpus) restricted to subset_idx.

    L2 ranking only needs ||b||^2 - 2 q.b; the ||q||^2 term is constant per
    query and cannot change the ordering, so it is dropped.
    """
    sub = np.ascontiguousarray(base[subset_idx])
    bn = (sub * sub).sum(1)
    top = min(k, len(subset_idx))
    out = np.empty((len(queries), top), dtype=np.int64)
    if top == 0:
        return out
    for s in range(0, len(queries), chunk):
        e = min(s + chunk, len(queries))
        d = bn[None, :] - 2.0 * (queries[s:e] @ sub.T)
        part = np.argpartition(d, top - 1, axis=1)[:, :top]
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
    ap.add_argument("--selectivities", default=None,
                    help="comma-separated selectivity grid; default "
                         + ",".join(str(x) for x in SELECTIVITIES))
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

    def _reusable():
        """Is the existing f3o the corpus this run is about to measure?

        A bare "does a relation named f3o exist" check will happily benchmark a
        leftover table from a different --n, dimension or dataset while ground
        truth still comes from the requested files -- producing results that look
        valid and are not. Verify shape, size, the index, and the actual vectors."""
        row = conn.execute(
            "SELECT to_regclass('public.f3o') IS NOT NULL,"
            "       (SELECT a.atttypmod FROM pg_attribute a"
            "          WHERE a.attrelid = to_regclass('public.f3o')"
            "            AND a.attname = 'embedding'),"
            "       EXISTS ("
            "         SELECT 1"
            "         FROM pg_index i"
            "         JOIN pg_class idx ON idx.oid = i.indexrelid"
            "         JOIN pg_namespace idx_ns ON idx_ns.oid = idx.relnamespace"
            "         JOIN pg_class tbl ON tbl.oid = i.indrelid"
            "         JOIN pg_namespace tbl_ns ON tbl_ns.oid = tbl.relnamespace"
            "         JOIN pg_am am ON am.oid = idx.relam"
            "         JOIN pg_attribute a"
            "           ON a.attrelid = tbl.oid AND a.attnum = i.indkey[0]"
            "         JOIN pg_opclass opc ON opc.oid = i.indclass[0]"
            "         WHERE tbl_ns.nspname = 'public' AND tbl.relname = 'f3o'"
            "           AND idx_ns.nspname = 'public' AND idx.relname = 'f3o_cagra'"
            "           AND i.indrelid = to_regclass('public.f3o')"
            "           AND i.indexrelid = to_regclass('public.f3o_cagra')"
            "           AND am.amname = 'cagra'"
            "           AND i.indnatts = 1"
            "           AND a.attname = 'embedding'"
            "           AND opc.opcname = 'vector_l2_ops'"
            "           AND i.indisvalid AND i.indisready)"
        ).fetchone()
        if not row[0]:
            return False, "table public.f3o missing"
        if row[1] != dim:
            return False, f"dim {row[1]} != corpus dim {dim}"
        if not row[2]:
            return False, "public.f3o_cagra contract mismatch"
        n_rows, stored_fingerprint = conn.execute(
            "SELECT count(*),"
            "       md5(string_agg("
            "         md5(int8send(id) || int4send(cat) || vector_send(embedding)),"
            "         '' ORDER BY id))"
            " FROM public.f3o"
        ).fetchone()
        if n_rows != args.n:
            return False, f"{n_rows} rows != --n {args.n}"
        expected_fingerprint = corpus_fingerprint(base, args.n)
        if stored_fingerprint != expected_fingerprint:
            return False, f"corpus/id/cat fingerprint does not match {corpus}"
        return True, "verified"

    reuse = False
    if args.reuse_table:
        reuse, why = _reusable()
        print(f"[setup] --reuse-table: {'reusing' if reuse else 'rebuilding'} ({why})",
              flush=True)
    if not reuse:
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

    grid = ([float(x) for x in args.selectivities.split(",")]
            if args.selectivities else list(SELECTIVITIES))
    rows = []
    for sel in grid:
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

        # (label, cuvs.filter_auto_threshold, cuvs.stream_bf_selectivity_threshold).
        # stream_bf takes precedence over the 3O prefilter when both would fire.
        for path, fthr, sthr in (("3O", 1.0, 0.0),
                                 ("D-wedge", 0.0, 0.0),
                                 ("stream_bf", 0.0, 1.0)):
            conn.execute(f"SET cuvs.stream_bf_selectivity_threshold = {sthr}")
            conn.execute(f"SET cuvs.filter_auto_threshold = {fthr}")
            got, lat, modes = [], [], []
            for qi in range(args.queries):
                q0 = time.perf_counter()
                res = conn.execute(
                    "SELECT ctid::text FROM cuvs_filtered_knn("
                    "  'f3o_cagra'::regclass, %s::vector,"
                    "  (SELECT tids FROM filterset WHERE sel = %s), %s)",
                    (Vector(queries[qi]), sel, args.k)).fetchall()
                lat.append((time.perf_counter() - q0) * 1000.0)
                got.append([ctid_of_id.get(encode_ctid(r[0]), -1) for r in res])
                # Per query: the daemon silently falls back (3O -> exact BF
                # prefilter -> D-wedge; stream_bf -> 3O when the sidecar is
                # absent). Reading the mode once at the end lets one final
                # successful query hide every earlier fallback, which would make
                # a point-level "this measured 3O" claim unsupportable.
                m = conn.execute(
                    "SELECT search_mode FROM pg_stat_gpu_search "
                    "WHERE index_oid = 'f3o_cagra'::regclass").fetchone()
                modes.append(m[0] if m else None)
            seen = sorted(set(modes), key=lambda x: (x is None, x))
            mode = (seen[0],) if len(seen) == 1 else ("MIXED:" + ",".join(
                f"{x}x{modes.count(x)}" for x in seen),)
            if len(seen) > 1:
                print(f"  [warn] {path} sel={actual_sel:.4f} mixed daemon routes: "
                      f"{mode[0]} -- point-level attribution is not uniform",
                      flush=True)
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
