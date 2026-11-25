#pragma once

#include "cuvs_cagra_fold.h"
#include <cuvs/neighbors/cagra.hpp>

struct CuvsCagraFoldYield {};

inline void cuvs_cagra_fold_checkpoint(const CuvsCagraFoldControl *control)
{
    if (control && control->should_yield && control->should_yield(control->context))
        throw CuvsCagraFoldYield{};
}

void cuvs_cagra_fold_sdk(
    raft::resources const& res,
    const cuvs::neighbors::cagra::index<float, uint32_t>& source,
    raft::device_matrix_view<const float, int64_t> added,
    raft::device_matrix_view<float, int64_t, raft::layout_stride> dataset,
    cuvs::neighbors::cagra::index<float, uint32_t>& result,
    const CuvsCagraFoldControl *control);
