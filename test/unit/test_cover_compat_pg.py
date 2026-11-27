#!/usr/bin/env python3
"""One index, the library of the fix bundle and the library of the feature bundle taking turns (CPU only).

The feature bundle writes published_at (offset 120 of the descriptor, a reserved area of the fix bundle) and the fix bundle writes cert_dropped_keys
(offset 128), so that an upgrade to the feature library and a return to the fix library both leave an index that every library reads right
(a library that publishes a certification used to leave the reserved bytes of the certification before, and the later library read a
count of 7 keys where there were none). --old-lib is the library of the fix bundle, --new-lib that of the feature bundle.

  K1 the fix library certifies: no published_at, keys since the certification 0;
  K2 the feature library reads it as it is (certified, in force, no published_at), counts the inserts made with it, and its own recertification writes published_at;
  K3 the fix library keeps the bytes of that certification through an insert (published_at is still in the descriptor);
  K4 a value in cert_dropped_keys of the feature certification is replaced by the fix library's recertification: the feature library then reads no published_at and
     the keys since the certification that are there (none), not the older ones;
  K5 the insert mark set by the feature library is read past by the fix library and kept by its recertification; the feature library then walks
     the index once more, clears the mark and leaves the index alone.
"""
import argparse
import json
from pathlib import Path
import shutil
import struct
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402
import test_cover_walk_pg as tw  # noqa: E402

check = dg.check
CONST = tw.CONST


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True, help='pg_cuvs.so of the feature bundle')
    ap.add_argument('--old-lib', type=Path, required=True, help='pg_cuvs.so of the fix bundle')
    ap.add_argument('--port', type=int, default=5941)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'cmp', out, args.port)
        c.auto_recert = False
        c.extra_opts = '-c autovacuum=off -c cuvs.cover_auto_recertify=off'
        try:
            def switch(which):
                c.stop()
                o.install(which)
                c.start()

            def raw(path):
                c.scalar('CHECKPOINT')
                return tw.descriptor_bytes(path)[1]

            o.install('old')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')
            c.scalar('CREATE TABLE k(id int, v vector(8))')
            c.scalar('CREATE INDEX ki ON k USING pg_cuvs_hnsw (v vector_l2_ops)')
            c.scalar(f"INSERT INTO k SELECT x, {CONST} FROM generate_series(1, 2000) x")
            path = c.data / c.scalar("SELECT pg_relation_filepath('ki')")
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('ki')::text")
            st = tw.status(c, 'ki')
            check('K1 the fix library certified the index; it has no published_at and no keys since the certification',
                  rc == 0 and '"result": "done"' in res and st.get('class') == 'certified' and 'published_at' not in st and st.get('keys_since_certification') == 0, f'{res} {st}')

            switch('new')
            st = tw.status(c, 'ki')
            check('K2 the feature library reads it as it is: certified, in force, no published_at, no keys since', st.get('class') == 'certified' and st.get('guarantee_in_force') is True
                  and 'published_at' not in st and st.get('keys_since_certification') == 0, str(st))
            c.scalar(f"INSERT INTO k SELECT 5000 + x, {CONST} FROM generate_series(1, 50) x")
            st = tw.status(c, 'ki')
            check('K2 it counts the 50 inserts made with it', st.get('keys_since_certification') == 50, str(st))
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('ki')::text")
            st = tw.status(c, 'ki')
            d = raw(path)
            check('K2 its own recertification writes published_at (the descriptor holds the time, the status shows it)',
                  rc == 0 and '"result": "done"' in res and 'published_at' in st and struct.unpack_from('<q', d, 120)[0] > 0, f'{res} {st}')

            switch('old')
            published = raw(path)[120:128]
            c.scalar(f"INSERT INTO k SELECT 9000 + x, {CONST} FROM generate_series(1, 700) x")		# the descriptor is rewritten for the new ledger pages
            d = raw(path)
            st = tw.status(c, 'ki')
            check('K3 the fix library reads the index certified, in force, and an insert keeps the bytes of the feature certification (published_at)',
                  st.get('class') == 'certified' and st.get('guarantee_in_force') is True and d[120:128] == published and struct.unpack_from('<q', d, 120)[0] > 0, str(st))

            # K4: a value of the feature certification that would be wrong for the next one
            c.stop()
            upper, dd = tw.descriptor_bytes(path)
            dd = bytearray(dd)
            struct.pack_into('<Q', dd, 128, 7)
            dd[180:184] = struct.pack('<I', dg.crc32c(bytes(dd[:180])))
            with open(path, 'r+b') as f:
                f.seek(upper)
                f.write(bytes(dd))
            c.start()
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('ki')::text")
            d = raw(path)
            check('K4 the fix library recertified; its publication wrote its own cert_dropped_keys (0) and cleared the published time',
                  rc == 0 and '"result": "done"' in res and struct.unpack_from('<Q', d, 128)[0] == 0 and d[120:128] == bytes(8), f'{res} {d[120:136].hex()}')
            switch('new')
            st = tw.status(c, 'ki')
            check('K4 the feature library reads that certification: no published_at, no keys since it (not 7, not the ones of the certification before)',
                  st.get('class') == 'certified' and 'published_at' not in st and st.get('keys_since_certification') == 0, str(st))

            # K5: the insert mark of the descriptor (flags 0x0004 and 0x0008) is unknown to the fix library, which reads the index as before and
            # whose recertification keeps the bits (it clears only the suspect flag). The feature library then finds the mark although nothing was inserted
            # since that certification: its sweep walks the index once more, clears the mark, and leaves the index alone afterwards.
            c.scalar(f"INSERT INTO k SELECT 20000 + x, {CONST} FROM generate_series(1, 1) x")
            st = tw.status(c, 'ki')
            check('K5 an insert with the feature library sets the mark', st.get('inserted_since_certification') is True, str(st))
            switch('old')
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('ki')::text")
            flags = struct.unpack_from('<H', raw(path), 12)[0]
            st = tw.status(c, 'ki')
            check('K5 the fix library reads the marked index as certified and in force, and its recertification keeps the two bits (flags 0x%04x)' % flags,
                  rc == 0 and '"result": "done"' in res and st.get('class') == 'certified' and st.get('guarantee_in_force') is True and flags & 0x000c == 0x000c, f'{res} {st}')
            c.auto_recert = True
            c.extra_opts = '-c autovacuum=off -c cuvs.cover_recertify_max_age=10 -c cuvs.cover_recertify_min_interval=1'
            switch('new')
            st = tw.status(c, 'ki')
            g5 = st.get('generation')
            check('K5 the feature library finds the mark and no key since that certification', st.get('inserted_since_certification') is True
                  and st.get('keys_since_certification') == 0, str(st))
            t5 = time.time()
            while time.time() - t5 < 45 and tw.status(c, 'ki').get('generation') == g5:
                time.sleep(0.5)
            st = tw.status(c, 'ki')
            check('K5 its sweep walks the index once more (%.0f s) and clears the mark' % (time.time() - t5),
                  st.get('generation') != g5 and st.get('inserted_since_certification') is False, str(st))
            g6 = st.get('generation')
            time.sleep(25)
            check('K5 and then leaves the index alone (same generation 25 s later)', tw.status(c, 'ki').get('generation') == g6, str(tw.status(c, 'ki')))
        finally:
            c.drop()
    finally:
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
