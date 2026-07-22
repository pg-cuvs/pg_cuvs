#!/usr/bin/env python3
"""
backend.py -- the cuvs-bench pluggable backend for PostgreSQL (pg_cuvs + pgvector).

cuvs-bench (RAPIDS 26.06+) ships no PostgreSQL/pgvector backend. This registers
one so pg_cuvs (GPU, via the sidecar) and pgvector (CPU) run *inside NVIDIA's own
tool*, on the same data + ground truth + methodology (recall buckets, Pareto).

Two classes + a register() hook, mirroring cpp_gbench:

  PgConfigLoader(ConfigLoader)   maps a dataset dir + a comma list of algos to
                                 cuvs-bench DatasetConfig + one BenchmarkConfig
                                 per (algo, sweep-param) so the orchestrator's
                                 "one result per IndexConfig" model yields a real
                                 Pareto curve.
  PgBackend(BenchmarkBackend)    build()/search() delegate to pg_engine.PgEngine
                                 and return cuvs-bench BuildResult / SearchResult
                                 carrying REAL neighbor ids -- so the orchestrator
                                 recomputes recall itself (compute_recall) against
                                 the ground truth. id == corpus row index keeps the
                                 returned ids in the gt id space.

Design notes that matter:

* One backend, several algos. cuvs-bench models a "backend" (how it runs) with
  one or more algos. Algos: pgvector_hnsw / pgvector_ivfflat / pgcuvs_cagra /
  pgcuvs_hnsw_import (3I). See pg_engine.ALGOS.

* Build reuse. The orchestrator creates a fresh backend per BenchmarkConfig and
  calls build() then search() for each. With one config per (algo, param) that
  would rebuild the index once per param. build() therefore reuses an existing
  index (recorded in an index-dir sidecar) unless force=True or the algo changed,
  so each algo is built once and its params are swept over the same index.

* Ground truth format. cuvs-bench's Dataset lazy-loads gt from a big-ann binary
  (.ibin int32); it does not read .npy. PgConfigLoader converts gt_<N>.npy ->
  gt_<N>_q<nq>.ibin, sliced to the same nq the backend searches, so the
  orchestrator's compute_recall gets matching (nq, k) shapes.

* Measurement boundary. search_time_ms is the FULL psql round-trip per query
  (client -> PG backend -> [pg_cuvs: shm IPC + GPU kernel] -> heap fetch), i.e.
  what a PostgreSQL application experiences -- NOT the in-process C++ kernel time
  cuVS's native cuvs_* rows report. Apples-to-apples across these Postgres algos
  and vs pgvector; label as end-to-end SQL latency next to cuVS C++ rows.

* Daemon. pg_cuvs algos need the pg-cuvs-server daemon up. pgvector is pure CPU
  and is unaffected by a resident GPU index, so this backend does NOT toggle the
  daemon (kept up throughout) -- matching the standalone 1M run for consistency.
"""
import json
import os
import struct
import sys
import tempfile

import numpy as np

# pg_engine lives beside this file; the driver puts this dir on sys.path.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pg_engine import (  # noqa: E402
    DEFAULT_SWEEPS,
    INDEX_DIR_DEFAULT,
    PgEngine,
    fbin_meta,
    percentiles_ms,
    read_fbin,
)

from cuvs_bench.backends.base import (  # noqa: E402
    BenchmarkBackend,
    BuildResult,
    SearchResult,
)
from cuvs_bench.orchestrator.config_loaders import (  # noqa: E402
    BenchmarkConfig,
    ConfigLoader,
    DatasetConfig,
    IndexConfig,
)

# algo -> the SQL index relation pg_engine builds for it.
_INDEX_OF = {
    "pgvector_hnsw": "t_hnsw",
    "pgvector_ivfflat": "t_ivf",
    "pgcuvs_cagra": "t_cagra",
    "pgcuvs_hnsw_import": "t_hnsw",
}
_SIDECAR = "pg_current_index.json"   # records which algo currently owns the index


