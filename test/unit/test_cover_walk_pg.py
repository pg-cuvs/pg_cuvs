#!/usr/bin/env python3
"""The recertification while the table is written and used (CPU only, needs a library built with -DCUVS_COVER_RECERT_TEST_GATE).

An independent review found four guards that no runner failed for when the guard was taken out (the mutation runs M1, M2, M5 and M9):
the carried-over keys of the walk, the drop of the ones the walk lists itself, the cover lock of an insert, and the position the next
copy starts from. The gates let a test do what the timing of a real walk does not: put inserts into the middle of it.

  W1 inserts made while the walk waits at its start (gate "walk"): every one of them has its row in the published ledger (BASE or KEYS), none twice;
     the test requires both kinds among them (a row the walk lists itself, whose key is not carried over, and one it reaches, whose key is);
  W3 an insert that wrote its key and has not inserted into the graph yet holds the cover lock: the recertification cannot take its first
     phase (deferred, nothing published) until the insert ends, and then it works;
  W4 a statement that needs a lock that conflicts with the table lock of a recertification (ALTER TABLE) is not made to wait for the whole
     walk: the walk gives way (deferred, nothing written), and the statement runs;
  W5 the lists of rows a recertification holds count against the memory limit: with a limit that fits the walk but not the list of the
     rows it cannot reach, the recertification fails (and says so), with a bigger one it works;
  W1 (also) the inserts of the walk whose keys were not carried over still count as keys since the certification (cert_dropped_keys);
  W6 a descriptor with bytes in the reserved area that a later library version uses (published_at, and the rest) is read as it is (certified, in
     force); an insert and the take-off of a VACUUM keep the bytes; the publication of a new certification clears them.
"""
import argparse
import json
import os
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
CONST = "'[0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5]'::vector"
RAND = "(SELECT array_agg(random()::real) FROM generate_series(1, 8) d)::vector"
# the same, but a subquery that depends on the outer row x (PostgreSQL evaluates one that does not once, and every row would hold the same vector)
RAND_X = "(SELECT array_agg(random()::real) FROM generate_series(1, 8) d WHERE x > 0)::vector"


def status(c, idx):
    return json.loads(c.scalar(f"SELECT pg_cuvs_hnsw_cover_status('{idx}')::text"))


def jres(rc, res):
    """The JSON answer of a function call; a WARNING in front of it (psql prints both) fails the test that looks for none, not the parsing."""
    lines = [ln for ln in res.splitlines() if ln.startswith('{')]
    return json.loads(lines[-1]) if rc == 0 and lines else {}


def tids(c, where):
    rows = json.loads(c.scalar(f"SELECT json_agg(ctid::text) FROM w WHERE {where}"))
    return {tuple(int(x) for x in t.strip('()').split(',')) for t in rows}


def wait_for(path, seconds=60):
    for _ in range(int(seconds * 10)):
        if path.exists():
            return True
        time.sleep(0.1)
    return False


def gated_recert(c, gate, name, index, during):
    """Start a recertification, wait until it reaches the gate `name`, run `during()`, open the gate, return (reached, (rc, answer))."""
    for f in gate.iterdir():
        f.unlink()
    (gate / f'{name}_armed').write_text('')
    box = []
    th = threading.Thread(target=lambda: box.append(c.q(f"SELECT pg_cuvs_cover_recertify('{index}')::text")))
    th.start()
    reached = wait_for(gate / f'{name}_reached')
    try:
        during()
    finally:
        (gate / f'{name}_go').write_text('')
        th.join()
        (gate / f'{name}_armed').unlink()
    return reached, box[0]


