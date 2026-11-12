#!/usr/bin/env python3
"""RED controls: benign-as-cancel and cleanup only on successful return."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    original = (repo / "src/hnsw_export.c").read_text()
    begin = original.index("static int hnsw_connectivity_cancel(void *arg)\n{")
    end = original.index("\n}", begin) + 2
    callback = original[begin:end]
    assert callback.count("CHECK_FOR_INTERRUPTS();") == 1
    bad_callback = callback.replace("CHECK_FOR_INTERRUPTS();\n    return 0;", "return InterruptPending;")
    assert bad_callback != callback
    mutants = {"benign-as-cancel": original[:begin] + bad_callback + original[end:]}
    pattern = re.compile(r"    PG_FINALLY\(\);\n    \{\n(        hnsw_connectivity_workspace_free\(workspace,bytes,spilled\);\n(?:        munmap\(.*\);\n)?)    \}\n    PG_END_TRY\(\);")
    assert len(list(pattern.finditer(original))) == 2
    mutants["cleanup-only-on-success"] = pattern.sub(
        lambda m: "    PG_CATCH();\n    {\n        PG_RE_THROW();\n    }\n    PG_END_TRY();\n" + m[1], original)
    args.output.mkdir(parents=True, exist_ok=True)
    rows = []
    for name, source in mutants.items():
        with tempfile.TemporaryDirectory(prefix="conn203-red-") as d:
            Path(d, "hnsw_export.c").write_text(source)
            out = (args.output / name).resolve()
            p = subprocess.run([sys.executable, str(repo / "test/unit/test_connectivity_pg_interrupts.py"),
                                "--source-dir", d, "--output", str(out)], capture_output=True, text=True)
            x = json.loads((out / "result.json").read_text())
            failed = [r for r in x["cases"] if r["verdict"] == "FAIL"]
            expected = {"generic_pending", "log_memory"} if name == "benign-as-cancel" else {"cancel", "timeout"}
            detected = p.returncode == 1 and len(x["cases"]) == 24 and len(failed) == 8 and {r["event"] for r in failed} == expected
            rows.append({"mutant": name, "exit": p.returncode, "detected": detected,
                         "passed": x.get("passed"), "failed": x.get("failed"),
                         "failed_cases": [{k: r[k] for k in ["adapter", "spilled", "event", "error"]} for r in failed]})
    (args.output / "MUTATIONS.json").write_text(json.dumps(rows, indent=2) + "\n")
    print(json.dumps([{"mutant": r["mutant"], "detected": r["detected"]} for r in rows]))
    return 0 if all(r["detected"] for r in rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())
