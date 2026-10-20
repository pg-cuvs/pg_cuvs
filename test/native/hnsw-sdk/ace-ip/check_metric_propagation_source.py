#!/usr/bin/env python3
"""CPU source regression for the actual HNSW-to-CAGRA ACE metric path."""

from pathlib import Path
import argparse
import sys


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source_root", type=Path)
    root = parser.parse_args().source_root

    cagra = (root / "cpp/src/neighbors/cagra.cpp").read_text()
    factory_start = cagra.index("cagra::index_params index_params::from_hnsw_params")
    factory_end = cagra.index("\n}\n", factory_start)
    factory = cagra[factory_start:factory_end]
    require(
        "params.metric = metric;" in factory,
        "from_hnsw_params still leaves the top-level metric at its default L2",
    )
    require(
        "graph_params_heuristic(dataset, params.intermediate_graph_degree, ef_construction, metric)"
        in factory,
        "the requested metric no longer reaches the graph training parameters",
    )
    require(
        "pgcuvs-hnsw-memory-guard-v2@60b19a0f62ba0bd716de349382828e43be6e2daa" in cagra,
        "cagra.cpp does not advertise the v2 metric-correct guard capability",
    )

    ace_build = (root / "cpp/src/neighbors/detail/cagra/cagra_build.cuh").read_text()
    factory_call_start = ace_build.index("cuvs::neighbors::cagra::index_params::from_hnsw_params(")
    factory_call_end = ace_build.index("params.metric);", factory_call_start)
    require(
        factory_call_end > factory_call_start,
        "ACE subpartition factory call does not pass the HNSW metric",
    )
    require(
        "const bool bounded_partition_builder = host_budget.has_value() || gpu_budget.has_value();"
        in ace_build,
        "positive effective ACE budgets do not select the bounded partition-builder path",
    )
    require(
        "nn_descent_workspace_bytes(" in ace_build
        and "graph_build_params::nn_descent_params(" in ace_build,
        "bounded ACE subbuild does not use source-derived NN-descent workspace sizing",
    )
    require(
        "if (bounded_partition_builder &&" in ace_build
        and "sub_index_params.graph_build_params =" in ace_build
        and "sub_index_params.intermediate_graph_degree, params.metric);" in ace_build,
        "positive-budget ACE subbuild does not force the ledgered builder with the requested metric",
    )
    partition_guard = ace_build.index("ACE actual partition host NN-descent builder workspace")
    partition_dataset_allocation = ace_build.index(
        "auto sub_dataset = raft::make_host_matrix<T, int64_t>(sub_dataset_size, dataset_dim);"
    )
    require(
        partition_guard < partition_dataset_allocation,
        "actual partition builder workspace is not checked before subdataset allocation",
    )

    hnsw = (root / "cpp/src/neighbors/detail/hnsw.hpp").read_text()
    guard_header = (root / "cpp/src/neighbors/detail/cagra/ace_memory_guard.hpp").read_text()
    require(
        "cagra_params.metric                    = params.metric;" in hnsw,
        "HNSW parent build does not initialize the top-level CAGRA metric",
    )
    require(
        "index_impl<T>>(dim, cagra_index.metric()," in hnsw,
        "HNSW metadata construction does not retain the built CAGRA metric",
    )
    require(
        "memory_guard::hnsw_upper_level_uses_nn_descent(rows, degree, has_memory_budget)" in hnsw
        and "num_pts, neighbor_size, has_memory_budget" in hnsw,
        "HNSW serializer preflight and actual upper-level dispatch do not share one algorithm decision",
    )
    require(
        "return checked_mul(rows, degree) < nn_descent_neighbor_element_threshold || has_memory_budget;"
        in guard_header,
        "shared upper-level policy does not preserve the no-budget threshold or force bounded NN-descent",
    )
    require(
        "fixed_host_arrays_bytes" in hnsw
        and "memory_guard::hnsw_serializer_host_peak_bytes(" in hnsw
        and "memory_guard::hnsw_serializer_builder_host_budget(" in hnsw
        and "index_.metric(),\n                          upper_workspace_host_budget," in hnsw,
        "upper-level NN-descent workspace is not charged alongside live serializer arrays and batch buffers",
    )
    preflight_start = hnsw.index("// Generate actual upper-level sizes from the random HNSW level histogram")
    host_peak_check = hnsw.index(
        'memory_guard::require_within("HNSW serializer actual upper-level host arrays and workspace"',
        preflight_start,
    )
    batch_check = hnsw.index(
        'memory_guard::require_within("HNSW serializer disk-read batch buffers and upper-level workspace"',
        preflight_start,
    )
    graph_buffer_allocation = hnsw.index("auto graph_buffer = raft::make_host_matrix<IdxT, int64_t>(batch_size")
    query_buffer_allocation = hnsw.index(
        "auto host_query_set = raft::make_host_matrix<T, int64_t>(upper_query_rows, dim)"
    )
    neighbor_allocation = hnsw.index(
        "host_neighbors.emplace_back(raft::make_host_matrix<IdxT, int64_t>(num_pts, neighbor_size))"
    )
    require(
        host_peak_check < graph_buffer_allocation
        and batch_check < graph_buffer_allocation
        and query_buffer_allocation > graph_buffer_allocation
        and neighbor_allocation > host_peak_check,
        "serializer host and GPU preflights must precede batch, upper-query, and neighbor allocations",
    )
    require(
        "fixed_host_arrays_bytes, max_upper_host_workspace, batch_bytes" in hnsw
        and "*host_budget, fixed_host_arrays_bytes, batch_bytes" in hnsw,
        "serializer peak or residual builder budget omits a simultaneously live host allocation",
    )
    parent_call_start = hnsw.rindex("all_neighbors_graph(res,")
    parent_call_end = hnsw.index(";", parent_call_start)
    parent_call = hnsw[parent_call_start:parent_call_end]
    require(
        "cagra_index.metric()" in parent_call
        and "host_budget" not in parent_call
        and "gpu_budget" not in parent_call,
        "non-serializer HNSW parent-build call unexpectedly acquired serializer-only memory limits",
    )
    print("PASS: actual HNSW ACE source path propagates the requested metric through factory, build, and metadata")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
