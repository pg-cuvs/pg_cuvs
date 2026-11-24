#include "cuvs_ipc.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static uint64_t
arg64(const char *text)
{
    char *tail;
    uint64_t number = strtoull(text, &tail, 10);
    if (!text[0] || *tail)
        exit(64);
    return number;
}

static int
try_search(const char *socket, uint32_t db, uint32_t idx,
           CuvsPageSearchRequest *request, int expected)
{
    const float query[8] = {1,1,1,1,1,1,1,1};
    CuvsResult rows[10];
    int count;
    uint32_t sample;
    int status = cuvs_ipc_search_pages(socket, db, idx, request, query, 1, 8, 10,
        CUVS_METRIC_L2, 0, 0, 0, 0, 0, NULL, 0, rows, &count, &sample);

    if (status != expected || (expected == CUVS_STATUS_OK && (count != 10 || rows[0].tid != 0)))
    {
        fprintf(stderr, "SEARCH returned status=%d count=%d; expected status=%d\n", status, count, expected);
        return 1;
    }
    return 0;
}

static int
expect_supply(const char *socket, uint32_t db, uint32_t index,
              const CuvsPageOverlayRequest *request, const CuvsPageSupplement *rows,
              int expected, uint64_t covered)
{
    CuvsPageProgress progress;
    int status = cuvs_ipc_supply_pages(socket, db, index, request, 8, CUVS_METRIC_L2,
                                       rows, &progress);

    bool has_progress = expected == CUVS_STATUS_OK || expected == CUVS_STATUS_PAGE_NEED ||
                        expected == CUVS_STATUS_PAGE_OVERLAY_FULL;
    if (status != expected || (has_progress &&
                              (!progress.daemon_id || progress.covered_upto != covered)))
    {
        fprintf(stderr, "SUPPLY returned status=%d coverage=%llu; want status=%d coverage=%llu\n",
                status, (unsigned long long) progress.covered_upto, expected,
                (unsigned long long) covered);
        return 1;
    }
    return 0;
}

static int
expect_overlay_search(const char *socket, uint32_t db, uint32_t index,
                      const CuvsPageOverlayRequest *request, const CuvsPageSupplement *rows,
                      int nq, int expected, uint64_t covered)
{
    const float query[2][8] = {{0}, {1,1,1,1,1,1,1,1}};
    CuvsPageOverlayResult results[2];
    CuvsPageProgress progress;
    int count;
    uint32_t latency;
    int status = cuvs_ipc_search_page_overlay(socket, db, index, request, &query[0][0], nq, 8, 1,
        CUVS_METRIC_L2, 0, 0, 0, 0, 0, rows, results, &count, &latency, &progress);

    if (status == expected && progress.covered_upto == covered &&
        (status != CUVS_STATUS_OK ||
         (count == nq && results[0].slot == request->base.n_slots + 1 &&
          results[0].block == 65 && results[0].distance == 0 &&
          (nq == 1 || results[1].slot == 0))))
        return 0;
    fprintf(stderr, "OVERLAY SEARCH status=%d count=%d coverage=%llu, expected status=%d\n",
            status, count, (unsigned long long) progress.covered_upto, expected);
    return 1;
}

