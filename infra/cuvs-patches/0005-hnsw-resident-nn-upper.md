# 0005: resident NN-descent upper levels

## Pinned source and composition

Apply after 0001~0004 on cuVS v26.04.00, commit `60b19a0f62ba0bd716de349382828e43be6e2daa`. The patch changes `hnsw.hpp`, `hnsw.cpp`, `hnsw_precision.hpp`, and adds `cuvs_hnsw_nn_upper_contract.h`. It does not change public headers or `cagra_build.cuh`.

The patch body was first written against the B3 consumer snapshot `616eb202e83f558b59755b521d4d949e5b7b8305`. For the four files it touches, that snapshot is byte-identical to upstream plus 0001~0004 (manifest `patch4_sha256` values), so the same hunks apply to the canonical patch set. B3 also changes `cagra_build.cuh` (row-chunk graph consumer). That change is **not** part of this patch set.

## What it does

- A resident (non-disk) hierarchy build with `build_algo=nn_descent` passes the product host/GPU budgets to the upper-level `all_neighbors_graph` call and selects NN-descent there.
- The product carries the base NN dtype through two impossible ACE partition counts (`PGC_UVS_NN_UPPER_FP16 = SIZE_MAX`, `PGC_UVS_NN_UPPER_FP32 = SIZE_MAX - 1`, `cuvs_hnsw_nn_upper_contract.h`). The upper level uses FP32 when the base used FP32 or when a level's values do not fit half.
- The existing six-argument `all_neighbors_graph` instantiations stay exported as forwarders, so older callers keep linking.
- Auto, IVF-PQ, and ACE builds never set the private signal and keep the 0004 behavior.

## Marker

`pgcuvs_hnsw_resident_nn_upper_patch_id()` returns `pgcuvs-hnsw-resident-nn-upper-v3@<digest>`. The digest is the SHA-256 of the concatenated post-0005 bytes of `hnsw.hpp`, `hnsw_precision.hpp`, and `cuvs_hnsw_nn_upper_contract.h`, currently `9a18eb49f1e124f9c7806033a1544ce8d50a25bbc4164a22f54d0d441c1b8746`. The marker lives in `hnsw.cpp`, which is excluded from the digest to avoid self-reference. Earlier builds used the suffix `@616eb202...`, which named the pre-patch snapshot rather than the patch content. Those DSOs are rejected by the current product and must be rebuilt.

## Verification

`make test-hnsw-nn-sdk CUVS_UPSTREAM_GIT=<cuvs clone containing 60b19a0f>` prepares upstream+0001~0005 with `scripts/prepare-patched-cuvs-source.sh`, checks every manifest `final_sha256`, and runs the CPU source/fixture tests for this patch. CUDA compilation and GPU execution are separate.
