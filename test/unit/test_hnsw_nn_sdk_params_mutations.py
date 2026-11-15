#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
header = (root / 'src/cuvs_hnsw_nn_sdk_params.cuh').read_text()
test = root / 'test/unit/test_hnsw_nn_sdk_params.cpp'
mutations = {
    'reconstructed SDK params': ('NnParams p(intermediate, metric);', 'NnParams p(intermediate / 2, metric);'),
    'fixed iteration count': ('cuvs_hnsw_scaled_quality(*sdk_default_iterations, ef, 1, 65535)', 'cuvs_hnsw_scaled_quality(*sdk_default_iterations, 64, 1, 65535) + 0 * ef'),
    'wrong dtype': ('p.dist_comp_dtype = dtype;', 'p.dist_comp_dtype = static_cast<DType>(0); (void)dtype;'),
}
with tempfile.TemporaryDirectory(prefix='pgcuvs-nn-sdk-') as tmp:
    dst = Path(tmp)
    for name, (old, new) in mutations.items():
        assert header.count(old) == 1, name
        (dst / 'cuvs_hnsw_nn_sdk_params.cuh').write_text(header.replace(old, new))
        exe = dst / 'check'
        built = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(dst),
                                '-I', str(root / 'src'), str(test), '-o', str(exe)], capture_output=True, text=True)
        if built.returncode: raise RuntimeError(f'{name}: did not compile: {built.stderr}')
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        if not run.returncode: raise RuntimeError(f'{name}: survived')
        print(f'{name}: detected (rc={run.returncode})')
