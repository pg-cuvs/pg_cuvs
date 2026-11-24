#include "cuvs_ipc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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

    if (status != expected || (expected != CUVS_STATUS_ERROR &&
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
    request.from = 1;
    request.base.tail = 3;
    if (expect_supply(socket, db, index, &request, &rows, CUVS_STATUS_PAGE_NEED, 2))
        return 1;
    request.boot_id = 2;
    if (expect_supply(socket, db, index, &request, &rows, CUVS_STATUS_PAGE_NEED, 0))
        return 1;
    request.from = 0;
    request.base.tail = 2;
    marks[0].state = CUVS_PAGE_POSITION_INVISIBLE;
    return expect_supply(socket, db, index, &request, &rows, CUVS_STATUS_ERROR, 0);
}

int
main(int argc, char **argv)
{
    if (argc != 9)
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
    puts("single and batch slots, eight mismatches, supply acknowledgments and boot fencing: PASS");
    return 0;
}
