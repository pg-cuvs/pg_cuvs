/* cuvs_hnsw_upper_write, the daemon's host-built upper layers (CUVS_OP_EXPORT_HNSW_UPPER).
 * - one thread and four threads give the same file size and the same level per row, and the levels are the
 *   conversion's draw (cuvs_hnsw_upper_level from the (n, M) seed), so the page layout does not depend on thread
 *   timing;
 * - every list of every level holds at most M ids, none of them the element itself, no duplicate, and only
 *   elements whose level reaches that list's level (pgvector indexes the slots by (level - lc) * m);
 * - with two or more elements above level 0 each has a level-1 link, and level 1 is one component;
 * - level 0 is the first `degree` ids of each row of a wider graph, without out-of-range ids, self links or
 *   duplicates; degree 4 (M = 2), no upper element and a single one build too;
 * - a cancel asked by the building threads stops the build and nothing is written. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cuvs_hnsw_upper.h"

#define N    3000
#define DIM  16
#define DEG  32
#define WIDE 40     /* graph stride wider than the level-0 degree, as the daemon passes it */

static float vecs[N * DIM];
static uint32_t graph[N * WIDE];

typedef struct Parsed
{
    long size;
    int *levels;
    int  maxlevel, entry, upper;
    int  level1_unlinked, level1_components;
} Parsed;

static unsigned lcg = 12345;
static float
rnd(void)
{
    lcg = lcg * 1103515245u + 12345u;
    return (float)((lcg >> 8) & 0xffff) / 65536.0f;
}

static int find(int *p, int a) { while (p[a] != a) a = p[a] = p[p[a]]; return a; }

/* Reads the whole file and checks every invariant; level-0 ids must come from the first `degree` of `stride`. */
static void
parse(FILE *f, int n, int degree, int stride, Parsed *out)
{
    const int M = degree / 2;
    uint64_t hdr[12];
    long sz;
    int32_t ml, ep;

    fseek(f, 0, SEEK_END); sz = ftell(f); rewind(f);
    out->size = sz;
    assert(fread(hdr, 8, 6, f) == 6);
    assert(fread(&ml, 4, 1, f) == 1 && fread(&ep, 4, 1, f) == 1);
    assert(fread(hdr + 6, 8, 5, f) == 5);
    uint64_t sizeData = hdr[3];
    assert(hdr[1] == (uint64_t)n && hdr[2] == (uint64_t)n && hdr[6] == (uint64_t)M && hdr[7] == (uint64_t)degree &&
           hdr[8] == (uint64_t)M);
    double mult; memcpy(&mult, &hdr[9], 8);
    assert(isfinite(mult) && mult > 0);
    assert(ep >= 0 && ep < n && ml >= 0);
    out->maxlevel = ml; out->entry = ep;

    uint32_t *rec = malloc(sizeData);
    for (int i = 0; i < n; i++)
    {
        assert(fread(rec, 1, sizeData, f) == sizeData);
        uint32_t cnt = rec[0];
        assert(cnt <= (uint32_t)degree);
        for (uint32_t a = 0; a < cnt; a++)
        {
            uint32_t c = rec[1 + a];
            assert(c < (uint32_t)n && c != (uint32_t)i);
            for (uint32_t b = 0; b < a; b++)
                assert(c != rec[1 + b]);
            int in_first = 0;
            for (int j = 0; j < degree; j++)
                in_first |= graph[(size_t)i * stride + j] == c;
            assert(in_first);
        }
        uint64_t label;
        memcpy(&label, (char *)rec + sizeData - 8, 8);
        assert(label == (uint64_t)i);
    }
    out->levels = calloc((size_t)n, sizeof(int));
    uint32_t **lists = calloc((size_t)n, sizeof(uint32_t *));
    for (int i = 0; i < n; i++)
    {
        uint32_t s;
        assert(fread(&s, 4, 1, f) == 1);
        assert(s % ((M + 1) * 4) == 0);
        out->levels[i] = (int)(s / ((M + 1) * 4));
        if (s)
        {
            lists[i] = malloc(s);
            assert(fread(lists[i], 1, s, f) == s);
        }
    }
    assert(ftell(f) == sz);
    assert(out->levels[ep] == ml);

    int *parent = malloc((size_t)n * sizeof(int));
    for (int i = 0; i < n; i++) parent[i] = i;
    out->upper = out->level1_unlinked = 0;
    for (int i = 0; i < n; i++)
    {
        int lv = out->levels[i];
        if (lv == 0)
            continue;
        out->upper++;
        assert(lv <= ml);
        for (int lc = 1; lc <= lv; lc++)
        {
            const uint32_t *l = lists[i] + (size_t)(lc - 1) * (M + 1);
            assert(l[0] <= (uint32_t)M);
            if (lc == 1 && l[0] == 0)
                out->level1_unlinked++;
            for (uint32_t k = 0; k < l[0]; k++)
            {
                uint32_t c = l[1 + k];
                assert(c < (uint32_t)n && c != (uint32_t)i);
                assert(out->levels[c] >= lc);
                for (uint32_t b = 0; b < k; b++)
                    assert(c != l[1 + b]);
                if (lc == 1)
                {
                    int a = find(parent, i), b = find(parent, (int)c);
                    if (a != b) parent[a] = b;
                }
            }
        }
    }
    int comps = 0;
    for (int i = 0; i < n; i++)
        if (out->levels[i] > 0 && find(parent, i) == i)
            comps++;
    out->level1_components = comps;
    for (int i = 0; i < n; i++) free(lists[i]);
    free(lists); free(parent); free(rec);
}

