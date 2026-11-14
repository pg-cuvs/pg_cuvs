#!/usr/bin/env python3
"""Detect removal of resident NN allocation terms in the phase model."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = (root / 'src/cuvs_hnsw_nn_capacity.h').read_text()
test = root / 'test/unit/test_hnsw_nn_capacity.c'
mutations = {
    'sort dataset': ('row + (uint64_t)intermediate * 4, &out->sort_gpu', '(uint64_t)intermediate * 4, &out->sort_gpu'),
    'batch staging': ('n < 100000 ? n : 100000, row, &staging', '0, row, &staging'),
    'fp32 dataset': ('dtype == CUVS_HNSW_NN_FP32 ? 4 : 2', 'dtype == CUVS_HNSW_NN_FP32 ? 2 : 2'),
    'host bloom': ('8 * e + 916 + 2 * q', '8 * e + 916 + 0 * q'),
    'upper hierarchy peak': ('out->hierarchy_gpu, staging, &out->hierarchy_gpu', 'out->hierarchy_gpu, 0, &out->hierarchy_gpu'),
    'serialize hierarchy': ('row * (hierarchy ? 6 : 4)', 'row * (hierarchy ? 4 : 4)'),
}
with tempfile.TemporaryDirectory(prefix='pgcuvs-nn-capacity-') as tmp:
    dst = Path(tmp)
    for name, (old, new) in mutations.items():
        assert header.count(old) == 1, name
        (dst / 'cuvs_hnsw_nn_capacity.h').write_text(header.replace(old, new))
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
