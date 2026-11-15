#!/usr/bin/env python3
"""Verify only the SDK source consumed by HNSW's ACE partition planner."""
import argparse, hashlib, json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--source',required=True);a=p.parse_args()
f=Path(a.source)/'neighbors/detail/cagra/cagra_build.cuh'
manifest=Path(__file__).resolve().parents[1]/'infra/cuvs-patches/patchset-manifest.json'
entry=json.loads(manifest.read_text())['source_files']['cpp/src/neighbors/detail/cagra/cagra_build.cuh']
# 'canonical' is the ACE planner after 0003; 0006 adds the B3 graph-consumer sort (b3 below).
expected=entry.get('patch3_sha256', entry['final_sha256'])
actual=hashlib.sha256(f.read_bytes()).hexdigest()
# B3's graph-consumer sorting patch changes this file outside the ACE planner.
# The two planner functions and ace_memory_guard.hpp match the canonical source;
# its build snapshot is bound to SDK 616eb202 in ACTUAL-B3-BINDING.json.
b3='38722f03f72efeb5a92e92ea30ebe9d87ec9b9b1afc2ca392a07de4b645c95bc'
if actual==expected:
    variant='canonical'
elif actual==b3:
    variant='b3_consumer'
    for name,digest in (
        ('ace_memory_guard.hpp','dc2ccb2d6920bf81d55e614181e3ceff8e7e0394eae56d0506fbb302190cf555'),
        ('consumer_row_sort_plan.hpp','07b7b724fce554cec74ee1cb779e24ced31867981f33e6afda4f2c17d98cac49'),
    ):
        dependency=f.parent/name
        if not dependency.is_file() or hashlib.sha256(dependency.read_bytes()).hexdigest()!=digest:
            raise SystemExit('Unsupported HNSW SDK planner dependency: '+name)
else:
    raise SystemExit('Unsupported HNSW SDK planner source: expected canonical '+expected+' or fixed B3 '+b3+', got '+actual+'. Resident builds do not require this source; ACE must not use an unverified planner.')
print(json.dumps({'path':str(f.resolve()),'sha256':actual,'variant':variant,'status':'PINNED_PLANNER_SOURCE_MATCH','scope':'planner source only, not a whole SDK/runtime audit'}))
