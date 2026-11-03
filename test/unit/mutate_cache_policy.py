#!/usr/bin/env python3
"""Mutation check for src/cuvs_cache_policy.c (I3-B).

Each mutation injects one defect the CK rows name, rebuilds the unit test in
a temp dir and requires it to FAIL (RED). The unmutated source must PASS.

    python3 test/unit/mutate_cache_policy.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "src", "cuvs_cache_policy.c")

# (name, CK row, old, new)
MUTATIONS = [
    ("eligible >= becomes >", "CK-23",
     "    return now - c->last_use >=\n           cuvs_evict_protect_s(",
     "    return now - c->last_use >\n           cuvs_evict_protect_s("),
    ("pin ignored", "CK-23",
     "    return c->pinned || c->in_use > 0;",
     "    return c->in_use > 0;"),
    ("in-use ignored", "CK-23",
     "    return c->pinned || c->in_use > 0;",
     "    return c->pinned;"),
    ("bonus cap removed", "CK-23",
     "    uint32_t h = hits < hits_cap ? hits : hits_cap;",
     "    uint32_t h = hits;"),
    ("scope none returns victims", "CK-23",
     "        case CUVS_SCOPE_NONE:\n            return 0;",
     "        case CUVS_SCOPE_NONE:\n            return cuvs_evict_eligible(c, now, p);"),
    ("order by raw last_use", "CK-23",
     "    return c->last_use > c->loaded_at ? c->last_use : c->loaded_at;",
     "    return c->last_use;"),
    ("soft evict takes pinned", "CK-23",
     "                take = !elig && !cuvs_evict_protected(c);",
     "                take = !elig && !c->in_use;"),
    ("soft victim labeled reload", "CK-23",
     "            else\n                why = CUVS_EVICT_SOFT;",
     "            else\n                why = CUVS_EVICT_RELOAD;"),
    ("soft applied to warmup", "CK-23",
     "    int soft = (req->rq == CUVS_RQ_LOAD || req->rq == CUVS_RQ_LOAD_SHARD) &&",
     "    int soft = (req->rq == CUVS_RQ_LOAD || req->rq == CUVS_RQ_LOAD_SHARD || req->rq == CUVS_RQ_LOAD_WARMUP) &&"),
    ("startup evicts", "CK-36",
     "            else if (req->rq == CUVS_RQ_LOAD_STARTUP)\n                take = 0;",
     "            else if (req->rq == CUVS_RQ_LOAD_STARTUP)\n                take = elig;"),
    ("startup reports AR5", "CK-36",
     "        out->verdict = CUVS_EVICT_SHORT; /* startup never reports AR5 */\n        return;",
     "        out->verdict = CUVS_EVICT_AR5;\n        return;"),
    ("partial eviction on reject", "CK-23",
     "    if (out->allowed_total >= req->need)",
     "    if (1)"),
    ("F-1: AR0 counted", "CK-24",
     "    return reason >= CUVS_ADMISSION_AR1 && reason <= CUVS_ADMISSION_AR6;",
     "    return reason >= CUVS_ADMISSION_AR0 && reason <= CUVS_ADMISSION_AR6;"),
    ("F-1: AR7 counted", "CK-24",
     "    return reason >= CUVS_ADMISSION_AR1 && reason <= CUVS_ADMISSION_AR6;",
     "    return reason >= CUVS_ADMISSION_AR1 && reason <= CUVS_ADMISSION_AR7;"),
    ("F-2: cap removed", "CK-24",
     "    if (out->f_bytes > out->f_cap)\n        out->f_bytes = out->f_cap;",
     ""),
    ("F-2: fixed F uncapped", "CK-24",
     "        out->f_bytes = fixed < out->f_cap ? fixed : out->f_cap;",
     "        out->f_bytes = fixed;"),
    ("F-3: window ignored", "CK-24",
     "        if (now - r->at > window_s || r->at > now)\n            continue;",
     ""),
    ("F-3: admission keeps rejects", "CK-24",
     "            if (x->db_oid == r->db_oid && x->index_oid == r->index_oid)\n                continue;",
     ""),
    ("DROP does not forget F records", "conch 2026-11-03",
     "        if (log->rec[i].db_oid != db_oid || log->rec[i].index_oid != index_oid)\n            log->rec[w++] = log->rec[i];",
     "        log->rec[w++] = log->rec[i];"),
    ("F-4: floor removed", "CK-24",
     "    if (resident <= pct_of(g->budget_eff, g->floor_pct))\n        return 0;   /* fence F-4: floor applies to F reclaim only */",
     ""),
    ("F-4: floor applied to usage lines", "CK-24 r18 A-1",
     "    if (starting)\n        return resident > pct_of(g->budget_eff, g->hi_pct);",
     "    if (resident <= pct_of(g->budget_eff, g->floor_pct))\n        return 0;\n    if (starting)\n        return resident > pct_of(g->budget_eff, g->hi_pct);"),
    ("F-4: victim may cross floor", "CK-24",
     "        return after >= pct_of(g->budget_eff, g->floor_pct);",
     "        return 1;"),
    ("usage mode uses F", "CK-24",
     "    if (g->mode != CUVS_RECLAIM_HEADROOM || g->f_bytes == 0)",
     "    if (g->mode == CUVS_RECLAIM_OFF || g->f_bytes == 0)"),
    ("off mode reclaims", "CK-24",
     "    if (g->mode == CUVS_RECLAIM_OFF || g->hi_pct >= 100 || g->budget_eff == 0)",
     "    if (g->hi_pct >= 100 || g->budget_eff == 0)"),
    ("headroom loses usage lines", "CK-24",
     "    int u = usage_side(g, resident, 1);",
     "    int u = g->mode == CUVS_RECLAIM_USAGE && usage_side(g, resident, 1);"),
    ("lo ignored", "CK-24",
     "    return resident > pct_of(g->budget_eff, g->lo_pct);",
     "    return resident > pct_of(g->budget_eff, g->hi_pct);"),
    ("r hint without floor", "UC-2 r18 B-1",
     "    return resident - evict >= floor_bytes;",
     "    return 1;"),
    ("runtime accepts accounting knob", "CK-9",
     "    { \"reclaim-free-max-pct\",    1, 100,        0 },",
     "    { \"reclaim-free-max-pct\",    1, 100,        1 },"),
    ("runtime accepts low > high", "CK-9",
     "    if (cuvs_reclaim_params_check(&tmp, err, errcap) != 0)\n        return -1;",
     ""),
    ("range check removed", "CK-9",
     "        if (parse_u64(value, r->hi, &v) != 0 || v < r->lo)",
     "        if (parse_u64(value, UINT64_MAX, &v) != 0)"),
    ("mode validation removed", "CK-9",
     "        if (cuvs_parse_reclaim_mode(value, &p->mode) != 0)\n            ERR(",
     "        if (cuvs_parse_reclaim_mode(value, &p->mode) != 0 && 0)\n            ERR("),
    ("usage file crc unchecked", "CK-36 (e)",
     "    if (cuvs_crc32(buf, body) != (uint32_t)crc_file)\n        BAD(\"crc mismatch\");",
     ""),
    ("usage file partial list", "CK-36 (e)",
     "            BAD(\"truncated at entry %ld\", i);",
     "            { *n_out = (int)i; return 0; }"),
    ("startup ignores stats", "CK-36 (e)",
     "    if ((a->stat != NULL) != (b->stat != NULL))\n        return a->stat ? -1 : 1;\n    if (a->stat)",
     "    if (0)"),
    ("startup order by enumeration", "CK-36 (e)",
     "    qsort(it, (size_t)n, sizeof(OrderItem), order_item_cmp);",
     ""),
    ("sidecar: .tombstone missed", "CK-24 r10",
     "    if (path_exists(dir, db_oid, index_oid, \"tombstone\"))",
     "    if (0)"),
    ("sidecar: skip ignores file_current", "UC-1 7b",
     "    return sidecar_kind != CUVS_SIDECAR_NONE && is_cagra_single && !file_current;",
     "    return sidecar_kind != CUVS_SIDECAR_NONE;"),
    ("self-replace ignores pin", "UC-1 2a",
     "    if (!knob_on || !has_old || old_pinned)",
     "    if (!knob_on || !has_old)"),
]


def build_and_run(workdir, src_text):
    srcdir = os.path.join(workdir, "src")
    with open(os.path.join(srcdir, "cuvs_cache_policy.c"), "w") as f:
        f.write(src_text)
    exe = os.path.join(workdir, "t")
    cc = os.environ.get("CC", "cc")
    cmd = [cc, "-std=gnu11", "-w", "-I", srcdir, "-o", exe,
           os.path.join(ROOT, "test", "unit", "test_cache_policy.c"),
           os.path.join(srcdir, "cuvs_cache_policy.c"),
           os.path.join(ROOT, "src", "cuvs_util.c"), "-lm"]
    if sys.platform.startswith("linux"):
        cmd.append("-lrt")
    b = subprocess.run(cmd, capture_output=True, text=True)
    if b.returncode != 0:
        return "BUILD_FAIL", b.stderr.strip().splitlines()[-1:] or [""]
    r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    lines = r.stdout.strip().splitlines()
    first_fail = next((l for l in lines if l.startswith("FAIL")), "")
    return ("PASS" if r.returncode == 0 else "RED"), [first_fail, lines[-1] if lines else ""]


def main():
    original = open(SRC).read()
    work = tempfile.mkdtemp(prefix="mut_cache_policy_")
    try:
        os.makedirs(os.path.join(work, "src"))
        for h in ("cuvs_cache_policy.h", "cuvs_admission_contract.h",
                  "cuvs_util.h", "cuvs_ipc.h"):
            shutil.copy(os.path.join(ROOT, "src", h), os.path.join(work, "src", h))
        status, out = build_and_run(work, original)
        print(f"baseline: {status} {out[-1]}")
        if status != "PASS":
            return 2
        bad = 0
        for name, ck, old, new in MUTATIONS:
            if original.count(old) != 1:
                print(f"SETUP-ERROR {name}: anchor found {original.count(old)}x")
                bad += 1
                continue
            status, out = build_and_run(work, original.replace(old, new, 1))
            ok = status == "RED"
            bad += 0 if ok else 1
            print(f"{'RED ' if ok else 'MISS'} [{ck}] {name}: {out[0] or out[-1]}")
        print(f"mutations: {len(MUTATIONS)}, survived/invalid: {bad}")
        return 1 if bad else 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
