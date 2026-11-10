#!/usr/bin/env python3
"""Verify only the SDK source consumed by HNSW's ACE partition planner."""
import argparse, hashlib, json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--source',required=True);a=p.parse_args()
f=Path(a.source)/'neighbors/detail/cagra/cagra_build.cuh'
manifest=Path(__file__).resolve().parents[1]/'infra/cuvs-patches/patchset-manifest.json'
expected=json.loads(manifest.read_text())['source_files']['cpp/src/neighbors/detail/cagra/cagra_build.cuh']['final_sha256']
actual=hashlib.sha256(f.read_bytes()).hexdigest()
if actual!=expected:raise SystemExit('Unsupported HNSW SDK planner source: expected '+expected+', got '+actual+'. Resident builds do not require this source; ACE must not use an unverified planner.')
print(json.dumps({'path':str(f.resolve()),'sha256':actual,'status':'PINNED_PLANNER_SOURCE_MATCH','scope':'planner source only, not a whole SDK/runtime audit'}))
