"""The two-phase case: an in-place TRUNCATE (the table was created in the same transaction) that is prepared
and then committed with COMMIT PREPARED. The drop of the old build is not sent at PREPARE, so only the build record can keep the
old build from answering: the in-place rebuild must have rewritten it as empty. Needs a disposable cluster it may reconfigure and
restart (max_prepared_transactions) and the daemon at the given socket. Run in the CPU CI container:

    python3 -I test/build_identity_prepared.py /tmp/pgr/data /tmp/.s.pg_cuvs /tmp/cuvs_indexes
"""
import subprocess,sys
PGDATA,SOCK,IDX=sys.argv[1:4]
PG='/usr/lib/postgresql/16/bin'
def as_pg(cmd):return ['runuser','-u','postgres','--']+cmd
def sql(s,ok=True):
    r=subprocess.run(as_pg(['psql','-X','-qAt','-v','ON_ERROR_STOP=1','-h','/tmp','-d','postgres']),
                     input=f"SET cuvs.socket_path='{SOCK}';SET cuvs.index_dir='{IDX}';SET cuvs.k=8;"+s,text=True,capture_output=True,timeout=300)
    assert r.returncode==0 or not ok,(s[:80],r.stderr[-500:]);return r.stdout.strip().splitlines()
sql("ALTER SYSTEM SET max_prepared_transactions = 4;")
r=subprocess.run(as_pg([PG+'/pg_ctl','-D',PGDATA,'-l',PGDATA+'/../prepared.log','-w','restart']),capture_output=True,text=True)
assert r.returncode==0,r.stderr
Q="SELECT string_agg(id::text,',' ORDER BY d,id) FROM (SELECT id,v <-> '[1,0,0,0]' d FROM bp ORDER BY v <-> '[1,0,0,0]' LIMIT 5) s;"
failures=[]
def check(ok,what):
    print(('ok ' if ok else 'FAIL ')+what,flush=True)
    if not ok:failures.append(what)
sql("CREATE EXTENSION IF NOT EXISTS vector;CREATE EXTENSION IF NOT EXISTS pg_cuvs;DROP TABLE IF EXISTS bp;"
    "BEGIN;CREATE TABLE bp (id int, v vector(4));INSERT INTO bp SELECT g, ARRAY[g,0,0,0]::real[]::vector FROM generate_series(1,200) g;"
    "CREATE INDEX bp_i ON bp USING cagra (v vector_l2_ops);TRUNCATE bp;"
    "INSERT INTO bp SELECT 1000+g, ARRAY[201-g,0,0,0]::real[]::vector FROM generate_series(1,200) g;PREPARE TRANSACTION 'bp';")
sql("COMMIT PREPARED 'bp';")
plan='\n'.join(sql("SET enable_seqscan=off;EXPLAIN (COSTS OFF) "+Q))
idx=sql("SET enable_seqscan=off;"+Q)[-1]
heap=sql("SET enable_indexscan=off;SET enable_bitmapscan=off;"+Q)[-1]
reason=sql("SELECT coalesce((SELECT last_reason FROM pg_stat_gpu_fallback WHERE index_oid='bp_i'::regclass),'none');")[-1]
check(idx==heap and 'Index Scan' not in plan and reason=='not_current',
      f'after COMMIT PREPARED the heap answers ({idx} vs {heap}, index used: {"Index Scan" in plan}, last_reason {reason})')
sql("REINDEX INDEX bp_i;")
plan='\n'.join(sql("SET enable_seqscan=off;EXPLAIN (COSTS OFF) "+Q))
idx=sql("SET enable_seqscan=off;"+Q)[-1]
check(idx==heap and 'Index Scan' in plan,f'after REINDEX the index answers like the heap ({idx} vs {heap})')
sql("DROP TABLE bp;ALTER SYSTEM RESET max_prepared_transactions;")
if failures:sys.exit('FAIL '+'; '.join(failures))
print('PASS an in-place TRUNCATE that is prepared and committed leaves no build that answers',flush=True)
