/*
 * SPDX-FileCopyrightText: Copyright (c) 2026, JAESOL SHIN
 * SPDX-License-Identifier: PostgreSQL
 */

#include <cuvs/distance/distance.hpp>
#include <cuvs/neighbors/hnsw.hpp>
#include <raft/core/host_mdarray.hpp>
#include <raft/core/host_mdspan.hpp>

#include <cuda_runtime_api.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" const char* pgcuvs_hnsw_memory_guard_patch_id();
extern "C" const char* pgcuvs_hnsw_disk_copy_patch_id();

namespace {

constexpr char kMemoryGuardPatch[] =
  "pgcuvs-hnsw-memory-guard-v2@60b19a0f62ba0bd716de349382828e43be6e2daa";
constexpr char kDiskCopyPatch[] =
  "pgcuvs-hnsw-disk-copy-v1@60b19a0f62ba0bd716de349382828e43be6e2daa";
constexpr std::size_t kRows = 8192;
constexpr std::size_t kDim = 16;
constexpr std::size_t kQueries = 128;
constexpr std::size_t kK = 10;
constexpr double kMinimumRecallAt10 = 0.80;
constexpr double kMaximumIpVsL2Top10Overlap = 0.70;
constexpr double kHostBudgetGiB = 1.0;
constexpr double kGpuBudgetGiB = 1.0;
constexpr std::size_t kPartitions = 4;
constexpr std::size_t kM = 32;
constexpr std::size_t kEfConstruction = 120;
constexpr int kEfSearch = 300;

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
    throw std::runtime_error("ACE build directory must be empty");
  }
}

template <typename Score>
std::vector<std::uint64_t> exact_top_k(Score score, bool descending)
{
  std::vector<std::pair<float, std::uint64_t>> ranked;
  ranked.reserve(kRows);
  for (std::size_t row = 0; row < kRows; ++row) {
    ranked.emplace_back(score(row), static_cast<std::uint64_t>(row));
  }
  const auto better = [descending](const auto& lhs, const auto& rhs) {
    if (lhs.first != rhs.first) { return descending ? lhs.first > rhs.first : lhs.first < rhs.first; }
    return lhs.second < rhs.second;
  };
  std::partial_sort(ranked.begin(), ranked.begin() + kK, ranked.end(), better);
  std::vector<std::uint64_t> result;
  result.reserve(kK);
  for (std::size_t rank = 0; rank < kK; ++rank) { result.push_back(ranked[rank].second); }
  return result;
}

}  // namespace

