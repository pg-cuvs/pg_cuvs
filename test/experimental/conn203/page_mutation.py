from pathlib import Path
import shutil,subprocess,tempfile,json,resource
R=Path('/src')
with tempfile.TemporaryDirectory() as tmp:
 d=Path(tmp);(d/'src').mkdir();(d/'test/unit').mkdir(parents=True)
 for p in (R/'src').glob('*.h'):shutil.copyfile(p,d/'src'/p.name)
 for name in ['hnsw_export.c','cuvs_util.c']:shutil.copyfile(R/'src'/name,d/'src'/name)
 shutil.copyfile(R/'test/unit/test_hnsw_converter.c',d/'test/unit/test_hnsw_converter.c')
 p=d/'src/hnsw_export.c';s=p.read_text();assert 'h->pd_upper=at;' in s;p.write_text(s.replace('h->pd_upper=at;','h->pd_upper=h->pd_special; /* mutation: leave snapshot in FPI hole */',1))
 cmd=['gcc','-std=gnu11','-O0','-ffunction-sections','-fdata-sections','-I'+str(d/'src'),'-I/usr/include/postgresql','-I/usr/include/postgresql/16/server',str(d/'test/unit/test_hnsw_converter.c'),str(d/'src/cuvs_util.c'),'-Wl,--gc-sections','-L/usr/lib/postgresql/16/lib','-lpgcommon','-lpgport','-lm','-lrt','-o',str(d/'runner')]
 c=subprocess.run(cmd,capture_output=True,text=True);assert c.returncode==0,c.stderr
 def no_core():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
 r=subprocess.run([str(d/'runner')],capture_output=True,text=True,preexec_fn=no_core);assert r.returncode!=0
 Path('/out/page-mutant.stderr').write_text(r.stderr);Path('/out/PAGE-MUTATION.json').write_text(json.dumps({'mutation':'metadata in WAL free-space hole','compile_rc':c.returncode,'run_rc':r.returncode,'killed':True},indent=2)+'\n')
 print('metadata hole mutation: RED confirmed')
