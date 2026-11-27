#!/usr/bin/env python3
"""A LOST(vacuum) index on a physical standby (CPU only, a build with -DCUVS_COVER_RECERT_TEST_GATE for --new-lib).

A standby cannot see whether the primary's VACUUM still runs, so it merges the graph's answers and the ledger while the stored reason is vacuum
(a user decision); the primary's recertification reaches it through the WAL.

  SB1 the standby replays a certified index whose rows are all ledger keys;
  SB2 while the primary's VACUUM is held after it took the guarantee off, the standby reads the index as LOST (vacuum) and merges: with
      hnsw.ef_search = 1 the 50 rows of the exact ranking, which only the ledger can give, and no heap notice;
  SB3 once that VACUUM ends, its recertification reaches the standby: certified again;
  SB4 a VACUUM that is cancelled after it took the guarantee off: the primary ranks the heap (its VACUUM does not run), the standby still merges,
      and the primary's next sweep recertifies the index, which reaches the standby; the time the standby merged after the cancel is reported.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402

check = dg.check


def clustered(table, n, dim=8):
    return (f"INSERT INTO {table} SELECT x, (SELECT array_agg((((x % 25) * 7 + d * 3) % 41 + (((x * 7919 + d * 104729) % 1000) / 1000.0 - 0.5) * 0.05)::real) "
            f"FROM generate_series(1, {dim}) d)::vector FROM generate_series(1, {n}) x")


def wait_for(cond, secs):
    t = time.time()
    while time.time() - t < secs:
        if cond():
            return time.time() - t
        time.sleep(0.2)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True, help='a build with -DCUVS_COVER_RECERT_TEST_GATE')
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5961)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    gate = out / 'gate'
    gate.mkdir()
    os.environ['CUVS_RECERT_GATE_DIR'] = str(gate)
    root = out / 'pgroot'
    p = s = None
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        o.install('new')
        p = dg.Cluster(o, 'pri', out, args.port)
        p.auto_recert = True
        p.extra_opts = '-c autovacuum=off -c cuvs.cover_recertify_max_age=100 -c cuvs.cover_recertify_min_interval=1 -c wal_level=replica'
        p.start()
        p.scalar('CREATE EXTENSION vector')
        p.scalar('CREATE EXTENSION pg_cuvs')
        # the standby: a base backup of the primary over its socket, started as a hot standby
        s = dg.Cluster(o, 'sby', out, args.port + 1)
        shutil.rmtree(s.data)
        r = subprocess.run([str(o.bin / 'pg_basebackup'), '-h', str(p.sock), '-p', str(p.port), '-U', 'postgres', '-D', str(s.data), '-R', '-X', 'stream'],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        assert r.returncode == 0, r.stdout
        s.extra_opts = '-c hot_standby=on'
        s.start()

        p.scalar('CREATE TABLE sb_flush(i int)')

        def caught_up():
            # a held VACUUM's records are not flushed yet, and a standby gets only flushed WAL: a small committed write flushes everything before it
            p.scalar('INSERT INTO sb_flush VALUES (1)')
            lsn = p.scalar('SELECT pg_current_wal_insert_lsn()').splitlines()[-1]
            return wait_for(lambda: s.scalar(f"SELECT pg_last_wal_replay_lsn() >= '{lsn}'::pg_lsn").splitlines()[-1] == 't', 60) is not None

        def status(c):
            return json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('ti')::text").splitlines()[-1])

        q50 = "SELECT id FROM t ORDER BY v <-> (SELECT v FROM t WHERE id = 1) LIMIT 50"

        def merged(c):
            rc, res = c.q(f'SET enable_seqscan = off; SET hnsw.ef_search = 1; {q50}')
            rco, reso = c.q(f'SET enable_indexscan = off; SET enable_bitmapscan = off; {q50}')
            ids = [x for x in res.split('\n') if x.isdigit()]
            return rc == 0 and 'answers from the heap' not in res and len(ids) == 50 and ids == [x for x in reso.split('\n') if x.isdigit()], res[-200:]

        # SB1
        p.scalar('CREATE TABLE t(id int, v vector(8))')
        p.scalar('CREATE INDEX ti ON t USING pg_cuvs_hnsw (v vector_l2_ops)')
        p.scalar(clustered('t', 400))					# every row is a ledger key
        p.scalar('DELETE FROM t WHERE id % 5 = 0')
        check('SB1 the standby caught up with the primary', caught_up())
        st = status(s)
        check('SB1 it reads the index as certified with the keys of the rows', st.get('class') == 'certified' and st.get('ledger_keys') == 400, str(st))

        def held_vacuum():
            for f in ('vacuum_reached', 'vacuum_go'):
                (gate / f).unlink(missing_ok=True)
            (gate / 'vacuum_armed').write_text('')
            box = []
            th = threading.Thread(target=lambda: box.append(p.q('VACUUM t')))
            th.start()
            reached = wait_for(lambda: (gate / 'vacuum_reached').exists(), 30) is not None
            return th, box, reached

        # SB2, SB3
        th, box, reached = held_vacuum()
        up = caught_up()
        st = status(s)
        ok, detail = merged(s)
        check('SB2 the primary\'s VACUUM is held after it took the guarantee off, and the standby replayed it: LOST (vacuum)',
              reached and up and st.get('class') == 'lost' and st.get('stored_reason') == 4, str(st))
        check('SB2 the standby merges the graph and the ledger while the reason is vacuum (50 rows of the exact ranking with hnsw.ef_search = 1, no heap notice)', ok, detail)
        (gate / 'vacuum_go').write_text('')
        th.join()
        (gate / 'vacuum_armed').unlink()
        check('SB3 the VACUUM ended with the index certified on the primary', box and box[0][0] == 0 and status(p).get('class') == 'certified', str(status(p)))
        check('SB3 and the standby reads it certified once it replayed that', caught_up() and status(s).get('class') == 'certified', str(status(s)))

        # SB4
        p.scalar('DELETE FROM t WHERE id % 7 = 0')
        th, box, reached = held_vacuum()
        p.q("SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE query = 'VACUUM t'")
        th.join(60)
        (gate / 'vacuum_armed').unlink()
        t_cancel = time.time()
        stp = status(p)
        rcp, resp = p.q(f'SET enable_seqscan = off; {q50}')
        check('SB4 the VACUUM was cancelled after it took the guarantee off: the primary reads LOST (vacuum) and ranks the heap, with the notice',
              reached and box and box[0][0] != 0 and stp.get('class') == 'lost' and stp.get('stored_reason') == 4 and 'answers from the heap' in resp, f'{box} {stp} {resp[-200:]}')
        up = caught_up()
        sts = status(s)
        # the recertification of SB3 left no key (every row is reached), so the merge here is the graph's own answer: what tells it from the heap
        # ranking the primary gives is the notice (SB2 showed the merge with ledger keys on the standby)
        rcs, ress = s.q(f'SET enable_seqscan = off; {q50}')
        check('SB4 the standby, which cannot see that the VACUUM ended, does not rank the heap while the reason is vacuum (no notice)',
              up and sts.get('class') == 'lost' and sts.get('stored_reason') == 4 and rcs == 0 and 'answers from the heap' not in ress, f'{sts} {ress[-200:]}')
        back = wait_for(lambda: status(s).get('class') == 'certified', 60)
        window = None if back is None else round(time.time() - t_cancel, 1)
        check('SB4 the primary\'s next sweep recertifies the index and the standby reads it certified: it merged for %s s after the cancel (sweep period 10 s here)' % window,
              back is not None, str(status(s)))
    finally:
        for c in (s, p):
            try:
                if c is not None:
                    c.drop()
            except Exception:
                pass
        (out / 'transcript.txt').write_text('\n'.join(dg.TRANSCRIPT) + '\n')
        shutil.rmtree(root, ignore_errors=True)
        for d in out.glob('data-*'):
            shutil.rmtree(d, ignore_errors=True)
    result = {'failures': dg.FAILURES, 'verdict': 'FAIL' if dg.FAILURES else 'PASS'}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 1 if dg.FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
