#!/usr/bin/env python3
"""Check that policy tests reject distinct malformed NN receipts."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = (root / 'src/cuvs_hnsw_policy.h').read_text()
test = root / 'test/unit/test_hnsw_nn_policy.c'
mutations = {
    'wrong builder': ('&& p->internal_builder == CUVS_HNSW_ROUTE_NN_DESCENT', '&& 1'),
    'training applicable': ('&& !p->n_probes && !p->query_batch && !p->training_requested && !p->training_actual', '&& !p->n_probes && !p->query_batch && !p->training_requested'),
    'wrong internal degree': ('&& r->internal_degree == internal && r->sdk_default_iterations > 0', '&& internal > 0 && r->sdk_default_iterations > 0'),
    'unknown dtype': ('&& (r->dtype == CUVS_HNSW_NN_DTYPE_FP16 || r->dtype == CUVS_HNSW_NN_DTYPE_FP32)', '&& 1'),
    'missing max iterations': ('r->max_iterations == cuvs_hnsw_scaled_quality(r->sdk_default_iterations,\n                                                          p->requested_ef, 1, 65535)', 'r->max_iterations <= 65535'),
    'wrong termination': ('&& r->termination_threshold == 0.0001f;', ';'),
}
with tempfile.TemporaryDirectory(prefix='pgcuvs-nn-policy-') as tmp:
    dst = Path(tmp)
    for name, (old, new) in mutations.items():
        assert header.count(old) == 1, name
        (dst / 'cuvs_hnsw_policy.h').write_text(header.replace(old, new))
        exe = dst / 'check'
        built = subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                '-I', str(dst), str(test), '-o', str(exe)], capture_output=True, text=True)
        if built.returncode:
            raise RuntimeError(f'{name}: mutation did not compile: {built.stderr}')
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        if not run.returncode:
            raise RuntimeError(f'{name}: mutation survived')
        print(f'{name}: detected (rc={run.returncode})')
