#!/usr/bin/env python3
"""How many rows that were reachable when an index was certified become unreachable because of LATER inserts, on
pgvector's own hnsw graph (CPU only; builds its own cluster). Decision A (narrowed guarantee) leaves exactly these rows
outside the promise until the next recertification, so their number is the exposure.

Reads the structural reachability with the product walk (src/cuvs_cover_reach_pg.c). For each case:
  * build an index on `main` uniform rows (plus optionally small tight clusters far away from them);
  * U0 = unreachable rows right after the build (what the ledger base would hold);
  * insert uniformly into the main cluster until +5%, +25% and +100% of the original rows (and, with small clusters,
    into the small clusters too), and after each step count
      new_old  rows that existed at the build, were reachable, and are unreachable now (the exposure),
      healed   rows of U0 that are reachable again,
      new_rows inserted rows that are unreachable (covered by their ledger keys, so not exposure).

    measure_insert_disconnect.py --output DIR --case uniform:100000x32 --case small:100000x32+50x40 ...
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_cover_reach_pg as t  # noqa: E402


def gen_sql(table, start, count, d, kind, cluster=None, clusters=None):
    if clusters is not None:
        # the table of NCL clusters (row number modulo NCL), each a point of a lattice with a uniform noise of width NOISE around it
        ncl, noise = clusters
        return (f"INSERT INTO {table}(id, grp, v) SELECT {start} + g, 0, l.a::vector FROM generate_series(1, {count}) g, "
                f"LATERAL (SELECT array_agg(((((({start} + g) % {ncl}) * 7 + k * 3) % 41)::real + (random() - 0.5) * {noise})::real) AS a FROM generate_series(1, {d}) k WHERE g > 0) l")
    if kind == 'uniform':
        return (f"INSERT INTO {table}(id, grp, v) SELECT {start} + g, 0, l.a::vector FROM generate_series(1, {count}) g, "
                f"LATERAL (SELECT array_agg(random()::real) AS a FROM generate_series(1, {d}) k WHERE g > 0) l")
    # a tight cluster far from the unit cube; `cluster` is its number
    return (f"INSERT INTO {table}(id, grp, v) SELECT {start} + g, {cluster}, l.a::vector FROM generate_series(1, {count}) g, "
            f"LATERAL (SELECT array_agg((((({cluster} * 7 + k * 3) % 41) + 5) + (random() - 0.5) * 0.04)::real) AS a FROM generate_series(1, {d}) k WHERE g > 0) l")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5497)
    ap.add_argument('--case', action='append', required=True, help='uniform:ROWSxDIM, small:ROWSxDIM+CxS or clusters:ROWSxDIM/NCL/NOISE (the table of 20000x8/50/0.05)')
    ap.add_argument('--maintenance-work-mem', default='1GB')
    ap.add_argument('--steps', default='5,25,100', help='cumulative inserted rows as percent of the original rows, comma separated')
    ap.add_argument('--m', type=int, default=16)
    ap.add_argument('--ef-construction', type=int, default=64)
    ap.add_argument('--product', nargs=2, metavar=('NEW_LIB', 'OLD_LIB'),
                    help='run on a pg_cuvs server (the libraries built for it) with the index created USING pg_cuvs_hnsw on the EMPTY table before the '
                         'rows are inserted: the graph is made by pgvector inserts into the metapage with m = 32, the product path that needs no GPU; '
                         '--m is then ignored (the metapage says 32) and --ef-construction is the reloption the inserts read')
    ap.add_argument('--switch', action='store_true',
                    help='with --product: the index is built by pgvector over the rows (m = --m, as without --product) and then switched to pg_cuvs_hnsw and certified, '
                         'the pages of a row-built index: the setup of an independent review (the graph of the base rows is the bulk build of pgvector, the inserts go through pg_cuvs)')
    ap.add_argument('--writers', type=int, default=1, help='sessions that make the inserts of a step together (the rows of a step are divided between them)')
    ap.add_argument('--seed', type=float, default=0.3, help='setseed() for the generated rows and the inserts, between -1 and 1')
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    include = subprocess.check_output(['pg_config', '--includedir-server'], text=True).strip()
    cflags = shlex.split(subprocess.check_output(['pg_config', '--cflags'], text=True))
    lib = out / 'cover_reach_probe.so'
    r = subprocess.run([os.environ.get('CC', 'cc'), *cflags, '-shared', '-fPIC', '-Wno-unused-parameter', '-Wno-declaration-after-statement', '-I' + include,
                        '-I' + str(repo / 'src'), str(repo / 'test/unit/cover_reach_pg_probe.c'), str(repo / 'src/cuvs_cover_reach_pg.c'), '-o', str(lib)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    assert r.returncode == 0, r.stdout
    if args.product:
        import test_downgrade_pg as dg
        o = dg.Overlay(out / 'pgroot', Path(args.product[0]).resolve(), Path(args.product[1]).resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        cl = dg.Cluster(o, 'disc', out, args.port)
        cl.extra_opts = '-c autovacuum=off -c max_parallel_maintenance_workers=0 -c shared_buffers=1GB -c max_wal_size=4GB'
        o.install('new')

        class Product:
            data = cl.data

            def start(self):
                cl.start()

            def stop(self, mode='fast'):
                cl.stop(mode)

            def q(self, sql):
                return cl.scalar(sql)

            scalar = q
        srv = Product()
        import test_cover_vacuum_pg as tv
        plant_bin = out / 'cover_plant'
        r = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(repo / 'src'), str(repo / 'test/unit/cover_plant.c'), '-o', str(plant_bin)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        assert r.returncode == 0, r.stdout
    else:
        srv = t.Server(out, args.port)
    results = []

    def gen(*a, **k):
        # the seed and the statement in one session (a separate setseed() call does not reach the session of the next statement)
        return f'SELECT setseed({args.seed}); ' + gen_sql(*a, **k)
    try:
        srv.start()
        srv.q('CREATE EXTENSION vector')
        if args.product:
            srv.q('CREATE EXTENSION pg_cuvs')
        srv.q(f"ALTER SYSTEM SET maintenance_work_mem = '{args.maintenance_work_mem}'")
        srv.q('ALTER SYSTEM SET max_parallel_maintenance_workers = 0')	# a parallel build needs shared memory of the size of the graph
        srv.q('SELECT pg_reload_conf()')
        srv.q(f"CREATE FUNCTION unreached(regclass, bigint, bigint) RETURNS SETOF bigint AS '{lib}', 'cover_reach_unreached' LANGUAGE C")
        srv.q(f"CREATE FUNCTION probe(regclass, bigint, bigint) RETURNS TABLE(status int, nblocks bigint, elements bigint, live bigint, reached bigint, unreached bigint, emitted bigint, mem bigint, spilled bigint, needed bigint) AS '{lib}', 'cover_reach_probe' LANGUAGE C")
        for case in args.case:
            kind, spec = case.split(':', 1)
            small_c = small_s = 0
            clusters = None
            if kind == 'clusters':
                spec, ncl, noise = spec.split('/')
                clusters = (int(ncl), float(noise))
            if kind == 'small':
                spec, small = spec.split('+')
                small_c, small_s = (int(x) for x in small.split('x'))
            n0, d = (int(x) for x in spec.split('x'))
            tbl = f'c{len(results)}'
            idx = tbl + '_i'
            srv.q(f'CREATE TABLE {tbl}(id int, grp int, v vector({d}))')
            t0 = time.time()
            if args.product and not args.switch:
                srv.q(f'CREATE INDEX {idx} ON {tbl} USING pg_cuvs_hnsw (v vector_l2_ops) WITH (ef_construction = {args.ef_construction})')
            srv.q(gen(tbl, 0, n0, d, 'uniform', clusters=clusters))
            for c in range(1, small_c + 1):
                srv.q(gen_sql(tbl, n0 + (c - 1) * small_s, small_s, d, 'cluster', c))
            old_max = n0 + small_c * small_s
            if not args.product:
                srv.q(f'CREATE INDEX {idx} ON {tbl} USING hnsw (v vector_l2_ops) WITH (m = {args.m}, ef_construction = {args.ef_construction})')
            elif args.switch:
                tv.hack_index(srv, tbl, idx, f'WITH (m = {args.m}, ef_construction = {args.ef_construction})')
                tv.plant(srv, plant_bin, idx, 'uncertified')
                srv.q(f"SELECT pg_cuvs_cover_recertify('{idx}')")
            build_s = time.time() - t0

            def snapshot(name):
                srv.q(f"DROP TABLE IF EXISTS {name}")
                srv.q(f"CREATE TABLE {name} AS SELECT format('(%s,%s)', v >> 16, v & 65535)::tid AS t FROM unreached('{idx}', 1::bigint << 34, 0) v")

            snapshot(f'{tbl}_u0')
            probe0 = [int(x) for x in srv.q(f"SELECT * FROM probe('{idx}', 1::bigint << 34, 0)").split('|')]
            case_out = {'case': case, 'rows_main': n0, 'dim': d, 'small_clusters': small_c, 'small_size': small_s, 'build_seconds': round(build_s, 1),
                        'u0_total': probe0[5], 'u0_small': int(srv.q(f"SELECT count(*) FROM {tbl} x WHERE x.grp > 0 AND x.ctid IN (SELECT t FROM {tbl}_u0)")) if small_c else 0,
                        'steps': []}
            inserted_main = 0
            inserted_small = 0
            next_id = old_max
            steps = [int(x) for x in args.steps.split(',')]
            for pct in steps:
                target = n0 * pct // 100
                add = target - inserted_main
                if args.writers > 1 and add >= args.writers:
                    each = add // args.writers
                    parts = [(next_id + k * each, each if k + 1 < args.writers else add - each * k) for k in range(args.writers)]
                    ths = [threading.Thread(target=lambda a=a: srv.q(gen(tbl, a[0], a[1], d, 'uniform', clusters=clusters))) for a in parts]
                    for th in ths:
                        th.start()
                    for th in ths:
                        th.join()
                else:
                    srv.q(gen(tbl, next_id, add, d, 'uniform', clusters=clusters))
                next_id += add
                inserted_main = target
                if small_c:
                    # the same proportion into every small cluster
                    target_c = max(1, small_s * pct // 100)
                    for c in range(1, small_c + 1):
                        srv.q(gen(tbl, next_id, target_c - inserted_small, d, 'cluster', c))
                        next_id += target_c - inserted_small
                    inserted_small = target_c
                snapshot(f'{tbl}_u{pct}')
                row = [int(x) for x in srv.q(f"SELECT * FROM probe('{idx}', 1::bigint << 34, 0)").split('|')]
                new_old = int(srv.q(f"SELECT count(*) FROM {tbl} x WHERE x.id <= {old_max} AND x.ctid IN (SELECT t FROM {tbl}_u{pct}) AND x.ctid NOT IN (SELECT t FROM {tbl}_u0)"))
                healed = int(srv.q(f"SELECT count(*) FROM {tbl} x WHERE x.id <= {old_max} AND x.ctid IN (SELECT t FROM {tbl}_u0) AND x.ctid NOT IN (SELECT t FROM {tbl}_u{pct})"))
                new_rows = int(srv.q(f"SELECT count(*) FROM {tbl} x WHERE x.id > {old_max} AND x.ctid IN (SELECT t FROM {tbl}_u{pct})"))
                small_now = int(srv.q(f"SELECT count(*) FROM {tbl} x WHERE x.grp > 0 AND x.id <= {old_max} AND x.ctid IN (SELECT t FROM {tbl}_u{pct})")) if small_c else 0
                step = {'inserted_pct': pct, 'inserted_rows': inserted_main, 'unreached_total': row[5], 'new_old_unreachable': new_old,
                        'new_old_pct_of_old_rows': round(100.0 * new_old / old_max, 4), 'healed': healed, 'new_rows_unreachable': new_rows,
                        'small_cluster_rows_unreachable': small_now}
                case_out['steps'].append(step)
                print(json.dumps({'case': case, 'am': 'pg_cuvs_hnsw' if args.product else 'hnsw', 'm': args.m if (args.switch or not args.product) else 32, 'switched': args.switch, 'ef_construction': args.ef_construction, 'seed': args.seed, **step}), flush=True)
            results.append(case_out)
            srv.q(f'DROP TABLE {tbl} CASCADE')
            for pct in [0] + steps:
                srv.q(f'DROP TABLE IF EXISTS {tbl}_u{pct}')
    finally:
        srv.stop('immediate')
        shutil.rmtree(srv.data, ignore_errors=True)
    (out / 'result.json').write_text(json.dumps(results, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
