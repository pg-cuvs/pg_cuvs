#!/usr/bin/env python3
"""CPU PG16 regression: with cuvs.hnsw_exact_scan=on a pg_cuvs_hnsw statement is exact or fails loudly.

Never approximate in silence. Covers shapes the planner expands after the hook sees the
Query (partition/inheritance children with row-level security), mid-statement setting
changes, tie order, and the post-planning safety check. Run against a disposable cluster
that preloads pg_cuvs; only a fresh schema and roles prefixed by it are created.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess
import threading
import sys

import hnsw_exact_cover_hold as cover_hold
from test_connectivity_pg_interrupts import Session


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--schema', required=True)
    ap.add_argument('--socket', required=True)
    ap.add_argument('--port', required=True)
    ap.add_argument('--user', required=True)
    ap.add_argument('--database', default='postgres')
    ap.add_argument('--expected-data-directory', required=True)
    ap.add_argument('--pg-bin', type=Path, required=True)
    ap.add_argument('--expected-library-sha')
    args = ap.parse_args()
    if not re.fullmatch('[a-z][a-z0-9_]{0,25}', args.schema):
        ap.error('schema must be a simple lower-case identifier, max 26 characters')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    s = args.schema
    role = s + '_u'
    repo = Path(__file__).resolve().parents[2]
    library = args.pg_bin.parent / 'lib' / 'pg_cuvs.so'
    sha = hashlib.sha256(library.read_bytes()).hexdigest()
    if args.expected_library_sha and sha != args.expected_library_sha:
        ap.error('installed pg_cuvs ELF differs from expected SHA')
    base = [str(args.pg_bin / 'psql'), '-XqAt', '-v', 'ON_ERROR_STOP=1', '-h', args.socket, '-p', args.port, '-d', args.database]
    result = {'scope': 'installed pg_cuvs/vector CPU PG16; exact mode is exact or fails loudly', 'candidate_sha256': sha, 'cases': []}
    transcript = []

    def session(user, pre=''):
        x = Session(base[:-2] + ['-U', user, '-d', args.database])
        x.ask('\\set ON_ERROR_STOP off\n' + pre)
        return x

    def ask(x, sql, timeout=30):
        sql = sql.replace('fx.', s + '.')
        lines = x.ask(sql, timeout)
        transcript.append({'sql': sql, 'output': lines})
        return lines

    def ids(x, sql):
        return [int(v) for v in ask(x, sql) if re.fullmatch('-?[0-9]+', v)]

    def nodes(plan):
        yield plan
        for child in plan.get('Plans', []):
            yield from nodes(child)

    def plan_of(x, query):
        text = '\n'.join(ask(x, 'EXPLAIN(FORMAT JSON,COSTS OFF) ' + query + ';'))
        return json.loads(text)[0]['Plan']

    barriers = iter(range(1, 1 << 30))

    def expect_error(x, sql, *fragments):
        start = len(x.errors)
        ask(x, sql)
        ask(x, 'ROLLBACK;')  # the failed statement left the transaction aborted
        # A tag of its own for each call: wait_error looks at every stderr line read so far, so a tag an earlier call
        # raised would end the wait before this statement's error reached the stderr reader thread.
        tag = f'PROTECTED_BARRIER_{next(barriers)}'
        ask(x, f"DO $$BEGIN RAISE NOTICE '{tag}'; END$$;")
        assert x.wait_error(tag, 10), x.errors
        got = '\n'.join(x.errors[start:])
        for f in fragments:
            assert f in got, (f, got)
        return got

    q20 = "SELECT id FROM {t} ORDER BY v <-> '[0,0]'::vector LIMIT 20"
    a = u = None
    cover_mode = False
    held = False
    try:
        actual_dir = subprocess.check_output(base[:-2] + ['-U', args.user, '-d', args.database, '-c', 'SHOW data_directory;'], text=True).strip()
        assert actual_dir == args.expected_data_directory, (actual_dir, args.expected_data_directory)
        a = session(args.user, 'SET client_min_messages=notice;')
        ask(a, f"CREATE SCHEMA {s}; CREATE ROLE {role} LOGIN; GRANT USAGE ON SCHEMA {s} TO {role}; GRANT USAGE ON SCHEMA public TO {role};")
        ask(a, f"SET search_path={s},public;")
        # Which access method handler the cluster has. A cluster at 0.9.3 (pg_cuvs_hnsw_handler_c1p) gives its indexes a cover descriptor: they
        # are certified, so an ordinary scan merges the graph's answers with every ledger key and is as exact as the sequential oracle; the cases
        # below that proved the protection by an approximate scan that differs from the oracle then prove it by the exact answer (the policy
        # is applied, no hidden row is evaluated) instead. A cluster at 0.9.2 (the older symbol) keeps the approximate scan: its indexes are what a
        # cluster that has not been updated has.
        cover_mode = ask(a, "SELECT prosrc FROM pg_proc WHERE proname='pg_cuvs_hnsw_handler';")[-1].endswith('_c1p')
        result['handler'] = 'cover (pg_cuvs_hnsw_handler_c1p)' if cover_mode else 'older symbol (pg_cuvs_hnsw_handler)'
        if cover_mode:
            # the exact answers below rest on every inserted row being a ledger key: no background recertification during the run
            cover_hold.hold(lambda q: ask(a, q))
            held = True
            # and no verification of the inserts, which is on by default and leaves a row it finds out of the ledger (the session that
            # inserts the rows is this superuser one)
            ask(a, 'SET cuvs.cover_verify_inserts = off;')

        def table(name, extra='', parent=None, owner=None, rows=True):
            cols = '(id int, v vector(2), g int, h int)'
            if parent:
                ask(a, f"CREATE TABLE fx.{name} PARTITION OF fx.{parent} {extra};")
            else:
                ask(a, f"CREATE TABLE fx.{name} {cols} {extra};")

        def index(name, tbl):
            ask(a, f"CREATE INDEX {name} ON fx.{tbl} USING pg_cuvs_hnsw(v public.vector_l2_ops);")

        def fill(tbl, mod_g=None):
            ask(a, f"INSERT INTO fx.{tbl} SELECT i, ('['||i||',0]')::vector, 1+i%2, 1+(i/2)%2 FROM generate_series(1,100) i;")

        def grant(tbl):
            ask(a, f"GRANT SELECT ON fx.{tbl} TO {role};")

        # P1 partitioned, RLS on one child only
        table('pp', 'PARTITION BY LIST(g)')
        table('pp1', 'FOR VALUES IN (1)', parent='pp')
        table('pp2', 'FOR VALUES IN (2)', parent='pp')
        index('pp1_i', 'pp1'); index('pp2_i', 'pp2')
        fill('pp')
        ask(a, 'ANALYZE fx.pp; ALTER TABLE fx.pp1 ENABLE ROW LEVEL SECURITY;')
        # P3 two partition levels, RLS only on a leaf
        table('gp', 'PARTITION BY LIST(g)')
        table('gp1', 'FOR VALUES IN (1) PARTITION BY LIST(h)', parent='gp')
        table('gp1a', 'FOR VALUES IN (1)', parent='gp1')
        table('gp1b', 'FOR VALUES IN (2)', parent='gp1')
        table('gp2', 'FOR VALUES IN (2)', parent='gp')
        for n, t in (('gp1a_i', 'gp1a'), ('gp1b_i', 'gp1b'), ('gp2_i', 'gp2')):
            index(n, t)
        fill('gp')
        ask(a, 'ANALYZE fx.gp; ALTER TABLE fx.gp1a ENABLE ROW LEVEL SECURITY;')
        # P4 traditional inheritance, RLS on a child only
        table('ih')
        ask(a, 'CREATE TABLE fx.ih1 () INHERITS (fx.ih); CREATE TABLE fx.ih2 () INHERITS (fx.ih);')
        for n, t in (('ih_i', 'ih'), ('ih1_i', 'ih1'), ('ih2_i', 'ih2')):
            index(n, t)
        ask(a, "INSERT INTO fx.ih1 SELECT i, ('['||i||',0]')::vector, 0, 0 FROM generate_series(1,50) i; INSERT INTO fx.ih2 SELECT i+50, ('['||(i+50)||',0]')::vector, 0, 0 FROM generate_series(1,50) i; ANALYZE fx.ih; ALTER TABLE fx.ih1 ENABLE ROW LEVEL SECURITY;")
        # P2 partitioned parent with RLS and a policy, queried by an ordinary role
        table('pr', 'PARTITION BY LIST(g)')
        table('pr1', 'FOR VALUES IN (1)', parent='pr')
        table('pr2', 'FOR VALUES IN (2)', parent='pr')
        index('pr1_i', 'pr1'); index('pr2_i', 'pr2')
        fill('pr')
        ask(a, f"ANALYZE fx.pr; ALTER TABLE fx.pr ENABLE ROW LEVEL SECURITY; CREATE POLICY pr_even ON fx.pr FOR SELECT TO {role} USING (id % 2 = 0); GRANT SELECT ON fx.pr, fx.pr1, fx.pr2 TO {role};")
        # P5/P6 owned by the ordinary role: FORCE (policy applies to the owner) and owner bypass
        for name in ('fo', 'ob'):
            table(name)
            ask(a, f"ALTER TABLE fx.{name} OWNER TO {role};")
            index(name + '_i', name)
            fill(name)
            ask(a, f"ANALYZE fx.{name}; ALTER TABLE fx.{name} ENABLE ROW LEVEL SECURITY;")
        ask(a, f"ALTER TABLE fx.fo FORCE ROW LEVEL SECURITY; CREATE POLICY fo_third ON fx.fo FOR SELECT TO {role} USING (id % 3 = 0);")
        for t in ('pp', 'pp1', 'pp2', 'gp', 'gp1', 'gp1a', 'gp1b', 'gp2', 'ih', 'ih1', 'ih2'):
            grant(t)
        ask(a, f"CREATE POLICY pp1_all ON fx.pp1 FOR SELECT TO {role} USING (true); CREATE POLICY gp1a_all ON fx.gp1a FOR SELECT TO {role} USING (true); CREATE POLICY ih1_all ON fx.ih1 FOR SELECT TO {role} USING (true);")

        u = session(role, f"SET search_path={s},public; SET hnsw.ef_search=1; SET enable_seqscan=off; SET max_parallel_workers_per_gather=0;")
        for x in (a,):
            ask(x, "SET hnsw.ef_search=1; SET enable_seqscan=off; SET max_parallel_workers_per_gather=0;")

        def shape(name, table_name, who, hnsw_indexes):
            compare(name, q20.format(t='fx.' + table_name), who, hnsw_indexes)

        def compare(name, q, who, hnsw_indexes):
            if cover_mode:
                cover_hold.assert_not_recertified(lambda x: ask(a, x), s, hnsw_indexes, name)
            ask(who, 'SET cuvs.hnsw_exact_scan=off;')
            approximate = ids(who, q + ';')
            ask(who, 'SET cuvs.hnsw_exact_scan=on;')
            exact = ids(who, q + ';')
            plan = plan_of(who, q)
            ask(who, 'SET cuvs.hnsw_exact_scan=off; SET enable_indexscan=off; SET enable_bitmapscan=off; RESET enable_seqscan;')
            oracle = ids(who, q + ';')
            ask(who, 'SET enable_indexscan=on; SET enable_bitmapscan=on; SET enable_seqscan=off;')
            assert oracle, (name, 'empty oracle')
            assert exact == oracle, (name, 'exact on differs from the sequential oracle', exact, oracle)
            if cover_mode:
                assert approximate == oracle, (name, 'an index with a cover descriptor answers a protected statement exactly', approximate, oracle)
            else:
                assert approximate != oracle, (name, 'approximate scan equals the oracle: the case would prove nothing', approximate)
            hit = [n.get('Index Name') for n in nodes(plan) if n.get('Index Name') in hnsw_indexes]
            assert not hit, (name, 'a protected statement kept an HNSW index path', hit, plan)
            result['cases'].append({'name': name, 'verdict': 'PASS', 'rows': len(exact), 'approximate_rows': len(approximate)})

        # The same child-RLS partition tree reached through a view, a subquery and a CTE
        ask(a, "CREATE VIEW fx.pp_view AS SELECT id, v FROM fx.pp; GRANT SELECT ON fx.pp_view TO " + role + ";")
        compare('child_rls_through_view', "SELECT id FROM fx.pp_view ORDER BY v <-> '[0,0]'::vector LIMIT 20", a, {'pp1_i', 'pp2_i'})
        compare('child_rls_in_subquery', "SELECT id FROM (SELECT id FROM fx.pp ORDER BY v <-> '[0,0]'::vector LIMIT 20) s", a, {'pp1_i', 'pp2_i'})
        compare('child_rls_in_cte', "WITH c AS (SELECT id FROM fx.pp ORDER BY v <-> '[0,0]'::vector LIMIT 20) SELECT id FROM c", a, {'pp1_i', 'pp2_i'})

        # Relations that enter the range table only while planning: an inlined SQL function, a view
        # of a view, a security_invoker view, an inlined function over a security-barrier view.
        ask(a, "CREATE FUNCTION fx.fo_rows() RETURNS TABLE(id int, v vector) LANGUAGE sql STABLE AS $$SELECT id, v FROM fx.fo$$; "
               "CREATE FUNCTION fx.fo_top(n int) RETURNS TABLE(id int) LANGUAGE sql STABLE AS $$SELECT id FROM fx.fo ORDER BY v <-> '[0,0]'::vector LIMIT n$$; "
               "CREATE VIEW fx.fo_v1 AS SELECT id, v FROM fx.fo; CREATE VIEW fx.fo_v2 AS SELECT id, v FROM fx.fo_v1; "
               "CREATE VIEW fx.fo_inv WITH (security_invoker=true) AS SELECT id, v FROM fx.fo; "
               "CREATE TABLE fx.bt(id int, v vector(2)); CREATE INDEX bt_i ON fx.bt USING pg_cuvs_hnsw(v public.vector_l2_ops); "
               "INSERT INTO fx.bt SELECT i, ('['||i||',0]')::vector FROM generate_series(1,50) i; ANALYZE fx.bt; "
               "CREATE VIEW fx.bv WITH(security_barrier=true) AS SELECT id,v FROM fx.bt ORDER BY v <-> '[0,0]'::vector; "
               "CREATE FUNCTION fx.bv_rows() RETURNS TABLE(id int, v vector) LANGUAGE sql STABLE AS $$SELECT id, v FROM fx.bv$$; "
               f"GRANT EXECUTE ON FUNCTION fx.fo_rows(), fx.fo_top(int), fx.bv_rows() TO {role}; GRANT SELECT ON fx.fo_v1, fx.fo_v2, fx.fo_inv, fx.bt, fx.bv TO {role};")
        compare('inlined_srf_over_rls_table', "SELECT id FROM fx.fo_rows() ORDER BY v <-> '[0,0]'::vector LIMIT 20", u, {'fo_i'})
        ask(a, "CREATE FUNCTION fx.ob_rows() RETURNS TABLE(id int, v vector) LANGUAGE sql STABLE AS $$SELECT id, v FROM fx.ob$$; GRANT EXECUTE ON FUNCTION fx.ob_rows() TO " + role + ";")
        compare('inlined_srf_over_owner_bypass_rls_table', "SELECT id FROM fx.ob_rows() ORDER BY v <-> '[0,0]'::vector LIMIT 20", u, {'ob_i'})
        compare('lateral_srf_over_rls_table', "SELECT t.id FROM (VALUES (20)) q(n) CROSS JOIN LATERAL fx.fo_top(q.n) t", u, {'fo_i'})
        compare('view_of_view_over_rls_table', "SELECT id FROM fx.fo_v2 ORDER BY v <-> '[0,0]'::vector LIMIT 20", u, {'fo_i'})
        compare('security_invoker_view_over_rls_table', "SELECT id FROM fx.fo_inv ORDER BY v <-> '[0,0]'::vector LIMIT 20", u, {'fo_i'})
        compare('inlined_srf_over_security_barrier_view', "SELECT id FROM fx.bv_rows() ORDER BY v <-> '[0,0]'::vector LIMIT 20", u, {'bt_i'})

        # Row-level security that only shows up while planning must not turn the scan of an
        # unprotected HNSW table in the same statement into a runtime error: the final plan is
        # checked once with the predicate the scan itself uses, and a statement whose plan keeps a
        # scan that cannot run exactly is planned again without any pg_cuvs_hnsw path.
        compare('unprotected_table_joined_with_inlined_rls_srf',
                "SELECT b.id FROM fx.bt b JOIN fx.fo_rows() r ON r.id = b.id ORDER BY b.v <-> '[0,0]'::vector LIMIT 20", u, {'bt_i', 'fo_i'})
        compare('unprotected_table_in_subquery_over_inlined_rls_srf',
                "SELECT id FROM fx.bt WHERE id IN (SELECT id FROM fx.fo_rows()) ORDER BY v <-> '[0,0]'::vector LIMIT 20", u, {'bt_i', 'fo_i'})
        compare('unprotected_table_union_all_inlined_rls_srf',
                "SELECT id FROM (SELECT id, v FROM fx.bt UNION ALL SELECT id, v FROM fx.fo_rows()) s ORDER BY v <-> '[0,0]'::vector LIMIT 20", u, {'bt_i', 'fo_i'})
        # The policy of a partitioned parent is not copied to the partitions' security quals, so a
        # parent read through an inlined function is judged only by the final plan.
        ask(a, "CREATE FUNCTION fx.pr_rows() RETURNS TABLE(id int, v vector) LANGUAGE sql STABLE AS $$SELECT id, v FROM fx.pr$$; "
               f"GRANT EXECUTE ON FUNCTION fx.pr_rows() TO {role};")
        compare('inlined_srf_over_rls_partitioned_parent', "SELECT id FROM fx.pr_rows() ORDER BY v <-> '[0,0]'::vector LIMIT 20", u, {'pr1_i', 'pr2_i'})
        compare('inlined_srf_over_rls_partitioned_parent_superuser', "SELECT id FROM fx.pr_rows() ORDER BY v <-> '[0,0]'::vector LIMIT 20", a, {'pr1_i', 'pr2_i'})

        # A cached generic plan made while no child had row-level security, then the child gets
        # it (or an RLS child is attached): the plan is invalidated and the next EXECUTE is exact.
        for label in ('rls_enabled_after_prepare', 'rls_child_attached_after_prepare'):
            t = 'pa_enable' if label == 'rls_enabled_after_prepare' else 'pa_attach'
            ask(a, f"CREATE TABLE fx.{t}(id int, v vector(2), g int) PARTITION BY LIST(g); CREATE TABLE fx.{t}1 PARTITION OF fx.{t} FOR VALUES IN (1); "
                   f"CREATE INDEX {t}1_i ON fx.{t}1 USING pg_cuvs_hnsw(v public.vector_l2_ops); "
                   f"INSERT INTO fx.{t} SELECT i, ('['||i||',0]')::vector, 1 FROM generate_series(1,50) i;")
            if label == 'rls_enabled_after_prepare':
                ask(a, f"CREATE TABLE fx.{t}2 PARTITION OF fx.{t} FOR VALUES IN (2); CREATE INDEX {t}2_i ON fx.{t}2 USING pg_cuvs_hnsw(v public.vector_l2_ops); "
                       f"INSERT INTO fx.{t} SELECT i, ('['||i||',0]')::vector, 2 FROM generate_series(51,100) i;")
            ask(a, f"ANALYZE fx.{t}; SET plan_cache_mode=force_generic_plan; SET cuvs.hnsw_exact_scan=on; PREPARE q_{t} AS SELECT id FROM fx.{t} ORDER BY v <-> '[0,0]'::vector LIMIT 20;")
            first = ids(a, f"EXECUTE q_{t};")
            if label == 'rls_enabled_after_prepare':
                ask(a, f"ALTER TABLE fx.{t}1 ENABLE ROW LEVEL SECURITY;")
            else:
                ask(a, f"CREATE TABLE fx.{t}2 (LIKE fx.{t}); ALTER TABLE fx.{t}2 ENABLE ROW LEVEL SECURITY; "
                       f"CREATE INDEX {t}2_i ON fx.{t}2 USING pg_cuvs_hnsw(v public.vector_l2_ops); "
                       f"INSERT INTO fx.{t}2 SELECT i, ('['||i||',0]')::vector, 2 FROM generate_series(51,100) i; "
                       f"ALTER TABLE fx.{t} ATTACH PARTITION fx.{t}2 FOR VALUES IN (2);")
            after = ids(a, f"EXECUTE q_{t};")
            ask(a, f"SET cuvs.hnsw_exact_scan=off; SET enable_indexscan=off; SET enable_bitmapscan=off; RESET enable_seqscan; SET plan_cache_mode=auto;")
            oracle = ids(a, f"SELECT id FROM fx.{t} ORDER BY v <-> '[0,0]'::vector LIMIT 20;")
            ask(a, "SET enable_indexscan=on; SET enable_bitmapscan=on; SET enable_seqscan=off;")
            assert after == oracle and len(oracle) == 20, (label, after, oracle, first)
            result['cases'].append({'name': 'prepared_plan_' + label, 'verdict': 'PASS'})

        shape('child_only_rls_partition', 'pp', a, {'pp1_i', 'pp2_i'})
        shape('two_level_partition_leaf_rls', 'gp', a, {'gp1a_i', 'gp1b_i', 'gp2_i'})
        shape('inheritance_child_rls', 'ih', a, {'ih_i', 'ih1_i', 'ih2_i'})
        shape('parent_only_rls_partition_policy', 'pr', u, {'pr1_i', 'pr2_i'})
        shape('force_rls_table_owner_policy', 'fo', u, {'fo_i'})
        shape('owner_bypass_rls_enabled', 'ob', u, {'ob_i'})
        # the same child-RLS shape queried by the ordinary role through the parent
        shape('child_only_rls_partition_ordinary_role', 'pp', u, {'pp1_i', 'pp2_i'})

        # Planning has side effects (an IMMUTABLE function with constant arguments is evaluated
        # while planning). A protected statement must be planned once, not twice.
        ask(a, "CREATE FUNCTION fx.plan_note() RETURNS boolean LANGUAGE plpgsql IMMUTABLE AS $$BEGIN RAISE NOTICE 'PLAN_ONCE_MARK'; RETURN true; END$$; SET client_min_messages=notice; SET cuvs.hnsw_exact_scan=on;")
        offset = len(a.errors)
        ask(a, "SELECT count(*) FROM (SELECT id FROM fx.pp WHERE fx.plan_note() ORDER BY v <-> '[0,0]'::vector LIMIT 5) x;")
        ask(a, "DO $$BEGIN RAISE NOTICE 'PLAN_ONCE_END'; END$$;")
        assert a.wait_error('PLAN_ONCE_END', 10), a.errors
        marks = [x for x in a.errors[offset:] if 'PLAN_ONCE_MARK' in x]
        assert len(marks) == 1, marks
        result['cases'].append({'name': 'protected_statement_is_planned_once', 'verdict': 'PASS'})
        ask(a, 'SET cuvs.hnsw_exact_scan=off;')

        # Mid-statement switches. The statement is planned with the setting off.
        ask(a, f"CREATE FUNCTION fx.set_exact(v vector, mode text) RETURNS vector LANGUAGE plpgsql VOLATILE STRICT AS $$BEGIN PERFORM set_config('cuvs.hnsw_exact_scan',mode,true);RETURN v;END$$; GRANT EXECUTE ON FUNCTION fx.set_exact(vector,text) TO {role};")
        lateral_on = "SELECT q.i,t.id FROM(VALUES(1,'[0,0]'::vector),(2,fx.set_exact('[0,0]'::vector,'on')))q(i,v) CROSS JOIN LATERAL(SELECT id FROM {t} ORDER BY v <-> q.v LIMIT 20)t ORDER BY q.i"
        # RLS table: the second outer row would start an exact scan on a protected table
        ask(u, 'SET cuvs.hnsw_exact_scan=off; SET enable_material=off; BEGIN;')
        plan = plan_of(u, lateral_on.format(t='fx.ob'))
        assert any(n.get('Index Name') == 'ob_i' for n in nodes(plan)), plan
        got = expect_error(u, lateral_on.format(t='fx.ob') + ';',
                           'cuvs.hnsw_exact_scan is on, but this scan cannot run exactly',
                           'row-level security is enabled on the table')
        result['cases'].append({'name': 'mid_statement_on_rls_table_fails_loudly', 'verdict': 'PASS', 'error': got.splitlines()[0]})
        # Security barrier subquery: the setting turned on while the barrier scan reruns
        barrier = ("SELECT q.i,t.id FROM(VALUES(1,'[0,0]'::vector),(2,fx.set_exact('[0,0]'::vector,'on')))q(i,v) "
                   "CROSS JOIN LATERAL(SELECT id FROM fx.bv WHERE id >= q.i LIMIT 1)t ORDER BY q.i")
        ask(u, 'SET cuvs.hnsw_exact_scan=off; SET enable_material=off; BEGIN;')
        plan = plan_of(u, barrier)
        assert any(n.get('Index Name') == 'bt_i' for n in nodes(plan)), plan
        got = expect_error(u, barrier + ';', 'cuvs.hnsw_exact_scan is on, but this scan cannot run exactly',
                           'a security barrier applies to the statement')
        result['cases'].append({'name': 'mid_statement_on_security_barrier_fails_loudly', 'verdict': 'PASS', 'error': got.splitlines()[0]})

        # on -> off between loops of one statement: the second loop is approximate again
        ask(a, f"CREATE TABLE fx.pl(id int, v vector(2)); CREATE INDEX pl_i ON fx.pl USING pg_cuvs_hnsw(v public.vector_l2_ops); INSERT INTO fx.pl SELECT i, ('['||i||',0]')::vector FROM generate_series(1,100) i; ANALYZE fx.pl; GRANT SELECT ON fx.pl TO {role};")
        lateral_off = ("SELECT jsonb_object_agg(i, ids) FROM (SELECT q.i, jsonb_agg(t.id ORDER BY t.n) ids FROM(VALUES(1,'[0,0]'::vector),(2,fx.set_exact('[0,0]'::vector,'off')))q(i,v) "
                       "CROSS JOIN LATERAL(SELECT id, row_number() OVER () n FROM(SELECT id FROM fx.pl ORDER BY v <-> q.v LIMIT 20)z)t GROUP BY q.i)w")
        ask(u, 'SET cuvs.hnsw_exact_scan=on; SET enable_material=off; BEGIN;')
        plan = plan_of(u, lateral_off)
        assert any(n.get('Index Name') == 'pl_i' for n in nodes(plan)), plan
        both = json.loads(ask(u, lateral_off + ';')[-1])
        ask(u, 'ROLLBACK;')
        ask(u, "SET cuvs.hnsw_exact_scan=on;")
        exact_ids = ids(u, "SELECT id FROM fx.pl ORDER BY v <-> '[0,0]'::vector LIMIT 20;")
        ask(u, "SET cuvs.hnsw_exact_scan=off;")
        approx_ids = ids(u, "SELECT id FROM fx.pl ORDER BY v <-> '[0,0]'::vector LIMIT 20;")
        if cover_mode:
            # pl_i was built on an empty table and filled by inserts, so its ledger holds every row: the ordinary scan (merge) is as exact as the
            # explicit mode, and the switch cannot be told apart here. The run on a cluster at 0.9.2 (older symbol) proves the switch; this one
            # proves that the answers stay exact on both sides of it.
            assert both['1'] == exact_ids and both['2'] == approx_ids and exact_ids == approx_ids, (both, exact_ids, approx_ids)
        else:
            assert exact_ids != approx_ids, 'exact and approximate agree: switch test would prove nothing'
            assert both['1'] == exact_ids, (both, exact_ids)
            assert both['2'] == approx_ids, ('second loop kept exact mode after the setting went off', both, approx_ids)
        result['cases'].append({'name': 'mid_statement_on_to_off_returns_to_delegate', 'verdict': 'PASS'})

        # Tie order: equal distances come back in heap order, with three groups interleaved
        # in the heap. Once with the answers in memory and once spilled through tuplesort.
        ask(a, f"CREATE TABLE fx.dup(id int, v vector(2)); CREATE INDEX dup_i ON fx.dup USING pg_cuvs_hnsw(v public.vector_l2_ops); INSERT INTO fx.dup SELECT i, ('['||(1+i%3)||',0]')::vector FROM generate_series(1,6000) i; ANALYZE fx.dup;")
        for label, memory in (('in_memory', '64MB'), ('spilled', '64kB')):
            ask(a, f"SET cuvs.hnsw_exact_scan=on; SET work_mem='{memory}'; SET enable_seqscan=off; SET enable_indexscan=on;")
            tied = ids(a, "SELECT id FROM fx.dup ORDER BY v <-> '[0,0]'::vector;")
            ask(a, "SET cuvs.hnsw_exact_scan=off; SET enable_indexscan=off; RESET enable_seqscan; SET work_mem='64MB';")
            in_heap_order = ids(a, "SELECT id FROM fx.dup ORDER BY v <-> '[0,0]'::vector, ctid;")
            ask(a, 'SET enable_indexscan=on; SET enable_seqscan=off;')
            assert len(tied) == 6000 and tied == in_heap_order and tied != sorted(tied), (label, tied[:20], in_heap_order[:20])
            result['cases'].append({'name': 'tie_order_is_heap_order_' + label, 'verdict': 'PASS'})
        ask(a, "SET work_mem='64MB';")

        # An exact scan reads the HOT root of every visible tuple from a per-page map while another
        # session keeps making HOT updates and pruning the same pages. Half of the rows are already
        # heap-only versions when the reader takes its snapshot, so a wrong root loses the row.
        ask(a, "CREATE FUNCTION fx.hot_distance(vector,vector) RETURNS float8 LANGUAGE plpgsql IMMUTABLE STRICT AS $$BEGIN PERFORM pg_sleep(0.004);RETURN $1 <-> $2;END$$; "
               "CREATE OPERATOR fx.<~> (LEFTARG=vector,RIGHTARG=vector,PROCEDURE=fx.hot_distance); "
               "CREATE OPERATOR CLASS fx.hot_ops FOR TYPE vector USING pg_cuvs_hnsw AS OPERATOR 1 fx.<~> (vector,vector) FOR ORDER BY float_ops, FUNCTION 1 vector_l2_squared_distance(vector,vector); "
               "CREATE TABLE fx.hotc(id int, v vector(2), note text) WITH (fillfactor=50); CREATE INDEX hotc_i ON fx.hotc USING pg_cuvs_hnsw(v fx.hot_ops); "
               "INSERT INTO fx.hotc SELECT i, ('['||i||',0]')::vector, repeat('a',60) FROM generate_series(1,600) i; UPDATE fx.hotc SET note=repeat('b',60) WHERE id % 2 = 0; UPDATE fx.hotc SET note=repeat('c',60) WHERE id % 3 = 0; ANALYZE fx.hotc;")
        reader = session(args.user, 'SET client_min_messages=warning;')
        writer = session(args.user, 'SET client_min_messages=warning;')
        try:
            ask(reader, f"SET search_path={s},public; SET cuvs.hnsw_exact_scan=on; SET enable_seqscan=off; SET max_parallel_workers_per_gather=0; BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM fx.hotc;")
            hot_query = "SELECT id FROM fx.hotc ORDER BY v OPERATOR(fx.<~>) '[0,0]'::vector"
            plan = plan_of(reader, hot_query)
            assert any(n.get('Index Name') == 'hotc_i' for n in nodes(plan)), plan
            seen = []

            def read_all():
                seen.extend(int(v) for v in ask(reader, hot_query + ';', 120) if re.fullmatch('-?[0-9]+', v))

            thread = threading.Thread(target=read_all)
            thread.start()
            for _ in range(6):
                ask(writer, f"SET search_path={s},public; UPDATE fx.hotc SET note = repeat('d',60) WHERE id % 2 = 0; UPDATE fx.hotc SET note = repeat('e',60) WHERE id % 3 = 0; VACUUM fx.hotc;", 60)
            thread.join(120)
            assert not thread.is_alive(), 'reader did not finish'
            ask(reader, 'SET cuvs.hnsw_exact_scan=off; SET enable_indexscan=off; SET enable_bitmapscan=off; RESET enable_seqscan;')
            oracle = ids(reader, "SELECT id FROM fx.hotc ORDER BY v <-> '[0,0]'::vector;")
            ask(reader, 'ROLLBACK;')
        finally:
            reader.close(); writer.close()
        assert len(oracle) == 600 and seen == oracle, (len(seen), len(oracle), seen[:12], oracle[:12])
        result['cases'].append({'name': 'exact_scan_under_concurrent_hot_updates_and_vacuum', 'verdict': 'PASS', 'rows': len(seen)})

        # The caller reads more than one pass keeps (work_mem / 16 bytes of answers) and the ordering
        # function is STABLE: its value changes between two FETCHes of one cursor. The second pass
        # cannot continue where the first stopped, and must say so instead of repeating or losing rows.
        ask(a, "CREATE FUNCTION fx.stable_distance(vector,vector) RETURNS float8 LANGUAGE sql STABLE STRICT AS $$SELECT ($1 <-> $2) * CASE WHEN current_setting('stab.flip', true) = 'on' THEN -1 ELSE 1 END$$; "
               "CREATE OPERATOR fx.<~~> (LEFTARG=vector,RIGHTARG=vector,PROCEDURE=fx.stable_distance); "
               "CREATE OPERATOR CLASS fx.stable_ops FOR TYPE vector USING pg_cuvs_hnsw AS OPERATOR 1 fx.<~~> (vector,vector) FOR ORDER BY float_ops, FUNCTION 1 vector_l2_squared_distance(vector,vector); "
               "CREATE TABLE fx.stab(id int, v vector(2)); CREATE INDEX stab_i ON fx.stab USING pg_cuvs_hnsw(v fx.stable_ops); "
               "INSERT INTO fx.stab SELECT i, ('['||i||',0]')::vector FROM generate_series(1,6000) i; ANALYZE fx.stab;")
        cursor = "DECLARE c NO SCROLL CURSOR FOR SELECT id FROM fx.stab ORDER BY v OPERATOR(fx.<~~>) '[0,0]'::vector;"
        ask(a, "SET cuvs.hnsw_exact_scan=on; SET enable_seqscan=off; SET work_mem='64kB'; SET stab.flip='off'; BEGIN; " + cursor)
        assert any(n.get('Index Name') == 'stab_i' for n in nodes(plan_of(a, "SELECT id FROM fx.stab ORDER BY v OPERATOR(fx.<~~>) '[0,0]'::vector")))
        control = ids(a, "FETCH 4000 FROM c;") + ids(a, "FETCH 3000 FROM c;")
        ask(a, "ROLLBACK;")
        assert control == list(range(1, 6001)), ('stable function without a change', control[:5], len(control))
        ask(a, "BEGIN; " + cursor)
        head = ids(a, "FETCH 4000 FROM c;")
        assert head == list(range(1, 4001)), head[:5]
        ask(a, "SET LOCAL stab.flip='on';")
        got = expect_error(a, "FETCH 3000 FROM c;", 'the ordering function returned different results during one exact scan')
        ask(a, "SET work_mem='64MB'; SET cuvs.hnsw_exact_scan=off; SET enable_seqscan=on;")
        result['cases'].append({'name': 'stable_ordering_function_change_between_passes_fails_loudly', 'verdict': 'PASS', 'error': got.splitlines()[0]})

        # NaN, NULL and numbers in one table, read in memory and through the spilled second pass
        ask(a, "CREATE TABLE fx.nan(id int, v vector(2)); CREATE INDEX nan_i ON fx.nan USING pg_cuvs_hnsw(v public.vector_cosine_ops); "
               "INSERT INTO fx.nan SELECT i, CASE i % 3 WHEN 0 THEN '[0,0]'::vector WHEN 1 THEN NULL ELSE ('['||(1+i%7)||','||(1+i%5)||']')::vector END FROM generate_series(1,6000) i; ANALYZE fx.nan;")
        nan_query = "SELECT id FROM fx.nan ORDER BY v <=> '[1,2]'::vector"
        for label, memory in (('in_memory', '64MB'), ('spilled', '64kB')):
            ask(a, f"SET cuvs.hnsw_exact_scan=on; SET work_mem='{memory}'; SET enable_seqscan=off; SET enable_indexscan=on;")
            got_ids = ids(a, nan_query + ';')
            ask(a, "SET cuvs.hnsw_exact_scan=off; SET enable_indexscan=off; RESET enable_seqscan; SET work_mem='64MB';")
            want = ids(a, nan_query.replace('ORDER BY v <=> ', 'ORDER BY v <=> ') + ', ctid;')
            assert len(got_ids) == 6000 and got_ids == want, (label, got_ids[:10], want[:10])
            result['cases'].append({'name': 'nan_null_and_numbers_in_sql_order_' + label, 'verdict': 'PASS'})
        ask(a, 'SET enable_indexscan=on; SET enable_seqscan=off; SET work_mem=\'64MB\';')

        # The post-planning check: a later planner hook re-adds the removed path
        include = subprocess.check_output(['pg_config', '--includedir-server'], text=True).strip()
        flags = shlex.split(subprocess.check_output(['pg_config', '--cflags'], text=True))
        probe = out / 'hnsw_plan_probe.so'
        compiled = subprocess.run([os.environ.get('CC', 'cc'), *flags, '-shared', '-fPIC', '-Werror', '-Wno-unused-parameter',
                                   '-Wno-declaration-after-statement', '-I' + include, str(repo / 'test/unit/hnsw_plan_probe.c'), '-o', str(probe)],
                                  text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / 'probe-compile.log').write_text(compiled.stdout)
        assert compiled.returncode == 0, compiled.stdout
        ask(a, f"LOAD '{probe}'; SET cuvs.hnsw_exact_scan=on; SET probe.readd=off;")
        control = ids(a, "SELECT id FROM fx.ob ORDER BY v <-> '[0,0]'::vector LIMIT 20;")
        assert control, 'probe control query returned nothing'
        ask(a, 'SET probe.readd=on;')
        got = expect_error(a, "SELECT id FROM fx.ob ORDER BY v <-> '[0,0]'::vector LIMIT 20;",
                           'an exact query retained an unsupported HNSW index path')
        ask(a, 'SET probe.readd=off;')
        result['cases'].append({'name': 'planned_hnsw_path_on_protected_table_is_rejected', 'verdict': 'PASS', 'error': got.splitlines()[0]})

        # The part of the post-planning check that looks at the whole range table. With the
        # path-level removal switched off for the first planning (probe.readd_first_planning),
        # a function that reads an inheritance parent with a row-level-security child, or a
        # partitioned table with one, is only recognised by the relations in the final range
        # table: no statement marker is set and the scan's own table has no row-level security.
        ask(a, "CREATE FUNCTION fx.ih_rows() RETURNS TABLE(id int, v vector) LANGUAGE sql STABLE AS $$SELECT id, v FROM fx.ih$$; "
               "CREATE FUNCTION fx.pp_rows() RETURNS TABLE(id int, v vector) LANGUAGE sql STABLE AS $$SELECT id, v FROM fx.pp$$; "
               "SET probe.readd_first_planning=on;")
        compare('range_table_check_alone_inheritance_child_rls', "SELECT id FROM fx.ih_rows() ORDER BY v <-> '[0,0]'::vector LIMIT 20", a, {'ih_i', 'ih1_i', 'ih2_i'})
        compare('range_table_check_alone_partition_child_rls', "SELECT id FROM fx.pp_rows() ORDER BY v <-> '[0,0]'::vector LIMIT 20", a, {'pp1_i', 'pp2_i'})
        ask(a, 'SET probe.readd_first_planning=off;')
        result['verdict'] = 'PASS'
        return 0
    except Exception as e:
        result.update(verdict='FAIL', error=type(e).__name__ + ': ' + str(e))
        return 1
    finally:
        try:
            if a:
                ask(a, f"RESET ALL; DROP SCHEMA IF EXISTS {s} CASCADE; DROP OWNED BY {role}; DROP ROLE IF EXISTS {role};")
                if held:
                    cover_hold.release(lambda q: ask(a, q))
        except Exception as e:
            result['cleanup_error'] = repr(e)
        (out / 'transcript.json').write_text(json.dumps(transcript, indent=1))
        (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps({k: result.get(k) for k in ('verdict', 'error', 'cleanup_error')} | {'cases': len(result['cases'])}))


if __name__ == '__main__':
    sys.exit(main())
