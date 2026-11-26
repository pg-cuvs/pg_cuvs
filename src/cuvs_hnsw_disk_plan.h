#ifndef CUVS_HNSW_DISK_PLAN_H
#define CUVS_HNSW_DISK_PLAN_H

/*
 * Local disk plan of a source-less pg_cuvs_hnsw build (CREATE INDEX ... USING
 * pg_cuvs_hnsw without source). PostgreSQL- and CUDA-free so the unit test
 * runs exactly what the backend evaluates.
 *
 * The post-scan plan estimates additional space in two phases:
 *
 *   daemon build  the daemon's private workdir (input copy, reorder, graph,
 *                 serialization);
 *   index write   after the reply the daemon has removed its workdir except
 *                 the open, unlinked graph file, and the backend has closed
 *                 the vector spool; the node-location spill and index pages
 *                 (and their WAL) are written. The TID spool already exists.
 *
 * Within each phase, probed PostgreSQL files on one filesystem (same st_dev)
 * are summed and compared with its free space. Daemon mounts are not
 * identified by the backend. WAL is shown but not enforced:
 * checkpoints recycle it, so its peak depends on max_wal_size, archiving and
 * replication slots, not on the index alone. An item whose filesystem could
 * not be probed is shown and never refuses.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef enum CuvsHnswDiskItemKind {
    CUVS_HNSW_DISK_WORKDIR = 0, /* daemon build: private workdir under --index-dir */
    CUVS_HNSW_DISK_GRAPH,       /* index write: the daemon's graph file */
    CUVS_HNSW_DISK_LOCATIONS,   /* index write: node-location spill */
    CUVS_HNSW_DISK_INDEX,       /* index write: index relation pages */
    CUVS_HNSW_DISK_WAL,         /* index write: illustrative WAL estimate */
    CUVS_HNSW_DISK_ITEMS
} CuvsHnswDiskItemKind;

#define CUVS_HNSW_DISK_PHASES 2

typedef struct CuvsHnswDiskItem {
    uint64_t need;     /* bytes; 0 = this build does not use the item */
    uint64_t avail;    /* free bytes of its filesystem (statvfs f_bavail) */
    uint64_t fs;       /* filesystem identity (st_dev) */
    bool known;        /* the filesystem was probed */
    const char *path;  /* probed directory, for messages */
} CuvsHnswDiskItem;

typedef struct CuvsHnswDiskVerdict {
    bool fits;            /* every phase fits on every probed filesystem */
    int short_item;       /* first item on the first short filesystem; -1 if fits */
    int short_phase;      /* its phase; -1 if fits */
    uint64_t short_need;  /* that phase's enforced bytes on that filesystem */
    uint64_t short_avail; /* its free bytes */
    bool wal_tight;       /* pg_wal's filesystem may not hold the estimated WAL */
    uint64_t phase_need[CUVS_HNSW_DISK_PHASES]; /* all items except WAL */
    uint64_t peak_need;   /* the larger phase */
} CuvsHnswDiskVerdict;

/* The hnswlib file header stays far below this. */
#define CUVS_HNSW_DISK_HEADER_BOUND UINT64_C(4096)

