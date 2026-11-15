# Reproducible cuVS 26.04 HNSW/ACE SDK

This patch set builds the cuVS disk HNSW hierarchy copy fix, runtime identity markers, and ACE/HNSW allocation guards from one pinned upstream source tree. The recipe applies patches in order, verifies exact source states and hashes, builds a shared cuVS SDK in an isolated prefix, then builds and runs both native GPU pilots.

## Upstream and patch provenance

The upstream is [`rapidsai/cuvs`](https://github.com/rapidsai/cuvs), tag `v26.04.00`, commit `60b19a0f62ba0bd716de349382828e43be6e2daa`. The pinned source archive SHA-256 is `2507bb2d2fc37da3d487f7130fbd0c0a5ea61c128c0203cce385aac79e72b078`. The root `LICENSE` at that commit is Apache-2.0 and has SHA-256 `756005f963846334943e8bfc08ef98cd254257d8467ac7a7ffd42a1be262f442`.

| Patch | SHA-256 | Effect |
| --- | --- | --- |
| `0001-hnsw-gpu-hierarchy-typed-copy.patch` | `2f5adf6d3cce3c9d60335e40992250e5044f0ed494c8f5cff6fe3e1654e7b04a` | Fixes the disk destination and types the memory destination; patch 0004 completes the memory source iterator type. |
| `0002-hnsw-disk-copy-patch-id.patch` | `d52ddd22a0e558849d75ec3ddfba4b52fcc200ed6869568593245289f9f963f8` | Exports the disk-copy runtime marker. |
| `0003-hnsw-memory-guards.patch` | `34831f221f32337c025afeb33a32d7aa304e703bd0e3ee85f0ff93902a9a130f` | Propagates ACE metric parameters and checks allocation peaks across ACE sub-builders and disk serialization; exports the memory-guard runtime marker. |
| `0004-hnsw-upper-precision.patch` | `8777280bc918cc26e8bca0bbf38021e1e901ccda8510e543738b9a53ff631fdd` | Applies the product norm policy to upper IVF-PQ and restores typed memory gather sources; exports the upper-precision marker. |
| `0005-hnsw-resident-nn-upper.patch` | `9d55a10e78f5ad285b94a133a583d4f576fc0586aa13c6ff32b5948a7be4dc06` | Lets a resident `build_algo=nn_descent` hierarchy pass its host/GPU budgets and base NN dtype to upper levels, keeps the legacy six-argument `all_neighbors_graph` forwarders, and exports the resident-NN-upper marker. See `0005-hnsw-resident-nn-upper.md`. |
| `0006-cagra-consumer-row-sort.patch` | `3acd107a74bd0fc29ee72e2e206558f86d4bc374e506775af60496b87ae48fa4` | B3 graph consumer: sorts the no-refine CAGRA IVF kNN result rows (score, ID) before the writer/prune consume positions as ranks, with row-chunked CUB temp allocation. See `0006-cagra-consumer-row-sort.md`. |

Patches 0001 and 0002 alone do not advertise memory-guard support. Only a library built with 0003 exports `pgcuvs_hnsw_memory_guard_patch_id()` with value `pgcuvs-hnsw-memory-guard-v2@60b19a0f62ba0bd716de349382828e43be6e2daa`. Patch 0002 independently exports `pgcuvs_hnsw_disk_copy_patch_id()` with value `pgcuvs-hnsw-disk-copy-v1@60b19a0f62ba0bd716de349382828e43be6e2daa`.

Only a library built with 0005 exports `pgcuvs_hnsw_resident_nn_upper_patch_id()` with value `pgcuvs-hnsw-resident-nn-upper-v3@9a18eb49f1e124f9c7806033a1544ce8d50a25bbc4164a22f54d0d441c1b8746`. Unlike the earlier markers, its suffix is not the upstream commit: it is the SHA-256 of the concatenated post-0005 bytes of `hnsw.hpp`, `hnsw_precision.hpp`, and `cuvs_hnsw_nn_upper_contract.h`, so it names the patched content. The recipe recomputes that digest from the source before building. The product refuses hierarchy builds with `build_algo=nn_descent` unless the loaded SDK returns exactly this value.

RAFT dependency patch `raft/0001-raft-select-k-key-bitwidth.patch` (SHA-256 `a2d153ccc4a6b3d61bf4c9bbadf6c98f8e104a85352312d2588b90899c58058c`) makes RAFT `select_k` radix sorts use the key bit width (`sizeof(KeyT) * 8`) instead of the value's. rapids-cmake/CPM prefers an installed RAFT package over a download. When the dependency prefixes contain one (`lib/cmake/raft` and the header), `prepare-raft-prefix.sh` copies it to `<build>/pgcuvs-raft-prefix`. It patches `include/raft/matrix/detail/select_k-inl.cuh` there from `3d6959c06710d420ceaf9236793a0eb91b2b587ba879a5c59ee79c76ee23abec` to `64a54cbaf855b2c3a333ad380ad85ceac41d222b6a557af6c1f490dacf5cf908`, configures with `-Draft_DIR` pinned to that copy, and checks after configure that the cache used it. This is the arrangement of the A2/B3 build, and the shared environment is never edited. Without an installed package, the recipe patches the CPM-downloaded RAFT source after configure with the same pins. Any other header stops the build.

The hashes below bind the clean source and the complete ordered patch set. `hnsw.hpp` includes 0001, 0003, 0004, and 0005. The historical composition receipt in the manifest covers only the first three patches.

| Source path | Clean base SHA-256 | Complete patch-set SHA-256 |
| --- | --- | --- |
| `cpp/src/neighbors/detail/hnsw.hpp` | `6c8530e39c6ed5fa4eb3fe7b9c6c24022842147c41814fc4c130fdb5a6b8fd0b` | `3e85f3b185b214617065cbce597d67b28424078221b55f74384987cee3caf755` (through 0004: `1378471e577268a0abbb5e575a4eeba5a45734f52e7aa35017545e28099d00ca`) |
| `cpp/src/neighbors/hnsw.cpp` | `59c7874565b049eed090e7423df638eb18e4551ccd1cd4b65f1197dc4a4a7102` | `5f7b5d6e2efe5024d6d11420148325900f49790544130440eae3f3c99054446d` (through 0004: `dce2bbaa933c6f046f561e35c6da5fb33c56ac37930cad5285123460e0b64069`) |
| `cpp/src/neighbors/cagra.cpp` | `100e9a8702bf1f5b373019583d873a99d9490b575d3aa53dec5a9e34e932c202` | `c270c872b8513179b5c3f9583a653aaa8091a3d08973ea19bd125f823803fcce` |
| `cpp/src/neighbors/detail/cagra/cagra_build.cuh` | `f72c7ac38e1b24f2f0b05c5a392a1eef2bec7fc88a1fa27f6393fc87795f5bc1` | `38722f03f72efeb5a92e92ea30ebe9d87ec9b9b1afc2ca392a07de4b645c95bc` (through 0003: `87239d05766dda6f99d17cbfbc75faa9858e976fc0aa80f679f63dc4a6297c8e`) |
| `cpp/src/neighbors/detail/cagra/consumer_row_sort_plan.hpp` | absent | `07b7b724fce554cec74ee1cb779e24ced31867981f33e6afda4f2c17d98cac49` |
| `cpp/src/neighbors/detail/cagra/ace_memory_guard.hpp` | absent | `dc2ccb2d6920bf81d55e614181e3ceff8e7e0394eae56d0506fbb302190cf555` |
| `cpp/src/neighbors/detail/hnsw_precision.hpp` | absent | `e6a0839d761fa6720c11c94f7b4a2b485ac707446b173b4c5946a9c56979838c` (through 0004: `c051c5bfe6c5b1031c61806ab7d0a8804741724011b41b33bb63eac9cb72dc69`) |
| `cpp/src/neighbors/detail/cuvs_hnsw_nn_upper_contract.h` | absent | `6db8ffea2fb73695ab107e95135e37d04599c03013cdcc6a3bbe614382758e42` |

## Build environment

Use Linux x86_64 with the cuVS 26.04 dependency environment activated. The cuVS 26.04 x86_64 release matrix lists CUDA 12.9 and 13.1; that matrix is not an absolute CUDA compiler minimum. The source's CUDA 11.2 comparison gates a warning flag and does not state a global compiler minimum. cuVS requires CMake 3.30.4 or newer. The RAPIDS 26.4 release environment uses GCC 14 and Ninja; these release choices are not a claim about the absolute compiler minimum.

The recipe needs `git`, `sha256sum`, CMake, Ninja, Python 3, `nvcc`, and `ldd`. Use the matching cuVS/RAPIDS dependencies. The cuVS cache pins CCCL 3.3.0 under `_deps/cccl-src`; downstream pilots use that same source's Thrust, CUB, and libcudacxx package files and put those headers ahead of environment/toolkit headers. They use the cached C++ compiler, C++ and CUDA flags, CUDA host compiler, and architecture, with C++20 enabled. CXX and CUDA host are separate compiler choices and may have different versions. The recipe adds explicit include, link, and runtime search paths for both the environment prefix and `targets/x86_64-linux`.

After SDK configuration, the recipe records `-print-sysroot` and `-print-search-dirs` for both cached CXX and CUDA host compilers, plus CMake's configured CXX implicit link directories. If those CXX link directories use an explicit sysroot, the pilot link flags pass that same canonical root to the linker with `-Wl,--sysroot`; an empty `-print-sysroot` is never guessed when CMake reports sysrooted link directories. The recipe requires the absolute `libm.so.6` and `libmvec.so.1` targets to exist inside that root and resolve within it before using the root. This link-only pilot setting lets the selected CUDA host linker resolve absolute paths inside the SDK CXX toolchain's linker scripts against that toolchain's sysroot. It does not change `CMAKE_SYSROOT`, the SDK configure/build flags, compiler identities, CUDA host selection, architecture, or ABI flags. The recipe saves the sysroot/search observations, validated target paths and hashes, and small `libm`/`libmvec` linker scripts under each native run's `toolchain-observations/` directory, and verifies both pilot caches retain the exact linker flags. A native pilot must still pass CMake's compiler and link checks; these checks are not skipped or converted to static-library-only probes.

## Reproduce

Fetch the exact source commit and invoke the recipe with an empty isolated install prefix and a separate SDK build directory. Keep the build directory and pilot artifact directories on persistent storage, not tmpfs.

```sh
git clone https://github.com/rapidsai/cuvs.git /path/to/cuvs-26.04
git -C /path/to/cuvs-26.04 checkout --detach 60b19a0f62ba0bd716de349382828e43be6e2daa

infra/cuvs-patches/build-patched-cuvs.sh \
  /path/to/cuvs-26.04 \
  /path/to/empty-isolated-sdk-prefix \
  /path/to/sdk-build
```

To choose a CUDA host compiler explicitly, export `CUDAHOSTCXX=/absolute/path/to/g++` before running the recipe. For a reused cache this is an identity check against the cached CUDA host; for a cold build it selects the host compiler used for both SDK and pilot configuration.

The install prefix must be empty, outside the active Conda environment, and separate from the source and build trees. The recipe never installs into Conda. It accepts only these source states: clean base, 0001 applied, 0001+0002 applied, 0001+0002+0003 applied, all four, all five, or all six applied. After each patch it checks exact source-file hashes and allowed Git paths. It rejects partial 0003~0006 application and unrelated staged, unstaged, untracked, or ignored source changes. The root license hash and the `BUILD_CAGRA_HNSWLIB=ON` setting are checked before the build is accepted.

An existing build cache is reusable only when it has the pinned cuVS source, Ninja generator, same C/C++/CUDA compilers, a valid cached `CMAKE_CUDA_HOST_COMPILER`, CUDA architecture, CCCL 3.3.0, and supported build options. The CUDA host compiler is preserved independently from CXX. If `CUDAHOSTCXX` is set for a reused cache, its canonical executable path must match the cached CUDA host; if unset, the cached host is used without inferring it from CXX. A reused cache without a CUDA host compiler is rejected. A cold build uses `CUDAHOSTCXX` when set and otherwise declares CXX as its CUDA host compiler. The chosen host is passed to the SDK and both pilots; after configure/build, each SDK/pilot CMake cache is checked for the same canonical host path. The recipe records CXX and CUDA host separately in stdout and `sdk-provenance.txt`.

The cache install prefix may point to an earlier SDK. For an incremental rebuild into a new empty prefix, the recipe verifies the old prefix is isolated, records its full file inventory hash, retargets the cache, and verifies that the prior SDK files and directory map remain unchanged. It records both prefix paths. The configured CUDA architecture and compiler flags are carried forward; a cold build selects `80-real` for the A100 pilot.

The build disables unit tests, C tests, the C library, cuVS benchmarks, and NVTX, keeps MG algorithms on (`BUILD_MG_ALGOS=ON`, matching the A2/B3/r3 objects whose compile lines carry `-DCUVS_BUILD_MG_ALGOS`), and enables shared libraries and CAGRA/HNSW support with `CUVS_COMPILE_DYNAMIC_ONLY=ON`. This excludes the unbuilt static archive from installation. The recipe configures the direct `cpp` CMake project, builds target `cuvs`, and installs into the new prefix. Final-source changes trigger CMake's incremental dependency rebuild in the existing build tree.

A direct `cpp` cache may omit `BUILD_C_TESTS`, which the pinned source reads as a false CMake boolean. With `BUILD_TESTS=OFF` and `BUILD_C_LIBRARY=OFF`, the recipe accepts that absence and explicitly configures `BUILD_C_TESTS=OFF`; other required cache values remain exact.

After installation, the recipe loads the canonical `libcuvs` DSO once with `ctypes`, resolves all three marker symbols from that same loaded object, checks their exact strings, and records the DSO path and SHA-256. It records `ldd` output plus paths and hashes for the resolved runtime dependencies. Each native pilot has a new timestamped build and artifact directory; prior outputs are retained.

The pilots use the installed SDK and require a visible CUDA GPU:

- `cuvs_hnsw_disk_gpu_pilot` requires a disk HNSW hierarchy with `maxlevel > 0`, checks serialized labels against the original input row IDs, performs HNSW search, and checks recall@10 against its quality threshold.
- `cuvs_hnsw_guard_ace_budget_pilot` checks separate small positive host-budget and GPU-budget rejections, then builds a skewed 8192x16 ACE disk hierarchy with positive 1 GiB budgets and verifies its disk artifacts.

Both pilots verify the two runtime markers and resolve `libcuvs` to the isolated SDK DSO. These GPU runs are native smoke and quality evidence, not a process-RSS guarantee. The memory budgets cover source-derived SDK arrays and workspace estimates; caller-owned input, CUDA/runtime state, RMM pool residency, allocator metadata/fragmentation, and unrelated allocations need wrapper-side reserve margins. Disk-backed `hnsw::search` materializes the serialized index in host RAM.

## CPU RED/GREEN checks

The copy regression compiles the exact upper-level copy statements from pinned `hnsw.hpp` and compares fractional and negative float bytes. Run it first on a clean source checkout for RED, then after the complete recipe applies 0004 for GREEN. The old fixture incorrectly modeled hnswlib as returning float*, masking the byte-source defect; the fixture now requires the actual char* getter header:

```sh
python3 test/native/hnsw-sdk/test_upper_level_copy.py \
  --source /path/to/cuvs-26.04 --compiler c++ \
  --hnswlib-header /path/to/sdk-build/libcuvs/_deps/hnswlib-src/hnswlib/hnswalg.h
```

The ACE guard arithmetic fixture is CPU-only and uses the source helper added by 0003:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -pedantic \
  -I /path/to/cuvs-26.04/cpp/src/neighbors/detail/cagra \
  test/native/hnsw-sdk/guard_memory_budget.cpp \
  -o /tmp/cuvs-hnsw-guard-memory-budget
/tmp/cuvs-hnsw-guard-memory-budget
```

These checks validate source-derived byte-copy and arithmetic behavior. They do not build cuVS, exercise CUDA allocation paths, or establish process-RSS limits.

## Upper IVF-PQ policy (0004)

Patch 0004 leaves the existing builder boundary and budget-driven NN-descent path intact. Only after upper IVF-PQ is selected does it scan the actual host query rows in double precision. The policy bound is `4 * max_squared_norm` for L2 and `max_squared_norm` for inner product/cosine. `!(bound < 65504)` selects FP32 for coarse, internal, and LUT, including NaN/inf. Normalized data retain the constructor's FP16 choices. This is a conservative product policy, not a proof of every PQ intermediate's range.

The common memory gather now interprets hnswlib's `char*` storage as `const T*` before counting `dim` elements. This restores original coordinates; it can change graphs previously built from incorrect byte coordinates, including normalized data. The builder/dtype policy on normalized data and NN-descent is unchanged. Default nsw bypasses this hierarchy construction.

The same loaded DSO must export `pgcuvs_hnsw_upper_precision_patch_id()` with value `pgcuvs-hnsw-upper-precision-v1@60b19a0f62ba0bd716de349382828e43be6e2daa`. The recipe resolves it alongside the two older markers. The product's existing `ivfpq_distance_dtype` receipt describes base-graph precision, not this upper choice. Upper selection is recorded by the SDK's `HNSW upper-level IVF-PQ precision` INFO log.

See [0004 evidence](0004-hnsw-upper-precision.md) for the defect, controls, upstream #2246 limitations, and measurement scope. The planner source verifier now reads the canonical `cagra_build.cuh` hash from the manifest rather than pinning the legacy diagnostic snapshot.

Run the actual SDK dispatch/policy source checks and in-memory mutation probes:

```sh
python3 test/native/hnsw-sdk/test_upper_level_precision.py \
  --source /path/to/cuvs-26.04 --compiler c++ \
  --hnswlib-header /path/to/sdk-build/libcuvs/_deps/hnswlib-src/hnswlib/hnswalg.h \
  --mutations --output /tmp/upper-precision.json
```

These CPU tests execute the actual dispatch body, norm helper and memory guard with GPU builders replaced by argument spies. They check all three selected dtypes, metric distinctions, threshold equality, late unsafe rows, non-finite norms, and that the NN branch never scans its input. Copy mutants use the actual source expressions and actual hnswlib getter type. GPU quality/performance and allocation peaks are separate checks.
