"""
#98 verification: the rules that decide WHICH index a benchmark row describes.

Everything here is pure (stdlib only) and runs on a CPU-only box with no
PostgreSQL, no GPU, and no cuvs_bench -- which is the point: these rules used to
live inside a module that could only be imported on the GPU VM, so the one
failure mode that silently corrupts a published benchmark (attributing a row to
the wrong build) had no test at all.
"""
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sidecar import (  # noqa: E402
    BUILD_GRIDS,
    RELATION_OF,
    SIBLING_OF,
    assert_gt_columns,
    canonical_json,
    completed_keys,
    expected_reloptions,
    expected_source_reloptions,
    ownership_record,
    pareto_point,
    parse_build_notices,
    parse_reloptions,
    reloptions_match,
    remaining_params,
    row_key,
    sidecar_matches,
)


# ── canonical_json ───────────────────────────────────────────────────────────
def test_canonical_json_is_key_order_independent():
    # Arrange: the same build config written two ways
    a = {"m": 16, "ef_construction": 64}
    b = {"ef_construction": 64, "m": 16}
    # Act / Assert: one key, or the CSV join splits one build into two
    assert canonical_json(a) == canonical_json(b) == '{"ef_construction":64,"m":16}'


def test_canonical_json_normalizes_empty():
    assert canonical_json(None) == canonical_json({}) == "{}"


def test_canonical_json_separates_different_builds():
    assert canonical_json({"m": 16}) != canonical_json({"m": 32})


# ── relation mapping ─────────────────────────────────────────────────────────
def test_relations_are_distinct_per_algo():
    # The pre-#98 map sent pgvector_hnsw and 3I to the same "t_hnsw".
    rels = list(RELATION_OF.values())
    assert len(rels) == len(set(rels))


def test_hnsw_siblings_are_mutual():
    assert SIBLING_OF["t_hnsw_pgv"] == "t_hnsw_3i"
    assert SIBLING_OF["t_hnsw_3i"] == "t_hnsw_pgv"


def test_build_grid_matches_the_plan_cell_counts():
    assert len(BUILD_GRIDS["pgvector_hnsw"]) == 4      # {16,32} x {64,128}
    assert len(BUILD_GRIDS["pgcuvs_cagra"]) == 3       # gd {32,64,128}
    assert len(BUILD_GRIDS["pgcuvs_hnsw_import"]) == 4  # {nsw,hnswlib} x {32,64}
    # intermediate_graph_degree is pinned across every GPU cell: letting it
    # track graph_degree would move source-graph quality together with M and
    # destroy the "M only" label the confounder split depends on.
    for cell in BUILD_GRIDS["pgcuvs_cagra"] + BUILD_GRIDS["pgcuvs_hnsw_import"]:
        assert cell["intermediate_graph_degree"] == 128


# ── ownership ────────────────────────────────────────────────────────────────
def test_ownership_key_accepts_only_the_same_algo_and_cfg():
    cfg = {"m": 32, "ef_construction": 128}
    side = {"t_hnsw_pgv": ownership_record("pgvector_hnsw", cfg, 94.0, 1234)}

    assert sidecar_matches(side, "pgvector_hnsw", cfg)
    assert sidecar_matches(side, "pgvector_hnsw", {"ef_construction": 128, "m": 32})
    # a different build config is a different index
    assert not sidecar_matches(side, "pgvector_hnsw", {"m": 16, "ef_construction": 128})
    # a different algo's relation has no record at all
    assert not sidecar_matches(side, "pgcuvs_cagra", cfg)
    assert not sidecar_matches({}, "pgvector_hnsw", cfg)


def test_ownership_record_carries_cost_and_notice_meta():
    rec = ownership_record("pgcuvs_hnsw_import", {"mode": "nsw"}, 35.5, 900,
                           {"max_level": 0, "mode": "nsw"})
    assert rec["build_time_seconds"] == 35.5
    assert rec["index_size_bytes"] == 900
    assert rec["notice_meta"]["max_level"] == 0


# ── reloptions ───────────────────────────────────────────────────────────────
def test_parse_reloptions_handles_null_and_pairs():
    assert parse_reloptions(None) == {}
    assert parse_reloptions(["m=32", "ef_construction=128"]) == {
        "m": "32", "ef_construction": "128"}


def test_reloptions_match_ignores_extra_catalog_keys():
    actual = parse_reloptions(["m=32", "ef_construction=128", "fillfactor=90"])
    assert reloptions_match(actual, expected_reloptions(
        "pgvector_hnsw", {"m": 32, "ef_construction": 128}))
    # int vs text must not read as a mismatch
    assert reloptions_match({"m": "32"}, {"m": 32})


def test_reloptions_match_rejects_a_different_build():
    actual = parse_reloptions(["m=16", "ef_construction=64"])
    assert not reloptions_match(actual, expected_reloptions(
        "pgvector_hnsw", {"m": 32, "ef_construction": 64}))


def test_3i_identity_splits_across_two_relations():
    cfg = {"mode": "hnswlib", "graph_degree": 32, "intermediate_graph_degree": 128}
    # the exported index knows its source and mode ...
    assert expected_reloptions("pgcuvs_hnsw_import", cfg) == {
        "source": "t_cagra", "mode": "hnswlib"}
    # ... but not the source graph's degree or build algorithm, which are
    # checked on t_cagra (build_algo is pinned, not swept -- see #98/ADR-084)
    assert expected_source_reloptions(cfg) == {
        "graph_degree": "32", "intermediate_graph_degree": "128",
        "build_algo": "ivf_pq"}


