#!/usr/bin/env python3
"""Can a recertification end with an unreachable row that is not in the ledger? (the direction opposite to the duplicate key of repro_ledger_dup.py)

The walk reads the graph page by page without the insert lock, so an insert that runs during the walk can cut the last edge to a row the walk has
already counted as reached. This tool measures, per round, the graph at the instant of publication against the ledger that is published:

  1. a graph with many unreachable rows and a high exposure to new disconnections (a pgvector hnsw index with m = 4, switched to pg_cuvs_hnsw, as in
     test_cover_recert_pg C1; certified by a first recertification);
  2. a round: U0 = the rows the graph cannot reach (read from the relation file with the independent oracle of the tests), then eight sessions insert
     while a recertification is held at its gate (after the walk, before the publication);
  3. the writers are stopped at the gate (the graph is quiet now: this is the graph at publication), U_pre = the rows it cannot reach; the gate opens, the
     recertification publishes; L = the TIDs the published ledger holds;
  4. U_pre minus L is reported by class: NEW (inserted after the round began: must be 0), OLD_NEWLY_CUT (existed at the start, reachable then, cut by
     an insert during the round) and OLD_RECUT (unreachable at the start too, reconnected and cut again).
Needs the gate build (-DCUVS_COVER_RECERT_TEST_GATE) as --new-lib.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
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
    ap.add_argument('--new-lib', type=Path, required=True, help='built with -DCUVS_COVER_RECERT_TEST_GATE')
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5901)
    ap.add_argument('--rounds', type=int, default=20)
    ap.add_argument('--writers', type=int, default=8)
    ap.add_argument('--m', type=int, default=4)
    ap.add_argument('--rows', type=int, default=4000)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    plant = out / 'cover_plant'
    r = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(repo / 'src'), str(repo / 'test/unit/cover_plant.c'), '-o', str(plant)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    assert r.returncode == 0, r.stdout
    gate = out / 'gate'
    gate.mkdir()
    os.environ['CUVS_RECERT_GATE_DIR'] = str(gate)
    tot = {'rounds': 0, 'new_rows_missing': 0, 'old_newly_cut_missing': 0, 'old_recut_missing': 0, 'unreachable_at_publication': 0, 'recert_done': 0}
    rounds = []
    clus = "(SELECT array_agg((((x % 25) * 7 + d * 3) % 41 + (random() - 0.5) * 0.05)::real) FROM generate_series(1, 8) d)::vector"
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'wv', out, args.port)
        c.extra_opts = '-c autovacuum=off -c max_connections=60'
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')
            c.scalar('CREATE TABLE g(id int, v vector(8))')
            c.scalar(f"SELECT setseed(0.42); INSERT INTO g SELECT x, {clus} FROM generate_series(1, {args.rows}) x")
            c.scalar(f'CREATE INDEX gi ON g USING hnsw (v vector_l2_ops) WITH (m = {args.m}, ef_construction = {max(8, 2 * args.m)})')
            c.scalar("UPDATE pg_class SET relam = (SELECT oid FROM pg_am WHERE amname = 'pg_cuvs_hnsw') WHERE relname = 'gi'")
            c.scalar("UPDATE pg_index SET indclass = (SELECT oid::text::oidvector FROM pg_opclass WHERE opcname = 'vector_l2_ops' AND opcmethod = (SELECT oid FROM pg_am WHERE amname = 'pg_cuvs_hnsw')) WHERE indexrelid = 'gi'::regclass")
            path = c.data / c.scalar("SELECT pg_relation_filepath('gi')")
            c.stop()
            rpl = subprocess.run([str(plant), str(path), 'uncertified'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            assert rpl.returncode == 0, rpl.stdout
            c.start()
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('gi')::text")
            assert rc == 0 and '"result": "done"' in res, res
            next_id = [1000000]
            for rnd in range(args.rounds):
                c.scalar('CHECKPOINT')
                u0, _ = tr.unreachable_tids(path)
                boundary = int(c.scalar('SELECT max(id) FROM g'))
                old_tids = {tuple(int(x) for x in t.strip('()').split(',')) for t in json.loads(c.scalar('SELECT json_agg(ctid::text) FROM g'))}
                for f in gate.iterdir():
                    f.unlink()
                (gate / 'recert_armed').write_text('')
                box = []
                th = threading.Thread(target=lambda: box.append(c.q("SELECT pg_cuvs_cover_recertify('gi')::text")))
                stop = threading.Event()

                def writer(k):
                    n = 0
                    while not stop.is_set():
                        n += 1
                        base = next_id[0] + k * 10000000 + n * 10
                        c.q(f"INSERT INTO g SELECT {base} + x, {clus} FROM generate_series(1, 10) x")
                ws = [threading.Thread(target=writer, args=(k,)) for k in range(args.writers)]
                th.start()
                for w in ws:
                    w.start()
                for _ in range(600):
                    if (gate / 'recert_reached').exists():
                        break
                    time.sleep(0.05)
                time.sleep(0.2)
                stop.set()
                for w in ws:
                    w.join()
                next_id[0] += 100000000
                c.scalar('CHECKPOINT')
                u_pre, _ = tr.unreachable_tids(path)
                (gate / 'recert_go').write_text('')
                th.join()
                (gate / 'recert_armed').unlink()
                rc, res = box[0]
                done = rc == 0 and '"result": "done"' in res
                c.scalar('CHECKPOINT')
                keys, kinds, order, d = tr.ledger_all(ti.read_pages(path))
                ledger = {(k[1], k[2]) for k in keys}
                missing = u_pre - ledger
                new_m = {t for t in missing if t not in old_tids}
                old_cut = {t for t in missing if t in old_tids and t not in u0}
                old_recut = {t for t in missing if t in old_tids and t in u0}
                tot['rounds'] += 1
                tot['recert_done'] += 1 if done else 0
                tot['unreachable_at_publication'] += len(u_pre)
                tot['new_rows_missing'] += len(new_m)
                tot['old_newly_cut_missing'] += len(old_cut)
                tot['old_recut_missing'] += len(old_recut)
                rounds.append({'round': rnd, 'done': done, 'u0': len(u0), 'u_pre': len(u_pre), 'ledger_keys': len(keys), 'new_missing': len(new_m),
                               'old_newly_cut_missing': len(old_cut), 'old_recut_missing': len(old_recut)})
                print(json.dumps(rounds[-1]), flush=True)
        finally:
            c.drop()
    finally:
        shutil.rmtree(root, ignore_errors=True)
        for d in out.glob('data-*'):
            shutil.rmtree(d, ignore_errors=True)
    print('SUMMARY', json.dumps(tot))
    return 0


if __name__ == '__main__':
    sys.exit(main())
