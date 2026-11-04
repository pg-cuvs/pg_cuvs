#ifndef CUVS_RMM_FAILURE_RESOURCE_HPP
#define CUVS_RMM_FAILURE_RESOURCE_HPP

#include "cuvs_failed_allocation.hpp"
#include <rmm/mr/limiting_resource_adaptor.hpp>

/* Observe the pool's actual upstream request, including its final backoff,
 * around the existing limiter. Preserve its quota/accounting and exceptions. */
class CuvsRmmFailureUpstream final : public rmm::mr::device_memory_resource {
    using Limiter = rmm::mr::limiting_resource_adaptor<rmm::mr::device_memory_resource>;
    Limiter limiter_;

  public:
    CuvsRmmFailureUpstream(rmm::mr::device_memory_resource *upstream, std::size_t limit)
        : limiter_(upstream, limit) {}
    std::size_t get_allocated_bytes() const { return limiter_.get_allocated_bytes(); }
    std::size_t get_allocation_limit() const { return limiter_.get_allocation_limit(); }

  private:
    void *do_allocate(std::size_t bytes, rmm::cuda_stream_view stream) override
    {
        return cuvs_failed_allocation::capture(bytes, [&] {
            return limiter_.allocate(stream, bytes);
        });
    }
    void do_deallocate(void *p, std::size_t bytes, rmm::cuda_stream_view stream) noexcept override
    {
        limiter_.deallocate(stream, p, bytes);
    }
    bool do_is_equal(rmm::mr::device_memory_resource const &other) const noexcept override
    {
        auto const *rhs = dynamic_cast<CuvsRmmFailureUpstream const *>(&other);
        return rhs && limiter_.get_upstream_resource() == rhs->limiter_.get_upstream_resource();
    }
};
#endif
