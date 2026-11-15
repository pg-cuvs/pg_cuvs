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
    'centered copy dropped': ('if (centered_copy) build_row += row;', '(void)centered_copy;'),
    'centered copy for every metric': ('if (centered_copy) build_row += row;', 'build_row += row;'),
}


def build_and_run(dst, text):
    (dst / 'cuvs_hnsw_nn_capacity.h').write_text(text)
    exe = dst / 'check'
    built = subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(dst), '-I', str(root / 'src'),
                            str(test), '-lm', '-o', str(exe)], capture_output=True, text=True)
    if built.returncode:
        return built, None
    return built, subprocess.run([str(exe)], capture_output=True, text=True)


with tempfile.TemporaryDirectory(prefix='pgcuvs-nn-capacity-') as tmp:
    dst = Path(tmp)
    # A mutant only counts when the same build passes without the mutation:
    # against a test that already fails, every mutant would "fail".
    built, run = build_and_run(dst, header)
    if run is None or run.returncode:
        raise RuntimeError('BASELINE_NOT_GREEN: test_hnsw_nn_capacity.c does not pass on the unmutated header; '
                           'no mutant result is meaningful. ' + (built.stderr if run is None else run.stdout + run.stderr))
    for name, (old, new) in mutations.items():
        assert header.count(old) == 1, name
        built, run = build_and_run(dst, header.replace(old, new))
        if run is None:
            raise RuntimeError(f'{name}: mutation did not compile: {built.stderr}')
        if not run.returncode:
            raise RuntimeError(f'{name}: mutation survived')
        print(f'{name}: detected (rc={run.returncode})')
