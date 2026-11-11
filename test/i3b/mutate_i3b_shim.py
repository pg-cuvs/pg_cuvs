#!/usr/bin/env python3
"""Daemon/backend-level mutation check for I3-B (runs on the host).

Each mutation edits one copy of the tree, runs test/i3b/run_i3b_shim.sh in
the pgcuvs-i1-cpu image (real PG16 + shim daemon) and requires at least one
FAIL line (RED). Runs N containers in parallel. Copies live under $HOME
because Colima mounts only $HOME.

    python3 test/i3b/mutate_i3b_shim.py [-j 4] [name-substring ...]
"""
import argparse
import concurrent.futures as cf
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORK = os.path.join(os.path.expanduser("~"), ".cache", "i3b-mut")
IMAGE = "pgcuvs-i1-cpu:20261031"
SRV = "src/pg_cuvs_server.c"

# (name, decision/CK, file, old, new)
MUTATIONS = [
    ("builds evict residents (scope ignored)", "UC-1 R1 / CK-36 (c)", SRV,
     "        req.scope = g_reclaim.scope;",
     "        req.scope.kind = CUVS_SCOPE_LRU;"),
    ("--load-soft-evict off ignored", "CK-9 r18 KB-25", SRV,
     "        req.soft_evict = g_reclaim.soft_evict;",
     "        req.soft_evict = 1;"),
    ("--reindex-self-replace off ignored", "CK-9 r18 KB-26", SRV,
     "                                 old_bytes, g_reclaim.self_replace)",
     "                                 old_bytes, 1)"),
    ("self-replace NOTICE text dropped", "UC-1 2a", SRV,
     '    LOG_INFO("[self_replace] db=%u idx=%u unloaded=%zu notice=\\"%s\\" detail=\\"%s\\"\\n",\n'
     '             db_oid, index_oid, freed, g_build_notice, g_build_notice_detail);',
     "    (void) freed;"),
    ("reclaim does not check sidecars", "decision 4 / CK-24 r9", SRV,
     "                                      entry_is_cagra_single(e), e->file_current))",
     "                                      entry_is_cagra_single(e), e->file_current) && 0)"),
    ("usage file only when reclaim is on", "CK-24 (usage file in mode off)", SRV,
     "        if (g_reclaim.usage_file)\n            usage_stats_write();",
     "        if (g_reclaim.usage_file && g_reclaim.mode != CUVS_RECLAIM_OFF)\n            usage_stats_write();"),
    ("startup ignores the usage file", "UC-1 4c / CK-36 (e)", SRV,
     "            n_st = usage_stats_load(st, CUVS_USAGE_STATS_MAX, why, sizeof(why));",
     "            n_st = -1;"),
    ("--usage-stats-file off ignored", "CK-9 r18 KB-27", SRV,
     "    if (g_reclaim.usage_file)\n    {\n        st = calloc(",
     "    if (1)\n    {\n        st = calloc("),
    ("reload keeps stale n_extended (defect 3)", "CK-26 r14 / CK-37", SRV,
     "    e->n_extended      = 0;\n    e->compact_count   = 0;\n    e->last_compact_at = 0;\n    /* I-FC (ii)",
     "    /* I-FC (ii)"),
    ("EXTEND leaves file_current 1", "CK-37 I-FC (1)", SRV,
     "    e->file_current = 0;   /* I-FC (1): graph now differs from the files */",
     ""),
    ("eviction always saves (skip removed)", "CK-37 (c)", SRV,
     "        if (!e->file_current)\n        {\n            uint64_t t0 = mono_us();",
     "        if (1)\n        {\n            uint64_t t0 = mono_us();"),
    ("unload ignores the pin", "EV-6 / CK-25", SRV,
     "            if (e->pinned)\n            {\n                pthread_mutex_unlock(&g_index_mutex);\n                reply_msg(client_fd, CUVS_STATUS_ERROR,\n                          \"pinned: run pg_cuvs_unpin first\");",
     "            if (0)\n            {\n                pthread_mutex_unlock(&g_index_mutex);\n                reply_msg(client_fd, CUVS_STATUS_ERROR,\n                          \"pinned: run pg_cuvs_unpin first\");"),
    ("pin cap ignored", "EV-6 --pin-max-pct", SRV,
     "                if (pinned + mine > beff / 100 * g_reclaim.pin_max_pct)",
     "                if (0)"),
    ("REINDEX clears the pin", "C7 7.2-N3", SRV,
     "    e->hits               = 0;\n    e->last_search        = 0;",
     "    e->hits               = 0;\n    e->pinned             = 0;\n    e->last_search        = 0;"),
    ("headroom F ignored by the daemon", "UC-1 E", SRV,
     "                          g_reclaim.mode == CUVS_RECLAIM_HEADROOM ? g_reclaim_f[dev].f_bytes : 0 };",
     "                          0 };"),
    ("runtime set does not record its source", "UC-7 reclaim_params", SRV,
     "        snprintf(g_reclaim_src[k], sizeof(g_reclaim_src[k]), \"function\");",
     ""),
    ("SHOW caches reclaim_params per session", "CK-26 r16 cross-session", "src/pg_cuvs.c",
     "    if (cuvs_ipc_text(cuvs_socket_path, CUVS_OP_RECLAIM_STATUS, &raw, &n) != CUVS_STATUS_OK)\n        return;",
     "    static char *cached; static size_t cached_n;\n"
     "    if (!cached) { if (cuvs_ipc_text(cuvs_socket_path, CUVS_OP_RECLAIM_STATUS, &cached, &cached_n) != CUVS_STATUS_OK) return; }\n"
     "    raw = strdup(cached); n = cached_n;"),
    ("S1 self-replace NOTICE wording replaced", "review BLOCK 2", "src/cuvs_cache_policy.c",
     '"pg_cuvs: previous resident copy of %s unloaded from GPU to make "',
     '"pg_cuvs: old copy of %s dropped to make "'),
    ("S1c NOTICE free number wrong", "review BLOCK 2", SRV,
     "                                        label, needed, room);",
     "                                        label, needed, 0);"),
    ("S2 failure NOTICE removed", "review BLOCK 2", SRV,
     "        LOG_ERROR(\"[handle_build] cuvs_cagra_build returned NULL\\n\");\n        note_self_replace_failure(cmd->index_oid, self_unloaded);",
     "        LOG_ERROR(\"[handle_build] cuvs_cagra_build returned NULL\\n\");"),
    ("S6 IVF-PQ build admits as LOAD", "review BLOCK 4", SRV,
     # integration: ensure_vram takes (needed, dev, rq, rej, line) on the I3 stack
     "    int admit_rc = ensure_vram(needed, target_gpu, CUVS_RQ_BUILD_IVFPQ, &rej);",
     "    int admit_rc = ensure_vram(needed, target_gpu, CUVS_RQ_LOAD, &rej);"),
    ("S7 shard build admits as LOAD", "review BLOCK 4", SRV,
     "        if (ensure_vram(needed, dev, CUVS_RQ_BUILD_SHARD, NULL) != 0)",
     "        if (ensure_vram(needed, dev, CUVS_RQ_LOAD, NULL) != 0)"),
    ("S8 CAGRA registry slot as LOAD", "review BLOCK 4", SRV,
     "            (void) make_slot(target_gpu, CUVS_RQ_BUILD_CAGRA);",
     "            (void) make_slot(target_gpu, CUVS_RQ_LOAD);"),
    ("S10 post-admission OOM evicts again (LOAD requester)", "review BLOCK 4 / ADR-086", SRV,
     '        log_build_oom("BUILD", target_gpu, cmd->db_oid, cmd->index_oid, needed);\n'
     '        cuvs_set_inject_build_oom(0);   /* #82: see the failure branch below */',
     '        (void) evict_lru(target_gpu, CUVS_RQ_LOAD);\n'
     '        log_build_oom("BUILD", target_gpu, cmd->db_oid, cmd->index_oid, needed);\n'
     '        cuvs_set_inject_build_oom(0);   /* #82: see the failure branch below */'),
    ("S10b post-admission OOM without [build_oom]", "ADR-086 [build_oom]", SRV,
     '        log_build_oom("BUILD", target_gpu, cmd->db_oid, cmd->index_oid, needed);\n'
     '        cuvs_set_inject_build_oom(0);   /* #82: see the failure branch below */',
     '        cuvs_set_inject_build_oom(0);   /* #82: see the failure branch below */'),
    ("S10c post-admission OOM reported as BUILD_FAILED", "ADR-086 status 2", SRV,
     '        send_admission_reject(client_fd, CUVS_STATUS_OOM_FALLBACK,\n'
     '                              "GPU memory exhausted during build; no index evicted",\n'
     '                              &oom_rej);',
     '        (void) oom_rej;\n'
     '        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED, "cuvs_cagra_build failed");'),
    ("startup evicts (LOAD instead of LOAD_STARTUP)", "C1 RQ LOAD_STARTUP", SRV,
     "        if (load_index(k->db_oid, k->index_oid, CUVS_RQ_LOAD_STARTUP) == 0)",
     "        if (load_index(k->db_oid, k->index_oid, CUVS_RQ_LOAD) == 0)"),
]


