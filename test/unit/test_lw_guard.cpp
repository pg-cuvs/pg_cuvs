// I4 / CK-28 (fake resource): the large_workspace guard installs only under
// policy managed, restores on normal, exception and OOM exits, and does so
// before the borrowed device_resources returns to the free list. The fake
// below mirrors cuvs_cagra_build_multi's declaration order exactly:
//   PooledRes _pr; Ops ops{_pr}; try { Guard g(ops, policy); ... } catch ...
// CK-34 shape for the lock-free lw_peak counter (8 threads).
#include <atomic>
#include <cstdio>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "cuvs_lw_guard.hpp"

static int failures;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static std::vector<std::string> events;
static bool resource_has_managed;   // state of the borrowed resource

struct FakePooledRes {
    bool poisoned = false;
    ~FakePooledRes()
    {
        // A pooled resource must come back with the default large_workspace.
        events.push_back(poisoned ? "destroy" : (resource_has_managed ? "pool:dirty" : "pool:clean"));
    }
    void poison() { poisoned = true; }
};

struct FakeOps {
    FakePooledRes &pr;
    bool fail_install = false;
    void install()
    {
        if (fail_install) throw std::runtime_error("install");
        resource_has_managed = true;
        events.push_back("install");
    }
    void restore() noexcept
    {
        resource_has_managed = false;
        events.push_back("restore");
    }
};

enum Mode { OK, THROW, OOM, INSTALL_FAILS };

static void
build(uint32_t policy, Mode mode)
{
    FakePooledRes pr;
    FakeOps ops{pr};
    ops.fail_install = mode == INSTALL_FAILS;
    try {
        CuvsLwWorkspaceGuard<FakeOps> guard(ops, policy);
        if (mode == THROW) throw std::runtime_error("cuvs");
        if (mode == OOM) throw std::bad_alloc();
        events.push_back(resource_has_managed ? "build:managed" : "build:default");
    } catch (const std::bad_alloc &) {
        events.push_back(resource_has_managed ? "catch_oom:dirty" : "catch_oom");
        pr.poison();
    } catch (const std::exception &) {
        events.push_back(resource_has_managed ? "catch:dirty" : "catch");
        pr.poison();
    }
}

static bool
seq(std::initializer_list<const char *> want)
{
    std::vector<std::string> w(want.begin(), want.end());
    bool ok = events == w;
    if (!ok) {
        std::fprintf(stderr, "  events:");
        for (auto &e : events) std::fprintf(stderr, " %s", e.c_str());
        std::fprintf(stderr, "\n");
    }
    events.clear();
    resource_has_managed = false;
    return ok;
}

static void
test_guard(void)
{
    build(CUVS_LW_OVERFLOW_MANAGED, OK);
    CHECK(seq({"install", "build:managed", "restore", "pool:clean"}));
    build(CUVS_LW_OVERFLOW_MANAGED, THROW);
    CHECK(seq({"install", "restore", "catch", "destroy"}));
    build(CUVS_LW_OVERFLOW_MANAGED, OOM);
    CHECK(seq({"install", "restore", "catch_oom", "destroy"}));
    // Policy error: zero install/restore calls on every exit.
    build(CUVS_LW_OVERFLOW_ERROR, OK);
    CHECK(seq({"build:default", "pool:clean"}));
    build(CUVS_LW_OVERFLOW_ERROR, OOM);
    CHECK(seq({"catch_oom", "destroy"}));
    // Unknown policy values are not managed.
    build(7u, OK);
    CHECK(seq({"build:default", "pool:clean"}));
    // A failed install installed nothing, so nothing is restored.
    build(CUVS_LW_OVERFLOW_MANAGED, INSTALL_FAILS);
    CHECK(seq({"catch", "destroy"}));
}

static void
test_counter(void)
{
    CuvsLwCounter c;
    CHECK(c.on_alloc(100));
    CHECK(c.on_alloc(50));
    c.on_free(100);
    CHECK(!c.on_alloc(80));       // live 130 < peak 150
    CHECK(c.peak.load() == 150 && c.live.load() == 130);
    CHECK(c.on_alloc(21));        // live 151 raises the peak
    c.note_free_at_peak(4096);
    CHECK(c.peak.load() == 151 && c.free_at_peak.load() == 4096);

    CuvsLwCounter shared;
    std::atomic<uint64_t> max_seen{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; t++)
        threads.emplace_back([&shared, &max_seen, t] {
            for (int i = 0; i < 20000; i++) {
                uint64_t bytes = 256u * (uint64_t)(1 + (i + t) % 7);
                shared.on_alloc(bytes);
                // Any live value observed at any instant must end up <= peak.
                uint64_t live = shared.live.load();
                uint64_t m = max_seen.load();
                while (live > m && !max_seen.compare_exchange_weak(m, live)) {}
                shared.on_free(bytes);
            }
        });
    for (auto &th : threads) th.join();
    CHECK(max_seen.load() > 0);
    CHECK(shared.live.load() == 0);
    CHECK(shared.peak.load() >= max_seen.load());
    CHECK(shared.peak.load() <= 8u * 256u * 7u);
}

int
main()
{
    test_guard();
    test_counter();
    if (failures) {
        std::fprintf(stderr, "test_lw_guard: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("PASS lw guard: managed install/restore before free-list return on normal/"
                "exception/OOM exits, zero calls under policy error, failed install, "
                "8-thread lock-free peak counter\n");
    return 0;
}
