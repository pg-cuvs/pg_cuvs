#!/usr/bin/env python3
"""
report_recall_tables.py -- render the #98 recall/QPS tables from one run CSV.

Input is a single `pg_cuvsbench_98`-shaped CSV: the CSV_FIELDS that
run_pg_cuvsbench.py already writes, plus `build_params` (canonical JSON of the
build config) and `axis` (`latency` | `throughput`). `notes` is read when
present and ignored when absent.

Output is markdown (stdout, or --out <file>):

  * primary table   -- per axis x algo x build_params, the highest-QPS point
    whose recall clears 0.95, with that point's build_time_s. This is the
    ann-benchmarks convention: a system is summarised by its fastest
    configuration at a fixed recall, not by its fastest configuration.
  * secondary tables -- the same selection at 0.90 and 0.99. An algo/build_cfg
    with no point clearing a threshold is rendered as an explicit
    "no point clears <t>" row; it is never silently dropped.
  * footnotes -- emitted only when the rows in this file warrant them.

## Why this tool refuses to divide across axes

The two axes measure different quantities. The latency axis serialises one
query at a time and reports percentiles; the throughput axis measures sustained
QPS of a batch dispatch or of N concurrent connections. A latency-axis QPS
divided by a throughput-axis QPS is not a speedup of anything. #98 exists
because BENCHMARK.md's headlines were built by stitching numbers that were
never commensurable, so this tool is built so that the stitch cannot be
expressed: every point is carried inside an `AxisGroup`, an AxisGroup refuses
to hold two axes, and the only ratio the module can compute
(`AxisGroup.ratio_to_baseline`) is a method on one group -- there is no
function anywhere that takes two groups.

Usage:
    python bench/cuvs_bench_backend/report_recall_tables.py \
        bench/results/pg_cuvsbench_98.csv --out bench/results/pg98_tables.md
"""
import argparse
import csv
import math
import sys
from dataclasses import dataclass, field

PRIMARY_THRESHOLD = 0.95
SECONDARY_THRESHOLDS = (0.90, 0.99)

BASELINE_ALGO = "pgvector_hnsw"

#: Axis label -> one-line statement of what the axis measures. Rendered above
#: each axis section so a reader cannot mistake one axis's QPS for the other's.
AXIS_BLURB = {
    "latency": "one query at a time, serial; QPS = nq / sum(latency). "
               "Percentiles are per-query.",
    "throughput": "sustained QPS of the arm's real dispatch mechanism "
                  "(batch round trip, or N concurrent connections).",
}


class CrossAxisError(ValueError):
    """Raised when points from more than one axis reach a single group."""


def _to_float(s):
    """CSV cell -> float. Empty / 'nan' / unparseable all become NaN, which
    compares False against every threshold, so such a point can never be
    selected as a Pareto pick."""
    if s is None:
        return float("nan")
    s = str(s).strip()
    if not s:
        return float("nan")
    try:
        return float(s)
    except ValueError:
        return float("nan")


def _to_bool(s):
    return str(s).strip().lower() in ("true", "1", "t", "yes")


@dataclass(frozen=True)
class Point:
    """One CSV row, typed. `axis` is read once here and never again outside
    AxisGroup construction."""
    axis: str
    algo: str
    build_params: str
    param: str
    k: str
    recall: float
    qps: float
    build_time_s: float
    p50_ms: float
    p95_ms: float
    p99_ms: float
    success: bool
    notes: str = ""

    @property
    def is_batch(self):
        return self.param.startswith("batch_k")

    @property
    def is_conc(self):
        return self.param.startswith("conc")


def load_points(path):
    """Read the run CSV into Points. Rows with an unknown/blank axis are a
    schema error, not something to guess about."""
    with open(path, newline="") as f:
        return parse_points(f)


def parse_points(fh):
    rows = list(csv.DictReader(fh))
    points = []
    for i, r in enumerate(rows, start=2):  # header is line 1
        axis = (r.get("axis") or "").strip()
        if axis not in AXIS_BLURB:
            raise ValueError(
                f"line {i}: axis={axis!r} is not one of {sorted(AXIS_BLURB)}; "
                "this CSV is not a #98 two-axis run")
        points.append(Point(
            axis=axis,
            algo=(r.get("algo") or "").strip(),
            build_params=(r.get("build_params") or "").strip(),
            param=(r.get("param") or "").strip(),
            k=(r.get("k") or "").strip(),
            recall=_to_float(r.get("recall")),
            qps=_to_float(r.get("qps")),
            build_time_s=_to_float(r.get("build_time_s")),
            p50_ms=_to_float(r.get("p50_ms")),
            p95_ms=_to_float(r.get("p95_ms")),
            p99_ms=_to_float(r.get("p99_ms")),
            success=_to_bool(r.get("success")),
            notes=(r.get("notes") or "").strip(),
        ))
    return points


