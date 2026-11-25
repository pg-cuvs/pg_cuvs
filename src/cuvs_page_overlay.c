#include "cuvs_page_overlay_gpu.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct PageGpuFooter
{
    CuvsPageOverlayGpuMirror *mirror;
    max_align_t storage[];
} PageGpuFooter;

static size_t page_gpu_footer_bytes;
static CuvsPageOverlayGpuMirror page_gpu_claimed;

void
cuvs_page_overlay_enable_gpu_cache(size_t mirror_bytes)
{
    /* Startup passes the compile-time size of the native mirror. */
    size_t align = _Alignof(PageGpuFooter);

    page_gpu_footer_bytes = sizeof(PageGpuFooter) + mirror_bytes +
        ((align - mirror_bytes % align) % align);
}

static PageGpuFooter *
page_gpu_footer(const CuvsPageOverlayChunk *chunk)
{
    return (PageGpuFooter *) ((char *) chunk + chunk->charged_bytes - page_gpu_footer_bytes);
}

CuvsPageOverlayGpuMirror *
cuvs_page_overlay_gpu_mirror(const CuvsPageOverlayChunk *chunk)
{
    if (!page_gpu_footer_bytes)
        return NULL;
    CuvsPageOverlayGpuMirror *mirror = __atomic_load_n(&page_gpu_footer(chunk)->mirror, __ATOMIC_ACQUIRE);

    return mirror == &page_gpu_claimed ? NULL : mirror;
}

