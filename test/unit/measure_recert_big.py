#!/usr/bin/env python3
"""How long a recertification takes on a big index, and how long it holds the cover lock exclusively (CPU, one private PostgreSQL 16 cluster). Prints JSON.

A table of --rows x 96 random vectors gets a pgvector hnsw index (m = 32, ef_construction = 64, the m of a graph converted from a CAGRA graph of degree 64),
which is switched to pg_cuvs_hnsw in the state of a row-built index and planted as uncertified. Then:
  B1 pg_cuvs_cover_recertify on the idle index: the time of the whole call, of the walk, and the exclusive cover lock of phase 1 and phase 3 (from the function's own answer);
  B2 the same while --writers sessions insert in a loop: the phases again, and the longest and the median time of one insert statement during the call;
  B3 after 5 percent of the rows were inserted (the ledger holds that many keys, which phase 1 reads under the lock): the idle recertification again.
The directory of the data is under --output; it is removed at the end. The run stops if the free space of that volume falls below --min-free-gib.
"""
import argparse
import json
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402
import test_cover_vacuum_pg as tv  # noqa: E402

# the subquery must depend on the outer row (WHERE g > 0), or PostgreSQL evaluates it once and every row holds the same vector
RAND = "(SELECT array_agg(random()::real) FROM generate_series(1, 96) d WHERE g > 0)::vector"


def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)


def free_gib(path):
    return shutil.disk_usage(path).free / 2**30


def jres(rc, res):
    lines = [ln for ln in res.splitlines() if ln.startswith('{')]
    return json.loads(lines[-1]) if rc == 0 and lines else {'raw': res[-300:]}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True)
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5961)
    ap.add_argument('--rows', type=int, default=1000000)
    ap.add_argument('--maintenance-work-mem', default='1GB')
    ap.add_argument('--writers', type=int, default=2)
    ap.add_argument('--min-free-gib', type=float, default=18.0)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    plant_bin = out / 'cover_plant'
    r = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(repo / 'src'), str(repo / 'test/unit/cover_plant.c'), '-o', str(plant_bin)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    assert r.returncode == 0, r.stdout
    res = {'rows': args.rows, 'min_free_gib_before': round(free_gib(out), 1)}
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'big', out, args.port)
        c.extra_opts = (f'-c autovacuum=off -c max_parallel_maintenance_workers=0 -c maintenance_work_mem={args.maintenance_work_mem} '
                        '-c shared_buffers=512MB -c max_wal_size=2GB -c checkpoint_timeout=30min -c fsync=off')
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')
            c.scalar('CREATE TABLE big(id int, v vector(96))')
            t0 = time.time()
            step = 100000
            for lo in range(0, args.rows, step):
                assert free_gib(out) > args.min_free_gib, 'free space fell below the floor while loading'
                c.scalar(f"INSERT INTO big SELECT {lo} + g, {RAND} FROM generate_series(1, {min(step, args.rows - lo)}) g")
            res['load_seconds'] = round(time.time() - t0, 1)
            log('loaded', res['load_seconds'])
            t0 = time.time()
            tv.hack_index(c, 'big', 'bigi', 'WITH (m = 32, ef_construction = 64)')
            res['index_build_seconds'] = round(time.time() - t0, 1)
            res['index_mib'] = round(int(c.scalar("SELECT pg_relation_size('bigi')")) / 2**20)
            res['table_mib'] = round(int(c.scalar("SELECT pg_relation_size('big')")) / 2**20)
            res['free_gib_after_build'] = round(free_gib(out), 1)
            log('built', res)
            assert free_gib(out) > args.min_free_gib, 'free space fell below the floor after the build'
            tv.plant(c, plant_bin, 'bigi', 'uncertified')
            c.scalar('CHECKPOINT')

            t0 = time.time()
            rc, ans = c.q("SELECT pg_cuvs_cover_recertify('bigi')::text")
            res['B1_call_seconds'] = round(time.time() - t0, 2)
            res['B1'] = jres(rc, ans)
            log('B1', res['B1_call_seconds'], res['B1'])

            stop = threading.Event()
            lat = []

            def writer(k):
                n = 0
                while not stop.is_set():
                    t = time.perf_counter()
                    c.q(f"INSERT INTO big SELECT {10**8 + k * 10**6 + n} + g, {RAND} FROM generate_series(1, 20) g")
                    lat.append((time.perf_counter() - t) * 1000)
                    n += 20

            ths = [threading.Thread(target=writer, args=(k,)) for k in range(args.writers)]
            for th in ths:
                th.start()
            time.sleep(3)
            before = len(lat)
            t0 = time.time()
            rc, ans = c.q("SELECT pg_cuvs_cover_recertify('bigi')::text")
            call = time.time() - t0
            during = lat[before:]
            stop.set()
            for th in ths:
                th.join()
            res['B2_call_seconds'] = round(call, 2)
            res['B2'] = jres(rc, ans)
            res['B2_insert_statements_during'] = len(during)
            if during:
                res['B2_insert_ms_median'] = round(statistics.median(during), 1)
                res['B2_insert_ms_max'] = round(max(during), 1)
            res['B2_insert_ms_median_before'] = round(statistics.median(lat[:before]), 1) if before else None
            log('B2', res['B2_call_seconds'], res['B2'])
            # B3: 5 percent of the rows are inserted after that certification (so the ledger holds that many keys, and phase 1 reads all of them under the
            # lock), then the idle recertification again
            extra = args.rows // 20
            for lo in range(0, extra, 50000):
                c.scalar(f"INSERT INTO big SELECT {2 * 10**8 + lo} + g, {RAND} FROM generate_series(1, {min(50000, extra - lo)}) g")
            res['B3_keys_before'] = tv.cover_status(c, 'bigi').get('keys_since_certification')
            t0 = time.time()
            rc, ans = c.q("SELECT pg_cuvs_cover_recertify('bigi')::text")
            res['B3_call_seconds'] = round(time.time() - t0, 2)
            res['B3'] = jres(rc, ans)
            log('B3', res['B3_call_seconds'], res['B3'])
            res['free_gib_end'] = round(free_gib(out), 1)
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