def descriptor_bytes(path):
    with open(path, 'rb') as f:
        meta = f.read(8192)
    upper = struct.unpack_from('<H', meta, 14)[0]
    return upper, meta[upper:upper + 184]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True, help='pg_cuvs.so built with -DCUVS_COVER_RECERT_TEST_GATE')
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5921)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    gate = out / 'gate'
    gate.mkdir()
    os.environ['CUVS_RECERT_GATE_DIR'] = str(gate)
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'wk', out, args.port)
        c.extra_opts = '-c autovacuum=off'
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')
            c.scalar('CREATE TABLE w(id int, v vector(8))')
            c.scalar('CREATE INDEX wi ON w USING pg_cuvs_hnsw (v vector_l2_ops)')
            # rows that all hold one vector hang loose in the graph (an element's neighbours are pruned to one): the walk lists most of them
            c.scalar(f"INSERT INTO w SELECT x, {CONST} FROM generate_series(1, 3000) x")
            wpath = c.data / c.scalar("SELECT pg_relation_filepath('wi')")

            # W1: half of the rows inserted while the walk waits at its start hold the one vector (the walk lists most of those and drops their
            # keys), half are random (reachable: their keys are carried over)
            reached, (rc, res) = gated_recert(c, gate, 'walk', 'wi', lambda: c.scalar(
                f"INSERT INTO w SELECT 10000 + x, CASE WHEN x % 2 = 0 THEN {CONST} ELSE {RAND_X} END FROM generate_series(1, 300) x"))
            check('W1 the gate was reached and the recertification finished', reached and rc == 0 and '"result": "done"' in res, res)
            c.scalar('CHECKPOINT')
            keys, kinds, order, d = tr.ledger_all(ti.read_pages(wpath))
            have = {}
            for ordinal, hb, ho, kind in keys:
                have[(hb, ho)] = have.get((hb, ho), 0) + 1
            late = tids(c, 'id > 10000')
            in_base = {t for t in late if any(k[3] == 2 and (k[1], k[2]) == t for k in keys)}
            in_keys = {t for t in late if any(k[3] == 1 and (k[1], k[2]) == t for k in keys)}
            check('W1 the inserted rows are 300 and every one of them is in the ledger', len(late) == 300 and late <= set(have), f'{len(late - set(have))} missing')
            check('W1 no row is in the ledger twice', all(n == 1 for n in have.values()) and len(have) == len(keys), f'{sum(1 for n in have.values() if n > 1)} twice')
            check('W1 the premise: some of them are rows the walk listed itself (BASE) and some are carried-over keys', len(in_base) > 0 and len(in_keys) > 0,
                  f'{len(in_base)} in BASE, {len(in_keys)} carried over')
            check('W1 ordinals are contiguous and the descriptor is certified with an intact crc', [k[0] for k in keys] == list(range(len(keys))) and d['state'] == 1 and d['crc_ok'])
            # the inserts of the walk are inserts since the certification also when the walk listed their rows (their keys are BASE keys now)
            st1 = status(c, 'wi')
            _, desc1 = descriptor_bytes(wpath)
            check('W1 the descriptor counts the keys that were not carried over (cert_dropped_keys) and the status counts all 300 inserts of the walk as keys since the certification',
                  struct.unpack_from('<Q', desc1, 128)[0] == len(in_base) and st1.get('keys_since_certification') == 300,
                  f'cert_dropped_keys {struct.unpack_from("<Q", desc1, 128)[0]} (BASE among them {len(in_base)}), keys_since_certification {st1.get("keys_since_certification")}')

            # W3: an insert that wrote its key and waits before the graph holds the cover lock
            gen = status(c, 'wi').get('generation')
            for f in gate.iterdir():
                f.unlink()
            (gate / 'insert_armed').write_text('')
            ins = []
            th = threading.Thread(target=lambda: ins.append(c.q(f"INSERT INTO w VALUES (20001, {RAND})")))
            th.start()
            held = wait_for(gate / 'insert_reached')
            t0 = time.time()
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('wi')::text")
            took = time.time() - t0
            j = jres(rc, res)
            gen_during = status(c, 'wi').get('generation')
            (gate / 'insert_go').write_text('')
            th.join()
            (gate / 'insert_armed').unlink()
            check('W3 the insert reached its gate', held)
            check('W3 while it was held the recertification could not take its first phase: deferred after the wait bound, nothing published',
                  j.get('result') == 'deferred' and gen_during == gen and took >= 1.0, f'{res} generation {gen} -> {gen_during}, {took:.1f} s')
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('wi')::text")
            check('W3 when the insert has ended the recertification works, and no lock is released that it does not hold (no WARNING)',
                  rc == 0 and '"result": "done"' in res and 'WARNING' not in res, res)

            # W4: a statement that needs more than the table lock of a recertification does not wait for the whole walk
            gen = status(c, 'wi').get('generation')
            ddl = []

            def during():
                th2 = threading.Thread(target=lambda: ddl.append(c.q('ALTER TABLE w ADD COLUMN extra int')))
                th2.start()
                ddl.append(th2)
                for _ in range(100):
                    if c.scalar("SELECT count(*) FROM pg_locks WHERE NOT granted AND relation = 'w'::regclass") != '0':
                        break
                    time.sleep(0.1)

            reached, (rc, res) = gated_recert(c, gate, 'walk', 'wi', during)
            ddl[0].join()
            j = jres(rc, res)
            check('W4 the walk gave way: deferred, and it says another session waits', reached and j.get('result') == 'deferred' and 'waits for a lock' in j.get('detail', ''), res)
            check('W4 nothing was published and the statement ran', status(c, 'wi').get('generation') == gen and
                  c.scalar("SELECT count(*) FROM pg_attribute WHERE attrelid = 'w'::regclass AND attname = 'extra'") == '1')
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('wi')::text")
            check('W4 without a waiting statement the recertification works', rc == 0 and '"result": "done"' in res, res)

            # W5: the memory limit counts the lists
            c.scalar('CREATE TABLE w2(id int, v vector(8))')
            c.scalar('CREATE INDEX w2i ON w2 USING pg_cuvs_hnsw (v vector_l2_ops)')
            c.scalar(f"INSERT INTO w2 SELECT x, {CONST} FROM generate_series(1, 4000) x")
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('w2i')::text")
            j = jres(rc, res)
            mem, unreached = j.get('mem_bytes', 0), j.get('unreached', 0)
            limit = mem + 4096
            check('W5 the premise: the walk itself needs less than the limit, the list of unreached rows (48 bytes each) more', unreached > 0 and limit // 48 < unreached,
                  f'walk {mem} bytes, limit {limit}, {unreached} unreached rows')
            os.environ['CUVS_RECERT_GATE_RAM_LIMIT'] = str(limit)
            c.restart()
            c.scalar("INSERT INTO w2 VALUES (99999, " + CONST + ")")
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('w2i')::text")
            j = jres(rc, res)
            check('W5 with that limit the recertification fails over the memory limits, naming the list', j.get('result') == 'failed' and 'list of rows' in j.get('detail', ''), res)
            check('W5 the index keeps the certification it had', status(c, 'w2i').get('class') == 'certified')
            del os.environ['CUVS_RECERT_GATE_RAM_LIMIT']
            c.restart()
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('w2i')::text")
            check('W5 with the limit gone it works', rc == 0 and '"result": "done"' in res, res)

            # W6: the bytes of the descriptor that this version does not name. A descriptor whose reserved bytes hold values of a later
            # version (published_at, and others) is read as it is; a rewrite that is not a new certification (an insert that adds a ledger page, the
            # take-off of a VACUUM) keeps them; the publication of a new certification clears them, so that the later version does not read a value
            # of the certification before as one of the new one. cert_dropped_keys (128) is this version's own: the publication sets it, as every other
            # field of the certification.
            w2path = c.data / c.scalar("SELECT pg_relation_filepath('w2i')")
            c.stop()
            upper, desc = descriptor_bytes(w2path)
            desc = bytearray(desc)
            marks = {'120..127': (120, 128, bytes(range(11, 19))), '152..179': (152, 180, bytes(range(41, 69)))}
            for lo, hi, val in marks.values():
                desc[lo:hi] = val
            struct.pack_into('<Q', desc, 128, 7)			# cert_dropped_keys
            desc[180:184] = struct.pack('<I', dg.crc32c(bytes(desc[:180])))
            with open(w2path, 'r+b') as f:
                f.seek(upper)
                f.write(bytes(desc))
            c.start()
            st = status(c, 'w2i')
            check('W6 a descriptor with bytes in the reserved area reads as certified, the guarantee in force', st.get('class') == 'certified' and st.get('guarantee_in_force') is True, str(st))

            def raw():
                c.scalar('CHECKPOINT')
                return descriptor_bytes(w2path)[1]

            def kept(after, what):
                return after[120:128] == marks['120..127'][2] and after[152:180] == marks['152..179'][2] and struct.unpack_from('<Q', after, 128)[0] == 7 and \
                    dg.crc32c(after[:180]) == struct.unpack_from('<I', after, 180)[0], what

            c.scalar("INSERT INTO w2 SELECT 200000 + x, " + CONST + " FROM generate_series(1, 700) x")		# two more ledger pages: the descriptor is rewritten for the new tail
            check('W6 inserts that add ledger pages keep the bytes (an insert rewrites the descriptor with the new tail)', *kept(raw(), 'after the inserts'))
            c.scalar('DELETE FROM w2 WHERE id % 7 = 0')
            for f in gate.iterdir():
                f.unlink()
            (gate / 'vacuum_armed').write_text('')
            box = []
            th = threading.Thread(target=lambda: box.append(c.q('VACUUM w2')))
            th.start()
            held = wait_for(gate / 'vacuum_reached')
            mid = raw()
            st_mid = status(c, 'w2i')
            (gate / 'vacuum_go').write_text('')
            th.join()
            (gate / 'vacuum_armed').unlink()
            check('W6 the take-off of a VACUUM (LOST, before the graph changes) keeps the bytes', held and st_mid.get('class') == 'lost' and kept(mid, 'at the gate')[0], str(st_mid))
            after = raw()
            st_end = status(c, 'w2i')
            check('W6 the VACUUM ended with a new certification', box and box[0][0] == 0 and st_end.get('class') == 'certified', str(box))
            check('W6 its publication cleared the bytes this version does not name (120..127, 152..179)', after[120:128] == bytes(8) and after[152:180] == bytes(28), after.hex())
            check('W6 and set cert_dropped_keys itself (no insert ran during that walk: 0) and left no pending pages',
                  struct.unpack_from('<Q', after, 128)[0] == 0 and struct.unpack_from('<I', after, 136)[0] == 0 and dg.crc32c(after[:180]) == struct.unpack_from('<I', after, 180)[0],
                  f'dropped {struct.unpack_from("<Q", after, 128)[0]}')
        finally:
            c.drop()
    finally:
        os.environ.pop('CUVS_RECERT_GATE_RAM_LIMIT', None)
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
