/* Local disk plan of a source-less pg_cuvs_hnsw build (cuvs_hnsw_disk_plan.h):
 * per-item needs, the two non-overlapping phases (daemon build, index write),
 * per-filesystem sums against free space within each phase, WAL shown but not
 * enforced, and the detail text. No PostgreSQL, CUDA or daemon needed. */
#include "cuvs_hnsw_disk_plan.h"
#include "cuvs_hnsw_build_common.h" /* cuvs_hnsw_disk_need: the daemon workdir */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MIB (UINT64_C(1) << 20)

static void
item(CuvsHnswDiskItem *items, int kind, uint64_t need, uint64_t fs, uint64_t avail,
     const char *path)
{
    items[kind].need = need;
    items[kind].fs = fs;
    items[kind].avail = avail;
    items[kind].known = true;
    items[kind].path = path;
}

static void
test_needs_match_the_88m_build(void)
{
    /* 87,555,327 x 768 fp32: the 88M import's measured location spill file
     * held 700,442,616 bytes, 8 bytes per node. */
    assert(cuvs_hnsw_location_need(87555327) == UINT64_C(700442616));
    assert(cuvs_hnsw_spool_need(87555327, 768) ==
           UINT64_C(87555327) * (768 * 4 + 8) + CUVS_HNSW_DISK_HEADER_BOUND);
    /* Level-0 record 260 + 3,072 + 8 B, the 4-byte upper size, 8 B upper bound. */
    assert(cuvs_hnsw_graph_file_need(87555327, 768, 32) ==
           UINT64_C(87555327) * (260 + 3072 + 8 + 4 + 8) + CUVS_HNSW_DISK_HEADER_BOUND);
    assert(cuvs_hnsw_spool_need(0, 768) == 0 && cuvs_hnsw_graph_file_need(0, 768, 32) == 0);
    printf("PASS 88M spool, graph file and location needs\n");
}

/* The daemon workdir and the index pages never coexist: the daemon removes
 * its workdir (all but the graph file) before it replies, and the backend
 * closes the vector spool before it writes pages. One filesystem must hold
 * the larger phase, not the sum of both. */
static void
test_88m_on_one_filesystem_needs_the_larger_phase(void)
{
    const uint64_t n = 87555327, tb = UINT64_C(1000000000000);
    CuvsHnswDiskItem items[CUVS_HNSW_DISK_ITEMS] = {{0}};
    CuvsHnswDiskVerdict v;
    uint64_t daemon_phase, write_phase;

    item(items, CUVS_HNSW_DISK_WORKDIR, cuvs_hnsw_disk_need(n, 768, 64), 9, 3 * tb, "/data/idx");
    item(items, CUVS_HNSW_DISK_SPOOL, cuvs_hnsw_spool_need(n, 768), 9, 3 * tb, "base");
    item(items, CUVS_HNSW_DISK_GRAPH, cuvs_hnsw_graph_file_need(n, 768, 32), 9, 3 * tb, "/data/idx");
    item(items, CUVS_HNSW_DISK_LOCATIONS, cuvs_hnsw_location_need(n) + n * 8, 9, 3 * tb, "base");
    item(items, CUVS_HNSW_DISK_INDEX, (UINT64_C(43777664) + 1) * 8192, 9, 3 * tb, "base");
    cuvs_hnsw_disk_evaluate(items, &v);
    daemon_phase = v.phase_need[0];
    write_phase = v.phase_need[1];
    /* The daemon's own forecast for this build was 1,126,311,726,528 B. */
    assert(items[CUVS_HNSW_DISK_WORKDIR].need == UINT64_C(1126311726528));
    assert(daemon_phase > write_phase && v.peak_need == daemon_phase);
    assert(daemon_phase > 13 * tb / 10 && daemon_phase < 14 * tb / 10);   /* ~1.40 TB */
    assert(daemon_phase + write_phase > 17 * tb / 10);                 /* sum ~1.75 TB */

    /* 1.5 TB free: the sum of both phases would refuse, the larger phase fits. */
    for (int i = 0; i < CUVS_HNSW_DISK_ITEMS; i++)
        items[i].avail = 15 * tb / 10;
    cuvs_hnsw_disk_evaluate(items, &v);
    assert(v.fits);

    /* One byte below the daemon phase: refused in that phase. */
    for (int i = 0; i < CUVS_HNSW_DISK_ITEMS; i++)
        items[i].avail = daemon_phase - 1;
    cuvs_hnsw_disk_evaluate(items, &v);
    assert(!v.fits && v.short_phase == 0 && v.short_item == CUVS_HNSW_DISK_WORKDIR);
    assert(v.short_need == daemon_phase && v.short_avail == daemon_phase - 1);
    printf("PASS 88M on one filesystem needs the larger phase\n");
}

