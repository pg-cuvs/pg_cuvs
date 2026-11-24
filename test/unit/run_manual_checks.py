#!/usr/bin/env python3
"""Run the manual Linux source and mutation checks with a prepared SDK source tree."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
UNIT = ROOT / "test" / "unit"
POSITIONAL_OUTPUT = {
    "check_hnsw_ace_receipt_source", "check_hnsw_auto_overflow_source",
    "check_hnsw_explicit_source", "test_admission_contract_mutations", "test_ar8_mutations",
    "test_hnsw_auto_overflow_mutations", "test_hnsw_explicit_mutations", "test_i3e_mutations",
    "test_lw_mutations", "test_notice_mutations", "test_res_pool_mutations",
}
SDK_NEIGHBORS = {"check_hnsw_nn_sdk_source", "test_hnsw_nn_upper_precision_mutations"}

def arguments(name, output, sdk, canonical):
    target = str(output / name)
    if name in POSITIONAL_OUTPUT:
        return [target]
    if name in SDK_NEIGHBORS:
        return ["--sdk-src", str(sdk / "neighbors")]
    if name == "test_hnsw_sdk_plan_mutations":
        return ["--canonical-header", str(canonical), "--row-sort-src", str(sdk)]
    if name == "test_connectivity_pg_interrupt_mutations":
        return ["--output", target]
    if name == "test_connectivity_pg_workspace_mutations":
        return ["--tmpfs", "/dev/shm", "--output", target]
    if name == "check_i3_logs":
        incomplete = output / "incomplete.log"
        incomplete.write_text("No build records in this fixture.\n")
        return [str(incomplete)]
    return []

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-source", required=True, type=Path)
    parser.add_argument("--canonical-header", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--only", nargs="+")
    args = parser.parse_args()
    sdk = args.sdk_source.resolve()
    canonical = args.canonical_header.resolve()
    if not (sdk / "neighbors").is_dir() or not canonical.is_file():
        parser.error("expected patched cpp/src and a level-0005 cagra_build.cuh")
    checks = sorted({*UNIT.glob("check_*.py"), *UNIT.glob("test_*_mutations.py")})
    if args.only:
        wanted = set(args.only)
        checks = [check for check in checks if check.stem in wanted]
        if len(checks) != len(wanted):
            parser.error("unknown check in --only")
    if args.output:
        args.output.resolve().mkdir()
        return run(checks, args.output.resolve(), sdk, canonical)
    with tempfile.TemporaryDirectory(prefix="pgcuvs-manual-checks-") as temp:
        return run(checks, Path(temp), sdk, canonical)

def run(checks, output, sdk, canonical):
    failures = 0
    with (output / "summary.tsv").open("w") as summary:
        for check in checks:
            name = check.stem
            cmd = [sys.executable, "-B", str(check), *arguments(name, output, sdk, canonical)]
            completed = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            log = output / (name + ".log")
            log.write_bytes(completed.stdout)
            ok = completed.returncode == 0
            if name == "check_i3_logs":
                # A synthetic incomplete log checks the parser's failure contract.
                # It supplies no runtime coverage for any admission family.
                try:
                    result = json.loads(completed.stdout)
                except ValueError:
                    result = {}
                ok = completed.returncode == 1 and result.get("status") == "INCOMPLETE"
            failures += not ok
            print(f"{name}\t{completed.returncode}\t{'PASS' if ok else 'FAIL'}", file=summary, flush=True)
            print(f"{name}: {'PASS' if ok else 'FAIL'} (exit {completed.returncode})", flush=True)
            if not ok:
                print(completed.stdout.decode(errors="replace")[-4000:], flush=True)
    print(f"manual checks: {len(checks) - failures}/{len(checks)} passed", flush=True)
    return bool(failures)

if __name__ == "__main__":
    sys.exit(main())
