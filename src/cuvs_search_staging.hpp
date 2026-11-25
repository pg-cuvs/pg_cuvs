#ifndef CUVS_SEARCH_STAGING_HPP
#define CUVS_SEARCH_STAGING_HPP

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>

namespace cuvs_search_staging {

// This error is confined to CPU scratch preparation, before any search copy.
class PageableAllocationError : public std::exception {
public:
    explicit PageableAllocationError(const char *reason) noexcept : reason_{reason} {}
    const char *what() const noexcept override { return reason_; }

private:
    const char *reason_;
};

/* Each device has a pinned budget for active, idle, quarantined and in-flight
 * allocations/frees. CUDA calls run outside the mutex with their full charge
 * held until settlement. Pageable admission closes on unconfirmed DMA. */
template <class Ops>
class Pool {
public:
    struct Block {
        void *data = nullptr;
        size_t capacity = 0;
        Block *next = nullptr;
    };

    struct Pageable {
        void *data = nullptr;
        void *extra = nullptr;
        Pageable *next = nullptr;

        ~Pageable()
        {
            std::free(data);
            std::free(extra);
        }
    };

    // The caller supplies the same startup cap for every pinned request on this device.
    Block *acquire(size_t bytes, size_t cap)
    {
        if (bytes == 0 || bytes > cap)
            return nullptr;
        const size_t granularity = Ops::pin_granularity();
        if (!granularity)
            return nullptr;
        cap -= cap % granularity;
        if (bytes > cap)
            return nullptr;

        size_t capacity = min_block_bytes < cap ? min_block_bytes : cap;
        while (capacity < bytes)
            capacity = capacity <= cap / 2 ? capacity * 2 : cap;
        if (const size_t remainder = capacity % granularity)
            capacity += granularity - remainder;

        for (;;) {
            Block *victim;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                Block **fit = nullptr;
                size_t idle_bytes = 0;
                for (Block **link = &idle_; *link; link = &(*link)->next) {
                    idle_bytes += (*link)->capacity;
                    if ((*link)->capacity >= bytes &&
                        (!fit || (*link)->capacity < (*fit)->capacity))
                        fit = link;
                }
                if (fit) {
                    Block *block = *fit;
                    *fit = block->next;
                    block->next = nullptr;
                    return block;
                }

                if (pinned_charged_ <= cap && capacity <= cap - pinned_charged_) {
                    pinned_charged_ += capacity;
                    break;
                }
                // Leave the idle pool intact when even reclaiming it cannot fit.
                const size_t unreclaimable = pinned_charged_ - idle_bytes;
                if (unreclaimable > cap || capacity > cap - unreclaimable)
                    return nullptr;
                // The remaining candidates are idle with confirmed DMA completion.
                victim = idle_;
                idle_ = victim->next;
                victim->next = nullptr;
            }

            if (!Ops::free_pinned(victim->data)) {
                quarantine(victim, nullptr);
                return nullptr;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pinned_charged_ -= victim->capacity;
            }
            delete victim;
        }

        auto block = std::unique_ptr<Block>(new (std::nothrow) Block);
        if (block) {
            block->capacity = capacity;
            if (Ops::allocate_pinned(&block->data, capacity) && block->data)
                return block.release();
            if (block->data) {
                quarantine(block.release(), nullptr);
                return nullptr;
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        pinned_charged_ -= capacity;
        return nullptr;
    }

    void release(Block *block) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        block->next = idle_;
        idle_ = block;
    }

    Pageable *acquire_pageable(size_t bytes, size_t extra)
    {
        if (!bytes && !extra)
            return nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Allocations admitted before this latch closes may still finish.
            if (pageable_blocked_)
                throw PageableAllocationError("search pageable staging blocked by unconfirmed DMA");
        }
        try {
            auto block = std::make_unique<Pageable>();
            block->data = allocate_pageable(bytes);
            block->extra = allocate_pageable(extra);
            return block.release();
        } catch (const std::bad_alloc &) {
            throw PageableAllocationError("search pageable staging allocation failed");
        }
    }

    void quarantine(Block *pinned, Pageable *pageable,
                    bool keep_data = false, bool keep_extra = false) noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pinned) {
                pinned->next = pinned_quarantine_;
                pinned_quarantine_ = pinned;
            }
            if (pageable) {
                pageable_blocked_ = true;
                pageable->next = pageable_quarantine_;
                pageable_quarantine_ = pageable;
            }
        }
        // Neither the pool nor any other lease accesses the retained addresses.
        if (pageable && !keep_data) {
            std::free(pageable->data);
            pageable->data = nullptr;
        }
        if (pageable && !keep_extra) {
            std::free(pageable->extra);
            pageable->extra = nullptr;
        }
    }

private:
    static void *allocate_pageable(size_t bytes)
    {
        if (!bytes)
            return nullptr;
        if (void *data = Ops::allocate_pageable(bytes))
            return data;
        throw std::bad_alloc{};
    }

    static constexpr size_t min_block_bytes = 256 * 1024;
    std::mutex mutex_;
    size_t pinned_charged_ = 0;
    bool pageable_blocked_ = false;
    // Keep ownership until process exit, without CUDA calls at teardown.
    Block *idle_ = nullptr;
    Block *pinned_quarantine_ = nullptr;
    Pageable *pageable_quarantine_ = nullptr;
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

/* The stream owner outlives this lease. Mark staging before enqueueing
 * a copy and clear it only after a successful wait. */
template <class Ops>
class Lease {
public:
    Lease(Pool<Ops> &pool, size_t cap, typename Ops::Stream stream,
          size_t bytes, size_t fallback_bytes, size_t extra_bytes)
        : pool_{pool}, stream_{stream}, pinned_{pool.acquire(bytes, cap)}
    {
        try {
            pageable_ = pool_.acquire_pageable(pinned_ ? 0 : fallback_bytes, extra_bytes);
        } catch (...) {
            if (pinned_)
                pool_.release(pinned_);
            throw;
        }
    }
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;

    ~Lease() noexcept
    {
        const bool pageable_pending = page_pending_ || extra_pending_;
        if ((pin_pending_ || pageable_pending) && !Ops::wait(stream_)) {
            pool_.quarantine(pin_pending_ ? pinned_ : nullptr,
                             pageable_pending ? pageable_ : nullptr,
                             page_pending_, extra_pending_);
            Ops::quarantine();
            if (pin_pending_)
                pinned_ = nullptr;
            if (pageable_pending)
                pageable_ = nullptr;
        }
        if (pinned_)
            pool_.release(pinned_);
        delete pageable_;
    }

    bool pinned() const { return pinned_ != nullptr; }
    void *data() const
    {
        if (pinned_)
            return pinned_->data;
        return pageable_ ? pageable_->data : nullptr;
    }
    void *extra() const { return pageable_ ? pageable_->extra : nullptr; }
    void mark_pending(bool pinned, bool extra = false)
    {
        if (pinned)
            pin_pending_ = true;
        else if (extra)
            extra_pending_ = true;
        else
            page_pending_ = true;
    }
    void completed() { pin_pending_ = page_pending_ = extra_pending_ = false; }

private:
    Pool<Ops> &pool_;
    typename Ops::Stream stream_;
    typename Pool<Ops>::Block *pinned_;
    typename Pool<Ops>::Pageable *pageable_ = nullptr;
    bool pin_pending_ = false;
    bool page_pending_ = false;
    bool extra_pending_ = false;
};

} // namespace cuvs_search_staging

#endif
