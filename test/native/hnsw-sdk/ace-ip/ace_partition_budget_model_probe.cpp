/*
 * SPDX-FileCopyrightText: Copyright (c) 2026, JAESOL SHIN
 * SPDX-License-Identifier: PostgreSQL
 */

#include "neighbors/detail/cagra/ace_memory_guard.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

using cuvs::neighbors::cagra::detail::memory_guard::checked_add;
using cuvs::neighbors::cagra::detail::memory_guard::checked_mul;
using cuvs::neighbors::cagra::detail::memory_guard::checked_sum;
using cuvs::neighbors::cagra::detail::memory_guard::matrix_bytes;

constexpr std::size_t kRows = 4'000'000;
constexpr std::size_t kPartitionRows = 900'000;
constexpr std::size_t kDim = 16;
constexpr std::size_t kIntermediateDegree = 128;
constexpr std::size_t kGraphDegree = 64;
constexpr std::size_t kPartitions = 512;
constexpr std::size_t kSubbuildDegree = 96;
constexpr std::size_t kSdkHostBudgetBytes = 1'879'048'192;

std::size_t source_optimize_host_workspace(std::size_t rows,
                                           std::size_t graph_degree,
                                           std::size_t intermediate_degree,
                                           std::size_t host_threads)
{
  const std::size_t row_index_bytes = checked_mul(rows, sizeof(std::uint32_t));
  const std::size_t final_graph_bytes = matrix_bytes(rows, graph_degree, sizeof(std::uint32_t));
  const std::size_t prune_host =
    checked_add(checked_mul(rows, intermediate_degree), size_t{2 * sizeof(std::uint64_t)});
  const std::size_t reverse_host = checked_sum(
    {final_graph_bytes, checked_mul(rows, sizeof(std::uint32_t)), row_index_bytes});
  const std::size_t combine_host = checked_sum(
    {final_graph_bytes,
     checked_mul(rows, sizeof(std::uint32_t)),
     checked_mul(checked_mul(graph_degree, sizeof(std::uint32_t)), host_threads)});
  return checked_add(row_index_bytes, std::max({prune_host, reverse_host, combine_host}));
}

}  // namespace

int main()
{
  const std::size_t label_bytes = matrix_bytes(kRows, 2, sizeof(std::uint32_t));
  const std::size_t histogram_bytes = matrix_bytes(kPartitions, 2, sizeof(std::uint32_t));
  const std::size_t mapping_bytes = matrix_bytes(kRows, 3, sizeof(std::uint32_t));
  const std::size_t offset_bytes = matrix_bytes(checked_add(kPartitions, 1), 2, sizeof(std::uint32_t));
  const std::size_t old_mapping_phase =
    checked_sum({label_bytes, histogram_bytes, mapping_bytes, offset_bytes});
  const std::size_t dataset_bytes = matrix_bytes(kPartitionRows, kDim, sizeof(float));
  const std::size_t old_build_graph_bytes =
    matrix_bytes(kPartitionRows, kIntermediateDegree + kGraphDegree, sizeof(std::uint32_t));
  const std::size_t old_optimize_bytes =
    source_optimize_host_workspace(kPartitionRows, kGraphDegree, kIntermediateDegree, 64);
  const std::size_t old_result_graph_bytes =
    matrix_bytes(kPartitionRows, kGraphDegree, sizeof(std::uint32_t));
  const std::size_t old_guard_peak = checked_sum(
    {old_mapping_phase,
     dataset_bytes,
     std::max(checked_add(old_build_graph_bytes, old_optimize_bytes), old_result_graph_bytes)});

  const auto [nn_host_workspace, nn_gpu_workspace] =
    cuvs::neighbors::cagra::detail::memory_guard::nn_descent_workspace_bytes(
      kPartitionRows, kDim, kSubbuildDegree);
  (void)nn_gpu_workspace;
  const std::size_t caller_knn_graph_bytes =
    matrix_bytes(kPartitionRows, kSubbuildDegree, sizeof(std::uint32_t));
  const std::size_t live_disk_mapping_bytes = checked_sum(
    {label_bytes,
     histogram_bytes,
     matrix_bytes(kRows, 2, sizeof(std::uint32_t)),
     offset_bytes});
  const std::size_t bounded_builder_peak =
    checked_sum({live_disk_mapping_bytes, dataset_bytes, caller_knn_graph_bytes, nn_host_workspace});

  // Lower bound independently quoted by the R1 review: raw input + caller graph + seven
  // pinned matrices and two int2 arrays + extended graph and distances + live mappings.
  const std::size_t review_lower_bound = checked_add(
    checked_mul(kPartitionRows, size_t{64 + 384 + 912 + 512 + 512}), live_disk_mapping_bytes);
  if (old_guard_peak > kSdkHostBudgetBytes || bounded_builder_peak <= kSdkHostBudgetBytes ||
      review_lower_bound <= kSdkHostBudgetBytes) {
    throw std::runtime_error("large ACE partition counterexample did not separate old and bounded ledgers");
  }
  std::cout << "PASS: N=" << kRows << " D=" << kDim << " I=" << kIntermediateDegree
            << " G=" << kGraphDegree << " P=" << kPartitions << " actualR=" << kPartitionRows
            << " SDKHostBudget=" << kSdkHostBudgetBytes
            << " oldGuardPeak=" << old_guard_peak
            << " reviewLowerBound=" << review_lower_bound
            << " sourceNNBuilderPeak=" << bounded_builder_peak << '\n';
  return 0;
}
