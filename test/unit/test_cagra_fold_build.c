#include "cuvs_cagra_fold.h"
#include "cuvs_lw_policy.h"

#include <math.h>
#include <stdio.h>

enum { ROWS=512, ADD=32, DIM=32 };
static float corpus[ROWS*DIM], added[ADD*DIM];
int test_cagra_fold_early_oom(CuvsCagraIndex source, const float *added, int dim);
int test_cagra_fold_rank_oom(CuvsCagraIndex source, const float *added, int dim);

static int in_range(CuvsCagraIndex index, const float *query, int64_t rows)
{
    CuvsSearchResult results[2];
    if (cuvs_cagra_search(index, query, DIM, 2, results, 0, 0, 0))
        return 0;
    for (int i=0; i<2; ++i)
        if (results[i].item_id < 0 || results[i].item_id >= rows)
            return 0;
    return 1;
}

int main(void)
{
    int passed=0;
    for (int i=0; i<ROWS*DIM; ++i)
        corpus[i]=sinf(i*.041f)+cosf(i*.037f);
    for (int i=0; i<ADD*DIM; ++i)
        added[i]=sinf(i*.051f)+cosf(i*.113f);
    CuvsCagraIndex base=cuvs_cagra_build(corpus,ROWS,DIM,CUVS_METRIC_L2,64,128,
                                      CUVS_CAGRA_BUILD_AUTO,0,CUVS_LW_OVERFLOW_ERROR,NULL);
    CuvsCagraIndex next=NULL;
    if (!base || !in_range(base,corpus,ROWS))
        goto done;
    if (CUVS_EXPECT_GPU_FOLD && (!test_cagra_fold_early_oom(base, added, DIM) ||
                                !test_cagra_fold_rank_oom(base, added, DIM) || !in_range(base, corpus, ROWS)))
        goto done;
    CuvsCagraFoldStatus status=cuvs_cagra_fold_reuse(base,added,ADD,DIM,0,&next);
    if (CUVS_EXPECT_GPU_FOLD) {
        CuvsCagraFoldShape old_shape, new_shape;
        if (status!=CUVS_CAGRA_FOLD_OK || !next || next==base ||
            cuvs_cagra_fold_shape(base,&old_shape) != 0 || cuvs_cagra_fold_shape(next,&new_shape) != 0 ||
            old_shape.rows != ROWS || new_shape.rows != ROWS+ADD || old_shape.dim != DIM ||
            new_shape.dim != DIM || old_shape.stride < DIM || new_shape.stride != old_shape.stride ||
            !old_shape.degree || new_shape.degree != old_shape.degree ||
            !in_range(base,corpus,ROWS))
            goto done;
        cuvs_cagra_free(base,0);
        base=NULL;
        passed=in_range(next,added,ROWS+ADD);
    } else {
        CuvsCagraIndex occupied=base;
        passed=(status==CUVS_CAGRA_FOLD_UNAVAILABLE && !next &&
                cuvs_cagra_fold_reuse(base,added,ADD,DIM,0,&occupied)==CUVS_CAGRA_FOLD_UNAVAILABLE &&
                occupied==base && in_range(base,corpus,ROWS));
    }
    if (passed)
        printf("PASS build/search/free, fold mode=%d status=%d\n",CUVS_EXPECT_GPU_FOLD,status);

done:
    if (next!=base) cuvs_cagra_free(next,0);
    cuvs_cagra_free(base,0);
    if (!passed) fputs("CAGRA fold build smoke failed\n",stderr);
    return passed ? 0 : 1;
}
