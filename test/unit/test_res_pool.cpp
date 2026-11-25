// I3-D CK-33 (CPU): the wrapper's resource pool wiring with fake Res/Stream.
// A normal return retains the handles. Retirement discards them while keeping
// the dedicated stream alive for buffers that may still use it.
#include "cuvs_wrapper.h"
#include "cuvs_res_pool.hpp"
#include <cassert>
#include <cstdio>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

static unsigned checks;
#define CHECK(x) do { assert(x); checks++; } while (0)

// Atomic: the concurrency test constructs fakes on 8 threads (slots are
// built outside the pool lock by design), so plain ints would race.
static std::atomic<int> live_streams, live_res, made_streams, made_res;
static std::vector<std::string> events;

struct FakeStream {
    int id;
    bool ready = true;
    FakeStream() : id(++made_streams) { live_streams++; }
    ~FakeStream() { live_streams--; events.push_back("stream~" + std::to_string(id)); }
};
struct FakeRes {
    FakeStream *stream; // nullptr = per-thread default stream
    int id;
    explicit FakeRes(FakeStream *s) : stream(s), id(++made_res) { live_res++; }
    ~FakeRes() { live_res--; events.push_back("res~" + std::to_string(id)); }
};
struct FakeTraits {
    using Res = FakeRes;
    using Stream = FakeStream;
    static inline bool fail_next_res = false;
    static std::unique_ptr<Stream> make_stream() { return std::make_unique<Stream>(); }
    static bool stream_ready(Stream *s) noexcept { return s->ready; }
    static std::unique_ptr<Res> make_res(Stream *s)
    {
        if (fail_next_res) {
            fail_next_res = false;
            throw std::bad_alloc();
        }
        return std::make_unique<Res>(s);
    }
};
using Pool = CuvsResPool<FakeTraits>;

static void reset() { live_streams = 0; live_res = 0; made_streams = 0; made_res = 0; events.clear(); }

static void test_default_on_owns_and_reuses()
{
    reset();
    Pool pool;
    CHECK(CUVS_RMM_DEDICATED_STREAM_DEFAULT == 1 && pool.dedicated());
    auto a = pool.acquire();
    CHECK(a->stream && a->res && a->res->stream == a->stream.get());
    auto b = pool.acquire(); // concurrent borrower gets its own slot
    CHECK(b->stream.get() != a->stream.get() && made_streams == 2);
    FakeRes *res_a = a->res.get();
    pool.release(std::move(a));
    auto c = pool.acquire(); // normal completion: handles are reused
    CHECK(c->res.get() == res_a && made_streams == 2 && made_res == 2);
    // Direct slot destruction is ordered when no buffer refers to the stream.
    int id = c->res->id, sid = c->stream->id;
    events.clear();
    c.reset();
    CHECK(events.size() == 2 && events[0] == "res~" + std::to_string(id)
          && events[1] == "stream~" + std::to_string(sid));
    pool.release(std::move(b));
    CHECK(pool.free_count() == 1);
    pool.clear();
    CHECK(live_streams == 0 && live_res == 0 && pool.free_count() == 0);
}

// A returned stream can still be referenced by a buffer outside the slot.
static void test_stream_outlives_retired_resources()
{
    reset();
    Pool pool;
    auto slot = pool.acquire();
    FakeStream *const retained = slot->stream.get();
    pool.release(std::move(slot));
    slot = pool.acquire();
    for (int i = 0; i < 250; i++) {
        const int old_id = slot->res->id;
        pool.retire(std::move(slot));
        CHECK(live_res == 0 && live_streams == 1 && pool.free_count() == 0);
        slot = pool.acquire();
        CHECK(slot->stream.get() == retained && made_streams == 1);
        CHECK(slot->res->id != old_id && slot->res->stream == retained);
    }
    pool.release(std::move(slot));
    pool.clear();
    CHECK(live_res == 0 && live_streams == 1);
    delete retained; // stand in for process exit after outstanding buffers are gone
}

static void test_busy_stream_is_retained()
{
    reset();
    Pool pool;
    auto slot = pool.acquire();
    FakeStream *const busy = slot->stream.get();
    busy->ready = false;
    pool.retire(std::move(slot));

    auto other = pool.acquire();
    CHECK(other->stream.get() != busy && live_streams == 2);
    FakeStream *const spare = other->stream.get();
    busy->ready = true;
    auto recovered = pool.acquire();
    CHECK(recovered->stream.get() == busy && made_streams == 2);
    pool.release(std::move(recovered));
    pool.retire(std::move(other));
    pool.clear();
    CHECK(live_res == 0 && live_streams == 2);
    delete busy;
    delete spare;
}

