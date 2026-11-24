#include "cuvs_page_overlay.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void
expect_supply(CuvsPageOverlayBudget *budget, CuvsPageOverlay *overlay, uint64_t boot,
              uint64_t from, uint32_t expected,
              uint64_t covered_upto)
{
    CuvsPageOverlayRequest request = {.base.tail = from + 2, .boot_id = boot,
                                     .from = from, .positions = 2};
    CuvsPagePosition marks[2] = {{0}};
    CuvsPageOverlayChunk *chunk;
    CuvsPageOverlayChunk *old = NULL;

    assert(cuvs_page_overlay_alloc(budget, &request, sizeof(marks), &chunk) == CUVS_STATUS_OK);
    memcpy(chunk->marks, marks, sizeof(marks));
    assert(cuvs_page_overlay_valid(chunk, 0, true));
    assert(cuvs_page_overlay_supply(overlay, &request, &chunk, &old) == expected);
    assert(overlay->covered_upto == covered_upto);
    cuvs_page_overlay_release(chunk);
    cuvs_page_overlay_release(old);
}

static void
check_budget(void)
{
    CuvsPageOverlayRequest request = {.positions = 1};
    const size_t charge = sizeof(CuvsPageOverlayChunk) + sizeof(CuvsPagePosition);
    CuvsPageOverlayBudget budget = {.limit = 2 * charge};
    CuvsPageOverlayChunk *retained, *incoming, *refused;

    assert(cuvs_page_overlay_alloc(&budget, &request, sizeof(CuvsPagePosition), &retained) ==
           CUVS_STATUS_OK);
    cuvs_page_overlay_retain(retained);
    assert(budget.used == charge);
    assert(cuvs_page_overlay_alloc(&budget, &request, sizeof(CuvsPagePosition), &incoming) ==
           CUVS_STATUS_OK);
    assert(budget.used == budget.limit);
    assert(cuvs_page_overlay_alloc(&budget, &request, sizeof(CuvsPagePosition), &refused) ==
           CUVS_STATUS_PAGE_OVERLAY_HOST_FULL && refused == NULL);
    cuvs_page_overlay_release(retained);
    assert(budget.used == budget.limit);
    cuvs_page_overlay_release(incoming);
    assert(budget.used == charge);
    cuvs_page_overlay_release(retained);
    assert(budget.used == 0);
}

int
main(void)
{
    CuvsPageOverlay overlay = {0};
    CuvsPageOverlayBudget budget = {.limit = SIZE_MAX};
    CuvsPageOverlayChunk *first;
    CuvsPageOverlayChunk *snapshot;

    expect_supply(&budget, &overlay, 1, 1, CUVS_STATUS_PAGE_NEED, 0);
    expect_supply(&budget, &overlay, 1, 0, CUVS_STATUS_OK, 2);
    first = overlay.head;
    cuvs_page_overlay_retain(first);
    assert(first->refs == 2);
    expect_supply(&budget, &overlay, 1, 0, CUVS_STATUS_OK, 2);
    assert(overlay.head == first && first->next == NULL && overlay.generation == 1);
    expect_supply(&budget, &overlay, 1, 2, CUVS_STATUS_OK, 4);
    snapshot = overlay.head;
    cuvs_page_overlay_retain(snapshot);
    assert(snapshot->from == 2 && snapshot->next == first && first->next == NULL);
    expect_supply(&budget, &overlay, 2, 2, CUVS_STATUS_PAGE_NEED, 0);
    assert(overlay.head == NULL && overlay.generation == 2);
    expect_supply(&budget, &overlay, 1, 2, CUVS_STATUS_PAGE_NEED, 0);
    assert(overlay.head == NULL && overlay.generation == 3);
    assert(snapshot->from == 2 && snapshot->next == first && first->positions == 2);
    cuvs_page_overlay_release(snapshot);
    assert(first->from == 0 && first->next == NULL);
    cuvs_page_overlay_release(first);
    assert(budget.used == 0);
    puts("Page overlay contiguous coverage and boot change: PASS");
    check_budget();
    puts("Page overlay host budget and retained references: PASS");
    return 0;
}
