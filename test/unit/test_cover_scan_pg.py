#!/usr/bin/env python3
"""What an ordered scan of a pg_cuvs_hnsw index returns when the index has a cover descriptor (CPU only).

Indexes that can be had without the CAGRA daemon: those built over an empty table (CERTIFIED, every inserted row is in the
ledger) and pgvector hnsw indexes whose access method was switched in the catalog, with a descriptor planted by test/unit/
cover_plant.c. The reference is PostgreSQL's own exact sort (index scans off).

  S1 CERTIFIED: every ordered scan equals the exact ranking (the ledger holds all rows), for many queries and limits;
     rolled-back and deleted rows never appear;
  S2 LOST and UNTRUSTED (a damaged descriptor): exact ranking from the heap, with a notice; a planted CERTIFIED whose ledger
     cannot be read falls back to the whole heap as well;
  S3 cuvs.hnsw_exact_scan = on on a CERTIFIED index still gives the exact ranking;
  S4 an index without a descriptor and an OFF one are answered as before (no notice, approximate path);
  S5 shapes: a plain-column, an expression and a partial index with a descriptor (CERTIFIED with a ledger, and LOST), each read as a
     table without row-level security, as a table with it, and through a security-barrier view: every statement is answered, and
     the answer equals the exact ranking. A statement that has row-level security or a barrier and an index with an expression or
     predicate does not use that index (PostgreSQL's sequential scan answers); the other shapes do;
  S6 the expression and the predicate of an index are evaluated as the role that reads, never as the owner; no source of the scan
     switches the user;
  S7 the one error that is left: a plan that keeps such an index on a protected statement stops at the scan with a message that says
     what to do. An index without a descriptor is answered as before on a protected table (S5).
  S8 a partition or an inheritance child under a parent that has the policy (the child shows no row-level security of its own): the
     setting turned on while the statement runs, and a plan that kept the index, stop before the expression is called on a hidden row;
  S9 the merge of a certified index with a policy that hides half of the rows gives fewer rows than LIMIT (as pgvector alone);
  S10 a predicate with a function that is not built in: planning does not choose the index;
  S11 a predicate function that the statement does not call is not run in the reading session (here: a superuser);
  S12 a scan that runs outside ExecutorRun (a data-modifying CTE that the main query does not read) does not touch hidden rows;
  S13 which predicates count as built in, and that the expression of an index is the one the statement wrote;
  S14 the same checks inside a function, a cursor, a trigger, SPI, COPY, a parallel plan and EXPLAIN ANALYZE;
  S15 the merge of a certified partial index after updates and deletes that move rows across the predicate.
"""
import argparse
import json
from pathlib import Path
import random
import re
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402

check = dg.check


def vec(rng, d=8):
    return '[' + ','.join(f'{rng.random():.5f}' for _ in range(d)) + ']'