int main(int argc, char** argv)
{
  try {
    if (argc != 2) {
      std::cerr << "usage: cuvs_hnsw_ace_ip_quality_pilot EMPTY_DISK_ROOT\n";
      return 2;
    }
    const char* memory_marker = pgcuvs_hnsw_memory_guard_patch_id();
    const char* disk_marker = pgcuvs_hnsw_disk_copy_patch_id();
    if (memory_marker == nullptr || std::string(memory_marker) != kMemoryGuardPatch ||
        disk_marker == nullptr || std::string(disk_marker) != kDiskCopyPatch) {
      throw std::runtime_error("loaded libcuvs does not expose the expected ACE and disk-copy markers");
    }
    require_gpu();

    const auto build_dir = std::filesystem::absolute(argv[1]);
    require_empty_directory(build_dir);

    raft::resources resources;
    auto dataset = raft::make_host_matrix<float, std::int64_t>(resources, kRows, kDim);
    auto queries  = raft::make_host_matrix<float, std::int64_t>(resources, kQueries, kDim);

    // Deterministic, non-normalized vectors: direction and magnitude vary independently.
    // Raw inner product therefore has a different exact ranking from squared L2 for this corpus.
    constexpr float kPi = 3.14159265358979323846F;
    float min_norm = std::numeric_limits<float>::max();
    float max_norm = 0.0F;
    for (std::size_t row = 0; row < kRows; ++row) {
      const std::size_t direction_code = (row * 7919 + 37) % kRows;
      const std::size_t magnitude_code = (row * 104729 + 83) % 997;
      const float angle = (2.0F * kPi * static_cast<float>(direction_code)) / static_cast<float>(kRows);
      const float scale = 0.35F + 5.65F * static_cast<float>(magnitude_code) / 996.0F;
      float norm_squared = 0.0F;
      for (std::size_t col = 0; col < kDim; ++col) {
        const float value = col == 0   ? scale * std::cos(angle)
                            : col == 1 ? scale * std::sin(angle)
                                       : scale * 0.08F * std::sin(static_cast<float>((row + 1) * (col + 3)) * 0.013F);
        dataset(row, col) = value;
        norm_squared += value * value;
      }
      const float norm = std::sqrt(norm_squared);
      min_norm = std::min(min_norm, norm);
      max_norm = std::max(max_norm, norm);
    }
    for (std::size_t query = 0; query < kQueries; ++query) {
      const float angle = (2.0F * kPi * (static_cast<float>(query) + 0.37F)) /
                          static_cast<float>(kQueries);
      const float scale = 0.75F + 0.45F * static_cast<float>(query % 13);
      for (std::size_t col = 0; col < kDim; ++col) {
        queries(query, col) = col == 0   ? scale * std::cos(angle)
                             : col == 1 ? scale * std::sin(angle)
                                        : scale * 0.06F * std::cos(static_cast<float>((query + 5) * (col + 1)) * 0.11F);
      }
    }
    if (min_norm >= 1.0F || max_norm / min_norm < 5.0F) {
      throw std::runtime_error("dataset does not exercise non-unit, widely varying raw vector norms");
    }

    cuvs::neighbors::hnsw::index_params params;
    params.metric = cuvs::distance::DistanceType::InnerProduct;
    params.hierarchy = cuvs::neighbors::hnsw::HnswHierarchy::GPU;
    params.M = kM;
    params.ef_construction = kEfConstruction;
    auto ace = cuvs::neighbors::hnsw::graph_build_params::ace_params{};
    ace.npartitions = kPartitions;
    ace.use_disk = true;
    ace.build_dir = build_dir.string();
    ace.max_host_memory_gb = kHostBudgetGiB;
    ace.max_gpu_memory_gb = kGpuBudgetGiB;
    params.graph_build_params = ace;

    auto index = cuvs::neighbors::hnsw::build(
      resources, params, raft::make_const_mdspan(dataset.view()));
    if (!index) { throw std::runtime_error("bounded raw-IP ACE build returned a null index"); }
    if (index->metric() != cuvs::distance::DistanceType::InnerProduct) {
      throw std::runtime_error("built HNSW metadata did not preserve InnerProduct");
    }
    if (index->hierarchy() != cuvs::neighbors::hnsw::HnswHierarchy::GPU) {
      throw std::runtime_error("build did not retain GPU hierarchy mode");
    }
    if (index->file_path() != (build_dir / "hnsw_index.bin").string()) {
      throw std::runtime_error("bounded raw-IP ACE build did not return the disk index");
    }
    for (const auto* artifact : {"reordered_dataset.npy", "cagra_graph.npy",
                                 "dataset_mapping.npy", "hnsw_index.bin"}) {
      if (!std::filesystem::is_regular_file(build_dir / artifact)) {
        throw std::runtime_error(std::string("raw-IP ACE build is missing ") + artifact);
      }
    }

    auto neighbors = raft::make_host_matrix<std::uint64_t, std::int64_t>(resources, kQueries, kK);
    auto distances = raft::make_host_matrix<float, std::int64_t>(resources, kQueries, kK);
    cuvs::neighbors::hnsw::search_params search_params;
    search_params.ef = kEfSearch;
    search_params.num_threads = 1;
    cuvs::neighbors::hnsw::search(
      resources, search_params, *index, queries.view(), neighbors.view(), distances.view());

    std::size_t recall_hits = 0;
    std::size_t ip_l2_overlap = 0;
    for (std::size_t query = 0; query < kQueries; ++query) {
      const auto ip_truth = exact_top_k([&](std::size_t row) {
        float score = 0.0F;
        for (std::size_t col = 0; col < kDim; ++col) {
          score += dataset(row, col) * queries(query, col);
        }
        return score;
      }, true);
      const auto l2_truth = exact_top_k([&](std::size_t row) {
        float distance = 0.0F;
        for (std::size_t col = 0; col < kDim; ++col) {
          const float delta = dataset(row, col) - queries(query, col);
          distance += delta * delta;
        }
        return distance;
      }, false);
      for (const auto expected : ip_truth) {
        if (std::find(l2_truth.begin(), l2_truth.end(), expected) != l2_truth.end()) { ++ip_l2_overlap; }
      }
      for (std::size_t rank = 0; rank < kK; ++rank) {
        const auto found = neighbors(query, rank);
        if (found >= kRows) { throw std::runtime_error("search returned a non-input label"); }
        if (std::find(ip_truth.begin(), ip_truth.end(), found) != ip_truth.end()) { ++recall_hits; }
      }
    }
    const double recall_at_10 = static_cast<double>(recall_hits) / (kQueries * kK);
    const double overlap = static_cast<double>(ip_l2_overlap) / (kQueries * kK);
    if (recall_at_10 < kMinimumRecallAt10) {
      throw std::runtime_error("raw-IP HNSW recall@10 is below the declared pilot threshold");
    }
    if (overlap > kMaximumIpVsL2Top10Overlap) {
      throw std::runtime_error("fixture's exact IP and L2 rankings are not distinct enough");
    }

    std::cout << "PASS: bounded raw-IP ACE build and GPU hierarchy; N=" << kRows
              << "; D=" << kDim << "; Q=" << kQueries << "; K=" << kK
              << "; M=" << kM << "; efConstruction=" << kEfConstruction
              << "; efSearch=" << kEfSearch << "; partitions=" << kPartitions
              << "; hostBudgetGiB=" << kHostBudgetGiB << "; gpuBudgetGiB=" << kGpuBudgetGiB
              << "; normRange=[" << min_norm << ',' << max_norm << ']'
              << "; recall@10=" << recall_at_10 << "; minimum=" << kMinimumRecallAt10
              << "; exactIpL2Top10Overlap=" << overlap
              << "; maximumOverlap=" << kMaximumIpVsL2Top10Overlap
              << "; markers=" << memory_marker << ',' << disk_marker
              << "; build_dir=" << build_dir << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