# ── NOTICE parser (two formats) ──────────────────────────────────────────────
IPC_NOTICE = ('pg_cuvs: direct import 1000000 elements (dim=768, M=16, '
              'graph_degree=32, max_level=0, mode=nsw) '
              'from cagra index 16384 into hnsw index 16400')

HNSWLIB_NOTICE = ('pg_cuvs: imported 1000000 elements (dim=768, M=32) from '
                  '"/dev/shm/pgcuvs_hnsw_16384.bin" (use_shm=1) into hnsw index 16400')


def test_parse_notice_cagra_ipc_format():
    # src/hnsw_export.c:1665 -- the only path that reports max_level
    meta = parse_build_notices(["some unrelated notice", IPC_NOTICE])
    assert meta == {"elements": 1000000, "dim": 768, "m": 16,
                    "graph_degree": 32, "max_level": 0, "mode": "nsw",
                    "notice_format": "cagra_ipc"}


def test_parse_notice_hnswlib_format_has_no_max_level():
    # src/hnsw_export.c:1163 -- a parser that knows only the other format
    # would record "no evidence" here and the hnswlib cells would look unbuilt.
    meta = parse_build_notices([HNSWLIB_NOTICE])
    assert meta["notice_format"] == "hnswlib"
    assert meta["m"] == 32 and meta["elements"] == 1000000
    assert "max_level" not in meta   # hierarchy evidence comes from entryLevel


def test_parse_notice_returns_empty_when_absent():
    assert parse_build_notices([]) == {}
    assert parse_build_notices(["NOTICE: extension pg_cuvs already exists"]) == {}


# ── ground truth ─────────────────────────────────────────────────────────────
def test_gt_assertion_needs_ten_columns():
    assert assert_gt_columns((2000, 10)) == 10
    assert assert_gt_columns((2000, 100)) == 100
    with pytest.raises(AssertionError):
        assert_gt_columns((2000, 5))


def test_gt_assertion_is_independent_of_search_k():
    # A K=400 sweep point is still scored at recall@10; asserting >= K would
    # kill a valid run.
    assert_gt_columns((2000, 10), k=10)


# ── Pareto ───────────────────────────────────────────────────────────────────
def _row(algo, param, recall, qps, **kw):
    r = {"algo": algo, "param": param, "recall": recall, "qps": qps,
         "success": True, "axis": "latency", "build_params": "{}"}
    r.update(kw)
    return r


def test_pareto_takes_highest_qps_above_the_floor():
    rows = [_row("pgcuvs_cagra", 16, 0.90, 900),   # fastest but below floor
            _row("pgcuvs_cagra", 64, 0.96, 600),
            _row("pgcuvs_cagra", 200, 0.99, 300)]
    point, fallback = pareto_point(rows, "pgcuvs_cagra")
    assert point["param"] == 64 and fallback is False


def test_pareto_falls_back_to_best_recall_and_says_so():
    rows = [_row("pgvector_hnsw", 10, 0.80, 900),
            _row("pgvector_hnsw", 40, 0.93, 400)]
    point, fallback = pareto_point(rows, "pgvector_hnsw")
    assert point["param"] == 40 and fallback is True


def test_pareto_ignores_failed_throughput_and_other_algo_rows():
    rows = [_row("pgcuvs_cagra", 400, 0.99, 9999, success=False),
            _row("pgcuvs_cagra", 16, 0.99, 8888, axis="throughput"),
            _row("pgvector_hnsw", 80, 0.99, 7777),
            _row("pgcuvs_cagra", 64, 0.96, 600)]
    point, fallback = pareto_point(rows, "pgcuvs_cagra")
    assert point["param"] == 64 and fallback is False


def test_pareto_returns_none_when_nothing_usable():
    assert pareto_point([], "pgcuvs_cagra") == (None, False)


# ── --resume ─────────────────────────────────────────────────────────────────
def test_row_key_separates_builds_of_the_same_algo_and_param():
    a = _row("pgvector_hnsw", 40, 0.95, 500, build_params='{"m":16}')
    b = _row("pgvector_hnsw", 40, 0.97, 400, build_params='{"m":32}')
    assert row_key(a) != row_key(b)


def test_resume_skips_completed_points_only():
    cfg = {"m": 16, "ef_construction": 64}
    key = canonical_json(cfg)
    prior = [_row("pgvector_hnsw", 10, 0.90, 900, build_params=key),
             _row("pgvector_hnsw", 20, 0.93, 700, build_params=key),
             # a failed point must be retried, not inherited
             _row("pgvector_hnsw", 40, 0.0, 0.0, build_params=key, success=False)]
    done = completed_keys(prior)
    todo = remaining_params(done, "pgvector_hnsw", cfg, [10, 20, 40, 80])
    assert todo == [40, 80]


def test_resume_reads_csv_string_flags():
    # csv.DictReader yields strings, not bools -- the gate must survive that.
    prior = [{"algo": "pgcuvs_cagra", "build_params": "{}", "param": "16",
              "success": "True"},
             {"algo": "pgcuvs_cagra", "build_params": "{}", "param": "32",
              "success": "False"}]
    done = completed_keys(prior)
    assert remaining_params(done, "pgcuvs_cagra", {}, [16, 32, 64]) == [32, 64]


def test_resume_does_not_skip_a_different_build_cfg():
    done = completed_keys([_row("pgvector_hnsw", 10, 0.9, 900,
                                build_params=canonical_json({"m": 16}))])
    assert remaining_params(done, "pgvector_hnsw", {"m": 32}, [10]) == [10]
