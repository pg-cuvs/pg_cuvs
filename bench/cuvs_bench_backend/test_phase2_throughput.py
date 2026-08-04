"""
#98 PR-C verification: the throughput axis's pure logic, without a database.

Everything that decides *what a throughput row says* is tested here: the
client-side top-k cut, the CSV row contract the report tool parses, the
Pareto-rebuild decision, resume skipping, the consistency gates, and the worker
GUC replication. The measurement itself needs the GPU VM; these rules do not,
and they are the ones that can silently mislabel a published number.
"""
import os
import sys

import numpy as np
import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import conc_runner  # noqa: E402
import report_recall_tables as report  # noqa: E402
import run_pg_cuvsbench as runner  # noqa: E402
from pg_engine import truncate_topk  # noqa: E402
from sidecar import CAGRA_BUILD_ALGO, canonical_json  # noqa: E402


class Args:
    """Minimal stand-in for the runner's parsed argv."""
    def __init__(self, **kw):
        self.k = 10
        self.batch_recall_tol = None
        self.__dict__.update(kw)


@pytest.fixture(autouse=True)
def _clear_gate_state():
    for bucket in (runner.GATE_VIOLATIONS, runner.GATE_OBSERVATIONS):
        bucket.clear()
    yield
    for bucket in (runner.GATE_VIOLATIONS, runner.GATE_OBSERVATIONS):
        bucket.clear()


# ── client-side top-k truncation ─────────────────────────────────────────────
def test_truncate_keeps_the_first_top_k_rows_per_query():
    # SQL is ORDER BY query_idx, distance, so rows arrive already sorted; the
    # cut keeps the leading top_k of each query and nothing else.
    rows = [(0, 100, 0.1), (0, 101, 0.2), (0, 102, 0.3),
            (1, 200, 0.05), (1, 201, 0.15)]
    ids = truncate_topk(rows, nq=2, top_k=2)
    assert ids.tolist() == [[100, 101], [200, 201]]


def test_truncate_marks_short_queries_as_misses():
    ids = truncate_topk([(0, 7, 0.1)], nq=2, top_k=3)
    assert ids.tolist() == [[7, -1, -1], [-1, -1, -1]]


def test_truncate_ignores_out_of_range_query_idx():
    # A split dispatch shifts query_idx back into the block's space; a value
    # outside it means the shift was wrong, and silently folding it into some
    # other query's neighbours would corrupt recall instead of showing the bug.
    ids = truncate_topk([(5, 9, 0.1), (0, 3, 0.2)], nq=1, top_k=2)
    assert ids.tolist() == [[3, -1]]


def test_truncate_does_not_resort():
    # If the statement ever loses its ORDER BY, recall must degrade visibly
    # rather than being repaired here.
    ids = truncate_topk([(0, 1, 9.0), (0, 2, 0.1)], nq=1, top_k=1)
    assert ids.tolist() == [[1]]


# ── CSV row contract (parsed by report_recall_tables.py) ─────────────────────
def test_batch_row_param_is_what_the_report_reads_as_a_batch_row():
    row = runner.throughput_row("pgcuvs_cagra_batch", "batch_k200", 10, 0.97,
                                4200.0, 476.0, '{"graph_degree":64}', 2000)
    assert row["axis"] == "throughput"
    pt = _as_point(row)
    assert pt.is_batch and not pt.is_conc
    assert "batch-bind" in report.footnote_ids_for(pt)
    assert "batch-pctl" in report.footnote_ids_for(pt)


def test_conc_row_param_is_what_the_report_reads_as_a_conc_row():
    row = runner.throughput_row("pgcuvs_cagra_conc32", "conc32", 10, 0.99,
                                1300.0, 30000.0, "{}", 39000,
                                p50=6.1, p95=6.3, p99=7.0)
    pt = _as_point(row)
    assert pt.is_conc and not pt.is_batch
    # the mutex caveat must attach to every pgcuvs conc row
    assert "conc-mutex" in report.footnote_ids_for(pt)


def test_pgvector_conc_row_is_the_reports_throughput_baseline():
    rows = [runner.throughput_row("pgvector_hnsw_conc32", "conc32", 10, 0.98,
                                  500.0, 30000.0, "{}", 15000),
            runner.throughput_row("pgcuvs_cagra_conc32", "conc32", 10, 0.99,
                                  1300.0, 30000.0, "{}", 39000)]
    group = report.AxisGroup("throughput", [_as_point(r) for r in rows])
    pt = group.pick("pgcuvs_cagra_conc32", "{}", 0.95)
    assert group.ratio_to_baseline(pt, 0.95) == pytest.approx(2.6)


