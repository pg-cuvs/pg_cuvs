#!/usr/bin/env python3
"""
pg_engine.py -- the pg_cuvs / pgvector benchmark engine.

Factored out of infra/anbench/run_pg{,_3i}.py so it can back a cuvs-bench
pluggable backend (which splits build() and search() into separate calls) AND
run standalone for validation. cuvs-bench ships no PostgreSQL/pgvector backend,
so this exposes pg_cuvs and pgvector side by side under one tool, on the same
data + ground truth + methodology.

Algos (one Postgres backend, several algos):
  pgvector_hnsw       pgvector CPU HNSW    (ef_search swept at search time)
  pgvector_ivfflat    pgvector CPU IVFFlat (probes swept at search time)
  pgcuvs_cagra        pg_cuvs GPU CAGRA resident search (cuvs.k swept)
  pgcuvs_hnsw_import  pg_cuvs 3I: GPU CAGRA build -> pgvector HNSW export,
                      CPU HNSW search (ef_search swept)

All use vector_l2_ops on L2-normalized vectors (L2-NN == cosine ranking).
Table t(id, embedding): id == corpus row index, so returned ids map directly
into the ground-truth id space (this prevents the recall==0 id-space bug seen
in the old 50M run).

MEASUREMENT BOUNDARY (important; also in README.md):
  Search latency here is the FULL psql round-trip per query:
    client -> PG backend -> [pg_cuvs: shm IPC + GPU kernel] -> heap fetch -> client
  i.e. what a PostgreSQL application actually experiences. This is deliberately
  NOT the in-process C++ kernel time that cuVS's native cuvs-bench backends
  report. Numbers are apples-to-apples ACROSS these Postgres algos (and vs
  pgvector), but NOT 1:1 comparable to cuVS's own C++ backend rows.
"""
import argparse
import csv
import struct
import sys
import time

import numpy as np

INDEX_DIR_DEFAULT = "/tmp/cuvs_indexes"

ALGOS = ("pgvector_hnsw", "pgvector_ivfflat", "pgcuvs_cagra", "pgcuvs_hnsw_import")

# Default per-algo parameter sweeps (the recall/latency knob). Each value is one
# point on that algo's recall-vs-latency curve; pg_cuvs CAGRA sweeps cuvs.k so
# it gets a real curve rather than a single point.
DEFAULT_SWEEPS = {
    "pgvector_hnsw":      [10, 20, 40, 80, 120, 200, 400],   # hnsw.ef_search
    "pgvector_ivfflat":   [1, 4, 8, 16, 32, 64, 128],        # ivfflat.probes
    "pgcuvs_cagra":       [16, 32, 64, 100, 200, 400],       # cuvs.k
    "pgcuvs_hnsw_import": [16, 32, 64, 128, 256, 512],       # hnsw.ef_search
}


# ── fbin / recall helpers (vendored so the backend is self-contained) ────────
def read_fbin(path, count=None, offset=0):
    """big-ann .fbin: int32 n, int32 dim, then n*dim float32 row-major.
    Returns an (count, dim) float32 memmap view."""
    with open(path, "rb") as f:
        n, dim = struct.unpack("<ii", f.read(8))
    if count is None:
        count = n - offset
    return np.memmap(path, dtype=np.float32, mode="r",
                     offset=8 + offset * dim * 4, shape=(count, dim))


def fbin_meta(path):
    with open(path, "rb") as f:
        n, dim = struct.unpack("<ii", f.read(8))
    return n, dim


def recall_at_k(returned_ids, gt_ids, k):
    """Mean |returned[:k] ∩ gt[:kk]| / kk over queries, in the same id space,
    where kk = min(k, gt columns) -- matches cuvs-bench's compute_recall."""
    gt = np.asarray(gt_ids)
    gk = min(k, gt.shape[1])
    r = np.asarray(returned_ids)[:, :k]
    g = gt[:, :gk]
    hits = 0
    for a, b in zip(r, g):
        hits += len(set(a.tolist()) & set(b.tolist()))
    return hits / (r.shape[0] * gk)


