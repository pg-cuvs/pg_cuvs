#include "cuvs_search_staging.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <future>
#include <limits>

namespace {

using namespace std::chrono_literals;
constexpr size_t region = 256 * 1024;
constexpr size_t cap = 4 * region;

class Gate {
public:
    void arm()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        armed_ = true;
        entered_ = false;
        resumed_ = false;
    }

    void pause_once()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!armed_)
            return;
        armed_ = false;
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return resumed_; });
    }

    bool entered()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, 5s, [this] { return entered_; });
    }

    void resume()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        resumed_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool armed_ = false;
    bool entered_ = false;
    bool resumed_ = false;
};

struct FakeCuda {
    using Stream = int;
    static inline Gate allocation_gate;
    static inline Gate free_gate;
    static inline Gate pageable_gate;
    static inline bool fail_alloc = false;
    static inline bool ambiguous_alloc = false;
    static inline bool fail_free = false;
    static inline int fail_pageable_at = 0;
    static inline bool emulate_large_pageable = false;
    static inline size_t large_pageable_bytes = 0;
    static inline bool idle = true;
    static inline std::atomic<int> allocations{0};
    static inline std::atomic<int> frees{0};
    static inline int waits = 0;
    static inline int quarantines = 0;

    static size_t pin_granularity() { return 4096; }

    static bool allocate_pinned(void **ptr, size_t bytes)
    {
        ++allocations;
        allocation_gate.pause_once();
        if (!(fail_alloc && !ambiguous_alloc))
            *ptr = std::malloc(bytes);
        return !fail_alloc && *ptr;
    }

    static void *allocate_pageable(size_t bytes)
    {
        pageable_gate.pause_once();
        if (fail_pageable_at && --fail_pageable_at == 0)
            return nullptr;
        if (emulate_large_pageable && bytes > 64) {
            large_pageable_bytes = bytes;
            return std::malloc(64); // Check requested size without touching a large buffer.
        }
        return std::malloc(bytes);
    }

    static bool free_pinned(void *ptr)
    {
        ++frees;
        free_gate.pause_once();
        if (fail_free)
            return false;
        std::free(ptr);
        return true;
    }

    static bool wait(Stream stream)
    {
        assert(stream != 0);
        ++waits;
        return idle;
    }
    static void quarantine() { ++quarantines; }
};

using Pool = cuvs_search_staging::Pool<FakeCuda>;
using Lease = cuvs_search_staging::Lease<FakeCuda>;

void layout_checks()
{
    cuvs_search_staging::Layout layout;
    assert(layout.add<uint32_t>(17) == 0);
    assert(layout.add<int64_t>(3) == 128);
    assert(layout.add<float>(16) == 192);
    assert(layout.size() == 256);
    try {
        layout.add<int64_t>(std::numeric_limits<size_t>::max());
        assert(false);
    } catch (const std::length_error &) {
        assert(layout.size() == 256);
    }
}

void prewarm_and_device_caps()
{
    Pool gpu0, gpu1;
    Pool::Block *held[4];
    for (auto &block : held) {
        block = gpu0.acquire(region, cap);
        assert(block && block->capacity == region);
    }
    for (size_t a = 0; a < 4; ++a)
        for (size_t b = 0; b < a; ++b)
            assert(held[a]->data != held[b]->data);
    const int before = FakeCuda::allocations;
    Lease fallback(gpu0, cap, 1, 128, 64, 0);
    assert(!fallback.pinned() && fallback.data());
    assert(FakeCuda::allocations == before);
    Lease other_device(gpu1, cap, 2, 128, 64, 0);
    assert(other_device.pinned());
    gpu0.release(held[0]);
    Lease reused(gpu0, cap, 3, 128, 64, 0);
    assert(reused.data() == held[0]->data);
    for (size_t i = 1; i < 4; ++i)
        gpu0.release(held[i]);

    Pool rounded;
    assert(!rounded.acquire(4097, 8191));
    auto *block = rounded.acquire(4096, 8191);
    assert(block && block->capacity == 4096);
    rounded.release(block);
}