static inline uint64_t
cuvs_hnsw_disk_add(uint64_t a, uint64_t b)
{
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

static inline uint64_t
cuvs_hnsw_disk_rows(uint64_t n, uint64_t row)
{
    if (n == 0)
        return 0;
    if (row != 0 && n > (UINT64_MAX - CUVS_HNSW_DISK_HEADER_BOUND) / row)
        return UINT64_MAX;
    return n * row + CUVS_HNSW_DISK_HEADER_BOUND;
}

/* The daemon's hnswlib graph file: per node the level-0 record (link count,
 * 2m links, fp32 vector, 8-byte label) and the 4-byte upper-level size, plus
 * 8 estimated bytes per row for upper-level links. This is a planning
 * estimate; the actual upper levels and serialized M can differ. */
static inline uint64_t
cuvs_hnsw_graph_file_need(uint64_t n, uint32_t dim, uint32_t m)
{
    return cuvs_hnsw_disk_rows(n, (uint64_t)(2 * (uint64_t)m + 1) * 4
                                  + (uint64_t)dim * 4 + 8 + 4 + 8);
}

/* Node-location spill: one 8-byte HnswNodeLocation per graph node. */
static inline uint64_t
cuvs_hnsw_location_need(uint64_t n)
{
    return n > UINT64_MAX / 8 ? UINT64_MAX : n * 8;
}

static inline int
cuvs_hnsw_disk_phase(int kind)
{
    return kind == CUVS_HNSW_DISK_WORKDIR ? 0 : 1;
}

/* Already allocated spool blocks can be reused in the write phase only. */
static inline void
cuvs_hnsw_disk_credit_vector_spool(CuvsHnswDiskItem items[CUVS_HNSW_DISK_ITEMS],
                                 uint64_t fs, uint64_t bytes)
{
    for (int i = 0; i < CUVS_HNSW_DISK_ITEMS; i++)
        if (cuvs_hnsw_disk_phase(i) == 1 && items[i].known && items[i].fs == fs)
            items[i].avail = cuvs_hnsw_disk_add(items[i].avail, bytes);
}

static inline unsigned long long
cuvs_hnsw_disk_need_mib(uint64_t bytes)
{
    return (unsigned long long)((bytes >> 20) + ((bytes & 0xFFFFF) != 0));
}

static inline bool
cuvs_hnsw_disk_enforced(int kind, const CuvsHnswDiskItem *it)
{
    return (kind == CUVS_HNSW_DISK_LOCATIONS || kind == CUVS_HNSW_DISK_INDEX)
        && it->need > 0 && it->known;
}

/* Enforced bytes of `phase` on the filesystem `fs`. */
static inline uint64_t
cuvs_hnsw_disk_phase_fs_need(const CuvsHnswDiskItem items[CUVS_HNSW_DISK_ITEMS],
                             int phase, uint64_t fs)
{
    uint64_t sum = 0;
    for (int j = 0; j < CUVS_HNSW_DISK_ITEMS; j++)
        if (cuvs_hnsw_disk_phase(j) == phase && cuvs_hnsw_disk_enforced(j, &items[j])
            && items[j].fs == fs)
            sum = cuvs_hnsw_disk_add(sum, items[j].need);
    return sum;
}

static inline void
cuvs_hnsw_disk_evaluate(const CuvsHnswDiskItem items[CUVS_HNSW_DISK_ITEMS],
                        CuvsHnswDiskVerdict *v)
{
    const CuvsHnswDiskItem *wal = &items[CUVS_HNSW_DISK_WAL];

    v->fits = true;
    v->short_item = v->short_phase = -1;
    v->short_need = v->short_avail = 0;
    v->wal_tight = false;
    v->phase_need[0] = v->phase_need[1] = 0;
    for (int i = 0; i < CUVS_HNSW_DISK_ITEMS; i++)
        if (i != CUVS_HNSW_DISK_WAL)
            v->phase_need[cuvs_hnsw_disk_phase(i)] =
                cuvs_hnsw_disk_add(v->phase_need[cuvs_hnsw_disk_phase(i)], items[i].need);
    v->peak_need = v->phase_need[0] > v->phase_need[1] ? v->phase_need[0] : v->phase_need[1];
    for (int i = 0; i < CUVS_HNSW_DISK_ITEMS && v->fits; i++)
    {
        uint64_t sum;
        if (!cuvs_hnsw_disk_enforced(i, &items[i]))
            continue;
        sum = cuvs_hnsw_disk_phase_fs_need(items, cuvs_hnsw_disk_phase(i), items[i].fs);
        if (sum > items[i].avail)
        {
            v->fits = false;
            v->short_item = i;
            v->short_phase = cuvs_hnsw_disk_phase(i);
            v->short_need = sum;
            v->short_avail = items[i].avail;
        }
    }
    if (wal->need > 0 && wal->known)
        v->wal_tight = cuvs_hnsw_disk_add(wal->need,
            cuvs_hnsw_disk_phase_fs_need(items, cuvs_hnsw_disk_phase(CUVS_HNSW_DISK_WAL),
                                         wal->fs)) > wal->avail;
}

static inline const char *
cuvs_hnsw_disk_phase_name(int phase)
{
    return phase == 0 ? "daemon build" : "index write";
}

static inline const char *
cuvs_hnsw_disk_label(int kind)
{
    switch (kind)
    {
        case CUVS_HNSW_DISK_WORKDIR: return "daemon workdir";
        case CUVS_HNSW_DISK_GRAPH: return "graph file";
        case CUVS_HNSW_DISK_LOCATIONS: return "node-location spill";
        case CUVS_HNSW_DISK_INDEX: return "index pages";
        case CUVS_HNSW_DISK_WAL: return "estimated WAL";
        default: return "?";
    }
}

/* "<phase>: <label> <MiB> MiB on <path> (<MiB> MiB free), ..." per phase,
 * phases joined by "; ". MiB are rounded up for needs and down for free
 * space. Truncates to len - 1 bytes. */
static inline void
cuvs_hnsw_disk_detail(const CuvsHnswDiskItem items[CUVS_HNSW_DISK_ITEMS],
                      char *buf, size_t len)
{
    size_t used = 0;
    int last_phase = -1;

    if (len == 0)
        return;
    buf[0] = '\0';
    for (int i = 0; i < CUVS_HNSW_DISK_ITEMS; i++)
    {
        const CuvsHnswDiskItem *it = &items[i];
        unsigned long long need_mib = cuvs_hnsw_disk_need_mib(it->need);
        int phase = cuvs_hnsw_disk_phase(i);
        char free_text[64];
        int w;

        if (it->need == 0)
            continue;
        if (it->known)
            snprintf(free_text, sizeof(free_text), "%llu MiB available for phase%s",
                     (unsigned long long)(it->avail >> 20),
                     i == CUVS_HNSW_DISK_WAL ? ", not enforced" : "");
        else
            snprintf(free_text, sizeof(free_text), "free space not checked");
        w = snprintf(buf + used, len - used, "%s%s%s %llu MiB on %s (%s)",
                     phase == last_phase ? ", " : (used ? "; " : ""),
                     phase == last_phase ? "" : (phase == 0 ? "daemon build: " : "index write: "),
                     cuvs_hnsw_disk_label(i), need_mib,
                     it->path ? it->path : "?", free_text);
        last_phase = phase;
        if (w < 0 || (size_t)w >= len - used)
        {
            buf[len - 1] = '\0';
            return;
        }
        used += (size_t)w;
    }
}

#endif
