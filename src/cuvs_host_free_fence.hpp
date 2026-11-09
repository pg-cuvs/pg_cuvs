#ifndef CUVS_HOST_FREE_FENCE_HPP
#define CUVS_HOST_FREE_FENCE_HPP

/* Q6 (run 20261109-q-03): a RAFT host buffer is not returned to the allocator
 * while GPU work queued on the request's stream may still write into it.
 *
 * Under HMM (cudaDevAttrPageableMemoryAccess = 1) cudaPointerGetAttributes
 * reports a malloc'd pointer as device-accessible, so cuVS utils::memzero
 * takes its host_and_device branch and zeroes a raft::host_vector with
 * cudaMemsetAsync on the build stream (ivf_pq::build: accum_sorted_sizes).
 * When the build then throws (RMM out_of_memory) before that stream drains,
 * ~index_impl frees the vector at once; the queued memset lands on the freed
 * chunk, zeroes glibc's free-list links, and a later free in the daemon
 * crashes in malloc_consolidate (SIGSEGV) or aborts.
 *
 * The CUDA wrapper installs CuvsFencedHostResource as RAFT's default host
 * resource. PooledRes opens a CuvsHostFreeFence::Scope for the borrowed
 * stream, so every RAFT host container freed on that thread while the
 * request runs (including frees during exception unwinding) first waits for
 * that stream. Other threads, and this thread outside a request, free as
 * before. CUDA-free so the CPU unit test exercises the same class.
 *
 * A failed wait (review A2 B1): the stream's work was not confirmed complete,
 * so a queued write may still land. The block is then quarantined: it is not
 * returned upstream (a deliberate leak), the error code, stream and device go
 * to the scope's failure hook (the wrapper logs them), process-wide counters
 * grow, and the scope records the failure so its owner can fail the request
 * and poison the resource. Nothing here throws, so this is safe during
 * exception unwinding.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory_resource>

struct CuvsHostFreeFence {
    using SyncFn = int (*)(void *stream);  /* 0 = complete, else an error code */
    /* Failure hook: error code, stream, device, bytes being quarantined. Must not throw. */
    using FailFn = void (*)(int err, void *stream, int device, std::size_t bytes);

    struct Stats {
        std::atomic<uint64_t> sync_failures{0};
        std::atomic<uint64_t> quarantined_blocks{0};
        std::atomic<uint64_t> quarantined_bytes{0};
    };
    static Stats &stats() noexcept
    {
        static Stats s;
        return s;
    }

    class Scope;
    struct Slot {
        bool active = false;
        void *stream = nullptr;  /* nullptr is a valid (default) stream */
        SyncFn sync = nullptr;
        int device = -1;
        FailFn on_fail = nullptr;
        Scope *owner = nullptr;
    };

    static Slot &current() noexcept
    {
        static thread_local Slot slot;
        return slot;
    }

    /* RAII: fence this thread's host frees on `stream` until destruction;
     * restores the enclosing scope (nesting-safe). Failures are counted on the
     * scope that was active when they happened. */
    class Scope {
      public:
        Scope(void *stream, SyncFn sync, int device = -1, FailFn on_fail = nullptr) noexcept
            : prev_(current())
        {
            Slot &s = current();
            s.active = sync != nullptr;
            s.stream = stream;
            s.sync = sync;
            s.device = device;
            s.on_fail = on_fail;
            s.owner = this;
        }
        ~Scope() { current() = prev_; }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
        /* Frees in this scope whose wait failed (each one was quarantined). */
        unsigned long failures() const noexcept { return failures_; }
        int last_error() const noexcept { return last_error_; }

      private:
        friend struct CuvsHostFreeFence;
        Slot prev_;
        unsigned long failures_ = 0;
        int last_error_ = 0;
    };

    enum : int { NOT_FENCED = 0, WAITED = 1, WAIT_FAILED = -1 };

    /* Called before each deallocation of `bytes`. NOT_FENCED outside a scope,
     * WAITED after a successful wait, WAIT_FAILED when the wait reported an
     * error: the caller must then not return the block upstream. */
    static int before_free(std::size_t bytes) noexcept
    {
        const Slot &s = current();
        if (!s.active)
            return NOT_FENCED;
        const int err = s.sync(s.stream);
        if (err == 0)
            return WAITED;
        stats().sync_failures.fetch_add(1, std::memory_order_relaxed);
        if (s.owner) {
            s.owner->failures_++;
            s.owner->last_error_ = err;
        }
        if (s.on_fail)
            s.on_fail(err, s.stream, s.device, bytes);
        return WAIT_FAILED;
    }
};

/* std::pmr resource: upstream allocation, fenced deallocation. A block whose
 * wait failed is quarantined (kept allocated) instead of freed. */
class CuvsFencedHostResource final : public std::pmr::memory_resource {
  public:
    explicit CuvsFencedHostResource(std::pmr::memory_resource *upstream =
                                        std::pmr::new_delete_resource()) noexcept
        : upstream_(upstream) {}

  private:
    std::pmr::memory_resource *upstream_;

    void *do_allocate(std::size_t bytes, std::size_t alignment) override
    {
        return upstream_->allocate(bytes, alignment);
    }
    void do_deallocate(void *p, std::size_t bytes, std::size_t alignment) override
    {
        if (CuvsHostFreeFence::before_free(bytes) == CuvsHostFreeFence::WAIT_FAILED) {
            CuvsHostFreeFence::Stats &st = CuvsHostFreeFence::stats();
            st.quarantined_blocks.fetch_add(1, std::memory_order_relaxed);
            st.quarantined_bytes.fetch_add(bytes, std::memory_order_relaxed);
            return;  /* quarantined: a queued write may still land here */
        }
        upstream_->deallocate(p, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override
    {
        return this == &other;
    }
};

#endif /* CUVS_HOST_FREE_FENCE_HPP */
