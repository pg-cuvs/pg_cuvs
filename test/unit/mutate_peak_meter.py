#!/usr/bin/env python3
"""CK-34 mutations for src/cuvs_peak_meter.c: each must turn test_peak_meter RED."""
import os, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "src", "cuvs_peak_meter.c")
M = [
    ("off ignored (peak updated when off)",
     "    if (!ok(dev) || !atomic_load_explicit(&g_enabled, memory_order_relaxed))\n        return;",
     "    if (!ok(dev))\n        return;"),
    ("reset missing", "    if (ok(dev))\n        atomic_store_explicit(&g_peak[dev],", "    if (0)\n        atomic_store_explicit(&g_peak[dev],"),
    ("race: load/store instead of CAS",
     "    while (level > prev &&\n           !atomic_compare_exchange_weak_explicit(&g_peak[dev], &prev, level,\n                                                  memory_order_relaxed,\n                                                  memory_order_relaxed))\n        ;",
     "    if (level > prev)\n        atomic_store_explicit(&g_peak[dev], level, memory_order_relaxed);"),
]
def run(text):
    d = tempfile.mkdtemp()
    p = os.path.join(d, "m.c"); open(p, "w").write(text)
    exe = os.path.join(d, "t")
    b = subprocess.run(["cc", "-std=gnu11", "-O2", "-I", os.path.join(ROOT, "src"), "-o", exe,
                        os.path.join(ROOT, "test", "unit", "test_peak_meter.c"), p, "-lpthread"],
                       capture_output=True, text=True)
    if b.returncode:
        return "BUILD_FAIL", b.stderr[-200:]
    reds = 0; first = ""
    for _ in range(5):   # the race mutation is probabilistic: any RED of 5 counts
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            reds += 1
            first = first or next((l for l in r.stdout.splitlines() if l.startswith("FAIL")), "")
    return ("RED" if reds else "PASS"), f"{reds}/5 red {first}"
orig = open(SRC).read()
st, info = run(orig); print("baseline:", st, info)
bad = 0 if st == "PASS" else 1
for name, old, new in M:
    if orig.count(old) != 1:
        print("SETUP-ERROR", name); bad += 1; continue
    st, info = run(orig.replace(old, new, 1))
    bad += 0 if st == "RED" else 1
    print(f"{'RED ' if st == 'RED' else 'MISS'} {name}: {info}")
print(f"mutations: {len(M)}, survived/invalid: {bad}")
sys.exit(1 if bad else 0)
