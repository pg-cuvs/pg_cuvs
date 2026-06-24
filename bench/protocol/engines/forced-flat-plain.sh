#!/usr/bin/env bash
# forced-flat-plain.sh — forced-flat with STORAGE PLAIN (D8 storage axis).
# The `storage` bench.yml input can't be added without a main-branch push, so
# this wrapper sets PGCUVS_STORAGE=plain (runner.setup_table honors it). Use at
# dim<=~480 (vector(dim)*4 must fit inline, else PG toasts anyway). The row's
# storage is recorded in params_json, so plain vs toast forced-flat are
# distinguishable by that field.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROTO="$(cd "$HERE/.." && pwd)"
source "$HERE/_common.sh"
export PGCUVS_STORAGE=plain
run_engine forced-flat "${1:?cell_id}"
