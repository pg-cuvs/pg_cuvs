#!/usr/bin/env python3
"""The pages of a recertification that does not publish (CPU only, needs a library built with -DCUVS_COVER_RECERT_TEST_GATE).

a recertification writes the pages of its new ledger chain before it publishes them. When it gives up (abandoned, the cover lock of
the last phase deferred) or is cancelled, the pages were in no chain, nobody took them back, and once there were more of them than the limit of the
growth monitor of an insert (256 pages) the next ordinary insert took them for rows written without ledger keys and took the index off the guarantee
(LOST). The same monitor took the new chain of a recertification that did publish for foreign growth, when the index had to be extended for it and
an insert was made between the extension and the publication. The test sets the limit of the monitor to four pages (CUVS_RECERT_GATE_GROWTH_PAGES)
so that an index of 3,000 rows reaches it.

  O1 abandoned: the index grew by more than the limit, the descriptor names the pages, and one ordinary insert leaves the index CERTIFIED with its
     guarantee (no "grew by more than" line in the log);
  O2 the next recertification takes the pages back (reclaimed_pages), the index does not grow, the descriptor names nothing afterwards and the
     ledger is exactly what an independent walk of the graph cannot reach;
  O3 cancelled: the same as O1 and O2;
  O4 repeated: abandoning and cancelling again and again does not make the index grow;
  O5 the cover lock of the last phase deferred (an insert holds it): the same as O1 and O2;
  O6 a recertification that publishes, on an index that has to be extended for its new chain, with an insert between: done, CERTIFIED, no line;
  O7 a pending_head that is not an orphan (the live chain's head, or a generation that is not newer than the descriptor's) is not taken back;
  O8 growth that nobody accounted for is still taken for rows written without keys (the monitor still works): the descriptor of a CERTIFIED index is
     made to remember a relation size smaller than it is by more than the limit, and one insert takes it off;
  O9 the process dies (kill -9 of the backend, or pg_ctl stop -m immediate) after the new chain was written and before it was published: the
     descriptor still names the pages after crash recovery, one insert leaves the index CERTIFIED, the next recertification takes the pages back;
  O10 the same, between the extension of the index and the writing of the pages: the page the monitor reads still remembers the new size, one
     insert leaves the index CERTIFIED (the pages are in no list: the index keeps them, they do not make it LOST).
 the records that say these things (the page that remembers the size, the descriptor that names the pages) were in the WAL buffers
 when the process died, the extension of the file was not, and recovery gave back an index that was larger than the page said. The cluster of this
 test has wal_writer_delay at 10 s, so that nothing but the code under test flushes the WAL between the gate and the kill.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402
import test_cover_insert_pg as ti  # noqa: E402
import test_cover_recert_pg as tr  # noqa: E402
import test_cover_vacuum_pg as tv  # noqa: E402
import test_cover_walk_pg as tw  # noqa: E402

check = dg.check
CONST = tw.CONST
LIMIT = 4


def nblocks(c, idx):
    return int(c.scalar(f"SELECT pg_relation_size('{idx}') / 8192"))


def pending(c, path):
    """(pending_head, pending_generation, generation, ledger_head) from the descriptor in the file (after a checkpoint)."""
    c.scalar('CHECKPOINT')
    _, d = tw.descriptor_bytes(path)
    return (struct.unpack_from('<I', d, 136)[0], struct.unpack_from('<Q', d, 144)[0], struct.unpack_from('<Q', d, 16)[0], struct.unpack_from('<I', d, 24)[0])


def log_has_growth_line(c):
    return 'grew by more than' in tv.server_log(c)


def growth_lines(c):
    return tv.server_log(c).count('grew by more than')


def wait_until_up(c, seconds=300):
    """The postmaster restarts every backend after one died from a signal: wait until it takes connections again."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        rc, _ = c.q('SELECT 1')
        if rc == 0:
            return True
        time.sleep(0.5)
    return False


