"""Q6: each mutant of the host-free fence must make test_host_free_fence fail.

Header mutants break the fence itself (including the review A2 B1 failure
path); the wrapper is checked by source wiring assertions (the CUDA wrapper cannot build on the CPU image): PooledRes
must open the scope on the borrowed stream, close it before returning the
resource, and the fenced resource must be installed as RAFT's default.
"""
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = ROOT / "src" / "cuvs_host_free_fence.hpp"
TEST = ROOT / "test" / "unit" / "test_host_free_fence.cpp"
WRAPPER = ROOT / "src" / "cuvs_wrapper.cu"
CXX = os.environ.get("CXX", "c++")

HEADER_MUTANTS = {
    "no_fence_in_deallocate": (
        "        if (CuvsHostFreeFence::before_free(bytes) == CuvsHostFreeFence::WAIT_FAILED) {",
        "        if (false) {"),
    "fence_after_free": (
        "        upstream_->deallocate(p, bytes, alignment);\n    }\n    bool do_is_equal",
        "        upstream_->deallocate(p, bytes, alignment);\n        (void)CuvsHostFreeFence::before_free(bytes);\n    }\n    bool do_is_equal"),
    # B1 reintroduced: a failed wait still returns the block upstream.
    "quarantine_dropped": (
        "            return;  /* quarantined: a queued write may still land here */",
        "            upstream_->deallocate(p, bytes, alignment);\n            return;"),
    "failure_reported_as_success": ("        return WAIT_FAILED;\n", "        return WAITED;\n"),
    "scope_failure_not_recorded": ("            s.owner->failures_++;\n", ""),
    "hook_not_called": ("            s.on_fail(err, s.stream, s.device, bytes);\n", "            ;\n"),
    "hook_wrong_bytes": ("s.on_fail(err, s.stream, s.device, bytes);", "s.on_fail(err, s.stream, s.device, 0);"),
    "failure_counter_dropped": ("        stats().sync_failures.fetch_add(1, std::memory_order_relaxed);\n", ""),
    "quarantine_counter_dropped": ("            st.quarantined_bytes.fetch_add(bytes, std::memory_order_relaxed);\n", ""),
    "scope_no_restore": ("        ~Scope() { current() = prev_; }", "        ~Scope() {}"),
    "null_stream_inactive": ("s.active = sync != nullptr;", "s.active = sync != nullptr && stream != nullptr;"),
    "process_wide_slot": ("static thread_local Slot slot;", "static Slot slot;"),
    "before_free_never_syncs": ("        const int err = s.sync(s.stream);", "        const int err = 0;"),
}

# Source wiring of the CUDA wrapper (no nvcc on the CPU image): regex checks only.
# They cannot prove the wrapper path runs (review A2, A1); the header mutants above can.
WIRING = {
    "scope_on_borrowed_stream_with_hook": r"fence\.emplace\(static_cast<void \*>\(r->res->get_stream\(\)\.value\(\)\), fence_sync_stream,\s*device_id, fence_log_failure\);",
    "scope_closed_before_release": r"fence\.reset\(\);\s*\n\s*if \(fence_failed && !poisoned\)",
    "fence_failure_poisons": r"const bool fence_failed = fence && fence->failures\(\) > 0;[\s\S]{0,400}poisoned = true;[\s\S]{0,80}if \(r && !poisoned\) release_res",
    "fence_check_throws": r"void fence_check\(\)\s*\{\s*if \(fence && fence->failures\(\) > 0\)\s*throw std::runtime_error",
    "cagra_build_checked": r"cuvs::neighbors::cagra::build\(\s*res,\s*params,\s*raft::make_const_mdspan\(d_corpus\.view\(\)\)\);\s*_pr\.fence_check\(\);",
    "ivfpq_build_checked": r"cuvs::neighbors::ivf_pq::build\(\s*res, params,\s*raft::make_const_mdspan\(d_corpus\.view\(\)\)\);\s*res\.sync_stream\(\);\s*_pr\.fence_check\(\);",
    "hnsw_file_build_checked": r"resources\.fence_check\(\);",
    "failure_logged": r"fence_log_failure\(int err, void \*stream, int device, std::size_t bytes\)[\s\S]{0,600}cudaGetErrorName",
    "installed_in_pooledres": r"ensure_fenced_host_resource\(\);\s*\n\s*fence\.emplace",
    "default_host_resource_set": r"raft::mr::set_default_host_resource\(raft::mr::host_resource_ref\{adaptor\}\)",
    "sync_is_stream_sync": r"cudaStreamSynchronize\(static_cast<cudaStream_t>\(stream\)\)",
}

# Request checks after every cuVS build/extend/serialize/deserialize/from_cagra call
# that can free RAFT host containers (count, not a repeated regex).
WIRING_COUNTS = {"ten_checked_calls": (r"\.fence_check\(\);", 10)}


def build_and_run(header_text: str, work: pathlib.Path) -> int:
    inc = work / "inc"
    inc.mkdir(exist_ok=True)
    (inc / "cuvs_host_free_fence.hpp").write_text(header_text)
    exe = work / "t"
    b = subprocess.run([CXX, "-std=c++17", "-Wall", "-Wextra", "-pthread", "-I", str(inc),
                        "-o", str(exe), str(TEST)], capture_output=True, text=True)
    if b.returncode != 0:
        return 2
    return subprocess.run([str(exe)], capture_output=True, text=True).returncode


def main() -> int:
    orig = HEADER.read_text()
    fails = 0
    with tempfile.TemporaryDirectory() as d:
        work = pathlib.Path(d)
        if build_and_run(orig, work) != 0:
            print("BASELINE FAIL: unmutated test does not pass")
            return 1
        for name, (old, new) in HEADER_MUTANTS.items():
            if orig.count(old) != 1:
                print(f"ANCHOR {name}: expected 1 match, found {orig.count(old)}")
                fails += 1
                continue
            rc = build_and_run(orig.replace(old, new), work)
            ok = rc not in (0, 2)   # 1 = a CHECK failed; a crash (negative rc) also fails the test
            print(f"{'RED ' if ok else 'SURVIVED'} {name} (rc={rc})")
            fails += 0 if ok else 1
    src = WRAPPER.read_text()
    for name, pat in WIRING.items():
        ok = re.search(pat, src) is not None
        print(f"{'WIRED' if ok else 'MISSING'} {name}")
        fails += 0 if ok else 1
    for name, (pat, want) in WIRING_COUNTS.items():
        got = len(re.findall(pat, src))
        ok = got >= want
        print(f"{'WIRED' if ok else 'MISSING'} {name} ({got} >= {want})")
        fails += 0 if ok else 1
    total = len(HEADER_MUTANTS) + len(WIRING) + len(WIRING_COUNTS)
    print(f"host-free-fence mutations: {total - fails}/{total}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
