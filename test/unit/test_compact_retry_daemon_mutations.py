"""Exercise NOTICE bytes returned by the real compact daemon handler."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CC = os.environ.get("CC", "cc")
SERVER = ROOT / "src/pg_cuvs_server.c"
OLD = 'ivfpq_retry == 2 ? "fp32" : "fp16"'
NEW = 'ivfpq_retry == 1 ? "fp32" : "fp16"'


def compile_and_run(mutated: bool) -> tuple[int, int | None]:
    with tempfile.TemporaryDirectory(prefix="compact-retry-daemon-") as d:
        work = Path(d)
        src = work / "src"
        tests = work / "test/unit"
        src.mkdir()
        tests.mkdir(parents=True)
        server = SERVER.read_text()
        if mutated:
            assert server.count(OLD) == 1
            server = server.replace(OLD, NEW)
        (src / "pg_cuvs_server.c").write_text(server)
        fixture = ROOT / "test/unit/test_compact_retry_daemon.c"
        shutil.copy2(fixture, tests / fixture.name)
        binary = work / "test-compact-retry-daemon"
        cmd = [CC, "-std=gnu11", "-O1", "-DPGCUVS_CPU_SHIM", "-ffunction-sections",
               "-fdata-sections", "-I", str(src), "-I", str(ROOT / "src"),
               str(tests / fixture.name),
               *[str(ROOT / f"src/{name}") for name in (
                   "cuvs_ipc.c", "cuvs_util.c", "cuvs_objstore.c", "cuvs_build_corpus.c",
                   "cuvs_wrapper_shim_cpu.c", "cuvs_cache_policy.c", "cuvs_peak_meter.c")],
               "-Wl,--gc-sections", "-lm", "-lpthread", "-lrt", "-lcurl", "-lssl", "-lcrypto",
               "-o", str(binary)]
        built = subprocess.run(cmd, capture_output=True, text=True)
        if built.returncode:
            print("COMPILE FAIL", built.stderr[-4000:])
            return 2, None
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        return 0, run.returncode


compile_rc, baseline_rc = compile_and_run(False)
if compile_rc or baseline_rc != 0:
    raise SystemExit("BASELINE FAIL: daemon NOTICE fixture did not pass")
print("GREEN daemon NOTICE: decoded fp16/fp32 and success/failure replies")
compile_rc, mutant_rc = compile_and_run(True)
if compile_rc:
    raise SystemExit("MUTANT COMPILE FAIL: notice_dtype_reversed")
if mutant_rc == 0:
    raise SystemExit("SURVIVED notice_dtype_reversed")
print(f"RED notice_dtype_reversed (runtime rc={mutant_rc})")