static int calls;
static int stop_from_second(void *arg) { (void)arg; return ++calls >= 2; }

static int
build(int n, int degree, int stride, int threads, int ip, Parsed *p)
{
    FILE *f = tmpfile();
    CuvsHnswUpperStats st;
    char err[160];
    assert(f);
    int rc = cuvs_hnsw_upper_write(fileno(f), graph, stride, degree, vecs, (uint64_t)n, DIM, ip, 2 * degree,
                                   threads, NULL, NULL, &st, err, sizeof(err));
    if (rc != CUVS_HNSW_UPPER_OK) fprintf(stderr, "rc=%d %s\n", rc, err);
    assert(rc == CUVS_HNSW_UPPER_OK);
    parse(f, n, degree, stride, p);
    fclose(f);
    return (int)st.upper_nodes;
}

static int
upper_count(int n, int M)
{
    unsigned seed = (unsigned)((uint64_t)n * 31337u ^ (uint64_t)M * 13u);
    int u = 0;
    for (int i = 0; i < n; i++)
        u += cuvs_hnsw_upper_level(M, &seed) > 0;
    return u;
}

int
main(void)
{
    static Parsed one, four, inner, d4, u0, u1;
    for (int i = 0; i < N * DIM; i++)
        vecs[i] = rnd();
    /* exact k nearest neighbors in the first DEG columns; the columns past DEG hold valid ids that must not be used */
    for (int i = 0; i < N; i++)
    {
        float best[DEG]; uint32_t id[DEG]; int k = 0;
        for (int j = 0; j < N; j++)
        {
            if (j == i) continue;
            float d = 0;
            for (int t = 0; t < DIM; t++) { float x = vecs[i * DIM + t] - vecs[j * DIM + t]; d += x * x; }
            if (k < DEG) { best[k] = d; id[k++] = (uint32_t)j; }
            else
            {
                int w = 0;
                for (int t = 1; t < DEG; t++) if (best[t] > best[w]) w = t;
                if (d < best[w]) { best[w] = d; id[w] = (uint32_t)j; }
            }
        }
        /* nearest first, as CAGRA orders a row */
        for (int a = 1; a < DEG; a++)
            for (int b = a; b > 0 && best[b] < best[b - 1]; b--)
            {
                float td = best[b]; best[b] = best[b - 1]; best[b - 1] = td;
                uint32_t ti = id[b]; id[b] = id[b - 1]; id[b - 1] = ti;
            }
        memcpy(graph + (size_t)i * WIDE, id, sizeof(id));
        for (int j = DEG; j < WIDE; j++)
            graph[(size_t)i * WIDE + j] = (uint32_t)((i + 1 + j * 7) % N);
    }
    /* row 0: a self link, a duplicate and an out-of-range id must not reach the file */
    graph[0] = 0; graph[1] = graph[2]; graph[3] = N + 7;

    int U = build(N, DEG, WIDE, 1, 0, &one);
    build(N, DEG, WIDE, 4, 0, &four);
    assert(U > 1 && one.upper == U);
    assert(one.size == four.size);
    assert(memcmp(one.levels, four.levels, N * sizeof(int)) == 0);
    assert(one.maxlevel == four.maxlevel && one.entry == four.entry);
    {
        unsigned seed = (unsigned)((uint64_t)N * 31337u ^ (uint64_t)(DEG / 2) * 13u);
        int maxl = 0, entry = 0;
        for (int i = 0; i < N; i++)
        {
            int lv = cuvs_hnsw_upper_level(DEG / 2, &seed);
            assert(one.levels[i] == lv);
            if (lv > maxl) maxl = lv, entry = i;
        }
        assert(one.maxlevel == maxl && one.entry == entry && maxl >= 1);
    }
    assert(one.level1_unlinked == 0 && four.level1_unlinked == 0);
    assert(one.level1_components == 1 && four.level1_components == 1);
    printf("PASS levels, size and entry are the same on 1 and 4 threads (file %ld bytes, top level %d, %d upper)\n",
           one.size, one.maxlevel, U);
    printf("PASS every list of every level: at most M ids, no self, no duplicate, only elements of that level or above\n");
    printf("PASS every element above level 0 has a level-1 link; level 1 is one component on 1 and 4 threads\n");
    printf("PASS level 0 takes the first degree ids of a wider row, without out-of-range ids, self links or duplicates\n");

    build(N, 4, WIDE, 2, 0, &d4);
    assert(d4.level1_unlinked == 0);
    printf("PASS degree 4 (M = 2)\n");

    {
        int n0 = 0, n1 = 0;
        for (int n = 2; n < 200 && (!n0 || !n1); n++)
        {
            int u = upper_count(n, DEG / 2);
            if (u == 0 && !n0) n0 = n;
            if (u == 1 && !n1) n1 = n;
        }
        assert(n0 && n1);
        assert(build(n0, DEG, WIDE, 4, 0, &u0) == 0 && u0.upper == 0 && u0.maxlevel == 0);
        assert(build(n1, DEG, WIDE, 4, 0, &u1) == 1 && u1.upper == 1 && u1.maxlevel >= 1);
        assert(u1.levels[u1.entry] == u1.maxlevel && u1.level1_unlinked == 1);
        printf("PASS no upper element (n=%d) and a single one, the entry point with nobody to link to (n=%d)\n", n0, n1);
    }

    for (int i = 0; i < N; i++)
    {
        float s = 0;
        for (int t = 0; t < DIM; t++) s += vecs[i * DIM + t] * vecs[i * DIM + t];
        s = 1.0f / sqrtf(s);
        for (int t = 0; t < DIM; t++) vecs[i * DIM + t] *= s;
    }
    build(N, DEG, WIDE, 2, 1, &inner);
    assert(inner.level1_unlinked == 0 && inner.size == one.size);
    printf("PASS inner product over unit rows builds the same layout\n");

    {
        /* the first ask passes; the building threads ask again after 64 insertions and stop there. With the
         * threads' asks gone, only the ask before the file remains (the first one), and the build completes. */
        FILE *f = tmpfile();
        CuvsHnswUpperStats st;
        char err[160];
        calls = 0;
        int rc = cuvs_hnsw_upper_write(fileno(f), graph, WIDE, DEG, vecs, N, DIM, 1, 2 * DEG, 1,
                                       stop_from_second, NULL, &st, err, sizeof(err));
        assert(rc == CUVS_HNSW_UPPER_CANCELED);
        assert(calls == 2);
        fseek(f, 0, SEEK_END);
        assert(ftell(f) == 0);
        fclose(f);
        printf("PASS a cancel asked by a building thread stops the build and writes nothing\n");
    }
    printf("test_hnsw_upper: all checks passed\n");
    return 0;
}
