#ifndef CUVS_SEARCH_STAGING_HPP
#define CUVS_SEARCH_STAGING_HPP

#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>

namespace cuvs_search_staging {

/* A pool belongs to one device. Idle blocks have process lifetime because
 * CUDA is not necessarily available during static destruction. */
template <class Ops>
class Pool {
public:
    struct Block {
        void *data = nullptr;
        size_t capacity = 0;
        Block *next = nullptr;
    };

    Block *acquire(size_t bytes, size_t cap)
    {
        if (cap == 0 || bytes > cap)
            return nullptr;

        Block *block = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (idle_) {
                block = idle_;
                idle_ = block->next;
                block->next = nullptr;
            }
        }
        if (!block) {
            block = new (std::nothrow) Block;
            if (!block)
                return nullptr;
        }
        if (bytes <= block->capacity)
            return block;

        size_t capacity = min_block_bytes < cap ? min_block_bytes : cap;
        while (capacity < bytes)
            capacity = capacity <= cap / 2 ? capacity * 2 : cap;

        /* Do not hold the pool mutex while CUDA allocates or frees. */
        void *data = Ops::allocate_pinned(capacity);
        if (!data) {
            if (block->data)
                release(block);
            else
                delete block;
            return nullptr;
        }
        if (block->data)
            Ops::free_pinned(block->data);
        block->data = data;
        block->capacity = capacity;
        return block;
    }

    void release(Block *block) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        block->next = idle_;
        idle_ = block;
    }

private:
    static constexpr size_t min_block_bytes = 256 * 1024;
    std::mutex mutex_;
    Block *idle_ = nullptr;
};

class Layout {
public:
    template <class T>
    size_t add(size_t count)
    {
        static_assert(alignof(T) <= alignof(std::max_align_t), "unsupported staging alignment");
        constexpr size_t max = std::numeric_limits<size_t>::max();
        if (count > (max - (alignment - 1)) / sizeof(T))
            throw std::length_error("search staging size overflow");
        size_t bytes = (count * sizeof(T) + alignment - 1) & ~(alignment - 1);
        if (bytes > max - size_)
            throw std::length_error("search staging size overflow");
        size_t offset = size_;
        size_ += bytes;
        return offset;
    }

    size_t size() const { return size_; }

private:
    static constexpr size_t alignment = 64;
    size_t size_ = 0;
};

/* The caller owns the stream and outlives this lease. Mark before enqueueing
 * a copy and clear only after a successful wait. On unwind, a failed wait
 * deliberately strands the storage (pinned and pageable) so pending DMA
 * cannot touch freed storage or storage leased to another request. */
template <class Ops>
class Lease {
public:
    Lease(Pool<Ops> *pool, size_t cap, typename Ops::Stream stream,
          size_t bytes, size_t fallback_bytes, size_t extra_bytes)
        : pool_{pool}, stream_{stream}, extra_{allocate(extra_bytes), &std::free}
    {
        if (pool_ && bytes)
            pinned_ = pool_->acquire(bytes, cap);
        if (!pinned_)
            pageable_.reset(allocate(fallback_bytes));
    }
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;

    ~Lease() noexcept
    {
        if (pending_ && !Ops::wait(stream_)) {
            Ops::quarantine();
            (void)pageable_.release();
            (void)extra_.release();
            return;
        }
        if (pinned_)
            pool_->release(pinned_);
    }

    bool pinned() const { return pinned_ != nullptr; }
    void *data() const { return pinned_ ? pinned_->data : pageable_.get(); }
    void *extra() const { return extra_.get(); }
    void mark_pending() { pending_ = true; }
    void completed() { pending_ = false; }

private:
    static void *allocate(size_t bytes)
    {
        if (bytes == 0)
            return nullptr;
        if (void *data = std::malloc(bytes))
            return data;
        throw std::bad_alloc{};
    }

    Pool<Ops> *pool_;
    typename Ops::Stream stream_;
    std::unique_ptr<void, decltype(&std::free)> pageable_{nullptr, &std::free};
    std::unique_ptr<void, decltype(&std::free)> extra_;
    typename Pool<Ops>::Block *pinned_ = nullptr;
    bool pending_ = false;
};

} // namespace cuvs_search_staging

#endif
