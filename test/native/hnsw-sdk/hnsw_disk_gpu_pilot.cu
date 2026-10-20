/*
 * SPDX-FileCopyrightText: Copyright (c) 2026, JAESOL SHIN
 * SPDX-License-Identifier: PostgreSQL
 */

#include <cuvs/neighbors/hnsw.hpp>
#include <raft/core/host_mdarray.hpp>
#include <raft/core/host_mdspan.hpp>

#include <cuda_runtime_api.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" const char* pgcuvs_hnsw_disk_copy_patch_id();

namespace {

constexpr char kExpectedPatchMarker[] =
  "pgcuvs-hnsw-disk-copy-v1@60b19a0f62ba0bd716de349382828e43be6e2daa";
constexpr std::size_t kRows = 4096;
constexpr std::size_t kDim = 16;
constexpr std::size_t kQueries = 128;
constexpr std::size_t kK = 10;
constexpr double kMinimumRecallAt10 = 0.90;

template <typename Value>
Value read_value(std::ifstream& input)
{
  Value value{};
  input.read(reinterpret_cast<char*>(&value), sizeof(value));
  if (!input) { throw std::runtime_error("truncated HNSW header or base layer"); }
  return value;
}

std::size_t verify_serialized_hierarchy_and_labels(const std::filesystem::path& index_path)
{
  static_assert(sizeof(std::size_t) == sizeof(std::uint64_t),
                "the HNSW file header check expects an LP64 build");
  std::ifstream input(index_path, std::ios::binary);
  if (!input) { throw std::runtime_error("cannot open generated hnsw_index.bin"); }

  const auto offset_level0 = read_value<std::size_t>(input);
  const auto max_elements = read_value<std::size_t>(input);
  const auto current_elements = read_value<std::size_t>(input);
  const auto element_stride = read_value<std::size_t>(input);
  const auto label_offset = read_value<std::size_t>(input);
  const auto offset_data = read_value<std::size_t>(input);
  const auto max_level = read_value<std::int32_t>(input);
  const auto entrypoint = read_value<std::int32_t>(input);
  const auto max_m = read_value<std::size_t>(input);
  const auto max_m0 = read_value<std::size_t>(input);
  const auto m = read_value<std::size_t>(input);
  const auto mult = read_value<double>(input);
  const auto ef_construction = read_value<std::size_t>(input);
  const auto header_size = static_cast<std::uint64_t>(input.tellg());

  if (current_elements != kRows || max_elements < kRows) {
    throw std::runtime_error("serialized HNSW element count does not match the input rows");
  }
  if (max_level <= 0) { throw std::runtime_error("GPU hierarchy maxlevel must be greater than zero"); }
  if (element_stride == 0 || label_offset + sizeof(std::size_t) > element_stride) {
    throw std::runtime_error("invalid HNSW element or label offset in file header");
  }
  if (offset_data >= element_stride || max_m == 0 || max_m0 == 0 || m == 0 ||
      !std::isfinite(mult) || ef_construction == 0) {
    throw std::runtime_error("invalid HNSW hierarchy header fields");
  }
  (void)entrypoint;

  const auto file_size = std::filesystem::file_size(index_path);
  const std::uint64_t base_layer_end =
    header_size + offset_level0 + current_elements * element_stride;
  if (base_layer_end > file_size) { throw std::runtime_error("truncated HNSW base layer"); }

  std::vector<bool> seen(kRows, false);
  for (std::size_t row = 0; row < current_elements; ++row) {
    const std::uint64_t label_position =
      header_size + offset_level0 + row * element_stride + label_offset;
    input.seekg(static_cast<std::streamoff>(label_position));
    const auto original_row = read_value<std::size_t>(input);
    if (original_row >= kRows || seen[original_row]) {
      throw std::runtime_error("serialized external labels are not a permutation of input rows");
    }
    seen[original_row] = true;
  }
  if (std::find(seen.begin(), seen.end(), false) != seen.end()) {
    throw std::runtime_error("serialized external labels omit an input row");
  }
  return static_cast<std::size_t>(max_level);
}

void require_gpu()
{
  int device_count = 0;
  const auto count_status = cudaGetDeviceCount(&device_count);
  if (count_status != cudaSuccess || device_count < 1) {
    throw std::runtime_error("pilot requires a visible CUDA GPU");
  }
  const auto sync_status = cudaDeviceSynchronize();
  if (sync_status != cudaSuccess) {
    throw std::runtime_error(std::string("CUDA synchronization failed: ") +
                             cudaGetErrorString(sync_status));
  }
}

}  // namespace