void fallbacks_and_growth()
{
    Pool pool;
    const int before = FakeCuda::allocations;
    Lease disabled(pool, 0, 1, 128, 64, 0);
    Lease oversized(pool, cap, 2, cap + 1, 64, 0);
    assert(disabled.data() && !disabled.pinned());
    assert(oversized.data() && !oversized.pinned());
    assert(FakeCuda::allocations == before);

    auto *small = pool.acquire(128, cap);
    assert(small && small->capacity == region);
    pool.release(small);

    FakeCuda::fail_alloc = true;
    assert(!pool.acquire(region + 1, cap));
    FakeCuda::fail_alloc = false;
    auto *unchanged = pool.acquire(128, cap);
    assert(unchanged == small);
    pool.release(unchanged);

    const int frees = FakeCuda::frees;
    auto *medium = pool.acquire(region + 1, cap);
    assert(medium && medium->capacity == 2 * region);
    assert(FakeCuda::frees == frees);
    assert(!pool.acquire(cap, cap));  // Medium is active and cannot be evicted.
    assert(FakeCuda::frees == frees);  // No idle block was freed for an impossible request.
    pool.release(medium);
    auto *big = pool.acquire(cap, cap);
    assert(big && big->capacity == cap);
    assert(FakeCuda::frees == frees + 2);
    pool.release(big);
}

void failed_free_and_ambiguous_allocation()
{
    Pool failed_free;
    auto *small = failed_free.acquire(region, 2 * region);
    assert(small);
    failed_free.release(small);
    FakeCuda::fail_free = true;
    assert(!failed_free.acquire(2 * region, 2 * region));
    FakeCuda::fail_free = false;
    assert(!failed_free.acquire(2 * region, 2 * region));
    auto *other = failed_free.acquire(region, 2 * region);
    assert(other && other->data != small->data);
    assert(!failed_free.acquire(region, 2 * region));
    failed_free.release(other);

    Pool failed_allocation;
    FakeCuda::fail_alloc = FakeCuda::ambiguous_alloc = true;
    assert(!failed_allocation.acquire(region, region));
    FakeCuda::fail_alloc = FakeCuda::ambiguous_alloc = false;
    const int before = FakeCuda::allocations;
    assert(!failed_allocation.acquire(region, region));
    assert(FakeCuda::allocations == before);
}

void reservations_under_contention(bool freeing)
{
    Pool pool;
    if (freeing) {
        auto *idle = pool.acquire(region, cap);
        assert(idle);
        pool.release(idle);
    }
    Gate &gate = freeing ? FakeCuda::free_gate : FakeCuda::allocation_gate;
    gate.arm();
    auto first = std::async(std::launch::async, [&] { return pool.acquire(cap, cap); });
    const bool entered = gate.entered();
    auto competing = std::async(std::launch::async, [&] {
        Lease lease(pool, cap, 1, cap, 64, 0);
        return lease.pinned();
    });
    const auto status = competing.wait_for(5s);
    gate.resume();
    auto *block = first.get();
    const bool competitor_pinned = competing.get();
    assert(entered && status == std::future_status::ready);
    assert(block && block->capacity == cap && !competitor_pinned);
    pool.release(block);
}

void pageable_admission_under_contention()
{
    Pool pool;
    Gate &gate = FakeCuda::pageable_gate;
    gate.arm();
    auto first = std::async(std::launch::async, [&] {
        return pool.acquire_pageable(48, 80);
    });
    const bool entered = gate.entered();
    auto competing = std::async(std::launch::async, [&] {
        Lease lease(pool, 0, 1, region, 48, 80);
        return lease.data() && lease.extra();
    });
    const auto status = competing.wait_for(5s);
    gate.resume();
    auto *block = first.get();
    const bool admitted = competing.get();
    assert(entered && status == std::future_status::ready && admitted);
    assert(block && block->data && block->extra);
    delete block;
}

void large_pageable_requests()
{
    Pool pool;
    constexpr size_t result_bytes = size_t{16384} * 2048 * (sizeof(int64_t) + sizeof(float));
    constexpr size_t slots = std::numeric_limits<int32_t>::max();
    constexpr size_t filter_bytes = ((slots + 31) / 32) * sizeof(uint32_t);
    FakeCuda::emulate_large_pageable = true;
    Lease bf(pool, 0, 1, result_bytes, result_bytes, 0);
    assert(!bf.pinned() && bf.data() && FakeCuda::large_pageable_bytes == result_bytes);
    Lease filtered(pool, 0, 2, region, 64, filter_bytes);
    assert(!filtered.pinned() && filtered.data() && filtered.extra());
    assert(FakeCuda::large_pageable_bytes == filter_bytes);
    FakeCuda::emulate_large_pageable = false;
}

