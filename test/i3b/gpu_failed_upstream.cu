/* DEV-only native test: real RMM limiter/pool, a few KiB, no ANN workload. */
#include "cuvs_rmm_failure_resource.hpp"
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
    std::puts("PASS real RMM quota X=4096, successful backoff clears X, final failed X=3072 != logical 3000");
}
