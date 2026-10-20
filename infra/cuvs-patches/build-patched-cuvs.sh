#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026, JAESOL SHIN
# SPDX-License-Identifier: PostgreSQL
set -euo pipefail

readonly EXPECTED_COMMIT="60b19a0f62ba0bd716de349382828e43be6e2daa"
readonly EXPECTED_SOURCE_ARCHIVE_SHA="2507bb2d2fc37da3d487f7130fbd0c0a5ea61c128c0203cce385aac79e72b078"
readonly EXPECTED_PATCH1_SHA="2f5adf6d3cce3c9d60335e40992250e5044f0ed494c8f5cff6fe3e1654e7b04a"
readonly EXPECTED_PATCH2_SHA="d52ddd22a0e558849d75ec3ddfba4b52fcc200ed6869568593245289f9f963f8"
readonly EXPECTED_PATCH3_SHA="34831f221f32337c025afeb33a32d7aa304e703bd0e3ee85f0ff93902a9a130f"
readonly EXPECTED_PATCHSET_MANIFEST_SHA="ac8791cf4060719cf4d566565bee935ddd578cec46af21b368a015cfb676fd03"
readonly EXPECTED_BASE_HNSW_HPP_SHA="6c8530e39c6ed5fa4eb3fe7b9c6c24022842147c41814fc4c130fdb5a6b8fd0b"
readonly EXPECTED_PATCH1_HNSW_HPP_SHA="49adab846921272a2aaf28f3c5749fb85925fbfde8ca43cd0af61e51adcbba99"
readonly EXPECTED_FINAL_HNSW_HPP_SHA="88192b1da4cb50a2de5a0f374854c1d0659734215136f86d7e53e52c0a1dc911"
readonly EXPECTED_BASE_HNSW_CPP_SHA="59c7874565b049eed090e7423df638eb18e4551ccd1cd4b65f1197dc4a4a7102"
readonly EXPECTED_PATCHED_HNSW_CPP_SHA="fcbe6c5c40d35854a6deb6a3182c70dca8e5b53e83bb79c11a2efcb4b1a4a39e"
readonly EXPECTED_BASE_CAGRA_CPP_SHA="100e9a8702bf1f5b373019583d873a99d9490b575d3aa53dec5a9e34e932c202"
readonly EXPECTED_PATCHED_CAGRA_CPP_SHA="c270c872b8513179b5c3f9583a653aaa8091a3d08973ea19bd125f823803fcce"
readonly EXPECTED_BASE_CAGRA_BUILD_SHA="f72c7ac38e1b24f2f0b05c5a392a1eef2bec7fc88a1fa27f6393fc87795f5bc1"
readonly EXPECTED_PATCHED_CAGRA_BUILD_SHA="87239d05766dda6f99d17cbfbc75faa9858e976fc0aa80f679f63dc4a6297c8e"
readonly EXPECTED_ACE_GUARD_SHA="dc2ccb2d6920bf81d55e614181e3ceff8e7e0394eae56d0506fbb302190cf555"
readonly EXPECTED_LICENSE_SHA="756005f963846334943e8bfc08ef98cd254257d8467ac7a7ffd42a1be262f442"
readonly EXPECTED_DISK_MARKER="pgcuvs-hnsw-disk-copy-v1@60b19a0f62ba0bd716de349382828e43be6e2daa"
readonly EXPECTED_GUARD_MARKER="pgcuvs-hnsw-memory-guard-v2@60b19a0f62ba0bd716de349382828e43be6e2daa"

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly PATCH1_FILE="${SCRIPT_DIR}/0001-hnsw-gpu-hierarchy-typed-copy.patch"
readonly PATCH2_FILE="${SCRIPT_DIR}/0002-hnsw-disk-copy-patch-id.patch"
readonly PATCH3_FILE="${SCRIPT_DIR}/0003-hnsw-memory-guards.patch"
readonly PATCHSET_MANIFEST_FILE="${SCRIPT_DIR}/patchset-manifest.json"
readonly HNSW_HPP_REL="cpp/src/neighbors/detail/hnsw.hpp"
readonly HNSW_CPP_REL="cpp/src/neighbors/hnsw.cpp"
readonly CAGRA_CPP_REL="cpp/src/neighbors/cagra.cpp"
readonly CAGRA_BUILD_REL="cpp/src/neighbors/detail/cagra/cagra_build.cuh"
readonly ACE_GUARD_REL="cpp/src/neighbors/detail/cagra/ace_memory_guard.hpp"
readonly EXPECTED_TRACKED_0001="$(printf '%s\n' "$HNSW_HPP_REL" | LC_ALL=C sort)"
readonly EXPECTED_TRACKED_0002="$(printf '%s\n' "$HNSW_CPP_REL" "$HNSW_HPP_REL" | LC_ALL=C sort)"
readonly EXPECTED_TRACKED_0003="$(printf '%s\n' "$CAGRA_BUILD_REL" "$CAGRA_CPP_REL" "$HNSW_CPP_REL" "$HNSW_HPP_REL" | LC_ALL=C sort)"

