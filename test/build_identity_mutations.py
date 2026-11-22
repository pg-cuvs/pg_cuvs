"""each planted defect of the build identity rule must turn build_identity (pg_regress), build_identity_cross (isolation),
test_build_identity_daemon or build_identity_refused.py RED. Run as root in the CPU CI container from the repository root, after the tree's own run of both is green:

    python3 -I test/build_identity_mutations.py

Each mutant: a copy of the tree with one edit, built with the CPU shim, a fresh cluster and daemon, both tests; RED means at least
one of them failed. A mutant that survives is a part of the rule that no test protects."""
import os,shutil,subprocess,sys,tempfile,time
from pathlib import Path
ROOT=Path.cwd()
MUTANTS={
 # the daemon answers any build: the planner's record check keeps SQL from reaching it, test_build_identity_daemon shows it
 'daemon_identity_check_skipped':('src/pg_cuvs_server.c',
     'return e->index_rfn != 0 && e->index_rfn == cmd->index_relfilenode;','return 1;'),
 # pg_cuvs_batch_search does not send the index's identity: the daemon refuses it after REINDEX
 'batch_search_sends_no_identity':('src/pg_cuvs.c',
     '            cuvs_identity_oid(index_oid);\n            rc = cuvs_ipc_search_batch(','            rc = cuvs_ipc_search_batch('),
 # the planner takes any build: the daemon refuses the scan, which errors instead of answering from the heap
 'planner_gate_skipped':('src/pg_cuvs.c',
     '    CuvsIndexAmCache *c = cuvs_index_amcache(indexRel);\n\n    if (c->identity_checked && c->identity_reason == CUVS_ID_DAEMON_REFUSED','    CuvsIndexAmCache *c = cuvs_index_amcache(indexRel);\n    return true;\n    if (c->identity_checked && c->identity_reason == CUVS_ID_DAEMON_REFUSED'),
 # the record is left 0600 by the backend's umask: the daemon (nobody here) cannot read it, so a build loaded again from disk has no
 # identity and the scan errors (build_identity_legacy L0, L1)
 'record_not_shared':('src/pg_cuvs.c',
     '        (void) fchmod(fileno(f), 0644);','        ;'),
 # a record of an earlier version is never accepted: the untouched index answers from the heap (build_identity_legacy L1)
 'legacy_record_rejected':('src/pg_cuvs.c',
     '    if (n == 2)\n    {','    if (n == -2)\n    {'),
 # the daemon does not read the record again for an entry that knows nothing of its build (L1: loaded before the record was readable)
 'daemon_no_reread':('src/pg_cuvs_server.c',
     '    if (e->heap_rfn == 0 && e->index_rfn == 0 && cmd->heap_relfilenode != 0)\n','    if (0)\n'),
 # a refusal of a build this backend judges current is not remembered past the relcache entry: after ANALYZE the statement fails
 # again (build_identity_refused.py)
 'refusal_not_remembered':('src/pg_cuvs.c',
     '    for (int i = 0; i < cuvs_n_refused; i++)\n    {\n        struct stat st;','    return false;\n    for (int i = 0; i < cuvs_n_refused; i++)\n    {\n        struct stat st;'),
 # the daemon refuses a build loaded with a record of an earlier version: the accepted index errors (L1, unit test)
 'daemon_legacy_rejected':('src/pg_cuvs_server.c',
     '        return cmd->heap_relfilenode != 0 && e->heap_rfn == cmd->heap_relfilenode;','        return 0;'),
 # the daemon does not compare the heap relfilenode on an export: a build of a rewritten heap is exported (unit test)
 'daemon_heap_not_compared':('src/pg_cuvs_server.c',
     '    if (strict && (cmd->heap_relfilenode == 0 || e->heap_rfn != cmd->heap_relfilenode))\n        return 0;','    if (0)\n        return 0;'),
}
PG='/usr/lib/postgresql/16/bin'
def sh(cmd,cwd,timeout=1800,env=None):
    return subprocess.run(cmd,cwd=cwd,shell=True,capture_output=True,text=True,timeout=timeout,env=env)
