#!/usr/bin/env python3
"""Source scan for the I3-B daemon invariants (v6 CK-36 (a), CK-37 (a)).

CK-36 (a): every ensure_vram / evict_lru / make_slot / load_index call names
a requester; load_index callers map to LOAD_STARTUP (startup), LOAD_WARMUP
(warmup worker) and LOAD (search side + pg_cuvs_load) and nothing else.
CK-37 (a): every function that assigns an IndexEntry's handle, tids, n_vecs
or n_extended also sets file_current (or gets a fresh 0 through
registry_new_slot / reset_entry_stats); handle_extend clears it after its
memory-only change; save_index sets it; the save skip is decided in exactly
one place and the SIGTERM save path never reads it.

    python3 test/unit/check_i3b_server_source.py [path/to/pg_cuvs_server.c]
"""
import os
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "src/pg_cuvs_server.c"
raw = open(path).read()
# Blank out comments and string literals (newlines kept, so line numbers hold).
src = re.sub(r"/\*.*?\*/|//[^\n]*|\"(?:\\.|[^\"\\])*\"",
             lambda m: re.sub(r"[^\n]", " ", m.group(0)), raw, flags=re.S)
fails = []


def fail(msg):
    fails.append(msg)
    print("FAIL " + msg)


# Function bodies: name -> text (top-level "name(\n...{ ... \n}" blocks).
funcs = {}
for m in re.finditer(r"\n(?:static\s+)?[A-Za-z_][\w \*]*?\n?([A-Za-z_]\w*)\(([^;{]*?)\)\s*\n\{", src):
    name = m.group(1)
    start = m.end()
    depth, i = 1, start
    while depth and i < len(src):
        c = src[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    funcs[name] = src[start:i]

# ---- CK-36 (a): requester argument on every call ----
def calls(fn):
    out = []
    for m in re.finditer(r"\b%s\(" % fn, src):
        line = src.count("\n", 0, m.start()) + 1
        prev = src[max(0, m.start() - 40):m.start()]
        if re.search(r"(static\s+\w+\s*\*?\s*|^)$", prev.split("\n")[-1]) and \
           re.match(r"\s*(int|size_t|static)", prev.split("\n")[-1] or "x"):
            continue
        depth, i = 1, m.end()
        while depth and i < len(src):
            depth += {"(": 1, ")": -1}.get(src[i], 0)
            i += 1
        args = src[m.end():i - 1]
        out.append((line, args, src[m.start() - 30:m.start()]))
    return out


def split_args(a):
    parts, depth, cur = [], 0, ""
    for ch in a:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur.strip())
            cur = ""
        else:
            cur += ch
    parts.append(cur.strip())
    return parts


arity = {"ensure_vram": 3, "evict_lru": 2, "make_slot": 2, "load_index": 3}
load_rq = {}
for fn, n in arity.items():
    for line, args, before in calls(fn):
        if "static" in before.split("\n")[-1] or before.rstrip().endswith("\n") and False:
            continue
        a = split_args(args)
        if a and a[0].startswith(("size_t needed", "int device_id", "uint32_t db_oid")):
            continue   # the definition or prototype itself
        if len(a) != n:
            fail(f"CK-36 (a): {fn}() at line {line} has {len(a)} args (requester missing?)")
            continue
        rq = a[-1]
        if not (rq.startswith("CUVS_RQ_") or rq == "rq"):
            fail(f"CK-36 (a): {fn}() at line {line} passes '{rq}', not a requester")
        if fn == "load_index":
            load_rq.setdefault(rq, []).append(line)

want = {"CUVS_RQ_LOAD_STARTUP": 1, "CUVS_RQ_LOAD_WARMUP": 1, "CUVS_RQ_LOAD": 7}
for rq, n in want.items():
    got = len(load_rq.get(rq, []))
    if got != n:
        fail(f"CK-36 (a): load_index({rq}) call sites {got}, expected {n}: {load_rq.get(rq)}")
extra = set(load_rq) - set(want)
if extra:
    fail(f"CK-36 (a): unexpected load_index requesters {sorted(extra)}")
if "CUVS_RQ_LOAD_STARTUP" not in funcs.get("startup_load_ordered", ""):
    fail("CK-36 (a): startup loads are not LOAD_STARTUP")
if "CUVS_RQ_LOAD_WARMUP" not in funcs.get("warmup_worker_thread", ""):
    fail("CK-36 (a): warmup loads are not LOAD_WARMUP")
if "CUVS_RQ_LOAD_STARTUP" in funcs.get("handle_search", "") or \
   "CUVS_RQ_LOAD_WARMUP" in funcs.get("handle_search", ""):
    fail("CK-36 (a): search side uses a startup/warmup requester")

