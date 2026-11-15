#!/usr/bin/env python3
"""Check the patched SDK source branch binding; this is static, not native execution."""
import argparse
import json
from pathlib import Path
root = Path(__file__).resolve().parents[2]
ap = argparse.ArgumentParser()
ap.add_argument('--sdk-src', type=Path, required=True,
                help='cpp/src/neighbors of upstream 60b19a0f + infra/cuvs-patches 0001..0005 '
                     '(scripts/prepare-patched-cuvs-source.sh)')
sdk = ap.parse_args().sdk_src
expected_marker = json.loads((root / 'infra/cuvs-patches/patchset-manifest.json').read_text())['runtime_markers']['resident_nn_upper']['value']
h = (sdk / 'detail/hnsw.hpp').read_text()
core = (sdk / 'detail/cagra/cagra_build.cuh').read_text()
marker = (sdk / 'hnsw.cpp').read_text()
wrapper = (root / 'src/cuvs_wrapper.cu').read_text()
checks = {
  'SDK resets mismatched degree': 'nn_descent_params.graph_degree != intermediate_degree' in core and 'nn_descent_params =\n          cagra::graph_build_params::nn_descent_params(intermediate_degree, params.metric)' in core,
  'NN wrapper asserts degree and metric': 'selected.graph_degree != cp.intermediate_graph_degree || selected.metric != cp.metric' in wrapper,
  'resident hierarchy budgets and dtype reach upper': 'cagra_index.metric(), resident_host_budget, resident_gpu_budget,\n                        resident_nn_upper ? resident_guard.npartitions : 0' in h,
  'resident FP32 has extra device budget': 'if (resident_fp32) gpu_workspace = checked_add(gpu_workspace,' in h,
  'upper actual dtype propagated': 'nn_params.dist_comp_dtype = resident_fp32 ? nn_descent::DIST_COMP_DTYPE::FP32' in h,
  'private contracts match': (root / 'src/cuvs_hnsw_nn_upper_contract.h').read_bytes() == (sdk / 'detail/cuvs_hnsw_nn_upper_contract.h').read_bytes(),
  'manifest marker exported and checked': ('"' + expected_marker + '"') in marker and ('"' + expected_marker + '"') in wrapper,
}
for name, ok in checks.items():
    print(f'{name}: {"PASS" if ok else "FAIL"} (source only)')
if not all(checks.values()): raise SystemExit(1)
