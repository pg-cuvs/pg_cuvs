#!/usr/bin/env python3
"""Real PG interrupt/cleanup regression, Linux CPU only; no CUDA or DB driver.

Requires PostgreSQL server/development binaries, a C compiler and psql. Starts
an isolated trust cluster with a private Unix socket, never an existing DB.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import pwd
import queue
import shutil
import subprocess
import tempfile
import threading
import time


class Session:
    def __init__(self, command):
        self.p = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, bufsize=1)
        self.lines = queue.Queue()
        self.errors = []
        self.error_changed = threading.Condition()
        self.ready = threading.Event()
        self.seq = 0

        def stdout():
            for line in self.p.stdout:
                self.lines.put(line.rstrip())
            self.lines.put(None)

        def stderr():
            for line in self.p.stderr:
                with self.error_changed:
                    self.errors.append(line.rstrip())
                    self.error_changed.notify_all()
                if "CONN203_WORKSPACE_READY" in line:
                    self.ready.set()

        threading.Thread(target=stdout, daemon=True).start()
        threading.Thread(target=stderr, daemon=True).start()

    def wait_error(self, fragment, timeout=2):
        # stdout and stderr have separate reader threads. Do not turn a reader
        # scheduling race into an absent SQLSTATE or relax the SQLSTATE check.
        with self.error_changed:
            return self.error_changed.wait_for(
                lambda: any(fragment in line for line in self.errors), timeout)

    def ask(self, sql, timeout=15):
        self.seq += 1
        marker = f"CONN203_END_{self.seq}"
        self.p.stdin.write(sql + "\n\\echo " + marker + "\n")
        self.p.stdin.flush()
        out = []
        deadline = time.monotonic() + timeout
        while True:
            line = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            if line is None:
                raise EOFError("backend disconnected")
            if line == marker:
                return out
            out.append(line)

    def close(self):
        if self.p.poll() is None:
            try:
                self.p.stdin.write("\\q\n")
                self.p.stdin.flush()
                self.p.wait(timeout=3)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                self.p.kill()
                self.p.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--coverage", action="store_true")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    source = (args.source_dir or repo / "src").resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    temp = Path(tempfile.mkdtemp(prefix="conn203-pg-interrupt-"))
    temp.chmod(0o755)
    pg = Path(subprocess.check_output(["pg_config", "--bindir"], text=True).strip())
    include = subprocess.check_output(["pg_config", "--includedir-server"], text=True).strip()
    owner = []
    if os.geteuid() == 0:
        account = pwd.getpwnam("postgres")
        os.chown(temp, account.pw_uid, account.pw_gid)
        owner = ["runuser", "-u", "postgres", "--"]
    result = {"source": str(source), "cases": [], "platform": dict(zip(
        ["system", "node", "release", "version", "machine"], os.uname()))}
    inputs = [source / "hnsw_export.c", repo / "test/unit/connectivity_pg_probe.c", Path(__file__)]
    inputs += sorted((repo / "src").glob("*.h"))
    result["inputs_sha256"] = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    result["pg_version"] = subprocess.check_output(["pg_config", "--version"], text=True).strip()
    started = False
    log = temp / "postgres.log"
    try:
        exports = temp / "probe.exports"
        exports.write_text("{global: Pg_magic_func; conn203_probe; pg_finfo_conn203_probe; "
                           "conn203_probe_state; pg_finfo_conn203_probe_state; local: *;};\n")
        command = [os.environ.get("CC", "cc"), "-std=gnu11", "-O2", "-g", "-fPIC", "-shared",
                   "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                   "-Wl,--version-script=" + str(exports), "-I" + include,
                   "-I" + str(source), "-I" + str(repo / "src"),
                   str(repo / "test/unit/connectivity_pg_probe.c"), "-o", str(temp / "probe.so")]
        result["compile_command"] = command
        if args.coverage:
            command.insert(1, "--coverage")
        with (output / "compile.log").open("w") as f:
            subprocess.run(command, stdout=f, stderr=subprocess.STDOUT, check=True)
        shutil.copy2(temp / "probe.so", output / "probe.so")
        with (output / "initdb.log").open("w") as f:
            subprocess.run(owner + [str(pg / "initdb"), "-D", str(temp / "data"),
                                   "-U", "conn203_test", "-A", "trust", "--no-locale"],
                           stdout=f, stderr=subprocess.STDOUT, check=True)
        with (temp / "data/postgresql.conf").open("a") as f:
            f.write(f"\nlisten_addresses=''\nport=6543\nunix_socket_directories='{temp}'\n")
        subprocess.run(owner + [str(pg / "pg_ctl"), "-D", str(temp / "data"),
                                "-l", str(log), "start"], capture_output=True, check=True)
        started = True
        psql = [str(pg / "psql"), "-XqAt", "-v", "VERBOSITY=verbose", "-h", str(temp),
                "-p", "6543", "-U", "conn203_test", "-d", "postgres"]
        setup = Session(psql)
        setup.ask(f"CREATE FUNCTION conn203_probe(boolean,integer) RETURNS text AS "
                  f"'{temp}/probe.so','conn203_probe' LANGUAGE C STRICT;\n"
                  f"CREATE FUNCTION conn203_probe_state() RETURNS text AS "
                  f"'{temp}/probe.so','conn203_probe_state' LANGUAGE C;")
        setup.close()
        for file in [False, True]:
            for spill in [False, True]:
                for event in ["normal", "generic_pending", "log_memory", "cancel", "timeout", "terminate"] + (["loop_cancel"] if file else []):
                    c = Session(psql)
                    row = {"adapter": "file" if file else "elements", "spilled": spill, "event": event}
                    try:
                        pid = int(c.ask("SELECT pg_backend_pid();")[0])
                        row["pid"] = pid
                        c.ask("SET maintenance_work_mem='" + ("1MB" if spill else "64MB") + "';")
                        if event == "timeout":
                            c.ask("SET statement_timeout='1500ms';")
                        action = {"normal": 0, "generic_pending": 1, "log_memory": 2,
                                  "cancel": 3, "timeout": 4, "terminate": 5, "loop_cancel": 6}[event]
                        outcome = {}

                        def run():
                            try:
                                outcome["lines"] = c.ask(f"SELECT conn203_probe({'true' if file else 'false'},{action});")
                            except Exception as e:
                                outcome["client_error"] = type(e).__name__

                        t = threading.Thread(target=run, daemon=True)
                        t.start()
                        if action >= 2:
                            assert c.ready.wait(6), "no post-allocation synchronization notice"
                            phase = "offsets" if event == "loop_cancel" else "kernel"
                            marker = f"CONN203_PROBE_WAIT pid={pid} phase={phase}"
                            deadline = time.monotonic() + 3
                            while marker not in log.read_text() and time.monotonic() < deadline:
                                time.sleep(0.001)
                            assert marker in log.read_text(), "not at requested production phase"
                            row["ready_notice"] = [x for x in c.errors if "CONN203_WORKSPACE_READY" in x]
                            if event in ["log_memory", "cancel", "terminate", "loop_cancel"]:
                                control = Session(psql)
                                function = {"log_memory": "pg_log_backend_memory_contexts",
                                            "cancel": "pg_cancel_backend", "terminate": "pg_terminate_backend", "loop_cancel": "pg_cancel_backend"}[event]
                                assert control.ask(f"SELECT {function}({pid});") == ["t"]
                                control.close()
                        t.join(10)
                        assert not t.is_alive(), "query did not finish"
                        row["stderr"] = list(c.errors)
                        if event == "terminate":
                            deadline = time.monotonic() + 3
                            while Path(f"/proc/{pid}").exists() and time.monotonic() < deadline:
                                time.sleep(0.02)
                            assert not Path(f"/proc/{pid}").exists(), "terminated backend remains"
                            assert c.wait_error("57P01"), "termination SQLSTATE not propagated"
                            assert f"CONN203_CALLBACK_INTERRUPT pid={pid}\n" in log.read_text(), "termination deferred past kernel"
                            assert f"CONN203_KERNEL_RETURNED pid={pid}\n" not in log.read_text(), "kernel completed before termination"
                            row["process_gone"] = True
                        else:
                            if event in ["cancel", "timeout", "loop_cancel"]:
                                assert c.wait_error("57014"), "cancel SQLSTATE not propagated"
                            else:
                                assert not outcome.get("client_error"), outcome
                                value = json.loads(outcome["lines"][0])
                                row["value"] = value
                                snap = value["connectivity"]
                                assert snap["status"] == "measured", value
                                assert snap["unreachable"] == 0 and snap["wcc_all"] == 1, value
                                assert snap["spilled"] == spill, value
                            state = json.loads(c.ask("SELECT conn203_probe_state();")[0])
                            row["after"] = state
                            assert state["heap_live"] == 0 and state["map_live"] == 0, state
                            if event in ["cancel", "timeout"]:
                                assert not state["kernel_returned"] and state["callback_cancel_seen"], state
                            elif event == "loop_cancel":
                                assert not state["kernel_returned"] and state["file_cfi_polls"] >= 3, state
                            elif event == "log_memory":
                                assert state["kernel_returned"] and not state["log_pending_at_return"], state
                            assert c.ask("SELECT 1;") == ["1"], "backend not reusable"
                        assert not list((temp / "data/base/pgsql_tmp").glob("*conn203-*")), "scratch filename leaked"
                        row["verdict"] = "PASS"
                    except Exception as e:
                        row.update(verdict="FAIL", error=str(e), error_type=type(e).__name__, stderr=list(c.errors))
                    finally:
                        c.close()
                        row["stderr"] = list(c.errors)
                        result["cases"].append(row)
                        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        result["passed"] = sum(x["verdict"] == "PASS" for x in result["cases"])
        result["failed"] = len(result["cases"]) - result["passed"]
        return 0 if result["failed"] == 0 else 1
    except Exception as e:
        result.update(harness_error=type(e).__name__, error=str(e))
        return 2
    finally:
        if started:
            subprocess.run(owner + [str(pg / "pg_ctl"), "-D", str(temp / "data"), "-m", "immediate", "stop"],
                           capture_output=True)
        if log.exists():
            shutil.copy2(log, output / "postgres.log")
        if args.coverage:
            coverage = output / "coverage"
            coverage.mkdir(exist_ok=True)
            for pattern in ["*.gcno", "*.gcda"]:
                for p in temp.glob(pattern):
                    shutil.copy2(p, coverage / p.name)
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps({k: result[k] for k in ["passed", "failed", "harness_error", "error"] if k in result}))
        shutil.rmtree(temp)


if __name__ == "__main__":
    raise SystemExit(main())
