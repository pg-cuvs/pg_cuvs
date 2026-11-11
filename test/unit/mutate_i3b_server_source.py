#!/usr/bin/env python3
"""Mutations for check_i3b_server_source.py: each must turn the scan RED."""
import os, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "src", "pg_cuvs_server.c")
SCAN = os.path.join(ROOT, "test", "unit", "check_i3b_server_source.py")
M = [
    ("startup loads as LOAD", "CUVS_RQ_LOAD_STARTUP) == 0)", "CUVS_RQ_LOAD) == 0)"),
    ("warmup loads as LOAD", "job.index_oid, CUVS_RQ_LOAD_WARMUP)", "job.index_oid, CUVS_RQ_LOAD)"),
    ("search reload as LOAD_STARTUP",
     "    if (!e && load_index(cmd->db_oid, cmd->index_oid, CUVS_RQ_LOAD) == 0)",
     "    if (!e && load_index(cmd->db_oid, cmd->index_oid, CUVS_RQ_LOAD_STARTUP) == 0)"),
    ("requester dropped from a call", "ensure_vram(needed, dev, CUVS_RQ_BUILD_SHARD, NULL)", "ensure_vram(needed, dev)"),
    ("EXTEND keeps file_current", "    e->file_current = 0;   /* I-FC (1): graph now differs from the files */\n", ""),
    ("save_index does not set file_current", "    e->file_current = 1;   /* I-FC (iii) */\n", ""),
    ("REINDEX branch skips file_current", "        existing->file_current = strcmp(save_dir, g_index_dir) == 0;\n", ""),
    ("load sets file_current before reset", "    reset_entry_stats(e);\n    /* I3-B defect 3",
     "    e->file_current = handle != NULL;\n    reset_entry_stats(e);\n    /* I3-B defect 3"),
    ("S6: IVF-PQ build admits as LOAD", "    int admit_rc = ensure_vram(needed, target_gpu, CUVS_RQ_BUILD_IVFPQ, &rej);",
     "    int admit_rc = ensure_vram(needed, target_gpu, CUVS_RQ_LOAD, &rej);"),
    ("S7: shard build admits as LOAD", "        if (ensure_vram(needed, dev, CUVS_RQ_BUILD_SHARD, NULL) != 0)",
     "        if (ensure_vram(needed, dev, CUVS_RQ_LOAD, NULL) != 0)"),
    ("S8: CAGRA slot as LOAD", "            (void) make_slot(target_gpu, CUVS_RQ_BUILD_CAGRA);",
     "            (void) make_slot(target_gpu, CUVS_RQ_LOAD);"),
    ("S10: single post-admission eviction restored",
     "    int build_oom = !new_handle && cuvs_last_build_was_oom();",
     "    (void)evict_lru(target_gpu, CUVS_RQ_BUILD_CAGRA);\n"
     "    int build_oom = !new_handle && cuvs_last_build_was_oom();"),
    ("flat slot as LOAD", "                (void) make_slot(fgpu, CUVS_RQ_BUILD_FLAT);",
     "                (void) make_slot(fgpu, CUVS_RQ_LOAD);"),
    ("multi post-admission eviction restored",
     "        build_oom = !new_handle && cuvs_last_build_was_oom();",
     "        (void)evict_lru(target_gpu, CUVS_RQ_BUILD_CAGRA);\n"
     "        build_oom = !new_handle && cuvs_last_build_was_oom();"),
    ("second skip site", "graceful_shutdown(void)\n{", "graceful_shutdown(void)\n{\n    if (!g_indexes[0].file_current) {}"),
    ("wrapper: peak CAS before the upstream allocate",
     "        std::size_t level = cuvs_peak_meter_reserve(dev_, sz);\n        try {\n            void *p = upstream_.allocate(stream, bytes);\n            cuvs_peak_meter_commit(dev_, level);",
     "        std::size_t level = cuvs_peak_meter_reserve(dev_, sz);\n        cuvs_peak_meter_commit(dev_, level);\n        try {\n            void *p = upstream_.allocate(stream, bytes);"),
]
WRAP = os.path.join(ROOT, "src", "cuvs_wrapper.cu")
orig = open(SRC).read()
worig = open(WRAP).read()
base = subprocess.run([sys.executable, SCAN, SRC], capture_output=True, text=True)
print("baseline:", base.stdout.strip().splitlines()[-1])
bad = 0 if base.returncode == 0 else 1
for name, old, new in M:
    is_wrap = name.startswith("wrapper:")
    text = worig if is_wrap else orig
    if text.count(old) != 1:
        print(f"SETUP-ERROR {name}: {text.count(old)}x"); bad += 1; continue
    d = tempfile.mkdtemp()
    # the scan reads cuvs_wrapper.cu next to the server source
    open(os.path.join(d, "pg_cuvs_server.c"), "w").write(orig if is_wrap else orig.replace(old, new, 1))
    open(os.path.join(d, "cuvs_wrapper.cu"), "w").write(worig.replace(old, new, 1) if is_wrap else worig)
    r = subprocess.run([sys.executable, SCAN, os.path.join(d, "pg_cuvs_server.c")],
                       capture_output=True, text=True)
    red = r.returncode != 0
    bad += 0 if red else 1
    first = next((l for l in r.stdout.splitlines() if l.startswith("FAIL")), "")
    print(f"{'RED ' if red else 'MISS'} {name}: {first}")
print(f"mutations: {len(M)}, survived/invalid: {bad}")
sys.exit(1 if bad else 0)
