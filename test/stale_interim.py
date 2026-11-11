#!/usr/bin/env python3
"""#202 CPU/GPU integration check; requires installed extension and live daemon.
Run as a test superuser: python3 test/stale_interim.py
Uses its own tables; preserves them on failure for diagnosis.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

PSQL = ['psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-v', 'VERBOSITY=verbose']
Q = "'[10,0,0,0,0,0,0,0]'::vector"

def sql(s, must=True):
    r = subprocess.run(PSQL, input=s, text=True, capture_output=True, timeout=90)
    if must:
        assert r.returncode == 0, r.stderr
    return r

def wait_file(p, token):
    for _ in range(300):
        if p.exists() and token in p.read_text():
            return
        time.sleep(.05)
    raise AssertionError('reader did not reach '+token)

def check_stale(r, idx):
    assert r.returncode != 0 and '55000' in r.stderr, r
    assert idx in r.stderr and 'REINDEX' in r.stderr, r.stderr

sql('CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_cuvs;')
with tempfile.TemporaryDirectory(prefix='fix202-') as tmp:
    root=Path(__file__).resolve().parents[1]
    client=Path(tmp)/'mark-stale'
    subprocess.run(['cc','-I'+str(root/'src'),str(root/'test/stale_mark_client.c'),'-o',str(client)],check=True)
    for am in ['cagra', 'flat', 'ivfpq']:
        table = 'fix202_'+am
        idx = table+'_idx'
        n=100000 if am=='cagra' else (5000 if am=='ivfpq' else 1000)
        sql(f"DROP TABLE IF EXISTS {table}; CREATE TABLE {table}(id int, v vector(8)) WITH (autovacuum_enabled=false); INSERT INTO {table} SELECT g,ARRAY[g/1000.0,0,0,0,0,0,0,0]::vector FROM generate_series(1,{n})g; CREATE INDEX {idx} ON {table} USING {am}(v vector_l2_ops); ANALYZE {table};")
        if am=='cagra':
            # A nonempty whitelist with no matching base TID must reach IPC.
            count_sql=f"SELECT search_count FROM pg_stat_gpu_search WHERE index_oid='{idx}'::regclass;"
            count_before=int(sql(count_sql).stdout.strip())
            empty=sql(f"SET cuvs.filter_auto_threshold=0; SET cuvs.stream_bf_selectivity_threshold=0; SELECT count(*) FROM cuvs_filtered_knn('{idx}'::regclass,{Q},ARRAY[9223372036854775807]::bigint[],5);")
            assert empty.stdout.strip()=='0',empty
            assert int(sql(count_sql).stdout.strip())>count_before,'empty result did not reach daemon'
            print('non-stale IPC filtered search: 0 rows, daemon search_count advanced',flush=True)
            before=Path(tmp)/'vac-before';after=Path(tmp)/'vac-after'
            cached=subprocess.Popen(PSQL,stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
            cached.stdin.write(f"SET enable_seqscan=off; SET cuvs.max_stale_fraction=1; SET plan_cache_mode=force_generic_plan; PREPARE p(vector) AS SELECT id FROM {table} ORDER BY v <-> $1 LIMIT 5;\n\o {before}\nEXPLAIN EXECUTE p({Q}); EXECUTE p({Q});\n\o\n");cached.stdin.flush()
            wait_file(before,'Index Scan')
        deleted=15000 if am=='cagra' else 101
        sql(f"DELETE FROM {table} WHERE id<={deleted};")
        gate = 2021100
        # PREPARE inside a reader blocked after planning but before the index scan.
        holder = subprocess.Popen(PSQL, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        holder.stdin.write(f'SELECT pg_advisory_lock({gate});\n'); holder.stdin.flush()
        for _ in range(100):
            if sql(f"SELECT count(*) FROM pg_locks WHERE locktype='advisory' AND objid={gate} AND granted;").stdout.strip()=='1': break
            time.sleep(.05)
        else: raise AssertionError('holder lock missing')
        query = f"WITH gate AS MATERIALIZED(SELECT {Q} AS q,pg_advisory_xact_lock({gate})) SELECT s.id FROM gate CROSS JOIN LATERAL(SELECT id FROM {table} ORDER BY v <-> gate.q LIMIT 5)s"
        assert 'Index Scan' in sql('SET enable_seqscan=off; SET cuvs.max_stale_fraction=1; EXPLAIN '+query+';').stdout, am+' index path not reached'
        env = dict(os.environ, PGAPPNAME='fix202_reader')
        reader = subprocess.Popen(PSQL, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
        reader.stdin.write('SET enable_seqscan=off; SET cuvs.max_stale_fraction=1; SET max_parallel_workers_per_gather=0; '+query+';\n'); reader.stdin.close()
        for _ in range(200):
            if sql("SELECT count(*) FROM pg_locks l JOIN pg_stat_activity a ON a.pid=l.pid WHERE a.application_name='fix202_reader' AND l.locktype='advisory' AND NOT l.granted;").stdout.strip()=='1': break
            time.sleep(.05)
        else: raise AssertionError('reader execution gate missing')
        db=sql("SELECT oid FROM pg_database WHERE datname=current_database();").stdout.strip()
        oid=sql(f"SELECT '{idx}'::regclass::oid;").stdout.strip()
        sock=sql('SHOW cuvs.socket_path;').stdout.strip()
        if am=='cagra':
            sql(f'SET cuvs.max_delta_rows=10000; VACUUM {table};')
            assert sql(f"SELECT stale FROM pg_stat_gpu_search WHERE index_oid='{idx}'::regclass;").stdout.strip()=='t'
            import struct
            index_dir=sql('SHOW cuvs.index_dir;').stdout.strip()
            header=(Path(index_dir)/(db+'_'+oid+'.tombstone')).read_bytes()[:32]
            assert struct.unpack_from('<q',header,8)[0]==10000,'VACUUM cap not reached'
        else:
            subprocess.run([str(client),sock,db,oid],check=True)
        holder.stdin.write(f'SELECT pg_advisory_unlock({gate});\n\q\n'); holder.stdin.flush(); holder.stdin.close(); holder.wait(timeout=10)
        reader.wait(timeout=20)
        result = subprocess.CompletedProcess([],reader.returncode,reader.stdout.read(),reader.stderr.read())
        check_stale(result,idx)
        truth=sql(f'SET enable_indexscan=off; SELECT id FROM {table} ORDER BY v <-> {Q} LIMIT 5;').stdout.split()
        assert sql(f'SELECT id FROM {table} ORDER BY v <-> {Q} LIMIT 5;').stdout.split()==truth
        if am=='cagra':
            cached.stdin.write(f"\o {after}\nEXPLAIN EXECUTE p({Q}); EXECUTE p({Q});\n\o\n\q\n");cached.stdin.flush();cached.stdin.close();cached.wait(timeout=20)
            assert cached.returncode==0,cached.stderr.read()
            output=after.read_text()
            assert 'Seq Scan' in output and 'Index Scan' not in output,output
            assert output.strip().endswith('\n'.join(truth)),output
            print('VACUUM tombstone cap=10000: in-flight 55000; generic plan replanned to CPU truth',flush=True)
        if am=='cagra':
            check_stale(sql(f"SELECT * FROM pg_cuvs_batch_search('{table}'::regclass,ARRAY[{Q}],5);",False),idx)
            check_stale(sql(f"SELECT * FROM cuvs_filtered_knn('{idx}'::regclass,{Q},NULL::tid[],5);",False),idx)
        if am=='cagra':
            assert sql(f"SELECT count(*) FROM cuvs_filtered_knn('{idx}'::regclass,{Q},ARRAY[]::tid[],5);").stdout.strip()=='0'
        sql(f'DROP TABLE {table};')
        print(am, 'in-flight STALE=55000; fresh CPU truth OK',flush=True)

    # A marker persists after abort: invalidate the other backend's generic plan.
    table='fix202_abort';idx=table+'_idx'
    sql(f"CREATE TABLE {table}(id int,v vector(8)); INSERT INTO {table} SELECT g,ARRAY[g/1000.0,0,0,0,0,0,0,0]::vector FROM generate_series(1,1000)g; CREATE INDEX {idx} ON {table} USING flat(v vector_l2_ops); ANALYZE {table};")
    before=Path(tmp)/'before';after=Path(tmp)/'after'
    r=subprocess.Popen(PSQL,stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
    r.stdin.write(f"SET enable_seqscan=off; SET plan_cache_mode=force_generic_plan; PREPARE p(vector) AS SELECT id FROM {table} ORDER BY v <-> $1 LIMIT 5;\n\o {before}\nEXPLAIN EXECUTE p({Q}); EXECUTE p({Q});\n\o\n");r.stdin.flush()
    wait_file(before,'Index Scan')
    sql(f"BEGIN; SET cuvs.max_delta_rows=0; INSERT INTO {table} VALUES(1001,ARRAY[2,0,0,0,0,0,0,0]::vector); ROLLBACK;")
    r.stdin.write(f"\o {after}\nEXPLAIN EXECUTE p({Q}); EXECUTE p({Q});\n\o\n\q\n");r.stdin.flush();r.stdin.close();r.wait(timeout=20)
    assert r.returncode==0,r.stderr.read()
    output=after.read_text()
    assert 'Seq Scan' in output and 'Index Scan' not in output,output
    assert output.strip().endswith('1000\n999\n998\n997\n996'),output
    sql(f'DROP TABLE {table};')
    print('generic plan invalidated across ROLLBACK; CPU truth OK',flush=True)
