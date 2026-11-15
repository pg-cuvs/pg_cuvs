from pathlib import Path
import subprocess,json
O=Path('/out');rows=[]
for name in ['sift','syn']:
 for spill in [False,True]:
  flag='--connectivity-file-spill' if spill else '--connectivity-file'
  r=subprocess.run(['/src/test-hnsw-converter',flag,str(O/(name+'-layout.hnsw'))],capture_output=True,text=True,check=True)
  values=[json.loads(l) for l in r.stdout.splitlines()]
  assert len(values)==3 and all(bool(v['spilled'])==spill and v['available']==1 for v in values)
  expected=0 if name=='sift' else 2199437;assert all(v['unreachable']==expected for v in values)
  rows.append({'name':name,'forced_file_backed':spill,'measurements':values})
(O/'FINAL-COST.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps(rows))
