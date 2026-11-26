#!/usr/bin/env python3
"""Local disk plan of a source-less pg_cuvs_hnsw build on a live backend.

Requires the installed extension and daemon binary (the CPU shim is enough:
only the route forecast, op 40, is used before the refusal), a running
PostgreSQL, a superuser session, and passwordless sudo to mount a small tmpfs
(make test-hnsw-disk-plan-live). It starts its own daemon on a private socket
so the budgets left by earlier tests cannot decide the route first.

Checks:
  refuse   a 1 MiB index tablespace refuses 5000 spooled rows with SQLSTATE
           53100 after the scan, with or without table statistics
  counts   stale statistics, partial predicates and NULL inputs leave enough
           space for the smaller spool; an empty partial index succeeds
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


def setup(sql_text):
    result = sql(sql_text)
    if result.returncode != 0 or 'ERROR' in result.stderr:
        raise RuntimeError(result.stderr)
    return result.stdout.strip()


def daemon_rows(log, offset, phase, n):
    return any(f'[hnsw_route] phase={phase} ' in line and f' rows={n} ' in line
               for line in log.read_text()[offset:].splitlines())


def check_small_spool(name, use_daemon, log, index_sql, rows):
    offset = len(log.read_text())
    result = sql(use_daemon + index_sql)
    check(f'{name}:actual_rows_reach_build',
          result.returncode == 0 and '53100' not in result.stderr and
          daemon_rows(log, offset, 'prespool', 5000) and
          daemon_rows(log, offset, 'postscan', rows) and
          daemon_rows(log, offset, 'build', rows), result.stderr)


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
        setup('CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_cuvs;\n'
              f"CREATE TABLESPACE {SPACE} LOCATION '{space_dir}';\n"
              f'CREATE TABLE {REL} (id int, v vector(16)) WITH (autovacuum_enabled = false);\n'
              f'INSERT INTO {REL} SELECT i, ARRAY(SELECT (i * 31 + j) % 97 / 97.0 '
              'FROM generate_series(1, 16) j)::vector FROM generate_series(1, 5000) i;\n')

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
        setup(f'ANALYZE {REL};')
        offset = len(log.read_text())
        r = sql(use_daemon + small)
        err = r.stderr
        check('refuse:sqlstate_53100', 'ERROR:  53100:' in err, err)
        check('refuse:names_the_tablespace_and_phase',
              'HNSW build needs' in err and 'of local disk on the filesystem of pg_tblspc/' in err
              and 'during the index write' in err, err)
        check('refuse:analyzed_uses_postscan_rows',
              'after the heap scan from 5000 spooled rows' in err and 'no GPU work was started' in err
              and daemon_rows(log, offset, 'prespool', 5000)
              and daemon_rows(log, offset, 'postscan', 5000), err)
        check('refuse:detail_lists_index_pages', 'index pages ' in err, err)
        check('refuse:hint_identifies_disk_action', 'Free space on that filesystem' in err, err)
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
        check('ace:notice_uses_actual_rows', 'after the heap scan from 5000 spooled rows' in err, err)
        check('ace:notice_lists_every_item',
              all(k in err for k in ('daemon build: daemon workdir ',
                                     'index write: graph file ', 'node-location spill ',
                                     'index pages ', 'estimated WAL ')), err)
        check('ace:daemon_mount_is_advisory',
              err.count('(free space not checked)') >= 2, err)
        print(f"wal_level={setup('SHOW wal_level;')}")
        for line in err.splitlines():
            if 'ACE-disk build needs about' in line or line.startswith('DETAIL:  ') and 'index pages' in line:
                print('  ' + line)
        first_error = err.find('ERROR:')
        check('ace:notice_precedes_any_error', notice >= 0 and (first_error < 0 or notice < first_error), err)

        setup(f'CREATE UNLOGGED TABLE {REL}_unlogged AS SELECT v FROM {REL};\n'
              f'ANALYZE {REL}_unlogged;')
        r = sql(use_daemon + f'CREATE INDEX ON {REL}_unlogged USING pg_cuvs_hnsw (v vector_l2_ops) '
                "WITH (build_algo = 'ace');\n")
        check('ace:unlogged_omits_index_page_wal',
              'ACE-disk build needs about ' in r.stderr and 'estimated WAL ' not in r.stderr, r.stderr)

        check_small_spool('partial', use_daemon, log,
                          f'CREATE INDEX {REL}_partial ON {REL} USING pg_cuvs_hnsw (v vector_l2_ops) '
                          f'TABLESPACE {SPACE} WHERE id <= 4;\n', 4)
        offset = len(log.read_text())
        r = sql(use_daemon
                + f'CREATE INDEX {REL}_empty ON {REL} USING pg_cuvs_hnsw (v vector_l2_ops) '
                  f'TABLESPACE {SPACE} WHERE id < 0;\n'
                  f"SELECT indisvalid FROM pg_index WHERE indexrelid = to_regclass('{REL}_empty');\n")
        check('partial_empty:creates_index_without_postscan_or_build',
              r.returncode == 0 and 'ERROR' not in r.stderr and r.stdout.strip() == 't' and
              daemon_rows(log, offset, 'prespool', 5000) and
              '[hnsw_route] phase=postscan ' not in log.read_text()[offset:] and
              build_event not in log.read_text()[offset:], r.stderr)

        setup(f'CREATE TABLE {REL}_nullable WITH (autovacuum_enabled = false) AS '
              f'SELECT CASE WHEN id <= 4 THEN v ELSE NULL END::vector(16) AS v FROM {REL};\n'
              f'ANALYZE {REL}_nullable;')
        check_small_spool('nulls', use_daemon, log,
                          f'CREATE INDEX ON {REL}_nullable USING pg_cuvs_hnsw (v vector_l2_ops) '
                          f'TABLESPACE {SPACE};\n', 4)

        setup(f'DELETE FROM {REL} WHERE id > 4;')
        counts = setup(f"SELECT reltuples::int, (SELECT count(*) FROM {REL}) "
                       f"FROM pg_class WHERE oid = '{REL}'::regclass;\n")
        check('stale:fixture_keeps_old_statistics', counts == '5000|4', counts)
        check_small_spool('stale', use_daemon, log,
                          f'CREATE INDEX {REL}_stale ON {REL} USING pg_cuvs_hnsw (v vector_l2_ops) '
                          f'TABLESPACE {SPACE};\n', 4)
    finally:
        try:
            setup(f'DROP TABLE IF EXISTS {REL}, {REL}_unlogged, {REL}_nullable;\n'
                  f'DROP TABLESPACE IF EXISTS {SPACE};')
        finally:
            sh('umount', str(mountpoint))
    print(f'{"FAIL" if failures else "PASS"} hnsw disk plan integration: {len(failures)} failed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
