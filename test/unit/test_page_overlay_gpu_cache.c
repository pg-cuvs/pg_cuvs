#include "cuvs_page_overlay_gpu.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static int released;

static void
release_fake(CuvsPageOverlayGpuMirror *mirror)
{
    assert(mirror != NULL);
    ++released;
}

static CuvsPageOverlayGpuMirror *
install_fake(CuvsPageOverlayChunk *chunk)
{
    CuvsPageOverlayGpuMirror *mirror = cuvs_page_overlay_claim_gpu_mirror(chunk);

    assert(mirror != NULL);
    assert(cuvs_page_overlay_gpu_mirror(chunk) == NULL);
    assert(cuvs_page_overlay_claim_gpu_mirror(chunk) == NULL);
    *mirror = (CuvsPageOverlayGpuMirror) {.release = release_fake};
    cuvs_page_overlay_publish_gpu_mirror(chunk, mirror);
    return mirror;
}

int
main(void)
{
    CuvsPageOverlayRequest supply = {.from = 20, .positions = 3, .vector_rows = 2};
    const size_t bytes = 3 * sizeof(CuvsPagePosition) + 2 * sizeof(float);
    const size_t legacy_charge = sizeof(CuvsPageOverlayChunk) + bytes;
    CuvsPageOverlayBudget budget = {.limit = legacy_charge};
    CuvsPageOverlayChunk *chunk = NULL;
    size_t estimated;

    assert(cuvs_page_overlay_allocation_bytes(bytes, &estimated) && estimated == legacy_charge);
    assert(cuvs_page_overlay_alloc(&budget, &supply, bytes, &chunk) == CUVS_STATUS_OK);
    assert(chunk->charged_bytes == legacy_charge && budget.used == legacy_charge);
    assert(!cuvs_page_overlay_claim_gpu_mirror(chunk));
    cuvs_page_overlay_release(chunk);
    assert(budget.used == 0 && released == 0);

    cuvs_page_overlay_enable_gpu_cache(sizeof(CuvsPageOverlayGpuMirror));
    assert(cuvs_page_overlay_allocation_bytes(bytes, &estimated) && estimated > legacy_charge);
    assert(!cuvs_page_overlay_allocation_bytes(SIZE_MAX, &estimated));
    assert(cuvs_page_overlay_alloc(&budget, &supply, bytes, &chunk) == CUVS_STATUS_PAGE_OVERLAY_HOST_FULL);
    budget.limit += 4 * sizeof(max_align_t);
    assert(cuvs_page_overlay_alloc(&budget, &supply, bytes, &chunk) == CUVS_STATUS_OK);
    assert(chunk->charged_bytes == estimated && budget.used == chunk->charged_bytes);
    assert(cuvs_page_overlay_gpu_mirror(chunk) == NULL);
    assert(cuvs_page_overlay_claim_gpu_mirror(chunk) != NULL);
    cuvs_page_overlay_publish_gpu_mirror(chunk, NULL);
    CuvsPageOverlayGpuMirror *first = install_fake(chunk);
    assert(!cuvs_page_overlay_claim_gpu_mirror(chunk));
    assert(cuvs_page_overlay_gpu_mirror(chunk) == first);

    chunk->marks[0] = (CuvsPagePosition) {.state = CUVS_PAGE_POSITION_VECTOR, .block = 71};
    chunk->marks[1] = (CuvsPagePosition) {.state = CUVS_PAGE_POSITION_DEAD};
    chunk->marks[2] = (CuvsPagePosition) {.state = CUVS_PAGE_POSITION_VECTOR, .block = 73};
    float *packed = (float *) (chunk->marks + chunk->positions);
    packed[0] = 1;
    packed[1] = 4;

    const CuvsPageSearchRequest search = {.n_slots = 10, .folded = 20, .tail = 23};
    const CuvsCmdFrame cmd = {.dim = 1, .metric = CUVS_METRIC_L2, .k = 1};
    const float query = 0;
    const float scores[] = {1, NAN};
    CuvsPageOverlayResult out[2];
    int count = 0;

    cuvs_page_overlay_append_scores(chunk, 0, 3, 0, &search, &query, &cmd, scores, out, &count);
    assert(count == 2 && out[0].slot == 10 && out[0].block == 71 && out[0].distance == 1);
    assert(out[1].slot == 12 && out[1].block == 73 && out[1].distance == 16);

    CuvsPageOverlay overlay = {.boot_id = 12, .head = chunk, .covered_upto = 23};
    cuvs_page_overlay_retain(chunk);
    CuvsPageOverlayChunk *retired = cuvs_page_overlay_start_boot(&overlay, 13, 20);

    assert(overlay.head == NULL && retired == chunk);
    cuvs_page_overlay_release(retired);
    assert(released == 0 && budget.used != 0);
    cuvs_page_overlay_release(chunk);
    assert(released == 1 && budget.used == 0);

    CuvsPageOverlayChunk *head;

    budget.limit *= 2;
    assert(cuvs_page_overlay_alloc(&budget, &supply, bytes, &chunk) == CUVS_STATUS_OK);
    supply.from = 23;
    assert(cuvs_page_overlay_alloc(&budget, &supply, bytes, &head) == CUVS_STATUS_OK);
    install_fake(chunk);
    head->next = chunk;
    overlay = (CuvsPageOverlay) {.boot_id = 13, .head = head, .covered_upto = 26};
    cuvs_page_overlay_retain(head);
    assert(cuvs_page_overlay_prune_folded(&overlay, 23) == NULL);
    assert(head->next == chunk && released == 1);
    cuvs_page_overlay_release(head);
    retired = cuvs_page_overlay_prune_folded(&overlay, 23);
    assert(retired == chunk && head->next == NULL);
    cuvs_page_overlay_release(retired);
    assert(released == 2 && budget.used == head->charged_bytes);
    cuvs_page_overlay_release(head);
    assert(budget.used == 0);
    puts("page overlay GPU cache: OFF budget, ownership, boot, fold and ordered fallback PASS");
    return 0;
}
