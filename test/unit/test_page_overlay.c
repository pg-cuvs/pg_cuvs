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

static void
check_fold_prune(void)
{
    CuvsPageOverlay overlay = {0};
    CuvsPageOverlayBudget budget = {.limit = SIZE_MAX};
    CuvsPageOverlayChunk *oldest, *boundary, *head, *obsolete;
    const size_t charge = sizeof(CuvsPageOverlayChunk) + 2 * sizeof(CuvsPagePosition);

    assert(cuvs_page_overlay_start_boot(&overlay, 2, 100) == NULL);
    expect_supply(&budget, &overlay, 2, 100, CUVS_STATUS_OK, 102);
    oldest = overlay.head;
    cuvs_page_overlay_retain(oldest);
    expect_supply(&budget, &overlay, 2, 102, CUVS_STATUS_OK, 104);
    boundary = overlay.head;
    cuvs_page_overlay_retain(boundary);
    expect_supply(&budget, &overlay, 2, 104, CUVS_STATUS_OK, 106);
    head = overlay.head;
    const uint64_t generation = overlay.generation;

    assert(cuvs_page_overlay_prune_folded(&overlay, 103) == NULL);
    assert(overlay.head == head && head->next == boundary && boundary->next == oldest);
    assert(overlay.covered_upto == 106 && overlay.generation == generation);
    assert(boundary->refs == 2 && oldest->refs == 2 && budget.used == 3 * charge);
    expect_supply(&budget, &overlay, 2, 106, CUVS_STATUS_OK, 108);

    cuvs_page_overlay_release(boundary);
    obsolete = cuvs_page_overlay_prune_folded(&overlay, 103);
    assert(obsolete == oldest && boundary->next == NULL);
    assert(overlay.head->next == head && head->next == boundary);
    assert(overlay.covered_upto == 108 && overlay.generation == generation);
    assert(oldest->refs == 2 && budget.used == 4 * charge);
    cuvs_page_overlay_release(obsolete);
    assert(oldest->refs == 1 && budget.used == 4 * charge);
    cuvs_page_overlay_release(oldest);
    assert(budget.used == 3 * charge);

    /* At an exact boundary the once-straddling chunk can go too. */
    obsolete = cuvs_page_overlay_prune_folded(&overlay, 104);
    assert(obsolete == boundary && head->next == NULL);
    cuvs_page_overlay_release(obsolete);
    assert(budget.used == 2 * charge);
    head = overlay.head;
    cuvs_page_overlay_retain(head);
    obsolete = cuvs_page_overlay_prune_folded(&overlay, 108);
    assert(obsolete == head && overlay.head == NULL && head->next != NULL);
    assert(overlay.covered_upto == 108 && overlay.generation == generation);
    cuvs_page_overlay_release(obsolete);
    assert(budget.used == 2 * charge);
    cuvs_page_overlay_release(head);
    assert(budget.used == 0);

    expect_supply(&budget, &overlay, 2, 108, CUVS_STATUS_OK, 110);
    expect_supply(&budget, &overlay, 2, 110, CUVS_STATUS_OK, 112);
    head = overlay.head;
    cuvs_page_overlay_retain(head);
    obsolete = cuvs_page_overlay_advance_fold(&overlay, 110);
    assert(obsolete == head && overlay.head == NULL && head->next != NULL);
    assert(overlay.covered_upto == 110 && overlay.generation == generation + 1);
    cuvs_page_overlay_release(obsolete);
    cuvs_page_overlay_release(head);
    assert(budget.used == 0);
    puts("Page overlay pinned fold pruning: PASS");
}