def _write_ibin(path, arr):
    """Write a 2-D int array as big-ann .ibin: uint32 n, uint32 dim, int32 data."""
    a = np.ascontiguousarray(arr, dtype=np.int32)
    with open(path, "wb") as f:
        f.write(struct.pack("<II", a.shape[0], a.shape[1]))
        f.write(a.tobytes())


def _read_ibin(path):
    """Read a big-ann .ibin (uint32 n, uint32 dim, int32 data) as an (n, dim) array."""
    with open(path, "rb") as f:
        n, d = struct.unpack("<II", f.read(8))
        return np.frombuffer(f.read(n * d * 4), dtype=np.int32).reshape(n, d)


def _recall_at_k(neighbors, gt, k):
    """recall@k: mean over queries of |returned_topk ∩ gt_topk| / k. neighbors and
    gt are in the same id space (corpus row index). gt may carry >k columns; only
    its first k are the true top-k."""
    nq = min(len(neighbors), len(gt))
    if nq == 0 or k == 0:
        return 0.0
    hits = 0
    for i in range(nq):
        hits += len(set(neighbors[i, :k].tolist()) & set(gt[i, :k].tolist()))
    return hits / float(nq * k)


# ── config loader ────────────────────────────────────────────────────────────
class PgConfigLoader(ConfigLoader):
    """Produce a DatasetConfig + one BenchmarkConfig per (algo, sweep-param).

    load() is called by the orchestrator as
        load(count=k, batch_size=..., **loader_kwargs)
    where loader_kwargs are whatever run_benchmark() was given beyond the standard
    flags. We accept:
        dataset       dataset name (label)
        dataset_path  dir holding corpus.fbin, queries_10k.fbin, gt_<n>.npy
        algorithms    comma list from pg_engine.ALGOS
        n             corpus rows to benchmark (table is built to this N)
        dim           vector dim
        dbname        Postgres db (default "postgres")
        index_dir     must match the daemon's --index-dir
        max_queries   number of queries to search (gt is sliced to match)
    """

    # ConfigLoader declares these abstract; PgConfigLoader overrides load()
    # wholesale (it doesn't use cuvs-bench's YAML config tree), so the base
    # template method that would call _build_benchmark_configs is never reached.
    @property
    def backend_type(self):
        return "pg"

    def _build_benchmark_configs(self, *args, **kwargs):
        raise NotImplementedError("PgConfigLoader builds configs directly in load()")

    def load(self, count=10, batch_size=10000, *, dataset, dataset_path,
             algorithms, n, dim=None, dbname="postgres",
             index_dir=INDEX_DIR_DEFAULT, max_queries=2000, subset_size=None,
             **_):
        corpus = os.path.join(dataset_path, "corpus.fbin")
        queries = os.path.join(dataset_path, "queries_10k.fbin")
        if dim is None:
            _, dim = fbin_meta(corpus)

        nq = self._prepare_gt(dataset_path, n, queries, max_queries)
        gt_ibin = os.path.join(dataset_path, f"gt_{n}_q{nq}.ibin")

        ds_cfg = DatasetConfig(
            name=dataset, base_file=corpus, query_file=queries,
            groundtruth_neighbors_file=gt_ibin, distance="euclidean",
            dims=dim, subset_size=subset_size,
        )

        algos = [a.strip() for a in algorithms.split(",") if a.strip()]
        configs = []
        for algo in algos:
            for param in DEFAULT_SWEEPS[algo]:
                # hnsw/3I ef_search must be >= k, else the CPU index can't
                # return k rows (mirrors pg_engine.main()).
                if algo in ("pgvector_hnsw", "pgcuvs_hnsw_import") and param < count:
                    continue
                idx = IndexConfig(
                    name=f"{algo}.{param}", algo=algo, build_param={},
                    search_params=[{"param": param}],
                    file=os.path.join(index_dir, f"{algo}.idx"),
                )
                backend_config = {
                    "name": f"{algo}.{param}", "algo": algo, "param": param,
                    "dbname": dbname, "index_dir": index_dir,
                    "n": n, "dim": dim, "nq": nq,
                }
                configs.append(BenchmarkConfig(indexes=[idx],
                                               backend_config=backend_config))
        return ds_cfg, configs

    @staticmethod
    def _prepare_gt(dataset_path, n, queries_path, max_queries):
        """Slice gt_<n>.npy to nq rows and write gt_<n>_q<nq>.ibin. Returns nq."""
        gt_npy = os.path.join(dataset_path, f"gt_{n}.npy")
        gt = np.load(gt_npy)
        nq_total = min(len(gt), _fbin_rows(queries_path))
        nq = min(max_queries, nq_total)
        _write_ibin(os.path.join(dataset_path, f"gt_{n}_q{nq}.ibin"), gt[:nq])
        return nq


