#ifndef CUVS_PAGE_OVERLAY_H
#define CUVS_PAGE_OVERLAY_H

#include <stdbool.h>

#include "cuvs_ipc.h"

typedef struct CuvsPageOverlayBudget
{
    size_t limit; /* immutable after daemon startup */
    size_t used;
} CuvsPageOverlayBudget;

typedef struct CuvsPageOverlayChunk
{
    /* An immutable link to the older prefix once this chunk is published. */
    struct CuvsPageOverlayChunk *next;
    struct CuvsPageOverlayChunk *retired_next;
    size_t refs;
    CuvsPageOverlayBudget *budget;
    size_t charged_bytes;
    uint64_t from;
    uint32_t positions;
    CuvsPagePosition marks[];
} CuvsPageOverlayChunk;

typedef struct CuvsPageOverlay
{
    uint64_t boot_id;
    uint64_t generation;
    uint64_t covered_upto;
    CuvsPageOverlayChunk *head;
} CuvsPageOverlay;

uint32_t cuvs_page_overlay_alloc(CuvsPageOverlayBudget *budget,
                                 const CuvsPageOverlayRequest *request, size_t bytes,
                                 CuvsPageOverlayChunk **result);
bool cuvs_page_overlay_valid(const CuvsPageOverlayChunk *chunk, uint32_t vector_rows, bool shared);
/* Retain while already owning a reference or while holding the registry lock.
 * Release can free a chain and must run outside the registry lock. */
void cuvs_page_overlay_retain(CuvsPageOverlayChunk *chunk);
void cuvs_page_overlay_release(CuvsPageOverlayChunk *chunk);
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
