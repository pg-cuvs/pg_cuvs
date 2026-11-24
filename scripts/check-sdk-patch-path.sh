#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026, JAESOL SHIN
# SPDX-License-Identifier: PostgreSQL
# Pre-PR check of the patched cuVS SDK build path. Needs git, python3 and make;
# no CUDA, no GPU, no pg_config.
#   1. infra/cuvs-patches 0001..0007 apply to the pinned cuVS source and every manifest hash
#      matches (scripts/prepare-patched-cuvs-source.sh).
#   2. scripts/verify-hnsw-sdk-plan.py accepts the patched planner source.
#   3. With CUVS_SDK_SOURCE_INCLUDE set, the Makefile's nvcc command for src/cuvs_wrapper.o
#      and the extension/daemon link flags carry what the patched planner needs. With it
#      empty, the nvcc command does not carry --expt-relaxed-constexpr (builds without the
#      planner are unchanged).
# The 0001..0007 patch glob and the manifest file count come from
# prepare-patched-cuvs-source.sh; this script does not check them itself.
# The temporary patched tree is removed on every normal exit, on errors and on
# SIGINT/SIGTERM; after SIGKILL it stays in the temporary directory.
# usage: check-sdk-patch-path.sh [--root PG_CUVS_CHECKOUT] [--cuvs-git CUVS_GIT] [--manual-checks]
#   CUVS_GIT defaults to $PGCUVS_CUVS_GIT, then a cache under ~/.cache/pg_cuvs (fetched once).
set -euo pipefail
readonly COMMIT="60b19a0f62ba0bd716de349382828e43be6e2daa"
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
CUVS_GIT="${PGCUVS_CUVS_GIT:-$HOME/.cache/pg_cuvs/cuvs.git}"
manual_checks=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --root) ROOT="$(cd -- "$2" && pwd)"; shift 2;;
    --cuvs-git) CUVS_GIT="$2"; shift 2;;
    --manual-checks) manual_checks=1; shift;;
    *) echo "usage: $0 [--root DIR] [--cuvs-git DIR] [--manual-checks]" >&2; exit 2;;
  esac
done

if ! git -C "$CUVS_GIT" cat-file -e "$COMMIT^{commit}" 2>/dev/null; then
  echo "fetching cuVS $COMMIT into $CUVS_GIT (once)"
  mkdir -p "$CUVS_GIT"
  git -C "$CUVS_GIT" init -q --bare
  # Limits on a stalled network. The low-speed settings
  # abort a transfer that stays below 1 kB/s for 30 s. That includes a server that accepts
  # the connection and then sends nothing: measured to fail after 30 s with "Operation too
  # slow". They do not cover a connection that is never established or a TLS handshake that
  # stalls; git and the OS end those. Measured on macOS: about 75 s to an address that does
  # not answer ("Failed to connect"), 300 s for a stalled TLS handshake ("SSL connection
  # timeout"). Not measured on Linux. Where timeout(1) exists, the fetch as a whole is also
  # limited to 120 s. Stock macOS has none, and there, by the settings alone, a transfer that
  # keeps arriving at 1 kB/s or more has no upper bound.
  fetch=(git -C "$CUVS_GIT" -c http.lowSpeedLimit=1000 -c http.lowSpeedTime=30
         fetch -q --depth 1 https://github.com/rapidsai/cuvs.git "$COMMIT")
  if command -v timeout >/dev/null 2>&1; then fetch=(timeout 120 "${fetch[@]}"); fi
  "${fetch[@]}" || { echo "FAIL: could not fetch cuVS $COMMIT (network error or the time limit)"; exit 1; }
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
printf 'pgcuvs-print-%%:\n\t@echo "$*: $($*)"\n' > "$TMP/print.mk"
links="$("${mk[@]}" -f Makefile -f "$TMP/print.mk" \
          pgcuvs-print-SHLIB_LINK pgcuvs-print-SERVER_LDFLAGS 2>&1 || true)"
# Without the planner include, the wrapper's nvcc command is the default one.
nvcc_default="$(make -s -C "$ROOT" CUVS_SDK_SOURCE_INCLUDE= NVCC=nvcc CUVS_PREFIX=/nonexistent \
                 -n -B src/cuvs_wrapper.o 2>&1 | grep -E '^nvcc .*src/cuvs_wrapper\.cu' || true)"
fail=0
if [[ -z "$nvcc_line" ]]; then
  echo "FAIL: no nvcc command for src/cuvs_wrapper.cu in 'make -n'"; fail=1
fi
if [[ -z "$nvcc_default" ]]; then
  echo "FAIL: no nvcc command for src/cuvs_wrapper.cu in 'make -n' without CUVS_SDK_SOURCE_INCLUDE"; fail=1
fi
# Flags the patched planner include needs on the wrapper's nvcc command. The SDK's own build
# passes all of them; --expt-relaxed-constexpr is the one 0.9.1 lacked.
for flag in "-I$SRC" "-Xcompiler=-fopenmp" "--expt-relaxed-constexpr" "--extended-lambda"; do
  case " $nvcc_line " in *" $flag "*) ;; *) echo "FAIL: nvcc command lacks $flag"; fail=1;; esac
done
case " $nvcc_default " in
  *" --expt-relaxed-constexpr "*) echo "FAIL: --expt-relaxed-constexpr is in the default nvcc command, not only with the planner"; fail=1;;
esac
# Each flag exactly once on both link lines. SHLIB_LINK and SERVER_LDFLAGS are expanded, so
# it does not matter which Makefile variable they come from.
for var in SHLIB_LINK SERVER_LDFLAGS; do
  line="$(grep "^$var: " <<<"$links" || true)"
  for flag in "-fopenmp" "-lrapids_logger"; do
    n="$(tr ' ' '\n' <<<"$line" | grep -cx -- "$flag" || true)"
    [[ "$n" == 1 ]] || { echo "FAIL: $flag appears $n times in $var, expected once"; fail=1; }
  done
done
[[ "$fail" == 0 ]] || { echo "nvcc: $nvcc_line"; echo "nvcc (default): $nvcc_default"; echo "links: $links"; exit 1; }
echo "OK: patch set, planner source and planner build flags"

if [[ "$manual_checks" == 1 ]]; then
  git -C "$TMP/cuvs" show level-0005:cpp/src/neighbors/detail/cagra/cagra_build.cuh > "$TMP/canonical.cuh"
  ulimit -c 0
  manual_args=(--sdk-source "$SRC" --canonical-header "$TMP/canonical.cuh")
  if [[ -n "${PGCUVS_MANUAL_CHECKS_OUTPUT:-}" ]]; then
    manual_args+=(--output "$PGCUVS_MANUAL_CHECKS_OUTPUT")
  fi
  python3 -B "$ROOT/test/unit/run_manual_checks.py" "${manual_args[@]}"
fi
