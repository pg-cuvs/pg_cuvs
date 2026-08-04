#!/usr/bin/env python3
"""
pg_engine.py -- the pg_cuvs / pgvector benchmark engine.

Factored out of bench/legacy/anbench/run_pg{,_3i}.py so it can back a cuvs-bench
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
import hashlib
import os
import struct
import sys
import time

import numpy as np

# adr079_reuse.corpus_fingerprint lives beside bench/filter_recall/*.py -- reuse
# its md5-of-ordered-row-md5 gate machinery (#78) rather than reimplementing it.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "..", "filter_recall"))
from adr079_reuse import corpus_fingerprint  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sidecar import (  # noqa: E402
    ALL_RELATIONS,
    CAGRA_BUILD_ALGO,
    INTERMEDIATE_GRAPH_DEGREE,
    RELATION_OF,
    SIBLING_OF,
    SOURCE_RELATION,
    parse_build_notices,
    parse_reloptions,
)

INDEX_DIR_DEFAULT = "/tmp/cuvs_indexes"

ALGOS = ("pgvector_hnsw", "pgvector_ivfflat", "pgcuvs_cagra", "pgcuvs_hnsw_import")

# The #98 latency axis also carries a non-Postgres algo, `cuvs` (raw cuVS CAGRA,
# cuvs_engine.CuvsEngine). It is deliberately NOT in ALGOS: ALGOS is what
# PgEngine.build() asserts against and what the standalone runners offer, and
# PgEngine cannot build or search a raw index. It IS in DEFAULT_SWEEPS, which is
# the harness-wide sweep table that both backend.py's loader and
# run_pg_cuvsbench.py index by algo name (a missing entry there is a KeyError,
# not a graceful skip).
RAW_ALGOS = ("cuvs",)
LATENCY_ALGOS = ALGOS + RAW_ALGOS

# Default per-algo parameter sweeps (the recall/latency knob). Each value is one
# point on that algo's recall-vs-latency curve; pg_cuvs CAGRA sweeps cuvs.k so
# it gets a real curve rather than a single point.
DEFAULT_SWEEPS = {
    "pgvector_hnsw":      [10, 20, 40, 80, 120, 200, 400],   # hnsw.ef_search
    "pgvector_ivfflat":   [1, 4, 8, 16, 32, 64, 128],        # ivfflat.probes
    "pgcuvs_cagra":       [16, 32, 64, 100, 200, 400],       # cuvs.k
    "pgcuvs_hnsw_import": [16, 32, 64, 128, 256, 512],       # hnsw.ef_search
    # Identical to pgcuvs_cagra's, and that identity is load-bearing: the raw
    # arm exists to be compared with pgcuvs_cagra point for point within the
    # latency axis, which only holds if both sweep the same knob over the same
    # values. Change one and you must change the other.
    "cuvs":               [16, 32, 64, 100, 200, 400],       # GPU candidate count
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
    # statement shape, exactly as bench/legacy/anbench/run_pg.py does.
    return "[" + ",".join(repr(float(x)) for x in v.tolist()) + "]"


def truncate_topk(rows, nq, top_k=10):
    """(query_idx, id, distance) rows -> an (nq, top_k) id array.

    The batch arm asks the daemon for K neighbours per query (K = the Pareto
    point's cuvs.k, up to 400) and scores recall@10, so the top-10 cut happens
    here, client-side. The SQL is ORDER BY query_idx, distance, so "the first
    top_k rows of each query_idx" IS that query's top-10 -- this function must
    not re-sort, or it would paper over a statement that lost its ordering.

    Missing slots stay -1 (a query that returned fewer than top_k rows scores
    those slots as misses rather than as some other query's neighbour).
    """
    ids = np.full((nq, top_k), -1, dtype=np.int64)
    seen = [0] * nq
    for qi, rid, _dist in rows:
        qi = int(qi)
        if not (0 <= qi < nq):
            continue
        s = seen[qi]
        if s < top_k:
            ids[qi, s] = rid
            seen[qi] = s + 1
    return ids


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

    # #98: one relation per algo (sidecar.RELATION_OF). The pre-#98 list mapped
    # both pgvector_hnsw and 3I onto a single "t_hnsw", so a reuse gate could
    # not tell whose index was resident.
    ANN_INDEXES = ALL_RELATIONS
    CORPUS_MARKER = "public._bench_corpus"
    # #78 review F3: string_agg() of one 32-byte md5 hex digest per row hits
    # PostgreSQL's 1GB varlena limit at 1_073_741_824 // 32 ~= 33.5M rows --
    # this repo has already published a 50M arm. Above this many rows, the
    # post-COPY verify folds into per-chunk digests + a rehash on both the SQL
    # and Python side instead of one aggregate over the whole table.
    VERIFY_CHUNK = 1_000_000
    # pgvector build params (build() and build_time_arm.py's label both derive
    # from these -- #78 review F8: a second hardcoded copy would drift silently).
    HNSW_M = 16
    HNSW_EF_CONSTRUCTION = 64

    @staticmethod
    def ivfflat_lists(n):
        return max(1, int(4 * (n ** 0.5)))

    def __init__(self, dbname="postgres", index_dir=INDEX_DIR_DEFAULT):
        import psycopg
        # autocommit from the start: building a cagra index inside an explicit
        # transaction block corrupts the backend so later cagra searches crash
        # the connection (run_pg.py). Autocommit is the working + realistic path.
        self.conn = psycopg.connect(dbname=dbname, autocommit=True)
        self.conn.execute("CREATE EXTENSION IF NOT EXISTS vector")
        try:
            self.conn.execute("CREATE EXTENSION IF NOT EXISTS pg_cuvs")
        except Exception as e:  # noqa: BLE001
            # pg_cuvs is a GPU extension; a CPU-only PostgreSQL (no CUDA/libcuvs
            # installed) won't have it. Only pgcuvs_* algos touch it -- defer the
            # failure to whichever pgcuvs_* call actually needs it, so
            # pgvector-only usage (e.g. build_time_arm.py's CPU-only path) works
            # on a box without pg_cuvs (#78 review F5).
            print(f"[engine] WARN: CREATE EXTENSION pg_cuvs failed ({e!r}); "
                  f"pgcuvs_* algos will fail later if used", flush=True)
        import pgvector.psycopg
        pgvector.psycopg.register_vector(self.conn)
        self.index_dir = index_dir
        # #98: the 3I build's only self-describing evidence (M, graph_degree,
        # max_level, mode) is a server NOTICE -- psycopg drops those unless a
        # handler is attached, so attach one for the connection's whole life and
        # let build() snapshot it. Diagnostic objects are not retained (they are
        # only valid inside the callback); the message text is.
        self._notices = []
        self.conn.add_notice_handler(
            lambda diag: self._notices.append(str(diag.message_primary)))

    # -- data ------------------------------------------------------------------
    def _dump_78_evidence(self, cur, got, n):
        """#78: count(*) intermittently reported 0 between configs. RESOLVED --
        nothing was emptying the table. Product-side cause tracked as #141.

        `SELECT count(*) FROM t` needs no columns, so the planner may answer it
        from ANY index on t, and it costed a pg_cuvs index at the table's full
        row count. But that AM returns zero tuples for an unqualified scan
        (src/pg_cuvs.c:3540-3542 returns false when numberOfOrderBys < 1), so
        the aggregate counted nothing:

            Aggregate  (cost=… rows=1)
              ->  Index Only Scan using t_cagra on t  (rows=100000 est) (actual rows=0)

        Measured on the VM during the Stage-1 rehearsal: count(*) = 0 while the
        same count with enable_indexscan/indexonlyscan/bitmapscan off returned
        100000, n_tup_del was 0, and pg_relation_size(t) was consistent with a
        full table (vector(768) rows are TOASTed, so the heap holds pointers).
        The intermittency was simply whether a pg_cuvs index happened to be
        resident -- which is why it only ever appeared between configs and never
        reproduced in the 50k isolated test, where none was.

        The gate now counts with those scan methods disabled (see
        _corpus_reusable), so this dump should no longer fire for that reason.
        It stays because a genuinely short table is still possible, and then the
        state it prints is the evidence. NOTE: the underlying AM behaviour is a
        product bug -- `SELECT count(*)` silently returns 0 for any table
        carrying a pg_cuvs index -- and is NOT fixed here. Tracked as #141."""
        cur.execute(
            "SELECT pg_relation_filenode('public.t'), "
            "       pg_relation_size('public.t'), "
            "       (SELECT n_tup_ins FROM pg_stat_all_tables "
            "         WHERE relid = 'public.t'::regclass), "
            "       (SELECT n_tup_del FROM pg_stat_all_tables "
            "         WHERE relid = 'public.t'::regclass)")
        fnode, relsize, ins, dele = cur.fetchone()
        print(f"[engine] #78 corpus reload: marker/count mismatch count={got} "
              f"want={n} filenode={fnode} relsize={relsize} "
              f"n_tup_ins={ins} n_tup_del={dele}", flush=True)

    #: Scan methods that let an index answer a zero-column aggregate. The corpus
    #: gate must not depend on which of them the planner picks.
    _COUNT_SCAN_GUCS = ("enable_indexscan", "enable_indexonlyscan",
                        "enable_bitmapscan")

    def _heap_row_count(self, cur):
        """How many rows `t` actually holds, independent of plan shape.

        A bare `SELECT count(*) FROM t` is not a fact about the data: it needs
        no columns, so the planner may answer it from any index on t, and a
        pg_cuvs index answers an unqualified scan with zero rows (#141; see
        _dump_78_evidence). That made a corpus-integrity gate return "the table
        is empty" for a full table whenever a CAGRA index was resident, and the
        spurious reload that followed dropped the ANN indexes mid-sweep -- one
        build_cfg then recorded two builds and the segment gate aborted the run.

        Counting with the index scan methods disabled forces the seq scan that
        reads the heap itself, which is the only thing the gate is actually
        asking about."""
        saved = {}
        for g in self._COUNT_SCAN_GUCS:
            cur.execute(f"SHOW {g}")
            saved[g] = cur.fetchone()[0]
            cur.execute(f"SET {g} = off")
        try:
            cur.execute("SELECT count(*) FROM public.t")
            return cur.fetchone()[0]
        finally:
            # Restore rather than RESET: the caller's session may legitimately
            # have set these, and this helper must not be the thing that
            # changes how a later measurement is planned.
            for g, v in saved.items():
                cur.execute(f"SET {g} = {v}")

    def _corpus_reusable(self, cur, dataset, corpus_path, n, dim):
        """Cheap per-config gate (#78): a durable marker table describes what
        `t` currently holds. If the marker matches this call's
        (dataset, corpus_path, size, mtime, n, dim) AND `t` still has exactly n
        rows, reuse it -- no full fingerprint recompute here, just a marker
        lookup + a count(*) (a parallel seq scan of a static-size heap costs
        ~17ms at 1M rows when the heap is EMPTY, per the #78 investigation;
        a full 1M-row heap's count(*) is a real scan and costs more, but is
        still one cheap query, not a reload).

        Since `t` is a single table, the marker holds AT MOST ONE row (#78
        review F1): a prior design keyed the marker on `dataset` alone, so it
        accumulated one row per dataset ever seen while `t` itself can only
        ever hold one dataset's data -- a later re-run of an EARLIER dataset
        could then match its own stale marker row and silently reuse `t`
        while it actually held a DIFFERENT dataset's vectors (mis-attributed
        recall/latency in the published CSV). Comparing corpus_path + size +
        mtime (not just the free-text `dataset` label, which callers can set
        to anything) closes the same hole for two different corpus files
        sharing one label."""
        try:
            size = os.path.getsize(corpus_path)
            mtime = os.path.getmtime(corpus_path)
        except OSError:
            return False
        cur.execute(
            f"SELECT fingerprint FROM {self.CORPUS_MARKER} "
            "WHERE dataset = %s AND corpus_path = %s AND corpus_size = %s "
            "  AND corpus_mtime = %s AND n = %s AND dim = %s",
            (dataset, corpus_path, size, mtime, n, dim))
        marker = cur.fetchone()
        if marker is None:
            return False
        cur.execute("SELECT to_regclass('public.t')")
        if cur.fetchone()[0] is None:
            return False
        got = self._heap_row_count(cur)
        if got == n:
            print(f"[engine] table t reused via #78 marker gate "
                  f"(dataset={dataset} n={n} fingerprint={marker[0][:12]}...)",
                  flush=True)
            return True
        self._dump_78_evidence(cur, got, n)
        return False

    def _sql_fingerprint(self, n):
        """The SQL-side fingerprint of what `t` actually holds, matching
        corpus_fingerprint's has_category=False contract. Above VERIFY_CHUNK
        rows, folds into per-chunk digests + a rehash instead of one
        string_agg() over the whole table (#78 review F3: a single aggregate
        would exceed PostgreSQL's 1GB varlena limit well before 50M rows)."""
        with self.conn.cursor() as cur:
            if n <= self.VERIFY_CHUNK:
                cur.execute(
                    "SELECT count(*), md5(string_agg("
                    "  md5(int8send(id) || vector_send(embedding)), '' ORDER BY id))"
                    " FROM public.t")
                return cur.fetchone()
            got_n, chunk_hashes = 0, []
            for s in range(0, n, self.VERIFY_CHUNK):
                e = min(s + self.VERIFY_CHUNK, n)
                cur.execute(
                    "SELECT count(*), md5(string_agg("
                    "  md5(int8send(id) || vector_send(embedding)), '' ORDER BY id))"
                    " FROM public.t WHERE id >= %s AND id < %s",
                    (s, e))
                cnt, fp = cur.fetchone()
                got_n += cnt
                chunk_hashes.append(fp)
            fp = hashlib.md5("".join(chunk_hashes).encode("ascii"),
                             usedforsecurity=False).hexdigest()
            return got_n, fp

    @classmethod
    def _py_fingerprint(cls, corpus_path, n):
        """The Python-side fingerprint of the source file, chunked the same
        way as _sql_fingerprint above VERIFY_CHUNK rows (#78 review F3) so the
        two sides stay directly comparable at any n, including the 50M scale
        this repo has already published."""
        if n <= cls.VERIFY_CHUNK:
            return corpus_fingerprint(
                np.ascontiguousarray(read_fbin(corpus_path, count=n)), n,
                has_category=False)
        chunk_hashes = []
        for s in range(0, n, cls.VERIFY_CHUNK):
            e = min(s + cls.VERIFY_CHUNK, n)
            chunk = np.ascontiguousarray(read_fbin(corpus_path, count=e - s, offset=s))
            chunk_hashes.append(corpus_fingerprint(
                chunk, e - s, has_category=False, id_offset=s))
        return hashlib.md5("".join(chunk_hashes).encode("ascii"),
                           usedforsecurity=False).hexdigest()

    def load_corpus(self, corpus_path, n, dim, dataset="default", batch=50_000,
                     force_reload=False):
        """Load `t` with the first n rows of corpus_path, reusing an existing
        load when possible (#78). Reuse is gated by a durable marker table
        (CORPUS_MARKER) describing what `t` currently holds -- dataset label,
        source corpus_path (+size/mtime), n, dim, and a fingerprint -- so it
        survives the fresh PgEngine/PgBackend instance the cuvs-bench
        orchestrator constructs per BenchmarkConfig. The effect is "load once
        per run" without needing a hook into the orchestrator itself. The
        marker holds at most one row: `t` is a single table, so the marker
        must describe exactly what it holds right now, not accumulate history
        (#78 review F1).

        On an actual (re)load, TRUNCATE + COPY replaces `DROP TABLE ...
        CASCADE` (the pre-#78 behaviour) so any ANN index on `t` survives --
        DROP...CASCADE was destroying it and forcing a spurious rebuild on the
        next config (see issue #78 comment 2026-07-22) -- UNLESS `t` already
        exists with a different `dim`: TRUNCATE cannot repair a typmod
        mismatch (the next COPY would fail), so that case still falls back to
        DROP + CREATE (#78 review F4; there is no index to preserve across a
        dim change anyway, since an index built for the old dim is meaningless
        for the new one).

        force_reload=True bypasses the marker gate unconditionally (test hook:
        deterministically exercise the reload path without waiting for a
        genuine #78 recurrence)."""
        from pgvector import Vector
        with self.conn.cursor() as cur:
            cur.execute(
                f"CREATE TABLE IF NOT EXISTS {self.CORPUS_MARKER} "
                "(dataset text NOT NULL, corpus_path text NOT NULL, "
                " corpus_size bigint NOT NULL, corpus_mtime double precision NOT NULL, "
                " n bigint NOT NULL, dim int NOT NULL, fingerprint text NOT NULL, "
                " loaded_at timestamptz NOT NULL DEFAULT now())")
            if not force_reload and self._corpus_reusable(cur, dataset, corpus_path, n, dim):
                return
            cur.execute("SELECT to_regclass('public.t')")
            exists = cur.fetchone()[0] is not None
            same_dim = False
            if exists:
                cur.execute(
                    "SELECT a.atttypmod FROM pg_attribute a "
                    "WHERE a.attrelid = 'public.t'::regclass AND a.attname = 'embedding'")
                same_dim = cur.fetchone()[0] == dim
            if exists and same_dim:
                # An ANN index left on `t` across a real reload is worse than
                # useless. TRUNCATE + COPY keeps it correct by maintaining it
                # one row at a time, which (a) ran past 15 minutes for 100k rows
                # with a resident HNSW + CAGRA index (measured on the VM; every
                # CAGRA insert is an IPC round trip to the daemon) and (b)
                # leaves an index assembled by insertion while the sidecar still
                # claims the bulk CREATE INDEX time of the previous one -- a
                # build_time attached to an index it does not describe. Drop
                # them and let the ownership gate rebuild. #78's TRUNCATE (over
                # DROP TABLE CASCADE) still does its job: it preserves the index
                # on the path that does NOT reload, which is the early return
                # above and the only place preserving one was ever the point.
                self._drop_ann_indexes()
                cur.execute("TRUNCATE t")
            else:
                if exists:
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

        # Verify the full fingerprint now, once, right after the load (not per
        # config): compare the SQL-side aggregate of what actually landed
        # against the Python-side hash of the source file. A mismatch means
        # the COPY silently produced the wrong contents.
        got_n, sql_fp = self._sql_fingerprint(n)
        if got_n != n:
            raise RuntimeError(f"load_corpus: post-COPY count {got_n} != {n}")
        py_fp = self._py_fingerprint(corpus_path, n)
        if sql_fp != py_fp:
            raise RuntimeError(
                f"load_corpus: fingerprint mismatch after COPY "
                f"(sql={sql_fp} py={py_fp}) -- corpus corrupted in transit")
        with self.conn.cursor() as cur:
            # The marker describes exactly what `t` holds right now -- delete
            # then insert, not upsert-by-dataset, so a stale row from an
            # earlier dataset can never remain alongside the current one
            # (#78 review F1).
            cur.execute(f"DELETE FROM {self.CORPUS_MARKER}")
            cur.execute(
                f"INSERT INTO {self.CORPUS_MARKER} "
                "(dataset, corpus_path, corpus_size, corpus_mtime, n, dim, "
                " fingerprint, loaded_at) "
                "VALUES (%s, %s, %s, %s, %s, %s, %s, now())",
                (dataset, corpus_path, os.path.getsize(corpus_path),
                 os.path.getmtime(corpus_path), n, dim, sql_fp))
        print(f"[engine] corpus fingerprint {sql_fp[:12]}... verified + stored "
              f"(dataset={dataset})", flush=True)

    def _drop_ann_indexes(self, keep=()):
        """Drop every ANN index except those named in `keep`.

        The default (keep=()) is the pre-#98 behaviour -- drop everything -- and
        it is the safe default on purpose: two candidate index paths on the same
        column let the planner choose, so a leftover index turns "what does this
        row measure" into a guess. `keep` is the deliberate opt-in for the
        already-verified co-resident configurations (t_cagra + t_hnsw_pgv).

        One exception is not negotiable: the two hnsw-shaped relations are never
        both resident, so a sibling in `keep` is still dropped (with a warning).
        """
        keep = set(keep)
        with self.conn.cursor() as cur:
            for nm in self.ANN_INDEXES:
                if nm in keep:
                    continue
                cur.execute("DROP INDEX IF EXISTS " + nm)

    def _drop_for(self, algo, keep=()):
        """Drop what must go before building `algo`: its own relation (a rebuild
        replaces it), its hnsw sibling if any, and anything not in `keep`."""
        keep = set(keep)
        target = RELATION_OF[algo]
        keep.discard(target)
        sibling = SIBLING_OF.get(target)
        if sibling and sibling in keep:
            print(f"[engine] WARN: keep={sorted(keep)} asked to retain {sibling} "
                  f"alongside {target}; dropping it anyway (two hnsw indexes on "
                  f"one column make the planner's choice ambiguous)", flush=True)
            keep.discard(sibling)
        if algo == "pgcuvs_hnsw_import":
            # 3I builds its own source graph, so t_cagra is rebuilt too.
            keep.discard(SOURCE_RELATION)
        self._drop_ann_indexes(keep=keep)

    def reloptions(self, name):
        """pg_class.reloptions of `name` as a dict, or None when absent."""
        with self.conn.cursor() as cur:
            cur.execute(
                "SELECT c.reloptions FROM pg_class c "
                "WHERE c.oid = to_regclass(%s)", (f"public.{name}",))
            row = cur.fetchone()
        if row is None:
            return None
        return parse_reloptions(row[0])

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
    def default_build_cfg(self, algo, n):
        """The build config used when a caller passes none.

        pgvector's cell derives from HNSW_M / HNSW_EF_CONSTRUCTION rather than
        repeating the numbers, because build_time_arm.py labels its rows from
        those same constants (#78 review F8) -- a second copy would drift and
        the published CSV would then describe the wrong build.
        """
        if algo == "pgvector_hnsw":
            return {"m": self.HNSW_M, "ef_construction": self.HNSW_EF_CONSTRUCTION}
        if algo == "pgvector_ivfflat":
            return {"lists": self.ivfflat_lists(n)}
        if algo == "pgcuvs_cagra":
            return {"graph_degree": 64,
                    "intermediate_graph_degree": INTERMEDIATE_GRAPH_DEGREE}
        if algo == "pgcuvs_hnsw_import":
            return {"mode": "nsw", "graph_degree": 64,
                    "intermediate_graph_degree": INTERMEDIATE_GRAPH_DEGREE}
        return {}

    def build(self, algo, n, sample_query=None, build_cfg=None, keep=()):
        """Build `algo`'s index under `build_cfg`.

        Returns (build_time_s, index_bytes, notice_meta) -- notice_meta carries
        whatever the server's build NOTICE reported (3I only; {} elsewhere).

        build_cfg is the #98 first-class build dimension: pgvector takes
        {m, ef_construction}, cagra {graph_degree, intermediate_graph_degree},
        3I {mode, graph_degree, intermediate_graph_degree}. None -> the algo's
        default cell (default_build_cfg).

        `keep` names relations that must survive the pre-build drop; see
        _drop_ann_indexes for why the default drops everything.

        sample_query is accepted for call compatibility and unused: the unified
        0.5.0 export needs no dummy search.
        """
        assert algo in ALGOS, f"unknown algo {algo}"
        cfg = dict(build_cfg) if build_cfg else self.default_build_cfg(algo, n)
        c = self.conn
        self._drop_for(algo, keep=keep)
        self._notices = []
        c.execute("SET maintenance_work_mem = '16GB'")
        c.execute("SET max_parallel_maintenance_workers = 7")

        if algo == "pgvector_hnsw":
            m = int(cfg.get("m", self.HNSW_M))
            efc = int(cfg.get("ef_construction", self.HNSW_EF_CONSTRUCTION))
            t0 = time.perf_counter()
            c.execute("CREATE INDEX t_hnsw_pgv ON t USING hnsw (embedding vector_l2_ops) "
                      f"WITH (m={m}, ef_construction={efc})")
            return time.perf_counter() - t0, self._relsize("t_hnsw_pgv"), {}

        if algo == "pgvector_ivfflat":
            lists = int(cfg.get("lists", self.ivfflat_lists(n)))
            t0 = time.perf_counter()
            c.execute("CREATE INDEX t_ivf ON t USING ivfflat (embedding vector_l2_ops) "
                      f"WITH (lists={lists})")
            return time.perf_counter() - t0, self._relsize("t_ivf"), {}

        if algo == "pgcuvs_cagra":
            c.execute(f"SET cuvs.index_dir = '{self.index_dir}'")
            c.execute("SET maintenance_work_mem = '8GB'")
            t0 = time.perf_counter()
            c.execute("CREATE INDEX t_cagra ON t USING cagra (embedding vector_l2_ops) "
                      + self._cagra_with(cfg))
            bt = time.perf_counter() - t0
            # CAGRA graph is VRAM-resident (pg_relation_size == 0); report the
            # daemon's self-accounted VRAM footprint instead (issue #75).
            vram = self._vram_bytes("t_cagra")
            if vram is None:
                print("[engine] WARN: no pg_stat_gpu_search row for t_cagra "
                      "(daemon down / not resident); index_bytes falls back to 0",
                      flush=True)
                vram = self._relsize("t_cagra")
            return bt, vram, {}

        # pgcuvs_hnsw_import (3I): GPU CAGRA build -> pgvector HNSW export.
        #
        # The export runs as a DDL CREATE INDEX on the pg_cuvs_hnsw AM, NOT via
        # pg_cuvs_build_hnsw(): that function is deprecated (it says so in a
        # NOTICE, src/hnsw_export.c:1837-1842) and creates the index with
        # reloptions NULL, which leaves the resulting relation unable to say
        # what built it. The DDL form stores source/mode/m/ef_construction as
        # reloptions (:1889-1905) and names the index directly, so the reuse
        # gate has a catalog fact to check and no RENAME is needed. A benchmark
        # meant for publication does not measure an API the repo tells callers
        # not to use.
        mode = str(cfg.get("mode", "nsw"))
        c.execute(f"SET cuvs.index_dir = '{self.index_dir}'")
        c.execute("SET maintenance_work_mem = '8GB'")
        t0 = time.perf_counter()
        c.execute("CREATE INDEX t_cagra ON t USING cagra (embedding vector_l2_ops) "
                  + self._cagra_with(cfg))
        t_cagra = time.perf_counter() - t0
        t1 = time.perf_counter()
        c.execute("CREATE INDEX t_hnsw_3i ON t USING pg_cuvs_hnsw "
                  "(embedding vector_l2_ops) "
                  f"WITH (source='{SOURCE_RELATION}', mode='{mode}')")
        t_export = time.perf_counter() - t1
        meta = parse_build_notices(self._notices)
        return t_cagra + t_export, self._relsize("t_hnsw_3i"), meta

    @staticmethod
    def _cagra_with(cfg):
        """WITH(...) clause for a CAGRA build.

        intermediate_graph_degree must be >= graph_degree (src/pg_cuvs.c:1265),
        so both travel together or neither does.

        build_algo is ALWAYS pinned, even when the cell names no degrees. The
        reloption default is `auto` -- a corpus-size heuristic that chooses
        ivf_pq or nn_descent -- while the raw arm (cuvs_engine) names its
        algorithm outright. Left on `auto`, the two arms could be built by
        different algorithms, and "same parameters except the integration"
        would be an unverified claim rather than a fact of the DDL.
        """
        gd = cfg.get("graph_degree")
        if gd is None:
            return f"WITH (build_algo='{CAGRA_BUILD_ALGO}')"
        igd = int(cfg.get("intermediate_graph_degree", INTERMEDIATE_GRAPH_DEGREE))
        return (f"WITH (graph_degree={int(gd)}, intermediate_graph_degree={igd}, "
                f"build_algo='{CAGRA_BUILD_ALGO}')")

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

    # -- batch search (3M, throughput axis) ------------------------------------
    #: GUC ceiling on one batch dispatch (src/pg_cuvs.c:994). Q must not exceed
    #: it; the harness sets the GUC to the ceiling so Q=2000 has 2x headroom.
    MAX_BATCH_QUERIES = 4096

    BATCH_SQL = (
        "SELECT b.query_idx, t.id, b.distance "
        "FROM pg_cuvs_batch_search('t'::regclass, %s::vector[], %s) b "
        "JOIN t ON t.ctid = b.ctid "
        "ORDER BY b.query_idx, b.distance")

    def _batch_gucs(self, cur):
        cur.execute("SET enable_seqscan = off")
        # The ctid join must be a Tid Scan; a hash join over the whole heap
        # would still be correct but would time the heap, not the search.
        cur.execute("SET enable_hashjoin = off")
        cur.execute(f"SET cuvs.index_dir = '{self.index_dir}'")
        cur.execute("SET cuvs.shard_count = 1")
        cur.execute(f"SET cuvs.max_batch_queries = {self.MAX_BATCH_QUERIES}")

    def batch_plan_guard(self, queries, k):
        """EXPLAIN the batch statement. Returns (plan_text, seqscan: bool).

        A Seq Scan here is a WARNING, not a failure (plan rev.4): the ctid join
        returns the same rows either way, so only the timing's interpretation is
        polluted -- unlike the latency axis, where a Seq Scan means the ANN index
        was never consulted and the recall is a different quantity entirely.
        """
        with self.conn.cursor() as cur:
            self._batch_gucs(cur)
            cur.execute("EXPLAIN (FORMAT TEXT) " + self.BATCH_SQL,
                        (self._vector_array(queries), int(k)))
            plan = "\n".join(r[0] for r in cur.fetchall())
        return plan, ("Seq Scan" in plan)

    @staticmethod
    def _vector_array(queries):
        """The query block as a psycopg BIND PARAMETER (list of pgvector Vector).

        Deliberate exception to this suite's same-statement-shape rule, and the
        report says so in a footnote: 2000 x 1024 floats spelled as an inline
        literal is a ~29 MB statement, so the parser -- not the search -- would
        be what the batch arm measures.
        """
        from pgvector import Vector
        return [Vector(q) for q in queries]

    def batch_search(self, queries, k, top_k=10, warmup=2, repeats=10, chunks=1):
        """One `pg_cuvs_batch_search` round trip per repeat over `queries`.

        Returns (ids (nq, top_k), per_repeat_seconds, notes). `chunks` splits Q
        into that many dispatches per repeat (the documented fallback when the
        reply shm -- 8 + Q*K*12 bytes, no hard cap in the daemon -- cannot be
        sized at the selected K); the split is merged back before truncation, so
        the returned ids are the same shape either way and the note records that
        the repeat was not a single round trip.
        """
        nq = len(queries)
        notes = []
        if chunks > 1:
            notes.append(f"batch split into {chunks} dispatches (shm sizing)")
        bounds = [(s, min(s + (nq + chunks - 1) // chunks, nq))
                  for s in range(0, nq, (nq + chunks - 1) // chunks)]
        params = [(self._vector_array(queries[a:b]), int(k)) for a, b in bounds]

        with self.conn.cursor() as cur:
            self._batch_gucs(cur)

            def one_pass():
                rows = []
                for (a, _b), p in zip(bounds, params):
                    cur.execute(self.BATCH_SQL, p)
                    # query_idx is per dispatch; shift it back into the block's
                    # own index space so a split repeat is indistinguishable
                    # from an unsplit one downstream.
                    rows.extend((qi + a, rid, d) for qi, rid, d in cur.fetchall())
                return rows

            for _ in range(warmup):
                one_pass()
            # repeats=0 is the shm pre-check: dispatch once (the warmup) to
            # prove the reply buffer sizes at this K, and time nothing.
            per_repeat, rows = [], []
            for _ in range(repeats):
                t0 = time.perf_counter()
                rows = one_pass()
                per_repeat.append(time.perf_counter() - t0)
        return truncate_topk(rows, nq, top_k), per_repeat, notes

    def fallback_count(self):
        """Total CPU-fallback events the daemon has recorded so far.

        The throughput arms gate on the DELTA of this being 0: the daemon's
        listen() backlog is 32 while every request opens its own UDS connection,
        so at N=64 an overflow could be absorbed as a silent CPU exact search --
        which returns recall~=1.0 at high latency and would contaminate the row
        rather than fail it.
        """
        with self.conn.cursor() as cur:
            cur.execute("SELECT coalesce(sum(fallback_count), 0) "
                        "FROM pg_stat_gpu_fallback")
            return int(cur.fetchone()[0])

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
        eng.load_corpus(args.corpus, args.n, dim, dataset=args.dataset)
        print(f"[engine] build {algo} ...", flush=True)
        bt, ibytes, _meta = eng.build(algo, args.n, sample_query=qset[0])
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
