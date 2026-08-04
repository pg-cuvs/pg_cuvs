"""
#98 verification: the runner's segment gate and its resume/CSV plumbing.

Imports run_pg_cuvsbench at module level on purpose -- it must stay importable
without cuvs_bench installed (the orchestrator import moved inside main()), so
these rules are testable off the GPU VM. numpy is the only third-party import
reached, via pg_engine.
"""
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_pg_cuvsbench as runner  # noqa: E402
from sidecar import canonical_json  # noqa: E402


def _row(param, recall=0.96, success=True, reused=False, build_params="{}"):
    return {"algo": "pgcuvs_cagra", "param": param, "k": 10, "recall": recall,
            "qps": 500.0, "search_time_ms": 4.0, "p50_ms": 1.9, "p95_ms": 2.5,
            "p99_ms": 3.0, "build_time_s": 35.0, "index_bytes": 100,
            "n_queries": 2000, "build_params": build_params, "axis": "latency",
            "reused": reused, "success": success, "error": "", "notes": ""}


# ── segment gate ─────────────────────────────────────────────────────────────
def test_segment_ok_returns_a_summary():
    rows = [_row(16), _row(32), _row(64)]
    s = runner.validate_segment(rows, 3, "pgcuvs_cagra", "{}")
    assert "rows=3" in s and "reused=False" in s


def test_segment_aborts_on_missing_rows():
    with pytest.raises(runner.SegmentError, match="expected 3"):
        runner.validate_segment([_row(16)], 3, "pgcuvs_cagra", "{}")


def test_segment_aborts_on_a_failed_row():
    rows = [_row(16), _row(32, success=False)]
    rows[1]["error"] = "daemon down"
    with pytest.raises(runner.SegmentError, match="daemon down"):
        runner.validate_segment(rows, 2, "pgcuvs_cagra", "{}")


def test_segment_aborts_on_impossible_recall():
    with pytest.raises(runner.SegmentError, match="recall outside"):
        runner.validate_segment([_row(16, recall=1.4)], 1, "pgcuvs_cagra", "{}")


def test_segment_aborts_when_rows_name_another_build():
    rows = [_row(16, build_params='{"graph_degree":32}')]
    with pytest.raises(runner.SegmentError, match="build_params"):
        runner.validate_segment(rows, 1, "pgcuvs_cagra", '{"graph_degree":64}')


def test_segment_aborts_when_the_index_changed_mid_sweep():
    # half the sweep reused the index and half rebuilt it -> the rows do not
    # describe one index, so the segment's build_time means nothing.
    rows = [_row(16, reused=True), _row(32, reused=False)]
    with pytest.raises(runner.SegmentError, match="mixed reused"):
        runner.validate_segment(rows, 2, "pgcuvs_cagra", "{}")


# ── CSV / resume ─────────────────────────────────────────────────────────────
def test_fresh_run_writes_a_header(tmp_path):
    out = str(tmp_path / "r.csv")
    f, w = runner.open_csv(out, resume=False)
    w.writerow(_row(16))
    f.close()
    rows = runner.read_existing(out)
    assert len(rows) == 1 and rows[0]["algo"] == "pgcuvs_cagra"


def test_resume_appends_without_a_second_header(tmp_path):
    out = str(tmp_path / "r.csv")
    f, w = runner.open_csv(out, resume=False)
    w.writerow(_row(16))
    f.close()

    f, w = runner.open_csv(out, resume=True)
    w.writerow(_row(32))
    f.close()

    rows = runner.read_existing(out)
    assert [r["param"] for r in rows] == ["16", "32"]


def test_resume_on_a_missing_file_starts_fresh(tmp_path):
    out = str(tmp_path / "nope.csv")
    assert runner.read_existing(out) == []
    f, w = runner.open_csv(out, resume=True)
    w.writerow(_row(16))
    f.close()
    assert len(runner.read_existing(out)) == 1


# ── Pareto selection over CSV rows ───────────────────────────────────────────
def test_select_pareto_reports_per_algo_points_and_fallbacks():
    cfg = canonical_json({"graph_degree": 64})
    rows = [_row(16, recall=0.90, build_params=cfg),
            _row(64, recall=0.97, build_params=cfg)]
    rows[1]["qps"] = 400.0
    low = dict(_row(10, recall=0.80), algo="pgvector_hnsw")
    picked = runner.select_pareto(rows + [low],
                                  ["pgcuvs_cagra", "pgvector_hnsw"])
    assert picked["pgcuvs_cagra"]["param"] == 64
    assert picked["pgcuvs_cagra"]["fallback"] is False
    assert picked["pgcuvs_cagra"]["build_params"] == cfg
    assert picked["pgvector_hnsw"]["fallback"] is True
