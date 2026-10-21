#!/usr/bin/env python3
import argparse,csv,json,statistics
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--candidate',type=int,required=True);p.add_argument('--prefix',default='build');a=p.parse_args();raw=[];rows=[]
for i,pool in enumerate([0,a.candidate,0,a.candidate,0,a.candidate],1):
 r=json.loads((a.root/'build-times'/f'{a.prefix}{i}-pool{pool}'/'metrics.json').read_text());assert r['pool_mb']==pool and r['n_vecs']==1000000;raw.append(r)
for pool in (0,a.candidate):
 source=[r for r in raw if r['pool_mb']==pool];assert len(source)==3;row={'pool_mb':pool,'N':3}
 for field in ('create_index_seconds','index_vram_estimate_bytes','all_index_vram_estimate_bytes','nvidia_smi_used_mib','physical_minus_logical_bytes'):
  vals=[r[field] for r in source];row[field+'_mean']=statistics.mean(vals);row[field+'_std']=statistics.stdev(vals)
 rows.append(row)
out={'candidate_mb':a.candidate,'unbounded_pool':True,'wallclock_ratio_pool0_over_pool_on':rows[0]['create_index_seconds_mean']/rows[1]['create_index_seconds_mean'],'rows':rows}
(a.root/'build-time-aggregate.json').write_text(json.dumps(out,indent=2)+'\n')
for filename,items in [('build-time-raw.csv',raw),('build-time-summary.csv',rows)]:
 with (a.root/filename).open('w',newline='') as f:
  w=csv.DictWriter(f,fieldnames=list(items[0]));w.writeheader();w.writerows(items)
print(json.dumps(out,indent=2))
