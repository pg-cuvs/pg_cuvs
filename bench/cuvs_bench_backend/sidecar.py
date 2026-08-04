#!/usr/bin/env python3
"""
sidecar.py -- pure helpers for the #98 build-parameter sweep.

Deliberately dependency-free (stdlib only, no cuvs_bench, no psycopg, no numpy)
so every rule that decides *which index a measurement describes* is unit-testable
on a CPU-only box with nothing installed -- that is exactly the class of bug that
silently mis-attributes a published benchmark row.

What lives here:

  RELATION_OF        algo -> SQL relation name. Replaces backend.py's _INDEX_OF,
                     whose pgvector_hnsw and pgcuvs_hnsw_import both mapped to
                     "t_hnsw": the two indexes collided, so the reuse gate could
                     not tell whose index was resident.
  canonical_json     the stable string form of a build config. This string is the
                     join key between a build row and its search rows in the CSV,
                     and the ownership key in the sidecar -- so it must not depend
                     on dict ordering or float formatting.
  ownership records  {relation: {algo, build_cfg, build_time_seconds,
                                 index_size_bytes, notice_meta}}
  expected_reloptions / reloptions_match
                     the second half of the reuse gate: what pg_class.reloptions
                     must say for the resident index to be the one we asked for.
  parse_build_notices
                     the 3I evidence path. pg_cuvs emits two different NOTICEs
                     depending on conversion mode (src/hnsw_export.c:1665 for the
                     cagra-IPC path, which carries max_level; :1163 for the
                     hnswlib path, which does not) -- a parser that knows only
                     one format silently records "no evidence" for the other.
  pareto_point / completed_keys
                     Phase-2 point selection and --resume row skipping.
"""
import json
import re

# ── relation mapping ─────────────────────────────────────────────────────────
# One relation per algo. pgvector's HNSW and pg_cuvs's exported 3I HNSW are both
# hnsw-shaped indexes on the same column, so they get distinct names AND are
# mutually exclusive (see SIBLING_OF): two candidate index paths on one column
# make the planner's choice -- and therefore what a row measures -- ambiguous.
RELATION_OF = {
    "pgvector_hnsw": "t_hnsw_pgv",
    "pgvector_ivfflat": "t_ivf",
    "pgcuvs_cagra": "t_cagra",
    "pgcuvs_hnsw_import": "t_hnsw_3i",
}

# All relations this harness may create (superset of RELATION_OF.values() plus
# t_cagra, which 3I also builds as its source graph).
ALL_RELATIONS = ("t_hnsw_pgv", "t_hnsw_3i", "t_ivf", "t_cagra")

# Never resident at the same time.
SIBLING_OF = {"t_hnsw_pgv": "t_hnsw_3i", "t_hnsw_3i": "t_hnsw_pgv"}

# The 3I export needs a source CAGRA graph, so building it also builds t_cagra.
SOURCE_RELATION = "t_cagra"

# Build grid for #98 (shared by both axes; see the plan's grid table).
# intermediate_graph_degree is pinned at 128 across every cell: the usual
# 2x-graph_degree rule would move the source-graph quality together with M,
# which destroys the "M only" label the confounder split depends on.
INTERMEDIATE_GRAPH_DEGREE = 128

BUILD_GRIDS = {
    "pgvector_hnsw": [{"m": m, "ef_construction": efc}
                      for m in (16, 32) for efc in (64, 128)],
    "pgcuvs_cagra": [{"graph_degree": gd,
                      "intermediate_graph_degree": INTERMEDIATE_GRAPH_DEGREE}
                     for gd in (32, 64, 128)],
    "pgcuvs_hnsw_import": [{"mode": mode, "graph_degree": gd,
                            "intermediate_graph_degree": INTERMEDIATE_GRAPH_DEGREE}
                           for mode in ("nsw", "hnswlib") for gd in (32, 64)],
    # Raw cuVS CAGRA (no Postgres). Same cells as pgcuvs_cagra by construction:
    # the raw arm is only interpretable as an integration-tax anchor if its
    # index is built with the parameters the SQL arm's index was built with.
    "cuvs": [{"graph_degree": gd,
              "intermediate_graph_degree": INTERMEDIATE_GRAPH_DEGREE}
             for gd in (32, 64, 128)],
    # ivfflat is explicitly out of #98's 4-algo scope (pgvector's representative
    # curve is HNSW); kept buildable with a single cell so nothing crashes if a
    # caller asks for it.
    "pgvector_ivfflat": [{}],
}


