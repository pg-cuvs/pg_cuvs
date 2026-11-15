#include "cuvs_hnsw_nn_sdk_params.cuh"
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <variant>

enum class DType { fp16, fp32 };
struct NN {
    size_t graph_degree, intermediate_graph_degree, max_iterations;
    int metric;
    float termination_threshold;
    bool return_distances;
    DType dist_comp_dtype;
    NN(size_t width, int m) : graph_degree(width), intermediate_graph_degree(width * 3 / 2),
        max_iterations(20), metric(m), termination_threshold(.0001f),
        return_distances(true), dist_comp_dtype(DType::fp16) {}
};
struct IVF {};
using Choice = std::variant<std::monostate, NN, IVF>;
/* Pinned SDK behavior: mismatched graph_degree reconstructs defaults, losing settings. */
static NN sdk_consume(Choice selected, size_t intermediate, int metric)
{
    NN p = std::get<NN>(selected);
    if (p.metric != metric) p.metric = metric;
    if (p.graph_degree != intermediate) p = NN(intermediate, metric);
    p.return_distances = false;
    return p;
}
int main()
{
    uint32_t base = 0;
    auto p = cuvs_hnsw_nn_make_params<NN>(96, 7, 128, DType::fp32, &base);
    Choice choice = p;
    NN consumed = sdk_consume(choice, 96, 7);
    assert(base == 20 && consumed.graph_degree == 96 && consumed.intermediate_graph_degree == 144);
    assert(consumed.max_iterations == 40 && consumed.dist_comp_dtype == DType::fp32);
    assert(consumed.termination_threshold == .0001f && consumed.metric == 7 && !consumed.return_distances);
    choice = NN(64, 7);
    consumed = sdk_consume(choice, 96, 7);
    assert(consumed.max_iterations == 20 && consumed.dist_comp_dtype == DType::fp16);
    puts("NN configured params survive modeled SDK consumer: PASS (CPU mock)");
}
