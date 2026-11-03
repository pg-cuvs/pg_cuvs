#ifndef CUVS_LW_GUARD_HPP
#define CUVS_LW_GUARD_HPP

/* I4 / ADR-088 decision 1: build-only large_workspace resource, CUDA-free.
 *
 * The CUDA wrapper instantiates CuvsLwWorkspaceGuard with operations that
 * call raft::resource::set_large_workspace_resource; the CPU unit test
 * instantiates it with a recording fake (CK-28). Keeping the lifetime rule
 * here lets the CPU test prove the order the wrapper relies on:
 *
 *   PooledRes acquired -> guard installs -> build -> guard restores
 *   -> device_resources returns to the free list (or is poisoned)
 *
 * The guard is declared after PooledRes in the same function, so C++ reverse
 * destruction restores the default before PooledRes runs, on every exit:
 * normal return, exception (the try block unwinds before its catch) and OOM.
 * Policy error never touches the resource (zero install calls).
 */

#include <atomic>
#include <cstdint>

#include "cuvs_lw_policy.h"

/* Lock-free live/peak counter for the build-only resource (lw_peak). */
struct CuvsLwCounter {
    std::atomic<uint64_t> live{0};
    std::atomic<uint64_t> peak{0};
    std::atomic<uint64_t> free_at_peak{UINT64_MAX};  /* physical free seen at the last peak */

    /* Returns true when this allocation raised the peak. */
    bool on_alloc(uint64_t bytes) noexcept
    {
        uint64_t now = live.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        uint64_t seen = peak.load(std::memory_order_relaxed);
        while (now > seen)
            if (peak.compare_exchange_weak(seen, now, std::memory_order_relaxed))
                return true;
        return false;
    }

    void on_free(uint64_t bytes) noexcept
    {
        live.fetch_sub(bytes, std::memory_order_relaxed);
    }

    void note_free_at_peak(uint64_t phys_free) noexcept
    {
        free_at_peak.store(phys_free, std::memory_order_relaxed);
    }
};

/* Ops: void install(); void restore() noexcept; */
template <class Ops>
class CuvsLwWorkspaceGuard {
  public:
    CuvsLwWorkspaceGuard(Ops &ops, uint32_t policy) : ops_(nullptr)
    {
        if (policy != CUVS_LW_OVERFLOW_MANAGED)
            return;
        ops.install();   /* may throw: then nothing was installed */
        ops_ = &ops;
    }
    ~CuvsLwWorkspaceGuard() { if (ops_) ops_->restore(); }
    bool active() const noexcept { return ops_ != nullptr; }

    CuvsLwWorkspaceGuard(const CuvsLwWorkspaceGuard &) = delete;
    CuvsLwWorkspaceGuard &operator=(const CuvsLwWorkspaceGuard &) = delete;

  private:
    Ops *ops_;
};

#endif /* CUVS_LW_GUARD_HPP */