static void test_rebuild_failure_keeps_the_stream()
{
    reset();
    Pool pool;
    auto slot = pool.acquire();
    FakeStream *const retained = slot->stream.get();
    pool.retire(std::move(slot));
    FakeTraits::fail_next_res = true;
    try {
        pool.acquire();
        CHECK(false);
    } catch (const std::bad_alloc &) {
        CHECK(live_res == 0 && live_streams == 1);
    }
    slot = pool.acquire();
    CHECK(slot->stream.get() == retained && made_streams == 1);
    pool.release(std::move(slot));
    pool.clear();
    CHECK(live_res == 0 && live_streams == 1);
    delete retained;
}

static void test_off_is_i2_behaviour()
{
    reset();
    Pool pool;
    CHECK(pool.set_dedicated(false) == 0 && !pool.dedicated());
    auto a = pool.acquire();
    CHECK(!a->stream && a->res && a->res->stream == nullptr && made_streams == 0);
    pool.retire(std::move(a));
    pool.clear();
    CHECK(live_res == 0 && made_streams == 0);
}

static void test_mode_fixed_after_first_use()
{
    reset();
    Pool pool;
    CHECK(pool.set_dedicated(true) == 0);
    auto a = pool.acquire();
    CHECK(pool.set_dedicated(false) == -1 && pool.dedicated()); // never mixes slots
    CHECK(pool.set_dedicated(true) == 0);
    pool.release(std::move(a));
    pool.clear();
}

static void test_abandon_leaks_by_design()
{
    reset();
    Pool pool;
    auto healthy = pool.acquire();
    auto failed = pool.acquire();
    pool.release(std::move(healthy));
    pool.retire(std::move(failed));
    pool.abandon(); // lost CUDA context at exit: driver reclaims, no destructor runs
    CHECK(pool.free_count() == 0 && live_res == 1 && live_streams == 2
          && events.size() == 1 && events[0] == "res~2");
}

static void test_threads_never_share_a_slot()
{
    reset();
    Pool pool;
    std::vector<std::thread> threads;
    std::vector<FakeRes *> seen(8);
    for (int t = 0; t < 8; t++)
        threads.emplace_back([&, t] {
            for (int i = 0; i < 200; i++) {
                auto s = pool.acquire();
                assert(s->res->stream == s->stream.get());
                seen[t] = s->res.get();
                pool.release(std::move(s));
            }
        });
    for (auto &th : threads) th.join();
    CHECK(made_streams <= 8 && made_streams == made_res); // reuse bounded by concurrency
    pool.clear();
    CHECK(live_streams == 0 && live_res == 0);
}

// Retained streams are bounded by concurrency: 256 retirements across 8
// concurrent slots reuse the same 8 streams instead of creating new ones.
static void test_retired_streams_bounded_by_concurrency()
{
    reset();
    std::vector<FakeStream *> streams;
    {
        Pool pool;
        std::vector<std::unique_ptr<Pool::Slot>> held(8);
        for (int round = 0; round < 32; round++) {
            for (auto &s : held) s = pool.acquire();
            for (auto &s : held) pool.retire(std::move(s));
            CHECK(made_streams == 8 && live_streams == 8 && live_res == 0);
        }
        for (auto &s : held) {
            s = pool.acquire();
            streams.push_back(s->stream.get());
        }
        for (auto &s : held) pool.release(std::move(s));
        pool.clear();
        CHECK(made_res == 8 * 33 && live_res == 0 && live_streams == 8);
    }
    for (FakeStream *s : streams) delete s;
    CHECK(live_streams == 0);
}

int main()
{
    test_default_on_owns_and_reuses();
    test_stream_outlives_retired_resources();
    test_busy_stream_is_retained();
    test_rebuild_failure_keeps_the_stream();
    test_off_is_i2_behaviour();
    test_mode_fixed_after_first_use();
    test_abandon_leaks_by_design();
    test_threads_never_share_a_slot();
    test_retired_streams_bounded_by_concurrency();
    std::printf("PASS %u CK-33 CPU checks: handle and stream lifetime, readiness, reuse and teardown\n", checks);
    return 0;
}
