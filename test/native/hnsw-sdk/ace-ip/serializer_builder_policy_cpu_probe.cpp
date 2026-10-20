/*
 * SPDX-FileCopyrightText: Copyright (c) 2026, JAESOL SHIN
 * SPDX-License-Identifier: PostgreSQL
 */

#include "neighbors/detail/cagra/ace_memory_guard.hpp"

#include <iostream>
#include <optional>
#include <stdexcept>

int main()
{
  using cuvs::neighbors::cagra::detail::memory_guard::hnsw_upper_level_uses_nn_descent;
  using cuvs::neighbors::cagra::detail::memory_guard::hnsw_serializer_builder_host_budget;
  using cuvs::neighbors::cagra::detail::memory_guard::hnsw_serializer_host_peak_bytes;
  using cuvs::neighbors::cagra::detail::memory_guard::require_within;
  const bool below_legacy_threshold = hnsw_upper_level_uses_nn_descent(999'999, 10, false);
  const bool at_legacy_threshold = hnsw_upper_level_uses_nn_descent(1'000'000, 10, false);
  const bool bounded_host = hnsw_upper_level_uses_nn_descent(1'000'000, 10, true);
  const bool bounded_gpu = hnsw_upper_level_uses_nn_descent(1'000'000, 10, true);
  if (!below_legacy_threshold || at_legacy_threshold || !bounded_host || !bounded_gpu) {
    throw std::runtime_error("serializer NN-descent policy does not preserve the shared threshold/budget rule");
  }

  constexpr std::size_t fixed_arrays = 1'000;
  constexpr std::size_t upper_builder_workspace = 2'000;
  constexpr std::size_t batch = 300;
  const std::size_t peak = hnsw_serializer_host_peak_bytes(
    fixed_arrays, upper_builder_workspace, batch);
  if (peak != 3'300) { throw std::runtime_error("serializer host peak omitted a live allocation phase"); }
  require_within("serializer viable fixture", peak, 3'300);
  const std::size_t builder_budget = hnsw_serializer_builder_host_budget(4'000, fixed_arrays, batch);
  if (builder_budget != 2'700 || upper_builder_workspace > builder_budget) {
    throw std::runtime_error("serializer did not pass only the residual host budget to the builder");
  }
  bool tight_budget_rejected = false;
  try {
    require_within("serializer tight fixture", peak, 3'299);
  } catch (const std::length_error&) {
    tight_budget_rejected = true;
  }
  if (!tight_budget_rejected) { throw std::runtime_error("serializer admitted a host peak one byte over budget"); }

  std::cout << "PASS: shared NN-descent policy and serializer live-array/batch/workspace host ledger; "
               "peak=3300 viable=4000 residualBuilder=2700 tight=3299 rejected\n";
  return 0;
}
