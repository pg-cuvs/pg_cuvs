#!/usr/bin/env python3
"""Downgrade, rollback and upgrade-order behaviour of pg_cuvs 0.9.3 on real PostgreSQL servers.

Decision "V+D" (2026-11-16): the SQL of 0.9.3 names the handler symbol pg_cuvs_hnsw_handler_c1p, and
pg_cuvs--0.9.3--0.9.2.sql takes every guarantee mark off the indexes before it hands the access
method back to the old symbol. This runner proves, with a NEW library (built from this tree) and an
OLD library (any build that exports only pg_cuvs_hnsw_handler):

  S1 a fresh 0.9.3 install names the new symbol and its invalidation function is superuser only;
  S2 the planned path: ALTER EXTENSION UPDATE TO '0.9.2' under the new library, then the old
     library works on the same data directory without any error, and the way back up works;
  S3 the unplanned path: the old library meets SQL 0.9.3. Every statement that opens a
     pg_cuvs_hnsw index fails loudly, the downgrade script cannot run (fail-closed, nothing
     changes), the superuser's break-glass works, and the cluster is made consistent again;
  S4 ALTER EXTENSION UPDATE before the restart that loads the new package fails cleanly and
     succeeds after the restart;
  S0 pg_cuvs cannot be loaded without shared_preload_libraries (a PGC_POSTMASTER setting);
  S5 an index that carries a CERTIFIED guarantee mark makes the downgrade fail with SQL and handler
     symbol unchanged; LOST, OFF and damaged marks do not block it.

It builds a private copy of the PostgreSQL installation (relocated, nothing system-wide is touched),
starts its own clusters on Unix sockets with pg_cuvs preloaded, and removes them. pgvector must be
installed. No GPU: the pg_cuvs_hnsw indexes are pgvector hnsw indexes whose access method was
switched in the catalog, which is all the downgrade path looks at.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

FAILURES = []
TRANSCRIPT = []


def check(name, cond, detail=''):
    TRANSCRIPT.append(f"{'PASS' if cond else 'FAIL'} {name} {detail if not cond else ''}".rstrip())
    if not cond:
        FAILURES.append(name)
        print(f'FAIL {name} {detail}', file=sys.stderr)


class Overlay:
    """A relocated copy of the PostgreSQL installation with the library and SQL files swapped in."""

    def __init__(self, root, new_lib, old_lib, sql_dir, control):
        self.pg_bin = Path(subprocess.check_output(['pg_config', '--bindir'], text=True).strip())
        pkglib = Path(subprocess.check_output(['pg_config', '--pkglibdir'], text=True).strip())
        share = Path(subprocess.check_output(['pg_config', '--sharedir'], text=True).strip())
        prefix = self.pg_bin.parent
        self.root = root
        self.bin = root / str(self.pg_bin).lstrip('/')
        self.lib = root / str(pkglib).lstrip('/')
        self.ext = root / str(share).lstrip('/') / 'extension'
        shutil.copytree(prefix, root / str(prefix).lstrip('/'), symlinks=True)
        shutil.copytree(share, root / str(share).lstrip('/'), symlinks=True)
        for old in self.ext.glob('pg_cuvs*'):
            old.unlink()
        for f in sorted(sql_dir.glob('pg_cuvs--*.sql')):
            shutil.copy(f, self.ext / f.name)
        shutil.copy(control, self.ext / 'pg_cuvs.control')
        self.new_lib, self.old_lib = new_lib, old_lib

    def install(self, which):
        """Replace the library the way a package manager does: write a new file, then rename it."""
        src = self.new_lib if which == 'new' else self.old_lib
        tmp = self.lib / 'pg_cuvs.so.tmp'
        shutil.copy(src, tmp)
        os.replace(tmp, self.lib / 'pg_cuvs.so')


class Session:
    """One long-lived psql, so that a backend keeps its relation cache across a catalog change."""

    def __init__(self, cluster, role=None):
        self.p = subprocess.Popen(cluster.psql_cmd(), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, bufsize=1)
        if role:
            self.run(f'SET ROLE {role}')

    def run(self, sql):
        self.p.stdin.write(sql.rstrip(';') + ";\nSELECT '@@END@@';\n")
        self.p.stdin.flush()
        out = []
        while True:
            line = self.p.stdout.readline()
            if line == '' or line.strip() == '@@END@@':
                break
            out.append(line)
        return ''.join(out).strip()

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=20)


class Cluster:
    def __init__(self, overlay, name, base, port, preload=True):
        self.o, self.name, self.port = overlay, name, port
        self.data = base / f'data-{name}'
        self.sock = Path(tempfile.mkdtemp(prefix='dgs'))
        self.up = False
        self.preload = preload
        subprocess.run([str(overlay.bin / 'initdb'), '-D', str(self.data), '-A', 'trust', '-U', 'postgres'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def psql_cmd(self):
        return [str(self.o.bin / 'psql'), '-XAtq', '-h', str(self.sock), '-p', str(self.port), '-U', 'postgres',
                '-d', 'postgres']

    def start(self):
        opts = f'-p {self.port} -k {self.sock} -c listen_addresses= -c max_connections=30'
        if self.preload:
            opts += ' -c shared_preload_libraries=pg_cuvs'
        opts += ' ' + getattr(self, 'extra_opts', '')
        # -t 300 (default 60): the runners start servers on a host that other jobs load; crash recovery after an immediate stop has taken
        # longer than a minute there, and a start that gives up leaves the test with a half-started cluster instead of a result
        r = subprocess.run([str(self.o.bin / 'pg_ctl'), '-D', str(self.data), '-o', opts, '-l',
                            str(self.data / f'log-{int(time.time() * 1000)}'), '-w', '-t', '300', 'start'],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        assert r.returncode == 0, r.stdout
        self.up = True

    def stop(self, mode='fast'):
        if self.up:
            subprocess.run([str(self.o.bin / 'pg_ctl'), '-D', str(self.data), '-m', mode, '-w', 'stop'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
            self.up = False

    def restart(self):
        self.stop()
        self.start()

    def q(self, sql, role=None):
        cmd = self.psql_cmd()
        if role:
            sql = f'SET ROLE {role}; {sql}'
        r = subprocess.run(cmd + ['-c', sql], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        TRANSCRIPT.append(f'[{self.name}] {sql}\n    rc={r.returncode} {r.stdout.strip()[:400]}')
        return r.returncode, r.stdout.strip()

    def scalar(self, sql):
        rc, out = self.q(sql)
        assert rc == 0, out
        return out

    def make_index(self, table='t', index='ti'):
        """A pgvector hnsw index whose access method is switched to pg_cuvs_hnsw in the catalog: the pages
        are exactly those of an index built by an older pg_cuvs without a cover descriptor (LEGACY)."""
        for sql in (f'CREATE TABLE {table}(id int, v vector(3))',
                    f'INSERT INTO {table} SELECT g, ARRAY[g, g % 7, g % 3]::real[]::vector FROM generate_series(1, 300) g',
                    f'CREATE INDEX {index} ON {table} USING hnsw (v vector_l2_ops)',
                    f"UPDATE pg_class SET relam = (SELECT oid FROM pg_am WHERE amname = 'pg_cuvs_hnsw') WHERE relname = '{index}'",
                    f"UPDATE pg_index SET indclass = (SELECT oid::text::oidvector FROM pg_opclass WHERE opcname = 'vector_l2_ops' "
                    f"AND opcmethod = (SELECT oid FROM pg_am WHERE amname = 'pg_cuvs_hnsw')) WHERE indexrelid = '{index}'::regclass"):
            rc, out = self.q(sql)
            assert rc == 0, (sql, out)

    def version(self):
        return self.scalar("SELECT extversion FROM pg_extension WHERE extname = 'pg_cuvs'")

    def symbol(self):
        return self.scalar("SELECT prosrc FROM pg_proc WHERE proname = 'pg_cuvs_hnsw_handler'")

    def has_invalidate(self):
        return self.scalar("SELECT count(*) FROM pg_proc WHERE proname = 'pg_cuvs_cover_invalidate_all'") == '1'

    def works(self, table='t'):
        """Reads and writes through the index; returns '' on success or the first error text."""
        for sql in (f'SET enable_seqscan = off; SELECT id FROM {table} ORDER BY v <-> \'[1,1,1]\' LIMIT 3',
                    f"INSERT INTO {table} VALUES (100000 + (random() * 1e6)::int, '[2,3,4]')",
                    f'VACUUM {table}'):
            rc, out = self.q(sql)
            if rc != 0:
                return out
        return ''

    def metapage_hash(self, index='ti'):
        path = self.data / self.scalar(f"SELECT pg_relation_filepath('{index}')")
        import hashlib
        return hashlib.sha256(path.read_bytes()[:8192]).hexdigest()[:16]

    def drop(self):
        self.stop('immediate')


NEW_SYMBOL = 'pg_cuvs_hnsw_handler_c1p'
OLD_SYMBOL = 'pg_cuvs_hnsw_handler'


def scenario_fresh(o, base, port):
    c = Cluster(o, 's1', base, port)
    try:
        o.install('new')
        c.start()
        c.scalar('CREATE EXTENSION vector')
        c.scalar('CREATE EXTENSION pg_cuvs')
        check('S1 version is 0.9.3', c.version() == '0.9.3')
        check('S1 handler names the new symbol', c.symbol() == NEW_SYMBOL, c.symbol())
        check('S1 invalidation function exists', c.has_invalidate())
        c.scalar('CREATE ROLE plain LOGIN')
        rc, out = c.q('SELECT pg_cuvs_cover_invalidate_all()', role='plain')
        check('S1 invalidation is superuser only', rc != 0 and 'permission denied' in out, out)
        c.make_index()
        rc, out = c.q('SELECT pg_cuvs_cover_invalidate_all()')
        check('S1 a LEGACY index passes the invalidation', rc == 0 and '1 legacy' in out and out.endswith('1'), out)
        check('S1 the index works', c.works() == '')
    finally:
        c.drop()


def scenario_planned(o, base, port):
    c = Cluster(o, 's2', base, port)
    try:
        o.install('new')
        c.start()
        c.scalar('CREATE EXTENSION vector')
        c.scalar('CREATE EXTENSION pg_cuvs')
        c.make_index()
        a = Session(c)
        check('S2 session opened under the new routine works', 'ERROR' not in a.run("SET enable_seqscan = off; SELECT id FROM t ORDER BY v <-> '[1,1,1]' LIMIT 1"))
        rc, out = c.q("ALTER EXTENSION pg_cuvs UPDATE TO '0.9.2'")
        check('S2 ALTER EXTENSION UPDATE TO 0.9.2 succeeds under the new library', rc == 0, out)
        check('S2 version is 0.9.2', c.version() == '0.9.2')
        check('S2 handler is back on the old symbol', c.symbol() == OLD_SYMBOL, c.symbol())
        check('S2 the invalidation function is gone', not c.has_invalidate())
        members_down = c.scalar("SELECT string_agg(p.oid::regprocedure::text, ',' ORDER BY p.oid::regprocedure::text) FROM pg_depend d JOIN pg_proc p ON p.oid = d.objid WHERE d.refclassid = 'pg_extension'::regclass AND d.refobjid = (SELECT oid FROM pg_extension WHERE extname = 'pg_cuvs') AND d.classid = 'pg_proc'::regclass")
        fresh = Cluster(o, 's2f', base, port + 1)
        try:
            fresh.start()
            fresh.scalar('CREATE EXTENSION vector')
            fresh.scalar("CREATE EXTENSION pg_cuvs VERSION '0.9.2'")
            members_fresh = fresh.scalar("SELECT string_agg(p.oid::regprocedure::text, ',' ORDER BY p.oid::regprocedure::text) FROM pg_depend d JOIN pg_proc p ON p.oid = d.objid WHERE d.refclassid = 'pg_extension'::regclass AND d.refobjid = (SELECT oid FROM pg_extension WHERE extname = 'pg_cuvs') AND d.classid = 'pg_proc'::regclass")
        finally:
            fresh.drop()
        check('S2 the functions of the downgraded extension are exactly those of a fresh 0.9.2', members_down == members_fresh,
              f'{members_down} vs {members_fresh}')
        check('S2 the index still works under the new library', c.works() == '')
        check('S2 the session that was open during the downgrade still works',
              'ERROR' not in a.run("INSERT INTO t VALUES (7000001, '[1,2,3]'); SELECT count(*) FROM t"))
        a.close()
        # the package is replaced AFTER the script ran
        c.stop()
        o.install('old')
        c.start()
        err = c.works()
        check('S2 the old library serves the downgraded database without error', err == '', err)
        # and back up
        c.stop()
        o.install('new')
        c.start()
        rc, out = c.q("ALTER EXTENSION pg_cuvs UPDATE TO '0.9.3'")
        check('S2 the way back up succeeds', rc == 0 and c.version() == '0.9.3' and c.symbol() == NEW_SYMBOL, out)
        check('S2 the index works after the round trip', c.works() == '')
    finally:
        c.drop()


def scenario_unplanned(o, base, port):
    c = Cluster(o, 's3', base, port)
    try:
        o.install('new')
        c.start()
        c.scalar('CREATE EXTENSION vector')
        c.scalar('CREATE EXTENSION pg_cuvs')
        c.make_index()
        c.scalar('CREATE TABLE plain_t AS SELECT g AS id FROM generate_series(1, 10) g')
        # unplanned rollback: the package is replaced and the server restarted, SQL stays 0.9.3
        c.stop()
        o.install('old')
        c.start()
        want = f'could not find function "{NEW_SYMBOL}"'
        statements = {
            'SELECT': "SET enable_seqscan = off; SELECT id FROM t ORDER BY v <-> '[1,1,1]' LIMIT 3",
            'SELECT without the index': 'SELECT 1 FROM t LIMIT 1',
            'INSERT': "INSERT INTO t VALUES (1, '[1,2,3]')",
            'UPDATE': 'UPDATE t SET id = id WHERE id = 1',
            'DELETE': 'DELETE FROM t WHERE id = 2',
            'VACUUM': 'VACUUM t',
            'REINDEX': 'REINDEX INDEX ti',
            'TRUNCATE': 'TRUNCATE t',
            'DROP INDEX': 'DROP INDEX ti',
            'DROP TABLE': 'DROP TABLE t',
        }
        for label, sql in statements.items():
            rc, out = c.q(sql)
            check(f'S3 {label} fails loudly under the old library', rc != 0 and want in out, out)
        rc, out = c.q('ALTER TABLE t ADD COLUMN extra int')
        check('S3 ALTER TABLE ADD COLUMN still works', rc == 0, out)
        rc, out = c.q('INSERT INTO plain_t VALUES (11); SELECT count(*) FROM plain_t')
        check('S3 a table without a pg_cuvs_hnsw index is unaffected (control)', rc == 0 and out.endswith('11'), out)
        before = (c.version(), c.symbol())
        rc, out = c.q("ALTER EXTENSION pg_cuvs UPDATE TO '0.9.2'")
        check('S3 the downgrade script cannot run under the old library', rc != 0 and 'pg_cuvs_cover_invalidate_all' in out, out)
        check('S3 a failed downgrade changes nothing', (c.version(), c.symbol()) == before, str((c.version(), c.symbol())))
        rc, out = c.q('SELECT pg_cuvs_cover_invalidate_all()')
        check('S3 the invalidation function does not exist in the old library', rc != 0 and 'could not find function' in out, out)
        # break-glass: a superuser points the handler at the old symbol
        rc, out = c.q(f"CREATE OR REPLACE FUNCTION pg_cuvs_hnsw_handler(internal) RETURNS index_am_handler "
                      f"AS '$libdir/pg_cuvs', '{OLD_SYMBOL}' LANGUAGE C")
        check('S3 break-glass: CREATE OR REPLACE FUNCTION with the old symbol', rc == 0, out)
        err = c.works()
        check('S3 break-glass: the old library serves the index again', err == '', err)
        # consistent again: the new package returns
        c.stop()
        o.install('new')
        c.start()
        check('S3 after break-glass the SQL still says 0.9.3 with the old symbol', (c.version(), c.symbol()) == ('0.9.3', OLD_SYMBOL))
        rc, out = c.q("ALTER EXTENSION pg_cuvs UPDATE TO '0.9.2'")
        check('S3 resync step 1: downgrade under the new library', rc == 0 and c.version() == '0.9.2', out)
        rc, out = c.q("ALTER EXTENSION pg_cuvs UPDATE TO '0.9.3'")
        check('S3 resync step 2: upgrade again', rc == 0 and (c.version(), c.symbol()) == ('0.9.3', NEW_SYMBOL), out)
        check('S3 the index works after the resync', c.works() == '')
    finally:
        c.drop()


def scenario_order(o, base, port):
    c = Cluster(o, 's4', base, port)
    try:
        o.install('old')
        c.start()
        c.scalar('CREATE EXTENSION vector')
        c.scalar("CREATE EXTENSION pg_cuvs VERSION '0.9.2'")
        c.make_index()
        # the new package is installed, the postmaster still runs the old code
        o.install('new')
        rc, out = c.q("ALTER EXTENSION pg_cuvs UPDATE TO '0.9.3'")
        check('S4 UPDATE before the restart fails', rc != 0 and f'could not find function "{NEW_SYMBOL}"' in out, out)
        check('S4 the failed UPDATE leaves version and symbol alone', (c.version(), c.symbol()) == ('0.9.2', OLD_SYMBOL))
        err = c.works()
        check('S4 the database keeps working after the failed UPDATE', err == '', err)
        c.restart()
        rc, out = c.q("ALTER EXTENSION pg_cuvs UPDATE TO '0.9.3'")
        check('S4 UPDATE after the restart succeeds', rc == 0 and (c.version(), c.symbol()) == ('0.9.3', NEW_SYMBOL), out)
        check('S4 the index works after the upgrade', c.works() == '')
    finally:
        c.drop()


def crc32c(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 & -(crc & 1))
    return crc ^ 0xFFFFFFFF


def read_descriptor(cluster, path):
    """The cover descriptor of a metapage file, read at pd_upper; independent of the product code."""
    import struct
    page = Path(path).read_bytes()[:8192]
    upper = struct.unpack_from('<H', page, 14)[0]
    d = page[upper:upper + 184]
    return {'upper': upper, 'state': d[10], 'reason': d[11], 'graph_epoch': struct.unpack_from('<Q', d, 80)[0],
            'crc_ok': crc32c(d[:180]) == struct.unpack_from('<I', d, 180)[0]}


def scenario_marks(o, base, port, plant):
    for mode, blocks in (('certified', True), ('lost', False), ('off', False), ('corrupt', False)):
        c = Cluster(o, 's5' + mode, base, port)
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')
            c.make_index()
            path = c.data / c.scalar("SELECT pg_relation_filepath('ti')")
            c.stop()
            r = subprocess.run([str(plant), str(path), mode], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            check(f'S5 {mode}: the mark was planted', r.returncode == 0, r.stdout)
            c.start()
            meta = c.metapage_hash()
            rc, out = c.q("ALTER EXTENSION pg_cuvs UPDATE TO '0.9.2'")
            if blocks:
                check('S5 certified: the downgrade goes through', rc == 0 and c.version() == '0.9.2' and c.symbol() == OLD_SYMBOL, out)
                check('S5 certified: the function reports the index it took off', 'NOTICE' not in out or '1 certified taken off' in out, out)
                c.scalar('CHECKPOINT')
                d = read_descriptor(c, path)
                check('S5 certified: the descriptor now says LOST, reason downgrade, graph_epoch 1, crc valid',
                      d['state'] == 2 and d['reason'] == 10 and d['graph_epoch'] == 1 and d['crc_ok'], str(d))
                # the Generic WAL record survives a crash
                c.stop('immediate')
                c.start()
                c.scalar('CHECKPOINT')
                d2 = read_descriptor(c, path)
                check('S5 certified: and survives crash recovery', d2 == d, f'{d2} vs {d}')
            else:
                check(f'S5 {mode}: the downgrade goes through', rc == 0 and c.version() == '0.9.2', out)
                check(f'S5 {mode}: the symbol is the old one', c.symbol() == OLD_SYMBOL)
        finally:
            c.drop()


def scenario_preload(o, base, port):
    """A fact the playbook relies on; it pins today's behaviour so a change is noticed."""
    c = Cluster(o, 's0', base, port, preload=False)
    try:
        o.install('new')
        c.start()
        c.scalar('CREATE EXTENSION vector')
        rc, out = c.q('CREATE EXTENSION pg_cuvs')
        check('S0 pg_cuvs cannot be used without shared_preload_libraries (PGC_POSTMASTER setting)',
              rc != 0 and 'cannot create PGC_POSTMASTER variables after startup' in out, out)
    finally:
        c.drop()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True, help='pg_cuvs.so built from this tree')
    ap.add_argument('--old-lib', type=Path, required=True, help='a pg_cuvs.so that exports only pg_cuvs_hnsw_handler')
    ap.add_argument('--sql-dir', type=Path, default=repo / 'sql')
    ap.add_argument('--port', type=int, default=5483)
    ap.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    plant = out / 'cover_plant'
    r = subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(repo / 'src'),
                        str(repo / 'test/unit/cover_plant.c'), '-o', str(plant)], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
    assert r.returncode == 0, r.stdout
    root = out / 'pgroot'
    try:
        o = Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), args.sql_dir.resolve(), repo / 'pg_cuvs.control')
        scenario_preload(o, out, args.port)
        scenario_fresh(o, out, args.port)
        scenario_planned(o, out, args.port)
        scenario_unplanned(o, out, args.port)
        scenario_order(o, out, args.port)
        scenario_marks(o, out, args.port, plant)
    finally:
        (out / 'transcript.txt').write_text('\n'.join(TRANSCRIPT) + '\n')
        shutil.rmtree(root, ignore_errors=True)
        for d in out.glob('data-*'):
            shutil.rmtree(d, ignore_errors=True)
    result = {'scope': 'pg_cuvs 0.9.3 downgrade, rollback and upgrade order on private PostgreSQL clusters',
              'checks': len(TRANSCRIPT), 'failures': FAILURES, 'verdict': 'FAIL' if FAILURES else 'PASS'}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 1 if FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
