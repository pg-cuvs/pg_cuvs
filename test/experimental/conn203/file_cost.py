import json,struct,time,subprocess
from pathlib import Path
import numpy as np
O=Path('/out');result=[]
for name in ['sift','syn']:
 P=O/'inputs'/name;u=np.load(P/'l0-adjacency.npy',mmap_mode='r');g=json.loads((P/'graph.json').read_text());n,width=u.shape;dim=128
 dtype=np.dtype([('count','<u4'),('links','<i4',(width,)),('vectors','<f4',(dim,)),('label','<u8')]);size=dtype.itemsize;f=O/(name+'-layout.hnsw')
 with f.open('wb') as w:w.truncate(96+n*size+n*4)
 h=struct.pack('<6Qii3QdQ',0,n,n,size,4+width*4+dim*4,4+width*4,0,g['entry_node'],width//2,width,width//2,1,500)
 with f.open('r+b') as w:w.write(h)
 rows=np.memmap(f,mode='r+',dtype=dtype,offset=96,shape=(n,))
 start=time.perf_counter()
 for i in range(0,n,16384):
  a=u[i:i+16384];rows['count'][i:i+len(a)]=(a>=0).sum(1);rows['links'][i:i+len(a)]=a;rows['label'][i:i+len(a)]=np.arange(i,i+len(a))
 rows.flush();del rows
 r=subprocess.run(['/src/test-hnsw-converter','--connectivity-file',str(f)],capture_output=True,text=True,check=True)
 values=[json.loads(l) for l in r.stdout.splitlines()];assert len(values)==3
 assert all(v['unreachable']==n-g['layer_respecting_reachable'] and v['wcc']==g['l0_wcc'] for v in values)
 result.append(dict(name=name,kind='real product file adapter; original adjacency in synthetic hnswlib layout, zero vector payload not read by checker',rows=n,bytes=f.stat().st_size,layout_and_checks_seconds=time.perf_counter()-start,measurements=values))
(O/'FILE-COST.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
