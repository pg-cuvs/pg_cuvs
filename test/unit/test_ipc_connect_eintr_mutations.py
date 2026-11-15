"""#224 mutations: each must turn a check RED.

Client mutants are applied to a copy of src/cuvs_ipc.c and run
test_ipc_connect_eintr.c (Linux only: on macOS a full backlog refuses at
once, so the test cannot wait in connect()). Backlog mutants are applied to
src/pg_cuvs_server.c and must fail check_listen_backlog_source.py.
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
IPC = SRC / "cuvs_ipc.c"
SERVER = SRC / "pg_cuvs_server.c"
TEST = ROOT / "test" / "unit" / "test_ipc_connect_eintr.c"
CHECK = ROOT / "test" / "unit" / "check_listen_backlog_source.py"
CC = os.environ.get("CC", "cc")
RT = os.environ.get("TEST_RT_LIB", "").split()
# The unmutated test runs in about 3.5 s; a mutant still running after
# TIMEOUT_S has lost a cancel or the budget and counts as RED (exit 124).
TIMEOUT_S = 60
SCENARIOS = "ABCDE"

CLIENT_MUTANTS = {
    # The code before #224: any failed connect() ends the wait.
    "no_retry": [("            if (err != EINTR && err != EAGAIN)\n",
                  "            if (1)\n")],
    # Only EINTR retries: an expired slice (EAGAIN) ends a wait that has budget left.
    "no_eagain_retry": [("            if (err != EINTR && err != EAGAIN)\n",
                         "            if (err != EINTR)\n")],
    # One wait of the whole remaining budget instead of 250 ms slices.
    "no_slice": [("slice_us = left_us < CUVS_CONNECT_SLICE_US ? left_us : CUVS_CONNECT_SLICE_US;",
                  "slice_us = left_us;")],
    # Retry even when a cancel is pending.
    "cancel_ignored": [("            if (g_wait_cb && g_wait_cb())\n            {\n"
                        "                if (canceled)\n",
                        "            if (0)\n            {\n"
                        "                if (canceled)\n")],
    # Look at the cancel only after EINTR: a cancel whose signal came before
    # connect() is missed.
    "cancel_checked_only_on_eintr": [("            if (g_wait_cb && g_wait_cb())\n            {\n"
                                      "                if (canceled)\n",
                                      "            if (err == EINTR && g_wait_cb && g_wait_cb())\n            {\n"
                                      "                if (canceled)\n")],
    # The cancel ends the connect but is reported as a daemon failure.
    "cancel_reported_unavailable": [("rc = canceled ? CUVS_STATUS_CANCELED",
                                     "rc = 0 ? CUVS_STATUS_CANCELED")],
    # Elapsed time is never counted: no overall budget.
    "no_budget": [("            clock_gettime(CLOCK_MONOTONIC, &now);\n            left_us",
                   "            now = start;\n            left_us")],
    # The connection keeps the last slice as its send timeout.
    "no_sndtimeo_restore": [("        /* The request itself gets the full send timeout, not the last slice. */\n"
                             "        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));\n",
                             "")],
}

SERVER_MUTANTS = {
    "backlog_32": [("#define PG_CUVS_LISTEN_BACKLOG 1024", "#define PG_CUVS_LISTEN_BACKLOG 32")],
    "literal_backlog": [("listen(g_server_fd, PG_CUVS_LISTEN_BACKLOG)", "listen(g_server_fd, 32)")],
}


def apply(text, edits):
    for old, new in edits:
        if text.count(old) != 1:
            return None
        text = text.replace(old, new)
    return text


def run_client(ipc_text, work):
    src = work / "src"
    if src.exists():
        shutil.rmtree(src)
    shutil.copytree(SRC, src, ignore=shutil.ignore_patterns("*.o", "*.bc", "*.so"))
    (src / "cuvs_ipc.c").write_text(ipc_text)
    exe = work / "t"
    b = subprocess.run([CC, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(src), "-o", str(exe),
                        str(TEST), str(src / "cuvs_ipc.c"), str(src / "cuvs_util.c"),
                        str(src / "cuvs_build_corpus.c"), "-lpthread", "-lm", *RT],
                       capture_output=True, text=True)
    if b.returncode != 0:
        sys.stderr.write(b.stderr)
        return 2
    try:
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired as e:
        # A mutant that loses the cancel or the budget keeps the client in
        # connect(): name the scenario that never returned.
        err = e.stderr or b""
        if isinstance(err, bytes):
            err = err.decode(errors="replace")
        done = [ln[0] for ln in err.splitlines() if len(ln) > 1 and ln[1] == ":" and ln[0] in SCENARIOS]
        for line in err.splitlines():
            if line.startswith("FAIL "):
                print(f"    {line}")
        hung = next((c for c in SCENARIOS if c not in done), "?")
        print(f"    TIMEOUT after {TIMEOUT_S} s: scenario {hung} did not return "
              f"(finished: {', '.join(done) or 'none'}); the call kept waiting in connect()")
        return 124
    for line in r.stderr.splitlines():
        if line.startswith("FAIL "):
            print(f"    {line}")
    return r.returncode


def run_check(server_text, work):
    p = work / "pg_cuvs_server.c"
    p.write_text(server_text)
    return subprocess.run([sys.executable, "-I", "-B", str(CHECK), str(p)],
                          capture_output=True, text=True).returncode


def main():
    fails = 0
    total = 0
    with tempfile.TemporaryDirectory() as d:
        work = pathlib.Path(d)
        if sys.platform.startswith("linux"):
            ipc = IPC.read_text()
            if run_client(ipc, work) != 0:
                print("BASELINE FAIL: unmutated test_ipc_connect_eintr does not pass")
                return 1
            for name, edits in CLIENT_MUTANTS.items():
                total += 1
                text = apply(ipc, edits)
                if text is None:
                    print(f"ANCHOR {name}"); fails += 1; continue
                t0 = time.monotonic()
                rc = run_client(text, work)
                ok = rc not in (0, 2)
                print(f"{'RED ' if ok else 'SURVIVED'} {name} (rc={rc}, {time.monotonic() - t0:.1f} s)")
                fails += 0 if ok else 1
        else:
            print("SKIP client mutants: Linux only")
        server = SERVER.read_text()
        if run_check(server, work) != 0:
            print("BASELINE FAIL: unmutated check_listen_backlog_source does not pass")
            return 1
        for name, edits in SERVER_MUTANTS.items():
            total += 1
            text = apply(server, edits)
            if text is None:
                print(f"ANCHOR {name}"); fails += 1; continue
            ok = run_check(text, work) != 0
            print(f"{'RED ' if ok else 'SURVIVED'} {name}")
            fails += 0 if ok else 1
    print(f"ipc connect EINTR mutations: {total - fails}/{total}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