def test_batch_row_records_nan_percentiles():
    row = runner.throughput_row("pgcuvs_cagra_batch", "batch_k200", 10, 0.97,
                                4200.0, 476.0, "{}", 2000)
    assert row["p50_ms"] == "nan"
    assert np.isnan(_as_point(row).p50_ms)


def test_throughput_rows_fit_the_csv_schema():
    row = runner.throughput_row("pgcuvs_cagra_batch", "batch_k200", 10, 0.97,
                                4200.0, 476.0, "{}", 2000)
    assert set(row) <= set(runner.CSV_FIELDS)


def _as_point(row):
    import csv
    import io
    buf = io.StringIO()
    w = csv.DictWriter(buf, fieldnames=runner.CSV_FIELDS, extrasaction="ignore")
    w.writeheader()
    w.writerow(row)
    buf.seek(0)
    return report.parse_points(buf)[0]


# ── Pareto -> rebuild decision ───────────────────────────────────────────────
class FakeEngine:
    """Just enough PgEngine to exercise ensure_pareto_index's decision."""

    def __init__(self, relopts):
        self._relopts = relopts
        self.built = []

    def reloptions(self, name):
        return self._relopts.get(name)

    def build(self, algo, n, build_cfg=None, keep=()):
        self.built.append((algo, canonical_json(build_cfg), tuple(keep)))
        # what the real DDL leaves in pg_class.reloptions: the cell, plus the
        # build_algo pin PgEngine._cagra_with() always writes.
        opts = {k: str(v) for k, v in build_cfg.items()}
        opts["build_algo"] = CAGRA_BUILD_ALGO
        self._relopts["t_cagra"] = opts
        return 33.0, 4242, {}


def _sidecar(tmp_path, monkeypatch, content):
    import json
    p = tmp_path / "pg_current_index.json"
    p.write_text(json.dumps(content))
    monkeypatch.setattr(runner, "_sidecar_path", lambda: str(p))
    return p


def test_no_rebuild_when_the_pareto_cell_is_already_resident(tmp_path, monkeypatch):
    cfg = {"graph_degree": 64, "intermediate_graph_degree": 128}
    _sidecar(tmp_path, monkeypatch,
             {"t_cagra": {"algo": "pgcuvs_cagra", "build_cfg": canonical_json(cfg),
                          "build_time_seconds": 35.0, "index_size_bytes": 99}})
    eng = FakeEngine({"t_cagra": {"graph_degree": "64",
                                  "intermediate_graph_degree": "128",
                                  "build_algo": CAGRA_BUILD_ALGO}})
    bt, ibytes, rebuilt = runner.ensure_pareto_index(eng, "pgcuvs_cagra", cfg, 100)
    assert (bt, ibytes, rebuilt) == (35.0, 99, False)
    assert eng.built == []


def test_rebuild_when_phase1_left_a_different_cell_resident(tmp_path, monkeypatch):
    # the honest case the plan calls out: Phase 1 ends on the LAST cell of the
    # grid, which is generally not the Pareto cell.
    want = {"graph_degree": 64, "intermediate_graph_degree": 128}
    last = {"graph_degree": 128, "intermediate_graph_degree": 128}
    _sidecar(tmp_path, monkeypatch,
             {"t_cagra": {"algo": "pgcuvs_cagra", "build_cfg": canonical_json(last),
                          "build_time_seconds": 70.0, "index_size_bytes": 1}})
    eng = FakeEngine({"t_cagra": {"graph_degree": "128",
                                  "intermediate_graph_degree": "128"}})
    bt, ibytes, rebuilt = runner.ensure_pareto_index(eng, "pgcuvs_cagra", want, 100)
    assert rebuilt is True and bt == 33.0 and ibytes == 4242
    assert eng.built == [("pgcuvs_cagra", canonical_json(want), ())]


