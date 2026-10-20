#!/usr/bin/env python3
import argparse,csv,json,platform,subprocess,sys,time
from pathlib import Path
assert platform.node()=='brev-v6nc08sjs'
p=argparse.ArgumentParser();p.add_argument('--pool',type=int,required=True);p.add_argument('--label',required=True);p.add_argument('--index-dir',required=True);a=p.parse_args()
root=Path('/home/ubuntu/index-accel-validation');out=root/'results'/'build-times'/a.label;out.mkdir(parents=True,exist_ok=False)
sys.path.insert(0,'/home/ubuntu/pg_cuvs/bench/cuvs_bench_backend')
from pg_engine import PgEngine
eng=PgEngine(dbname='index_accel_pool_build',index_dir=a.index_dir);c=eng.conn
try:
 assert c.execute('SHOW cuvs.rmm_pool_mb').fetchone()[0]==str(a.pool)
 for sql in ("SET statement_timeout='15min'","SET maintenance_work_mem='8GB'",'SET max_parallel_maintenance_workers=7'):
  c.execute(sql)
 c.execute("SELECT set_config('cuvs.index_dir',%s,false)",(a.index_dir,))
 rels=[c.execute("SELECT 't'::regclass::oid").fetchone()[0]];toast=c.execute("SELECT reltoastrelid FROM pg_class WHERE oid='t'::regclass").fetchone()[0];rels += [toast]+[r[0] for r in c.execute('SELECT indexrelid FROM pg_index WHERE indrelid=%s',(toast,)).fetchall()]
 blocks={str(r):c.execute('SELECT pg_prewarm(%s::regclass)',(str(r),)).fetchone()[0] for r in rels if r}
 ddl="CREATE INDEX t_pool_bench ON t USING cagra (embedding vector_l2_ops) WITH (graph_degree=64,intermediate_graph_degree=256,build_algo='ivf_pq')"
 begin=time.perf_counter();c.execute(ddl);seconds=time.perf_counter()-begin
 stat=c.execute("SELECT index_oid::oid,n_vecs,vram_bytes FROM pg_stat_gpu_search WHERE index_oid='t_pool_bench'::regclass").fetchone();assert stat and stat[1]==1000000
 logical=c.execute('SELECT coalesce(sum(vram_bytes),0) FROM pg_stat_gpu_search').fetchone()[0]
 raw=subprocess.check_output(['nvidia-smi','--query-gpu=memory.total,memory.used,memory.free','--format=csv,noheader,nounits'],text=True).strip();total,used,free=map(float,raw.split(','))
 metric={'label':a.label,'pool_mb':a.pool,'create_index_seconds':seconds,'index_oid':stat[0],'n_vecs':stat[1],'index_vram_estimate_bytes':stat[2],'all_index_vram_estimate_bytes':int(logical),'nvidia_smi_total_mib':total,'nvidia_smi_used_mib':used,'nvidia_smi_free_mib':free,'physical_minus_logical_bytes':int(used*1024*1024-logical)}
 (out/'metrics.json').write_text(json.dumps(metric,indent=2)+'\n')
 (out/'metadata.json').write_text(json.dumps({'ddl':ddl,'dbname':'index_accel_pool_build','index_dir':a.index_dir,'table_rows':1000000,'table_origin':'CREATE DATABASE TEMPLATE index_accel_smoke; source table unchanged','prewarm_blocks':blocks,'postmaster_start':str(c.execute('SELECT pg_postmaster_start_time()').fetchone()[0]),'pool_status':c.execute('SHOW cuvs.rmm_pool_mb').fetchone()[0]},indent=2)+'\n')
 print(json.dumps(metric),flush=True)
 c.execute('DROP INDEX t_pool_bench')
finally:eng.close()
