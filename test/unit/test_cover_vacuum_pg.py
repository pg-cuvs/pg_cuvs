#!/usr/bin/env python3
"""F6b: what VACUUM does to a pg_cuvs_hnsw index that carries a cover descriptor, on real PostgreSQL servers (CPU only).

  V1 a VACUUM that removes rows takes the guarantee off before pgvector changes the graph (graph_epoch + 1) and the same
     VACUUM recertifies the index (generation + 1): CERTIFIED again, and its BASE keys are exactly what an independent walk
     of the vacuumed graph cannot reach;
  V2 a CERTIFIED index is left alone by VACUUM while its ledger has grown by less than the threshold (5% of
     the rows the certification covered) and is recertified once it has;
  V3 a recertification that cannot run (the page chain is damaged) is remembered in the descriptor: the next VACUUM at the
     same size and limits skips it, a VACUUM with more memory tries again;
  V4 ANALYZE (which also calls amvacuumcleanup) does not recertify; a VACUUM certifies an index that was built without a
     certification;
  V5 autovacuum does the same without anyone calling VACUUM.

The oracle that reads relation files is test_cover_reach_pg.py's, which shares no code with the library.
"""
import argparse
import json
import os
import re
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402
import test_cover_insert_pg as ti  # noqa: E402
import test_cover_recert_pg as tr  # noqa: E402

check = dg.check


def descriptor_full(path):
    """The descriptor with the certification failure fields (read from the relation file; the server must have checkpointed)."""
    pages = ti.read_pages(path)
    meta = pages[0]
    upper = struct.unpack_from('<H', meta, 14)[0]
    raw = meta[upper:upper + 184]
    d = ti.descriptor(pages)
    d['last_fail_nblocks'] = struct.unpack_from('<I', raw, 96)[0]
    d['last_fail_status'] = struct.unpack_from('<I', raw, 100)[0]
    d['last_fail_limit'] = struct.unpack_from('<Q', raw, 104)[0]
    d['nblocks'] = len(pages)
    return d


def server_log(cluster):
    return ''.join(f.read_text(errors='replace') for f in sorted(cluster.data.glob('log-*')))


def hack_index(c, table, index, opts=''):
    """A pgvector hnsw index whose access method is switched to pg_cuvs_hnsw (the pages of a row-built index)."""
    c.scalar(f"CREATE INDEX {index} ON {table} USING hnsw (v vector_l2_ops) {opts}")
    c.scalar(f"UPDATE pg_class SET relam = (SELECT oid FROM pg_am WHERE amname = 'pg_cuvs_hnsw') WHERE relname = '{index}'")
    c.scalar(f"UPDATE pg_depend SET refobjid = (SELECT oid FROM pg_opclass WHERE opcname = 'vector_l2_ops' AND opcmethod = (SELECT oid FROM pg_am WHERE amname = 'pg_cuvs_hnsw')) WHERE classid = 'pg_class'::regclass AND objid = '{index}'::regclass AND refclassid = 'pg_opclass'::regclass")
    c.scalar(f"UPDATE pg_index SET indclass = (SELECT oid::text::oidvector FROM pg_opclass WHERE opcname = 'vector_l2_ops' "
             f"AND opcmethod = (SELECT oid FROM pg_am WHERE amname = 'pg_cuvs_hnsw')) WHERE indexrelid = '{index}'::regclass")


