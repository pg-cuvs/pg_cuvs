/* DEV-only native test: real RMM limiter/pool, no ANN workload. */
#include "cuvs_rmm_failure_resource.hpp"
#include "cuvs_admission_contract.h"
#include <rmm/mr/cuda_memory_resource.hpp>
#include <rmm/mr/pool_memory_resource.hpp>
#include <cassert>
#include <cstdio>

int main()
{
    namespace failure = cuvs_failed_allocation;
    rmm::mr::cuda_memory_resource device;
    rmm::cuda_stream_view stream{};
    {
        CuvsRmmFailureUpstream limited(&device,1024);
        failure::clear();
        try { limited.allocate(stream,4096); assert(false); }
        catch (const rmm::out_of_memory &) {}
        assert(failure::take()==4096 && limited.get_allocated_bytes()==0);
    }
    {
        CuvsRmmFailureUpstream limited(&device,6000);
        rmm::mr::pool_memory_resource<CuvsRmmFailureUpstream> pool(&limited,4096);
        auto *a = failure::outer([&]{return pool.allocate(stream,3000);});
        auto *b = failure::outer([&]{return pool.allocate(stream,1500);});
        assert(limited.get_allocated_bytes()==5632);
        assert(failure::take()==-1); // smaller successful backoff must clear X
        int64_t failed = -1;
        try {
            failure::outer([&]{return pool.allocate(stream,3000);});
            assert(false);
        } catch (const rmm::out_of_memory &) {
            failed = failure::take_for_current_exception();
        }
        assert(failed==3072); // upstream-aligned request, not logical 3000
        assert(limited.get_allocated_bytes()==5632);
        pool.deallocate(stream,b,1500);
        pool.deallocate(stream,a,3000);
    }
#ifdef PGCUVS_BETA3A_GPU_FOLD
    {
        constexpr std::size_t mib = 1u << 20;
        /* Start with 80 MiB outside the modelled pool. Real RMM growth is
         * measured below; 80 MiB is a deterministic raw-free fixture. */
        CuvsRmmFailureUpstream limited(&device, 96 * mib);
        rmm::mr::pool_memory_resource<CuvsRmmFailureUpstream> pool(&limited, 32 * mib);
        auto raw = [&] { return 80 * mib - limited.get_allocated_bytes(); };
        auto *resident = pool.allocate(stream, 31 * mib);
        assert(raw() == 48 * mib);
        {
            CuvsRmmFailureUpstream::NoGrowth fold(&limited);
            auto *idle = pool.allocate(stream, mib);
            try { pool.allocate(stream, 8 * mib); assert(false); }
            catch (const rmm::out_of_memory &) {}
            pool.deallocate(stream, idle, mib);
            assert(raw() == 48 * mib && !cuvs_build_physical_refuse(raw()));
        }
        /* Without the fold guard the next allocation expands upstream by
         * 32 MiB. Freeing the live allocation does not return that backing. */
        auto *grown = pool.allocate(stream, 8 * mib);
        pool.deallocate(stream, grown, 8 * mib);
        assert(raw() == 16 * mib && cuvs_build_physical_refuse(raw()));
        pool.deallocate(stream, resident, 31 * mib);
        std::puts("PASS RMM fold can reuse backing, cannot grow it; ungated backing persists after free");
    }
#endif
    std::puts("PASS real RMM quota X=4096, successful backoff clears X, final failed X=3072 != logical 3000");
}