# ── canonical build-config string ────────────────────────────────────────────
def canonical_json(build_cfg):
    """Stable string form of a build config.

    Sorted keys and no whitespace, so `{"m": 16, "ef_construction": 64}` and
    `{"ef_construction": 64, "m": 16}` produce the same key -- the CSV join
    between a build row and its search rows, and the sidecar ownership key,
    both rely on that. None/{} canonicalize to "{}" so an unparameterized
    build still has one well-defined key rather than two spellings.
    """
    return json.dumps(build_cfg or {}, sort_keys=True, separators=(",", ":"))


# ── ownership records ────────────────────────────────────────────────────────
def ownership_record(algo, build_cfg, build_time_seconds, index_size_bytes,
                     notice_meta=None):
    """One sidecar entry: what this relation is, and what it cost to make."""
    return {
        "algo": algo,
        "build_cfg": canonical_json(build_cfg),
        "build_time_seconds": float(build_time_seconds),
        "index_size_bytes": int(index_size_bytes),
        "notice_meta": notice_meta or {},
    }


def sidecar_matches(sidecar, algo, build_cfg):
    """True when the sidecar claims algo's relation was built with build_cfg.

    This is only the *claim*; the caller must still confirm the relation exists
    and that pg_class.reloptions agrees (see expected_reloptions) before reusing
    it. A sidecar alone can outlive the index it describes -- that is precisely
    how a stale build_time gets attributed to a fresh index.
    """
    entry = (sidecar or {}).get(RELATION_OF.get(algo, ""))
    if not entry:
        return False
    return (entry.get("algo") == algo
            and entry.get("build_cfg") == canonical_json(build_cfg))


# ── reloptions ───────────────────────────────────────────────────────────────
def parse_reloptions(reloptions):
    """pg_class.reloptions (a text[] of 'key=value', or NULL) -> dict of str.

    Values stay strings: reloptions are stored as text and comparing them as
    text avoids an int/str mismatch making a matching index look stale.
    """
    out = {}
    for item in reloptions or ():
        if "=" in item:
            k, v = item.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def expected_reloptions(algo, build_cfg):
    """What pg_class.reloptions must contain for `algo` built with build_cfg.

    Only the keys this harness sets are checked -- a superset in the catalog is
    fine. Returns {} for algos whose reloptions carry no build identity.

    Note the 3I entry checks source/mode only: the exported HNSW's own
    reloptions do not record the source graph's degree, so graph_degree is
    verified separately against the SOURCE relation's reloptions (see
    expected_source_reloptions).
    """
    cfg = build_cfg or {}
    if algo == "pgvector_hnsw":
        return {k: str(cfg[k]) for k in ("m", "ef_construction") if k in cfg}
    if algo == "pgcuvs_cagra":
        return {k: str(cfg[k])
                for k in ("graph_degree", "intermediate_graph_degree") if k in cfg}
    if algo == "pgcuvs_hnsw_import":
        exp = {"source": SOURCE_RELATION}
        if "mode" in cfg:
            exp["mode"] = str(cfg["mode"])
        return exp
    return {}


def expected_source_reloptions(build_cfg):
    """reloptions the 3I cell's source t_cagra must carry (graph_degree etc.)."""
    cfg = build_cfg or {}
    return {k: str(cfg[k])
            for k in ("graph_degree", "intermediate_graph_degree") if k in cfg}


def reloptions_match(actual, expected):
    """True when every expected key is present in actual with the same value.

    Extra keys in `actual` are ignored (PostgreSQL may record defaults we never
    set); a missing or differing key is a mismatch -> rebuild.
    """
    for k, v in (expected or {}).items():
        if str(actual.get(k)) != str(v):
            return False
    return True


# ── build NOTICE parser ──────────────────────────────────────────────────────
# src/hnsw_export.c:1665-1668 -- the cagra-IPC path (mode nsw / hnsw). Carries
# max_level, which is the hierarchy evidence the #98 confounder split needs.
_NOTICE_IPC = re.compile(
    r"direct import (?P<elements>\d+) elements \("
    r"dim=(?P<dim>\d+), M=(?P<m>\d+), graph_degree=(?P<graph_degree>\d+), "
    r"max_level=(?P<max_level>-?\d+), mode=(?P<mode>[A-Za-z_]+)\)")

