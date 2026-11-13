import pathlib,subprocess,tempfile,resource,json
root=pathlib.Path('/src');source=(root/'src/cuvs_connectivity.h').read_text();results=[]
mutations={'zero_missing':('*missing=n;','*missing=0;'),'omit_upper':('l<=seen[u]','l<=0'),'omit_union':('if(a!=b){d[a]=b;','if(0 && a!=b){d[a]=b;'),'ascend_from_entry':('cuvs_conn_reach(n,entry,entry_level,','cuvs_conn_reach(n,entry,maxlevel,')}
for name,(before,after) in mutations.items():
 assert before in source
 with tempfile.TemporaryDirectory() as tmp:
  d=pathlib.Path(tmp);(d/'cuvs_connectivity.h').write_text(source.replace(before,after))
  cc=subprocess.run(['gcc','-std=c11','-O2','-I'+tmp,str(root/'test/unit/test_connectivity.c'),'-o',str(d/'test')],capture_output=True,text=True);assert cc.returncode==0,cc.stderr
  def no_core():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
  r=subprocess.run([str(d/'test')],capture_output=True,text=True,preexec_fn=no_core);results.append({'mutation':name,'returncode':r.returncode,'killed':r.returncode!=0})
assert all(x['killed'] for x in results)
pathlib.Path('/out/MUTATIONS.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results))
