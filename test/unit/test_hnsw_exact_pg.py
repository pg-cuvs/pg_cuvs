#!/usr/bin/env python3
"""Exercise exact scans in an explicitly supplied, disposable PG16 cluster.

Requires pgvector and server headers. The data-directory match is mandatory;
this runner creates a fresh schema and AM and never restarts/deletes a server.
Artifacts and SQL are preserved. A failing psql process makes the runner fail.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--socket', required=True)
    ap.add_argument('--port', required=True)
    ap.add_argument('--user', required=True)
    ap.add_argument('--database', default='postgres')
    ap.add_argument('--expected-data-directory', required=True)
    ap.add_argument('--schema', required=True)
    ap.add_argument('--pg-bin', type=Path)
    ap.add_argument('--mutate-orderby', action='store_true')
    args = ap.parse_args()
    if not re.fullmatch(r'[a-z][a-z0-9_]{0,35}', args.schema):
        ap.error('schema must be a simple lower-case identifier (max 36 characters)')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    repo = Path(__file__).resolve().parents[2]
    pg = args.pg_bin or Path(subprocess.check_output(['pg_config', '--bindir'], text=True).strip())
    psql = [str(pg / 'psql'), '-XqAt', '-v', 'ON_ERROR_STOP=1', '-h', args.socket,
            '-p', args.port, '-U', args.user, '-d', args.database]
    result = {'scope': 'private CPU PG exact AM; no product integration or GPU proof',
              'schema': args.schema, 'mutant': args.mutate_orderby, 'cases': []}
    try:
        actual = subprocess.check_output(psql + ['-c', 'SHOW data_directory;'], text=True).strip()
        assert actual == args.expected_data_directory, (actual, args.expected_data_directory)
        result['data_directory'] = actual
        inputs = [repo / 'src/cuvs_hnsw_exact.c', repo / 'src/cuvs_hnsw_exact.h',
                  repo / 'test/unit/hnsw_exact_pg_probe.c', Path(__file__)]
        result['inputs_sha256'] = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        include = subprocess.check_output(['pg_config', '--includedir-server'], text=True).strip()
        flags = shlex.split(subprocess.check_output(['pg_config', '--cflags'], text=True))
        library = output / 'exact_probe.so'
        command = [os.environ.get('CC', 'cc'), *flags, '-shared', '-fPIC',
                   '-Werror', '-Wno-unused-parameter', '-Wno-declaration-after-statement',
                   '-I' + include, '-I' + str(repo / 'src'), str(inputs[0]), str(inputs[2]),
                   '-DCUVS_HNSW_EXACT_TEST_SKIP_DELAY_US=1000', '-o', str(library)]
        if args.mutate_orderby:
            command.insert(1, '-DEXACT_PROBE_MUTATE_ORDERBY')
        result['compile_command'] = command
        compiled = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (output / 'compile.log').write_text(compiled.stdout)
        assert compiled.returncode == 0, 'compile failed'
        result['library_sha256'] = hashlib.sha256(library.read_bytes()).hexdigest()
        s = args.schema
        am = s + '_am'
        sql = ["CREATE EXTENSION IF NOT EXISTS vector;", f"CREATE SCHEMA {s};",
               f"CREATE FUNCTION {s}.handler(internal) RETURNS index_am_handler AS '{library}',"
               "'hnsw_exact_probe_handler' LANGUAGE C STRICT;",
               f"CREATE ACCESS METHOD {am} TYPE INDEX HANDLER {s}.handler;",
               f"CREATE OPERATOR CLASS {s}.l2_ops DEFAULT FOR TYPE vector USING {am} AS "
               "OPERATOR 1 <-> (vector,vector) FOR ORDER BY float_ops, "
               "FUNCTION 1 vector_l2_squared_distance(vector,vector);",
               f"CREATE TABLE {s}.hot(id int,v vector(2),note text) WITH(fillfactor=50);",
               f"INSERT INTO {s}.hot VALUES (1,'[0,0]','base'),(2,'[1,0]','base'),(3,'[2,0]','base');",
               f"CREATE INDEX hot_i ON {s}.hot USING {am}(v {s}.l2_ops);",
               'SET enable_seqscan=off; SET enable_bitmapscan=off;']
        expected = {}

        def sample(name, table='hot', query='[0,0]', limit='', values=None):
            sql.append(f"EXPLAIN (FORMAT JSON,COSTS OFF) SELECT id FROM {s}.{table} "
                       f"ORDER BY v <-> '{query}'::vector {limit};")
            sql.append(f"SELECT jsonb_build_object('case','{name}','pid',pg_backend_pid(),"
                       "'rows',coalesce(jsonb_agg(x),'[]'::jsonb)) FROM "
                       f"(SELECT id,ctid::text AS tid,note,v <-> '{query}'::vector AS d "
                       f"FROM {s}.{table} ORDER BY v <-> '{query}'::vector {limit}) x;")
            expected[name] = values

        base = [[1, '(0,1)', 'base', 0], [2, '(0,2)', 'base', 1], [3, '(0,3)', 'base', 2]]
        hot = [[1, '(0,4)', 'HOT', 0], *base[1:]]
        sample('before', values=base)
        sql.append("BEGIN;")
        sql.append(f"UPDATE {s}.hot SET note='HOT' WHERE id=1;")
        for n in range(8):
            sample('hot_' + str(n), values=hot)
        # HOT is proven by the counter rather than inferred from CTID alone.
        sql.append(f"SELECT jsonb_build_object('hot_updates',n_tup_hot_upd) "
                   f"FROM pg_stat_xact_user_tables WHERE relid='{s}.hot'::regclass;")
        sample('early_end', limit='LIMIT 1', values=hot[:1])
        sample('after_early_end', values=hot)
        sql.append(f"PREPARE exactq(vector) AS SELECT id FROM {s}.hot ORDER BY v <-> $1 LIMIT 1;")
        sql.append("SELECT jsonb_build_object('prepared',true); EXECUTE exactq('[2,0]'); EXECUTE exactq('[0,0]');")
        # One executor scan is parameterized by three outer rows: rescan changes
        # orderByData while the previous exact state must be released.
        sql.append('SET enable_material=off;')
        sql.append(f"EXPLAIN(ANALYZE,FORMAT JSON,COSTS OFF) SELECT q.id,a.id FROM {s}.hot q CROSS JOIN LATERAL "
                   f"(SELECT id FROM {s}.hot t ORDER BY t.v <-> q.v LIMIT 1) a;")
        sql.append("SELECT jsonb_build_object('case','rescan','pid',pg_backend_pid(),'rows',jsonb_agg(x ORDER BY q)) FROM "
                   f"(SELECT q.id AS q,a.id FROM {s}.hot q CROSS JOIN LATERAL "
                   f"(SELECT id FROM {s}.hot t ORDER BY t.v <-> q.v LIMIT 1) a)x;")
        sql.append("COMMIT;")
        sql.append(f"VACUUM {s}.hot;")
        sample('after_vacuum', values=hot)
        sql.append(f"CREATE TABLE {s}.spill AS SELECT i AS id, ('['||i||',0]')::vector(2) AS v,'large'::text AS note "
                   "FROM generate_series(0,5999) i;")
        sql.append(f"CREATE INDEX spill_i ON {s}.spill USING {am}(v {s}.l2_ops);")
        sql.append("SET work_mem='64kB'; SET log_temp_files=0;")
        sample('spill', table='spill')
        # Force a real tuplesort error during exact_begin, then keep using the
        # same backend. The exception block rolls back its resource owner.
        sql.append(f"EXPLAIN(FORMAT JSON,COSTS OFF) SELECT id FROM {s}.spill "
                   "ORDER BY v <-> '[0,0]'::vector LIMIT 1;")
        sql.append(f"SELECT jsonb_build_object('error_am',am.amname) FROM pg_class c "
                   f"JOIN pg_namespace n ON n.oid=c.relnamespace JOIN pg_am am ON am.oid=c.relam "
                   f"WHERE n.nspname='{s}' AND c.relname='spill_i';")
        sql.append("SET temp_file_limit='0';")
        sql.append(f"DO $$BEGIN PERFORM count(*) FROM (SELECT id FROM {s}.spill ORDER BY v <-> '[0,0]'::vector LIMIT 6000) x; "
                   "RAISE EXCEPTION 'expected temp_file_limit error was absent'; "
                   "EXCEPTION WHEN configuration_limit_exceeded THEN "
                   "RAISE NOTICE 'EXACT_ERROR_53400 pid=%',pg_backend_pid(); END$$;")
        sql.append("SET temp_file_limit='-1';")
        # A cancel must be honoured while the second pass skips the answers the first pass returned
        # (the private build sleeps 1 ms per skipped answer: about 4 s without a check).
        sql.append("SET statement_timeout='300ms';")
        sql.append(f"DO $$DECLARE t0 timestamptz := clock_timestamp(); BEGIN "
                   f"PERFORM count(*) FROM (SELECT id FROM {s}.spill ORDER BY v <-> '[0,0]'::vector LIMIT 6000) x; "
                   "RAISE EXCEPTION 'the cancel was not delivered'; "
                   "EXCEPTION WHEN query_canceled THEN "
                   "RAISE NOTICE 'EXACT_SKIP_CANCEL ms=%',round(extract(epoch from clock_timestamp()-t0)*1000); END$$;")
        sql.append("SET statement_timeout=0;")
        sample('after_error', values=hot)
        # A direct utility caller must never bypass the PostgreSQL executor path for a table
        # with row-level security enabled. This is the last defence after planning and rescan.
        sql.append(f"CREATE TABLE {s}.rlsd(id int,v vector(2)); INSERT INTO {s}.rlsd VALUES (1,'[0,0]'),(2,'[1,0]');"
                   f"CREATE INDEX rlsd_i ON {s}.rlsd USING {am}(v {s}.l2_ops); ALTER TABLE {s}.rlsd ENABLE ROW LEVEL SECURITY;")
        sql.append(f"DO $$BEGIN PERFORM id FROM {s}.rlsd ORDER BY v <-> '[0,0]'::vector LIMIT 1; "
                   "RAISE EXCEPTION 'direct RLS exact call was not rejected'; "
                   "EXCEPTION WHEN feature_not_supported THEN "
                   "RAISE NOTICE 'EXACT_RLS_BACKSTOP pid=% msg=%',pg_backend_pid(),SQLERRM; END$$;")
        sql.append('SET enable_indexscan=off; SET enable_seqscan=on;')
        sql.append("SELECT jsonb_build_object('oracle',jsonb_agg(x)) FROM "
                   f"(SELECT id,ctid::text AS tid,note,v <-> '[0,0]'::vector AS d "
                   f"FROM {s}.spill ORDER BY v <-> '[0,0]'::vector) x;")
        sql.append("SELECT jsonb_build_object('final_pid',pg_backend_pid());")
        (output / 'regression.sql').write_text('\n'.join(sql) + '\n')
        proc = subprocess.run(psql + ['-f', str(output / 'regression.sql')], text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        (output / 'stdout.log').write_text(proc.stdout)
        (output / 'stderr.log').write_text(proc.stderr)
        result['psql_exit'] = proc.returncode
        assert proc.returncode == 0, 'psql failed (see stdout.log/stderr.log)'
        rows = [json.loads(line) for line in proc.stdout.splitlines() if line.startswith('{')]
        cases = [r for r in rows if 'case' in r]
        assert len(cases) == len(expected) + 1, 'missing cases'
        pid = cases[0]['pid']
        scalar_results = [line for line in proc.stdout.splitlines() if re.fullmatch(r'[0-9]+', line)]
        assert scalar_results == ['3', '1'], ('PREPARE/EXECUTE results', scalar_results)
        # Parse complete EXPLAIN JSON arrays rather than matching an unrelated
        # Actual Loops field anywhere in the output.
        plans = []
        pending = ''
        for line in proc.stdout.splitlines():
            if pending or line == '[':
                pending += line + '\n'
                try:
                    plans.append(json.loads(pending)[0]['Plan'])
                    pending = ''
                except json.JSONDecodeError:
                    pass
        assert not pending, 'truncated EXPLAIN JSON'

        def nodes(plan):
            yield plan
            for child in plan.get('Plans', []):
                yield from nodes(child)

        rescans = [n for plan in plans for n in nodes(plan)
                   if n.get('Node Type') == 'Index Scan' and n.get('Index Name') == 'hot_i'
                   and n.get('Alias') == 't' and n.get('Actual Loops') == 3]
        assert len(rescans) == 1 and rescans[0]['Actual Rows'] == 1, rescans
        result['rescan_index_node'] = rescans[0]
        error_plans = [plan for plan in plans if plan.get('Node Type') == 'Limit'
                       and any(n.get('Index Name') == 'spill_i' for n in nodes(plan))]
        assert len(error_plans) == 1, error_plans
        assert not any(n.get('Node Type') in ('Sort', 'Incremental Sort')
                       for n in nodes(error_plans[0])), 'error could come from executor sort'
        assert [r['error_am'] for r in rows if 'error_am' in r] == [am]
        result['error_plan'] = error_plans[0]
        result['prepared_results'] = scalar_results
        for r in cases:
            assert r['pid'] == pid, 'backend changed'
            if r['case'] == 'rescan':
                assert r['rows'] == [{'q': 1, 'id': 1}, {'q': 2, 'id': 2}, {'q': 3, 'id': 3}], r
            elif r['case'] == 'spill':
                assert [x['id'] for x in r['rows']] == list(range(6000)), 'spill ordering/coverage'
                assert r['rows'] == next(x['oracle'] for x in rows if 'oracle' in x), 'spill sequential oracle'
            else:
                actual = [[x['id'], x['tid'], x['note'], x['d']] for x in r['rows']]
                assert actual == expected[r['case']], (r['case'], actual, expected[r['case']])
            result['cases'].append({'name': r['case'], 'pid': pid, 'rows': len(r['rows']), 'verdict': 'PASS'})
        assert [r['hot_updates'] for r in rows if 'hot_updates' in r] == [1], 'HOT was not exercised'
        assert [r['final_pid'] for r in rows if 'final_pid' in r] == [pid], 'final backend changed'
        assert f'EXACT_ERROR_53400 pid={pid}' in proc.stderr, 'same-backend error cleanup path not exercised'
        assert f'EXACT_RLS_BACKSTOP pid={pid} msg=RLS exact scans require the PostgreSQL executor path' in proc.stderr, 'RLS backstop not exercised'
        cancelled = re.search(r'EXACT_SKIP_CANCEL ms=(\d+)', proc.stderr)
        assert cancelled, 'cancel during the skipping pass was not delivered'
        assert int(cancelled.group(1)) < 2500, ('cancel waited for the skipping pass', cancelled.group(1))
        result['skip_cancel_ms'] = int(cancelled.group(1))
        assert 'Index Scan' in proc.stdout, 'no scan plan'
        result['verdict'] = 'PASS'
        return 0
    except Exception as e:
        result.update(verdict='FAIL', error=type(e).__name__ + ': ' + str(e))
        return 1
    finally:
        (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result))


if __name__ == '__main__':
    sys.exit(main())