static int
check_supply(const char *socket, uint32_t db, uint32_t index, CuvsPageSearchRequest base)
{
    const float vector[8] = {0};
    CuvsPagePosition marks[2] = {
        {.state = CUVS_PAGE_POSITION_DEAD},
        {.block = 65, .state = CUVS_PAGE_POSITION_VECTOR}
    };
    CuvsPageSupplement rows = {.marks = marks, .vectors = vector, .positions = 2, .vector_rows = 1};
    CuvsPageOverlayRequest request = {.base = base, .boot_id = 1, .from = 1};

    request.base.tail = 3;
    if (expect_supply(socket, db, index, &request, &rows, CUVS_STATUS_PAGE_NEED, 0))
        return 1;
    request.from = 0;
    request.base.tail = 2;
    if (expect_supply(socket, db, index, &request, &rows, CUVS_STATUS_OK, 2) ||
        expect_supply(socket, db, index, &request, &rows, CUVS_STATUS_OK, 2))
        return 1;
    request.from = 2;
    if (expect_overlay_search(socket, db, index, &request, NULL, 1, CUVS_STATUS_OK, 2) ||
        expect_overlay_search(socket, db, index, &request, NULL, 2, CUVS_STATUS_OK, 2))
        return 1;
    request.from = 1;
    request.base.tail = 3;
    if (expect_supply(socket, db, index, &request, &rows, CUVS_STATUS_PAGE_NEED, 2))
        return 1;
    request.from = 2;
    request.base.tail = 4;
    if (expect_supply(socket, db, index, &request, &rows, CUVS_STATUS_OK, 4))
        return 1;

    CuvsPageProgress progress;
    if (cuvs_ipc_supply_pages(socket, db, index, &request, 7, CUVS_METRIC_L2, &rows, &progress)
            != CUVS_STATUS_NOT_FOUND ||
        cuvs_ipc_supply_pages(socket, db, index, &request, 8, CUVS_METRIC_IP, &rows, &progress)
            != CUVS_STATUS_NOT_FOUND ||
        expect_supply(socket, db + 1, index, &request, &rows, CUVS_STATUS_NOT_FOUND, 0) ||
        expect_supply(socket, db, index + 1, &request, &rows, CUVS_STATUS_NOT_FOUND, 0))
        return 1;
    uint64_t *identity[] = {&request.base.system_identifier, &request.base.index_nonce,
                           &request.base.base_nonce, &request.base.graph_nonce, &request.base.n_slots};
    for (size_t i = 0; i < sizeof(identity) / sizeof(*identity); ++i)
    {
        *identity[i] ^= 1;
        if (expect_supply(socket, db, index, &request, &rows, CUVS_STATUS_NOT_FOUND, 0))
            return 1;
        *identity[i] ^= 1;
    }
    request.base.folded = 1;
    if (expect_supply(socket, db, index, &request, &rows, CUVS_STATUS_NOT_FOUND, 0))
        return 1;
    request.base.folded = 0;
    request.from = CUVS_PAGE_OVERLAY_MAX;
    request.base.tail = request.from + 2;
    if (expect_supply(socket, db, index, &request, &rows, CUVS_STATUS_PAGE_OVERLAY_FULL, 4))
        return 1;

    request.boot_id = 2;
    request.from = 1;
    request.base.tail = 3;
    if (expect_supply(socket, db, index, &request, &rows, CUVS_STATUS_PAGE_NEED, 0))
        return 1;
    request.from = 0;
    request.base.tail = 2;
    rows.vector_rows = 0;
    if (expect_supply(socket, db, index, &request, &rows, CUVS_STATUS_ERROR, 0))
        return 1;
    rows.vector_rows = 1;
    marks[1].block = 0;
    if (expect_supply(socket, db, index, &request, &rows, CUVS_STATUS_ERROR, 0))
        return 1;
    marks[1].block = 65;
    marks[0].state = CUVS_PAGE_POSITION_INVISIBLE;
    if (expect_supply(socket, db, index, &request, &rows, CUVS_STATUS_ERROR, 0) ||
        expect_overlay_search(socket, db, index, &request, &rows, 1, CUVS_STATUS_OK, 0))
        return 1;
    request.from = 2;
    return expect_overlay_search(socket, db, index, &request, NULL, 1, CUVS_STATUS_PAGE_NEED, 0);
}

