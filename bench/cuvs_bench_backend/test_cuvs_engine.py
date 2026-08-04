"""
#98 verification: the raw cuVS arm's pure logic.

Everything here runs on a CPU-only box with neither cupy nor cuvs installed --
cuvs_engine imports them lazily inside the methods precisely so these rules can
be tested where CI runs. What is covered is what can silently mis-describe a
published row: which build a raw cell asks for, which search params it derives
(the axis-internal comparability with pgcuvs_cagra rests entirely on those being
identical), and what the row's notes say about GPU tenancy.

The GPU paths themselves (build/search against a real index) are verified by
hand on the VM; see the PR-B smoke numbers.
"""
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cuvs_engine import (  # noqa: E402
    BUILD_ALGO,
    METRIC,
    RAW_ALGO,
    RAW_SIDECAR_KEY,
    CuvsEngine,
    gpu_note,
    itopk_for,
    validate_build_cfg,
)
from pg_engine import ALGOS, DEFAULT_SWEEPS, LATENCY_ALGOS, RAW_ALGOS  # noqa: E402
from sidecar import BUILD_GRIDS, INTERMEDIATE_GRAPH_DEGREE  # noqa: E402


# ── build config validation ──────────────────────────────────────────────────
def test_validate_build_cfg_returns_degrees():
    # Arrange
    cfg = {"graph_degree": 64, "intermediate_graph_degree": 128}
    # Act
    gd, igd = validate_build_cfg(cfg)
    # Assert
    assert (gd, igd) == (64, 128)


def test_validate_build_cfg_defaults_intermediate_to_the_pinned_value():
    # The #98 grid pins intermediate at 128 in every cell; a cfg that omits it
    # must land on the same value rather than on cuvs's own default, or the
    # "graph_degree only" label the confounder split depends on breaks.
    gd, igd = validate_build_cfg({"graph_degree": 32})
    assert (gd, igd) == (32, INTERMEDIATE_GRAPH_DEGREE)


def test_validate_build_cfg_rejects_missing_graph_degree():
    with pytest.raises(ValueError, match="graph_degree"):
        validate_build_cfg({"intermediate_graph_degree": 128})


def test_validate_build_cfg_rejects_nonpositive_graph_degree():
    with pytest.raises(ValueError, match="positive"):
        validate_build_cfg({"graph_degree": 0})


def test_validate_build_cfg_rejects_intermediate_below_graph_degree():
    # Same constraint the extension enforces (src/pg_cuvs.c:1265).
    with pytest.raises(ValueError, match="must be >="):
        validate_build_cfg({"graph_degree": 128, "intermediate_graph_degree": 64})


# ── search-param derivation ──────────────────────────────────────────────────
@pytest.mark.parametrize("k,expected", [
    (10, 64),     # floor
    (16, 64),     # floor
    (64, 64),     # exact multiple, at the floor
    (65, 96),     # rounds up to the next multiple of 32
    (100, 128),
    (200, 224),
    (400, 416),
])
def test_itopk_for_mirrors_the_extension(k, expected):
    # src/cuvs_wrapper.cu:1122-1124 -- round up to a multiple of 32, floor 64.
    assert itopk_for(k) == expected


def test_itopk_is_always_at_least_k():
    # An itopk below the requested top-k cannot return k neighbours.
    for k in DEFAULT_SWEEPS[RAW_ALGO]:
        assert itopk_for(k) >= k


# ── sweep / grid tables ──────────────────────────────────────────────────────
def test_raw_sweep_is_identical_to_pgcuvs_cagra():
    # This identity IS the axis-internal comparison: the raw arm anchors the
    # integration tax only if it is swept over the same knob values.
    assert DEFAULT_SWEEPS[RAW_ALGO] == DEFAULT_SWEEPS["pgcuvs_cagra"]


def test_raw_build_grid_is_identical_to_pgcuvs_cagra():
    assert BUILD_GRIDS[RAW_ALGO] == BUILD_GRIDS["pgcuvs_cagra"]


def test_every_raw_build_cell_validates():
    for cell in BUILD_GRIDS[RAW_ALGO]:
        gd, igd = validate_build_cfg(cell)
        assert igd == INTERMEDIATE_GRAPH_DEGREE and gd in (32, 64, 128)


def test_raw_algo_is_outside_the_postgres_algo_list():
    # PgEngine.build() asserts against ALGOS and cannot build a raw index, so
    # the raw name must not leak into it -- but it must be in the latency-axis
    # list and in the sweep table (a missing sweep entry is a KeyError at run
    # time, not a graceful skip).
    assert RAW_ALGO not in ALGOS
    assert RAW_ALGO in RAW_ALGOS
    assert RAW_ALGO in LATENCY_ALGOS
    assert RAW_ALGO in DEFAULT_SWEEPS


def test_raw_sidecar_key_is_not_a_relation_name():
    # The raw ownership record must never be mistaken for something to_regclass
    # or reloptions() could be asked about.
    assert RAW_SIDECAR_KEY.startswith("raw:")


def test_metric_matches_the_sql_arms():
    # vector_l2_ops on unit-norm vectors == cosine ranking; the raw arm must
    # rank the same way or its recall is not comparable.
    assert METRIC == "sqeuclidean"
    assert BUILD_ALGO == "ivf_pq"


# ── row notes ────────────────────────────────────────────────────────────────
def test_gpu_note_reports_before_after_and_delta():
    note = gpu_note(before=1000.0, after=4500.0)
    assert "gpu_mem_used_mb_before=1000" in note
    assert "gpu_mem_used_mb=4500" in note
    assert "gpu_mem_used_mb_delta=3500" in note


def test_gpu_note_omits_what_it_does_not_have():
    # nvidia-smi absent -> the note must not fabricate a zero reading.
    note = gpu_note()
    assert note == "raw-arm=wall-clock"
    assert "gpu_mem_used_mb" not in note


def test_gpu_note_marks_the_row_as_the_wall_clock_arm():
    # The note is where a CSV reader learns these rows are not kernel time.
    assert gpu_note(after=42.0).startswith("raw-arm=wall-clock")


# ── engine state machine (no GPU needed) ─────────────────────────────────────
def test_fresh_engine_owns_no_index():
    assert CuvsEngine().has_index({"graph_degree": 64}) is False


def test_has_index_is_false_for_an_invalid_cfg():
    # A malformed cell must read as "not ours", not raise out of the reuse gate.
    assert CuvsEngine().has_index({}) is False


def test_search_before_build_is_a_clear_error():
    eng = CuvsEngine()
    # cupy/cuvs may be absent here; either the missing import or the missing
    # index is a legitimate refusal -- what must not happen is a silent result.
    with pytest.raises((RuntimeError, ImportError, ModuleNotFoundError)):
        eng.search(RAW_ALGO, [[0.0]], 10, 16)


def test_build_rejects_a_foreign_algo():
    eng = CuvsEngine()
    with pytest.raises((ValueError, RuntimeError, ImportError, ModuleNotFoundError)):
        eng.build("pgcuvs_cagra", 100, build_cfg={"graph_degree": 64})
