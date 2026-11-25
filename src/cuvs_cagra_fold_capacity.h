#ifndef CUVS_CAGRA_FOLD_CAPACITY_H
#define CUVS_CAGRA_FOLD_CAPACITY_H

#include "cuvs_admission_contract.h"

#define CUVS_CAGRA_FOLD_BATCH_ROWS 64u
#define CUVS_CAGRA_FOLD_WORKSPACE_MARGIN (UINT64_C(64) << 20)

/* Incremental device claim. The source graph is accounted separately.
 * Keep the measured reserve floor, and cover the explicit allocations in
 * cuvs_cagra_fold_reuse_with_yield, add_graph_nodes_beta3a and its per-batch
 * search. Size both graphs even if their stream lifetimes do not overlap. */
static inline uint64_t
cuvs_cagra_fold_peak_bytes(uint64_t base, uint64_t added, uint64_t dim,
                           uint64_t stride, uint64_t degree, uint64_t *retained)
{
    uint64_t total, chunk, graph, dataset, input, touched, dirty, query_row, arrays, floor;

    *retained = CUVS_ADMISSION_BYTES_SATURATED;
    if (!base || !added || !dim || stride < dim || !degree ||
        base >= UINT32_MAX || added >= UINT32_MAX - base)
        return *retained;

    total = base + added;
    chunk = added < CUVS_CAGRA_FOLD_BATCH_ROWS ? added : CUVS_CAGRA_FOLD_BATCH_ROWS;
    graph = cuvs_admission_sat_mul(total, cuvs_admission_sat_mul(degree, sizeof(uint32_t)));
    dataset = cuvs_admission_sat_mul(total, cuvs_admission_sat_mul(stride, sizeof(float)));
    input = cuvs_admission_sat_mul(added, cuvs_admission_sat_mul(dim, sizeof(float)));
    touched = cuvs_admission_sat_mul(chunk, degree / 2 + 1);
    dirty = cuvs_admission_sat_mul(touched,
        cuvs_admission_sat_mul(cuvs_admission_sat_add(degree, 1), sizeof(uint32_t)));

    /* 2*degree ranks and distances, plus packed query and staging rows. */
    query_row = cuvs_admission_sat_add(cuvs_admission_sat_mul(degree, 4 * sizeof(uint32_t)),
                                      cuvs_admission_sat_mul(stride, 2 * sizeof(float)));
    *retained = cuvs_admission_sat_add(dataset, graph);
    arrays = cuvs_admission_sat_add(*retained, input);
    arrays = cuvs_admission_sat_add(arrays, graph);
    arrays = cuvs_admission_sat_add(arrays, dirty);
    arrays = cuvs_admission_sat_add(arrays, cuvs_admission_sat_mul(total, sizeof(int)));
    arrays = cuvs_admission_sat_add(arrays, cuvs_admission_sat_mul(chunk, query_row));
    arrays = cuvs_admission_sat_add(arrays, CUVS_CAGRA_FOLD_WORKSPACE_MARGIN);
    floor = cuvs_admission_needed_bytes(*retained, 0, 410, CUVS_CAGRA_FOLD_WORKSPACE_MARGIN);
    return arrays > floor ? arrays : floor;
}

#endif
