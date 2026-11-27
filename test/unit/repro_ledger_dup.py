#!/usr/bin/env python3
"""Raise the rate of the duplicate ledger key seen once in about twenty-five runs of test_cover_recert_pg C3 (one heap TID twice).

Rounds: W sessions insert in tight server-side loops while the main thread recertifies N times; then the writers stop, CHECKPOINT, and the
ledger chain is read from the relation file with the independent reader of the tests. Every TID that has more than one key is reported with the
page kinds (BASE = 2: found unreachable by the walk, KEYS = 1: written by an insert) and the ordinals. Prints one JSON object per round with
duplicates and a summary line.
"""
import argparse
import collections
import json
from pathlib import Path
import shutil
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402
import test_cover_insert_pg as ti  # noqa: E402
import test_cover_recert_pg as tr  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True)
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5891)
    ap.add_argument('--writers', type=int, default=8)
    ap.add_argument('--rounds', type=int, default=30)
    ap.add_argument('--recerts', type=int, default=4)
    ap.add_argument('--dim', type=int, default=8)
    ap.add_argument('--c3', action='store_true', help='each round on a fresh table: 3000 clustered rows in one statement, then the C3 writers and three recertifications')
    ap.add_argument('--statements', action='store_true', help='writers run one 10 row INSERT per connection like test_cover_recert_pg C3 instead of a server-side loop')
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    summary = collections.Counter()
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'dup', out, args.port)
        c.extra_opts = '-c autovacuum=off -c max_connections=100'
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')
            c.scalar('CREATE TABLE h(id int, v vector(8))')
            c.scalar('CREATE INDEX hi ON h USING pg_cuvs_hnsw (v vector_l2_ops)')
            path = c.data / c.scalar("SELECT pg_relation_filepath('hi')")
            clustered = ("(SELECT array_agg((((x % 25) * 7 + d * 3) % 41 + (random() - 0.5) * 0.05)::real) FROM generate_series(1, 8) d)::vector")
            sessions = [dg.Session(c) for _ in range(args.writers)]
            seq = [0]
            for rnd in range(args.rounds):
                stop = threading.Event()
                if args.c3:
                    tbl = f'h{rnd}'
                    c.scalar(f'CREATE TABLE {tbl}(id int, v vector(8))')
                    c.scalar(f'CREATE INDEX {tbl}i ON {tbl} USING pg_cuvs_hnsw (v vector_l2_ops)')
                    c.scalar(f"SELECT setseed(0.5); INSERT INTO {tbl} SELECT x, {clustered} FROM generate_series(1, 3000) x")
                    path = c.data / c.scalar(f"SELECT pg_relation_filepath('{tbl}i')")

                def writer(k):
                    while not stop.is_set():
                        seq[0] += 1
                        n = seq[0]
                        if args.c3:
                            c.q(f"INSERT INTO {tbl} SELECT 100000 + {k} * 100000 + {n} * 10 + x, {clustered} FROM generate_series(1, 10) x")
                            continue
                        if args.statements:
                            c.q(f"INSERT INTO h SELECT {k} * 1000000000 + {n} * 1000 + x, ARRAY[x, {k}, {n} % 7, 1, 2, 3, 4, 5]::real[]::vector FROM generate_series(1, 10) x")
                            continue
                        sessions[k].run(f"DO $$ BEGIN FOR i IN 1..200 LOOP INSERT INTO h VALUES ({k} * 1000000000 + {n} * 1000 + i, ARRAY[i, {k}, {n} % 7, 1, 2, 3, 4, 5]::real[]::vector); END LOOP; END $$")
                threads = [threading.Thread(target=writer, args=(k,)) for k in range(args.writers)]
                for th in threads:
                    th.start()
                time.sleep(0.0 if args.c3 else 0.5)
                answers = []
                for _ in range(args.recerts):
                    rc, r = c.q(f"SELECT pg_cuvs_cover_recertify('{tbl + 'i' if args.c3 else 'hi'}')::text")
                    answers.append(json.loads(r).get('result') if r.startswith('{') else r[:60])
                stop.set()
                for th in threads:
                    th.join()
                c.scalar('CHECKPOINT')
                pages = ti.read_pages(path)
                keys, kinds, order, d = tr.ledger_all(pages)
                by_tid = collections.defaultdict(list)
                for ordinal, hb, ho, kind in keys:
                    by_tid[(hb, ho)].append((ordinal, kind))
                dups = {f'{t[0]},{t[1]}': v for t, v in by_tid.items() if len(v) > 1}
                summary['rounds'] += 1
                summary['recerts_done'] += sum(1 for a in answers if a == 'done')
                summary['rounds_with_dup'] += 1 if dups else 0
                summary['dup_tids'] += len(dups)
                for v in dups.values():
                    summary['dup_kinds_' + '+'.join(str(k) for _, k in sorted(v, key=lambda x: x[0]))] += 1
                base_tids = sorted({(hb, ho) for _, hb, ho, kind in keys if kind == 2})
                base_ids = []
                if base_tids and args.c3:
                    arr = ','.join(f"'({b},{o})'::tid" for b, o in base_tids)
                    base_ids = [int(x) for x in c.scalar(f"SELECT id FROM {tbl} WHERE ctid = ANY(ARRAY[{arr}])").split()]
                    summary['rounds_with_base_keys'] += 1
                    summary['base_keys_of_writer_rows'] += sum(1 for i in base_ids if i >= 100000)
                    summary['base_keys_of_initial_rows'] += sum(1 for i in base_ids if i < 100000)
                probe = None
                if dups and args.c3:
                    # what a reader sees with the duplicate in the ledger: the row is returned once, and by its own vector it is first
                    probe = []
                    for t in dups:
                        rid = c.scalar(f"SELECT id FROM {tbl} WHERE ctid = '({t})'::tid")
                        vec = c.scalar(f"SELECT v::text FROM {tbl} WHERE ctid = '({t})'::tid")
                        rc, res = c.q(f"SET enable_seqscan = off; SELECT id FROM {tbl} ORDER BY v <-> '{vec}' LIMIT 40")
                        ids = res.split() if rc == 0 else []
                        probe.append({'id': rid, 'rows': len(ids), 'distinct': len(set(ids)), 'present': rid in ids, 'first': bool(ids) and ids[0] == rid})
                    summary['probes_with_repeated_row'] += sum(1 for pr in probe if pr['rows'] != pr['distinct'])
                    summary['probes_row_missing'] += sum(1 for pr in probe if not pr['present'])
                print(json.dumps({'round': rnd, 'keys': len(keys), 'answers': answers, 'dups': dups, 'base_row_ids': base_ids, 'probe': probe}), flush=True)
            for s in sessions:
                s.close()
        finally:
            c.drop()
    finally:
        shutil.rmtree(root, ignore_errors=True)
        for d in out.glob('data-*'):
            shutil.rmtree(d, ignore_errors=True)
    print('SUMMARY', json.dumps(dict(summary)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
