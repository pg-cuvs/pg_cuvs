#!/usr/bin/env python3
"""Decision B guard mutants: each must make test_hnsw_nn_center.c fail, and the
wrapper source contract must hold (centered copy only reaches cagra::build;
both serializers receive the original `data`). CPU only."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
test = root / 'test/unit/test_hnsw_nn_center.c'
center = (root / 'src/cuvs_hnsw_nn_center.h').read_text()
precision = (root / 'src/cuvs_hnsw_nn_precision.h').read_text()
wrapper = (root / 'src/cuvs_wrapper.cu').read_text()

# 1. wrapper source contract
build = wrapper.index('            if (!tiny && prepared.route.route == CUVS_HNSW_ROUTE_NN_DESCENT\n                && cuvs_hnsw_nn_centers(params->metric))')
block = wrapper[build:wrapper.index('            if (!tiny) return cagra::build(res, cp, data);', build)]
assert 'return cagra::build(res, cp, centered_view);' in block
assert wrapper.count('centered_view') == 2 and wrapper.count('centered.data()') == 2, 'centered copy leaks beyond the graph call'
assert wrapper.count('cagra::serialize_to_hnswlib(res, output, graph, data);') == 1
assert wrapper.count('hnsw::from_cagra(res, hp, graph, data);') == 1
assert 'cuvs_hnsw_nn_precision_ex(static_cast<const float *>(files.mapping),' in wrapper
assert 'cuvs_hnsw_nn_centers(params->metric),' in wrapper
# Text-level contract only; test_hnsw_nn_center_wrapper.py executes this code.
# Upper hierarchy levels are centered inside the SDK (patch 0005), not here.
print('wrapper contract (text): graph input centered, serializers get original data')

mutants = {
    'no subtraction': ('center', '(float)((double)data[k] - mean[j])', '(float)((double)data[k])'),
    'float32 mean': ('center', 'mean[j] += (double)data[(size_t)i * dim + j];', 'mean[j] = (float)(mean[j] + data[(size_t)i * dim + j]);'),
    'centers IP too': ('center', 'return metric == CUVS_METRIC_L2;', 'return metric <= CUVS_METRIC_IP;'),
    'precision ignores centered flag': ('precision', '!graph_input_centered && ', ''),
    'dtype from centered rows': ('precision', 'double half_bound = metric == CUVS_METRIC_L2 ? 4.0 * max_norm : max_norm;',
                                 'double half_bound = graph_input_centered ? 4.0 * out->spread : metric == CUVS_METRIC_L2 ? 4.0 * max_norm : max_norm;'),
}
with tempfile.TemporaryDirectory(prefix='pgcuvs-nn-center-') as tmp:
    d = Path(tmp)
    for name, (which, old, new) in mutants.items():
        files = {'center': center, 'precision': precision}
        assert files[which].count(old) == 1, name
        files[which] = files[which].replace(old, new)
        (d / 'cuvs_hnsw_nn_center.h').write_text(files['center'])
        (d / 'cuvs_hnsw_nn_precision.h').write_text(files['precision'])
        exe = d / 'check'
        b = subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-I', str(d), '-I', str(root / 'src'),
                            str(test), '-lm', '-o', str(exe)], capture_output=True, text=True)
        if b.returncode:
            raise RuntimeError(f'{name}: did not compile: {b.stderr}')
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        if r.returncode == 0:
            raise RuntimeError(f'{name}: survived')
        print(f'{name}: detected (rc={r.returncode})')
