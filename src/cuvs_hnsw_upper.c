/*
 * cuvs_hnsw_upper.c — hnswlib file with host-built upper layers from a
 * CAGRA graph. See cuvs_hnsw_upper.h. Plain C and pthreads: the GPU daemon
 * and the CPU shim daemon run the same code.
 */
#include "cuvs_hnsw_upper.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define UPPER_LOCK_STRIPES 4096
#define UPPER_CANCEL_EVERY 64
#define UPPER_MAX_THREADS  8
#define UPPER_LEVEL_CAP    16

/* hnswlib saveIndex header; the backend's HnswlibHeader reads the same 96 bytes. */
typedef struct UpperFileHeader
{
    uint64_t offsetLevel0;
    uint64_t max_elements;
    uint64_t cur_element_count;
    uint64_t sizeDataPerElement;
    uint64_t labelOffset;
    uint64_t offsetData;
    int32_t  maxlevel;
    int32_t  enterpointNode;
    uint64_t maxM;
    uint64_t maxM0;
    uint64_t M;
    double   mult;
    uint64_t efConstruction;
} UpperFileHeader;

_Static_assert(sizeof(UpperFileHeader) == 96, "hnswlib header is 96 bytes");

typedef struct Cand
{
    float    d;
    uint32_t id;
} Cand;

typedef struct Upper
{
    uint64_t        n;
    int             dim;
    int             M;
    int             ef;
    int             ip;
    int             max_level;
    const float    *vecs;
    uint32_t        U;          /* elements above level 0, local ids in row order */
    uint32_t       *row_of;     /* [U] */
    uint8_t        *level_of;   /* [U] */
    size_t         *link_off;   /* [U] start of the level-1 list in links */
    uint32_t       *links;      /* per element and level: count, then M local ids */
    uint32_t        entry;      /* local id of the first element of the top level */
    pthread_mutex_t locks[UPPER_LOCK_STRIPES];
    atomic_uint_fast32_t next;
    atomic_int      cancel;
    atomic_int      failed;
    CuvsHnswUpperCancel cancel_cb;
    void           *cancel_arg;
} Upper;

typedef struct Worker
{
    Upper    *u;
    uint32_t *visited;
    uint32_t  stamp;
    Cand     *cand;     /* min-heap */
    size_t    ncand, capcand;
    Cand     *res;      /* max-heap, at most ef + 1 */
    size_t    nres;
    Cand     *w;        /* search result, ascending */
    uint32_t *nb;       /* neighbor list copy */
    uint32_t *sel;
    Cand     *prune;    /* M + 1 */
} Worker;

int
cuvs_hnsw_upper_level(int M, unsigned int *seed)
{
    /* hnsw_export.c cagra_assign_level: the same draw, so the same levels. */
    double mL = 1.0 / log((double)(M > 1 ? M : 2));
    double r = ((double)rand_r(seed) + 1.0) / ((double)RAND_MAX + 2.0);
    int lv = (int)(-log(r) * mL);
    return lv > UPPER_LEVEL_CAP ? UPPER_LEVEL_CAP : lv;
}

static uint64_t
mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static inline const float *
vec_of(const Upper *u, uint32_t local)
{
    return u->vecs + (size_t)u->row_of[local] * (size_t)u->dim;
}

static inline float
dist(const Upper *u, const float *a, const float *b)
{
    float s = 0.0f;
    if (u->ip)
    {
        for (int k = 0; k < u->dim; k++)
            s += a[k] * b[k];
        return 1.0f - s;      /* hnswlib InnerProductSpace */
    }
    for (int k = 0; k < u->dim; k++)
    {
        float t = a[k] - b[k];
        s += t * t;
    }
    return s;
}

static inline uint32_t *
list_of(const Upper *u, uint32_t local, int lc)
{
    return u->links + u->link_off[local] + (size_t)(lc - 1) * (size_t)(u->M + 1);
}

