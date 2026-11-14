#!/usr/bin/env python3
"""The verifier tests must reject changes that widen the source contract."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--canonical-header',type=Path,required=True);p.add_argument('--b3-src',type=Path,required=True);a=p.parse_args()
source=(root/'scripts/verify-hnsw-sdk-plan.py').read_text()
mutations={
 'accept all planners':('if actual==expected:', 'if True:'),
 'skip B3 dependency checks':('if not dependency.is_file() or hashlib.sha256(dependency.read_bytes()).hexdigest()!=digest:', 'if False:'),
 'skip consumer header':('        dependency=f.parent/name\n', "        if name == 'consumer_row_sort_plan.hpp': continue\n        dependency=f.parent/name\n"),
 'accept unknown planner':("    raise SystemExit('Unsupported HNSW SDK planner source: expected canonical '+expected+' or fixed B3 '+b3+', got '+actual+'. Resident builds do not require this source; ACE must not use an unverified planner.')", "    variant='unknown'"),
}
with tempfile.TemporaryDirectory(prefix='pgcuvs-plan-mutants-') as tmp:
 d=Path(tmp);(d/'scripts').mkdir();(d/'infra/cuvs-patches').mkdir(parents=True)
 shutil.copyfile(root/'infra/cuvs-patches/patchset-manifest.json',d/'infra/cuvs-patches/patchset-manifest.json')
 for name,(old,new) in mutations.items():
  assert source.count(old)==1,name
  verifier=d/'scripts/verify-hnsw-sdk-plan.py';verifier.write_text(source.replace(old,new))
  command=[sys.executable,'-I','-B',str(root/'test/unit/test_hnsw_sdk_plan_verifier.py'), '--canonical-header',str(a.canonical_header), '--b3-src',str(a.b3_src), '--verifier',str(verifier)]
  result=subprocess.run(command,capture_output=True,text=True)
  if result.returncode==0:raise AssertionError(name+' survived')
  if 'AssertionError' not in result.stderr:raise AssertionError(name+' failed for an unexpected reason: '+result.stderr)
  print(name+': detected')
