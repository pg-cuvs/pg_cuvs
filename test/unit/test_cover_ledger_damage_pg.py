#!/usr/bin/env python3
"""What damage to the ledger of a CERTIFIED index does, on real servers (CPU only), and what the status function says about it.

  L1 one flipped bit in the heap TID of one ledger key: the status function finds it (ledger_ok false, the crc named),
     guarantee_in_force is false, a scan answers from the heap with the exact result, inserts go on, and a recertification
     replaces the chain (chain_replaced) and puts the guarantee back;
  L2 the next pointer of a middle ledger page overwritten with "end of chain": the same, the chain missing its tail named;
  L3 a flipped bit in a creator xid, in a flag and in the ordinal of a key: each one is found;
  L6 a VACUUM heals a damaged ledger by itself and the status and the scan notice say which call heals it;
  L5 (with --gate, a build with the test gates) while a recertification of an index with a damaged ledger is between its walk and its
     publication, the index reads as lost (ledger damaged), not as the certified index it was;
  L4 who may ask: a role without a privilege on the table gets permission denied from the status function, a role with SELECT
     on the table gets the answer.
The independent reader of the relation files is the one of test_cover_insert_pg.py; the crc is Python's own.
"""
import argparse
import json
import os
import threading
import time
from pathlib import Path
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402
import test_cover_insert_pg as ti  # noqa: E402
import test_cover_recert_pg as tr  # noqa: E402

check = dg.check


def status(c, idx):
    return json.loads(c.scalar(f"SELECT pg_cuvs_hnsw_cover_status('{idx}')::text"))


def flip(path, offset, bit=0):
    data = bytearray(path.read_bytes())
    data[offset] ^= 1 << bit
    path.write_bytes(bytes(data))


