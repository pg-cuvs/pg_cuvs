/*
 * SPDX-FileCopyrightText: Copyright (c) 2026, JAESOL SHIN
 * SPDX-License-Identifier: PostgreSQL
 */

#include <cuvs/distance/distance.hpp>
#include <cuvs/neighbors/hnsw.hpp>
#include <raft/core/host_mdarray.hpp>
#include <raft/core/host_mdspan.hpp>

#include <cuda_runtime_api.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr std::size_t kRows = 8192;
constexpr std::size_t kDim = 16;
constexpr double kHostBudgetGiB = 0.007;
constexpr double kGpuBudgetGiB = 1.0;

void require_gpu()
{
  int count = 0;
  if (cudaGetDeviceCount(&count) != cudaSuccess || count < 1) {
    throw std::runtime_error("probe requires a visible CUDA GPU");
  }
}

}  // namespace

int main(int argc, char** argv)
{
  try {
    if (argc != 3 || (std::string(argv[1]) != "legacy-admits" &&
                      std::string(argv[1]) != "guard-rejects")) {
      std::cerr << "usage: cuvs_hnsw_ace_subbuilder_budget_probe legacy-admits|guard-rejects NEW_TASK_BUILD_DIR\n";
      return 2;
    }
    const std::string mode = argv[1];
    const std::filesystem::path requested_dir = argv[2];
    const auto task_root = std::filesystem::canonical("/home/ubuntu/pgcuvs-hnsw-001");
    const auto build_dir = std::filesystem::weakly_canonical(requested_dir);
    const auto relative_dir = build_dir.lexically_relative(task_root);
    if (!requested_dir.is_absolute() || relative_dir.empty() || relative_dir == "." ||
        *relative_dir.begin() == ".." || std::filesystem::exists(requested_dir) ||
        std::filesystem::is_symlink(requested_dir)) {
      throw std::runtime_error("ACE probe requires a new build directory below the task root");
    }
    std::cout << "ACE probe task build directory: " << build_dir << '\n';
    require_gpu();

    raft::resources resources;
    auto dataset = raft::make_host_matrix<float, std::int64_t>(resources, kRows, kDim);
    for (std::size_t row = 0; row < kRows; ++row) {
      for (std::size_t col = 0; col < kDim; ++col) {
        dataset(row, col) = static_cast<float>((row * (col + 3) + 11) % 101) / 101.0F;
      }
    }

    cuvs::neighbors::hnsw::index_params params;
    params.metric = cuvs::distance::DistanceType::L2Expanded;
    params.hierarchy = cuvs::neighbors::hnsw::HnswHierarchy::GPU;
    params.M = 4;
    params.ef_construction = 80;
    auto ace = cuvs::neighbors::hnsw::graph_build_params::ace_params{};
    ace.npartitions = 2;
    ace.use_disk = false;
    ace.build_dir = build_dir.string();
    ace.max_host_memory_gb = kHostBudgetGiB;
    ace.max_gpu_memory_gb = kGpuBudgetGiB;
    params.graph_build_params = ace;

    try {
      auto index = cuvs::neighbors::hnsw::build(
        resources, params, raft::make_const_mdspan(dataset.view()));
      if (!index) { throw std::runtime_error("legacy-budget build returned a null index"); }
      if (mode == "guard-rejects") {
        throw std::runtime_error("bounded ACE build admitted the deliberately undersized NN-descent host budget");
      }
      std::cout << "PASS: legacy guard admits the 0.007 GiB case without an NN-descent builder preflight\n";
      return 0;
    } catch (const std::exception& error) {
      const std::string message = error.what();
      const bool is_builder_guard =
        message.find("memory guard: ACE actual largest partition host NN-descent builder workspace") !=
          std::string::npos ||
        message.find("memory guard: ACE actual partition host NN-descent builder workspace") !=
          std::string::npos;
      if (!is_builder_guard) { throw; }
      if (mode == "legacy-admits") {
        throw std::runtime_error("legacy guard unexpectedly rejected the case at the new NN-descent builder stage");
      }
      std::cout << "PASS: bounded ACE rejected before subdataset allocation: " << message << '\n';
      return 0;
    }
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
