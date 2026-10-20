#!/usr/bin/env python3
import argparse,csv,hashlib,importlib.util,json,platform,sys,time
from pathlib import Path
assert platform.node()=='brev-v6nc08sjs'
p=argparse.ArgumentParser();p.add_argument('--pool',type=int,required=True);p.add_argument('--k',type=int,required=True);p.add_argument('--label',required=True);a=p.parse_args()
root=Path('/home/ubuntu/index-accel-validation');repo=Path('/home/ubuntu/pg_cuvs')
spec=importlib.util.spec_from_file_location('helper',root/'feature_gpu_check_smoke_frozen.py');helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
sys.path.insert(0,str(repo/'bench/cuvs_bench_backend'))
import numpy as np
from pg_engine import PgEngine,read_fbin,_vec_literal,recall_at_k
out=root/'results'/'pool-sweep'/a.label;out.mkdir(parents=True,exist_ok=False)
q=np.ascontiguousarray(read_fbin('/home/ubuntu/data/queries_10k.fbin'))
gt=np.load('/home/ubuntu/data/gt_1000000.npy',mmap_mode='r')[:10000,:10]
literals=[_vec_literal(v) for v in q]
eng=PgEngine(dbname='index_accel_smoke',index_dir='/tmp/cuvs_indexes');c=eng.conn
try:
 for sql in ('SET enable_seqscan=off','SET cuvs.cagra_batch_wait_us=0','SET cuvs.bf_batch_wait_us=0','SET cuvs.shard_count=1',f'SET cuvs.k={a.k}'):
  c.execute(sql)
 assert c.execute('SHOW cuvs.rmm_pool_mb').fetchone()[0]==str(a.pool)
 base=json.loads((root/'results/baseline-noise.json').read_text());graph=helper.graph_metadata(c,'/tmp/cuvs_indexes');assert graph==base['graph']
 assert hashlib.sha256(q.tobytes()).hexdigest()==base['query_bytes_sha256']
 assert hashlib.sha256(np.asarray(gt).tobytes()).hexdigest()==base['truth_bytes_sha256']
 rels=[c.execute("SELECT 't'::regclass::oid").fetchone()[0]]
 toast=c.execute("SELECT reltoastrelid FROM pg_class WHERE oid='t'::regclass").fetchone()[0]
 rels+=[toast]+[r[0] for r in c.execute('SELECT indexrelid FROM pg_index WHERE indrelid=%s',(toast,)).fetchall()]
 blocks={str(r):c.execute('SELECT pg_prewarm(%s::regclass)',(str(r),)).fetchone()[0] for r in rels if r}
 def statement(lit):return "SELECT id, embedding <-> '{}'::vector FROM t ORDER BY embedding <-> '{}'::vector LIMIT 10".format(lit,lit)
 plan='\n'.join(r[0] for r in c.execute('EXPLAIN '+statement(literals[0])).fetchall());assert 't_cagra' in plan and 'Seq Scan' not in plan
 def mark():return c.execute("SELECT search_count,error_count,search_mode FROM pg_stat_gpu_search WHERE index_oid='t_cagra'::regclass").fetchone()
 begin=mark();cold=[]
 for lit in literals[:100]:
  start=time.perf_counter();rows=c.execute(statement(lit)).fetchall();cold.append(time.perf_counter()-start);assert len(rows)==10
 for lit in literals[:100]:c.execute(statement(lit)).fetchall()
 ids=np.full((10000,10),-1,dtype=np.int64);times=[]
 for i,lit in enumerate(literals):
  start=time.perf_counter();rows=c.execute(statement(lit)).fetchall();times.append(time.perf_counter()-start);assert len(rows)==10;ids[i]=[r[0] for r in rows]
 end=mark();assert end[0]-begin[0]==10200 and end[1]==begin[1] and end[2]=='gpu_cagra'
 assert helper.graph_metadata(c,'/tmp/cuvs_indexes')==graph
 metric={'pool_mb':a.pool,'k':a.k,'recall_at10':recall_at_k(ids,gt,10),'p50_ms':float(np.percentile(times,50)*1000),'first100_p99_ms':float(np.percentile(cold,99)*1000),'first100_max_ms':max(cold)*1000,'gpu_queries':end[0]-begin[0],'gpu_errors':end[1]-begin[1]}
 (out/'metrics.json').write_text(json.dumps(metric,indent=2)+'\n');np.save(out/'ids.npy',ids)
 with (out/'query-latency.csv').open('w',newline='') as f:
  w=csv.writer(f);w.writerow(['phase','query_idx','milliseconds']);w.writerows(('first100',i,t*1000) for i,t in enumerate(cold));w.writerows(('steady10000',i,t*1000) for i,t in enumerate(times))
 (out/'metadata.json').write_text(json.dumps({'graph':graph,'pool_mb':a.pool,'k':a.k,'postmaster_start':str(c.execute('SELECT pg_postmaster_start_time()').fetchone()[0]),'prewarm_blocks':blocks,'harness_sha256':helper.sha256(__file__),'cold_definition':'first100 client requests after daemon startup auto-load+hardware warmup and PG heap/toast/index prewarm; no client query warmup','steady_definition':'100 client queries warmup after first100, then all10000 queries','query_sha256':base['query_bytes_sha256'],'truth_sha256':base['truth_bytes_sha256']},indent=2)+'\n')
 print(json.dumps(metric),flush=True)
finally:eng.close()