def test_pinned_build_algo_forces_one_rebuild_and_then_settles(tmp_path,
                                                               monkeypatch):
    # PR-B pinned build_algo='ivf_pq' in both the DDL and expected_reloptions.
    # An index built before the pin (reloptions carry no build_algo) must fail
    # the gate once -- and the rebuild must then satisfy it, or Phase 2 would
    # rebuild the same cell on every arm.
    cfg = {"graph_degree": 64, "intermediate_graph_degree": 128}
    _sidecar(tmp_path, monkeypatch,
             {"t_cagra": {"algo": "pgcuvs_cagra", "build_cfg": canonical_json(cfg),
                          "build_time_seconds": 35.0, "index_size_bytes": 99}})
    eng = FakeEngine({"t_cagra": {"graph_degree": "64",
                                  "intermediate_graph_degree": "128"}})
    _bt, _b, rebuilt = runner.ensure_pareto_index(eng, "pgcuvs_cagra", cfg, 100)
    assert rebuilt is True
    # FakeEngine.build() writes back what the real DDL writes, build_algo pin
    # included -- so the second call must be a no-op.
    _bt, _b, again = runner.ensure_pareto_index(eng, "pgcuvs_cagra", cfg, 100)
    assert again is False, "Pareto rebuild did not settle -- every arm rebuilds"


def test_rebuild_when_the_sidecar_agrees_but_the_catalog_does_not(tmp_path,
                                                                 monkeypatch):
    # a sidecar can outlive the index it describes; reloptions is the server's
    # own statement and wins.
    cfg = {"graph_degree": 64, "intermediate_graph_degree": 128}
    _sidecar(tmp_path, monkeypatch,
             {"t_cagra": {"algo": "pgcuvs_cagra", "build_cfg": canonical_json(cfg),
                          "build_time_seconds": 35.0, "index_size_bytes": 99}})
    eng = FakeEngine({})   # relation absent
    _bt, _b, rebuilt = runner.ensure_pareto_index(eng, "pgcuvs_cagra", cfg, 100)
    assert rebuilt is True


# ── Phase-2 resume ───────────────────────────────────────────────────────────
def _tp_csv_row(algo, param, build_params="{}", success="True"):
    return {"algo": algo, "param": param, "build_params": build_params,
            "axis": "throughput", "success": success, "recall": "0.99",
            "qps": "1300"}


def test_phase2_resume_skips_recorded_rows_and_retries_failed_ones():
    prior = [_tp_csv_row("pgcuvs_cagra_conc8", "conc8"),
             _tp_csv_row("pgcuvs_cagra_conc16", "conc16", success="False")]
    done = {(r["algo"], r["build_params"], r["param"]) for r in prior
            if r["success"] == "True"}
    assert ("pgcuvs_cagra_conc8", "{}", "conc8") in done
    assert ("pgcuvs_cagra_conc16", "{}", "conc16") not in done


def test_phase2_only_plans_no_phase1_segment():
    # the recovery path: Phase 1 is already in the CSV, so re-entering it would
    # re-measure (or, when a build flakes, re-abort) work that is already done.
    assert runner.phase1_segments(Args(phase2_only=True), set(),
                                  ["pgcuvs_cagra"]) == []


def test_without_phase2_only_the_unmeasured_cells_are_still_planned():
    segs = runner.phase1_segments(Args(phase2_only=False), set(), ["pgcuvs_cagra"])
    assert len(segs) == 3                      # the three graph_degree cells
    assert all(todo for _a, _c, todo in segs)


def test_pareto_ignores_throughput_rows_when_recomputed_from_the_csv():
    # a resumed run recomputes the Pareto point from the file, which by then
    # also holds throughput rows -- those must not be selectable as one.
    lat = {"algo": "pgcuvs_cagra", "param": "100", "recall": "0.97",
           "qps": "600", "axis": "latency", "success": "True",
           "build_params": '{"graph_degree":64}'}
    tp = dict(_tp_csv_row("pgcuvs_cagra", "conc32"), qps="99999")
    picked = runner.select_pareto([lat, tp], ["pgcuvs_cagra"])
    assert picked["pgcuvs_cagra"]["param"] == "100"


# ── consistency gates ────────────────────────────────────────────────────────
def test_fallback_gate_is_hard_and_passes_only_on_a_zero_delta():
    assert runner.gate_fallback("a", 0).startswith("gate-ok")
    assert not runner.GATE_VIOLATIONS
    # HARD: a nonzero delta means the arm may have measured a CPU exact search
    # under a GPU arm's label, so the run must not exit clean.
    assert runner.gate_fallback("a", 3).startswith("GATE-VIOLATION")
    assert runner.GATE_VIOLATIONS


