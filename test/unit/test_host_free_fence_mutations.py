"""Q6: each mutant of the host-free fence must make test_host_free_fence fail.

Header mutants break the fence itself; wrapper mutants are checked by source
wiring assertions (the CUDA wrapper cannot build on the CPU image): PooledRes
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
    "no_fence_in_deallocate": ("        CuvsHostFreeFence::before_free();\n        upstream_->deallocate", "        upstream_->deallocate"),
    "fence_after_free": ("        CuvsHostFreeFence::before_free();\n        upstream_->deallocate(p, bytes, alignment);",
                         "        upstream_->deallocate(p, bytes, alignment);\n        CuvsHostFreeFence::before_free();"),
    "scope_no_restore": ("        ~Scope() { current() = prev_; }", "        ~Scope() {}"),
    "null_stream_inactive": ("s.active = sync != nullptr;", "s.active = sync != nullptr && stream != nullptr;"),
    "process_wide_slot": ("static thread_local Slot slot;", "static Slot slot;"),
    "before_free_never_syncs": ("(void)s.sync(s.stream);", "(void)0;"),
}

WIRING = {
    "scope_on_borrowed_stream": r"fence\.emplace\(static_cast<void \*>\(r->res->get_stream\(\)\.value\(\)\), fence_sync_stream\);",
    "scope_closed_before_release": r"fence\.reset\(\);\s*\n\s*if \(r && !poisoned\) release_res",
    "installed_in_pooledres": r"ensure_fenced_host_resource\(\);\s*\n\s*fence\.emplace",
    "default_host_resource_set": r"raft::mr::set_default_host_resource\(raft::mr::host_resource_ref\{adaptor\}\)",
    "sync_is_stream_sync": r"cudaStreamSynchronize\(static_cast<cudaStream_t>\(stream\)\)",
}


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
            ok = rc == 1
            print(f"{'RED ' if ok else 'SURVIVED'} {name} (rc={rc})")
            fails += 0 if ok else 1
    src = WRAPPER.read_text()
    for name, pat in WIRING.items():
        ok = re.search(pat, src) is not None
        print(f"{'WIRED' if ok else 'MISSING'} {name}")
        fails += 0 if ok else 1
    total = len(HEADER_MUTANTS) + len(WIRING)
    print(f"host-free-fence mutations: {total - fails}/{total}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