static inline pthread_mutex_t *
lock_of(Upper *u, uint32_t local)
{
    return &u->locks[local % UPPER_LOCK_STRIPES];
}

/* A copy of one list, taken under its element's lock. */
static int
copy_list(Upper *u, uint32_t local, int lc, uint32_t *out)
{
    pthread_mutex_lock(lock_of(u, local));
    uint32_t *l = list_of(u, local, lc);
    int cnt = (int)l[0];
    memcpy(out, l + 1, (size_t)cnt * sizeof(uint32_t));
    pthread_mutex_unlock(lock_of(u, local));
    return cnt;
}

/* ---- heaps: cand is a min-heap, res a max-heap, both on .d ---- */
static void
heap_push(Cand *h, size_t *n, Cand c, int max_heap)
{
    size_t i = (*n)++;
    h[i] = c;
    while (i > 0)
    {
        size_t p = (i - 1) / 2;
        int swap = max_heap ? h[p].d < h[i].d : h[p].d > h[i].d;
        if (!swap)
            break;
        Cand t = h[p]; h[p] = h[i]; h[i] = t;
        i = p;
    }
}

static Cand
heap_pop(Cand *h, size_t *n, int max_heap)
{
    Cand top = h[0];
    h[0] = h[--(*n)];
    size_t i = 0;
    for (;;)
    {
        size_t l = 2 * i + 1, r = l + 1, best = i;
        if (l < *n && (max_heap ? h[l].d > h[best].d : h[l].d < h[best].d))
            best = l;
        if (r < *n && (max_heap ? h[r].d > h[best].d : h[r].d < h[best].d))
            best = r;
        if (best == i)
            break;
        Cand t = h[best]; h[best] = h[i]; h[i] = t;
        i = best;
    }
    return top;
}

static int
cand_grow(Worker *w)
{
    if (w->ncand + (size_t)w->u->M + 1 <= w->capcand)
        return 1;
    size_t cap = w->capcand * 2 + (size_t)w->u->M + 1;
    Cand *c = realloc(w->cand, cap * sizeof(Cand));
    if (!c)
        return 0;
    w->cand = c;
    w->capcand = cap;
    return 1;
}

/* ef-bounded search of level lc from ep; w->w gets the results ascending. */
static int
search_layer(Worker *w, const float *q, uint32_t ep, int lc, size_t *out_n)
{
    Upper *u = w->u;
    if (++w->stamp == 0)
    {
        memset(w->visited, 0, (size_t)u->U * sizeof(uint32_t));
        w->stamp = 1;
    }
    w->ncand = w->nres = 0;
    Cand c0 = { dist(u, q, vec_of(u, ep)), ep };
    heap_push(w->cand, &w->ncand, c0, 0);
    heap_push(w->res, &w->nres, c0, 1);
    w->visited[ep] = w->stamp;
    while (w->ncand > 0)
    {
        Cand c = heap_pop(w->cand, &w->ncand, 0);
        if (w->nres >= (size_t)u->ef && c.d > w->res[0].d)
            break;
        int cnt = copy_list(u, c.id, lc, w->nb);
        if (!cand_grow(w))
            return 0;
        for (int j = 0; j < cnt; j++)
        {
            uint32_t v = w->nb[j];
            if (w->visited[v] == w->stamp)
                continue;
            w->visited[v] = w->stamp;
            float dv = dist(u, q, vec_of(u, v));
            if (w->nres < (size_t)u->ef || dv < w->res[0].d)
            {
                Cand cv = { dv, v };
                heap_push(w->cand, &w->ncand, cv, 0);
                heap_push(w->res, &w->nres, cv, 1);
                if (w->nres > (size_t)u->ef)
                    (void)heap_pop(w->res, &w->nres, 1);
            }
        }
    }
    size_t k = w->nres;
    *out_n = k;
    while (w->nres > 0)
        w->w[--k] = heap_pop(w->res, &w->nres, 1);
    return 1;
}

