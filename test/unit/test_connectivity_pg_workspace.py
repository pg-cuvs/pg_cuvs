#!/usr/bin/env python3
"""Linux PG tmpfs reservation signal regression. Needs >=1.25GiB free tmpfs.

Example Docker option: --tmpfs /pgtmp:size=1600m,mode=1777
No existing cluster or GPU is used. Source-dir supports isolated RED copies.
"""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import pwd
import shutil
import signal
import subprocess
import tempfile
import threading
import time
from test_connectivity_pg_interrupts import Session


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source-dir", type=Path)
    ap.add_argument("--tmpfs", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--rounds", type=int, default=6)
    args = ap.parse_args()
    repo = Path(__file__).resolve().parents[2]
    source = (args.source_dir or repo / "src").resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    temp = Path(tempfile.mkdtemp(prefix="conn203-reserve-"))
    temp.chmod(0o755)
    owner = []
    if os.geteuid() == 0:
        p = pwd.getpwnam("postgres")
        os.chown(temp, p.pw_uid, p.pw_gid)
        owner = ["runuser", "-u", "postgres", "--"]
    pg = Path(subprocess.check_output(["pg_config", "--bindir"], text=True).strip())
    inc = subprocess.check_output(["pg_config", "--includedir-server"], text=True).strip()
    result = {"cases": [], "source_sha256": hashlib.sha256((source / "hnsw_export.c").read_bytes()).hexdigest()}
    started = False
    scratch = None
    log = temp / "server.log"
    try:
        fs = subprocess.check_output(["stat", "-f", "-c", "%T", str(args.tmpfs)], text=True).strip()
        assert fs == "tmpfs", "tmpfs precondition, got " + fs
        assert shutil.disk_usage(args.tmpfs).free >= (5 << 28), "tmpfs has <1.25GiB free"
        exp = temp / "exports"
        exp.write_text("{global: Pg_magic_func;conn203_workspace_probe;pg_finfo_conn203_workspace_probe;local:*;};")
        with (args.output / "compile.log").open("w") as f:
            subprocess.run(["cc", "-std=gnu11", "-O2", "-shared", "-fPIC", "-ffunction-sections",
                            "-fdata-sections", "-Wl,--gc-sections", "-Wl,--version-script=" + str(exp),
                            "-I" + inc, "-I" + str(source), "-I" + str(repo / "src"),
                            str(repo / "test/unit/connectivity_pg_workspace_probe.c"),
                            "-o", str(temp / "probe.so")], stdout=f, stderr=subprocess.STDOUT, check=True)
        subprocess.run(owner + [str(pg / "initdb"), "-D", str(temp / "data"), "-U", "w", "-A", "trust", "--no-locale"], capture_output=True, check=True)
        with (temp / "data/postgresql.conf").open("a") as f:
            f.write(f"\nlisten_addresses=''\nport=6545\nunix_socket_directories='{temp}'\n")
        subprocess.run(owner + [str(pg / "pg_ctl"), "-D", str(temp / "data"), "-l", str(log), "start"], capture_output=True, check=True)
        started = True
        scratch = Path(tempfile.mkdtemp(prefix="conn203-reserve-", dir=args.tmpfs))
        scratch.chmod(0o777)
        (temp / "data/base/pgsql_tmp").symlink_to(scratch)
        psql = [str(pg / "psql"), "-XqAt", "-v", "VERBOSITY=verbose", "-h", str(temp), "-p", "6545", "-U", "w", "-d", "postgres"]
        ctl = Session(psql)
        ctl.ask(f"CREATE FUNCTION workspace(bigint,boolean) RETURNS text AS '{temp}/probe.so','conn203_workspace_probe' LANGUAGE C;")
        for event in ["none", "sigusr1_storm", "log_memory_once", "retry_once", "enospc"]:
            for cycle in range(args.rounds if event not in ["retry_once", "enospc"] else 1):
                c = Session(psql)
                row = {"event": event, "cycle": cycle}
                stop = threading.Event()
                try:
                    pid = int(c.ask("SELECT pg_backend_pid();")[0])
                    c.ask("SET maintenance_work_mem='1MB'; SET temp_file_limit='-1';")
                    outcome = {}
                    size = (os.statvfs(args.tmpfs).f_blocks * os.statvfs(args.tmpfs).f_frsize + 4096
                            if event == "enospc" else 1 << 30)

                    def run():
                        try:
                            outcome["lines"] = c.ask(f"SELECT workspace({size},{'true' if event=='retry_once' else 'false'});", timeout=12)
                        except Exception as e:
                            outcome["error"] = type(e).__name__

                    start = time.monotonic()
                    t = threading.Thread(target=run, daemon=True)
                    t.start()
                    marker = f"CONN203_FALLOC_BEGIN pid={pid}"
                    deadline = time.monotonic() + 5
                    while marker not in log.read_text() and time.monotonic() < deadline:
                        time.sleep(0.001)
                    assert marker in log.read_text(), "reservation not entered"
                    sent = []

                    def storm():
                        while not stop.is_set():
                            try:
                                os.kill(pid, signal.SIGUSR1)
                                sent.append(1)
                            except ProcessLookupError:
                                break
                            time.sleep(0.0002)

                    sender = None
                    if event == "sigusr1_storm":
                        sender = threading.Thread(target=storm, daemon=True)
                        sender.start()
                    elif event == "log_memory_once":
                        assert ctl.ask(f"SELECT pg_log_backend_memory_contexts({pid});") == ["t"]
                    t.join(3)
                    bounded = not t.is_alive()
                    stop.set()
                    if sender:
                        sender.join(2)
                    t.join(6)  # let a broken unmasked retry finish after storm stops
                    row.update(seconds=time.monotonic() - start, completed_while_storm=bounded,
                               signals_sent=len(sent), stderr=list(c.errors))
                    assert not t.is_alive(), "reservation did not finish after signals stopped"
                    assert not outcome.get("error"), outcome
                    value = json.loads(outcome["lines"][0])
                    row["value"] = value
                    if event == "enospc":
                        assert not value["allocated"] and value["last_rc"] == errno.ENOSPC, value
                    else:
                        assert value["allocated"] and value["spilled"] and value["last_rc"] == 0, value
                    if event == "retry_once":
                        assert value["calls"] == 2, value
                    assert value["all_sigusr1_masked"] and value["mask_restored"], value
                    assert bounded, "unmasked EINTR retry starved under continuous signal"
                    assert not value["log_memory_pending"], value
                    if event == "sigusr1_storm":
                        assert len(sent) > 0, "no signal delivered"
                    assert not list(scratch.iterdir()), "reservation file leaked"
                    assert c.ask("SELECT 1;") == ["1"]
                    row["verdict"] = "PASS"
                except Exception as e:
                    row.update(verdict="FAIL", error_type=type(e).__name__, error=str(e))
                finally:
                    stop.set()
                    c.close()
                    result["cases"].append(row)
        ctl.close()
        result["passed"] = sum(r["verdict"] == "PASS" for r in result["cases"])
        result["failed"] = len(result["cases"]) - result["passed"]
        return 1 if result["failed"] else 0
    except Exception as e:
        result.update(harness_error=type(e).__name__, error=str(e))
        return 2
    finally:
        if started:
            subprocess.run(owner + [str(pg / "pg_ctl"), "-D", str(temp / "data"), "-m", "immediate", "stop"], capture_output=True)
        if log.exists():
            shutil.copy2(log, args.output / "server.log")
        (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps({k: result[k] for k in ["passed", "failed", "harness_error", "error"] if k in result}))
        shutil.rmtree(temp)
        if scratch:
            shutil.rmtree(scratch)


if __name__ == "__main__":
    raise SystemExit(main())
