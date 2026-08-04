"""
#98 PR-D1: tests for report_recall_tables.py.

CPU-only and dependency-light on purpose -- no PostgreSQL, no GPU, and no
`cuvs_bench` import (the module under test uses stdlib csv only), so this file
is collectable and green in the tier1-shim CI job.

The fixture below is a synthetic two-axis CSV shaped like pg_cuvsbench_98:
run_pg_cuvsbench.py's CSV_FIELDS plus `build_params`, `axis` and `notes`.
"""
import csv
import io
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import report_recall_tables as rrt  # noqa: E402

FIELDS = ["algo", "param", "k", "recall", "qps", "search_time_ms",
          "p50_ms", "p95_ms", "p99_ms", "build_time_s", "index_bytes",
          "n_queries", "reused", "success", "error", "build_params", "axis",
          "notes"]


def _row(algo, param, recall, qps, axis, build_params="{}", k=10,
         p50="1.0", p95="2.0", p99="3.0", build_time="30.0", success="True",
         notes=""):
    return [algo, param, str(k), str(recall), str(qps), "1.0",
            p50, p95, p99, build_time, "1000", "2000", "False", success, "",
            build_params, axis, notes]


def _csv(rows):
    """Real csv quoting -- build_params is canonical JSON and contains commas."""
    buf = io.StringIO()
    w = csv.writer(buf)
    w.writerow(FIELDS)
    w.writerows(rows)
    buf.seek(0)
    return buf


GD32 = '{"graph_degree": 32}'
GD64 = '{"graph_degree": 64}'
PGV16 = '{"m": 16, "ef_construction": 64}'

# One cell per (algo, build_params). pgcuvs_cagra/gd32 has three latency points:
# the highest-QPS one (k=16) misses 0.95, so the 0.95 pick must be the k=32
# point, not the fastest point overall.
BASE_ROWS = [
    _row("pgcuvs_cagra", "16", 0.91, 5000, "latency", GD32, k=16),
    _row("pgcuvs_cagra", "32", 0.962, 4000, "latency", GD32, k=32),
    _row("pgcuvs_cagra", "64", 0.991, 2500, "latency", GD32, k=64),
    _row("pgcuvs_cagra", "64", 0.996, 2000, "latency", GD64, k=64),
    _row("pgvector_hnsw", "64", 0.955, 1000, "latency", PGV16),
]


def _points(rows):
    return rrt.parse_points(_csv(rows))


# --- Pareto selection ------------------------------------------------------

def test_primary_pick_is_highest_qps_clearing_threshold():
    g = rrt.group_by_axis(_points(BASE_ROWS))["latency"]
    p = g.pick("pgcuvs_cagra", GD32, 0.95)
    assert (p.param, p.qps, p.recall) == ("32", 4000.0, 0.962)
    assert p.build_time_s == 30.0  # the pick's own build time travels with it


def test_lower_threshold_admits_the_faster_point():
    g = rrt.group_by_axis(_points(BASE_ROWS))["latency"]
    assert g.pick("pgcuvs_cagra", GD32, 0.90).param == "16"


def test_failed_rows_are_never_selected():
    rows = BASE_ROWS + [_row("pgcuvs_cagra", "128", 0.99, 99999, "latency",
                             GD32, success="False")]
    g = rrt.group_by_axis(_points(rows))["latency"]
    assert g.pick("pgcuvs_cagra", GD32, 0.95).qps == 4000.0


# --- "no point clears" rendering -------------------------------------------

def test_cell_with_no_qualifying_point_is_rendered_not_dropped():
    g = rrt.group_by_axis(_points(BASE_ROWS))["latency"]
    table = "\n".join(rrt.render_table(g, 0.99))
    assert "no point clears recall >= 0.99" in table
    # the cell is still present as a row, with its identity intact
    assert PGV16 in table
    assert table.count("| pgvector_hnsw |") == 1


def test_every_cell_appears_at_every_threshold():
    points = _points(BASE_ROWS)
    g = rrt.group_by_axis(points)["latency"]
    for t in (0.90, 0.95, 0.99):
        table = "\n".join(rrt.render_table(g, t))
        for algo, bp in g.cells:
            assert algo in table and (bp in table or bp == "")


def test_nan_recall_never_clears_a_threshold():
    rows = BASE_ROWS + [_row("pgvector_hnsw", "128", "", 9999, "latency", PGV16)]
    g = rrt.group_by_axis(_points(rows))["latency"]
    assert g.pick("pgvector_hnsw", PGV16, 0.90).qps == 1000.0


