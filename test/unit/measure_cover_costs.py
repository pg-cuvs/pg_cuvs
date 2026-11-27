#!/usr/bin/env python3
"""Measurements for the cover work that are not pass/fail checks (CPU, one private PostgreSQL 16 cluster). Prints JSON.

  R1 what a scan costs when the ledger is long (review a00c214a N-9): an index built on an empty table and filled by inserts holds a key
     for every row until it is recertified, and a CERTIFIED scan ranks every key exactly. Latency of ORDER BY v <-> q LIMIT 10
     (median of 30 queries, warm) against the number of keys, on a table of 100,000 x 96, next to the same table with a plain
     pgvector hnsw index and to the heap-ranked answer of a LOST index;
  R2 the recertification itself: time of pg_cuvs_cover_recertify on that index;
  S1 the reachability walk with RAM and with spill (a 64 kB RAM limit sends every array to a mapped temp file) on a pgvector index
     of --spill-rows rows x 32: the same counts, the time of each.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import statistics
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402


def timed(c, sql, n=30):
    ts = []
    for _ in range(n):
        t = time.perf_counter()
        rc, out = c.q(sql)
        assert rc == 0, out
        ts.append((time.perf_counter() - t) * 1000)
    return round(statistics.median(ts), 1), round(max(ts), 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True)
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5681)
    ap.add_argument('--rows', type=int, default=100000)
    ap.add_argument('--spill-rows', type=int, default=1000000)
    ap.add_argument('--skip-spill', action='store_true')
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    res = {}
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'cost', out, args.port)
        c.extra_opts = '-c autovacuum=off -c max_parallel_maintenance_workers=0 -c maintenance_work_mem=256MB -c shared_buffers=512MB -c max_wal_size=2GB'
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')
            dim = 96
            n = args.rows
            # the same rows in a plain pgvector table
            c.scalar(f'CREATE TABLE base(id int, v vector({dim}))')
            c.scalar(f'CREATE TABLE cov(id int, v vector({dim}))')
            c.scalar('CREATE INDEX cov_i ON cov USING pg_cuvs_hnsw (v vector_l2_ops)')
            t0 = time.time()
            c.scalar(f"SELECT setseed(0.5); INSERT INTO base SELECT g, (SELECT array_agg(random()::real) FROM generate_series(1, {dim}) d WHERE g > 0)::vector FROM generate_series(1, {n}) g")
            c.scalar('INSERT INTO cov SELECT * FROM base')
            res['load_seconds'] = round(time.time() - t0, 1)
            t0 = time.time()
            c.scalar('CREATE INDEX base_i ON base USING hnsw (v vector_l2_ops)')
            res['pgvector_build_seconds'] = round(time.time() - t0, 1)
            # the query vector as a literal: a subselect for it would scan the table once per query and swamp what is measured
            qv = c.scalar("SELECT v::text FROM base WHERE id = 4242")
            sql_cov = f"SET enable_seqscan = off; SELECT id FROM cov ORDER BY v <-> '{qv}' LIMIT 10"
            sql_base = f"SET enable_seqscan = off; SELECT id FROM base ORDER BY v <-> '{qv}' LIMIT 10"
            st = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('cov_i')::text"))
            res['r1'] = []
            res['r1'].append({'what': 'index filled by inserts, never recertified', 'keys': st.get('ledger_keys'), 'guarantee_in_force': st.get('guarantee_in_force'),
                              'ms_median_max': timed(c, sql_cov)})
            res['pgvector_hnsw_ms_median_max'] = timed(c, sql_base)
            t0 = time.time()
            rc, out_r = c.q("SELECT pg_cuvs_cover_recertify('cov_i')::text")
            res['r2'] = {'seconds': round(time.time() - t0, 2), 'result': json.loads(out_r)}
            st = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('cov_i')::text"))
            res['r1'].append({'what': 'after the recertification', 'keys': st.get('ledger_keys'), 'ms_median_max': timed(c, sql_cov)})
            nid = n
            for add in (900, 9000, 90000):
                c.scalar(f"INSERT INTO cov SELECT g, (SELECT array_agg(random()::real) FROM generate_series(1, {dim}) d WHERE g > 0)::vector FROM generate_series({nid + 1}, {nid + add}) g")
                nid += add
                st = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('cov_i')::text"))
                res['r1'].append({'what': f'after {add} more inserts', 'keys': st.get('ledger_keys'), 'recommended': st.get('recertify_recommended'),
                                  'ms_median_max': timed(c, sql_cov)})
            # the heap-ranked answer of an index that is lost
            c.scalar('DELETE FROM cov WHERE id % 1000 = 0')
            c.scalar('VACUUM cov')
            st = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('cov_i')::text"))
            res['r1'].append({'what': 'status after a vacuum (recertified by it)', 'class': st.get('class'), 'keys': st.get('ledger_keys')})
            # a LOST index: take it off with the downgrade function and time the scan that ranks the heap
            c.scalar('SELECT pg_cuvs_cover_invalidate_all()')
            res['lost_heap_ms_median_max'] = timed(c, sql_cov, 10)
            # S1: the walk with RAM and with spill
            if not args.skip_spill:
                include = subprocess.check_output(['pg_config', '--includedir-server'], text=True).strip()
                cflags = shlex.split(subprocess.check_output(['pg_config', '--cflags'], text=True))
                lib = out / 'cover_reach_probe.so'
                r = subprocess.run([os.environ.get('CC', 'cc'), *cflags, '-shared', '-fPIC', '-Wno-unused-parameter', '-Wno-declaration-after-statement',
                                    '-I' + include, '-I' + str(repo / 'src'), str(repo / 'test/unit/cover_reach_pg_probe.c'),
                                    str(repo / 'src/cuvs_cover_reach_pg.c'), '-o', str(lib)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                assert r.returncode == 0, r.stdout
                c.scalar(f"CREATE FUNCTION probe(regclass, bigint, bigint) RETURNS TABLE(status int, nblocks bigint, elements bigint, live bigint, reached bigint, unreached bigint, emitted bigint, mem bigint, spilled bigint, needed bigint) AS '{lib}', 'cover_reach_probe' LANGUAGE C")
                m = args.spill_rows
                c.scalar(f'CREATE TABLE big(id int, v vector(32))')
                t0 = time.time()
                c.scalar(f"INSERT INTO big SELECT g, (SELECT array_agg(random()::real) FROM generate_series(1, 32) d WHERE g > 0)::vector FROM generate_series(1, {m}) g")
                c.scalar('CREATE INDEX big_i ON big USING hnsw (v vector_l2_ops)')
                res['s1_build_seconds'] = round(time.time() - t0, 1)
                c.scalar('CHECKPOINT')
                s1 = {}
                for label, ram, spill in (('ram', 1 << 30, 0), ('spill', 65536, 1 << 34), ('ram_again', 1 << 30, 0), ('spill_again', 65536, 1 << 34)):
                    t0 = time.perf_counter()
                    row = [int(x) for x in c.scalar(f"SELECT * FROM probe('big_i', {ram}, {spill})").split('|')]
                    s1[label] = {'seconds': round(time.perf_counter() - t0, 2), 'status': row[0], 'nblocks': row[1], 'elements': row[2], 'live': row[3],
                                 'reached': row[4], 'unreached': row[5], 'ram_bytes': row[7], 'spilled_bytes': row[8]}
                res['s1'] = s1
        finally:
            c.drop()
    finally:
        shutil.rmtree(root, ignore_errors=True)
        for d in out.glob('data-*'):
            shutil.rmtree(d, ignore_errors=True)
    (out / 'result.json').write_text(json.dumps(res, indent=2) + '\n')
    print(json.dumps(res, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
