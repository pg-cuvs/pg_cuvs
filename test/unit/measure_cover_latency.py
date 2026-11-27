#!/usr/bin/env python3
"""End-to-end read latency of a cover index against the ANN alone, the r4 criterion R1 of the design (10.7): same index, same 200
queries, 7 repeats; p50, p95, p99 inside the server (a loop in plpgsql with clock_timestamp around each query, so no client or
process-start time); the "ANN alone" side is the very same index file scanned through the older handler symbol (no wrapper), each
repeat on fresh connections. Cells: ledger of 0.03%, 0.3% and 3% of the rows (the 3% cell is reported without a criterion).

  r4: p95 ratio <= 2.0 at 0.03% and 0.3%; added p95 <= 20 ms warm.
Cold-cache cells are not run (they need to drop the OS page cache, which needs root), nor the 1M and 10M rows grids.
Prints JSON.
"""
import argparse
import json
from pathlib import Path
import shutil
import statistics
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402

OLD = dg.OLD_SYMBOL
NEW = dg.NEW_SYMBOL


def pct(xs, p):
    xs = sorted(xs)
    k = (len(xs) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True)
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5761)
    ap.add_argument('--rows', type=int, default=100000)
    ap.add_argument('--dim', type=int, default=96)
    ap.add_argument('--repeats', type=int, default=7)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    res = {'rows': args.rows, 'dim': args.dim, 'cells': []}
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'lat', out, args.port)
        c.extra_opts = '-c autovacuum=off -c max_parallel_maintenance_workers=0 -c shared_buffers=1GB -c max_wal_size=2GB'
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')
            d, n = args.dim, args.rows
            c.scalar(f'CREATE TABLE cov(id int, v vector({d}))')
            c.scalar('CREATE INDEX cov_i ON cov USING pg_cuvs_hnsw (v vector_l2_ops)')
            c.scalar(f"SELECT setseed(0.61); INSERT INTO cov SELECT g, (SELECT array_agg(random()::real) FROM generate_series(1, {d}) x WHERE g > 0)::vector FROM generate_series(1, {n}) g")
            rc, r = c.q("SELECT pg_cuvs_cover_recertify('cov_i')::text")
            assert rc == 0, r
            c.scalar(f'CREATE TABLE qv AS SELECT row_number() OVER () AS n, v FROM (SELECT v FROM cov WHERE id % {max(1, n // 200)} = 0 LIMIT 200) s')
            c.scalar('CREATE TABLE lat(mode text, cell text, rep int, ms float8)')
            nid = n

            def run_mode(mode, cell, rep):
                sym = OLD if mode == 'ann_alone' else NEW
                c.scalar(f"CREATE OR REPLACE FUNCTION pg_cuvs_hnsw_handler(internal) RETURNS index_am_handler AS '$libdir/pg_cuvs', '{sym}' LANGUAGE C")
                sql = f"""DO $$ DECLARE q vector; t0 timestamptz; BEGIN
                    SET LOCAL enable_seqscan = off;
                    FOR i IN 1..200 LOOP
                      SELECT v INTO q FROM qv WHERE n = i;
                      t0 := clock_timestamp();
                      PERFORM id FROM cov ORDER BY v <-> q LIMIT 10;
                      INSERT INTO lat VALUES ('{mode}', '{cell}', {rep}, extract(epoch FROM clock_timestamp() - t0) * 1000);
                    END LOOP; END $$"""
                rc, r = c.q(sql)
                assert rc == 0, r

            for pctg, label in ((0.03, '0.03%'), (0.3, '0.3%'), (3.0, '3%')):
                want = int(n * pctg / 100)
                st = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('cov_i')::text"))
                have = st.get('ledger_keys', 0)
                if want > have:
                    add = want - have
                    c.scalar(f"INSERT INTO cov SELECT g, (SELECT array_agg(random()::real) FROM generate_series(1, {d}) x WHERE g > 0)::vector FROM generate_series({nid + 1}, {nid + add}) g")
                    nid += add
                st = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('cov_i')::text"))
                c.scalar('CHECKPOINT')
                for rep in range(args.repeats):
                    for mode in (('ann_alone', 'merged') if rep % 2 == 0 else ('merged', 'ann_alone')):
                        run_mode(mode, label, rep)
                cell = {'cell': label, 'ledger_keys': st.get('ledger_keys'), 'rows': nid}
                for mode in ('ann_alone', 'merged'):
                    per = []
                    for rep in range(args.repeats):
                        xs = [float(x) for x in c.scalar(f"SELECT ms FROM lat WHERE mode = '{mode}' AND cell = '{label}' AND rep = {rep}").split()]
                        per.append((pct(xs, 50), pct(xs, 95), pct(xs, 99)))
                    cell[mode] = {'p50_ms': round(statistics.median(p[0] for p in per), 3), 'p95_ms': round(statistics.median(p[1] for p in per), 3),
                                  'p99_ms': round(statistics.median(p[2] for p in per), 3), 'p95_each_repeat': [round(p[1], 2) for p in per]}
                cell['p95_ratio'] = round(cell['merged']['p95_ms'] / cell['ann_alone']['p95_ms'], 2)
                cell['p95_added_ms'] = round(cell['merged']['p95_ms'] - cell['ann_alone']['p95_ms'], 3)
                res['cells'].append(cell)
                print(json.dumps(cell), flush=True)
        finally:
            c.drop()
    finally:
        shutil.rmtree(root, ignore_errors=True)
        for dd in out.glob('data-*'):
            shutil.rmtree(dd, ignore_errors=True)
    (out / 'result.json').write_text(json.dumps(res, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
