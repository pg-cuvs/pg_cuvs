/*
 * GPU check of the I3-A wrapper changes against the real cuVS wrapper (needs a
 * CUDA device; build on the dev VM, not part of make test-unit):
 *   1. dim % 4 != 0 builds attach a padded, non-owning dataset (v6 C3, CK-30):
 *      self-queries still return the row itself, and the VRAM held by a dim 7
 *      index is that of dim 8 (one copy), not dim 8 + a dim 7 duplicate.
 *   2. The dataset is attached: a successful build reports no attach failure.
 * Usage: test_wrapper_i3 [n_rows]   (default 500000)
 */
#include "cuvs_wrapper.h"
#include "cuvs_lw_policy.h"
#include "cuvs_ipc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int g_failures;
#define CHECK(label, cond)                                                  \
    do {                                                                    \
        if (!(cond)) { fprintf(stderr, "FAIL %s\n", (label)); g_failures++; } \
        else printf("ok   %s\n", (label));                                  \
    } while (0)

static float *
make_vecs(int64_t n, int dim)
{
    float *v = malloc((size_t) n * (size_t) dim * sizeof(float));
    unsigned s = 12345u;
    for (int64_t i = 0; i < n * dim; i++) {
        s = s * 1103515245u + 12345u;
        v[i] = (float) ((s >> 8) & 0xffff) / 65536.0f;
    }
    return v;
}

/* Build, measure the VRAM the index keeps, and self-query a few rows. */
static size_t
build_and_check(const float *v, int64_t n, int dim, const char *tag)
{
    char label[96];
    size_t before, after;
    int ok = 1;
    CuvsCagraIndex h;

    before = cuvs_vram_free_bytes_on(0);
    h = cuvs_cagra_build(v, n, dim, CUVS_METRIC_L2, 32, 64, CUVS_CAGRA_BUILD_NN_DESCENT, 0, CUVS_LW_OVERFLOW_ERROR, NULL);
    snprintf(label, sizeof(label), "%s build", tag);
    CHECK(label, h != NULL);
    snprintf(label, sizeof(label), "%s no attach failure", tag);
    CHECK(label, cuvs_last_build_attach_failed() == 0);
    if (!h)
        return 0;
    after = cuvs_vram_free_bytes_on(0);
    for (int64_t q = 0; q < 5; q++) {
        int64_t row = (q * 7919) % n;
        CuvsSearchResult r[4];
        if (cuvs_cagra_search(h, v + row * dim, dim, 4, r, 0, 64, 1) != 0 ||
            r[0].item_id != row || fabsf(r[0].distance) > 1e-3f)
            ok = 0;
    }
    snprintf(label, sizeof(label), "%s self-queries return the row", tag);
    CHECK(label, ok);
    printf("info %s held_bytes=%zu\n", tag, before > after ? before - after : 0);
    cuvs_cagra_free(h, 0);
    return before > after ? before - after : 0;
}

int
main(int argc, char **argv)
{
    int64_t n = argc > 1 ? atoll(argv[1]) : 500000;
    float *v7 = make_vecs(n, 7), *v8 = make_vecs(n, 8);
    size_t h8, h7;

    if (!cuvs_gpu_available()) {
        fprintf(stderr, "no CUDA device\n");
        return 2;
    }
    h8 = build_and_check(v8, n, 8, "dim8");
    h7 = build_and_check(v7, n, 7, "dim7");
    /* One padded copy: dim 7 holds what dim 8 holds. A duplicate contiguous
     * copy would add n*7*4 bytes; allow n*4 bytes of allocator noise. */
    printf("info n=%lld dim7_held=%zu dim8_held=%zu duplicate_would_add=%lld\n",
           (long long) n, h7, h8, (long long) n * 7 * 4);
    CHECK("dim7 keeps one dataset copy", h7 > 0 && h8 > 0 && h7 <= h8 + (size_t) n * 4);
    free(v7);
    free(v8);
    if (g_failures) {
        fprintf(stderr, "test_wrapper_i3: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_wrapper_i3: all checks passed\n");
    return 0;
}
