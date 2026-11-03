/*
 * GPU smoke for the RMM peak meter (r5): an allocation that the pool quota
 * refuses must not raise the peak; a later successful one must. Built on the
 * dev VM against cuvs_wrapper.o (see the PR body for the command).
 *   ./gpu_peak_oom   -> prints PASS/FAIL lines, exit 0 iff all PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "cuvs_ipc.h"
#include "cuvs_wrapper.h"

int
main(void)
{
    int fails = 0;
    /* quota = 1% of device memory (about 485 MB on a 48 GB A6000) */
    if (cuvs_rmm_pool_init(0, 64u << 20, 1) != 0) { printf("FAIL pool init\n"); return 2; }
    size_t base = cuvs_rmm_pool_peak_bytes(0);

    int64_t n_big = 600000; int dim = 256;            /* 614 MB of float32 */
    float *big = calloc((size_t)n_big * dim, sizeof(float));
    CuvsBfIndex h = cuvs_bf_build(big, n_big, dim, CUVS_METRIC_L2, 0, 0);
    size_t after_fail = cuvs_rmm_pool_peak_bytes(0);
    printf("%s big build refused by the quota (handle=%p)\n", h ? "FAIL" : "PASS", (void *)h);
    fails += h != NULL;
    printf("%s peak after the refused allocation %zu < 614 MB (base %zu)\n",
           after_fail < (size_t)n_big * dim * 4 ? "PASS" : "FAIL", after_fail, base);
    fails += !(after_fail < (size_t)n_big * dim * 4);
    free(big);

    int64_t n_small = 10000;                           /* 10 MB */
    float *small = calloc((size_t)n_small * dim, sizeof(float));
    CuvsBfIndex s = cuvs_bf_build(small, n_small, dim, CUVS_METRIC_L2, 0, 0);
    size_t after_ok = cuvs_rmm_pool_peak_bytes(0);
    printf("%s small build succeeds and the peak covers it (%zu >= %zu)\n",
           s && after_ok >= (size_t)n_small * dim * 4 ? "PASS" : "FAIL", after_ok,
           (size_t)n_small * dim * 4);
    fails += !(s && after_ok >= (size_t)n_small * dim * 4);
    if (s) cuvs_bf_free(s, 0);
    free(small);
    printf("gpu_peak_oom: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
