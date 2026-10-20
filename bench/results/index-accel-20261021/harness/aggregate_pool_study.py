#!/usr/bin/env python3
import argparse,csv,json,statistics
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);a=p.parse_args()
protocol=json.loads((a.root.parent/'rmm-study-protocol.json').read_text())
base=json.loads((a.root/'baseline-noise.json').read_text());raw=[]
for i,pool in enumerate(protocol['interleaved_sequence_mb'],1):
 for k in (64,128):
  label=f'trial{i}-pool{pool}-k{k}';path=a.root/'pool-sweep'/label
  row=json.loads((path/'metrics.json').read_text());meta=json.loads((path/'metadata.json').read_text())
  assert row['pool_mb']==pool and row['k']==k and row['gpu_queries']==10200 and row['gpu_errors']==0
  assert meta['graph']==base['graph']
  raw.append({'order':i,**row})
rows=[]
for pool in protocol['sizes_mb']:
 for k in (64,128):
  source=[r for r in raw if r['pool_mb']==pool and r['k']==k];assert len(source)==3
  row={'pool_mb':pool,'k':k,'N':3}
  for field in ('recall_at10','p50_ms','first100_p99_ms','first100_max_ms'):
   vals=[r[field] for r in source];row[field+'_mean']=statistics.mean(vals);row[field+'_std']=statistics.stdev(vals)
  rows.append(row)
best={k:min(r['p50_ms_mean'] for r in rows if r['pool_mb']>0 and r['k']==k) for k in (64,128)}
eligible=[pool for pool in protocol['sizes_mb'] if pool>0 and all(next(r for r in rows if r['pool_mb']==pool and r['k']==k)['p50_ms_mean']<=best[k]*1.05 for k in (64,128))]
assert eligible
for name,items in [('pool-sweep-raw.csv',raw),('pool-sweep-summary.csv',rows)]:
 with (a.root/name).open('w',newline='') as f:
  w=csv.DictWriter(f,fieldnames=list(items[0]));w.writeheader();w.writerows(items)
out={'candidate_mb':min(eligible),'criterion':protocol['candidate_rule'],'best_positive_p50_ms':best,'eligible_sizes_mb':eligible,'rows':rows}
(a.root/'pool-sweep-aggregate.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out,indent=2))
