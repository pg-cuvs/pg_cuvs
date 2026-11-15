#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026, JAESOL SHIN
# SPDX-License-Identifier: PostgreSQL
# Pre-PR check of the patched cuVS SDK build path. Needs git, python3, make and pg_config;
# no CUDA, no GPU.
#   1. infra/cuvs-patches 0001..0007 apply to the pinned cuVS source and every manifest hash
#      matches (scripts/prepare-patched-cuvs-source.sh).
#   2. scripts/verify-hnsw-sdk-plan.py accepts the patched planner source.
#   3. With CUVS_SDK_SOURCE_INCLUDE set, the Makefile's nvcc command for src/cuvs_wrapper.o
#      and the extension/daemon link flags carry what the patched planner needs.
# usage: check-sdk-patch-path.sh [--root PG_CUVS_CHECKOUT] [--cuvs-git CUVS_GIT]
#   CUVS_GIT defaults to $PGCUVS_CUVS_GIT, then a cache under ~/.cache/pg_cuvs (fetched once).
set -euo pipefail
readonly COMMIT="60b19a0f62ba0bd716de349382828e43be6e2daa"
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
CUVS_GIT="${PGCUVS_CUVS_GIT:-$HOME/.cache/pg_cuvs/cuvs.git}"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --root) ROOT="$(cd -- "$2" && pwd)"; shift 2;;
    --cuvs-git) CUVS_GIT="$2"; shift 2;;
    *) echo "usage: $0 [--root DIR] [--cuvs-git DIR]" >&2; exit 2;;
  esac
done

if ! git -C "$CUVS_GIT" cat-file -e "$COMMIT^{commit}" 2>/dev/null; then
  echo "fetching cuVS $COMMIT into $CUVS_GIT (once)"
  mkdir -p "$CUVS_GIT"
  git -C "$CUVS_GIT" init -q --bare
  git -C "$CUVS_GIT" fetch -q --depth 1 https://github.com/rapidsai/cuvs.git "$COMMIT"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
SRC="$TMP/cuvs/cpp/src"

echo "[1/3] patch set on the pinned source"
"$ROOT/scripts/prepare-patched-cuvs-source.sh" "$CUVS_GIT" "$TMP/cuvs"

echo "[2/3] planner source"
python3 -I "$ROOT/scripts/verify-hnsw-sdk-plan.py" --source "$SRC"

echo "[3/3] Makefile flags with CUVS_SDK_SOURCE_INCLUDE set"
mk=(make -s -C "$ROOT" CUVS_SDK_SOURCE_INCLUDE="$SRC" NVCC=nvcc CUVS_PREFIX=/nonexistent)
nvcc_line="$("${mk[@]}" -n -B src/cuvs_wrapper.o 2>&1 | grep -E '^nvcc .*src/cuvs_wrapper\.cu' || true)"
# A second makefile prints variables (GNU make 3.81, the macOS default, has no --eval).
printf 'pgcuvs-print-%%:\n\t@echo $($*)\n' > "$TMP/print.mk"
links="$("${mk[@]}" -f Makefile -f "$TMP/print.mk" \
          pgcuvs-print-SHLIB_LINK pgcuvs-print-SERVER_LDFLAGS 2>&1 || true)"
fail=0
if [[ -z "$nvcc_line" ]]; then
  echo "FAIL: no nvcc command for src/cuvs_wrapper.cu in 'make -n'"; fail=1
fi
# Flags the patched planner include needs on the wrapper's nvcc command. The SDK's own build
# passes all of them; --expt-relaxed-constexpr is the one 0.9.1 lacked.
for flag in "-I$SRC" "-Xcompiler=-fopenmp" "--expt-relaxed-constexpr" "--extended-lambda"; do
  case " $nvcc_line " in *" $flag "*) ;; *) echo "FAIL: nvcc command lacks $flag"; fail=1;; esac
done
for flag in "-fopenmp" "-lrapids_logger"; do
  n="$(grep -c -- " $flag\( \|$\)" <<<" ${links//$'\n'/$'\n' }" || true)"
  [[ "$n" == 2 ]] || { echo "FAIL: $flag is not in both SHLIB_LINK and SERVER_LDFLAGS"; fail=1; }
done
[[ "$fail" == 0 ]] || { echo "nvcc: $nvcc_line"; echo "links: $links"; exit 1; }
echo "OK: patch set, planner source and planner build flags"
