"""CPU research harness. Original replay and controlled graphs are explicitly labeled.
No GPU, PostgreSQL, product mutation, or claimed production latency equivalence.
"""
import ctypes,heapq,json,os,platform,time,gc,sys
from pathlib import Path
import numpy as np
from scipy.sparse import csr_matrix
from scipy.sparse.csgraph import connected_components
OUT=Path('/out');INPUT=OUT/'inputs'
class Stats(ctypes.Structure):
 _fields_=[(k,ctypes.c_uint32) for k in ['magic','version','available','reserved']]+[(k,ctypes.c_uint64) for k in ['nodes','unreachable','unreachable_l0','wcc_l0','wcc_all','elapsed_us']]
lib=ctypes.CDLL('/tmp/conn203-kernel.so');lib.conn203_check.argtypes=[ctypes.c_uint32]*3+[ctypes.c_void_p]*3
lib.conn203_check_mask.argtypes=[ctypes.c_uint32]*3+[ctypes.c_void_p]*4

def fbin(p):
 n,d=np.fromfile(p,dtype='<u4',count=2);return np.memmap(p,dtype='<f4',mode='r',offset=8,shape=(int(n),int(d)))
def exact(X,q,ids=None,k=10):
 bi=np.empty(0,dtype='i8');bd=np.empty(0)
 for start in range(0,len(X) if ids is None else len(ids),32768):
  ii=np.arange(start,min(start+32768,len(X))) if ids is None else ids[start:start+32768]
  delta=X[ii].astype('f8')-q.astype('f8');ds=np.einsum('ij,ij->i',delta,delta)
  if not len(ds):continue
  threshold=np.partition(ds,min(k-1,len(ds)-1))[min(k-1,len(ds)-1)];mask=ds<=threshold;ci=np.r_[bi,ii[mask]];cd=np.r_[bd,ds[mask]];p=np.lexsort((ci,cd))[:k];bi=ci[p];bd=cd[p]
 return bi,bd

def ann(X,U,counts,q,ef,seeds,reverse):
 seen=set();front=[];best=[]
 def insert(ids):
  ids=[int(i) for i in ids if int(i) not in seen];seen.update(ids)
  if not ids:return
  delta=X[ids].astype('f8')-q.astype('f8');ds=np.einsum('ij,ij->i',delta,delta)
  for i,d in zip(ids,ds):
   key=(float(d),i)
   if len(best)<ef or key<(-best[0][0],-best[0][1]):
    heapq.heappush(front,key);heapq.heappush(best,(-float(d),-i))
    if len(best)>ef:heapq.heappop(best)
 insert(set(map(int,seeds)))
 while front:
  d,u=heapq.heappop(front)
  if len(best)>=ef and (d,u)>(-best[0][0],-best[0][1]):break
  insert(list(U[u,:counts[u]])+reverse.get(u,[]))
 return np.array([i for d,i in sorted((-d,-i) for d,i in best)[:10]],dtype='i8')