/* HNSW heuristic (Malkov & Yashunin, Algorithm 4; hnswlib getNeighborsByHeuristic2):
 * c (ascending by distance to the query) is kept unless a kept s is closer to c than the query is. */
static int
heuristic(const Upper *u, const Cand *c, size_t nc, uint32_t skip, uint32_t *sel)
{
    int k = 0;
    for (size_t i = 0; i < nc && k < u->M; i++)
    {
        if (c[i].id == skip)
            continue;
        const float *vc = vec_of(u, c[i].id);
        int good = 1;
        for (int s = 0; s < k && good; s++)
            if (dist(u, vc, vec_of(u, sel[s])) < c[i].d)
                good = 0;
        if (good)
            sel[k++] = c[i].id;
    }
    return k;
}

static int
cand_cmp(const void *a, const void *b)
{
    float x = ((const Cand *)a)->d, y = ((const Cand *)b)->d;
    return x < y ? -1 : x > y;
}

static void
connect(Worker *w, uint32_t me, int lc, size_t nw)
{
    Upper *u = w->u;
    int k = heuristic(u, w->w, nw, me, w->sel);

    pthread_mutex_lock(lock_of(u, me));
    uint32_t *mine = list_of(u, me, lc);
    mine[0] = (uint32_t)k;
    memcpy(mine + 1, w->sel, (size_t)k * sizeof(uint32_t));
    pthread_mutex_unlock(lock_of(u, me));

    for (int i = 0; i < k; i++)
    {
        uint32_t v = w->sel[i];
        pthread_mutex_lock(lock_of(u, v));
        uint32_t *l = list_of(u, v, lc);
        int cnt = (int)l[0], dup = 0;
        for (int j = 0; j < cnt; j++)
            dup |= l[1 + j] == me;
        if (!dup)
        {
            if (cnt < u->M)
                l[1 + cnt] = me, l[0] = (uint32_t)(cnt + 1);
            else
            {
                const float *vv = vec_of(u, v);
                for (int j = 0; j < cnt; j++)
                {
                    w->prune[j].id = l[1 + j];
                    w->prune[j].d = dist(u, vv, vec_of(u, l[1 + j]));
                }
                w->prune[cnt].id = me;
                w->prune[cnt].d = dist(u, vv, vec_of(u, me));
                qsort(w->prune, (size_t)cnt + 1, sizeof(Cand), cand_cmp);
                uint32_t keep[256];
                int nk = heuristic(u, w->prune, (size_t)cnt + 1, v, keep);
                memcpy(l + 1, keep, (size_t)nk * sizeof(uint32_t));
                l[0] = (uint32_t)nk;
            }
        }
        pthread_mutex_unlock(lock_of(u, v));
    }
}

static int
insert_one(Worker *w, uint32_t me)
{
    Upper *u = w->u;
    const float *q = vec_of(u, me);
    int l = u->level_of[me];
    uint32_t cur = u->entry;
    float dcur = dist(u, q, vec_of(u, cur));

    for (int lc = u->max_level; lc > l; lc--)
    {
        int changed = 1;
        while (changed)
        {
            changed = 0;
            int cnt = copy_list(u, cur, lc, w->nb);
            for (int j = 0; j < cnt; j++)
            {
                float d = dist(u, q, vec_of(u, w->nb[j]));
                if (d < dcur)
                    dcur = d, cur = w->nb[j], changed = 1;
            }
        }
    }
    for (int lc = l < u->max_level ? l : u->max_level; lc >= 1; lc--)
    {
        size_t nw = 0;
        if (!search_layer(w, q, cur, lc, &nw))
            return 0;
        connect(w, me, lc, nw);
        for (size_t i = 0; i < nw; i++)
            if (w->w[i].id != me)
            {
                cur = w->w[i].id;
                break;
            }
    }
    return 1;
}