def percentiles_ms(lat_s):
    a = np.asarray(lat_s, dtype=np.float64) * 1000.0
    if a.size == 0:
        return (float("nan"),) * 3
    return (float(np.percentile(a, 50)),
            float(np.percentile(a, 95)),
            float(np.percentile(a, 99)))


def _vec_literal(v):
    # inline literal (not a bind param) so every algo runs an identical
    # statement shape, exactly as infra/anbench/run_pg.py does.
    return "[" + ",".join(repr(float(x)) for x in v.tolist()) + "]"


# ── the engine ───────────────────────────────────────────────────────────────
class PgEngine:
    """One connection, one table t; build() an index, then search() sweeps.

    build() drops all other ANN indexes first (so the planner can't pick a
    leftover), builds the target, and returns (build_time_s, index_bytes).
    search(param) sets the algo's knob and runs the query set one statement at a
    time. Recall is computed by the caller against ground truth.

    NOTE on the daemon: pg_cuvs algos (pgcuvs_cagra) need the pg-cuvs-server
    daemon UP; pgvector algos need it DOWN for VRAM-fair CPU baselines. That
    toggling is systemctl (outside SQL) and is the orchestrator's job, not the
    engine's -- see run_cohere.sh restart_daemon/stop_daemon, mirrored by the
    cuvs-bench backend and the standalone main() below."""

    ANN_INDEXES = ("t_hnsw", "t_ivf", "t_cagra")

    def __init__(self, dbname="postgres", index_dir=INDEX_DIR_DEFAULT):
        import psycopg
        # autocommit from the start: building a cagra index inside an explicit
        # transaction block corrupts the backend so later cagra searches crash
        # the connection (run_pg.py). Autocommit is the working + realistic path.
        self.conn = psycopg.connect(dbname=dbname, autocommit=True)
        self.conn.execute("CREATE EXTENSION IF NOT EXISTS vector")
        self.conn.execute("CREATE EXTENSION IF NOT EXISTS pg_cuvs")
        import pgvector.psycopg
        pgvector.psycopg.register_vector(self.conn)
        self.index_dir = index_dir

    # -- data ------------------------------------------------------------------
    def load_corpus(self, corpus_path, n, dim, batch=50_000):
        from pgvector import Vector
        with self.conn.cursor() as cur:
            cur.execute("SELECT to_regclass('public.t')")
            if cur.fetchone()[0] is not None:
                cur.execute("SELECT count(*) FROM public.t")
                got = cur.fetchone()[0]
                if got != n:
                    # #78: at 1M scale this intermittently reports 0 between
                    # configs. Measured facts so far: the heap really is empty
                    # (parallel seq scan returns rows=0 in ~17 ms), relfilenode is
                    # unchanged, n_tup_del is 0, the orchestrator loop is strictly
                    # sequential, an autocommit COPY is visible to other
                    # connections immediately, and neither pg_cuvs_build_hnsw nor
                    # a cagra CREATE INDEX empties the heap at 50k. So re-loading
                    # is the correct response -- what is unknown is what empties
                    # the table. Dump the state here so the next occurrence
                    # carries its own evidence instead of just a lost 2 minutes.
                    cur.execute(
                        "SELECT pg_relation_filenode('public.t'), "
                        "       pg_relation_size('public.t'), "
                        "       (SELECT n_tup_ins FROM pg_stat_all_tables "
                        "         WHERE relid = 'public.t'::regclass), "
                        "       (SELECT n_tup_del FROM pg_stat_all_tables "
                        "         WHERE relid = 'public.t'::regclass)")
                    fnode, relsize, ins, dele = cur.fetchone()
                    print(f"[engine] #78 corpus reload: count={got} want={n} "
                          f"filenode={fnode} relsize={relsize} "
                          f"n_tup_ins={ins} n_tup_del={dele}", flush=True)
                if got == n:
                    print(f"[engine] table t already has {n} rows; reuse", flush=True)
                    return
                cur.execute("DROP TABLE t CASCADE")
            cur.execute(f"CREATE TABLE t (id bigint, embedding vector({dim}))")
        t0 = time.perf_counter()
        with self.conn.cursor().copy(
                "COPY t (id, embedding) FROM STDIN WITH (FORMAT BINARY)") as cp:
            cp.set_types(["int8", "vector"])
            for s in range(0, n, batch):
                e = min(s + batch, n)
                chunk = np.ascontiguousarray(read_fbin(corpus_path, count=e - s, offset=s))
                for i in range(e - s):
                    cp.write_row((s + i, Vector(chunk[i])))
        print(f"[engine] COPY {n} rows in {time.perf_counter()-t0:.1f}s", flush=True)

    def _drop_ann_indexes(self):
        with self.conn.cursor() as cur:
            for nm in self.ANN_INDEXES:
                cur.execute("DROP INDEX IF EXISTS " + nm)

    def _relsize(self, name):
        with self.conn.cursor() as cur:
            cur.execute(f"SELECT pg_relation_size('{name}')")
            return cur.fetchone()[0]

    def _vram_bytes(self, name):
        """Daemon-resident VRAM footprint (bytes) for a GPU-resident index.

        A CAGRA/flat graph lives in the sidecar daemon's VRAM, not a Postgres
        relation, so pg_relation_size() returns 0 for it. pg_stat_gpu_search
        exposes the daemon's self-accounted vram_bytes, populated at build time
        (no search required). Returns None if the daemon is down or the index is
        not resident (empty result set)."""
        with self.conn.cursor() as cur:
            cur.execute(
                "SELECT vram_bytes FROM pg_stat_gpu_search "
                "WHERE index_oid = %s::regclass",
                (name,))
            row = cur.fetchone()
        return int(row[0]) if row and row[0] is not None else None

    # -- build -----------------------------------------------------------------
    def build(self, algo, n, sample_query=None):
        """Build the index for `algo`. Returns (build_time_s, index_bytes).
        sample_query (1-D float array) is REQUIRED for pgcuvs_hnsw_import: a
        dummy search triggers the lazy .hnsw sidecar serialization needed by
        pg_cuvs_import_hnsw."""
        assert algo in ALGOS, f"unknown algo {algo}"
        c = self.conn
        self._drop_ann_indexes()
        c.execute("SET maintenance_work_mem = '16GB'")
        c.execute("SET max_parallel_maintenance_workers = 7")

        if algo == "pgvector_hnsw":
            t0 = time.perf_counter()
            c.execute("CREATE INDEX t_hnsw ON t USING hnsw (embedding vector_l2_ops) "
                      "WITH (m=16, ef_construction=64)")
            return time.perf_counter() - t0, self._relsize("t_hnsw")

        if algo == "pgvector_ivfflat":
            lists = max(1, int(4 * (n ** 0.5)))
            t0 = time.perf_counter()
            c.execute(f"CREATE INDEX t_ivf ON t USING ivfflat (embedding vector_l2_ops) "
                      f"WITH (lists={lists})")
            return time.perf_counter() - t0, self._relsize("t_ivf")

        if algo == "pgcuvs_cagra":
            c.execute(f"SET cuvs.index_dir = '{self.index_dir}'")
            c.execute("SET maintenance_work_mem = '8GB'")
            t0 = time.perf_counter()
            c.execute("CREATE INDEX t_cagra ON t USING cagra (embedding vector_l2_ops)")
            bt = time.perf_counter() - t0
            # CAGRA graph is VRAM-resident (pg_relation_size == 0); report the
            # daemon's self-accounted VRAM footprint instead (issue #75).
            vram = self._vram_bytes("t_cagra")
            if vram is None:
                print("[engine] WARN: no pg_stat_gpu_search row for t_cagra "
                      "(daemon down / not resident); index_bytes falls back to 0",
                      flush=True)
                vram = self._relsize("t_cagra")
            return bt, vram

        # pgcuvs_hnsw_import (3I): GPU CAGRA build -> pg_cuvs_build_hnsw(cagra, mode).
        # The unified 0.5.0 API (ADR-037) creates the pgvector HNSW index directly
        # from the CAGRA graph via INDEX_CREATE_SKIP_BUILD (no 285s CPU build) and
        # RETURNS the new index's regclass. It replaces the removed two-step
        # pg_cuvs_import_hnsw(cagra, hnsw). mode 'nsw' = flat level-0 NSW (recommended
        # default). build_time = CAGRA build + HNSW export (the GPU-build-accelerator
        # figure). sample_query is unused now (no dummy fallback search needed).
        c.execute(f"SET cuvs.index_dir = '{self.index_dir}'")
        c.execute("SET maintenance_work_mem = '8GB'")
        t0 = time.perf_counter()
        c.execute("CREATE INDEX t_cagra ON t USING cagra (embedding vector_l2_ops)")
        t_cagra = time.perf_counter() - t0
        t1 = time.perf_counter()
        with c.cursor() as cur:
            cur.execute("SELECT pg_cuvs_build_hnsw('t_cagra'::regclass, 'nsw')::regclass::text")
            gen = cur.fetchone()[0]
            assert gen is not None, "pg_cuvs_build_hnsw returned NULL"
        t_build = time.perf_counter() - t1
        # normalize the generated index name to t_hnsw so _drop_ann_indexes and the
        # planner-driven search path stay uniform across algos.
        if str(gen).split(".")[-1] != "t_hnsw":
            c.execute(f"ALTER INDEX {gen} RENAME TO t_hnsw")
        return t_cagra + t_build, self._relsize("t_hnsw")

    # -- search ----------------------------------------------------------------
    def search(self, algo, queries, kmax, param, warmup=200):
        """Run the query set one statement at a time under `param`. Returns
        (ids ndarray (nq, kmax), latencies list). Caller computes recall/QPS."""
        c = self.conn
        nq = len(queries)
        with c.cursor() as cur:
            if algo == "pgvector_hnsw":
                cur.execute("SET enable_seqscan = off")
                cur.execute(f"SET hnsw.ef_search = {param}")
            elif algo == "pgvector_ivfflat":
                cur.execute("SET enable_seqscan = off")
                cur.execute(f"SET ivfflat.probes = {param}")
            elif algo == "pgcuvs_cagra":
                cur.execute("SET enable_seqscan = off")
                cur.execute(f"SET cuvs.index_dir = '{self.index_dir}'")
                cur.execute(f"SET cuvs.k = {param}")
            elif algo == "pgcuvs_hnsw_import":
                cur.execute("SET enable_cuvs = off")     # search CPU HNSW, not GPU
                cur.execute("SET enable_seqscan = off")
                cur.execute(f"SET hnsw.ef_search = {param}")

            # Guard: refuse to report a fake exact-seqscan result. If the ANN
            # index isn't in the plan (build failed / cuvs.index_dir mis-set /
            # daemon down), pgvector & pg_cuvs both fall back to an exact Seq
            # Scan that returns recall~=1.0 at huge latency -- a silently-wrong
            # benchmark row. Any Seq Scan on t here is disqualifying.
            probe = ("SELECT id FROM t ORDER BY embedding <-> "
                     f"'{_vec_literal(queries[0])}'::vector LIMIT {kmax}")
            cur.execute("EXPLAIN (FORMAT TEXT) " + probe)
            plan = "\n".join(r[0] for r in cur.fetchall())
            if "Seq Scan" in plan:
                raise RuntimeError(
                    f"{algo} param={param}: planner chose Seq Scan (ANN index "
                    f"not used) -- refusing to report an exact-scan result.\n"
                    + plan)

            def one(i):
                cur.execute("SELECT id FROM t ORDER BY embedding <-> "
                            f"'{_vec_literal(queries[i])}'::vector LIMIT {kmax}")
                return cur.fetchall()

            for i in range(min(warmup, nq)):
                one(i)
            ids = np.full((nq, kmax), -1, dtype=np.int64)
            lat = []
            for i in range(nq):
                t1 = time.perf_counter()
                rows = one(i)
                lat.append(time.perf_counter() - t1)
                for j, r in enumerate(rows):
                    ids[i, j] = r[0]
        return ids, lat

    def close(self):
        try:
            self.conn.close()
        except Exception:
            pass


