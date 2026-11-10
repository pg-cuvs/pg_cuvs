"""Runtime mutations: each must compile, then fail the actual-handler fixture."""
from pathlib import Path
import os
import resource
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
CC = os.environ.get("CC", "cc")
SERVER = "pg_cuvs_server.c"
mutations = {
    "guard_disabled": ("cuvs_admission_contract.h", "return raw_free < CUVS_BUILD_PHYSICAL_FLOOR_BYTES;", "return 0;"),
    "boundary_equal_refused": ("cuvs_admission_contract.h", "return raw_free < CUVS_BUILD_PHYSICAL_FLOOR_BYTES;", "return raw_free <= CUVS_BUILD_PHYSICAL_FLOOR_BYTES;"),
    "pool_cache_counted_as_raw": (SERVER, "uint64_t raw_free = cuvs_vram_raw_free_bytes_on(dev);", "uint64_t raw_free = cuvs_vram_physical_free_bytes_on(dev);"),
    "notice_lost": (SERVER, '"pg_cuvs: %s deferred before cuVS: GPU %d physical memory pressure"', '"pg_cuvs: %s deferred on GPU %d"'),
    "status_count_lost": (SERVER, "g_physical_build_rejects[dev]++;", "/* lost count */"),
    "sqlstate_permanent": ("cuvs_util.c", "if (reason == CUVS_ADMISSION_AR0)\n        return CUVS_ADMISSION_SQLSTATE_LIMIT;", "if (reason == CUVS_ADMISSION_AR0 || reason == CUVS_ADMISSION_PHYS)\n        return CUVS_ADMISSION_SQLSTATE_LIMIT;"),
    "hnsw_reply_lost": ("cuvs_ipc.c", "reply = wire.header;\n        remember_reply(&reply);", "reply = wire.header;"),
    "hnsw_reply_stale": ("cuvs_ipc.c", "char token = 'O';\n    forget_reply();", "char token = 'O';"),
}
for name, call in {
    "single": 'physical_build_check(target_gpu, "BUILD", cmd, &rej)',
    "multi": 'physical_build_check(target_gpu, "BUILD_MULTI", cmd, &rej)',
    "shard": 'physical_build_check(dev, "BUILD_SHARD", cmd, &shard_rej)',
    "ivfpq": 'physical_build_check(target_gpu, "BUILD_IVFPQ", cmd, &rej)',
    "compact": 'physical_build_check((int)e->gpu_device_id, "COMPACT", cmd, &physical_rej)',
    "hnsw": 'physical_build_check(device, "BUILD_HNSW_FILE", cmd, &physical_rej)',
}.items():
    mutations[f"{name}_hook_bypassed"] = (SERVER, call, "0")


def run_case(name, mutation):
    with tempfile.TemporaryDirectory(prefix="physical-build-mut-") as d:
        work = Path(d)
        shutil.copytree(ROOT / "src", work / "src", ignore=shutil.ignore_patterns("*.o", "*.bc"))
        (work / "test/unit").mkdir(parents=True)
        shutil.copy2(ROOT / "test/unit/test_physical_build_guard.c", work / "test/unit")
        if mutation:
            file, before, after = mutation
            p = work / "src" / file
            text = p.read_text()
            assert before in text, (name, "missing mutation anchor")
            p.write_text(text.replace(before, after))
        cmd = [CC, "-std=gnu11", "-O1", "-DPGCUVS_CPU_SHIM", "-ffunction-sections", "-fdata-sections", "-I", str(work / "src"),
               str(work / "test/unit/test_physical_build_guard.c"),
               *[str(work / "src" / f) for f in ("cuvs_ipc.c", "cuvs_util.c", "cuvs_objstore.c", "cuvs_build_corpus.c", "cuvs_wrapper_shim_cpu.c", "cuvs_cache_policy.c", "cuvs_peak_meter.c")],
               "-Wl,--gc-sections", "-lm", "-lpthread", "-lrt", "-lcurl", "-lssl", "-lcrypto", "-o", str(work / "test-guard")]
        built = subprocess.run(cmd, capture_output=True, text=True)
        if built.returncode:
            raise RuntimeError(f"COMPILE FAIL {name}: {built.stderr[-3000:]}")
        result = subprocess.run([str(work / "test-guard")], capture_output=True, text=True, timeout=70)
        if mutation is None:
            assert result.returncode == 0, result.stderr[-3000:]
            print("GREEN physical guard real handlers", flush=True)
        else:
            assert result.returncode != 0, f"SURVIVED {name}"
            assert "assert" in result.stderr.lower(), f"not an assertion failure: {name}: {result.stderr[-1000:]}"
            print(f"RED {name} runtime_rc={result.returncode}", flush=True)

run_case("baseline", None)
for name, mutation in mutations.items():
    run_case(name, mutation)
print(f"PASS {len(mutations)}/{len(mutations)} runtime mutations killed")
