#pragma once

#include "cuvs_cagra_fold.h"
#include <cuvs/neighbors/cagra.hpp>
#include <chrono>

struct CuvsCagraFoldYield {};

inline void cuvs_cagra_fold_checkpoint(const CuvsCagraFoldControl *control)
{
    if (control && control->should_yield && control->should_yield(control->context))
        throw CuvsCagraFoldYield{};
}

class CuvsCagraFoldTimer {
    using Clock = std::chrono::steady_clock;
    CuvsCagraFoldPhases *phases;
    Clock::time_point start;

public:
    explicit CuvsCagraFoldTimer(const CuvsCagraFoldControl *control)
        : phases(control ? control->phases : nullptr),
          start(phases ? Clock::now() : Clock::time_point{}) {}

    bool enabled() const { return phases != nullptr; }

    void finish(uint64_t CuvsCagraFoldPhases::*phase)
    {
        if (phases) {
            const auto now = Clock::now();
            phases->*phase += std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
            start = now;
        }
    }
};

void cuvs_cagra_fold_sdk(
    raft::resources const& res,
    const cuvs::neighbors::cagra::index<float, uint32_t>& source,
    raft::device_matrix_view<const float, int64_t> added,
    raft::device_matrix_view<float, int64_t, raft::layout_stride> dataset,
    cuvs::neighbors::cagra::index<float, uint32_t>& result,
    const CuvsCagraFoldControl *control);
