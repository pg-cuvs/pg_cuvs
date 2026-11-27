#!/usr/bin/env python3
"""Measure the memory the structural reachability walk accounts per row on real pgvector hnsw indexes, and from it the index
size above which the default maintenance_work_mem (64 MB) is too small. CPU only; builds its own cluster.

    measure_cover_reach_memory.py --output DIR [--shape [clustered:]ROWSxDIM ...]

A shape is uniform random vectors, or `clustered:` for 40 tight clusters far apart (the shape where HNSW graphs break).
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_cover_reach_pg as t  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5491)
    ap.add_argument('--shape', action='append', help='default: 100000x96 and 30000x768')
    ap.add_argument('--mem', type=int, default=1 << 34, help='RAM limit handed to the walk (bytes)')
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    repo = Path(__file__).resolve().parents[2]
    import os, shlex
    include = subprocess.check_output(['pg_config', '--includedir-server'], text=True).strip()
    cflags = shlex.split(subprocess.check_output(['pg_config', '--cflags'], text=True))
    lib = out / 'cover_reach_probe.so'
    r = subprocess.run([os.environ.get('CC', 'cc'), *cflags, '-shared', '-fPIC', '-Wno-unused-parameter', '-Wno-declaration-after-statement', '-I' + include,
                        '-I' + str(repo / 'src'), str(repo / 'test/unit/cover_reach_pg_probe.c'), str(repo / 'src/cuvs_cover_reach_pg.c'), '-o', str(lib)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    assert r.returncode == 0, r.stdout
    srv = t.Server(out, args.port)
    rows_out = []
    try:
        srv.start()
        srv.q('CREATE EXTENSION vector')
        srv.q(f"CREATE FUNCTION probe(regclass, bigint, bigint) RETURNS TABLE(status int, nblocks bigint, elements bigint, live bigint, reached bigint, unreached bigint, emitted bigint, mem bigint, spilled bigint, needed bigint) AS '{lib}', 'cover_reach_probe' LANGUAGE C")
        srv.q("ALTER SYSTEM SET maintenance_work_mem = '1GB'")
        for shape in args.shape or ['100000x96', '30000x768']:
            kind, spec = (shape.split(':', 1) + [None])[:2] if ':' in shape else ('uniform', shape)
            n, d = (int(x) for x in spec.split('x'))
            name = f's{kind[:3]}{n}_{d}'
            srv.q(f'CREATE TABLE {name}(id int, v vector({d}))')
            if kind == 'uniform':
                # LATERAL with a reference to g, so that the vector is drawn per row (an uncorrelated subquery gives every row the same vector)
                srv.q(f'INSERT INTO {name} SELECT g, l.a::vector FROM generate_series(1, {n}) g, LATERAL (SELECT array_agg(random()::real) AS a FROM generate_series(1, {d}) k WHERE g > 0) l')
            else:
                srv.q(f"INSERT INTO {name} SELECT g, l.a::vector FROM generate_series(1, {n}) g, LATERAL (SELECT array_agg((((g % 40) * 7 + k * 3) % 41 + (random() - 0.5) * 0.05)::real) AS a FROM generate_series(1, {d}) k WHERE g > 0) l")
            srv.q(f'CREATE INDEX {name}_i ON {name} USING hnsw (v vector_l2_ops)')
            status, nblocks, elements, live, reached, unreached, emitted, mem, spilled, needed = [int(x) for x in srv.q(f"SELECT * FROM probe('{name}_i', {args.mem}::bigint, 0)").split('|')]
            per_row = mem / elements
            rows_out.append({'kind': kind, 'rows': n, 'dim': d, 'status': status, 'nblocks': nblocks, 'elements': elements, 'reached': reached, 'unreached': unreached,
                             'mem_bytes': mem, 'bytes_per_row': round(per_row, 2), 'rows_per_block': round(elements / (nblocks - 1), 2),
                             'rows_that_fit_in_64MB': int(64 * 1024 * 1024 / per_row)})
            print(json.dumps(rows_out[-1]), flush=True)
    finally:
        srv.stop('immediate')
        shutil.rmtree(srv.data, ignore_errors=True)
    (out / 'result.json').write_text(json.dumps(rows_out, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
