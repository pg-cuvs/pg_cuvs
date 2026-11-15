#!/usr/bin/env python3
"""Verify the isolated precision guard tests reject specific broken guards."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = (root / 'src/cuvs_hnsw_nn_precision.h').read_text()
test = root / 'test/unit/test_hnsw_nn_precision.c'
# Since r87 D2 the norm bound implies the element bound (|x| > 65504 gives
# 4*norm >= 65504), so removing only 'max_abs > 65504.0' is an equivalent
# mutant. Remove the whole half-safety condition, and the bound alone.
mutations = {
    'half conversion': ('max_abs > 65504.0 || !(half_bound < 65504.0)', 'max_abs > INFINITY + half_bound'),
    'norm cancellation bound': ('|| !(half_bound < 65504.0)', '|| (half_bound < 0.0)'),
    'small dimension': ('dim <= 16 || ', ''),
    'distance overflow': ('(double)FLT_MAX / (metric == CUVS_METRIC_L2 ? 8.0 : 2.0)', '(double)INFINITY'),
    # r87 offset guard: L2 norm-expansion cancellation must refuse
    'cancellation guard': ('        return CUVS_HNSW_NN_PRECISION_CANCELLATION;', '        return 0;'),
    'cancellation threshold': ('#define CUVS_HNSW_NN_L2_NORM_TO_SPREAD_MAX 4096.0', '#define CUVS_HNSW_NN_L2_NORM_TO_SPREAD_MAX 1e12'),
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
    finite_checks = ('if (!isfinite(v)) {', 'if (!isfinite(norm)) {')
    for anchor in finite_checks:
        assert header.count(anchor) == 1, anchor
    broken = header.replace(finite_checks[0], 'if (0) {').replace(finite_checks[1], 'if (0) {')
    (dst / 'cuvs_hnsw_nn_precision.h').write_text(broken)
    built = subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(dst), '-I', str(root / 'src'), str(test), '-lm', '-o', str(exe)], capture_output=True, text=True)
    if built.returncode:
        raise RuntimeError(f'nonfinite: mutation did not compile: {built.stderr}')
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    if not run.returncode:
        raise RuntimeError('nonfinite: mutation survived')
    print(f'nonfinite pair: detected (rc={run.returncode})')