# --- axis separation -------------------------------------------------------

THROUGHPUT_ROWS = [
    _row("pgcuvs_cagra_batch", "batch_k32", 0.962, 40000, "throughput", GD32,
         k=32, p50="", p95="nan", p99=""),
    _row("cuvs_batch", "batch_k32", 0.965, 90000, "throughput", GD32, k=32,
         p50="", p95="", p99=""),
    _row("pgcuvs_cagra_conc8", "conc8", 0.962, 3900, "throughput", GD32, k=32,
         notes="slice repeated 3x in window"),
    _row("pgvector_hnsw_conc8", "conc8", 0.955, 7000, "throughput", PGV16),
]


def test_axis_group_refuses_points_from_another_axis():
    points = _points(BASE_ROWS + THROUGHPUT_ROWS)
    with pytest.raises(rrt.CrossAxisError):
        rrt.AxisGroup("latency", points)


def test_group_by_axis_partitions_and_never_mixes():
    groups = rrt.group_by_axis(_points(BASE_ROWS + THROUGHPUT_ROWS))
    assert set(groups) == {"latency", "throughput"}
    for axis, g in groups.items():
        assert {p.axis for p in g.points} == {axis}
    assert len(groups["latency"].points) + len(groups["throughput"].points) == \
        len(BASE_ROWS) + len(THROUGHPUT_ROWS)


def test_no_module_function_can_take_two_axis_groups():
    """Structural guarantee: the ratio is a *method* on one AxisGroup, and no
    module-level callable accepts a second group, so a cross-axis quotient has
    no expression in this module."""
    import inspect
    for name, fn in vars(rrt).items():
        if not inspect.isfunction(fn) or name.startswith("_"):
            continue
        params = list(inspect.signature(fn).parameters)
        assert sum(1 for p in params if p in ("group", "g", "a", "b")) <= 1, \
            f"{name}{tuple(params)} could receive two groups"


def test_ratio_baseline_is_taken_from_the_same_axis_only():
    """The throughput baseline is 7x the latency baseline. If the ratio column
    leaked across axes, the latency ratios would move."""
    groups = rrt.group_by_axis(_points(BASE_ROWS + THROUGHPUT_ROWS))
    lat = groups["latency"]
    pick = lat.pick("pgcuvs_cagra", GD32, 0.95)
    assert lat.ratio_to_baseline(pick, 0.95) == pytest.approx(4000 / 1000)

    thr = groups["throughput"]
    # throughput's baseline is that axis's own pgvector arm (pgvector_hnsw_conc8,
    # 7000 QPS) -- not the latency axis's pgvector_hnsw (1000 QPS)
    tpick = thr.pick("pgcuvs_cagra_batch", GD32, 0.95)
    assert thr.ratio_to_baseline(tpick, 0.95) == pytest.approx(40000 / 7000)


def test_ratio_is_absent_when_the_axis_has_no_baseline():
    rows = [r for r in THROUGHPUT_ROWS if not r[0].startswith("pgvector")]
    g = rrt.group_by_axis(_points(rows))["throughput"]
    assert g.ratio_to_baseline(g.points[0], 0.95) is None


def test_report_keeps_axes_in_separate_sections():
    report = rrt.render_report(_points(BASE_ROWS + THROUGHPUT_ROWS))
    assert "## axis: latency" in report and "## axis: throughput" in report
    lat_i = report.index("## axis: latency")
    thr_i = report.index("## axis: throughput")
    latency_section = report[lat_i:thr_i]
    assert "batch_k32" not in latency_section
    assert "conc8" not in latency_section


def test_unknown_axis_is_a_schema_error():
    with pytest.raises(ValueError, match="axis"):
        _points([_row("pgcuvs_cagra", "32", 0.96, 4000, "")])
    with pytest.raises(ValueError, match="axis"):
        _points([_row("pgcuvs_cagra", "32", 0.96, 4000, "wall_clock")])


# --- NaN percentiles -------------------------------------------------------

def test_batch_percentiles_parse_as_nan_and_render_as_na():
    g = rrt.group_by_axis(_points(THROUGHPUT_ROWS))["throughput"]
    p = g.pick("cuvs_batch", GD32, 0.95)
    assert all(x != x for x in (p.p50_ms, p.p95_ms, p.p99_ms))  # NaN
    row = [ln for ln in rrt.render_table(g, 0.95) if "cuvs_batch |" in ln][0]
    assert row.count("n/a") == 3
    assert "nan" not in row.lower().replace("n/a", "")


