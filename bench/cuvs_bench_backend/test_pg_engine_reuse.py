"""
#78 verification: PgEngine.load_corpus's durable marker gate.

CPU-only, no pg_cuvs extension and no GPU daemon required -- PgEngine.__init__
unconditionally does `CREATE EXTENSION pg_cuvs`, which this box doesn't have,
so these tests bypass __init__ (PgEngine.__new__) and wire up a bare psycopg
connection with only the `vector` extension, exactly what load_corpus() itself
touches. Needs a local PostgreSQL with pgvector reachable via psycopg defaults
(PGDATABASE / PGHOST / etc, or edit TEST_DBNAME below); skips with a clear
reason if unreachable, per the constraint against faking evidence.

The root cause of #78 (what empties `t` at 1M scale) is still open (see
pg_engine.py's #78 evidence dump). These tests inject the emptying directly
(a manual TRUNCATE between load_corpus calls) to exercise the gate
deterministically without waiting for a real recurrence -- mirroring the
`force_reload` test hook load_corpus() also exposes.
"""
import os
import struct
import sys

import numpy as np
import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pg_engine import PgEngine, read_fbin  # noqa: E402

TEST_DBNAME = os.environ.get("PGCUVS_TEST_DBNAME", "pg78test")


def _write_fbin(path, arr):
    arr = np.ascontiguousarray(arr, dtype=np.float32)
    with open(path, "wb") as f:
        f.write(struct.pack("<ii", arr.shape[0], arr.shape[1]))
        f.write(arr.tobytes())


@pytest.fixture
def eng(tmp_path):
    import psycopg
    import pgvector.psycopg

    try:
        conn = psycopg.connect(dbname=TEST_DBNAME, autocommit=True)
    except Exception as e:  # noqa: BLE001
        pytest.skip(f"no local PostgreSQL reachable (dbname={TEST_DBNAME}): {e!r}")
    conn.execute("CREATE EXTENSION IF NOT EXISTS vector")
    pgvector.psycopg.register_vector(conn)
    conn.execute("DROP TABLE IF EXISTS t CASCADE")
    conn.execute("DROP TABLE IF EXISTS public._bench_corpus")

    e = PgEngine.__new__(PgEngine)  # bypass __init__: no pg_cuvs extension here
    e.conn = conn
    e.index_dir = str(tmp_path)
    yield e
    conn.execute("DROP TABLE IF EXISTS t CASCADE")
    conn.execute("DROP TABLE IF EXISTS public._bench_corpus")
    conn.close()


def _copy_count(captured_out):
    """Count real COPY operations from load_corpus's own log line -- a
    synchronous, in-process signal. pg_stat_all_tables counters are updated
    asynchronously by the stats collector/shared-memory flush and are NOT
    reliably visible immediately after an autocommit statement returns, so
    they are not a safe assertion target in a fast unit test."""
    return captured_out.count("[engine] COPY ")


def test_load_corpus_reuses_across_configs_and_preserves_index_on_forced_reload(
    eng, tmp_path, capsys,
):
    n, dim = 200, 8
    rng = np.random.default_rng(0)
    corpus_path = str(tmp_path / "corpus.fbin")
    _write_fbin(corpus_path, rng.standard_normal((n, dim)))

    # Given the first load_corpus call for this dataset (simulates config 1 of
    # a sweep: PgBackend.build() -> eng.load_corpus(...)).
    eng.load_corpus(corpus_path, n, dim, dataset="unit-test")
    assert eng.conn.execute("SELECT count(*) FROM t").fetchone()[0] == n
    assert eng.conn.execute(
        "SELECT fingerprint FROM public._bench_corpus WHERE dataset = 'unit-test'"
    ).fetchone() is not None
    assert _copy_count(capsys.readouterr().out) == 1

    # And an ANN index built on it, as a real sweep would before the next config
    # runs (backend.py's _index_present gate checks for this).
    eng.conn.execute(
        "CREATE INDEX t_hnsw ON t USING hnsw (embedding vector_l2_ops) "
        "WITH (m=16, ef_construction=64)")

    # When the next config's load_corpus call reuses the marker gate (no #78
    # recurrence yet).
    eng.load_corpus(corpus_path, n, dim, dataset="unit-test")

    # Then no COPY happened (reused) and the index is untouched.
    assert _copy_count(capsys.readouterr().out) == 0
    assert eng.conn.execute("SELECT count(*) FROM t").fetchone()[0] == n
    assert eng.conn.execute("SELECT to_regclass('public.t_hnsw')").fetchone()[0] is not None

    # Given #78 recurs: something empties `t` between configs (root cause still
    # open -- injected here directly, exactly what the marker gate must detect).
    eng.conn.execute("TRUNCATE t")
    assert eng.conn.execute("SELECT count(*) FROM t").fetchone()[0] == 0

    # When load_corpus runs for the next config.
    eng.load_corpus(corpus_path, n, dim, dataset="unit-test")

    # Then it reloaded in place (TRUNCATE + COPY, not DROP TABLE ... CASCADE):
    # exactly one real COPY happened, row count is restored, AND -- the actual
    # #78 regression this fix targets -- t_hnsw is STILL PRESENT. The pre-fix
    # code used `DROP TABLE t CASCADE` here, which would have destroyed t_hnsw
    # and forced every remaining config in the sweep to rebuild it from scratch.
    assert _copy_count(capsys.readouterr().out) == 1
    assert eng.conn.execute("SELECT count(*) FROM t").fetchone()[0] == n
    assert eng.conn.execute("SELECT to_regclass('public.t_hnsw')").fetchone()[0] is not None
    valid = eng.conn.execute(
        "SELECT indisvalid FROM pg_index WHERE indexrelid = 'public.t_hnsw'::regclass"
    ).fetchone()[0]
    assert valid is True


def test_load_corpus_force_reload_bypasses_marker_gate(eng, tmp_path, capsys):
    n, dim = 50, 4
    rng = np.random.default_rng(1)
    corpus_path = str(tmp_path / "corpus.fbin")
    _write_fbin(corpus_path, rng.standard_normal((n, dim)))

    eng.load_corpus(corpus_path, n, dim, dataset="force-test")
    capsys.readouterr()

    # A plain call would hit the marker gate and skip (see the previous test).
    # force_reload=True must always reload, regardless of the marker.
    eng.load_corpus(corpus_path, n, dim, dataset="force-test", force_reload=True)

    assert _copy_count(capsys.readouterr().out) == 1
    assert eng.conn.execute("SELECT count(*) FROM t").fetchone()[0] == n


def test_load_corpus_rejects_a_second_dataset_with_a_stale_marker(eng, tmp_path, capsys):
    """A marker keyed on a DIFFERENT dataset must not let a stale/foreign `t`
    be reused -- closes the gap in the pre-#78 code, which reused on a bare
    `count(*) == n` with no dataset check at all."""
    n, dim = 30, 4
    rng = np.random.default_rng(2)
    corpus_a = str(tmp_path / "a.fbin")
    corpus_b = str(tmp_path / "b.fbin")
    _write_fbin(corpus_a, rng.standard_normal((n, dim)))
    _write_fbin(corpus_b, rng.standard_normal((n, dim)))

    eng.load_corpus(corpus_a, n, dim, dataset="dataset-a")
    capsys.readouterr()

    # dataset-b has never been loaded, even though t already has n rows of the
    # right shape (dataset-a's data) -- must reload, not silently reuse.
    eng.load_corpus(corpus_b, n, dim, dataset="dataset-b")
    assert _copy_count(capsys.readouterr().out) == 1
