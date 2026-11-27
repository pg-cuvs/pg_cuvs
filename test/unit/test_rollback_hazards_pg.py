#!/usr/bin/env python3
"""What an unplanned rollback does to a database with REAL pg_cuvs_hnsw indexes, measured (V+D decision, 2026-11-16).

The indexes here are made by CREATE INDEX USING pg_cuvs_hnsw over an empty table and then filled by inserts, so their
catalog dependencies are the real ones (no catalog edits). The old library is a build that exports only the old handler
symbol; SQL stays at 0.9.3. Measured, in this order:

  R1 symptoms: every statement that opens the index fails; COPY t TO STDOUT works; pg_dump of the table fails and the
     dump without it works; a table without such an index is untouched;
  R2 autovacuum cannot vacuum the table: the error repeats, dead tuples stay, last_autovacuum stays empty;
  R3 the anti-wraparound autovacuum cannot either: with autovacuum_freeze_max_age at its minimum and the age of the table
     driven past it, the age keeps growing while a table without the index is frozen. VACUUM (FREEZE) fails by hand;
  R4 the break-glass statement (one CREATE OR REPLACE FUNCTION with the old symbol) lets autovacuum and the wraparound
     vacuum run again and the age drops;
  R5 the way back: while the SQL names the old symbol the new library reports the guarantee as not in force (no
     matter what the descriptor says), and ALTER EXTENSION UPDATE TO '0.9.2' then '0.9.3' leaves every certified index
     LOST (reason downgrade).
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402
import test_cover_insert_pg as ti  # noqa: E402

check = dg.check
NEW_SYMBOL, OLD_SYMBOL = dg.NEW_SYMBOL, dg.OLD_SYMBOL


def log_text(c):
    return ''.join(p.read_text(errors='replace') for p in sorted(c.data.glob('log-*')))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True)
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5493)
    ap.add_argument('--xids', type=int, default=130000, help='transactions burnt for the wraparound check')
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    measured = {}
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'h', out, args.port)
        c.extra_opts = ('-c autovacuum_naptime=2s -c autovacuum_vacuum_cost_delay=0 -c log_min_messages=info -c log_autovacuum_min_duration=0 '
                        '-c vacuum_freeze_min_age=0 -c vacuum_freeze_table_age=0')
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')
            c.scalar('CREATE TABLE t(id int, v vector(3)) WITH (autovacuum_freeze_max_age = 100000, autovacuum_vacuum_threshold = 20)')
            c.scalar('CREATE INDEX ti ON t USING pg_cuvs_hnsw (v vector_l2_ops)')
            c.scalar("INSERT INTO t SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 500) g")
            c.scalar('CREATE TABLE plain_t(id int) WITH (autovacuum_freeze_max_age = 100000, autovacuum_vacuum_threshold = 20)')
            c.scalar('INSERT INTO plain_t SELECT g FROM generate_series(1, 500) g')
            # dead tuples while the new library is still there (under the old one DELETE cannot even open the index)
            c.scalar('DELETE FROM t WHERE id % 2 = 0')
            c.scalar('DELETE FROM plain_t WHERE id % 2 = 0')
            c.stop()
            o.install('old')
            c.start()

            # R1
            for label, sql in (('SELECT 1 FROM t', 'SELECT 1 FROM t LIMIT 1'), ('INSERT', "INSERT INTO t VALUES (1, '[1,2,3]')"),
                               ('DROP INDEX', 'DROP INDEX ti'), ('DROP TABLE', 'DROP TABLE t'),
                               ('DROP EXTENSION CASCADE', 'DROP EXTENSION pg_cuvs CASCADE'), ('VACUUM (FREEZE)', 'VACUUM (FREEZE) t')):
                rc, res = c.q(sql)
                check(f'R1 {label} fails', rc != 0 and 'could not find function' in res, res)
            rc, res = c.q('COPY t TO STDOUT')
            check('R1 COPY t TO STDOUT works and returns the rows', rc == 0 and len(res.splitlines()) == 250, res[:200])
            rc, res = c.q('COPY (SELECT * FROM t) TO STDOUT')
            check('R1 COPY (SELECT ...) fails', rc != 0, res)
            rc, res = c.q('SELECT count(*) FROM plain_t')
            check('R1 a table without such an index is untouched', rc == 0 and res == '250', res)
            pgdump = str(o.bin / 'pg_dump')
            base = [pgdump, '-h', str(c.sock), '-p', str(c.port), '-U', 'postgres', 'postgres']
            r = subprocess.run(base, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            check('R1 pg_dump of the database fails', r.returncode != 0 and 'could not find function' in r.stderr, r.stderr[:300])
            r = subprocess.run(base + ['-T', 't'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            check('R1 pg_dump without the table works', r.returncode == 0, r.stderr[:300])

            # R2: the dead tuples are there; autovacuum should vacuum both tables
            deadline = time.time() + 40
            while time.time() < deadline:
                txt = log_text(c)
                if txt.count('automatic vacuum of table "postgres.public.t"') >= 3:
                    break
                time.sleep(2)
            txt = log_text(c)
            attempts = txt.count('automatic vacuum of table "postgres.public.t"')
            check('R2 autovacuum tried the table repeatedly and failed', attempts >= 3 and 'could not find function' in txt, f'{attempts} attempts')
            attempts_before_burn = attempts
            st = c.scalar("SELECT coalesce(last_autovacuum::text, 'never') || '|' || n_dead_tup FROM pg_stat_user_tables WHERE relname = 't'")
            check('R2 last_autovacuum stays empty and the dead tuples stay', st.startswith('never|') and int(st.split('|')[1]) > 0, st)
            st2 = c.scalar("SELECT coalesce(last_autovacuum::text, 'never') FROM pg_stat_user_tables WHERE relname = 'plain_t'")
            check('R2 the table without the index is vacuumed', st2 != 'never', st2)
            measured['R2_failed_attempts'] = attempts

            # R3: burn transactions so that the age of both tables passes autovacuum_freeze_max_age (100000)
            script = out / 'burn.sql'
            script.write_text('SELECT txid_current();\n')
            subprocess.run([str(o.bin / 'pgbench'), '-n', '-h', str(c.sock), '-p', str(c.port), '-U', 'postgres', '-f', str(script),
                            '-c', '4', '-j', '2', '-t', str(args.xids // 4), 'postgres'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
            deadline = time.time() + 60
            age_t = age_plain = 0
            while time.time() < deadline:
                age_plain = int(c.scalar("SELECT age(relfrozenxid) FROM pg_class WHERE relname = 'plain_t'"))
                age_t = int(c.scalar("SELECT age(relfrozenxid) FROM pg_class WHERE relname = 't'"))
                if age_plain < 50000:
                    break
                time.sleep(2)
            check('R3 the table without the index was frozen by the wraparound autovacuum', age_plain < 50000, str(age_plain))
            check('R3 the table with the index was not: its age is past autovacuum_freeze_max_age', age_t > 100000, str(age_t))
            time.sleep(6)
            txt = log_text(c)
            wrap = txt.count('automatic vacuum of table "postgres.public.t"') - attempts_before_burn
            check('R3 autovacuum went on trying (and failing) on it after the age passed the limit', wrap >= 2, f'{wrap} more attempts')
            age_t2 = int(c.scalar("SELECT age(relfrozenxid) FROM pg_class WHERE relname = 't'"))
            check('R3 and the age is still past the limit after the attempts', age_t2 > 100000, str(age_t2))
            measured.update({'R3_age_t': age_t, 'R3_age_plain': age_plain, 'R3_wraparound_log_lines': wrap, 'R3_xids_burnt': args.xids})

            # R4: break-glass
            rc, res = c.q(f"CREATE OR REPLACE FUNCTION pg_cuvs_hnsw_handler(internal) RETURNS index_am_handler AS '$libdir/pg_cuvs', '{OLD_SYMBOL}' LANGUAGE C")
            check('R4 break-glass statement', rc == 0, res)
            deadline = time.time() + 60
            age_after = age_t2
            while time.time() < deadline:
                age_after = int(c.scalar("SELECT age(relfrozenxid) FROM pg_class WHERE relname = 't'"))
                if age_after < 50000:
                    break
                time.sleep(2)
            check('R4 the wraparound vacuum runs again and freezes the table', age_after < 50000, str(age_after))
            rc, res = c.q("INSERT INTO t SELECT 10000 + g, ARRAY[g, 3, 3]::real[]::vector FROM generate_series(1, 50) g")
            check('R4 inserts work again (no keys: the old library does not write the ledger)', rc == 0, res)
            measured['R4_age_after'] = age_after

            # R5: back to the new library with the SQL still naming the old symbol
            c.stop()
            o.install('new')
            c.start()
            st = ti_status(c, 't', 'ti')
            check('R5 the new library reports the index as not in force while the SQL names the old symbol',
                  st.get('class') == 'certified' and st.get('guarantee_in_force') is False and st.get('handler_symbol') == OLD_SYMBOL, str(st))
            rc, res = c.q("ALTER EXTENSION pg_cuvs UPDATE TO '0.9.2'")
            check('R5 resync step 1 (invalidates every certified index)', rc == 0, res)
            c.scalar('CHECKPOINT')
            rel = c.scalar("SELECT pg_relation_filepath('ti')")
            d = dg.read_descriptor(c, c.data / rel)
            check('R5 the descriptor is LOST (downgrade) and not a stale CERTIFIED', d['state'] == 2 and d['reason'] == 10 and d['crc_ok'], str(d))
            rc, res = c.q("ALTER EXTENSION pg_cuvs UPDATE TO '0.9.3'")
            check('R5 resync step 2', rc == 0 and c.symbol() == NEW_SYMBOL, res)
            st = ti_status(c, 't', 'ti')
            check('R5 after the resync the index is LOST, not certified', st.get('class') == 'lost', str(st))
        finally:
            c.drop()
    finally:
        (out / 'transcript.txt').write_text('\n'.join(dg.TRANSCRIPT) + '\n')
        shutil.rmtree(root, ignore_errors=True)
        for d in out.glob('data-*'):
            shutil.rmtree(d, ignore_errors=True)
    result = {'failures': dg.FAILURES, 'measured': measured, 'verdict': 'FAIL' if dg.FAILURES else 'PASS',
              'not_run': 'the real 2 billion transaction exhaustion (only the failing wraparound autovacuum is shown); builds over rows'}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 1 if dg.FAILURES else 0


def ti_status(c, table, index):
    rc, out = c.q(f"SELECT pg_cuvs_hnsw_cover_status('{index}')::text")
    assert rc == 0, out
    return json.loads(out)


if __name__ == '__main__':
    sys.exit(main())
