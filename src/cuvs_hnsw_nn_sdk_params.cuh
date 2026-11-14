#pragma once
#include "cuvs_hnsw_policy.h"

/* Call with the already clamped CAGRA intermediate degree; B3 recreates NN
 * parameters if graph_degree differs, discarding caller settings. */
template <typename NnParams, typename Metric, typename DType>
static inline NnParams cuvs_hnsw_nn_make_params(uint32_t intermediate, Metric metric,
                                                 uint32_t ef, DType dtype,
                                                 uint32_t *sdk_default_iterations)
{
    NnParams p(intermediate, metric);
    *sdk_default_iterations = static_cast<uint32_t>(p.max_iterations);
    p.max_iterations = cuvs_hnsw_scaled_quality(*sdk_default_iterations, ef, 1, 65535);
    p.termination_threshold = 0.0001f;
    p.dist_comp_dtype = dtype;
    p.return_distances = false;
    return p;
}
