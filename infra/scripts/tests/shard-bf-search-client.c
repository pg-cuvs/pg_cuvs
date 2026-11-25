/* Integration fixture for the public legacy SEARCH IPC. */
#include "cuvs_ipc.h"
#include "cuvs_util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static int
parse_oid(const char *text, uint32_t *oid)
{
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || end == text || *end || value == 0 || value > UINT32_MAX)
        return 0;
    *oid = (uint32_t) value;
    return 1;
}

int
main(int argc, char **argv)
{
    uint32_t db_oid, index_oid;
    if (argc != 4 || !parse_oid(argv[2], &db_oid) || !parse_oid(argv[3], &index_oid))
    {
        fprintf(stderr, "usage: %s socket db_oid index_oid\n", argv[0]);
        return 2;
    }

    const float query[] = {150, 1, 0, 0};
    uint64_t tid = 0;
    float distance = 0;
    int count = 0, merged = 0;
    int status = cuvs_ipc_search(argv[1], db_oid, index_oid, query, 4, 1, CUVS_METRIC_L2,
                                 0 /* shard overfetch */, 1 /* parallel fanout */,
                                 0 /* CPU HNSW */, 1 /* brute force */, 0 /* float32 */,
                                 0 /* BF batch */, 0 /* CAGRA batch */, CUVS_DELTA_SEARCH_GPU,
                                 &tid, &distance, &count, NULL, &merged);

    uint32_t block;
    uint16_t offset;
    cuvs_tid_decode(tid, &block, &offset);
    printf("%s %d %d (%u,%u) %.9g\n", cuvs_status_str(status), count, merged,
           block, (unsigned int) offset, (double) distance);
    return 0;
}