# ── standalone runner (validate the engine without cuvs-bench) ───────────────
def _daemon(action):
    """restart|stop the pg-cuvs-server daemon (VRAM-fair baselines). Best-effort."""
    import subprocess
    try:
        if action == "restart":
            subprocess.run(["sudo", "systemctl", "restart", "pg-cuvs-server"], check=True)
            for _ in range(20):
                import os
                if os.path.exists("/tmp/.s.pg_cuvs"):
                    return
                time.sleep(1)
        elif action == "stop":
            subprocess.run(["sudo", "systemctl", "stop", "pg-cuvs-server"], check=False)
            time.sleep(2)
    except Exception as e:
        print(f"[engine] daemon {action} warn: {e}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--queries", required=True)
    ap.add_argument("--gt", required=True)
    ap.add_argument("--n", type=int, required=True)
    ap.add_argument("--out", required=True, help="CSV output path")
    ap.add_argument("--algos", default="pgcuvs_cagra,pgvector_hnsw",
                    help="comma list from " + ",".join(ALGOS))
    ap.add_argument("--ks", default="10,100")
    ap.add_argument("--dataset", default="cohere-wiki-en-1024")
    ap.add_argument("--dbname", default="postgres")
    ap.add_argument("--index-dir", default=INDEX_DIR_DEFAULT)
    ap.add_argument("--max-queries", type=int, default=2000)
    ap.add_argument("--toggle-daemon", action="store_true",
                    help="restart daemon for pg_cuvs algos, stop for pgvector (VRAM-fair)")
    args = ap.parse_args()

    ks = [int(x) for x in args.ks.split(",")]
    kmax = max(ks)
    algos = [a.strip() for a in args.algos.split(",") if a.strip()]
    queries = np.ascontiguousarray(read_fbin(args.queries))
    gt = np.load(args.gt)
    _, dim = fbin_meta(args.corpus)
    nq = min(args.max_queries, len(queries))
    qset, gtset = queries[:nq], gt[:nq]

    fields = ["system", "dataset", "N", "dim", "metric", "k", "param_set",
              "build_time_s", "index_bytes", "recall", "qps", "p50_ms", "p95_ms",
              "p99_ms", "n_queries", "notes"]
    fout = open(args.out, "w", newline="")
    writer = csv.DictWriter(fout, fieldnames=fields)
    writer.writeheader()

    for algo in algos:
        is_gpu = algo in ("pgcuvs_cagra", "pgcuvs_hnsw_import")
        if args.toggle_daemon:
            _daemon("restart" if is_gpu else "stop")
        eng = PgEngine(dbname=args.dbname, index_dir=args.index_dir)
        eng.load_corpus(args.corpus, args.n, dim)
        print(f"[engine] build {algo} ...", flush=True)
        bt, ibytes = eng.build(algo, args.n, sample_query=qset[0])
        print(f"[engine] {algo} build {bt:.1f}s size {ibytes/1e6:.0f}MB", flush=True)
        for param in DEFAULT_SWEEPS[algo]:
            if algo in ("pgvector_hnsw", "pgcuvs_hnsw_import") and param < kmax:
                continue
            ids, lat = eng.search(algo, qset, kmax, param)
            p50, p95, p99 = percentiles_ms(lat)
            qps = nq / sum(lat)
            for k in ks:
                rec = recall_at_k(ids[:, :k], gtset[:, :k], k)
                writer.writerow(dict(
                    system=algo, dataset=args.dataset, N=args.n, dim=dim,
                    metric="cosine(L2-normed)", k=k, param_set=str(param),
                    build_time_s=round(bt, 3), index_bytes=ibytes,
                    recall=round(rec, 4), qps=round(qps, 1),
                    p50_ms=round(p50, 3), p95_ms=round(p95, 3), p99_ms=round(p99, 3),
                    n_queries=nq, notes=""))
                fout.flush()
                print(f"[result] {algo} k={k} {param} recall={rec:.4f} "
                      f"qps={qps:.0f} p50={p50:.2f}ms", flush=True)
        eng.close()
    fout.close()
    print(f"[engine] DONE -> {args.out}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
