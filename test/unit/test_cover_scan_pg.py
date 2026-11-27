#!/usr/bin/env python3
"""What an ordered scan of a pg_cuvs_hnsw index returns when the index has a cover descriptor (CPU only).

Indexes that can be had without the CAGRA daemon: those built over an empty table (CERTIFIED, every inserted row is in the
ledger) and pgvector hnsw indexes whose access method was switched in the catalog, with a descriptor planted by test/unit/
cover_plant.c. The reference is PostgreSQL's own exact sort (index scans off).

  S1 CERTIFIED: every ordered scan equals the exact ranking (the ledger holds all rows), for many queries and limits;
     rolled-back and deleted rows never appear;
  S2 LOST and UNTRUSTED (a damaged descriptor): exact ranking from the heap, with a notice; a planted CERTIFIED whose ledger
     cannot be read falls back to the whole heap as well;
  S3 cuvs.hnsw_exact_scan = on on a CERTIFIED index still gives the exact ranking;
  S4 an index without a descriptor and an OFF one are answered as before (no notice, approximate path);
  S5 a scan that cannot run exactly (row-level security) on an index with a descriptor is refused, not answered approximately.
"""
import argparse
import json
from pathlib import Path
import random
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402

check = dg.check


def vec(rng, d=8):
    return '[' + ','.join(f'{rng.random():.5f}' for _ in range(d)) + ']'


