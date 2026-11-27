#!/usr/bin/env python3
"""The order the guarantee stands on: the ledger key of a row reaches the WAL before pgvector inserts the row into the graph.

  O1 (every build) for single-row inserts into a cover index, the first WAL record that touches a ledger page comes before every
     record that touches a graph page (pg_waldump -r Generic of the LSN range of one INSERT, checked for five inserts);
  O2 (with --gate, a build with -DCUVS_COVER_RECERT_TEST_GATE) the insert is held between the key and the graph insert, another
     session checkpoints (the WAL up to there is durable), the server is killed (immediate) and restarted: the ledger has the key of
     the row that never got into the graph, and the row itself is not visible.

A library that inserted into the graph first would pass the tests of the other runners: the commit comes after both records.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402
import test_cover_insert_pg as ti  # noqa: E402

check = dg.check
LEDGER_MAGIC = 0x4C45444C


def ledger_blocks(pages):
    return {b for b in range(2, len(pages)) if struct.unpack_from('<I', pages[b], 24)[0] == LEDGER_MAGIC}


def count_keys(pages):
    n = 0
    for b in ledger_blocks(pages):
        magic, version, kind, gen, first, count, seen, crc = struct.unpack_from('<IHHQQIII', pages[b], 24)
        n += count
    return n


def wal_records(waldump, wal_dir, lsn0, lsn1, rfn):
    """For each Generic record in [lsn0, lsn1) the list of blocks of relation file `rfn` it touches."""
    r = subprocess.run([waldump, '-p', str(wal_dir), '-s', lsn0, '-e', lsn1, '-r', 'Generic'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    records = []
    for line in r.stdout.splitlines():
        if not line.startswith('rmgr: Generic'):
            continue
        blocks = [int(m.group(2)) for m in re.finditer(r'rel (\d+/\d+/\d+) blk (\d+)', line) if m.group(1).endswith('/' + rfn)]
        records.append(blocks)
    return r.returncode, r.stdout, records


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True)
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5521)
    ap.add_argument('--gate', action='store_true', help='--new-lib is built with -DCUVS_COVER_RECERT_TEST_GATE: run O2')
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    gate = out / 'gate'
    gate.mkdir()
    os.environ['CUVS_RECERT_GATE_DIR'] = str(gate)
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        waldump = str(o.bin / 'pg_waldump')
        c = dg.Cluster(o, 'ord', out, args.port)
        c.extra_opts = '-c autovacuum=off -c wal_level=replica'
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')
            c.scalar('CREATE TABLE t(id int, v vector(3))')
            c.scalar('CREATE INDEX ti ON t USING pg_cuvs_hnsw (v vector_l2_ops)')
            c.scalar("INSERT INTO t SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 60) g")
            rel = c.scalar("SELECT pg_relation_filepath('ti')")
            rfn = rel.split('/')[-1]
            path = c.data / rel

            # O1
            ok_all, detail = True, []
            for i in range(5):
                l0 = c.scalar('SELECT pg_current_wal_insert_lsn()')
                c.scalar(f"INSERT INTO t VALUES ({1000 + i}, '[{i},1,1]')")
                l1 = c.scalar('SELECT pg_current_wal_insert_lsn()')
                c.scalar('CHECKPOINT')
                led = ledger_blocks(ti.read_pages(path))
                rc, text, recs = wal_records(waldump, c.data / 'pg_wal', l0, l1, rfn)
                first_led = next((n for n, bl in enumerate(recs) if any(b in led for b in bl)), None)
                first_graph = next((n for n, bl in enumerate(recs) if any(b not in led and b >= 1 for b in bl)), None)
                good = rc == 0 and first_led is not None and first_graph is not None and first_led < first_graph
                ok_all &= good
                detail.append((i, rc, first_led, first_graph, recs))
            check('O1 for five single-row inserts, the first WAL record that touches a ledger page comes before the first that touches a graph page',
                  ok_all, str(detail)[:600])
            check('O1 and the records are there to be compared (each insert wrote at least two Generic records on the index)',
                  all(len(d[4]) >= 2 for d in detail), str([len(d[4]) for d in detail]))

            if args.gate:
                # O2
                c.scalar('CHECKPOINT')
                k0 = count_keys(ti.read_pages(path))
                (gate / 'insert_armed').write_text('')
                box = []
                th = threading.Thread(target=lambda: box.append(c.q("INSERT INTO t VALUES (2000, '[9,9,9]')")))
                th.start()
                for _ in range(300):
                    if (gate / 'insert_reached').exists():
                        break
                    time.sleep(0.1)
                reached = (gate / 'insert_reached').exists()
                c.scalar('CHECKPOINT')
                c.stop('immediate')
                th.join()
                (gate / 'insert_armed').unlink()
                c.start()
                c.scalar('CHECKPOINT')
                k1 = count_keys(ti.read_pages(path))
                rc, vis = c.q("SET enable_indexscan = off; SET enable_bitmapscan = off; SELECT count(*) FROM t WHERE id = 2000")
                check('O2 the insert was held between the key and the graph insert', reached)
                check('O2 after the kill and the recovery the ledger has the key of the row that never got into the graph', k1 == k0 + 1, f'{k0} -> {k1}')
                check('O2 and that row is not visible (its transaction never committed)', rc == 0 and vis.strip().splitlines()[-1] == '0', vis)
                c.scalar("INSERT INTO t VALUES (2001, '[8,8,8]')")
                c.scalar('CHECKPOINT')
                check('O2 later inserts go on writing keys', count_keys(ti.read_pages(path)) == k1 + 1)
        finally:
            c.drop()
    finally:
        (out / 'transcript.txt').write_text('\n'.join(dg.TRANSCRIPT) + '\n')
        shutil.rmtree(root, ignore_errors=True)
        for d in out.glob('data-*'):
            shutil.rmtree(d, ignore_errors=True)
    result = {'failures': dg.FAILURES, 'verdict': 'FAIL' if dg.FAILURES else 'PASS',
              'not_run': 'a crash inside the key write (between the tail page and the metapage of a tail extension)' + ('' if args.gate else '; O2 (build without the gate)')}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 1 if dg.FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
