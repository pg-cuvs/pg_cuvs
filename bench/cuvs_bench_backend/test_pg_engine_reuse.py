"""
#78 verification: PgEngine.load_corpus's durable marker gate.

CPU-only, no pg_cuvs extension and no GPU daemon required -- PgEngine.__init__
unconditionally does `CREATE EXTENSION pg_cuvs`, which this box doesn't have,
so these tests bypass __init__ (PgEngine.__new__) and wire up a bare psycopg
connection with only the `vector` extension, exactly what load_corpus() itself
touches. Needs a local PostgreSQL with pgvector reachable via psycopg defaults
(PGDATABASE / PGHOST / etc, or edit TEST_DBNAME below); skips with a clear
reason if unreachable, per the constraint against faking evidence.

The root cause of #78 is now known: nothing emptied `t`. `count(*)` needs no
columns, so the planner answered it from the resident pg_cuvs index, whose
unqualified scan returns zero rows -- see pg_engine._dump_78_evidence. The gate
now counts with the index scan methods disabled;
test_heap_row_count_is_not_answered_by_an_index below is that regression.

The tests that need a real reload still inject the emptying directly (a manual
TRUNCATE between load_corpus calls) to exercise the gate deterministically
without waiting for a genuine short table -- mirroring the `force_reload` test
hook load_corpus() also exposes.
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
    # psycopg/pgvector are absent on a CPU-only CI runner; that is a skip, not
    # an error -- the job must still collect and run the rest of this directory.
    psycopg = pytest.importorskip("psycopg")
    pgvector_psycopg = pytest.importorskip("pgvector.psycopg")

    try:
        conn = psycopg.connect(dbname=TEST_DBNAME, autocommit=True)
    except Exception as e:  # noqa: BLE001
        pytest.skip(f"no local PostgreSQL reachable (dbname={TEST_DBNAME}): {e!r}")
    conn.execute("CREATE EXTENSION IF NOT EXISTS vector")
    pgvector_psycopg.register_vector(conn)
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


def test_load_corpus_reuses_across_configs_and_drops_the_index_on_reload(
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
        "CREATE INDEX t_hnsw_pgv ON t USING hnsw (embedding vector_l2_ops) "
        "WITH (m=16, ef_construction=64)")

    # When the next config's load_corpus call reuses the marker gate (no #78
    # recurrence yet).
    eng.load_corpus(corpus_path, n, dim, dataset="unit-test")

    # Then no COPY happened (reused) and the index is untouched.
    assert _copy_count(capsys.readouterr().out) == 0
    assert eng.conn.execute("SELECT count(*) FROM t").fetchone()[0] == n
    assert eng.conn.execute(
        "SELECT to_regclass('public.t_hnsw_pgv')").fetchone()[0] is not None

    # Given `t` really is short between configs -- injected directly here,
    # exactly what the marker gate must detect. (The #78 field reports of this
    # were not a short table at all; see the module docstring.)
    eng.conn.execute("TRUNCATE t")
    assert eng.conn.execute("SELECT count(*) FROM t").fetchone()[0] == 0

    # When load_corpus runs for the next config.
    eng.load_corpus(corpus_path, n, dim, dataset="unit-test")

    # Then it reloaded in place (TRUNCATE + COPY, not DROP TABLE ... CASCADE)
    # and DROPPED the ANN index first.
    #
    # #78 originally asserted the opposite -- that t_hnsw survives this path --
    # to avoid a "spurious rebuild". #98 measured what surviving actually costs:
    # TRUNCATE empties the index too, so COPY refills it one row at a time, and
    # at 100k rows with a resident HNSW + CAGRA index that COPY ran past 15
    # minutes on the A100 VM (every CAGRA insert is an IPC round trip to the
    # daemon) versus ~7 s with no index. Worse, the surviving index was
    # assembled by insertion while the ownership sidecar still claimed the bulk
    # CREATE INDEX time of the one before it -- a build_time describing an index
    # that no longer exists. Dropping is both cheaper and honest.
    #
    # The preservation #78 wanted is still there, and is the case that mattered:
    # the marker-gate hit above returns before any of this and leaves the index
    # untouched.
    assert _copy_count(capsys.readouterr().out) == 1
    assert eng.conn.execute("SELECT count(*) FROM t").fetchone()[0] == n
    assert eng.conn.execute(
        "SELECT to_regclass('public.t_hnsw_pgv')").fetchone()[0] is None


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
    `count(*) == n` with no dataset check at all.

    A -> B -> A is the actual #78 review F1 regression: the ORIGINAL fix kept
    one marker row PER DATASET (PRIMARY KEY on `dataset`), so after A then B
    (same n/dim) the marker table held BOTH rows even though `t` -- a single
    table -- can only ever hold one of them. A third call for dataset-a would
    then match its own still-present marker row and reuse `t`, which actually
    holds dataset-b's vectors: a silent mis-attribution of B's data as A's
    results. The fix makes the marker a singleton describing exactly what `t`
    currently holds (delete-then-insert, not upsert-by-dataset), so step 3
    below must reload, not reuse."""
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
    assert eng.conn.execute(
        "SELECT count(*) FROM public._bench_corpus"
    ).fetchone()[0] == 1, "marker must hold at most one row -- t is one table"

    # dataset-a again: the pre-fix code would find its OLD marker row still
    # present (never deleted when dataset-b loaded) and wrongly reuse `t`,
    # which actually holds dataset-b's vectors.
    eng.load_corpus(corpus_a, n, dim, dataset="dataset-a")
    assert _copy_count(capsys.readouterr().out) == 1, (
        "must reload for dataset-a: t currently holds dataset-b's vectors")
    assert eng.conn.execute(
        "SELECT count(*) FROM public._bench_corpus"
    ).fetchone()[0] == 1


