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
 */

#include <cstddef>
#include <memory_resource>

struct CuvsHostFreeFence {
    using SyncFn = int (*)(void *stream);

    struct Slot {
        bool active = false;
        void *stream = nullptr;  /* nullptr is a valid (default) stream */
        SyncFn sync = nullptr;
    };

    static Slot &current() noexcept
    {
        static thread_local Slot slot;
        return slot;
    }

    /* RAII: fence this thread's host frees on `stream` until destruction;
     * restores the enclosing scope (nesting-safe). */
    class Scope {
      public:
        Scope(void *stream, SyncFn sync) noexcept : prev_(current())
        {
            Slot &s = current();
            s.active = sync != nullptr;
            s.stream = stream;
            s.sync = sync;
        }
        ~Scope() { current() = prev_; }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;

      private:
        Slot prev_;
    };

    /* Called before each deallocation. Returns 1 when it waited on a stream. */
    static int before_free() noexcept
    {
        const Slot &s = current();
        if (!s.active)
            return 0;
        (void)s.sync(s.stream);  /* a failed sync still frees: the context is unusable */
        return 1;
    }
};

/* std::pmr resource: upstream allocation, fenced deallocation. */
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
        CuvsHostFreeFence::before_free();
        upstream_->deallocate(p, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override
    {
        return this == &other;
    }
};

#endif /* CUVS_HOST_FREE_FENCE_HPP */
