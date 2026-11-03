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
};

template <class Traits>
class CuvsResPool {
public:
    using Slot = CuvsResSlot<Traits>;

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
        {
            std::lock_guard<std::mutex> lk(mutex_);
            acquired_ = true;
            dedicated = dedicated_;
            if (!free_.empty()) {
                auto slot = std::move(free_.back());
                free_.pop_back();
                return slot;
            }
        }
        auto slot = std::make_unique<Slot>();
        if (dedicated) slot->stream = Traits::make_stream();
        slot->res = Traits::make_res(slot->stream.get());
        return slot;
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
    }
    size_t free_count()
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return free_.size();
    }

private:
    std::mutex mutex_;
    std::vector<std::unique_ptr<Slot>> free_;
    bool dedicated_ = CUVS_RMM_DEDICATED_STREAM_DEFAULT != 0;
    bool acquired_ = false;
};

#endif /* CUVS_RES_POOL_HPP */
