"""#224 source check: the daemon's listen() backlog holds a batch of reconnects.

Every backend request opens its own connection, and one micro-batch releases
up to CUVS_BATCH_MAX waiters at once. A backlog of 32 made their reconnects
wait in connect(), where a signal turned the wait into a spurious
"daemon unavailable". The daemon socket must therefore be listened with the
named constant PG_CUVS_LISTEN_BACKLOG, and that constant must be at least
CUVS_BATCH_MAX. This checks the source constant only: at run time the kernel
caps the backlog at net.core.somaxconn, and the daemon logs a WARN at startup
when that cap is below CUVS_BATCH_MAX. (The client's EINTR retry is tested by
test_ipc_connect_eintr.c.)

Usage: check_listen_backlog_source.py [path/to/pg_cuvs_server.c]
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def mask(s):
    # Blank comments and string literals, keeping offsets and newlines.
    return re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                  lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), s, flags=re.S)


def define(code, name):
    m = re.findall(r'^\s*#\s*define\s+' + name + r'\s+(\d+)\s*$', code, flags=re.M)
    return int(m[0]) if len(m) == 1 else None


def check(text):
    code = mask(text)
    errors = []
    calls = re.findall(r'\blisten\s*\(\s*g_server_fd\s*,\s*([^)]*?)\s*\)', code)
    if len(calls) != 1:
        errors.append(f'expected one listen(g_server_fd, ...) call, found {len(calls)}')
    elif calls[0] != 'PG_CUVS_LISTEN_BACKLOG':
        errors.append(f'listen(g_server_fd, {calls[0]}) must use PG_CUVS_LISTEN_BACKLOG')
    backlog = define(code, 'PG_CUVS_LISTEN_BACKLOG')
    batch = define(code, 'CUVS_BATCH_MAX')
    if backlog is None or batch is None:
        errors.append('PG_CUVS_LISTEN_BACKLOG and CUVS_BATCH_MAX must each be one integer #define')
    elif backlog < batch:
        errors.append(f'PG_CUVS_LISTEN_BACKLOG {backlog} < CUVS_BATCH_MAX {batch}: '
                      'one batch of reconnects would overflow the backlog')
    return errors


def main(argv):
    src = pathlib.Path(argv[1]) if len(argv) > 1 else ROOT / 'src' / 'pg_cuvs_server.c'
    errors = check(src.read_text())
    for e in errors:
        print(f'FAIL {src.name}: {e}')
    if errors:
        return 1
    print('check_listen_backlog_source: ok')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