def class_of(c, idx):
    st = tw.status(c, idx)
    return st.get('class'), st.get('guarantee_in_force')


SLACK = 2	# a new chain may need a page more than the one it replaces (a key or an unreached row more can start a page): not growth by the pages of an orphan chain


def ledger_is_what_the_walk_cannot_reach(c, path):
    c.scalar('CHECKPOINT')
    keys, kinds, order, d = tr.ledger_all(ti.read_pages(path))
    unreached, rc = tr.unreachable_tids(path)
    base = {(k[1], k[2]) for k in keys if k[3] == 2}
    return base == unreached and len(base) > 0 and d['state'] == 1 and d['crc_ok'], f'{len(base)} BASE keys, {len(unreached)} unreached, state {d["state"]}'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True, help='pg_cuvs.so built with -DCUVS_COVER_RECERT_TEST_GATE')
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5931)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    gate = out / 'gate'
    gate.mkdir()
    os.environ['CUVS_RECERT_GATE_DIR'] = str(gate)
    os.environ['CUVS_RECERT_GATE_GROWTH_PAGES'] = str(LIMIT)
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'orph', out, args.port)
        c.extra_opts = '-c autovacuum=off -c max_parallel_maintenance_workers=0 -c wal_writer_delay=10s'
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')
            nid = [100000]

            def mk(tag):
                """A table of 3,000 rows that all hold one vector and an index on it, built while the table was empty (so CERTIFIED, with a ledger chain of
                keys and an empty free list: the first recertification of it has to extend the index for its new chain)."""
                c.scalar(f'CREATE TABLE t{tag}(id int, v vector(8))')
                c.scalar(f'CREATE INDEX i{tag} ON t{tag} USING pg_cuvs_hnsw (v vector_l2_ops)')
                c.scalar(f"INSERT INTO t{tag} SELECT x, {CONST} FROM generate_series(1, 3000) x")
                return f'i{tag}', f't{tag}', c.data / c.scalar(f"SELECT pg_relation_filepath('i{tag}')")

            def plain_insert(tbl):
                nid[0] += 1
                c.scalar(f"INSERT INTO {tbl} VALUES ({nid[0]}, {CONST})")

            def recertify(idx):
                rc, res = c.q(f"SELECT pg_cuvs_cover_recertify('{idx}')::text")
                return rc, res, tw.jres(rc, res)

            def abandon():
                (gate / 'recert_touch').write_text('e')

            def cancel():
                c.scalar("SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE query LIKE '%pg_cuvs_cover_recertify%' AND pid <> pg_backend_pid()")

            def no_touch():
                if (gate / 'recert_touch').exists():
                    (gate / 'recert_touch').unlink()

            # O0 the setup is what the other cases need
            ix, tb, pth = mk('0')
            rc, res, j = recertify(ix)
            check('O0 setup: a recertification of such an index works and its new chain is longer than the limit of the monitor',
                  rc == 0 and j.get('result') == 'done' and j.get('unreached', 0) > LIMIT * 338, res)
            plain_insert(tb)
            check('O0 setup: an insert after it leaves the index certified', class_of(c, ix) == ('certified', True), str(tw.status(c, ix)))

            # O1, O2: abandoned
            ix, tb, pth = mk('1')
            before = nblocks(c, ix)
            reached, (rc, res) = tw.gated_recert(c, gate, 'recert', ix, abandon)
            no_touch()
            after = nblocks(c, ix)
            j = tw.jres(rc, res)
            ph, pg, gen, lhead = pending(c, pth)
            check('O1 the recertification was abandoned and left more pages than the limit of the monitor', reached and j.get('result') == 'abandoned' and after - before > LIMIT,
                  f'{res}: {before} -> {after} blocks')
            check('O1 the descriptor names the pages: pending_head is a block of the index, its generation is the next one', 2 <= ph < after and pg == gen + 1, f'head {ph} generation {pg} (descriptor {gen})')
            plain_insert(tb)
            check('O1 one ordinary insert leaves the index certified, the guarantee in force', class_of(c, ix) == ('certified', True), str(tw.status(c, ix)))
            check('O1 and the log has no line about rows inserted without keys', not log_has_growth_line(c), tv.server_log(c)[-600:])
            grown = after - before
            rc, res, j = recertify(ix)
            ph, pg, gen, lhead = pending(c, pth)
            check('O2 the next recertification took the pages back before it did anything else', rc == 0 and j.get('result') == 'done' and j.get('reclaimed_pages', 0) >= grown, res)
            check('O2 and the index did not grow (by more than the page a longer chain may need)', 0 <= nblocks(c, ix) - after <= SLACK < grown, f'{after} -> {nblocks(c, ix)}, orphans {grown}')
            check('O2 the descriptor names no pages any more', ph == 0 and pg == 0, f'head {ph} generation {pg}')
            ok, why = ledger_is_what_the_walk_cannot_reach(c, pth)
            check('O2 the ledger is exactly what an independent walk of the graph cannot reach, in a certified descriptor', ok, why)

            # O3: cancelled
            ix, tb, pth = mk('3')
            before = nblocks(c, ix)
            reached, (rc, res) = tw.gated_recert(c, gate, 'recert', ix, cancel)
            ph, pg, gen, lhead = pending(c, pth)
            n3 = nblocks(c, ix)
            check('O3 the recertification was cancelled, left more pages than the limit and the descriptor names them', reached and 'canceling statement' in res and ph != 0 and n3 - before > LIMIT,
                  f'{res} head {ph}, {before} -> {n3}')
            plain_insert(tb)
            check('O3 one ordinary insert leaves the index certified, the guarantee in force', class_of(c, ix) == ('certified', True), str(tw.status(c, ix)))
            check('O3 and the log has no line about rows inserted without keys', not log_has_growth_line(c))
            rc, res, j = recertify(ix)
            check('O3 the next recertification took the pages back and the index did not grow', j.get('result') == 'done' and j.get('reclaimed_pages', 0) > 0 and 0 <= nblocks(c, ix) - n3 <= SLACK,
                  f'{res}: {n3} -> {nblocks(c, ix)}')

            # O4: again and again (on the index of O3: its free list has the replaced chain now)
            sizes = []
            for _ in range(4):
                tw.gated_recert(c, gate, 'recert', ix, abandon)
                no_touch()
                sizes.append(nblocks(c, ix))
                tw.gated_recert(c, gate, 'recert', ix, cancel)
                sizes.append(nblocks(c, ix))
            rc, res, j = recertify(ix)
            sizes.append(nblocks(c, ix))
            check('O4 four times abandoned and four times cancelled, then recertified: the index is as large as it was after the first cancelled one', max(sizes) - min(sizes) <= SLACK and sizes[0] - n3 <= SLACK,
                  f'sizes {sizes}, before {n3}')
            check('O4 and the last recertification works', rc == 0 and j.get('result') == 'done', res)

            # O5: the cover lock of the last phase is held by an insert: deferred
            ix, tb, pth = mk('5')
            ins = []

            def hold_an_insert():
                (gate / 'insert_armed').write_text('')
                nid[0] += 1
                t = threading.Thread(target=lambda: ins.append(c.q(f"INSERT INTO {tb} VALUES ({nid[0]}, {CONST})")))
                t.start()
                ins.append(t)
                tw.wait_for(gate / 'insert_reached')

            before = nblocks(c, ix)
            reached, (rc, res) = tw.gated_recert(c, gate, 'recert', ix, hold_an_insert)
            (gate / 'insert_go').write_text('')
            ins[0].join()
            (gate / 'insert_armed').unlink()
            j = tw.jres(rc, res)
            ph, pg, gen, lhead = pending(c, pth)
            check('O5 the last phase could not take the cover lock: deferred, the pages are named, the index grew by more than the limit',
                  j.get('result') == 'deferred' and 'to publish' in j.get('detail', '') and ph != 0 and nblocks(c, ix) - before > LIMIT, f'{res} head {ph}')
            plain_insert(tb)
            check('O5 one ordinary insert leaves the index certified, the guarantee in force', class_of(c, ix) == ('certified', True), str(tw.status(c, ix)))
            n5 = nblocks(c, ix)
            rc, res, j = recertify(ix)
            ok, why = ledger_is_what_the_walk_cannot_reach(c, pth)
            check('O5 the next recertification took them back, the index did not grow and the ledger is right', j.get('result') == 'done' and j.get('reclaimed_pages', 0) > 0 and 0 <= nblocks(c, ix) - n5 <= SLACK and ok,
                  f'{res} {why}')

            # O6: a recertification that publishes, on an index it has to extend, with an insert between the extension and the publication
            ix, tb, pth = mk('6')
            before = nblocks(c, ix)
            reached, (rc, res) = tw.gated_recert(c, gate, 'recert', ix, lambda: c.scalar(f"INSERT INTO {tb} VALUES (200001, {CONST})"))
            j = tw.jres(rc, res)
            check('O6 the recertification extended the index by more than the limit (the premise)', nblocks(c, ix) - before > LIMIT and j.get('reused_pages', 99) < 3, f'{before} -> {nblocks(c, ix)}, {res}')
            check('O6 it published (not abandoned: the insert did not take the index off the guarantee) and the index is certified, the guarantee in force',
                  j.get('result') == 'done' and class_of(c, ix) == ('certified', True), f'{res} {tw.status(c, ix)}')
            check('O6 and the log has no line about rows inserted without keys', not log_has_growth_line(c))

            # O7: a pending_head that is not an orphan is not taken back (on the index of O6)
            c.scalar('CHECKPOINT')
            _, dsc = tw.descriptor_bytes(pth)
            gen = struct.unpack_from('<Q', dsc, 16)[0]
            lhead = struct.unpack_from('<I', dsc, 24)[0]
            ltail = struct.unpack_from('<I', dsc, 28)[0]
            for label, head, pgen in (('the live chain head, a newer generation', lhead, gen + 5), ('a live page, a generation that is not newer', ltail, gen)):
                c.stop()
                upper, dd = tw.descriptor_bytes(pth)
                dd = bytearray(dd)
                struct.pack_into('<I', dd, 136, head)
                struct.pack_into('<Q', dd, 144, pgen)
                dd[180:184] = struct.pack('<I', dg.crc32c(bytes(dd[:180])))
                with open(pth, 'r+b') as f:
                    f.seek(upper)
                    f.write(bytes(dd))
                c.start()
                rc, res, j = recertify(ix)
                ph, pg, gen2, lhead2 = pending(c, pth)
                ok, why = ledger_is_what_the_walk_cannot_reach(c, pth)
                check(f'O7 pending_head = {label}: nothing was taken back, the chain was not replaced, the recertification works and the ledger is right',
                      j.get('result') == 'done' and j.get('reclaimed_pages') == 0 and j.get('chain_replaced') is False and ph == 0 and ok, f'{res} head {ph} {why}')
                ltail = struct.unpack_from('<I', tw.descriptor_bytes(pth)[1], 28)[0]

            # O8: growth nobody accounted for is still taken for rows written without keys (the index of O6)
            c.scalar('CHECKPOINT')
            tail_blk = struct.unpack_from('<I', tw.descriptor_bytes(pth)[1], 28)[0]
            c.stop()
            with open(pth, 'r+b') as f:
                f.seek(tail_blk * 8192)
                page = bytearray(f.read(8192))
            # the ledger header starts at 24 (graph_nblocks_seen at +28, its crc at +32 over the first 32 bytes): the page remembers a smaller index
            hdr_at = 24
            seen = struct.unpack_from('<I', page, hdr_at + 28)[0]
            check('O8 setup: the tail page remembers a relation size', seen > LIMIT + 3, f'seen {seen}')
            struct.pack_into('<I', page, hdr_at + 28, seen - (LIMIT + 3))
            struct.pack_into('<I', page, hdr_at + 32, dg.crc32c(bytes(page[hdr_at:hdr_at + 32])))
            with open(pth, 'r+b') as f:
                f.seek(tail_blk * 8192)
                f.write(bytes(page))
            c.start()
            plain_insert(tb)
            st = tw.status(c, ix)
            check('O8 the monitor still takes growth that nobody accounted for: one insert and the index is LOST (unkeyed writer)',
                  st.get('class') == 'lost' and log_has_growth_line(c), str(st))

            # O9, O10: the process dies with the WAL of the recertification in the WAL buffers
            def kill_the_backend():
                pid = c.scalar("SELECT pid FROM pg_stat_activity WHERE query LIKE '%pg_cuvs_cover_recertify%' AND pid <> pg_backend_pid()")
                os.kill(int(pid), 9)
                check('O9 setup: the postmaster took the connections again after the backend was killed', wait_until_up(c))

            def stop_immediately():
                c.stop('immediate')
                c.start()

            def crashed(tag, gate_name, how, what):
                ix, tb, pth = mk(tag)
                before = nblocks(c, ix)
                reached, (rc, res) = tw.gated_recert(c, gate, gate_name, ix, how)
                for f in gate.iterdir():
                    f.unlink()
                after = nblocks(c, ix)
                ph, pg, gen, lhead = pending(c, pth)
                check(f'{what}: the process died at the gate (no answer) and the index is larger than before by more than the limit',
                      reached and rc != 0 and after - before > LIMIT, f'rc {rc} {res[:120]}: {before} -> {after}')
                return ix, tb, pth, after, ph, pg, gen

            for tag, how, label in (('9k', kill_the_backend, 'kill -9'), ('9i', stop_immediately, 'immediate stop')):
                ix, tb, pth, after, ph, pg, gen = crashed(tag, 'recert', how, f'O9 {label}')
                check(f'O9 {label}: after crash recovery the descriptor still names the pages of the new chain', 2 <= ph < after and pg == gen + 1, f'head {ph} generation {pg} (descriptor {gen})')
                n0 = growth_lines(c)
                plain_insert(tb)
                check(f'O9 {label}: one ordinary insert leaves the index certified, the guarantee in force, and the log has no new line about rows inserted without keys',
                      class_of(c, ix) == ('certified', True) and growth_lines(c) == n0, f'{tw.status(c, ix)} lines {n0} -> {growth_lines(c)}')
                rc, res, j = recertify(ix)
                ok, why = ledger_is_what_the_walk_cannot_reach(c, pth)
                check(f'O9 {label}: the next recertification took the pages back, the index did not grow and the ledger is right',
                      j.get('result') == 'done' and j.get('reclaimed_pages', 0) > LIMIT and 0 <= nblocks(c, ix) - after <= SLACK and ok, f'{res} {why}, {after} -> {nblocks(c, ix)}')
            for tag, how, label in (('10k', kill_the_backend, 'kill -9'), ('10i', stop_immediately, 'immediate stop')):
                ix, tb, pth, after, ph, pg, gen = crashed(tag, 'extended', how, f'O10 {label}')
                n0 = growth_lines(c)
                plain_insert(tb)
                check(f'O10 {label}: one ordinary insert leaves the index certified, the guarantee in force, and the log has no new line about rows inserted without keys',
                      class_of(c, ix) == ('certified', True) and growth_lines(c) == n0, f'{tw.status(c, ix)} lines {n0} -> {growth_lines(c)}')
                rc, res, j = recertify(ix)
                ok, why = ledger_is_what_the_walk_cannot_reach(c, pth)
                check(f'O10 {label}: the next recertification works and the ledger is right', j.get('result') == 'done' and ok, f'{res} {why}')
        finally:
            c.drop()
    finally:
        os.environ.pop('CUVS_RECERT_GATE_GROWTH_PAGES', None)
        os.environ.pop('CUVS_RECERT_GATE_DIR', None)
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