static int
check_merge_l2(const char *socket, uint32_t db, uint32_t index, CuvsPageSearchRequest base)
{
    const float query[2][8] = {{0}, {1,1,1,1,1,1,1,1}};
    const float shared_vectors[2][8] = {{1,1,1,1,1,1,1,1}, {0}};
    const float supplement_vector[8] = {0};
    CuvsPagePosition shared_marks[2] = {
        {.block = 66, .state = CUVS_PAGE_POSITION_VECTOR},
        {.block = 67, .state = CUVS_PAGE_POSITION_VECTOR}
    };
    CuvsPagePosition supplement_marks[2] = {
        {.state = CUVS_PAGE_POSITION_INVISIBLE},
        {.block = 68, .state = CUVS_PAGE_POSITION_VECTOR}
    };
    CuvsPageSupplement shared = {shared_marks, &shared_vectors[0][0], 2, 2};
    CuvsPageSupplement supplement = {supplement_marks, supplement_vector, 2, 1};
    CuvsPageOverlayRequest request = {.base = base, .boot_id = 3, .from = 0};
    CuvsPageProgress progress = {0};
    CuvsPageOverlayResult results[520] = {0};
    uint32_t latency;
    int count = 0;

    request.base.tail = 2;
    if (cuvs_ipc_supply_pages(socket, db, index, &request, 8, CUVS_METRIC_L2,
                              &shared, &progress) != CUVS_STATUS_OK || progress.covered_upto != 2)
        return 1;
    request.from = 1;  /* Cut the shared [0,2) chunk in its middle. */
    request.base.tail = 3;
    int status = cuvs_ipc_search_page_overlay(socket, db, index, &request, &query[0][0],
        2, 8, 3, CUVS_METRIC_L2, 0, 0, 0, 0, 0, &supplement,
        results, &count, &latency, &progress);
    if (status != CUVS_STATUS_OK || count != 6 || progress.covered_upto != 2 ||
        results[0].slot != base.n_slots + 2 || results[0].distance != 0 ||
        results[1].slot != 0 || results[1].distance != 8 ||
        results[2].slot != base.n_slots || results[2].distance != 8 ||
        results[3].slot != 0 || results[4].slot != base.n_slots || results[5].slot != 1)
    {
        fprintf(stderr, "L2 mixed/cut/trim status=%d count=%d slots=%llu,%llu,%llu,%llu,%llu,%llu\n",
            status, count, (unsigned long long) results[0].slot,
            (unsigned long long) results[1].slot, (unsigned long long) results[2].slot,
            (unsigned long long) results[3].slot, (unsigned long long) results[4].slot,
            (unsigned long long) results[5].slot);
        return 1;
    }
    status = cuvs_ipc_search_page_overlay(socket, db, index, &request, &query[0][0],
        2, 8, 260, CUVS_METRIC_L2, 0, 0, 0, 0, 0, &supplement,
        results, &count, &latency, &progress);
    if (status != CUVS_STATUS_OK || count != 520 ||
        results[258].slot != CUVS_PAGE_SLOT_INVALID ||
        results[259].slot != CUVS_PAGE_SLOT_INVALID ||
        results[518].slot != CUVS_PAGE_SLOT_INVALID ||
        results[519].slot != CUVS_PAGE_SLOT_INVALID)
    {
        fprintf(stderr, "L2 batch short fill status=%d count=%d\n", status, count);
        return 1;
    }
    return 0;
}

static int
check_merge_ip(const char *socket, uint32_t db, uint32_t index, CuvsPageSearchRequest base)
{
    const float query[8] = {1,1,1,1,1,1,1,1};
    const float shared_vectors[2][8] = {{0.45,0.45,0.45,0.45,0.45,0.45,0.45,0.45},
                                         {0.75,0.75,0.75,0.75,0.75,0.75,0.75,0.75}};
    const float supplement_vector[8] = {0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5};
    const float weak_vector[8] = {0.15,0.15,0.15,0.15,0.15,0.15,0.15,0.15};
    CuvsPagePosition shared_marks[2] = {
        {.block = 76, .state = CUVS_PAGE_POSITION_VECTOR},
        {.block = 77, .state = CUVS_PAGE_POSITION_VECTOR}
    };
    CuvsPagePosition supplement_marks[2] = {
        {.state = CUVS_PAGE_POSITION_INVISIBLE},
        {.block = 78, .state = CUVS_PAGE_POSITION_VECTOR}
    };
    CuvsPageSupplement shared = {shared_marks, &shared_vectors[0][0], 2, 2};
    CuvsPageSupplement supplement = {supplement_marks, supplement_vector, 2, 1};
    CuvsPageSupplement weak_supplement = {supplement_marks, weak_vector, 2, 1};
    CuvsPageOverlayRequest request = {.base = base, .boot_id = 4, .from = 0};
    CuvsPageProgress progress = {0};
    CuvsResult old[4] = {0};
    CuvsPageOverlayResult results[4] = {0};
    uint32_t latency;
    int count = 0;
    int status = cuvs_ipc_search_pages(socket, db, index, &base, query, 1, 8, 4,
        CUVS_METRIC_IP, 0, 0, 1, 0, 0, NULL, 0, old, &count, &latency);
    if (status != CUVS_STATUS_OK || count != 4 || old[0].tid != 0 ||
        fabsf(old[0].distance - (1.0f - 7.2f)) > 0.001f)
    {
        fprintf(stderr, "old CPU HNSW IP contract status=%d count=%d slot=%llu distance=%g\n",
                status, count, (unsigned long long) old[0].tid, old[0].distance);
        return 1;
    }
    request.base.tail = 2;
    if (cuvs_ipc_supply_pages(socket, db, index, &request, 8, CUVS_METRIC_IP,
                              &shared, &progress) != CUVS_STATUS_OK || progress.covered_upto != 2)
        return 1;
    request.from = 1;
    request.base.tail = 3;
    status = cuvs_ipc_search_page_overlay(socket, db, index, &request, query, 1, 8, 4,
        CUVS_METRIC_IP, 0, 0, 1, 0, 0, &supplement,
        results, &count, &latency, &progress);
    if (status != CUVS_STATUS_OK || count != 4 || progress.covered_upto != 2 ||
        results[0].slot != 0 || fabsf(results[0].distance - 7.2f) > 0.001f ||
        results[1].slot != base.n_slots + 2 || results[1].distance != 4 ||
        results[2].slot != 1 || fabsf(results[2].distance - 3.6f) > 0.001f ||
        results[3].slot != base.n_slots || fabsf(results[3].distance - 3.6f) > 0.001f)
    {
        fprintf(stderr, "IP mixed/cut/order/trim status=%d count=%d slots=%llu,%llu,%llu,%llu distances=%g,%g,%g,%g\n",
            status, count, (unsigned long long) results[0].slot,
            (unsigned long long) results[1].slot, (unsigned long long) results[2].slot,
            (unsigned long long) results[3].slot, results[0].distance,
            results[1].distance, results[2].distance, results[3].distance);
        return 1;
    }
    /* This supplement enters exactly at the final retained position. */
    status = cuvs_ipc_search_page_overlay(socket, db, index, &request, query, 1, 8, 4,
        CUVS_METRIC_IP, 0, 0, 1, 0, 0, &weak_supplement,
        results, &count, &latency, &progress);
    if (status != CUVS_STATUS_OK || count != 4 ||
        results[0].slot != 0 || results[1].slot != 1 ||
        results[2].slot != base.n_slots || results[3].slot != base.n_slots + 2 ||
        fabsf(results[3].distance - 1.2f) > 0.001f)
    {
        fprintf(stderr, "IP last retained slot status=%d count=%d slots=%llu,%llu,%llu,%llu\n",
            status, count, (unsigned long long) results[0].slot,
            (unsigned long long) results[1].slot, (unsigned long long) results[2].slot,
            (unsigned long long) results[3].slot);
        return 1;
    }
    return 0;
}

