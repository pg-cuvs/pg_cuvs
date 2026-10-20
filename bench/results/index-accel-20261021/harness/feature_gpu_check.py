#!/usr/bin/env python3
"""Private feature validation on an assigned, new index-accel VM only.

Prepare one graph on the baseline snapshot. Measure baseline, baseline after a
daemon restart, GUC defaults, RMM disabled, and RMM enabled without rebuilding
that graph. Each label gets a new output directory; baseline comparisons check
both graph hashes and exact bytes of returned ids/distances. The sweep compares
single and explicit batch AUTO search on that same graph; it does not claim to
have profiled or explicitly forced a particular CUDA kernel.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def graph_metadata(conn, index_dir):
    db_oid, index_oid, ddl = conn.execute(
        "SELECT d.oid, c.oid, pg_get_indexdef(c.oid) FROM pg_database d, pg_class c "
        "WHERE d.datname=current_database() AND c.oid='t_cagra'::regclass"
    ).fetchone()
    files = {}
    for suffix in ("cagra", "tids", "vectors"):
        path = Path(index_dir) / f"{db_oid}_{index_oid}.{suffix}"
        if path.exists():
            files[path.name] = {"bytes": path.stat().st_size, "sha256": sha256(path)}
    if not any(name.endswith(".cagra") for name in files):
        raise RuntimeError("resident graph file not found")
    return {"db_oid": db_oid, "index_oid": index_oid, "ddl": ddl, "files": files}


def setting(conn, name):
    row = conn.execute("SELECT setting FROM pg_settings WHERE name=%s", (name,)).fetchone()
    return row[0] if row else "not_supported"


def save_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def compare_ids(old, new, truth):
    jaccard = 0.0
    changed = reorder_only = gt_outside = within_gt = lost = gained = 0
    for a, b, gt in zip(old, new, truth):
        sa, sb, sg = set(a.tolist()), set(b.tolist()), set(gt.tolist())
        jaccard += len(sa & sb) / len(sa | sb) if sa | sb else 1.0
        if (a != b).any():
            changed += 1
            removed, added = sa - sb, sb - sa
            if sa == sb:
                reorder_only += 1
            elif (removed | added) <= sg:
                within_gt += 1
            else:
                gt_outside += 1
            lost += len(removed & sg)
            gained += len(added & sg)
    return {"mean_query_jaccard": jaccard / len(old), "id_changed_queries": changed,
            "reorder_only_queries": reorder_only, "within_gt_only_changed_queries": within_gt,
            "gt_outside_involved_queries": gt_outside, "gt_hits_lost": lost, "gt_hits_gained": gained}


def run():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("prepare", "measure", "sweep"))
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--owner-file", type=Path, required=True)
    parser.add_argument("--expected-commit", required=True)
    parser.add_argument("--dbname", default="index_accel_smoke")
    parser.add_argument("--data-dir", type=Path, required=True)
    parser.add_argument("--index-dir", default="/tmp/cuvs_indexes")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--noise-reference", type=Path)
    parser.add_argument("--require-byte-equality", action="store_true")
    parser.add_argument("--enforce-noise-floor", action="store_true")
    parser.add_argument("--n", type=int, default=1_000_000)
    parser.add_argument("--queries", type=int, default=10_000)
    parser.add_argument("--points", default="64,128")
    parser.add_argument("--itopks", default="64,128,256,512")
    parser.add_argument("--batch-queries", type=int, default=1_000)
    parser.add_argument("--expect-rmm-mb", type=int)
    parser.add_argument("--graph-degree", type=int, default=32)
    parser.add_argument("--intermediate-degree", type=int, default=128)
    parser.add_argument("--build-algo", choices=("ivf_pq", "nn_descent"), default="ivf_pq")
    args = parser.parse_args()
    owner = json.loads(args.owner_file.read_text())
    if not owner["vm_name"].startswith("pg-cuvs-index-accel-"):
        raise RuntimeError("this script requires the new lane-owned VM")
    if (not owner.get("vm_id") or owner["vm_id"] == "dc9tztkyr"
            or not args.dbname.startswith("index_accel_")):
        raise RuntimeError("protected host or nonprivate database")
    commit = subprocess.check_output(
        ["git", "-C", str(args.repo), "rev-parse", "HEAD"], text=True
    ).strip()
    expected = subprocess.check_output(
        ["git", "-C", str(args.repo), "rev-parse", args.expected_commit], text=True
    ).strip()
    if commit != expected:
        raise RuntimeError(f"source snapshot mismatch: {commit} != {expected}")
    sys.path.insert(0, str(args.repo / "bench/cuvs_bench_backend"))
    import numpy as np
    from pg_engine import PgEngine, read_fbin, recall_at_k, _vec_literal

    args.out.mkdir(parents=True, exist_ok=False)
    eng = PgEngine(dbname=args.dbname, index_dir=args.index_dir)
    conn = eng.conn
    try:
        conn.execute("SET statement_timeout = '15min'")
        conn.execute("SET enable_seqscan = off")
        conn.execute("SET cuvs.shard_count = 1")
        conn.execute("SET cuvs.cagra_batch_wait_us = 0")
        conn.execute("SET cuvs.bf_batch_wait_us = 0")
        conn.execute("SELECT set_config('cuvs.index_dir', %s, false)", (args.index_dir,))
        for name in ("cuvs.itopk_size", "cuvs.search_width"):
            if setting(conn, name) != "not_supported":
                conn.execute("SELECT set_config(%s,'0',false)", (name,))
        pool = setting(conn, "cuvs.rmm_pool_mb")
        if args.expect_rmm_mb is not None and pool != str(args.expect_rmm_mb):
            raise RuntimeError(f"applied pool mismatch: {pool} != {args.expect_rmm_mb}")
        metadata = {
            "owner": owner, "commit": commit, "postgres": conn.execute("SELECT version()").fetchone()[0],
            "extensions": conn.execute("SELECT extname, extversion FROM pg_extension ORDER BY extname").fetchall(),
            "n": args.n, "queries": args.queries, "rmm_pool_mb": pool,
            "selection": "AUTO; no kernel trace collected", "mode": args.mode,
            "harness_file_sha256": sha256(__file__),
            "harness": "private pg_cuvs feature validation; PgEngine loader/batch; serial id+distance SQL",
            "search_settings": {name: setting(conn, name) for name in
                                ("cuvs.itopk_size", "cuvs.search_width", "cuvs.cagra_batch_wait_us",
                                 "cuvs.bf_batch_wait_us", "cuvs.search_mode")},
        }
        if args.mode == "prepare":
            eng.load_corpus(str(args.data_dir / "corpus.fbin"), args.n, 768, dataset="wiki_all_1M")
            conn.execute("SET maintenance_work_mem = '8GB'")
            conn.execute("SET max_parallel_maintenance_workers = 7")
            conn.execute("DROP INDEX IF EXISTS t_cagra")
            started = time.perf_counter()
            conn.execute(
                "CREATE INDEX t_cagra ON t USING cagra (embedding vector_l2_ops) "
                f"WITH (graph_degree={args.graph_degree}, "
                f"intermediate_graph_degree={args.intermediate_degree}, build_algo='{args.build_algo}')"
            )
            metadata["create_index_seconds"] = time.perf_counter() - started
            metadata["graph"] = graph_metadata(conn, args.index_dir)
            save_json(args.out / "metadata.json", metadata)
            return

        queries = np.ascontiguousarray(read_fbin(str(args.data_dir / "queries_10k.fbin")))[:args.queries]
        truth = np.load(args.data_dir / f"gt_{args.n}.npy", mmap_mode="r")[:len(queries), :10]
        if len(queries) != args.queries or len(truth) != len(queries):
            raise RuntimeError("query or ground-truth count does not match the fixed protocol")
        before = graph_metadata(conn, args.index_dir)
        metadata["graph"] = before
        metadata["query_bytes_sha256"] = hashlib.sha256(queries.tobytes()).hexdigest()
        metadata["truth_bytes_sha256"] = hashlib.sha256(np.asarray(truth).tobytes()).hexdigest()
        relations = [conn.execute("SELECT 't'::regclass::oid").fetchone()[0]]
        toast = conn.execute("SELECT reltoastrelid FROM pg_class WHERE oid='t'::regclass").fetchone()[0]
        if toast:
            relations.append(toast)
            relations.extend(row[0] for row in conn.execute("SELECT indexrelid FROM pg_index WHERE indrelid=%s", (toast,)).fetchall())
        metadata["prewarm_buffer_blocks"] = {
            str(oid): conn.execute("SELECT pg_prewarm(%s::regclass)", (str(oid),)).fetchone()[0]
            for oid in relations
        }
        metadata["warming"] = "PG restarted per arm/control; buffer prewarm of heap/toast/index, then 100 query warmups per point"
        metadata["shared_buffers"] = conn.execute("SHOW shared_buffers").fetchone()[0]
        metrics = []

        def gpu_mark():
            row = conn.execute(
                "SELECT search_count, error_count, search_mode FROM pg_stat_gpu_search "
                "WHERE index_oid='t_cagra'::regclass"
            ).fetchone()
            if row is None:
                raise RuntimeError("resident index missing from daemon statistics")
            return row

        def single(k):
            conn.execute("SELECT set_config('cuvs.k',%s,false)", (str(k),))
            literals = [_vec_literal(q) for q in queries]
            sql = "SELECT id, embedding <-> '{}'::vector FROM t ORDER BY embedding <-> '{}'::vector LIMIT 10"
            probe = sql.format(literals[0], literals[0])
            plan = "\n".join(row[0] for row in conn.execute("EXPLAIN " + probe).fetchall())
            if "t_cagra" not in plan or "Seq Scan" in plan:
                raise RuntimeError("CAGRA index not selected: " + plan)
            for literal in literals[:min(100, len(literals))]:
                conn.execute(sql.format(literal, literal)).fetchall()
            start_mark = gpu_mark()
            ids = np.full((len(queries), 10), -1, dtype=np.int64)
            distances = np.full((len(queries), 10), np.nan, dtype=np.float64)
            elapsed = []
            for i, literal in enumerate(literals):
                t0 = time.perf_counter()
                rows = conn.execute(sql.format(literal, literal)).fetchall()
                elapsed.append(time.perf_counter() - t0)
                if len(rows) != 10:
                    raise RuntimeError(f"query {i}: short fill {len(rows)}")
                ids[i] = [row[0] for row in rows]
                distances[i] = [row[1] for row in rows]
            end_mark = gpu_mark()
            dispatch = {"observed_gpu_queries": end_mark[0] - start_mark[0],
                        "observed_gpu_errors": end_mark[1] - start_mark[1],
                        "observed_search_mode": end_mark[2]}
            if dispatch["observed_gpu_queries"] != len(queries) or dispatch["observed_gpu_errors"]:
                raise RuntimeError("unexpected daemon query/error delta: " + repr(dispatch))
            if dispatch["observed_search_mode"] != "gpu_cagra":
                raise RuntimeError("unexpected search mode: " + repr(dispatch))
            return ids, distances, elapsed, dispatch

        if args.mode == "measure":
            for k in map(int, args.points.split(",")):
                ids, distances, elapsed, dispatch = single(k)
                np.save(args.out / f"k{k}-ids.npy", ids)
                np.save(args.out / f"k{k}-distances.npy", distances)
                same = None
                comparison = {}
                if args.baseline:
                    prior = json.loads((args.baseline / "metadata.json").read_text())
                    if prior["graph"] != before or prior["query_bytes_sha256"] != metadata["query_bytes_sha256"]:
                        raise RuntimeError("comparison graph or queries differ from baseline")
                    if prior["truth_bytes_sha256"] != metadata["truth_bytes_sha256"]:
                        raise RuntimeError("ground truth differs from baseline")
                    old_ids = np.load(args.baseline / f"k{k}-ids.npy")
                    same = (ids.tobytes() == old_ids.tobytes()
                            and distances.tobytes() == np.load(args.baseline / f"k{k}-distances.npy").tobytes())
                    comparison = compare_ids(old_ids, ids, truth)
                row = {"k": k, "recall_at10": recall_at_k(ids, truth, 10),
                                "qps": len(queries) / sum(elapsed),
                                "p50_ms": float(np.percentile(elapsed, 50) * 1000),
                                "byte_identical_to_baseline": same, **dispatch, **comparison}
                if args.noise_reference:
                    noise = json.loads(args.noise_reference.read_text())
                    if noise["graph"] != before:
                        raise RuntimeError("noise baseline graph differs")
                    point = noise["points"][str(k)]
                    for field in ("recall_at10", "p50_ms"):
                        delta = row[field] - point[field]["mean"]
                        span = point[field]["max"] - point[field]["min"]
                        row[field + "_delta_to_control_mean"] = delta
                        row[field + "_control_span"] = span
                        row[field + "_within_control_span"] = abs(delta) <= span + 1e-12
                metrics.append(row)
        else:
            if setting(conn, "cuvs.itopk_size") == "not_supported":
                raise RuntimeError("sweep requires the GUC feature snapshot")
            conn.execute("SET cuvs.search_width = 1")
            for itopk in map(int, args.itopks.split(",")):
                conn.execute("SELECT set_config('cuvs.itopk_size',%s,false)", (str(itopk),))
                ids, _, elapsed, dispatch = single(64)
                chunks = (len(queries) + args.batch_queries - 1) // args.batch_queries
                batch_plan, batch_seqscan = eng.batch_plan_guard(queries[:args.batch_queries], 64)
                batch_start = gpu_mark()
                fallback_start = eng.fallback_count()
                batch, times, notes = eng.batch_search(
                    queries, 64, top_k=10, warmup=2, repeats=3, chunks=chunks
                )
                batch_end = gpu_mark()
                batch_dispatch = {
                    "batch_observed_gpu_dispatches": batch_end[0] - batch_start[0],
                    "batch_observed_gpu_errors": batch_end[1] - batch_start[1],
                    "batch_observed_fallbacks": eng.fallback_count() - fallback_start,
                    "batch_observed_search_mode": batch_end[2],
                    "batch_plan_has_seqscan": batch_seqscan,
                }
                if (batch_dispatch["batch_observed_gpu_dispatches"] != chunks * 5
                        or batch_dispatch["batch_observed_gpu_errors"]
                        or batch_dispatch["batch_observed_fallbacks"]
                        or batch_dispatch["batch_observed_search_mode"] != "gpu_cagra"
                        or (batch < 0).any()):
                    raise RuntimeError("batch dispatch/short-fill guard failed: " + repr(batch_dispatch))
                (args.out / f"itopk{itopk}-batch-plan.txt").write_text(batch_plan + "\n")
                rs = recall_at_k(ids, truth, 10)
                rb = recall_at_k(batch, truth, 10)
                np.save(args.out / f"itopk{itopk}-single-ids.npy", ids)
                np.save(args.out / f"itopk{itopk}-batch-ids.npy", batch)
                metrics.append({"itopk_size": itopk, "search_width": 1, "returned_k": 64,
                                "single_recall_at10": rs, "batch_recall_at10": rb,
                                "batch_minus_single": rb - rs,
                                "single_qps": len(queries) / sum(elapsed),
                                "batch_qps": len(queries) / float(np.median(times)),
                                "batch_dispatches": chunks,
                                "cta_mode": "AUTO/unobserved", "notes": notes, **dispatch, **batch_dispatch})
        if graph_metadata(conn, args.index_dir) != before:
            raise RuntimeError("resident graph artifacts changed during measurement")
        save_json(args.out / "metadata.json", metadata)
        save_json(args.out / "metrics.json", metrics)
        if metrics:
            with (args.out / "metrics.csv").open("w", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=list(metrics[0]))
                writer.writeheader()
                writer.writerows(metrics)
        if args.mode == "sweep":
            drops = {}
            for field in ("single_recall_at10", "batch_recall_at10"):
                drops[field] = [
                    {"from_itopk": previous["itopk_size"], "at_itopk": current["itopk_size"],
                     "delta": current[field] - previous[field]}
                    for previous, current in zip(metrics, metrics[1:])
                    if current[field] < previous[field]
                ]
            drops["first_batch_below_single"] = next(
                (row["itopk_size"] for row in metrics if row["batch_minus_single"] < 0), None
            )
            save_json(args.out / "recall-drops.json", drops)
            print("FIRST OBSERVED RECALL DROP: " + json.dumps(drops), flush=True)
        print(json.dumps(metrics, indent=2), flush=True)
        if args.require_byte_equality and any(row["byte_identical_to_baseline"] is False for row in metrics):
            raise RuntimeError("default behavior differs from the same-graph baseline; evidence saved")
        if args.enforce_noise_floor and any(
                not row.get(field + "_within_control_span", False)
                for row in metrics for field in ("recall_at10", "p50_ms")):
            raise RuntimeError("default smoke differs beyond the observed control span; evidence saved")
    finally:
        eng.close()


if __name__ == "__main__":
    run()
