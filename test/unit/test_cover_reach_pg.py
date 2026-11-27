#!/usr/bin/env python3
"""The structural reachability walk on real pgvector hnsw indexes (isolated PostgreSQL 16 cluster, CPU only).

For several data shapes the index is built, thinned by DELETE + VACUUM, and then compared with an independent
oracle: a Python breadth first search over the relation file itself, read while the server is stopped. The
module under test (test/unit/cover_reach_pg_probe.c) includes src/cuvs_cover_reach_pg.c unchanged. Checked:
  * counts (elements, live, reached, unreached) and the exact set of unreached heap TIDs equal the oracle's;
  * the walk reports a clustered shape that really has unreached rows (otherwise the test proves nothing);
  * a memory limit below the accounted peak gives status OVER_LIMIT (1 is OK, 2 is the status for OVER_LIMIT);
  * while eight sessions insert, repeated walks keep returning consistent counts (reached <= live <= elements).
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time

FAILURES = []


def check(name, cond, detail=''):
    if not cond:
        FAILURES.append(name)
        print(f'FAIL {name} {detail}', file=sys.stderr)


def oracle(path):
    """Reachability from the entry point over the relation file, written without any of the product code."""
    data = Path(path).read_bytes()
    nblocks = len(data) // 8192
    page = lambda b: data[b * 8192:(b + 1) * 8192]
    meta = page(0)
    assert struct.unpack_from('<I', meta, 24)[0] == 0xA953A953
    entry_blk, entry_off = struct.unpack_from('<IH', meta, 40)

    def items(b):
        p = page(b)
        lower = struct.unpack_from('<H', p, 12)[0]
        out = {}
        for i in range((lower - 24) // 4):
            lp = struct.unpack_from('<I', p, 24 + 4 * i)[0]
            off, flags, ln = lp & 0x7FFF, (lp >> 15) & 3, lp >> 17
            if flags == 1:
                out[i + 1] = p[off:off + ln]
        return p, out

    live, elements, tids, nbr = {}, 0, {}, {}
    blk, seen_blk = 1, set()
    while blk != 0xFFFFFFFF and blk < nblocks and blk not in seen_blk:
        seen_blk.add(blk)
        p, it = items(blk)
        for off, t in it.items():
            if t[0] == 1 and len(t) >= 72:
                elements += 1
                nb, no = struct.unpack_from('>HH', t, 64)[0] << 16 | struct.unpack_from('>HH', t, 64)[1], 0
                nbr[(blk, off)] = (struct.unpack_from('<HHH', t, 64))
                if t[2] == 0:
                    live[(blk, off)] = True
                    tids[(blk, off)] = [(struct.unpack_from('<HHH', t, 4 + 6 * k)) for k in range(10)]
        blk = struct.unpack_from('<I', p, 8184)[0]
    cache = {}

    def neighbors(node):
        hi, lo, o = nbr[node]
        nb = (hi << 16) | lo
        if nb >= nblocks:
            return []
        if nb not in cache:
            cache[nb] = items(nb)[1]
        t = cache[nb].get(o)
        if t is None or t[0] != 2:
            return []
        count = struct.unpack_from('<H', t, 2)[0]
        count = min(count, (len(t) - 4) // 6)
        res = []
        for k in range(count):
            h, l, po = struct.unpack_from('<HHH', t, 4 + 6 * k)
            res.append((((h << 16) | l), po))
        return res

    reached = set()
    start = (entry_blk, entry_off)
    queue = []
    if start in live:
        reached.add(start)
        queue.append(start)
    while queue:
        u = queue.pop()
        for v in neighbors(u):
            if v in live and v not in reached:
                reached.add(v)
                queue.append(v)
    unreached = set()
    for node in live:
        if node not in reached:
            for h, l, po in tids[node]:
                if po != 0:
                    unreached.add((((h << 16) | l) << 16) | po)
    return {'elements': elements, 'live': len(live), 'reached': len(reached), 'unreached': len(live) - len(reached), 'unreached_tids': unreached}


class Server:
    def __init__(self, base, port):
        self.pgbin = Path(subprocess.check_output(['pg_config', '--bindir'], text=True).strip())
        self.data = base / 'data'
        self.sock = Path(tempfile.mkdtemp(prefix='crs'))
        self.port = port
        subprocess.run([str(self.pgbin / 'initdb'), '-D', str(self.data), '-A', 'trust', '-U', 'postgres'], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def start(self):
        opts = f'-p {self.port} -k {self.sock} -c listen_addresses= -c max_connections=40 -c autovacuum=off -c max_wal_size=200MB -c min_wal_size=32MB'
        subprocess.run([str(self.pgbin / 'pg_ctl'), '-D', str(self.data), '-o', opts, '-l', str(self.data / 'log'), '-w', 'start'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def stop(self, mode='fast'):
        subprocess.run([str(self.pgbin / 'pg_ctl'), '-D', str(self.data), '-m', mode, '-w', 'stop'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def q(self, sql):
        r = subprocess.run([str(self.pgbin / 'psql'), '-XAtq', '-h', str(self.sock), '-p', str(self.port), '-U', 'postgres', '-d', 'postgres',
                            '-v', 'ON_ERROR_STOP=1', '-c', sql], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        assert r.returncode == 0, (sql, r.stdout)
        return r.stdout.strip()


SHAPES = {
    # name: (rows, dim, clusters, m, ef_construction, delete percent)
    'clustered-small-m': (3000, 8, 25, 4, 8, 20),
    'clustered-vacuumed': (4000, 6, 40, 4, 8, 35),
    'uniform': (3000, 16, 1, 16, 64, 10),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5485)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    include = subprocess.check_output(['pg_config', '--includedir-server'], text=True).strip()
    cflags = shlex.split(subprocess.check_output(['pg_config', '--cflags'], text=True))
    lib = out / 'cover_reach_probe.so'
    r = subprocess.run([os.environ.get('CC', 'cc'), *cflags, '-shared', '-fPIC', '-Wno-unused-parameter', '-Wno-declaration-after-statement',
                        '-I' + include, '-I' + str(repo / 'src'), str(repo / 'test/unit/cover_reach_pg_probe.c'),
                        str(repo / 'src/cuvs_cover_reach_pg.c'), '-o', str(lib)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (out / 'compile.log').write_text(r.stdout)
    assert r.returncode == 0, r.stdout
    srv = Server(out, args.port)
    result = {'shapes': {}}
    try:
        srv.start()
        srv.q('CREATE EXTENSION vector')
        srv.q(f"CREATE FUNCTION probe(regclass, bigint, bigint) RETURNS TABLE(status int, nblocks bigint, elements bigint, live bigint, reached bigint, unreached bigint, emitted bigint, mem bigint, spilled bigint, needed bigint) AS '{lib}', 'cover_reach_probe' LANGUAGE C")
        srv.q(f"CREATE FUNCTION unreached(regclass, bigint, bigint) RETURNS SETOF bigint AS '{lib}', 'cover_reach_unreached' LANGUAGE C")
        for name, (rows, dim, clusters, m, efc, delp) in SHAPES.items():
            t = name.replace('-', '_')
            srv.q(f'CREATE TABLE {t}(id int, v vector({dim}))')
            # cluster centers far apart, points tightly around them
            srv.q(f"SELECT setseed(0.42); INSERT INTO {t} SELECT g, (SELECT array_agg((c.x + (random() - 0.5) * {0.05 if clusters > 1 else 1.0})::real) FROM (SELECT (((g % {clusters}) * 7 + d * 3) % 41)::real AS x FROM generate_series(1, {dim}) d) c)::vector FROM generate_series(1, {rows}) g")
            srv.q(f'CREATE INDEX {t}_i ON {t} USING hnsw (v vector_l2_ops) WITH (m = {m}, ef_construction = {efc})')
            srv.q(f'SELECT setseed(0.17); DELETE FROM {t} WHERE random() < {delp / 100.0}')
            srv.q(f'VACUUM {t}')
            probe = [int(x) for x in srv.q(f"SELECT * FROM probe('{t}_i', 1 << 28, 0)").split('|')]
            got_tids = {int(x) for x in srv.q(f"SELECT * FROM unreached('{t}_i', 1 << 28, 0)").split('\n') if x}
            path = srv.data / srv.q(f"SELECT pg_relation_filepath('{t}_i')")
            srv.q('CHECKPOINT')
            want = oracle(path)
            status, nblocks, elements, live, reached, unreached, emitted, mem, spilled, needed = probe
            check(f'{name}: status ok', status == 0, str(probe))
            check(f'{name}: elements', elements == want['elements'], f'{elements} vs {want["elements"]}')
            check(f'{name}: live', live == want['live'], f'{live} vs {want["live"]}')
            check(f'{name}: reached', reached == want['reached'], f'{reached} vs {want["reached"]}')
            check(f'{name}: unreached', unreached == want['unreached'], f'{unreached} vs {want["unreached"]}')
            check(f'{name}: unreached heap TIDs are exactly the oracle set', got_tids == want['unreached_tids'], f'{len(got_tids)} vs {len(want["unreached_tids"])}')
            small = srv.q(f"SELECT status FROM probe('{t}_i', {max(mem - 1, 0)}, 0)")
            check(f'{name}: a limit below the peak is refused', small == '2', small)
            # RAM for the page copy only: the block table and every bitmap live in an unlinked mapped temp file
            sp = [int(x) for x in srv.q(f"SELECT * FROM probe('{t}_i', 9000, 1 << 30)").split('|')]
            sp_tids = {int(x) for x in srv.q(f"SELECT * FROM unreached('{t}_i', 9000, 1 << 30)").split('\n') if x}
            check(f'{name}: with spill the status is ok and something spilled', sp[0] == 0 and sp[8] > 0 and sp[3 + 1 - 1] >= 0, str(sp))
            check(f'{name}: with spill the counts equal the oracle', sp[2] == want['elements'] and sp[3] == want['live'] and sp[4] == want['reached'] and sp[5] == want['unreached'], str(sp))
            check(f'{name}: with spill the unreached heap TIDs equal the oracle', sp_tids == want['unreached_tids'])
            check(f'{name}: with spill the RAM stays within its limit', sp[7] <= 9000, str(sp))
            no_spill = [int(x) for x in srv.q(f"SELECT * FROM probe('{t}_i', 9000, 0)").split('|')]
            check(f'{name}: without spill the same limit gives no result and says what is needed', no_spill[0] == 2 and no_spill[9] > 9000, str(no_spill))
            result['shapes'][name] = {'rows': rows, 'elements': elements, 'live': live, 'reached': reached, 'unreached': unreached,
                                      'nblocks': nblocks, 'mem_bytes': mem}
        check('some shape has unreached live rows (the oracle is not comparing zeros)', any(v['unreached'] > 0 for v in result['shapes'].values()),
              json.dumps(result['shapes']))

        # concurrent inserts: the counts stay consistent while the graph changes under the walk
        srv.q('CREATE TABLE hot(id serial, v vector(8))')
        srv.q("INSERT INTO hot(v) SELECT array_fill(g::real, ARRAY[8])::vector FROM generate_series(1, 2000) g")
        srv.q('CREATE INDEX hot_i ON hot USING hnsw (v vector_l2_ops) WITH (m = 8, ef_construction = 32)')
        stop = threading.Event()

        def writer(k):
            while not stop.is_set():
                srv.q(f"INSERT INTO hot(v) SELECT array_fill((random() * 3000)::real, ARRAY[8])::vector FROM generate_series(1, 50)")

        threads = [threading.Thread(target=writer, args=(k,)) for k in range(8)]
        for th in threads:
            th.start()
        bad = 0
        walks = 0
        t_end = time.time() + 12
        while time.time() < t_end:
            row = [int(x) for x in srv.q("SELECT * FROM probe('hot_i', 1 << 28, 0)").split('|')]
            status, nblocks, elements, live, reached, unreached, emitted, mem, spilled, needed = row
            walks += 1
            if status != 0 or not (reached <= live <= elements) or unreached != live - reached:
                bad += 1
        stop.set()
        for th in threads:
            th.join()
        check('concurrent inserts: every walk consistent', bad == 0 and walks >= 3, f'{bad} bad of {walks}')
        result['concurrent_walks'] = walks

        # the limits a recertification gets: RAM is maintenance_work_mem; the spill is at most half of what the volume under
        # base/pgsql_tmp can still give (a full volume stops the server) and nothing at all when temp_file_limit is finite
        srv.q("CREATE FUNCTION limits() RETURNS TABLE(ram bigint, spill bigint) AS '%s', 'cover_reach_limits' LANGUAGE C" % lib)
        sv_before = os.statvfs(srv.data)
        ram_l, spill_l = [int(x) for x in srv.q("SELECT * FROM limits()").split('|')]
        sv_after = os.statvfs(srv.data)
        half_before, half_after = sv_before.f_bavail * sv_before.f_frsize // 2, sv_after.f_bavail * sv_after.f_frsize // 2
        check('limits: RAM is maintenance_work_mem', ram_l == int(srv.q("SELECT setting FROM pg_settings WHERE name = 'maintenance_work_mem'")) * 1024, str(ram_l))
        check('limits: the spill is half of what the volume can still give rounded down to a power of two, never more',
              spill_l & (spill_l - 1) == 0 and spill_l <= max(half_before, half_after) and spill_l > min(half_before, half_after) // 2 - 1 and spill_l <= (1 << 46),
              f'{spill_l} vs {half_before} / {half_after}')
        finite = [int(x) for x in srv.q("SET temp_file_limit = '100MB'; SELECT * FROM limits()").splitlines()[-1].split('|')]
        check('limits: a finite temp_file_limit means no spill', finite[1] == 0, str(finite))

        # damage the relation file of a real index in the ways that used to end in OK: the walk must say DAMAGED (status 4),
        # and the independent count (every graph page of the relation, whatever the chain says) must show that rows were hidden
        srv.q('CREATE TABLE dm(id int, v vector(8))')
        srv.q("SELECT setseed(0.3); INSERT INTO dm SELECT g, (SELECT array_agg(random()::real) FROM generate_series(1, 8) d)::vector FROM generate_series(1, 6000) g")
        srv.q('CREATE INDEX dm_i ON dm USING hnsw (v vector_l2_ops) WITH (m = 8, ef_construction = 32)')
        dpath = srv.data / srv.q("SELECT pg_relation_filepath('dm_i')")
        srv.q('CHECKPOINT')
        srv.stop()
        original = dpath.read_bytes()
        npages = len(original) // 8192
        check('the damage index is long enough to have a middle', npages >= 12, str(npages))

        def graph_elements(data):
            """Element tuples on every page whose page id says graph page, found without following the chain."""
            n = 0
            for b in range(1, len(data) // 8192):
                pg = data[b * 8192:(b + 1) * 8192]
                if struct.unpack_from('<H', pg, 8190)[0] != 0xFF90:
                    continue
                lower = struct.unpack_from('<H', pg, 12)[0]
                for i in range((lower - 24) // 4):
                    lp = struct.unpack_from('<I', pg, 24 + 4 * i)[0]
                    if (lp >> 15) & 3 == 1 and pg[lp & 0x7FFF] == 1:
                        n += 1
            return n

        mid = npages // 2
        variants = {}
        d = bytearray(original); struct.pack_into('<I', d, mid * 8192 + 8184, 0xFFFFFFFF); variants['chain cut in the middle'] = bytes(d)
        d = bytearray(original); lp0 = struct.unpack_from('<I', d, mid * 8192 + 24)[0]
        struct.pack_into('<I', d, mid * 8192 + 24, (lp0 & 0x1FFFF) | (3 << 17)); variants['first item of a middle page: length 3'] = bytes(d)
        d = bytearray(original); struct.pack_into('<I', d, mid * 8192 + 24, (lp0 & ~(3 << 15)) | (3 << 15)); variants['first item of a middle page: LP_DEAD'] = bytes(d)
        d = bytearray(original); d[mid * 8192 + (lp0 & 0x7FFF)] = 9; variants['first item of a middle page: type 9'] = bytes(d)
        d = bytearray(original); d[mid * 8192:(mid + 1) * 8192] = bytes(8192); variants['a middle page of zeros'] = bytes(d)
        for label, data in variants.items():
            dpath.write_bytes(data)
            srv.start()
            row = [int(x) for x in srv.q("SELECT * FROM probe('dm_i', 1 << 28, 0)").split('|')]
            srv.stop()
            check(f'damage: {label}: the walk says DAMAGED (4) and gives no counts', row[0] == 4 and row[4] == 0 and row[5] == 0, str(row))
            if label == 'chain cut in the middle':
                check('damage: chain cut: the independent count of graph elements is the same as before the cut (the rows are there, the chain no longer reaches them)',
                      graph_elements(data) == graph_elements(original))
        dpath.write_bytes(original)
        srv.start()
        row = [int(x) for x in srv.q("SELECT * FROM probe('dm_i', 1 << 28, 0)").split('|')]
        check('damage: the undamaged file is OK again', row[0] == 0 and row[2] == graph_elements(original), str((row, graph_elements(original))))
    finally:
        srv.stop('immediate')
        shutil.rmtree(srv.data, ignore_errors=True)
    result['failures'] = FAILURES
    result['verdict'] = 'FAIL' if FAILURES else 'PASS'
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 1 if FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
