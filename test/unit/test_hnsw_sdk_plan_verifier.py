#!/usr/bin/env python3
"""Exercise the actual source-only planner verifier with pinned source bytes."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser()
p.add_argument('--canonical-header', type=Path, required=True)
p.add_argument('--b3-src', type=Path, required=True)
p.add_argument('--verifier', type=Path, default=root / 'scripts/verify-hnsw-sdk-plan.py')
a = p.parse_args()


def run(source):
    return subprocess.run([sys.executable, '-I', '-B', str(a.verifier), '--source', str(source)],
                          capture_output=True, text=True)

with tempfile.TemporaryDirectory(prefix='pgcuvs-plan-verifier-') as temp:
    src = Path(temp)
    target = src / 'neighbors/detail/cagra'
    target.mkdir(parents=True)
    names = ('cagra_build.cuh', 'ace_memory_guard.hpp', 'consumer_row_sort_plan.hpp')
    for name in names:
        shutil.copyfile(a.b3_src / 'neighbors/detail/cagra' / name, target / name)
    b3 = run(src)
    if b3.returncode != 0:
        raise AssertionError('B3 expected accepted: '+b3.stderr.strip())
    assert json.loads(b3.stdout)['status'] == 'PINNED_PLANNER_SOURCE_MATCH'
    for name in names:
        f = target / name
        before = f.read_bytes()
        f.write_bytes(before + b'\n')
        rejected = run(src)
        assert rejected.returncode != 0 and ('Unsupported HNSW SDK planner source' in rejected.stderr
              or 'Unsupported HNSW SDK planner dependency' in rejected.stderr), name
        print(name+': one-byte mutation rejected')
        f.write_bytes(before)
    (target / 'cagra_build.cuh').write_bytes(a.canonical_header.read_bytes())
    canonical = run(src)
    assert canonical.returncode == 0 and json.loads(canonical.stdout)['status'] == 'PINNED_PLANNER_SOURCE_MATCH'
    (target / 'cagra_build.cuh').write_bytes(b'unknown planner\n')
    rejected = run(src)
    assert rejected.returncode != 0 and 'Unsupported HNSW SDK planner source' in rejected.stderr
    print('canonical accepted, exact B3 accepted, unrecognized planner rejected')
