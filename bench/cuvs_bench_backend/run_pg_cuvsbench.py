#!/usr/bin/env python3
"""
run_pg_cuvsbench.py -- drive the pg_cuvs/pgvector cuvs-bench backend end to end.

This is the modern cuvs-bench entrypoint: register the pg backend, then use
BenchmarkOrchestrator(backend_type="pg").run_benchmark(...) -- the same
orchestrator, Dataset, IndexConfig, BuildResult/SearchResult, and compute_recall
that cuvs-bench's own backends use. The module-level cuvs_bench.run.run() is
deprecated upstream in favour of exactly this call.

It proves pg_cuvs runs *inside NVIDIA's cuvs-bench* and emits a native
Google-Benchmark-shaped CSV (items_per_second, Recall, real_time, p50/p95/p99)
for the Pareto plot -- the ecosystem-entry Stage-2 artifact.

#98 shape -- one process, one set of indexes, one CSV, two phases:

  Phase 1 (latency axis, through the orchestrator)
    run_benchmark() is called ONCE PER BUILD CONFIG, and each call sweeps that
    build's search params. A build and its search points therefore share a call,
    which is what makes the (algo, build_params) join in the CSV a fact rather
    than a reconstruction. Rows are appended and flushed after every call, so a
    crash costs one segment, not the run.

  Phase 2 (throughput axis, outside the orchestrator)
    the same process picks each algo's Pareto point from the Phase-1 rows,
    rebuilds to that build config when the resident index is a different cell,
    and calls the batch / concurrent / raw arms directly, appending to the same
    CSV. The orchestrator never learns those arm names (DEFAULT_SWEEPS[algo]
    would KeyError); see run_phase2() below. Resuming skips throughput rows the
    CSV already has and recomputes the Pareto point from the file.

Progress is a live stream, not a post-mortem: every meaningful event prints one
`[pg98] …` line with flush=True (gpu-run.sh redirects stdout to the run log, so
`gpu-run.sh log -f` follows it), and a heartbeat covers the long silent stretches
(big builds, COPY) so silence never has to be interpreted.

Usage (on the GPU VM, cuvs_bench env; daemon up; data in --data-dir):
    python bench/cuvs_bench_backend/run_pg_cuvsbench.py \
        --data-dir /home/ubuntu/anbench/data --n 1000000 \
        --algos pgcuvs_cagra,pgvector_hnsw --k 10 --max-queries 2000 \
        --out bench/results/pg_cuvsbench_98.csv [--resume]

Ground truth: gt_<n>.npy must exist in --data-dir (built by build_gt.py or
cuvs_bench.generate_groundtruth); the loader slices it to <max-queries> rows and
writes the .ibin cuvs-bench needs. Table t is (re)built to <n> rows on first use
and reused across algos/params.
"""
import argparse
import csv
import json
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pg_engine import DEFAULT_SWEEPS  # noqa: E402
from sidecar import (  # noqa: E402
    BUILD_GRIDS,
    RELATION_OF,
    assert_gt_columns,
    canonical_json,
    completed_keys,
    expected_reloptions,
    ownership_record,
    pareto_point,
    reloptions_match,
    remaining_params,
)

NAN = float("nan")

# `reused` predates #98 -- build_params and axis are the new columns.
CSV_FIELDS = ["algo", "param", "k", "recall", "qps", "search_time_ms",
              "p50_ms", "p95_ms", "p99_ms", "build_time_s", "index_bytes",
              "n_queries", "build_params", "axis", "reused", "success", "error",
              "notes"]

RECALL_FLOOR = 0.95      # Pareto: the recall a throughput point must clear
RECALL_SANITY = (0.0, 1.0000001)   # anything outside is a broken measurement


# ── live stream ──────────────────────────────────────────────────────────────
def log(stage, event, *parts):
    """One event, one line, flushed. `[pg98] <stage> <event> …`"""
    msg = " ".join(str(p) for p in parts)
    print(f"[pg98] {stage} {event} {msg}".rstrip(), flush=True)
    Heartbeat.touch()


