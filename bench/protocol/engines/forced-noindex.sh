#!/usr/bin/env bash
# forced-noindex.sh — plain vector table, no index (= pgvector write-heavy W1).
# Only meaningful for PGCUVS_MODULE=incremental (D3): the write-regime baseline
# vs forced-flat. cell_id is the usual N.._d.._k.._r.. form.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROTO="$(cd "$HERE/.." && pwd)"
source "$HERE/_common.sh"
run_engine forced-noindex "${1:?cell_id}"
