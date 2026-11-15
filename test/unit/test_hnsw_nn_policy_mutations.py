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


def build_and_run(dst, text):
    (dst / 'cuvs_hnsw_policy.h').write_text(text)
    exe = dst / 'check'
    built = subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(dst), str(test), '-o', str(exe)], capture_output=True, text=True)
    if built.returncode:
        return built, None
    return built, subprocess.run([str(exe)], capture_output=True, text=True)


with tempfile.TemporaryDirectory(prefix='pgcuvs-nn-policy-') as tmp:
    dst = Path(tmp)
    # A mutant only counts when the same build passes without the mutation:
    # against a test that already fails, every mutant would "fail".
    built, run = build_and_run(dst, header)
    if run is None or run.returncode:
        raise RuntimeError('BASELINE_NOT_GREEN: test_hnsw_nn_policy.c does not pass on the unmutated header; '
                           'no mutant result is meaningful. ' + (built.stderr if run is None else run.stdout + run.stderr))
    for name, (old, new) in mutations.items():
        assert header.count(old) == 1, name
        built, run = build_and_run(dst, header.replace(old, new))
        if run is None:
            raise RuntimeError(f'{name}: mutation did not compile: {built.stderr}')
        if not run.returncode:
            raise RuntimeError(f'{name}: mutation survived')
        print(f'{name}: detected (rc={run.returncode})')
