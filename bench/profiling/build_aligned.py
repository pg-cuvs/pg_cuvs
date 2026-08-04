#!/usr/bin/env python3
"""Build decomposition aligned to the CREATE INDEX wall-clock window.

nsys stores TARGET_INFO_SESSION_START_TIME.utcEpochNs alongside a systemClockNs
anchor, and event timestamps are ns on that same system clock. The shell printed
the epoch immediately before and after CREATE INDEX, so the statement's window
maps onto the capture exactly instead of being inferred from poll counts.
"""
import sqlite3
import sys

REP = sys.argv[1] if len(sys.argv) > 1 else "/tmp/pg98_build2.sqlite"
E_BEFORE = float(sys.argv[2])
E_AFTER = float(sys.argv[3])

db = sqlite3.connect(REP)
c = db.cursor()
c.execute("select utcEpochNs, systemClockNs from TARGET_INFO_SESSION_START_TIME")
utc0, sys0 = c.fetchone()

# event timestamps are ns since session start; epoch(ev) = utc0 + ev
def to_ev(epoch_s):
    return int(epoch_s * 1e9) - utc0

w0, w1 = to_ev(E_BEFORE), to_ev(E_AFTER)
print(f"session utcEpoch = {utc0/1e9:.3f}")
print(f"CREATE INDEX window in capture time: t+{w0/1e9:.3f}s .. t+{w1/1e9:.3f}s "
      f"({(w1-w0)/1e9:.3f} s wall)")

c.execute("""select count(*), coalesce(sum(end-start),0), min(start), max(end)
             from CUPTI_ACTIVITY_KIND_KERNEL where start >= ? and end <= ?""", (w0, w1))
kn, kns, kmin, kmax = c.fetchone()

KIND = {1: "H2D", 2: "D2H", 8: "D2D"}
mem_ns = 0
print("\nmemcpy inside CREATE INDEX window:")
for k in (1, 2, 8):
    c.execute("""select count(*), coalesce(sum(end-start),0), coalesce(sum(bytes),0)
                 from CUPTI_ACTIVITY_KIND_MEMCPY
                 where copyKind=? and start >= ? and end <= ?""", (k, w0, w1))
    n, ns, by = c.fetchone()
    mem_ns += ns
    if n:
        print(f"  {KIND[k]:>4}: n={n:>6}  {ns/1e6:>9.3f} ms  {by/2**20:>9.1f} MiB")

c.execute("""select count(*), coalesce(sum(end-start),0) from CUPTI_ACTIVITY_KIND_MEMSET
             where start >= ? and end <= ?""", (w0, w1))
sn, sns = c.fetchone()

wall = (w1 - w0) / 1e9
gpu_span = (kmax - kmin) / 1e9 if kn else 0.0
gpu_busy = (kns + mem_ns + sns) / 1e9
print(f"\nCREATE INDEX wall        : {wall:>8.3f} s")
print(f"  GPU activity span      : {gpu_span:>8.3f} s  (first kernel -> last kernel)")
print(f"    kernel               : {kns/1e9:>8.3f} s   n={kn}")
print(f"    memcpy               : {mem_ns/1e9:>8.3f} s")
print(f"    memset               : {sns/1e9:>8.3f} s   n={sn}")
print(f"    = GPU busy           : {gpu_busy:>8.3f} s  ({gpu_busy/wall*100:.1f}% of build wall)")
print(f"    GPU idle within span : {gpu_span-gpu_busy:>8.3f} s")
print(f"  non-GPU (PG backend +")
print(f"    IPC + host-side cuVS): {wall-gpu_span:>8.3f} s  ({(wall-gpu_span)/wall*100:.1f}% of build wall)")

print("\ntop kernels in window:")
c.execute("""select s.value, count(*), sum(k.end-k.start)
             from CUPTI_ACTIVITY_KIND_KERNEL k join StringIds s on s.id=k.demangledName
             where k.start >= ? and k.end <= ?
             group by s.value order by sum(k.end-k.start) desc limit 8""", (w0, w1))
for name, n, ns in c.fetchall():
    print(f"  {ns/1e6:>9.3f} ms n={n:>6}  {name[:66]}")
