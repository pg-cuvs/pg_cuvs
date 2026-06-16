#!/usr/bin/env python3
"""d1_pareto.py — D1 resource/$ Pareto from the protocol CSV (post-hoc).

The runner doesn't stamp price (the instance_type/price bench.yml inputs don't
exist — main-branch blocked), so we apply a known instance $/hr post-hoc to the
measured qps/p99/VRAM rows. Emits, per (config, cell):
  $/1M queries  = price_hr * 1e6 / (qps * 3600)
  $/QPS         = price_hr / qps            (cost to hold 1 sustained QPS)
  resident VRAM (peak_vram_mb)              (the D1/D6 VRAM-budget axis)
and flags the recall-vs-$ Pareto frontier (no other point is both cheaper and
higher-recall). Usage:
  python3 tools/d1_pareto.py results/protocol/A.csv [--price 3.67]
Default price = GCP a2-highgpu-1g (1x A100-40GB) on-demand, us-central1.
"""
import argparse
import csv
import sys


def f(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--price", type=float, default=3.67, help="instance $/hr")
    ap.add_argument("--cell", default=None, help="filter to one cell_id")
    a = ap.parse_args()

    rows = []
    with open(a.csv) as fh:
        for r in csv.DictReader(fh):
            if r.get("phase") != "query":
                continue
            if a.cell and r.get("cell_id") != a.cell:
                continue
            qps, p99 = f(r.get("qps")), f(r.get("p99_us"))
            if not qps or qps <= 0:
                continue
            rows.append({
                "config": r["config"], "cell": r["cell_id"],
                "recall": f(r.get("recall_at_k")), "qps": qps, "p99_ms": (p99 or 0) / 1000,
                "vram_mb": f(r.get("peak_vram_mb")),
                "usd_1m": a.price * 1e6 / (qps * 3600),
                "usd_qps": a.price / qps,
            })
    if not rows:
        sys.exit("no query rows with qps in " + a.csv)

    # Pareto frontier on (recall ↑, $/1M ↓): keep points not dominated on both.
    for x in rows:
        x["pareto"] = not any(
            y is not x and (y["recall"] or 0) >= (x["recall"] or 0)
            and y["usd_1m"] <= x["usd_1m"]
            and ((y["recall"] or 0) > (x["recall"] or 0) or y["usd_1m"] < x["usd_1m"])
            for y in rows if y["cell"] == x["cell"])

    print(f"# D1 resource/$ Pareto  price=${a.price}/hr  ({len(rows)} points)")
    print(f"{'config':>20} {'cell':>22} {'recall':>7} {'qps':>8} {'p99ms':>7} "
          f"{'vramMB':>7} {'$/1M':>8} {'$/QPS':>9} {'pareto':>7}")
    for x in sorted(rows, key=lambda r: (r["cell"], r["usd_1m"])):
        vr = f"{x['vram_mb']:.0f}" if x["vram_mb"] is not None else "-"
        rc = f"{x['recall']:.4f}" if x["recall"] is not None else "-"
        print(f"{x['config']:>20} {x['cell']:>22} {rc:>7} {x['qps']:>8.0f} "
              f"{x['p99_ms']:>7.2f} {vr:>7} {x['usd_1m']:>8.4f} {x['usd_qps']:>9.5f} "
              f"{'  *' if x['pareto'] else '':>7}")


if __name__ == "__main__":
    main()
