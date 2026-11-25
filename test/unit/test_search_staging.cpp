#include "cuvs_search_staging.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {

struct FakeCuda {
    using Stream = int;
    static inline bool fail_alloc = false;
    static inline bool idle = true;
    static inline int allocations = 0;
    static inline int waits = 0;
    static inline int abandoned = 0;
    static inline int last_stream = 0;

    static void *allocate_pinned(size_t bytes)
    {
        ++allocations;
        return fail_alloc ? nullptr : std::malloc(bytes);
    }
    static void free_pinned(void *ptr) { std::free(ptr); }
    static bool wait(Stream stream)
    {
        ++waits;
        last_stream = stream;
        return idle;
    }
    static void quarantine() { ++abandoned; }
};

using Pool = cuvs_search_staging::Pool<FakeCuda>;
using Lease = cuvs_search_staging::Lease<FakeCuda>;
constexpr size_t cap = 1 << 20;

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

void exclusive_and_idle_reuse()
{
    Pool gpu0, gpu1;
    void *first;
    {
        Lease request(&gpu0, cap, 11, 128, 64, 64);
        Lease concurrent(&gpu0, cap, 12, 128, 64, 0);
        Lease other_device(&gpu1, cap, 13, 128, 64, 0);
        assert(request.pinned() && concurrent.pinned() && other_device.pinned());
        first = request.data();
        assert(first != concurrent.data() && first != other_device.data());
        assert(request.extra() && request.extra() != first);
        request.mark_pending();
        request.completed();
    }
    Lease reused(&gpu0, cap, 14, 128, 64, 0);
    assert(reused.data() == first && FakeCuda::waits == 0);
}

void fallbacks()
{
    Pool pool;
    const int before = FakeCuda::allocations;
    Lease disabled(&pool, 0, 21, 128, 64, 0);
    Lease oversized(&pool, 64, 22, 128, 64, 0);
    assert(disabled.data() && !disabled.pinned());
    assert(oversized.data() && !oversized.pinned());
    assert(FakeCuda::allocations == before);
    FakeCuda::fail_alloc = true;
    Lease failed_allocation(&pool, cap, 23, 128, 64, 0);
    FakeCuda::fail_alloc = false;
    assert(failed_allocation.data() && !failed_allocation.pinned());
    assert(FakeCuda::allocations == before + 1);

    void *small;
    {
        Lease warmed(&pool, cap, 24, 128, 64, 0);
        small = warmed.data();
        assert(warmed.pinned());
    }
    FakeCuda::fail_alloc = true;
    Lease failed_growth(&pool, cap, 25, cap, 64, 0);
    FakeCuda::fail_alloc = false;
    assert(!failed_growth.pinned());
    Lease reused(&pool, cap, 26, 128, 64, 0);
    assert(reused.pinned() && reused.data() == small);
}

void unwind_checks()
{
    Pool pool;
    void *recoverable = nullptr;
    try {
        Lease lease(&pool, cap, 31, 128, 64, 0);
        recoverable = lease.data();
        lease.mark_pending();
        throw 1;
    } catch (int) {
        assert(FakeCuda::waits == 1 && FakeCuda::last_stream == 31);
    }

    FakeCuda::idle = false;
    try {
        Lease lease(&pool, cap, 32, 128, 64, 32);
        assert(lease.data() == recoverable);
        lease.mark_pending();
        throw 2;
    } catch (int) {
        assert(FakeCuda::waits == 2 && FakeCuda::last_stream == 32);
        assert(FakeCuda::abandoned == 1);
    }
    Lease next(&pool, cap, 33, 128, 64, 0);
    assert(next.pinned() && next.data() != recoverable);

    {
        Lease pageable(&pool, 0, 34, 128, 64, 32);
        assert(pageable.data() && pageable.extra() && !pageable.pinned());
        pageable.mark_pending();
    }
    assert(FakeCuda::waits == 3 && FakeCuda::last_stream == 34);
    assert(FakeCuda::abandoned == 2);
}

} // namespace

int main()
{
    layout_checks();
    exclusive_and_idle_reuse();
    fallbacks();
    unwind_checks();
    std::puts("test_search_staging: all checks passed");
}
