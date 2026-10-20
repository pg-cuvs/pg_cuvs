# Reproducible cuVS 26.04 HNSW/ACE SDK

This patch set builds the cuVS disk HNSW hierarchy copy fix, runtime identity markers, and ACE/HNSW allocation guards from one pinned upstream source tree. The recipe applies patches in order, verifies exact source states and hashes, builds a shared cuVS SDK in an isolated prefix, then builds and runs both native GPU pilots.

## Upstream and patch provenance

The upstream is [`rapidsai/cuvs`](https://github.com/rapidsai/cuvs), tag `v26.04.00`, commit `60b19a0f62ba0bd716de349382828e43be6e2daa`. The pinned source archive SHA-256 is `2507bb2d2fc37da3d487f7130fbd0c0a5ea61c128c0203cce385aac79e72b078`. The root `LICENSE` at that commit is Apache-2.0 and has SHA-256 `756005f963846334943e8bfc08ef98cd254257d8467ac7a7ffd42a1be262f442`.

| Patch | SHA-256 | Effect |
| --- | --- | --- |
| `0001-hnsw-gpu-hierarchy-typed-copy.patch` | `2f5adf6d3cce3c9d60335e40992250e5044f0ed494c8f5cff6fe3e1654e7b04a` | Copies upper-level float rows into typed destinations in disk serialization and in-memory GPU hierarchy construction. |
| `0002-hnsw-disk-copy-patch-id.patch` | `d52ddd22a0e558849d75ec3ddfba4b52fcc200ed6869568593245289f9f963f8` | Exports the disk-copy runtime marker. |
| `0003-hnsw-memory-guards.patch` | `34831f221f32337c025afeb33a32d7aa304e703bd0e3ee85f0ff93902a9a130f` | Propagates ACE metric parameters and checks allocation peaks across ACE sub-builders and disk serialization; exports the memory-guard runtime marker. |

Patches 0001 and 0002 alone do not advertise memory-guard support. Only a library built with 0003 exports `pgcuvs_hnsw_memory_guard_patch_id()` with value `pgcuvs-hnsw-memory-guard-v2@60b19a0f62ba0bd716de349382828e43be6e2daa`. Patch 0002 independently exports `pgcuvs_hnsw_disk_copy_patch_id()` with value `pgcuvs-hnsw-disk-copy-v1@60b19a0f62ba0bd716de349382828e43be6e2daa`.

The hashes below bind the clean source and the complete ordered patch set. `hnsw.hpp` includes both 0001 and 0003.

| Source path | Clean base SHA-256 | Complete patch-set SHA-256 |
| --- | --- | --- |
| `cpp/src/neighbors/detail/hnsw.hpp` | `6c8530e39c6ed5fa4eb3fe7b9c6c24022842147c41814fc4c130fdb5a6b8fd0b` | `88192b1da4cb50a2de5a0f374854c1d0659734215136f86d7e53e52c0a1dc911` |
| `cpp/src/neighbors/hnsw.cpp` | `59c7874565b049eed090e7423df638eb18e4551ccd1cd4b65f1197dc4a4a7102` | `fcbe6c5c40d35854a6deb6a3182c70dca8e5b53e83bb79c11a2efcb4b1a4a39e` |
| `cpp/src/neighbors/cagra.cpp` | `100e9a8702bf1f5b373019583d873a99d9490b575d3aa53dec5a9e34e932c202` | `c270c872b8513179b5c3f9583a653aaa8091a3d08973ea19bd125f823803fcce` |
| `cpp/src/neighbors/detail/cagra/cagra_build.cuh` | `f72c7ac38e1b24f2f0b05c5a392a1eef2bec7fc88a1fa27f6393fc87795f5bc1` | `87239d05766dda6f99d17cbfbc75faa9858e976fc0aa80f679f63dc4a6297c8e` |
| `cpp/src/neighbors/detail/cagra/ace_memory_guard.hpp` | absent | `dc2ccb2d6920bf81d55e614181e3ceff8e7e0394eae56d0506fbb302190cf555` |

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

The install prefix must be empty, outside the active Conda environment, and separate from the source and build trees. The recipe never installs into Conda. It accepts only these source states: clean base, 0001 applied, 0001+0002 applied, or all three applied. After each patch it checks exact source-file hashes and allowed Git paths. It rejects partial 0003 application and unrelated staged, unstaged, untracked, or ignored source changes. The root license hash and the `BUILD_CAGRA_HNSWLIB=ON` setting are checked before the build is accepted.

An existing build cache is reusable only when it has the pinned cuVS source, Ninja generator, same C/C++/CUDA compilers, a valid cached `CMAKE_CUDA_HOST_COMPILER`, CUDA architecture, CCCL 3.3.0, and supported build options. The CUDA host compiler is preserved independently from CXX. If `CUDAHOSTCXX` is set for a reused cache, its canonical executable path must match the cached CUDA host; if unset, the cached host is used without inferring it from CXX. A reused cache without a CUDA host compiler is rejected. A cold build uses `CUDAHOSTCXX` when set and otherwise declares CXX as its CUDA host compiler. The chosen host is passed to the SDK and both pilots; after configure/build, each SDK/pilot CMake cache is checked for the same canonical host path. The recipe records CXX and CUDA host separately in stdout and `sdk-provenance.txt`.

The cache install prefix may point to an earlier SDK. For an incremental rebuild into a new empty prefix, the recipe verifies the old prefix is isolated, records its full file inventory hash, retargets the cache, and verifies that the prior SDK files and directory map remain unchanged. It records both prefix paths. The configured CUDA architecture and compiler flags are carried forward; a cold build selects `80-real` for the A100 pilot.

The build disables unit tests, C tests, the C library, MG algorithms, cuVS benchmarks, and NVTX, and enables shared libraries and CAGRA/HNSW support. The recipe configures the direct `cpp` CMake project, builds target `cuvs`, and installs into the new prefix. Final-source changes trigger CMake's incremental dependency rebuild in the existing build tree.

A direct `cpp` cache may omit `BUILD_C_TESTS`, which the pinned source reads as a false CMake boolean. With `BUILD_TESTS=OFF` and `BUILD_C_LIBRARY=OFF`, the recipe accepts that absence and explicitly configures `BUILD_C_TESTS=OFF`; other required cache values remain exact.

After installation, the recipe loads the canonical `libcuvs` DSO once with `ctypes`, resolves both marker symbols from that same loaded object, checks their exact strings, and records the DSO path and SHA-256. It records `ldd` output plus paths and hashes for the resolved runtime dependencies. Each native pilot has a new timestamped build and artifact directory; prior outputs are retained.

The pilots use the installed SDK and require a visible CUDA GPU:

- `cuvs_hnsw_disk_gpu_pilot` requires a disk HNSW hierarchy with `maxlevel > 0`, checks serialized labels against the original input row IDs, performs HNSW search, and checks recall@10 against its quality threshold.
- `cuvs_hnsw_guard_ace_budget_pilot` checks separate small positive host-budget and GPU-budget rejections, then builds a skewed 8192x16 ACE disk hierarchy with positive 1 GiB budgets and verifies its disk artifacts.

Both pilots verify the two runtime markers and resolve `libcuvs` to the isolated SDK DSO. These GPU runs are native smoke and quality evidence, not a process-RSS guarantee. The memory budgets cover source-derived SDK arrays and workspace estimates; caller-owned input, CUDA/runtime state, RMM pool residency, allocator metadata/fragmentation, and unrelated allocations need wrapper-side reserve margins. Disk-backed `hnsw::search` materializes the serialized index in host RAM.

## CPU RED/GREEN checks

The copy regression compiles the exact upper-level copy statements from pinned `hnsw.hpp` and compares fractional and negative float bytes. Run it first on a clean source checkout for RED, then after the recipe applies 0001 for GREEN:

```sh
python3 test/native/hnsw-sdk/test_upper_level_copy.py \
  --source /path/to/cuvs-26.04 --compiler c++
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
