#!/usr/bin/env python3
"""Local disk plan of a source-less pg_cuvs_hnsw build on a live backend.

Requires the installed extension and daemon binary (the CPU shim is enough:
only the route forecast, op 40, is used before the refusal), a running
PostgreSQL, a superuser session, and passwordless sudo to mount a small tmpfs
(make test-hnsw-disk-plan-live). It starts its own daemon on a private socket
so the budgets left by earlier tests cannot decide the route first.

Checks:
  refuse   the index's tablespace sits on a 1 MiB tmpfs; with table
           statistics the build is refused before the heap scan with
           SQLSTATE 53100, naming that tablespace, and no index remains
  fits     into the default tablespace the plan does not refuse and the
           build reaches the daemon (the CPU shim then refuses the GPU build)
  ace      build_algo = 'ace' announces the plan before the build:
           "ACE-disk build needs about <MiB> MiB of additional local disk" with every item
"""
import os, subprocess, sys, tempfile, time
from pathlib import Path

DB = os.environ.get('PGDATABASE') or 'postgres'
PSQL = ['psql', '-d', DB, '-X', '-qAt', '-v', 'ON_ERROR_STOP=0', '-v', 'VERBOSITY=verbose']
REL = f'disk_plan_it_{os.getpid()}'
SPACE = f'disk_plan_small_{os.getpid()}'
failures = []


def check(name, ok, detail=''):
    print(('PASS ' if ok else 'FAIL ') + name + ('' if ok else ' ' + str(detail)[:800]))
    if not ok:
        failures.append(name)


def sql(s, timeout=300):
    return subprocess.run(PSQL, input=s, text=True, capture_output=True, timeout=timeout)


def bindir():
    return subprocess.run(['pg_config', '--bindir'], capture_output=True, text=True,
                          check=True).stdout.strip()


def sh(*cmd):
    subprocess.run(['sudo', '-n', *cmd], check=True)


def main():
    with tempfile.TemporaryDirectory(prefix='disk-plan-') as tmp:
        os.chmod(tmp, 0o755)
        sock = os.path.join(tmp, 'socket')
        idx = os.path.join(tmp, 'indexes')
        os.mkdir(idx)
        mountpoint = Path(tmp) / 'small'
        mountpoint.mkdir()
        log = Path(tmp) / 'daemon.log'
        executable = os.environ.get('PGCUVS_DISK_TEST_DAEMON') or os.path.join(bindir(), 'pg_cuvs_server')
        with log.open('w') as output:
            daemon = subprocess.Popen([executable, '--socket', sock, '--index-dir', idx,
                                       '--max-vram-mb', '38000'],
                                      stdout=output, stderr=subprocess.STDOUT)
            try:
                for _ in range(300):
                    if os.path.exists(sock):
                        break
                    if daemon.poll() is not None:
                        break
                    time.sleep(0.1)
                if not os.path.exists(sock):
                    raise RuntimeError(f'private daemon did not create a socket: {log.read_text()[-2000:]}')
                os.chmod(sock, 0o666)
                return run(f"SET cuvs.socket_path = '{sock}';\n", mountpoint, log)
            finally:
                daemon.terminate()
                daemon.wait(timeout=10)


def run(use_daemon, mountpoint, log):
    sh('mount', '-t', 'tmpfs', '-o', 'size=1m', 'tmpfs', str(mountpoint))
    try:
        space_dir = str(mountpoint / 'ts')
        sh('mkdir', space_dir)
        sh('chown', os.environ.get('PGCUVS_DISK_TEST_OS_USER', 'postgres'), space_dir)
        r = sql('CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_cuvs;\n'
                f"CREATE TABLESPACE {SPACE} LOCATION '{space_dir}';\n"
                f'CREATE TABLE {REL} (v vector(16)) WITH (autovacuum_enabled = false);\n'
                f'INSERT INTO {REL} SELECT ARRAY(SELECT (i * 31 + j) % 97 / 97.0 '
                'FROM generate_series(1, 16) j)::vector FROM generate_series(1, 5000) i;\n')
        if r.returncode != 0 or 'ERROR' in r.stderr:
            raise RuntimeError(r.stderr)

        small = (f"CREATE INDEX {REL}_small ON {REL} USING pg_cuvs_hnsw (v vector_l2_ops) "
                 f"TABLESPACE {SPACE};\n"
                 f"SELECT count(*) FROM pg_class WHERE relname = '{REL}_small';\n")
        build_event = '[hnsw_route] phase=build '
        r = sql(use_daemon + small)
        check('postscan:refuses_with_actual_rows',
              'ERROR:  53100:' in r.stderr and 'after the heap scan' in r.stderr and
              'from 5000 spooled rows' in r.stderr and
              r.stdout.strip().splitlines()[-1:] == ['0'], r.stderr)
        check('postscan:no_build_request', build_event not in log.read_text())
        r = sql(f'ANALYZE {REL};')
        if r.returncode != 0 or 'ERROR' in r.stderr:
            raise RuntimeError(r.stderr)
        r = sql(use_daemon + small)
        err = r.stderr
        check('refuse:sqlstate_53100', 'ERROR:  53100:' in err, err)
        check('refuse:names_the_tablespace_and_phase',
              'HNSW build needs' in err and 'of local disk on the filesystem of pg_tblspc/' in err
              and 'during the index write' in err, err)
        check('refuse:before_the_scan',
              'before the heap scan from reltuples=5000' in err and 'no GPU work was started' in err, err)
        check('refuse:detail_lists_index_pages', 'index pages ' in err, err)
        check('refuse:hint_suggests_analyze', 'run ANALYZE and retry' in err, err)
        check('refuse:no_index_left', r.stdout.strip().splitlines()[-1:] == ['0'], r.stdout)
        check('refuse:no_build_request', build_event not in log.read_text())

        r = sql(use_daemon + f"CREATE INDEX {REL}_default ON {REL} USING pg_cuvs_hnsw (v vector_l2_ops);\n")
        err = r.stderr
        check('fits:default_tablespace_passes_the_plan',
              'HNSW build needs' not in err and '53100' not in err and build_event in log.read_text(), err)

        r = sql(use_daemon + "SET client_min_messages = notice;\n"
                f"CREATE INDEX {REL}_ace ON {REL} USING pg_cuvs_hnsw (v vector_l2_ops) "
                "WITH (build_algo = 'ace');\n")
        err = r.stderr
        notice = err.find('NOTICE:  00000: pg_cuvs: ACE-disk build needs about ')
        check('ace:notice_announces_the_plan', notice >= 0, err)
        check('ace:notice_lists_every_item',
              all(k in err for k in ('daemon build: daemon workdir ', 'input spool ',
                                     'index write: graph file ', 'node-location spill ',
                                     'index pages ', 'estimated WAL ')), err)
        for line in err.splitlines():
            if 'ACE-disk build needs about' in line or line.startswith('DETAIL:  ') and 'index pages' in line:
                print('  ' + line)
        first_error = err.find('ERROR:')
        check('ace:notice_precedes_any_error', notice >= 0 and (first_error < 0 or notice < first_error), err)
    finally:
        try:
            r = sql(f'DROP TABLE IF EXISTS {REL};\nDROP TABLESPACE IF EXISTS {SPACE};')
            if r.returncode != 0 or 'ERROR' in r.stderr:
                raise RuntimeError(r.stderr)
        finally:
            sh('umount', str(mountpoint))
    print(f'{"FAIL" if failures else "PASS"} hnsw disk plan integration: {len(failures)} failed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
