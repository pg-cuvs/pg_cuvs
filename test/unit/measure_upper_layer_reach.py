#!/usr/bin/env python3
"""Does the reachability definition of the walk (an element is reached when a path of neighbour slots of ANY layer leads to it from the entry point)
differ from what the search of pgvector can do, and how many rows does the difference touch? (CPU, one private PostgreSQL 16 cluster; prints JSON.)

The walk (src/cuvs_cover_reach.h) follows every slot of an element's neighbour tuple, the upper layers' and layer 0's. pgvector's search goes down
from the entry point one layer at a time (one candidate per upper layer, the nearest it finds) and searches layer 0 from the element it ends on. So an
element that only an upper layer's slot leads to is a start of the layer-0 search for the queries that end the descent on it, and is not reachable by
layer 0's slots alone from the entry point. This tool counts those elements on graphs of a few shapes, with an independent reader of the relation
file, and checks with the ordered scan of the row's own vector (ef_search 40) whether such a row is found.

  all_slots  elements reached over the slots of every layer (what the walk calls reached)
  layer0     elements reached from the entry element over layer 0's slots only
  upper_only all_slots minus layer0, and how many of those an ordered scan by their own vector finds
"""
import argparse
import json
from pathlib import Path
import shutil
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402
import test_cover_insert_pg as ti  # noqa: E402


def parse(pages):
    meta = pages[0]
    m = struct.unpack_from('<H', meta, 36)[0]
    entry = (struct.unpack_from('<I', meta, 40)[0], struct.unpack_from('<H', meta, 44)[0])
    elems, neigh = {}, {}
    for blk in range(1, len(pages)):
        p = pages[blk]
        lower = struct.unpack_from('<H', p, 12)[0]
        for off in range(1, (lower - 24) // 4 + 1):
            lp, = struct.unpack_from('<I', p, 24 + 4 * (off - 1))
            at, flags, ln = lp & 0x7FFF, (lp >> 15) & 3, lp >> 17
            if flags != 1 or ln < 8:
                continue
            typ = p[at]
            if typ == 1 and ln >= 72:
                level = p[at + 1]
                deleted = p[at + 2]
                nb = (struct.unpack_from('<H', p, at + 64)[0] << 16 | struct.unpack_from('<H', p, at + 66)[0], struct.unpack_from('<H', p, at + 68)[0])
                heap = (struct.unpack_from('<H', p, at + 4)[0] << 16 | struct.unpack_from('<H', p, at + 6)[0], struct.unpack_from('<H', p, at + 8)[0])
                elems[(blk, off)] = (level, deleted, nb, heap)
            elif typ == 2:
                count = struct.unpack_from('<H', p, at + 2)[0]
                slots = []
                for k in range(count):
                    b = struct.unpack_from('<H', p, at + 4 + 6 * k)[0] << 16 | struct.unpack_from('<H', p, at + 6 + 6 * k)[0]
                    o = struct.unpack_from('<H', p, at + 8 + 6 * k)[0]
                    slots.append((b, o))
                neigh[(blk, off)] = slots
    return m, entry, elems, neigh


def reach(m, entry, elems, neigh, layer0_only):
    if entry not in elems:
        return set()
    seen, todo = {entry}, [entry]
    while todo:
        e = todo.pop()
        level, deleted, nb, heap = elems[e]
        slots = neigh.get(nb, [])
        if layer0_only:
            slots = slots[level * m:level * m + 2 * m]
        for s in slots:
            if s in elems and s not in seen and elems[s][1] == 0 and s != (0xFFFFFFFF, 0):
                seen.add(s)
                todo.append(s)
    return seen


def one(c, name, n, dim, m, efc, cluster):
    c.scalar(f'CREATE TABLE {name}(id int, v vector({dim}))')
    if cluster:
        gen = f"(SELECT array_agg((((x % 40) * 7 + d * 3) % 41 + (random() - 0.5) * 2.0)::real) FROM generate_series(1, {dim}) d)::vector"
    else:
        gen = f"(SELECT array_agg(random()::real) FROM generate_series(1, {dim}) d WHERE x > 0)::vector"
    c.scalar(f"INSERT INTO {name} SELECT x, {gen} FROM generate_series(1, {n}) x")
    c.scalar(f'CREATE INDEX {name}_i ON {name} USING hnsw (v vector_l2_ops) WITH (m = {m}, ef_construction = {efc})')
    c.scalar('CHECKPOINT')
    path = c.data / c.scalar(f"SELECT pg_relation_filepath('{name}_i')")
    mm, entry, elems, neigh = parse(ti.read_pages(path))
    live = {e for e, v in elems.items() if v[1] == 0}
    a = reach(mm, entry, elems, neigh, False)
    b = reach(mm, entry, elems, neigh, True)
    upper_only = sorted(a - b)
    found = 0
    sample = upper_only[:200]
    for e in sample:
        h = elems[e][3]
        rid = c.scalar(f"SELECT id FROM {name} WHERE ctid = '({h[0]},{h[1]})'")
        got = c.scalar(f"SET enable_seqscan = off; SET hnsw.ef_search = 40; SELECT id FROM {name} ORDER BY v <-> (SELECT v FROM {name} WHERE id = {rid}) LIMIT 1")
        found += 1 if got.strip().splitlines()[-1] == rid else 0
    return {'rows': n, 'dim': dim, 'm': mm, 'ef_construction': efc, 'clustered': cluster, 'live': len(live), 'all_slots': len(a), 'layer0': len(b),
            'unreached_by_all_slots': len(live - a), 'upper_only': len(upper_only), 'upper_only_checked': len(sample), 'upper_only_found_by_own_vector': found}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True)
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5971)
    ap.add_argument('--rows', type=int, default=20000)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    res = []
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'up', out, args.port)
        c.extra_opts = '-c autovacuum=off -c max_parallel_maintenance_workers=0 -c maintenance_work_mem=256MB'
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            for i, (dim, m, efc, cl) in enumerate([(16, 16, 64, False), (16, 8, 32, False), (96, 32, 64, False), (16, 4, 16, False), (16, 16, 64, True), (16, 4, 16, True)]):
                res.append(one(c, f't{i}', args.rows, dim, m, efc, cl))
                print(json.dumps(res[-1]), flush=True)
        finally:
            c.drop()
    finally:
        shutil.rmtree(root, ignore_errors=True)
        for d in out.glob('data-*'):
            shutil.rmtree(d, ignore_errors=True)
    (out / 'result.json').write_text(json.dumps(res, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
