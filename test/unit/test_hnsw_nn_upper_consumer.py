#!/usr/bin/env python3
"""Run the patched SDK's actual resident-upper parameter code in a CPU fixture."""
import argparse
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
ap = argparse.ArgumentParser()
ap.add_argument('--sdk-src', type=Path, required=True, help='patched cpp/src/neighbors')
sdk = ap.parse_args().sdk_src / 'detail'
source = (sdk / 'hnsw.hpp').read_text()
def block_at(anchor):
    start = source.index(anchor)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        if source[end] == '{': depth += 1
        if source[end] == '}': depth -= 1
        end += 1
    return source[opening + 1:end - 1]
body = block_at('if (memory_guard::hnsw_upper_level_uses_nn_descent(rows, degree, has_memory_budget))')
start = source.index('  const auto resident_guard =')
end = source.index(': std::nullopt;', source.index('const auto resident_gpu_budget', start)) + len(': std::nullopt;')
guard = source[start:end]
prefix = r'''
#include "cuvs_hnsw_nn_upper_contract.h"
#include "hnsw_precision.hpp"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
using std::size_t;
namespace cuvs::distance { enum class DistanceType { L2Expanded, L2SqrtExpanded, InnerProduct, CosineExpanded }; }
#define RAFT_EXPECTS(condition, ...) do { if (!(condition)) throw std::runtime_error("guard"); } while(0)
#define RAFT_LOG_INFO(...) do {} while(0)
namespace graph_build_params { struct ace_params { size_t npartitions=0; bool use_disk=false; double max_host_memory_gb=0, max_gpu_memory_gb=0; }; }
struct Params { std::variant<std::monostate,graph_build_params::ace_params> graph_build_params; };
namespace memory_guard {
static uint64_t captured_gpu = 0;
inline size_t checked_add(size_t a, size_t b) { return a+b; }
inline size_t checked_mul(size_t a, size_t b) { return a*b; }
inline std::pair<size_t,size_t> nn_descent_workspace_bytes(size_t, size_t, size_t) { return {100,200}; }
inline std::optional<size_t> budget_bytes_from_gib(double n) { return static_cast<size_t>(n*1073741824); }
static uint64_t captured_host = 0;
inline void require_within(const char *stage, size_t bytes, std::optional<size_t> limit) {
  if (std::strstr(stage, "GPU")) captured_gpu=bytes;
  if (std::strstr(stage, "host")) captured_host=bytes;
  if (limit && bytes>*limit) throw std::runtime_error("budget");
}
}
namespace upper_precision = cuvs::neighbors::hnsw::detail::upper_precision;
namespace nn_descent {
enum class DIST_COMP_DTYPE { AUTO, FP32, FP16 };
struct index_params { size_t graph_degree=0, intermediate_graph_degree=0, max_iterations=20; cuvs::distance::DistanceType metric=cuvs::distance::DistanceType::L2Expanded; bool return_distances=true; DIST_COMP_DTYPE dist_comp_dtype=DIST_COMP_DTYPE::AUTO; };
static index_params consumed;
static std::vector<float> consumed_input;  /* the rows NN-descent actually received */
struct Result { ~Result() {} };
template<class D> Result build(int, index_params p, const D& d, int) {
  consumed=p; consumed_input.assign(d.data_handle(), d.data_handle() + 34); return {}; }
}
struct Dataset { const float *data; const float *data_handle() const { return data; } };
namespace raft {
template<class T, class I> Dataset make_host_matrix_view(T* p, I, I) { return Dataset{p}; }
}
static cuvs::distance::DistanceType g_metric = cuvs::distance::DistanceType::L2Expanded;
template<class T> void sdk_upper(int res, const Dataset& dataset, int neighbors, cuvs::distance::DistanceType metric,
 size_t rows, size_t dim, size_t degree, std::optional<size_t> host_budget,
 std::optional<size_t> gpu_budget, size_t resident_nn_dtype) {
  using memory_guard::checked_add; using memory_guard::checked_mul; using memory_guard::require_within;
  const bool resident_nn_upper = resident_nn_dtype == PGC_UVS_NN_UPPER_FP16 || resident_nn_dtype == PGC_UVS_NN_UPPER_FP32;
'''
middle = r'''
}
static void run(const Params& params, const Dataset& dataset) {
'''
suffix = r'''
    if (resident_nn_upper) {
      assert(resident_host_budget && *resident_host_budget == (size_t)(31.75 * 1073741824));
      assert(resident_gpu_budget && *resident_gpu_budget == (size_t)(31.5 * 1073741824));
    } else assert(!resident_host_budget && !resident_gpu_budget);
    sdk_upper<float>(0,dataset,0,g_metric,2,17,8,resident_host_budget,resident_gpu_budget,
                     resident_nn_upper ? resident_guard.npartitions : 0);
}
int main() {
  float data[34] = {}; data[0] = 1.0f; Dataset dataset{data}; Params params;
  graph_build_params::ace_params upper; upper.use_disk=false;
  upper.max_host_memory_gb=31.75; upper.max_gpu_memory_gb=31.5;
  upper.npartitions=PGC_UVS_NN_UPPER_FP32; params.graph_build_params=upper;
  run(params,dataset);
  assert(nn_descent::consumed.dist_comp_dtype == nn_descent::DIST_COMP_DTYPE::FP32);
  assert(nn_descent::consumed.graph_degree == 8 && nn_descent::consumed.intermediate_graph_degree == 16);
  assert(nn_descent::consumed.max_iterations == 20 && !nn_descent::consumed.return_distances);
  assert(memory_guard::captured_gpu == 200 + 2 * 17 * 4);
  upper.npartitions=PGC_UVS_NN_UPPER_FP16; params.graph_build_params=upper;
  run(params,dataset);
  assert(nn_descent::consumed.dist_comp_dtype == nn_descent::DIST_COMP_DTYPE::FP16);
  assert(memory_guard::captured_gpu == 200);
  data[0]=65505.0f;
  bool rejected=false; try { run(params,dataset); } catch(const std::runtime_error&) { rejected=true; }
  assert(rejected);
  data[0]=1.0f; upper.npartitions=0; params.graph_build_params=upper;
  run(params,dataset);
  assert(nn_descent::consumed.dist_comp_dtype == nn_descent::DIST_COMP_DTYPE::AUTO);
  /* Upper-level centering: resident NN + L2 gives NN-descent
   * this level's column-centered copy and reserves its host bytes; IP/cosine
   * and non-resident calls get the original rows. The caller's rows stay intact. */
  data[0]=1001.0f; for (int j = 1; j < 17; j++) data[j] = 1000.0f;
  for (int j = 0; j < 17; j++) data[17 + j] = 1000.0f;   /* two rows, offset 1000 */
  const std::vector<float> original(data, data + 34);
  upper.npartitions=PGC_UVS_NN_UPPER_FP32; params.graph_build_params=upper;
  g_metric = cuvs::distance::DistanceType::L2Expanded; run(params,dataset);
  assert(std::vector<float>(data, data + 34) == original);
  assert(nn_descent::consumed_input[0] == 0.5f && nn_descent::consumed_input[17] == -0.5f);
  for (int j = 1; j < 17; j++) assert(nn_descent::consumed_input[j] == 0.0f && nn_descent::consumed_input[17 + j] == 0.0f);
  assert(memory_guard::captured_host == 100 + 2 * 17 * sizeof(float));
  g_metric = cuvs::distance::DistanceType::L2SqrtExpanded; run(params,dataset);
  assert(nn_descent::consumed_input[0] == 0.5f);
  g_metric = cuvs::distance::DistanceType::InnerProduct; run(params,dataset);
  assert(nn_descent::consumed_input == original && memory_guard::captured_host == 100);
  g_metric = cuvs::distance::DistanceType::CosineExpanded; run(params,dataset);
  assert(nn_descent::consumed_input == original);
  g_metric = cuvs::distance::DistanceType::L2Expanded; upper.npartitions=0; params.graph_build_params=upper;
  run(params,dataset);
  assert(nn_descent::consumed_input == original);
  puts("SDK resident upper params/budgets/centering: PASS (CPU fixture)");
}
'''
mutations = {
    'upper fp32 lost': (body.replace('resident_fp32 = resident_nn_dtype == PGC_UVS_NN_UPPER_FP32;', 'resident_fp32 = false;'), guard),
    'upper budget omitted': (body.replace('if (resident_fp32) gpu_workspace = checked_add(gpu_workspace,\n        checked_mul(checked_mul(rows, dim), sizeof(float)));', 'if (resident_fp32) gpu_workspace = checked_add(gpu_workspace, 0);'), guard),
    'upper dtype reverted': (body.replace('nn_params.dist_comp_dtype = resident_fp32 ? nn_descent::DIST_COMP_DTYPE::FP32\n                                                  : nn_descent::DIST_COMP_DTYPE::FP16;', 'nn_params.dist_comp_dtype = nn_descent::DIST_COMP_DTYPE::AUTO;'), guard),
    'private marker ignored': (body, guard.replace('resident_guard.npartitions == PGC_UVS_NN_UPPER_FP32', 'resident_guard.npartitions == 0')),
    # upper-level centering
    'upper centering skipped': (body.replace('if (center_upper) {\n        centered.resize', 'if (false) {\n        centered.resize'), guard),
    'upper centering for IP too': (body.replace('center_upper = resident_nn_upper &&', 'center_upper = resident_nn_upper || '), guard),
    'upper centering budget omitted': (body.replace('if (center_upper) host_workspace = checked_add(host_workspace,', 'if (false) host_workspace = checked_add(host_workspace,'), guard),
    'upper centered copy unused': (body.replace('nn_input = raft::make_host_matrix_view<const T, int64_t>(', '(void)raft::make_host_matrix_view<const T, int64_t>('), guard),
}
for _name, (_b, _g) in mutations.items():
    assert _b != body or _g != guard, ('MUTANT_ANCHOR', _name)
with tempfile.TemporaryDirectory(prefix='pgcuvs-nn-upper-consumer-') as tmp:
    dst=Path(tmp)
    for name,(b,g) in [('baseline',(body,guard)),*mutations.items()]:
        assert name == 'baseline' or b != body or g != guard, name
        (dst/'fixture.cpp').write_text(prefix+b+middle+g+suffix)
        exe=dst/'fixture'
        built=subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(root/'src'),'-I',str(sdk),str(dst/'fixture.cpp'),'-o',str(exe)],capture_output=True,text=True)
        if built.returncode: raise RuntimeError(f'{name}: compile failed: {built.stderr}')
        run=subprocess.run([str(exe)],capture_output=True,text=True)
        if (run.returncode==0)!=(name=='baseline'): raise RuntimeError(f'{name}: rc={run.returncode}: {run.stderr}')
        print(f'{name}: {"PASS" if name == "baseline" else "detected"} (rc={run.returncode})')
