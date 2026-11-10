"""amvacuumcleanup policy mutations: each must turn a check RED.

Header mutants run test_vacuum_policy.c. pg_cuvs.c needs the PostgreSQL
server headers, so its wiring is checked at source level: the handler must
ask the policy with info->analyze_only and return before the tombstone
compaction, and the VACUUM compaction (stale check, tombstone stat, COMPACT)
must still follow. Wiring mutants are applied to the source text and must
fail those checks.
"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = ROOT / "src" / "cuvs_vacuum_policy.h"
TEST = ROOT / "test" / "unit" / "test_vacuum_policy.c"
SOURCE = ROOT / "src" / "pg_cuvs.c"
CC = os.environ.get("CC", "cc")

HEADER_MUTANTS = {
    "analyze_only_ignored": ("return !analyze_only && daemon_configured;", "return daemon_configured;"),
    "always_considered": ("return !analyze_only && daemon_configured;", "return true;"),
    "never_considered": ("return !analyze_only && daemon_configured;", "return false;"),
    "daemon_ignored": ("return !analyze_only && daemon_configured;", "return !analyze_only;"),
}

FN = re.compile(r"cuvs_amvacuumcleanup\(IndexVacuumInfo \*info, IndexBulkDeleteResult \*stats\)\n\{(.*?)\n\}\n", re.S)


def wiring(src: str) -> dict:
    m = FN.search(src)
    body = m.group(1) if m else ""
    gate = re.search(r"if \(!cuvs_vacuum_cleanup_considers_compact\(info->analyze_only,\s*"
                     r"cuvs_socket_path && cuvs_socket_path\[0\] != '\\0'\)\)\s*return stats;", body)
    compact = body.find("cuvs_ipc_compact(")
    return {
        "handler_found": m is not None,
        "gate_on_analyze_only": gate is not None,
        "gate_before_compaction": gate is not None and compact > gate.start(),
        "vacuum_path_kept": bool(re.search(r"if \(!cuvs_index_is_stale\(index_oid\)\)\s*\{\s*"
                                           r"cuvs_tombstone_path\(index_oid, tpath, sizeof\(tpath\)\);\s*"
                                           r"if \(stat\(tpath, &st\) == 0\)\s*\{\s*\(void\) cuvs_ipc_compact\(", body)),
        "header_included": '#include "cuvs_vacuum_policy.h"' in src,
    }


WIRING_MUTANTS = {
    "gate_removed": ("    if (!cuvs_vacuum_cleanup_considers_compact(info->analyze_only,\n"
                     "                                               cuvs_socket_path && cuvs_socket_path[0] != '\\0'))\n"
                     "        return stats;\n", ""),
    "analyze_only_not_passed": ("cuvs_vacuum_cleanup_considers_compact(info->analyze_only,",
                                "cuvs_vacuum_cleanup_considers_compact(false,"),
    "vacuum_compaction_dropped": ("        if (stat(tpath, &st) == 0)\n        {\n            (void) cuvs_ipc_compact(",
                                  "        if (stat(tpath, &st) == 0 && false)\n        {\n            (void) cuvs_ipc_compact("),
}


def run_test(header_text: str, work: pathlib.Path) -> int:
    inc = work / "inc"
    inc.mkdir(exist_ok=True)
    (inc / "cuvs_vacuum_policy.h").write_text(header_text)
    exe = work / "t"
    b = subprocess.run([CC, "-std=gnu11", "-Wall", "-Wextra", "-I", str(inc), "-o", str(exe), str(TEST)],
                       capture_output=True, text=True)
    if b.returncode != 0:
        return 2
    return subprocess.run([str(exe)], capture_output=True, text=True).returncode


def main() -> int:
    fails = 0
    header = HEADER.read_text()
    src = SOURCE.read_text()
    with tempfile.TemporaryDirectory() as d:
        work = pathlib.Path(d)
        if run_test(header, work) != 0:
            print("BASELINE FAIL: unmutated test does not pass")
            return 1
        for name, (old, new) in HEADER_MUTANTS.items():
            if header.count(old) != 1:
                print(f"ANCHOR {name}"); fails += 1; continue
            rc = run_test(header.replace(old, new), work)
            ok = rc not in (0, 2)
            print(f"{'RED ' if ok else 'SURVIVED'} {name} (rc={rc})")
            fails += 0 if ok else 1
    base = wiring(src)
    for name, ok in base.items():
        print(f"{'WIRED' if ok else 'MISSING'} {name}")
        fails += 0 if ok else 1
    for name, (old, new) in WIRING_MUTANTS.items():
        if src.count(old) != 1:
            print(f"ANCHOR {name}"); fails += 1; continue
        broken = not all(wiring(src.replace(old, new)).values())
        print(f"{'RED ' if broken else 'SURVIVED'} {name}")
        fails += 0 if broken else 1
    total = len(HEADER_MUTANTS) + len(base) + len(WIRING_MUTANTS)
    print(f"vacuum policy mutations: {total - fails}/{total}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
