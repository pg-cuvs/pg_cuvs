#!/usr/bin/env python3
"""Reproduction of a finding of an independent review at the real size (CPU, one private PostgreSQL 16 cluster, a library built with -DCUVS_COVER_RECERT_TEST_GATE). Prints JSON.

A table of --rows rows that all hold one vector (the walk lists nearly every one of them, so the new ledger chain is long: 338 keys a page) gets an index
that is planted as uncertified. The product threshold of the growth monitor (256 pages) is in force. Then
  A  a recertification that gives up between its new chain and its publication (the graph_epoch changes under it): how many pages the index grew by,
     and what the first plain insert afterwards does to the state of the index;
  B  the same, cancelled at that point (pg_cancel_backend);
  C  A and B again --repeat times: the size of the index after each;
  D  a recertification that works, while one insert is made between its new chain and its publication, on an index that has to be extended for the new
     chain and on one whose free list gives the pages: the state afterwards.
The directory of the data is under --output; it is removed at the end.
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
import test_cover_vacuum_pg as tv  # noqa: E402
import test_cover_walk_pg as tw  # noqa: E402

CONST = tw.CONST


def nblocks(c, idx):
    return int(c.scalar(f"SELECT pg_relation_size('{idx}') / 8192"))


def run_gated(c, gate, idx, during):
    return tw.gated_recert(c, gate, 'recert', idx, during)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True)
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5971)
    ap.add_argument('--rows', type=int, default=100000)
    ap.add_argument('--repeat', type=int, default=4)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    gate = out / 'gate'
    gate.mkdir()
    plant_bin = out / 'cover_plant'
    r = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(repo / 'src'), str(repo / 'test/unit/cover_plant.c'), '-o', str(plant_bin)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    assert r.returncode == 0, r.stdout
    os.environ['CUVS_RECERT_GATE_DIR'] = str(gate)
    res = {'rows': args.rows}
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'orph', out, args.port)
        c.extra_opts = '-c autovacuum=off -c max_parallel_maintenance_workers=0 -c maintenance_work_mem=512MB'
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')
            c.scalar('CREATE TABLE o(id int, v vector(8))')
            c.scalar(f"INSERT INTO o SELECT x, {CONST} FROM generate_series(1, {args.rows}) x")
            t0 = time.time()
            tv.hack_index(c, 'o', 'oi')
            res['build_seconds'] = round(time.time() - t0, 1)
            tv.plant(c, plant_bin, 'oi', 'uncertified')
            c.scalar('CHECKPOINT')
            n0 = nblocks(c, 'oi')
            rc, ans = c.q("SELECT pg_cuvs_cover_recertify('oi')::text")
            j = tw.jres(rc, ans)
            res['first_certification'] = {k: j.get(k) for k in ('result', 'unreached', 'kept_keys', 'reused_pages')}
            n1 = nblocks(c, 'oi')
            res['blocks_before'] = n0
            res['blocks_after_first_certification'] = n1
            st = tv.cover_status(c, 'oi')
            res['status_after_first'] = {k: st.get(k) for k in ('class', 'guarantee_in_force')}

            def give_up():
                (gate / 'recert_touch').write_text('e')

            def cancel():
                c.scalar("SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE query LIKE '%pg_cuvs_cover_recertify%' AND pid <> pg_backend_pid()")

            def one_round(kind):
                before = nblocks(c, 'oi')
                for f in gate.iterdir():
                    f.unlink()
                if kind == 'abandoned':
                    reached, (rc2, ans2) = run_gated(c, gate, 'oi', give_up)
                else:
                    reached, (rc2, ans2) = run_gated(c, gate, 'oi', cancel)
                after = nblocks(c, 'oi')
                j2 = tw.jres(rc2, ans2)
                return {'kind': kind, 'result': j2.get('result') or ans2[-120:], 'blocks_before': before, 'blocks_after': after, 'grew_by': after - before}

            a = one_round('abandoned')
            c.scalar("INSERT INTO o VALUES (900000001, " + CONST + ")")
            st = tv.cover_status(c, 'oi')
            a['after_one_plain_insert'] = {k: st.get(k) for k in ('class', 'guarantee_in_force', 'reason')}
            log = tv.server_log(c)
            a['log_says_grew_more_than_256'] = 'grew by more than' in log
            res['A_abandoned'] = a

            rc, ans = c.q("SELECT pg_cuvs_cover_recertify('oi')::text")
            res['A_recovery_call'] = tw.jres(rc, ans).get('result')
            b = one_round('cancelled')
            c.scalar("INSERT INTO o VALUES (900000002, " + CONST + ")")
            st = tv.cover_status(c, 'oi')
            b['after_one_plain_insert'] = {k: st.get(k) for k in ('class', 'guarantee_in_force', 'reason')}
            res['B_cancelled'] = b

            sizes = []
            for i in range(args.repeat):
                rc, ans = c.q("SELECT pg_cuvs_cover_recertify('oi')::text")
                sizes.append({'round': i, 'kind': 'recovery', 'result': tw.jres(rc, ans).get('result'), 'blocks': nblocks(c, 'oi')})
                for kind in ('abandoned', 'cancelled'):
                    r2 = one_round(kind)
                    sizes.append({'round': i, 'kind': kind, 'result': r2['result'], 'blocks': r2['blocks_after']})
            res['C_sizes'] = sizes

            # D: a recertification that works, on an index whose free list is empty (the new chain extends the index), with one insert between its
            # new chain and its publication; and the same where the free list gives the pages
            c.scalar('CREATE TABLE o2(id int, v vector(8))')
            c.scalar(f"INSERT INTO o2 SELECT x, {CONST} FROM generate_series(1, {args.rows}) x")
            tv.hack_index(c, 'o2', 'o2i')
            tv.plant(c, plant_bin, 'o2i', 'uncertified')
            before = nblocks(c, 'o2i')
            reached, (rc2, ans2) = run_gated(c, gate, 'o2i', lambda: c.scalar("INSERT INTO o2 VALUES (900000003, " + CONST + ")"))
            j2 = tw.jres(rc2, ans2)
            st = tv.cover_status(c, 'o2i')
            res['D_fresh_index_recert_with_one_insert_between'] = {
                'result': j2.get('result'), 'detail': j2.get('detail'), 'grew_by': nblocks(c, 'o2i') - before,
                'state_after': {k: st.get(k) for k in ('class', 'guarantee_in_force', 'reason')}}
            reached, (rc2, ans2) = run_gated(c, gate, 'o2i', lambda: c.scalar("INSERT INTO o2 VALUES (900000004, " + CONST + ")"))
            j2 = tw.jres(rc2, ans2)
            st = tv.cover_status(c, 'o2i')
            res['D_second_recert_free_list_pages'] = {
                'result': j2.get('result'), 'reused_pages': j2.get('reused_pages'), 'state_after': {k: st.get(k) for k in ('class', 'guarantee_in_force', 'reason')}}
            res['server_log_lines'] = [ln for ln in tv.server_log(c).splitlines() if 'pg_cuvs' in ln][-6:]
        finally:
            c.drop()
    finally:
        os.environ.pop('CUVS_RECERT_GATE_DIR', None)
        shutil.rmtree(root, ignore_errors=True)
        for d in out.glob('data-*'):
            shutil.rmtree(d, ignore_errors=True)
    (out / 'result.json').write_text(json.dumps(res, indent=2) + '\n')
    print(json.dumps(res, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
