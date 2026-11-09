// Q6 (run 20261109-q-03): a RAFT host buffer freed while a request's stream
// still has a queued write into it (cuVS memzero -> cudaMemsetAsync on a
// malloc'd host_vector under HMM, then an OOM unwind) must not reach the
// allocator before that write lands. The fake stream below queues writes and
// runs them on sync; the fake upstream keeps freed blocks and records any
// queued write that lands on one ("late write", the observed corruption).
// The unfenced case reproduces the late write, so the fenced checks are not
// vacuous. Review A2 B1: a wait that reports an error must not free the
// block; it is quarantined, the error goes to the failure hook and the
// counters, and the scope records it for its owner.
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
    std::vector<void *> to_release;   // every allocation (quarantined ones too)
    int frees = 0;
    void *do_allocate(std::size_t bytes, std::size_t) override
    {
        void *p = ::operator new(bytes);
        std::memset(p, 0x5A, bytes);
        events.push_back("alloc");
        to_release.push_back(p);
        return p;
    }
    void do_deallocate(void *p, std::size_t, std::size_t) override
    {
        frees++;
        freed.insert(p);
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

// A wait that fails (e.g. a sticky CUDA error, 700) without draining the stream.
static int failing_sync(void *stream)
{
    static_cast<FakeStream *>(stream)->syncs++;
    events.push_back("sync-fail");
    return 700;
}

struct HookCall { int err; void *stream; int device; std::size_t bytes; };
static std::vector<HookCall> hook_calls;
static void record_hook(int err, void *stream, int device, std::size_t bytes)
{
    hook_calls.push_back({err, stream, device, bytes});
}

// A stream that fails its first wait and then recovers.
static int fail_once_sync(void *stream)
{
    auto *s = static_cast<FakeStream *>(stream);
    if (s->syncs++ == 0) return 1;
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
    hook_calls.clear();
}

struct StatsSnap { uint64_t f, b, by; };
static StatsSnap snap()
{
    auto &st = CuvsHostFreeFence::stats();
    return {st.sync_failures.load(), st.quarantined_blocks.load(), st.quarantined_bytes.load()};
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
        CHECK(CuvsHostFreeFence::before_free(0) == CuvsHostFreeFence::NOT_FENCED);
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
        std::thread t([&] { other_returned = CuvsHostFreeFence::before_free(0); });
        t.join();
        CHECK(other_returned == 0);
        CHECK(s.syncs == 0);
        CHECK(CuvsHostFreeFence::before_free(0) == CuvsHostFreeFence::WAITED);
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

    // 8. B1: a failed wait during the OOM unwind quarantines the block (not
    //    freed upstream), so the queued write that lands later hits memory
    //    still owned; the hook gets the error, stream, device and size; the
    //    counters and the scope record the failure. No exception escapes
    //    besides the build's own bad_alloc.
    {
        FakeStream s;
        StatsSnap before = snap();
        unsigned long scope_failures = 0;
        int scope_err = 0;
        bool only_bad_alloc = false;
        {
            CuvsHostFreeFence::Scope scope(&s, failing_sync, 5, record_hook);
            try { build_that_ooms(&fenced, s); } catch (const std::bad_alloc &) { only_bad_alloc = true; }
            scope_failures = scope.failures();
            scope_err = scope.last_error();
        }
        gpu_finishes(s);   // the GPU write lands after the failed wait
        StatsSnap after = snap();
        CHECK(only_bad_alloc);
        CHECK(up.frees == 0);
        CHECK(late_writes == 0);
        CHECK(scope_failures == 1 && scope_err == 700);
        CHECK(hook_calls.size() == 1);
        CHECK(hook_calls.size() == 1 && hook_calls[0].err == 700 && hook_calls[0].stream == &s &&
              hook_calls[0].device == 5 && hook_calls[0].bytes == 4000);
        CHECK(after.f - before.f == 1 && after.b - before.b == 1 && after.by - before.by == 4000);
        reset(up);
    }

    // 9. A later successful wait in the same scope frees normally; only the
    //    failed block stays quarantined and the scope keeps the failure.
    {
        FakeStream s;
        StatsSnap before = snap();
        unsigned long scope_failures = 0;
        {
            CuvsHostFreeFence::Scope scope(&s, fail_once_sync, 0, record_hook);
            { HostVector v(&fenced, 16); queue_zero(s, v.p, v.n); }
            { HostVector v(&fenced, 32); queue_zero(s, v.p, v.n); }
            scope_failures = scope.failures();
        }
        gpu_finishes(s);
        StatsSnap after = snap();
        CHECK(up.frees == 1);
        CHECK(late_writes == 0);
        CHECK(scope_failures == 1);
        CHECK(after.b - before.b == 1 && after.by - before.by == 16);
        reset(up);
    }

    // 10. Nested scopes: the failure is recorded on the scope active when it
    //     happened; the enclosing scope stays clean and frees normally after.
    {
        FakeStream a, b;
        unsigned long outer_failures = 9, inner_failures = 0;
        {
            CuvsHostFreeFence::Scope outer(&a, fake_sync);
            {
                CuvsHostFreeFence::Scope inner(&b, failing_sync, 1, nullptr);   // no hook: still quarantined
                { HostVector v(&fenced, 8); }
                inner_failures = inner.failures();
            }
            { HostVector v(&fenced, 8); }
            outer_failures = outer.failures();
        }
        CHECK(inner_failures == 1);
        CHECK(outer_failures == 0);
        CHECK(up.frees == 1);
        reset(up);
    }

    if (failures) {
        std::fprintf(stderr, "test_host_free_fence: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_host_free_fence: OK\n");
    return 0;
}
