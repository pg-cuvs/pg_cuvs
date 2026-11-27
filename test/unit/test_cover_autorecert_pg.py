#!/usr/bin/env python3
"""Recertification that nobody asks for, on real servers (CPU only).

In this version no insert asks for a worker: the sweeper looks at every index once per period (a tenth of cuvs.cover_recertify_max_age, at
least 10 s), and an index whose keys, or pages, since its certification are past cuvs.cover_recertify_threshold_percent of what the
certification covered is recertified by a short-lived background worker at that sweep (an index under the threshold waits for the age).
The cluster runs with cuvs.cover_recertify_max_age = 100 s, so the period is 10 s. Checked:

  A1 an index built on an empty table and filled by one statement is certified by the workers while the load goes on and after it, with
     nothing called by anyone: its status ends certified, in force, with few keys since, and the log has the lines of the workers;
  A2 below the threshold no worker is started within a sweep (no new generation), past it one is at the next sweep (a new generation) without any call;
  A3 with cuvs.cover_auto_recertify off nothing happens by itself and the status recommends the recertification;
  A4 without a free worker slot the inserts go through unharmed and the VACUUM that follows recertifies;
  A5 eight sessions inserting at once all succeed, and the ledger afterwards reads and covers the heap;
  A6 a worker that could not lock the table ends deferred; the next sweep after the interval and a VACUUM pick the index up, and the status says so;
  A10 a worker that did its work does not delay the next one by the interval (two bursts, the second one served at the sweep after it);
  A9 a burst that a single statement inserts leaves nothing past the threshold one sweep after the statement is over.
The checks of the earlier version that inserts ask for at most one worker per interval (A7, A8) belong to the request from the insert path,
which this version does not have; the limit of cuvs.cover_recertify_max_workers at once is checked by test_cover_sweep_pg.py T5.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402
import test_cover_insert_pg as ti  # noqa: E402
import test_cover_recert_pg as tr  # noqa: E402

check = dg.check


def status(c, idx='ti'):
    return json.loads(c.scalar(f"SELECT pg_cuvs_hnsw_cover_status('{idx}')::text"))


def log_text(c):
    return ''.join(f.read_text(errors='replace') for f in sorted(c.data.glob('log-*')))


def wait_for(cond, secs=60):
    t = time.time()
    while time.time() - t < secs:
        if cond():
            return True
        time.sleep(0.5)
    return False


def generation(c):
    return status(c).get('generation')


def settle(c, quiet=6.0, secs=90):
    """Wait until no recertification is pending or running: the generation has not moved for `quiet` seconds and nothing is recommended."""
    last, since = None, time.time()
    t0 = time.time()
    while time.time() - t0 < secs:
        st = status(c)
        key = st.get('generation')
        if key != last:
            last, since = key, time.time()
        elif time.time() - since >= quiet and st.get('recertify_recommended') is False:
            return True
        time.sleep(0.5)
    return False


def gen_rows(start, n):
    return f"INSERT INTO t SELECT {start} + g, ARRAY[g, g % 7, g % 3, (g * 7) % 11]::real[]::vector FROM generate_series(1, {n}) g"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True)
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5811)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    plant = out / 'cover_plant'
    r = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(repo / 'src'), str(repo / 'test/unit/cover_plant.c'), '-o', str(plant)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    assert r.returncode == 0, r.stdout
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'ar', out, args.port)
        c.auto_recert = True
        c.extra_opts = '-c autovacuum=off -c cuvs.cover_recertify_min_interval=1 -c cuvs.cover_recertify_max_age=100 -c log_min_messages=debug1 -c max_connections=60'
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')
            c.scalar('CREATE TABLE t(id int, v vector(4))')
            c.scalar('CREATE INDEX ti ON t USING pg_cuvs_hnsw (v vector_l2_ops)')
            path = c.data / c.scalar("SELECT pg_relation_filepath('ti')")

            # A1
            c.scalar(gen_rows(0, 6000))
            ok = wait_for(lambda: status(c).get('generation', 1) > 1, 60)
            st = status(c)
            check('A1 the workers recertified while the load went on: a new generation, in force', ok and st.get('guarantee_in_force') is True, str(st))
            check('A1 the log has the lines of the workers', 'by background worker: done' in log_text(c), log_text(c)[-400:])
            # the last seconds of a load can be left past the threshold until the next sweep (10 s here)
            check('A1 and the workers settle within a sweep after the load, with the keys since the certification within the threshold',
                  settle(c) and status(c).get('keys_since_certification', 10 ** 9) * 20 <= max(20, status(c).get('base_rows', 0)), str(status(c)))

            # A2
            covered = status(c).get('base_rows')
            g0 = generation(c)
            room = covered * 5 // 100 - status(c).get('keys_since_certification', 0)	# keys that still fit under the threshold
            below = max(1, room // 2)
            c.scalar(gen_rows(100000, below))
            time.sleep(12)		# one sweep period and a little more, well inside max_age (100 s)
            check('A2 below the threshold (%d keys, half of what is left under 5 percent) no worker is started: same generation' % below,
                  generation(c) == g0 and status(c).get('recertify_recommended') is False, str(status(c)))
            c.scalar(gen_rows(200000, max(2, covered * 6 // 100)))
            ok = wait_for(lambda: generation(c) != g0, 30)
            check('A2 past the threshold a worker recertifies at the next sweep without a call: a new generation', ok, str(status(c)))
            check('A2 and the workers settle', settle(c))

            # A2b: the keys the certification covered (the rows the walk listed) are not keys since the certification. The graph is the one of C1 of
            # test_cover_recert_pg (small m, clustered rows: it has rows the entry cannot reach), planted as an uncertified index and certified by hand.
            clus = "(SELECT array_agg((((x % 25) * 7 + d * 3) % 41 + (random() - 0.5) * 0.05)::real) FROM generate_series(1, 8) d)::vector"
            c.scalar('CREATE TABLE w8(id int, v vector(8))')
            c.scalar(f"SELECT setseed(0.42); INSERT INTO w8 SELECT x, {clus} FROM generate_series(1, 4000) x")
            c.scalar('CREATE INDEX w8i ON w8 USING hnsw (v vector_l2_ops) WITH (m = 4, ef_construction = 8)')
            c.scalar("UPDATE pg_class SET relam = (SELECT oid FROM pg_am WHERE amname = 'pg_cuvs_hnsw') WHERE relname = 'w8i'")
            c.scalar("UPDATE pg_index SET indclass = (SELECT oid::text::oidvector FROM pg_opclass WHERE opcname = 'vector_l2_ops' AND opcmethod = (SELECT oid FROM pg_am WHERE amname = 'pg_cuvs_hnsw')) WHERE indexrelid = 'w8i'::regclass")
            w8path = c.data / c.scalar("SELECT pg_relation_filepath('w8i')")
            c.stop()
            rpl = subprocess.run([str(plant), str(w8path), 'uncertified'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            check('A2b the uncertified state was planted', rpl.returncode == 0, rpl.stdout)
            c.start()
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('w8i')::text")
            stw = status(c, 'w8i')
            check('A2b the graph has rows the entry cannot reach, and the index is certified', rc == 0 and stw.get('base_unreached', 0) > 0 and stw.get('class') == 'certified', f'{res} {stw}')
            thr = -(-stw.get('base_rows', 0) * 5 // 100)
            asked_w = log_text(c).count('asked for a background recertification')
            c.scalar(f"INSERT INTO w8 SELECT 800000 + x, {clus} FROM generate_series(1, {max(1, thr - 1)}) x")
            time.sleep(4)
            stw2 = status(c, 'w8i')
            check('A2b just below the threshold (%d keys of %d, with %d listed rows) no worker is asked for: the listed rows are not counted' % (max(1, thr - 1), thr, stw.get('base_unreached', 0)),
                  log_text(c).count('asked for a background recertification') == asked_w and stw2.get('generation') == stw.get('generation'), f'{stw2}')

            # A3
            c.scalar("ALTER SYSTEM SET cuvs.cover_auto_recertify = off")
            c.scalar("SELECT pg_reload_conf()")
            time.sleep(1.5)
            covered = status(c).get('base_rows')
            g1 = generation(c)
            c.scalar(gen_rows(300000, max(5, covered // 10)))
            time.sleep(6)
            st = status(c)
            check('A3 with cuvs.cover_auto_recertify off nothing happens by itself and the recertification is recommended', generation(c) == g1 and st.get('recertify_recommended') is True, str(st))
            c.scalar("ALTER SYSTEM RESET cuvs.cover_auto_recertify")
            c.scalar("SELECT pg_reload_conf()")

            # A4: no worker slot
            c.extra_opts += ' -c max_worker_processes=1'
            c.restart()
            g2 = generation(c)
            rc, res = c.q(gen_rows(400000, 300))
            check('A4 without a free worker slot the insert goes through', rc == 0, res)
            time.sleep(4)
            check('A4 and nothing was recertified meanwhile', generation(c) == g2, str(status(c)))
            rc, res = c.q('VACUUM t')
            check('A4 the VACUUM recertifies', rc == 0 and generation(c) != g2 and status(c).get('guarantee_in_force') is True, f'{res} {status(c)}')
            c.extra_opts = c.extra_opts.replace(' -c max_worker_processes=1', '')
            c.restart()

            # A5
            errs = []

            def writer(k):
                for i in range(30):
                    rc, res = c.q(gen_rows(1000000 + k * 100000 + i * 100, 100))
                    if rc != 0:
                        errs.append(res)
            threads = [threading.Thread(target=writer, args=(k,)) for k in range(8)]
            for th in threads:
                th.start()
            for th in threads:
                th.join()
            check('A5 eight sessions inserting at once all succeed', not errs, str(errs[:2]))
            settle(c, quiet=12)		# a sweep (10 s) has come and gone with nothing left to do
            c.scalar('CHECKPOINT')
            st = status(c)
            check('A5 the ledger reads afterwards', st.get('ledger_ok') is True, str(st))
            pages = ti.read_pages(path)
            keys, kinds, order, d = tr.ledger_all(pages)
            have = {(k[1], k[2]) for k in keys}
            unreached, _ = tr.unreachable_tids(path)
            check('A5 and every row the graph cannot reach has a key', unreached <= have, f'{len(unreached - have)} missing')

            # A6: a worker that could not do its work (it did not get the table's lock) leaves the request wanted, and the next worker starts min_interval
            # after that one ended: the next insert after the interval, or a VACUUM, picks it up
            c.extra_opts = c.extra_opts.replace('cuvs.cover_recertify_min_interval=1 ', 'cuvs.cover_recertify_min_interval=10 ')		# a command-line value is not moved by ALTER SYSTEM
            c.restart()
            settle(c, quiet=2)

            def hold_back():
                """A transaction holds SHARE UPDATE EXCLUSIVE on the table (inserts are not blocked by it): the worker that is asked for cannot lock the table."""
                holder = dg.Session(c)
                holder.run('BEGIN')
                holder.run('LOCK TABLE t IN SHARE UPDATE EXCLUSIVE MODE')
                covered = status(c).get('base_rows')
                c.scalar(gen_rows(2000000 + int(time.time()) % 1000000 * 10, max(2, covered * 6 // 100)))
                time.sleep(14)			# a sweep (every 10 s) starts the worker, which polls for the lock for 2 s and ends, deferred
                holder.run('COMMIT')
                holder.close()
            g3 = generation(c)
            hold_back()
            st = status(c)
            check('A6 a worker that could not lock the table ends deferred (logged); nothing is recertified: same generation, recommended, and the status says what picks it up',
                  generation(c) == g3 and st.get('recertify_recommended') is True and 'next sweep' in st.get('pending', '') and 'next VACUUM' in st.get('pending', '')
                  and 'by background worker: deferred' in log_text(c), f'deferred_logged={"by background worker: deferred" in log_text(c)} g3={g3} {st}')
            ok = wait_for(lambda: generation(c) != g3, 60)
            check('A6 a sweep after the interval starts the worker: a new generation, nothing recommended', ok and status(c).get('recertify_recommended') is False, str(status(c)))
            settle(c, quiet=2)
            g4 = generation(c)
            hold_back()
            check('A6 held back again: same generation, recommended', generation(c) == g4 and status(c).get('recertify_recommended') is True, str(status(c)))
            rc, res = c.q('VACUUM t')
            st = status(c)
            check('A6 a VACUUM picks it up: a new generation, nothing recommended', rc == 0 and generation(c) != g4 and st.get('recertify_recommended') is False, f'{res} {st}')
            settle(c, quiet=2)

            # A10: a worker that did its work does not make the next one wait for the interval: two bursts, the second one 2.5 s after the first was served
            # (the first worker has ended by then: it waits 1 s for a request), well inside cuvs.cover_recertify_min_interval (10 s)
            c.scalar('CREATE TABLE b2(id int, v vector(4))')
            c.scalar('CREATE INDEX b2i ON b2 USING pg_cuvs_hnsw (v vector_l2_ops)')
            c.scalar("INSERT INTO b2 SELECT g, ARRAY[g, g % 7, g % 3, (g * 7) % 11]::real[]::vector FROM generate_series(1, 8000) g")
            c.q("SELECT pg_cuvs_cover_recertify('b2i')::text")
            c.scalar("INSERT INTO b2 SELECT 100000 + g, ARRAY[g, g % 5, g % 2, (g * 3) % 13]::real[]::vector FROM generate_series(1, 6000) g")
            ok1 = wait_for(lambda: status(c, 'b2i').get('recertify_recommended') is False, 25)
            time.sleep(2.5)
            g_b2 = status(c, 'b2i').get('generation')
            c.scalar("INSERT INTO b2 SELECT 200000 + g, ARRAY[g, g % 5, g % 2, (g * 3) % 13]::real[]::vector FROM generate_series(1, 6000) g")
            ok2 = wait_for(lambda: status(c, 'b2i').get('generation') != g_b2 and status(c, 'b2i').get('recertify_recommended') is False, 25)
            check('A10 the first burst was served, and the second one, 2.5 s later, is served at the sweep after it (within 25 s)', ok1 and ok2, f'ok1={ok1} ok2={ok2} {status(c, "b2i")}')

            # A9: one statement inserts a burst; when it is over nothing is left past the threshold, within the interval (10 s) and one pass
            c.scalar('CREATE TABLE b(id int, v vector(4))')
            c.scalar('CREATE INDEX bi ON b USING pg_cuvs_hnsw (v vector_l2_ops)')
            c.scalar("INSERT INTO b SELECT g, ARRAY[g, g % 7, g % 3, (g * 7) % 11]::real[]::vector FROM generate_series(1, 8000) g")
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('bi')::text")
            check('A9 the index of 8000 rows is certified', rc == 0 and '"result": "done"' in res, res)
            c.scalar("INSERT INTO b SELECT 100000 + g, ARRAY[g, g % 5, g % 2, (g * 3) % 13]::real[]::vector FROM generate_series(1, 6000) g")
            t0 = time.time()
            ok = wait_for(lambda: status(c, 'bi').get('recertify_recommended') is False and status(c, 'bi').get('keys_since_certification', 10 ** 9) * 20 <= max(20, status(c, 'bi').get('base_rows', 0)), 25)
            check('A9 after the burst nothing stays past the threshold: within 25 s of the end of the statement (one sweep and the work) the status is not recommending (it stayed 80 s with 43 percent before)',
                  ok, f'{round(time.time() - t0, 1)}s {status(c, "bi")}')
        finally:
            c.drop()
    finally:
        (out / 'transcript.txt').write_text('\n'.join(dg.TRANSCRIPT) + '\n')
        shutil.rmtree(root, ignore_errors=True)
        for d in out.glob('data-*'):
            shutil.rmtree(d, ignore_errors=True)
    result = {'failures': dg.FAILURES, 'verdict': 'FAIL' if dg.FAILURES else 'PASS',
              'not_run': 'requests from several databases at once, a server shutdown while a worker runs, a worker that meets a damaged ledger'}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 1 if dg.FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
