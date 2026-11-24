#!/usr/bin/env python3
"""The verifier tests must reject changes that widen the source contract."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--canonical-header',type=Path,required=True);p.add_argument('--row-sort-src',type=Path,required=True);a=p.parse_args()
source=(root/'scripts/verify-hnsw-sdk-plan.py').read_text()
reject_unknown = [
    line for line in source.splitlines()
    if line.startswith("    raise SystemExit('Unsupported HNSW SDK planner source:")
]
assert len(reject_unknown) == 1, 'expected one unknown-planner rejection'
mutations={
 'accept all planners':('if actual==expected:', 'if True:'),
 'skip row-sort dependency checks':('if not dependency.is_file() or hashlib.sha256(dependency.read_bytes()).hexdigest()!=digest:', 'if False:'),
 'skip consumer header':('        dependency=f.parent/name\n', "        if name == 'consumer_row_sort_plan.hpp': continue\n        dependency=f.parent/name\n"),
 'accept unknown planner':(reject_unknown[0], "    variant='unknown'"),
}
with tempfile.TemporaryDirectory(prefix='pgcuvs-plan-mutants-') as tmp:
 d=Path(tmp);(d/'scripts').mkdir();(d/'infra/cuvs-patches').mkdir(parents=True)
 shutil.copyfile(root/'infra/cuvs-patches/patchset-manifest.json',d/'infra/cuvs-patches/patchset-manifest.json')
 for name,(old,new) in mutations.items():
  assert source.count(old)==1,name
  verifier=d/'scripts/verify-hnsw-sdk-plan.py';verifier.write_text(source.replace(old,new))
  command=[sys.executable,'-I','-B',str(root/'test/unit/test_hnsw_sdk_plan_verifier.py'), '--canonical-header',str(a.canonical_header), '--row-sort-src',str(a.row_sort_src), '--verifier',str(verifier)]
  result=subprocess.run(command,capture_output=True,text=True)
  if result.returncode==0:raise AssertionError(name+' survived')
  if 'AssertionError' not in result.stderr:raise AssertionError(name+' failed for an unexpected reason: '+result.stderr)
  print(name+': detected')
