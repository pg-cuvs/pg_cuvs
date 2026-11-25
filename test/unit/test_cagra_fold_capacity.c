#include "cuvs_cagra_fold_capacity.h"

#include <assert.h>
#include <stdio.h>

int
main(void)
{
    uint64_t retained;
    uint64_t peak = cuvs_cagra_fold_peak_bytes(512, 4096, 12000, 12000, 64, &retained);

    /* The wrapper's old and new device matrices use different row counts.
     * While adding, the added input, full output dataset and working graph
     * necessarily coexist, even before counting the scratch arrays. */
    const uint64_t required = UINT64_C(4096) * 12000 * 4 +
        UINT64_C(4608) * (12000 + 64) * 4;
    assert(retained == UINT64_C(4608) * (12000 + 64) * 4);
    assert(peak >= required + (UINT64_C(64) << 20));

    /* Small padded shape, including both adjacencies, one chunk's dirty
     * entries and IDs, incoming degrees, rank output and query staging. */
    peak = cuvs_cagra_fold_peak_bytes(512, 128, 33, 36, 64, &retained);
    assert(retained == UINT64_C(640) * (36 + 64) * 4);
    assert(peak >= retained + UINT64_C(128) * 33 * 4 + UINT64_C(640) * 64 * 4 +
                   UINT64_C(64) * 33 * (64 + 1) * 4 + UINT64_C(640) * 4 +
                   UINT64_C(64) * (4 * 64 + 2 * 36) * 4 + (UINT64_C(64) << 20));

    assert(cuvs_cagra_fold_peak_bytes(1, 2, 9, 8, 64, &retained) == CUVS_ADMISSION_BYTES_SATURATED);
    assert(cuvs_cagra_fold_peak_bytes(UINT64_MAX, 2, 8, 8, 64, &retained) == CUVS_ADMISSION_BYTES_SATURATED);
    assert(cuvs_cagra_fold_peak_bytes(UINT32_MAX - 2, 1, UINT32_MAX, UINT32_MAX,
                                    UINT32_MAX, &retained) == CUVS_ADMISSION_BYTES_SATURATED);
    puts("PASS incremental fold bounds and overflow refusal");
    return 0;
}
