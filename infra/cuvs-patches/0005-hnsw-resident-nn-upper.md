# 0005: resident NN-descent upper levels

## Pinned source and composition

Apply after 0001~0004 on cuVS v26.04.00, commit `60b19a0f62ba0bd716de349382828e43be6e2daa`. The patch changes `hnsw.hpp`, `hnsw.cpp`, `hnsw_precision.hpp`, and adds `cuvs_hnsw_nn_upper_contract.h`. It does not change public headers or `cagra_build.cuh`.

The patch body was first written against the SDK snapshot the product was measured with. For the four files it touches, that snapshot is byte-identical to upstream plus 0001~0004 (manifest `patch4_sha256` values), so the same hunks apply to the canonical patch set. That snapshot also changes `cagra_build.cuh` (graph-consumer row sort); that change is patch 0006, not this one.

## What it does

- A resident (non-disk) hierarchy build with `build_algo=nn_descent` passes the product host/GPU budgets to the upper-level `all_neighbors_graph` call and selects NN-descent there.
- The product carries the base NN dtype through two impossible ACE partition counts (`PGC_UVS_NN_UPPER_FP16 = SIZE_MAX`, `PGC_UVS_NN_UPPER_FP32 = SIZE_MAX - 1`, `cuvs_hnsw_nn_upper_contract.h`). The upper level uses FP32 when the base used FP32 or when a level's values do not fit half.
- The existing six-argument `all_neighbors_graph` instantiations stay exported as forwarders, so older callers keep linking.
- Auto, IVF-PQ, and ACE builds never set the private signal and keep the 0004 behavior.
- Upper-level centering (same rule as the product's L2 base-graph centering). `from_cagra<GPU>` gathers each level's points from hnswlib's original vectors into `host_query_set` and calls `all_neighbors_graph`, which calls `nn_descent::build`. NN-descent then evaluates L2 as `||a||^2 + ||b||^2 - 2a.b` in float32 (`nn_descent.cuh` `calculate_metric`). On the resident NN path with an L2 metric, `all_neighbors_graph` now passes `nn_descent::build` a copy of that level's set centered by its own float64 column mean (`upper_precision::center_columns`). That copy is counted in the level's host workspace check. Only neighbor indices come back, so hnswlib's stored vectors and all search distances are unchanged. IP/cosine and non-resident calls are not centered.

Earlier contents of 0005 (digest `9a18eb49…`) did not center the upper levels.

## Marker

`pgcuvs_hnsw_resident_nn_upper_patch_id()` returns `pgcuvs-hnsw-resident-nn-upper-v3@<digest>`. The digest is the SHA-256 of the concatenated post-0005 bytes of `hnsw.hpp`, `hnsw_precision.hpp`, and `cuvs_hnsw_nn_upper_contract.h`, currently `e2bcd729e91b2c332dddd3981460469e45d43cc946fe5b2e93d5e49253d24276`. The marker lives in `hnsw.cpp`, which is excluded from the digest to avoid self-reference. Earlier builds used a suffix that named the pre-patch snapshot rather than the patch content. Those DSOs are rejected by the current product and must be rebuilt.

## Verification

`make test-hnsw-nn-sdk CUVS_UPSTREAM_GIT=<cuvs clone containing 60b19a0f>` prepares upstream+0001~0005 with `scripts/prepare-patched-cuvs-source.sh`, checks every manifest `final_sha256`, and runs the CPU source/fixture tests for this patch. CUDA compilation and GPU execution are separate.
