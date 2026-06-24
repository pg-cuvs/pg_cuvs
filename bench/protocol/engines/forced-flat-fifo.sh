#!/usr/bin/env bash
# forced-flat-fifo.sh — D3 incremental scenario=fifo for forced-flat (module=incremental).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROTO="$(cd "$HERE/.." && pwd)"
source "$HERE/_common.sh"
export PGCUVS_INC_SCENARIO=fifo
run_engine forced-flat "${1:?cell_id}"