def run_one(i, m):
    name, ck, rel, old, new = m
    d = os.path.join(WORK, f"m{i:02d}")
    if os.path.exists(d):
        shutil.rmtree(d)
    shutil.copytree(ROOT, d, ignore=shutil.ignore_patterns(".git", "*.o", "*.so", "pg_cuvs_server",
                                                          "pg_cuvs_server_test", "test-*"))
    path = os.path.join(d, rel)
    src = open(path).read()
    if src.count(old) != 1:
        return name, ck, "SETUP-ERROR", f"anchor found {src.count(old)}x"
    open(path, "w").write(src.replace(old, new, 1))
    r = subprocess.run(["docker", "run", "--rm", "-v", f"{d}:/w", "-w", "/w", IMAGE,
                        "sh", "test/i3b/run_i3b_shim.sh"],
                       capture_output=True, text=True, timeout=1800)
    out = r.stdout.splitlines()
    fails = [l for l in out if l.startswith("FAIL ")]
    built = any(l.startswith("PASS") or l.startswith("FAIL") for l in out)
    shutil.rmtree(d, ignore_errors=True)
    if not built:
        return name, ck, "BUILD_FAIL", (r.stdout + r.stderr).strip().splitlines()[-1:]
    return name, ck, "RED" if fails else "MISS", fails[:2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-j", type=int, default=4)
    ap.add_argument("only", nargs="*")
    a = ap.parse_args()
    os.makedirs(WORK, exist_ok=True)
    todo = [(i, m) for i, m in enumerate(MUTATIONS)
            if not a.only or any(s in m[0] for s in a.only)]
    bad = 0
    with cf.ThreadPoolExecutor(a.j) as ex:
        for name, ck, status, info in ex.map(lambda t: run_one(*t), todo):
            if status != "RED":
                bad += 1
            print(f"{status:11s} [{ck}] {name}: {info}", flush=True)
    print(f"mutations: {len(todo)}, survived/invalid: {bad}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
