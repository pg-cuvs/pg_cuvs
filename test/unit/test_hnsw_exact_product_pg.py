#!/usr/bin/env python3
"""CPU product regression for an explicitly supplied disposable PG16 cluster.

Install pg_cuvs and vector first and preload pg_cuvs. All indexes are built on
empty test tables, then populated through normal INSERT; no GPU daemon is used.
Only a fresh schema is created. Data directory and optional ELF identity are
checked before SQL writes; existing fixture data and server lifecycle are kept.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess
import threading

import hnsw_exact_cover_hold as cover_hold
from test_connectivity_pg_interrupts import Session

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--schema', required=True)
    parser.add_argument('--socket', required=True)
    parser.add_argument('--port', required=True)
    parser.add_argument('--user', required=True)
    parser.add_argument('--database', default='postgres')
    parser.add_argument('--expected-data-directory', required=True)
    parser.add_argument('--pg-bin', type=Path, required=True)
    parser.add_argument('--expected-library-sha')
    parser.add_argument('--library-dir', type=Path)
    args = parser.parse_args()
    if not re.fullmatch('[a-z][a-z0-9_]{0,35}', args.schema):
        parser.error('schema must be a simple lower-case identifier, max 36 characters')
    p = args.output.resolve()
    p.mkdir(parents=True, exist_ok=False)
    schema = args.schema
    repo = Path(__file__).resolve().parents[2]
    library_dir = args.library_dir or args.pg_bin.parent / 'lib'
    library = library_dir / 'pg_cuvs.so'
    library_sha = hashlib.sha256(library.read_bytes()).hexdigest()
    if args.expected_library_sha and library_sha != args.expected_library_sha:
        parser.error('installed pg_cuvs ELF differs from expected SHA')
    conn = [str(args.pg_bin / 'psql'), '-XqAt', '-v', 'ON_ERROR_STOP=1', '-h', args.socket, '-p', args.port, '-U', args.user, '-d', args.database]
    transcript = []
    result = {'scope': 'installed pg_cuvs/vector CPU PG16; explicit D', 'candidate_sha256': library_sha, 'pgvector_library_sha256': hashlib.sha256((library_dir / 'vector.so').read_bytes()).hexdigest(), 'cases': []}
    a = b = None
    held = False

    def ask(c, sql):
        sql = sql.replace('review_ops.', schema + '.').replace('CREATE SCHEMA review_ops;', 'CREATE SCHEMA ' + schema + ';')
        out = c.ask(sql)
        transcript.append({'sql': sql, 'output': out})
        return out

    def js(c, sql):
        return json.loads(ask(c, sql)[-1])

    def exact_case(c, name, query, required_rows=None):
        plan = json.loads('\n'.join(ask(c, 'EXPLAIN(FORMAT JSON,COSTS OFF) ' + query + ';')))[0]['Plan']
        if name == 'partial_expression':
            assert 'Seq Scan' in json.dumps(plan) and 'Sort' in json.dumps(plan), plan
            assert 'Index Scan' not in json.dumps(plan), plan
        else:
            assert 'Index Scan' in json.dumps(plan), plan
        actual = js(c, "SELECT coalesce(jsonb_agg(x),'[]'::jsonb) FROM(" + query + ')x;')
        ask(c, 'SET enable_indexscan=off;SET enable_indexonlyscan=off;SET enable_bitmapscan=off;SET enable_seqscan=on;')
        oracle_plan = json.loads('\n'.join(ask(c, 'EXPLAIN(FORMAT JSON,COSTS OFF) ' + query + ';')))[0]['Plan']
        assert 'Seq Scan' in json.dumps(oracle_plan), oracle_plan
        assert 'Index Scan' not in json.dumps(oracle_plan) and 'Index Only Scan' not in json.dumps(oracle_plan), oracle_plan
        expected = js(c, "SELECT coalesce(jsonb_agg(x),'[]'::jsonb) FROM(" + query + ')x;')
        ask(c, 'SET enable_indexscan=on;SET enable_indexonlyscan=on;SET enable_seqscan=off;')
        assert actual == expected, (name, actual, expected)
        if required_rows is not None:
            assert actual == required_rows, (name, actual, required_rows)
        result['cases'].append({'name': name, 'verdict': 'PASS', 'plan': plan, 'oracle_plan': oracle_plan, 'rows': actual})
        return actual

    def nodes(plan):
        yield plan
        for child in plan.get('Plans', []):
            yield from nodes(child)
    try:
        a = Session(conn)
        b = Session(conn)
        assert ask(a, 'SHOW data_directory;') == [args.expected_data_directory], 'unexpected server data directory'
        versions = js(a, "SELECT jsonb_object_agg(extname,extversion) FROM pg_extension WHERE extname IN ('vector','pg_cuvs');")
        assert versions and 'pg_cuvs' in versions and ('vector' in versions), 'install vector and pg_cuvs first'
        # Which access method handler the cluster has: a database created or updated to 0.9.3 names pg_cuvs_hnsw_handler_c1p (its indexes get a
        # cover descriptor), one at 0.9.2 the older symbol (the indexes of a cluster that was not updated). They answer differently on a table
        # with a partial index whose predicate is a user function: the older one serves the index, the new one never chooses it.
        cover_mode = ask(a, "SELECT prosrc FROM pg_proc WHERE proname='pg_cuvs_hnsw_handler';")[-1].endswith('_c1p')
        result['handler'] = 'cover (pg_cuvs_hnsw_handler_c1p)' if cover_mode else 'older symbol (pg_cuvs_hnsw_handler)'
        if cover_mode:
            # the cover cases below rest on every inserted row being a ledger key: no background recertification during the run
            cover_hold.hold(lambda q: ask(a, q))
            held = True
        result['extension_versions'] = versions
        result['source_sha256'] = {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in [repo / 'src/cuvs_hnsw_scan.c', repo / 'src/cuvs_hnsw_exact.c', Path(__file__)]}
        ask(a, 'CREATE SCHEMA review_ops; CREATE OPERATOR CLASS review_ops.vector_ip_ops FOR TYPE vector USING pg_cuvs_hnsw AS OPERATOR 1 <#> (vector,vector) FOR ORDER BY float_ops,FUNCTION 1 vector_negative_inner_product(vector,vector); CREATE OPERATOR CLASS review_ops.vector_cosine_ops FOR TYPE vector USING pg_cuvs_hnsw AS OPERATOR 1 <=> (vector,vector) FOR ORDER BY float_ops,FUNCTION 1 vector_negative_inner_product(vector,vector),FUNCTION 2 vector_norm(vector);')
        for name, op, opc in [('l2', '<->', 'public.vector_l2_ops'), ('ip', '<#>', 'review_ops.vector_ip_ops'), ('cosine', '<=>', 'review_ops.vector_cosine_ops')]:
            ask(a, f"CREATE TABLE review_ops.{name}(id int,v vector(2)); CREATE INDEX {name}_i ON review_ops.{name} USING pg_cuvs_hnsw(v {opc}); INSERT INTO review_ops.{name} VALUES(1,'[1,0]'),(2,'[0,2]'),(3,'[-1,0]'),(4,'[1,1]'),(5,'[-2,1]');")
            ask(a, 'SET enable_seqscan=off;SET cuvs.hnsw_exact_scan=on;')
            exact_case(a, name + '_empty_ambuild_insert', f"SELECT id,v {op} '[0.7,0.3]'::vector AS d FROM review_ops.{name} ORDER BY v {op} '[0.7,0.3]'::vector")
        # pgvector's HNSW omits NULL and zero-norm cosine vectors.  The
        # explicit heap path still owes the SQL ordering, including NaN/NULL.
        ask(a, "CREATE TABLE review_ops.cosine_edge(id int,v vector(2)); CREATE INDEX cosine_edge_i ON review_ops.cosine_edge USING pg_cuvs_hnsw(v review_ops.vector_cosine_ops); INSERT INTO review_ops.cosine_edge VALUES (1,'[1,0]'),(2,'[0,0]'),(3,NULL);")
        exact_case(a, 'cosine_zero_and_null', "SELECT id,v <=> '[1,0]'::vector AS d FROM review_ops.cosine_edge ORDER BY v <=> '[1,0]'::vector LIMIT 3",
                   [{'id': 1, 'd': 0}, {'id': 2, 'd': 'NaN'}, {'id': 3, 'd': None}])
        ask(a, "CREATE TABLE review_ops.expr(id int,v vector(2)); CREATE INDEX expr_i ON review_ops.expr USING pg_cuvs_hnsw(((v+'[1,0]'::vector)::vector(2)) public.vector_l2_ops) WHERE id<=3; INSERT INTO review_ops.expr VALUES(1,'[0,0]'),(2,'[1,0]'),(3,'[2,0]'),(4,'[-1,0]');")
        exact_case(a, 'partial_expression', "SELECT id,(v+'[1,0]'::vector)::vector(2) <-> '[0,0]'::vector AS d FROM review_ops.expr WHERE id<=3 ORDER BY (v+'[1,0]'::vector)::vector(2) <-> '[0,0]'::vector")
        ask(a, "CREATE FUNCTION review_ops.parallel_pred(i int) RETURNS boolean LANGUAGE plpgsql IMMUTABLE STRICT PARALLEL UNSAFE AS $$BEGIN RAISE NOTICE 'PARALLEL_PRED stage=% id=%',current_setting('rls_probe.tag',true),i;RETURN false;END$$; CREATE TABLE review_ops.parallel_items(id int,v vector(2)); CREATE INDEX parallel_i ON review_ops.parallel_items USING pg_cuvs_hnsw(v public.vector_l2_ops) WHERE id>0 OR review_ops.parallel_pred(id); INSERT INTO review_ops.parallel_items VALUES(-1,'[5,0]'),(1,'[1,0]'),(2,'[2,0]'),(3,'[3,0]'); SET debug_parallel_query=on;SET parallel_setup_cost=0;SET parallel_tuple_cost=0;SET max_parallel_workers_per_gather=2;SET rls_probe.tag='parallel_query';SET cuvs.hnsw_exact_scan=on;")
        offset = len(a.errors)
        query = "SELECT id FROM review_ops.parallel_items WHERE id>0 ORDER BY v <-> '[0,0]'::vector"
        plan = json.loads('\n'.join(ask(a, 'EXPLAIN(ANALYZE,FORMAT JSON,COSTS OFF) ' + query + ';')))[0]['Plan']
        ask(a, "DO $$BEGIN RAISE NOTICE 'PARALLEL_END';END$$;")
        assert a.wait_error('PARALLEL_END', 10), a.errors
        notices = a.errors[offset:]
        assert any(n['Node Type'] == 'Seq Scan' for n in nodes(plan)), plan
        assert not any(n.get('Index Name') == 'parallel_i' for n in nodes(plan)), plan
        assert not any('PARALLEL_PRED stage=parallel_query' in x for x in notices), notices
        result['cases'].append({'name': 'parallel_unsafe_index_predicate', 'verdict': 'PASS', 'plan': plan, 'notices': notices})
        if cover_mode:
            # A partial index of built-in functions only is chosen by planning and answered by the exact module (merge of the graph answers and
            # the ledger), also inside a parallel plan (debug_parallel_query runs the whole plan in a worker, whose executor hooks give the scan its
            # statement): the answer equals the table scan's.
            ask(a, "CREATE TABLE review_ops.parallel_builtin(id int,v vector(2)); CREATE INDEX parallel_bi ON review_ops.parallel_builtin USING pg_cuvs_hnsw(v public.vector_l2_ops) WHERE id>0; INSERT INTO review_ops.parallel_builtin VALUES(-1,'[5,0]'),(1,'[1,0]'),(2,'[2,0]'),(3,'[3,0]'); SET debug_parallel_query=on;SET parallel_setup_cost=0;SET parallel_tuple_cost=0;SET max_parallel_workers_per_gather=2;SET cuvs.hnsw_exact_scan=off;")
            bq = "SELECT id FROM review_ops.parallel_builtin WHERE id>0 ORDER BY v <-> '[0,0]'::vector"
            bplan = json.loads('\n'.join(ask(a, 'EXPLAIN(FORMAT JSON,COSTS OFF) ' + bq + ';')))[0]['Plan']
            assert any(n.get('Index Name') == 'parallel_bi' for n in nodes(bplan)), bplan
            cover_hold.assert_not_recertified(lambda q: ask(a, q), schema, {'parallel_bi'}, 'builtin_partial_index_answered_exactly_in_a_parallel_plan')
            got = js(a, "SELECT jsonb_agg(x ORDER BY ord) FROM(SELECT id, row_number() OVER () ord FROM(" + bq + ")y)x;")
            ask(a, 'SET debug_parallel_query=off;SET enable_indexscan=off;SET enable_bitmapscan=off;SET enable_seqscan=on;')
            want = js(a, "SELECT jsonb_agg(x ORDER BY ord) FROM(SELECT id, row_number() OVER () ord FROM(" + bq + ")y)x;")
            ask(a, 'SET enable_indexscan=on;SET enable_seqscan=off;')
            assert [r['id'] for r in got] == [1, 2, 3] and got == want, (got, want)
            result['cases'].append({'name': 'builtin_partial_index_answered_exactly_in_a_parallel_plan', 'verdict': 'PASS', 'plan': bplan, 'rows': got})
        # Exact mode planned off, switched on by a volatile function while the statement runs. The predicate of this partial index calls a
        # function that is not built in, and the exact module does not run such a predicate (the planner chose the index because the
        # statement's conditions imply the predicate, which does not make the function part of the statement): the scan stops and the
        # function does not run. (Before the decisions of 2026-11-19 this scan stopped with "cannot run exactly ... expressions or a predicate";
        # for a predicate of built-in functions it is answered exactly, see test_cover_scan_pg.py.)
        ask(a, "CREATE FUNCTION review_ops.toggle_vec(v vector) RETURNS vector LANGUAGE plpgsql VOLATILE STRICT AS $$BEGIN PERFORM set_config('cuvs.hnsw_exact_scan','on',true);RETURN v;END$$; SET debug_parallel_query=off; SET rls_probe.tag='mid_toggle_nonrls'; SET cuvs.hnsw_exact_scan=off; \\set ON_ERROR_STOP off\nBEGIN;")
        lateral = "SELECT q.i,t.id FROM(VALUES(1,'[0,0]'::vector),(2,review_ops.toggle_vec('[1,0]'::vector)))q(i,v) CROSS JOIN LATERAL(SELECT id FROM review_ops.parallel_items WHERE id>0 ORDER BY v <-> q.v LIMIT 1)t ORDER BY q.i"
        plan = json.loads('\n'.join(ask(a, 'EXPLAIN(FORMAT JSON,COSTS OFF) ' + lateral + ';')))[0]['Plan']
        offset = len(a.errors)
        if cover_mode:
            # the index has a descriptor and a predicate that is not built in: planning does not choose it, PostgreSQL's table scan answers
            # (here: the nearest row of each outer row, id 1 twice), and the predicate function of the owner does not run
            assert not any(n.get('Index Name') == 'parallel_i' for n in nodes(plan)), plan
            rows = js(a, 'SELECT jsonb_agg(x ORDER BY i) FROM(' + lateral + ')x;')
            assert [r['id'] for r in rows] == [1, 1], rows
            ask(a, "DO $$BEGIN RAISE NOTICE 'COVER_TOGGLE_END';END$$;")
            assert a.wait_error('COVER_TOGGLE_END', 10), a.errors
            notices = a.errors[offset:]
            assert not any('PARALLEL_PRED stage=mid_toggle_nonrls' in x for x in notices), notices
            ask(a, 'ROLLBACK;')
            assert ask(a, 'SHOW cuvs.hnsw_exact_scan;') == ['off']
            result['cases'].append({'name': 'partial_user_predicate_not_chosen_and_answered_by_table_scan', 'verdict': 'PASS', 'plan': plan, 'notices': notices})
        else:
            assert any(n.get('Index Name') == 'parallel_i' for n in nodes(plan)), plan
            ask(a, 'SELECT jsonb_agg(x) FROM(' + lateral + ')x;')
            assert a.wait_error('cannot answer this statement', 10), a.errors
            notices = a.errors[offset:]
            assert not any('PARALLEL_PRED stage=mid_toggle_nonrls' in x for x in notices), notices
            ask(a, 'ROLLBACK;')
            assert ask(a, 'SHOW cuvs.hnsw_exact_scan;') == ['off']
            result['cases'].append({'name': 'partial_midquery_toggle_user_predicate_stops', 'verdict': 'PASS', 'plan': plan, 'notices': notices})
        # With the setting off the partial index keeps serving ANN across rescans and must not
        # evaluate its predicate (restored from the first version of this case).
        ask(a, "SET cuvs.hnsw_exact_scan=off; SET rls_probe.tag='partial_rescan_exact_off';")
        loop_offset = len(a.errors)
        plain = "SELECT q.i,t.id FROM(VALUES(1,'[0,0]'::vector),(2,'[1,0]'::vector))q(i,v) CROSS JOIN LATERAL(SELECT id FROM review_ops.parallel_items WHERE id>0 ORDER BY v <-> q.v LIMIT 1)t ORDER BY q.i"
        loop_plan = json.loads('\n'.join(ask(a, 'EXPLAIN(ANALYZE,FORMAT JSON,COSTS OFF) ' + plain + ';')))[0]['Plan']
        ask(a, "DO $$BEGIN RAISE NOTICE 'PARTIAL_RESCAN_END';END$$;")
        assert a.wait_error('PARTIAL_RESCAN_END', 10), a.errors
        loops = [n for n in nodes(loop_plan) if n.get('Index Name') == 'parallel_i' and n.get('Actual Loops') == 2]
        if cover_mode:
            assert not loops and not any(n.get('Index Name') == 'parallel_i' for n in nodes(loop_plan)), loop_plan   # not chosen: see above
        else:
            assert len(loops) == 1, loop_plan
        assert not any('PARALLEL_PRED stage=partial_rescan_exact_off' in x for x in a.errors[loop_offset:]), a.errors[loop_offset:]
        result['cases'].append({'name': 'partial_rescan_exact_off_evaluates_no_predicate', 'verdict': 'PASS', 'plan': loop_plan})
        ask(a, '\\set ON_ERROR_STOP on')
        ask(a, 'COMMIT;SET cuvs.hnsw_exact_scan=on;SET debug_parallel_query=off;')
        ask(a, "CREATE TABLE review_ops.spill(id int,v vector(2)); CREATE INDEX spill_i ON review_ops.spill USING pg_cuvs_hnsw(v public.vector_l2_ops); INSERT INTO review_ops.spill SELECT i,NULL FROM generate_series(1,6000)i; ANALYZE review_ops.spill; SET work_mem='64kB';SET log_temp_files=0;")
        plan = json.loads('\n'.join(ask(a, "EXPLAIN(FORMAT JSON,COSTS OFF) SELECT id FROM review_ops.spill ORDER BY v <-> '[0,0]'::vector LIMIT 1;")))[0]['Plan']
        assert plan['Node Type'] == 'Limit' and (not any((n['Node Type'] in ('Sort', 'Incremental Sort') for n in nodes(plan))))
        assert any((n.get('Index Name') == 'spill_i' for n in nodes(plan)))
        count = int(ask(a, "SELECT count(*) FROM(SELECT id FROM review_ops.spill ORDER BY v <-> '[0,0]'::vector LIMIT 6000)x;")[-1])
        assert count == 6000
        pid = int(ask(a, 'SELECT pg_backend_pid();')[-1])
        ask(a, "SET temp_file_limit='0'; DO $$BEGIN PERFORM count(*) FROM (SELECT id FROM review_ops.spill ORDER BY v <-> '[0,0]'::vector LIMIT 6000) x; RAISE EXCEPTION 'expected missing error'; EXCEPTION WHEN configuration_limit_exceeded THEN RAISE NOTICE 'D_ERROR_53400 pid=%',pg_backend_pid(); END$$; SET temp_file_limit='-1';")
        assert a.wait_error('D_ERROR_53400 pid=' + str(pid), 10), a.errors
        assert int(ask(a, 'SELECT pg_backend_pid();')[-1]) == pid
        # A small LIMIT is answered by the bounded first pass and needs no temporary file at all.
        ask(a, "SET temp_file_limit='0'; DO $$BEGIN PERFORM id FROM review_ops.spill ORDER BY v <-> '[0,0]'::vector LIMIT 10; RAISE NOTICE 'D_NO_SPILL_OK pid=%',pg_backend_pid(); END$$; SET temp_file_limit='-1';")
        assert a.wait_error('D_NO_SPILL_OK pid=' + str(pid), 10), a.errors
        assert int(ask(a, "SELECT count(*) FROM(SELECT id FROM review_ops.spill ORDER BY v <-> '[0,0]'::vector LIMIT 6000)x;")[-1]) == 6000
        result['cases'].append({'name': 'actual_product_spill_error_recovery', 'verdict': 'PASS', 'pid': pid, 'rows': count, 'plan': plan, 'notices': [x for x in a.errors if 'D_ERROR_' in x]})
        ask(a, "CREATE TABLE review_ops.rescan(id int,v vector(2)); CREATE INDEX rescan_i ON review_ops.rescan USING pg_cuvs_hnsw(v public.vector_l2_ops); INSERT INTO review_ops.rescan VALUES(0,'[0,0]'),(1,'[1,0]'),(2,'[2,0]'),(3,'[100,0]'),(4,'[101,0]'),(5,NULL); ANALYZE review_ops.rescan;")
        lateral = "SELECT q.v::text,t.id FROM(VALUES('[100,0]'::vector),('[102,0]'::vector))q(v) CROSS JOIN LATERAL(SELECT id FROM review_ops.rescan t ORDER BY t.v <-> q.v LIMIT 6)t"
        off = None
        # With the setting off, pgvector's graph does not hold the row with a NULL vector, so an index of the older handler returns 5 rows; an
        # index with a cover descriptor keeps every inserted row in its ledger and merges it in, so it returns the same 6 rows as the exact mode.
        for mode, want in [('off', 6 if cover_mode else 5), ('on', 6)]:
            if cover_mode and mode == 'off':
                cover_hold.assert_not_recertified(lambda q: ask(a, q), schema, {'rescan_i'}, 'exact_discriminating_parameterized_rescan')
            ask(a, 'SET cuvs.hnsw_exact_scan=' + mode + ';')
            actual = js(a, 'SELECT jsonb_agg(x) FROM(' + lateral + ')x;')
            assert all((sum((row['v'] == value for row in actual)) == want for value in ['[100,0]', '[102,0]'])), actual
            assert sum((row['id'] == 5 for row in actual)) == (2 if (mode == 'on' or cover_mode) else 0), actual
            if mode == 'on':
                plan = json.loads('\n'.join(ask(a, 'EXPLAIN(ANALYZE,FORMAT JSON,COSTS OFF) ' + lateral + ';')))[0]['Plan']
                inner = [n for n in nodes(plan) if n.get('Index Name') == 'rescan_i' and n.get('Actual Loops') == 2]
                assert len(inner) == 1 and inner[0]['Actual Rows'] == 6, plan
            else:
                off = actual
        result['cases'].append({'name': 'exact_discriminating_parameterized_rescan', 'verdict': 'PASS', 'off': off, 'on': actual, 'inner_index_scan': inner[0]})
        ask(a, 'SET plan_cache_mode=force_custom_plan; PREPARE c(vector) AS SELECT jsonb_agg(x) FROM(SELECT id FROM review_ops.rescan ORDER BY v <-> $1 LIMIT 6)x;')
        actual = js(a, "EXECUTE c('[100,0]');")
        assert [x['id'] for x in actual] == [3, 4, 2, 1, 0, 5]
        stats = js(a, "SELECT jsonb_build_object('custom',custom_plans,'generic',generic_plans) FROM pg_prepared_statements WHERE name='c';")
        assert stats == {'custom': 1, 'generic': 0}, stats
        result['cases'].append({'name': 'actual_custom_plan', 'verdict': 'PASS', 'rows': actual, 'counts': stats})
        ask(a, 'SET plan_cache_mode=force_generic_plan; PREPARE g(vector) AS SELECT jsonb_agg(x) FROM(SELECT id FROM review_ops.rescan ORDER BY v <-> $1 LIMIT 6)x; SET cuvs.hnsw_exact_scan=off;')
        if cover_mode:
            cover_hold.assert_not_recertified(lambda q: ask(a, q), schema, {'rescan_i'}, 'actual_generic_plan')
        off = js(a, "EXECUTE g('[100,0]');")
        if cover_mode:
            assert len(off) == 6 and off[-1]['id'] == 5
        else:
            assert len(off) == 5 and all((row['id'] != 5 for row in off))
        ask(a, 'SET cuvs.hnsw_exact_scan=on;')
        on = js(a, "EXECUTE g('[100,0]');")
        assert len(on) == 6 and on[-1]['id'] == 5
        stats = js(a, "SELECT jsonb_build_object('custom',custom_plans,'generic',generic_plans) FROM pg_prepared_statements WHERE name='g';")
        assert stats == {'custom': 0, 'generic': 2}, stats
        result['cases'].append({'name': 'actual_generic_plan_policy_transition', 'verdict': 'PASS', 'off': off, 'on': on, 'counts': stats})
        # A plain-column index with an instrumented SQL ordering operator
        # reaches the raw exact heap pass without an expression index.
        ask(a, "CREATE FUNCTION review_ops.slow_distance(vector,vector) RETURNS float8 LANGUAGE plpgsql IMMUTABLE STRICT AS $$BEGIN RAISE NOTICE 'D_EXACT_EXPR pid=%',pg_backend_pid();PERFORM pg_sleep(0.05);RETURN $1 <-> $2;END$$; CREATE OPERATOR review_ops.<~> (LEFTARG=vector,RIGHTARG=vector,PROCEDURE=review_ops.slow_distance); CREATE OPERATOR CLASS review_ops.slow_ops FOR TYPE vector USING pg_cuvs_hnsw AS OPERATOR 1 review_ops.<~> (vector,vector) FOR ORDER BY float_ops, FUNCTION 1 vector_l2_squared_distance(vector,vector); CREATE TABLE review_ops.slow_items(id int,v vector(2)); CREATE INDEX slow_i ON review_ops.slow_items USING pg_cuvs_hnsw(v review_ops.slow_ops); INSERT INTO review_ops.slow_items SELECT i,('[ '||i||',0]')::vector(2) FROM generate_series(1,12)i;")
        query = "SELECT id FROM review_ops.slow_items ORDER BY v OPERATOR(review_ops.<~>) '[0,0]'::vector LIMIT 1;"
        for event in ['cancel', 'timeout']:
            target = Session(conn)
            try:
                ask(target, '\\set ON_ERROR_STOP off\n\\set VERBOSITY verbose\nSET enable_seqscan=off;SET cuvs.hnsw_exact_scan=on;')
                tpid = int(ask(target, 'SELECT pg_backend_pid();')[-1])
                plan = json.loads('\n'.join(ask(target, 'EXPLAIN(FORMAT JSON,COSTS OFF) ' + query)))[0]['Plan']
                assert any((n.get('Index Name') == 'slow_i' for n in nodes(plan)))
                if event == 'timeout':
                    ask(target, "SET statement_timeout='200ms';")
                outcome = {}

                def worker():
                    try:
                        outcome['out'] = ask(target, query)
                    except Exception as e:
                        outcome['error'] = repr(e)
                t = threading.Thread(target=worker)
                t.start()
                assert target.wait_error('D_EXACT_EXPR pid=' + str(tpid), 10), 'did not enter exact expression evaluation'
                if event == 'cancel':
                    assert ask(b, f'SELECT pg_cancel_backend({tpid});') == ['t']
                t.join(5)
                assert not t.is_alive() and 'error' not in outcome, outcome
                assert target.wait_error('57014', 10), target.errors
                ask(target, 'SET statement_timeout=0;')
                assert int(ask(target, 'SELECT pg_backend_pid();')[-1]) == tpid
                rows = js(target, "SELECT jsonb_agg(x) FROM(SELECT id FROM review_ops.l2 ORDER BY v <-> '[0,0]'::vector)x;")
                assert len(rows) == 5
                result['cases'].append({'name': 'actual_product_' + event + '_same_backend_recovery', 'verdict': 'PASS', 'pid': tpid, 'plan': plan, 'errors': target.errors, 'post_rows': rows})
            finally:
                target.close()
        result['verdict'] = 'PASS'
    except Exception as e:
        result.update(verdict='FAIL', error=repr(e))
        raise
    finally:
        if a:
            if held:
                try:
                    cover_hold.release(lambda q: ask(a, q))
                except Exception as e:
                    result.setdefault('cleanup_error', repr(e))
            a.close()
        if b:
            b.close()
        (p / 'transcript.json').write_text(json.dumps(transcript, indent=2) + '\n')
        (p / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps({'verdict': result.get('verdict'), 'cases': len(result['cases']), 'error': result.get('error')}))
if __name__ == '__main__':
    main()