def run(name,X,U,rowids,root,queries,kinds,provenance):
 start=time.perf_counter();U=np.ascontiguousarray(U,dtype='i4');counts=(U>=0).sum(1).astype('u4');N=len(U);assert len(X)==N
 # Input graphs have prefix-packed valid neighbors. Reject silently truncated graphs.
 assert np.all((U>=0)==(np.arange(U.shape[1])[None,:]<counts[:,None]))
 stats=Stats();times=[]
 for rep in range(3):
  t=time.perf_counter();assert lib.conn203_check(N,U.shape[1],root,U.ctypes.data,counts.ctypes.data,ctypes.byref(stats));times.append(time.perf_counter()-t)
 # Map original node IDs to row IDs; in provided resident replays this is identity.
 assert np.array_equal(rowids,np.arange(N)), 'nonidentity row map requires explicit coordinate conversion'
 scc_start=time.perf_counter()
 indptr=np.r_[0,np.cumsum(counts,dtype='i8')];valid=U>=0
 graph=csr_matrix((np.ones(int(indptr[-1]),dtype='u1'),U[valid],indptr),shape=(N,N));del valid
 t=time.perf_counter();nc,labels=connected_components(graph,directed=True,connection='strong');scc_seconds=time.perf_counter()-scc_start;del graph;gc.collect()
 t=time.perf_counter()
 reps=np.full(nc,N,dtype='i8');np.minimum.at(reps,labels,np.arange(N));centers=np.zeros((nc,X.shape[1]),dtype='f8');sizes=np.bincount(labels)
 for i in range(0,N,32768):np.add.at(centers,labels[i:i+32768],X[i:i+32768])
 centers/=sizes[:,None];centers=centers.astype('f4')
 centroid_seconds=time.perf_counter()-t
 t=time.perf_counter()
 # Geometric escape seeds, built without queries/GT: 32 extrema in each of 8 coordinates.
 extrema=[]
 for j in range(min(8,X.shape[1])):
  col=np.asarray(X[:,j]);extrema.extend(np.argpartition(col,31)[:32]);extrema.extend(np.argpartition(-col,31)[:32])
 extrema=np.unique(extrema);seed_ids=np.r_[reps,extrema];seed_centers=np.vstack([centers,X[extrema]])
 geometric_seconds=time.perf_counter()-t
 t=time.perf_counter();reached=np.zeros(N,dtype='u1');assert lib.conn203_check_mask(N,U.shape[1],root,U.ctypes.data,counts.ctypes.data,ctypes.byref(stats),reached.ctypes.data);reached=reached.astype(bool)
 reach_seconds=time.perf_counter()-t
 missing=np.flatnonzero(~reached);reverse={};t=time.perf_counter()
 for begin in range(0,len(missing),16384):
  nodes=missing[begin:begin+16384];a=U[nodes];ok=a>=0;reach=np.zeros(a.shape,dtype=bool);reach[ok]=reached[a[ok]]
  rr,cc=np.nonzero(reach)
  for r,c in zip(rr,cc):reverse.setdefault(int(a[r,c]),[]).append(int(nodes[r]))
 reverse_seconds=time.perf_counter()-t;build_seconds=time.perf_counter()-start
 assert len(missing)==stats.unreachable
 records=[]
 for qi,(q,kind) in enumerate(zip(queries,kinds)):
  t=time.perf_counter();gt,gd=exact(X,q);exact_seconds=time.perf_counter()-t
  for ef in [64,128]:
   for method in ['baseline','exact_missing','scc_seeds','scc_geometric_seeds','reverse_edges','exact_all']:
    t=time.perf_counter()
    if method=='exact_all':ids=gt;elapsed=exact_seconds
    else:
     seeds=[root]
     if method in ['scc_seeds','scc_geometric_seeds']:
      cs=centers if method=='scc_seeds' else seed_centers;ss=reps if method=='scc_seeds' else seed_ids
      d=((cs.astype('f8')-q.astype('f8'))**2).sum(1);sel=np.lexsort((ss,d))[:32];seeds+=ss[sel].tolist()
     ids=ann(X,U,counts,q,ef,seeds,reverse if method=='reverse_edges' else {})
     if method=='exact_missing' and len(missing):
      extra,_=exact(X,q,missing);cand=np.unique(np.r_[ids,extra]);d=((X[cand].astype('f8')-q.astype('f8'))**2).sum(1);ids=cand[np.lexsort((cand,d))[:10]]
     elapsed=time.perf_counter()-t
    ds=((X[ids].astype('f8')-q.astype('f8'))**2).sum(1);canonical=len(set(ids)&set(gt))/10;tie=float(np.sum(ds<=gd[-1]+1e-8))/10
    records.append(dict(query=qi,kind=kind,ef=ef,method=method,ids=ids.tolist(),gt_ids=gt.tolist(),gt_kth_sqdist=float(gd[-1]),result_sqdist=ds.tolist(),canonical_recall=canonical,tie_recall=tie,ms=elapsed*1000))
 meta=dict(name=name,provenance=provenance,rows=N,dim=X.shape[1],entry=root,checker_seconds=times,unreachable=int(stats.unreachable),wcc=int(stats.wcc_all),scc=nc,preprocessing_seconds=build_seconds,scc_seconds=scc_seconds,centroid_seconds=centroid_seconds,geometric_seconds=geometric_seconds,reach_seconds=reach_seconds,reverse_seconds=reverse_seconds,storage=dict(missing_ids=len(missing)*4,scc_centers=int(centers.nbytes+reps.nbytes),geometric_extra=int(len(extrema)*(X.shape[1]*4+4)),reverse_edges=sum(map(len,reverse.values()))*4),queries=len(queries),records=records)
 (OUT/(name+'.json')).write_text(json.dumps(meta,indent=2)+'\n');print(json.dumps({k:v for k,v in meta.items() if k!='records'}),flush=True)

