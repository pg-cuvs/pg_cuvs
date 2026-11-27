#!/usr/bin/env python3
"""The background recertification worker next to the things a server does around it (CPU only).

  O1 three databases of one cluster request recertifications at once: every index is certified without a call, and no more than
     cuvs.cover_recertify_max_workers (2) workers run at the same time in the whole server;
  O2 DROP DATABASE of a database whose worker may still be running or lingering succeeds, and the database can be created again;
  O3 a fast shutdown while workers run ends in bounded time with no crash or PANIC in the log, and after the restart the index reads and certifies;
  O5 an immediate stop (a crash) while workers run and inserts go on: after the recovery the ledger reads and every row the graph cannot reach has a key;
  O4 workers that are sent SIGTERM again and again while inserts go on leave an index whose ledger reads, covers every row the graph cannot
     reach, and that a manual recertification brings to done (the worker looks at SIGTERM between looks, so a walk that is running ends first).
"""
import argparse
import json
from pathlib import Path
import shutil
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402
import test_cover_insert_pg as ti  # noqa: E402
import test_cover_recert_pg as tr  # noqa: E402

check = dg.check


def qdb(c, db, sql):
    cmd = c.psql_cmd()
    cmd[cmd.index('-d') + 1] = db
    import subprocess
    r = subprocess.run(cmd + ['-c', sql], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return r.returncode, r.stdout.strip()


def status(c, db, idx='ti'):
    rc, out = qdb(c, db, f"SELECT pg_cuvs_hnsw_cover_status('{idx}')::text")
    return json.loads(out) if rc == 0 and out.startswith('{') else {'class': 'error:' + out[:80]}


def log_text(c):
    return ''.join(f.read_text(errors='replace') for f in sorted(c.data.glob('log-*')))


def manual_recertify(c, db, secs=30):
    """pg_cuvs_cover_recertify() in `db`, called again while it answers deferred: a background worker (the sweep after a restart, or one that
    was running) can hold the table at that moment, and "deferred ... call again" is then the right answer, not a failure (seen in
    run 1 of 20 on the merged tree)."""
    t = time.time()
    while True:
        rc, res = qdb(c, db, "SELECT pg_cuvs_cover_recertify('ti')::text")
        if rc != 0 or '"result": "deferred"' not in res or time.time() - t > secs:
            return rc, res
        time.sleep(1)


def wait_for(cond, secs=60):
    t = time.time()
    while time.time() - t < secs:
        if cond():
            return True
        time.sleep(0.3)
    return False


def rows(start, n):
    return f"INSERT INTO t SELECT {start} + g, ARRAY[g, g % 7, g % 3, (g * 7) % 11]::real[]::vector FROM generate_series(1, {n}) g"


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
    ap.add_argument('--port', type=int, default=5821)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'op', out, args.port)
        c.auto_recert = True
        c.extra_opts = '-c autovacuum=off -c cuvs.cover_recertify_min_interval=1 -c cuvs.cover_recertify_max_age=100 -c max_connections=60 -c log_min_messages=debug1'
        try:
            o.install('new')
            c.start()

            # O1
            dbs = ['o1a', 'o1b', 'o1c']
            for db in dbs:
                setup_db(c, db)
            peak = [0]
            stop_watch = threading.Event()

            def watcher():
                while not stop_watch.is_set():
                    rc, n = c.q("SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'pg_cuvs recertification'")
                    if rc == 0 and n.isdigit():
                        peak[0] = max(peak[0], int(n))
                    time.sleep(0.02)
            wt = threading.Thread(target=watcher)
            wt.start()
            errs = []

            def load(db):
                for i in range(6):
                    rc, res = qdb(c, db, rows(i * 100000, 1500))
                    if rc != 0:
                        errs.append(res)
            ths = [threading.Thread(target=load, args=(db,)) for db in dbs]
            for th in ths:
                th.start()
            for th in ths:
                th.join()
            ok = wait_for(lambda: all(status(c, db).get('generation', 1) > 1 and status(c, db).get('recertify_recommended') is False for db in dbs), 90)
            stop_watch.set()
            wt.join()
            check('O1 the loads of three databases ran without an error', not errs, str(errs[:2]))
            check('O1 every index was certified by the workers, nothing is left past the threshold', ok, str([status(c, db) for db in dbs]))
            check('O1 and the whole server never ran more than 2 workers at once (it saw %d)' % peak[0], 1 <= peak[0] <= 2, f'peak={peak[0]}')

            # O2
            setup_db(c, 'o2')
            qdb(c, 'o2', rows(0, 3000))		# a burst: a worker is asked for and may be running or lingering at the DROP
            rc, res = c.q('DROP DATABASE o2')
            check('O2 DROP DATABASE of a database with a worker that may still run or linger succeeds', rc == 0, res)
            setup_db(c, 'o2')
            rc, res = qdb(c, 'o2', rows(0, 2000))
            check('O2 the database can be created again and written to', rc == 0, res)

            # O3
            stop = threading.Event()

            def writer(k):
                n = 0
                while not stop.is_set():
                    n += 1
                    qdb(c, 'o1a', rows(10000000 + k * 1000000 + n * 10, 10))
            ws = [threading.Thread(target=writer, args=(k,)) for k in range(6)]
            for w in ws:
                w.start()
            seen = wait_for(lambda: c.q("SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'pg_cuvs recertification'")[1] not in ('0', ''), 40)
            t0 = time.time()
            c.stop('fast')
            took = time.time() - t0
            stop.set()
            for w in ws:
                w.join()
            lg = log_text(c)
            check('O3 a worker was running when the server was stopped (not required for the next two checks, which hold either way)', seen, 'no worker seen')
            check('O3 a fast shutdown ends in bounded time (%.1f s)' % took, took < 30, f'{took:.1f}s')
            check('O3 and the log has no crash, PANIC or "terminated by signal"', 'PANIC' not in lg and 'terminated by signal' not in lg, lg[-600:])
            c.start()
            st = status(c, 'o1a')
            check('O3 after the restart the index reads (class certified or lost, ledger ok)', st.get('class') in ('certified', 'lost') and st.get('ledger_ok', True) is True, str(st))
            rc, res = manual_recertify(c, 'o1a')
            check('O3 and a manual recertification ends done', rc == 0 and '"result": "done"' in res, res)

            # O4
            setup_db(c, 'o4')
            stop = threading.Event()

            def writer4(k):
                n = 0
                while not stop.is_set():
                    n += 1
                    qdb(c, 'o4', rows(20000000 + k * 1000000 + n * 10, 10))
            ws = [threading.Thread(target=writer4, args=(k,)) for k in range(6)]
            for w in ws:
                w.start()
            t_end = time.time() + 25		# the sweeper starts workers once per period (10 s here), not the inserts
            killed = 0
            while time.time() < t_end:
                rc, n = c.q("SELECT count(*) FROM (SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE backend_type = 'pg_cuvs recertification') x")
                killed += int(n) if rc == 0 and n.isdigit() else 0
                time.sleep(0.02)
            stop.set()
            for w in ws:
                w.join()
            time.sleep(3)
            lg = log_text(c)
            check('O4 workers were sent SIGTERM (%d times) and the log has no crash or PANIC' % killed, killed > 0 and 'PANIC' not in lg and 'terminated by signal' not in lg, f'killed={killed}')
            st = status(c, 'o4')
            check('O4 the ledger reads and the class is certified or lost', st.get('class') in ('certified', 'lost') and st.get('ledger_ok', True) is True, str(st))
            rc, res = manual_recertify(c, 'o4')
            check('O4 a manual recertification ends done', rc == 0 and '"result": "done"' in res, res)
            rc, res = qdb(c, 'o4', "CHECKPOINT; SELECT pg_relation_filepath('ti')")
            # the file of the index in database o4: read it with the independent reader and require every unreachable row to be in the ledger
            rc, fp = qdb(c, 'o4', "SELECT pg_relation_filepath('ti')")
            qdb(c, 'o4', 'CHECKPOINT')
            ipath = c.data / fp.strip().splitlines()[-1]
            keys, kinds, order, d = tr.ledger_all(ti.read_pages(ipath))
            have = {(k[1], k[2]) for k in keys}
            unreached, _ = tr.unreachable_tids(ipath)
            check('O4 every row the graph cannot reach has a key', unreached <= have, f'{len(unreached - have)} missing of {len(unreached)}')

            # O5
            setup_db(c, 'o5')
            stop = threading.Event()

            def writer5(k):
                n = 0
                while not stop.is_set():
                    n += 1
                    qdb(c, 'o5', rows(30000000 + k * 1000000 + n * 10, 10))
            ws = [threading.Thread(target=writer5, args=(k,)) for k in range(6)]
            for w in ws:
                w.start()
            wait_for(lambda: c.q("SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'pg_cuvs recertification'")[1] not in ('0', ''), 40)
            time.sleep(1.0)
            c.stop('immediate')
            stop.set()
            for w in ws:
                w.join()
            c.start()
            rc, fp = qdb(c, 'o5', "SELECT pg_relation_filepath('ti')")
            st = status(c, 'o5')
            check('O5 after the crash recovery the ledger reads and the class is certified or lost', st.get('class') in ('certified', 'lost') and st.get('ledger_ok', True) is True, str(st))
            qdb(c, 'o5', 'CHECKPOINT')
            ipath = c.data / fp.strip().splitlines()[-1]
            keys, kinds, order, d = tr.ledger_all(ti.read_pages(ipath))
            have = {(k[1], k[2]) for k in keys}
            unreached, _ = tr.unreachable_tids(ipath)
            # rows that inserts cut off after the last published certification are outside the ledger until the next one (the guarantee says so);
            # what the crash must not break is that the next recertification ends done and covers every row the graph cannot reach
            check('O5 (information) rows the graph cannot reach without a key right after the crash: %d of %d' % (len(unreached - have), len(unreached)), True)
            rc, res = qdb(c, 'o5', "SELECT pg_cuvs_cover_recertify('ti')::text")
            qdb(c, 'o5', 'CHECKPOINT')
            keys, kinds, order, d = tr.ledger_all(ti.read_pages(ipath))
            have = {(k[1], k[2]) for k in keys}
            unreached, _ = tr.unreachable_tids(ipath)
            check('O5 after the crash a recertification ends done and every row the graph cannot reach has a key', rc == 0 and '"result": "done"' in res and unreached <= have,
                  f'{res[:120]} {len(unreached - have)} missing of {len(unreached)}')
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
              'not_run': 'a walk that is running when SIGTERM arrives, on a graph large enough to take long (the worker looks at SIGTERM between looks)'}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 1 if dg.FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
