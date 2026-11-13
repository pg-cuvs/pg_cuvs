#!/usr/bin/env python3
"""RED controls for missing signal mask and missing EINTR retry (tmpfs)."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--tmpfs", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    a = p.parse_args()
    repo = Path(__file__).resolve().parents[2]
    s = (repo / "src/hnsw_export.c").read_text()
    block = "    if(IsUnderPostmaster)\n        sigprocmask(SIG_SETMASK,&BlockSig,&save_sigmask);\n"
    restore = "    if(IsUnderPostmaster) {\n        save_errno=errno;\n        sigprocmask(SIG_SETMASK,&save_sigmask,NULL);\n        errno=save_errno;\n    }\n"
    loop = "do { rc=posix_fallocate(fd,0,(off_t)bytes); } while(rc==EINTR);"
    assert s.count(block) == 1 and s.count(restore) == 1 and s.count(loop) == 1
    mutants = {"unmasked-EINTR-retry": s.replace(block, "").replace(restore, ""),
               "blocked-no-retry": s.replace(loop, "rc=posix_fallocate(fd,0,(off_t)bytes);")}
    a.output.mkdir(parents=True, exist_ok=True)
    rows = []
    for name, changed in mutants.items():
        out = a.output / name
        with tempfile.TemporaryDirectory(prefix="conn203-reserve-red-") as d:
            Path(d, "hnsw_export.c").write_text(changed)
            r = subprocess.run([sys.executable, str(repo / "test/unit/test_connectivity_pg_workspace.py"),
                                "--source-dir", d, "--tmpfs", str(a.tmpfs), "--rounds", "1",
                                "--output", str(out)], capture_output=True, text=True, timeout=45)
            x = json.loads((out / "result.json").read_text())
            failed = [c for c in x["cases"] if c["verdict"] == "FAIL"]
            if name == "unmasked-EINTR-retry":
                storm = next(c for c in x["cases"] if c["event"] == "sigusr1_storm")
                detected = r.returncode == 1 and len(x["cases"]) == 5 and all(
                    not c.get("value", {}).get("all_sigusr1_masked", True) for c in x["cases"])
                extra = {"completed_during_storm": storm.get("completed_while_storm")}
            else:
                detected = r.returncode == 1 and len(x["cases"]) == 5 and len(failed) == 1 and failed[0]["event"] == "retry_once"
                extra = {}
            rows.append({"mutant": name, "detected": detected, "failed": x.get("failed"),
                         "exit": r.returncode, **extra})
    (a.output / "MUTATIONS.json").write_text(json.dumps(rows, indent=2) + "\n")
    print(json.dumps(rows))
    return 0 if all(x["detected"] for x in rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())
