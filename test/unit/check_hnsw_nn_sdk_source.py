#!/usr/bin/env python3
"""Check B3 source branch binding; this is static, not native execution."""
from pathlib import Path
root = Path(__file__).resolve().parents[2]
sdk = root / 'evidence/nn-descent-implementation-20261115/sdk-b3-nn/cpp/src/neighbors'
h = (sdk / 'detail/hnsw.hpp').read_text()
core = (sdk / 'detail/cagra/cagra_build.cuh').read_text()
marker = (sdk / 'hnsw.cpp').read_text()
wrapper = (root / 'src/cuvs_wrapper.cu').read_text()
checks = {
  'SDK resets mismatched degree': 'nn_descent_params.graph_degree != intermediate_degree' in core and 'nn_descent_params =\n          cagra::graph_build_params::nn_descent_params(intermediate_degree, params.metric)' in core,
  'NN wrapper asserts degree and metric': 'selected.graph_degree != cp.intermediate_graph_degree || selected.metric != cp.metric' in wrapper,
  'resident hierarchy budgets reach upper': 'cagra_index.metric(), resident_host_budget, resident_gpu_budget,\n                        resident_nn_upper' in h,
  'resident FP32 has extra device budget': 'if (resident_fp32) gpu_workspace = checked_add(gpu_workspace,' in h,
  'upper actual dtype propagated': 'if (resident_fp32) nn_params.dist_comp_dtype = nn_descent::DIST_COMP_DTYPE::FP32;' in h,
  'marker exported and checked': 'pgcuvs_hnsw_resident_nn_upper_patch_id' in marker and 'pgcuvs_hnsw_resident_nn_upper_patch_id' in wrapper,
}
for name, ok in checks.items():
    print(f'{name}: {"PASS" if ok else "FAIL"} (source only)')
if not all(checks.values()): raise SystemExit(1)
