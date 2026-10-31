/*
 * SPDX-FileCopyrightText: Copyright (c) 2026, JAESOL SHIN
 * SPDX-License-Identifier: PostgreSQL
 */

#include <cuvs/neighbors/hnsw.hpp>
#include <raft/core/host_mdarray.hpp>
#include <raft/core/host_mdspan.hpp>

#include <cuda_runtime_api.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

extern "C" const char* pgcuvs_hnsw_memory_guard_patch_id();
extern "C" const char* pgcuvs_hnsw_disk_copy_patch_id();

namespace {

constexpr char kMemoryGuardPatch[] =
  "pgcuvs-hnsw-memory-guard-v2@60b19a0f62ba0bd716de349382828e43be6e2daa";
constexpr char kDiskCopyPatch[] =
  "pgcuvs-hnsw-disk-copy-v1@60b19a0f62ba0bd716de349382828e43be6e2daa";
constexpr std::size_t kRows = 8192;
constexpr std::size_t kDim = 16;

void require_gpu()
{
  int count = 0;
  if (cudaGetDeviceCount(&count) != cudaSuccess || count < 1) {
    throw std::runtime_error("pilot requires a visible CUDA GPU");
  }
  if (cudaDeviceSynchronize() != cudaSuccess) {
    throw std::runtime_error("CUDA synchronization failed");
  }
}

void require_empty_directory(const std::filesystem::path& path)
{
  std::filesystem::create_directories(path);
  if (!std::filesystem::is_empty(path)) {
    throw std::runtime_error("each build directory must be empty");
  }
}

}  // namespace

int main(int argc, char** argv)
{
  try {
    if (argc != 2) {
      std::cerr << "usage: cuvs_hnsw_guard_ace_budget_pilot DISK_ROOT\n";
      return 2;
    }
    const char* memory_marker = pgcuvs_hnsw_memory_guard_patch_id();
    const char* disk_marker = pgcuvs_hnsw_disk_copy_patch_id();
    if (memory_marker == nullptr || std::string(memory_marker) != kMemoryGuardPatch ||
        disk_marker == nullptr || std::string(disk_marker) != kDiskCopyPatch) {
      throw std::runtime_error("loaded libcuvs does not expose both expected patch markers");
    }
    require_gpu();

    const auto disk_root = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(disk_root);
    const auto reject_host_dir = disk_root / "guard-reject-host";
    const auto reject_gpu_dir = disk_root / "guard-reject-gpu";
    const auto viable_dir = disk_root / "guard-viable";
    require_empty_directory(reject_host_dir);
    require_empty_directory(reject_gpu_dir);
    require_empty_directory(viable_dir);

    raft::resources resources;
    auto dataset = raft::make_host_matrix<float, std::int64_t>(resources, kRows, kDim);
    for (std::size_t row = 0; row < kRows; ++row) {
      // Deliberately skewed two-cluster input exercises real partition histograms.
      const float center = row < (kRows * 7 / 8) ? -1.0F : 3.0F;
      for (std::size_t col = 0; col < kDim; ++col) {
        const float ripple = static_cast<float>((row * (col + 3)) % 97) / 97.0F;
        dataset(row, col) = center + ripple * 0.02F;
      }
    }

    cuvs::neighbors::hnsw::index_params params;
    params.metric = cuvs::distance::DistanceType::L2Expanded;
    params.hierarchy = cuvs::neighbors::hnsw::HnswHierarchy::GPU;
    params.M = 16;
    params.ef_construction = 80;
    auto ace = cuvs::neighbors::hnsw::graph_build_params::ace_params{};
    ace.npartitions = 4;
    ace.use_disk = true;
    params.graph_build_params = ace;

    auto expect_budget_rejection = [&](const std::string& label,
                                       const std::filesystem::path& directory,
                                       double host_budget_gib,
                                       double gpu_budget_gib) {
      ace.build_dir = directory.string();
      ace.max_host_memory_gb = host_budget_gib;
      ace.max_gpu_memory_gb = gpu_budget_gib;
      params.graph_build_params = ace;
      try {
        (void)cuvs::neighbors::hnsw::build(
          resources, params, raft::make_const_mdspan(dataset.view()));
      } catch (const std::exception& error) {
        if (std::string(error.what()).find("memory guard") != std::string::npos) {
          std::cout << "PASS: " << label << " budget rejected before the offending guarded allocation: "
                    << error.what() << '\n';
          return;
        }
        throw;
      }
      throw std::runtime_error(label + " budget was not rejected by a memory guard");
    };

    expect_budget_rejection("host", reject_host_dir, 0.0003, 1.0);
    expect_budget_rejection("GPU", reject_gpu_dir, 1.0, 0.0003);

    ace.build_dir = viable_dir.string();
    ace.max_host_memory_gb = 1.0;
    ace.max_gpu_memory_gb = 1.0;
    params.graph_build_params = ace;
    auto index = cuvs::neighbors::hnsw::build(
      resources, params, raft::make_const_mdspan(dataset.view()));
    if (!index) { throw std::runtime_error("bounded ACE build returned a null index"); }
    if (index->file_path() != (viable_dir / "hnsw_index.bin").string()) {
      throw std::runtime_error("bounded ACE build did not return its disk index");
    }
    for (const auto* artifact : {"reordered_dataset.npy", "cagra_graph.npy",
                                 "dataset_mapping.npy", "hnsw_index.bin"}) {
      if (!std::filesystem::is_regular_file(viable_dir / artifact)) {
        throw std::runtime_error(std::string("bounded ACE build is missing ") + artifact);
      }
    }
    std::cout << "PASS: skewed " << kRows << "x" << kDim
              << " HNSW/ACE disk build completed within positive 1 GiB host/GPU budgets\n"
              << "PASS: markers=" << memory_marker << ',' << disk_marker
              << "; disk_root=" << disk_root << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