static void
test_index_write_phase_refuses_on_its_own(void)
{
    CuvsHnswDiskItem items[CUVS_HNSW_DISK_ITEMS] = {{0}};
    CuvsHnswDiskVerdict v;

    item(items, CUVS_HNSW_DISK_WORKDIR, 100 * MIB, 1, 1000 * MIB, "/data/idx");
    item(items, CUVS_HNSW_DISK_SPOOL, 50 * MIB, 2, 500 * MIB, "base");
    item(items, CUVS_HNSW_DISK_GRAPH, 60 * MIB, 1, 1000 * MIB, "/data/idx");
    item(items, CUVS_HNSW_DISK_LOCATIONS, 10 * MIB, 2, 500 * MIB, "base");
    item(items, CUVS_HNSW_DISK_INDEX, 490 * MIB, 2, 500 * MIB, "base");
    cuvs_hnsw_disk_evaluate(items, &v);
    assert(v.fits && v.phase_need[0] == 150 * MIB && v.phase_need[1] == 560 * MIB);

    items[CUVS_HNSW_DISK_INDEX].need = 491 * MIB;  /* base: 10 + 491 > 500 */
    cuvs_hnsw_disk_evaluate(items, &v);
    assert(!v.fits && v.short_phase == 1 && v.short_item == CUVS_HNSW_DISK_LOCATIONS);
    assert(v.short_need == 501 * MIB && v.short_avail == 500 * MIB);
    printf("PASS index write phase refuses on its own\n");
}

static void
test_postscan_spool_credit_is_scoped_to_write_phase_and_source_filesystem(void)
{
    CuvsHnswDiskItem items[CUVS_HNSW_DISK_ITEMS] = {{0}};
    CuvsHnswDiskVerdict v;

    item(items, CUVS_HNSW_DISK_WORKDIR, 70 * MIB, 5, 80 * MIB, "/data/idx");
    item(items, CUVS_HNSW_DISK_INDEX, 100 * MIB, 5, 80 * MIB, "base");
    item(items, CUVS_HNSW_DISK_WAL, 10 * MIB, 6, 5 * MIB, "pg_wal");
    cuvs_hnsw_disk_credit_vector_spool(items, 6, 20 * MIB);
    cuvs_hnsw_disk_evaluate(items, &v);
    assert(!v.fits && v.short_phase == 1 && v.short_avail == 80 * MIB && !v.wal_tight);
    cuvs_hnsw_disk_credit_vector_spool(items, 5, 20 * MIB);
    cuvs_hnsw_disk_evaluate(items, &v);
    assert(v.fits);
    assert(items[CUVS_HNSW_DISK_WORKDIR].avail == 80 * MIB);
    assert(items[CUVS_HNSW_DISK_INDEX].avail == 100 * MIB);
    assert(items[CUVS_HNSW_DISK_WAL].avail == 25 * MIB);
}

