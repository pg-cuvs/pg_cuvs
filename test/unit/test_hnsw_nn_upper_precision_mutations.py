#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile
import signal
import argparse
root = Path(__file__).resolve().parents[2]
ap = argparse.ArgumentParser()
ap.add_argument('--sdk-src', type=Path, required=True, help='patched cpp/src/neighbors')
header = (ap.parse_args().sdk_src / 'detail/hnsw_precision.hpp').read_text()
test = root / 'test/unit/test_hnsw_nn_upper_precision.cpp'
changes = {'half overflow': ('std::abs(value) > 65504.0', 'std::abs(value) > INFINITY'),
           'nonfinite': ('!std::isfinite(value) || ', '')}
with tempfile.TemporaryDirectory(prefix='pgcuvs-nn-upper-') as tmp:
    dst = Path(tmp)
    for name, change in [('baseline', None), *changes.items()]:
        source = header
        if change:
            old, new = change
            assert header.count(old) == 1
            source = header.replace(old, new)
        (dst / 'hnsw_precision.hpp').write_text(source)
        exe = dst / 'check'
        built = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(dst), str(test), '-o', str(exe)], capture_output=True, text=True)
        if built.returncode: raise RuntimeError(f'{name}: did not compile: {built.stderr}')
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        if change is None:
            if run.returncode:
                raise RuntimeError(f'{name}: failed: {run.stderr}')
            print('baseline: PASS')
        elif run.returncode == -signal.SIGABRT and 'Assertion' in run.stderr:
            print(f'{name}: detected (rc={run.returncode})')
        else:
            raise RuntimeError(f'{name}: expected assertion failure, got {run.returncode}: {run.stderr}')