class Heartbeat:
    """Prints `[pg98] hb <what> elapsed=…s` every 30s while nothing else does.

    A long build or COPY otherwise leaves the log silent for minutes, and a
    silent log cannot be told apart from a dead job -- which is the exact
    ambiguity gpu-run.sh exists to remove.
    """

    _last = time.time()
    _lock = threading.Lock()

    def __init__(self, what, interval=30.0, quiet_after=60.0):
        self.what = what
        self.interval = interval
        self.quiet_after = quiet_after
        self._stop = threading.Event()
        self._t0 = time.time()
        self._thread = threading.Thread(target=self._run, daemon=True)

    @classmethod
    def touch(cls):
        with cls._lock:
            cls._last = time.time()

    def _run(self):
        while not self._stop.wait(self.interval):
            with Heartbeat._lock:
                silent = time.time() - Heartbeat._last
            if silent >= self.quiet_after:
                print(f"[pg98] hb {self.what} elapsed={time.time()-self._t0:.0f}s",
                      flush=True)

    def __enter__(self):
        self._thread.start()
        return self

    def __exit__(self, *exc):
        self._stop.set()
        return False


# ── CSV ──────────────────────────────────────────────────────────────────────
def read_existing(path):
    """Rows already in `path` (empty when it does not exist)."""
    if not os.path.exists(path):
        return []
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def open_csv(path, resume):
    """Open the CSV for append (resume) or fresh write. Returns (file, writer)."""
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    existed = os.path.exists(path) and os.path.getsize(path) > 0
    f = open(path, "a" if (resume and existed) else "w", newline="")
    w = csv.DictWriter(f, fieldnames=CSV_FIELDS, extrasaction="ignore")
    if not (resume and existed):
        w.writeheader()
        f.flush()
    return f, w


def _rows(results, axis="latency"):
    """Fold the orchestrator's flat [BuildResult|SearchResult, …] list into one
    CSV row per search point.

    The join key is (algo, build_params), NOT algo alone: with a build grid,
    "the algo's most recent build" would attach every sweep point to whichever
    build happened to run last.
    """
    from cuvs_bench.backends.base import BuildResult, SearchResult

    builds, rows = {}, []
    for r in results:
        if isinstance(r, BuildResult):
            builds[(r.algorithm, canonical_json(r.build_params))] = r
        elif isinstance(r, SearchResult):
            md = r.metadata or {}
            bp = md.get("build_params", "{}")
            b = builds.get((r.algorithm, bp))
            lp = r.latency_percentiles or {}
            sp = r.search_params[0] if r.search_params else {}
            rows.append({
                "algo": r.algorithm,
                "param": sp.get("param"),
                "k": sp.get("k"),
                "recall": round(r.recall, 4),
                "qps": round(r.queries_per_second, 1),
                "search_time_ms": round(r.search_time_ms, 3),
                "p50_ms": round(lp.get("p50_ms", float("nan")), 3),
                "p95_ms": round(lp.get("p95_ms", float("nan")), 3),
                "p99_ms": round(lp.get("p99_ms", float("nan")), 3),
                "build_time_s": round(b.build_time_seconds, 3) if b else "",
                "index_bytes": b.index_size_bytes if b else "",
                "n_queries": md.get("n_queries"),
                "build_params": bp,
                "axis": md.get("axis", axis),
                "reused": (b.metadata or {}).get("reused") if b else "",
                "success": r.success,
                "error": r.error_message or "",
                # Backends that have something to say about the conditions a
                # point was measured under (the raw arm's GPU tenancy) put it
                # here; "" for everything else.
                "notes": md.get("notes", ""),
            })
    return rows


# ── segment validation ───────────────────────────────────────────────────────
class SegmentError(RuntimeError):
    """A segment produced rows that must not enter the CSV's conclusions."""


def validate_segment(rows, expected_n, algo, build_params, recall_range=RECALL_SANITY):
    """Check one build_cfg's rows the moment they land, and abort on violation.

    Carrying on past a bad segment accumulates contamination that is only
    discovered at the end, when the run has already cost hours -- so this raises
    instead of warning. Returns a short human summary for the log line.
    """
    if len(rows) != expected_n:
        raise SegmentError(
            f"{algo} {build_params}: {len(rows)} rows, expected {expected_n}")
    bad = [r for r in rows if not r["success"]]
    if bad:
        raise SegmentError(
            f"{algo} {build_params}: {len(bad)} failed rows; first error: "
            + str(bad[0]["error"]))
    lo, hi = recall_range
    off = [r for r in rows if not (lo <= float(r["recall"]) <= hi)]
    if off:
        raise SegmentError(
            f"{algo} {build_params}: recall outside {recall_range}: "
            + ", ".join(f"param={r['param']} recall={r['recall']}" for r in off))
    # Exactly one build per segment: every row must name the same build, and
    # the reuse flag must be coherent across them (a segment that is half
    # reused and half rebuilt means the index changed mid-sweep).
    keys = {r["build_params"] for r in rows}
    if keys != {build_params}:
        raise SegmentError(
            f"{algo}: rows carry build_params {sorted(keys)}, expected {build_params}")
    # A healthy segment builds once and reuses that index for the rest of the
    # sweep, so the reuse flags must read False?True* -- at most one build, and
    # it first. (On --resume with the index already resident, every row is True.)
    # A True followed by a False means the index was replaced mid-sweep, so the
    # rows do not describe one index and the segment's build_time means nothing.
    flags = [str(r["reused"]) == "True" for r in rows]
    if flags != sorted(flags):
        raise SegmentError(
            f"{algo} {build_params}: index rebuilt mid-sweep (reused={flags})")
    n_built = flags.count(False)
    if n_built > 1:
        raise SegmentError(
            f"{algo} {build_params}: {n_built} builds in one segment")
    return (f"rows={len(rows)} builds={n_built} "
            f"recall=[{min(float(r['recall']) for r in rows):.4f},"
            f"{max(float(r['recall']) for r in rows):.4f}]")