# src/hnsw_export.c:1163-1166 -- the hnswlib path (mode hnswlib / hnswlib_file).
# NO max_level and NO mode field: the hierarchy evidence for these cells must
# come from pgvector's metapage (entryLevel), not from this NOTICE.
_NOTICE_HNSWLIB = re.compile(
    r"imported (?P<elements>\d+) elements \("
    r"dim=(?P<dim>\d+), M=(?P<m>\d+)\) from ")


def parse_build_notices(notices):
    """Extract 3I build metadata from a list of NOTICE message strings.

    Returns a dict with whichever of {elements, dim, m, graph_degree, max_level,
    mode, notice_format} the matched NOTICE actually carried, or {} if no build
    NOTICE was seen. `notice_format` records which of the two formats matched,
    so a downstream reader can tell "hierarchy evidence absent because this path
    does not print it" apart from "parser failed".
    """
    for msg in notices or ():
        text = str(msg)
        m = _NOTICE_IPC.search(text)
        if m:
            d = m.groupdict()
            return {"elements": int(d["elements"]), "dim": int(d["dim"]),
                    "m": int(d["m"]), "graph_degree": int(d["graph_degree"]),
                    "max_level": int(d["max_level"]), "mode": d["mode"],
                    "notice_format": "cagra_ipc"}
        m = _NOTICE_HNSWLIB.search(text)
        if m:
            d = m.groupdict()
            return {"elements": int(d["elements"]), "dim": int(d["dim"]),
                    "m": int(d["m"]), "notice_format": "hnswlib"}
    return {}


# ── ground truth ─────────────────────────────────────────────────────────────
def assert_gt_columns(gt_shape, k=10):
    """recall@k needs k ground-truth columns, and no more.

    Asserting `>= K` (the search-time k / cuvs.k knob) would kill valid runs:
    the batch arm truncates client-side to top-10, so a K=400 sweep point is
    still scored at recall@10 against a 10-column ground truth.
    """
    cols = gt_shape[1] if len(gt_shape) > 1 else 0
    if cols < k:
        raise AssertionError(
            f"ground truth has {cols} columns, need >= {k} for recall@{k}")
    return cols


# ── Pareto selection (Phase 2 input) ─────────────────────────────────────────
def pareto_point(rows, algo, recall_floor=0.95):
    """Pick the throughput-axis operating point for `algo` from latency rows.

    The point is the highest-QPS row that still clears `recall_floor`. If no row
    clears it, fall back to the highest-recall row and say so -- silently
    reporting a sub-floor point as "the Pareto point" would misdescribe the
    throughput axis.

    rows: dicts with at least algo/recall/qps (CSV rows or equivalent). Rows for
    other algos, failed rows, and non-latency rows are ignored.
    Returns (row, fallback: bool) or (None, False) when nothing is usable.
    """
    cand = [r for r in rows
            if r.get("algo") == algo
            and str(r.get("success", True)).lower() in ("true", "1", "yes")
            and str(r.get("axis", "latency")) == "latency"
            and _is_num(r.get("recall")) and _is_num(r.get("qps"))]
    if not cand:
        return None, False
    clearing = [r for r in cand if float(r["recall"]) >= recall_floor]
    if clearing:
        return max(clearing, key=lambda r: float(r["qps"])), False
    return max(cand, key=lambda r: float(r["recall"])), True


def _is_num(v):
    try:
        float(v)
        return True
    except (TypeError, ValueError):
        return False


# ── --resume ─────────────────────────────────────────────────────────────────
def row_key(row):
    """The identity of one measured point: (algo, build_params, param).

    build_params is the canonical_json of the build config, so two rows of the
    same algo and search param but different builds are distinct points.
    """
    return (str(row.get("algo", "")),
            str(row.get("build_params", "") or "{}"),
            str(row.get("param", "")))


def completed_keys(rows):
    """row_key set of every SUCCESSFUL row -- what --resume may skip.

    Failed rows are deliberately NOT included: a resume must retry them, not
    inherit the failure.
    """
    return {row_key(r) for r in rows
            if str(r.get("success", "")).lower() in ("true", "1", "yes")}


def remaining_params(completed, algo, build_cfg, params):
    """The subset of `params` still to measure for (algo, build_cfg)."""
    key_prefix = (algo, canonical_json(build_cfg))
    return [p for p in params
            if (key_prefix[0], key_prefix[1], str(p)) not in completed]
