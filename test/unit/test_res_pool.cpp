// I3-D CK-33 (CPU): the wrapper's resource pool wiring with fake Res/Stream.
// on: each slot owns a stream and its Res is built on that stream; poison
// (drop without release) destroys Res before its stream; normal release keeps
// the slot for reuse (no new stream/handles). off: the default stream, as I2.
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
    static std::unique_ptr<Stream> make_stream() { return std::make_unique<Stream>(); }
    static std::unique_ptr<Res> make_res(Stream *s) { return std::make_unique<Res>(s); }
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
    // poison: the slot is dropped, Res before its own stream, nothing leaks
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

static void test_off_is_i2_behaviour()
{
    reset();
    Pool pool;
    CHECK(pool.set_dedicated(false) == 0 && !pool.dedicated());
    auto a = pool.acquire();
    CHECK(!a->stream && a->res && a->res->stream == nullptr && made_streams == 0);
    pool.release(std::move(a));
    pool.clear();
    CHECK(live_res == 0);
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
    pool.release(pool.acquire());
    pool.abandon(); // lost CUDA context at exit: driver reclaims, no destructor runs
    CHECK(pool.free_count() == 0 && live_res == 1 && live_streams == 1 && events.empty());
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

int main()
{
    test_default_on_owns_and_reuses();
    test_off_is_i2_behaviour();
    test_mode_fixed_after_first_use();
    test_abandon_leaks_by_design();
    test_threads_never_share_a_slot();
    std::printf("PASS %u CK-33 CPU checks: default on, slot owns stream and builds Res on it, reuse on release, poison destroys Res then stream, off = per-thread default, mode fixed after first use; CUDA effect not run (QR)\n", checks);
    return 0;
}
