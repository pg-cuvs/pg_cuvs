#!/usr/bin/env python3
import datetime,hashlib,json,platform,shutil,subprocess
from pathlib import Path
assert platform.node()=='brev-v6nc08sjs'
root=Path('/home/ubuntu/index-accel-validation')
def digest(p):
 h=hashlib.sha256()
 with p.open('rb') as f:
  for b in iter(lambda:f.read(8*1024*1024),b''):h.update(b)
 return h.hexdigest()
base=json.loads((root/'results/baseline-noise.json').read_text());graph={}
for name,expected in base['graph']['files'].items():
 p=Path('/tmp/cuvs_indexes')/name;actual={'bytes':p.stat().st_size,'sha256':digest(p)};assert actual==expected;graph[name]=actual
state={'observed_at_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'hostname':platform.node(),'source_commit':subprocess.check_output(['git','-C','/home/ubuntu/pg_cuvs','rev-parse','HEAD'],text=True).strip(),'original_graph_after_all_experiments':graph,'graph_snapshot_manifest':json.loads((root/'graph-snapshot/manifest.json').read_text()),'services':(root/'services-stopped.txt').read_text(),'remaining_gpu_processes':(root/'gpu-processes-after-stop.txt').read_text()}
(root/'final-state.json').write_text(json.dumps(state,indent=2)+'\n')
logs=root/'evidence-logs';logs.mkdir(exist_ok=True)
for p in root.iterdir():
 if p.is_file() and p.suffix in ('.log','.exit','.started','.completed') and p.name!='bootstrap.log':shutil.copy2(p,logs/p.name)
paths=[]
for directory in ('results','profiles','peak-profiles','serial-profiles','bounded-results','evidence-logs'):
 paths += [p for p in (root/directory).rglob('*') if p.is_file()]
paths += [root/name for name in ('final-state.json','native-peak-wrapper.patch','bounded-quota.patch','services-stopped.txt','gpu-processes-after-stop.txt')]
manifest={str(p.relative_to(root)):{'bytes':p.stat().st_size,'sha256':digest(p)} for p in paths}
(root/'export-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps({'files':len(manifest),'bytes':sum(v['bytes'] for v in manifest.values()),'original_graph_matches':True,'services_stopped':state['services'],'remaining_gpu_processes':state['remaining_gpu_processes']},indent=2))
