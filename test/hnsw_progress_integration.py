#!/usr/bin/env python3
"""pg_stat_progress_create_index for a source-less pg_cuvs_hnsw build.

Requires the installed extension and a running PostgreSQL; run as a test
superuser (make test-hnsw-progress). Reuses the scripted fake daemon of the K3
test (test/k3_fake_daemon.c) in its gate-fail mode: the daemon accepts the
build and holds the reply until STATEDIR/release exists. While it holds, a
second session reads the progress view of the building backend.

Checks:
  daemon   while the daemon holds the build, the view shows
           "building index: building graph in the GPU daemon" and the heap
           scan's blocks_done = blocks_total
  done     after the release the build fails as scripted and the backend
           leaves the view
The page-writing phase needs a real graph and is covered by
test-hnsw-converter (tuples_done/tuples_total of hnsw_write_disk_pages).
"""
import os, subprocess, sys, tempfile, time
from pathlib import Path

DB = os.environ.get('PGDATABASE') or 'postgres'
PSQL = ['psql', '-d', DB, '-X', '-qAt', '-v', 'ON_ERROR_STOP=0']
ROOT = Path(__file__).resolve().parents[1]
APP = f'hnsw-progress-it-{os.getpid()}'
failures = []


def check(name, ok, detail=''):
    print(('PASS ' if ok else 'FAIL ') + name + ('' if ok else ' ' + str(detail)[:600]))
    if not ok:
        failures.append(name)


def sql(s, timeout=120):
    return subprocess.run(PSQL, input=s, text=True, capture_output=True, timeout=timeout)


def progress():
    r = sql("SELECT p.phase || '|' || p.blocks_done || '|' || p.blocks_total "
            "FROM pg_stat_progress_create_index p JOIN pg_stat_activity a USING (pid) "
            f"WHERE a.application_name = '{APP}';")
    if r.returncode != 0 or 'ERROR' in r.stderr:
        raise RuntimeError(r.stderr)
    return r.stdout.strip()


def main():
    r = sql('CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_cuvs;')
    if r.returncode != 0 or 'ERROR' in r.stderr:
        raise RuntimeError(r.stderr)
    with tempfile.TemporaryDirectory(prefix='hnsw-progress-') as tmp:
        os.chmod(tmp, 0o755)
        state = Path(tmp)
        sock = str(state / 'socket')
        daemon_bin = state / 'k3-fake-daemon'
        subprocess.run(['cc', '-std=gnu11', '-I' + str(ROOT / 'src'), str(ROOT / 'test/k3_fake_daemon.c'),
                        str(ROOT / 'src/cuvs_ipc.c'), str(ROOT / 'src/cuvs_util.c'), str(ROOT / 'src/cuvs_build_corpus.c'),
                        '-o', str(daemon_bin), '-lpthread', '-lm', '-lrt'], check=True)
        daemon = subprocess.Popen([str(daemon_bin), sock, tmp])
        builder = None
        try:
            for _ in range(100):
                if os.path.exists(sock):
                    break
                if daemon.poll() is not None:
                    raise RuntimeError(f'fake daemon exited: {daemon.returncode}')
                time.sleep(0.05)
            else:
                raise RuntimeError('fake daemon socket was not created')
            events = state / 'events'
            (state / 'mode').write_text('gate-fail')
            builder = subprocess.Popen(PSQL, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                       stderr=subprocess.PIPE, text=True)
            assert builder.stdin is not None and builder.stderr is not None
            builder.stdin.write(f"SET application_name = '{APP}'; SET cuvs.socket_path = '{sock}';\n"
                                'CREATE TEMP TABLE hnsw_progress_it (v vector(16));\n'
                                'INSERT INTO hnsw_progress_it SELECT ARRAY(SELECT (i * 31 + j) % 97 / 97.0 '
                                'FROM generate_series(1, 16) j)::vector FROM generate_series(1, 5000) i;\n'
                                "CREATE INDEX ON hnsw_progress_it USING pg_cuvs_hnsw (v vector_l2_ops) "
                                "WITH (mode = 'hnsw');\n")
            builder.stdin.close()
            for _ in range(400):
                if events.exists() and 'mode=gate-fail' in events.read_text():
                    break
                if builder.poll() is not None:
                    raise RuntimeError(builder.stderr.read())
                time.sleep(0.025)
            else:
                raise RuntimeError('fake daemon did not receive the build')
            held = progress()
            phase, done, total = (held.split('|') + ['', '', ''])[:3]
            check('daemon:phase_names_the_GPU_daemon',
                  phase == 'building index: building graph in the GPU daemon', held)
            check('daemon:heap_scan_blocks_complete', done == total and done.isdigit() and int(done) > 0, held)
            (state / 'release').write_text('go')
            builder.wait(timeout=60)
            err = builder.stderr.read()
            check('done:scripted_build_failure',
                  'pg_cuvs: temporary HNSW build failed (status 5): k3 fake build failure' in err, err)
            check('done:backend_left_the_view', progress() == '', progress())
        finally:
            (state / 'release').touch()
            if builder and builder.poll() is None:
                builder.kill()
                builder.wait(timeout=5)
            daemon.terminate()
            daemon.wait(timeout=5)
    print(f'{"FAIL" if failures else "PASS"} hnsw progress integration: {len(failures)} failed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