# ── Phase 2 (PR-C) ───────────────────────────────────────────────────────────
def select_pareto(rows, algos, recall_floor=RECALL_FLOOR):
    """The throughput-axis operating point per algo, from the latency rows.

    Returns {algo: {"param":…, "build_params":…, "recall":…, "qps":…,
                    "fallback": bool}}. `fallback` marks an algo where no point
    cleared the floor and the highest-recall point was taken instead -- the
    report must say so rather than presenting it as a recall>=floor point.
    """
    out = {}
    for algo in algos:
        row, fallback = pareto_point(rows, algo, recall_floor)
        if row is None:
            continue
        out[algo] = {"param": row["param"], "build_params": row["build_params"],
                     "recall": float(row["recall"]), "qps": float(row["qps"]),
                     "fallback": fallback}
    return out


def throughput_row(algo, param, k, recall, qps, search_time_ms, build_params,
                   n_queries, build_time_s="", index_bytes="", reused="",
                   p50=NAN, p95=NAN, p99=NAN, notes=(), success=True, error=""):
    """One `axis=throughput` CSV row.

    The param spelling is a contract with report_recall_tables.py, which reads
    `param.startswith("batch_k")` / `("conc")` to decide which caveat footnotes a
    row carries: a row spelled differently would silently lose its caveat.
    """
    return {"algo": algo, "param": param, "k": k,
            "recall": round(recall, 4) if recall == recall else "",
            "qps": round(qps, 1), "search_time_ms": round(search_time_ms, 3),
            "p50_ms": round(p50, 3) if p50 == p50 else "nan",
            "p95_ms": round(p95, 3) if p95 == p95 else "nan",
            "p99_ms": round(p99, 3) if p99 == p99 else "nan",
            "build_time_s": build_time_s, "index_bytes": index_bytes,
            "n_queries": n_queries, "build_params": build_params,
            "axis": "throughput", "reused": reused, "success": success,
            "error": error, "notes": "; ".join(n for n in notes if n)}


def _sidecar_path():
    """Where PgBackend keeps its per-relation ownership records (backend.py)."""
    import tempfile
    return os.path.join(tempfile.gettempdir(), "pg_current_index.json")


def _read_sidecar():
    try:
        with open(_sidecar_path()) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def ensure_pareto_index(eng, algo, cfg, n, keep=()):
    """Make `algo`'s Pareto build config the resident index, rebuilding if not.

    Phase 1 leaves whichever cell ran last resident, which is generally NOT the
    Pareto cell -- so the throughput axis usually has to rebuild. That rebuild is
    a real cost of the run and is logged as one; it is never skipped by treating
    "an index of this algo exists" as good enough, because the row would then
    describe a different build than the one it names.

    Returns (build_time_s, index_bytes, rebuilt: bool).
    """
    rel = RELATION_OF[algo]
    side = _read_sidecar()
    entry = side.get(rel, {})
    actual = eng.reloptions(rel)
    ok = (actual is not None
          and entry.get("algo") == algo
          and entry.get("build_cfg") == canonical_json(cfg)
          and reloptions_match(actual, expected_reloptions(algo, cfg)))
    if ok:
        log("P2", "index", algo, canonical_json(cfg), "resident (no rebuild)")
        return (entry.get("build_time_seconds", ""),
                entry.get("index_size_bytes", ""), False)
    log("P2", "rebuild", algo, canonical_json(cfg),
        f"reason={'absent' if actual is None else 'not the Pareto cell'}")
    with Heartbeat(f"rebuild {algo}"):
        bt, ibytes, notice_meta = eng.build(algo, n, build_cfg=cfg, keep=keep)
    side = {k: v for k, v in side.items()
            if eng.reloptions(k) is not None and k != rel}
    side[rel] = ownership_record(algo, cfg, bt, ibytes, notice_meta)
    with open(_sidecar_path(), "w") as f:
        json.dump(side, f)
    log("P2", "rebuilt", algo, canonical_json(cfg), f"{bt:.1f}s", f"bytes={ibytes}")
    return bt, ibytes, True


