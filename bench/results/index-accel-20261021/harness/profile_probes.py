#!/usr/bin/env python3
import importlib.metadata
import json
import platform
import sys
from pathlib import Path
import numpy as np
import nvtx

if platform.node() != 'brev-v6nc08sjs':
    raise RuntimeError('wrong host')
sys.path.insert(0, '/home/ubuntu/pg_cuvs/bench/cuvs_bench_backend')
from pg_engine import PgEngine, read_fbin

queries=np.ascontiguousarray(read_fbin('/home/ubuntu/data/queries_10k.fbin'))[:1000]
eng=PgEngine(dbname='index_accel_smoke',index_dir='/tmp/cuvs_indexes')
rows=[]
try:
    for statement in ('SET cuvs.cagra_batch_wait_us=0','SET cuvs.bf_batch_wait_us=0','SET cuvs.search_width=1'):
        eng.conn.execute(statement)
    for itopk in (64,128,256,512):
        eng.conn.execute("SELECT set_config('cuvs.itopk_size',%s,false)",(str(itopk),))
        with nvtx.annotate(message=f'itopk={itopk}/single',domain='pgcuvs-index-accel'):
            ids,_=eng.search('pgcuvs_cagra',queries[:1],10,64,warmup=0)
        with nvtx.annotate(message=f'itopk={itopk}/batch',domain='pgcuvs-index-accel'):
            bids,_,_=eng.batch_search(queries,64,top_k=10,warmup=0,repeats=1,chunks=1)
        rows.append({'itopk_size':itopk,'search_width':1,'returned_k':64,
                     'single_queries':1,'batch_queries':1000,
                     'single_first_ids':ids[0].tolist(),'batch_first_ids':bids[0].tolist()})
finally:
    eng.close()
Path('/home/ubuntu/index-accel-validation/profiles/probe-requests.json').write_text(json.dumps(
    {'nvtx_version':importlib.metadata.version('nvtx'),'timings_not_for_benchmark':True,'probes':rows},indent=2)+'\n')
