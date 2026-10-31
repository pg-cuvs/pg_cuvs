/*
 * SPDX-FileCopyrightText: Copyright (c) 2026, JAESOL SHIN
 * SPDX-License-Identifier: PostgreSQL
 */

#include <cuvs/distance/distance.hpp>
#include <cuvs/neighbors/cagra.hpp>
#include <raft/core/mdspan_types.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

int main()
{
  using cuvs::distance::DistanceType;
  using cuvs::neighbors::cagra::hnsw_heuristic_type;
  using cuvs::neighbors::cagra::index_params;

  constexpr std::array cases = {
    std::pair{DistanceType::InnerProduct, std::string_view{"InnerProduct"}},
    std::pair{DistanceType::CosineExpanded, std::string_view{"CosineExpanded"}},
    std::pair{DistanceType::L2Expanded, std::string_view{"L2Expanded"}},
  };
  const auto dataset = raft::make_extents<std::int64_t>(64, 16);
  for (const auto& [requested_metric, name] : cases) {
    const auto params = index_params::from_hnsw_params(
      dataset, 16, 80, hnsw_heuristic_type::SAME_GRAPH_FOOTPRINT, requested_metric);
    if (params.metric != requested_metric) {
      std::cerr << "FAIL: from_hnsw_params(" << name
                << ") returned a different top-level index metric\n";
      return 1;
    }
    const auto* nn_params = std::get_if<cuvs::neighbors::graph_build_params::nn_descent_params>(
      &params.graph_build_params);
    if (nn_params == nullptr || nn_params->metric != requested_metric) {
      std::cerr << "FAIL: from_hnsw_params(" << name
                << ") did not preserve the metric in graph-training parameters\n";
      return 1;
    }
  }
  std::cout << "PASS: from_hnsw_params preserves all three metrics in index and graph parameters\n";
  return 0;
}