void *
cuvs_page_overlay_claim_gpu_mirror(const CuvsPageOverlayChunk *chunk)
{
    CuvsPageOverlayGpuMirror *expected = NULL;

    if (!page_gpu_footer_bytes)
        return NULL;
    PageGpuFooter *footer = page_gpu_footer(chunk);

    if (!__atomic_compare_exchange_n(&footer->mirror, &expected, &page_gpu_claimed,
                                      false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return NULL;
    return footer->storage;
}

void
cuvs_page_overlay_publish_gpu_mirror(const CuvsPageOverlayChunk *chunk, CuvsPageOverlayGpuMirror *mirror)
{
    __atomic_store_n(&page_gpu_footer(chunk)->mirror, mirror, __ATOMIC_RELEASE);
}

bool
cuvs_page_overlay_reserve(CuvsPageOverlayBudget *budget, size_t bytes)
{
    size_t used = __atomic_load_n(&budget->used, __ATOMIC_RELAXED);

    do
    {
        if (used > budget->limit || bytes > budget->limit - used)
            return false;
    } while (!__atomic_compare_exchange_n(&budget->used, &used, used + bytes, true,
                                          __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    return true;
}

void
cuvs_page_overlay_unreserve(CuvsPageOverlayBudget *budget, size_t bytes)
{
    __atomic_fetch_sub(&budget->used, bytes, __ATOMIC_RELAXED);
}

bool
cuvs_page_overlay_allocation_bytes(size_t bytes, size_t *allocation_bytes)
{
    if (bytes > SIZE_MAX - sizeof(CuvsPageOverlayChunk))
        return false;
    size_t charge = sizeof(CuvsPageOverlayChunk) + bytes;
    if (page_gpu_footer_bytes)
    {
        size_t align = _Alignof(PageGpuFooter);
        size_t tail = (align - charge % align) % align;

        if (tail > SIZE_MAX - charge || page_gpu_footer_bytes > SIZE_MAX - charge - tail)
            return false;
        charge += tail + page_gpu_footer_bytes;
    }
    *allocation_bytes = charge;
    return true;
}

uint32_t
cuvs_page_overlay_alloc(CuvsPageOverlayBudget *budget, const CuvsPageOverlayRequest *request,
                       size_t bytes, CuvsPageOverlayChunk **result)
{
    CuvsPageOverlayChunk *chunk;
    size_t charge;

    *result = NULL;
    if (!cuvs_page_overlay_allocation_bytes(bytes, &charge) || !cuvs_page_overlay_reserve(budget, charge))
        return CUVS_STATUS_PAGE_OVERLAY_HOST_FULL;

    chunk = malloc(charge);
    if (!chunk)
    {
        cuvs_page_overlay_unreserve(budget, charge);
        return CUVS_STATUS_OOM_FALLBACK;
    }
    chunk->next = NULL;
    chunk->retired_next = NULL;
    chunk->deferred_releases = 0;
    chunk->refs = 1;
    chunk->budget = budget;
    chunk->charged_bytes = charge;
    chunk->from = request->from;
    chunk->positions = request->positions;
    if (page_gpu_footer_bytes)
        page_gpu_footer(chunk)->mirror = NULL;
    *result = chunk;
    return CUVS_STATUS_OK;
}

bool
cuvs_page_overlay_valid(const CuvsPageOverlayChunk *chunk, uint32_t vector_rows, bool shared)
{
    uint32_t vectors = 0;

    for (uint32_t i = 0; i < chunk->positions; ++i)
    {
        const CuvsPagePosition *mark = &chunk->marks[i];

        if (mark->state == CUVS_PAGE_POSITION_VECTOR &&
            mark->block != 0 && mark->block != UINT32_MAX)
            ++vectors;
        else if (mark->state != CUVS_PAGE_POSITION_DEAD &&
                 (shared || mark->state != CUVS_PAGE_POSITION_INVISIBLE))
            return false;
    }
    return vectors == vector_rows;
}

void
cuvs_page_overlay_retain(CuvsPageOverlayChunk *chunk)
{
    if (chunk)
        __atomic_fetch_add(&chunk->refs, 1, __ATOMIC_RELAXED);
}

void
cuvs_page_overlay_release(CuvsPageOverlayChunk *chunk)
{
    while (chunk && __atomic_fetch_sub(&chunk->refs, 1, __ATOMIC_ACQ_REL) == 1)
    {
        CuvsPageOverlayChunk *next = chunk->next;
        CuvsPageOverlayBudget *budget = chunk->budget;
        size_t charge = chunk->charged_bytes;

        CuvsPageOverlayGpuMirror *mirror = cuvs_page_overlay_gpu_mirror(chunk);

        if (mirror)
            mirror->release(mirror);
        free(chunk);
        cuvs_page_overlay_unreserve(budget, charge);
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
    *overlay = (CuvsPageOverlay) {.boot_id = boot_id, .generation = overlay->generation + 1,
                                  .covered_upto = folded};
    return previous;
}

static CuvsPageOverlayChunk *
page_overlay_advance_fold(CuvsPageOverlay *overlay, uint64_t folded, bool defer_pinned)
{
    CuvsPageOverlayChunk **link = &overlay->head;
    CuvsPageOverlayChunk *obsolete;
    bool reader_can_reach_cut = false;

    /* Keep the unpublished suffix and at most one chunk crossing its start.
     * Each registry link owns a reference, including the head. */
    while (*link && (*link)->from + (*link)->positions > folded)
    {
        if (__atomic_load_n(&(*link)->refs, __ATOMIC_ACQUIRE) > 1)
        {
            /* This retained chunk can still reach the link we would cut. */
            if (defer_pinned)
                return NULL;
            reader_can_reach_cut = true;
        }
        link = &(*link)->next;
    }
    if (!*link)
        return NULL;

    if (reader_can_reach_cut)
    {
        /* An older search can still traverse this link outside the lock.
         * Detach the registry's view and let PostgreSQL supply the suffix. */
        link = &overlay->head;
        overlay->covered_upto = folded;
        ++overlay->generation;
    }
    obsolete = *link;
    *link = NULL;
    return obsolete;
}

CuvsPageOverlayChunk *
cuvs_page_overlay_advance_fold(CuvsPageOverlay *overlay, uint64_t folded)
{
    return page_overlay_advance_fold(overlay, folded, false);
}

CuvsPageOverlayChunk *
cuvs_page_overlay_prune_folded(CuvsPageOverlay *overlay, uint64_t folded)
{
    return page_overlay_advance_fold(overlay, folded, true);
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
    if (!*chunk)
        return CUVS_STATUS_OK; /* preflight before reserving and reading a contiguous chunk */

    /* Move the registry's prefix reference into the new chunk. A reader
     * that retained the former head can still walk exactly its own prefix. */
    (*chunk)->next = overlay->head;
    overlay->head = *chunk;
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

static void
page_overlay_score_range(const CuvsPageOverlayChunk *chunk, uint64_t from, uint64_t upto,
                         const CuvsPageSearchRequest *request, const float *query,
                         const CuvsCmdFrame *cmd, CuvsPageOverlayResult *results, int *count,
                         bool ranked)
{
    uint64_t first, upto_offset;
    uint32_t end, skipped_rows = 0;
    const float *vector;

    if (!chunk || from >= upto || chunk->from >= upto)
        return;
    first = from > chunk->from ? from - chunk->from : 0;
    if (first >= chunk->positions)
        return;
    upto_offset = upto - chunk->from;
    end = upto_offset < chunk->positions ? (uint32_t) upto_offset : chunk->positions;

    /* Only VECTOR marks have rows in the packed payload. */
    for (uint32_t i = 0; i < first; ++i)
        skipped_rows += chunk->marks[i].state == CUVS_PAGE_POSITION_VECTOR;
    vector = (const float *) (chunk->marks + chunk->positions) + (size_t) skipped_rows * cmd->dim;
    for (uint32_t i = (uint32_t) first; i < end; ++i)
    {
        CuvsPagePosition mark = chunk->marks[i];

        if (mark.state == CUVS_PAGE_POSITION_VECTOR)
        {
            CuvsPageOverlayResult candidate = {
                .slot = request->n_slots + chunk->from + i - request->folded,
                .distance = page_distance(query, vector, cmd->dim, cmd->metric),
                .block = mark.block,
            };

            if (ranked)
                page_add_result(results, count, cmd->k, candidate, cmd->metric);
            else if (isfinite(candidate.distance))
                results[(*count)++] = candidate;
            vector += cmd->dim;
        }
    }
}

void
cuvs_page_overlay_score(const CuvsPageOverlayChunk *chunk, uint64_t upto,
                        const CuvsPageSearchRequest *request, const float *query,
                        const CuvsCmdFrame *cmd, CuvsPageOverlayResult *results, int *count)
{
    page_overlay_score_range(chunk, 0, upto, request, query, cmd, results, count, true);
}

void
cuvs_page_overlay_score_range(const CuvsPageOverlayChunk *chunk, uint64_t from, uint64_t upto,
                              const CuvsPageSearchRequest *request, const float *query,
                              const CuvsCmdFrame *cmd, CuvsPageOverlayResult *results, int *count)
{
    if (from < request->folded)
        from = request->folded;
    page_overlay_score_range(chunk, from, upto, request, query, cmd, results, count, true);
}

void
cuvs_page_overlay_collect_range(const CuvsPageOverlayChunk *chunk, uint64_t from, uint64_t upto,
                               const CuvsPageSearchRequest *request, const float *query,
                               const CuvsCmdFrame *cmd, CuvsPageOverlayResult *results, int *count)
{
    if (from < request->folded)
        from = request->folded;
    page_overlay_score_range(chunk, from, upto, request, query, cmd, results, count, false);
}

void
cuvs_page_overlay_append_scores(const CuvsPageOverlayChunk *chunk,
                                uint32_t first, uint32_t end, uint32_t skip,
                                const CuvsPageSearchRequest *request, const float *query,
                                const CuvsCmdFrame *cmd, const float *scores,
                                CuvsPageOverlayResult *results, int *count)
{
    const float *vector = (const float *) (chunk->marks + chunk->positions) + (size_t) skip * cmd->dim;

    for (uint32_t i = first; i < end; ++i)
    {
        const CuvsPagePosition mark = chunk->marks[i];

        if (mark.state != CUVS_PAGE_POSITION_VECTOR)
            continue;
        float distance = *scores++;

        if (!isfinite(distance))
            distance = page_distance(query, vector, cmd->dim, cmd->metric);
        if (isfinite(distance))
            results[(*count)++] = (CuvsPageOverlayResult) {
                .slot = request->n_slots + chunk->from + i - request->folded,
                .block = mark.block,
                .distance = distance,
            };
        vector += cmd->dim;
    }
}