int main(int argc, char** argv)
{
  try {
    if (argc != 2) {
      std::cerr << "usage: cuvs_hnsw_disk_gpu_pilot EMPTY_BUILD_DIR\n";
      return 2;
    }
    const char* patch_marker = pgcuvs_hnsw_disk_copy_patch_id();
    if (patch_marker == nullptr || std::string(patch_marker) != kExpectedPatchMarker) {
      throw std::runtime_error("loaded libcuvs does not expose the expected HNSW disk-copy marker");
    }
    std::cout << "PASS: loaded libcuvs marker=" << patch_marker << '\n';
    require_gpu();

    const std::filesystem::path build_dir = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(build_dir);
    if (!std::filesystem::is_empty(build_dir)) {
      throw std::runtime_error("build directory must be empty to keep ACE artifacts attributable");
    }

    raft::resources resources;
    auto dataset = raft::make_host_matrix<float, std::int64_t>(resources, kRows, kDim);
    auto queries = raft::make_host_matrix<float, std::int64_t>(resources, kQueries, kDim);

    std::mt19937 generator(0x26A04U);
    std::uniform_real_distribution<float> vector_value(-2.0F, 2.0F);
    std::uniform_real_distribution<float> query_noise(-0.0005F, 0.0005F);
    for (std::size_t row = 0; row < kRows; ++row) {
      for (std::size_t col = 0; col < kDim; ++col) {
        dataset(row, col) = vector_value(generator);
      }
    }
    std::vector<std::size_t> query_source_rows(kQueries);
    for (std::size_t query = 0; query < kQueries; ++query) {
      const auto source_row = (query * 31 + 7) % kRows;
      query_source_rows[query] = source_row;
      for (std::size_t col = 0; col < kDim; ++col) {
        queries(query, col) = dataset(source_row, col) + query_noise(generator);
      }
    }

    cuvs::neighbors::hnsw::index_params params;
    params.metric = cuvs::distance::DistanceType::L2Expanded;
    params.hierarchy = cuvs::neighbors::hnsw::HnswHierarchy::GPU;
    params.M = 32;
    params.ef_construction = 120;

    auto ace = cuvs::neighbors::hnsw::graph_build_params::ace_params{};
    ace.npartitions = 2;
    ace.use_disk = true;
    ace.build_dir = build_dir.string();
    params.graph_build_params = ace;

    auto index = cuvs::neighbors::hnsw::build(
      resources, params, raft::make_const_mdspan(dataset.view()));
    if (!index) { throw std::runtime_error("cuVS returned a null HNSW index"); }
    if (index->hierarchy() != cuvs::neighbors::hnsw::HnswHierarchy::GPU) {
      throw std::runtime_error("build did not retain GPU hierarchy mode");
    }

    const auto index_path = build_dir / "hnsw_index.bin";
    if (index->file_path() != index_path.string()) {
      throw std::runtime_error("returned index does not point at the ACE disk artifact");
    }
    for (const auto& required_artifact : {"reordered_dataset.npy", "cagra_graph.npy",
                                          "dataset_mapping.npy", "hnsw_index.bin"}) {
      if (!std::filesystem::is_regular_file(build_dir / required_artifact)) {
        throw std::runtime_error(std::string("missing disk ACE artifact: ") + required_artifact);
      }
    }

    const auto max_level = verify_serialized_hierarchy_and_labels(index_path);

    auto neighbors = raft::make_host_matrix<std::uint64_t, std::int64_t>(resources, kQueries, kK);
    auto distances = raft::make_host_matrix<float, std::int64_t>(resources, kQueries, kK);
    cuvs::neighbors::hnsw::search_params search_params;
    search_params.ef = 200;
    search_params.num_threads = 1;
    cuvs::neighbors::hnsw::search(
      resources, search_params, *index, queries.view(), neighbors.view(), distances.view());

    std::size_t recall_hits = 0;
    for (std::size_t query = 0; query < kQueries; ++query) {
      std::vector<std::pair<float, std::uint64_t>> exact(kRows);
      for (std::size_t row = 0; row < kRows; ++row) {
        float distance = 0.0F;
        for (std::size_t col = 0; col < kDim; ++col) {
          const float delta = dataset(row, col) - queries(query, col);
          distance += delta * delta;
        }
        exact[row] = {distance, row};
      }
      std::partial_sort(exact.begin(), exact.begin() + kK, exact.end());

      for (std::size_t rank = 0; rank < kK; ++rank) {
        const auto found = neighbors(query, rank);
        if (found >= kRows) { throw std::runtime_error("search returned a non-input label"); }
        const auto exact_match = std::find_if(exact.begin(), exact.begin() + kK, [&](const auto& p) {
          return p.second == found;
        });
        if (exact_match != exact.begin() + kK) { ++recall_hits; }
      }
      if (neighbors(query, 0) != query_source_rows[query]) {
        throw std::runtime_error("search did not return the source row for a near-duplicate query");
      }
    }

    const double recall_at_10 = static_cast<double>(recall_hits) / (kQueries * kK);
    if (recall_at_10 < kMinimumRecallAt10) {
      throw std::runtime_error("search recall@10 is below the pilot threshold");
    }

    std::cout << "PASS: disk ACE HNSW GPU hierarchy; maxlevel=" << max_level
              << "; original labels=permutation[0," << kRows << ")"
              << "; search=cuVS HNSW wrapper (CPU)"
              << "; recall@10=" << recall_at_10
              << "; minimum=" << kMinimumRecallAt10
              << "; build_dir=" << build_dir << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
