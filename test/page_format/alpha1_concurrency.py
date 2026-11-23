import subprocess
import sys
import time

db=sys.argv[1]
command=["psql","-XqAt","-v","ON_ERROR_STOP=1","-d",db]
def execute(sql):
    return subprocess.check_output(command+["-c",sql],text=True).strip()

execute("""
    CREATE EXTENSION IF NOT EXISTS pageinspect;
    CREATE TABLE page_race(id int PRIMARY KEY,v vector);
    CREATE INDEX page_race_idx ON page_race USING cagra(v vector_l2_ops);
""")
guard=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
workers=[]
try:
    guard.stdin.write("SELECT pg_advisory_lock(7492);\n\\echo locked\n")
    guard.stdin.flush()
    while guard.stdout.readline().strip()!="locked":
        if guard.poll() is not None:
            raise RuntimeError("test lock session exited")
    for dim, vector in ((2,"[1,2]"),(3,"[1,2,3]")):
        workers.append((dim,vector,subprocess.Popen(command+["-c",
            f"BEGIN; SELECT pg_advisory_xact_lock_shared(7492); INSERT INTO page_race VALUES ({dim},'{vector}'); COMMIT;"],
            text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)))
    deadline=time.monotonic()+20
    while time.monotonic()<deadline:
        if execute("SELECT count(*) FROM pg_locks WHERE locktype='advisory' AND objid=7492 AND NOT granted;")=="2":
            break
        time.sleep(0.025)
    else:
        raise RuntimeError("both INSERT connections did not reach the start gate")
    guard.stdin.write("SELECT pg_advisory_unlock(7492);\n")
    guard.stdin.flush()
    winner=[]
    for dim,vec,p in workers:
        stdout,stderr=p.communicate(timeout=30)
        if not p.returncode:
            winner.append((dim,vec))
        elif "expected" not in stderr or "dimensions" not in stderr:
            raise RuntimeError(f"INSERT failed for an unexpected reason: {stdout} {stderr}")
    if len(winner)!=1:
        raise RuntimeError(f"expected one successful dimension, found {winner}")
finally:
    guard.stdin.close()
    guard.wait(timeout=30)
    for _,_,p in workers:
        if p.poll() is None:
            p.terminate()
            p.communicate(timeout=5)

dim,vec=winner[0]
if execute(f"SET enable_seqscan=off; SELECT id FROM page_race ORDER BY v <-> '{vec}' LIMIT 1;")!=str(dim):
    raise RuntimeError("winning tuple not returned")
execute(f"INSERT INTO page_race SELECT g,'{vec}'::vector FROM generate_series(100,1499) g;")
actual=execute("""
    SELECT (page_header(get_raw_page('page_race_idx',1))).lower,
           (page_header(get_raw_page('page_race_idx',3))).lower,
           (SELECT count(*) FROM page_race);
""")
if actual!="8160|294|1401":
    raise RuntimeError(f"pending rollover changed positions: {actual}")
print(f"one dimension won ({dim}); rollover {actual}")