# Per-handler requester (review BLOCK 4): every build path names its own
# BUILD_* kind on every admission, slot and post-admission OOM retry call,
# and has at least one such call (a removed call is also a defect).
EXPECT = {
    "admit_build_cagra":   ("CUVS_RQ_BUILD_CAGRA", ("ensure_vram",)),
    "handle_build":        ("CUVS_RQ_BUILD_CAGRA", ("evict_lru",)),          # OOM retry
    "handle_build_multi":  ("CUVS_RQ_BUILD_CAGRA", ("evict_lru",)),          # OOM retry
    "finish_build_commit": ("CUVS_RQ_BUILD_CAGRA", ("make_slot",)),          # slot
    "build_sharded":       ("CUVS_RQ_BUILD_SHARD", ("ensure_vram", "make_slot")),
    "handle_build_flat":   ("CUVS_RQ_BUILD_FLAT",  ("make_slot",)),
    "handle_build_ivfpq":  ("CUVS_RQ_BUILD_IVFPQ", ("ensure_vram", "make_slot")),
}
for fname, (want_rq, fns) in EXPECT.items():
    body = funcs.get(fname)
    if body is None:
        fail(f"CK-36 (b): build handler {fname} not found (scan out of date)")
        continue
    for fn in fns:
        found = 0
        for m in re.finditer(r"\b%s\(" % fn, body):
            depth, i = 1, m.end()
            while depth and i < len(body):
                depth += {"(": 1, ")": -1}.get(body[i], 0)
                i += 1
            rq = split_args(body[m.end():i - 1])[-1]
            found += 1
            if rq != want_rq:
                fail(f"CK-36 (b): {fname} calls {fn}() with {rq}, expected {want_rq}")
        if not found:
            fail(f"CK-36 (b): {fname} has no {fn}() call (expected requester {want_rq})")
if "admit_build_cagra(" not in funcs.get("handle_build", "") or \
   "admit_build_cagra(" not in funcs.get("handle_build_multi", ""):
    fail("CK-36 (b): CAGRA builds do not admit through admit_build_cagra")

# ---- CK-37 (a): I-FC assignments ----
assign = re.compile(r"\b(e|existing)->(handle|tids|n_vecs|n_extended)\s*(\+?=)(?!=)")
for name, body in funcs.items():
    if not assign.search(body):
        continue
    fresh = "registry_new_slot()" in body or "reset_entry_stats(" in body or "memset(e, 0" in body
    if "file_current" not in body and not fresh:
        fail(f"CK-37 (a): {name} assigns graph/tids/n_vecs/n_extended but never sets file_current")

# CAGRA regions must set 1 after their reset (reset_entry_stats zeroes it).
fbc = funcs.get("finish_build_commit", "")
if len(re.findall(r"(e|existing)->file_current = (1|strcmp\(save_dir, g_index_dir\) == 0);", fbc)) != 2:
    fail("CK-37 (a): finish_build_commit does not set file_current = 1 on both "
         "the REINDEX and the new-entry branch")
li = funcs.get("load_index", "")
mr, mf = li.rfind("reset_entry_stats(e);"), li.find("e->file_current = handle != NULL;")
if mf < 0 or mf < mr:
    fail("CK-37 (a): load_index does not set file_current after reset_entry_stats")

ext = funcs.get("handle_extend", "")
m1 = re.search(r"e->n_extended \+= n_new;", ext)
m2 = re.search(r"e->file_current = 0;", ext)
if not (m1 and m2 and m2.start() > m1.start()):
    fail("CK-37 (a): handle_extend does not clear file_current after the memory-only change")
if "e->file_current = 1;" not in funcs.get("save_index", ""):
    fail("CK-37 (a): save_index success does not set file_current")
skip_sites = len(re.findall(r"if \(!e->file_current\)", src))
if skip_sites != 1 or "if (!e->file_current)" not in funcs.get("evict_entry", ""):
    fail(f"CK-37 (a): save skip decided in {skip_sites} places (want exactly evict_entry)")
for name in ("graceful_shutdown",):
    if "file_current" in funcs.get(name, ""):
        fail(f"CK-37 (a): {name} reads file_current (SIGTERM must always save)")
if "graceful_shutdown" not in funcs:
    fail("CK-37 (a): SIGTERM save function not found (scan out of date)")

# r5: the RMM adaptor raises the peak only after the upstream allocation
# succeeded (a failed allocation must not leave a peak).
wpath = os.path.join(os.path.dirname(os.path.abspath(path)), "cuvs_wrapper.cu")
if os.path.exists(wpath):
    w = open(wpath).read()
    da = w[w.find("do_allocate("):w.find("do_deallocate(")]
    ia, ic = da.find("upstream_.allocate("), da.find("cuvs_peak_meter_commit(")
    if "cuvs_peak_meter_alloc(" in da or ia < 0 or ic < 0 or ic < ia:
        fail("peak meter: do_allocate updates the peak before the upstream allocation succeeds")
else:
    fail("peak meter: cuvs_wrapper.cu not found next to the server source")

print(f"check_i3b_server_source: {'OK' if not fails else str(len(fails)) + ' failure(s)'}")
sys.exit(1 if fails else 0)
