#!/usr/bin/env python3
"""F4a: the insert side of the cover ledger on real PostgreSQL servers (CPU only, no GPU).

The only pg_cuvs_hnsw indexes that can be built without the CAGRA daemon are the ones over an empty table, which start
CERTIFIED with an empty ledger. Rows are then added through the real aminsert path. The ledger is read back from the
relation file by code that shares nothing with the product, and compared with the heap's own ctids.

  I1 1000 rows in one statement: 1000 keys, ordinals 0..999, exactly the heap TIDs, three ledger pages, the descriptor
     names the last page as the tail;
  I2 eight sessions inserting at once: every row has exactly one key, ordinals are contiguous, the chain is intact;
  I3 a rolled-back insert leaves its keys (the safe direction): keys are a superset of the live rows;
  I4 a crash does not lose a committed key (the key is logged before the graph insert);
  I5 an index of the old symbol, an index without a descriptor and an OFF index get no keys and no ledger pages;
  I6 an index whose descriptor names a ledger page that is not one refuses the insert with the page number
     instead of writing into a graph page;
  I7 a ledger page of another generation (a chain that a recertification replaced) is refused the same way;
  I8 rows inserted without keys (an old library through the one-statement break-glass) are noticed by the growth monitor at
     the next keyed insert: LOST (unkeyed_writer) with the SUSPECT flag and graph_epoch 1, and keys go on being written.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import binascii
import struct
import sys
import threading

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402

check = dg.check


def read_pages(path):
    data = Path(path).read_bytes()
    return [data[i:i + 8192] for i in range(0, len(data), 8192)]


def descriptor(pages):
    meta = pages[0]
    upper = struct.unpack_from('<H', meta, 14)[0]
    d = meta[upper:upper + 184]
    return {'state': d[10], 'reason': d[11], 'flags': struct.unpack_from('<H', d, 12)[0], 'epoch': struct.unpack_from('<Q', d, 80)[0], 'generation': struct.unpack_from('<Q', d, 16)[0], 'head': struct.unpack_from('<I', d, 24)[0],
            'tail': struct.unpack_from('<I', d, 28)[0], 'crc_ok': dg.crc32c(d[:180]) == struct.unpack_from('<I', d, 180)[0]}


def decode_key(raw):
    """A ledger key as the page holds it: ordinal in 6 bytes, a CRC-16 (CCITT, start 0xFFFF) of the 24 bytes with the crc zeroed
    in the next 2, then xid, heap block, heap offset, flags. Python's own crc, not the library's."""
    w0, = struct.unpack_from('<Q', raw, 0)
    zeroed = struct.pack('<Q', w0 & 0xFFFFFFFFFFFF) + bytes(raw[8:24])
    assert binascii.crc_hqx(zeroed, 0xFFFF) == w0 >> 48, 'key crc does not match'
    xid, hb, ho, flags = struct.unpack_from('<QIHH', raw, 8)
    return w0 & 0xFFFFFFFFFFFF, xid, hb, ho, flags


def ledger(pages):
    """Follow the chain named by the descriptor; return (keys, page_numbers). Keys are (ordinal, heap block, offset)."""
    d = descriptor(pages)
    keys, order, blk = [], [], d['head']
    while blk != 0xFFFFFFFF:
        assert blk not in order, 'loop in the ledger chain'
        order.append(blk)
        p = pages[blk]
        magic, version, kind, gen, first, count, seen, crc = struct.unpack_from('<IHHQQIII', p, 24)
        assert magic == 0x4C45444C and gen == d['generation'], (blk, hex(magic), gen)
        assert crc == dg.crc32c(p[24:24 + 32])
        for i in range(count):
            ordinal, xid, hb, ho, flags = decode_key(p[64 + 24 * i:64 + 24 * i + 24])
            assert ordinal == first + i
            keys.append((ordinal, hb, ho))
        blk = struct.unpack_from('<I', p, 8184)[0]
    return keys, order


def ctids(c, table):
    out = set()
    for line in c.scalar(f"SELECT ctid FROM {table}").split('\n'):
        m = re.fullmatch(r'\((\d+),(\d+)\)', line.strip())
        if m:
            out.add((int(m.group(1)), int(m.group(2))))
    return out


def index_pages(c, index):
    c.scalar('CHECKPOINT')
    return read_pages(c.data / c.scalar(f"SELECT pg_relation_filepath('{index}')"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True)
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5489)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    plant = out / 'cover_plant'
    import subprocess
    r = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(repo / 'src'), str(repo / 'test/unit/cover_plant.c'), '-o', str(plant)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    assert r.returncode == 0, r.stdout
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'i1', out, args.port)
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')
            c.scalar('CREATE TABLE e(id int, v vector(3))')
            c.scalar('CREATE INDEX ei ON e USING pg_cuvs_hnsw (v vector_l2_ops)')

            # I1
            c.scalar("INSERT INTO e SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 1000) g")
            pages = index_pages(c, 'ei')
            keys, order = ledger(pages)
            live = ctids(c, 'e')
            d = descriptor(pages)
            check('I1 one key per row', len(keys) == 1000, str(len(keys)))
            check('I1 ordinals are 0..999', [k[0] for k in keys] == list(range(1000)))
            check('I1 the keys are exactly the heap TIDs', {(k[1], k[2]) for k in keys} == live)
            check('I1 three ledger pages (338 keys each)', len(order) == 3, str(order))
            check('I1 the descriptor names the last page as the tail and is intact', d['tail'] == order[-1] and d['crc_ok'] and d['state'] == 1, str(d))

            # I2
            def writer(k):
                for _ in range(15):
                    c.scalar(f"INSERT INTO e SELECT 100000 + {k} * 1000 + g, ARRAY[g, {k}, 1]::real[]::vector FROM generate_series(1, 10) g")
            threads = [threading.Thread(target=writer, args=(k,)) for k in range(8)]
            for th in threads:
                th.start()
            for th in threads:
                th.join()
            pages = index_pages(c, 'ei')
            keys, order = ledger(pages)
            live = ctids(c, 'e')
            check('I2 eight sessions: 1000 + 1200 keys', len(keys) == 2200, str(len(keys)))
            check('I2 ordinals are contiguous', [k[0] for k in keys] == list(range(len(keys))))
            check('I2 and the heavy concurrent workload did not trip the growth monitor', descriptor(pages)['state'] == 1 and descriptor(pages)['flags'] & 2 == 0, str(descriptor(pages)))
            c.scalar("INSERT INTO e SELECT 800000 + g, ARRAY[g, 8, 8]::real[]::vector FROM generate_series(1, 9000) g")
            pages = index_pages(c, 'ei')
            check('I2 and 9000 more keyed rows (over 256 pages of graph growth in total) still leave it certified: the size seen is refreshed with every key',
                  descriptor(pages)['state'] == 1 and descriptor(pages)['flags'] & 2 == 0, str(descriptor(pages)))
            pages = index_pages(c, 'ei')
            keys, order = ledger(pages)
            live = ctids(c, 'e')
            check('I2 every row has exactly one key', len({(k[1], k[2]) for k in keys}) == len(keys) and {(k[1], k[2]) for k in keys} == live)
            check('I2 11200 keys fill ceil(11200 / 338) = 34 pages and the tail is the last', len(keys) == 11200 and len(order) == 34 and descriptor(pages)['tail'] == order[-1], str(order))

            # I3
            rc, res = c.q("BEGIN; INSERT INTO e SELECT 900000 + g, ARRAY[g, 9, 9]::real[]::vector FROM generate_series(1, 20) g; ROLLBACK")
            pages = index_pages(c, 'ei')
            keys, order = ledger(pages)
            check('I3 the rolled-back rows left their keys', len(keys) == 11220, str(len(keys)))
            check('I3 the keys are a superset of the live rows', ctids(c, 'e') <= {(k[1], k[2]) for k in keys})

            # I4
            c.scalar("INSERT INTO e SELECT 500000 + g, ARRAY[g, 5, 5]::real[]::vector FROM generate_series(1, 300) g")
            committed = ctids(c, 'e')
            c.stop('immediate')
            c.start()
            pages = index_pages(c, 'ei')
            keys, order = ledger(pages)
            check('I4 after a crash every committed row still has its key', committed <= {(k[1], k[2]) for k in keys}, f'{len(committed)} rows, {len(keys)} keys')
            check('I4 the chain is still intact (ordinals contiguous)', [k[0] for k in keys] == list(range(len(keys))))

            # I5: no descriptor, OFF, old symbol
            c.make_index('t', 'ti')
            before = len(index_pages(c, 'ti'))
            c.scalar("INSERT INTO t SELECT 5000 + g, ARRAY[g, 1, 1]::real[]::vector FROM generate_series(1, 200) g")
            pages = index_pages(c, 'ti')
            check('I5 an index without a descriptor gets no ledger page', not any(struct.unpack_from('<H', p, 8190)[0] == 0xC0E1 for p in pages))
            c.scalar('CREATE TABLE t2(id int, v vector(3))')
            c.scalar('CREATE INDEX t2i ON t2 USING pg_cuvs_hnsw (v vector_l2_ops)')
            path2 = c.data / c.scalar("SELECT pg_relation_filepath('t2i')")
            c.stop()
            rp = subprocess.run([str(plant), str(path2), 'off'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            check('I5 planting is refused on a CERTIFIED index (it only plants on legacy ones)', rp.returncode != 0, rp.stdout)
            c.start()
            # OFF: build a legacy-shaped index and plant OFF
            c.make_index('t3', 't3i')
            path3 = c.data / c.scalar("SELECT pg_relation_filepath('t3i')")
            c.stop()
            rp = subprocess.run([str(plant), str(path3), 'off'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            check('I5 an OFF mark was planted', rp.returncode == 0, rp.stdout)
            c.start()
            n_before = len(index_pages(c, 't3i'))
            c.scalar("INSERT INTO t3 SELECT 7000 + g, ARRAY[g, 2, 2]::real[]::vector FROM generate_series(1, 50) g")
            pages = index_pages(c, 't3i')
            check('I5 an OFF index takes inserts and gets no ledger page', not any(struct.unpack_from('<H', p, 8190)[0] == 0xC0E1 for p in pages))

            # I6: a descriptor that names a ledger page that is a graph page: nothing is written to that page, the insert goes on
            # without a key, and the index is LOST (ledger damaged) with no chain
            c.make_index('t4', 't4i')
            path4 = c.data / c.scalar("SELECT pg_relation_filepath('t4i')")
            c.stop()
            rp = subprocess.run([str(plant), str(path4), 'certified'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            check('I6 a CERTIFIED descriptor was planted (ledger head 2 is a graph page of this index)', rp.returncode == 0, rp.stdout)
            c.start()
            snapshot = index_pages(c, 't4i')[2]
            rc, res = c.q("INSERT INTO t4 VALUES (99999, '[1,2,3]')")
            check('I6 the insert goes through with a warning that names the ledger page', rc == 0 and 'WARNING' in res and 'ledger page 2' in res, res)
            check('I6 and the page it named is untouched', index_pages(c, 't4i')[2] == snapshot)
            c.scalar('CHECKPOINT')
            dd6 = descriptor(index_pages(c, 't4i'))
            check('I6 the descriptor is LOST (ledger damaged, reason 11), has no chain and graph_epoch 1',
                  dd6['state'] == 2 and dd6['reason'] == 11 and dd6['head'] == 0xFFFFFFFF and dd6['tail'] == 0xFFFFFFFF and dd6['epoch'] == 1 and dd6['crc_ok'], str(dd6))
            rc, res = c.q("INSERT INTO t4 VALUES (99998, '[1,2,3]')")
            check('I6 the next insert goes through without a warning (nothing left to drop)', rc == 0 and 'WARNING' not in res, res)
            st6 = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('t4i')::text"))
            check('I6 the status function says lost and why', st6.get('class') == 'lost' and st6.get('guarantee_in_force') is False and 'ledger page did not read' in st6.get('pending', ''), str(st6))
            rc, res = c.q("SET enable_seqscan = off; SELECT id FROM t4 ORDER BY v <-> '[1,2,3]' LIMIT 1")
            check('I6 a scan answers from the heap', rc == 0 and 'answers from the heap' in res, res)

            # I7: the descriptor says generation 2, the ledger page still says 1: the same
            c.scalar('CREATE TABLE g(id int, v vector(3))')
            c.scalar('CREATE INDEX gi ON g USING pg_cuvs_hnsw (v vector_l2_ops)')
            pathg = c.data / c.scalar("SELECT pg_relation_filepath('gi')")
            c.stop()
            data = bytearray(pathg.read_bytes())
            upper = struct.unpack_from('<H', data, 14)[0]
            struct.pack_into('<Q', data, upper + 16, 2)
            struct.pack_into('<I', data, upper + 180, dg.crc32c(bytes(data[upper:upper + 180])))
            pathg.write_bytes(bytes(data))
            c.start()
            rc, res = c.q("INSERT INTO g VALUES (1, '[1,2,3]')")
            check('I7 a ledger page of another generation: the insert goes through with a warning', rc == 0 and 'WARNING' in res and 'ledger page 2' in res, res)

            # I9: a CERTIFIED descriptor of a newer format (readable, not writable) refuses the insert instead of skipping the key
            c.make_index('t9', 't9i')
            path9 = c.data / c.scalar("SELECT pg_relation_filepath('t9i')")
            c.stop()
            rp = subprocess.run([str(plant), str(path9), 'newer'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            check('I9 a CERTIFIED descriptor of a newer format was planted', rp.returncode == 0, rp.stdout)
            c.start()
            st9 = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('t9i')::text"))
            check('I9 the status function reads it and says it is not writable and not in force', st9.get('class') == 'certified' and st9.get('writable') is False and st9.get('guarantee_in_force') is False, str(st9))
            rc, res = c.q("INSERT INTO t9 VALUES (1, '[1,2,3]')")
            check('I9 the insert is refused and says why', rc != 0 and 'newer pg_cuvs' in res, res)

            # I9b: the same for a format this library cannot read (min_reader_version above ours): UNTRUSTED for reads, refused for inserts
            c.make_index('t9b', 't9bi')
            path9b = c.data / c.scalar("SELECT pg_relation_filepath('t9bi')")
            c.stop()
            rp = subprocess.run([str(plant), str(path9b), 'future'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            check('I9b a descriptor of a format that cannot be read was planted', rp.returncode == 0, rp.stdout)
            c.start()
            st9b = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('t9bi')::text"))
            check('I9b the status function says untrusted (future format)', st9b.get('class') == 'untrusted' and st9b.get('untrusted_reason') == 'future_format' and st9b.get('guarantee_in_force') is False, str(st9b))
            rc, res = c.q("INSERT INTO t9b VALUES (1, '[1,2,3]')")
            check('I9b the insert is refused too', rc != 0 and 'newer pg_cuvs' in res, res)

            # I8: an old library inserts a lot without keys
            c.scalar('CREATE TABLE m(id int, v vector(3))')
            c.scalar('CREATE INDEX mi ON m USING pg_cuvs_hnsw (v vector_l2_ops)')
            c.scalar("INSERT INTO m SELECT g, ARRAY[g, 1, 1]::real[]::vector FROM generate_series(1, 50) g")
            c.stop()
            o.install('old')
            c.start()
            c.scalar(f"CREATE OR REPLACE FUNCTION pg_cuvs_hnsw_handler(internal) RETURNS index_am_handler AS '$libdir/pg_cuvs', '{dg.OLD_SYMBOL}' LANGUAGE C")
            c.scalar("INSERT INTO m SELECT 1000 + g, ARRAY[g, 2, 2]::real[]::vector FROM generate_series(1, 12000) g")
            c.stop()
            o.install('new')
            c.start()
            # the unsupported manual switch back to the new symbol: exactly the case the monitor exists for
            c.scalar(f"CREATE OR REPLACE FUNCTION pg_cuvs_hnsw_handler(internal) RETURNS index_am_handler AS '$libdir/pg_cuvs', '{dg.NEW_SYMBOL}' LANGUAGE C")
            pages = index_pages(c, 'mi')
            check('I8 before the next keyed insert the descriptor still says certified (nothing noticed yet)', descriptor(pages)['state'] == 1)
            keys_before = len(ledger(pages)[0])
            c.scalar("INSERT INTO m VALUES (99999, '[1,2,3]')")
            pages = index_pages(c, 'mi')
            dd = descriptor(pages)
            check('I8 the next keyed insert moved the descriptor to LOST (unkeyed_writer) with the SUSPECT flag and graph_epoch 1',
                  dd['state'] == 2 and dd['reason'] == 7 and dd['flags'] & 2 and dd['epoch'] == 1 and dd['crc_ok'], str(dd))
            keys, _ = ledger(pages)
            check('I8 and that insert still got its key', len(keys) == keys_before + 1)
            c.scalar("INSERT INTO m VALUES (99998, '[1,2,3]')")
            pages = index_pages(c, 'mi')
            check('I8 a LOST index with a chain keeps getting keys and is not flagged again', len(ledger(pages)[0]) == keys_before + 2 and descriptor(pages)['epoch'] == 1)
            st = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('mi')::text"))
            check('I8 the status function says lost, not in force', st.get('class') == 'lost' and st.get('guarantee_in_force') is False, str(st))

            # I10: the same rows inserted without keys, noticed by the next insert when that insert is verified (cuvs.cover_verify_inserts = on) and
            # needs no key: a verified insert is an event of the ledger like a key, with the same growth threshold
            c.scalar('CREATE TABLE m2(id int, v vector(3))')
            c.scalar('CREATE INDEX mi2 ON m2 USING pg_cuvs_hnsw (v vector_l2_ops)')
            c.scalar("INSERT INTO m2 SELECT g, ARRAY[g, 1, 1]::real[]::vector FROM generate_series(1, 50) g")
            c.stop()
            o.install('old')
            c.start()
            c.scalar(f"CREATE OR REPLACE FUNCTION pg_cuvs_hnsw_handler(internal) RETURNS index_am_handler AS '$libdir/pg_cuvs', '{dg.OLD_SYMBOL}' LANGUAGE C")
            c.scalar("INSERT INTO m2 SELECT 1000 + g, ARRAY[g, 2, 2]::real[]::vector FROM generate_series(1, 12000) g")
            c.stop()
            o.install('new')
            c.start()
            c.scalar(f"CREATE OR REPLACE FUNCTION pg_cuvs_hnsw_handler(internal) RETURNS index_am_handler AS '$libdir/pg_cuvs', '{dg.NEW_SYMBOL}' LANGUAGE C")
            check('I10 before the next insert the descriptor still says certified', descriptor(index_pages(c, 'mi2'))['state'] == 1)
            rc, res = c.q("SET cuvs.cover_verify_inserts = on; INSERT INTO m2 VALUES (99999, '[1,2,3]')")
            dd = descriptor(index_pages(c, 'mi2'))
            check('I10 the next insert, verified, moved the descriptor to LOST (unkeyed_writer) with the SUSPECT flag and graph_epoch 1',
                  rc == 0 and dd['state'] == 2 and dd['reason'] == 7 and dd['flags'] & 2 and dd['epoch'] == 1 and dd['crc_ok'], f'{res[:100]} {dd}')
        finally:
            c.drop()

        # I5b: the old symbol builds an index that takes inserts without a ledger
        c = dg.Cluster(o, 'i5b', out, args.port)
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar("CREATE EXTENSION pg_cuvs VERSION '0.9.2'")
            c.scalar('CREATE TABLE o(id int, v vector(3))')
            c.scalar('CREATE INDEX oi ON o USING pg_cuvs_hnsw (v vector_l2_ops)')
            c.scalar("INSERT INTO o SELECT g, ARRAY[g, 1, 1]::real[]::vector FROM generate_series(1, 400) g")
            pages = index_pages(c, 'oi')
            check('I5b an index of the old symbol takes inserts and has no ledger page', not any(struct.unpack_from('<H', p, 8190)[0] == 0xC0E1 for p in pages))
        finally:
            c.drop()
    finally:
        (out / 'transcript.txt').write_text('\n'.join(dg.TRANSCRIPT) + '\n')
        shutil.rmtree(root, ignore_errors=True)
        for d in out.glob('data-*'):
            shutil.rmtree(d, ignore_errors=True)
    result = {'failures': dg.FAILURES, 'verdict': 'FAIL' if dg.FAILURES else 'PASS',
              'not_run': 'inserts into indexes built over rows (need the CAGRA daemon / GPU)'}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 1 if dg.FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
