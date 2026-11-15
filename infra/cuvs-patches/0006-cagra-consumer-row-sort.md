# 0006: CAGRA graph-consumer row sort

## Provenance

This is the graph-consumer change of the SDK build the product was measured with (patch SHA-256 `3acd107a74bd0fc29ee72e2e206558f86d4bc374e506775af60496b87ae48fa4`). It changes only `cagra_build.cuh` and adds `consumer_row_sort_plan.hpp`, and it applies after 0001~0005 on cuVS v26.04.00 `60b19a0f`. The resulting files are:

- `cagra_build.cuh` `38722f03…95bc`
- `consumer_row_sort_plan.hpp` `07b7b724…ac49`

These are the same bytes that SDK build and the product wrapper consumed.

Applying 0001~0006 to upstream reproduces that SDK's source tree byte-for-byte except `hnsw.cpp`. In `hnsw.cpp` the only difference is the 0005 marker value: it is now a content digest instead of the earlier snapshot identifier.

## Behavior

The fixed public IVF final select returns unsorted rows (`sorted=false`), and K<=256 and K>256 use different selectors. The no-refine CAGRA consumer used the returned positions as neighbor ranks. 0006 changes this:

- Each row's (float score, int64 ID) pairs are sorted before the consumer uses them.
- The CUB temp allocation is chunked by complete rows within the actual workspace. A row that does not fit is an explicit RMM error, with no hidden retry.
- IVF selection, training, dtype and seeds are unchanged.

## Dependency

The same SDK build also used `raft/0001-raft-select-k-key-bitwidth.patch` on RAFT's `select_k-inl.cuh`. The recipe applies it to a private copy of the installed RAFT package, or to the CPM RAFT source (see README). It is a separate dependency patch, not part of the cuVS source tree.

## Verification

`make test-hnsw-row-sort-sdk CUVS_UPSTREAM_GIT=<cuvs clone>` rebuilds upstream+0001~0006. It then runs the planner verifier, its mutations, and the row-sort planner equivalence against the `level-0005` canonical header. CPU tests do not re-run the GPU behavior.

On the GPU, in single runs with 10000 queries on the 999000×768 corpus, product builds with intermediate degree 256 improved recall@10 at ef_search 10:

| Configuration | Without the sort | With the sort |
|---|---|---|
| configuration 1 (intermediate degree 256) | 0.537 | 0.841 |
| configuration 2 (intermediate degree 256, graph degree 32) | 0.620 | 0.940 |
