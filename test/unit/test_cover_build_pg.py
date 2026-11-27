#!/usr/bin/env python3
"""F2b: which metapage a pg_cuvs_hnsw build writes, on real PostgreSQL servers (CPU only, no GPU).

Only the builds that need no GPU can run here: an index over an empty table (REINDEX and TRUNCATE rebuild it the
same way) and the init fork of an UNLOGGED index. The builds over rows go through the same single finishing
function (hnsw_stage_metapage_tail) but need the CAGRA daemon; they are NOT RUN by this test.

  B1 an empty index of the new handler is CERTIFIED with an empty ledger at block 2, three blocks long;
  B2 REINDEX and TRUNCATE keep that;
  B3 an UNLOGGED empty index has the same state after a crash reset (the init fork carries the descriptor);
  B4 an index created while the SQL names the old symbol has no descriptor and no ledger page (LEGACY shape);
  B5 the connectivity record is still readable below the descriptor;
  B6 an index of an older build, loaded as LEGACY, is reported as legacy;
  B7 the downgrade takes the guarantee off the init fork of an UNLOGGED index too (a crash reset must not bring it back);
  B8 another session's temporary index is named in the downgrade error.
Reuses the relocated private installation of test_downgrade_pg.py (new and old pg_cuvs.so).
"""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402

check = dg.check


def status(c, index):
    rc, out = c.q(f"SELECT pg_cuvs_hnsw_cover_status('{index}')::text")
    assert rc == 0, out
    return json.loads(out)


def nblocks(c, index):
    return int(c.scalar(f"SELECT pg_relation_size('{index}') / 8192"))


