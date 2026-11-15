# 0007: CAGRA prune rank index

## Pinned source and composition

Source: cuVS v26.04.00, commit `60b19a0f62ba0bd716de349382828e43be6e2daa`. Patch: `0007-cagra-prune-rank-index.patch`, SHA-256 `dae5a4fa072bbfeb3676d4d83677913bd922445aca41026815b0028d2685fe6d`. It changes one file, `cpp/src/neighbors/detail/cagra/graph_core.cuh`, whose clean SHA-256 is `c77ad7878831d73a9dd637bb1e35b6450a6e4588312149b48580816b08cbb0d9` and whose patched SHA-256 is `8b2fb100d45f7168bb20a343a3fccb368a2b476da6a52ba1e505eac2a2f77baa`.

No other patch of this set touches that file. 0001 to 0005 change `hnsw.hpp`, `hnsw.cpp`, `cagra.cpp`, `cagra_build.cuh` and add `ace_memory_guard.hpp`, `hnsw_precision.hpp` and `cuvs_hnsw_nn_upper_contract.h`; 0006 changes `cagra_build.cuh` and adds `consumer_row_sort_plan.hpp`. 0007 therefore applies to the clean source and in any position among them, with byte-identical results (checked with `git apply --check`, `git diff --check` and a tree comparison for 0006-then-0007 and 0007-then-0006). The patch is numbered after 0006 to keep the linear prefix states of the build recipe, which applies 0001 through 0007 in order.

## What it changes

`kern_prune` counts, for every row A of the intermediate kNN graph, the 2-hop detours A -> D -> B per edge A -> B. For every pair (kAD, kDB) one thread scans A's row from rank kAD + 1 for the first entry equal to B. With a row degree of 256 this scan dominates the prune phase of `cagra::build`.

`kern_prune_rank_index<DEGREE, IdxT>` replaces that scan by a binary search. A block copies row A into shared memory as 64-bit keys `(ID << 32) | rank`, sorts the keys with a bitonic network (the keys are unique, so the order is total and no tie rule exists), and for each (kAD, kDB) takes the lower bound of `(B << 32) | (kAD + 1)`. The first key with ID B and rank >= kAD + 1 is exactly the first rank after kAD whose ID equals B. Only the auxiliary key array is sorted; the kNN graph, the rank meaning, the detour counts, the clamp at 255, the statistics, the MAX_DEGREE check and the host-side reverse and merge code are unchanged. Candidate IDs are not filtered, so duplicate, self, invalid and out-of-range candidate IDs match exactly as they do in `kern_prune`.

Compared with the first form of this change, this patch:

- makes the degree a template constant `DEGREE` (power of two, 2 to 1024) instead of a literal 256, and makes the kernel trap when `graph_degree != DEGREE`, so correctness does not depend on the host branch alone;
- moves the row setup and the row epilogue (clamp, warp reduction, statistics) into `prune_init_row` and `prune_finish_row`, called by both kernels, so the two kernels cannot drift apart;
- moves the kernel choice into one host function `launch_kern_prune` instead of two copies of the original launch.

## Supported range and guards

- Dispatch: `IdxT == uint32_t` and `graph_degree == kPruneRankIndexDegree` (256). Every other degree and every other ID type runs `kern_prune`, now with the shared helpers. 256 is the only degree measured on a GPU. Enable another power of two by adding its dispatch case together with its own CPU and GPU checks.
- Device guard: `kern_prune_rank_index` executes `__trap()` when `graph_degree != DEGREE` before it touches shared memory or outputs, so a mis-dispatch fails loudly with a sticky CUDA error instead of reading uninitialised shared memory. `assert` is not used because cuVS builds define `NDEBUG`.
- Compile-time guards: `static_assert` on `IdxT == uint32_t` (the key packs a 32-bit ID) and on `DEGREE` being a power of two in [2, 1024] (1024 is `kern_prune`'s `MAX_DEGREE`).
- Launch geometry: 32 threads per block, as in `kern_prune` (the epilogue is a full-warp reduction). The host launch geometry is unchanged.
- Shared memory: `kern_prune<1024, IdxT>` 4096 bytes (unchanged); `kern_prune_rank_index<256, uint32_t>` 1024 + 2048 = 3072 bytes.

## Verification on a CPU

The CPU checks compile the kernel, helper and launch-helper text extracted mechanically from the patched and the original `graph_core.cuh`, run them with one host thread per CUDA thread and per-block shared memory poisoned before each block, with `__syncthreads`, warp shuffles, atomics and `__trap` emulated, and compare against an independent oracle that counts per target rank instead of per (kAD, kDB) pair. The matrix covers output equality (detour bytes, no-detour counts and statistics) of the original kernel, the refactored `kern_prune`, the rank kernel and the oracle; thread-order perturbation; ThreadSanitizer; hand-computed boundary rows; every power-of-two DEGREE from 2 to 1024; launches whose degree differs from DEGREE; batch-split launches; and mutation tests of the kernel, the shared helpers, the dispatch and the template constant. These checks are not part of this repository.

A clang CUDA front-end parse of the patched region in host and device mode (`sm_80`, no SDK, stubbed runtime declarations) is clean and rejects an instantiation with `IdxT = uint64_t` through the `static_assert`; this is a syntax and type check, not a build.

## Verification on a GPU

On an A100-SXM4-80GB, with cuVS at the pinned commit plus 0001 to 0004, 0006 and this patch:

- nvcc build with `-Werror -Wall -Werror=all-warnings`: no warnings. `cuobjdump`: `kern_prune_rank_index<256, uint32_t>` 20 registers, 3072 bytes of shared memory, no spills; `kern_prune<1024, IdxT>` unchanged at 21 registers and 4096 bytes.
- Byte equality of `d_detour_count`, `d_num_no_detour_edges` and the statistics against the unpatched `kern_prune` in 52 cases: `uint32_t` degrees 2 to 1024, `uint64_t` degrees 128 and 256 (`graph::optimize` is instantiated for `uint32_t` and `uint64_t`), and a 999000 x 256 graph repeated three times. Also on synthetic `uint32_t` degree-256 graphs of 17,000,000 and 30,000,000 nodes (4.35e9 and 7.68e9 elements, past 2^32), with the output buffer pre-filled to check that every byte is written.
- Eight launches whose degree differs from DEGREE trap before writing output; no trap on the normal path.
- `compute-sanitizer` memcheck, racecheck, initcheck and synccheck: no errors on a small suite.
- Kernel time on the 999000 x 256 graph: 7.58 s to 0.55 s; the refactored `kern_prune` shows no regression. Standalone `cagra::build` on a 999000 x 768 corpus (three interleaved runs, median): 15.60 s without the prune change, 9.15 s with the earlier form, 8.92 s with this patch.

## Not covered

Occupancy; an nsys trace showing the new kernel inside a full SDK build (its use is inferred from the timing); `compute-sanitizer` at the 999000-row or larger sizes; an end-to-end `CREATE INDEX` with this exact patch; an in-tree test (the checks above are not part of this repository). The kernel does not check `blockDim.x == 32`; the only launch site fixes it.

## Scope

An earlier form of this change (the literal-256 version without the helpers) took the `cagra::build` call of a 999000-row build with intermediate degree 256 on an A100 from 13.65 s to 6.96 s (median of three runs). That effect belongs to the earlier form; for this patch see the standalone timing above. The patch does not change the default parameters, the algorithm, the precision or the ID type policy, and exports no runtime marker (a marker would need an edit of `cagra.cpp` or `hnsw.cpp`, which other patches own); the build recipe verifies it by source hash only.
