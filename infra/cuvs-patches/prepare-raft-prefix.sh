#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026, JAESOL SHIN
# SPDX-License-Identifier: PostgreSQL
# Make a private, patched copy of an installed RAFT package for the cuVS build.
#
# cuVS 26.04 resolves RAFT through rapids-cmake/CPM, which prefers an installed
# package ("CPM: Using local package raft@26.04.0") over a source download. The
# SDK the product was measured with was built against such an installed RAFT with
# raft/0001-raft-select-k-key-bitwidth.patch applied to a private copy. This
# script reproduces that: it never edits the shared environment.
#
# Usage: prepare-raft-prefix.sh OUT_PREFIX DEPENDENCY_PREFIX...
# Prints the raft_DIR to pass to cmake (OUT_PREFIX/lib/cmake/raft).
set -euo pipefail
readonly BASE_SHA="3d6959c06710d420ceaf9236793a0eb91b2b587ba879a5c59ee79c76ee23abec"
readonly FINAL_SHA="64a54cbaf855b2c3a333ad380ad85ceac41d222b6a557af6c1f490dacf5cf908"
readonly PATCH_SHA="a2d153ccc4a6b3d61bf4c9bbadf6c98f8e104a85352312d2588b90899c58058c"
readonly HEADER_REL="include/raft/matrix/detail/select_k-inl.cuh"
readonly PATCH_FILE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/raft/0001-raft-select-k-key-bitwidth.patch"

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}

[[ $# -ge 2 ]] || { echo "usage: $0 OUT_PREFIX DEPENDENCY_PREFIX..." >&2; exit 2; }
readonly OUT="$1"; shift
[[ "$(sha256_of "$PATCH_FILE")" == "$PATCH_SHA" ]] || { echo "RAFT patch hash mismatch" >&2; exit 2; }

source_prefix=""
for prefix in "$@"; do
  if [[ -f "${prefix}/lib/cmake/raft/raft-config.cmake" && -f "${prefix}/${HEADER_REL}" ]]; then
    source_prefix="$prefix"; break
  fi
done
[[ -n "$source_prefix" ]] || { echo "no installed RAFT package (lib/cmake/raft + ${HEADER_REL}) in: $*" >&2; exit 3; }

if [[ -e "$OUT" ]]; then
  # A reused build directory keeps its private copy; accept it only if patched.
  [[ "$(sha256_of "${OUT}/${HEADER_REL}")" == "$FINAL_SHA" ]] \
    || { echo "existing private RAFT prefix is not the patched header: $OUT" >&2; exit 4; }
else
  installed_sha="$(sha256_of "${source_prefix}/${HEADER_REL}")"
  [[ "$installed_sha" == "$BASE_SHA" ]] \
    || { echo "installed RAFT select_k-inl.cuh is not the pinned base: $installed_sha" >&2; exit 4; }
  mkdir -p "${OUT}/include" "${OUT}/lib/cmake"
  cp -a "${source_prefix}/include/raft" "${OUT}/include/raft"
  cp -a "${source_prefix}/lib/cmake/raft" "${OUT}/lib/cmake/raft"
  for library in "${source_prefix}"/lib/libraft*; do
    [[ -e "$library" ]] && cp -a "$library" "${OUT}/lib/"
  done
  patch --batch --forward --silent -p1 -d "$OUT" < "$PATCH_FILE"
  [[ "$(sha256_of "${OUT}/${HEADER_REL}")" == "$FINAL_SHA" ]] \
    || { echo "patched private RAFT header has an unexpected hash" >&2; exit 4; }
  [[ "$(sha256_of "${source_prefix}/${HEADER_REL}")" == "$BASE_SHA" ]] \
    || { echo "installed RAFT header changed while copying" >&2; exit 4; }
fi
echo "raft_source_prefix=${source_prefix}" >&2
echo "raft_select_k_sha256=${FINAL_SHA}" >&2
echo "${OUT}/lib/cmake/raft"