def run_tree(tree):
    """Build tree, start a cluster and the daemon, run both tests; returns (ok, summary)."""
    env=dict(os.environ,PATH=PG+':'+os.environ['PATH'],PGHOST='/tmp',PGUSER='postgres')
    b=sh('make PGCUVS_CPU_SHIM=1 -j4 && make PGCUVS_CPU_SHIM=1 install && make PGCUVS_CPU_SHIM=1 server && make install-server',tree,env=env)
    if b.returncode:return False,'build failed: '+b.stderr[-400:]
    sh(f'rm -rf /tmp/bim; install -d -o postgres -g postgres /tmp/bim; mkdir -p /tmp/cuvs_indexes /tmp/cuvs_bi4; rm -rf /tmp/cuvs_indexes/* /tmp/cuvs_bi4/*; chmod 0777 /tmp/cuvs_indexes /tmp/cuvs_bi4',tree)
    sh(f'runuser -u postgres -- {PG}/initdb -D /tmp/bim/data -A trust',tree)
    sh(f'runuser -u postgres -- {PG}/pg_ctl -D /tmp/bim/data -l /tmp/bim/pg.log -o "-k /tmp -c shared_preload_libraries=pg_cuvs -c cuvs.index_dir=/tmp/cuvs_indexes" -w start',tree)
    d=subprocess.Popen(['runuser','-u','nobody','--',PG+'/pg_cuvs_server','--socket','/tmp/.s.pg_cuvs','--index-dir','/tmp/cuvs_indexes'],stdout=open('/tmp/bim-daemon.log','w'),stderr=subprocess.STDOUT)
    try:
        for _ in range(300):
            if os.path.exists('/tmp/.s.pg_cuvs'):break
            time.sleep(.1)
        os.chmod('/tmp/.s.pg_cuvs',0o666)
        r1=sh('make PGCUVS_CPU_SHIM=1 installcheck REGRESS="build_identity build_identity_legacy"',tree,env=env)
        r2=sh('make PGCUVS_CPU_SHIM=1 installcheck-isolation ISOLATION=build_identity_cross',tree,env=env)
        r3=sh('make -B test-build-identity-daemon',tree,env=env)   # -B: the copied tree holds the original's binary
        r4=sh('python3 -I test/build_identity_refused.py /tmp/cuvs_indexes /tmp/bim-daemon.log',tree,env=env,timeout=600)
        return (r1.returncode==0 and r2.returncode==0 and r3.returncode==0 and r4.returncode==0,
                f'regress={r1.returncode} isolation={r2.returncode} daemon_unit={r3.returncode} refused={r4.returncode}')
    finally:
        d.terminate();d.wait(timeout=60)
        sh(f'runuser -u postgres -- {PG}/pg_ctl -D /tmp/bim/data -m fast stop',tree)
        sh('rm -f /tmp/.s.pg_cuvs',tree)
ok,why=run_tree(str(ROOT))
print('BASE',('green' if ok else 'RED'),why,flush=True)
if not ok:sys.exit('baseline not green')
survived=[]
for name,(f,old,new) in MUTANTS.items():
    d=Path(tempfile.mkdtemp(prefix='bim-'));t=d/'tree'
    shutil.copytree(ROOT,t,symlinks=True,ignore=shutil.ignore_patterns('*.o','*.bc','*.so','results','output_iso'))
    s=(t/f).read_text()
    assert s.count(old)==1,(name,old[:60],s.count(old))
    (t/f).write_text(s.replace(old,new))
    ok,why=run_tree(str(t))
    print(name,('survived' if ok else 'RED'),why,flush=True)
    if ok:survived.append(name)
    shutil.rmtree(d,ignore_errors=True)
if survived:sys.exit('mutants survived: '+', '.join(survived))
print('PASS every planted defect of the build identity rule turns a test RED',flush=True)