# Which throughput arms each latency algo contributes, in run order. pg_cuvs
# gets a batch arm (its real throughput mechanism) plus the concurrency curve
# that shows why; pgvector gets the concurrency curve only -- it has no batch
# dispatch to measure.
BATCH_ALGOS = ("pgcuvs_cagra",)
CONC_ALGOS = ("pgcuvs_cagra", "pgvector_hnsw")


def run_phase2(args, writer, fout, latency_rows):
    """Throughput axis: same process, same indexes, same CSV as Phase 1.

    Phase 2 must run here, in the process that ran Phase 1, because "the same
    run" is defined as one process / one set of indexes / one CSV -- the property
    #98 exists to restore.
    """
    import numpy as np
    from pg_engine import PgEngine, read_fbin

    done = {(r["algo"], r.get("build_params", "{}"), r["param"])
            for r in latency_rows
            if str(r.get("axis", "")) == "throughput"
            and str(r.get("success", "")).lower() in ("true", "1", "yes")}
    if done:
        log("P2", "resume", f"throughput rows already recorded={len(done)}")

    pareto = select_pareto(latency_rows, sorted({r["algo"] for r in latency_rows}))
    for algo, p in sorted(pareto.items()):
        log("P2", "pareto", algo, f"param={p['param']}",
            f"build={p['build_params']}", f"recall={p['recall']:.4f}",
            f"qps={p['qps']:.0f}", f"fallback={p['fallback']}")
    if not pareto:
        log("P2", "skip", "no latency rows to pick a Pareto point from")
        return []

    queries_all = np.ascontiguousarray(
        read_fbin(os.path.join(args.data_dir, "queries_10k.fbin")))
    gt = np.load(os.path.join(args.data_dir, f"gt_{args.n}.npy"))
    assert_gt_columns(gt.shape, k=args.k)
    q_lat = queries_all[:args.max_queries]
    conc_ns = [int(x) for x in str(args.conc_n).split(",") if x.strip()]

    eng = PgEngine(dbname=args.dbname, index_dir=args.index_dir)
    with eng.conn.cursor() as cur:
        cur.execute("SHOW max_connections")
        max_conn = cur.fetchone()[0]
    env_note = f"max_connections={max_conn}; daemon up; cuvs.shard_count=1"
    log("P2", "env", env_note, f"conc_n={conc_ns}", f"window={args.conc_window}s")

    rows = []

    def emit(row):
        writer.writerow(row)
        fout.flush()
        rows.append(row)

    # -- pg_cuvs arms (t_cagra resident) --------------------------------------
    for algo in BATCH_ALGOS:
        if algo not in pareto:
            continue
        p = pareto[algo]
        cfg = json.loads(p["build_params"] or "{}")
        bt, ibytes, rebuilt = ensure_pareto_index(eng, algo, cfg, args.n)
        emit_batch(args, eng, emit, done, algo, p, cfg, q_lat, gt, bt, ibytes,
                   rebuilt, env_note)

    for algo in CONC_ALGOS:
        if algo not in pareto:
            continue
        p = pareto[algo]
        cfg = json.loads(p["build_params"] or "{}")
        # pgvector's index is built alongside the resident CAGRA graph (a
        # co-residency this repo has already validated); the CAGRA arm keeps its
        # index so a later resume does not rebuild it.
        keep = ("t_cagra",) if algo.startswith("pgvector") else ()
        bt, ibytes, _ = ensure_pareto_index(eng, algo, cfg, args.n, keep=keep)
        for n_workers in conc_ns:
            emit_conc(args, eng, emit, done, algo, p, n_workers, queries_all, gt,
                      bt, ibytes, env_note, latency_rows)

    # -- raw cuVS batch (PR-B's engine; absent until that PR lands) ------------
    if not args.skip_raw_batch:
        emit_raw_batch(args, emit, done, pareto, q_lat, gt, env_note)

    eng.close()
    log("P2", "done", f"throughput rows={len(rows)}")
    return rows


