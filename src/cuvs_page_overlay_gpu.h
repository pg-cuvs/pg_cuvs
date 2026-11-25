#ifndef CUVS_PAGE_OVERLAY_GPU_H
#define CUVS_PAGE_OVERLAY_GPU_H

#ifdef __cplusplus
extern "C" {
#endif

#include "cuvs_page_overlay.h"

typedef struct CuvsPageOverlayGpuMirror
{
    void (*release)(struct CuvsPageOverlayGpuMirror *mirror);
} CuvsPageOverlayGpuMirror;

/* Startup only. The native workspace provides the mirror's storage size.
 * Chunk allocation charges for the aligned footer and storage together. */
void cuvs_page_overlay_gpu_init(void);
void cuvs_page_overlay_enable_gpu_cache(size_t mirror_bytes);
bool cuvs_page_overlay_allocation_bytes(size_t payload_bytes, size_t *allocation_bytes);
CuvsPageOverlayGpuMirror *cuvs_page_overlay_gpu_mirror(const CuvsPageOverlayChunk *chunk);
void *cuvs_page_overlay_claim_gpu_mirror(const CuvsPageOverlayChunk *chunk);
/* NULL relinquishes a claim whose construction or upload failed. */
void cuvs_page_overlay_publish_gpu_mirror(const CuvsPageOverlayChunk *chunk,
                                          CuvsPageOverlayGpuMirror *mirror);

typedef struct CuvsPageOverlayGpuAdmission
{
    bool (*reserve)(int device, size_t bytes);
    void (*finish)(int device, size_t bytes, bool resident);
    void (*release)(int device, size_t bytes);
} CuvsPageOverlayGpuAdmission;

/* Append the resident prefix in chunk order and return the number of rows
 * sent to CUDA. Zero leaves results and count untouched for a CPU retry. */
uint32_t cuvs_page_overlay_gpu_collect(int device, uint32_t min_rows,
                                    const CuvsPageOverlayGpuAdmission *admission,
                                    CuvsPageOverlayBudget *host_budget,
                                    const CuvsPageOverlayChunk *snapshot, uint64_t upto,
                                    const CuvsPageSearchRequest *request, const float *query,
                                    const CuvsCmdFrame *cmd, CuvsPageOverlayResult *results,
                                    int *count);

/* Try to surrender the device cache and scratch for fold admission. Call
 * outside the registry lock. A live scoring workspace is left intact. */
bool cuvs_page_overlay_gpu_reclaim(int device);

/* The packed score array corresponds to VECTOR marks in [first, end).
 * Uncertain scores are resolved with the existing ordered CPU calculation. */
void cuvs_page_overlay_append_scores(const CuvsPageOverlayChunk *chunk,
                                      uint32_t first, uint32_t end, uint32_t skip,
                                      const CuvsPageSearchRequest *request, const float *query,
                                      const CuvsCmdFrame *cmd, const float *scores,
                                      CuvsPageOverlayResult *results, int *count);

#ifdef __cplusplus
}
#endif

#endif
