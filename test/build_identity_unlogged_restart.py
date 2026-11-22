"""the index of an UNLOGGED table after a crash restart. A crash resets unlogged relations to empty without a new
relfilenode, so the relfilenode alone would call the old build current; the build record also keeps the postmaster start time, and
a build from before the running postmaster is not used. Needs a running cluster it may restart (PGDATA of a disposable cluster,
started by this user or with runuser as below) and the daemon at the given socket. Run in the CPU CI container:

    python3 -I test/build_identity_unlogged_restart.py /tmp/pgr/data /tmp/.s.pg_cuvs /tmp/cuvs_indexes

Old rows: id g, v [g,0,0,0]; after the crash restart new rows are inserted in reverse (id 1000+g, v [201-g,0,0,0]), so the old
build's TIDs name other rows. The index path must answer like the heap and pg_stat_gpu_fallback must say not_current; after REINDEX
the index is used again and answers like the heap."""
import subprocess,sys,time
PGDATA,SOCK,IDX=sys.argv[1:4]
PG='/usr/lib/postgresql/16/bin'
def as_pg(cmd):return ['runuser','-u','postgres','--']+cmd
def sql(s):
    r=subprocess.run(as_pg(['psql','-X','-qAt','-v','ON_ERROR_STOP=1','-h','/tmp','-d','postgres']),
                     input=f"SET cuvs.socket_path='{SOCK}';SET cuvs.index_dir='{IDX}';SET cuvs.k=8;"+s,text=True,capture_output=True,timeout=300)
    assert r.returncode==0,(s[:80],r.stderr[-500:]);return r.stdout.strip().splitlines()
Q="SELECT string_agg(id::text,',' ORDER BY d,id) FROM (SELECT id,v <-> '[1,0,0,0]' d FROM bu ORDER BY v <-> '[1,0,0,0]' LIMIT 5) s;"
def case():
    plan='\n'.join(sql("SET enable_seqscan=off;EXPLAIN (COSTS OFF) "+Q))
    idx=sql("SET enable_seqscan=off;"+Q)[-1]
    heap=sql("SET enable_indexscan=off;SET enable_bitmapscan=off;"+Q)[-1]
    reason=sql("SELECT coalesce((SELECT last_reason FROM pg_stat_gpu_fallback WHERE index_oid='bu_i'::regclass),'none');")[-1]
    return 'Index Scan' in plan,idx,heap,reason
failures=[]
def check(ok,what):
    print(('ok ' if ok else 'FAIL ')+what,flush=True)
    if not ok:failures.append(what)
sql("CREATE EXTENSION IF NOT EXISTS vector;CREATE EXTENSION IF NOT EXISTS pg_cuvs;DROP TABLE IF EXISTS bu;"
    "CREATE UNLOGGED TABLE bu (id int, v vector(4));INSERT INTO bu SELECT g, ARRAY[g,0,0,0]::real[]::vector FROM generate_series(1,200) g;"
    "CREATE INDEX bu_i ON bu USING cagra (v vector_l2_ops);CHECKPOINT;")
used,idx,heap,_=case()
check(used and idx==heap,f'before the crash the index is used and answers like the heap ({used}, {idx} vs {heap})')
r=subprocess.run(as_pg([PG+'/pg_ctl','-D',PGDATA,'-m','immediate','stop']),capture_output=True,text=True)
assert r.returncode==0,r.stderr
r=subprocess.run(as_pg([PG+'/pg_ctl','-D',PGDATA,'-l',PGDATA+'/../restart.log','-w','start']),capture_output=True,text=True)
assert r.returncode==0,r.stderr
time.sleep(1)
n=sql("SELECT count(*) FROM bu;")[-1]
check(n=='0',f'the crash restart reset the unlogged table ({n} rows)')
sql("INSERT INTO bu SELECT 1000+g, ARRAY[201-g,0,0,0]::real[]::vector FROM generate_series(1,200) g;")
used,idx,heap,reason=case()
check(not used and idx==heap and reason=='not_current',f'after the restart the heap answers ({used}, {idx} vs {heap}, last_reason {reason})')
sql("REINDEX INDEX bu_i;")
used,idx,heap,_=case()
check(used and idx==heap,f'after REINDEX the index is used again and answers like the heap ({used}, {idx} vs {heap})')
sql("DROP TABLE bu;")
if failures:sys.exit('FAIL '+'; '.join(failures))
print('PASS an unlogged index from before a crash restart answers from the heap until REINDEX',flush=True)