def ids(c, table, q, k, index_scan, extra=''):
    flags = 'SET enable_seqscan = off; SET enable_bitmapscan = off;' if index_scan else 'SET enable_indexscan = off; SET enable_bitmapscan = off;'
    rc, out = c.q(f"{flags} {extra} SELECT id FROM {table} ORDER BY v <-> '{q}' LIMIT {k}")
    return rc, out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True)
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5499)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    plant = out / 'cover_plant'
    r = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(repo / 'src'), str(repo / 'test/unit/cover_plant.c'), '-o', str(plant)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    assert r.returncode == 0, r.stdout
    rng = random.Random(7)
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'sc', out, args.port)
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')

            # S1
            c.scalar('CREATE TABLE e(id int, v vector(8))')
            c.scalar('CREATE INDEX ei ON e USING pg_cuvs_hnsw (v vector_l2_ops)')
            c.scalar("SELECT setseed(0.5); INSERT INTO e SELECT g, l.a::vector FROM generate_series(1, 3000) g, "
                     "LATERAL (SELECT array_agg(random()::real) AS a FROM generate_series(1, 8) k WHERE g > 0) l")
            rc, _ = c.q("BEGIN; INSERT INTO e VALUES (90001, '[0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5]'); ROLLBACK")
            c.scalar('DELETE FROM e WHERE id % 10 = 0')
            plan = c.scalar("SET enable_seqscan = off; EXPLAIN SELECT id FROM e ORDER BY v <-> '[0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5]' LIMIT 5")
            check('S1 the planner uses the index', 'Index Scan using ei' in plan, plan)
            bad = 0
            for _ in range(25):
                q = vec(rng)
                for k in (1, 10, 100):
                    rc1, got = ids(c, 'e', q, k, True)
                    rc2, want = ids(c, 'e', q, k, False)
                    if rc1 != 0 or rc2 != 0 or got != want:
                        bad += 1
            check('S1 75 ordered scans (25 queries x limits 1, 10, 100) equal the exact ranking', bad == 0, f'{bad} differ')
            rc, res = c.q("SET enable_seqscan = off; SELECT count(*) FROM e WHERE id = 90001 OR id % 10 = 0")
            rc, res = c.q("SET enable_seqscan = off; SELECT id FROM e ORDER BY v <-> '[0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5]' LIMIT 3000")
            got = set(int(x) for x in res.split('\n') if x)
            check('S1 rolled-back and deleted rows do not appear', 90001 not in got and not any(i % 10 == 0 for i in got) and len(got) == 2700, str(len(got)))

            # S3
            bad = 0
            for _ in range(5):
                q = vec(rng)
                rc1, got = ids(c, 'e', q, 20, True, extra='SET cuvs.hnsw_exact_scan = on;')
                rc2, want = ids(c, 'e', q, 20, False)
                if rc1 != 0 or got != want:
                    bad += 1
            check('S3 cuvs.hnsw_exact_scan = on on a certified index gives the exact ranking', bad == 0, f'{bad} differ')

            # S2/S4 on pgvector indexes under the pg_cuvs_hnsw access method
            def hack(table, n=1500):
                c.make_index(table, table + 'i')
                return c.data / c.scalar(f"SELECT pg_relation_filepath('{table}i')")

            def plant_mode(path, mode):
                c.stop()
                rp = subprocess.run([str(plant), str(path), mode], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                assert rp.returncode == 0, rp.stdout
                c.start()

            def compare_exact(table, label, expect_notice):
                bad = 0
                notices = 0
                for _ in range(8):
                    q = vec(rng, 3)
                    rc1, got = c.q(f"SET enable_seqscan = off; SELECT id FROM {table} ORDER BY v <-> '{q}' LIMIT 100")
                    rc2, want = c.q(f"SET enable_indexscan = off; SET enable_bitmapscan = off; SELECT id FROM {table} ORDER BY v <-> '{q}' LIMIT 100")
                    notices += 'answers from the heap' in got
                    got_ids = [x for x in got.split('\n') if x.isdigit()]
                    if rc1 != 0 or got_ids != [x for x in want.split('\n') if x.isdigit()]:
                        bad += 1
                check(f'{label}: ordered scans equal the exact ranking', bad == 0, f'{bad} differ')
                check(f'{label}: the notice {"appears" if expect_notice else "does not appear"}', (notices > 0) == expect_notice, str(notices))

            for mode, label, notice in (('lost', 'S2 LOST', True), ('corrupt', 'S2 damaged descriptor (UNTRUSTED)', True),
                                        ('certified', 'S2 CERTIFIED whose ledger cannot be read', True)):
                t = 'h_' + mode
                c.make_index(t, t + 'i')
                path = c.data / c.scalar(f"SELECT pg_relation_filepath('{t}i')")
                plant_mode(path, mode)
                compare_exact(t, label, notice)

            # S4
            c.make_index('h_legacy', 'h_legacyi')
            rc, res = c.q("SET enable_seqscan = off; SELECT count(*) FROM (SELECT id FROM h_legacy ORDER BY v <-> '[1,1,1]' LIMIT 15) x")
            check('S4 an index without a descriptor is answered as before, silently', rc == 0 and res.endswith('15') and 'answers from the heap' not in res, res)
            c.make_index('h_off', 'h_offi')
            plant_mode(c.data / c.scalar("SELECT pg_relation_filepath('h_offi')"), 'off')
            rc, res = c.q("SET enable_seqscan = off; SELECT count(*) FROM (SELECT id FROM h_off ORDER BY v <-> '[1,1,1]' LIMIT 15) x")
            check('S4 an OFF index is answered as before, silently', rc == 0 and res.endswith('15') and 'answers from the heap' not in res, res)

            # S5
            c.scalar('ALTER TABLE e ENABLE ROW LEVEL SECURITY')
            rc, res = c.q("SET enable_seqscan = off; SELECT id FROM e ORDER BY v <-> '[0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5]' LIMIT 5")
            check('S5 an index with a descriptor on a row-level security table is refused, not answered approximately',
                  rc != 0 and 'cannot run exactly' in res, res)
            c.scalar('ALTER TABLE h_legacy ENABLE ROW LEVEL SECURITY')
            rc, res = c.q("SET enable_seqscan = off; SELECT count(*) FROM (SELECT id FROM h_legacy ORDER BY v <-> '[1,1,1]' LIMIT 15) x")
            check('S5 an index without a descriptor on such a table is answered as before', rc == 0 and res.endswith('15'), res)

            # S6: a partial index with a descriptor (built on an empty table: it starts certified) is refused as well, until the decision on
            # these shapes (E1/E2) is made; one without a descriptor is not
            c.scalar('CREATE TABLE pt(id int, v vector(3))')
            c.scalar('CREATE INDEX pti ON pt USING pg_cuvs_hnsw (v vector_l2_ops) WHERE id > 5')
            c.scalar("INSERT INTO pt SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 60) g")
            rc, res = c.q("SET enable_seqscan = off; SELECT id FROM pt WHERE id > 5 ORDER BY v <-> '[3,3,0]' LIMIT 5")
            check('S6 a partial index with a descriptor is refused, with the reason', rc != 0 and 'cannot run exactly' in res and 'predicate' in res, res)
        finally:
            c.drop()
    finally:
        (out / 'transcript.txt').write_text('\n'.join(dg.TRANSCRIPT) + '\n')
        shutil.rmtree(root, ignore_errors=True)
        for d in out.glob('data-*'):
            shutil.rmtree(d, ignore_errors=True)
    result = {'failures': dg.FAILURES, 'verdict': 'FAIL' if dg.FAILURES else 'PASS',
              'not_run': 'merge of ANN answers with ledger keys for rows outside the ledger (needs a certified index with base keys: a certification or a GPU build)'}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 1 if dg.FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
