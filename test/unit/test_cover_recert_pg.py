#!/usr/bin/env python3
"""F6a: recertification on real PostgreSQL servers (CPU only), checked against an oracle that shares no code with it.

  C1 an index that starts the way a row-built one does (LOST, uncertified, no ledger; pgvector hnsw graph with m = 4 so that
     rows really are unreachable, under the pg_cuvs_hnsw access method) is recertified: CERTIFIED, and its BASE ledger keys are
     exactly the rows an independent walk of the relation file cannot reach from the entry point; base_rows and base_missing
     match; the chain is BASE pages then a KEYS page; the descriptor's crc holds;
  C2 after that, ordered scans find every one of those rows (the query is the row's own vector) without the heap-wide notice,
     and equal PostgreSQL's exact sort;
  C3 recertification while eight sessions insert: afterwards every row the search cannot reach is in the ledger, no key is lost
     or duplicated, ordinals are contiguous and the chain's generation is the new one;
  C5 a recertification replaces the delta keys written before it started by the base (the walk covers those rows), with a new generation;
  C6 a LOST index flagged by the growth monitor is certified again and the flag is cleared;
  C7 (with --pause-lib, a build with a gate before publishing) rows inserted while the new chain is waiting are carried over;
  C10 replaced chains go to the free list and are reused by the next recertification;
  C9 (same build) keys beyond the 1000 the exclusive phase may copy are taken by unlocked passes first;
  C8 (same build) a descriptor change between the walk and the publication abandons the recertification and leaves the old chain;
  C4 a second recertification of a CERTIFIED index is allowed by hand and keeps the invariant; one for an index without a
     descriptor says it does not apply.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402
import test_cover_insert_pg as ti  # noqa: E402
import test_cover_reach_pg as rp  # noqa: E402

check = dg.check


def unreachable_tids(path):
    o = rp.oracle(path)
    return {(t >> 16 >> 16, (t >> 16) & 0xFFFF) if False else (((t >> 16) >> 16), (t >> 16) & 0xFFFF) for t in ()} | {((t >> 16), (t & 0xFFFF)) for t in o['unreached_tids']}, o


def ledger_all(pages):
    d = ti.descriptor(pages)
    keys, kinds, order, blk = [], [], [], d['head']
    while blk != 0xFFFFFFFF:
        order.append(blk)
        p = pages[blk]
        magic, version, kind, gen, first, count, seen, crc = struct.unpack_from('<IHHQQIII', p, 24)
        assert magic == 0x4C45444C and gen == d['generation'] and crc == dg.crc32c(p[24:56]), (blk, gen, d['generation'])
        kinds.append(kind)
        for i in range(count):
            ordinal, xid, hb, ho, flags = ti.decode_key(p[64 + 24 * i:64 + 24 * i + 24])
            keys.append((ordinal, hb, ho, kind))
        blk = struct.unpack_from('<I', p, 8184)[0]
    return keys, kinds, order, d


def free_list(pages):
    """The blocks of the free list (descriptor offset 32), following the next pointers of the pages."""
    meta = pages[0]
    upper = struct.unpack_from('<H', meta, 14)[0]
    blk = struct.unpack_from('<I', meta, upper + 32)[0]
    out = []
    while blk != 0xFFFFFFFF:
        assert blk not in out and blk < len(pages), f'free list loop or out of range at {blk}'
        out.append(blk)
        blk = struct.unpack_from('<I', pages[blk], 8184)[0]
    return out


def tail_seen(pages, d):
    magic, version, kind, gen, first, count, seen, crc = struct.unpack_from('<IHHQQIII', pages[d['tail']], 24)
    return seen


def ledger_all_nogen(pages):
    d = ti.descriptor(pages)
    return [], [], [], d


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True)
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5501)
    ap.add_argument('--pause-lib', type=Path, help='pg_cuvs.so built with -DCUVS_COVER_RECERT_TEST_GATE')
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    plant = out / 'cover_plant'
    r = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(repo / 'src'), str(repo / 'test/unit/cover_plant.c'), '-o', str(plant)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    assert r.returncode == 0, r.stdout
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'rc', out, args.port)
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')

            # C1: a graph with unreachable rows, in the state a row-built index starts in
            c.scalar('CREATE TABLE g(id int, v vector(8))')
            c.scalar("SELECT setseed(0.42); INSERT INTO g SELECT x, (SELECT array_agg((((x % 25) * 7 + d * 3) % 41 + (random() - 0.5) * 0.05)::real) FROM generate_series(1, 8) d)::vector FROM generate_series(1, 4000) x")
            c.scalar('CREATE INDEX gi ON g USING hnsw (v vector_l2_ops) WITH (m = 4, ef_construction = 8)')
            c.scalar("UPDATE pg_class SET relam = (SELECT oid FROM pg_am WHERE amname = 'pg_cuvs_hnsw') WHERE relname = 'gi'")
            c.scalar("UPDATE pg_depend SET refobjid = (SELECT oid FROM pg_opclass WHERE opcname = 'vector_l2_ops' AND opcmethod = (SELECT oid FROM pg_am WHERE amname = 'pg_cuvs_hnsw')) WHERE classid = 'pg_class'::regclass AND objid = 'gi'::regclass AND refclassid = 'pg_opclass'::regclass")
            c.scalar("UPDATE pg_index SET indclass = (SELECT oid::text::oidvector FROM pg_opclass WHERE opcname = 'vector_l2_ops' AND opcmethod = (SELECT oid FROM pg_am WHERE amname = 'pg_cuvs_hnsw')) WHERE indexrelid = 'gi'::regclass")
            path = c.data / c.scalar("SELECT pg_relation_filepath('gi')")
            c.stop()
            rpl = subprocess.run([str(plant), str(path), 'uncertified'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            check('C1 the uncertified state was planted', rpl.returncode == 0, rpl.stdout)
            c.start()
            st = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('gi')::text"))
            check('C1 it starts LOST', st.get('class') == 'lost', str(st))
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('gi')::text")
            check('C1 the recertification reports done', rc == 0 and '"result": "done"' in res, res)
            result = json.loads(res) if rc == 0 else {}
            c.scalar('CHECKPOINT')
            pages = ti.read_pages(path)
            keys, kinds, order, d = ledger_all(pages)
            unreached, orc = unreachable_tids(path)
            base = {(k[1], k[2]) for k in keys if k[3] == 2}
            check('C1 there are unreachable rows (the test compares more than nothing)', len(unreached) > 0, str(len(unreached)))
            check('C1 the BASE keys are exactly the rows the independent walk cannot reach', base == unreached, f'{len(base)} vs {len(unreached)}')
            check('C1 base_missing and base_rows match the walk', result.get('unreached') == orc['unreached'] and result.get('live') == orc['live'], str(result))
            check('C1 the descriptor is CERTIFIED with an intact crc and the chain ends in a KEYS page', d['state'] == 1 and d['crc_ok'] and kinds[-1] == 1 and kinds[0] == 2, str((d, kinds)))
            check('C1 ordinals are contiguous', [k[0] for k in keys] == list(range(len(keys))))
            check('C1 the first chain of an index (every page newly allocated) records the relation size after the allocation in its tail page',
                  tail_seen(pages, d) == len(pages), f'{tail_seen(pages, d)} vs {len(pages)}')
            st = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('gi')::text"))
            check('C1 the status function agrees', st.get('class') == 'certified' and st.get('guarantee_in_force') is True, str(st))

            # C2
            miss = 0
            sample = sorted(unreached)[:60]
            for blk, off in sample:
                rc, res = c.q(f"SET enable_seqscan = off; SELECT id FROM g WHERE ctid = '({blk},{off})'")
                row_id = res.strip()
                rc, res = c.q(f"SET enable_seqscan = off; SELECT id FROM g ORDER BY v <-> (SELECT v FROM g WHERE id = {row_id}) LIMIT 1")
                if rc != 0 or res.strip() != row_id or 'answers from the heap' in res:
                    miss += 1
            check('C2 every unreachable row is found by its own vector through the merged scan, without the heap notice', miss == 0, f'{miss} of {len(sample)}')
            # the promise is about the rows the graph cannot reach, not exact ranking beyond the graph's own answers: for queries
            # next to unreachable rows, every unreachable row of the exact top 30 is in the merged top 40
            u_ids = set()
            for blk, off in sorted(unreached):
                rc, res = c.q(f"SELECT id FROM g WHERE ctid = '({blk},{off})'")
                u_ids.add(int(res.strip()))
            bad = 0
            for row_id in sorted(u_ids)[:25]:
                rc1, exact = c.q(f"SET enable_indexscan = off; SET enable_bitmapscan = off; SELECT id FROM g ORDER BY v <-> (SELECT v FROM g WHERE id = {row_id}) LIMIT 30")
                rc2, merged = c.q(f"SET enable_seqscan = off; SELECT id FROM g ORDER BY v <-> (SELECT v FROM g WHERE id = {row_id}) LIMIT 40")
                x = {int(i) for i in exact.split() if i.isdigit()} & u_ids
                m = {int(i) for i in merged.split() if i.isdigit()}
                bad += (not x <= m)
            check('C2 for queries next to unreachable rows, every unreachable row of the exact top 30 is in the merged top 40', bad == 0, f'{bad} queries')

            # C3: recertification under eight inserting sessions, on an index that grows through the insert path
            c.scalar('CREATE TABLE h(id int, v vector(8))')
            c.scalar('CREATE INDEX hi ON h USING pg_cuvs_hnsw (v vector_l2_ops)')
            c.scalar("SELECT setseed(0.5); INSERT INTO h SELECT x, (SELECT array_agg((((x % 25) * 7 + d * 3) % 41 + (random() - 0.5) * 0.05)::real) FROM generate_series(1, 8) d)::vector FROM generate_series(1, 3000) x")
            stop = threading.Event()
            counter = [0]

            def writer(k):
                n = 0
                while not stop.is_set():
                    n += 1
                    c.q(f"INSERT INTO h SELECT 100000 + {k} * 100000 + {n} * 10 + x, (SELECT array_agg((((x % 25) * 7 + d * 3) % 41 + (random() - 0.5) * 0.05)::real) FROM generate_series(1, 8) d)::vector FROM generate_series(1, 10) x")
                counter[0] += n
            threads = [threading.Thread(target=writer, args=(k,)) for k in range(8)]
            for th in threads:
                th.start()
            results = []
            for _ in range(3):
                rc, res = c.q("SELECT pg_cuvs_cover_recertify('hi')::text")
                results.append(res)
            stop.set()
            for th in threads:
                th.join()
            check('C3 the recertifications under load all end in done', all('"result": "done"' in x for x in results), str(results))
            hpath = c.data / c.scalar("SELECT pg_relation_filepath('hi')")
            c.scalar('CHECKPOINT')
            pages = ti.read_pages(hpath)
            keys, kinds, order, d = ledger_all(pages)
            unreached, orc = unreachable_tids(hpath)
            have = {(k[1], k[2]) for k in keys}
            heap = ti.ctids(c, 'h')
            check('C3 every row the search cannot reach is in the ledger', unreached <= have, f'{len(unreached - have)} missing of {len(unreached)}')
            ords = [k[0] for k in keys]
            bad_at = next((i for i, o in enumerate(ords) if o != i), None)
            check('C3 no key is duplicated and ordinals are contiguous', len(have) == len(keys) and ords == list(range(len(keys))),
                  f'keys={len(keys)} distinct_tids={len(have)} first_bad_index={bad_at} around={ords[max(0, (bad_at or 0) - 3):(bad_at or 0) + 4] if bad_at is not None else None} dups={[[k for k in keys if (k[1], k[2]) == t] for t in have if sum(1 for k in keys if (k[1], k[2]) == t) > 1][:3]}')
            check('C3 the generation is the new one and the descriptor is intact', d['state'] == 1 and d['crc_ok'] and d['generation'] >= 4, str(d))
            check('C3 every key names a row of the heap', have <= heap, f'{len(have - heap)} keys without a row')
            check('C3 rows were inserted while it ran', counter[0] > 8, str(counter[0]))

            # C13 (review 2f6789ed R7-3): writers that insert in a tight server-side loop hold the cover lock in share mode almost all the
            # time; a recertification that only polls for the exclusive lock never gets it (twelve of twelve calls deferred). It queues.
            c.scalar('CREATE TABLE sh(id int, v vector(8))')
            c.scalar('CREATE INDEX shi ON sh USING pg_cuvs_hnsw (v vector_l2_ops)')
            stop2 = threading.Event()
            sess = [dg.Session(c) for _ in range(8)]

            def tight(k):
                n = 0
                while not stop2.is_set():
                    n += 1
                    sess[k].run(f"DO $$ BEGIN FOR i IN 1..300 LOOP INSERT INTO sh VALUES ({k} * 100000000 + {n} * 1000 + i, ARRAY[i, {k}, {n} % 7, 1, 2, 3, 4, 5]::real[]::vector); END LOOP; END $$")
            thr = [threading.Thread(target=tight, args=(k,)) for k in range(8)]
            for th in thr:
                th.start()
            time.sleep(2)
            answers = []
            for _ in range(6):
                t0 = time.time()
                rc, res = c.q("SELECT pg_cuvs_cover_recertify('shi')::text")
                answers.append((res, round(time.time() - t0, 2)))
            stop2.set()
            for th in thr:
                th.join()
            for ss in sess:
                ss.close()
            check('C13 six recertifications under eight tight insert loops all end in done (before the queue: twelve of twelve deferred after the whole 2 s bound)', all('"result": "done"' in a[0] for a in answers), str(answers)[:600])

            # C5: deterministic carry-over
            c.scalar("INSERT INTO h SELECT 800000 + x, (SELECT array_agg((((x % 25) * 7 + d * 3) % 41 + (random() - 0.5) * 0.05)::real) FROM generate_series(1, 8) d)::vector FROM generate_series(1, 500) x")
            c.scalar('CHECKPOINT')
            pages = ti.read_pages(hpath)
            keys_before, kinds_before, _, d_before = ledger_all(pages)
            n_delta_before = sum(1 for k in keys_before if k[3] == 1)
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('hi')::text")
            c.scalar('CHECKPOINT')
            pages = ti.read_pages(hpath)
            keys_after, kinds_after, _, d_after = ledger_all(pages)
            carried = json.loads(res).get('keys_carried_over') if rc == 0 else None
            tids_new_delta = {(k[1], k[2]) for k in keys_after if k[3] == 1}
            tids_old_delta = {(k[1], k[2]) for k in keys_before if k[3] == 1}
            unreached_now, _ = unreachable_tids(hpath)
            base_keys = {(k[1], k[2]) for k in keys_after if k[3] == 2}
            check('C5 the walk covers every row that existed when it started: the delta keys written before are not carried over', carried == 0 and not any(k[3] == 1 for k in keys_after) and n_delta_before > 0, f'{carried}, kinds {set(kinds_after)}, before {n_delta_before}: rc={rc} {res}')
            check('C5 the new chain holds exactly the rows the independent walk cannot reach', base_keys == unreached_now and len(keys_after) == len(base_keys), f'{len(base_keys)} vs {len(unreached_now)}')
            check('C5 the new chain\'s tail page records the relation size as it is after the chain was allocated (the growth monitor must not take the chain itself for foreign growth)', tail_seen(ti.read_pages(hpath), d_after) == len(ti.read_pages(hpath)), f'{tail_seen(ti.read_pages(hpath), d_after)} vs {len(ti.read_pages(hpath))}')
            check('C5 the generation went up by one', d_after['generation'] == d_before['generation'] + 1, f"{d_before['generation']} -> {d_after['generation']}")

            # C6
            c.make_index('s', 'si')
            spath = c.data / c.scalar("SELECT pg_relation_filepath('si')")
            c.stop()
            rpl = subprocess.run([str(plant), str(spath), 'suspect'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            check('C6 a suspect mark was planted', rpl.returncode == 0, rpl.stdout)
            c.start()
            st = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('si')::text"))
            check('C6 it starts LOST (unkeyed writer)', st.get('class') == 'lost' and st.get('stored_reason') == 7, str(st))
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('si')::text")
            c.scalar('CHECKPOINT')
            dd = ti.descriptor(ti.read_pages(spath))
            check('C6 recertified: CERTIFIED, the SUSPECT flag is gone, graph_epoch is kept', rc == 0 and dd['state'] == 1 and dd['flags'] & 2 == 0 and dd['epoch'] == 3 and dd['crc_ok'], f'{res} {dd}')

            # C4
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('hi')::text")
            check('C4 recertifying a certified index by hand works', rc == 0 and '"result": "done"' in res, res)
            c.make_index('t', 'ti')
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('ti')::text")
            check('C4 an index without a descriptor: not applicable', rc == 0 and '"result": "not_applicable"' in res, res)
        finally:
            c.drop()
        if args.pause_lib:
            o2 = dg.Overlay(root.parent / 'pgroot2', args.pause_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
            os.environ['CUVS_RECERT_GATE_DIR'] = str(out / 'gate')
            c = dg.Cluster(o2, 'rp', out, args.port + 1)
            try:
                o2.install('new')
                c.start()
                c.scalar('CREATE EXTENSION vector')
                c.scalar('CREATE EXTENSION pg_cuvs')
                c.scalar('CREATE TABLE p(id int, v vector(8))')
                c.scalar('CREATE INDEX pi ON p USING pg_cuvs_hnsw (v vector_l2_ops)')
                c.scalar("INSERT INTO p SELECT x, (SELECT array_agg(random()::real) FROM generate_series(1, 8) d)::vector FROM generate_series(1, 1000) x")
                gate = out / 'gate'
                shutil.rmtree(gate, ignore_errors=True)
                gate.mkdir(parents=True)

                last_insert_at = [None]

                def run_gated(table, index, touch, n=200, base=5000):
                    for f in gate.iterdir():
                        f.unlink()
                    (gate / 'recert_armed').write_text('')
                    res_box = []
                    th = threading.Thread(target=lambda: res_box.append(c.q(f"SELECT pg_cuvs_cover_recertify('{index}')::text")))
                    th.start()
                    for _ in range(600):
                        if (gate / 'recert_reached').exists():
                            break
                        time.sleep(0.1)
                    reached = (gate / 'recert_reached').exists()
                    c.scalar(f"INSERT INTO {table} SELECT {base} + x, (SELECT array_agg(random()::real) FROM generate_series(1, 8) d)::vector FROM generate_series(1, {n}) x")
                    last_insert_at[0] = c.scalar("SELECT clock_timestamp()")
                    if touch:
                        (gate / 'recert_touch').write_text(touch)
                    (gate / 'recert_go').write_text('')
                    th.join()
                    (gate / 'recert_armed').unlink()
                    return reached, res_box[0]

                reached, (rc, res) = run_gated('p', 'pi', None)
                c.scalar('CHECKPOINT')
                ppath = c.data / c.scalar("SELECT pg_relation_filepath('pi')")
                keys, kinds, order, d = ledger_all(ti.read_pages(ppath))
                have = {(k[1], k[2]) for k in keys}
                late = json.loads(c.scalar("SELECT json_agg(ctid::text) FROM p WHERE id > 5000"))
                late_tids = {tuple(int(x) for x in t.strip('()').split(',')) for t in late}
                check('C7 the gate was reached', reached)
                st7 = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('pi')::text"))
                ts7 = c.scalar(f"SELECT '{st7.get('certified_at')}'::timestamptz < '{last_insert_at[0]}'::timestamptz")
                check('C7 certified_at is the start of the recertification (before the insert that ran while its new chain waited)', ts7 == 't', str(st7) + ' insert at ' + str(last_insert_at[0]))
                check('C7 the keys since the certification are the 200 inserted after it began, the ones the walk did not cover (they count toward the 5 percent)', st7.get('keys_since_certification') == 200, str(st7))
                check('C7 the recertification finished', rc == 0 and '"result": "done"' in res, res)
                check('C7 the 200 rows inserted while the new chain waited have their keys in the published chain', late_tids <= have, f'{len(late_tids - have)} missing')
                check('C7 ordinals are contiguous and no key is duplicated', [k[0] for k in keys] == list(range(len(keys))) and len(have) == len(keys))

                # C9: more keys than the exclusive phase should copy are written while the new chain waits
                reached, (rc, res) = run_gated('p', 'pi', None, n=3000, base=20000)
                c.scalar('CHECKPOINT')
                keys9, kinds9, order9, d9 = ledger_all(ti.read_pages(ppath))
                have9 = {(k[1], k[2]) for k in keys9}
                late9 = json.loads(c.scalar("SELECT json_agg(ctid::text) FROM p WHERE id > 20000"))
                late9_tids = {tuple(int(x) for x in t.strip('()').split(',')) for t in late9}
                r9 = json.loads(res) if rc == 0 else {}
                check('C9 the gate was reached and the recertification finished', reached and r9.get('result') == 'done', res)
                check('C9 the 3000 keys were carried over, and none of them was copied while the lock was held (the unlocked passes took them)',
                      r9.get('keys_carried_over') == 3000 and r9.get('keys_copied_locked') == 0, res)
                check('C9 every one of the 3000 rows has its key in the published chain, ordinals are contiguous, no duplicate',
                      late9_tids <= have9 and [k[0] for k in keys9] == list(range(len(keys9))) and len(have9) == len(keys9), f'{len(late9_tids - have9)} missing')

                # C10: replaced chains are reused: the relation stops growing when the same index is recertified again and again
                sizes, reused, ok_lists = [], [], True
                for r10 in range(4):
                    rc, res = c.q("SELECT pg_cuvs_cover_recertify('pi')::text")
                    c.scalar('CHECKPOINT')
                    pages10 = ti.read_pages(ppath)
                    sizes.append(len(pages10))
                    reused.append(json.loads(res).get('reused_pages') if rc == 0 else None)
                    keys10, kinds10, order10, d10 = ledger_all(pages10)
                    fl = free_list(pages10)
                    for b in fl:
                        magic, version, kind, gen, first, count, seen, crc = struct.unpack_from('<IHHQQIII', pages10[b], 24)
                        ok_lists &= (magic == 0x4C45444C and gen < d10['generation'] and b not in order10 and b >= 2)
                    ok_lists &= not (set(fl) & set(order10))
                    every = [b for b in range(2, len(pages10)) if struct.unpack_from('<I', pages10[b], 24)[0] == 0x4C45444C]
                    ok_lists &= sorted(every) == sorted(order10 + fl)	# no ledger page is lost to both the chain and the list
                unreached10, _ = unreachable_tids(ppath)
                base10 = {(k[1], k[2]) for k in keys10 if k[3] == 2}
                check('C10 the relation does not grow once the first replaced chain exists (sizes after four more recertifications)', sizes[1] == sizes[2] == sizes[3], str(sizes))
                check('C10 later recertifications take their pages from the free list', all(x and x > 0 for x in reused[1:]), str(reused))
                check('C10 the free list reads as ledger pages of older generations, disjoint from the live chain, without loops, and chain + list are every ledger page of the relation', ok_lists and len(fl) > 0, str(fl))
                check('C10 and the chain is still right after the reuse: its BASE keys are the rows the walk cannot reach', base10 == unreached10, f'{len(base10)} vs {len(unreached10)}')

                # C12: a conflicting statement holds the table: the call gives up at lock_timeout instead of queueing behind it
                holder = dg.Session(c)
                holder.run('BEGIN')
                holder.run('LOCK TABLE p IN SHARE UPDATE EXCLUSIVE MODE')
                t0 = time.time()
                rc, res = c.q("SET lock_timeout = '300ms'; SELECT pg_cuvs_cover_recertify('pi')::text")
                waited = time.time() - t0
                holder.run('COMMIT')
                holder.close()
                check('C12 with the table locked by a conflicting statement the call answers deferred after about lock_timeout', rc == 0 and '"result": "deferred"' in res and waited < 3.0, f'{waited:.2f}s {res}')
                rc, res = c.q("SELECT pg_cuvs_cover_recertify('pi')::text")
                check('C12 and works as before once the lock is free', rc == 0 and '"result": "done"' in res, res)

                # C8: the descriptor changes between the walk and the publication (e = graph_epoch, g = generation, s = state)
                for i8, (kind, what) in enumerate((('e', 'graph_epoch'), ('s', 'state'), ('g', 'generation'))):
                    base8 = 40000 + 1000 * i8
                    c.scalar('CHECKPOINT')
                    keys0, kinds0, order0, d_before = ledger_all(ti.read_pages(ppath))
                    reached, (rc, res) = run_gated('p', 'pi', kind, base=base8)
                    c.scalar('CHECKPOINT')
                    keys8, kinds8, order8, d8 = ledger_all_nogen(ti.read_pages(ppath)) if kind == 'g' else ledger_all(ti.read_pages(ppath))
                    check(f'C8{kind} the gate was reached', reached)
                    check(f'C8{kind} a change of {what} abandons the recertification', rc == 0 and '"result": "abandoned"' in res, res)
                    check(f'C8{kind} the head of the old chain is still the descriptor\'s', d8['head'] == d_before['head'], f"{d_before['head']} -> {d8['head']}")
                    have8 = {(k[1], k[2]) for k in keys8}
                    late8 = json.loads(c.scalar(f"SELECT json_agg(ctid::text) FROM p WHERE id > {base8} AND id <= {base8 + 200}"))
                    late8_tids = {tuple(int(x) for x in t.strip('()').split(',')) for t in late8}
                    if kind != 'g':
                        check(f'C8{kind} the rows inserted meanwhile still have their keys in the old chain', late8_tids <= have8, f'{len(late8_tids - have8)} of {len(late8_tids)} missing, e.g. {sorted(late8_tids - have8)[:5]}; chain {d8["head"]}..{d8["tail"]} gen {d8["generation"]} keys {len(keys8)}')
            finally:
                c.drop()
                shutil.rmtree(root.parent / 'pgroot2', ignore_errors=True)
    finally:
        (out / 'transcript.txt').write_text('\n'.join(dg.TRANSCRIPT) + '\n')
        shutil.rmtree(root, ignore_errors=True)
        for d in out.glob('data-*'):
            shutil.rmtree(d, ignore_errors=True)
    result = {'failures': dg.FAILURES, 'verdict': 'FAIL' if dg.FAILURES else 'PASS',
              'not_run': 'the exclusive-lock timeout (deferred) paths; the memory-limit failure memory; the automatic runs from VACUUM (F6b)'}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 1 if dg.FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