static void
check_score_range(void)
{
    const CuvsPagePosition marks[] = {
        {.block = 11, .state = CUVS_PAGE_POSITION_VECTOR},
        {.state = CUVS_PAGE_POSITION_DEAD},
        {.state = CUVS_PAGE_POSITION_INVISIBLE},
        {.block = 12, .state = CUVS_PAGE_POSITION_VECTOR},
        {.block = 22, .state = CUVS_PAGE_POSITION_VECTOR},
        {.state = CUVS_PAGE_POSITION_DEAD},
        {.block = 33, .state = CUVS_PAGE_POSITION_VECTOR},
        {.block = 44, .state = CUVS_PAGE_POSITION_VECTOR},
    };
    const float vectors[][2] = {{3, 4}, {1, 0}, {1, 2}, {2, 0}, {0, 0}};
    const float query[] = {0, 0};
    const size_t bytes = sizeof(marks) + sizeof(vectors);
    CuvsPageOverlayBudget budget = {.limit = sizeof(CuvsPageOverlayChunk) + bytes};
    CuvsPageOverlayRequest request = {
        .base = {.n_slots = 40, .folded = 103, .tail = 108},
        .from = 100,
        .positions = sizeof(marks) / sizeof(marks[0]),
        .vector_rows = sizeof(vectors) / sizeof(vectors[0]),
    };
    const CuvsCmdFrame cmd = {.dim = 2, .k = 4, .metric = CUVS_METRIC_L2};
    CuvsPageOverlayResult results[4] = {{.slot = 39, .distance = 5}};
    CuvsPageOverlayChunk *chunk;
    int count = 1;

    assert(cuvs_page_overlay_alloc(&budget, &request, bytes, &chunk) == CUVS_STATUS_OK);
    memcpy(chunk->marks, marks, sizeof(marks));
    memcpy(chunk->marks + chunk->positions, vectors, sizeof(vectors));
    assert(cuvs_page_overlay_valid(chunk, request.vector_rows, false));

    cuvs_page_overlay_score_range(chunk, 104, 107, &request.base, query, &cmd, results, &count);
    assert(count == 3);
    assert(results[0].slot == 43 && results[0].block == 33 && results[0].distance == 4);
    assert(results[1].slot == 39 && results[1].distance == 5);
    assert(results[2].slot == 41 && results[2].block == 22 && results[2].distance == 5);

    cuvs_page_overlay_score_range(chunk, 105, 105, &request.base, query, &cmd, results, &count);
    cuvs_page_overlay_score_range(chunk, 106, 103, &request.base, query, &cmd, results, &count);
    cuvs_page_overlay_score_range(chunk, 90, 100, &request.base, query, &cmd, results, &count);
    cuvs_page_overlay_score_range(chunk, 108, 109, &request.base, query, &cmd, results, &count);
    assert(count == 3 && results[0].slot == 43 && results[1].slot == 39 && results[2].slot == 41);

    count = 0;
    cuvs_page_overlay_score_range(chunk, 99, 104, &request.base, query, &cmd, results, &count);
    assert(count == 1 && results[0].slot == 40 && results[0].block == 12 &&
           results[0].distance == 1);
    count = 0;
    cuvs_page_overlay_score(chunk, 103, &request.base, query, &cmd, results, &count);
    assert(count == 1 && results[0].slot == 37 && results[0].block == 11 &&
           results[0].distance == 25);

    assert(chunk->refs == 1 && budget.used == budget.limit);
    assert(memcmp(chunk->marks, marks, sizeof(marks)) == 0);
    assert(memcmp(chunk->marks + chunk->positions, vectors, sizeof(vectors)) == 0);
    cuvs_page_overlay_release(chunk);
    assert(budget.used == 0);
    puts("Page overlay packed range scoring: PASS");
}

static void
check_supply(void)
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
}

int
main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "fold") == 0)
    {
        check_fold_prune();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "score") == 0)
    {
        check_score_range();
        return 0;
    }
    if (argc != 1)
    {
        fprintf(stderr, "usage: %s [fold|score]\n", argv[0]);
        return 2;
    }

    check_supply();
    check_budget();
    puts("Page overlay host budget and retained references: PASS");
    check_fold_prune();
    check_score_range();
    return 0;
}
