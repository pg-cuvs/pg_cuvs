#include "cuvs_cagra_fold.h"

int
cuvs_cagra_fold_shape(CuvsCagraIndex source, CuvsCagraFoldShape *shape)
{
    (void) source;
    (void) shape;
    return -1;
}

CuvsCagraFoldStatus
cuvs_cagra_fold_reuse(CuvsCagraIndex source, const float *new_vectors,
                      int64_t n_new, int dim, int device_id, CuvsCagraIndex *result)
{
    return cuvs_cagra_fold_reuse_with_yield(source, new_vectors, n_new, dim, device_id,
                                          result, NULL);
}

CuvsCagraFoldStatus
cuvs_cagra_fold_reuse_with_yield(CuvsCagraIndex source, const float *new_vectors,
                                 int64_t n_new, int dim, int device_id, CuvsCagraIndex *result,
                                 const CuvsCagraFoldControl *control)
{
    (void)source;
    (void)new_vectors;
    (void)n_new;
    (void)dim;
    (void)device_id;
    (void)result;
    (void)control;
    return CUVS_CAGRA_FOLD_UNAVAILABLE;
}
