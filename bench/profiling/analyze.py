#!/usr/bin/env python3
"""Per-capture kernel/memcpy totals from the nsys SQLite exports.

The 3.2 GB startup index load and the shutdown save are single multi-hundred-ms
transfers; per-search transfers are microseconds. Splitting at 1 ms separates
them, and the baseline capture (startup + warm-up, no workload) is subtracted so
what remains is the workload's own GPU cost.
"""
import sqlite3
import sys

CAPS = ["pg98_none", "pg98_singles", "pg98_batch"]
res = {}

for lab in CAPS:
    db = sqlite3.connect(f"/tmp/{lab}.sqlite")
    c = db.cursor()
    out = {}
    c.execute("select count(*), coalesce(sum(end-start),0) from CUPTI_ACTIVITY_KIND_KERNEL")
    out["kern_n"], out["kern_ns"] = c.fetchone()
    for tag, cond in (("all", "1=1"), ("big", "(end-start) > 1000000"),
                      ("small", "(end-start) <= 1000000")):
        c.execute("select count(*), coalesce(sum(end-start),0), coalesce(sum(bytes),0) "
                  f"from CUPTI_ACTIVITY_KIND_MEMCPY where {cond}")
        n, ns, by = c.fetchone()
        out[f"mem_{tag}"] = (n, ns, by)
    # per-kernel-name breakdown, biggest first
    c.execute("""select s.value, count(*), sum(k.end-k.start)
                 from CUPTI_ACTIVITY_KIND_KERNEL k
                 join StringIds s on s.id = k.demangledName
                 group by s.value order by sum(k.end-k.start) desc limit 6""")
    out["top"] = c.fetchall()
    res[lab] = out
    db.close()

for lab in CAPS:
    o = res[lab]
    print(f"===== {lab}")
    print(f"  kernels: n={o['kern_n']:>6}  total={o['kern_ns']/1e6:>10.3f} ms")
    for t in ("all", "big", "small"):
        n, ns, by = o[f"mem_{t}"]
        print(f"  memcpy[{t:>5}]: n={n:>5} total={ns/1e6:>10.3f} ms  bytes={by/2**20:>10.1f} MiB")
    for name, n, ns in o["top"]:
        print(f"    {ns/1e6:>9.3f} ms  n={n:>4}  {name[:70]}")

print()
print("===== workload-attributable (capture - baseline) =====")
base = res["pg98_none"]
for lab, n_units, unit in (("pg98_singles", 305, "search"),
                           ("pg98_batch", 6, "dispatch")):
    o = res[lab]
    dk = o["kern_ns"] - base["kern_ns"]
    dm_small = o["mem_small"][1] - base["mem_small"][1]
    dm_small_n = o["mem_small"][0] - base["mem_small"][0]
    dm_small_by = o["mem_small"][2] - base["mem_small"][2]
    print(f"{lab}: over {n_units} {unit}(s)")
    print(f"  kernel   delta = {dk/1e6:>10.3f} ms  ->  {dk/n_units/1e3:>9.2f} us/{unit}")
    print(f"  memcpy   delta = {dm_small/1e6:>10.3f} ms  ->  {dm_small/n_units/1e3:>9.2f} us/{unit}"
          f"   (n={dm_small_n}, {dm_small_by/2**20:.2f} MiB)")
