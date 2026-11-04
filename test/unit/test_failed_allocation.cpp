#include "cuvs_failed_allocation.hpp"
#include <cassert>
#include <atomic>
#include <limits>
#include <new>
#include <thread>
#include <vector>
#include <cstdio>

namespace failure = cuvs_failed_allocation;
static void *fail() { throw std::bad_alloc(); }
static void caught(std::size_t bytes) {
    try { failure::capture(bytes, fail); assert(false); }
    catch (const std::bad_alloc &) {}
}
int main() {
    assert(failure::take() == -1);
    caught(8192);
    assert(failure::take() == 8192);  // upstream X, independent of logical N
    assert(failure::take() == -1);
    caught(8192); caught(4096);  // the pool's final failed backoff request
    assert(failure::take() == 4096);
    caught(8192);
    failure::outer([]() -> void * {
        caught(4096);             // first growth attempt fails
        return nullptr;          // a smaller growth succeeds
    });
    assert(failure::take() == -1); // successful pool request must erase retries
    caught(8192);
    try {
        failure::outer([]() -> void * { throw std::bad_alloc(); });
    } catch (const std::bad_alloc &) {}
    assert(failure::take() == -1); // unrelated failure must not inherit old X
    try {
        try {
            failure::outer([]() -> void * { return failure::capture(8192,fail); });
        } catch (const std::bad_alloc &) {} // SDK handles this allocation failure
        throw std::bad_alloc();            // a later, unrelated host failure
    } catch (const std::bad_alloc &) {
        assert(failure::take_for_current_exception() == -1);
    }
    try {
        failure::outer([]() -> void * { return failure::capture(4096,fail); });
    } catch (const std::bad_alloc &) {
        assert(failure::take_for_current_exception() == 4096);
    }
    try {
        failure::outer([]() -> void * {
            caught(8192); // initial upstream growth refused
            failure::capture(4096, []() -> void * { return nullptr; });
            // Upstream allocation succeeded; pool bookkeeping then failed.
            // RMM can turn that host exception into its final OOM exception.
            throw std::bad_alloc();
        });
    } catch (const std::bad_alloc &) {
        assert(failure::take_for_current_exception() == -1);
    }
    caught(0);
    assert(failure::take() == -1);
    caught(std::numeric_limits<std::size_t>::max());
    assert(failure::take() == -1);
    std::vector<std::thread> threads;
    std::atomic<int> ready{0};
    for (int i=1; i<=8; ++i) threads.emplace_back([i,&ready] {
        caught(1024*i);
        ready.fetch_add(1,std::memory_order_release);
        while (ready.load(std::memory_order_acquire)<8) std::this_thread::yield();
        assert(failure::take() == 1024*i);
    });
    for (auto &thread : threads) thread.join();
    assert(failure::take() == -1);
    std::puts("PASS upstream request, final backoff, success/reset, unknown, 8-thread isolation");
}
