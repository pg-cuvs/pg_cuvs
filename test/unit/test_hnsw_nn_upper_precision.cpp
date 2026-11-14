#include "hnsw_precision.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
int main() {
    using namespace cuvs::neighbors::hnsw::detail::upper_precision;
    float x[] = {0.0f, 65504.0f, -65504.0f};
    assert(!nn_input_needs_fp32(x, 1, 3));
    x[0] = 65505.0f;
    assert(nn_input_needs_fp32(x, 1, 3));
    x[0] = NAN;
    assert(nn_input_needs_fp32(x, 1, 3));
    puts("B3 NN upper input conversion precision: PASS (CPU helper)");
}
