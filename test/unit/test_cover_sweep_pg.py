#!/usr/bin/env python3
"""The time-based recertification (cuvs.cover_recertify_max_age): rows that inserts cut off from the graph must not stay outside the ledger for good
when the inserts stop below the threshold (CPU only). The cluster runs with cuvs.cover_recertify_max_age = 15 s and a threshold of 50 percent, so that nothing
but the time trigger can act on a few inserts.

  T1 a few inserts (1 percent of the rows) and then nothing: the status schedules a recertification (recertify_scheduled_at), and within max_age plus one
     sweep period (10 s) plus the work the index is certified again with no key since;
  T2 the same after a server restart (the request table is gone: the sweeper finds the index again), in a database that no session connected to after the
     restart before the check;
  T3 an index with no key since its certification is never walked (no log line of a recertification, same generation);
  T4 on a graph with many unreachable rows and a high exposure (pgvector hnsw with m = 4, switched to pg_cuvs_hnsw), inserts of 45 percent of the rows
     cut old rows off the graph; after the time-based recertification every row the graph cannot reach has a key (independent oracle) and each row that
     was missing before is found by a search with its own vector;
  T5 sixty indexes in one database with a few inserts each: every one is recertified, never more than cuvs.cover_recertify_max_workers (2) recertification
     workers at once, and the sweep of the database is quick;
  T8 (--gate) a worker that ends with an error (cancelled while it waits at the start of its walk) is not started again at every sweep: one
     start in 40 s (four sweeps), a log line that says when the next one comes, and a start again at the first sweep after the index grew;
  T9 the candidates of a database that was dropped, or that stopped accepting connections, leave the table: no sweep worker tries to
     connect to it in the next three sweeps;
  T10 (--gate) an insert that notes a candidate while a sweep has judged it and not yet acted keeps it in the table: the time rule
     recertifies the index within the bound;
  T11 with cuvs.cover_recertify_max_age = 0 nothing is scheduled by time, but the sweep still recertifies an index past the threshold;
  T12 verified inserts that write no key and add no page mark the descriptor once and make the index due by time; a
     recertification clears the mark and the next insert sets it again;
  T13 (--gate) while a VACUUM runs the search merges and the sweeper starts no worker for the index; after the VACUUM is
     cancelled the search is the exact heap ranking and the next sweep recertifies the index;
  T14 (--gate) DROP DATABASE, ALTER DATABASE ... SET TABLESPACE and CREATE DATABASE ... TEMPLATE succeed while a recertification worker
     is connected to the database: it steps aside.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402
import test_cover_insert_pg as ti  # noqa: E402
import test_cover_recert_pg as tr  # noqa: E402

check = dg.check
MAX_AGE = 15
PERIOD = 10
BOUND = MAX_AGE + PERIOD + 25


def qdb(c, db, sql):
    cmd = c.psql_cmd()
    cmd[cmd.index('-d') + 1] = db
    r = subprocess.run(cmd + ['-c', sql], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return r.returncode, r.stdout.strip()


def status(c, db, idx='ti'):
    rc, out = qdb(c, db, f"SELECT pg_cuvs_hnsw_cover_status('{idx}')::text")
    return json.loads(out) if rc == 0 and out.startswith('{') else {'class': 'error:' + out[:80]}


def log_text(c):
    return ''.join(f.read_text(errors='replace') for f in sorted(c.data.glob('log-*')))


def wait_for(cond, secs):
    t = time.time()
    while time.time() - t < secs:
        if cond():
            return time.time() - t
        time.sleep(0.5)
    return None


def rows(table, start, n):
    return f"INSERT INTO {table} SELECT {start} + g, ARRAY[g, g % 7, g % 3, (g * 7) % 11]::real[]::vector FROM generate_series(1, {n}) g"


def setup_db(c, db):
    rc, out = c.q(f'CREATE DATABASE {db}')
    assert rc == 0, out
    for sql in ('CREATE EXTENSION vector', 'CREATE EXTENSION pg_cuvs', 'CREATE TABLE t(id int, v vector(4))',
                'CREATE INDEX ti ON t USING pg_cuvs_hnsw (v vector_l2_ops)'):
        rc, out = qdb(c, db, sql)
        assert rc == 0, (db, sql, out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True)
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5841)
    ap.add_argument('--gate', action='store_true', help='--new-lib is built with -DCUVS_COVER_RECERT_TEST_GATE: also run T8')
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    plant = out / 'cover_plant'
    r = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(repo / 'src'), str(repo / 'test/unit/cover_plant.c'), '-o', str(plant)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    assert r.returncode == 0, r.stdout
    gate = out / 'gate'
    gate.mkdir()
    os.environ['CUVS_RECERT_GATE_DIR'] = str(gate)	# read by the server at its start; a gate does nothing until a test arms it
    clus = "(SELECT array_agg((((x % 25) * 7 + d * 3) % 41 + (random() - 0.5) * 0.05)::real) FROM generate_series(1, 8) d)::vector"
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'sw', out, args.port)
        c.auto_recert = True
        c.extra_opts = (f'-c autovacuum=off -c cuvs.cover_recertify_max_age={MAX_AGE} -c cuvs.cover_recertify_threshold_percent=50 '
                        '-c cuvs.cover_recertify_min_interval=1 -c log_min_messages=debug1')
        try:
            o.install('new')
            c.start()

            # T1
            setup_db(c, 's1')
            qdb(c, 's1', rows('t', 0, 2000))
            rc, res = qdb(c, 's1', "SELECT pg_cuvs_cover_recertify('ti')::text")
            t_cert = time.time()
            st0 = status(c, 's1')
            check('T1 the index is certified with no key since', rc == 0 and st0.get('keys_since_certification') == 0, str(st0))
            qdb(c, 's1', rows('t', 100000, 20))
            st1 = status(c, 's1')
            check('T1 20 inserts (1 percent) leave the ledger under the threshold, nothing is recommended, and a recertification is scheduled by time',
                  st1.get('keys_since_certification') == 20 and st1.get('recertify_recommended') is False and 'recertify_scheduled_at' in st1, str(st1))
            took = wait_for(lambda: status(c, 's1').get('generation') != st0.get('generation') and status(c, 's1').get('keys_since_certification') == 0, BOUND + 10)
            check('T1 within max_age (%d s) plus one sweep period (%d s) and the work the index is certified again (took %s s)' % (MAX_AGE, PERIOD, None if took is None else round(took, 1)),
                  took is not None and took <= BOUND, str(status(c, 's1')))

            elapsed = time.time() - t_cert
            check('T1 and not earlier than max_age after the certification (%.1f s after it): the time trigger waits for the age, it does not walk at every sweep' % elapsed,
                  took is not None and elapsed >= MAX_AGE - 1, f'{elapsed}')

            # T3 (the index of s1 is quiet now)
            time.sleep(4)		# the worker of T1 has looked once more and ended
            g_quiet = status(c, 's1').get('generation')
            lg0 = log_text(c).count('by background worker: done')
            time.sleep(MAX_AGE + 2 * PERIOD + 5)
            check('T3 an index with no key since its certification is not walked again (same generation, no recertification done in the log)',
                  status(c, 's1').get('generation') == g_quiet and log_text(c).count('by background worker: done') == lg0, f'{g_quiet} {status(c, "s1")}')

            # T6: verified inserts leave no key, and the pages they add are what the time trigger goes by. A database with cuvs.cover_verify_inserts = on: the
            # index is certified, 60 inserts are found by their own search (no ledger key), and the time-based recertification still takes the index
            # (the relation has grown since the certification), then leaves it alone while nothing is inserted.
            rc, o6 = c.q('CREATE DATABASE s6')
            assert rc == 0, o6
            for sql in ("ALTER DATABASE s6 SET cuvs.cover_verify_inserts = on", 'CREATE EXTENSION vector', 'CREATE EXTENSION pg_cuvs', 'CREATE TABLE t(id int, v vector(4))',
                        'CREATE INDEX ti ON t USING pg_cuvs_hnsw (v vector_l2_ops)'):
                rc, o6 = c.q(sql) if sql.startswith('ALTER') else qdb(c, 's6', sql)
                assert rc == 0, (sql, o6)
            qdb(c, 's6', rows('t', 0, 2000))
            qdb(c, 's6', "SELECT pg_cuvs_cover_recertify('ti')::text")
            st6 = status(c, 's6')
            t6_cert = time.time()
            qdb(c, 's6', rows('t', 100000, 60))
            st6b = status(c, 's6')
            check('T6 the setup holds: 60 verified inserts left no key since the certification, and the relation grew', st6b.get('keys_since_certification') == 0 and st6b.get('pages_since_certification', 0) >= 1, str(st6b))
            check('T6 a recertification is scheduled by time although no key was written', 'recertify_scheduled_at' in st6b, str(st6b))
            took6 = wait_for(lambda: status(c, 's6').get('generation') != st6.get('generation'), BOUND + 10)
            check('T6 within max_age (%d s) plus one sweep period (%d s) and the work the index is certified again (took %s s)' % (MAX_AGE, PERIOD, None if took6 is None else round(took6, 1)),
                  took6 is not None and took6 <= BOUND, str(status(c, 's6')))
            time.sleep(4)
            g6 = status(c, 's6').get('generation')
            lg6 = log_text(c).count('by background worker: done')
            time.sleep(MAX_AGE + 2 * PERIOD + 5)
            check('T6 and with no insert since, the index is not walked again (same generation, nothing done in the log)',
                  status(c, 's6').get('generation') == g6 and log_text(c).count('by background worker: done') == lg6, f'{g6} {status(c, "s6")}')

            # T7: a database without pg_cuvs, and a cluster where nothing grows, cost no sweep: after the full sweep that follows the start, the
            # sweeper visits only databases with a candidate (an index whose relation grew, or that lost its guarantee), so no sweep worker
            # connects to a database that does not use pg_cuvs, and none at all while no index grows
            rc, o7 = c.q('CREATE DATABASE nocuvs')
            assert rc == 0, o7
            time.sleep(PERIOD + 2)		# the indexes above are quiet now and leave the table at the next sweep
            seen = {'nocuvs': 0, 'any': 0}
            t7 = time.time()
            while time.time() - t7 < 3 * PERIOD + 2:
                rc, n = c.q("SELECT count(*) FILTER (WHERE datname = 'nocuvs'), count(*) FROM pg_stat_activity WHERE backend_type = 'pg_cuvs cover sweep'")
                if rc == 0 and '|' in n:
                    a7, b7 = n.split('|')
                    seen['nocuvs'] += int(a7)
                    seen['any'] += int(b7)
                time.sleep(0.05)
            check('T7 over three sweep periods no sweep worker connected to a database without pg_cuvs, and none at all while nothing grew', seen['nocuvs'] == 0 and seen['any'] == 0, str(seen))
            # an idle sweeper (no candidate, no transaction) still takes the barrier that DROP DATABASE waits for
            import threading
            box8 = []
            t8 = time.time()
            th8 = threading.Thread(target=lambda: box8.append(c.q('DROP DATABASE nocuvs')))
            th8.start()
            th8.join(30)
            hung = th8.is_alive()
            if hung:
                c.q("SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE query LIKE 'DROP DATABASE nocuvs%'")
                th8.join(30)
            check('T7 DROP DATABASE ends while the sweeper is idle (%.1f s)' % (time.time() - t8), not hung and box8 and box8[0][0] == 0, str(box8)[-200:])
            st7 = status(c, 's1')
            check('T7 the status says whether the background recertification is available', st7.get('background_recertification') is True, str(st7))

            # T2: a database that nobody connects to after the restart
            setup_db(c, 's2')
            qdb(c, 's2', rows('t', 0, 2000))
            qdb(c, 's2', "SELECT pg_cuvs_cover_recertify('ti')::text")
            g2 = status(c, 's2').get('generation')
            qdb(c, 's2', rows('t', 100000, 5))
            c.restart()
            t0 = time.time()
            time.sleep(MAX_AGE + PERIOD + 20)
            st2 = status(c, 's2')
            check('T2 after a restart, with no session in the database meanwhile, 5 inserts are recertified by the sweeper (generation moved, no key since)',
                  st2.get('generation') != g2 and st2.get('keys_since_certification') == 0, str(st2))

            # T4. Whether the inserts cut rows off depends on the graph: pgvector draws the levels from the backend's own random
            # state, which setseed does not set, and each statement here is a new session. A graph the inserts cut nothing off
            # is built again (new rows, new levels), up to four times.
            setup_db(c, 's4')
            qdb(c, 's4', 'DROP TABLE t')
            planted = []
            for attempt in range(4):
                for sql in ('DROP TABLE IF EXISTS g', 'CREATE TABLE g(id int, v vector(8))'):
                    qdb(c, 's4', sql)
                qdb(c, 's4', f"SELECT setseed({0.42 + 0.01 * attempt}); INSERT INTO g SELECT x, {clus} FROM generate_series(1, 4000) x")
                qdb(c, 's4', 'CREATE INDEX gi ON g USING hnsw (v vector_l2_ops) WITH (m = 4, ef_construction = 8)')
                qdb(c, 's4', "UPDATE pg_class SET relam = (SELECT oid FROM pg_am WHERE amname = 'pg_cuvs_hnsw') WHERE relname = 'gi'")
                qdb(c, 's4', "UPDATE pg_index SET indclass = (SELECT oid::text::oidvector FROM pg_opclass WHERE opcname = 'vector_l2_ops' AND opcmethod = (SELECT oid FROM pg_am WHERE amname = 'pg_cuvs_hnsw')) WHERE indexrelid = 'gi'::regclass")
                rc, fp = qdb(c, 's4', "SELECT pg_relation_filepath('gi')")
                gpath = c.data / fp.strip().splitlines()[-1]
                c.stop()
                rpl = subprocess.run([str(plant), str(gpath), 'uncertified'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                planted.append((rpl.returncode, rpl.stdout))
                c.start()
                rc, res = qdb(c, 's4', "SELECT pg_cuvs_cover_recertify('gi')::text")
                g4 = status(c, 's4', 'gi').get('generation')
                qdb(c, 's4', f"SELECT setseed({0.52 + 0.01 * attempt}); INSERT INTO g SELECT 900000 + x, {clus} FROM generate_series(1, 1800) x")	# 45 percent of the rows, under the 50 percent threshold
                sti = status(c, 's4', 'gi')
                qdb(c, 's4', 'CHECKPOINT')
                u_before, _ = tr.unreachable_tids(gpath)
                keys, kinds, order, d = tr.ledger_all(ti.read_pages(gpath))
                missing = sorted(u_before - {(k[1], k[2]) for k in keys})
                if missing:
                    break
            check('T4 the uncertified state was planted', all(p[0] == 0 for p in planted), str(planted))
            check('T4 the inserts are under the threshold (not recommended) and cut old rows off the graph that the ledger does not hold (%d rows, graph %d of at most 4)' % (len(missing), len(planted)),
                  sti.get('recertify_recommended') is False and len(missing) > 0, f'{len(missing)} {sti}')
            took = wait_for(lambda: status(c, 's4', 'gi').get('generation') != g4 and status(c, 's4', 'gi').get('keys_since_certification') == 0, BOUND + 10)
            check('T4 the time-based recertification ran within the bound (took %s s)' % (None if took is None else round(took, 1)), took is not None and took <= BOUND, str(status(c, 's4', 'gi')))
            qdb(c, 's4', 'CHECKPOINT')
            u_after, _ = tr.unreachable_tids(gpath)
            keys2, kinds2, order2, d2 = tr.ledger_all(ti.read_pages(gpath))
            have2 = {(k[1], k[2]) for k in keys2}
            check('T4 afterwards every row the graph cannot reach has a key (independent oracle)', u_after <= have2, f'{len(u_after - have2)} missing of {len(u_after)}')
            bad = 0
            for t in missing[:20]:
                rc, vec = qdb(c, 's4', f"SELECT v::text FROM g WHERE ctid = '({t[0]},{t[1]})'::tid")
                rc, rid = qdb(c, 's4', f"SELECT id FROM g WHERE ctid = '({t[0]},{t[1]})'::tid")
                rc, res = qdb(c, 's4', f"SET enable_seqscan = off; SELECT id FROM g ORDER BY v <-> '{vec.strip()}' LIMIT 1")
                if rc != 0 or res.strip().splitlines()[-1] != rid.strip().splitlines()[-1]:
                    bad += 1
            check('T4 and each of the rows that were missing is the first answer of a search with its own vector (heap truth)', bad == 0, f'{bad} of {len(missing[:20])}')

            # T5
            setup_db(c, 's5')
            qdb(c, 's5', "DO $$ BEGIN FOR i IN 1..60 LOOP EXECUTE format('CREATE TABLE m%s(id int, v vector(4))', i); "
                         "EXECUTE format('CREATE INDEX mi%s ON m%s USING pg_cuvs_hnsw (v vector_l2_ops)', i, i); "
                         "EXECUTE format('INSERT INTO m%s SELECT g, ARRAY[g, g %% 7, g %% 3, 1]::real[]::vector FROM generate_series(1, 100) g', i); "
                         "PERFORM pg_cuvs_cover_recertify(('mi' || i)::regclass); "
                         "EXECUTE format('INSERT INTO m%s SELECT 1000 + g, ARRAY[g, g %% 5, g %% 2, 2]::real[]::vector FROM generate_series(1, 3) g', i); END LOOP; END $$")
            rc, n = qdb(c, 's5', "SELECT count(*) FROM generate_series(1, 60) g WHERE (pg_cuvs_hnsw_cover_status(('mi' || g)::regclass)->>'keys_since_certification')::int = 3")
            check('T5 sixty indexes certified, each with 3 keys since', n.strip() == '60', n)
            peak = [0]
            t_start = time.time()
            while time.time() - t_start < BOUND + 45:
                rc, k = c.q("SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'pg_cuvs recertification'")
                if rc == 0 and k.strip().isdigit():
                    peak[0] = max(peak[0], int(k))
                rc, n = qdb(c, 's5', "SELECT count(*) FROM generate_series(1, 60) g WHERE (pg_cuvs_hnsw_cover_status(('mi' || g)::regclass)->>'keys_since_certification')::int = 0")
                if n.strip() == '60':
                    break
                time.sleep(0.2)
            check('T5 all sixty are recertified by time (%.0f s after they were certified)' % (time.time() - t_start), n.strip() == '60', n)
            check('T5 and never more than 2 recertification workers at once (saw %d)' % peak[0], 1 <= peak[0] <= 2, f'peak={peak[0]}')

            # T8: an ERROR ends a background worker (no error recovery there), and it used to leave no failure behind, so
            # the next sweep started the same worker again after cuvs.cover_recertify_min_interval (1 s here). The worker is cancelled while it
            # waits at the "walk" gate, which raises the ERROR "canceling statement due to user request" in it. The wait after an error is max_age
            # (15 s here, set on the command line), longer than the sweep period (10 s): two starts are never one sweep apart.
            if args.gate:
                setup_db(c, 's8')
                qdb(c, 's8', rows('t', 0, 2000))
                qdb(c, 's8', "SELECT pg_cuvs_cover_recertify('ti')")
                mark8 = len(log_text(c))
                (gate / 'walk_armed').touch()
                qdb(c, 's8', rows('t', 100000, 1200))		# 60 percent of the rows: past the threshold (50), the next sweep asks for a worker

                def reached8():
                    return (gate / 'walk_reached').exists()

                def cancel8():
                    (gate / 'walk_reached').unlink()
                    for _ in range(20):
                        rc, n = c.q("SELECT count(*) FROM (SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE backend_type = 'pg_cuvs recertification') x")
                        if rc == 0 and n.strip() not in ('', '0'):
                            return
                        time.sleep(0.1)
                first = wait_for(reached8, 2 * PERIOD + 5)
                check('T8 setup: past the threshold, a sweep starts a worker that reaches the walk gate', first is not None, str(status(c, 's8')))
                at = []
                if first is not None:
                    at.append(time.time())
                    cancel8()
                    t_end = time.time() + 50
                    while time.time() < t_end:
                        if reached8():
                            at.append(time.time())
                            cancel8()
                        time.sleep(0.2)
                gaps = [round(b - a, 1) for a, b in zip(at, at[1:])]
                lines8 = [l for l in log_text(c)[mark8:].splitlines() if 'recertification' in l or 'canceling' in l or 'ended with an error' in l]
                check('T8 a worker that ended with an error is not started again at the next sweep: starts %s s apart (max_age %d s, sweep period %d s)' % (gaps, MAX_AGE, PERIOD),
                      len(at) >= 2 and all(g >= MAX_AGE - 1 for g in gaps), f'starts={len(at)} ' + ' | '.join(lines8[-12:]))
                check('T8 the log says when the next one comes', 'ended with an error; the next one starts in %d s' % MAX_AGE in log_text(c)[mark8:], ' | '.join(lines8[-6:]))
                qdb(c, 's8', rows('t', 200000, 600))		# new pages: what failed may not fail again
                again = wait_for(reached8, 2 * PERIOD + 5)
                check('T8 after the index grew, the next sweep starts a worker again (%s s)' % (None if again is None else round(again, 1)), again is not None, str(status(c, 's8')))
                # a worker that is terminated (pg_terminate_backend, DROP DATABASE ... WITH (FORCE)) did not fail: the next sweep starts it
                # again, without the wait of max_age, and the log says it was stopped
                mark8b = len(log_text(c))
                (gate / 'walk_reached').unlink(missing_ok=True)
                c.q("SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE backend_type = 'pg_cuvs recertification' AND datname = 's8'")
                t8b = time.time()
                back = wait_for(reached8, MAX_AGE + 5)
                check('T8 a terminated worker is started again at a later sweep, before max_age (%s s)' % (None if back is None else round(back, 1)),
                      back is not None and back < MAX_AGE, str(status(c, 's8')))
                check('T8 and the log says it was stopped, not that it failed', 'was stopped; a later sweep starts it again' in log_text(c)[mark8b:]
                      and 'ended with an error' not in log_text(c)[mark8b:], log_text(c)[mark8b:][-500:])
                (gate / 'walk_armed').unlink()
                (gate / 'walk_go').touch()
                done8 = wait_for(lambda: status(c, 's8').get('keys_since_certification') == 0, 3 * PERIOD + 10)
                check('T8 and once the gate is open that worker certifies the index', done8 is not None, str(status(c, 's8')))
                for f in ('walk_go', 'walk_reached'):
                    (gate / f).unlink(missing_ok=True)

            # T9: a database with candidates that is then dropped, and one that stops accepting connections. A sweep worker
            # of either would end at the connection (FATAL) at every sweep, and the candidates would stay for good.
            mark9 = len(log_text(c))
            for db in ('s9a', 's9b'):
                setup_db(c, db)
                qdb(c, db, rows('t', 0, 300))			# an index built on an empty table grows: a candidate of that database
            rc_a, o_a = c.q('DROP DATABASE s9a WITH (FORCE)')
            rc_b, o_b = c.q('ALTER DATABASE s9b ALLOW_CONNECTIONS false')
            check('T9 setup: one database dropped, one closed to connections', rc_a == 0 and rc_b == 0, f'{o_a} {o_b}')
            # a sweep that read pg_database just before the DROP can still start a worker of it that fails once: the sweeps after it count
            time.sleep(PERIOD + 2)
            mark9 = len(log_text(c))
            time.sleep(3 * PERIOD + 5)
            tail9 = log_text(c)[mark9:]
            bad9 = [l for l in tail9.splitlines() if 'FATAL' in l and ('does not exist' in l or 'not currently accepting connections' in l)]
            check('T9 no sweep worker tried to connect to them in three sweeps (%d FATAL lines)' % len(bad9), not bad9, '\n'.join(bad9[:4]))
            c.q('ALTER DATABASE s9b ALLOW_CONNECTIONS true')

            # T10 (--gate): a sweep judges a candidate with nothing pending; before it takes the entry off, an insert grows the index and
            # notes it, but the entry is still there, so the note writes nothing new. Taking the entry off then lost the index until its next growth: the
            # time rule never came. The sweeper is held off while the index is set up, so the first sweep that sees it judges "nothing pending".
            if args.gate:
                for db in ('s1', 's2', 's4', 's5', 's6', 's8', 's9b'):
                    c.q(f'DROP DATABASE IF EXISTS {db} WITH (FORCE)')		# no other database with a candidate reaches the gate
                c.q('ALTER SYSTEM SET cuvs.cover_auto_recertify = off')
                c.q('SELECT pg_reload_conf()')
                time.sleep(2)
                setup_db(c, 's10')
                qdb(c, 's10', rows('t', 0, 2000))			# the growth makes the index a candidate
                qdb(c, 's10', "SELECT pg_cuvs_cover_recertify('ti')")	# certified with nothing since: the next sweep finds nothing pending
                g10 = status(c, 's10').get('generation')
                (gate / 'sweep_armed').touch()
                c.q('ALTER SYSTEM RESET cuvs.cover_auto_recertify')
                c.q('SELECT pg_reload_conf()')
                first = wait_for(lambda: (gate / 'sweep_reached').exists(), 2 * PERIOD + 5)
                check('T10 setup: a sweep worker has judged the index (nothing pending) and waits before it acts', first is not None, str(status(c, 's10')))
                qdb(c, 's10', rows('t', 100000, 300))		# 15 percent, under the threshold (50); the index grows, the note finds the entry present
                st10 = status(c, 's10')
                (gate / 'sweep_armed').unlink()
                (gate / 'sweep_go').touch()
                took = wait_for(lambda: status(c, 's10').get('generation') != g10 and status(c, 's10').get('keys_since_certification') == 0, BOUND + 10)
                check('T10 the insert made during the judgment is not lost: the time rule recertifies the index within the bound (took %s s)' % (None if took is None else round(took, 1)),
                      took is not None and took <= BOUND + 5, f'{st10} -> {status(c, "s10")}')
                for f in ('sweep_go', 'sweep_reached'):
                    (gate / f).unlink(missing_ok=True)

            # T11: cuvs.cover_recertify_max_age = 0 turned the sweeper off, and with it the recertification of an index past the threshold
            # (no insert asks for a worker in this version), while the status said the next sweep would do it. Now it turns only the time rule off. The
            # setting is on the command line, so the server is started again with 0 for this case.
            opts11 = c.extra_opts
            c.stop()
            c.extra_opts = opts11.replace(f'cuvs.cover_recertify_max_age={MAX_AGE}', 'cuvs.cover_recertify_max_age=0')
            c.start()
            setup_db(c, 's11')
            qdb(c, 's11', rows('t', 0, 2000))
            qdb(c, 's11', "SELECT pg_cuvs_cover_recertify('ti')")
            g11 = status(c, 's11').get('generation')
            qdb(c, 's11', rows('t', 100000, 1200))		# 60 percent: past the threshold (50)
            st11 = status(c, 's11')
            check('T11 with max_age 0 nothing is scheduled by time, and the guarantee names no time bound',
                  'recertify_scheduled_at' not in st11 and 'no time bound' in st11.get('guarantee', ''), str(st11))
            took11 = wait_for(lambda: status(c, 's11').get('generation') != g11, 2 * 30 + 15)
            check('T11 and the sweep (every 30 s then) still recertifies an index past the threshold (took %s s)' % (None if took11 is None else round(took11, 1)),
                  took11 is not None, str(status(c, 's11')))
            c.stop()
            c.extra_opts = opts11
            c.start()

            # T12: with cuvs.cover_verify_inserts = on, an insert the verification finds reachable writes no key, and one that lands in
            # a page that was already there adds no page: nothing made the index a candidate or due, so the time rule never came. The first insert after the
            # certification now marks the descriptor (inserted_since_certification) and writes the candidate. (a) eight inserts that come together after a
            # certification write the mark once (one log line); (b) a recertification clears it and the next insert sets it again.
            import threading
            rc, o12 = c.q('CREATE DATABASE s12')
            assert rc == 0, o12
            for sql in ('ALTER DATABASE s12 SET cuvs.cover_verify_inserts = on', 'CREATE EXTENSION vector', 'CREATE EXTENSION pg_cuvs',
                        'CREATE TABLE t(id int, v vector(4))', 'CREATE INDEX ti ON t USING pg_cuvs_hnsw (v vector_l2_ops)'):
                rc, o12 = c.q(sql) if sql.startswith('ALTER') else qdb(c, 's12', sql)
                assert rc == 0, (sql, o12)
            qdb(c, 's12', rows('t', 0, 2000))
            qdb(c, 's12', "SELECT pg_cuvs_cover_recertify('ti')")
            st12a = status(c, 's12')
            check('T12 setup: right after the certification the mark is clear', st12a.get('inserted_since_certification') is False, str(st12a))
            mark12 = len(log_text(c))
            ths = [threading.Thread(target=qdb, args=(c, 's12', rows('t', 100000 + k * 10, 1))) for k in range(8)]
            for th in ths:
                th.start()
            for th in ths:
                th.join()
            marks = log_text(c)[mark12:].count('"ti" marked as inserted into since its certification')
            check('T12 (a) eight inserts that came together set the mark with one write (%d log lines)' % marks,
                  marks == 1 and status(c, 's12').get('inserted_since_certification') is True, log_text(c)[mark12:][-400:])
            # the time rule on the mark alone: one verified insert after a certification, which wrote no key and stayed in a page that was there (eight
            # together can add a page, which counts by itself); tried again after a new certification if it did add one
            for attempt in range(6):
                qdb(c, 's12', "SELECT pg_cuvs_cover_recertify('ti')")
                g12 = status(c, 's12').get('generation')
                qdb(c, 's12', rows('t', 300000 + attempt * 10, 1))
                st12b = status(c, 's12')
                if st12b.get('pages_since_certification') == 0:
                    break
            check('T12 one verified insert left no key and added no page, and set the mark',
                  st12b.get('keys_since_certification') == 0 and st12b.get('pages_since_certification') == 0 and st12b.get('inserted_since_certification') is True, str(st12b))
            check('T12 and a recertification is scheduled by time', 'recertify_scheduled_at' in st12b, str(st12b))
            took12 = wait_for(lambda: status(c, 's12').get('generation') != g12, BOUND + 10)
            st12c = status(c, 's12')
            check('T12 the time rule recertifies the index within the bound (took %s s)' % (None if took12 is None else round(took12, 1)),
                  took12 is not None and took12 <= BOUND + 5, str(st12c))
            check('T12 (b) the recertification cleared the mark', st12c.get('inserted_since_certification') is False, str(st12c))
            qdb(c, 's12', rows('t', 200000, 1))
            check('T12 (b) and the next insert sets it again', status(c, 's12').get('inserted_since_certification') is True, str(status(c, 's12')))

            # T13 (--gate, a user decision and an independent review): a VACUUM held right after it took the guarantee off. While it runs the search merges
            # the graph and the ledger, and the sweeper starts no worker for the index (the VACUUM recertifies it at its end; a worker would only be
            # deferred by the VACUUM's lock at every sweep). The VACUUM is then cancelled: the index stays LOST (vacuum), the search is the exact heap
            # ranking with its notice, and the next sweep recertifies it.
            if args.gate:
                # the background recertification is held off while the index is set up, so that every row stays a ledger key (with an
                # empty ledger a scan that answered from the graph alone would pass for a merge); it is on again before the sweeps that are watched
                c.q('ALTER SYSTEM SET cuvs.cover_auto_recertify = off')
                c.q('SELECT pg_reload_conf()')
                time.sleep(2)
                setup_db(c, 's13')
                qdb(c, 's13', rows('t', 0, 400))			# built on an empty table: every row is a ledger key
                qdb(c, 's13', 'DELETE FROM t WHERE id % 5 = 0')
                mark13 = len(log_text(c))
                (gate / 'vacuum_armed').touch()
                box13 = []
                th13 = threading.Thread(target=lambda: box13.append(qdb(c, 's13', 'VACUUM t')))
                th13.start()
                r13 = wait_for(lambda: (gate / 'vacuum_reached').exists(), 30)
                c.q('ALTER SYSTEM RESET cuvs.cover_auto_recertify')
                c.q('SELECT pg_reload_conf()')
                # hnsw.ef_search = 1: the graph alone gives about one row; the merge ranks every ledger key too, so 50 rows come back in the exact order
                q13 = "SELECT id FROM t ORDER BY v <-> '[1,1,1,1]' LIMIT 50"
                rc13, res13 = qdb(c, 's13', f'SET enable_seqscan = off; SET hnsw.ef_search = 1; {q13}')
                rc13h, res13h = qdb(c, 's13', f'SET enable_indexscan = off; SET enable_bitmapscan = off; {q13}')
                ids13 = [x for x in res13.split('\n') if x.isdigit()]
                check('T13 while the VACUUM runs (held after it took the guarantee off) the search merges the graph and the ledger: no heap notice, and with '
                      'hnsw.ef_search = 1 the 50 rows of the exact ranking, which only the ledger can give (%d rows)' % len(ids13),
                      r13 is not None and rc13 == 0 and 'answers from the heap' not in res13 and len(ids13) == 50
                      and ids13 == [x for x in res13h.split('\n') if x.isdigit()], f'{res13[-300:]} / {res13h[-120:]}')
                peak13 = 0
                t13 = time.time()
                while time.time() - t13 < 2 * PERIOD + 5:
                    rc, k = c.q("SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'pg_cuvs recertification' AND datname = 's13'")
                    if rc == 0 and k.strip().isdigit():
                        peak13 = max(peak13, int(k))
                    time.sleep(0.5)
                deferred13 = log_text(c)[mark13:].count('by background worker: deferred')
                check('T13 and for two sweep periods the sweeper starts no worker for it (peak %d, %d deferred lines)' % (peak13, deferred13),
                      peak13 == 0 and deferred13 == 0, log_text(c)[mark13:][-400:])
                c.q("SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE datname = 's13' AND query = 'VACUUM t'")
                th13.join(60)
                (gate / 'vacuum_armed').unlink()
                for f in ('vacuum_reached', 'vacuum_go'):
                    (gate / f).unlink(missing_ok=True)
                st13 = status(c, 's13')
                rc13, res13 = qdb(c, 's13', "SET enable_seqscan = off; SELECT id FROM t ORDER BY v <-> '[1,1,1,1]' LIMIT 5")
                check('T13 the VACUUM was cancelled: the index is LOST (vacuum) and the search is the exact heap ranking, with the notice',
                      box13 and box13[0][0] != 0 and st13.get('class') == 'lost' and st13.get('stored_reason') == 4 and rc13 == 0 and 'answers from the heap' in res13,
                      f'{box13} {st13} {res13[-200:]}')
                took13 = wait_for(lambda: status(c, 's13').get('class') == 'certified', 2 * PERIOD + 15)
                check('T13 and the next sweep recertifies it (%s s)' % (None if took13 is None else round(took13, 1)), took13 is not None, str(status(c, 's13')))

            # T14 (--gate): DROP DATABASE, ALTER DATABASE ... SET TABLESPACE and CREATE DATABASE ... TEMPLATE wait 5 s for the other
            # sessions of the database to go and then fail ("is being accessed by other users"); PostgreSQL ends only autovacuum workers. A
            # recertification worker that is connected to the database (held at the walk gate, which stands for a long walk) steps aside: each
            # command succeeds.
            if args.gate:
                setup_db(c, 's14')
                qdb(c, 's14', rows('t', 0, 3000))
                qdb(c, 's14', "SELECT pg_cuvs_cover_recertify('ti')")
                (c.data / 'ts14').mkdir(exist_ok=True)
                c.q(f"CREATE TABLESPACE ts14 LOCATION '{c.data / 'ts14'}'")
                k14 = [400000]

                def hold14():
                    for f in ('walk_reached', 'walk_go'):
                        (gate / f).unlink(missing_ok=True)
                    (gate / 'walk_armed').touch()
                    base = status(c, 's14').get('base_rows') or 3000
                    k14[0] += 100000
                    qdb(c, 's14', rows('t', k14[0], int(base * 0.6) + 10))		# past the threshold: the next sweep starts a worker
                    got = wait_for(lambda: (gate / 'walk_reached').exists(), 2 * PERIOD + 10)
                    rc, n = c.q("SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'pg_cuvs recertification' AND datname = 's14'")
                    return got is not None and n.strip() == '1'

                def release14():
                    (gate / 'walk_armed').unlink(missing_ok=True)
                    (gate / 'walk_go').touch()
                    wait_for(lambda: c.q("SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'pg_cuvs recertification' AND datname = 's14'")[1].strip() == '0', 30)

                for name, sql in (('ALTER DATABASE ... SET TABLESPACE', 'ALTER DATABASE s14 SET TABLESPACE ts14'),
                                  ('CREATE DATABASE ... TEMPLATE', 'CREATE DATABASE s14copy TEMPLATE s14'),
                                  ('DROP DATABASE', 'DROP DATABASE s14')):
                    held14 = hold14()
                    mark14 = len(log_text(c))
                    t14 = time.time()
                    rc14, out14 = c.q(sql)
                    el14 = time.time() - t14
                    check(f'T14 {name} while a recertification worker is connected to the database (held at the walk gate: {held14}) succeeds (%.1f s)' % el14,
                          held14 and rc14 == 0, out14[-200:])
                    check(f'T14 and the worker logs that it stepped aside for it (not an error)', 'stepped aside' in log_text(c)[mark14:]
                          and 'ended with an error' not in log_text(c)[mark14:], log_text(c)[mark14:][-400:])
                    release14()
                c.q('DROP DATABASE IF EXISTS s14copy')
                c.q('DROP DATABASE IF EXISTS s14')
                c.q('DROP TABLESPACE IF EXISTS ts14')
        finally:
            try:
                c.drop()
            except Exception:
                pass
    finally:
        (out / 'transcript.txt').write_text('\n'.join(dg.TRANSCRIPT) + '\n')
        shutil.rmtree(root, ignore_errors=True)
        for d in out.glob('data-*'):
            shutil.rmtree(d, ignore_errors=True)
    result = {'failures': dg.FAILURES, 'verdict': 'FAIL' if dg.FAILURES else 'PASS',
              'not_run': 'a sweep over many databases and indexes at the limit of workers' + ('' if args.gate else '; T8, T10, T13 and T14 (need --gate)')}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 1 if dg.FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