static void *
worker_main(void *arg)
{
    Worker *w = arg;
    Upper *u = w->u;
    unsigned done = 0;
    for (;;)
    {
        if (atomic_load(&u->cancel) || atomic_load(&u->failed))
            break;
        uint32_t k = (uint32_t)atomic_fetch_add(&u->next, 1);
        if (k >= u->U)
            break;
        if (k == u->entry)
            continue;
        if (!insert_one(w, k))
        {
            atomic_store(&u->failed, 1);
            break;
        }
        if (u->cancel_cb && ++done % UPPER_CANCEL_EVERY == 0 && u->cancel_cb(u->cancel_arg))
            atomic_store(&u->cancel, 1);
    }
    return NULL;
}

/* ---- the file ---- */
typedef struct Out
{
    int    fd;
    size_t len;
    int    bad;
    char   buf[1 << 16];
} Out;

static void
out_flush(Out *o)
{
    size_t off = 0;
    while (!o->bad && off < o->len)
    {
        ssize_t n = write(o->fd, o->buf + off, o->len - off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            o->bad = 1;
        else
            off += (size_t)n;
    }
    o->len = 0;
}

static void
out_put(Out *o, const void *p, size_t len)
{
    const char *s = p;
    while (len > 0 && !o->bad)
    {
        size_t room = sizeof(o->buf) - o->len, take = len < room ? len : room;
        memcpy(o->buf + o->len, s, take);
        o->len += take; s += take; len -= take;
        if (o->len == sizeof(o->buf))
            out_flush(o);
    }
}

static int
write_file(Upper *u, int fd, const uint32_t *graph, int stride, int degree, int ef_construction,
           const int *level_of_row, const int32_t *local_of_row, uint64_t entry_row)
{
    const uint64_t M = (uint64_t)u->M, maxM0 = (uint64_t)degree;
    const uint64_t links0 = maxM0 * 4 + 4, data_size = (uint64_t)u->dim * 4;
    UpperFileHeader h;
    Out *o = malloc(sizeof(Out));
    uint32_t *rec = malloc((size_t)links0);
    int ok;

    if (!o || !rec)
    {
        free(o); free(rec);
        return 0;
    }
    memset(&h, 0, sizeof(h));
    h.offsetLevel0 = 0;
    h.max_elements = u->n;
    h.cur_element_count = u->n;
    h.sizeDataPerElement = links0 + data_size + sizeof(uint64_t);
    h.labelOffset = links0 + data_size;
    h.offsetData = links0;
    h.maxlevel = u->max_level;
    h.enterpointNode = (int32_t)entry_row;
    h.maxM = M;
    h.maxM0 = maxM0;
    h.M = M;
    h.mult = 1.0 / log((double)M);
    h.efConstruction = (uint64_t)ef_construction > M ? (uint64_t)ef_construction : M;
    o->fd = fd; o->len = 0; o->bad = 0;
    out_put(o, &h, sizeof(h));

    for (uint64_t i = 0; i < u->n; i++)
    {
        const uint32_t *row = graph + i * (uint64_t)stride;
        uint32_t cnt = 0;
        memset(rec, 0, (size_t)links0);
        for (int j = 0; j < degree; j++)
        {
            uint32_t c = row[j];
            int dup = c >= u->n || c == (uint32_t)i;
            for (uint32_t k = 0; k < cnt && !dup; k++)
                dup = rec[1 + k] == c;
            if (!dup)
                rec[1 + cnt++] = c;
        }
        rec[0] = cnt;
        uint64_t label = i;
        out_put(o, rec, (size_t)links0);
        out_put(o, u->vecs + i * (uint64_t)u->dim, (size_t)data_size);
        out_put(o, &label, sizeof(label));
    }
    for (uint64_t i = 0; i < u->n; i++)
    {
        int lv = level_of_row[i];
        uint32_t size = lv > 0 ? (uint32_t)((M + 1) * 4 * (uint64_t)lv) : 0;
        out_put(o, &size, sizeof(size));
        for (int lc = 1; lc <= lv; lc++)
        {
            const uint32_t *l = list_of(u, (uint32_t)local_of_row[i], lc);
            memset(rec, 0, (size_t)((M + 1) * 4));
            rec[0] = l[0];
            for (uint32_t k = 0; k < l[0]; k++)
                rec[1 + k] = u->row_of[l[1 + k]];
            out_put(o, rec, (size_t)((M + 1) * 4));
        }
    }
    out_flush(o);
    ok = !o->bad;
    free(o);
    free(rec);
    return ok;
}

int
cuvs_hnsw_upper_write(int out_fd, const uint32_t *graph, int graph_stride, int degree,
                      const float *vecs, uint64_t n, int dim, int inner_product,
                      int ef_construction, int threads,
                      CuvsHnswUpperCancel cancel, void *cancel_arg,
                      CuvsHnswUpperStats *stats, char *error, size_t error_len)
{
    Upper *u = NULL;
    int *level_of_row = NULL;
    int32_t *local_of_row = NULL;
    Worker *ws = NULL;
    pthread_t *tids = NULL;
    int rc = CUVS_HNSW_UPPER_ERROR, nthreads = 0, started = 0;
    uint64_t entry_row = 0, t0;

    memset(stats, 0, sizeof(*stats));
    if (error_len)
        error[0] = '\0';
    if (!graph || !vecs || n == 0 || n > UINT32_MAX || dim <= 0 || degree < 4 || degree % 2 != 0 ||
        degree > graph_stride || degree / 2 > 255)
    {
        snprintf(error, error_len, "invalid upper-layer build input");
        return CUVS_HNSW_UPPER_ERROR;
    }
    u = calloc(1, sizeof(Upper));
    level_of_row = malloc((size_t)n * sizeof(int));
    local_of_row = malloc((size_t)n * sizeof(int32_t));
    if (!u || !level_of_row || !local_of_row)
        goto oom;
    u->n = n; u->dim = dim; u->M = degree / 2; u->vecs = vecs; u->ip = inner_product ? 1 : 0;
    u->ef = ef_construction > u->M ? ef_construction : u->M;
    u->cancel_cb = cancel; u->cancel_arg = cancel_arg;

    /* Levels in row order from the conversion's seed. */
    {
        unsigned int seed = (unsigned int)(n * 31337u ^ (uint64_t)u->M * 13u);
        uint64_t total = 0;
        u->max_level = 0;
        for (uint64_t i = 0; i < n; i++)
        {
            int lv = cuvs_hnsw_upper_level(u->M, &seed);
            level_of_row[i] = lv;
            local_of_row[i] = lv > 0 ? (int32_t)u->U++ : -1;
            total += (uint64_t)lv;
            if (lv > u->max_level)
                u->max_level = lv, entry_row = i;
        }
        if (u->U > 0)
        {
            u->row_of = malloc((size_t)u->U * sizeof(uint32_t));
            u->level_of = malloc((size_t)u->U);
            u->link_off = malloc((size_t)u->U * sizeof(size_t));
            u->links = calloc((size_t)total * (size_t)(u->M + 1), sizeof(uint32_t));
            if (!u->row_of || !u->level_of || !u->link_off || !u->links)
                goto oom;
            size_t off = 0;
            for (uint64_t i = 0; i < n; i++)
                if (local_of_row[i] >= 0)
                {
                    uint32_t l = (uint32_t)local_of_row[i];
                    u->row_of[l] = (uint32_t)i;
                    u->level_of[l] = (uint8_t)level_of_row[i];
                    u->link_off[l] = off;
                    off += (size_t)level_of_row[i] * (size_t)(u->M + 1);
                }
            u->entry = (uint32_t)local_of_row[entry_row];
        }
    }

    t0 = mono_us();
    if (u->U > 1)
    {
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        nthreads = threads > 0 ? threads : (int)(cpus > UPPER_MAX_THREADS ? UPPER_MAX_THREADS : (cpus > 0 ? cpus : 1));
        if ((uint32_t)nthreads > u->U)
            nthreads = (int)u->U;
        for (int i = 0; i < UPPER_LOCK_STRIPES; i++)
            pthread_mutex_init(&u->locks[i], NULL);
        atomic_init(&u->next, 0);
        atomic_init(&u->cancel, 0);
        atomic_init(&u->failed, 0);
        ws = calloc((size_t)nthreads, sizeof(Worker));
        tids = calloc((size_t)nthreads, sizeof(pthread_t));
        if (!ws || !tids)
            goto oom_locks;
        for (int t = 0; t < nthreads; t++)
        {
            Worker *w = &ws[t];
            w->u = u;
            w->visited = calloc(u->U, sizeof(uint32_t));
            w->capcand = (size_t)u->ef * 4 + (size_t)u->M + 1;
            w->cand = malloc(w->capcand * sizeof(Cand));
            w->res = malloc(((size_t)u->ef + 2) * sizeof(Cand));
            w->w = malloc(((size_t)u->ef + 2) * sizeof(Cand));
            w->nb = malloc((size_t)(u->M + 1) * sizeof(uint32_t));
            w->sel = malloc((size_t)(u->M + 1) * sizeof(uint32_t));
            w->prune = malloc((size_t)(u->M + 2) * sizeof(Cand));
            if (!w->visited || !w->cand || !w->res || !w->w || !w->nb || !w->sel || !w->prune)
                goto oom_workers;
        }
        for (int t = 1; t < nthreads; t++)
        {
            if (pthread_create(&tids[t], NULL, worker_main, &ws[t]) != 0)
                break;
            started = t;
        }
        worker_main(&ws[0]);
        for (int t = 1; t <= started; t++)
            pthread_join(tids[t], NULL);
    }
    stats->build_us = mono_us() - t0;
    stats->upper_nodes = u->U;
    stats->max_level = u->max_level;
    stats->threads = nthreads;

    if (u->U > 1 && atomic_load(&u->failed))
    {
        snprintf(error, error_len, "out of memory in the upper-layer search");
        rc = CUVS_HNSW_UPPER_ERROR;
    }
    else if (u->U > 1 && atomic_load(&u->cancel))
        rc = CUVS_HNSW_UPPER_CANCELED;
    else
    {
        uint64_t t1 = mono_us();
        if (cancel && cancel(cancel_arg))
            rc = CUVS_HNSW_UPPER_CANCELED;
        else if (!write_file(u, out_fd, graph, graph_stride, degree, ef_construction,
                             level_of_row, local_of_row, entry_row))
        {
            snprintf(error, error_len, "cannot write the hnswlib file: %s", strerror(errno));
            rc = CUVS_HNSW_UPPER_ERROR;
        }
        else
            rc = CUVS_HNSW_UPPER_OK;
        stats->write_us = mono_us() - t1;
    }

oom_workers:
    if (ws)
        for (int t = 0; t < nthreads; t++)
        {
            free(ws[t].visited); free(ws[t].cand); free(ws[t].res); free(ws[t].w);
            free(ws[t].nb); free(ws[t].sel); free(ws[t].prune);
        }
oom_locks:
    if (u && u->U > 1)
        for (int i = 0; i < UPPER_LOCK_STRIPES; i++)
            pthread_mutex_destroy(&u->locks[i]);
    if (rc == CUVS_HNSW_UPPER_ERROR && !error[0])
        snprintf(error, error_len, "out of memory for the upper layers");
    free(ws);
    free(tids);
    if (u)
    {
        free(u->row_of); free(u->level_of); free(u->link_off); free(u->links);
    }
    free(u);
    free(level_of_row);
    free(local_of_row);
    return rc;

oom:
    snprintf(error, error_len, "out of memory for the upper layers");
    if (u)
    {
        free(u->row_of); free(u->level_of); free(u->link_off); free(u->links);
    }
    free(u);
    free(level_of_row);
    free(local_of_row);
    return CUVS_HNSW_UPPER_ERROR;
}
