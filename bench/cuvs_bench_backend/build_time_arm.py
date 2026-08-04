#!/usr/bin/env python3
"""
build_time_arm.py -- #78 axis (b): a real build-time distribution, sampled
separately from the search sweep.

The search sweep (run_pg_cuvsbench.py / pg_engine.DEFAULT_SWEEPS) sweeps a
SEARCH-time knob (hnsw.ef_search / ivfflat.probes / cuvs.k). Build params don't
change across that sweep, so PgBackend.build() (backend.py:262) builds each
algo's index once and reuses it for every remaining param -- by design, that
collapses build_time_s to one real observation per algo (see issue #78
comment 2026-07-24). The one run that ever showed a spread of build times was
an artifact of the #78 re-COPY bug forcing a rebuild on every config; fixing
that bug (pg_engine.PgEngine.load_corpus) removes even that accidental sample.

This script fixes the corpus + build params and repeats DROP INDEX; CREATE
INDEX `--build-reps` times per algo, to get a real build-time distribution. It
writes its OWN csv (BUILD_CSV_FIELDS below) -- run_pg_cuvsbench.py's search CSV
schema (CSV_FIELDS there) is untouched. Rows are appended and flushed after
EACH rep, not buffered to the end, so a mid-run failure in a multi-hour sweep
doesn't lose the reps that already completed (#78 review F5c).

Usage (CPU-only, no daemon needed for pgvector_hnsw/pgvector_ivfflat; pg_cuvs
itself is now optional at connect time -- see PgEngine.__init__ -- so this
script's pgvector-only path genuinely runs without CUDA/libcuvs installed):
    python bench/cuvs_bench_backend/build_time_arm.py \
        --data-dir /home/ubuntu/anbench/data --n 1000000 \
        --algos pgvector_hnsw --build-reps 5 \
        --out bench/results/build_time_1m.csv

Caveat: rep 0 of each algo runs against a cold page cache (a fresh COPY just
landed); later reps may be warmer, understating the true variance an operator
would see on an isolated build. Not excluded from the CSV -- it's real data --
but treat it as a caveat when reading the distribution (#78 review F6).
"""
import argparse
import csv
import os
import socket
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pg_engine import ALGOS, PgEngine, fbin_meta, read_fbin  # noqa: E402

BUILD_CSV_FIELDS = ["algo", "build_params", "rep", "build_time_s", "index_bytes",
                     "host", "gpu", "n", "dim", "dataset"]


def gpu_name():
    """First GPU's model name via nvidia-smi, or "" if unavailable (CPU-only
    box / no driver) -- comment 2026-07-24 measured ~2x build_time variance
    across hosts, so this column is required, but its absence must not be
    fatal for the CPU-only algos this script also covers."""
    try:
        out = subprocess.run(
            ["nvidia-smi", "--query-gpu=name", "--format=csv,noheader"],
            capture_output=True, text=True, timeout=10, check=True)
        names = [ln.strip() for ln in out.stdout.splitlines() if ln.strip()]
        return names[0] if names else ""
    except Exception:
        return ""


def build_params_label(algo, n):
    """Label the fixed build configuration used for every rep. Derives from
    PgEngine's own constants (not a second hardcoded copy -- #78 review F8: a
    duplicate would drift silently and the published CSV would then describe
    the wrong build)."""
    if algo == "pgvector_hnsw":
        return f"m={PgEngine.HNSW_M},ef_construction={PgEngine.HNSW_EF_CONSTRUCTION}"
    if algo == "pgvector_ivfflat":
        return f"lists={PgEngine.ivfflat_lists(n)}"
    return "default"


def run_build_reps(eng, algo, n, reps, sample_query=None):
    """DROP INDEX; CREATE INDEX, one rep at a time. eng.build() already drops
    all ANN indexes before building (PgEngine._drop_ann_indexes), so no new
    SQL is needed here. A generator (not a list) so the caller can write +
    flush each rep's CSV row as it lands (#78 review F5c)."""
    for _ in range(reps):
        yield eng.build(algo, n, sample_query=sample_query)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-dir", required=True)
    ap.add_argument("--n", type=int, required=True)
    ap.add_argument("--algos", default="pgcuvs_cagra,pgvector_hnsw",
                    help="comma list from " + ",".join(ALGOS))
    ap.add_argument("--build-reps", type=int, default=5)
    ap.add_argument("--dataset", default="cohere-wiki-en-1024")
    ap.add_argument("--dbname", default="postgres")
    ap.add_argument("--index-dir", default="/tmp/cuvs_indexes")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    corpus = os.path.join(args.data_dir, "corpus.fbin")
    queries_path = os.path.join(args.data_dir, "queries_10k.fbin")
    _, dim = fbin_meta(corpus)
    algos = [a.strip() for a in args.algos.split(",") if a.strip()]

    # #78 review F5b: PgEngine's own class docstring says pg_cuvs algos need
    # the daemon UP and pgvector algos need it DOWN for a VRAM-fair CPU
    # baseline -- this script (unlike pg_engine.main()'s --toggle-daemon)
    # never touches the daemon, so a mixed algo list can silently violate
    # that protocol.
    has_pgvector = any(a.startswith("pgvector_") for a in algos)
    has_pgcuvs = any(a.startswith("pgcuvs_") for a in algos)
    if has_pgvector and has_pgcuvs:
        print("[build-arm] WARN: mixing pgvector_* and pgcuvs_* algos in one "
              "run -- this script does not toggle the pg-cuvs-server daemon, "
              "so pgvector's build_time_s may not be a VRAM-fair CPU baseline "
              "if the daemon is up. See PgEngine's class docstring.", flush=True)

    host = socket.gethostname()
    gpu = gpu_name()

    eng = PgEngine(dbname=args.dbname, index_dir=args.index_dir)
    eng.load_corpus(corpus, args.n, dim, dataset=args.dataset)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    n_rows = 0
    with open(args.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=BUILD_CSV_FIELDS)
        w.writeheader()
        f.flush()
        for algo in algos:
            sample_query = None
            if algo == "pgcuvs_hnsw_import":
                sample_query = np.ascontiguousarray(read_fbin(queries_path, count=1)[0])
            params = build_params_label(algo, args.n)
            bts = []
            for i, (bt, ibytes, _meta) in enumerate(
                    run_build_reps(eng, algo, args.n, args.build_reps,
                                   sample_query=sample_query)):
                w.writerow(dict(algo=algo, build_params=params, rep=i,
                                build_time_s=round(bt, 3), index_bytes=ibytes,
                                host=host, gpu=gpu, n=args.n, dim=dim,
                                dataset=args.dataset))
                f.flush()
                n_rows += 1
                bts.append(bt)
                print(f"[build-arm] {algo} rep={i} build_time_s={bt:.2f} "
                      f"index_bytes={ibytes}", flush=True)
            print(f"[build-arm] {algo}: median={float(np.median(bts)):.2f}s "
                  f"min={min(bts):.2f}s max={max(bts):.2f}s reps={len(bts)}", flush=True)
    eng.close()
    print(f"[build-arm] {n_rows} rows -> {args.out}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