def plant(c, plant_bin, index, mode, damage_block=None):
    path = c.data / c.scalar(f"SELECT pg_relation_filepath('{index}')")
    c.stop()
    if damage_block is not None:
        data = bytearray(path.read_bytes())
        nblocks = len(data) // 8192
        struct.pack_into('<I', data, damage_block * 8192 + 8184, nblocks + 1000)
        path.write_bytes(bytes(data))
    r = subprocess.run([str(plant_bin), str(path), mode], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    assert r.returncode == 0, r.stdout
    c.start()
    return path


def clustered(table, n, dim=8):
    """25 clusters with a noise that is a function of the row number and the dimension (not random(): the data is the same in every run; an independent review
    found a setseed() in a session of its own that did not reach the session of the insert, and data that differed from run to run)."""
    return (f"INSERT INTO {table} SELECT x, (SELECT array_agg((((x % 25) * 7 + d * 3) % 41 + (((x * 7919 + d * 104729) % 1000) / 1000.0 - 0.5) * 0.05)::real) "
            f"FROM generate_series(1, {dim}) d)::vector FROM generate_series(1, {n}) x")


def cover_status(c, index):
    return json.loads(c.scalar(f"SELECT pg_cuvs_hnsw_cover_status('{index}')::text"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True)
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5511)
    ap.add_argument('--gate', action='store_true', help='--new-lib is built with -DCUVS_COVER_RECERT_TEST_GATE: also check the state in the middle of a VACUUM (V1c)')
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    plant_bin = out / 'cover_plant'
    r = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(repo / 'src'), str(repo / 'test/unit/cover_plant.c'), '-o', str(plant_bin)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    assert r.returncode == 0, r.stdout
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        gate = out / 'gate'
        gate.mkdir()
        os.environ['CUVS_RECERT_GATE_DIR'] = str(gate)
        c = dg.Cluster(o, 'vac', out, args.port)
        c.extra_opts = '-c autovacuum=off -c max_parallel_maintenance_workers=0'
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')

            # V1
            c.scalar('CREATE TABLE g(id int, v vector(8))')
            c.scalar(clustered('g', 4000))
            # rows that all hold one vector hang loose in the graph whatever order pgvector's own random levels give: the premise of V1 (the graph has
            # rows that nothing reaches after the VACUUM) does not depend on a draw
            c.scalar("INSERT INTO g SELECT 100000 + x, '[0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5]'::vector FROM generate_series(1, 600) x")
            hack_index(c, 'g', 'gi', 'WITH (m = 4, ef_construction = 8)')
            path = plant(c, plant_bin, 'gi', 'uncertified')
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('gi')::text")
            check('V1 setup: certified by the function', rc == 0 and '"result": "done"' in res, res)
            c.scalar('CHECKPOINT')
            d0 = descriptor_full(path)
            c.scalar('DELETE FROM g WHERE id % 3 = 0')
            rc, res = c.q('VACUUM g')
            check('V1 VACUUM succeeded', rc == 0, res)
            c.scalar('CHECKPOINT')
            d1 = descriptor_full(path)
            keys, kinds, order, _ = tr.ledger_all(ti.read_pages(path))
            unreached, orc = tr.unreachable_tids(path)
            base = {(k[1], k[2]) for k in keys if k[3] == 2}
            check('V1 the index is CERTIFIED again with an intact descriptor', d1['state'] == 1 and d1['crc_ok'], str(d1))
            check('V1 graph_epoch went up by one (the index was taken off before the graph changed) and the generation by one (recertified)',
                  d1['epoch'] == d0['epoch'] + 1 and d1['generation'] == d0['generation'] + 1, f'{d0} -> {d1}')
            check('V1 the BASE keys are exactly what the independent walk of the vacuumed graph cannot reach', base == unreached and len(unreached) > 0,
                  f'{len(base)} vs {len(unreached)}')
            check('V1 the server log says what happened', 'recertification of index "gi" by vacuum: done' in server_log(c))

            # V1c: in the middle of the VACUUM, after the guarantee was taken off and before pgvector changes the graph
            if args.gate:
                c.scalar('CREATE TABLE gc(id int, v vector(8))')
                c.scalar(clustered('gc', 1500))
                # rows of one vector hang loose in the graph (as in V1): the ledger holds rows the graph cannot reach
                c.scalar("INSERT INTO gc SELECT 100000 + x, '[0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5]'::vector FROM generate_series(1, 200) x")
                hack_index(c, 'gc', 'gci', 'WITH (m = 8, ef_construction = 16)')
                plant(c, plant_bin, 'gci', 'uncertified')
                c.scalar("SELECT pg_cuvs_cover_recertify('gci')")
                # rows the graph cannot reach that the ledger holds: one that stays and one that the DELETE below removes
                c.scalar('CHECKPOINT')
                gpath = c.data / c.scalar("SELECT pg_relation_filepath('gci')")
                gkeys, _, _, _ = tr.ledger_all(ti.read_pages(gpath))
                g_unreached, _ = tr.unreachable_tids(gpath)
                held = sorted(t for t in g_unreached if t in {(k[1], k[2]) for k in gkeys})
                tid_ids = {}
                for line in c.scalar("SELECT ctid::text || '|' || id FROM gc").split('\n'):
                    t_txt, i_txt = line.split('|')
                    tid_ids[tuple(int(x) for x in t_txt.strip('()').split(','))] = int(i_txt)
                held_ids = [tid_ids[t] for t in held if t in tid_ids]
                stay = next((i for i in held_ids if i % 4 != 0), None)
                gone = next((i for i in held_ids if i % 4 == 0), None)
                rc_e0, res_e0 = c.q("SET enable_seqscan = off; EXPLAIN (ANALYZE, BUFFERS) SELECT id FROM gc ORDER BY v <-> (SELECT v FROM gc WHERE id = 1) LIMIT 3")
                m0 = re.search(r'Index Scan using gci.*?Buffers: shared hit=(\d+)(?: read=(\d+))?', res_e0, re.S)
                buffers_before = (int(m0.group(1)) + int(m0.group(2) or 0)) if m0 else None
                c.scalar('CREATE TABLE gc_deleted AS SELECT id, v FROM gc WHERE id % 4 = 0')
                c.scalar('DELETE FROM gc WHERE id % 4 = 0')
                (gate / 'vacuum_armed').write_text('')
                box = []
                th = threading.Thread(target=lambda: box.append(c.q('VACUUM gc')))
                th.start()
                for _ in range(300):
                    if (gate / 'vacuum_reached').exists():
                        break
                    time.sleep(0.1)
                reached = (gate / 'vacuum_reached').exists()
                st_mid = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('gci')::text"))
                rc_s, res_s = c.q("SET enable_seqscan = off; SELECT id FROM gc ORDER BY v <-> (SELECT v FROM gc WHERE id = 1) LIMIT 3")
                rc_e, res_e = c.q("SET enable_seqscan = off; EXPLAIN (ANALYZE, BUFFERS) SELECT id FROM gc ORDER BY v <-> (SELECT v FROM gc WHERE id = 1) LIMIT 3")
                m_buf = re.search(r'Index Scan using gci.*?Buffers: shared hit=(\d+)(?: read=(\d+))?', res_e, re.S)
                scan_buffers = (int(m_buf.group(1)) + int(m_buf.group(2) or 0)) if m_buf else None
                # the rows of one vector are all at distance 0: the 250 first answers hold every one of them that is an answer at all
                rc_k, res_k = c.q(f"SET enable_seqscan = off; SELECT id FROM gc ORDER BY v <-> (SELECT v FROM gc WHERE id = {stay}) LIMIT 250") if stay is not None else (1, 'no row')
                rc_g, res_g = (c.q(f"SET enable_seqscan = off; SELECT id FROM gc ORDER BY v <-> (SELECT v FROM gc_deleted WHERE id = {gone}) LIMIT 250")
                               if gone is not None else (1, 'no row'))
                (gate / 'vacuum_go').write_text('')
                th.join()
                (gate / 'vacuum_armed').unlink()
                st_end = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('gci')::text"))
                check('V1c the VACUUM reached the point between the transition and pgvector', reached)
                check('V1c there the index is LOST (vacuum), not in force', st_mid.get('class') == 'lost' and st_mid.get('stored_reason') == 4 and st_mid.get('guarantee_in_force') is False, str(st_mid))
                # the index is LOST because the graph is about to change, but its ledger reads: the scan answers from the graph and the ledger (no
                # ranking of the whole heap, which made a search 180 times slower for the length of the VACUUM), and a row only the ledger holds is found.
                # This is the one state that CUVS_COVER_MERGE_DURING_VACUUM in src/cuvs_hnsw_scan.c decides (a user decision: merge only
                # while that VACUUM runs; V1d checks the heap ranking after it): with it false these three checks expect the heap ranking instead.
                check('V1c a scan there answers from the graph and the ledger, not from the heap', rc_s == 0 and 'answers from the heap' not in res_s, res_s[-300:])
                check('V1c and the scan reads about what it read before the VACUUM (%s against %s buffers; the whole heap ranked would be far more on a large table)' % (scan_buffers, buffers_before),
                      scan_buffers is not None and buffers_before is not None and scan_buffers <= 2 * buffers_before + 10, res_e[-400:])
                check('V1c a row the graph cannot reach and the ledger holds is an answer of a search with its own vector there', rc_k == 0 and str(stay) in res_k.split(), f'{stay} {res_k[-120:]}')
                check('V1c a deleted row whose key is in the ledger is not an answer there', gone is not None and rc_g == 0 and str(gone) not in res_g.split(), f'{gone} {res_g[-200:]}')
                check('V1c and the VACUUM ends with the index certified again', box and box[0][0] == 0 and st_end.get('class') == 'certified', str((box, st_end)))

                # V1d: a VACUUM that ends normally but whose recertification at the end is deferred leaves the index LOST with the reason
                # vacuum. The merge is only for the time that VACUUM runs: after it, the search is the exact ranking of the heap with its notice.
                # An insert held at the "insert" gate holds the cover lock in share mode, so the end-of-VACUUM recertification cannot take it
                # (at most 2 s) and is deferred.
                c.scalar('CREATE TABLE gd(id int, v vector(8))')
                c.scalar('CREATE INDEX gdi ON gd USING pg_cuvs_hnsw (v vector_l2_ops)')
                c.scalar(clustered('gd', 400))			# built on an empty table: every row is a ledger key
                c.scalar('DELETE FROM gd WHERE id % 5 = 0')
                (gate / 'insert_armed').write_text('')
                box_i = []
                th_i = threading.Thread(target=lambda: box_i.append(c.q("INSERT INTO gd VALUES (99999, '[9,9,9,9,9,9,9,9]')")))
                th_i.start()
                for _ in range(300):
                    if (gate / 'insert_reached').exists():
                        break
                    time.sleep(0.1)
                held_i = (gate / 'insert_reached').exists()
                rc_v, res_v = c.q('VACUUM gd')
                st_d = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('gdi')::text"))
                (gate / 'insert_go').write_text('')
                th_i.join()
                (gate / 'insert_armed').unlink()
                for f in ('insert_go', 'insert_reached'):
                    (gate / f).unlink(missing_ok=True)
                rc_d, res_d = c.q("SET enable_seqscan = off; SELECT id FROM gd ORDER BY v <-> (SELECT v FROM gd WHERE id = 1) LIMIT 20")
                rc_h, res_h = c.q("SET enable_indexscan = off; SET enable_bitmapscan = off; SELECT id FROM gd ORDER BY v <-> (SELECT v FROM gd WHERE id = 1) LIMIT 20")
                check('V1d setup: an insert held the cover lock, and the VACUUM ended with its recertification deferred (LOST, reason vacuum)',
                      held_i and rc_v == 0 and st_d.get('class') == 'lost' and st_d.get('stored_reason') == 4 and 'recertification of index "gdi" by vacuum: deferred' in server_log(c), f'{held_i} {res_v[-200:]} {st_d}')
                check('V1d after that VACUUM the search answers exactly from the heap, with the notice', rc_d == 0 and 'answers from the heap' in res_d
                      and [x for x in res_d.split('\n') if x.isdigit()] == [x for x in res_h.split('\n') if x.isdigit()], f'{res_d[-300:]} / {res_h[-120:]}')

                # V1e (parallel VACUUM): ambulkdelete records the VACUUM's leader (lockGroupLeader), so that an index vacuumed by a
                # parallel worker that ends before the VACUUM does is still merged while the VACUUM runs. Six VACUUMs (PARALLEL 2, two btree indexes
                # beside the cover index): each search during the held VACUUM must merge (with hnsw.ef_search = 1 the 50 rows of the exact ranking,
                # which only the ledger can give, and no heap notice). On the server (2026-11-21, pgvector 0.8.x) two parallel workers ran every time
                # and the leader vacuumed the cover index every time (12 of 12, the cover index created first or last): pgvector's hnsw does not
                # seem to offer parallel bulk deletion, so the worker branch is not reached; which process recorded it is kept as information.
                attempts, in_worker, merged_all = [], 0, True
                c.scalar('ALTER SYSTEM SET log_min_messages = debug1')
                c.scalar('SELECT pg_reload_conf()')
                for att in range(6):
                    t = f'gp{att}'
                    c.scalar(f'CREATE TABLE {t}(id int, v vector(8))')
                    # the leader takes the indexes in order too: the cover index comes last, so that a worker can be the one that takes it
                    c.scalar(f'CREATE INDEX {t}_b1 ON {t} (id)')
                    c.scalar(f'CREATE INDEX {t}_b2 ON {t} (id DESC)')
                    c.scalar(f'CREATE INDEX {t}_i ON {t} USING pg_cuvs_hnsw (v vector_l2_ops)')
                    c.scalar(clustered(t, 400))				# every row is a ledger key
                    c.scalar(f'DELETE FROM {t} WHERE id % 5 = 0')
                    oid_e = c.scalar(f"SELECT '{t}_i'::regclass::oid").strip().splitlines()[-1]
                    mark_e = len(server_log(c))
                    for f in ('vacuum_reached', 'vacuum_go'):
                        (gate / f).unlink(missing_ok=True)
                    (gate / 'vacuum_armed').write_text('')
                    box_e = []

                    def vac_e(t=t):
                        # VACUUM cannot run in the transaction block of a multi-statement string: the settings go in PGOPTIONS
                        r = subprocess.run(c.psql_cmd() + ['-c', f'VACUUM (PARALLEL 2, VERBOSE) {t}'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                           env=dict(os.environ, PGOPTIONS='-c min_parallel_index_scan_size=0 -c max_parallel_maintenance_workers=2'))
                        box_e.append((r.returncode, r.stdout))
                    th_e = threading.Thread(target=vac_e)
                    th_e.start()
                    for _ in range(300):
                        if (gate / 'vacuum_reached').exists():
                            break
                        time.sleep(0.1)
                    held_e = (gate / 'vacuum_reached').exists()
                    q_e = f"SELECT id FROM {t} ORDER BY v <-> (SELECT v FROM {t} WHERE id = 1) LIMIT 50"
                    rc_m, res_m = c.q(f'SET enable_seqscan = off; SET hnsw.ef_search = 1; {q_e}')
                    rc_o, res_o = c.q(f'SET enable_indexscan = off; SET enable_bitmapscan = off; {q_e}')
                    (gate / 'vacuum_go').write_text('')
                    th_e.join()
                    (gate / 'vacuum_armed').unlink()
                    for f in ('vacuum_go', 'vacuum_reached'):
                        (gate / f).unlink(missing_ok=True)
                    rec = [l for l in server_log(c)[mark_e:].splitlines() if f'index {oid_e} taken off by a VACUUM' in l]
                    m_e = re.search(r'process (\d+), VACUUM leader (\d+)', rec[-1]) if rec else None
                    worker = bool(m_e) and m_e.group(1) != m_e.group(2)
                    ids_m = [x for x in res_m.split('\n') if x.isdigit()]
                    ok_m = held_e and rc_m == 0 and 'answers from the heap' not in res_m and len(ids_m) == 50 and ids_m == [x for x in res_o.split('\n') if x.isdigit()]
                    pw = re.findall(r'launched \d+ parallel vacuum workers? for index \w+ \(planned: \d+\)', box_e[0][1]) if box_e else []
                    attempts.append({'held': held_e, 'in_parallel_worker': worker, 'merged': ok_m, 'vacuum_rc': box_e[0][0] if box_e else None,
                                     'record': rec[-1][-90:] if rec else None, 'parallel_workers': pw})
                    merged_all &= ok_m
                    in_worker += worker
                check('V1e (information) parallel workers ran, and the cover index was vacuumed by a parallel worker in %d of %d VACUUMs' % (in_worker, len(attempts)),
                      all(a['parallel_workers'] for a in attempts), str(attempts))
                c.scalar('ALTER SYSTEM RESET log_min_messages')
                c.scalar('SELECT pg_reload_conf()')
                check('V1e and in every attempt the search merged the graph and the ledger while that VACUUM ran', merged_all, str(attempts))

            # V2
            c.scalar('CREATE TABLE q(id int, v vector(8))')
            c.scalar(clustered('q', 2000))
            hack_index(c, 'q', 'qi', 'WITH (m = 8, ef_construction = 16)')
            qpath = plant(c, plant_bin, 'qi', 'uncertified')
            c.scalar("SELECT pg_cuvs_cover_recertify('qi')")
            c.scalar('CHECKPOINT')
            dq0 = descriptor_full(qpath)
            st = cover_status(c, 'qi')
            check('V2 status: certification time, no keys since, no recommendation right after the certification',
                  'certified_at' in st and st.get('keys_since_certification') == 0 and st.get('recertify_recommended') is False and st.get('base_rows', 0) > 0, str(st))
            c.scalar(clustered('q', 90).replace('SELECT x,', 'SELECT 10000 + x,'))
            st = cover_status(c, 'qi')
            check('V2 status: 90 keys since the certification, below the recommendation', st.get('keys_since_certification') == 90 and st.get('recertify_recommended') is False, str(st))
            s = dg.Session(c)
            s.run('SET client_min_messages = debug1')
            res = s.run('VACUUM q')
            c.scalar('CHECKPOINT')
            dq1 = descriptor_full(qpath)
            check('V2 90 keys since the certification (below 5 percent of the 2000 rows it covered): VACUUM leaves the index alone', dq1['generation'] == dq0['generation'] and dq1['state'] == 1, f'{dq0} -> {dq1}')
            check('V2 and says so at debug level', 'not applicable' in res and 'not enough keys' in res, res[-400:])
            c.scalar(clustered('q', 20).replace('SELECT x,', 'SELECT 30000 + x,'))
            st = cover_status(c, 'qi')
            check('V2 status: 110 keys since the certification, the recertification is recommended', st.get('keys_since_certification') == 110 and st.get('recertify_recommended') is True, str(st))
            res = s.run('VACUUM q')
            s.close()
            c.scalar('CHECKPOINT')
            dq2 = descriptor_full(qpath)
            keys, kinds, order, _ = tr.ledger_all(ti.read_pages(qpath))
            check('V2 at 110 keys (past 5 percent of 2000, and no minimum number of keys): recertified, new generation, the delta keys are folded into the walk',
                  dq2['generation'] == dq0['generation'] + 1 and dq2['state'] == 1 and not any(k[3] == 1 for k in keys), f'{dq0} -> {dq2}')
            # a chain of 11000 more... the threshold is relative to the rows covered: now 12500
            c.scalar(clustered('q', 50).replace('SELECT x,', 'SELECT 40000 + x,'))
            res = c.q('VACUUM q')[1]
            c.scalar('CHECKPOINT')
            check('V2 the threshold is measured from the new certification: 50 keys (2.4 percent) do not trigger another one', descriptor_full(qpath)['generation'] == dq2['generation'])

            # V2b: the percent clause of the keys. The covered row count is part of the descriptor; set it by hand (crc recomputed) to a size where
            # 10500 keys are 1% and then 10.5%. The pages the certification covered are set high as well, so that the clause of the pages (the
            # relation grew by 5 percent since the certification) does not take the index first
            def set_base_rows(n, nblocks_at_cert=None):
                c.stop()
                data = bytearray(qpath.read_bytes())
                upper = struct.unpack_from('<H', data, 14)[0]
                struct.pack_into('<Q', data, upper + 40, n)
                if nblocks_at_cert is not None:
                    struct.pack_into('<I', data, upper + 36, nblocks_at_cert)
                struct.pack_into('<I', data, upper + 180, dg.crc32c(bytes(data[upper:upper + 180])))
                qpath.write_bytes(bytes(data))
                c.start()

            gen_before = descriptor_full(qpath)['generation']
            set_base_rows(1000000, nblocks_at_cert=1000000)
            c.scalar(clustered('q', 10500).replace('SELECT x,', 'SELECT 50000 + x,'))
            s = dg.Session(c)
            s.run('SET client_min_messages = debug1')
            res = s.run('VACUUM q')
            c.scalar('CHECKPOINT')
            check('V2b 10500 keys against 1000000 covered rows (1%): left alone', descriptor_full(qpath)['generation'] == gen_before and 'not enough keys' in res, res[-300:])
            s.close()
            set_base_rows(100000)
            rc, res = c.q('VACUUM q')
            c.scalar('CHECKPOINT')
            check('V2b the same keys against 100000 covered rows (10.5%): recertified', rc == 0 and descriptor_full(qpath)['generation'] == gen_before + 1, res)

            # V3
            c.scalar('CREATE TABLE d(id int, v vector(8))')
            c.scalar(clustered('d', 1500))
            hack_index(c, 'd', 'di', 'WITH (m = 8, ef_construction = 16)')
            dpath = plant(c, plant_bin, 'di', 'uncertified', damage_block=5)
            s = dg.Session(c)
            s.run('SET client_min_messages = log')
            res = s.run('VACUUM d')
            c.scalar('CHECKPOINT')
            dd1 = descriptor_full(dpath)
            st = cover_status(c, 'di')
            check('V3 status: the pending text says the last recertification gave up, and the failure is described',
                  'gave up' in st.get('pending', '') and st.get('last_recertification_failure', {}).get('cause') == 'page_chain_damaged'
                  and st['last_recertification_failure'].get('relation_blocks') == dd1['nblocks'] and st['last_recertification_failure'].get('limit_bytes', 0) > 0, str(st))
            check('V3 the walk reports the damage and the vacuum itself succeeds', 'by vacuum: failed' in res and 'page chain is damaged' in res and 'ERROR' not in res, res[-400:])
            check('V3 the descriptor remembers the failure: size, status 4 (damaged), limit',
                  dd1['state'] == 2 and dd1['last_fail_nblocks'] == dd1['nblocks'] and dd1['last_fail_status'] == 4 and dd1['last_fail_limit'] > 0, str(dd1))
            s.run('SET client_min_messages = debug1')
            res = s.run('VACUUM d')
            check('V3 the next VACUUM at the same size and limits skips the walk', 'by vacuum: skipped' in res and 'by vacuum: failed' not in res, res[-400:])
            s.run("SET maintenance_work_mem = '512MB'")
            res = s.run('VACUUM d')
            check('V3 with more memory the walk is tried again', 'by vacuum: failed' in res, res[-400:])
            s.run('RESET maintenance_work_mem')
            res = s.run('VACUUM d')
            check('V3 back at the default memory the failure at the larger limit is remembered: skipped', 'by vacuum: skipped' in res and 'by vacuum: failed' not in res, res[-400:])
            c.scalar(clustered('d', 3000).replace('SELECT x,', 'SELECT 5000 + x,'))
            res = s.run('VACUUM d')
            check('V3 once the index has grown the walk is tried again', 'by vacuum: failed' in res, res[-400:])
            s.close()
            st = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('di')::text"))
            check('V3 the index is still LOST and answers from the heap', st.get('class') == 'lost' and st.get('guarantee_in_force') is False, str(st))

            # V4
            c.scalar('CREATE TABLE u(id int, v vector(8))')
            c.scalar(clustered('u', 1500))
            hack_index(c, 'u', 'ui', 'WITH (m = 8, ef_construction = 16)')
            upath = plant(c, plant_bin, 'ui', 'uncertified')
            c.scalar('ANALYZE u')
            st = cover_status(c, 'ui')
            check('V4 status: an index built without a certification says what it waits for', 'built without a certification' in st.get('pending', ''), str(st))
            st = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('ui')::text"))
            check('V4 ANALYZE does not recertify', st.get('class') == 'lost', str(st))
            rc, res = c.q('VACUUM u')
            st = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('ui')::text"))
            check('V4 VACUUM certifies an index that was built without a certification', rc == 0 and st.get('class') == 'certified' and st.get('guarantee_in_force') is True, f'{res} {st}')

            # V4b: a LOST index keeps its reason when a VACUUM deletes rows, and graph_epoch still goes up (ambulkdelete can run
            # several times in one VACUUM; every call may repair the graph again)
            c.scalar('CREATE TABLE w(id int, v vector(8))')
            c.scalar(clustered('w', 1500))
            hack_index(c, 'w', 'wi', 'WITH (m = 8, ef_construction = 16)')
            wpath = plant(c, plant_bin, 'wi', 'uncertified')
            c.scalar('CHECKPOINT')
            e0 = descriptor_full(wpath)['epoch']
            c.scalar('DELETE FROM w WHERE id % 5 = 0')
            s = dg.Session(c)
            s.run('SET client_min_messages = log')
            res = s.run('VACUUM w')
            s.close()
            c.scalar('CHECKPOINT')
            dw = descriptor_full(wpath)
            check('V4b VACUUM that deletes rows from a LOST index: graph_epoch went up by one for the one ambulkdelete call',
                  dw['epoch'] == e0 + 1, f"{e0} -> {dw['epoch']}: {res[-300:]}")

            # V5: autovacuum
            c.scalar('CREATE TABLE a(id int, v vector(8))')
            c.scalar(clustered('a', 1500))
            hack_index(c, 'a', 'ai', 'WITH (m = 8, ef_construction = 16)')
            c.scalar("ALTER TABLE a SET (autovacuum_vacuum_threshold = 50, autovacuum_vacuum_scale_factor = 0, autovacuum_enabled = true)")
            apath = plant(c, plant_bin, 'ai', 'uncertified')
            c.extra_opts = '-c max_parallel_maintenance_workers=0 -c autovacuum_naptime=1'
            c.restart()
            c.scalar('DELETE FROM a WHERE id % 4 = 0')
            done = False
            for _ in range(90):
                st = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('ai')::text"))
                if st.get('class') == 'certified':
                    done = True
                    break
                time.sleep(1)
            check('V5 autovacuum recertified the index after the deletes, with no one calling VACUUM', done, str(st))
            check('V5 its log line is in the server log', 'recertification of index "ai" by vacuum: done' in server_log(c))

            # V6: an index that was built without a certification, on a table that only receives inserts: the insert-triggered
            # autovacuum certifies it (no row is ever deleted)
            c.scalar('CREATE TABLE ins6(id int, v vector(8))')
            c.scalar(clustered('ins6', 1500))
            hack_index(c, 'ins6', 'ins6i', 'WITH (m = 8, ef_construction = 16)')
            c.scalar("ALTER TABLE ins6 SET (autovacuum_enabled = true, autovacuum_vacuum_insert_threshold = 50, autovacuum_vacuum_insert_scale_factor = 0, "
                     "autovacuum_vacuum_threshold = 100000000)")
            plant(c, plant_bin, 'ins6i', 'uncertified')
            st = cover_status(c, 'ins6i')
            check('V6 setup: lost, uncertified, and the status says when autovacuum comes', st.get('class') == 'lost' and 'autovacuum vacuums the table by itself only after enough rows changed' in st.get('pending', ''), str(st))
            c.scalar(clustered('ins6', 100).replace('SELECT x,', 'SELECT 20000 + x,'))
            done6 = False
            for _ in range(90):
                st = cover_status(c, 'ins6i')
                if st.get('class') == 'certified':
                    done6 = True
                    break
                time.sleep(1)
            check('V6 insert-only autovacuum certified the index', done6, str(st))

            # V7: a CERTIFIED index whose ledger holds no key is scanned unwrapped; one key later it is merged again
            c.scalar('CREATE TABLE z(id int, v vector(8))')
            c.scalar("INSERT INTO z SELECT g, ARRAY[g, g % 5, g % 3, 1, 2, 3, 4, 5]::real[]::vector FROM generate_series(1, 200) g")
            hack_index(c, 'z', 'zi', 'WITH (m = 8, ef_construction = 16)')
            plant(c, plant_bin, 'zi', 'uncertified')
            c.scalar("SELECT pg_cuvs_cover_recertify('zi')")
            st = cover_status(c, 'zi')
            check('V7 setup: certified with no key', st.get('class') == 'certified' and st.get('ledger_keys') == 0, str(st))
            s7 = dg.Session(c)
            s7.run('SET client_min_messages = debug1')
            s7.run('SET enable_seqscan = off')
            res = s7.run("SELECT id FROM z ORDER BY v <-> '[3,3,0,1,2,3,4,5]' LIMIT 5")
            check('V7 the scan is delegated to the graph, the log says so', 'delegated to the graph' in res, res[-300:])
            c.scalar("INSERT INTO z VALUES (9999, '[3,3,0,1,2,3,4,5]')")
            res = s7.run("SELECT id FROM z ORDER BY v <-> '[3,3,0,1,2,3,4,5]' LIMIT 5")
            check('V7 with one key in the ledger it is merged again and the new row is found', 'delegated to the graph' not in res and '9999' in res, res[-300:])
            s7.close()
        finally:
            c.drop()
    finally:
        (out / 'transcript.txt').write_text('\n'.join(dg.TRANSCRIPT) + '\n')
        shutil.rmtree(root, ignore_errors=True)
        for d in out.glob('data-*'):
            shutil.rmtree(d, ignore_errors=True)
    result = {'failures': dg.FAILURES, 'verdict': 'FAIL' if dg.FAILURES else 'PASS',
              'not_run': 'a crash between the WAL record that takes the guarantee off and pgvector\'s page changes; parallel VACUUM; '
                         'the exclusive-lock timeout (deferred) path inside VACUUM; the over-the-memory-limit failure (damage stands in for it)'}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 1 if dg.FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
