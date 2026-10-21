"""Local subprocess/hostname boundary used by the live follow-up runner."""
from pathlib import Path
import subprocess
import time


def probe_hostname(argv, expected, stdout_path, stderr_path, timeout=12):
    started = time.monotonic()
    timed_out = False
    with Path(stdout_path).open('wb') as stdout, Path(stderr_path).open('wb') as stderr:
        try:
            result = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=stdout,
                                    stderr=stderr, timeout=timeout, check=False)
            returncode = result.returncode
        except subprocess.TimeoutExpired:
            timed_out = True
            returncode = None
    out = Path(stdout_path).read_text(errors='replace')
    err = Path(stderr_path).read_text(errors='replace')
    if timed_out:
        status, error = 'ssh_timeout', f'SSH timed out after {timeout}s: {err.strip()}'
    elif returncode != 0:
        status, error = 'ssh_failed', f'SSH exited {returncode}: {err.strip()}'
    elif out.strip() != expected:
        status, error = 'wrong_host', f'Expected {expected!r}, received stdout {out!r}'
    else:
        status, error = 'ready', None
    return {'status': status, 'returncode': returncode, 'stdout': out, 'stderr': err,
            'error': error, 'seconds': time.monotonic()-started}


def accept_probe(probe):
    if probe['status'] == 'wrong_host':
        raise RuntimeError(probe['error'])
    return probe['status'] == 'ready'
