#ifndef CUVS_RES_POOL_HPP
#define CUVS_RES_POOL_HPP

/* I3-D (UC-6 A, v6 1.4 I3-D, CK-33): per-device free list of RAFT resources
 * that may own a dedicated RMM stream. CUDA-free template so the CPU test can
 * drive the same code with fake Res/Stream types; the wrapper instantiates it
 * with raft::device_resources and rmm::cuda_stream.
 *
 * Traits must provide:
 *   static std::unique_ptr<Stream> make_stream();             // dedicated stream
 *   static std::unique_ptr<Res> make_res(Stream *stream);     // nullptr = default
 *                                                              // (per-thread) stream
 *   static bool stream_ready(Stream *stream) noexcept;
 * A slot destroys its Res before the Stream it was built on (member order). */
#include <memory>
#include <mutex>
#include <vector>

#ifndef CUVS_RMM_DEDICATED_STREAM_DEFAULT
#error "include cuvs_wrapper.h first: CUVS_RMM_DEDICATED_STREAM_DEFAULT is defined there"
#endif

template <class Traits>
struct CuvsResSlot {
    using Res = typename Traits::Res;
    using Stream = typename Traits::Stream;
    std::unique_ptr<Stream> stream; /* null: --rmm-dedicated-stream off */
    std::unique_ptr<Res> res;       /* declared after stream: destroyed first */
    std::unique_ptr<CuvsResSlot> next_retired;
    bool stream_retired = false;

    ~CuvsResSlot()
    {
        /* A detached request can free a buffer during daemon teardown.
         * Retired streams keep their handle until CUDA exits, including when
         * a stream was recycled into a healthy slot and later cleared. */
        if (stream_retired) (void)stream.release();
    }
};

template <class Traits>
class CuvsResPool {
public:
    using Slot = CuvsResSlot<Traits>;

    ~CuvsResPool() { clear(); }

    /* Mode is fixed before the first acquisition (daemon startup flag), so
     * the pool never mixes slots with and without a dedicated stream. */
    int set_dedicated(bool on)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (acquired_ && on != dedicated_) return -1;
        dedicated_ = on;
        return 0;
    }
    bool dedicated() const { return dedicated_; }

    /* Reuse a returned slot or build one. Construction happens outside the
     * lock; the caller has already selected the device. */
    std::unique_ptr<Slot> acquire()
    {
        bool dedicated;
        std::unique_ptr<Slot> slot;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            acquired_ = true;
            dedicated = dedicated_;
            if (!free_.empty()) {
                auto slot = std::move(free_.back());
                free_.pop_back();
                return slot;
            }
            slot = take_retired_locked();
        }
        /* Probe one retired stream per miss, without holding the pool lock.
         * A buffer can still free on this stream while its handles are gone. */
        if (slot && !Traits::stream_ready(slot->stream.get()))
            retire(std::move(slot));
        const bool recycled = slot != nullptr;
        if (!slot) {
            slot = std::make_unique<Slot>();
            if (dedicated) slot->stream = Traits::make_stream();
        }
        try {
            slot->res = Traits::make_res(slot->stream.get());
        } catch (...) {
            if (recycled) retire(std::move(slot));
            throw;
        }
        return slot;
    }

    /* Failed handles never go back to the free list. Retain the dedicated
     * stream in its existing slot so retirement needs no allocation. */
    void retire(std::unique_ptr<Slot> slot) noexcept
    {
        slot->res.reset();
        if (!slot->stream) return;
        slot->stream_retired = true;
        std::lock_guard<std::mutex> lk(mutex_);
        Slot *tail = slot.get();
        if (retired_tail_)
            retired_tail_->next_retired = std::move(slot);
        else
            retired_ = std::move(slot);
        retired_tail_ = tail;
    }

    /* Normal completion: keep the handles (and their descriptor caches). */
    void release(std::unique_ptr<Slot> slot)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        free_.push_back(std::move(slot));
    }

    /* Teardown helpers used by the RMM registry destructor. */
    void clear()
    {
        std::lock_guard<std::mutex> lk(mutex_);
        free_.clear();
        while (retired_) take_retired_locked();
    }
    void abandon() /* lost CUDA context: let the driver reclaim */
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto &slot : free_) {
            if (slot) {
                (void)slot->res.release();
                (void)slot->stream.release();
                (void)slot.release();
            }
        }
        free_.clear();
        (void)retired_.release();
        retired_tail_ = nullptr;
    }
    size_t free_count()
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return free_.size();
    }

private:
    /* Caller holds mutex_. Detach the link before destroying a slot. */
    std::unique_ptr<Slot> take_retired_locked()
    {
        auto slot = std::move(retired_);
        if (slot) {
            retired_ = std::move(slot->next_retired);
            if (!retired_) retired_tail_ = nullptr;
        }
        return slot;
    }

    std::mutex mutex_;
    std::vector<std::unique_ptr<Slot>> free_;
    std::unique_ptr<Slot> retired_;
    Slot *retired_tail_ = nullptr;
    bool dedicated_ = CUVS_RMM_DEDICATED_STREAM_DEFAULT != 0;
    bool acquired_ = false;
};

#endif /* CUVS_RES_POOL_HPP */