def _fbin_rows(path):
    with open(path, "rb") as f:
        n, _ = struct.unpack("<ii", f.read(8))
    return n


# ── backend ──────────────────────────────────────────────────────────────────
class PgBackend(BenchmarkBackend):
    """Adapts pg_engine.PgEngine to the cuvs-bench BenchmarkBackend interface.

    One instance per BenchmarkConfig (the orchestrator constructs it fresh), so
    all state that must survive across params (which algo owns the built index,
    its build time) lives in an index-dir sidecar, not on the instance.
    """

    def __init__(self, config):
        super().__init__(config)
        self.index_dir = config["index_dir"]
        self.dbname = config.get("dbname", "postgres")
        self._engine = None

    @property
    def algo(self):
        """Algorithm name -- BenchmarkBackend requires this as a property."""
        return self.config["algo"]

    def _eng(self):
        if self._engine is None:
            self._engine = PgEngine(dbname=self.dbname, index_dir=self.index_dir)
        return self._engine

    def _sidecar_path(self):
        # NOT index_dir -- that's the daemon's dir (not writable by us). Use a
        # writable state dir so build-reuse state survives across the per-config
        # backend instances the orchestrator creates within one run.
        state_dir = self.config.get("state_dir") or tempfile.gettempdir()
        return os.path.join(state_dir, _SIDECAR)

    def _read_sidecar(self):
        try:
            with open(self._sidecar_path()) as f:
                return json.load(f)
        except (OSError, ValueError):
            return {}

    def _index_present(self, eng, algo):
        rel = _INDEX_OF[algo]
        row = eng.conn.execute(
            "SELECT to_regclass(%s)", (f"public.{rel}",)).fetchone()
        return row[0] is not None

    # -- build -----------------------------------------------------------------
    def build(self, dataset, indexes, force=False, dry_run=False):
        idx = indexes[0]
        algo, n, dim = self.algo, self.config["n"], self.config["dim"]
        if dry_run:
            return BuildResult(index_path=idx.file, build_time_seconds=0.0,
                               index_size_bytes=0, algorithm=algo,
                               build_params={}, metadata={"dry_run": True},
                               success=True)
        eng = self._eng()
        eng.load_corpus(dataset.base_file, n, dim)

        side = self._read_sidecar()
        if (not force and side.get("algo") == algo
                and self._index_present(eng, algo)):
            return BuildResult(
                index_path=idx.file,
                build_time_seconds=float(side.get("build_time_seconds", 0.0)),
                index_size_bytes=int(side.get("index_size_bytes", 0)),
                algorithm=algo, build_params={},
                metadata={"reused": True}, success=True)

        # 3I needs a sample query to trigger the .hnsw sidecar serialization.
        sample_query = None
        if algo == "pgcuvs_hnsw_import":
            sample_query = np.ascontiguousarray(read_fbin(dataset.query_file, count=1)[0])
        try:
            bt, ibytes = eng.build(algo, n, sample_query=sample_query)
        except Exception as e:  # noqa: BLE001 -- surface as a failed BuildResult
            return BuildResult(index_path=idx.file, build_time_seconds=0.0,
                               index_size_bytes=0, algorithm=algo,
                               build_params={}, metadata={}, success=False,
                               error_message=repr(e))
        with open(self._sidecar_path(), "w") as f:
            json.dump({"algo": algo, "build_time_seconds": bt,
                       "index_size_bytes": ibytes}, f)
        return BuildResult(index_path=idx.file, build_time_seconds=bt,
                           index_size_bytes=ibytes, algorithm=algo,
                           build_params={}, metadata={"reused": False},
                           success=True)

    # -- search ----------------------------------------------------------------
    def search(self, dataset, indexes, k, batch_size=10000, mode="latency",
               force=False, search_threads=None, dry_run=False):
        idx = indexes[0]
        algo = self.algo
        param = idx.search_params[0]["param"]
        nq = self.config["nq"]
        if dry_run:
            return SearchResult(neighbors=np.empty((0, 0), np.int64),
                                distances=np.empty((0, 0), np.float32),
                                search_time_ms=0.0, queries_per_second=0.0,
                                recall=0.0, algorithm=algo,
                                search_params=[{"param": param}],
                                metadata={"dry_run": True}, success=True)
        queries = np.ascontiguousarray(read_fbin(dataset.query_file))[:nq]
        try:
            ids, lat = self._eng().search(algo, queries, k, param)
        except Exception as e:  # noqa: BLE001
            return SearchResult(neighbors=np.empty((0, 0), np.int64),
                                distances=np.empty((0, 0), np.float32),
                                search_time_ms=0.0, queries_per_second=0.0,
                                recall=0.0, algorithm=algo,
                                search_params=[{"param": param}],
                                metadata={}, success=False, error_message=repr(e))
        total_s = float(sum(lat))
        p50, p95, p99 = percentiles_ms(lat)
        # neighbors carry real ids (corpus row index == gt id space). This
        # cuvs-bench version has the orchestrator report result.recall as-is
        # (Python-native backends compute their own recall from the ground
        # truth), so compute recall@k here rather than leaving a 0.0 placeholder.
        nbr = np.asarray(ids[:, :k], dtype=np.int64)
        recall = _recall_at_k(nbr, _read_ibin(dataset.groundtruth_neighbors_file), k)
        return SearchResult(
            neighbors=nbr,
            distances=np.full((nq, k), -1.0, dtype=np.float32),
            search_time_ms=total_s * 1000.0,
            queries_per_second=(nq / total_s if total_s > 0 else 0.0),
            recall=recall, algorithm=algo,
            search_params=[{"param": param, "k": k}],
            latency_percentiles={"p50_ms": p50, "p95_ms": p95, "p99_ms": p99},
            metadata={"n_queries": nq, "algo": algo, "param": param},
            success=True)

    def cleanup(self):
        if self._engine is not None:
            self._engine.close()
            self._engine = None


# ── registration ─────────────────────────────────────────────────────────────
def register(name="pg"):
    """Register PgBackend + PgConfigLoader under `name` in cuvs-bench's registries.

    Idempotent: unregisters an existing same-name entry first so re-imports in one
    process don't raise. Call this before BenchmarkOrchestrator(backend_type=name).
    """
    from cuvs_bench.backends import registry as _r

    for reg_fn, unreg_fn, cls in (
        ("register_backend", "unregister_backend", PgBackend),
        ("register_config_loader", "unregister_config_loader", PgConfigLoader),
    ):
        try:
            getattr(_r, unreg_fn)(name)
        except Exception:  # noqa: BLE001 -- not registered yet is fine
            pass
        getattr(_r, reg_fn)(name, cls)
    return name


if __name__ == "__main__":
    # import/registration smoke test (no DB, no GPU)
    register("pg")
    from cuvs_bench.backends.registry import get_backend_class, get_config_loader
    print("[ok] PgBackend        =", get_backend_class("pg").__name__)
    print("[ok] PgConfigLoader   =", get_config_loader("pg").__name__)
    print("[ok] instantiate loader:", PgConfigLoader().__class__.__name__)