def test_load_corpus_reload_on_dim_change_does_not_truncate_wrong_typmod(
    eng, tmp_path, capsys,
):
    """#78 review F4: TRUNCATE preserves the table's column type. If `t`
    exists as vector(dim_old) and the new load needs a different dim, TRUNCATE
    would leave a table COPY can't load into (typmod mismatch) -- must fall
    back to DROP + CREATE, exactly like the pre-#78 DROP...CASCADE path did
    for this case (there's no index worth preserving across a dim change: an
    index built for the old dim doesn't apply to the new one)."""
    rng = np.random.default_rng(3)
    corpus_4d = str(tmp_path / "a4.fbin")
    corpus_8d = str(tmp_path / "a8.fbin")
    _write_fbin(corpus_4d, rng.standard_normal((20, 4)))
    _write_fbin(corpus_8d, rng.standard_normal((20, 8)))

    eng.load_corpus(corpus_4d, 20, 4, dataset="dim-test")
    capsys.readouterr()

    eng.load_corpus(corpus_8d, 20, 8, dataset="dim-test")
    assert _copy_count(capsys.readouterr().out) == 1
    typmod = eng.conn.execute(
        "SELECT a.atttypmod FROM pg_attribute a "
        "WHERE a.attrelid = 'public.t'::regclass AND a.attname = 'embedding'"
    ).fetchone()[0]
    assert typmod == 8
    assert eng.conn.execute("SELECT count(*) FROM t").fetchone()[0] == 20


def test_load_corpus_verify_matches_when_chunked(eng, tmp_path, monkeypatch, capsys):
    """#78 review F3: above VERIFY_CHUNK rows, the post-COPY verify folds into
    per-chunk digests on both the SQL and Python side instead of one
    string_agg() over the whole table (which hits PostgreSQL's 1GB varlena
    limit well before 50M rows). Force a tiny chunk size so this test actually
    exercises the chunked path (and its id_offset bookkeeping) without a
    multi-million-row corpus, and confirm it still verifies cleanly -- a
    load_corpus() call that raises here means the chunked SQL and Python
    fingerprints disagree."""
    from pg_engine import PgEngine

    monkeypatch.setattr(PgEngine, "VERIFY_CHUNK", 5)
    n, dim = 23, 4  # deliberately not a multiple of the chunk size
    rng = np.random.default_rng(4)
    corpus_path = str(tmp_path / "corpus.fbin")
    _write_fbin(corpus_path, rng.standard_normal((n, dim)))

    eng.load_corpus(corpus_path, n, dim, dataset="chunk-test")

    assert eng.conn.execute("SELECT count(*) FROM t").fetchone()[0] == n
    assert "verified + stored" in capsys.readouterr().out


class _IndexAnsweredCursor:
    """A cursor where a bare count(*) is answered by an index that returns no
    rows -- exactly what a resident pg_cuvs index does to `SELECT count(*)
    FROM t` (src/pg_cuvs.c:3540-3542 returns false for an unqualified scan).

    Faked rather than built for real because reproducing it needs the pg_cuvs
    extension and a GPU daemon; the behaviour under test is the harness's, not
    the AM's."""

    TRUE_ROWS = 100000

    def __init__(self):
        self.gucs = {"enable_indexscan": "on", "enable_indexonlyscan": "on",
                     "enable_bitmapscan": "on"}
        self.log = []
        self._result = None

    def execute(self, sql, *_args):
        self.log.append(sql)
        if sql.startswith("SHOW "):
            self._result = (self.gucs[sql.split()[1]],)
        elif sql.startswith("SET "):
            name, val = sql[4:].split(" = ")
            self.gucs[name] = val
            self._result = None
        elif "count(*)" in sql:
            # An index can serve a zero-column aggregate whenever any index
            # scan method is enabled; here that index yields nothing.
            served_by_index = any(v == "on" for v in self.gucs.values())
            self._result = (0 if served_by_index else self.TRUE_ROWS,)
        else:
            self._result = None

    def fetchone(self):
        return self._result


def test_heap_row_count_is_not_answered_by_an_index():
    """The corpus gate asks a question about the heap, so its answer must not
    depend on which scan method the planner picks.

    Regression for the #98 Stage-1 rehearsal abort: with a CAGRA index resident,
    the bare count(*) returned 0 for a full 100k-row table, the gate concluded
    the corpus was gone and reloaded it, the reload dropped the ANN indexes
    mid-sweep, and the segment recorded two builds for one build_cfg."""
    eng = PgEngine.__new__(PgEngine)
    cur = _IndexAnsweredCursor()

    got = eng._heap_row_count(cur)

    assert got == _IndexAnsweredCursor.TRUE_ROWS
    # and the caller's planner settings are left exactly as they were found
    assert cur.gucs == {"enable_indexscan": "on", "enable_indexonlyscan": "on",
                        "enable_bitmapscan": "on"}


def test_heap_row_count_restores_gucs_the_caller_had_turned_off():
    """Restore, not RESET: a caller that deliberately disabled a scan method
    must still have it disabled afterwards, or this helper silently changes how
    a later measurement is planned."""
    eng = PgEngine.__new__(PgEngine)
    cur = _IndexAnsweredCursor()
    cur.gucs["enable_indexscan"] = "off"

    eng._heap_row_count(cur)

    assert cur.gucs["enable_indexscan"] == "off"