if [[ $# -ne 3 ]]; then
  echo "usage: $0 CUVS_SOURCE_ROOT EMPTY_ISOLATED_INSTALL_PREFIX SDK_BUILD_DIR" >&2
  exit 2
fi

readonly SOURCE_ROOT="$(cd -- "$1" && pwd)"
readonly INSTALL_PREFIX="$2"
readonly BUILD_DIR="$3"
readonly LICENSE_FILE="${SOURCE_ROOT}/LICENSE"

for command_name in git sha256sum cmake ninja python3 ldd; do
  command -v "$command_name" >/dev/null || {
    echo "required build command not found: $command_name" >&2
    exit 2
  }
done

for output_path in "$INSTALL_PREFIX" "$BUILD_DIR"; do
  if [[ "$output_path" != /* ]]; then
    echo "install prefix and build directory must be absolute paths" >&2
    exit 2
  fi
done

readonly SOURCE_ROOT_REAL="$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$SOURCE_ROOT")"
readonly INSTALL_PREFIX_REAL="$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$INSTALL_PREFIX")"
readonly BUILD_DIR_REAL="$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$BUILD_DIR")"

path_is_within() {
  local candidate="$1"
  local parent="$2"
  [[ "$candidate" == "$parent" || "$candidate" == "$parent/"* ]]
}

paths_overlap() {
  path_is_within "$1" "$2" || path_is_within "$2" "$1"
}

if paths_overlap "$BUILD_DIR_REAL" "$SOURCE_ROOT_REAL" ||
   paths_overlap "$INSTALL_PREFIX_REAL" "$SOURCE_ROOT_REAL" ||
   paths_overlap "$INSTALL_PREFIX_REAL" "$BUILD_DIR_REAL"; then
  echo "source, isolated install prefix, and SDK build directory must be separate trees" >&2
  exit 2
fi

if [[ -n "${CONDA_PREFIX:-}" ]]; then
  readonly CONDA_PREFIX_REAL="$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$CONDA_PREFIX")"
  if paths_overlap "$INSTALL_PREFIX_REAL" "$CONDA_PREFIX_REAL"; then
    echo "install prefix must be outside the active Conda environment: $CONDA_PREFIX" >&2
    exit 2
  fi
  if paths_overlap "$BUILD_DIR_REAL" "$CONDA_PREFIX_REAL"; then
    echo "SDK build directory must be outside the active Conda environment: $CONDA_PREFIX" >&2
    exit 2
  fi
fi

if [[ -d "$INSTALL_PREFIX" ]] &&
   [[ -n "$(find "$INSTALL_PREFIX" -mindepth 1 -print -quit)" ]]; then
  echo "install prefix must be empty before the build: $INSTALL_PREFIX" >&2
  exit 2
fi

readonly ACTUAL_COMMIT="$(git -C "$SOURCE_ROOT" rev-parse HEAD)"
if [[ "$ACTUAL_COMMIT" != "$EXPECTED_COMMIT" ]]; then
  echo "source commit mismatch: expected $EXPECTED_COMMIT, got $ACTUAL_COMMIT" >&2
  exit 2
fi

sha256_of() {
  sha256sum "$1" | awk '{print $1}'
}

readonly PATCH1_SHA="$(sha256_of "$PATCH1_FILE")"
readonly PATCH2_SHA="$(sha256_of "$PATCH2_FILE")"
readonly PATCH3_SHA="$(sha256_of "$PATCH3_FILE")"
readonly PATCHSET_MANIFEST_SHA="$(sha256_of "$PATCHSET_MANIFEST_FILE")"
for patch_tuple in \
  "0001:$PATCH1_SHA:$EXPECTED_PATCH1_SHA" \
  "0002:$PATCH2_SHA:$EXPECTED_PATCH2_SHA" \
  "0003:$PATCH3_SHA:$EXPECTED_PATCH3_SHA" \
  "manifest:$PATCHSET_MANIFEST_SHA:$EXPECTED_PATCHSET_MANIFEST_SHA"; do
  IFS=: read -r patch_name actual_sha expected_sha <<< "$patch_tuple"
  if [[ "$actual_sha" != "$expected_sha" ]]; then
    echo "$patch_name patch hash mismatch: expected $expected_sha, got $actual_sha" >&2
    exit 2
  fi
done

source_file_matches() {
  local relative_path="$1"
  local expected_sha="$2"
  local source_file="${SOURCE_ROOT}/${relative_path}"
  [[ -f "$source_file" ]] && [[ "$(sha256_of "$source_file")" == "$expected_sha" ]]
}

source_state_matches() {
  local prefix_level="$1"
  local expected_hpp="$EXPECTED_BASE_HNSW_HPP_SHA"
  local expected_cpp="$EXPECTED_BASE_HNSW_CPP_SHA"
  local expected_cagra="$EXPECTED_BASE_CAGRA_CPP_SHA"
  local expected_cagra_build="$EXPECTED_BASE_CAGRA_BUILD_SHA"
  local expected_tracked=""
  local expected_untracked=""
  local actual_tracked
  local actual_untracked
  local actual_ignored

  case "$prefix_level" in
    0) ;;
    1)
      expected_hpp="$EXPECTED_PATCH1_HNSW_HPP_SHA"
      expected_tracked="$EXPECTED_TRACKED_0001"
      ;;
    2)
      expected_hpp="$EXPECTED_PATCH1_HNSW_HPP_SHA"
      expected_cpp="$EXPECTED_PATCHED_HNSW_CPP_SHA"
      expected_tracked="$EXPECTED_TRACKED_0002"
      ;;
    3)
      expected_hpp="$EXPECTED_FINAL_HNSW_HPP_SHA"
      expected_cpp="$EXPECTED_PATCHED_HNSW_CPP_SHA"
      expected_cagra="$EXPECTED_PATCHED_CAGRA_CPP_SHA"
      expected_cagra_build="$EXPECTED_PATCHED_CAGRA_BUILD_SHA"
      expected_tracked="$EXPECTED_TRACKED_0003"
      expected_untracked="$ACE_GUARD_REL"
      ;;
    *) return 1 ;;
  esac

  source_file_matches "$HNSW_HPP_REL" "$expected_hpp" || return 1
  source_file_matches "$HNSW_CPP_REL" "$expected_cpp" || return 1
  source_file_matches "$CAGRA_CPP_REL" "$expected_cagra" || return 1
  source_file_matches "$CAGRA_BUILD_REL" "$expected_cagra_build" || return 1
  if [[ "$prefix_level" == 3 ]]; then
    source_file_matches "$ACE_GUARD_REL" "$EXPECTED_ACE_GUARD_SHA" || return 1
  elif [[ -e "${SOURCE_ROOT}/${ACE_GUARD_REL}" ]]; then
    return 1
  fi

  actual_tracked="$(git -C "$SOURCE_ROOT" diff --name-only HEAD | LC_ALL=C sort)"
  actual_untracked="$(git -C "$SOURCE_ROOT" ls-files --others --exclude-standard | LC_ALL=C sort)"
  actual_ignored="$(git -C "$SOURCE_ROOT" ls-files --others --ignored --exclude-standard | LC_ALL=C sort)"
  [[ "$actual_tracked" == "$expected_tracked" ]] || return 1
  [[ "$actual_untracked" == "$expected_untracked" ]] || return 1
  [[ -z "$actual_ignored" ]] || return 1
  [[ -z "$(git -C "$SOURCE_ROOT" diff --summary HEAD)" ]] || return 1
}

SOURCE_PATCH_LEVEL=""
for candidate_level in 0 1 2 3; do
  if source_state_matches "$candidate_level"; then
    if [[ -n "$SOURCE_PATCH_LEVEL" ]]; then
      echo "source checkout ambiguously matches multiple patch-prefix states" >&2
      exit 2
    fi
    SOURCE_PATCH_LEVEL="$candidate_level"
  fi
done
if [[ -z "$SOURCE_PATCH_LEVEL" ]]; then
  echo "source checkout is not clean or an exact 0001, 0001+0002, or full patch-prefix state" >&2
  echo "tracked changes:" >&2
  git -C "$SOURCE_ROOT" diff --name-only HEAD >&2
  echo "untracked/ignored paths:" >&2
  git -C "$SOURCE_ROOT" ls-files --others --ignored --exclude-standard >&2
  exit 2
fi
readonly INITIAL_PATCH_LEVEL="$SOURCE_PATCH_LEVEL"

apply_next_patch() {
  local patch_file="$1"
  local expected_level="$2"
  git -C "$SOURCE_ROOT" apply --check "$patch_file"
  git -C "$SOURCE_ROOT" apply "$patch_file"
  if ! source_state_matches "$expected_level"; then
    echo "source does not match the exact patch prefix after applying $(basename "$patch_file")" >&2
    exit 2
  fi
}

if [[ "$SOURCE_PATCH_LEVEL" == 0 ]]; then
  apply_next_patch "$PATCH1_FILE" 1
  SOURCE_PATCH_LEVEL=1
fi
if [[ "$SOURCE_PATCH_LEVEL" == 1 ]]; then
  apply_next_patch "$PATCH2_FILE" 2
  SOURCE_PATCH_LEVEL=2
fi
if [[ "$SOURCE_PATCH_LEVEL" == 2 ]]; then
  apply_next_patch "$PATCH3_FILE" 3
  SOURCE_PATCH_LEVEL=3
fi
if [[ "$SOURCE_PATCH_LEVEL" != 3 ]] || ! source_state_matches 3; then
  echo "source checkout did not reach the exact complete 0001+0002+0003 patch state" >&2
  exit 2
fi

git -C "$SOURCE_ROOT" diff --check HEAD
if LC_ALL=C grep -nE '[[:blank:]]+$' "${SOURCE_ROOT}/${ACE_GUARD_REL}"; then
  echo "new ACE guard header contains trailing whitespace" >&2
  exit 2
fi
readonly ACTUAL_LICENSE_SHA="$(sha256_of "$LICENSE_FILE")"
if [[ "$ACTUAL_LICENSE_SHA" != "$EXPECTED_LICENSE_SHA" ]]; then
  echo "cuVS LICENSE hash mismatch: expected $EXPECTED_LICENSE_SHA, got $ACTUAL_LICENSE_SHA" >&2
  exit 2
fi

requested_nvcc="${CUDACXX:-$(command -v nvcc || true)}"
if [[ -x "$requested_nvcc" ]]; then
  readonly NVCC="$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$requested_nvcc")"
else
  readonly NVCC="$(command -v "$requested_nvcc" || true)"
fi
if [[ -z "$NVCC" || ! -x "$NVCC" ]]; then
  echo "CUDA compiler not found; activate the isolated cuVS 26.04 build environment" >&2
  exit 2
fi
resolve_compiler() {
  local configured="$1"
  local located
  if [[ "$configured" == *[[:space:]]* ]]; then
    echo "compiler variables must name one executable without extra arguments: $configured" >&2
    exit 2
  fi
  if [[ "$configured" == */* ]]; then
    [[ -x "$configured" ]] || {
      echo "configured compiler is not executable: $configured" >&2
      exit 2
    }
    located="$configured"
  else
    located="$(command -v "$configured" || true)"
    [[ -n "$located" && -x "$located" ]] || {
      echo "configured compiler was not found: $configured" >&2
      exit 2
    }
  fi
  python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$located"
}
select_sdk_cuda_host() {
  local cache_state="$1"
  local cached_host="$2"
  local requested_host="$3"
  local default_host="$4"
  local cached_host_real

  if [[ "$cache_state" == "reused" ]]; then
    if [[ -z "$cached_host" ]]; then
      echo "existing cuVS cache has no CMAKE_CUDA_HOST_COMPILER; refusing to guess it from CXX" >&2
      return 2
    fi
    if ! cached_host_real="$(resolve_compiler "$cached_host")"; then
      return 2
    fi
    if [[ -n "$requested_host" && "$cached_host_real" != "$requested_host" ]]; then
      echo "CUDAHOSTCXX does not match the existing cuVS cache CUDA host compiler: cached=$cached_host ($cached_host_real) requested=$requested_host" >&2
      return 2
    fi
    SDK_CUDA_HOST_COMPILER="$cached_host_real"
    CUDA_HOST_SELECTION="reused-cache-preserved"
  elif [[ -n "$requested_host" ]]; then
    SDK_CUDA_HOST_COMPILER="$requested_host"
    CUDA_HOST_SELECTION="explicit-CUDAHOSTCXX"
  else
    SDK_CUDA_HOST_COMPILER="$default_host"
    CUDA_HOST_SELECTION="declared-default-CXX"
  fi
}
readonly REQUESTED_CC="$(resolve_compiler "${CC:-cc}")"
readonly REQUESTED_CXX="$(resolve_compiler "${CXX:-c++}")"
CUDA_HOST_REQUESTED=""
if [[ ${CUDAHOSTCXX+x} ]]; then
  CUDA_HOST_REQUESTED="$(resolve_compiler "$CUDAHOSTCXX")"
fi
readonly CUDA_VERSION="$("$NVCC" --version | sed -n 's/.*release \([0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' | tail -1)"
case "$CUDA_VERSION" in
  # cuVS 26.04's x86_64 release matrix includes CUDA 12.9 and 13.1.
  12.9|13.1) ;;
  *)
    echo "this recipe follows the cuVS 26.04 x86_64 release matrix (CUDA 12.9 or 13.1); found nvcc $CUDA_VERSION at $NVCC" >&2
    exit 2
    ;;
