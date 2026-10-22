#pragma once

/* The pinned disk SDK can reduce/increase the requested partition count.
 * Consume its own positive-budget planning functions, rather than recording
 * our candidate as an observed count or maintaining a second memory model.
 * No GPU allocation/build is performed here. Successful fixed-point checking
 * makes the explicit count stable under the same SDK build-time planner.
 * Builds without the matching SDK source refuse ACE before generating it. */
#if defined(_OPENMP) && __has_include(<neighbors/detail/cagra/cagra_build.cuh>)
#include <neighbors/detail/cagra/cagra_build.cuh>
#define CUVS_HNSW_HAS_SDK_PLAN 1

static inline size_t cuvs_hnsw_sdk_ace_plan(
    const cuvs::neighbors::cagra::index_params& cp, uint64_t n, uint32_t dim,
    size_t candidate, const char *workdir, double host_gib, double gpu_gib)
{
    using namespace cuvs::neighbors::cagra::detail;
    size_t partitions = candidate;
    if (partitions > n / 1000) partitions = std::max<size_t>(2, n / 1000);
    std::string dir(workdir);
    ace_memory_requirements memory;
    bool disk = ace_check_use_disk_mode<float, uint32_t>(true, dir, n, dim,
        partitions, cp.intermediate_graph_degree, cp.graph_degree,
        host_gib, gpu_gib, memory);
    if (!disk || dir != workdir) throw std::runtime_error("ACE planner changed the private disk route");
    ace_validate_disk_mode_partitions<float, uint32_t>(partitions, n, dim,
        cp.intermediate_graph_degree, cp.graph_degree, cp.guarantee_connectivity, memory);
    if (!partitions || partitions > UINT32_MAX)
        throw std::runtime_error("ACE effective partition count is not representable");
    return partitions;
}
#else
#define CUVS_HNSW_HAS_SDK_PLAN 0
#endif
