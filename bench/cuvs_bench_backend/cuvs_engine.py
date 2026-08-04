#!/usr/bin/env python3
"""
cuvs_engine.py -- the raw cuVS CAGRA arm of #98 (algo `cuvs`). No PostgreSQL.

Same method surface as pg_engine.PgEngine (load_corpus / build / search / close)
so backend.PgBackend can dispatch to either engine without a second code path,
and so the raw rows land in the same CSV, from the same process, against the
same corpus and ground truth as the SQL rows. That co-location is the whole
point: the previously published raw number (BENCHMARK.md §1.1) came from a
different day and host than the SQL numbers, which is why it could not be
subtracted from them.

WHAT THIS MEASURES (and what it does not)
-----------------------------------------
The latency-axis `cuvs` rows are **host wall-clock per query, python dispatch
included** -- `cagra.search()` plus the pybind/DLPack round trip, with
`deviceSynchronize()` closing the timing window. They are therefore:

  * a legitimate anchor for the integration tax *within the latency axis*
    (raw wall-clock vs the same query through SQL), and
  * NOT the same quantity as BENCHMARK.md §1.1's 715 us, which is an nsys
    kernel-time sum. Updating §1.1 is PR-E's job (nsys); these rows are
    published alongside it, never as a replacement (ADR-084).

Two further honesty notes, both deliberate:

  * queries are transferred to the device ONCE, before timing, and the per-query
    window covers only the search call (mirroring
    bench/legacy/anbench/run_cuvs.py:84-89). Host->device transfer of the query
    vector is therefore EXCLUDED, so this is a lower bound on what a client
    outside the process would see.
  * the index lives in this process's GPU memory for the whole run, i.e. the raw
    arm is a second GPU tenant beside the pg-cuvs-server daemon. Every raw row
    carries an `nvidia-smi memory.used` reading so that tenancy is visible in
    the CSV rather than inferred.

PARAMETER MATCHING WITH pgcuvs_cagra
------------------------------------
The sweep knob is the same one pg_cuvs sweeps (`cuvs.k`, the GPU candidate
count) and the derived search params are the ones the extension derives from it:
src/cuvs_wrapper.cu:1122-1124 rounds itopk_size up to a multiple of 32 with a
floor of 64. Mirroring that here is what makes an axis-internal comparison
between `cuvs` and `pgcuvs_cagra` a comparison of the *integration*, not of two
differently-tuned searches.

cupy / cuvs are imported lazily inside the methods, so this module imports (and
its pure logic tests run) on a CPU-only box with neither installed.
"""
import os
import subprocess
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sidecar import INTERMEDIATE_GRAPH_DEGREE  # noqa: E402

# The raw arm's algo name, and the sidecar key its ownership record lives under.
# It is NOT a SQL relation (there is no pg_class row to cross-check), hence the
# "raw:" prefix -- a reader of the sidecar must not mistake it for one.
RAW_ALGO = "cuvs"
RAW_SIDECAR_KEY = "raw:cuvs"

# Matches bench/legacy/anbench/run_cuvs.py: L2 on unit-norm vectors == cosine
# ranking, which is what every SQL algo here uses (vector_l2_ops).
METRIC = "sqeuclidean"
BUILD_ALGO = "ivf_pq"


# ── build config ─────────────────────────────────────────────────────────────
def validate_build_cfg(build_cfg):
    """Normalize + check one raw build cell. Returns (graph_degree, intermediate).

    intermediate_graph_degree >= graph_degree is the same constraint the
    extension enforces (src/pg_cuvs.c:1265); violating it here would fail deep
    inside cuvs with a message that does not name the benchmark cell.
    """
    cfg = build_cfg or {}
    if "graph_degree" not in cfg:
        raise ValueError(f"raw build cfg needs graph_degree: {cfg!r}")
    gd = int(cfg["graph_degree"])
    igd = int(cfg.get("intermediate_graph_degree", INTERMEDIATE_GRAPH_DEGREE))
    if gd <= 0:
        raise ValueError(f"graph_degree must be positive: {gd}")
    if igd < gd:
        raise ValueError(
            f"intermediate_graph_degree ({igd}) must be >= graph_degree ({gd})")
    return gd, igd