@dataclass
class AxisGroup:
    """Every point that belongs to one axis of one file.

    Construction is the single choke point where axis homogeneity is enforced;
    everything downstream takes an AxisGroup, so no rendering or arithmetic can
    reach across axes."""
    axis: str
    points: list = field(default_factory=list)

    def __post_init__(self):
        foreign = sorted({p.axis for p in self.points if p.axis != self.axis})
        if foreign:
            raise CrossAxisError(
                f"AxisGroup({self.axis!r}) was handed points from {foreign}; "
                "cross-axis aggregation is not representable in this tool")

    @property
    def cells(self):
        """(algo, build_params) keys in first-seen order -- the unit a table row
        summarises."""
        seen = []
        for p in self.points:
            key = (p.algo, p.build_params)
            if key not in seen:
                seen.append(key)
        return seen

    def pick(self, algo, build_params, threshold):
        """Highest-QPS point of this cell whose recall clears `threshold`, or
        None when the cell has no such point."""
        candidates = [p for p in self.points
                      if p.algo == algo and p.build_params == build_params
                      and p.success and p.recall >= threshold]
        if not candidates:
            return None
        return max(candidates, key=lambda p: p.qps)

    def ratio_to_baseline(self, point, threshold):
        """`point`'s QPS over the best baseline-algo QPS *in this same group*.

        The baseline is any arm of the baseline algo present on this axis --
        `pgvector_hnsw` on the latency axis, `pgvector_hnsw_conc{N}` on the
        throughput axis. Both operands come from self.points, so the quotient is
        within one axis and one file by construction. Returns None when this
        axis has no baseline point clearing the threshold."""
        base = [p for p in self.points
                if p.algo.startswith(BASELINE_ALGO) and p.success
                and p.recall >= threshold]
        if not base or point is None:
            return None
        best = max(base, key=lambda p: p.qps).qps
        if not best or math.isnan(best) or math.isnan(point.qps):
            return None
        return point.qps / best


def group_by_axis(points):
    """The only axis split in the module. Returns axis -> AxisGroup, in the
    canonical axis order."""
    out = {}
    for axis in AXIS_BLURB:
        sel = [p for p in points if p.axis == axis]
        if sel:
            out[axis] = AxisGroup(axis, sel)
    return out


def _fmt(v, nd=3, na="n/a"):
    if v is None or (isinstance(v, float) and math.isnan(v)):
        return na
    return f"{v:.{nd}f}"


_HEADER = ("| algo | build_params | search param | k | recall | QPS | "
           f"vs {BASELINE_ALGO} | p50 ms | p95 ms | p99 ms | build s | caveats |")
_RULE = "|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|"
#: the 9 columns after (algo, build_params, search param), left blank on a
#: "no point clears" row so every row keeps the header's column count
_EMPTY_TAIL = "| " * 9 + "|"


def render_table(group, threshold):
    """Markdown table of one axis at one recall threshold. Cells with no
    qualifying point get an explicit 'no point clears' row."""
    lines = [_HEADER, _RULE]
    for algo, build_params in group.cells:
        p = group.pick(algo, build_params, threshold)
        if p is None:
            lines.append(
                f"| {algo} | {build_params or '-'} | "
                f"**no point clears recall >= {threshold:.2f}** {_EMPTY_TAIL}")
            continue
        ratio = group.ratio_to_baseline(p, threshold)
        ratio_s = "-" if ratio is None else f"{ratio:.2f}x"
        caveats = ", ".join(footnote_ids_for(p)) or "-"
        lines.append(
            f"| {algo} | {build_params or '-'} | {p.param} | {p.k} | "
            f"{_fmt(p.recall, 4)} | {_fmt(p.qps, 1)} | {ratio_s} | "
            f"{_fmt(p.p50_ms)} | {_fmt(p.p95_ms)} | {_fmt(p.p99_ms)} | "
            f"{_fmt(p.build_time_s, 1)} | {caveats} |")
    return lines


