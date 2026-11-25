/*
 * SPDX-FileCopyrightText: Copyright (c) 2024-2026, NVIDIA CORPORATION.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cuvs_cagra_fold_sdk.hpp"
#include "cuvs_cagra_fold_capacity.h"
#include "../../../core/omp_wrapper.hpp"
#include "../ann_utils.cuh"
#include <cuvs/neighbors/cagra.hpp>
#include <raft/core/copy.cuh>
#include <raft/core/device_resources.hpp>
#include <raft/core/mdspan_types.hpp>
#include <raft/core/resource/cuda_stream.hpp>
#include <raft/core/resources.hpp>
#include <raft/matrix/init.cuh>
#include <raft/stats/histogram.cuh>

#include <rmm/device_buffer.hpp>

#include <cstdint>
#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

namespace cuvs::neighbors::cagra {

template <class IdxT>
struct FoldRankSlot {
  IdxT id{};
  std::uint32_t detours = 0;
  std::uint32_t last_source_rank = 0;
  bool occupied = false;
};

template <class IdxT>
void count_rank_detours(const IdxT* neighbors,
                        std::uint32_t count,
                        std::size_t index_size,
                        raft::host_matrix_view<IdxT, std::int64_t> graph,
                        FoldRankSlot<IdxT>* table,
                        std::size_t table_mask,
                        std::pair<IdxT, std::size_t>* ranks)
{
  const auto find_slot = [&](IdxT id) -> FoldRankSlot<IdxT>& {
    auto hash = static_cast<std::uint64_t>(id);
    hash = (hash ^ (hash >> 30)) * 0xbf58476d1ce4e5b9ULL;
    hash = (hash ^ (hash >> 27)) * 0x94d049bb133111ebULL;
    auto slot = static_cast<std::size_t>(hash ^ (hash >> 31)) & table_mask;
    while (table[slot].occupied && table[slot].id != id) {
      slot = (slot + 1) & table_mask;
    }
    return table[slot];
  };

  for (std::uint32_t i = 0; i < count; ++i) {
    const auto id = neighbors[i];
    if (id < index_size) {
      auto& slot = find_slot(id);
      slot.id = id;
      slot.occupied = true;
    }
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto id = neighbors[i];
    if (id >= index_size) {
      ranks[i] = {id, count + 1};
      continue;
    }
    ranks[i] = {id, find_slot(id).detours};
    // One contribution per earlier rank and destination, even with duplicate
    // edges. A repeated source ID at another rank contributes again.
    if (i + 1 == count) { break; }
    for (std::int64_t edge = 0; edge < graph.extent(1); ++edge) {
      auto& target = find_slot(graph(id, edge));
      if (target.occupied && target.last_source_rank != i + 1) {
        ++target.detours;
        target.last_source_rank = i + 1;
      }
    }
  }
}

template <class IdxT>
void rebuild_incoming_beta3a(raft::resources const& handle,
                             const IdxT* graph,
                             std::size_t rows,
                             std::size_t degree,
                             int* counts,
                             std::size_t capacity)
{
  const auto stream = raft::resource::get_cuda_stream(handle);
  RAFT_CUDA_TRY(cudaMemsetAsync(counts, 0, capacity * sizeof(int), stream));
  raft::stats::histogram<IdxT, std::int64_t>(
    raft::stats::HistTypeAuto, counts, rows, graph, rows * degree, 1, stream);
}

template <class T, class IdxT, class Accessor>
void add_node_core_beta3a(
  raft::resources const& handle,
  const cuvs::neighbors::cagra::index<T, IdxT>& idx,
  raft::mdspan<const T, raft::matrix_extent<int64_t>, raft::layout_stride, Accessor>
    additional_dataset_view,
  raft::host_matrix_view<IdxT, std::int64_t> updated_graph,
  std::vector<IdxT>& dirty_rows,
  int* incoming_counts,
  const CuvsCagraFoldControl* control)
{
  using DistanceT                 = float;
  const std::size_t degree        = idx.graph_degree();
  const std::size_t dim           = idx.dim();
  const std::size_t old_size      = idx.dataset().extent(0);
  const std::size_t num_add       = additional_dataset_view.extent(0);
  const std::size_t new_size      = old_size + num_add;
  const std::uint32_t base_degree = degree * 2;

  CuvsCagraFoldTimer timer(control);
  // Step 0: Copy a per-chunk scratch snapshot, including zero bins for new rows.
  using Counts = decltype(raft::make_device_vector<int, std::uint64_t>(handle, 0));
  std::optional<Counts> rebuilt_incoming;
  if (!incoming_counts) {
    rebuilt_incoming.emplace(raft::make_device_vector<int, std::uint64_t>(handle, new_size));
    incoming_counts = rebuilt_incoming->data_handle();
    raft::matrix::fill(handle, rebuilt_incoming->view(), int(0));
    raft::stats::histogram<IdxT, std::int64_t>(
      raft::stats::HistTypeAuto, incoming_counts, old_size, idx.graph().data_handle(),
      old_size * degree, 1, raft::resource::get_cuda_stream(handle));
  }

  auto host_num_incoming_edges = raft::make_host_vector<int, std::uint64_t>(new_size);
  raft::copy(handle, host_num_incoming_edges.view(),
             raft::make_device_vector_view(incoming_counts, std::uint64_t(new_size)));
  if (timer.enabled()) { raft::resource::sync_stream(handle); }
  timer.finish(&CuvsCagraFoldPhases::incoming_us);

  std::size_t data_size_per_vector =
    sizeof(IdxT) * base_degree + sizeof(DistanceT) * base_degree + sizeof(T) * dim;
  cudaPointerAttributes attr;
  RAFT_CUDA_TRY(cudaPointerGetAttributes(&attr, additional_dataset_view.data_handle()));
  if (attr.devicePointer == nullptr) {
    // for batch_load_iterator
    data_size_per_vector += sizeof(T) * dim;
  }

  const std::size_t max_search_batch_size =
    std::min(std::max(1lu, raft::resource::get_workspace_free_bytes(handle) / data_size_per_vector),
             num_add);
  RAFT_EXPECTS(max_search_batch_size > 0, "No enough working memory space is left.");

  cuvs::neighbors::cagra::search_params params;
  params.itopk_size = std::max(base_degree * 2lu, 256lu);

  // Memory space for rank-based neighbor list
  auto mr = raft::resource::get_workspace_resource(handle);

  auto neighbor_indices = raft::make_device_mdarray<IdxT, std::int64_t>(
    handle, mr, raft::make_extents<std::int64_t>(max_search_batch_size, base_degree));

  auto neighbor_distances = raft::make_device_mdarray<DistanceT, std::int64_t>(
    handle, mr, raft::make_extents<std::int64_t>(max_search_batch_size, base_degree));

  auto queries = raft::make_device_mdarray<T, std::int64_t>(
    handle, mr, raft::make_extents<std::int64_t>(max_search_batch_size, dim));

  auto host_neighbor_indices =
    raft::make_host_matrix<IdxT, std::int64_t>(max_search_batch_size, base_degree);

  cuvs::spatial::knn::detail::utils::batch_load_iterator<T> additional_dataset_batch(
    additional_dataset_view.data_handle(),
    num_add,
    additional_dataset_view.stride(0),
    max_search_batch_size,
    raft::resource::get_cuda_stream(handle),
    mr);
  for (const auto& batch : additional_dataset_batch) {
    // Step 1: Obtain K (=base_degree) nearest neighbors of the new vectors by CAGRA search
    // Create queries
    RAFT_CUDA_TRY(cudaMemcpy2DAsync(queries.data_handle(),
                                    sizeof(T) * dim,
                                    batch.data(),
                                    sizeof(T) * additional_dataset_view.stride(0),
                                    sizeof(T) * dim,
                                    batch.size(),
                                    cudaMemcpyDefault,
                                    raft::resource::get_cuda_stream(handle)));

    const auto queries_view = raft::make_device_matrix_view<const T, std::int64_t>(
      queries.data_handle(), batch.size(), dim);

    auto neighbor_indices_view = raft::make_device_matrix_view<IdxT, std::int64_t>(
      neighbor_indices.data_handle(), batch.size(), base_degree);
    auto neighbor_distances_view = raft::make_device_matrix_view<float, std::int64_t>(
      neighbor_distances.data_handle(), batch.size(), base_degree);

    neighbors::cagra::search(
      handle, params, idx, queries_view, neighbor_indices_view, neighbor_distances_view);

    raft::copy(
      handle,
      raft::make_host_vector_view(host_neighbor_indices.data_handle(), batch.size() * base_degree),
      raft::make_device_vector_view(neighbor_indices.data_handle(), batch.size() * base_degree));
    raft::resource::sync_stream(handle);
    timer.finish(&CuvsCagraFoldPhases::search_us);

    // Check search results
    constexpr int max_warnings = 3;
    int num_warnings           = 0;
    for (std::size_t vec_i = 0; vec_i < batch.size(); vec_i++) {
      std::uint32_t invalid_edges = 0;
      for (std::uint32_t i = 0; i < base_degree; i++) {
        if (host_neighbor_indices(vec_i, i) >= old_size) { invalid_edges++; }
      }
      if (invalid_edges > 0) {
        if (num_warnings < max_warnings) {
          RAFT_LOG_WARN(
            "Invalid edges found in search results "
            "(vec_i:%lu, invalid_edges:%lu, degree:%lu, base_degree:%lu)",
            (uint64_t)vec_i,
            (uint64_t)invalid_edges,
            (uint64_t)degree,
            (uint64_t)base_degree);
        }
        num_warnings += 1;
      }
    }
    if (num_warnings > max_warnings) {
      RAFT_LOG_WARN("The number of queries that contain invalid search results: %d", num_warnings);
    }

    // Step 2: rank-based reordering
    // C++ allocation failures must unwind on the calling thread, outside OpenMP.
    std::vector<std::pair<IdxT, std::size_t>> rank_scratch(batch.size() * base_degree);
    const bool fast_rank = control && control->optimized;
    std::size_t table_size = 0;
    if (fast_rank) {
      table_size = 1;
      while (table_size < 2 * std::size_t(base_degree)) { table_size *= 2; }
    }
    std::vector<FoldRankSlot<IdxT>> rank_table(batch.size() * table_size);
#pragma omp parallel
    {
      for (std::size_t vec_i = cuvs::core::omp::get_thread_num(); vec_i < batch.size();
           vec_i += cuvs::core::omp::get_num_threads()) {
        auto* detourable_node_count_list = rank_scratch.data() + vec_i * base_degree;
        if (fast_rank) {
          count_rank_detours(host_neighbor_indices.data_handle() + vec_i * base_degree,
                             base_degree, old_size, updated_graph,
                             rank_table.data() + vec_i * table_size, table_size - 1,
                             detourable_node_count_list);
        } else {
          // Count detourable edges
          for (std::uint32_t i = 0; i < base_degree; i++) {
            std::uint32_t detourable_node_count = 0;
            const auto a_id                     = host_neighbor_indices(vec_i, i);
            if (a_id >= idx.size()) {
              // If the node ID is not valid, the number of detours is increased
              // to a value greater than the maximum, so that the edge to that
              // node is not selected as much as possible.
              detourable_node_count_list[i] = std::make_pair(a_id, base_degree + 1);
              continue;
            }
            for (std::uint32_t j = 0; j < i; j++) {
              const auto b0_id = host_neighbor_indices(vec_i, j);
              if (b0_id >= idx.size()) { continue; }
              for (std::uint32_t k = 0; k < degree; k++) {
                const auto b1_id = updated_graph(b0_id, k);
                if (a_id == b1_id) {
                  detourable_node_count++;
                  break;
                }
              }
            }
            detourable_node_count_list[i] = std::make_pair(a_id, detourable_node_count);
          }
        }

        std::sort(detourable_node_count_list,
                  detourable_node_count_list + base_degree,
                  [&](const std::pair<IdxT, std::size_t> a, const std::pair<IdxT, std::size_t> b) {
                    return a.second < b.second;
                  });

        for (std::size_t i = 0; i < degree; i++) {
          updated_graph(old_size + batch.offset() + vec_i, i) = detourable_node_count_list[i].first;
        }
      }
    }
    timer.finish(&CuvsCagraFoldPhases::rank_us);

    // Step 3: Add reverse edges
    const std::uint32_t rev_edge_search_range = degree / 2;
    const std::uint32_t num_rev_edges         = degree / 2;
    std::vector<IdxT> rev_edges(num_rev_edges), temp(degree);
    for (std::size_t vec_i = 0; vec_i < batch.size(); vec_i++) {
      // Create a reverse edge list
      const auto target_new_node_id = old_size + batch.offset() + vec_i;
      for (std::size_t i = 0; i < num_rev_edges; i++) {
        const auto target_node_id = updated_graph(old_size + batch.offset() + vec_i, i);
        if (target_node_id >= new_size) {
          RAFT_FAIL("Invalid node ID found in updated_graph (%u)\n", target_node_id);
        }
        IdxT replace_id                        = new_size;
        IdxT replace_id_j                      = 0;
        std::size_t replace_num_incoming_edges = 0;
        for (std::int32_t j = degree - 1; j >= static_cast<std::int32_t>(rev_edge_search_range);
             j--) {
          const auto neighbor_id = updated_graph(target_node_id, j);
          if (neighbor_id >= new_size) {
            RAFT_FAIL("Invalid node ID found in updated_graph (%u)\n", neighbor_id);
          }
          const std::size_t num_incoming_edges = host_num_incoming_edges(neighbor_id);
          if (num_incoming_edges > replace_num_incoming_edges) {
            // Check duplication
            bool dup = false;
            for (std::uint32_t k = 0; k < i; k++) {
              if (rev_edges[k] == neighbor_id) {
                dup = true;
                break;
              }
            }
            if (dup) { continue; }

            // Update rev edge candidate
            replace_num_incoming_edges = num_incoming_edges;
            replace_id                 = neighbor_id;
            replace_id_j               = j;
          }
        }
        updated_graph(target_node_id, replace_id_j) = target_new_node_id;
        dirty_rows.push_back(target_node_id);
        rev_edges[i]                                = replace_id;
      }
      host_num_incoming_edges(target_new_node_id) = num_rev_edges;

      // Create a neighbor list of a new node by interleaving the kNN neighbor list and reverse edge
      // list
      std::uint32_t interleave_switch = 0, rank_base_i = 0, rev_edges_return_i = 0, num_add = 0;
      const auto rank_based_list_ptr =
        updated_graph.data_handle() + (old_size + batch.offset() + vec_i) * degree;
      const auto rev_edges_return_list_ptr = rev_edges.data();
      while ((num_add < degree) &&
             ((rank_base_i < degree) || (rev_edges_return_i < num_rev_edges))) {
        const auto node_list_ptr =
          interleave_switch == 0 ? rank_based_list_ptr : rev_edges_return_list_ptr;
        auto& node_list_index          = interleave_switch == 0 ? rank_base_i : rev_edges_return_i;
        const auto max_node_list_index = interleave_switch == 0 ? degree : num_rev_edges;
        for (; node_list_index < max_node_list_index; node_list_index++) {
          const auto candidate = node_list_ptr[node_list_index];
          if (candidate >= new_size) { continue; }
          // Check duplication
          bool dup = false;
          for (std::uint32_t j = 0; j < num_add; j++) {
            if (temp[j] == candidate) {
              dup = true;
              break;
            }
          }
          if (!dup) {
            temp[num_add] = candidate;
            num_add++;
            break;
          }
        }
        interleave_switch = 1 - interleave_switch;
      }
      if (num_add < degree) {
        RAFT_FAIL("Number of edges is not enough (target_new_node_id:%lu, num_add:%lu, degree:%lu)",
                  (uint64_t)target_new_node_id,
                  (uint64_t)num_add,
                  (uint64_t)degree);
      }
      for (std::uint32_t i = 0; i < degree; i++) {
        updated_graph(target_new_node_id, i) = temp[i];
      }
    }
    timer.finish(&CuvsCagraFoldPhases::reverse_us);
  }
}

template <bool Incremental, class IdxT>
__global__ void scatter_graph_beta3a(IdxT* graph, const IdxT* rows, const IdxT* block_data,
                                    size_t degree, int* incoming_counts,
                                    size_t old_size, size_t new_size)
{
  const size_t offset = blockIdx.x;
  const size_t row = rows[offset];
  for (size_t edge = threadIdx.x; edge < degree; edge += blockDim.x) {
    const auto next = block_data[offset * degree + edge];
    auto& slot = graph[row * degree + edge];
    if constexpr (Incremental) {
      if (row < old_size) {
        const auto prev = slot;
        if (prev == next && size_t(prev) < old_size) { continue; }
        if (size_t(prev) < old_size) { atomicAdd(incoming_counts + size_t(prev), -1); }
      }
      if (size_t(next) < new_size) { atomicAdd(incoming_counts + size_t(next), 1); }
    }
    slot = next;
  }
}

template <class T, class IdxT>
void add_graph_nodes_beta3a(
  raft::resources const& handle,
  raft::device_matrix_view<const T, int64_t, raft::layout_stride> input_updated_dataset_view,
  const neighbors::cagra::index<T, IdxT>& index,
  raft::host_matrix_view<IdxT, std::int64_t> updated_graph_view,
  std::size_t max_chunk_size,
  bool host_graph_ready,
  const CuvsCagraFoldControl *control)
{
  if (input_updated_dataset_view.extent(0) < index.size()) {
    RAFT_FAIL("Updated dataset must be not smaller than the previous index state.");
  }

  const std::size_t initial_dataset_size = index.size();
  const std::size_t new_dataset_size     = input_updated_dataset_view.extent(0);
  const std::size_t num_new_nodes        = new_dataset_size - initial_dataset_size;
  const std::size_t degree               = index.graph_degree();
  const std::size_t dim                  = index.dim();
  const std::size_t stride               = input_updated_dataset_view.stride(0);
  const std::size_t max_chunk_size_ = max_chunk_size;

  CuvsCagraFoldTimer setup_timer(control);
  if (!host_graph_ready) {
    raft::copy(handle,
               raft::make_device_vector_view(updated_graph_view.data_handle(), index.graph().size()),
               raft::make_device_vector_view(index.graph().data_handle(), index.graph().size()));
  }

  neighbors::cagra::index<T, IdxT> internal_index(
    handle,
    index.metric(),
    raft::make_device_matrix_view<const T, int64_t>(nullptr, 0, dim),
    raft::make_device_matrix_view<const IdxT, int64_t>(nullptr, 0, degree));

  const auto stream = raft::resource::get_cuda_stream(handle);
  const size_t max_touched = std::min(num_new_nodes, max_chunk_size_) * (degree / 2 + 1);
  auto persistent = raft::make_device_matrix<IdxT, int64_t>(handle, new_dataset_size, degree);
  auto new_values_dev = raft::make_device_matrix<IdxT, int64_t>(handle, max_touched, degree);
  auto ids_dev = raft::make_device_vector<IdxT, int64_t>(handle, max_touched);
  std::vector<IdxT> dirty;
  dirty.reserve(max_touched);
  std::vector<IdxT> new_values(max_touched * degree);
  const size_t original_graph_bytes = initial_dataset_size * degree * sizeof(IdxT);
  RAFT_CUDA_TRY(cudaMemcpyAsync(persistent.data_handle(), index.graph().data_handle(),
                              original_graph_bytes, cudaMemcpyDeviceToDevice, stream));
  using Counts = decltype(raft::make_device_vector<int, std::uint64_t>(handle, 0));
  std::optional<Counts> incoming;
  if (control && control->optimized) {
    incoming.emplace(raft::make_device_vector<int, std::uint64_t>(handle, new_dataset_size));
    rebuild_incoming_beta3a(handle, persistent.data_handle(), initial_dataset_size, degree,
                            incoming->data_handle(), new_dataset_size);
  }
  if (setup_timer.enabled() || (control && control->should_yield)) {
    raft::resource::sync_stream(handle);
  }
  setup_timer.finish(&CuvsCagraFoldPhases::graph_setup_us);
  cuvs_cagra_fold_checkpoint(control);

  std::size_t previous_size = initial_dataset_size;
  for (std::size_t additional_dataset_offset = 0; additional_dataset_offset < num_new_nodes;
       additional_dataset_offset += max_chunk_size_) {
    const auto actual_chunk_size =
      std::min(num_new_nodes - additional_dataset_offset, max_chunk_size_);

    CuvsCagraFoldTimer scatter_timer(control);
    const auto current_size = initial_dataset_size + additional_dataset_offset;
    auto dataset_view = raft::make_device_strided_matrix_view<const T, std::int64_t>(
      input_updated_dataset_view.data_handle(),
      current_size,
      dim,
      stride);
    internal_index.update_dataset(handle, dataset_view);
    if (!dirty.empty()) {
      std::sort(dirty.begin(), dirty.end());
      dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
      for (size_t i = 0; i < dirty.size(); ++i) {
        std::memcpy(new_values.data() + i * degree,
                    updated_graph_view.data_handle() + size_t(dirty[i]) * degree,
                    degree * sizeof(IdxT));
      }
      RAFT_CUDA_TRY(cudaMemcpyAsync(ids_dev.data_handle(), dirty.data(),
                                  dirty.size() * sizeof(IdxT), cudaMemcpyHostToDevice, stream));
      RAFT_CUDA_TRY(cudaMemcpyAsync(new_values_dev.data_handle(), new_values.data(),
                                  dirty.size() * degree * sizeof(IdxT), cudaMemcpyHostToDevice, stream));
      if (incoming) {
        scatter_graph_beta3a<true><<<dirty.size(), 128, 0, stream.value()>>>(
          persistent.data_handle(), ids_dev.data_handle(), new_values_dev.data_handle(), degree,
          incoming->data_handle(), previous_size, current_size);
      } else {
        scatter_graph_beta3a<false><<<dirty.size(), 128, 0, stream.value()>>>(
          persistent.data_handle(), ids_dev.data_handle(), new_values_dev.data_handle(), degree,
          nullptr, 0, 0);
      }
      RAFT_CUDA_TRY(cudaPeekAtLastError());
    }
    const auto current = raft::make_device_matrix_view<IdxT, int64_t>(
      persistent.data_handle(), current_size, degree);
    internal_index.update_graph(handle, raft::make_const_mdspan(current));
    raft::resource::sync_stream(handle);
    scatter_timer.finish(&CuvsCagraFoldPhases::scatter_us);
    cuvs_cagra_fold_checkpoint(control);
    dirty.clear();

    auto updated_graph = raft::make_host_matrix_view<IdxT, std::int64_t>(
      updated_graph_view.data_handle(),
      initial_dataset_size + additional_dataset_offset + actual_chunk_size,
      degree);
    auto additional_dataset_view = raft::make_device_strided_matrix_view<const T, std::int64_t>(
      input_updated_dataset_view.data_handle() +
        (initial_dataset_size + additional_dataset_offset) * stride,
      actual_chunk_size,
      dim,
      stride);

    neighbors::cagra::add_node_core_beta3a<T, IdxT>(
      handle, internal_index, additional_dataset_view, updated_graph, dirty,
      incoming ? incoming->data_handle() : nullptr, control);
    previous_size = current_size;
    for (size_t row = current_size; row < current_size + actual_chunk_size; ++row)
      dirty.push_back(static_cast<IdxT>(row));
    raft::resource::sync_stream(handle);
    cuvs_cagra_fold_checkpoint(control);
  }
}

}  // namespace cuvs::neighbors::cagra

void cuvs_cagra_fold_sdk(
    raft::resources const& res,
    const cuvs::neighbors::cagra::index<float, uint32_t>& source,
    raft::device_matrix_view<const float, int64_t> added,
    raft::device_matrix_view<float, int64_t, raft::layout_stride> dataset,
    cuvs::neighbors::cagra::index<float, uint32_t>& result,
    const CuvsCagraFoldControl *control)
{
    const auto old = source.dataset();
    const auto n_old = old.extent(0);
    const auto dim = old.extent(1);
    const auto stride = old.stride(0);
    const auto new_rows = added.extent(0);
    const auto total_rows = n_old + new_rows;
    const auto degree = source.graph_degree();
    const auto stream = raft::resource::get_cuda_stream(res);

    cuvs_cagra_fold_checkpoint(control);
    const bool optimized = control && control->optimized;
    auto* host_graph_data = optimized ? control->host_graph : nullptr;
    const bool host_graph_ready = host_graph_data &&
        control->cached_device_graph == source.graph().data_handle();
    if (control && control->phases)
        control->phases->host_graph_cache_hit = host_graph_ready;
    using HostGraph = decltype(raft::make_host_matrix<uint32_t, int64_t>(0, 0));
    std::optional<HostGraph> owned_host_graph;
    if (!host_graph_data) {
        owned_host_graph.emplace(raft::make_host_matrix<uint32_t, int64_t>(total_rows, degree));
        host_graph_data = owned_host_graph->data_handle();
    }
    auto host_graph = raft::make_host_matrix_view<uint32_t, int64_t>(
        host_graph_data, total_rows, degree);

    CuvsCagraFoldTimer timer(control);
    using HostDataset = decltype(raft::make_host_matrix<float, int64_t>(0, 0));
    std::optional<HostDataset> owned_host_dataset;
    const bool direct = optimized && stride == dim && dataset.stride(0) == dim &&
                        dataset.extent(1) == dim && added.stride(0) == dim && added.extent(1) == dim;
    if (direct) {
        RAFT_EXPECTS(dataset.data_handle() != old.data_handle() &&
                     dataset.data_handle() != added.data_handle(),
                     "Direct CAGRA fold requires an independent dataset");
        const size_t row_bytes = static_cast<size_t>(dim) * sizeof(float);
        const bool yield = control->should_yield != nullptr;
        const int64_t band = yield ? static_cast<int64_t>(
            std::max(size_t{1}, std::min(size_t{16384}, (size_t{64} << 20) / row_bytes))) : total_rows;
        auto copy_rows = [&](float *dest, const float *src, int64_t rows) {
            for (int64_t row = 0; row < rows;) {
                const int64_t count = std::min(band, rows - row);
                RAFT_CUDA_TRY(cudaMemcpyAsync(dest + row * stride, src + row * stride,
                                             count * row_bytes, cudaMemcpyDeviceToDevice, stream));
                row += count;
                if (yield) {
                    raft::resource::sync_stream(res);
                    cuvs_cagra_fold_checkpoint(control);
                }
            }
        };
        copy_rows(dataset.data_handle(), old.data_handle(), n_old);
        copy_rows(dataset.data_handle() + n_old * stride, added.data_handle(), new_rows);
    } else {
        owned_host_dataset.emplace(raft::make_host_matrix<float, int64_t>(total_rows, stride));
        auto& host_dataset = *owned_host_dataset;
        if (control && control->should_yield) {
            /* Cap both row count and bytes. A single oversized row still needs
             * one complete transfer. Each check follows a finished host band or
             * a stream fence, so unwind can retire all temporary buffers. */
            const size_t row_bytes = static_cast<size_t>(stride) * sizeof(float);
            const size_t rows_by_bytes = (size_t{64} << 20) / row_bytes;
            const int64_t band = static_cast<int64_t>(std::max(size_t{1}, std::min(size_t{16384}, rows_by_bytes)));
            cuvs_cagra_fold_checkpoint(control);
            for (int64_t row = 0; row < total_rows;) {
                const int64_t count = std::min<int64_t>(band, total_rows - row);
                std::memset(host_dataset.data_handle() + row * stride, 0, count * row_bytes);
                row += count;
                cuvs_cagra_fold_checkpoint(control);
            }
            auto copy_to_host = [&](float *dest, const float *src, int64_t src_stride, int64_t rows) {
                for (int64_t row = 0; row < rows;) {
                    const int64_t count = std::min<int64_t>(band, rows - row);
                    RAFT_CUDA_TRY(cudaMemcpy2DAsync(dest + row * stride, row_bytes,
                                                  src + row * src_stride, src_stride * sizeof(float),
                                                  dim * sizeof(float), count, cudaMemcpyDeviceToHost, stream));
                    raft::resource::sync_stream(res);
                    row += count;
                    cuvs_cagra_fold_checkpoint(control);
                }
            };
            copy_to_host(host_dataset.data_handle(), old.data_handle(), old.stride(0), n_old);
            copy_to_host(host_dataset.data_handle() + n_old * stride,
                         added.data_handle(), added.stride(0), new_rows);
            for (int64_t row = 0; row < total_rows;) {
                const int64_t count = std::min<int64_t>(band, total_rows - row);
                RAFT_CUDA_TRY(cudaMemcpyAsync(dataset.data_handle() + row * stride,
                                             host_dataset.data_handle() + row * stride, count * row_bytes,
                                             cudaMemcpyHostToDevice, stream));
                raft::resource::sync_stream(res);
                row += count;
                cuvs_cagra_fold_checkpoint(control);
            }
        } else {
            std::memset(host_dataset.data_handle(), 0, host_dataset.size() * sizeof(float));
            RAFT_CUDA_TRY(cudaMemcpy2DAsync(host_dataset.data_handle(), stride * sizeof(float),
                                          old.data_handle(), old.stride(0) * sizeof(float),
                                          dim * sizeof(float), n_old, cudaMemcpyDeviceToHost, stream));
            RAFT_CUDA_TRY(cudaMemcpy2DAsync(host_dataset.data_handle() + n_old * stride, stride * sizeof(float),
                                          added.data_handle(), added.stride(0) * sizeof(float),
                                          dim * sizeof(float), new_rows, cudaMemcpyDeviceToHost, stream));
            raft::resource::sync_stream(res);
            raft::copy(res, raft::make_device_vector_view(dataset.data_handle(), total_rows * stride),
                       raft::make_host_vector_view(host_dataset.data_handle(), total_rows * stride));
        }
    }
    if (timer.enabled()) { raft::resource::sync_stream(res); }
    timer.finish(&CuvsCagraFoldPhases::dataset_us);

    cuvs::neighbors::cagra::add_graph_nodes_beta3a<float, uint32_t>(
        res, raft::make_const_mdspan(dataset), source, host_graph,
        CUVS_CAGRA_FOLD_BATCH_ROWS, host_graph_ready, control);
    cuvs_cagra_fold_checkpoint(control);
    CuvsCagraFoldTimer final_timer(control);
    result.update_dataset(res, raft::make_const_mdspan(dataset));
    // The host overload makes the final graph an allocation owned by result.
    result.update_graph(res, raft::make_const_mdspan(host_graph));
    raft::resource::sync_stream(res);
    final_timer.finish(&CuvsCagraFoldPhases::final_graph_us);
}