esac

readonly CMAKE_VERSION="$(cmake --version | sed -n '1s/.* //p')"
python3 - "$CMAKE_VERSION" <<'PY'
import re
import sys

match = re.match(r"^(\d+)\.(\d+)\.(\d+)", sys.argv[1])
if match is None or tuple(map(int, match.groups())) < (3, 30, 4):
    raise SystemExit(f"cuVS requires CMake >= 3.30.4; found {sys.argv[1]}")
PY

readonly CMAKE_CACHE_ROOT="${BUILD_DIR}/CMakeCache.txt"
readonly CMAKE_CACHE_NESTED="${BUILD_DIR}/libcuvs/CMakeCache.txt"
LIBCUVS_BUILD_DIR="${BUILD_DIR}/libcuvs"
if [[ -f "$CMAKE_CACHE_ROOT" ]]; then
  LIBCUVS_BUILD_DIR="$BUILD_DIR"
elif [[ -f "$CMAKE_CACHE_NESTED" ]]; then
  LIBCUVS_BUILD_DIR="${BUILD_DIR}/libcuvs"
fi
readonly LIBCUVS_BUILD_DIR

directory_has_entries() {
  [[ -d "$1" ]] && [[ -n "$(find "$1" -mindepth 1 -maxdepth 1 -print -quit)" ]]
}

cache_value() {
  local cache_file="$1"
  local cache_key="$2"
  sed -n "s/^${cache_key}:[^=]*=//p" "$cache_file" | tail -1
}

assert_cuda_host_cache() {
  local cache_file="$1"
  local cache_label="$2"
  local expected_host="$3"
  local cached_host
  local cached_host_real
  cached_host="$(cache_value "$cache_file" CMAKE_CUDA_HOST_COMPILER)"
  if [[ -z "$cached_host" ]]; then
    echo "$cache_label CMake cache is missing CMAKE_CUDA_HOST_COMPILER: $cache_file" >&2
    return 1
  fi
  cached_host_real="$(resolve_compiler "$cached_host")"
  if [[ "$cached_host_real" != "$expected_host" ]]; then
    echo "$cache_label CMake cache changed CUDA host compiler: cached=$cached_host ($cached_host_real) expected=$expected_host" >&2
    return 1
  fi
  echo "${cache_label,,}_cuda_host_compiler=$cached_host"
  echo "${cache_label,,}_cuda_host_compiler_canonical=$cached_host_real"
}

assert_pilot_link_flags_cache() {
  local cache_file="$1"
  local cache_label="$2"
  local cached_flags
  cached_flags="$(cache_value "$cache_file" CMAKE_EXE_LINKER_FLAGS)"
  if [[ "$cached_flags" != "$PILOT_EXE_LINKER_FLAGS" ]]; then
    echo "$cache_label CMake cache did not preserve the pilot linker flags/sysroot" >&2
    echo "cached=$cached_flags" >&2
    echo "expected=$PILOT_EXE_LINKER_FLAGS" >&2
    return 1
  fi
  echo "${cache_label,,}_exe_linker_flags_verified=true"
}

