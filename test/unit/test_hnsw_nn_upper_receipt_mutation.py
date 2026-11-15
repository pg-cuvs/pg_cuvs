#!/usr/bin/env python3
"""Prove the stored receipt test catches reintroduced base-dtype inference."""
from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'src/hnsw_export.c').read_text()
old='policy.hierarchy ? "null" : "\\\"not_applicable\\\""'
new='policy.hierarchy ? (nn.dtype == CUVS_HNSW_NN_DTYPE_FP32 ? "\\\"fp32\\\"" : "\\\"fp16\\\"") : "\\\"not_applicable\\\""'
assert source.count(old)==1
with tempfile.TemporaryDirectory(prefix='pgcuvs-upper-receipt-mutant-') as tmp:
 p=Path(tmp)/'hnsw_export.c';p.write_text(source.replace(old,new))
 result=subprocess.run([sys.executable,'-I','-B',str(root/'test/unit/test_hnsw_nn_upper_receipt.py'),'--source',str(p)],capture_output=True,text=True)
 if result.returncode!=2 or 'unrecorded upper configured dtype inferred' not in result.stdout+result.stderr:
  raise AssertionError(f'wrong mutation result {result.returncode}: {result.stdout}{result.stderr}')
 print('base-dtype inference mutation: detected (fixture exit 2)')
