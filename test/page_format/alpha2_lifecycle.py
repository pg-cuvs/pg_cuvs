import os
from pathlib import Path
import signal
import subprocess
import sys
import time

db, pgdata, out_dir = sys.argv[1:]
primary_host=os.environ["PGHOST"]
primary_port=os.environ["PGPORT"]
out=Path(out_dir)
replica=Path(pgdata).with_name("standby")
standby_host=replica.with_name("standby-sock")
standby_host.mkdir()
standby_port="55433"
owner_command=["psql","-XqAt","-v","ON_ERROR_STOP=1","-d",db]

def checked(args, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=True, timeout=180, **kwargs).stdout

def sql(text, host=primary_host, port=primary_port):
    return checked(owner_command+["-h",str(host),"-p",port,"-c",text]).strip()

def psend(proc, statement):
    proc.stdin.write(statement+"\n\\echo __page_probe__\n")
    proc.stdin.flush()
    rows=[]
    for line in proc.stdout:
        if line.rstrip()=="__page_probe__":
            return rows
        rows.append(line.rstrip())
    raise RuntimeError("\n".join(rows))

def exact_without_daemon(label, host, port, expected, recovery):
    proc=subprocess.Popen(owner_command+["-h",str(host),"-p",port],
                         stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT,text=True,bufsize=1)
    log=out/f"{label}-strace.stderr"
    trace=out/f"{label}-connect.trace"
    daemon_path=sql("SHOW cuvs.socket_path;")
    tracer=None
    try:
        state=psend(proc,"SELECT pg_backend_pid(), pg_is_in_recovery(), current_setting('cuvs.socket_path');")
        pid, in_recovery, path=state[0].split("|")
        if (in_recovery=="t") != recovery or path!=daemon_path or not Path(path).is_socket():
            raise RuntimeError(f"fixture is not using its live primary daemon socket: {state}")
        with log.open("w") as err:
            tracer=subprocess.Popen(
                ["sudo","-n","timeout","-s","INT","25","strace","-e","trace=connect",
                 "-o",str(trace),"-p",pid],
                start_new_session=True,stdout=subprocess.DEVNULL,stderr=err)
            end=time.monotonic()+10
            while time.monotonic()<end:
                if "attached" in log.read_text():
                    break
                if tracer.poll() is not None:
                    raise RuntimeError(log.read_text())
                time.sleep(0.05)
            else:
                raise RuntimeError("did not attach to the query backend")
            rows=psend(proc,"""
                SET enable_seqscan=off;
                EXPLAIN (COSTS OFF) SELECT id FROM page_lifecycle ORDER BY v <-> '[0,0,0]' LIMIT 3;
                SELECT array_to_string(
                    ARRAY(SELECT id FROM page_lifecycle ORDER BY v <-> '[0,0,0]' LIMIT 3),',');
            """)
            if rows[-1]!=expected or not any("Index Scan using page_lifecycle_idx" in line for line in rows):
                raise RuntimeError(f"wrong execution path or result: {rows}")
            (out/f"{label}-read.txt").write_text("\n".join([state[0],*rows])+"\n")
    finally:
        if tracer is not None and tracer.poll() is None:
            try:
                os.killpg(tracer.pid,signal.SIGINT)
            except PermissionError:
                pass
            tracer.wait(timeout=30)
        proc.stdin.close()
        proc.wait(timeout=10)
    if daemon_path in trace.read_text():
        raise RuntimeError("exact page-format search reached the daemon")
    print(f"{label}: expected rows, CAGRA Index Scan, zero daemon connections",flush=True)

sql("""
    CREATE EXTENSION vector;
    CREATE EXTENSION pg_cuvs;
    CREATE TABLE page_lifecycle (id int, v vector(3));
    INSERT INTO page_lifecycle
      SELECT i,array_fill(i::real,ARRAY[3])::vector FROM generate_series(1,16) i;
    CREATE INDEX page_lifecycle_idx ON page_lifecycle USING cagra(v vector_l2_ops);
""")
backup=checked(["pg_basebackup","-h",primary_host,"-p",primary_port,"-D",str(replica),
                "-X","stream","-R","--checkpoint","fast"])
(out/"basebackup.log").write_text(backup)
same_directory=sql("SHOW cuvs.index_dir;")
try:
    checked(["pg_ctl","-D",str(replica),"-l",str(out/"standby.log"),"-w",
             "-o",f"-k {standby_host} -p {standby_port}","start"])
    if sql("SHOW cuvs.index_dir;",standby_host,standby_port)!=same_directory:
        raise RuntimeError("standby is not sharing the primary index directory")
    exact_without_daemon("recovery",standby_host,standby_port,"1,2,3",True)
finally:
    if (replica/"postmaster.pid").exists():
        checked(["pg_ctl","-D",str(replica),"-m","fast","-w","stop"])

checked(["pg_ctl","-D",pgdata,"-m","fast","-w","stop"])
checked(["pg_ctl","-D",pgdata,"-l",str(out/"postgres.log"),"-o",
         "-c shared_preload_libraries=","-w","start"])
if sql("SHOW shared_preload_libraries;")!="":
    raise RuntimeError("preload is still active")
sql("""
    INSERT INTO page_lifecycle VALUES (99, '[0,0,0]');
    DELETE FROM page_lifecycle WHERE id=1;
""")
sql("VACUUM (INDEX_CLEANUP ON,TRUNCATE OFF) page_lifecycle;")
exact_without_daemon("no-preload",primary_host,primary_port,"99,2,3",False)
sql("CREATE TABLE preload_gate(v vector(3));")
attempt=subprocess.run(owner_command+["-c",
    "CREATE INDEX preload_gate_idx ON preload_gate USING cagra(v vector_l2_ops);"],
    text=True,capture_output=True,timeout=30)
(out/"create-without-preload.txt").write_text(attempt.stdout+attempt.stderr)
if attempt.returncode==0 or "shared_preload_libraries" not in attempt.stderr:
    raise RuntimeError("creating page format without preload did not fail with the hint")
print("no-preload: INSERT and VACUUM worked; CREATE INDEX refused with a hint",flush=True)
