#!/usr/bin/env python3
"""
aggregate.py - collect results/*.jsonl into summary.csv, a fixed-width text
table, and (if matplotlib is available) recall-QPS Pareto plots per (N, k).
Text output is always produced; plots are best-effort.
"""
import argparse
import csv
import glob
import json
import os
import sys


def load_rows(results_dir):
    rows = []
    for p in glob.glob(os.path.join(results_dir, "*.jsonl")):
        with open(p) as f:
            for line in f:
                line = line.strip()
                if line:
                    rows.append(json.loads(line))
    return rows


def write_csv(rows, path):
    if not rows:
        return
    fields = list(rows[0].keys())
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in rows:
            w.writerow(r)


def f(x, nd=1):
    return "-" if x is None else (f"{x:.{nd}f}" if isinstance(x, float) else str(x))


def text_table(rows, out):
    Ns = sorted(set(r["N"] for r in rows))
    ks = sorted(set(r["k"] for r in rows))
    lines = []
    for N in Ns:
        for k in ks:
            sub = [r for r in rows if r["N"] == N and r["k"] == k]
            if not sub:
                continue
            lines.append(f"\n=== N={N}  k={k}  (recall@{k} vs QPS; per-query p50/95/99 ms) ===")
            hdr = f"{'system':22} {'param':22} {'recall':>7} {'qps':>9} {'p50':>8} {'p95':>8} {'p99':>8} {'build_s':>8} {'idxMB':>8} {'gpuMB':>8}"
            lines.append(hdr)
            lines.append("-" * len(hdr))
            # sort by system then recall
            for r in sorted(sub, key=lambda r: (r["system"], -(r["recall"] or 0))):
                idxmb = "-" if r["index_bytes"] in (None, "") else f"{r['index_bytes']/1e6:.0f}"
                lines.append(
                    f"{r['system']:22} {str(r['param_set']):22} {f(r['recall'],4):>7} "
                    f"{f(r['qps']):>9} {f(r['p50_ms'],2):>8} {f(r['p95_ms'],2):>8} "
                    f"{f(r['p99_ms'],2):>8} {f(r['build_time_s'],1):>8} {idxmb:>8} {f(r['gpu_mem_mb']):>8}")
    txt = "\n".join(lines)
    with open(out, "w") as fh:
        fh.write(txt + "\n")
    print(txt)


def make_plots(rows, plot_dir):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as e:
        print(f"[aggregate] matplotlib unavailable ({e}); skipping plots")
        return
    os.makedirs(plot_dir, exist_ok=True)
    Ns = sorted(set(r["N"] for r in rows))
    ks = sorted(set(r["k"] for r in rows))
    for N in Ns:
        for k in ks:
            sub = [r for r in rows if r["N"] == N and r["k"] == k and r["recall"] is not None]
            if not sub:
                continue
            plt.figure(figsize=(8, 6))
            systems = sorted(set(r["system"] for r in sub))
            for s in systems:
                pts = sorted([(r["recall"], r["qps"]) for r in sub if r["system"] == s])
                xs = [p[0] for p in pts]
                ys = [p[1] for p in pts]
                plt.plot(xs, ys, marker="o", label=s)
            plt.xlabel(f"recall@{k}")
            plt.ylabel("QPS (queries/sec)")
            plt.yscale("log")
            plt.title(f"Recall-QPS  N={N}  k={k}  (Cohere wiki en 1024d, cosine)")
            plt.grid(True, which="both", alpha=0.3)
            plt.legend(fontsize=8)
            out = os.path.join(plot_dir, f"pareto_N{N}_k{k}.png")
            plt.savefig(out, dpi=120, bbox_inches="tight")
            plt.close()
            print(f"[aggregate] wrote {out}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results-dir", default="design/anbench/results")
    ap.add_argument("--out-dir", default="design/anbench")
    args = ap.parse_args()
    rows = load_rows(args.results_dir)
    if not rows:
        print(f"[aggregate] no results in {args.results_dir}")
        return 1
    os.makedirs(args.out_dir, exist_ok=True)
    write_csv(rows, os.path.join(args.out_dir, "summary.csv"))
    text_table(rows, os.path.join(args.out_dir, "summary.txt"))
    make_plots(rows, os.path.join(args.out_dir, "plots"))
    print(f"\n[aggregate] {len(rows)} rows -> {args.out_dir}/summary.csv,.txt,plots/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
