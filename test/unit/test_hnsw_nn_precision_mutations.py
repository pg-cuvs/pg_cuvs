#!/usr/bin/env python3
"""Verify the isolated precision guard tests reject specific broken guards."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = (root / 'src/cuvs_hnsw_nn_precision.h').read_text()
test = root / 'test/unit/test_hnsw_nn_precision.c'
mutations = {
    'half conversion': ('max_abs > 65504.0', 'max_abs > INFINITY'),
    'small dimension': ('dim <= 16 || ', ''),
    'distance overflow': ('(double)FLT_MAX / (metric == CUVS_METRIC_L2 ? 8.0 : 2.0)', '(double)INFINITY'),
}
with tempfile.TemporaryDirectory(prefix='pgcuvs-nn-precision-') as tmp:
    dst = Path(tmp)
    for name, (old, new) in mutations.items():
        assert header.count(old) == 1, name
        (dst / 'cuvs_hnsw_nn_precision.h').write_text(header.replace(old, new))
        exe = dst / 'check'
        built = subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                '-I', str(dst), '-I', str(root / 'src'),
                                str(test), '-lm', '-o', str(exe)], capture_output=True, text=True)
        if built.returncode:
            raise RuntimeError(f'{name}: mutation did not compile: {built.stderr}')
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        if not run.returncode:
            raise RuntimeError(f'{name}: mutation survived')
        print(f'{name}: detected (rc={run.returncode})')
    broken = header.replace('if (!isfinite(v)) return -1;', 'if (0) return -1;').replace('if (!isfinite(norm)) return -1;', 'if (0) return -1;')
    (dst / 'cuvs_hnsw_nn_precision.h').write_text(broken)
    built = subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(dst), '-I', str(root / 'src'), str(test), '-lm', '-o', str(exe)], capture_output=True, text=True)
    if built.returncode:
        raise RuntimeError(f'nonfinite: mutation did not compile: {built.stderr}')
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    if not run.returncode:
        raise RuntimeError('nonfinite: mutation survived')
    print(f'nonfinite pair: detected (rc={run.returncode})')
