#!/usr/bin/env python3
"""K3 HNSW input digest: live-backend checks against a scripted fake daemon.

Requires the installed extension and a running PostgreSQL; run as a test
superuser (make test-k3-digest). Builds and starts test/k3_fake_daemon.c and
points cuvs.socket_path at it per session, so no GPU or real daemon is used.
The digest LOG line reaches the client through client_min_messages = log.

PGCUVS_SERVER_LOG must name the server log file (read with pg_read_file) for
the late-interrupt check, whose client is gone by the time the backend acts.

Checks (each guards a regression of src/pg_cuvs.c / cuvs_hnsw_digest.h):
  fail     daemon BUILD_FAILED: one LOG, digest = SHA-256 of the expected
           float32 rows, build error unchanged (supplementary digest, byte total)
  cancel   daemon status CANCELED: still one LOG + "(status 11)" error
           (only a backend-sent cancel skips the digest)
  ok       successful RPC: exactly one LOG
  hold     statement_timeout while the RPC is held: no LOG, timeout error
  late     client lost during the RPC, detected only by the client connection
           check after the reply: interrupt followed before the digest, no LOG
"""
import array, hashlib, math, os, re, signal, subprocess, sys, tempfile, time
from pathlib import Path

DB = os.environ.get('PGDATABASE') or 'postgres'
PSQL = ['psql', '-d', DB, '-X', '-qAt', '-v', 'ON_ERROR_STOP=0']
ROOT = Path(__file__).resolve().parents[1]
LOG_RE = re.compile(r'LOG:  pg_cuvs: HNSW input kind=sha256-float32-native-row-scan-order digest=([0-9a-f]{64}) '
                    r'rows=(\d+) dim=(\d+) metric=(\d+) input_dev=\d+ input_ino=\d+ index_oid=\d+')
failures = []


def check(name, ok, detail=''):
    print(('PASS ' if ok else 'FAIL ') + name + ('' if ok else ' ' + str(detail)[:600]))
    if not ok:
        failures.append(name)


def sql(s, timeout=120):
    return subprocess.run(PSQL, input=s, text=True, capture_output=True, timeout=timeout)


def f32(x):
    return array.array('f', [x])[0]


def rows_for(n, dim, seed, zero_every=0, half=False):
    out, s = [], seed
    for i in range(n):
        if zero_every and i % zero_every == 0:
            out.append([0.0] * dim)
            continue
        r = []
        for _ in range(dim):
            s = (s * 1103515245 + 12345) & 0x7FFFFFFF
            k = (s >> 8) % 2001 - 1000
            r.append(k / 64.0 if half else k / 256.0)
        out.append(r)
    return out


def expected(rows, cosine):
    h, n = hashlib.sha256(), 0
    for r in rows:
        if cosine:
            norm2 = 0.0
            for f in r:
                norm2 += f * f
            if norm2 == 0:
                continue
            norm = math.sqrt(norm2)
            h.update(array.array('f', [f32(f) / norm for f in r]).tobytes())
        else:
            h.update(array.array('f', r).tobytes())
        n += 1
    return h.hexdigest(), n


TABLES = {  # name: (type, dim, opclass, n, seed, zero_every)
    'k3it_l2': ('vector', 3, 'vector_l2_ops', 50, 3, 0),
    'k3it_cos': ('vector', 8, 'vector_cosine_ops', 400, 5, 7),
    'k3it_half': ('halfvec', 5, 'halfvec_l2_ops', 300, 7, 0),
    'k3it_big': ('vector', 64, 'vector_l2_ops', 20000, 9, 0),   # several 1 MiB chunks
    'k3it_tiny': ('vector', 5, 'vector_l2_ops', 7, 11, 0),
}


def setup():
    r = sql('CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_cuvs;')
    assert r.returncode == 0, r.stderr
    exp = {}
    for name, (typ, dim, opc, n, seed, ze) in TABLES.items():
        rows = rows_for(n, dim, seed, ze, half=(typ == 'halfvec'))
        data = ''.join('[' + ','.join(repr(f) for f in r) + ']\n' for r in rows)
        r = sql(f'DROP TABLE IF EXISTS {name}; CREATE TABLE {name} (v {typ}({dim}));\n'
                f'COPY {name} FROM STDIN;\n{data}\\.\n')
        assert r.returncode == 0 and 'ERROR' not in r.stderr, r.stderr
        exp[name] = expected(rows, 'cosine' in opc)
    return exp


def create(sock, name, extra=''):
    typ, dim, opc = TABLES[name][:3]
    r = sql(f"SET client_min_messages = log; SET cuvs.socket_path = '{sock}'; {extra}\n"
            f"CREATE INDEX ON {name} USING pg_cuvs_hnsw (v {opc}) WITH (mode = 'hnsw');\n")
    logs = LOG_RE.findall(r.stderr)
    errors = [l.split('ERROR:', 1)[1].strip() for l in r.stderr.splitlines() if 'ERROR:' in l]
    return logs, errors, r.stderr


def server_log_size(path):
    r = sql(f"SELECT (pg_stat_file('{path}')).size;")
    return int(r.stdout.strip())


