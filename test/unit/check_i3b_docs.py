#!/usr/bin/env python3
"""I3-B parts of CK-19 (doc wording) and CK-27 (defaults vs source).

CK-19: the GPU memory operations chapter of docs/reference.md carries the
REINDEX fixed sentence (three elements + the pinned refusal), the sidecar
definition, the observation names, the four INSERT entry routes and the
compaction case, the 55000 symptom, the unload warning string and the two
limits of pg_cuvs_gpu_search_stats; it never names internal tracks, merge
timing or EV- labels.
CK-27: every flag default in the § 8.2 knob table equals the source macro,
and the knob table and the daemon's flag list are the same set.

    python3 test/unit/check_i3b_docs.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
doc = open(os.path.join(ROOT, "docs", "reference.md")).read()
hdr = open(os.path.join(ROOT, "src", "cuvs_cache_policy.h")).read()
pol = open(os.path.join(ROOT, "src", "cuvs_cache_policy.c")).read()
srv = open(os.path.join(ROOT, "src", "pg_cuvs_server.c")).read()
fails = []


def fail(m):
    fails.append(m)
    print("FAIL " + m)


i = doc.find("## 8. GPU memory operations")
if i < 0:
    fail("chapter '## 8. GPU memory operations' missing")
ch = doc[i:] if i >= 0 else ""

need = {
    "sidecar definition": "*sidecar* means only the `.delta` and `.tombstone` files",
    "REINDEX: old copy may be unloaded": "REINDEX may unload the existing\ncopy",
    "REINDEX: queries may be slower": "queries on it may be slower meanwhile",
    "REINDEX: pin or room": "pin it with `pg_cuvs_pin` or keep enough free room for both copies",
    "pinned REINDEX refused": "A pinned\nindex's REINDEX is refused when both copies do not fit",
    "reclaim_exempt": "reclaim_exempt",
    "reclaim_skip_sidecar": "reclaim_skip_sidecar",
    "extend_count": "extend_count",
    "route: another index needs room": "when another\nindex needs the room",
    "route: build makes room": "when a build makes room",
    "route: manual unload": "on manual\n`pg_cuvs_unload`",
    "route: daemon stop": "when the daemon stops",
    "compaction case": "Compaction is separate",
    "55000 symptom": "SQLSTATE 55000",
    "unload warning": "unloaded; warning: delta or tombstone file present, GPU path switches to CPU replan until REINDEX",
    "search_stats limits": "only the current database and at most 64 rows",
    "SIGHUP status": "SIGHUP re-reading of these values is **not implemented**",
    "first rejection not prevented": "it cannot prevent the first rejection",
}
for what, text in need.items():
    if text.replace("\n", " ") not in ch.replace("\n", " "):
        fail(f"CK-19: chapter lacks {what!r}")
for bad in ("I2b", "ADR-089", "merges", "EV-", "머지 때", "백로그", "backlog"):
    if bad in ch:
        fail(f"CK-19: chapter names {bad!r}")

# CK-27: knob table defaults vs source.
rows = dict(re.findall(r"^\| `--([a-z-]+)` \| [^|]+ \| `([^`]+)` \|", ch, flags=re.M))
macros = {
    "evict-min-idle-s": "CUVS_EVICT_MIN_IDLE_S_DEFAULT",
    "evict-hit-bonus-s": "CUVS_EVICT_HIT_BONUS_S_DEFAULT",
    "evict-hit-cap": "CUVS_EVICT_HIT_CAP_DEFAULT",
    "reclaim-high-pct": "CUVS_RECLAIM_HIGH_PCT_DEFAULT",
    "reclaim-low-pct": "CUVS_RECLAIM_LOW_PCT_DEFAULT",
    "reclaim-tick-s": "CUVS_RECLAIM_TICK_S_DEFAULT",
    "pin-max-pct": "CUVS_PIN_MAX_PCT_DEFAULT",
    "reclaim-free-max-pct": "CUVS_RECLAIM_FREE_MAX_PCT_DEFAULT",
    "reclaim-free-window-min": "CUVS_RECLAIM_FREE_WINDOW_MIN_DEFAULT",
    "reclaim-floor-pct": "CUVS_RECLAIM_FLOOR_PCT_DEFAULT",
}
for flag, macro in macros.items():
    m = re.search(r"#define %s\s+(\d+)" % macro, hdr)
    if not m:
        fail(f"CK-27: macro {macro} not found")
    elif rows.get(flag) != m.group(1):
        fail(f"CK-27: --{flag} documented {rows.get(flag)!r}, source {m.group(1)}")
fixed = {"build-evict-scope": "none", "reclaim-mode": "headroom", "reclaim-free-bytes": "0",
         "load-soft-evict": "on", "reindex-self-replace": "on", "usage-stats-file": "on",
         "rmm-peak-meter": "on"}
src_default = {
    "build-evict-scope": "p->scope.kind  = CUVS_SCOPE_NONE;" in pol,
    "reclaim-mode": "p->mode        = CUVS_RECLAIM_HEADROOM;" in pol,
    "reclaim-free-bytes": "p->f_fixed     = 0;" in pol,
    "load-soft-evict": "p->soft_evict  = 1;" in pol,
    "reindex-self-replace": "p->self_replace = 1;" in pol,
    "usage-stats-file": "p->usage_file  = 1;" in pol,
    "rmm-peak-meter": "p->peak_meter  = 1;" in pol,
}
for flag, val in fixed.items():
    if rows.get(flag) != val:
        fail(f"CK-27: --{flag} documented {rows.get(flag)!r}, expected {val!r}")
    if not src_default[flag]:
        fail(f"CK-27: source default of --{flag} is not {val!r}")
# Same set: documented flags == flags the daemon accepts.
known = set(re.findall(r'\{ "([a-z-]+)",\s+\d', pol)) | {
    "reclaim-mode", "build-evict-scope", "load-soft-evict", "reindex-self-replace",
    "usage-stats-file", "rmm-peak-meter"}
if set(rows) != known:
    fail(f"CK-27: knob table vs daemon flags differ: doc-only={sorted(set(rows) - known)} "
         f"source-only={sorted(known - set(rows))}")

print(f"check_i3b_docs: {'OK' if not fails else str(len(fails)) + ' failure(s)'}")
sys.exit(1 if fails else 0)
