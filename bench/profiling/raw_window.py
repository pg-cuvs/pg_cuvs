#!/usr/bin/env python3
"""Raw-arm GPU breakdown, windowed by the harness's own epoch marks."""
import sqlite3
import sys

REP = "/tmp/pg98_raw.sqlite"
SEARCH_START, SINGLE_END, BATCH_END = (float(x) for x in sys.argv[1:4])

db = sqlite3.connect(REP)
c = db.cursor()
c.execute("select utcEpochNs from TARGET_INFO_SESSION_START_TIME")
utc0, = c.fetchone()
ev = lambda e: int(e * 1e9) - utc0

KIND = {1: "H2D", 2: "D2H", 8: "D2D"}
for label, a, b, units, unit in (
        ("SINGLES", SEARCH_START, SINGLE_END, 300, "search"),
        ("BATCH",   SINGLE_END,   BATCH_END,    7, "dispatch")):
    w0, w1 = ev(a), ev(b)
    c.execute("""select count(*), coalesce(sum(end-start),0)
                 from CUPTI_ACTIVITY_KIND_KERNEL where start>=? and end<=?""", (w0, w1))
    kn, kns = c.fetchone()
    mem = 0
    parts = []
    for k in (1, 2, 8):
        c.execute("""select count(*), coalesce(sum(end-start),0), coalesce(sum(bytes),0)
                     from CUPTI_ACTIVITY_KIND_MEMCPY
                     where copyKind=? and start>=? and end<=?""", (k, w0, w1))
        n, ns, by = c.fetchone()
        mem += ns
        if n:
            parts.append(f"{KIND[k]} n={n} {ns/1e6:.3f}ms {by/2**20:.2f}MiB")
    wall = b - a
    print(f"--- {label}: wall {wall*1e6:.1f} us over {units} {unit}(s)")
    print(f"    kernel {kns/1e6:>9.3f} ms -> {kns/units/1e3:>8.2f} us/{unit}  (n={kn})")
    print(f"    memcpy {mem/1e6:>9.3f} ms -> {mem/units/1e3:>8.2f} us/{unit}   [{'; '.join(parts)}]")
    print(f"    wall   {wall*1e6/units:>9.2f} us/{unit}")
    c.execute("""select s.value, count(*), sum(k.end-k.start)
                 from CUPTI_ACTIVITY_KIND_KERNEL k join StringIds s on s.id=k.demangledName
                 where k.start>=? and k.end<=? group by s.value
                 order by sum(k.end-k.start) desc limit 4""", (w0, w1))
    for name, n, ns in c.fetchall():
        print(f"      {ns/1e6:>8.3f} ms n={n:>5} {name[:60]}")
