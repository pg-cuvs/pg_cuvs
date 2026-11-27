#!/usr/bin/env python3
"""The emergency action and the marker it leaves ("break-glass"), on real PostgreSQL servers (CPU only).

  G1 pg_cuvs_cover_breakglass() is plain SQL: with the OLDER library installed it runs (where every statement that opens an
     index fails), points the handler at the older C symbol and creates the marker pg_cuvs_cover_breakglass_marker(), an
     extension member; it needs superuser; a second call changes nothing;
  G2 back with the new library: whatever the handler names (also after the unsupported hand-made CREATE OR REPLACE FUNCTION back
     to the new symbol), the status function says guarantee_in_force false and says why, scans of an index that carries a mark are
     answered from the heap (the older-symbol path of the new library too), VACUUM does not recertify, pg_cuvs_cover_recertify() declines;
  G3 ALTER EXTENSION UPDATE TO '0.9.2' and TO '0.9.3' with the new library installed removes the marker, names the new symbol again,
     leaves the index LOST (downgrade) until it is recertified, and then the guarantee is in force and scans merge again;
  G4 the marker is not in a schema-only pg_dump and a restore of it has none; pg_upgrade keeps the marker together with the index
     files it copies (pg_upgrade from this PostgreSQL to itself).
Indexes that need the CAGRA daemon (built over rows) are not part of this runner; the ones here are built on empty tables.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402

check = dg.check
NEW_SYMBOL = dg.NEW_SYMBOL
OLD_SYMBOL = dg.OLD_SYMBOL


def status(c, idx):
    return json.loads(c.scalar(f"SELECT pg_cuvs_hnsw_cover_status('{idx}')::text"))


def handler_symbol(c):
    return c.scalar("SELECT prosrc FROM pg_proc WHERE proname = 'pg_cuvs_hnsw_handler'")


def marker_exists(c, db=None):
    return c.scalar("SELECT to_regprocedure('pg_cuvs_cover_breakglass_marker()') IS NOT NULL") == 't'


def prepare(c):
    c.scalar('CREATE EXTENSION vector')
    c.scalar('CREATE EXTENSION pg_cuvs')
    c.scalar('CREATE TABLE t(id int, v vector(3))')
    c.scalar('CREATE INDEX ti ON t USING pg_cuvs_hnsw (v vector_l2_ops)')
    c.scalar("INSERT INTO t SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 300) g")


def breakglass_with_old_library(o, c):
    c.stop()
    o.install('old')
    c.start()
    rc, res = c.q('SELECT count(*) FROM t')
    precondition = rc != 0 and 'pg_cuvs_hnsw_handler_c1p' in res
    rc2, res2 = c.q('SELECT pg_cuvs_cover_breakglass()')
    return precondition, rc2, res2


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True)
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5571)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'bg', out, args.port)
        try:
            o.install('new')
            c.start()
            prepare(c)
            st = status(c, 'ti')
            check('G0 before: certified, in force, ledger ok, no marker', st.get('class') == 'certified' and st.get('guarantee_in_force') is True
                  and st.get('ledger_ok') is True and not marker_exists(c), str(st))
            c.scalar('CREATE ROLE rr LOGIN')
            rc, res = c.q('SELECT pg_cuvs_cover_breakglass()', role='rr')
            check('G1 a role that is not a superuser may not run it', rc != 0 and 'permission denied for function pg_cuvs_cover_breakglass' in res, res)

            precondition, rc, res = breakglass_with_old_library(o, c)
            check('G1 with the older library a statement that opens the index fails (the situation the action is for)', precondition)
            check('G1 pg_cuvs_cover_breakglass() runs with the older library installed and says what it did', rc == 0 and 'older C symbol' in res, res)
            check('G1 the handler names the older C symbol', handler_symbol(c) == OLD_SYMBOL, handler_symbol(c))
            check('G1 the marker exists and is a member of the extension',
                  marker_exists(c) and c.scalar("SELECT count(*) FROM pg_depend d WHERE d.objid = 'pg_cuvs_cover_breakglass_marker()'::regprocedure "
                                                "AND d.deptype = 'e' AND d.refobjid = (SELECT oid FROM pg_extension WHERE extname = 'pg_cuvs')") == '1')
            rc, res = c.q('SELECT pg_cuvs_cover_breakglass()')
            check('G1 a second call changes nothing and does not fail', rc == 0 and handler_symbol(c) == OLD_SYMBOL, res)
            rc, res = c.q("INSERT INTO t SELECT 1000 + g, ARRAY[g, 2, 2]::real[]::vector FROM generate_series(1, 100) g")
            check('G1 the older library inserts again', rc == 0, res)
            rc, res = c.q('DELETE FROM t WHERE id % 5 = 0')
            rc2, res2 = c.q('VACUUM t')
            check('G1 and deletes and vacuums', rc == 0 and rc2 == 0, res + res2)

            # G2
            c.stop()
            o.install('new')
            c.start()
            st = status(c, 'ti')
            check('G2 new library, handler on the older symbol: not in force, marker reported, the way out is named',
                  st.get('guarantee_in_force') is False and st.get('breakglass_marker') is True and 'UPDATE TO' in st.get('pending', ''), str(st))
            rc, res = c.q("SET enable_seqscan = off; SELECT id FROM t ORDER BY v <-> '[3,3,0]' LIMIT 5")
            check('G2 and a scan through the older symbol, by the new library, is answered from the heap too, with the notice that names the emergency rollback (review af8d48e1 R6-2)',
                  rc == 0 and 'answers from the heap' in res and 'emergency rollback' in res, res)
            c.scalar(f"CREATE OR REPLACE FUNCTION pg_cuvs_hnsw_handler(internal) RETURNS index_am_handler AS '$libdir/pg_cuvs', '{NEW_SYMBOL}' LANGUAGE C")
            st = status(c, 'ti')
            check('G2 after the unsupported hand-made switch back to the new symbol: still not in force (this is what the marker is for)',
                  st.get('handler_symbol') == NEW_SYMBOL and st.get('class') == 'certified' and st.get('guarantee_in_force') is False
                  and st.get('breakglass_marker') is True, str(st))
            rc, res = c.q("SET enable_seqscan = off; SELECT id FROM t ORDER BY v <-> '[3,3,0]' LIMIT 5")
            check('G2 a scan is answered from the heap and the notice names the emergency rollback',
                  rc == 0 and 'answers from the heap' in res and 'emergency rollback' in res, res)
            # eight rows: the pair of equal vectors (ids 1 and 1001) at ranks 7 and 8 is inside, so the set is the same whatever order ties come in
            ids_index = c.scalar("SET enable_seqscan = off; SELECT string_agg(id::text, ',' ORDER BY id) FROM (SELECT id FROM t ORDER BY v <-> '[3,3,0]' LIMIT 8) s").splitlines()[-1]
            ids_heap = c.scalar("SET enable_indexscan = off; SET enable_bitmapscan = off; SELECT string_agg(id::text, ',' ORDER BY id) FROM (SELECT id FROM t ORDER BY v <-> '[3,3,0]' LIMIT 8) s").splitlines()[-1]
            check('G2 and the answer is the exact one', ids_index == ids_heap, f'{ids_index} vs {ids_heap}')
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('ti')::text")
            check('G2 pg_cuvs_cover_recertify declines and says why', rc == 0 and '"result": "not_applicable"' in res and 'break-glass' in res, res)
            c.scalar('DELETE FROM t WHERE id % 7 = 0')
            c.scalar('VACUUM t')
            st = status(c, 'ti')
            check('G2 VACUUM takes the stored state off (lost, vacuum) and does not recertify', st.get('class') == 'lost' and st.get('stored_reason') == 4, str(st))

            # G3
            rc, res = c.q("ALTER EXTENSION pg_cuvs UPDATE TO '0.9.2'")
            check('G3 UPDATE TO 0.9.2 with the new library installed runs', rc == 0, res)
            rc, res = c.q("ALTER EXTENSION pg_cuvs UPDATE TO '0.9.3'")
            check('G3 and UPDATE TO 0.9.3', rc == 0, res)
            check('G3 the marker is gone', not marker_exists(c))
            check('G3 the handler names the new symbol', handler_symbol(c) == NEW_SYMBOL, handler_symbol(c))
            st = status(c, 'ti')
            # it was LOST (vacuum) already when the downgrade ran, which only takes CERTIFIED ones off; reason 10 is the downgrade test's
            check('G3 the index is lost and not in force', st.get('class') == 'lost' and st.get('stored_reason') in (4, 10) and st.get('guarantee_in_force') is False, str(st))
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('ti')::text")
            st = status(c, 'ti')
            check('G3 recertified: certified, in force, no marker field', rc == 0 and '"result": "done"' in res and st.get('guarantee_in_force') is True
                  and 'breakglass_marker' not in st, f'{res} {st}')
            rc, res = c.q("SET enable_seqscan = off; SELECT id FROM t ORDER BY v <-> '[3,3,0]' LIMIT 5")
            check('G3 scans merge again (no heap notice)', rc == 0 and 'answers from the heap' not in res, res)
            check('G3 pg_cuvs_cover_breakglass() is there again to be used', c.scalar("SELECT to_regprocedure('pg_cuvs_cover_breakglass()') IS NOT NULL") == 't')

            # G5: an index committed after the downgrade script ran (review a00c214a N-12): the downgrade cannot list what is not yet
            # committed, so the next UPDATE TO 0.9.3 takes it off (the first step of that script)
            creator = dg.Session(c)
            creator.run('BEGIN')
            creator.run('CREATE TABLE late(id int, v vector(3))')
            creator.run('CREATE INDEX late_i ON late USING pg_cuvs_hnsw (v vector_l2_ops)')
            rc, res = c.q("ALTER EXTENSION pg_cuvs UPDATE TO '0.9.2'")
            check('G5 the downgrade does not wait for the open transaction', rc == 0, res)
            creator.run('COMMIT')
            creator.close()
            c.scalar('CHECKPOINT')
            lpath = c.data / c.scalar("SELECT pg_relation_filepath('late_i')")
            import test_cover_insert_pg as ti
            check('G5 the late index is CERTIFIED in its file (the downgrade could not see it)', ti.descriptor(ti.read_pages(lpath))['state'] == 1)
            rc, res = c.q("ALTER EXTENSION pg_cuvs UPDATE TO '0.9.3'")
            c.scalar('CHECKPOINT')
            dl = ti.descriptor(ti.read_pages(lpath))
            check('G5 UPDATE TO 0.9.3 takes it off (LOST, reason downgrade)', rc == 0 and dl['state'] == 2 and dl['reason'] == 10 and dl['crc_ok'], f'{res} {dl}')
            st = status(c, 'late_i')
            check('G5 and the status function says so', st.get('class') == 'lost' and st.get('guarantee_in_force') is False, str(st))

            # G4: dump and restore; the marker made again first
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('ti')::text")
            check('G4 setup: the index is certified again', rc == 0 and status(c, 'ti').get('guarantee_in_force') is True, res)
            precondition, rc, res = breakglass_with_old_library(o, c)
            check('G4 the action again, for the dump', rc == 0, res)
            c.stop()
            o.install('new')
            c.start()
            c.scalar(f"CREATE OR REPLACE FUNCTION pg_cuvs_hnsw_handler(internal) RETURNS index_am_handler AS '$libdir/pg_cuvs', '{NEW_SYMBOL}' LANGUAGE C")
            check('G4 marker present before the dump', marker_exists(c))
            dump = subprocess.run([str(o.bin / 'pg_dump'), '-s', '-h', str(c.sock), '-p', str(c.port), '-U', 'postgres', 'postgres'],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            check('G4 pg_dump works and does not contain the marker', dump.returncode == 0 and 'pg_cuvs_cover_breakglass_marker' not in dump.stdout, dump.stderr[-300:])
            c.scalar('CREATE DATABASE restored')
            ps = subprocess.run(c.psql_cmd()[:-1] + ['restored', '-v', 'ON_ERROR_STOP=1'], input=dump.stdout, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            check('G4 the dump restores into a new database', ps.returncode == 0, ps.stdout[-300:])
            chk = subprocess.run(c.psql_cmd()[:-1] + ['restored', '-c',
                                  "SELECT to_regprocedure('pg_cuvs_cover_breakglass_marker()') IS NULL, (SELECT prosrc FROM pg_proc WHERE proname = 'pg_cuvs_hnsw_handler'), "
                                  "(SELECT (pg_cuvs_hnsw_cover_status('ti')::jsonb ->> 'guarantee_in_force'))"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            check('G4 the restored database has no marker, names the new symbol, and its (empty, rebuilt) index is in force',
                  chk.stdout.strip() == f't|{NEW_SYMBOL}|true', chk.stdout)

            # G4: pg_upgrade from this cluster to a new one of the same version
            c.stop()
            new = dg.Cluster(o, 'bg2', out, args.port + 1)
            for cl in (c, new):
                with open(cl.data / 'postgresql.conf', 'a') as f:
                    f.write("\nshared_preload_libraries = 'pg_cuvs'\n")
            up = subprocess.run([str(o.bin / 'pg_upgrade'), '-b', str(o.bin), '-B', str(o.bin), '-d', str(c.data), '-D', str(new.data),
                                 '-p', str(args.port + 10), '-P', str(args.port + 11), '-s', tempfile.mkdtemp(prefix='pgu')],
                                cwd=str(out), env={**os.environ, 'PGUSER': 'postgres'}, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            logs = ''.join(f.read_text(errors='replace')[-1500:] for f in sorted(out.glob('data-bg2/pg_upgrade_output.d/*/log/pg_upgrade_server.log')))
            check('G4 pg_upgrade succeeds', up.returncode == 0, up.stdout[-300:] + ' SERVER LOG: ' + logs)
            if up.returncode == 0:
                new.start()
                st = status(new, 'ti')
                check('G4 the upgraded cluster still has the marker, and the index it copied is not in force',
                      marker_exists(new) and st.get('guarantee_in_force') is False and st.get('breakglass_marker') is True, str(st))
                new.stop()
        finally:
            c.drop()
    finally:
        (out / 'transcript.txt').write_text('\n'.join(dg.TRANSCRIPT) + '\n')
        shutil.rmtree(root, ignore_errors=True)
        for d in out.glob('data-*'):
            shutil.rmtree(d, ignore_errors=True)
    result = {'failures': dg.FAILURES, 'verdict': 'FAIL' if dg.FAILURES else 'PASS',
              'not_run': 'indexes built over rows (need the CAGRA daemon / GPU), so a pg_dump with data (the restore would rebuild them); '
                         'per-database extension versions in one cluster at different stages'}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 1 if dg.FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
