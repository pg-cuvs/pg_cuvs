# 0006: CAGRA graph-consumer row sort (B3)

## Provenance

This is the B3 SDK change (`SDK-B3-FINAL/B3-vs-canonical-sdk.diff`, SHA-256 `3acd107a74bd0fc29ee72e2e206558f86d4bc374e506775af60496b87ae48fa4`, analysis commit `616eb202e83f558b59755b521d4d949e5b7b8305`). It changes only `cagra_build.cuh` and adds `consumer_row_sort_plan.hpp`. It applies after 0001~0005 on cuVS v26.04.00 `60b19a0f`. The result is `cagra_build.cuh` `38722f03…95bc` and `consumer_row_sort_plan.hpp` `07b7b724…ac49`, the same bytes the B3 build and the product wrapper consumed.

Applying 0001~0006 to upstream reproduces the B3+NN SDK source tree byte-for-byte except `hnsw.cpp`. The only difference there is the 0005 marker value, which is a content digest instead of `@616eb202`.

## Behavior

The fixed public IVF final select returns unsorted rows (`sorted=false`), and K<=256 and K>256 use different selectors. The no-refine CAGRA consumer used the returned positions as neighbor ranks. 0006 sorts each row's (float score, int64 ID) pairs before that consumption and chunks the CUB temp allocation by complete rows within the actual workspace. A row that does not fit is an explicit RMM error, with no hidden retry. IVF selection, training, dtype and seeds are unchanged.

## Dependency

The B3 build also used `raft/0001-raft-select-k-key-bitwidth.patch` on RAFT's `select_k-inl.cuh`. The recipe applies it to the CPM RAFT source after configure (see README). It is a separate dependency patch, not part of the cuVS source tree.

## Verification

`make test-hnsw-b3-sdk CUVS_UPSTREAM_GIT=<cuvs clone>` rebuilds upstream+0001~0006 and runs the planner verifier, its mutations, and the B3 planner equivalence against the `level-0005` canonical header. Native evidence of the behavior is in SDK-B3-FINAL and is not re-run by CPU tests.
