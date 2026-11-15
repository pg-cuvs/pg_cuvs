# 0006: CAGRA graph-consumer row sort (B3)

## Provenance and composition

This is the B3 SDK change (SHA-256 `3acd107a74bd0fc29ee72e2e206558f86d4bc374e506775af60496b87ae48fa4`, analysis commit `616eb202e83f558b59755b521d4d949e5b7b8305`). It changes only `cagra_build.cuh` and adds `consumer_row_sort_plan.hpp`, and applies on cuVS v26.04.00 `60b19a0f62ba0bd716de349382828e43be6e2daa`. The result is `cagra_build.cuh` `38722f03…95bc` and `consumer_row_sort_plan.hpp` `07b7b724…ac49`.

0006 touches none of the files that 0005 (resident NN-descent upper levels) touches (`hnsw.hpp`, `hnsw.cpp`, `hnsw_precision.hpp` and its own new header), so it gives the same `cagra_build.cuh` and `consumer_row_sort_plan.hpp` whether it follows 0001~0004 or 0001~0005. The recipe applies 0001 through 0007 in order.

Applying 0001~0004 and 0006 to the pinned tree gives a `cpp/` tree of 815 files that is byte-identical to the source archive recorded with the validated B3 build.

## Behavior

The fixed public IVF-PQ final select returns unsorted rows (`sorted=false`), and K<=256 and K>256 use different selectors. The no-refine CAGRA consumer used the returned positions as neighbor ranks. With intermediate degree 256 the search asks for K = 257, which crosses the RAFT select-k boundary at 256 (separate from the cuVS fused-select capacity of 128). 0006 sorts each row's (float score, int64 ID) pairs with `cub::DeviceSegmentedRadixSort` into the existing refined-output buffers (ascending for L2 and cosine, descending for inner product) before that consumption, and the async host copy reads those buffers. The CUB temporary allocation is chunked by complete rows within the actual normal workspace, with offsets and temporary storage in one 256-byte-aligned allocation. A row that does not fit is an explicit RMM error, with no hidden retry and no large-memory fallback. IVF selection, training, dtype and seeds are unchanged. Custom limiting adaptors with another alignment and physical fragmentation are not covered by the accounting.

## Dependency

The B3 build also used `raft/0001-raft-select-k-key-bitwidth.patch` on RAFT's `select_k-inl.cuh`. The recipe applies it to the CPM RAFT source after configure (see README), from the pinned base hash to the pinned result hash, and stops on any other header. It is a separate dependency patch, not part of the cuVS source tree.

## Validation recorded for this change

- Full scale: pg_cuvs HNSW builds over a 999000 x 768 fp32 corpus with intermediate degree 256, recall@10 at ef_search 10 over 10,000 queries, before and after: 0.53653 to 0.84139 in one configuration and 0.62018 to 0.93995 in a second one.
- Native, private helper: K 256 and 257 for L2 and inner product with FP16- and FP32-origin scores over 16383 rows, 32 MiB, zero rows and a partial last chunk, checking pair bits, ids, finiteness and order; a real one-million-row PQ index at K 401 and 513.
- Native, maximum shape: a real CAGRA build of 131072 x 96 with intermediate degree 256 and graph degree 64, fp16 and fp32, final graph with zero invalid, self or duplicate edges. Requested bytes, not exact physical peaks.
- Small inputs with exact FP64 ground truth over all 128 x 100 entries (synthetic 16k x 96 and a 16k x 768 text-embedding set).
- CPU: the production select/writer contract red against the old code and green against the patch, with reverted-change mutants.

## Not established

- Default adoption: none. The planned full native comparison matrix has not been run.
- Not covered: the training ids and tensors of the IVF-PQ index, custom memory resources, fragmentation, the ACE path and API types other than the one exercised. Timing and memory figures from the validation runs are diagnostics.
- Builds are not bit-reproducible; the binding between source and binary is the source hash.