def itopk_for(k):
    """The itopk_size pg_cuvs derives from a top-k request.

    src/cuvs_wrapper.cu:1122-1124: round up to a multiple of 32, floor 64. The
    raw arm uses the same rule so that a `cuvs` row and a `pgcuvs_cagra` row at
    the same sweep point describe the same search, not two tunings.
    """
    itopk = ((int(k) + 31) // 32) * 32
    return max(itopk, 64)


# ── GPU tenancy ──────────────────────────────────────────────────────────────
def gpu_memory_used_mb():
    """Whole-device memory.used via nvidia-smi, or None when unavailable.

    Returns None rather than NaN so "not measured" stays distinguishable from
    "measured as nothing" in the notes string.
    """
    try:
        out = subprocess.check_output(
            ["nvidia-smi", "--query-gpu=memory.used",
             "--format=csv,noheader,nounits"], text=True, timeout=15)
        return float(out.strip().splitlines()[0])
    except Exception:  # noqa: BLE001 -- absent driver / no GPU / timeout
        return None


def gpu_note(before=None, after=None):
    """The raw arm's `notes` cell: what the second GPU tenant was using.

    Pure string formatting (no subprocess) so the CSV's note format is testable
    without a GPU. Omits what it does not have rather than writing a placeholder
    that would read as a measurement.
    """
    parts = ["raw-arm=wall-clock"]
    if before is not None:
        parts.append(f"gpu_mem_used_mb_before={before:.0f}")
    if after is not None:
        parts.append(f"gpu_mem_used_mb={after:.0f}")
    if before is not None and after is not None:
        parts.append(f"gpu_mem_used_mb_delta={after - before:.0f}")
    return " ".join(parts)


# ── the engine ───────────────────────────────────────────────────────────────
class CuvsEngine:
    """A resident CAGRA index in this process's GPU memory.

    Mirrors PgEngine's surface: load_corpus() then build() then search(param)*.
    Unlike PgEngine, the "index" is an in-process object, not a database
    relation -- so reuse across the orchestrator's per-config backend instances
    cannot be recovered from a catalog, and the engine must outlive them. That
    is what get_raw_engine() below provides; do not construct this directly from
    the backend.
    """

    def __init__(self):
        self._corpus_key = None      # (path, n, dim) currently on the device
        self._d_corpus = None
        self._index = None
        self._build_key = None       # (corpus_key, gd, igd) the index describes
        self._build_time = 0.0

    # -- corpus ---------------------------------------------------------------
    def load_corpus(self, corpus_path, n, dim, dataset="default", **_):
        """Put the first n rows of corpus_path on the device (idempotent).

        `dataset` is accepted and ignored: it exists so the call site is
        identical to PgEngine.load_corpus, which uses it as a reuse-marker
        label. Here the device array itself is the marker.
        """
        import cupy as cp

        key = (os.path.abspath(corpus_path), int(n), int(dim))
        if key == self._corpus_key and self._d_corpus is not None:
            return
        # A corpus change invalidates the index built from the old one.
        self._release_index()
        self._d_corpus = None
        from pg_engine import read_fbin
        host = np.ascontiguousarray(read_fbin(corpus_path, count=n))
        if host.shape[1] != dim:
            raise ValueError(
                f"corpus dim {host.shape[1]} != requested dim {dim}")
        self._d_corpus = cp.asarray(host)
        self._corpus_key = key

    # -- build ----------------------------------------------------------------
    def has_index(self, build_cfg):
        """True when the resident index is exactly this cell's index.

        This is the raw arm's whole reuse gate. The SQL arms cross-check a
        sidecar against pg_class.reloptions because their index outlives the
        process; a raw index does not, so its only truthful witness is this
        object. A sidecar record alone must never authorise raw reuse -- after a
        restart it would attribute a stale build_time to an index that no longer
        exists.
        """
        if self._index is None:
            return False
        try:
            gd, igd = validate_build_cfg(build_cfg)
        except ValueError:
            return False
        return self._build_key == (self._corpus_key, gd, igd)

    def build(self, algo, n, sample_query=None, build_cfg=None, keep=()):
        """Build the CAGRA index. Returns (build_time_s, index_bytes, meta).

        Signature matches PgEngine.build so the backend has one call site.
        `sample_query` and `keep` are SQL-path concepts (plan warm-up, selective
        index drop) and are ignored.

        index_bytes is 0 by construction, exactly as it is for `pgcuvs_cagra`:
        there is no heap relation, and the on-device footprint is reported as
        the nvidia-smi delta in `meta` / the row notes rather than guessed from
        a formula.
        """
        import cupy as cp
        from cuvs.neighbors import cagra

        if algo != RAW_ALGO:
            raise ValueError(f"CuvsEngine builds {RAW_ALGO!r}, not {algo!r}")
        if self._d_corpus is None:
            raise RuntimeError("load_corpus() before build()")
        gd, igd = validate_build_cfg(build_cfg)

        self._release_index()
        before = gpu_memory_used_mb()
        params = cagra.IndexParams(graph_degree=gd, intermediate_graph_degree=igd,
                                   metric=METRIC, build_algo=BUILD_ALGO)
        t0 = time.perf_counter()
        index = cagra.build(params, self._d_corpus)
        cp.cuda.runtime.deviceSynchronize()
        build_time = time.perf_counter() - t0
        after = gpu_memory_used_mb()

        self._index = index
        self._build_key = (self._corpus_key, gd, igd)
        self._build_time = build_time
        meta = {"graph_degree": gd, "intermediate_graph_degree": igd,
                "build_algo": BUILD_ALGO, "metric": METRIC,
                "gpu_mem_used_mb_before": before, "gpu_mem_used_mb_after": after,
                "notes": gpu_note(before, after)}
        print(f"[pg98] raw build gd={gd} igd={igd} {build_time:.2f}s "
              f"{gpu_note(before, after)}", flush=True)
        return build_time, 0, meta

    # -- search ---------------------------------------------------------------
    def _device_queries(self, queries):
        """Upload the query set once per search() call.

        Not cached across calls on purpose: the only cheap cache key would be
        the array's identity, and a freed numpy array's id() is reusable -- a
        cache hit on a *different* query set would silently produce wrong
        neighbours. A 2000x768 upload is milliseconds against thousands of
        timed searches, and it happens outside the timing window.
        """
        import cupy as cp

        return cp.asarray(np.ascontiguousarray(queries))

    def search(self, algo, queries, kmax, param, warmup=200):
        """One query at a time under `param` (== cuvs.k). Returns (ids, latencies).

        `param` is the GPU candidate count, matching what pg_cuvs's AM path does
        with cuvs.k: search for `param` neighbours, then take the top `kmax` --
        the SQL side's LIMIT. ids are corpus row indices, i.e. already in the
        ground-truth id space (the same invariant t.id == row index gives the
        SQL arms).
        """
        import cupy as cp
        from cuvs.neighbors import cagra

        if algo != RAW_ALGO:
            raise ValueError(f"CuvsEngine searches {RAW_ALGO!r}, not {algo!r}")
        if self._index is None:
            raise RuntimeError("build() before search()")
        k_search = max(int(param), int(kmax))
        sp = cagra.SearchParams(itopk_size=itopk_for(k_search))
        d_q = self._device_queries(queries)
        nq = len(queries)

        for i in range(min(warmup, nq)):
            cagra.search(sp, self._index, d_q[i:i + 1], k_search)
        cp.cuda.runtime.deviceSynchronize()

        ids = np.full((nq, kmax), -1, dtype=np.int64)
        lat = []
        for i in range(nq):
            t1 = time.perf_counter()
            _, nbrs = cagra.search(sp, self._index, d_q[i:i + 1], k_search)
            cp.cuda.runtime.deviceSynchronize()
            lat.append(time.perf_counter() - t1)
            # copy_to_host() is a plain D2H memcpy on cuvs's returned
            # device_ndarray. cupy.asnumpy() would instead compile a cast
            # kernel at runtime, which needs the CUDA *headers*; the VM's
            # cuvs_bench env ships the runtime without them, so that path
            # failed every search while the build succeeded.
            ids[i] = nbrs.copy_to_host()[0, :kmax]
        return ids, lat

    def search_batch(self, queries, kmax, param, warmup=2, repeats=1):
        """One cagra.search() over ALL queries -- the throughput-axis mechanism.

        Returned so PR-C's Phase 2 has it available; nothing in the latency
        path calls it. Returns (ids, elapsed_seconds) where elapsed is the
        MEDIAN of `repeats` timed dispatches (a single dispatch is one sample of
        a noisy quantity; the caller decides how many it wants).

        Its recall may legitimately differ from the per-query loop's: the batch
        size selects between single-CTA and multi-CTA kernels
        (src/cuvs_wrapper.cu:1116-1130 vs :1211-1220), which is why #98 measures
        the batch-vs-single delta instead of asserting a hardcoded tolerance.
        """
        import cupy as cp
        from cuvs.neighbors import cagra

        if self._index is None:
            raise RuntimeError("build() before search_batch()")
        k_search = max(int(param), int(kmax))
        sp = cagra.SearchParams(itopk_size=itopk_for(k_search))
        d_q = self._device_queries(queries)

        for _ in range(warmup):
            cagra.search(sp, self._index, d_q, k_search)
        cp.cuda.runtime.deviceSynchronize()

        times, nbrs = [], None
        for _ in range(max(1, repeats)):
            t0 = time.perf_counter()
            _, nbrs = cagra.search(sp, self._index, d_q, k_search)
            cp.cuda.runtime.deviceSynchronize()
            times.append(time.perf_counter() - t0)
        ids = np.asarray(nbrs.copy_to_host()[:, :kmax], dtype=np.int64)
        return ids, float(np.median(times))

    # -- lifecycle ------------------------------------------------------------
    def row_note(self):
        """The `notes` cell for a raw row: current GPU tenancy."""
        return gpu_note(after=gpu_memory_used_mb())

    def _release_index(self):
        self._index = None
        self._build_key = None

    def close(self):
        self._release_index()
        self._d_corpus = None
        self._corpus_key = None
        try:
            import cupy as cp
            cp.get_default_memory_pool().free_all_blocks()
        except Exception:  # noqa: BLE001 -- nothing to free without cupy
            pass


# ── process-level singleton ──────────────────────────────────────────────────
# The orchestrator constructs a fresh backend per BenchmarkConfig, i.e. per
# sweep point. For the SQL arms that is harmless (the index is in the database).
# For the raw arm a per-config engine would drop the GPU index after every
# point and rebuild it for the next -- turning a 6-point sweep into 6 builds and
# making every `reused` flag False. The engine therefore lives at module scope,
# for the life of the process.
_ENGINE = None


def get_raw_engine():
    global _ENGINE
    if _ENGINE is None:
        _ENGINE = CuvsEngine()
    return _ENGINE


def close_raw_engine():
    """Free the resident raw index. Call at process end, not per config."""
    global _ENGINE
    if _ENGINE is not None:
        _ENGINE.close()
        _ENGINE = None
