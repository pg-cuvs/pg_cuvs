#ifndef CUVS_PAGE_OVERLAY_H
#define CUVS_PAGE_OVERLAY_H

#include <stdbool.h>

#include "cuvs_ipc.h"

typedef struct CuvsPageOverlayChunk
{
    struct CuvsPageOverlayChunk *next;
    uint64_t from;
    uint32_t positions;
    CuvsPagePosition marks[];
} CuvsPageOverlayChunk;

typedef struct CuvsPageOverlay
{
    uint64_t boot_id;
    uint64_t covered_upto;
    CuvsPageOverlayChunk *head;
    CuvsPageOverlayChunk *tail;
} CuvsPageOverlay;

CuvsPageOverlayChunk *cuvs_page_overlay_copy(const CuvsPageOverlayRequest *request,
                                             const void *payload, size_t bytes, bool shared);
void cuvs_page_overlay_free(CuvsPageOverlayChunk *chunk);
/* These operations run under the graph registry lock. */
CuvsPageOverlayChunk *cuvs_page_overlay_start_boot(CuvsPageOverlay *overlay, uint64_t boot_id,
                                                   uint64_t folded);
void cuvs_page_overlay_score(const CuvsPageOverlayChunk *chunk, uint64_t upto,
                             const CuvsPageSearchRequest *base, const float *query,
                             const CuvsCmdFrame *cmd, CuvsPageOverlayResult *results, int *count);

/* Registry lock required. A new boot detaches the old list for disposal. */
uint32_t cuvs_page_overlay_supply(CuvsPageOverlay *overlay, const CuvsPageOverlayRequest *request,
                                 CuvsPageOverlayChunk **chunk, CuvsPageOverlayChunk **obsolete);

#endif