# --- footnotes -------------------------------------------------------------
# Each entry is (id, predicate over one Point or None for file-scope, text).
# A footnote is emitted only when some row in the file triggers it, so the
# report never carries a caveat its own rows do not warrant -- and every
# triggering row is tagged with the caveat's id, so a caveated number cannot be
# lifted out of the table without it.

FN_BATCH_BIND = (
    "batch-bind",
    lambda p: p.is_batch,
    "Batch arms send the query block as psycopg **bind parameters**, not as an "
    "inline literal -- a deliberate exception to this suite's "
    "same-statement-shape rule. 2000x768 vectors inlined is a ~29 MB statement, "
    "so parse time would dominate the measurement.")

FN_BATCH_PCTL = (
    "batch-pctl",
    lambda p: p.is_batch,
    "Batch arms have **undefined per-query latency percentiles** (recorded NaN, "
    "rendered `n/a`): one dispatch returns the whole block, so there is no "
    "per-query latency to take percentiles of.")

FN_CONC_MUTEX = (
    "conc-mutex",
    lambda p: p.is_conc and p.algo.startswith("pgcuvs"),
    "`pgcuvs_*_conc{N}` rows were measured while the daemon serialises "
    "non-sharded single searches under its **global index mutex** "
    "(`pg_cuvs_server.c`), so QPS is expected to be flat in N. This is an "
    "**implementation-status** statement about the current daemon, not a claim "
    "about the algorithm; pg_cuvs's throughput mechanism is the batch arm.")

FN_CONC_CACHE = (
    "conc-cache",
    lambda p: p.is_conc and "repeat" in p.notes.lower(),
    "At least one concurrency arm exhausted its disjoint query slice and "
    "repeated it within the measurement window (see the row's notes). Those "
    "rows are **cache-hot** to that degree.")

FN_IVFFLAT = (
    "ivfflat",
    None,  # file-scope: always stated, tags no row
    "`pgvector_ivfflat` is excluded by design: #98 compares four algos and "
    "pgvector's representative recall/QPS curve is HNSW, so an IVFFlat curve "
    "would add a second pgvector line without adding a comparison.")

FOOTNOTES = (FN_BATCH_BIND, FN_BATCH_PCTL, FN_CONC_MUTEX, FN_CONC_CACHE, FN_IVFFLAT)


def footnote_ids_for(point):
    """Caveat ids that this single row carries."""
    return [fid for fid, predicate, _ in FOOTNOTES
            if predicate is not None and predicate(point)]


def collect_footnotes(groups):
    """Footnote texts warranted by the rows in `groups`, in declaration order."""
    out = []
    for fid, predicate, text in FOOTNOTES:
        if predicate is None or any(predicate(p) for g in groups.values()
                                    for p in g.points):
            out.append((fid, text))
    return out


def render_report(points, source=None):
    groups = group_by_axis(points)
    lines = ["# #98 recall/QPS tables", ""]
    if source:
        lines += [f"Source: `{source}` (single file, single run).", ""]
    lines += [
        "Each section below is one **axis**. Ratios are computed only within a "
        "section; the two axes measure different quantities and are never "
        "divided into each other.",
        "",
    ]
    for axis, group in groups.items():
        lines += [f"## axis: {axis}", "", AXIS_BLURB[axis], ""]
        lines += [f"### primary -- recall >= {PRIMARY_THRESHOLD:.2f} "
                  "(highest QPS clearing the threshold)", ""]
        lines += render_table(group, PRIMARY_THRESHOLD)
        lines.append("")
        for t in SECONDARY_THRESHOLDS:
            lines += [f"### secondary -- recall >= {t:.2f}", ""]
            lines += render_table(group, t)
            lines.append("")
    notes = collect_footnotes(groups)
    if notes:
        lines += ["## Notes", ""]
        for i, (fid, text) in enumerate(notes, start=1):
            lines.append(f"{i}. ({fid}) {text}")
        lines.append("")
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("csv", help="pg_cuvsbench_98 CSV (one run, one file)")
    ap.add_argument("--out", help="write markdown here instead of stdout")
    args = ap.parse_args(argv)

    report = render_report(load_points(args.csv), source=args.csv)
    if args.out:
        with open(args.out, "w") as f:
            f.write(report)
        print(f"[pg98] report -> {args.out}", flush=True)
    else:
        sys.stdout.write(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
