#!/usr/bin/env python3
import argparse,collections,csv,json,re,sqlite3
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('directory',type=Path);a=p.parse_args();out=[]
for mode in ('search','ivf_pq','nn_descent'):
 text=(a.directory/(mode+'.log')).read_text();records=[json.loads(line) for line in text.splitlines() if line.startswith('{')]
 setup=next(r for r in records if r.get('setup'));end=next(r for r in records if r.get('success'));phases=[r for r in records if 'phase' in r]
 db=sqlite3.connect(a.directory/(mode+'.sqlite'));db.row_factory=sqlite3.Row
 names={r[0]:r[1] for r in db.execute('SELECT id,value FROM StringIds')}
 ranges={}
 for raw in db.execute('SELECT * FROM NVTX_EVENTS'):
  e=dict(raw);label=e.get('text') or names.get(e.get('textId'),'')
  if label in ('pool_init','load_original_graph','search_it512_q10000_k64','ivf_pq','nn_descent') and e.get('end'):
   ranges[label]=e
 runtime=[dict(r) for r in db.execute('SELECT * FROM CUPTI_ACTIVITY_KIND_RUNTIME')]
 for phase in phases:
  event=ranges[phase['phase']];calls=collections.Counter(names[r['nameId']] for r in runtime if event['start']<=r['start'] and r['end']<=event['end'] and ('Malloc' in names[r['nameId']] or 'Free' in names[r['nameId']]))
  out.append({'mode':mode,'N':1,'pool_mb':setup['pool_mb'],**phase,'cuda_allocation_api_counts':dict(calls),'lifetime_pool_reserved_bytes':end['lifetime_pool_reserved_bytes'],'lifetime_pool_allocations_including_initial':end['lifetime_pool_allocations'],'success':True})
 with (a.directory/(mode+'-memory-events.csv')).open('w',newline='') as f:
  q=db.execute('SELECT * FROM CUDA_GPU_MEMORY_USAGE_EVENTS ORDER BY start');w=csv.writer(f);w.writerow([x[0] for x in q.description]);w.writerows(q)
(a.directory/'peak-summary.json').write_text(json.dumps(out,indent=2)+'\n')
with (a.directory/'peak-summary.csv').open('w',newline='') as f:
 w=csv.DictWriter(f,fieldnames=list(out[0]));w.writeheader();w.writerows(out)
print(json.dumps(out,indent=2))
