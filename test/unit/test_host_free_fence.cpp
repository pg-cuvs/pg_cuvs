// Q6 (run 20261109-q-03): a RAFT host buffer freed while a request's stream
// still has a queued write into it (cuVS memzero -> cudaMemsetAsync on a
// malloc'd host_vector under HMM, then an OOM unwind) must not reach the
// allocator before that write lands. The fake stream below queues writes and
// runs them on sync; the fake upstream keeps freed blocks and records any
// queued write that lands on one ("late write", the observed corruption).
// The unfenced case reproduces the late write, so the fenced checks are not
// vacuous.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory_resource>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "cuvs_host_free_fence.hpp"

static int failures;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static std::vector<std::string> events;

struct FakeUpstream final : std::pmr::memory_resource {
    std::set<void *> freed;   // kept, not released, so a late write is observable
    std::vector<void *> to_release;
    int frees = 0;
    void *do_allocate(std::size_t bytes, std::size_t) override
    {
        void *p = ::operator new(bytes);
        std::memset(p, 0x5A, bytes);
        events.push_back("alloc");
        return p;
    }
    void do_deallocate(void *p, std::size_t, std::size_t) override
    {
        frees++;
        freed.insert(p);
        to_release.push_back(p);
        events.push_back("free");
    }
    bool do_is_equal(const std::pmr::memory_resource &o) const noexcept override { return this == &o; }
    void release_all()
    {
        for (void *p : to_release) ::operator delete(p);
        to_release.clear();
        freed.clear();
    }
};

struct FakeStream {
    std::vector<std::function<void()>> queued;
    int syncs = 0;
};

static FakeUpstream *g_up;
static int late_writes;

static void queue_zero(FakeStream &s, void *p, std::size_t n)
{
    s.queued.push_back([p, n] {
        if (g_up->freed.count(p)) late_writes++;   // landed on a freed chunk
        std::memset(p, 0, n);
    });
}

static int fake_sync(void *stream)
{
    auto *s = static_cast<FakeStream *>(stream);
    s->syncs++;
    events.push_back("sync");
    for (auto &w : s->queued) w();
    s->queued.clear();
    return 0;
}

// Drain whatever is still queued (the GPU finishing later, after the free).
static void gpu_finishes(FakeStream &s)
{
    for (auto &w : s.queued) w();
    s.queued.clear();
}

// Mirrors ivf_pq::build: a host vector zeroed on the stream, then an OOM
// before the stream drains; the vector is freed during unwinding.
struct HostVector {
    std::pmr::memory_resource *mr;
    void *p;
    std::size_t n;
    HostVector(std::pmr::memory_resource *m, std::size_t bytes) : mr(m), p(m->allocate(bytes, 8)), n(bytes) {}
    ~HostVector() { mr->deallocate(p, n, 8); }
};

static void build_that_ooms(std::pmr::memory_resource *mr, FakeStream &s)
{
    HostVector accum(mr, 4000);
    queue_zero(s, accum.p, accum.n);
    throw std::bad_alloc();
}

static void reset(FakeUpstream &up)
{
    up.release_all();
    up.frees = 0;
    events.clear();
    late_writes = 0;
}

int main()
{
    FakeUpstream up;
    g_up = &up;
    CuvsFencedHostResource fenced(&up);

    // 1. The unfenced resource reproduces the defect: the OOM unwind frees
    //    the vector, then the queued zero-fill lands on the freed chunk.
    {
        FakeStream s;
        try { build_that_ooms(&up, s); } catch (const std::bad_alloc &) {}
        gpu_finishes(s);
        CHECK(late_writes == 1);
        reset(up);
    }

    // 2. Fenced, OOM unwind inside the request scope (the PooledRes scope
    //    encloses the try/catch, as in cuvs_cagra_build_multi): the free
    //    waits for the stream, so nothing lands after the free.
    {
        FakeStream s;
        {
            CuvsHostFreeFence::Scope scope(&s, fake_sync);
            try { build_that_ooms(&fenced, s); } catch (const std::bad_alloc &) {}
        }
        gpu_finishes(s);
        CHECK(late_writes == 0);
        CHECK(s.syncs == 1);
        CHECK(up.frees == 1);
        CHECK(events.size() == 3 && events[0] == "alloc" && events[1] == "sync" && events[2] == "free");
        reset(up);
    }

    // 3. Fenced, normal free inside the scope: sync precedes the free.
    {
        FakeStream s;
        {
            CuvsHostFreeFence::Scope scope(&s, fake_sync);
            HostVector v(&fenced, 64);
            queue_zero(s, v.p, v.n);
        }
        gpu_finishes(s);
        CHECK(late_writes == 0);
        CHECK(events.size() == 3 && events[1] == "sync" && events[2] == "free");
        reset(up);
    }

    // 4. Outside any scope a free does not sync (other threads, idle paths).
    {
        FakeStream s;
        { HostVector v(&fenced, 64); }
        CHECK(s.syncs == 0);
        CHECK(up.frees == 1);
        CHECK(CuvsHostFreeFence::before_free() == 0);
        reset(up);
    }

    // 5. Nested scopes: the inner stream while inside, the outer after it
    //    closes, none after both close.
    {
        FakeStream a, b;
        {
            CuvsHostFreeFence::Scope outer(&a, fake_sync);
            {
                CuvsHostFreeFence::Scope inner(&b, fake_sync);
                { HostVector v(&fenced, 8); }
                CHECK(b.syncs == 1 && a.syncs == 0);
            }
            { HostVector v(&fenced, 8); }
            CHECK(a.syncs == 1 && b.syncs == 1);
        }
        { HostVector v(&fenced, 8); }
        CHECK(a.syncs == 1 && b.syncs == 1);
        reset(up);
    }

    // 6. Thread isolation: a scope on this thread does not fence another
    //    thread's frees.
    {
        FakeStream s;
        CuvsHostFreeFence::Scope scope(&s, fake_sync);
        int other_returned = -1;
        std::thread t([&] { other_returned = CuvsHostFreeFence::before_free(); });
        t.join();
        CHECK(other_returned == 0);
        CHECK(s.syncs == 0);
        CHECK(CuvsHostFreeFence::before_free() == 1);
        CHECK(s.syncs == 1);
        reset(up);
    }

    // 7. The default stream (nullptr handle) is still fenced.
    {
        static int null_syncs;
        null_syncs = 0;
        auto sync_null = [](void *stream) -> int { if (stream == nullptr) null_syncs++; return 0; };
        {
            CuvsHostFreeFence::Scope scope(nullptr, sync_null);
            HostVector v(&fenced, 8);
        }
        CHECK(null_syncs == 1);
        reset(up);
    }

    if (failures) {
        std::fprintf(stderr, "test_host_free_fence: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_host_free_fence: OK\n");
    return 0;
}