def server_log_from(path, offset):
    r = sql(f"SELECT pg_read_file('{path}', {offset}, (pg_stat_file('{path}')).size - {offset});")
    return r.stdout


def main():
    exp = setup()
    with tempfile.TemporaryDirectory(prefix='k3-digest-') as tmp:
        os.chmod(tmp, 0o777)
        state = Path(tmp)
        sock = str(state / 'socket')
        daemon_bin = state / 'k3-fake-daemon'
        subprocess.run(['cc', '-std=gnu11', '-I' + str(ROOT / 'src'), str(ROOT / 'test/k3_fake_daemon.c'),
                        str(ROOT / 'src/cuvs_ipc.c'), str(ROOT / 'src/cuvs_util.c'), str(ROOT / 'src/cuvs_build_corpus.c'),
                        '-o', str(daemon_bin), '-lpthread', '-lm', '-lrt'], check=True)
        daemon = subprocess.Popen([str(daemon_bin), sock, tmp])
        try:
            for _ in range(100):
                if os.path.exists(sock):
                    break
                time.sleep(0.05)
            mode = state / 'mode'
            events = state / 'events'

            mode.write_text('fail')
            for name in ('k3it_l2', 'k3it_cos', 'k3it_half', 'k3it_big'):
                logs, errors, _ = create(sock, name)
                e_digest, e_rows = exp[name]
                check(f'fail:{name}:one_LOG_expected_digest',
                      len(logs) == 1 and logs[0][0] == e_digest and int(logs[0][1]) == e_rows
                      and int(logs[0][2]) == TABLES[name][1], (logs, e_digest, e_rows))
                check(f'fail:{name}:build_error_unchanged',
                      errors == ['pg_cuvs: temporary HNSW build failed (status 5): k3 fake build failure'], errors)

            mode.write_text('cancel')
            for name in ('k3it_l2', 'k3it_big'):
                logs, errors, _ = create(sock, name)
                check(f'cancel_status:{name}:one_LOG_and_status11_error',
                      len(logs) == 1 and logs[0][0] == exp[name][0]
                      and errors == ['pg_cuvs: temporary HNSW build failed (status 11): k3 fake daemon cancel'], (logs, errors))

            mode.write_text('ok')
            for name in ('k3it_l2', 'k3it_big'):
                logs, errors, raw = create(sock, name)
                check(f'ok:{name}:exactly_one_LOG', len(logs) == 1 and logs[0][0] == exp[name][0], (logs, raw[-400:]))

            mode.write_text('hold')
            before = events.read_text().count('mode=hold') if events.exists() else 0
            logs, errors, _ = create(sock, 'k3it_big', "SET statement_timeout = '1500ms';")
            held = events.read_text().count('mode=hold') - before
            check('hold:timeout_in_RPC_no_LOG',
                  held == 1 and logs == [] and any('statement timeout' in e for e in errors), (held, logs, errors))

            log_path = os.environ.get('PGCUVS_SERVER_LOG')
            if not log_path:
                check('late:server_log_configured', False, 'set PGCUVS_SERVER_LOG to the server log file')
            else:
                mode.write_text('gate-fail')
                (state / 'release').unlink(missing_ok=True)
                before = events.read_text().count('mode=gate-fail') if events.exists() else 0
                offset = server_log_size(log_path)
                p = subprocess.Popen(PSQL, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, text=True)
                p.stdin.write("SET application_name = 'k3-late'; SET client_min_messages = log; "
                              f"SET cuvs.socket_path = '{sock}'; SET client_connection_check_interval = '50ms';\n"
                              "CREATE INDEX ON k3it_tiny USING pg_cuvs_hnsw (v vector_l2_ops) WITH (mode = 'hnsw');\n")
                p.stdin.flush()
                for _ in range(400):
                    if events.exists() and events.read_text().count('mode=gate-fail') > before:
                        break
                    time.sleep(0.025)
                reached = events.exists() and events.read_text().count('mode=gate-fail') > before
                p.kill(); p.wait()
                time.sleep(0.5)                       # several connection-check periods
                (state / 'release').write_text('go')  # reply only now: the loss is noticed after it
                gone = False
                for _ in range(200):
                    r = sql("SELECT count(*) FROM pg_stat_activity WHERE application_name = 'k3-late';")
                    if r.stdout.strip() == '0':
                        gone = True
                        break
                    time.sleep(0.05)
                time.sleep(0.3)
                text = server_log_from(log_path, offset)
                tiny = [m for m in LOG_RE.findall(text) if m[1] == '7' and m[2] == '5']
                check('late:interrupt_followed_before_digest_no_LOG',
                      reached and gone and tiny == [] and 'connection to client lost' in text,
                      (reached, gone, tiny, text[-800:]))
        finally:
            daemon.send_signal(signal.SIGTERM)
            daemon.wait()
    alive = sql('SELECT 1;').stdout.strip() == '1'
    check('server_alive', alive)
    print(f'{"FAIL" if failures else "PASS"} k3 digest integration: {len(failures)} failed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