(OUT/'CPU-ENV.json').write_text(json.dumps(dict(platform=platform.platform(),python=platform.python_version(),numpy=np.__version__,cpu_count=os.cpu_count(),boundary='in-process Python CPU reference, not PostgreSQL/cuVS latency'),indent=2))
for name in ['syn','sift','far']:
 if '--dup-only' in sys.argv and name!='sift':continue
 p=INPUT/name;X=fbin(p/'base.fbin');Q=fbin(p/'queries.fbin');U=np.load(p/'l0-adjacency.npy',mmap_mode='r');rid=np.load(p/'row-ids.npy');g=json.loads((p/'graph.json').read_text());qs=list(Q[:16]);kinds=['natural']*16
 if name=='far':qs.append(Q[-1]);kinds.append('bundle')
 if '--dup-only' not in sys.argv:run(name,X,U,rid,int(g['entry_node']),qs,kinds,'original #203 resident L0 graph replay; different CPU search engine')
 if name=='sift':
  # Controlled structural duplicate counterexample, NOT the unretrieved original dup500 graph.
  n=len(X);XD=np.vstack([X,np.tile(X[431],(500,1))]);UD=np.full((n+500,64),-1,dtype='i4');UD[:n]=U
  for i in range(500):
   UD[n+i,:]=np.r_[431,n+70+(np.arange(63)+(i-70)+1)%430] if i>=70 else np.r_[431,np.arange(n+70,n+133)]
  UD[431,-1]=n+70
  run('dup500-controlled',XD,UD,np.arange(n+500),int(g['entry_node']),[X[431]],['bundle'],'CPU controlled model: 500 duplicates, lowest70 IDs have no incoming edges; not original GPU graph')
  del XD,UD
 if name=='far':
  base_count=1000000;N64=base_count+64;U64=np.array(U[:N64],dtype='i4');U64[U64>=N64]=-1
  for i in range(base_count,N64):
   valid=U64[i][U64[i]>=0];U64[i,:]=-1;U64[i,:len(valid)]=valid
  run('far64-induced-control',X[:N64],U64,np.arange(N64),int(g['entry_node']),[Q[-1]],['bundle'],'Induced subgraph of original far1000; not original GPU far64 graph')
  del U64
  # A reachable/strongly-connected far trap: a narrow bridge at a fixed base anchor.
  UC=np.array(U,dtype='i4');mask=np.load(p/'reachable-rows.npy');dead=np.flatnonzero(~mask);anchor=int(np.flatnonzero(mask)[-1]);UC[anchor,0]=dead[0]
  for j,u in enumerate(dead):UC[u,:]=-1;UC[u,0]=dead[(j+1)%len(dead)];UC[u,1]=anchor
  run('far-connected-control',X,UC,rid,int(g['entry_node']),[Q[-1]],['bundle'],'CPU modified graph; bridge/ring makes far rows reachable without guaranteeing finite-ef discovery')
  del UC
 del X,Q,U,rid;gc.collect()