static void
test_wal_is_shown_but_not_enforced(void)
{
    CuvsHnswDiskItem items[CUVS_HNSW_DISK_ITEMS] = {{0}};
    CuvsHnswDiskVerdict v;

    item(items, CUVS_HNSW_DISK_INDEX, 400 * MIB, 5, 500 * MIB, "base");
    item(items, CUVS_HNSW_DISK_WAL, 400 * MIB, 5, 500 * MIB, "pg_wal");
    cuvs_hnsw_disk_evaluate(items, &v);
    assert(v.fits && v.wal_tight && v.peak_need == 400 * MIB);

    items[CUVS_HNSW_DISK_WAL].fs = 6;   /* pg_wal on its own filesystem */
    cuvs_hnsw_disk_evaluate(items, &v);
    assert(v.fits && !v.wal_tight);

    items[CUVS_HNSW_DISK_WAL].avail = 300 * MIB;
    cuvs_hnsw_disk_evaluate(items, &v);
    assert(v.fits && v.wal_tight);

    items[CUVS_HNSW_DISK_WAL].need = 0;  /* UNLOGGED: no WAL item */
    cuvs_hnsw_disk_evaluate(items, &v);
    assert(v.fits && !v.wal_tight);
    printf("PASS WAL is shown but not enforced\n");
}

static void
test_unprobed_filesystems_do_not_refuse(void)
{
    CuvsHnswDiskItem items[CUVS_HNSW_DISK_ITEMS] = {{0}};
    CuvsHnswDiskVerdict v;

    item(items, CUVS_HNSW_DISK_INDEX, 400 * MIB, 5, 500 * MIB, "base");
    items[CUVS_HNSW_DISK_LOCATIONS].need = 900 * MIB; /* temp spread over filesystems */
    items[CUVS_HNSW_DISK_LOCATIONS].path = "2 temporary tablespaces";
    cuvs_hnsw_disk_evaluate(items, &v);
    assert(v.fits && v.phase_need[1] == 1300 * MIB);
    printf("PASS unprobed filesystems do not refuse\n");
}

static void
test_detail_groups_items_by_phase(void)
{
    CuvsHnswDiskItem items[CUVS_HNSW_DISK_ITEMS] = {{0}};
    char detail[1024];

    item(items, CUVS_HNSW_DISK_WORKDIR, 600 * MIB, 7, 1000 * MIB, "/data/cuvs");
    item(items, CUVS_HNSW_DISK_GRAPH, 200 * MIB, 7, 1000 * MIB, "/data/cuvs");
    item(items, CUVS_HNSW_DISK_INDEX, 150 * MIB, 7, 1000 * MIB, "base");
    item(items, CUVS_HNSW_DISK_WAL, 150 * MIB, 8, 90 * MIB, "pg_wal");
    items[CUVS_HNSW_DISK_SPOOL].need = 5 * MIB;
    items[CUVS_HNSW_DISK_SPOOL].path = "2 temporary tablespaces";
    cuvs_hnsw_disk_detail(items, detail, sizeof(detail));
    assert(strcmp(detail,
        "daemon build: daemon workdir 600 MiB on /data/cuvs (1000 MiB available for phase), "
        "input spool 5 MiB on 2 temporary tablespaces (free space not checked); "
        "index write: graph file 200 MiB on /data/cuvs (1000 MiB available for phase), "
        "index pages 150 MiB on base (1000 MiB available for phase), "
        "estimated WAL 150 MiB on pg_wal (90 MiB available for phase, not enforced)") == 0);
    /* A short buffer truncates without overflowing. */
    cuvs_hnsw_disk_detail(items, detail, 16);
    assert(strlen(detail) == 15);
    printf("PASS detail groups items by phase\n");
}

int
main(void)
{
    test_needs_match_the_88m_build();
    test_88m_on_one_filesystem_needs_the_larger_phase();
    test_index_write_phase_refuses_on_its_own();
    test_postscan_spool_credit_is_scoped_to_write_phase_and_source_filesystem();
    test_wal_is_shown_but_not_enforced();
    test_unprobed_filesystems_do_not_refuse();
    test_detail_groups_items_by_phase();
    printf("hnsw disk plan: ok\n");
    return 0;
}