def test_conc_percentiles_are_kept():
    g = rrt.group_by_axis(_points(THROUGHPUT_ROWS))["throughput"]
    row = [ln for ln in rrt.render_table(g, 0.95) if "conc8 " in ln][0]
    assert "1.000" in row and "n/a" not in row


# --- footnotes -------------------------------------------------------------

def _ids(rows):
    return [fid for fid, _ in
            rrt.collect_footnotes(rrt.group_by_axis(_points(rows)))]


def test_batch_and_conc_footnotes_emitted_when_such_rows_exist():
    ids = _ids(BASE_ROWS + THROUGHPUT_ROWS)
    assert ids == ["batch-bind", "batch-pctl", "conc-mutex", "conc-cache",
                   "ivfflat"]


def test_batch_and_conc_footnotes_absent_from_a_latency_only_file():
    assert _ids(BASE_ROWS) == ["ivfflat"]


def test_cache_hot_footnote_requires_a_repetition_note():
    rows = [r[:-1] + [""] for r in THROUGHPUT_ROWS]  # same rows, notes cleared
    ids = _ids(rows)
    assert "conc-cache" not in ids
    assert "conc-mutex" in ids  # pgcuvs conc row still present


def test_mutex_footnote_not_raised_by_pgvector_conc_alone():
    rows = [r for r in THROUGHPUT_ROWS if not r[0].startswith("pgcuvs")]
    assert "conc-mutex" not in _ids(rows)


def test_caveated_rows_are_tagged_in_the_table():
    """A caveated number must not be liftable out of the table without its
    caveat, so each row carries the ids of the footnotes it triggers."""
    g = rrt.group_by_axis(_points(THROUGHPUT_ROWS))["throughput"]
    table = rrt.render_table(g, 0.95)
    batch = [ln for ln in table if "cuvs_batch |" in ln][0]
    assert batch.rstrip().endswith("| batch-bind, batch-pctl |")
    conc = [ln for ln in table if "| pgcuvs_cagra_conc8 |" in ln][0]
    assert conc.rstrip().endswith("| conc-mutex, conc-cache |")
    pgv = [ln for ln in table if "| pgvector_hnsw_conc8 |" in ln][0]
    assert pgv.rstrip().endswith("| - |")


def test_every_table_row_has_the_same_column_count():
    g = rrt.group_by_axis(_points(BASE_ROWS))["latency"]
    table = rrt.render_table(g, 0.99)  # contains a "no point clears" row
    widths = {ln.count("|") for ln in table}
    assert len(widths) == 1, table


def test_notes_column_is_optional():
    """PR-A's CSV_FIELDS adds `build_params` and `axis`; `notes` may or may not
    be there. Without it the report still renders -- only the cache-hot
    footnote, whose evidence lives in notes, drops out."""
    buf = io.StringIO()
    w = csv.writer(buf)
    keep = [i for i, f in enumerate(FIELDS) if f != "notes"]
    w.writerow([FIELDS[i] for i in keep])
    w.writerows([[r[i] for i in keep] for r in BASE_ROWS + THROUGHPUT_ROWS])
    buf.seek(0)
    points = rrt.parse_points(buf)
    ids = [fid for fid, _ in rrt.collect_footnotes(rrt.group_by_axis(points))]
    assert ids == ["batch-bind", "batch-pctl", "conc-mutex", "ivfflat"]
    assert "## axis: throughput" in rrt.render_report(points)


def test_footnote_texts_reach_the_rendered_report():
    report = rrt.render_report(_points(BASE_ROWS + THROUGHPUT_ROWS),
                               source="synthetic.csv")
    assert "bind parameters" in report
    assert "global index mutex" in report
    assert "implementation-status" in report
    assert "cache-hot" in report
    assert "pgvector_ivfflat" in report
    assert "synthetic.csv" in report


# --- CLI -------------------------------------------------------------------

def test_cli_writes_markdown_to_out_file(tmp_path, capsys):
    src = tmp_path / "run.csv"
    src.write_text(_csv(BASE_ROWS + THROUGHPUT_ROWS).getvalue())
    out = tmp_path / "tables.md"
    assert rrt.main([str(src), "--out", str(out)]) == 0
    text = out.read_text()
    assert "## axis: latency" in text and "no point clears" in text


def test_cli_writes_markdown_to_stdout(tmp_path, capsys):
    src = tmp_path / "run.csv"
    src.write_text(_csv(BASE_ROWS).getvalue())
    assert rrt.main([str(src)]) == 0
    assert "# #98 recall/QPS tables" in capsys.readouterr().out
