from pathlib import Path
import json,numpy as np
O=Path('/out');checks=[]
for name in ['syn','sift','dup500-controlled','far','far64-induced-control','far-connected-control']:
 x=json.loads((O/(name+'.json')).read_text());rs=x['records'];assert rs
 for r in rs:
  assert len(r['ids'])==len(set(r['ids']))==10 and all(0<=i<x['rows'] for i in r['ids'])
  assert all(np.isfinite(r['result_sqdist'])) and min(r['result_sqdist'])>=0
  if r['method']=='exact_all':assert r['canonical_recall']==r['tie_recall']==1
 if name=='dup500-controlled':assert x['unreachable']==70 and x['wcc']==1 and x['scc']==71
 if name=='far-connected-control':
  assert x['unreachable']==0 and x['wcc']==1
  for ef in [64,128]:
   a=next(r for r in rs if r['method']=='baseline' and r['ef']==ef);b=next(r for r in rs if r['method']=='exact_missing' and r['ef']==ef)
   assert a['ids']==b['ids'] and a['canonical_recall']==0
 if name in ['syn','sift','far']:
  p=O/'inputs'/name/'canonical-gt10.npy'
  if p.exists():
   g=np.load(p);mismatches=0
   for r in rs:
    if r['method']=='exact_all' and r['ef']==64:
     qi=r['query'] if r['kind']=='natural' else len(g)-1
     mismatches+=set(r['ids'])!=set(map(int,g[qi]))
   checks.append({'case':name,'original_gt_compared':True,'canonical_set_mismatches':mismatches})
 checks.append({'case':name,'record_count':len(rs),'valid_results':True})
(O/'RESULT-VERIFY.json').write_text(json.dumps(checks,indent=2)+'\n');print(json.dumps(checks))
