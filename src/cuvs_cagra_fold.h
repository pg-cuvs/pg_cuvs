#pragma once

#include "cuvs_wrapper.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CuvsCagraFoldStatus {
    CUVS_CAGRA_FOLD_OK = 0,
    CUVS_CAGRA_FOLD_INVALID = 1,
    CUVS_CAGRA_FOLD_OOM = 2,
    CUVS_CAGRA_FOLD_CUDA_UNCONFIRMED = 3,
    CUVS_CAGRA_FOLD_ERROR = 4,
    CUVS_CAGRA_FOLD_UNAVAILABLE = 5,
    CUVS_CAGRA_FOLD_YIELDED = 6
} CuvsCagraFoldStatus;

typedef struct CuvsCagraFoldPhases {
    uint64_t input_us;
    uint64_t dataset_us;
    uint64_t graph_setup_us;
    uint64_t incoming_us;
    uint64_t search_us;
    uint64_t rank_us;
    uint64_t reverse_us;
    uint64_t scatter_us;
    uint64_t final_graph_us;
    bool host_graph_cache_hit;
} CuvsCagraFoldPhases;

typedef struct CuvsCagraFoldControl {
    int (*should_yield)(void *context);
    void *context;
    /* Replace any unfilled row with this verified slot in the old graph. */
    const unsigned char *filled_rows;
    uint32_t placeholder_slot;
    bool optimized;
    /* Optional accumulator, initialized by the caller. May add stream fences. */
    CuvsCagraFoldPhases *phases;
    /* Optimized calls may borrow a host graph of (old + new) * degree entries.
     * Pass a cached device pointer only when the host prefix belongs to the
     * exact immutable source generation and shape. The fold overwrites the
     * host buffer even on failure. Only OK writes the output device pointer. */
    uint32_t *host_graph;
    const void *cached_device_graph;
    const void **result_device_graph;
} CuvsCagraFoldControl;

typedef struct CuvsCagraFoldShape {
    uint64_t rows;
    uint64_t dim;
    uint64_t stride;
    uint64_t degree;
} CuvsCagraFoldShape;

/* Metadata only. The caller holds the source live. Returns 0 on success. */
int cuvs_cagra_fold_shape(CuvsCagraIndex source, CuvsCagraFoldShape *shape);

/*
 * GPU fold for an in-memory, uncompressed CAGRA graph. source is borrowed,
 * immutable, and must stay alive through this synchronous call. new_vectors
 * points to n_new finite float32 rows in host memory, in graph-slot order.
 * The caller supplies the source's CUDA device. *result must initially be NULL.
 * Only OK writes *result: it owns an independent graph and dataset and is
 * released by cuvs_cagra_free. Failure leaves the slot and source unchanged.
 * OOM means a typed allocation failure with successful stream completion;
 * CUDA_UNCONFIRMED must not be used as a retryable memory refusal.
 * CPU shims and disabled native builds return UNAVAILABLE without reading
 * or changing either handle.
 */
CuvsCagraFoldStatus cuvs_cagra_fold_reuse(CuvsCagraIndex source,
                                          const float *new_vectors,
                                          int64_t n_new, int dim, int device_id,
                                          CuvsCagraIndex *result);

/* The optional callback only reads admission state. It must not block or
 * call CUDA. YIELDED confirms that the temporary fold is gone, the request
 * stream is healthy, and *result is still NULL. The control is borrowed
 * only during this synchronous call. */
CuvsCagraFoldStatus cuvs_cagra_fold_reuse_with_yield(CuvsCagraIndex source,
                                                    const float *new_vectors,
                                                    int64_t n_new, int dim, int device_id,
                                                    CuvsCagraIndex *result,
                                                    const CuvsCagraFoldControl *control);

#ifdef __cplusplus
}
#endif
