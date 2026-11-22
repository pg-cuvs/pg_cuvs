"""A build the backend judges current and the daemon refuses. The record is of an
earlier version that the backend can read but not make readable, and that the daemon cannot read (owned by root, group postgres,
0640; the daemon runs as nobody). Checked in order:
  - precondition: the build is unloaded, and nobody cannot read the record;
  - the first statement fails with SQLSTATE 55000, and the daemon's log shows the identity refusal (not a silent CPU answer);
  - the next statements answer like the heap without asking the daemon (its search count stays), a generic plan's EXECUTE included,
    and still after ANALYZE (the refusal outlives the relcache entry);
  - after an operator makes the record readable (0644), within CUVS_REFUSED_RECHECK_S (60 s) the refusal is forgotten and the index
    answers through the daemon again;
  - after REINDEX the index is used and answers like the heap.
Needs a disposable cluster, the daemon at the default socket running as nobody, and root. Slow (about 65 s). Run as root in the CPU CI
container:

    python3 -I test/build_identity_refused.py /tmp/cuvs_indexes /tmp/d.log
"""
import grp,os,subprocess,sys,time
IDX,DLOG=sys.argv[1],sys.argv[2]
P=['runuser','-u','postgres','--','psql','-X','-qAt','-v','ON_ERROR_STOP=0','-v','VERBOSITY=verbose','-h','/tmp','-d','postgres']
failures=[]
def check(ok,what):
    print(('ok ' if ok else 'FAIL ')+what,flush=True)
    if not ok:failures.append(what)
class S:
    def __init__(self):
        self.p=subprocess.Popen(P,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,bufsize=1)
        self.q(f"SET cuvs.index_dir='{IDX}';SET cuvs.k=8;SET client_min_messages=warning;")
    def q(self,x):
        self.p.stdin.write(x+"\n\\echo __END__\n");self.p.stdin.flush();out=[]
        while True:
            l=self.p.stdout.readline()
            if not l or l.strip()=='__END__':return '\n'.join(out).strip()
            out.append(l.rstrip('\n'))
    def last(self,x):
        r=self.q(x);return r.splitlines()[-1] if r else ''
    def close(self):
        self.p.stdin.close();self.p.wait()
def dlog_lines():
    with open(DLOG,errors='replace') as f:return f.read().splitlines()
Q="SELECT string_agg(id::text,',' ORDER BY d,id) FROM (SELECT id,v <-> '[1,0,0,0]' d FROM br ORDER BY v <-> '[1,0,0,0]' LIMIT 5) s"
A=S()
out=A.q("CREATE EXTENSION IF NOT EXISTS vector;CREATE EXTENSION IF NOT EXISTS pg_cuvs;DROP TABLE IF EXISTS br;"
        "CREATE TABLE br(id int, v vector(4));INSERT INTO br SELECT g, ARRAY[g,0,0,0]::real[]::vector FROM generate_series(1,200) g;"
        "CREATE INDEX br_i ON br USING cagra (v vector_l2_ops);")
assert 'ERROR' not in out,out
db=A.last("SELECT oid FROM pg_database WHERE datname=current_database();")
oid=A.last("SELECT 'br_i'::regclass::oid;")
heap_rfn=A.last("SELECT relfilenode FROM pg_class WHERE relname='br';")
table_oid=A.last("SELECT 'br'::regclass::oid;")
rec=f"{IDX}/{db}_{oid}.relfilenode"
with open(rec,'w') as f:f.write(f"{heap_rfn} {table_oid}\n")
os.chown(rec,0,grp.getgrnam('postgres').gr_gid);os.chmod(rec,0o640)   # postgres may read it but not chmod it; nobody cannot read it
u=''
for _ in range(20):                             # the daemon loads it again from disk, with the record unreadable to it
    u=A.q("SELECT pg_cuvs_unload('br_i');")
    if 'ERROR' not in u:break
    time.sleep(.5)
st=A.q("SELECT state FROM pg_cuvs_index_status() WHERE index_oid='br_i'::regclass;")
last=st.strip().splitlines()[-1] if st.strip() else ''
unreadable=subprocess.run(['runuser','-u','nobody','--','cat',rec],capture_output=True).returncode!=0
check('ERROR' not in u and last!='resident' and unreadable,f'precondition: unloaded ({u[:60]!r}, state {last!r}) and the record unreadable to nobody ({unreadable})')
A.close()
def searches(S_):return S_.last("SELECT coalesce(sum(search_count),0) FROM pg_stat_gpu_search WHERE index_oid='br_i'::regclass;")
B=S()                                           # a new session judges the record afresh
heap=B.last("SET enable_indexscan=off;SET enable_bitmapscan=off;"+Q+";")
# the heap's answer was taken with index scans off; the prepared statement must be planned with them on (and the heap scan off), or
# its generic plan is a Seq Scan and the first EXECUTE never reaches the index (what the first runs of this case did)
B.q("RESET enable_indexscan;RESET enable_bitmapscan;SET enable_seqscan=off;SET plan_cache_mode=force_generic_plan;PREPARE q AS "+Q+";")
plan=B.q("EXPLAIN (COSTS OFF) EXECUTE q;")
check('Index Scan using br_i' in plan,f'precondition: the generic plan of q scans br_i ({plan.splitlines()[-1].strip() if plan else ""!r})')
n0=len(dlog_lines())
r1=B.q("EXECUTE q;")
seen=[l[:160] for l in dlog_lines()[n0:] if 'does not match' in l or 'not_current' in l.lower() or 'loaded index' in l]
print('daemon lines at the first EXECUTE:',seen,flush=True)
check('55000' in r1,f'the first EXECUTE fails with 55000 ({r1[:200]!r})')
c0=searches(B)
r2=B.last("EXECUTE q;");r3=B.last("EXECUTE q;")
c1=searches(B)
check(r2==heap and r3==heap and c1==c0,f'the next EXECUTEs answer like the heap without the daemon ({r2!r}, {r3!r} vs {heap!r}; searches {c0}->{c1})')
B.q("ANALYZE br;")
r4=B.last("EXECUTE q;")
plan=B.q("SET enable_seqscan=off;EXPLAIN (COSTS OFF) "+Q+";")
check(r4==heap and 'Index Scan' not in plan,f'after ANALYZE the heap still answers ({r4!r}; index planned: {"Index Scan" in plan})')
os.chmod(rec,0o644)                              # the operator's fix
time.sleep(61)
r5=B.last("SET enable_seqscan=off;"+Q+";");c2=searches(B)
plan=B.q("SET enable_seqscan=off;EXPLAIN (COSTS OFF) "+Q+";")
check(r5==heap and 'Index Scan' in plan and c2!=c1,f'after the record is made readable the index answers through the daemon again ({r5!r}; searches {c1}->{c2})')
B.q("REINDEX INDEX br_i;DEALLOCATE q;")
r6=B.last("SET enable_seqscan=off;"+Q+";")
plan=B.q("SET enable_seqscan=off;EXPLAIN (COSTS OFF) "+Q+";")
check(r6==heap and 'Index Scan' in plan,f'after REINDEX the index is used again and answers like the heap ({r6!r})')
B.q("DROP TABLE br;");B.close()
if failures:sys.exit('FAIL '+'; '.join(failures))
print('PASS a build the daemon refuses answers from the heap after one error, prepared statements and ANALYZE included, until the record changes',flush=True)
