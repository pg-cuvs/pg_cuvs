#include "cuvs_page_overlay.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

CuvsPageOverlayChunk *
cuvs_page_overlay_copy(const CuvsPageOverlayRequest *request, const void *payload, size_t bytes, bool shared)
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
        else if (mark->state != CUVS_PAGE_POSITION_DEAD &&
                 (shared || mark->state != CUVS_PAGE_POSITION_INVISIBLE))
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

CuvsPageOverlayChunk *
cuvs_page_overlay_start_boot(CuvsPageOverlay *overlay, uint64_t boot_id, uint64_t folded)
{
    CuvsPageOverlayChunk *previous;

    if (boot_id == overlay->boot_id)
        return NULL;
    previous = overlay->head;
    *overlay = (CuvsPageOverlay) {.boot_id = boot_id, .covered_upto = folded};
    return previous;
}

uint32_t
cuvs_page_overlay_supply(CuvsPageOverlay *overlay, const CuvsPageOverlayRequest *request,
                         CuvsPageOverlayChunk **chunk, CuvsPageOverlayChunk **obsolete)
{
    *obsolete = cuvs_page_overlay_start_boot(overlay, request->boot_id, request->base.folded);
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

static float
page_distance(const float *query, const float *value, uint32_t dim, uint32_t metric)
{
    double dot = 0, qnorm = 0, vnorm = 0, l2 = 0;

    if (metric == CUVS_METRIC_L2)
    {
        for (uint32_t i = 0; i < dim; ++i)
        {
            double difference = (double) query[i] - value[i];

            l2 += difference * difference;
        }
        return (float) l2;
    }
    for (uint32_t i = 0; i < dim; ++i)
    {
        double q = query[i], v = value[i];

        dot += q * v;
        if (metric == CUVS_METRIC_COSINE)
        {
            qnorm += q * q;
            vnorm += v * v;
        }
    }
    if (metric == CUVS_METRIC_IP)
        return (float) dot;
    return qnorm > 0 && vnorm > 0 ? (float) (1 - dot / sqrt(qnorm * vnorm)) : 1;
}

static void
page_add_result(CuvsPageOverlayResult *results, int *count, int k,
                CuvsPageOverlayResult candidate, uint32_t metric)
{
    int lo = 0, hi = *count;

    if (!isfinite(candidate.distance))
        return;
    while (lo < hi)
    {
        int mid = lo + (hi - lo) / 2;
        float previous = results[mid].distance;
        bool better = metric == CUVS_METRIC_IP ?
            candidate.distance > previous : candidate.distance < previous;

        if (better || (candidate.distance == previous && candidate.slot < results[mid].slot))
            hi = mid;
        else
            lo = mid + 1;
    }
    if (lo == k)
        return;
    if (*count < k)
        ++*count;
    memmove(results + lo + 1, results + lo, (size_t) (*count - lo - 1) * sizeof(*results));
    results[lo] = candidate;
}

void
cuvs_page_overlay_score(const CuvsPageOverlayChunk *chunk, uint64_t upto,
                   const CuvsPageSearchRequest *request, const float *query,
                   const CuvsCmdFrame *cmd, CuvsPageOverlayResult *results, int *count)
{
    const float *vector;

    if (!chunk || chunk->from >= upto)
        return;
    vector = (const float *) (chunk->marks + chunk->positions);
    for (uint32_t i = 0; i < chunk->positions && chunk->from + i < upto; ++i)
    {
        CuvsPagePosition mark = chunk->marks[i];

        if (mark.state == CUVS_PAGE_POSITION_VECTOR)
        {
            CuvsPageOverlayResult candidate = {
                .slot = request->n_slots + chunk->from + i - request->folded,
                .distance = page_distance(query, vector, cmd->dim, cmd->metric),
                .block = mark.block,
            };

            page_add_result(results, count, cmd->k, candidate, cmd->metric);
            vector += cmd->dim;
        }
    }
}
