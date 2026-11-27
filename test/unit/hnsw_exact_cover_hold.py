"""The ledger state that the exact runner's comparisons on indexes with a cover descriptor depend on.

Those cases build a pg_cuvs_hnsw index on an empty table and insert the rows afterwards, so every row is a ledger key and an ordinary scan
(the merge of the graph's answers and the ledger) ranks all of them: it equals the sequential oracle. A background recertification keeps only
the keys of rows the graph cannot reach, after which the same scan is the graph's answer (hnsw.ef_search rows, as with pgvector alone), and
whether a case sees one or the other depends on when the sweeper runs. The runner's cluster is a disposable one (see the Makefile target), so
the background recertification is turned off for the run and turned back on at its end, and each comparison first checks that its indexes
were not recertified since they were built (a built index has generation 1; each recertification adds one): a case can then no longer pass or
fail by the sweeper's timing. `ask` is a function of one SQL string that returns the output lines of one session that may run ALTER SYSTEM.
"""
import time


def hold(ask):
    ask('ALTER SYSTEM SET cuvs.cover_auto_recertify = off;')
    ask('SELECT pg_reload_conf();')
    # the reload signal wakes the sweeper, which reads the file before its next sweep; a second is ample, and the indexes of the run do not
    # exist yet
    time.sleep(1)


def release(ask):
    ask('ALTER SYSTEM RESET cuvs.cover_auto_recertify;')
    ask('SELECT pg_reload_conf();')


def assert_not_recertified(ask, schema, names, label):
    """The pg_cuvs_hnsw indexes `names` of `schema` still have the ledger of their build (generation 1)."""
    listed = ','.join("'" + n.replace("'", "''") + "'" for n in sorted(names))
    lines = ask("SELECT string_agg(c.relname || '=' || coalesce(pg_cuvs_hnsw_cover_status(c.oid)->>'generation', 'none'), ',' ORDER BY c.relname) "
                "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace JOIN pg_am am ON am.oid = c.relam "
                f"WHERE am.amname = 'pg_cuvs_hnsw' AND n.nspname = '{schema}' AND c.relname IN ({listed});")
    got = lines[-1] if lines else ''
    pairs = {k: v for k, _, v in (p.partition('=') for p in got.split(',') if '=' in p)}
    assert set(pairs) == set(names), (label, 'the indexes of the case are not all pg_cuvs_hnsw indexes of the schema', sorted(names), got)
    moved = {n: g for n, g in pairs.items() if g != '1'}
    assert not moved, (label, 'an index was recertified (or has no descriptor) before the comparison: its ledger no longer holds every row', moved)