def emit_batch(args, eng, emit, done, algo, p, cfg, queries, gt, bt, ibytes,
               rebuilt, env_note):
    """`pg_cuvs_batch_search`: the whole query block in one IPC round trip."""
    K = int(p["param"])
    name, param = f"{algo}_batch", f"batch_k{K}"
    if (name, p["build_params"], param) in done:
        log("P2", "skip", name, param, "(already recorded)")
        return
    notes = [env_note, f"K={K} (Pareto cuvs.k), client-side top-{args.k} cut"]

    plan, seqscan = eng.batch_plan_guard(queries[:2], K)
    if seqscan:
        log("P2", "WARN", "seqscan-detected", name,
            "(row accuracy unaffected; timing interpretation only)")
        notes.append("WARN seq scan in batch plan: "
                     + " | ".join(plan.splitlines()[:3]))

    # shm pre-check at the SELECTED K -- reply shm is 8 + Q*K*12 bytes with no
    # hard cap in the daemon, so the sizing must be proven before the timed
    # repeats rather than discovered inside them.
    chunks = 1
    try:
        eng.batch_search(queries, K, top_k=args.k, warmup=1, repeats=0)
        log("P2", "shm", name, f"Q={len(queries)}", f"K={K}",
            f"~{(8 + len(queries)*K*12)/1e6:.1f}MB ok")
    except Exception as e:  # noqa: BLE001
        chunks = 2
        log("P2", "WARN", "shm-dispatch-failed", name, repr(e),
            f"-> splitting Q into {chunks}")
        notes.append(f"shm dispatch at K={K} failed ({e!r}); Q split")

    fb0 = eng.fallback_count()
    with Heartbeat(f"batch {name}"):
        ids, per_repeat, bnotes = eng.batch_search(
            queries, K, top_k=args.k, warmup=args.batch_warmup,
            repeats=args.batch_repeats, chunks=chunks)
    fb_delta = eng.fallback_count() - fb0
    notes += bnotes

    med = float(sorted(per_repeat)[len(per_repeat) // 2])
    recall = _recall_at_k(ids, gt[:len(ids)], args.k)
    qps = len(queries) / med
    notes.append(f"repeats={len(per_repeat)} median={med*1000:.1f}ms "
                 f"min={min(per_repeat)*1000:.1f}ms max={max(per_repeat)*1000:.1f}ms")
    notes.append(gate_fallback(name, fb_delta))
    notes.append(gate_batch_recall(args, name, recall, p["recall"]))

    emit(throughput_row(name, param, args.k, recall, qps, med * 1000.0,
                        p["build_params"], len(queries), build_time_s=bt,
                        index_bytes=ibytes, reused=not rebuilt, notes=notes))
    log("P2", "batch", name, param, f"recall={recall:.4f}", f"qps={qps:.0f}",
        f"median={med*1000:.1f}ms", f"fallback-delta={fb_delta}")


def emit_conc(args, eng, emit, done, algo, p, n_workers, queries_all, gt, bt,
              ibytes, env_note, latency_rows):
    """N concurrent connections, single-query scans, fixed wall-clock window."""
    import conc_runner

    name, param = f"{algo}_conc{n_workers}", f"conc{n_workers}"
    if (name, p["build_params"], param) in done:
        log("P2", "skip", name, param, "(already recorded)")
        return
    fb0 = eng.fallback_count()
    with Heartbeat(f"conc {name}"):
        r = conc_runner.run_arm(algo, n_workers, queries_all, gt, p["param"],
                                args.index_dir, dbname=args.dbname,
                                window=args.conc_window, top_k=args.k)
    fb_delta = eng.fallback_count() - fb0

    notes = [env_note, f"param={p['param']}",
             f"recall over {r['recall_queries']} gt-covered queries"]
    notes += r["notes"]
    notes.append(gate_fallback(name, fb_delta))
    if n_workers == 1:
        notes.append(gate_conc1_ratio(name, r["qps"], p["qps"],
                                      fmt_ms=r.get("literal_format_ms")))

    emit(throughput_row(name, param, args.k, r["recall"], r["qps"],
                        r["wall"] * 1000.0, p["build_params"], r["queries"],
                        build_time_s=bt, index_bytes=ibytes, reused=True,
                        p50=r["p50_ms"], p95=r["p95_ms"], p99=r["p99_ms"],
                        notes=notes))
    log("P2", "conc", name, f"N={n_workers}", f"window={r['wall']:.0f}s",
        f"qps={r['qps']:.0f}", f"recall={r['recall']:.4f}",
        f"p50={r['p50_ms']:.2f}ms", f"fallback-delta={fb_delta}")


def emit_raw_batch(args, emit, done, pareto, queries, gt, env_note):
    """raw cuVS `cagra.search` over the whole block (PR-B's cuvs_engine).

    The raw index lives in this process's GPU memory, so Phase 2 reuses the
    engine singleton Phase 1 left behind when it already holds the Pareto cell,
    and rebuilds with the SAME BUILD PARAMETERS otherwise. "Same parameters" is
    the honest label rather than "same index": a CAGRA build is not guaranteed
    bit-identical run to run, so a rebuilt graph is not the graph Phase 1 swept.
    """
    try:
        import cuvs_engine
    except Exception as e:  # noqa: BLE001 -- cupy/cuvs absent (CPU box)
        log("P2", "skip", "cuvs_batch", f"cuvs_engine unavailable ({e!r})")
        return
    src = pareto.get("cuvs") or pareto.get("pgcuvs_cagra")
    if src is None:
        log("P2", "skip", "cuvs_batch", "no raw/CAGRA Pareto point")
        return
    K = int(src["param"])
    name, param = "cuvs_batch", f"batch_k{K}"
    if (name, src["build_params"], param) in done:
        log("P2", "skip", name, param, "(already recorded)")
        return
    cfg = json.loads(src["build_params"] or "{}")
    notes = [env_note, f"K={K} (Pareto point), recall@{args.k}"]
    try:
        from pg_engine import fbin_meta

        corpus = os.path.join(args.data_dir, "corpus.fbin")
        _, dim = fbin_meta(corpus)
        eng = cuvs_engine.get_raw_engine()
        with Heartbeat("raw batch"):
            eng.load_corpus(corpus, args.n, dim, dataset=args.dataset)
            reused = eng.has_index(cfg)
            if reused:
                log("P2", "index", "cuvs", canonical_json(cfg),
                    "resident in this process (no rebuild)")
                bt = getattr(eng, "_build_time", "")
            else:
                log("P2", "rebuild", "cuvs", canonical_json(cfg),
                    "reason=not the Pareto cell")
                bt, _ibytes, _meta = eng.build("cuvs", args.n, build_cfg=cfg)
                notes.append("raw index rebuilt at the Pareto cell's build "
                             "PARAMETERS (a CAGRA build is not bit-identical, "
                             "so this is not the Phase-1 graph)")
            ids, med = eng.search_batch(queries, args.k, K,
                                        warmup=args.batch_warmup,
                                        repeats=args.batch_repeats)
    except Exception as e:  # noqa: BLE001 -- a raw failure must not lose the
        log("P2", "WARN", "raw-batch-failed", repr(e))   # Postgres arms' rows
        return
    recall = _recall_at_k(ids, gt[:len(ids)], args.k)
    qps = len(queries) / med
    # The raw arm is a SECOND GPU tenant beside the resident daemon, so the row
    # carries the occupancy it actually saw rather than an assumption of one.
    notes.append(eng.row_note())
    notes.append(f"repeats={args.batch_repeats} median={med*1000:.1f}ms")
    emit(throughput_row(name, param, args.k, recall, qps, med * 1000.0,
                        src["build_params"], len(queries), build_time_s=bt,
                        index_bytes=0, reused=reused, notes=notes))
    log("P2", "batch", name, param, f"recall={recall:.4f}", f"qps={qps:.0f}",
        f"median={med*1000:.1f}ms")


# ── Phase-2 consistency gates ────────────────────────────────────────────────
GATE_VIOLATIONS = []


def _gate(name, ok, text):
    if not ok:
        GATE_VIOLATIONS.append(f"{name}: {text}")
        log("P2", "WARN", "gate-violation", name, text)
    return ("gate-ok " if ok else "GATE-VIOLATION ") + text


def gate_fallback(name, delta):
    """The daemon must not have absorbed any request as a CPU exact search.

    A fallback returns recall~=1.0 at high latency, so a contaminated arm looks
    *better* on recall while measuring something else entirely -- which is why
    this is checked per arm rather than once for the run.
    """
    return _gate(name, delta == 0, f"fallback-delta={delta} (want 0)")


def gate_conc1_ratio(name, conc_qps, latency_qps, fmt_ms=None, lo=0.8, hi=1.0):
    """conc N=1 vs the latency axis: the two must describe the same query.

    The band assumes conc N=1 is the lower number (wall-clock, including loop
    overhead the latency axis's nq/sum(latency) excludes). Measured at 100k it
    came out HIGHER, and the cause is a real asymmetry rather than noise: the
    latency axis formats each query's inline vector literal INSIDE its timed
    region (pg_engine.search), while the throughput arm precomputes literals
    before the window on purpose -- at N=64 a timed format would make the arm
    report the client's float formatting speed, not the server's throughput.

    So the raw ratio is what is gated (it is the honest cross-check, and it did
    catch this), and when the arm measured its own formatting cost the note also
    carries the parity-corrected ratio: the latency axis's per-query time minus
    that measured cost. Recorded, never fatal."""
    if not latency_qps:
        return "conc1-ratio: no latency QPS to compare"
    ratio = conc_qps / latency_qps
    extra = ""
    if fmt_ms:
        parity_s = 1.0 / latency_qps - fmt_ms / 1000.0
        if parity_s > 0:
            extra = (f"; literal-format-corrected ratio="
                     f"{conc_qps * parity_s:.3f} (latency axis includes "
                     f"{fmt_ms:.3f}ms/query client-side literal formatting that "
                     f"the conc arm precomputes)")
    return _gate(name, lo <= ratio <= hi,
                 f"conc1/latency QPS ratio={ratio:.3f} (want [{lo},{hi}])" + extra)


def gate_batch_recall(args, name, batch_recall, single_recall):
    """Batch and single-query recall need not be identical: the AM path picks
    the CAGRA kernel by batch size (single-CTA vs multi-CTA), so a delta is
    expected rather than a bug. The tolerance is therefore a measured input
    (--batch-recall-tol, set from the rehearsal's observed delta x2), and with
    no tolerance given the arm runs in observe-and-record mode: both recalls go
    into the CSV and the report compares them. Hardcoding 0.001 here would
    manufacture a failure out of a known kernel difference."""
    delta = abs(batch_recall - single_recall)
    txt = (f"batch recall={batch_recall:.4f} vs single={single_recall:.4f} "
           f"delta={delta:.4f}")
    if args.batch_recall_tol is None:
        return "observe-and-record " + txt
    return _gate(name, delta <= args.batch_recall_tol,
                 txt + f" (tol={args.batch_recall_tol})")


def _recall_at_k(ids, gt, k):
    """recall@k of returned ids against ground truth, same id space."""
    import numpy as np
    gt = np.asarray(gt)
    kk = min(k, gt.shape[1])
    hits = 0
    for a, b in zip(ids[:, :k], gt[:, :kk]):
        hits += len(set(int(x) for x in a) & set(int(x) for x in b))
    return hits / float(len(ids) * kk)


# ── main ─────────────────────────────────────────────────────────────────────
def phase1_segments(args, done, algos):
    """The (algo, build_cfg, params) segments Phase 1 still has to measure.

    Empty under --phase2-only: the Phase-1 rows are already in the CSV, so
    re-entering the sweep would re-measure finished work (and, when a build
    flakes, re-abort on it) instead of running the arms that are missing.
    """
    segments = []
    for algo in (() if args.phase2_only else algos):
        for cell in BUILD_GRIDS[algo]:
            sweep = [p for p in DEFAULT_SWEEPS[algo]
                     if not (algo in ("pgvector_hnsw", "pgcuvs_hnsw_import")
                             and p < args.k)]
            todo = remaining_params(done, algo, cell, sweep)
            if todo:
                segments.append((algo, cell, todo))
            else:
                log("S1", "skip", algo, canonical_json(cell), "(already complete)")
    return segments


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-dir", required=True)
    ap.add_argument("--n", type=int, required=True)
    ap.add_argument("--algos", default="pgcuvs_cagra,pgvector_hnsw")
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--max-queries", type=int, default=2000)
    ap.add_argument("--dataset", default="cohere-wiki-en-1024")
    ap.add_argument("--dbname", default="postgres")
    ap.add_argument("--index-dir", default="/tmp/cuvs_indexes")
    ap.add_argument("--out", required=True)
    ap.add_argument("--resume", action="store_true",
                    help="append to --out, skipping (algo, build_params, param) "
                         "points it already records as successful")
    ap.add_argument("--phase2", action="store_true",
                    help="run the throughput axis after Phase 1")
    ap.add_argument("--phase2-only", action="store_true",
                    help="skip Phase 1 entirely and run the throughput axis "
                         "against the Phase-1 rows already in --out (the "
                         "recovery path when a run died after Phase 1, and the "
                         "way to re-measure an arm without re-sweeping)")
    ap.add_argument("--conc-n", default="1,8,16,32,64",
                    help="concurrency ladder for the conc arms")
    ap.add_argument("--conc-window", type=float, default=30.0,
                    help="seconds of sustained load per conc arm")
    ap.add_argument("--batch-repeats", type=int, default=10)
    ap.add_argument("--batch-warmup", type=int, default=2)
    ap.add_argument("--batch-recall-tol", type=float, default=None,
                    help="batch-vs-single recall gate = measured delta x2. "
                         "Deltas so far (N=100k): raw arm +0.0002 / +0.0003 "
                         "(gd=64, K=100), SQL batch arm 0.0012 (gd=32, K=64) -- "
                         "they move, so this stays a config value and gets "
                         "recalibrated at the Stage-1 rehearsal rather than "
                         "being frozen here. Omitted -> observe-and-record "
                         "(both recalls in the CSV, nothing gated)")
    ap.add_argument("--skip-raw-batch", action="store_true",
                    help="skip the raw cuVS batch arm (PR-B's cuvs_engine)")
    args = ap.parse_args()

    import backend as pg_backend       # imports cuvs_bench; keep it out of
    from cuvs_bench.orchestrator.orchestrator import (  # module import so the
        BenchmarkOrchestrator)         # pure helpers stay CPU-testable
    pg_backend.register("pg")

    algos = [a.strip() for a in args.algos.split(",") if a.strip()]
    # --phase2-only reads the same CSV the same way --resume does; it just does
    # not plan any Phase-1 segment from it.
    resume = args.resume or args.phase2_only
    args.phase2 = args.phase2 or args.phase2_only
    prior = read_existing(args.out) if resume else []
    done = completed_keys(prior)
    if resume:
        log("S0", "resume", f"csv={args.out}", f"rows={len(prior)}",
            f"completed={len(done)}")
    else:
        # A fresh run must not inherit a previous run's indexes or build times.
        import tempfile
        try:
            os.remove(os.path.join(tempfile.gettempdir(), "pg_current_index.json"))
        except OSError:
            pass

    segments = phase1_segments(args, done, algos)
    total = sum(len(t) for _, _, t in segments)
    log("S1", "plan", f"segments={len(segments)}", f"search-points={total}")

    fout, writer = open_csv(args.out, resume)
    all_rows = list(prior)
    done_pts = 0
    try:
        for algo, cell, todo in segments:
            cell_key = canonical_json(cell)
            log("S1", "segment", algo, cell_key, f"params={todo}")
            with Heartbeat(f"{algo} {cell_key}"):
                t0 = time.perf_counter()
                orch = BenchmarkOrchestrator(backend_type="pg")
                results = orch.run_benchmark(
                    mode="sweep", build=True, search=True, count=args.k,
                    dataset=args.dataset, dataset_path=args.data_dir,
                    algorithms=algo, n=args.n, dbname=args.dbname,
                    index_dir=args.index_dir, max_queries=args.max_queries,
                    build_cfg=cell, params=todo,
                )
            rows = _rows(results)
            summary = validate_segment(rows, len(todo), algo, cell_key)
            writer.writerows(rows)
            fout.flush()
            all_rows.extend(rows)
            for r in rows:
                done_pts += 1
                log("S1", "search", r["algo"], cell_key, f"param={r['param']}",
                    f"recall={float(r['recall']):.4f}", f"qps={float(r['qps']):.0f}",
                    f"p50={r['p50_ms']}ms", f"[{done_pts}/{total}]")
            log("S1", "gate", "segment-ok", algo, cell_key, summary,
                f"build={rows[0]['build_time_s']}s",
                f"{time.perf_counter()-t0:.1f}s")
    except SegmentError as e:
        log("S1", "ABORT", "gate-failed", str(e))
        fout.close()
        return 2
    except Exception as e:  # noqa: BLE001
        log("S1", "ABORT", "exception", repr(e))
        fout.close()
        raise

    tp_rows = []
    if args.phase2:
        try:
            tp_rows = run_phase2(args, writer, fout, all_rows)
        except Exception as e:  # noqa: BLE001
            log("P2", "ABORT", "exception", repr(e))
            fout.close()
            raise
    fout.close()

    log("S1", "done", f"points={done_pts}", f"throughput-rows={len(tp_rows)}",
        f"-> {args.out}")
    if GATE_VIOLATIONS:
        # Non-fatal by design (an arm's number is still a measurement), but the
        # run must not end looking clean when a gate did not hold.
        log("P2", "gate-summary", f"violations={len(GATE_VIOLATIONS)}")
        for v in GATE_VIOLATIONS:
            log("P2", "gate-violation", v)
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
