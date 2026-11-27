#ifndef CUVS_PAGE_OVERLAY_GPU_RMM_HPP
#define CUVS_PAGE_OVERLAY_GPU_RMM_HPP

#include <cstddef>
#include <cuda_runtime_api.h>

/* The caller has selected device and owns stream for the allocation's life. */
void *cuvs_page_overlay_rmm_allocate(int device, std::size_t bytes, cudaStream_t stream) noexcept;
void cuvs_page_overlay_rmm_free(int device, void *data, std::size_t bytes, cudaStream_t stream) noexcept;

#endif
