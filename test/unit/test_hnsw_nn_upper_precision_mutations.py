#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile
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
    for name, (old, new) in changes.items():
        assert header.count(old) == 1
        (dst / 'hnsw_precision.hpp').write_text(header.replace(old, new))
        exe = dst / 'check'
        built = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(dst), str(test), '-o', str(exe)], capture_output=True, text=True)
        if built.returncode: raise RuntimeError(f'{name}: did not compile: {built.stderr}')
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        if not run.returncode: raise RuntimeError(f'{name}: survived')
        print(f'{name}: detected (rc={run.returncode})')
