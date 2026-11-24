#!/usr/bin/env python3
"""Run the manual Linux source and mutation checks with a prepared SDK source tree."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
UNIT = ROOT / "test" / "unit"
UNIT_CHECKS = (
    "check_bf_verdict_source.py",
    "check_cagra_relcache_source.py",
    "check_circuit_source.py",
    "check_hnsw_ace_receipt_source.py",
    "check_hnsw_auto_overflow_source.py",
    "check_hnsw_explicit_source.py",
    "check_hnsw_nn_sdk_source.py",
    "check_i3_logs.py",
    "check_i3b_docs.py",
    "check_i3b_server_source.py",
    "check_i3b_sql_perms.py",
    "check_i3e_sql.py",
    "check_listen_backlog_source.py",
    "check_lw_docs.py",
    "mutate_cache_policy.py",
    "mutate_i3b_docs.sh",
    "mutate_i3b_server_source.py",
    "mutate_peak_meter.py",
    "mutate_status_text.py",
    "test_admission_contract_mutations.py",
    "test_ar8_mutations.py",
    "test_compact_retry_daemon_mutations.py",
    "test_connectivity_pg_interrupt_mutations.py",
    "test_connectivity_pg_workspace_mutations.py",
    "test_hnsw_ace_quality_mutations.py",
    "test_hnsw_auto_overflow_mutations.py",
    "test_hnsw_explicit_mutations.py",
    "test_hnsw_nn_capacity_mutations.py",
    "test_hnsw_nn_center_mutations.py",
    "test_hnsw_nn_policy_mutations.py",
    "test_hnsw_nn_precision_mutations.py",
    "test_hnsw_nn_sdk_params_mutations.py",
    "test_hnsw_nn_upper_consumer.py",
    "test_hnsw_nn_upper_precision_mutations.py",
    "test_hnsw_precision_mutations.py",
    "test_hnsw_row_sort_planner_equivalence.py",
    "test_hnsw_sdk_plan_mutations.py",
    "test_host_free_fence_mutations.py",
    "test_i3e_mutations.py",
    "test_ipc_connect_eintr_mutations.py",
    "test_lw_mutations.py",
    "test_notice_mutations.py",
    "test_page_overlay_mutations.py",
    "test_physical_build_mutations.py",
    "test_res_pool_mutations.py",
    "test_vacuum_policy_mutations.py",
)
SDK_CHECKS = {
    "check_metric_propagation_source": "test/native/hnsw-sdk/ace-ip/check_metric_propagation_source.py",
    "test_nn_upper_legacy_forwarder": "test/native/hnsw-sdk/test_nn_upper_legacy_forwarder.py",
    "test_nn_upper_legacy_forwarder_mutant": "test/native/hnsw-sdk/test_nn_upper_legacy_forwarder.py",
}
POSITIONAL_OUTPUT = {
    "check_hnsw_ace_receipt_source", "check_hnsw_auto_overflow_source",
    "check_hnsw_explicit_source", "test_admission_contract_mutations", "test_ar8_mutations",
    "test_hnsw_auto_overflow_mutations", "test_hnsw_explicit_mutations", "test_i3e_mutations",
    "test_lw_mutations", "test_notice_mutations", "test_res_pool_mutations",
}
SDK_NEIGHBORS = {"check_hnsw_nn_sdk_source", "test_hnsw_nn_upper_consumer",
                 "test_hnsw_nn_upper_precision_mutations"}

def arguments(name, output, sdk, canonical):
    target = str(output / name)
    if name in POSITIONAL_OUTPUT:
        return [target]
    if name in SDK_NEIGHBORS:
        return ["--sdk-src", str(sdk / "neighbors")]
    if name in ("test_hnsw_sdk_plan_mutations", "test_hnsw_row_sort_planner_equivalence"):
        return ["--canonical-header", str(canonical), "--row-sort-src", str(sdk)]
    if name == "check_metric_propagation_source":
        return [str(sdk.parent.parent)]
    if name == "test_nn_upper_legacy_forwarder":
        return ["--source", str(sdk)]
    if name == "test_nn_upper_legacy_forwarder_mutant":
        return ["--source", str(sdk), "--mutant"]
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
    checks = {Path(filename).stem: UNIT / filename for filename in UNIT_CHECKS}
    checks.update({name: ROOT / filename for name, filename in SDK_CHECKS.items()})
    if args.only:
        wanted = set(args.only)
        if wanted - checks.keys():
            parser.error("unknown check in --only")
        checks = {name: path for name, path in checks.items() if name in wanted}
    for path in checks.values():
        if not path.is_file():
            parser.error(f"missing check: {path.relative_to(ROOT)}")
    if args.output:
        args.output.resolve().mkdir()
        return run(checks, args.output.resolve(), sdk, canonical)
    with tempfile.TemporaryDirectory(prefix="pgcuvs-manual-checks-") as temp:
        return run(checks, Path(temp), sdk, canonical)

def run(checks, output, sdk, canonical):
    failures = 0
    with (output / "summary.tsv").open("w") as summary:
        for name, check in sorted(checks.items()):
            if name == "mutate_i3b_docs":
                # The shell fixture temporarily edits files next to the test.
                isolated = output / name
                for relative in ("docs/reference.md", "src/cuvs_cache_policy.h",
                                 "src/cuvs_cache_policy.c", "src/pg_cuvs_server.c",
                                 "test/unit/check_i3b_docs.py", "test/unit/mutate_i3b_docs.sh"):
                    target = isolated / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(ROOT / relative, target)
                cmd = ["sh", str(isolated / "test/unit/mutate_i3b_docs.sh")]
            else:
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