declare -A EXPECTED_CACHE_VALUES=(
  [BUILD_TESTS]=OFF
  [BUILD_C_TESTS]=OFF
  [BUILD_C_LIBRARY]=OFF
  [BUILD_MG_ALGOS]=OFF
  [BUILD_CUVS_BENCH]=OFF
  [BUILD_CAGRA_HNSWLIB]=ON
  [BUILD_SHARED_LIBS]=ON
  [CUVS_NVTX]=OFF
  [CMAKE_BUILD_TYPE]=Release
)
REUSED_CUDA_ARCH=""
CONFIGURED_INSTALL_PREFIX=""
CONFIGURED_INSTALL_PREFIX_REAL=""
SDK_CC_COMPILER="$REQUESTED_CC"
SDK_CXX_COMPILER="$REQUESTED_CXX"
CACHED_CUDA_HOST_COMPILER=""
CACHED_CXX_FLAGS=""
CACHED_CUDA_FLAGS=""
CACHED_EXE_LINKER_FLAGS=""
if directory_has_entries "$BUILD_DIR"; then
  cuvs_cache="${LIBCUVS_BUILD_DIR}/CMakeCache.txt"
  if [[ ! -f "$cuvs_cache" ]]; then
    echo "non-empty SDK build directory has no reusable cuVS CMake cache: $cuvs_cache" >&2
    exit 2
  fi
  configured_cuvs_source="$(cache_value "$cuvs_cache" CMAKE_HOME_DIRECTORY)"
  configured_cuvs_generator="$(cache_value "$cuvs_cache" CMAKE_GENERATOR)"
  CONFIGURED_INSTALL_PREFIX="$(cache_value "$cuvs_cache" CMAKE_INSTALL_PREFIX)"
  REUSED_CUDA_ARCH="$(cache_value "$cuvs_cache" CMAKE_CUDA_ARCHITECTURES)"
  if [[ "$configured_cuvs_source" != "${SOURCE_ROOT}/cpp" ||
        "$configured_cuvs_generator" != "Ninja" ||
        -z "$CONFIGURED_INSTALL_PREFIX" ||
        "$CONFIGURED_INSTALL_PREFIX" != /* ||
        -z "$REUSED_CUDA_ARCH" ]]; then
    echo "existing cuVS cache does not match the pinned source, absolute install prefix, Ninja, and a cached CUDA architecture" >&2
    echo "source=$configured_cuvs_source; generator=$configured_cuvs_generator; prefix=$CONFIGURED_INSTALL_PREFIX; arch=$REUSED_CUDA_ARCH" >&2
    exit 2
  fi
  CONFIGURED_INSTALL_PREFIX_REAL="$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$CONFIGURED_INSTALL_PREFIX")"
  cached_cc="$(cache_value "$cuvs_cache" CMAKE_C_COMPILER)"
  cached_cxx="$(cache_value "$cuvs_cache" CMAKE_CXX_COMPILER)"
  cached_cuda="$(cache_value "$cuvs_cache" CMAKE_CUDA_COMPILER)"
  CACHED_CUDA_HOST_COMPILER="$(cache_value "$cuvs_cache" CMAKE_CUDA_HOST_COMPILER)"
  for compiler_tuple in \
    "C:$cached_cc:$REQUESTED_CC" \
    "CXX:$cached_cxx:$REQUESTED_CXX" \
    "CUDA:$cached_cuda:$NVCC"; do
    IFS=: read -r compiler_name cached_compiler requested_compiler <<< "$compiler_tuple"
    cached_compiler_real="$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$cached_compiler")"
    requested_compiler_real="$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$requested_compiler")"
    if [[ -z "$cached_compiler" || "$cached_compiler_real" != "$requested_compiler_real" ]]; then
      echo "existing cuVS cache uses a different $compiler_name compiler: cached=$cached_compiler requested=$requested_compiler" >&2
      exit 2
    fi
  done
  SDK_CC_COMPILER="$cached_cc"
  SDK_CXX_COMPILER="$cached_cxx"
  CACHED_CXX_FLAGS="$(cache_value "$cuvs_cache" CMAKE_CXX_FLAGS)"
  CACHED_CUDA_FLAGS="$(cache_value "$cuvs_cache" CMAKE_CUDA_FLAGS)"
  CACHED_EXE_LINKER_FLAGS="$(cache_value "$cuvs_cache" CMAKE_EXE_LINKER_FLAGS)"
  for cache_key in "${!EXPECTED_CACHE_VALUES[@]}"; do
    cached_value="$(cache_value "$cuvs_cache" "$cache_key")"
    # The direct cpp project only reads BUILD_C_TESTS in an if(); it does
    # not declare an option. An absent CMake boolean is false. The configure
    # below explicitly writes OFF, while all other required keys stay strict.
    if [[ "$cache_key" == BUILD_C_TESTS && -z "$cached_value" &&
          "$(cache_value "$cuvs_cache" BUILD_TESTS)" == OFF &&
          "$(cache_value "$cuvs_cache" BUILD_C_LIBRARY)" == OFF ]]; then
      echo "cache_BUILD_C_TESTS=absent(false); configure_explicitly=OFF"
      continue
    fi
    if [[ "$cached_value" != "${EXPECTED_CACHE_VALUES[$cache_key]}" ]]; then
      echo "existing cuVS cache has an unsupported $cache_key=$cached_value (expected ${EXPECTED_CACHE_VALUES[$cache_key]})" >&2
      exit 2
    fi
  done
  BUILD_CACHE_STATE="reused"
else
  REUSED_CUDA_ARCH="80-real"
  BUILD_CACHE_STATE="cold"
fi
select_sdk_cuda_host "$BUILD_CACHE_STATE" "$CACHED_CUDA_HOST_COMPILER" "$CUDA_HOST_REQUESTED" "$REQUESTED_CXX"
readonly SDK_CUDA_HOST_COMPILER CUDA_HOST_SELECTION CUDA_HOST_REQUESTED
readonly CUDA_ARCHITECTURES="$REUSED_CUDA_ARCH"

if [[ "$BUILD_CACHE_STATE" == "reused" && "$CONFIGURED_INSTALL_PREFIX_REAL" != "$INSTALL_PREFIX_REAL" ]]; then
  if paths_overlap "$CONFIGURED_INSTALL_PREFIX_REAL" "$SOURCE_ROOT_REAL" ||
     paths_overlap "$CONFIGURED_INSTALL_PREFIX_REAL" "$BUILD_DIR_REAL" ||
     paths_overlap "$CONFIGURED_INSTALL_PREFIX_REAL" "$INSTALL_PREFIX_REAL"; then
    echo "cached previous install prefix overlaps source, build, or new install prefix: $CONFIGURED_INSTALL_PREFIX" >&2
    exit 2
  fi
  if [[ -n "${CONDA_PREFIX_REAL:-}" ]] && paths_overlap "$CONFIGURED_INSTALL_PREFIX_REAL" "$CONDA_PREFIX_REAL"; then
    echo "cached previous install prefix overlaps the active Conda environment: $CONFIGURED_INSTALL_PREFIX" >&2
    exit 2
  fi
  if [[ ! -d "$CONFIGURED_INSTALL_PREFIX_REAL" ]] || ! directory_has_entries "$CONFIGURED_INSTALL_PREFIX_REAL"; then
    echo "cannot retarget a reused build cache because its prior SDK prefix is missing or empty: $CONFIGURED_INSTALL_PREFIX" >&2
    exit 2
  fi
fi

prefix_inventory_sha() {
  python3 - "$1" <<'PY'
import hashlib
import os
import pathlib
import stat
import sys

root = pathlib.Path(sys.argv[1]).resolve(strict=True)
entries = []
for path in sorted(root.rglob("*"), key=lambda item: item.as_posix()):
    metadata = path.lstat()
    relative = path.relative_to(root).as_posix()
    mode = stat.S_IMODE(metadata.st_mode)
    if stat.S_ISLNK(metadata.st_mode):
        entries.append(f"L\t{mode:o}\t{relative}\t{os.readlink(path)}")
    elif stat.S_ISREG(metadata.st_mode):
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        entries.append(f"F\t{mode:o}\t{relative}\t{digest}")
    elif stat.S_ISDIR(metadata.st_mode):
        entries.append(f"D\t{mode:o}\t{relative}")
    else:
        entries.append(f"O\t{mode:o}\t{relative}")
print(hashlib.sha256("\n".join(entries).encode()).hexdigest())
PY
}
OLD_PREFIX_INVENTORY_SHA=""
if [[ "$BUILD_CACHE_STATE" == "reused" && "$CONFIGURED_INSTALL_PREFIX_REAL" != "$INSTALL_PREFIX_REAL" ]]; then
  OLD_PREFIX_INVENTORY_SHA="$(prefix_inventory_sha "$CONFIGURED_INSTALL_PREFIX_REAL")"
fi

declare -a RAW_DEPENDENCY_PREFIXES=()
declare -a DEPENDENCY_PREFIXES=()
add_dependency_prefix() {
  local prefix_candidate="$1"
  local prefix_real
  [[ -n "$prefix_candidate" && -d "$prefix_candidate" ]] || return 0
  prefix_real="$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$prefix_candidate")"
  for existing_prefix in "${DEPENDENCY_PREFIXES[@]}"; do
    [[ "$existing_prefix" == "$prefix_real" ]] && return 0
  done
  DEPENDENCY_PREFIXES+=("$prefix_real")
}

add_dependency_prefix "${CONDA_PREFIX:-}"
add_dependency_prefix "$(dirname -- "$(dirname -- "$NVCC")")"
IFS=':;' read -r -a RAW_DEPENDENCY_PREFIXES <<< "${CMAKE_PREFIX_PATH:-}"
for dependency_prefix in "${RAW_DEPENDENCY_PREFIXES[@]}"; do
  add_dependency_prefix "$dependency_prefix"
done
if [[ ${#DEPENDENCY_PREFIXES[@]} -eq 0 ]]; then
  echo "dependency search roots are empty; activate the isolated cuVS dependency environment" >&2
  exit 2
fi

join_by() {
  local separator="$1"
  shift
  local joined=""
  local list_item
  for list_item in "$@"; do
    [[ -n "$list_item" ]] || continue
    if [[ -n "$joined" ]]; then joined+="$separator"; fi
    joined+="$list_item"
  done
  printf '%s' "$joined"
}

declare -a DEPENDENCY_INCLUDE_DIRS=()
declare -a DEPENDENCY_LIBRARY_DIRS=()
for dependency_prefix in "${DEPENDENCY_PREFIXES[@]}"; do
  for candidate_include in \
    "${dependency_prefix}/include" \
    "${dependency_prefix}/targets/x86_64-linux/include"; do
    [[ -d "$candidate_include" ]] && DEPENDENCY_INCLUDE_DIRS+=("$candidate_include")
  done
  for candidate_library in \
    "${dependency_prefix}/lib" \
    "${dependency_prefix}/lib64" \
    "${dependency_prefix}/lib/x86_64-linux-gnu" \
    "${dependency_prefix}/targets/x86_64-linux/lib"; do
    [[ -d "$candidate_library" ]] && DEPENDENCY_LIBRARY_DIRS+=("$candidate_library")
  done
done
if [[ ${#DEPENDENCY_INCLUDE_DIRS[@]} -eq 0 || ${#DEPENDENCY_LIBRARY_DIRS[@]} -eq 0 ]]; then
  echo "dependency roots must contain include and library directories" >&2
  exit 2
fi

readonly DEPENDENCY_PREFIX_PATH_COLON="$(join_by : "${DEPENDENCY_PREFIXES[@]}")"
readonly DEPENDENCY_PREFIX_PATH_CMAKE="$(join_by ';' "${DEPENDENCY_PREFIXES[@]}")"
readonly DEPENDENCY_INCLUDE_PATH="$(join_by : "${DEPENDENCY_INCLUDE_DIRS[@]}")"
readonly DEPENDENCY_INCLUDE_PATH_CMAKE="$(join_by ';' "${DEPENDENCY_INCLUDE_DIRS[@]}")"
readonly DEPENDENCY_LIBRARY_PATH="$(join_by : "${DEPENDENCY_LIBRARY_DIRS[@]}")"
readonly DEPENDENCY_LIBRARY_PATH_CMAKE="$(join_by ';' "${DEPENDENCY_LIBRARY_DIRS[@]}")"
readonly PILOT_PREFIX_PATH="${INSTALL_PREFIX};${DEPENDENCY_PREFIX_PATH_CMAKE}"
readonly BUILD_INCLUDE_PATH="${DEPENDENCY_INCLUDE_PATH}${CMAKE_INCLUDE_PATH:+:${CMAKE_INCLUDE_PATH}}"
readonly BUILD_LIBRARY_PATH="${DEPENDENCY_LIBRARY_PATH}${CMAKE_LIBRARY_PATH:+:${CMAKE_LIBRARY_PATH}}"
readonly BUILD_RUNTIME_PATH="${DEPENDENCY_LIBRARY_PATH}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
existing_cmake_include_path="${CMAKE_INCLUDE_PATH:-}"
existing_cmake_include_path="${existing_cmake_include_path//:/;}"
existing_cmake_library_path="${CMAKE_LIBRARY_PATH:-}"
existing_cmake_library_path="${existing_cmake_library_path//:/;}"
readonly PILOT_INCLUDE_PATH_CMAKE="${INSTALL_PREFIX}/include;${DEPENDENCY_INCLUDE_PATH_CMAKE}${existing_cmake_include_path:+;${existing_cmake_include_path}}"
readonly PILOT_LIBRARY_PATH_CMAKE="${INSTALL_PREFIX}/lib;${INSTALL_PREFIX}/lib64;${DEPENDENCY_LIBRARY_PATH_CMAKE}${existing_cmake_library_path:+;${existing_cmake_library_path}}"

RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
readonly RUN_ROOT="${BUILD_DIR}/sdk-native-${RUN_ID}"
readonly DISK_PILOT_BUILD_DIR="${RUN_ROOT}/build/disk-hierarchy"
readonly GUARD_PILOT_BUILD_DIR="${RUN_ROOT}/build/guard-ace"
readonly DISK_PILOT_ARTIFACT_DIR="${RUN_ROOT}/artifacts/disk-hierarchy"
readonly GUARD_PILOT_ARTIFACT_DIR="${RUN_ROOT}/artifacts/guard-ace"
if [[ -e "$RUN_ROOT" ]]; then
  echo "unique native run directory already exists; choose a fresh build directory: $RUN_ROOT" >&2
  exit 2
fi
mkdir -p "${INSTALL_PREFIX}" "$BUILD_DIR" "${RUN_ROOT}/build" \
  "$DISK_PILOT_ARTIFACT_DIR" "$GUARD_PILOT_ARTIFACT_DIR"

readonly PARALLEL_JOBS="${PARALLEL_LEVEL:-$(getconf _NPROCESSORS_ONLN)}"
readonly BUILD_CMAKE_ARGS='-DBUILD_TESTS=OFF -DBUILD_C_TESTS=OFF -DBUILD_C_LIBRARY=OFF -DBUILD_MG_ALGOS=OFF -DBUILD_CUVS_BENCH=OFF -DBUILD_CAGRA_HNSWLIB=ON -DBUILD_SHARED_LIBS=ON -DCUVS_NVTX=OFF'

echo "source=$SOURCE_ROOT"
echo "source_commit=$ACTUAL_COMMIT"
echo "source_archive_sha256=$EXPECTED_SOURCE_ARCHIVE_SHA"
echo "source_initial_patch_level=$INITIAL_PATCH_LEVEL"
echo "patch_prefix=0001+0002+0003"
echo "0001_patch_sha256=$PATCH1_SHA"
echo "0002_patch_sha256=$PATCH2_SHA"
echo "0003_patch_sha256=$PATCH3_SHA"
echo "patchset_manifest_sha256=$PATCHSET_MANIFEST_SHA"
echo "hnsw_hpp_sha256=$(sha256_of "${SOURCE_ROOT}/${HNSW_HPP_REL}")"
echo "hnsw_cpp_sha256=$(sha256_of "${SOURCE_ROOT}/${HNSW_CPP_REL}")"
echo "cagra_cpp_sha256=$(sha256_of "${SOURCE_ROOT}/${CAGRA_CPP_REL}")"
echo "cagra_build_sha256=$(sha256_of "${SOURCE_ROOT}/${CAGRA_BUILD_REL}")"
echo "ace_memory_guard_sha256=$(sha256_of "${SOURCE_ROOT}/${ACE_GUARD_REL}")"
echo "license_sha256=$ACTUAL_LICENSE_SHA"
echo "disk_marker_expected=$EXPECTED_DISK_MARKER"
echo "memory_guard_marker_expected=$EXPECTED_GUARD_MARKER"
echo "nvcc=$NVCC"
echo "cuda_version=$CUDA_VERSION"
echo "cmake_version=$CMAKE_VERSION"
echo "ninja_version=$(ninja --version)"
echo "cc_compiler=$SDK_CC_COMPILER"
echo "cxx_compiler=$SDK_CXX_COMPILER"
echo "cuda_host_compiler=$SDK_CUDA_HOST_COMPILER"
echo "cuda_host_selection=$CUDA_HOST_SELECTION"
echo "build_cache=$BUILD_CACHE_STATE"
echo "build_options=$BUILD_CMAKE_ARGS"
echo "cuda_architectures=$CUDA_ARCHITECTURES"
echo "dependency_prefixes=$DEPENDENCY_PREFIX_PATH_COLON"
echo "dependency_include_paths=$DEPENDENCY_INCLUDE_PATH"
echo "dependency_library_paths=$DEPENDENCY_LIBRARY_PATH"
echo "pilot_include_paths=$PILOT_INCLUDE_PATH_CMAKE"
echo "pilot_link_paths=$PILOT_LIBRARY_PATH_CMAKE"
echo "install_prefix_previous=${CONFIGURED_INSTALL_PREFIX:-none}"
echo "install_prefix_new=$INSTALL_PREFIX"
echo "previous_prefix_inventory_sha256=${OLD_PREFIX_INVENTORY_SHA:-none}"
echo "sdk_build_dir=$LIBCUVS_BUILD_DIR"
echo "native_run_root=$RUN_ROOT"

env \
  CUDACXX="$NVCC" \
  CUDAHOSTCXX="$SDK_CUDA_HOST_COMPILER" \
  CC="$SDK_CC_COMPILER" \
  CXX="$SDK_CXX_COMPILER" \
  CMAKE_GENERATOR=Ninja \
  CMAKE_PREFIX_PATH="$DEPENDENCY_PREFIX_PATH_COLON" \
  CMAKE_INCLUDE_PATH="$BUILD_INCLUDE_PATH" \
  CMAKE_LIBRARY_PATH="$BUILD_LIBRARY_PATH" \
  LD_LIBRARY_PATH="$BUILD_RUNTIME_PATH" \
  cmake -S "${SOURCE_ROOT}/cpp" -B "$LIBCUVS_BUILD_DIR" -GNinja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
    -DCMAKE_C_COMPILER="$SDK_CC_COMPILER" \
    -DCMAKE_CXX_COMPILER="$SDK_CXX_COMPILER" \
    -DCMAKE_CUDA_COMPILER="$NVCC" \
    -DCMAKE_CUDA_HOST_COMPILER="$SDK_CUDA_HOST_COMPILER" \
    -DCMAKE_CUDA_ARCHITECTURES="$CUDA_ARCHITECTURES" \
    -DCMAKE_PREFIX_PATH="$DEPENDENCY_PREFIX_PATH_CMAKE" \
    -DCMAKE_INCLUDE_PATH="$DEPENDENCY_INCLUDE_PATH_CMAKE${existing_cmake_include_path:+;${existing_cmake_include_path}}" \
    -DCMAKE_LIBRARY_PATH="$DEPENDENCY_LIBRARY_PATH_CMAKE${existing_cmake_library_path:+;${existing_cmake_library_path}}" \
    -DBUILD_TESTS=OFF \
    -DBUILD_C_TESTS=OFF \
    -DBUILD_C_LIBRARY=OFF \
    -DBUILD_MG_ALGOS=OFF \
    -DBUILD_CUVS_BENCH=OFF \
    -DBUILD_CAGRA_HNSWLIB=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DCUVS_NVTX=OFF \
    2>&1 | tee "${RUN_ROOT}/cuvs-configure.log"

cuvs_cache="${LIBCUVS_BUILD_DIR}/CMakeCache.txt"
assert_cuda_host_cache "$cuvs_cache" SDK "$SDK_CUDA_HOST_COMPILER"
if [[ "$(cache_value "$cuvs_cache" CMAKE_INSTALL_PREFIX)" != "$INSTALL_PREFIX" ||
      "$(cache_value "$cuvs_cache" CMAKE_CUDA_ARCHITECTURES)" != "$CUDA_ARCHITECTURES" ]]; then
  echo "cuVS build reconfigured the install prefix or CUDA architecture unexpectedly" >&2
  exit 1
fi
if [[ "$BUILD_CACHE_STATE" == "reused" &&
      ("$(cache_value "$cuvs_cache" CMAKE_CXX_FLAGS)" != "$CACHED_CXX_FLAGS" ||
       "$(cache_value "$cuvs_cache" CMAKE_CUDA_FLAGS)" != "$CACHED_CUDA_FLAGS" ||
       "$(cache_value "$cuvs_cache" CMAKE_EXE_LINKER_FLAGS)" != "$CACHED_EXE_LINKER_FLAGS") ]]; then
  echo "cuVS build changed cached C++/CUDA/compiler-linker flags" >&2
  exit 1
fi
for cache_key in "${!EXPECTED_CACHE_VALUES[@]}"; do
  cached_value="$(cache_value "$cuvs_cache" "$cache_key")"
  if [[ "$cached_value" != "${EXPECTED_CACHE_VALUES[$cache_key]}" ]]; then
    echo "cuVS build cache changed $cache_key to $cached_value" >&2
    exit 1
  fi
done
if [[ "$(cache_value "$cuvs_cache" BUILD_CAGRA_HNSWLIB)" != "ON" ]]; then
  echo "cuVS build cache did not enable CAGRA/HNSW support" >&2
  exit 1
fi

readonly TOOLCHAIN_OBSERVATION_DIR="${RUN_ROOT}/toolchain-observations"
mkdir -p "$TOOLCHAIN_OBSERVATION_DIR"
if ! SDK_CXX_PRINT_SYSROOT="$("$SDK_CXX_COMPILER" -print-sysroot)"; then
  echo "cannot query cached CXX compiler sysroot: $SDK_CXX_COMPILER" >&2
  exit 2
fi
if ! CUDA_HOST_PRINT_SYSROOT="$("$SDK_CUDA_HOST_COMPILER" -print-sysroot)"; then
  echo "cannot query selected CUDA host compiler sysroot: $SDK_CUDA_HOST_COMPILER" >&2
  exit 2
fi
if ! "$SDK_CXX_COMPILER" -print-search-dirs > "${TOOLCHAIN_OBSERVATION_DIR}/cxx-print-search-dirs.txt"; then
  echo "cannot query cached CXX compiler search directories: $SDK_CXX_COMPILER" >&2
  exit 2
fi
if ! "$SDK_CUDA_HOST_COMPILER" -print-search-dirs > "${TOOLCHAIN_OBSERVATION_DIR}/cuda-host-print-search-dirs.txt"; then
  echo "cannot query selected CUDA host compiler search directories: $SDK_CUDA_HOST_COMPILER" >&2
  exit 2
fi
printf '%s\n' "$SDK_CXX_PRINT_SYSROOT" > "${TOOLCHAIN_OBSERVATION_DIR}/cxx-print-sysroot.txt"
printf '%s\n' "$CUDA_HOST_PRINT_SYSROOT" > "${TOOLCHAIN_OBSERVATION_DIR}/cuda-host-print-sysroot.txt"

mapfile -t CXX_COMPILER_METADATA_FILES < <(
  find "${LIBCUVS_BUILD_DIR}/CMakeFiles" -maxdepth 3 -type f -name CMakeCXXCompiler.cmake -print | LC_ALL=C sort
)
if [[ ${#CXX_COMPILER_METADATA_FILES[@]} -ne 1 ]]; then
  echo "expected one configured CMakeCXXCompiler.cmake for implicit link-directory provenance; found ${#CXX_COMPILER_METADATA_FILES[@]}" >&2
  exit 2
fi
readonly CXX_COMPILER_METADATA_FILE="${CXX_COMPILER_METADATA_FILES[0]}"
readonly CXX_IMPLICIT_LINK_DIRECTORIES="$(python3 - "$CXX_COMPILER_METADATA_FILE" <<'PY'
import pathlib
import re
import sys

text = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
matches = re.findall(
    r'^set\(CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES "([^"]*)"\)$', text, re.MULTILINE
)
if len(matches) != 1:
    raise SystemExit("CMakeCXXCompiler.cmake must contain exactly one implicit link-directory record")
print(matches[0])
PY
)"
printf '%s\n' "$CXX_IMPLICIT_LINK_DIRECTORIES" \
  | python3 -c 'import sys; print("\n".join(sys.stdin.read().rstrip("\n").split(";")))' \
  > "${TOOLCHAIN_OBSERVATION_DIR}/cxx-implicit-link-directories.txt"

SDK_CXX_SYSROOT_REAL=""
if [[ -n "$SDK_CXX_PRINT_SYSROOT" ]]; then
  if [[ "$SDK_CXX_PRINT_SYSROOT" != /* || ! -d "$SDK_CXX_PRINT_SYSROOT" ]]; then
    echo "cached CXX compiler returned a non-absolute or missing sysroot: $SDK_CXX_PRINT_SYSROOT" >&2
    exit 2
  fi
  SDK_CXX_SYSROOT_REAL="$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$SDK_CXX_PRINT_SYSROOT")"
  if [[ "$SDK_CXX_SYSROOT_REAL" == "/" ]]; then
    SDK_CXX_SYSROOT_REAL=""
  fi
fi
if [[ -z "$SDK_CXX_SYSROOT_REAL" && "$CXX_IMPLICIT_LINK_DIRECTORIES" == *"/sysroot/"* ]]; then
  echo "cached CXX implicit link directories contain a sysroot but -print-sysroot is empty; refusing to infer the pilot link sysroot" >&2
  exit 2
fi
if [[ -n "$SDK_CXX_SYSROOT_REAL" ]]; then
  IFS=';' read -r -a CXX_IMPLICIT_LINK_DIRECTORY_ARRAY <<< "$CXX_IMPLICIT_LINK_DIRECTORIES"
  for implicit_link_dir in "${CXX_IMPLICIT_LINK_DIRECTORY_ARRAY[@]}"; do
    if [[ "$implicit_link_dir" == *"/sysroot/"* ]]; then
      implicit_link_dir_real="$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$implicit_link_dir")"
      case "$implicit_link_dir" in
        "$SDK_CXX_PRINT_SYSROOT"/*) ;;
        *)
          case "$implicit_link_dir_real" in
            "$SDK_CXX_SYSROOT_REAL"/*) ;;
            *)
              echo "cached CXX implicit link directory is outside its -print-sysroot tree: $implicit_link_dir" >&2
              exit 2
              ;;
          esac
          ;;
      esac
    fi
  done
fi
readonly SDK_CXX_SYSROOT_REAL
readonly PILOT_LINK_SYSROOT="$SDK_CXX_SYSROOT_REAL"

: > "${TOOLCHAIN_OBSERVATION_DIR}/cxx-sysroot-absolute-libm-targets.txt"
if [[ -n "$PILOT_LINK_SYSROOT" ]]; then
  for sysroot_lib in lib64/libm.so.6 lib64/libmvec.so.1; do
    if [[ ! -e "${PILOT_LINK_SYSROOT}/${sysroot_lib}" ]]; then
      echo "cached CXX sysroot lacks an absolute linker-script target required by the pilot: ${PILOT_LINK_SYSROOT}/${sysroot_lib}" >&2
      exit 2
    fi
    sysroot_lib_real="$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "${PILOT_LINK_SYSROOT}/${sysroot_lib}")"
    case "$sysroot_lib_real" in
      "$PILOT_LINK_SYSROOT"/*) ;;
      *)
        echo "cached CXX sysroot linker target resolves outside its sysroot: ${PILOT_LINK_SYSROOT}/${sysroot_lib} -> $sysroot_lib_real" >&2
        exit 2
        ;;
    esac
    {
      printf 'path=%s\ncanonical_path=%s\n' "${PILOT_LINK_SYSROOT}/${sysroot_lib}" "$sysroot_lib_real"
      sha256_of "${PILOT_LINK_SYSROOT}/${sysroot_lib}"
    } >> "${TOOLCHAIN_OBSERVATION_DIR}/cxx-sysroot-absolute-libm-targets.txt"
  done
fi

: > "${TOOLCHAIN_OBSERVATION_DIR}/cxx-libm-linker-scripts.txt"
declare -A RECORDED_LINKER_SCRIPTS=()
while IFS= read -r implicit_link_dir; do
  [[ -n "$implicit_link_dir" ]] || continue
  for linker_script_name in libm.so libmvec.so; do
    linker_script="${implicit_link_dir}/${linker_script_name}"
    if [[ -f "$linker_script" ]]; then
      linker_script_real="$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$linker_script")"
      if [[ -z "${RECORDED_LINKER_SCRIPTS[$linker_script_real]+present}" ]]; then
        RECORDED_LINKER_SCRIPTS["$linker_script_real"]=1
        {
          printf 'path=%s\ncanonical_path=%s\n' "$linker_script" "$linker_script_real"
          sha256_of "$linker_script"
          if [[ "$(wc -c < "$linker_script")" -le 65536 ]] && LC_ALL=C grep -Iq . "$linker_script"; then
            cat "$linker_script"
          else
            echo "content_omitted=non-text-or-over-65536-bytes"
          fi
          printf '\n'
        } >> "${TOOLCHAIN_OBSERVATION_DIR}/cxx-libm-linker-scripts.txt"
      fi
    fi
  done
done < "${TOOLCHAIN_OBSERVATION_DIR}/cxx-implicit-link-directories.txt"

env LD_LIBRARY_PATH="$BUILD_RUNTIME_PATH" \
  cmake --build "$LIBCUVS_BUILD_DIR" --target cuvs --parallel "$PARALLEL_JOBS" \
  2>&1 | tee "${RUN_ROOT}/cuvs-build.log"
env LD_LIBRARY_PATH="$BUILD_RUNTIME_PATH" \
  cmake --install "$LIBCUVS_BUILD_DIR" 2>&1 | tee "${RUN_ROOT}/cuvs-install.log"

if [[ -n "$OLD_PREFIX_INVENTORY_SHA" ]]; then
  current_old_prefix_inventory="$(prefix_inventory_sha "$CONFIGURED_INSTALL_PREFIX_REAL")"
  if [[ "$current_old_prefix_inventory" != "$OLD_PREFIX_INVENTORY_SHA" ]]; then
    echo "previous SDK install prefix changed during retargeted build: $CONFIGURED_INSTALL_PREFIX" >&2
    exit 1
  fi
fi

CCCL_SOURCE_DIR="$(cache_value "$cuvs_cache" CPM_PACKAGE_CCCL_SOURCE_DIR)"
if [[ -z "$CCCL_SOURCE_DIR" ]]; then
  CCCL_SOURCE_DIR="$(cache_value "$cuvs_cache" CCCL_SOURCE_DIR)"
fi
CCCL_VERSION="$(cache_value "$cuvs_cache" CPM_PACKAGE_CCCL_VERSION)"
if [[ -z "$CCCL_SOURCE_DIR" || ! -d "$CCCL_SOURCE_DIR" || "$CCCL_VERSION" != "3.3.0" ]]; then
  echo "cuVS build cache must expose pinned CCCL 3.3.0 source for downstream pilot compilation" >&2
  echo "CCCL_SOURCE_DIR=${CCCL_SOURCE_DIR:-missing}; CCCL_VERSION=${CCCL_VERSION:-missing}" >&2
  exit 1
fi
readonly CCCL_SOURCE_DIR_REAL="$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$CCCL_SOURCE_DIR")"
readonly CCCL_THRUST_DIR="${CCCL_SOURCE_DIR_REAL}/lib/cmake/thrust"
readonly CCCL_CUB_DIR="${CCCL_SOURCE_DIR_REAL}/lib/cmake/cub"
readonly CCCL_LIBCUDACXX_DIR="${CCCL_SOURCE_DIR_REAL}/lib/cmake/libcudacxx"
for cccl_config_dir in "$CCCL_THRUST_DIR" "$CCCL_CUB_DIR" "$CCCL_LIBCUDACXX_DIR"; do
  if [[ ! -d "$cccl_config_dir" ]]; then
    echo "pinned CCCL source is missing its CMake package directory: $cccl_config_dir" >&2
    exit 1
  fi
done
readonly CCCL_SOURCE_INVENTORY_SHA="$(prefix_inventory_sha "$CCCL_SOURCE_DIR_REAL")"
readonly SDK_CXX_FLAGS="$(cache_value "$cuvs_cache" CMAKE_CXX_FLAGS)"
readonly SDK_CUDA_FLAGS="$(cache_value "$cuvs_cache" CMAKE_CUDA_FLAGS)"
readonly SDK_EXE_LINKER_FLAGS="$(cache_value "$cuvs_cache" CMAKE_EXE_LINKER_FLAGS)"
readonly PILOT_CXX_FLAGS="-I${CCCL_SOURCE_DIR_REAL}/thrust -I${CCCL_SOURCE_DIR_REAL}/libcudacxx/include -I${CCCL_SOURCE_DIR_REAL}/cub${SDK_CXX_FLAGS:+ ${SDK_CXX_FLAGS}}"
readonly PILOT_CUDA_FLAGS="-I${CCCL_SOURCE_DIR_REAL}/thrust -I${CCCL_SOURCE_DIR_REAL}/libcudacxx/include -I${CCCL_SOURCE_DIR_REAL}/cub${SDK_CUDA_FLAGS:+ ${SDK_CUDA_FLAGS}}"
readonly PILOT_LINK_SYSROOT_FLAG="${PILOT_LINK_SYSROOT:+-Wl,--sysroot=${PILOT_LINK_SYSROOT}}"
PILOT_LINK_SEARCH_ARGS=""
for pilot_library_dir in "${INSTALL_PREFIX}/lib" "${INSTALL_PREFIX}/lib64" "${DEPENDENCY_LIBRARY_DIRS[@]}"; do
  if [[ -d "$pilot_library_dir" ]]; then
    PILOT_LINK_SEARCH_ARGS+=" -L${pilot_library_dir} -Wl,-rpath,${pilot_library_dir}"
  fi
done
readonly PILOT_EXE_LINKER_FLAGS="${SDK_EXE_LINKER_FLAGS}${SDK_EXE_LINKER_FLAGS:+ }${PILOT_LINK_SYSROOT_FLAG}${PILOT_LINK_SYSROOT_FLAG:+ }-Wl,--enable-new-dtags${PILOT_LINK_SEARCH_ARGS}"
readonly PILOT_RUNTIME_PATH="${INSTALL_PREFIX}/lib:${INSTALL_PREFIX}/lib64:${DEPENDENCY_LIBRARY_PATH}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
echo "cccl_version=$CCCL_VERSION"
echo "cccl_source=$CCCL_SOURCE_DIR_REAL"
echo "cccl_source_inventory_sha256=$CCCL_SOURCE_INVENTORY_SHA"
echo "pilot_cccl_include_order=${CCCL_SOURCE_DIR_REAL}/thrust:${CCCL_SOURCE_DIR_REAL}/libcudacxx/include:${CCCL_SOURCE_DIR_REAL}/cub"
echo "cxx_print_sysroot=${SDK_CXX_PRINT_SYSROOT:-<empty>}"
echo "cuda_host_print_sysroot=${CUDA_HOST_PRINT_SYSROOT:-<empty>}"
echo "pilot_link_sysroot=${PILOT_LINK_SYSROOT:-<none>}"
echo "cxx_implicit_link_directories=${CXX_IMPLICIT_LINK_DIRECTORIES}"
echo "toolchain_observation_dir=$TOOLCHAIN_OBSERVATION_DIR"

libcuvs_file="$(find "$INSTALL_PREFIX" -type f -name 'libcuvs.so*' -print -quit)"
if [[ -z "$libcuvs_file" ]]; then
  echo "isolated install did not produce libcuvs.so" >&2
  exit 1
fi
readonly LIBCUVS_REAL="$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$libcuvs_file")"
readonly LIBCUVS_DIR="$(dirname -- "$LIBCUVS_REAL")"
readonly LIBCUVS_SHA="$(sha256_of "$LIBCUVS_REAL")"
readonly RUNTIME_LIBRARY_PATH="${LIBCUVS_DIR}:${PILOT_RUNTIME_PATH}"

LD_LIBRARY_PATH="$RUNTIME_LIBRARY_PATH" python3 - "$LIBCUVS_REAL" "$EXPECTED_DISK_MARKER" "$EXPECTED_GUARD_MARKER" "$LIBCUVS_SHA" <<'PY' | tee "${RUN_ROOT}/runtime-markers.txt"
import ctypes
import hashlib
import os
import pathlib
import sys

library_path = pathlib.Path(sys.argv[1]).resolve(strict=True)
expected = (sys.argv[2], sys.argv[3])
expected_sha = sys.argv[4]
actual_sha = hashlib.sha256(library_path.read_bytes()).hexdigest()
if actual_sha != expected_sha:
    raise SystemExit(f"loaded DSO hash changed: expected {expected_sha}, got {actual_sha}")
try:
    loaded_library = ctypes.CDLL(str(library_path), mode=os.RTLD_NOW | os.RTLD_LOCAL)
    disk_marker_function = loaded_library.pgcuvs_hnsw_disk_copy_patch_id
    guard_marker_function = loaded_library.pgcuvs_hnsw_memory_guard_patch_id
except (AttributeError, OSError) as error:
    raise SystemExit(f"same loaded libcuvs DSO is missing a required marker: {error}")

for marker_function in (disk_marker_function, guard_marker_function):
    marker_function.argtypes = []
    marker_function.restype = ctypes.c_char_p

disk_marker = disk_marker_function()
guard_marker = guard_marker_function()
if disk_marker is None or guard_marker is None:
    raise SystemExit("loaded libcuvs returned a null runtime marker")
actual = (disk_marker.decode("ascii"), guard_marker.decode("ascii"))
if actual != expected:
    raise SystemExit(f"loaded libcuvs marker mismatch: expected {expected}, got {actual}")
print(f"marker_dso={library_path}")
print(f"marker_dso_sha256={actual_sha}")
print(f"disk_copy_marker={actual[0]}")
print(f"memory_guard_marker={actual[1]}")
PY

LD_LIBRARY_PATH="$RUNTIME_LIBRARY_PATH" ldd "$LIBCUVS_REAL" | tee "${RUN_ROOT}/libcuvs-ldd.txt"
python3 - "${RUN_ROOT}/libcuvs-ldd.txt" "${RUN_ROOT}/runtime-dependencies.tsv" <<'PY'
import hashlib
import pathlib
import re
import sys

ldd_path = pathlib.Path(sys.argv[1])
output_path = pathlib.Path(sys.argv[2])
dependencies = {}
for line in ldd_path.read_text(encoding="utf-8").splitlines():
    if "not found" in line:
        raise SystemExit(f"unresolved libcuvs runtime dependency: {line.strip()}")
    match = re.search(r"=>\s+(/\S+)", line) or re.match(r"^\s*(/\S+)", line)
    if match:
        path = pathlib.Path(match.group(1)).resolve(strict=True)
        dependencies[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()

with output_path.open("w", encoding="utf-8") as output:
    output.write("sha256\tresolved_path\n")
    for path, digest in sorted(dependencies.items()):
        output.write(f"{digest}\t{path}\n")
        print(f"dependency_sha256={digest} dependency_path={path}")
PY

verify_pilot_dependency() {
  local pilot_binary="$1"
  local ldd_file="$2"
  local resolved_libcuvs
  ldd "$pilot_binary" > "$ldd_file"
  resolved_libcuvs="$(awk '$1 ~ /^libcuvs\.so/ && $2 == "=>" {print $3; exit}' "$ldd_file")"
  if [[ -z "$resolved_libcuvs" ||
        "$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$resolved_libcuvs")" != "$LIBCUVS_REAL" ]]; then
    echo "native pilot does not resolve libcuvs to the verified isolated DSO: $pilot_binary" >&2
    echo "resolved=${resolved_libcuvs:-missing}; expected=$LIBCUVS_REAL" >&2
    exit 1
  fi
}

declare -a PILOT_CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE=Release
  "-DCMAKE_PREFIX_PATH=${PILOT_PREFIX_PATH}"
  "-DCMAKE_INCLUDE_PATH=${PILOT_INCLUDE_PATH_CMAKE}"
  "-DCMAKE_LIBRARY_PATH=${PILOT_LIBRARY_PATH_CMAKE}"
  "-DCMAKE_CXX_COMPILER=${SDK_CXX_COMPILER}"
  "-DCMAKE_CUDA_COMPILER=${NVCC}"
  "-DCMAKE_CUDA_HOST_COMPILER=${SDK_CUDA_HOST_COMPILER}"
  "-DCMAKE_CUDA_ARCHITECTURES=${CUDA_ARCHITECTURES}"
  "-DCMAKE_CXX_FLAGS=${PILOT_CXX_FLAGS}"
  "-DCMAKE_CUDA_FLAGS=${PILOT_CUDA_FLAGS}"
  "-DCMAKE_EXE_LINKER_FLAGS=${PILOT_EXE_LINKER_FLAGS}"
  "-DThrust_DIR=${CCCL_THRUST_DIR}"
  "-DCUB_DIR=${CCCL_CUB_DIR}"
  "-Dlibcudacxx_DIR=${CCCL_LIBCUDACXX_DIR}"
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
)

env LD_LIBRARY_PATH="$RUNTIME_LIBRARY_PATH" \
  CUDAHOSTCXX="$SDK_CUDA_HOST_COMPILER" \
  cmake -S "${REPO_ROOT}/test/native/hnsw-sdk" -B "$DISK_PILOT_BUILD_DIR" -GNinja \
  "${PILOT_CMAKE_ARGS[@]}" 2>&1 | tee "${RUN_ROOT}/disk-pilot-configure.log"
assert_cuda_host_cache "${DISK_PILOT_BUILD_DIR}/CMakeCache.txt" "disk-pilot" "$SDK_CUDA_HOST_COMPILER"
assert_pilot_link_flags_cache "${DISK_PILOT_BUILD_DIR}/CMakeCache.txt" "disk-pilot"
env LD_LIBRARY_PATH="$RUNTIME_LIBRARY_PATH" \
  cmake --build "$DISK_PILOT_BUILD_DIR" --target cuvs_hnsw_disk_gpu_pilot --parallel "$PARALLEL_JOBS" \
  2>&1 | tee "${RUN_ROOT}/disk-pilot-build.log"
readonly DISK_PILOT_BIN="${DISK_PILOT_BUILD_DIR}/cuvs_hnsw_disk_gpu_pilot"
LD_LIBRARY_PATH="$RUNTIME_LIBRARY_PATH" \
  verify_pilot_dependency "$DISK_PILOT_BIN" "${RUN_ROOT}/disk-pilot-ldd.txt"

env LD_LIBRARY_PATH="$RUNTIME_LIBRARY_PATH" \
  CUDAHOSTCXX="$SDK_CUDA_HOST_COMPILER" \
  cmake -S "${REPO_ROOT}/test/native/hnsw-sdk/guard-ace" -B "$GUARD_PILOT_BUILD_DIR" -GNinja \
  "${PILOT_CMAKE_ARGS[@]}" 2>&1 | tee "${RUN_ROOT}/guard-ace-configure.log"
assert_cuda_host_cache "${GUARD_PILOT_BUILD_DIR}/CMakeCache.txt" "guard-ace-pilot" "$SDK_CUDA_HOST_COMPILER"
assert_pilot_link_flags_cache "${GUARD_PILOT_BUILD_DIR}/CMakeCache.txt" "guard-ace-pilot"
env LD_LIBRARY_PATH="$RUNTIME_LIBRARY_PATH" \
  cmake --build "$GUARD_PILOT_BUILD_DIR" --target cuvs_hnsw_guard_ace_budget_pilot --parallel "$PARALLEL_JOBS" \
  2>&1 | tee "${RUN_ROOT}/guard-ace-build.log"
readonly GUARD_PILOT_BIN="${GUARD_PILOT_BUILD_DIR}/cuvs_hnsw_guard_ace_budget_pilot"
LD_LIBRARY_PATH="$RUNTIME_LIBRARY_PATH" \
  verify_pilot_dependency "$GUARD_PILOT_BIN" "${RUN_ROOT}/guard-ace-pilot-ldd.txt"

LD_LIBRARY_PATH="$RUNTIME_LIBRARY_PATH" "$DISK_PILOT_BIN" "$DISK_PILOT_ARTIFACT_DIR" \
  2>&1 | tee "${RUN_ROOT}/disk-pilot-run.log"
LD_LIBRARY_PATH="$RUNTIME_LIBRARY_PATH" "$GUARD_PILOT_BIN" "$GUARD_PILOT_ARTIFACT_DIR" \
  2>&1 | tee "${RUN_ROOT}/guard-ace-pilot-run.log"

OLD_PREFIX_INVENTORY_SHA_AFTER="none"
if [[ -n "$OLD_PREFIX_INVENTORY_SHA" ]]; then
  OLD_PREFIX_INVENTORY_SHA_AFTER="$(prefix_inventory_sha "$CONFIGURED_INSTALL_PREFIX_REAL")"
  if [[ "$OLD_PREFIX_INVENTORY_SHA_AFTER" != "$OLD_PREFIX_INVENTORY_SHA" ]]; then
    echo "previous SDK install prefix changed during native pilot build or run: $CONFIGURED_INSTALL_PREFIX" >&2
    exit 1
  fi
fi
cp "$cuvs_cache" "${RUN_ROOT}/CMakeCache-final.txt"

cat > "${RUN_ROOT}/sdk-provenance.txt" <<EOF
source_commit=$ACTUAL_COMMIT
source_archive_sha256=$EXPECTED_SOURCE_ARCHIVE_SHA
patch_prefix=0001+0002+0003
patch_0001_sha256=$PATCH1_SHA
patch_0002_sha256=$PATCH2_SHA
patch_0003_sha256=$PATCH3_SHA
patchset_manifest_sha256=$PATCHSET_MANIFEST_SHA
source_hnsw_hpp_sha256=$(sha256_of "${SOURCE_ROOT}/${HNSW_HPP_REL}")
source_hnsw_cpp_sha256=$(sha256_of "${SOURCE_ROOT}/${HNSW_CPP_REL}")
source_cagra_cpp_sha256=$(sha256_of "${SOURCE_ROOT}/${CAGRA_CPP_REL}")
source_cagra_build_sha256=$(sha256_of "${SOURCE_ROOT}/${CAGRA_BUILD_REL}")
source_ace_memory_guard_sha256=$(sha256_of "${SOURCE_ROOT}/${ACE_GUARD_REL}")
source_license_sha256=$ACTUAL_LICENSE_SHA
install_prefix_previous=${CONFIGURED_INSTALL_PREFIX:-none}
install_prefix_new=$INSTALL_PREFIX
previous_prefix_inventory_sha256=${OLD_PREFIX_INVENTORY_SHA:-none}
previous_prefix_inventory_sha256_after=$OLD_PREFIX_INVENTORY_SHA_AFTER
sdk_build_dir=$LIBCUVS_BUILD_DIR
sdk_cuda_architectures=$CUDA_ARCHITECTURES
nvcc=$NVCC
cuda_version=$CUDA_VERSION
cmake_version=$CMAKE_VERSION
ninja_version=$(ninja --version)
c_compiler=$SDK_CC_COMPILER
cxx_compiler=$SDK_CXX_COMPILER
cuda_host_compiler=$SDK_CUDA_HOST_COMPILER
cuda_host_compiler_selection=$CUDA_HOST_SELECTION
cxx_print_sysroot=${SDK_CXX_PRINT_SYSROOT:-<empty>}
cxx_sysroot_canonical=${SDK_CXX_SYSROOT_REAL:-<none>}
cuda_host_print_sysroot=${CUDA_HOST_PRINT_SYSROOT:-<empty>}
cxx_implicit_link_directories=$CXX_IMPLICIT_LINK_DIRECTORIES
pilot_link_sysroot=${PILOT_LINK_SYSROOT:-<none>}
pilot_link_sysroot_flag=${PILOT_LINK_SYSROOT_FLAG:-<none>}
cxx_compiler_metadata=$CXX_COMPILER_METADATA_FILE
toolchain_observation_dir=$TOOLCHAIN_OBSERVATION_DIR
cccl_version=$CCCL_VERSION
cccl_source=$CCCL_SOURCE_DIR_REAL
cccl_source_inventory_sha256=$CCCL_SOURCE_INVENTORY_SHA
cccl_thrust_dir=$CCCL_THRUST_DIR
cccl_cub_dir=$CCCL_CUB_DIR
cccl_libcudacxx_dir=$CCCL_LIBCUDACXX_DIR
libcuvs_path=$LIBCUVS_REAL
libcuvs_sha256=$LIBCUVS_SHA
runtime_marker_dso=$LIBCUVS_REAL
runtime_marker_dso_sha256=$LIBCUVS_SHA
disk_copy_marker=$EXPECTED_DISK_MARKER
memory_guard_marker=$EXPECTED_GUARD_MARKER
runtime_dependency_manifest=${RUN_ROOT}/runtime-dependencies.tsv
runtime_dependency_ldd=${RUN_ROOT}/libcuvs-ldd.txt
disk_pilot_artifacts=$DISK_PILOT_ARTIFACT_DIR
guard_ace_pilot_artifacts=$GUARD_PILOT_ARTIFACT_DIR
EOF
echo "sdk_provenance=${RUN_ROOT}/sdk-provenance.txt"
echo "runtime_dependencies=${RUN_ROOT}/runtime-dependencies.tsv"
echo "disk_pilot_artifacts=$DISK_PILOT_ARTIFACT_DIR"
echo "guard_ace_pilot_artifacts=$GUARD_PILOT_ARTIFACT_DIR"