def fresh(o, c, n=1500):
    c.scalar('CREATE TABLE t(id int, v vector(3))')
    c.scalar('CREATE INDEX ti ON t USING pg_cuvs_hnsw (v vector_l2_ops)')
    c.scalar(f"INSERT INTO t SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, {n}) g")
    return c.data / c.scalar("SELECT pg_relation_filepath('ti')")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True)
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5591)
    ap.add_argument('--gate', action='store_true', help='--new-lib is built with -DCUVS_COVER_RECERT_TEST_GATE: run L5')
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    gate = out / 'gate'
    gate.mkdir()
    os.environ['CUVS_RECERT_GATE_DIR'] = str(gate)
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'ld', out, args.port)
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')
            path = fresh(o, c)
            c.scalar('CHECKPOINT')
            pages = ti.read_pages(path)
            keys, order = ti.ledger(pages)
            check('L0 setup: 1500 keys on 5 pages, in force, ledger ok', len(keys) == 1500 and len(order) == 5 and status(c, 'ti').get('guarantee_in_force') is True
                  and status(c, 'ti').get('ledger_ok') is True, str(status(c, 'ti')))

            # L1: one bit in the heap_block of key 0 (bytes 16..19 of the key; the first key is at 64 of the first ledger page)
            first_page = order[0]
            c.stop()
            flip(path, first_page * 8192 + 64 + 16, 3)
            c.start()
            st = status(c, 'ti')
            check('L1 the status function finds the flipped bit and names it', st.get('ledger_ok') is False and 'crc' in st.get('ledger_problem', '')
                  and st.get('guarantee_in_force') is False and st.get('class') == 'certified', str(st))
            rc, res = c.q("SET enable_seqscan = off; SELECT id FROM t ORDER BY v <-> '[5,5,2]' LIMIT 5")
            check('L1 a scan answers from the heap', rc == 0 and 'answers from the heap' in res and 'ledger' in res, res)
            ids_index = c.scalar("SET enable_seqscan = off; SELECT string_agg(id::text, ',' ORDER BY id) FROM (SELECT id FROM t ORDER BY v <-> '[5,5,2]' LIMIT 5) s").splitlines()[-1]
            ids_heap = c.scalar("SET enable_indexscan = off; SET enable_bitmapscan = off; SELECT string_agg(id::text, ',' ORDER BY id) FROM (SELECT id FROM t ORDER BY v <-> '[5,5,2]' LIMIT 5) s").splitlines()[-1]
            check('L1 and the answer is the exact one', ids_index == ids_heap, f'{ids_index} vs {ids_heap}')
            rc, res = c.q("INSERT INTO t SELECT 5000 + g, ARRAY[g, 1, 1]::real[]::vector FROM generate_series(1, 20) g")
            check('L1 inserts go on (the insert path does not read old keys)', rc == 0, res)
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('ti')::text")
            st = status(c, 'ti')
            check('L1 a recertification replaces the chain and the guarantee is back',
                  rc == 0 and '"result": "done"' in res and '"chain_replaced": true' in res and st.get('guarantee_in_force') is True and st.get('ledger_ok') is True, f'{res} {st}')
            c.scalar('CHECKPOINT')
            pages = ti.read_pages(path)
            keys_after, kinds, order_new, d = tr.ledger_all(pages)
            unreached, _ = tr.unreachable_tids(path)
            check('L1 the new chain holds exactly the rows the graph cannot reach', {(k[1], k[2]) for k in keys_after} == unreached, f'{len(keys_after)} vs {len(unreached)}')
            fl = tr.free_list(pages)
            every = [b for b in range(2, len(pages)) if struct.unpack_from('<I', pages[b], 24)[0] == 0x4C45444C]
            orphans = sorted(set(every) - set(fl) - set(order_new))
            check('L1 the fresh empty chain made before the walk is on the free list; the pages of the damaged chain cannot be trusted and are not reused (orphans)',
                  len(fl) == 1 and len(orphans) == 5, f'free {fl} orphans {orphans}')

            # L2: the next pointer of a middle page says "end of chain"
            keys, order = ti.ledger(ti.read_pages(path))
            c.scalar('INSERT INTO t SELECT 8000 + g, ARRAY[g, 2, 2]::real[]::vector FROM generate_series(1, 800) g')
            c.scalar('CHECKPOINT')
            keys, order = ti.ledger(ti.read_pages(path))
            check('L2 setup: a chain of at least three pages', len(order) >= 3, str(order))
            c.stop()
            data = bytearray(path.read_bytes())
            struct.pack_into('<I', data, order[1] * 8192 + 8184, 0xFFFFFFFF)
            path.write_bytes(bytes(data))
            c.start()
            st = status(c, 'ti')
            check('L2 the status function finds the cut chain', st.get('ledger_ok') is False and 'ends before the tail' in st.get('ledger_problem', '')
                  and st.get('guarantee_in_force') is False, str(st))
            rc, res = c.q("SET enable_seqscan = off; SELECT id FROM t ORDER BY v <-> '[5,5,2]' LIMIT 5")
            check('L2 a scan answers from the heap', rc == 0 and 'answers from the heap' in res, res)
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('ti')::text")
            st = status(c, 'ti')
            check('L2 a recertification replaces the chain', rc == 0 and '"chain_replaced": true' in res and st.get('guarantee_in_force') is True, f'{res} {st}')

            # L3: a bit in the xid, in the flags, in the ordinal, in the crc: of a key that exists
            c.scalar('INSERT INTO t SELECT 9000 + g, ARRAY[g, 3, 3]::real[]::vector FROM generate_series(1, 40) g')
            c.scalar('CHECKPOINT')
            pages = ti.read_pages(path)
            keys, order = ti.ledger(pages)
            check('L3 setup: the chain holds keys', len(keys) >= 40, str(len(keys)))
            page0 = next(b for b in order if struct.unpack_from('<I', pages[b], 24 + 24)[0] > 0)
            for label, off, bit in (('the creator xid', 8, 5), ('the flags', 22, 0), ('the ordinal', 0, 1), ('the crc itself', 6, 7)):
                c.stop()
                flip(path, page0 * 8192 + 64 + off, bit)
                c.start()
                st = status(c, 'ti')
                check(f'L3 a bit in {label} is found', st.get('ledger_ok') is False and st.get('guarantee_in_force') is False, str(st))
                c.stop()
                flip(path, page0 * 8192 + 64 + off, bit)	# put it back
                c.start()
                st = status(c, 'ti')
                check(f'L3 and put back it reads again', st.get('ledger_ok') is True and st.get('guarantee_in_force') is True, str(st))

            # L6: a damaged ledger is healed by VACUUM by itself, and says what to do meanwhile (review af8d48e1 R6-1)
            c.stop()
            flip(path, page0 * 8192 + 64 + 16, 4)
            c.start()
            st = status(c, 'ti')
            check('L6 damaged: not in force, no promise sentence in the answer, a recertification is recommended and the pending text says what to do',
                  st.get('guarantee_in_force') is False and 'guarantee' not in st and st.get('recertify_recommended') is True and 'pg_cuvs_cover_recertify()' in st.get('pending', ''), str(st))
            rc, res = c.q("SET enable_seqscan = off; SELECT id FROM t ORDER BY v <-> '[5,5,2]' LIMIT 5")
            check('L6 the scan notice names the call that heals it, not REINDEX', 'answers from the heap' in res and "pg_cuvs_cover_recertify('ti')" in res and 'REINDEX' not in res, res)
            rc, res = c.q('VACUUM t')
            st = status(c, 'ti')
            check('L6 a VACUUM without a single deleted row builds a new ledger: in force again, with the promise sentence', rc == 0 and st.get('guarantee_in_force') is True and 'guarantee' in st, f'{res} {st}')

            # L5: the damaged chain is replaced before the walk, and from then on the index is not the certified one it was
            if args.gate:
                c.scalar('INSERT INTO t SELECT 9500 + g, ARRAY[g, 4, 4]::real[]::vector FROM generate_series(1, 30) g')
                c.scalar('CHECKPOINT')
                pages = ti.read_pages(path)
                keys, order = ti.ledger(pages)
                page0 = next(b for b in order if struct.unpack_from('<I', pages[b], 24 + 24)[0] > 0)	# the chain was replaced by the VACUUM above
                c.stop()
                flip(path, page0 * 8192 + 64 + 16, 2)
                c.start()
                st0 = status(c, 'ti')
                check('L5 setup: damaged again', st0.get('ledger_ok') is False, str(st0))
                (gate / 'recert_armed').write_text('')
                box = []
                th = threading.Thread(target=lambda: box.append(c.q("SELECT pg_cuvs_cover_recertify('ti')::text")))
                th.start()
                for _ in range(300):
                    if (gate / 'recert_reached').exists():
                        break
                    time.sleep(0.1)
                st_mid = status(c, 'ti')
                (gate / 'recert_go').write_text('')
                th.join()
                (gate / 'recert_armed').unlink()
                st_end = status(c, 'ti')
                check('L5 between the walk and the publication the index is lost, reason ledger damaged (11), not in force',
                      (gate / 'recert_reached').exists() and st_mid.get('class') == 'lost' and st_mid.get('stored_reason') == 11 and st_mid.get('guarantee_in_force') is False, str(st_mid))
                check('L5 and the recertification ends with it certified again', box and box[0][0] == 0 and st_end.get('guarantee_in_force') is True, f'{box} {st_end}')

            # L4: who may ask
            c.scalar('CREATE ROLE nobody_here LOGIN')
            c.scalar('CREATE ROLE reader LOGIN')
            c.scalar('GRANT SELECT ON t TO reader')
            rc, res = c.q("SELECT pg_cuvs_hnsw_cover_status('ti')::text", role='nobody_here')
            check('L4 without a privilege on the table: permission denied', rc != 0 and 'permission denied for table t' in res, res)
            rc, res = c.q("SELECT pg_cuvs_hnsw_cover_status('ti')::text", role='reader')
            check('L4 with SELECT on the table the answer is given', rc == 0 and '"class": "certified"' in res, res)
        finally:
            c.drop()
    finally:
        (out / 'transcript.txt').write_text('\n'.join(dg.TRANSCRIPT) + '\n')
        shutil.rmtree(root, ignore_errors=True)
        for d in out.glob('data-*'):
            shutil.rmtree(d, ignore_errors=True)
    result = {'failures': dg.FAILURES, 'verdict': 'FAIL' if dg.FAILURES else 'PASS',
              'not_run': 'a cluster with data checksums enabled (it would find a flipped bit on the page level before the key crc)'}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 1 if dg.FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