def ids(c, table, q, k, index_scan, extra=''):
    flags = 'SET enable_seqscan = off; SET enable_bitmapscan = off;' if index_scan else 'SET enable_indexscan = off; SET enable_bitmapscan = off;'
    rc, out = c.q(f"{flags} {extra} SELECT id FROM {table} ORDER BY v <-> '{q}' LIMIT {k}")
    return rc, out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True)
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5499)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    plant = out / 'cover_plant'
    r = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(repo / 'src'), str(repo / 'test/unit/cover_plant.c'), '-o', str(plant)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    assert r.returncode == 0, r.stdout
    rng = random.Random(7)
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'sc', out, args.port)
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')

            # S1
            c.scalar('CREATE TABLE e(id int, v vector(8))')
            c.scalar('CREATE INDEX ei ON e USING pg_cuvs_hnsw (v vector_l2_ops)')
            c.scalar("SELECT setseed(0.5); INSERT INTO e SELECT g, l.a::vector FROM generate_series(1, 3000) g, "
                     "LATERAL (SELECT array_agg(random()::real) AS a FROM generate_series(1, 8) k WHERE g > 0) l")
            rc, _ = c.q("BEGIN; INSERT INTO e VALUES (90001, '[0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5]'); ROLLBACK")
            c.scalar('DELETE FROM e WHERE id % 10 = 0')
            plan = c.scalar("SET enable_seqscan = off; EXPLAIN SELECT id FROM e ORDER BY v <-> '[0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5]' LIMIT 5")
            check('S1 the planner uses the index', 'Index Scan using ei' in plan, plan)
            bad = 0
            for _ in range(25):
                q = vec(rng)
                for k in (1, 10, 100):
                    rc1, got = ids(c, 'e', q, k, True)
                    rc2, want = ids(c, 'e', q, k, False)
                    if rc1 != 0 or rc2 != 0 or got != want:
                        bad += 1
            check('S1 75 ordered scans (25 queries x limits 1, 10, 100) equal the exact ranking', bad == 0, f'{bad} differ')
            rc, res = c.q("SET enable_seqscan = off; SELECT count(*) FROM e WHERE id = 90001 OR id % 10 = 0")
            rc, res = c.q("SET enable_seqscan = off; SELECT id FROM e ORDER BY v <-> '[0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5]' LIMIT 3000")
            got = set(int(x) for x in res.split('\n') if x)
            check('S1 rolled-back and deleted rows do not appear', 90001 not in got and not any(i % 10 == 0 for i in got) and len(got) == 2700, str(len(got)))

            # S3
            bad = 0
            for _ in range(5):
                q = vec(rng)
                rc1, got = ids(c, 'e', q, 20, True, extra='SET cuvs.hnsw_exact_scan = on;')
                rc2, want = ids(c, 'e', q, 20, False)
                if rc1 != 0 or got != want:
                    bad += 1
            check('S3 cuvs.hnsw_exact_scan = on on a certified index gives the exact ranking', bad == 0, f'{bad} differ')

            # S2/S4 on pgvector indexes under the pg_cuvs_hnsw access method
            def hack(table, n=1500):
                c.make_index(table, table + 'i')
                return c.data / c.scalar(f"SELECT pg_relation_filepath('{table}i')")

            def plant_mode(path, mode):
                c.stop()
                rp = subprocess.run([str(plant), str(path), mode], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                assert rp.returncode == 0, rp.stdout
                c.start()

            def compare_exact(table, label, expect_notice):
                bad = 0
                notices = 0
                for _ in range(8):
                    q = vec(rng, 3)
                    rc1, got = c.q(f"SET enable_seqscan = off; SELECT id FROM {table} ORDER BY v <-> '{q}' LIMIT 100")
                    rc2, want = c.q(f"SET enable_indexscan = off; SET enable_bitmapscan = off; SELECT id FROM {table} ORDER BY v <-> '{q}' LIMIT 100")
                    notices += 'answers from the heap' in got
                    got_ids = [x for x in got.split('\n') if x.isdigit()]
                    if rc1 != 0 or got_ids != [x for x in want.split('\n') if x.isdigit()]:
                        bad += 1
                check(f'{label}: ordered scans equal the exact ranking', bad == 0, f'{bad} differ')
                check(f'{label}: the notice {"appears" if expect_notice else "does not appear"}', (notices > 0) == expect_notice, str(notices))

            for mode, label, notice in (('lost', 'S2 LOST', True), ('corrupt', 'S2 damaged descriptor (UNTRUSTED)', True),
                                        ('certified', 'S2 CERTIFIED whose ledger cannot be read', True)):
                t = 'h_' + mode
                c.make_index(t, t + 'i')
                path = c.data / c.scalar(f"SELECT pg_relation_filepath('{t}i')")
                plant_mode(path, mode)
                compare_exact(t, label, notice)

            # S2b: a LOST index whose chain reads, for a reason other than a VACUUM (here the downgrade mark of
            # pg_cuvs_cover_invalidate_all(), before an older library writes to the index), is answered from the heap, exactly, with a notice: for
            # such a reason the ledger is known not to hold every row the graph may miss. The merge of a LOST index is kept only for one that a VACUUM
            # took off (cover_vacuum_merge in src/cuvs_hnsw_scan.c). Built on an empty table, so all 300 rows are ledger keys and a merge would give
            # no notice: the notice is what tells the two apart. The mark also takes ei (S1, S3) off, which nothing below reads.
            c.scalar('CREATE TABLE h_dg(id int, v vector(3))')
            c.scalar('CREATE INDEX h_dgi ON h_dg USING pg_cuvs_hnsw (v vector_l2_ops)')
            c.scalar('INSERT INTO h_dg SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 300) g')
            c.scalar('SELECT pg_cuvs_cover_invalidate_all()')
            st = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('h_dgi')::text"))
            check('S2b the downgrade mark leaves the index LOST with a chain that reads', st.get('class') == 'lost' and st.get('ledger_ok') is True
                  and st.get('ledger_keys') == 300, str(st))
            compare_exact('h_dg', 'S2b LOST (downgrade) whose chain reads', True)

            # S4
            c.make_index('h_legacy', 'h_legacyi')
            rc, res = c.q("SET enable_seqscan = off; SELECT count(*) FROM (SELECT id FROM h_legacy ORDER BY v <-> '[1,1,1]' LIMIT 15) x")
            check('S4 an index without a descriptor is answered as before, silently', rc == 0 and res.endswith('15') and 'answers from the heap' not in res, res)
            c.make_index('h_off', 'h_offi')
            plant_mode(c.data / c.scalar("SELECT pg_relation_filepath('h_offi')"), 'off')
            rc, res = c.q("SET enable_seqscan = off; SELECT count(*) FROM (SELECT id FROM h_off ORDER BY v <-> '[1,1,1]' LIMIT 15) x")
            check('S4 an OFF index is answered as before, silently', rc == 0 and res.endswith('15') and 'answers from the heap' not in res, res)

            # S5: shapes x tables x states. The CERTIFIED indexes are built on an empty table (every inserted row is in the ledger,
            # so the merge ranks all of them); the LOST ones are planted pgvector indexes.
            role = 'shape_reader'
            c.scalar(f'CREATE ROLE {role} LOGIN')
            zero = "'[0,0,0]'::vector"
            shapes = {
                'plain': dict(key='v', where='', order='v', cond=''),
                'expr': dict(key=f'((v + {zero})::vector(3))', where='', order=f'(v + {zero})::vector(3)', cond=''),
                'pred': dict(key='v', where='id > 5', order='v', cond='id > 5'),
            }
            built = []
            for shape, sp in shapes.items():
                for state in ('cert', 'lost'):
                    for prot in ('n', 'rls'):
                        t = f'{shape}_{state}_{prot}'
                        if state == 'cert':
                            c.scalar(f'CREATE TABLE {t}(id int, v vector(3))')
                            c.scalar(f'CREATE INDEX {t}i ON {t} USING pg_cuvs_hnsw ({sp["key"]} vector_l2_ops)' + (f' WHERE {sp["where"]}' if sp['where'] else ''))
                            c.scalar(f'INSERT INTO {t} SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 300) g')
                        else:
                            c.make_index(t, t + 'i', key=sp['key'], where=sp['where'])
                            built.append(c.data / c.scalar(f"SELECT pg_relation_filepath('{t}i')"))
                        if prot == 'rls':
                            c.scalar(f'ALTER TABLE {t} ENABLE ROW LEVEL SECURITY; CREATE POLICY pol ON {t} USING (id % 2 = 0); GRANT SELECT ON {t} TO {role}')
                        else:
                            # a security-barrier view over the unprotected table; the ordering is inside the view, so the index scan is inside
                            # the barrier subquery
                            c.scalar(f'CREATE VIEW {t}_bv WITH (security_barrier = true) AS SELECT id, v FROM {t} WHERE id % 2 = 0'
                                     + (f' AND {sp["cond"]}' if sp['cond'] else '') + f" ORDER BY {sp['order']} <-> '[1,1,1]'::vector LIMIT 100; GRANT SELECT ON {t}_bv TO {role}")
            c.stop()
            for path in built:
                rp = subprocess.run([str(plant), str(path), 'lost'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                assert rp.returncode == 0, rp.stdout
            c.start()

            def sql_of(shape, t, how, q):
                sp = shapes[shape]
                if how == 'barrier':
                    return f'SELECT id FROM {t}_bv'
                return f"SELECT id FROM {t}" + (f" WHERE {sp['cond']}" if sp['cond'] else '') + f" ORDER BY {sp['order']} <-> '{q}' LIMIT 100"

            def id_list(res):
                return [x for x in res.split('\n') if x.isdigit()]

            for shape in shapes:
                for state in ('cert', 'lost'):
                    for how, prot, who in (('none', 'n', None), ('rls', 'rls', role), ('barrier', 'n', role)):
                        t = f'{shape}_{state}_{prot}'
                        label = f'S5 {shape} index, {"CERTIFIED merge" if state == "cert" else "LOST heap"}, {how}'
                        expects_index = shape == 'plain' or how == 'none'
                        bad, errors, notices = 0, 0, 0
                        for k in range(4):
                            q = vec(rng, 3)
                            sql = sql_of(shape, t, how, q)
                            rc1, got = c.q(f'SET enable_seqscan = off; SET enable_bitmapscan = off; {sql}', role=who)
                            rc2, want = c.q(f'SET enable_indexscan = off; SET enable_bitmapscan = off; {sql}', role=who)
                            errors += rc1 != 0
                            notices += 'answers from the heap' in got
                            if rc1 != 0 or rc2 != 0 or id_list(got) != id_list(want) or not id_list(want):
                                bad += 1
                            if how == 'barrier':
                                break
                        rc, plan = c.q(f'SET enable_seqscan = off; SET enable_bitmapscan = off; EXPLAIN (COSTS OFF) {sql_of(shape, t, how, "[1,1,1]")}', role=who)
                        check(f'{label}: every statement is answered (no error)', errors == 0, f'{errors} errors: {got}')
                        check(f'{label}: the answer equals the exact ranking', bad == 0, f'{bad} differ')
                        check(f'{label}: the index is {"used" if expects_index else "not used (PostgreSQL scans the table)"}',
                              (f'Index Scan using {t}i' in plan) == expects_index, plan)
                        check(f'{label}: the notice of a heap answer is {"given" if (state == "lost" and expects_index) else "not given"}',
                              (notices > 0) == (state == 'lost' and expects_index), str(notices))

            # an index without a descriptor and an OFF one on a protected table are answered as before
            c.scalar('ALTER TABLE h_legacy ENABLE ROW LEVEL SECURITY')
            rc, res = c.q("SET enable_seqscan = off; SELECT count(*) FROM (SELECT id FROM h_legacy ORDER BY v <-> '[1,1,1]' LIMIT 15) x")
            check('S5 an index without a descriptor on a row-level security table is answered as before', rc == 0 and res.endswith('15'), res)
            c.make_index('x_legacy', 'x_legacyi', key=shapes['expr']['key'])
            c.scalar(f'ALTER TABLE x_legacy ENABLE ROW LEVEL SECURITY; CREATE POLICY pol ON x_legacy USING (id % 2 = 0); GRANT SELECT ON x_legacy TO {role}')
            rc, plan = c.q(f"SET enable_seqscan = off; EXPLAIN (COSTS OFF) SELECT id FROM x_legacy ORDER BY {shapes['expr']['order']} <-> '[1,1,1]' LIMIT 15", role=role)
            check('S5 an expression index without a descriptor keeps its index path on a protected table', 'Index Scan using x_legacyi' in plan, plan)

            # S6: the expression and the predicate run as the role that reads. The probe function says who runs it.
            c.scalar("CREATE FUNCTION pf_pred(i int) RETURNS boolean LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN "
                     "IF current_setting('probe.active', true) = 'on' THEN RAISE NOTICE 'PRED_AS=%', current_user; END IF; RETURN i > 5; END $$")
            c.scalar("CREATE FUNCTION pf_expr(x vector) RETURNS vector LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN "
                     "IF current_setting('probe.active', true) = 'on' THEN RAISE NOTICE 'EXPR_AS=%', current_user; END IF; RETURN x; END $$")
            c.scalar('CREATE TABLE pf(id int, v vector(3))')
            c.scalar('CREATE INDEX pfi ON pf USING pg_cuvs_hnsw (v vector_l2_ops) WHERE pf_pred(id)')
            c.scalar('CREATE TABLE pe(id int, v vector(3))')
            c.scalar('CREATE INDEX pei ON pe USING pg_cuvs_hnsw ((pf_expr(v)::vector(3)) vector_l2_ops)')
            c.scalar(f'INSERT INTO pf SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 120) g; '
                     f'INSERT INTO pe SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 120) g; '
                     f'GRANT SELECT ON pf, pe TO {role}')
            rc, res = c.q("SET enable_seqscan = off; SET probe.active = 'on'; SELECT id FROM pf WHERE pf_pred(id) ORDER BY v <-> '[3,3,0]' LIMIT 5", role=role)
            check('S6 the predicate of a partial index is evaluated by the scan, as the reading role', rc == 0 and res.count('PRED_AS=' + role) > 0 and 'PRED_AS=postgres' not in res, res[:400])
            rc, res = c.q("SET enable_seqscan = off; SET probe.active = 'on'; SELECT id FROM pe ORDER BY pf_expr(v)::vector(3) <-> '[3,3,0]' LIMIT 5", role=role)
            check('S6 the expression of an expression index is evaluated by the scan, as the reading role', rc == 0 and res.count('EXPR_AS=' + role) > 0 and 'EXPR_AS=postgres' not in res, res[:400])
            sources = ' '.join((repo / 'src' / f).read_text() for f in ('cuvs_hnsw_scan.c', 'cuvs_hnsw_exact.c', 'cuvs_hnsw_rls.c'))
            check('S6 no source of the scan, the exact module or the planner part changes the user',
                  not any(w in sources for w in ('SetUserIdAndSecContext', 'SetUserId(', 'SetSessionUserId', 'SECURITY_LOCAL_USERID_CHANGE', 'SECURITY_RESTRICTED_OPERATION')), '')

            # S5b: a plain-column index on a row-level security table. The rows the policy hides never come back, the count is not
            # short of the LIMIT because of them (merge and heap alike), and the policy runs as the reading role.
            for state in ('cert', 'lost'):
                t = f'plain_{state}_rls'
                for limit, want_n in ((100, 100), (150, 150), (200, 150)):
                    rc, res = c.q(f"SET enable_seqscan = off; SELECT id FROM {t} ORDER BY v <-> '[3,3,0]' LIMIT {limit}", role=role)
                    got = id_list(res)
                    check(f'S5b {state}: LIMIT {limit} on a table where the policy shows 150 of 300 rows gives {want_n} rows, none of them hidden',
                          rc == 0 and len(got) == want_n and all(int(x) % 2 == 0 for x in got), f'{len(got)} rows: {res[:200]}')
            c.scalar("CREATE FUNCTION pol_fn(i int) RETURNS boolean LANGUAGE plpgsql STABLE AS $$ BEGIN "
                     "IF current_setting('probe.active', true) = 'on' THEN RAISE NOTICE 'POL_AS=%', current_user; END IF; RETURN i % 2 = 0; END $$")
            c.scalar('CREATE TABLE polp(id int, v vector(3))')
            c.scalar('CREATE INDEX polpi ON polp USING pg_cuvs_hnsw (v vector_l2_ops)')
            c.scalar(f'INSERT INTO polp SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 120) g; '
                     f'ALTER TABLE polp ENABLE ROW LEVEL SECURITY; CREATE POLICY pol ON polp USING (pol_fn(id)); GRANT SELECT ON polp TO {role}')
            rc, plan = c.q("SET enable_seqscan = off; EXPLAIN (COSTS OFF) SELECT id FROM polp ORDER BY v <-> '[3,3,0]' LIMIT 5", role=role)
            check('S5b the plain-column index is used on the table whose policy calls a function', 'Index Scan using polpi' in plan, plan)
            rc, res = c.q("SET enable_seqscan = off; SET probe.active = 'on'; SELECT id FROM polp ORDER BY v <-> '[3,3,0]' LIMIT 5", role=role)
            check('S5b the policy runs as the reading role', rc == 0 and res.count('POL_AS=' + role) > 0 and 'POL_AS=postgres' not in res and len(id_list(res)) == 5, res[:300])

            # S5c: where the premise of the plan could break. A prepared statement with a generic plan, the table turned into one with row-level
            # security after the first execution, a role that changes, the setting turned on and off: none of them ends in an error.
            sess = dg.Session(c)
            sess.run("SET plan_cache_mode = force_generic_plan; SET enable_seqscan = off")
            sess.run(f"PREPARE pq(vector) AS SELECT id FROM expr_cert_n ORDER BY (v + {zero})::vector(3) <-> $1 LIMIT 5")
            r1 = id_list(sess.run("EXECUTE pq('[3,3,0]')"))
            rc, want1 = c.q(f"SET enable_indexscan = off; SELECT id FROM expr_cert_n ORDER BY (v + {zero})::vector(3) <-> '[3,3,0]' LIMIT 5")
            check('S5c prepared statement, generic plan, no row-level security: exact answer', r1 == id_list(want1) and len(r1) == 5, f'{r1} {want1}')
            c.scalar(f"ALTER TABLE expr_cert_n ENABLE ROW LEVEL SECURITY; CREATE POLICY pol ON expr_cert_n USING (id % 2 = 0); GRANT SELECT ON expr_cert_n TO {role}")
            out2 = sess.run("EXECUTE pq('[3,3,0]')")
            check('S5c the same prepared statement after row-level security was enabled: planned again, answered', 'ERROR' not in out2 and len(id_list(out2)) == 5, out2[:200])
            sess.run(f"SET ROLE {role}")
            out3 = sess.run("EXECUTE pq('[3,3,0]')")
            rc, want3 = c.q(f"SET enable_indexscan = off; SELECT id FROM expr_cert_n ORDER BY (v + {zero})::vector(3) <-> '[3,3,0]' LIMIT 5", role=role)
            check('S5c ... and under another role: only the rows that role may see', 'ERROR' not in out3 and id_list(out3) == id_list(want3) and all(int(x) % 2 == 0 for x in id_list(out3)), out3[:200])
            sess.run("SET cuvs.hnsw_exact_scan = on")
            out4 = sess.run("EXECUTE pq('[3,3,0]')")
            sess.run("SET cuvs.hnsw_exact_scan = off")
            out5 = sess.run("EXECUTE pq('[3,3,0]')")
            check('S5c the setting turned on and off between executions: answered both times', 'ERROR' not in out4 + out5 and id_list(out4) == id_list(want3) and id_list(out5) == id_list(want3), (out4 + out5)[:200])
            sess.close()

            # a function that returns the table's rows hands out a vector without a dimension, so a statement that reads it spells the cast out
            ord2 = f"(v::vector + {zero})::vector(3)"
            # S5d: how often the remaining error is reached by statements under the default setting. A join, a subquery, an inlined function,
            # a partitioned table whose parent has the policy: none of them reaches it.
            c.scalar(f"CREATE TABLE ir(id int, v vector(3)); CREATE INDEX iri ON ir USING pg_cuvs_hnsw (({ord2}) vector_l2_ops); "
                     f"INSERT INTO ir SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 300) g; "
                     f"ALTER TABLE ir ENABLE ROW LEVEL SECURITY; CREATE POLICY pol ON ir USING (id % 2 = 0); GRANT SELECT ON ir TO {role}; "
                     f"CREATE FUNCTION rows_of() RETURNS TABLE(id int, v vector) LANGUAGE sql STABLE AS $$ SELECT id, v FROM ir $$; GRANT EXECUTE ON FUNCTION rows_of() TO {role}")
            c.scalar(f"CREATE TABLE other(id int); INSERT INTO other SELECT g FROM generate_series(1, 300) g; GRANT SELECT ON other TO {role}")
            ordexpr = f"(v + {zero})::vector(3)"
            c.scalar("CREATE TABLE pp(id int, v vector(3)) PARTITION BY RANGE (id); CREATE TABLE pp1 PARTITION OF pp FOR VALUES FROM (0) TO (150); CREATE TABLE pp2 PARTITION OF pp FOR VALUES FROM (150) TO (1000)")
            c.scalar(f"CREATE INDEX pp1i ON pp1 USING pg_cuvs_hnsw (({ord2}) vector_l2_ops); CREATE INDEX pp2i ON pp2 USING pg_cuvs_hnsw (({ord2}) vector_l2_ops)")
            c.scalar(f"INSERT INTO pp SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 300) g; ALTER TABLE pp ENABLE ROW LEVEL SECURITY; "
                     f"CREATE POLICY pol ON pp USING (id % 2 = 0); GRANT SELECT ON pp TO {role}; "
                     f"CREATE FUNCTION pp_rows() RETURNS TABLE(id int, v vector) LANGUAGE sql STABLE AS $$ SELECT id, v FROM pp $$; GRANT EXECUTE ON FUNCTION pp_rows() TO {role}")
            statements = {
                'a join': f"SELECT t.id FROM expr_cert_rls t JOIN other o ON o.id = t.id ORDER BY {ordexpr} <-> '[3,3,0]' LIMIT 20",
                'a subquery': f"SELECT id FROM expr_cert_rls WHERE id IN (SELECT id FROM other WHERE id < 250) ORDER BY {ordexpr} <-> '[3,3,0]' LIMIT 20",
                'an inlined function': f"SELECT id FROM rows_of() ORDER BY {ord2} <-> '[3,3,0]' LIMIT 20",
                'a union all': f"SELECT id FROM (SELECT id, v FROM expr_cert_rls UNION ALL SELECT id, v FROM pred_lost_rls) s ORDER BY {ordexpr} <-> '[3,3,0]' LIMIT 20",
                'a partitioned table, parent with the policy': f"SELECT id FROM pp ORDER BY {ord2} <-> '[3,3,0]' LIMIT 20",
                'a partitioned table through an inlined function': f"SELECT id FROM pp_rows() ORDER BY {ord2} <-> '[3,3,0]' LIMIT 20",
                'an inlined function over a security-barrier view': "SELECT id FROM ctl_bv_rows() LIMIT 100",
            }
            # controls: the same two shapes without row-level security do use the index, so that the two cases above are not about
            # statements that never reach it
            c.scalar(f"CREATE TABLE ctl(id int, v vector(3)); CREATE INDEX ctli ON ctl USING pg_cuvs_hnsw (({ord2}) vector_l2_ops); "
                     f"INSERT INTO ctl SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 300) g; "
                     f"CREATE FUNCTION ctl_rows() RETURNS TABLE(id int, v vector) LANGUAGE sql STABLE AS $$ SELECT id, v FROM ctl $$")
            rc, plan = c.q(f"SET enable_seqscan = off; EXPLAIN (COSTS OFF) SELECT id FROM ctl_rows() ORDER BY {ord2} <-> '[3,3,0]' LIMIT 20")
            check('S5d control: an inlined function over a table without row-level security reaches the expression index', 'Index Scan using ctli' in plan, plan)
            c.scalar("CREATE TABLE cp(id int, v vector(3)) PARTITION BY RANGE (id); CREATE TABLE cp1 PARTITION OF cp FOR VALUES FROM (0) TO (150); CREATE TABLE cp2 PARTITION OF cp FOR VALUES FROM (150) TO (1000)")
            c.scalar(f"CREATE INDEX cp1i ON cp1 USING pg_cuvs_hnsw (({ord2}) vector_l2_ops); CREATE INDEX cp2i ON cp2 USING pg_cuvs_hnsw (({ord2}) vector_l2_ops); "
                     f"INSERT INTO cp SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 300) g; "
                     f"CREATE FUNCTION cp_rows() RETURNS TABLE(id int, v vector) LANGUAGE sql STABLE AS $$ SELECT id, v FROM cp $$")
            rc, plan = c.q(f"SET enable_seqscan = off; EXPLAIN (COSTS OFF) SELECT id FROM cp_rows() ORDER BY {ord2} <-> '[3,3,0]' LIMIT 20")
            check('S5d control: a partitioned table without row-level security reaches the expression indexes of its partitions', 'cp1i' in plan and 'cp2i' in plan, plan)
            c.scalar(f"CREATE VIEW ctl_bv WITH (security_barrier = true) AS SELECT id, v FROM ctl WHERE id % 2 = 0 ORDER BY {ord2} <-> '[1,1,1]'::vector LIMIT 100; "
                     f"CREATE FUNCTION ctl_bv_rows() RETURNS TABLE(id int) LANGUAGE sql STABLE AS $$ SELECT id FROM ctl_bv $$; "
                     f"GRANT SELECT ON ctl_bv TO {role}; GRANT EXECUTE ON FUNCTION ctl_bv_rows() TO {role}")
            for label, sql in statements.items():
                if sql is None:
                    continue
                rc1, got = c.q(f"SET enable_seqscan = off; {sql}", role=role)
                rc2, want = c.q(f"SET enable_indexscan = off; {sql}", role=role)
                check(f'S5d {label} on a table with row-level security and an expression index: answered, as the exact ranking',
                      rc1 == 0 and rc2 == 0 and id_list(got) == id_list(want) and len(id_list(got)) == (100 if 'barrier' in label else 20) and all(int(x) % 2 == 0 for x in id_list(got)), got[:300])

            # S5e: a partition that has row-level security of its own under a parent that has none. A statement on the parent applies the policies of
            # the parent only, so the policy of the partition does not filter anything, and the statement is still one that the expression indexes
            # of the partitions must not serve: the planning of the partitions (and the check of the final range table) sees the flag of the
            # partition, not a look-ahead over the descendants of the parent at the start of every statement on it.
            c.scalar("CREATE TABLE cc(id int, v vector(3)) PARTITION BY RANGE (id); CREATE TABLE cc1 PARTITION OF cc FOR VALUES FROM (0) TO (150); CREATE TABLE cc2 PARTITION OF cc FOR VALUES FROM (150) TO (1000)")
            c.scalar(f"CREATE INDEX cc1i ON cc1 USING pg_cuvs_hnsw (({ord2}) vector_l2_ops); CREATE INDEX cc2i ON cc2 USING pg_cuvs_hnsw (({ord2}) vector_l2_ops); "
                     f"INSERT INTO cc SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 300) g; "
                     f"ALTER TABLE cc1 ENABLE ROW LEVEL SECURITY; CREATE POLICY pol ON cc1 USING (id % 2 = 0); GRANT SELECT ON cc TO {role}")
            sql = f"SELECT id FROM cc ORDER BY {ord2} <-> '[3,3,0]' LIMIT 20"
            rc, plan = c.q(f"SET enable_seqscan = off; EXPLAIN (COSTS OFF) {sql}", role=role)
            check('S5e a partition with row-level security under a parent without: the plan keeps neither expression index', rc == 0 and 'cc1i' not in plan and 'cc2i' not in plan, plan)
            rc1, got = c.q(f"SET enable_seqscan = off; {sql}", role=role)
            rc2, want = c.q(f"SET enable_indexscan = off; {sql}", role=role)
            check('S5e a partition with row-level security under a parent without: answered, as the exact ranking, with every row visible',
                  rc1 == 0 and rc2 == 0 and id_list(got) == id_list(want) and len(id_list(got)) == 20 and any(int(x) % 2 == 1 for x in id_list(got)), got[:300])

            # S7: the error that is left. A later planner hook (the test probe) puts the index path back after pg_cuvs removed it.
            include = subprocess.check_output(['pg_config', '--includedir-server'], text=True).strip()
            flags = __import__('shlex').split(subprocess.check_output(['pg_config', '--cflags'], text=True))
            probe = out / 'hnsw_plan_probe.so'
            r = subprocess.run(['cc', *flags, '-shared', '-fPIC', '-Wno-unused-parameter', '-Wno-declaration-after-statement', '-I' + include,
                                str(repo / 'test/unit/hnsw_plan_probe.c'), '-o', str(probe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            assert r.returncode == 0, r.stdout
            qe = "SELECT id FROM expr_cert_rls ORDER BY (v + '[0,0,0]'::vector)::vector(3) <-> '[3,3,0]' LIMIT 5"
            rc, res = c.q(f"LOAD '{probe}'; SET probe.readd = off; SET ROLE {role}; SET enable_seqscan = off; {qe}")
            check('S7 control: without the probe the statement is answered', rc == 0 and len(id_list(res)) == 5, res)
            rc, res = c.q(f"LOAD '{probe}'; SET probe.readd = on; SET ROLE {role}; SET enable_seqscan = off; {qe}")
            check('S7 a plan that keeps the index on a protected statement stops at the scan, with the cause and the action',
                  rc != 0 and 'cannot answer this statement' in res and 'row-level security' in res and 'cuvs.hnsw_exact_scan' in res, res)
            rc, res = c.q(f"LOAD '{probe}'; SET probe.readd = on; SET ROLE {role}; SET enable_seqscan = off; SET cuvs.hnsw_exact_scan = on; {qe}")
            check('S7 with cuvs.hnsw_exact_scan = on the same statement is answered by PostgreSQL', rc != 0 or len(id_list(res)) == 5, res)

            # S8: a partition or an inheritance child whose parent has the policy. The child shows no row-level security of its own, and
            # the function of an index expression says which rows it was called on (the first component of the vector is the id; the
            # policy shows the even ids).
            c.scalar("CREATE FUNCTION leak_expr(x vector) RETURNS vector LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN "
                     "IF current_setting('probe.active', true) = 'on' THEN RAISE NOTICE 'LEAK=%', x; END IF; RETURN x; END $$")
            c.scalar("CREATE FUNCTION toggle_exact(v vector) RETURNS vector LANGUAGE plpgsql VOLATILE STRICT AS $$ BEGIN "
                     "PERFORM set_config('cuvs.hnsw_exact_scan', 'on', true); RETURN v; END $$")
            lkey = '((leak_expr(v)::vector(3)))'
            lord = 'leak_expr(v)::vector(3)'

            def hidden_leaks(res):
                return [int(x) for x in re.findall(r'LEAK=\[(\d+),', res) if int(x) % 2]

            def under_parent(name, state, kind):
                t, parent = f'{name}_c', f'{name}_p'
                if state == 'cert':
                    c.scalar(f'CREATE TABLE {t}(id int, v vector(3))')
                    c.scalar(f'CREATE INDEX {t}i ON {t} USING pg_cuvs_hnsw ({lkey} vector_l2_ops)')
                    c.scalar(f'INSERT INTO {t} SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 300) g')
                else:
                    c.make_index(t, t + 'i', key=lkey)
                    if state == 'lost':
                        path = c.data / c.scalar(f"SELECT pg_relation_filepath('{t}i')")
                        c.stop()
                        rp = subprocess.run([str(plant), str(path), 'lost'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                        assert rp.returncode == 0, rp.stdout
                        c.start()
                if kind == 'part':
                    c.scalar(f'CREATE TABLE {parent}(id int, v vector(3)) PARTITION BY RANGE (id)')
                    c.scalar(f'ALTER TABLE {parent} ATTACH PARTITION {t} FOR VALUES FROM (1) TO (1000)')
                else:
                    c.scalar(f'CREATE TABLE {parent}(id int, v vector(3))')
                    c.scalar(f'ALTER TABLE {t} INHERIT {parent}')
                c.scalar(f'ALTER TABLE {parent} ENABLE ROW LEVEL SECURITY; CREATE POLICY pol ON {parent} USING (id % 2 = 0); GRANT SELECT ON {parent} TO {role}')
                return t, parent

            # S8a: the index has no descriptor (approximate scan, the expression is not evaluated), the setting is turned on while the
            # statement runs. The scan must not start evaluating the expression on the rows the policy hides.
            for kind in ('part', 'inh'):
                t, parent = under_parent('lk_' + kind, 'legacy', kind)
                lat = (f"SELECT q.i, t.id FROM (VALUES (1, '[0,0,0]'::vector), (2, toggle_exact('[1,0,0]'::vector))) q(i, v) "
                       f"CROSS JOIN LATERAL (SELECT id FROM {parent} ORDER BY {lord} <-> q.v LIMIT 1) t ORDER BY q.i")
                pre = f"SET ROLE {role}; SET enable_seqscan = off; SET enable_material = off;"
                rc, plan = c.q(f"{pre} EXPLAIN (COSTS OFF) {lat}")
                check(f'S8a {kind}: the statement is planned with the index of the child', f'{t}i' in plan, plan)
                rc, res = c.q(f"{pre} SET probe.active = 'on'; {lat}")
                check(f'S8a {kind}: turning the setting on while the statement runs stops it, with the reason', rc != 0 and 'cannot run exactly' in res, res[:300])
                check(f'S8a {kind}: the expression was not called on a row the policy hides', not hidden_leaks(res), str(hidden_leaks(res))[:200])

            # S8b: the index has a descriptor, the plan kept the index although planning removes it (the test probe puts the path back).
            # The scan stops with its own message, and no hidden row is touched.
            for kind in ('part', 'inh'):
                for state in ('cert', 'lost'):
                    t, parent = under_parent(f'lk_{kind}_{state}', state, kind)
                    q8 = f"SELECT id FROM {parent} ORDER BY {lord} <-> '[3,3,0]' LIMIT 5"
                    rc, res = c.q(f"LOAD '{probe}'; SET probe.readd = off; SET ROLE {role}; SET probe.active = 'on'; SET enable_seqscan = off; {q8}")
                    check(f'S8b {kind}, {state}: control, the statement is answered', rc == 0 and len(id_list(res)) == 5 and not hidden_leaks(res), res[:300])
                    rc, res = c.q(f"LOAD '{probe}'; SET probe.readd = on; SET ROLE {role}; SET probe.active = 'on'; SET enable_seqscan = off; {q8}")
                    check(f'S8b {kind}, {state}: a plan that kept the index stops at the scan', rc != 0 and 'cannot answer this statement' in res, res[:300])
                    check(f'S8b {kind}, {state}: the expression was not called on a row the policy hides', not hidden_leaks(res), str(hidden_leaks(res))[:200])

            # S8c: where the policy of a partitioned table is (the parent only, a partition only, both) against how the statement reaches it
            # (the table, a security-invoker view over it, pruned to one partition or not). The index of every partition has an expression, the
            # function of the expression says which rows it was called on. Every cell is answered as the exact ranking, the expression is not
            # called on a row that a policy hides, and where a policy applies or a partition has row-level security, planning keeps neither
            # index of the partitions. The control without any policy does reach the indexes, so that the cells are not about statements that never do.
            for place in ('none', 'parent', 'child', 'both'):
                par, pa, pb = f'sc_{place}', f'sc_{place}_a', f'sc_{place}_b'
                c.scalar(f'CREATE TABLE {par}(id int, v vector(3)) PARTITION BY RANGE (id); CREATE TABLE {pa} PARTITION OF {par} FOR VALUES FROM (0) TO (150); '
                         f'CREATE TABLE {pb} PARTITION OF {par} FOR VALUES FROM (150) TO (1000)')
                c.scalar(f'CREATE INDEX {pa}i ON {pa} USING pg_cuvs_hnsw ({lkey} vector_l2_ops); CREATE INDEX {pb}i ON {pb} USING pg_cuvs_hnsw ({lkey} vector_l2_ops)')
                c.scalar(f'INSERT INTO {par} SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 300) g')
                for tbl in {'none': [], 'parent': [par], 'child': [pa], 'both': [par, pa, pb]}[place]:
                    c.scalar(f'ALTER TABLE {tbl} ENABLE ROW LEVEL SECURITY; CREATE POLICY pol ON {tbl} USING (id % 2 = 0)')
                c.scalar(f'GRANT SELECT ON {par} TO {role}; CREATE VIEW {par}_v WITH (security_invoker = true) AS SELECT id, v FROM {par}; GRANT SELECT ON {par}_v TO {role}')
                shapes = {
                    'the table': f"SELECT id FROM {par} ORDER BY {lord} <-> '[3,3,0]' LIMIT 5",
                    'the table, pruned to one partition': f"SELECT id FROM {par} WHERE id < 100 ORDER BY {lord} <-> '[3,3,0]' LIMIT 5",
                    'a security-invoker view': f"SELECT id FROM {par}_v ORDER BY {lord} <-> '[3,3,0]' LIMIT 5",
                }
                applied = place in ('parent', 'both')
                for shape, q8 in shapes.items():
                    rc, plan = c.q(f"SET enable_seqscan = off; EXPLAIN (COSTS OFF) {q8}", role=role)
                    reaches = f'{pa}i' in plan or f'{pb}i' in plan
                    check(f'S8c policy on {place}, {shape}: planning {"keeps neither index" if place != "none" else "reaches an index of a partition (control)"}',
                          rc == 0 and (reaches == (place == 'none')), plan)
                    rc1, got = c.q(f"SET probe.active = 'on'; SET enable_seqscan = off; {q8}", role=role)
                    rc2, want = c.q(f"SET enable_indexscan = off; {q8}", role=role)
                    check(f'S8c policy on {place}, {shape}: answered, as the exact ranking', rc1 == 0 and rc2 == 0 and id_list(got) == id_list(want) and len(id_list(got)) == 5, got[:300])
                    if applied:
                        check(f'S8c policy on {place}, {shape}: the expression was not called on a row the policy hides', not hidden_leaks(got), str(hidden_leaks(got))[:200])

            # S8d: the four shapes S8c leaves out. A security-barrier view (not security-invoker) over a partitioned table, inheritance without
            # partitions, three levels of partitions with row-level security on the middle level only, and a prepared statement (generic plan
            # with run-time pruning, run more than five times) across the moment a policy appears or the role changes.
            def check_cell(label, par, parts, q8, applied, expect_index_free=True):
                rc, plan = c.q(f"SET enable_seqscan = off; EXPLAIN (COSTS OFF) {q8}", role=role)
                check(f'S8d {label}: planning keeps no index of the partitions or children', rc == 0 and not any(f'{x}i' in plan for x in parts), plan)
                rc1, got = c.q(f"SET probe.active = 'on'; SET enable_seqscan = off; {q8}", role=role)
                rc2, want = c.q(f"SET enable_indexscan = off; {q8}", role=role)
                check(f'S8d {label}: answered, as the exact ranking', rc1 == 0 and rc2 == 0 and id_list(got) == id_list(want) and len(id_list(got)) == 5, got[:300])
                if applied:
                    check(f'S8d {label}: the expression was not called on a row the policy or the view hides', not hidden_leaks(got), str(hidden_leaks(got))[:200])

            # a security-barrier view over a partitioned table (the view hides the odd ids, the expression is not leak-proof)
            c.scalar('CREATE TABLE sd_par(id int, v vector(3)) PARTITION BY RANGE (id); CREATE TABLE sd_a PARTITION OF sd_par FOR VALUES FROM (0) TO (150); CREATE TABLE sd_b PARTITION OF sd_par FOR VALUES FROM (150) TO (1000)')
            c.scalar(f'CREATE INDEX sd_ai ON sd_a USING pg_cuvs_hnsw ({lkey} vector_l2_ops); CREATE INDEX sd_bi ON sd_b USING pg_cuvs_hnsw ({lkey} vector_l2_ops)')
            c.scalar('INSERT INTO sd_par SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 300) g')
            c.scalar(f'CREATE VIEW sd_bv WITH (security_barrier = true) AS SELECT id, v FROM sd_par WHERE id % 2 = 0; GRANT SELECT ON sd_bv TO {role}')
            check_cell('security-barrier view over partitions', 'sd_par', ['sd_a', 'sd_b'], f"SELECT id FROM sd_bv ORDER BY {lord} <-> '[3,3,0]' LIMIT 5", True)

            # inheritance without partitions: policy on the parent, on a child, on both
            for place in ('parent', 'child', 'both'):
                par, ca, cb = f'si_{place}', f'si_{place}_a', f'si_{place}_b'
                c.scalar(f'CREATE TABLE {par}(id int, v vector(3)); CREATE TABLE {ca}() INHERITS ({par}); CREATE TABLE {cb}() INHERITS ({par})')
                c.scalar(f'CREATE INDEX {ca}i ON {ca} USING pg_cuvs_hnsw ({lkey} vector_l2_ops); CREATE INDEX {cb}i ON {cb} USING pg_cuvs_hnsw ({lkey} vector_l2_ops)')
                c.scalar(f'INSERT INTO {ca} SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 150) g; INSERT INTO {cb} SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(151, 300) g')
                for tbl in {'parent': [par], 'child': [ca], 'both': [par, ca, cb]}[place]:
                    c.scalar(f'ALTER TABLE {tbl} ENABLE ROW LEVEL SECURITY; CREATE POLICY pol ON {tbl} USING (id % 2 = 0)')
                c.scalar(f'GRANT SELECT ON {par} TO {role}')
                check_cell(f'inheritance, policy on {place}', par, [ca, cb], f"SELECT id FROM {par} ORDER BY {lord} <-> '[3,3,0]' LIMIT 5", place in ('parent', 'both'))

            # three levels, row-level security on the middle level only
            c.scalar('CREATE TABLE sm_top(id int, v vector(3)) PARTITION BY RANGE (id); CREATE TABLE sm_mid PARTITION OF sm_top FOR VALUES FROM (0) TO (1000) PARTITION BY RANGE (id); '
                     'CREATE TABLE sm_leaf1 PARTITION OF sm_mid FOR VALUES FROM (0) TO (150); CREATE TABLE sm_leaf2 PARTITION OF sm_mid FOR VALUES FROM (150) TO (1000)')
            c.scalar(f'CREATE INDEX sm_leaf1i ON sm_leaf1 USING pg_cuvs_hnsw ({lkey} vector_l2_ops); CREATE INDEX sm_leaf2i ON sm_leaf2 USING pg_cuvs_hnsw ({lkey} vector_l2_ops)')
            c.scalar('INSERT INTO sm_top SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 300) g')
            c.scalar(f'ALTER TABLE sm_mid ENABLE ROW LEVEL SECURITY; CREATE POLICY pol ON sm_mid USING (id % 2 = 0); GRANT SELECT ON sm_top TO {role}')
            check_cell('three levels, security on the middle level', 'sm_top', ['sm_leaf1', 'sm_leaf2'], f"SELECT id FROM sm_top ORDER BY {lord} <-> '[3,3,0]' LIMIT 5", False)

            # a prepared statement (generic plan, run-time pruning) across a new policy and a role change
            c.scalar('CREATE TABLE sg_par(id int, v vector(3)) PARTITION BY RANGE (id); CREATE TABLE sg_a PARTITION OF sg_par FOR VALUES FROM (0) TO (150); CREATE TABLE sg_b PARTITION OF sg_par FOR VALUES FROM (150) TO (1000)')
            c.scalar(f'CREATE INDEX sg_ai ON sg_a USING pg_cuvs_hnsw ({lkey} vector_l2_ops); CREATE INDEX sg_bi ON sg_b USING pg_cuvs_hnsw ({lkey} vector_l2_ops)')
            c.scalar(f'INSERT INTO sg_par SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 300) g; GRANT SELECT ON sg_par TO {role}')
            sg = dg.Session(c)
            try:
                sg.run("SET plan_cache_mode = force_generic_plan; SET enable_seqscan = off; SET probe.active = 'on'")
                sg.run(f"PREPARE sg_q(int) AS SELECT id FROM sg_par WHERE id < $1 ORDER BY {lord} <-> '[3,3,0]' LIMIT 5")
                outs = [sg.run(f"EXECUTE sg_q({100 + 40 * i})") for i in range(7)]
                check('S8d prepared, generic plan, seven runs without row-level security: answered', all('ERROR' not in x and len(id_list(x)) == 5 for x in outs), outs[-1][:200])
                c.scalar("ALTER TABLE sg_par ENABLE ROW LEVEL SECURITY; CREATE POLICY pol ON sg_par USING (id % 2 = 0)")
                sg.run(f"SET ROLE {role}")
                last = sg.run("EXECUTE sg_q(300)")
                check('S8d prepared statement after a policy appeared and the role changed: answered with the visible rows only, the expression not called on a hidden row',
                      'ERROR' not in last and len(id_list(last)) == 5 and all(int(x) % 2 == 0 for x in id_list(last)) and not hidden_leaks(last), last[:300])
                outs = [sg.run(f"EXECUTE sg_q({100 + 40 * i})") for i in range(7)]
                check('S8d the same statement run seven more times as the role: visible rows only', all('ERROR' not in r and all(int(v) % 2 == 0 for v in id_list(r)) and not hidden_leaks(r) for r in outs), outs[-1][:200])
            finally:
                sg.close()

            # S9: what the merge gives when the policy hides rows. The candidates are the graph's own answers (about ef_search of them) and
            # the ledger keys, so fewer than LIMIT rows come back, as with pgvector alone; the heap ranking (S5b) fills the LIMIT.
            c.scalar('CREATE TABLE sf(id int, v vector(8))')
            c.scalar("SELECT setseed(0.42); INSERT INTO sf SELECT x, (SELECT array_agg((((x % 25) * 7 + d * 3) % 41 + (random() - 0.5) * 0.05)::real) FROM generate_series(1, 8) d)::vector FROM generate_series(1, 4000) x")
            c.scalar('CREATE INDEX sfi ON sf USING hnsw (v vector_l2_ops) WITH (m = 4, ef_construction = 8)')
            c.scalar("UPDATE pg_class SET relam = (SELECT oid FROM pg_am WHERE amname = 'pg_cuvs_hnsw') WHERE relname = 'sfi'")
            c.scalar("UPDATE pg_depend SET refobjid = (SELECT oid FROM pg_opclass WHERE opcname = 'vector_l2_ops' AND opcmethod = (SELECT oid FROM pg_am WHERE amname = 'pg_cuvs_hnsw')) WHERE classid = 'pg_class'::regclass AND objid = 'sfi'::regclass AND refclassid = 'pg_opclass'::regclass")
            c.scalar("UPDATE pg_index SET indclass = (SELECT oid::text::oidvector FROM pg_opclass WHERE opcname = 'vector_l2_ops' AND opcmethod = (SELECT oid FROM pg_am WHERE amname = 'pg_cuvs_hnsw')) WHERE indexrelid = 'sfi'::regclass")
            path = c.data / c.scalar("SELECT pg_relation_filepath('sfi')")
            c.stop()
            rp = subprocess.run([str(plant), str(path), 'uncertified'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            assert rp.returncode == 0, rp.stdout
            c.start()
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('sfi')::text")
            st = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('sfi')::text"))
            res_unreached = re.search(r'"unreached": *(\d+)', res).group(1) if re.search(r'"unreached": *(\d+)', res) else '?'
            check('S9 the index is certified, and its ledger holds the rows the graph cannot reach', rc == 0 and st.get('class') == 'certified' and '"unreached": 0' not in res, f'{res} {st}')
            c.scalar(f'ALTER TABLE sf ENABLE ROW LEVEL SECURITY; CREATE POLICY pol ON sf USING (id % 20 = 0); GRANT SELECT ON sf TO {role}')
            rc, res = c.q("SET hnsw.ef_search = 40; SET enable_seqscan = off; SELECT id FROM sf ORDER BY v <-> '[1,2,3,4,5,6,7,8]' LIMIT 1000", role=role)
            got = id_list(res)
            # the policy shows 200 of the 4,000 rows; the candidates are the graph's answers and the ledger keys, not every row
            check('S9 merge mode with a policy that shows 1 row in 20: fewer rows than the 200 visible ones come back, all of them visible',
                  rc == 0 and len(got) < 200 and all(int(x) % 20 == 0 for x in got), f'{len(got)} rows ({res_unreached}): {res[:200]}')
            rc, res = c.q("SET cuvs.hnsw_exact_scan = on; SET enable_seqscan = off; SELECT id FROM sf ORDER BY v <-> '[1,2,3,4,5,6,7,8]' LIMIT 1000", role=role)
            check('S9 control: with cuvs.hnsw_exact_scan = on PostgreSQL ranks every visible row and fills the LIMIT with the 200 of them', rc == 0 and len(id_list(res)) == 200, str(len(id_list(res))))

            # S10: a predicate that calls a function the reading role may not execute. The module prepares the whole predicate (the executor
            # checks EXECUTE then), although the statement's own condition implies it through the other arm of the OR.
            c.scalar("CREATE FUNCTION secret_fn(i int) RETURNS boolean LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN RETURN i > 1000000; END $$")
            c.scalar('REVOKE EXECUTE ON FUNCTION secret_fn(int) FROM PUBLIC')
            c.scalar('CREATE TABLE sx(id int, v vector(3))')
            c.scalar('CREATE INDEX sxi ON sx USING pg_cuvs_hnsw (v vector_l2_ops) WHERE id > 5 OR secret_fn(id)')
            c.scalar(f"INSERT INTO sx SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 120) g; GRANT SELECT ON sx TO {role}")
            rc, plan = c.q("SET enable_seqscan = off; EXPLAIN (COSTS OFF) SELECT id FROM sx WHERE id > 7 ORDER BY v <-> '[3,3,0]' LIMIT 5", role=role)
            check('S10 a predicate with a function that is not built in: planning does not choose the index', 'Index Scan using sxi' not in plan, plan)
            rc, res = c.q("SET enable_seqscan = off; SELECT id FROM sx WHERE id > 7 ORDER BY v <-> '[3,3,0]' LIMIT 5", role=role)
            check('S10 ... and the role without EXECUTE on that function gets its rows (PostgreSQL scans the table)', rc == 0 and len(id_list(res)) == 5, res[:300])

            # S11: a predicate function that the statement does not call. The owner's partial index has WHERE evil(id) OR id > 5; a
            # statement with id > 7 implies it, so the index is chosen, and a function that only the owner chose must not run in the
            # reading session (here: as a role with the superuser attribute).
            c.scalar("CREATE ROLE admin_r SUPERUSER LOGIN")
            c.scalar("CREATE FUNCTION evil(i int) RETURNS boolean LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN "
                     "IF current_setting('probe.active', true) = 'on' THEN RAISE NOTICE 'EVIL_AS=%', current_user; END IF; RETURN false; END $$")
            for state in ('cert', 'lost'):
                t = f'ev_{state}'
                if state == 'cert':
                    c.scalar(f'CREATE TABLE {t}(id int, v vector(3))')
                    c.scalar(f'CREATE INDEX {t}i ON {t} USING pg_cuvs_hnsw (v vector_l2_ops) WHERE evil(id) OR id > 5')
                    c.scalar(f'INSERT INTO {t} SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 300) g')
                else:
                    c.make_index(t, t + 'i', where='evil(id) OR id > 5')
                    path = c.data / c.scalar(f"SELECT pg_relation_filepath('{t}i')")
                    c.stop()
                    rp = subprocess.run([str(plant), str(path), 'lost'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                    assert rp.returncode == 0, rp.stdout
                    c.start()
                q11 = f"SELECT id FROM {t} WHERE id > 7 ORDER BY v <-> '[3,3,0]' LIMIT 5"
                rc, plan = c.q(f"SET enable_seqscan = off; EXPLAIN (COSTS OFF) {q11}", role='admin_r')
                rc2, res = c.q(f"SET probe.active = 'on'; SET enable_seqscan = off; {q11}", role='admin_r')
                if f'Index Scan using {t}i' in plan:
                    check(f'S11 {state}: the owner\'s predicate function was not run in the reading session', 'EVIL_AS=' not in res, res[:200])
                else:
                    check(f'S11 {state}: the statement does not use the index (the answer is PostgreSQL\'s own)', rc2 == 0 and len(id_list(res)) == 5 and 'EVIL_AS=' not in res, plan + res[:200])

            # S11b: the plan kept such an index anyway (the test probe puts the path back). The exact module stops before it runs the predicate.
            for state in ('cert', 'lost'):
                t = f'ev_{state}'
                q11 = f"SELECT id FROM {t} WHERE id > 7 ORDER BY v <-> '[3,3,0]' LIMIT 5"
                rc, res = c.q(f"LOAD '{probe}'; SET probe.readd = on; SET ROLE admin_r; SET probe.active = 'on'; SET enable_seqscan = off; {q11}")
                check(f'S11b {state}: a plan that kept the index stops, and the predicate function does not run', rc != 0 and 'cannot answer this statement' in res and 'EVIL_AS=' not in res, res[:300])

            # S12: a scan that runs outside the executor hook's ExecutorRun. A data-modifying CTE that the main query does not read runs
            # to its end in ExecutorFinish; there the statement is not known to the hook.
            c.scalar(f'CREATE TABLE sink(x int); GRANT INSERT ON sink TO {role}')
            t, parent = under_parent('cte_part', 'legacy', 'part')
            cte = (f"WITH w AS (INSERT INTO sink SELECT t.id FROM (VALUES (1, '[0,0,0]'::vector), (2, toggle_exact('[1,0,0]'::vector))) q(i, v) "
                   f"CROSS JOIN LATERAL (SELECT id FROM {parent} ORDER BY {lord} <-> q.v LIMIT 1) t RETURNING 1) SELECT 1")
            pre = f"SET ROLE {role}; SET enable_seqscan = off; SET enable_material = off;"
            rc, plan = c.q(f"{pre} EXPLAIN (COSTS OFF) {cte}")
            check('S12 the data-modifying CTE is planned with the index of the child', f'{t}i' in plan, plan)
            rc, res = c.q(f"{pre} SET probe.active = 'on'; {cte}")
            check('S12 the expression was not called on a row the policy hides (data-modifying CTE)', not hidden_leaks(res), str(hidden_leaks(res))[:200])
            c.make_index('bvl_t', 'bvl_ti', key=lkey)
            c.scalar(f"CREATE VIEW bvl WITH (security_barrier = true) AS SELECT id, v FROM bvl_t WHERE id % 2 = 0 ORDER BY {lord} <-> '[0,0,0]'::vector; GRANT SELECT ON bvl TO {role}")
            latb = (f"SELECT q.i, t.id FROM (VALUES (1, 1), (2, toggle_exact('[1,0,0]'::vector) IS NOT NULL)::int)) q(i, z) "
                    f"CROSS JOIN LATERAL (SELECT id FROM bvl WHERE id >= q.i LIMIT 1) t ORDER BY q.i")
            latb = (f"SELECT q.i, t.id FROM (VALUES (1,'[0,0,0]'::vector), (2, toggle_exact('[1,0,0]'::vector))) q(i, v) "
                    f"CROSS JOIN LATERAL (SELECT id FROM bvl WHERE id >= q.i LIMIT 1) t ORDER BY q.i")
            rc, plan = c.q(f"{pre} EXPLAIN (COSTS OFF) {latb}")
            check('S12 the security-barrier view is planned with the expression index inside', 'bvl_ti' in plan, plan)
            rc, res = c.q(f"{pre} SET probe.active = 'on'; {latb}")
            check('S12 the expression was not called on a row the view hides (barrier view, setting turned on while it runs)', not hidden_leaks(res), str(hidden_leaks(res))[:200])
            ctb = (f"WITH w AS (INSERT INTO sink SELECT t.id FROM (VALUES (1,'[0,0,0]'::vector), (2, toggle_exact('[1,0,0]'::vector))) q(i, v) "
                   f"CROSS JOIN LATERAL (SELECT id FROM bvl WHERE id >= q.i LIMIT 1) t RETURNING 1) SELECT 1")
            rc, res = c.q(f"{pre} SET probe.active = 'on'; {ctb}")
            check('S12 the expression was not called on a row the view hides (barrier view inside a data-modifying CTE)', not hidden_leaks(res), str(hidden_leaks(res))[:200])

            # S13a: which predicates are built in. Each row: the predicate, whether planning may choose the index for a statement that has it.
            c.scalar("CREATE TYPE mood AS ENUM ('a', 'b')")
            c.scalar("CREATE TYPE pair AS (a int, b int)")
            c.scalar("CREATE FUNCTION chk(i int) RETURNS boolean LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN "
                     "IF current_setting('probe.active', true) = 'on' THEN RAISE NOTICE 'CHK_AS=%', current_user; END IF; RETURN i > 0; END $$")
            c.scalar("CREATE DOMAIN posint AS int CHECK (chk(VALUE))")
            c.scalar("CREATE FUNCTION plus_gt(a int, b int) RETURNS boolean LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN "
                     "IF current_setting('probe.active', true) = 'on' THEN RAISE NOTICE 'OP_AS=%', current_user; END IF; RETURN a + b > 3; END $$")
            c.scalar("CREATE OPERATOR <+> (LEFTARG = int, RIGHTARG = int, FUNCTION = plus_gt)")
            preds = [
                ('int comparison', 'id > 5', True),
                ('and, modulo', 'id > 5 AND id % 3 = 1', True),
                ('in list', 'id IN (7, 9, 11, 13)', True),
                ('is not null of the vector column', 'v IS NOT NULL AND id > 5', True),
                ('boolean expression with case', "(CASE WHEN id > 50 THEN true ELSE id > 7 END)", True),
                ('user function', 'chk(id)', False),
                ('user function in an OR', 'chk(id) OR id > 5', False),
                ('user domain cast', 'id::posint > 5', False),
                ('user operator', 'id <+> 5', False),
                ('enum column', "m = 'a'", False),
                ('field of a user composite column', '(p).a > 5', False),
                ('field of a ROW() (PostgreSQL simplifies it away)', '(ROW(id, id)::pair).a > 5', True),
                ('extension function (the vector distance)', "(v <-> '[0,0,0]'::vector) < 1000", False),
                ('GREATEST (MinMaxExpr)', 'GREATEST(id, 3) > 5', True),
                ('array constructor with an element type coercion (ArrayCoerceExpr)', 'ARRAY[id, 1]::bigint[] = ARRAY[5, 1]::bigint[]', True),
                ('cast through the text form (CoerceViaIO)', "id::text > '5'", True),
                ('null test of the whole row', '{t} IS NOT NULL AND id > 5', True),
            ]
            for k, (label, pred, builtin) in enumerate(preds):
                t = f'bp_{k}'
                pred = pred.replace('{t}', t)
                c.scalar(f"CREATE TABLE {t}(id int, v vector(3), m mood, p pair)")
                c.scalar(f"CREATE INDEX {t}i ON {t} USING pg_cuvs_hnsw (v vector_l2_ops) WHERE {pred}")
                c.scalar(f"INSERT INTO {t} SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector, 'a', ROW(g, g)::pair FROM generate_series(1, 120) g")
                q = f"SELECT id FROM {t} WHERE {pred} ORDER BY v <-> '[3,3,0]' LIMIT 5"
                rc, plan = c.q(f"SET enable_seqscan = off; EXPLAIN (COSTS OFF) {q}")
                used = f'Index Scan using {t}i' in plan
                rc, res = c.q(f"SET probe.active = 'on'; SET enable_seqscan = off; {q}", role='admin_r')
                if builtin:
                    check(f'S13a built in ({label}): planning may choose the index', used, plan)
                else:
                    check(f'S13a not built in ({label}): planning does not choose the index', not used, plan)
                    # the plan without the index runs the functions the statement wrote and nothing else: the same notices come with the index allowed
                    rc2, res2 = c.q(f"SET probe.active = 'on'; SET enable_indexscan = off; {q}", role='admin_r')
                    check(f'S13a not built in ({label}): no function ran that the plan without the index does not run', rc == 0 and rc2 == 0 and res.count('NOTICE') == res2.count('NOTICE'), f'{res.count("NOTICE")} {res2.count("NOTICE")}')

            # S13c: a function with constant arguments in the condition of an index. PostgreSQL itself folds it to a constant when the planner reads the
            # predicate of any index of the table (relcache.c, eval_const_expressions), as the role that plans, so the function runs at planning for a
            # btree partial index too: that is not something the pg_cuvs scan does, and the two index kinds behave the same way.
            c.scalar("CREATE TABLE fc_b(id int, v vector(3)); CREATE TABLE fc_h(id int, v vector(3)); "
                     "CREATE INDEX fc_bi ON fc_b (id) WHERE id > 5 AND chk(5); CREATE INDEX fc_hi ON fc_h USING pg_cuvs_hnsw (v vector_l2_ops) WHERE id > 5 AND chk(5); "
                     "INSERT INTO fc_b SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 120) g; INSERT INTO fc_h SELECT * FROM fc_b")
            ran = {}
            for t in ('fc_b', 'fc_h'):
                rc, res = c.q(f"SET probe.active = 'on'; EXPLAIN (COSTS OFF) SELECT id FROM {t} WHERE id > 5", role='admin_r')
                ran[t] = 'CHK_AS' in res
            check('S13c a function with constant arguments in the condition of a btree partial index runs at planning (PostgreSQL, not pg_cuvs)', ran['fc_b'], str(ran))
            check('S13c the same function in the condition of a pg_cuvs_hnsw index runs at planning exactly as often as for the btree index', ran['fc_h'] == ran['fc_b'], str(ran))

            # S13b: the expression of an index is the one the statement wrote
            c.scalar("CREATE FUNCTION fx(x vector) RETURNS vector LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN "
                     "IF current_setting('probe.active', true) = 'on' THEN RAISE NOTICE 'FX_AS=%', current_user; END IF; RETURN x; END $$")
            c.scalar("CREATE FUNCTION wrap_fx(x vector) RETURNS vector LANGUAGE sql IMMUTABLE AS $$ SELECT fx(x) $$")
            c.scalar("CREATE FUNCTION other_fx(x vector) RETURNS vector LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN "
                     "IF current_setting('probe.active', true) = 'on' THEN RAISE NOTICE 'OTHER_AS=%', current_user; END IF; RETURN x; END $$")
            for tag, key in (('direct', 'fx(v)::vector(3)'), ('wrapped', 'wrap_fx(v)::vector(3)')):
                t = f'xe_{tag}'
                c.scalar(f'CREATE TABLE {t}(id int, v vector(3))')
                c.scalar(f'CREATE INDEX {t}i ON {t} USING pg_cuvs_hnsw (({key}) vector_l2_ops)')
                c.scalar(f'INSERT INTO {t} SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 120) g')
                for qtag, order, expect_used, ran in (('the function of the index', 'fx(v)::vector(3)', True, 'FX_AS='),
                                                       ('the wrapper of the index', 'wrap_fx(v)::vector(3)', True, 'FX_AS='),
                                                       ('another function', 'other_fx(v)::vector(3)', False, 'OTHER_AS='),
                                                       ('the function plus something', '(fx(v) + \'[0,0,0]\'::vector)::vector(3)', False, 'FX_AS=')):
                    q = f"SELECT id FROM {t} ORDER BY {order} <-> '[3,3,0]' LIMIT 5"
                    rc, plan = c.q(f"SET enable_seqscan = off; EXPLAIN (COSTS OFF) {q}")
                    used = f'Index Scan using {t}i' in plan
                    rc, res = c.q(f"SET probe.active = 'on'; SET enable_seqscan = off; {q}", role='admin_r')
                    check(f'S13b index on {tag}, statement orders by {qtag}: the index is {"chosen" if expect_used else "not chosen"}', used == expect_used, plan)
                    ran_tags = set(re.findall(r'([A-Z]+)_AS=', res))
                    written = {'the function of the index': {'FX'}, 'the wrapper of the index': {'FX'}, 'another function': {'OTHER'}, 'the function plus something': {'FX'}}[qtag]
                    check(f'S13b index on {tag}, statement orders by {qtag}: only functions that the statement wrote ran', rc == 0 and ran_tags <= written, f'{ran_tags} {res[:150]}')

            # S14: the same statement shapes inside other contexts. The ordinary ones must be answered (a scan whose statement is not known stops;
            # none of these may reach that); the leak ones must not touch a hidden row.
            c.scalar("CREATE FUNCTION in_fn() RETURNS SETOF int LANGUAGE plpgsql AS $$ BEGIN RETURN QUERY SELECT id FROM ctl ORDER BY " + ord2 + " <-> '[3,3,0]' LIMIT 5; END $$")
            qo = f"SELECT id FROM ctl ORDER BY {ord2} <-> '[3,3,0]' LIMIT 5"
            rc, want = c.q(f"SET enable_indexscan = off; {qo}")
            want = id_list(want)
            c.scalar("CREATE TABLE trg_t(x int); CREATE TABLE trg_out(x int)")
            c.scalar("CREATE FUNCTION trg_fn() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN INSERT INTO trg_out SELECT id FROM ctl ORDER BY " + ord2 + " <-> '[3,3,0]' LIMIT 5; RETURN NEW; END $$")
            c.scalar("CREATE TRIGGER trg AFTER INSERT ON trg_t FOR EACH ROW EXECUTE FUNCTION trg_fn()")
            contexts = {
                'a function (RETURN QUERY)': ("SET enable_seqscan = off; SELECT * FROM in_fn()", None),
                'a cursor': (f"SET enable_seqscan = off; BEGIN; DECLARE cur CURSOR FOR {qo}; FETCH ALL FROM cur; COMMIT", None),
                'COPY (query)': (f"SET enable_seqscan = off; COPY ({qo}) TO STDOUT", None),
                'EXPLAIN ANALYZE': (f"SET enable_seqscan = off; EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) {qo}", 'Index Scan using ctli'),
                'a parallel plan (debug_parallel_query)': (f"SET enable_seqscan = off; SET debug_parallel_query = on; {qo}", None),
                'a data-modifying CTE that the main query reads': (f"SET enable_seqscan = off; WITH w AS (INSERT INTO trg_out SELECT id FROM ctl ORDER BY {ord2} <-> '[3,3,0]' LIMIT 5 RETURNING x) SELECT x FROM w", None),
                'a data-modifying CTE that the main query does not read': (f"SET enable_seqscan = off; WITH w AS (INSERT INTO trg_out SELECT id FROM ctl ORDER BY {ord2} <-> '[3,3,0]' LIMIT 5 RETURNING x) SELECT 1", 'MARK'),
            }
            for label, (sql, expect_text) in contexts.items():
                rc, res = c.q(sql)
                ok = rc == 0 and 'cannot' not in res and 'not known' not in res
                if expect_text:
                    ok = ok and (expect_text in res or expect_text == 'MARK')
                else:
                    ok = ok and sorted(id_list(res)) == sorted(want)
                check(f'S14 {label}: the statement is answered', ok, res[:300])
            rc, cnt = c.q("SELECT count(*) FROM trg_out")
            check('S14 the data-modifying CTE that the main query does not read ran its scan to its end (rows were inserted)', rc == 0 and int(cnt) >= 10, cnt)
            rc, res = c.q("INSERT INTO trg_t VALUES (1)")
            rc2, trg_rows = c.q("SELECT x FROM trg_out ORDER BY x")
            check('S14 a trigger that runs a statement with the index: answered', rc == 0 and 'cannot' not in res and len(id_list(trg_rows)) >= 5, (res + trg_rows)[:300])
            c.scalar("CREATE FUNCTION toggle_spi(v vector) RETURNS vector LANGUAGE plpgsql VOLATILE STRICT AS $$ BEGIN "
                     "PERFORM count(*) FROM other; PERFORM set_config('cuvs.hnsw_exact_scan', 'on', true); RETURN v; END $$")
            t, parent = 'lk_part_c', 'lk_part_p'
            lat_t = (f"SELECT q.i, t.id FROM (VALUES (1, '[0,0,0]'::vector), (2, {{tog}}('[1,0,0]'::vector))) q(i, v) "
                     f"CROSS JOIN LATERAL (SELECT id FROM {parent} ORDER BY {lord} <-> q.v LIMIT 1) t ORDER BY q.i")
            pre = f"SET ROLE {role}; SET enable_seqscan = off; SET enable_material = off; SET probe.active = 'on';"
            c.scalar(f"GRANT SELECT ON other TO {role}")
            c.scalar("CREATE FUNCTION lk_fn() RETURNS int LANGUAGE plpgsql AS $$ DECLARE n int; BEGIN SELECT count(*) INTO n FROM (" + lat_t.format(tog='toggle_exact') + ") s; RETURN n; END $$")
            c.scalar(f"GRANT EXECUTE ON FUNCTION lk_fn() TO {role}")
            for label, sql in {
                'inside a function': f"{pre} SELECT lk_fn()",
                'a cursor': f"{pre} BEGIN; DECLARE cur CURSOR FOR {lat_t.format(tog='toggle_exact')}; FETCH ALL FROM cur; COMMIT",
                'the toggle runs a nested statement first': f"{pre} {lat_t.format(tog='toggle_spi')}",
                'COPY (query)': f"{pre} COPY ({lat_t.format(tog='toggle_exact')}) TO STDOUT",
            }.items():
                rc, res = c.q(sql)
                check(f'S14 hidden rows, {label}: the expression was not called on a row the policy hides', not hidden_leaks(res), str(hidden_leaks(res))[:200])

            # S15: the merge of a certified partial index after updates and deletes that move rows across the predicate equals the exact ranking
            c.scalar('CREATE TABLE mp(id int, v vector(3))')
            c.scalar('CREATE INDEX mpi ON mp USING pg_cuvs_hnsw (v vector_l2_ops) WHERE id > 5')
            c.scalar("INSERT INTO mp SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 400) g")
            c.scalar("UPDATE mp SET id = id - 1000 WHERE id % 7 = 0; DELETE FROM mp WHERE id % 11 = 0; UPDATE mp SET id = id + 1000 WHERE id < 0 AND id % 2 = 0")
            bad = 0
            for _ in range(15):
                qv = vec(rng, 3)
                sqlm = f"SELECT id FROM mp WHERE id > 5 ORDER BY v <-> '{qv}', id LIMIT 60"
                rc1, got = c.q(f"SET enable_seqscan = off; {sqlm}")
                rc2, want = c.q(f"SET enable_indexscan = off; {sqlm}")
                if rc1 != 0 or id_list(got) != id_list(want) or not id_list(want):
                    bad += 1
            check('S15 a certified partial index after updates and deletes: 15 ordered scans equal the exact ranking', bad == 0, f'{bad} differ')
            # S17: cuvs.cover_verify_inserts (experimental, off by default). A row that a search for its own vector finds right after pgvector
            # inserted it was reachable from the entry point then and gets no ledger key; a row the search does not find gets one, as without
            # the setting.
            def ledger_keys(index):
                return int(json.loads(c.scalar(f"SELECT pg_cuvs_hnsw_cover_status('{index}')::text")).get('ledger_keys', -1))

            c.scalar("CREATE FUNCTION pg_cuvs_cover_probe_unreached(regclass) RETURNS tid[] LANGUAGE c AS '$libdir/pg_cuvs', 'pg_cuvs_cover_probe_unreached'")
            c.scalar("CREATE FUNCTION pg_cuvs_cover_probe_keys(regclass) RETURNS tid[] LANGUAGE c AS '$libdir/pg_cuvs', 'pg_cuvs_cover_probe_keys'")
            c.scalar("CREATE FUNCTION pg_cuvs_cover_search_probe(regclass, vector, tid) RETURNS boolean LANGUAGE c AS '$libdir/pg_cuvs', 'pg_cuvs_cover_search_probe'")

            def unkeyed_unreached(index):
                # rows the graph does not reach that hold no key: the rows the guarantee does not cover
                return int(c.scalar(f"SELECT count(*) FROM unnest(pg_cuvs_cover_probe_unreached('{index}'::regclass)) u "
                                    f"WHERE u <> ALL (pg_cuvs_cover_probe_keys('{index}'::regclass))"))

            for tname, on in (('vf_off', False), ('vf_srch', True)):
                c.scalar(f'CREATE TABLE {tname}(id int, v vector(8))')
                c.scalar(f'CREATE INDEX {tname}_i ON {tname} USING pg_cuvs_hnsw (v vector_l2_ops)')
                setting = f"SET cuvs.cover_verify_inserts = {'on' if on else 'off'};"
                rc, res = c.q(f"{setting} SELECT setseed(0.31); INSERT INTO {tname} SELECT g, (SELECT array_agg((random() * 10)::real) FROM generate_series(1, 8) d WHERE g > 0)::vector FROM generate_series(1, 1500) g")
                keys = ledger_keys(f'{tname}_i')
                if on:
                    check('S17 verification on: most of 1,500 inserted rows get no ledger key', rc == 0 and keys <= 150, f'{keys} keys {res[:120]}')
                    check('S17 verification on: every row is reached by the graph or holds a key', unkeyed_unreached(f'{tname}_i') == 0, str(unkeyed_unreached(f'{tname}_i')))
                else:
                    check('S17 verification off: every inserted row gets a ledger key', rc == 0 and keys == 1500, f'{keys} keys {res[:120]}')
            # the search itself (a test function): a row the graph reaches is found, a heap TID the index does not hold is not
            rc, res = c.q("SELECT count(*) FILTER (WHERE pg_cuvs_cover_search_probe('vf_srch_i'::regclass, v, ctid)), count(*) FROM vf_srch")
            found_all, total = [int(x) for x in res.split('|')]
            check('S17 the search finds most rows of the index with their own vector', rc == 0 and found_all >= int(total * 0.9), res)
            rc, res = c.q("SELECT pg_cuvs_cover_search_probe('vf_srch_i'::regclass, v, '(999999,1)'::tid) FROM vf_srch LIMIT 1")
            check('S17 a heap TID that the index does not hold is not found', rc == 0 and res == 'f', res)

            # S17 the two guards of the search that no SQL statement can reach, through test functions: without an active snapshot the search answers
            # "not found" (and does not crash the backend), and the search leaves the memory of its caller where it was
            c.scalar("CREATE FUNCTION pg_cuvs_cover_search_nosnapshot_probe(regclass, vector, tid) RETURNS boolean LANGUAGE c AS '$libdir/pg_cuvs', 'pg_cuvs_cover_search_nosnapshot_probe'")
            c.scalar("CREATE FUNCTION pg_cuvs_cover_search_memory_probe(regclass, vector, tid, int) RETURNS bigint LANGUAGE c AS '$libdir/pg_cuvs', 'pg_cuvs_cover_search_memory_probe'")
            rc, res = c.q("SELECT pg_cuvs_cover_search_nosnapshot_probe('vf_srch_i'::regclass, v, ctid) FROM vf_srch WHERE id = 7")
            check('S17 without an active snapshot the search answers "not found" and the backend lives', rc == 0 and res == 'f', res)
            rc, res = c.q("SELECT pg_cuvs_cover_search_memory_probe('vf_srch_i'::regclass, v, ctid, 2000) FROM vf_srch WHERE id = 7")
            check('S17 2,000 searches in a row leave the memory of the caller within 64 KB of where it was', rc == 0 and int(res) < 65536, res)

            # S17 duplicates: more than 200 rows with the same vector (distance 0): the search reads at most 200 rows, so some rows are not found and
            # get a key; none may end up neither reached nor keyed
            c.scalar('CREATE TABLE vf_dup(id int, v vector(3))')
            c.scalar('CREATE INDEX vf_dup_i ON vf_dup USING pg_cuvs_hnsw (v vector_l2_ops)')
            rc, res = c.q("SET cuvs.cover_verify_inserts = on; INSERT INTO vf_dup SELECT g, '[1,2,3]' FROM generate_series(1, 450) g")
            check('S17 450 identical vectors: the insert succeeds and no row is left neither reached nor keyed', rc == 0 and unkeyed_unreached('vf_dup_i') == 0, f'{unkeyed_unreached("vf_dup_i")} {res[:100]}')

            # S17 growth monitor with an insert that adds two pages (a vector too large for an element and its neighbor tuple to share a page): each
            # verified insert is an event of the ledger like a key, so the relation size it records follows the growth, and a key that is written
            # after hundreds of such inserts does not find "more than 256 pages since the last key"
            c.scalar('CREATE TABLE vf_big(id int, v vector(1900))')
            c.scalar('CREATE INDEX vf_big_i ON vf_big USING pg_cuvs_hnsw (v vector_l2_ops)')
            rc, res = c.q("SET cuvs.cover_verify_inserts = on; SELECT setseed(0.5); INSERT INTO vf_big SELECT g, (SELECT array_agg(random()::real) FROM generate_series(1, 1900) d WHERE g > 0)::vector FROM generate_series(1, 300) g")
            rc2, res2 = c.q("SET cuvs.cover_verify_inserts = off; INSERT INTO vf_big SELECT 1000, (SELECT array_agg(random()::real) FROM generate_series(1, 1900) d)::vector")
            st = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('vf_big_i')::text"))
            pages_now = int(c.scalar("SELECT pg_relation_size('vf_big_i') / 8192"))
            check('S17 300 verified inserts of a high-dimensional vector (more than 256 pages of growth), then a key: the index is still certified',
                  rc == 0 and rc2 == 0 and st.get('class') == 'certified' and pages_now > 256, f"{st.get('class')} {pages_now} pages {res[:80]} {res2[:80]}")

            # Cut-off-row case: an element C whose incoming edges were all cut by later inserts and a row E that only C points to.
            # E is not reachable, and neither is C. The cut is made in the index file (graph_cut.py)
            # while the cluster is stopped. The search does not find E (it is not reached from the entry point); the exposure that remains is the
            # one the guarantee names: rows cut off after their check are outside it until the next recertification, which keys them.
            c.make_index('nc', 'nci')
            ncpath = c.data / c.scalar("SELECT pg_relation_filepath('nci')")
            plant_mode(ncpath, 'uncertified')
            c.scalar("SELECT pg_cuvs_cover_recertify('nci')")
            keys0 = ledger_keys('nci')
            unreached0 = c.scalar("SELECT cardinality(pg_cuvs_cover_probe_unreached('nci'::regclass))")
            check('S17 cut-off rows setup: the certified index holds exactly the unreached rows as keys', str(keys0) == unreached0, f'{keys0} keys, {unreached0} unreached')
            flaw = None
            for cid, eid in ((11, 12), (21, 22), (31, 32), (41, 42)):
                cb, co = [int(x) for x in c.scalar(f"SELECT replace(replace(ctid::text, '(', ''), ')', '') FROM nc WHERE id = {cid}").split(',')]
                eb, eo = [int(x) for x in c.scalar(f"SELECT replace(replace(ctid::text, '(', ''), ')', '') FROM nc WHERE id = {eid}").split(',')]
                c.stop()
                rc = subprocess.run([sys.executable, str(repo / 'test/unit/graph_cut.py'), str(ncpath), str(cb), str(co), str(eb), str(eo)],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                assert rc.returncode == 0, rc.stdout
                c.start()
                both = c.scalar(f"SELECT count(*) FROM unnest(pg_cuvs_cover_probe_unreached('nci'::regclass)) u WHERE u IN ((SELECT ctid FROM nc WHERE id = {cid}), (SELECT ctid FROM nc WHERE id = {eid}))")
                if both == '2':
                    flaw = (cid, eid)
                    break
            check('S17 cut-off rows: C and E are unreachable after the cut', flaw is not None, str(flaw))
            if flaw is not None:
                e_found = c.scalar(f"SELECT pg_cuvs_cover_search_probe('nci'::regclass, v, ctid) FROM nc WHERE id = {flaw[1]}")
                ok_found = c.scalar("SELECT pg_cuvs_cover_search_probe('nci'::regclass, v, ctid) FROM nc WHERE id = 100")
                check('S17 cut-off rows: the search does not find E, which the entry point does not lead to', e_found == 'f', e_found)
                check('S17 the search finds a row the graph reaches', ok_found == 't', ok_found)
                # the size of the exposure, deterministically: both rows are outside the ledger now, and a recertification keys them
                gap = unkeyed_unreached('nci')
                c.scalar("SELECT pg_cuvs_cover_recertify('nci')")
                check('S17 cut-off rows: the two cut-off rows are outside the ledger until the next recertification, which keys them',
                      gap >= 2 and unkeyed_unreached('nci') == 0, f'{gap} before, {unkeyed_unreached("nci")} after')

            # S16: the backend remembers that the database has no index of the access method with an expression or a predicate, and passes
            # statements through the planner hooks untouched. Another session (or the backend itself, in the open transaction) creates such an
            # index on a table with row-level security: the next statement must be planned without it, as it is when the hooks run.
            c2 = dg.Cluster(o, 'sc2', out, args.port + 1)
            try:
                c2.start()
                c2.scalar('CREATE EXTENSION vector')
                c2.scalar('CREATE EXTENSION pg_cuvs')
                c2.scalar("CREATE ROLE inv_r LOGIN")
                ordx = "(v + '[0,0,0]'::vector)::vector(3)"
                fill = "INSERT INTO {t} SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 300) g"
                for t in ('inv_a', 'inv_b', 'inv_c', 'inv_p'):
                    # an index of the access method is built on an empty table here (the CPU build of the test library has no table to read), the rows come after it
                    c2.scalar(f"CREATE TABLE {t}(id int, v vector(3)); GRANT SELECT ON {t} TO inv_r")
                    if t != 'inv_p':
                        c2.scalar(f"ALTER TABLE {t} ENABLE ROW LEVEL SECURITY; CREATE POLICY pol ON {t} USING (id % 2 = 0)")
                c2.scalar(fill.format(t='inv_c'))
                sa = dg.Session(c2, role='inv_r')
                try:
                    def explain(table):
                        return sa.run(f"SET enable_seqscan = off; EXPLAIN (COSTS OFF) SELECT id FROM {table} ORDER BY {ordx} <-> '[3,3,0]' LIMIT 5")
                    check('S16 before any such index: the plan on a table with row-level security is the sequential one', 'Index Scan' not in explain('inv_a'), explain('inv_a'))
                    # another session creates the index
                    c2.scalar(f"CREATE INDEX inv_ai ON inv_a USING pg_cuvs_hnsw (({ordx}) vector_l2_ops)")
                    c2.scalar(fill.format(t='inv_a'))
                    plan = explain('inv_a')
                    check('S16 an index created by another session is not used for a statement on a table with row-level security', 'inv_ai' not in plan, plan)
                    # a plain table does reach the index of the same kind: the hooks were not what kept it away
                    c2.scalar(f"CREATE INDEX inv_pi ON inv_p USING pg_cuvs_hnsw (({ordx}) vector_l2_ops)")
                    c2.scalar(fill.format(t='inv_p'))
                    plan = explain('inv_p')
                    check('S16 control: the same kind of index on a table without row-level security is used', 'inv_pi' in plan, plan)
                    # every such index is dropped: the answer is false again, then a new one is created in the open transaction of the backend itself
                    c2.scalar('DROP INDEX inv_ai; DROP INDEX inv_pi')
                    check('S16 after every such index is dropped: still the sequential plan', 'Index Scan' not in explain('inv_a'), explain('inv_a'))
                    # the backend itself creates the index in an open transaction (as the owner), then reads the table as the role in the same transaction
                    sb = dg.Session(c2)
                    try:
                        sb.run('BEGIN')
                        sb.run(f"CREATE INDEX inv_bi ON inv_b USING pg_cuvs_hnsw (({ordx}) vector_l2_ops)")
                        sb.run(fill.format(t='inv_b'))
                        sb.run('SET ROLE inv_r')
                        sb.run("SET enable_seqscan = off")
                        plan = sb.run(f"EXPLAIN (COSTS OFF) SELECT id FROM inv_b ORDER BY {ordx} <-> '[3,3,0]' LIMIT 5")
                        sb.run('ROLLBACK')
                    finally:
                        sb.close()
                    check('S16 an index created by the backend itself in the open transaction is not used on a table with row-level security', 'inv_bi' not in plan, plan)
                    # a plan cached before the index exists, the index created by another session, the plan used again
                    sa.run("SET plan_cache_mode = force_generic_plan")
                    sa.run(f"PREPARE inv_q AS SELECT id FROM inv_c ORDER BY {ordx} <-> '[3,3,0]' LIMIT 5")
                    first = sa.run("EXECUTE inv_q")
                    c2.scalar(f"TRUNCATE inv_c; CREATE INDEX inv_ci ON inv_c USING pg_cuvs_hnsw (({ordx}) vector_l2_ops)")
                    c2.scalar(fill.format(t='inv_c'))
                    again = sa.run("EXECUTE inv_q")
                    check('S16 a prepared statement planned before the index existed is answered after it exists, with the same rows',
                          'ERROR' not in again and again == first and len([x for x in again.split("\n") if x.isdigit()]) == 5, again[:200])
                    # the same from other ways in which an index appears or the invalidations are lost; each starts from a database without such an
                    # index (the backend looks, and remembers that there is none), then the index appears, then the same statement
                    c2.scalar('DROP INDEX IF EXISTS inv_ci')
                    for t in ('inv_d', 'inv_e', 'inv_f', 'inv_g'):
                        c2.scalar(f"CREATE TABLE {t}(id int, v vector(3)); GRANT SELECT ON {t} TO inv_r; ALTER TABLE {t} ENABLE ROW LEVEL SECURITY; CREATE POLICY pol ON {t} USING (id % 2 = 0)")

                    def appears(label, table, action):
                        before = explain(table)
                        action()
                        c2.scalar(fill.format(t=table))
                        after = explain(table)
                        check(f'S16 {label}: not used on a table with row-level security (and the plan before it was the sequential one)', 'Index Scan' not in before and 'Index Scan' not in after, f'{before} || {after}')
                    appears('an index created CONCURRENTLY by another session', 'inv_d', lambda: c2.scalar(f"CREATE INDEX CONCURRENTLY inv_di ON inv_d USING pg_cuvs_hnsw (({ordx}) vector_l2_ops)"))
                    sb2 = dg.Session(c2)
                    try:
                        def rolled_back_then_real():
                            sb2.run('BEGIN')
                            sb2.run(f"CREATE INDEX inv_ei0 ON inv_e USING pg_cuvs_hnsw (({ordx}) vector_l2_ops)")
                            sb2.run('ROLLBACK')
                            check('S16 after a rolled-back creation the plan is still the sequential one', 'Index Scan' not in explain('inv_e'), explain('inv_e'))
                            c2.scalar(f"CREATE INDEX inv_ei ON inv_e USING pg_cuvs_hnsw (({ordx}) vector_l2_ops)")
                        appears('an index created after another one was created and rolled back', 'inv_e', rolled_back_then_real)
                    finally:
                        sb2.close()

                    def overflow():
                        # the queue of invalidation messages holds 4096: a burst of catalog changes while this backend is idle makes it reset its caches instead
                        c2.scalar(f"CREATE INDEX inv_fi ON inv_f USING pg_cuvs_hnsw (({ordx}) vector_l2_ops)")
                        for _ in range(8):
                            c2.scalar("DO $$ BEGIN FOR i IN 1..400 LOOP EXECUTE 'CREATE TEMP TABLE ov_' || i || '(a int primary key, b text)'; END LOOP; END $$")
                    appears('an index whose message was lost in an overflow of the invalidation queue', 'inv_f', overflow)

                    def new_extension():
                        c2.scalar('DROP EXTENSION pg_cuvs CASCADE')
                        c2.scalar('CREATE EXTENSION pg_cuvs')
                        c2.scalar(f"CREATE INDEX inv_gi ON inv_g USING pg_cuvs_hnsw (({ordx}) vector_l2_ops)")
                    appears('an index of the access method of an extension that was dropped and created again', 'inv_g', new_extension)
                finally:
                    sa.close()
            finally:
                c2.drop()
        finally:
            c.drop()
    finally:
        (out / 'transcript.txt').write_text('\n'.join(dg.TRANSCRIPT) + '\n')
        shutil.rmtree(root, ignore_errors=True)
        for d in out.glob('data-*'):
            shutil.rmtree(d, ignore_errors=True)
    result = {'failures': dg.FAILURES, 'verdict': 'FAIL' if dg.FAILURES else 'PASS',
              'not_run': 'merge of ANN answers with ledger keys for rows outside the ledger (needs a certified index with base keys: a certification or a GPU build)'}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 1 if dg.FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
