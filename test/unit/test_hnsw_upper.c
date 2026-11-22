/* cuvs_hnsw_upper_write, the daemon's host-built upper layers (CUVS_OP_EXPORT_HNSW_UPPER).
 * - one thread and four threads give the same file size and the same level per row, and the levels are the
 *   conversion's draw (cuvs_hnsw_upper_level from the (n, M) seed), so pages do not depend on thread timing;
 * - every element above level 0 has a level-1 link, and level 1 is one component (one thread);
 * - level 0 is the given graph without out-of-range ids, self links or duplicates;
 * - the cancel callback stops the build and nothing is written. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cuvs_hnsw_upper.h"

#define N   3000
#define DIM 16
#define DEG 32
#define M   (DEG / 2)

static float vecs[N * DIM];
static uint32_t graph[N * DEG];

typedef struct Parsed
{
    long size;
    int levels[N];
    int maxlevel, entry;
    int level1_unlinked, level1_components;
} Parsed;

static unsigned lcg = 12345;
static float
rnd(void)
{
    lcg = lcg * 1103515245u + 12345u;
    return (float)((lcg >> 8) & 0xffff) / 65536.0f;
}

static int find(int *p, int a) { while (p[a] != a) a = p[a] = p[p[a]]; return a; }

static void
parse(FILE *f, Parsed *out)
{
    uint64_t hdr[12];
    long sz;
    uint64_t sizeData, maxM0;
    int32_t ml, ep;

    fseek(f, 0, SEEK_END); sz = ftell(f); rewind(f);
    out->size = sz;
    assert(fread(hdr, 8, 6, f) == 6);
    assert(fread(&ml, 4, 1, f) == 1 && fread(&ep, 4, 1, f) == 1);
    assert(fread(hdr + 6, 8, 5, f) == 5);
    sizeData = hdr[3]; maxM0 = hdr[7];
    assert(hdr[1] == N && hdr[2] == N && hdr[6] == M && maxM0 == DEG && hdr[8] == M);
    out->maxlevel = ml; out->entry = ep;
    /* level 0 */
    uint32_t *rec = malloc(sizeData);
    for (int i = 0; i < N; i++)
    {
        assert(fread(rec, 1, sizeData, f) == sizeData);
        uint32_t cnt = rec[0];
        assert(cnt <= DEG);
        for (uint32_t a = 0; a < cnt; a++)
        {
            assert(rec[1 + a] < N && rec[1 + a] != (uint32_t)i);
            for (uint32_t b = 0; b < a; b++)
                assert(rec[1 + a] != rec[1 + b]);
        }
        uint64_t label;
        memcpy(&label, (char *)rec + sizeData - 8, 8);
        assert(label == (uint64_t)i);
    }
    /* upper lists */
    int *parent = malloc(N * sizeof(int));
    for (int i = 0; i < N; i++) parent[i] = i;
    int unlinked = 0;
    uint32_t *lists = malloc((size_t)N * 17 * (M + 1) * 4);
    int nup = 0;
    for (int i = 0; i < N; i++)
    {
        uint32_t s;
        assert(fread(&s, 4, 1, f) == 1);
        assert(s % ((M + 1) * 4) == 0);
        int lv = (int)(s / ((M + 1) * 4));
        out->levels[i] = lv;
        if (lv == 0)
            continue;
        nup++;
        uint32_t *l = malloc(s);
        assert(fread(l, 1, s, f) == s);
        /* level 1 is the first list */
        if (l[0] == 0)
            unlinked++;
        for (uint32_t k = 0; k < l[0]; k++)
        {
            assert(l[1 + k] < N && l[1 + k] != (uint32_t)i);
            int a = find(parent, i), b = find(parent, (int)l[1 + k]);
            if (a != b) parent[a] = b;
        }
        free(l);
    }
    assert(ftell(f) == sz);
    int comps = 0;
    for (int i = 0; i < N; i++)
        if (out->levels[i] > 0 && find(parent, i) == i)
            comps++;
    out->level1_unlinked = unlinked;
    out->level1_components = comps;
    free(parent); free(rec); free(lists);
    (void)nup;
}

static int stop_now(void *arg) { (void)arg; return 1; }

static void
run(int threads, int ip, Parsed *p)
{
    FILE *f = tmpfile();
    CuvsHnswUpperStats st;
    char err[160];
    assert(f);
    int rc = cuvs_hnsw_upper_write(fileno(f), graph, DEG, DEG, vecs, N, DIM, ip, 2 * DEG, threads,
                                   NULL, NULL, &st, err, sizeof(err));
    if (rc != CUVS_HNSW_UPPER_OK) fprintf(stderr, "rc=%d %s\n", rc, err);
    assert(rc == CUVS_HNSW_UPPER_OK);
    assert(st.threads == threads && st.upper_nodes > 0);
    parse(f, p);
    fclose(f);
}

int
main(void)
{
    static Parsed one, four, inner;
    for (int i = 0; i < N * DIM; i++)
        vecs[i] = rnd();
    /* exact k nearest neighbors as the level-0 graph */
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
        memcpy(graph + (size_t)i * DEG, id, sizeof(id));
    }
    /* row 0: a self link, a duplicate and an out-of-range id must not reach the file */
    graph[0] = 0; graph[1] = graph[2]; graph[3] = N + 7;

    run(1, 0, &one);
    run(4, 0, &four);
    assert(one.size == four.size);
    assert(memcmp(one.levels, four.levels, sizeof(one.levels)) == 0);
    assert(one.maxlevel == four.maxlevel && one.entry == four.entry);
    {
        unsigned seed = (unsigned)((uint64_t)N * 31337u ^ (uint64_t)M * 13u);
        int maxl = 0, entry = 0;
        for (int i = 0; i < N; i++)
        {
            int lv = cuvs_hnsw_upper_level(M, &seed);
            assert(one.levels[i] == lv);
            if (lv > maxl) maxl = lv, entry = i;
        }
        assert(one.maxlevel == maxl && one.entry == entry && maxl >= 1);
    }
    assert(one.level1_unlinked == 0 && four.level1_unlinked == 0);
    assert(one.level1_components == 1);
    printf("PASS levels, size and entry are the same on 1 and 4 threads (file %ld bytes, top level %d)\n",
           one.size, one.maxlevel);
    printf("PASS every element above level 0 has a level-1 link; level 1 is one component\n");
    printf("PASS level 0 drops out-of-range ids, self links and duplicates\n");

    for (int i = 0; i < N; i++)
    {
        float s = 0;
        for (int t = 0; t < DIM; t++) s += vecs[i * DIM + t] * vecs[i * DIM + t];
        s = 1.0f / sqrtf(s);
        for (int t = 0; t < DIM; t++) vecs[i * DIM + t] *= s;
    }
    run(2, 1, &inner);
    assert(inner.level1_unlinked == 0 && inner.size == one.size);
    printf("PASS inner product over unit rows builds the same layout\n");

    {
        FILE *f = tmpfile();
        CuvsHnswUpperStats st;
        char err[160];
        int rc = cuvs_hnsw_upper_write(fileno(f), graph, DEG, DEG, vecs, N, DIM, 1, 2 * DEG, 1,
                                       stop_now, NULL, &st, err, sizeof(err));
        assert(rc == CUVS_HNSW_UPPER_CANCELED);
        fseek(f, 0, SEEK_END);
        assert(ftell(f) == 0);
        fclose(f);
        printf("PASS a cancel stops the build and writes nothing\n");
    }
    printf("test_hnsw_upper: all checks passed\n");
    return 0;
}
