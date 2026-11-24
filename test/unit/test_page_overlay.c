#include "cuvs_page_overlay.h"

#include <assert.h>
#include <stdio.h>

static void
expect_supply(CuvsPageOverlay *overlay, uint64_t boot, uint64_t from, uint32_t expected,
              uint64_t covered_upto)
{
    CuvsPageOverlayRequest request = {.base.tail = from + 2, .boot_id = boot,
                                     .from = from, .positions = 2};
    CuvsPagePosition marks[2] = {{0}};
    CuvsPageOverlayChunk *chunk = cuvs_page_overlay_copy(&request, marks, sizeof(marks), true);
    CuvsPageOverlayChunk *old = NULL;

    assert(chunk);
    assert(cuvs_page_overlay_supply(overlay, &request, &chunk, &old) == expected);
    assert(overlay->covered_upto == covered_upto);
    cuvs_page_overlay_free(chunk);
    cuvs_page_overlay_free(old);
}

int
main(void)
{
    CuvsPageOverlay overlay = {0};
    CuvsPageOverlayChunk *first;

    expect_supply(&overlay, 1, 1, CUVS_STATUS_PAGE_NEED, 0);
    expect_supply(&overlay, 1, 0, CUVS_STATUS_OK, 2);
    first = overlay.head;
    expect_supply(&overlay, 1, 0, CUVS_STATUS_OK, 2);
    assert(overlay.head == first && overlay.tail == first && first->next == NULL);
    expect_supply(&overlay, 1, 2, CUVS_STATUS_OK, 4);
    assert(overlay.head == first && first->next == overlay.tail && overlay.tail->from == 2);
    expect_supply(&overlay, 2, 2, CUVS_STATUS_PAGE_NEED, 0);
    assert(overlay.head == NULL && overlay.tail == NULL);
    puts("Page overlay contiguous coverage and boot change: PASS");
    return 0;
}
