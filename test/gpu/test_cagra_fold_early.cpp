#include "cuvs_cagra_fold.h"

#include <new>
#include <utility>

static thread_local std::size_t fail_bytes;
static thread_local bool allocation_failed;

extern "C" void *__real__Znwm(std::size_t);

extern "C" void *__wrap__Znwm(std::size_t bytes)
{
    if (fail_bytes && bytes == fail_bytes)
    {
        fail_bytes = 0;
        allocation_failed = true;
        throw std::bad_alloc{};
    }
    return __real__Znwm(bytes);
}

/* Throw from the pre-acquisition checkpoint. This exercises the same
 * bad_alloc handler as a failed scope.emplace before any work submission. */
static int
fail_host_allocation(void *)
{
    throw std::bad_alloc{};
}

extern "C" int
test_cagra_fold_rank_oom(CuvsCagraIndex source, const float *added, int dim)
{
    constexpr int rows = 23;
    CuvsCagraIndex result = nullptr;
    CuvsCagraFoldShape shape;
    if (cuvs_cagra_fold_shape(source, &shape) != 0)
        return 0;

    /* One caller-thread allocation for all 23 rows of rank scratch. A
     * return to per-thread allocations must miss this injected failure. */
    allocation_failed = false;
    fail_bytes = rows * 2 * shape.degree * sizeof(std::pair<uint32_t, std::size_t>);
    const auto status = cuvs_cagra_fold_reuse(source, added, rows, dim, 0, &result);
    fail_bytes = 0;
    if (result && result != source)
        cuvs_cagra_free(result, 0);
    return allocation_failed && status == CUVS_CAGRA_FOLD_OOM && result == nullptr;
}

extern "C" int
test_cagra_fold_early_oom(CuvsCagraIndex source, const float *added, int dim)
{
    CuvsCagraIndex result = nullptr;
    CuvsCagraFoldControl control{};
    control.should_yield = fail_host_allocation;
    const auto status = cuvs_cagra_fold_reuse_with_yield(source, added, 1, dim, 0, &result, &control);
    if (result && result != source)
        cuvs_cagra_free(result, 0);
    return status == CUVS_CAGRA_FOLD_OOM && result == nullptr;
}