void quarantine_closes_only_pageable_admission()
{
    Pool pool;
    void *normal = nullptr;
    const int waits = FakeCuda::waits;
    const int quarantines = FakeCuda::quarantines;
    try {
        Lease lease(pool, cap, 1, region, 64, 0);
        normal = lease.data();
        lease.mark_pending(true);
        throw 1;
    } catch (int) {
        assert(FakeCuda::waits == waits + 1);
    }

    FakeCuda::idle = false;
    {
        Lease lease(pool, cap, 2, region, 64, 128);
        assert(lease.data() == normal);
        lease.mark_pending(true);
        // The unused pageable buffer is released and admission stays open.
    }
    auto first = std::make_unique<Lease>(pool, 0, 3, region, 64, 128);
    Lease concurrent(pool, 0, 4, region, 64, 128);
    assert(first->data() != concurrent.data() && first->extra() != concurrent.extra());
    first->mark_pending(false, true);
    first.reset();

    for (int i = 0; i < 3; ++i) {
        try {
            Lease rejected(pool, 0, 5, region, 64, 0);
            assert(false);
        } catch (const cuvs_search_staging::PageableAllocationError &) {
            assert(FakeCuda::quarantines == quarantines + 2);
        }
    }
    try {
        Lease rejected(pool, cap, 6, region, 64, 128);
        assert(false);
    } catch (const cuvs_search_staging::PageableAllocationError &) {
    }
    Lease pinned(pool, cap, 6, region, 64, 0);
    assert(pinned.pinned() && pinned.data() != normal);
    Pool other_device;
    Lease pageable(other_device, 0, 7, region, 64, 0);
    assert(pageable.data() && !pageable.pinned());

    // A second, already admitted lease can also become unconfirmed.
    concurrent.mark_pending(false);
    assert(FakeCuda::waits == waits + 3);
}

void retain_only_submitted_pageable_address()
{
    Pool pool;
    auto *extra_only = pool.acquire_pageable(64, 128);
    auto *main_only = pool.acquire_pageable(64, 128);
    assert(extra_only && main_only);
    const auto *extra = extra_only->extra;
    const auto *data = main_only->data;
    pool.quarantine(nullptr, extra_only, false, true);
    pool.quarantine(nullptr, main_only, true, false);
    assert(extra_only->data == nullptr && extra_only->extra == extra);
    assert(main_only->data == data && main_only->extra == nullptr);
}

void pinned_quarantine_stays_capped()
{
    Pool pool;
    FakeCuda::idle = false;
    void *addresses[4];
    for (size_t i = 0; i < 4; ++i) {
        Lease lease(pool, cap, 6, region, 64, 0);
        assert(lease.pinned());
        addresses[i] = lease.data();
        for (size_t previous = 0; previous < i; ++previous)
            assert(lease.data() != addresses[previous]);
        lease.mark_pending(true);
    }
    const int pinned_calls = FakeCuda::allocations;
    const int free_calls = FakeCuda::frees;
    for (int i = 0; i < 3; ++i) {
        Lease fallback(pool, cap, 7, region, 64, 0);
        assert(!fallback.pinned() && fallback.data());
    }
    assert(FakeCuda::allocations == pinned_calls && FakeCuda::frees == free_calls);
    FakeCuda::idle = true;
}

void cpu_allocation_failure_is_recoverable()
{
    Pool pool;
    const int waits = FakeCuda::waits;
    for (int mode = 0; mode < 3; ++mode) {
        const size_t pin_cap = mode == 0 ? 0 : region;
        const size_t bytes = mode == 1 ? 2 * region : region;
        FakeCuda::fail_alloc = mode == 2;
        // Fail the extra buffer after the first pageable allocation succeeded.
        FakeCuda::fail_pageable_at = 2;
        try {
            Lease rejected(pool, pin_cap, 1, bytes, 64, 64);
            assert(false);
        } catch (const cuvs_search_staging::PageableAllocationError &) {
            assert(FakeCuda::fail_pageable_at == 0);
        }
        FakeCuda::fail_alloc = false;
        Lease recovered(pool, 0, 1, region, 64, 64);
        assert(recovered.data() && recovered.extra());
    }
    assert(FakeCuda::waits == waits);
}

} // namespace

int main()
{
    layout_checks();
    prewarm_and_device_caps();
    fallbacks_and_growth();
    failed_free_and_ambiguous_allocation();
    reservations_under_contention(false);
    reservations_under_contention(true);
    pageable_admission_under_contention();
    large_pageable_requests();
    quarantine_closes_only_pageable_admission();
    retain_only_submitted_pageable_address();
    pinned_quarantine_stays_capped();
    cpu_allocation_failure_is_recoverable();
    std::puts("test_search_staging: all checks passed");
}
