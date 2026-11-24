#include "cuvs_page_overlay.h"

#include <stdlib.h>
#include <string.h>

CuvsPageOverlayChunk *
cuvs_page_overlay_copy(const CuvsPageOverlayRequest *request, const void *payload, size_t bytes)
{
    CuvsPageOverlayChunk *chunk = malloc(sizeof(*chunk) + bytes);
    uint32_t vectors = 0;

    if (!chunk)
        return NULL;
    chunk->next = NULL;
    chunk->from = request->from;
    chunk->positions = request->positions;
    memcpy(chunk->marks, payload, bytes);
    for (uint32_t i = 0; i < chunk->positions; ++i)
    {
        const CuvsPagePosition *mark = &chunk->marks[i];

        if (mark->state == CUVS_PAGE_POSITION_VECTOR &&
            mark->block != 0 && mark->block != UINT32_MAX)
            ++vectors;
        else if (mark->state != CUVS_PAGE_POSITION_DEAD)
            goto invalid;
    }
    if (vectors == request->vector_rows)
        return chunk;

invalid:
    free(chunk);
    return NULL;
}

void
cuvs_page_overlay_free(CuvsPageOverlayChunk *chunk)
{
    while (chunk)
    {
        CuvsPageOverlayChunk *next = chunk->next;

        free(chunk);
        chunk = next;
    }
}

uint32_t
cuvs_page_overlay_supply(CuvsPageOverlay *overlay, const CuvsPageOverlayRequest *request,
                         CuvsPageOverlayChunk **chunk, CuvsPageOverlayChunk **obsolete)
{
    if (request->boot_id != overlay->boot_id)
    {
        *obsolete = overlay->head;
        *overlay = (CuvsPageOverlay) {
            .boot_id = request->boot_id, .covered_upto = request->base.folded,
        };
    }
    if (request->base.tail - request->base.folded > CUVS_PAGE_OVERLAY_MAX)
        return CUVS_STATUS_PAGE_OVERLAY_FULL;
    if (request->base.tail <= overlay->covered_upto)
        return CUVS_STATUS_OK;
    if (request->from != overlay->covered_upto)
        return CUVS_STATUS_PAGE_NEED;

    if (overlay->tail)
        overlay->tail->next = *chunk;
    else
        overlay->head = *chunk;
    overlay->tail = *chunk;
    overlay->covered_upto = request->base.tail;
    *chunk = NULL;
    return CUVS_STATUS_OK;
}
