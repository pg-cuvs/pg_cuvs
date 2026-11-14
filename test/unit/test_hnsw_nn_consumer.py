#!/usr/bin/env python3
"""Execute the production NN wrapper branch with a CPU model of B3's consumer."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
wrapper = (root / 'src/cuvs_wrapper.cu').read_text()
start = wrapper.index('        else if (prepared.route.route == CUVS_HNSW_ROUTE_NN_DESCENT)')
brace = wrapper.index('{', start)
depth = 1
end = brace + 1
while depth:
    if wrapper[end] == '{': depth += 1
    if wrapper[end] == '}': depth -= 1
    end += 1
branch = wrapper[start:end].replace('else if', 'if', 1)
helper = (root / 'src/cuvs_hnsw_nn_sdk_params.cuh').read_text()
prefix = r'''
#include "cuvs_route_preflight.h"
#include "cuvs_hnsw_nn_precision.h"
#include "cuvs_hnsw_nn_sdk_params.cuh"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <variant>
namespace cuvs::neighbors::nn_descent { enum class DIST_COMP_DTYPE { AUTO, FP32, FP16 }; }
namespace cagra::graph_build_params {
struct nn_descent_params {
    size_t graph_degree, intermediate_graph_degree, max_iterations;
    int metric;
    float termination_threshold;
    bool return_distances;
    cuvs::neighbors::nn_descent::DIST_COMP_DTYPE dist_comp_dtype;
    nn_descent_params(size_t d, int m): graph_degree(d), intermediate_graph_degree(d*3/2),
        max_iterations(20), metric(m), termination_threshold(.0001f), return_distances(true),
        dist_comp_dtype(cuvs::neighbors::nn_descent::DIST_COMP_DTYPE::AUTO) {}
};
struct ivf_pq_params {};
struct ace_params { bool use_disk; double max_host_memory_gb, max_gpu_memory_gb; };
}
struct CP { size_t intermediate_graph_degree; int metric; std::variant<std::monostate,
  cagra::graph_build_params::nn_descent_params, cagra::graph_build_params::ivf_pq_params,
  cagra::graph_build_params::ace_params> graph_build_params; };
struct HP { std::variant<std::monostate, cagra::graph_build_params::ace_params> graph_build_params; };
struct Params { uint64_t n_vecs; uint32_t ef_construction, hierarchy; };
struct Prepared { CuvsHnswRouteDecision route; CuvsHnswNnPrecision nn_precision;
  CuvsHnswNnReadback nn_readback; };
int main() {
    char error[256] = {};
    CuvsHnswForecast forecast{};
    assert(cuvs_hnsw_forecast_route(10000, 128, 64, 128, 0, CUVS_HNSW_ALGO_NN_DESCENT,
      128, CUVS_HNSW_OVERFLOW_ACE, 32ULL<<30, 32ULL<<30, &forecast, error, sizeof(error)) == CUVS_STATUS_OK);
    Prepared prepared{}; prepared.route = forecast.decision;
    float input[128] = {}; input[0] = 65505.0f;
    assert(cuvs_hnsw_nn_precision(input, 1, 128, CUVS_METRIC_L2, &prepared.nn_precision) == 0);
    Params value{10000, 128, 0}, *params = &value;
    uint32_t intermediate = 256; CP cp{intermediate, 7, {}}; HP hp{};
    uint64_t sdk_host_budget = 32ULL<<30, sdk_gpu_budget = 32ULL<<30;
    const char *workdir = "cpu-fixture";
'''
suffix = r'''
    using NN = cagra::graph_build_params::nn_descent_params;
    auto consumed = std::get<NN>(cp.graph_build_params);
    if (consumed.graph_degree != cp.intermediate_graph_degree) consumed = NN(cp.intermediate_graph_degree, cp.metric);
    assert(consumed.graph_degree == 256 && consumed.max_iterations == 40);
    assert(consumed.dist_comp_dtype == cuvs::neighbors::nn_descent::DIST_COMP_DTYPE::FP32);
    CuvsHnswPolicy receipt{};
    receipt.magic = CUVS_HNSW_POLICY_MAGIC; receipt.version = CUVS_HNSW_POLICY_NN_VERSION;
    receipt.bytes = sizeof(receipt); receipt.route = prepared.route.route;
    receipt.requested_algo = CUVS_HNSW_ALGO_NN_DESCENT; receipt.requested_ef = 128;
    receipt.degree = 64; receipt.intermediate = 256; receipt.guard_passed = 1;
    receipt.internal_builder = CUVS_HNSW_ROUTE_NN_DESCENT; receipt.n_rows = 10000; receipt.dim = 128;
    assert(cuvs_hnsw_nn_readback_valid(&prepared.nn_readback, &receipt));
    assert(prepared.nn_readback.max_iterations == consumed.max_iterations);
    struct Page { CuvsHnswPolicy policy; CuvsHnswNnReadback nn; } page{receipt, prepared.nn_readback};
    CuvsHnswPolicy read{};
    assert(cuvs_hnsw_policy_read(&page, sizeof(page), &read) == CUVS_HNSW_POLICY_RECORDED);
    assert(read.route == CUVS_HNSW_ROUTE_NN_DESCENT);
    puts("production NN wrapper branch -> modeled B3 -> receipt: PASS (CPU mock)");
}
'''
mutations = {
    'wrong builder': (branch.replace('cp.graph_build_params = nn;', '(void)nn; cp.graph_build_params = cagra::graph_build_params::ivf_pq_params{};'), helper),
    'SDK reconstruction': (branch.replace('                intermediate, cp.metric, params->ef_construction,', '                intermediate - 1, cp.metric, params->ef_construction,'), helper),
    'wrong effective iterations': (branch, helper.replace('cuvs_hnsw_scaled_quality(*sdk_default_iterations, ef, 1, 65535)', 'cuvs_hnsw_scaled_quality(*sdk_default_iterations, 64, 1, 65535) + 0 * ef')),
}
with tempfile.TemporaryDirectory(prefix='pgcuvs-nn-consumer-') as tmp:
    dst = Path(tmp)
    for name, (source, h) in [('baseline', (branch, helper)), *mutations.items()]:
        assert (source != branch or h != helper) if name != 'baseline' else True
        (dst / 'cuvs_hnsw_nn_sdk_params.cuh').write_text(h)
        (dst / 'fixture.cpp').write_text(prefix + source + suffix)
        exe = dst / 'fixture'
        built = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(dst), '-I', str(root / 'src'), str(dst / 'fixture.cpp'), '-lpthread', '-lm', '-o', str(exe)], capture_output=True, text=True)
        if built.returncode: raise RuntimeError(f'{name}: compile failed: {built.stderr}')
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        if (run.returncode == 0) != (name == 'baseline'):
            raise RuntimeError(f'{name}: unexpected rc {run.returncode}: {run.stderr}')
        print(f'{name}: {"PASS" if name == "baseline" else "detected"} (rc={run.returncode})')