def test_conc1_ratio_gate_accepts_the_expected_band():
    assert runner.gate_conc1_ratio("a", 90.0, 100.0).startswith("gate-ok")
    assert not runner.GATE_VIOLATIONS and not runner.GATE_OBSERVATIONS


def test_conc1_ratio_violation_is_observational_not_fatal():
    # conc N=1 is wall-clock and must be the LOWER number; above 1.0 one of the
    # two stopped describing the same query. Worth recording -- but the rows are
    # still measurements, and the plan says this check never aborts. A non-zero
    # exit here would make gpu-run.sh record a completed run as state=failed.
    txt = runner.gate_conc1_ratio("a", 130.0, 100.0)
    assert txt.startswith("GATE-OBSERVATION")
    assert runner.GATE_OBSERVATIONS
    assert not runner.GATE_VIOLATIONS, "observational gate must not set exit code"


def test_batch_recall_gate_is_observe_and_record_without_a_tolerance():
    txt = runner.gate_batch_recall(Args(), "a", 0.90, 0.99)
    assert txt.startswith("observe-and-record")
    assert "0.9000" in txt and "0.9900" in txt   # both recalls recorded
    assert not runner.GATE_VIOLATIONS            # nothing is gated


def test_batch_recall_gate_uses_the_measured_tolerance_when_given():
    args = Args(batch_recall_tol=0.02)
    assert runner.gate_batch_recall(args, "a", 0.97, 0.98).startswith("gate-ok")
    # HARD: past the tolerance the batch arm is not returning the neighbours of
    # the point it is labelled with.
    assert runner.gate_batch_recall(args, "a", 0.90, 0.99).startswith("GATE-VIOL")
    assert runner.GATE_VIOLATIONS


# ── concurrency runner (pure parts) ──────────────────────────────────────────
def test_slices_are_disjoint_and_cover_the_full_query_pool():
    sl = conc_runner.slices_for(10000, 64)
    assert len(sl) == 64
    allq = np.concatenate(sl)
    assert len(allq) == 10000 and len(set(allq.tolist())) == 10000


def test_slices_never_hand_a_worker_an_empty_range():
    assert all(len(s) for s in conc_runner.slices_for(5, 8))


def test_conc_worker_formats_its_literal_inside_the_timed_loop():
    # The measurement boundary for BOTH axes is the full round trip the
    # application experiences, and formatting the inline vector literal
    # (~0.45 ms/query at dim=768) is part of what it pays. Precomputing it made
    # conc N=1 come out 1.33x the latency axis for a reason unrelated to
    # concurrency. The batch arm's bind parameter is the only sanctioned
    # exception to the same-statement-shape rule.
    import inspect
    src = inspect.getsource(conc_runner._worker)
    body = src.split("def one(j):")[1].split("# Plan guard")[0]
    assert "_vec_literal(qvecs[j])" in body, \
        "the timed statement must build its own literal, as the latency axis does"
    assert "lits = [" not in src, "no precomputed literal list may come back"


def test_worker_gucs_replicate_the_operating_point_per_algo():
    g = conc_runner.worker_gucs("pgcuvs_cagra", 100, "/tmp/ix")
    assert "SET enable_seqscan = off" in g
    assert "SET cuvs.k = 100" in g
    assert "SET cuvs.shard_count = 1" in g
    assert "SET cuvs.index_dir = '/tmp/ix'" in g


def test_pgvector_workers_are_forced_onto_the_cpu_path():
    g = conc_runner.worker_gucs("pgvector_hnsw", 200, "/tmp/ix")
    assert "SET enable_cuvs = off" in g
    assert "SET hnsw.ef_search = 200" in g
    assert not any("cuvs.k" in s for s in g)


def test_conc_arm_refuses_an_algo_it_has_no_operating_point_for():
    with pytest.raises(ValueError):
        conc_runner.worker_gucs("pgvector_ivfflat", 8, "/tmp/ix")


def test_conc_recall_scores_only_gt_covered_queries():
    got = np.array([[1, 2], [3, 4]])
    gt = np.array([[1, 9], [3, 4]])
    assert conc_runner._recall(got, gt, 2) == 0.75