int
main(int argc, char **argv)
{
    if (argc != 9 && argc != 10)
        return 64;
    uint32_t db = (uint32_t) arg64(argv[2]);
    uint32_t idx = (uint32_t) arg64(argv[3]);
    CuvsPageSearchRequest request = {
        .length = sizeof(request),
        .system_identifier = arg64(argv[4]),
        .index_nonce = arg64(argv[5]),
        .base_nonce = arg64(argv[6]),
        .graph_nonce = arg64(argv[7]),
        .n_slots = arg64(argv[8])
    };

    if (argc == 10)
        return check_merge_ip(argv[1], db, idx, request);

    if (try_search(argv[1], db, idx, &request, CUVS_STATUS_OK))
        return 1;
    if (try_search(argv[1], db ^ 1, idx, &request, CUVS_STATUS_NOT_FOUND) ||
        try_search(argv[1], db, idx ^ 1, &request, CUVS_STATUS_NOT_FOUND))
        return 1;

    const float queries[2][8] = {
        {1,1,1,1,1,1,1,1},
        {2,2,2,2,2,2,2,2},
    };
    CuvsResult batch[6];
    int count;
    uint32_t latency;
    int status = cuvs_ipc_search_pages(argv[1], db, idx, &request, &queries[0][0],
        2, 8, 3, CUVS_METRIC_L2, 0, 0, 0, 0, 0, NULL, 0, batch, &count, &latency);

    if (status != CUVS_STATUS_OK || count != 6 || batch[0].tid != 0 || batch[3].tid != 1)
    {
        fprintf(stderr, "batched slot search failed: status=%d count=%d\n", status, count);
        return 1;
    }

    uint64_t *keys[] = {&request.system_identifier, &request.index_nonce,
        &request.base_nonce, &request.graph_nonce, &request.n_slots, &request.tail};
    for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); ++i)
    {
        *keys[i] ^= 1;
        if (try_search(argv[1], db, idx, &request, CUVS_STATUS_NOT_FOUND))
            return 1;
        *keys[i] ^= 1;
    }
    if (check_supply(argv[1], db, idx, request))
        return 1;
    if (check_merge_l2(argv[1], db, idx, request))
        return 1;
    puts("single/batch slots, overlay merge and fill, mismatches, supply and boot fencing: PASS");
    return 0;
}