def metapage_upper(c, index, fork='main'):
    c.scalar('CHECKPOINT')
    rel = c.scalar(f"SELECT pg_relation_filepath('{index}')")
    path = c.data / (rel if fork == 'main' else rel + '_init')
    return struct.unpack_from('<H', path.read_bytes()[:8192], 14)[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True)
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5487)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'b1', out, args.port)
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')
            c.scalar('CREATE TABLE e(id int, v vector(3))')
            c.scalar('CREATE INDEX ei ON e USING pg_cuvs_hnsw (v vector_l2_ops)')
            st = status(c, 'ei')
            check('B1 empty index is certified', st.get('class') == 'certified' and st.get('writable') is True, str(st))
            check('B1 ledger head is block 2, generation 1', st.get('ledger_head') == 2 and st.get('generation') == 1, str(st))
            check('B1 the index has three blocks (meta, first element page, ledger)', nblocks(c, 'ei') == 3, str(nblocks(c, 'ei')))
            check('B1 the metapage tail starts at pd_upper 7936', metapage_upper(c, 'ei') == 7936, str(metapage_upper(c, 'ei')))
            rc, res = c.q("SET enable_seqscan = off; SELECT count(*) FROM (SELECT id FROM e ORDER BY v <-> '[1,2,3]' LIMIT 5) x")
            check('B1 a scan of the empty index works', rc == 0 and res.endswith('0'), res)
            conn_new = c.scalar("SELECT pg_cuvs_hnsw_connectivity_status('ei')::text")
            check('B5 the connectivity record is read below the descriptor', '"not_recorded"' not in conn_new and 'connectivity' in conn_new or 'status' in conn_new, conn_new)

            c.scalar('REINDEX INDEX ei')
            st = status(c, 'ei')
            check('B2 REINDEX keeps a certified empty index of three blocks', st.get('class') == 'certified' and nblocks(c, 'ei') == 3, str(st))
            c.scalar("INSERT INTO e SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 300) g")
            st = status(c, 'ei')
            check('B2 pgvector inserts (entry point and insert page updates) leave the descriptor alone', st.get('class') == 'certified' and st.get('ledger_head') == 2, str(st))
            c.scalar('VACUUM e')	# 300 keys against the 0 rows the empty build covered: past the threshold, so this VACUUM recertifies
            gen_before = status(c, 'ei').get('generation')
            c.scalar('DELETE FROM e WHERE id % 3 = 0')
            c.scalar('VACUUM e')
            st = status(c, 'ei')
            check('B2 VACUUM after deletes takes the guarantee off and the same VACUUM recertifies it (F6b): certified again, the next ledger chain generation',
                  st.get('class') == 'certified' and gen_before is not None and st.get('generation') == gen_before + 1 and st.get('guarantee_in_force') is True, f'{gen_before} {st}')
            ledger_after_vacuum = st.get('ledger_head')
            c.scalar('CHECKPOINT')
            c.stop('immediate')
            c.start()
            st = status(c, 'ei')
            check('B2 and after crash recovery', st.get('class') == 'certified' and st.get('ledger_head') == ledger_after_vacuum and st.get('generation') == gen_before + 1, str(st))
            c.scalar('TRUNCATE e')
            st = status(c, 'ei')
            check('B2 TRUNCATE rebuilds it certified', st.get('class') == 'certified' and nblocks(c, 'ei') == 3, str(st))

            # B8: the concurrent forms, on a table with no rows (a build over rows needs the GPU)
            c.scalar('CREATE TABLE ce(id int, v vector(3))')
            c.scalar('CREATE INDEX CONCURRENTLY cei ON ce USING pg_cuvs_hnsw (v vector_l2_ops)')
            st = status(c, 'cei')
            check('B8 CREATE INDEX CONCURRENTLY on an empty table: certified with a ledger', st.get('class') == 'certified' and st.get('ledger_ok') is True and st.get('guarantee_in_force') is True, str(st))
            c.scalar("INSERT INTO ce SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 200) g")
            rc, res = c.q('REINDEX INDEX CONCURRENTLY cei')
            st = status(c, 'cei')
            check('B8 REINDEX CONCURRENTLY of that index: certified again, in force (the build over rows is the GPU path; the table is emptied first)',
                  rc != 0 or (st.get('class') == 'certified' and st.get('guarantee_in_force') is True), f'{res} {st}')
            c.scalar('TRUNCATE ce')
            rc, res = c.q('REINDEX INDEX CONCURRENTLY cei')
            st = status(c, 'cei')
            check('B8 REINDEX CONCURRENTLY of an index on an empty table: certified, ledger ok, inserts after it are keyed', rc == 0 and st.get('class') == 'certified' and st.get('ledger_ok') is True, f'{res} {st}')
            c.scalar("INSERT INTO ce SELECT g, ARRAY[g, 1, 1]::real[]::vector FROM generate_series(1, 50) g")
            st = status(c, 'cei')
            check('B8 and its ledger holds a key for each of the 50 rows', st.get('ledger_keys') == 50, str(st))

            # B9 (review 32af2a8c R8-1, R8-2): an invalid index that a VACUUM changes is taken off; the empty file of a failed concurrent build is named
            c.scalar('CREATE TABLE iv(id int, v vector(3))')
            c.scalar('CREATE INDEX ivi ON iv USING pg_cuvs_hnsw (v vector_l2_ops)')
            c.scalar("INSERT INTO iv SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 300) g")
            check('B9 the index starts certified', status(c, 'ivi').get('class') == 'certified', str(status(c, 'ivi')))
            # recertified by hand, so that no growth makes the VACUUM below recertify for another reason
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('ivi')::text")
            check('B9 recertified, nothing since the certification', rc == 0 and status(c, 'ivi').get('keys_since_certification') == 0, res)
            c.scalar("UPDATE pg_index SET indisvalid = false WHERE indexrelid = 'ivi'::regclass")
            c.scalar("DELETE FROM iv WHERE id % 3 = 0")
            gen_iv = status(c, 'ivi').get('generation')
            rc, res = c.q('VACUUM iv')
            st = status(c, 'ivi')
            check('B9 a VACUUM that deletes from an invalid index takes it off and its cleanup certifies it again (a new generation); it was not left certified on a changed graph',
                  rc == 0 and st.get('class') == 'certified' and st.get('generation') == gen_iv + 1, f'{res} gen_before={gen_iv} {st}')
            c.scalar('CREATE TABLE ie(id int, v vector(3))')
            c.scalar('CREATE INDEX iei ON ie USING pg_cuvs_hnsw (v vector_l2_ops)')
            c.scalar("UPDATE pg_index SET indisvalid = false, indisready = false WHERE indexrelid = 'iei'::regclass")
            path = c.data / c.scalar("SELECT pg_relation_filepath('iei')")
            c.stop()
            open(path, 'wb').close()		# the file a failed build leaves: length 0
            c.start()
            st = status(c, 'iei')
            check('B9 the empty file of a failed concurrent build is named by the status function (not an error class), with what to do',
                  st.get('class') == 'not_built' and 'DROP INDEX' in st.get('pending', ''), str(st))
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('iei')::text")
            check('B9 and by the manual recertification (not a raw read error)', rc == 0 and 'not_applicable' in res and 'DROP INDEX' in res, res)

            c.scalar('CREATE UNLOGGED TABLE ue(id int, v vector(3))')
            c.scalar('CREATE INDEX uei ON ue USING pg_cuvs_hnsw (v vector_l2_ops)')
            st = status(c, 'uei')
            check('B3 UNLOGGED empty index is certified before the crash', st.get('class') == 'certified', str(st))
            c.scalar('CHECKPOINT')
            c.stop('immediate')
            c.start()
            st = status(c, 'uei')
            check('B3 after crash recovery the init fork gives a certified empty index', st.get('class') == 'certified' and st.get('ledger_head') == 2, str(st))
            check('B3 and three blocks', nblocks(c, 'uei') == 3, str(nblocks(c, 'uei')))
            check('B3 the init fork carries the descriptor (pd_upper 7936)', metapage_upper(c, 'uei', 'init') == 7936, str(metapage_upper(c, 'uei', 'init')))

            # another session's temporary index names itself instead of failing inside index_open
            other = dg.Session(c)
            other.run('CREATE TEMP TABLE tmp_e(id int, v vector(3))')
            other.run('CREATE INDEX tmp_ei ON tmp_e USING pg_cuvs_hnsw (v vector_l2_ops)')
            rc, res = c.q('SELECT pg_cuvs_cover_invalidate_all()')
            check('B8 another session\'s temporary index is named in the error', rc != 0 and 'tmp_ei' in res and 'temporary' in res, res)
            other.close()

            # an index of an older build: a pgvector hnsw index whose access method is switched in the catalog
            c.make_index('t', 'ti')
            st = status(c, 'ti')
            check('B6 an index without a descriptor is legacy', st.get('class') == 'legacy' and st.get('writable') is False, str(st))

            # the SQL goes back to the old symbol: new indexes are built as before. The UNLOGGED index uei is CERTIFIED in
            # both forks; the downgrade must take both off, or a crash reset would bring the guarantee back (review H3)
            rc, res = c.q("ALTER EXTENSION pg_cuvs UPDATE TO '0.9.2'")
            check('B4 downgrade to 0.9.2', rc == 0, res)
            c.restart()	# a clean shutdown writes the buffers of an UNLOGGED relation (a checkpoint does not)
            rel = c.scalar("SELECT pg_relation_filepath('uei')")
            main_d = dg.read_descriptor(c, c.data / rel)
            init_d = dg.read_descriptor(c, c.data / (rel + '_init'))
            check('B7 the downgrade wrote LOST (reason downgrade) in the main fork of the UNLOGGED index',
                  main_d['state'] == 2 and main_d['reason'] == 10 and main_d['crc_ok'], str(main_d))
            check('B7 and in its init fork', init_d['state'] == 2 and init_d['reason'] == 10 and init_d['graph_epoch'] == 1 and init_d['crc_ok'], str(init_d))
            c.stop('immediate')
            c.start()
            main_after = dg.read_descriptor(c, c.data / rel)
            check('B7 after a crash reset the UNLOGGED index is still not certified', main_after['state'] == 2 and main_after['reason'] == 10, str(main_after))
            c.scalar('CREATE TABLE oe(id int, v vector(3))')
            c.scalar('CREATE INDEX oei ON oe USING pg_cuvs_hnsw (v vector_l2_ops)')
            up = metapage_upper(c, 'oei')
            check('B4 an index built under the old symbol has the legacy shape (pd_upper 8184 or 8120)', up in (8184, 8120), str(up))
            check('B4 and no ledger page', nblocks(c, 'oei') == 2, str(nblocks(c, 'oei')))
            conn_old = c.scalar("SELECT pg_cuvs_hnsw_connectivity_status('oei')::text")
            check('B5 the same connectivity report with and without the descriptor', conn_old == conn_new, f'{conn_old} vs {conn_new}')
            rc, res = c.q("ALTER EXTENSION pg_cuvs UPDATE TO '0.9.3'")
            check('B4 upgrade again', rc == 0, res)
            st = status(c, 'oei')
            check('B4 the old-shape index stays legacy after the upgrade', st.get('class') == 'legacy', str(st))
        finally:
            c.drop()
    finally:
        (out / 'transcript.txt').write_text('\n'.join(dg.TRANSCRIPT) + '\n')
        shutil.rmtree(root, ignore_errors=True)
        for d in out.glob('data-*'):
            shutil.rmtree(d, ignore_errors=True)
    result = {'failures': dg.FAILURES, 'verdict': 'FAIL' if dg.FAILURES else 'PASS',
              'not_run': 'builds over rows (need the CAGRA daemon / GPU): the covered build paths'}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 1 if dg.FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
