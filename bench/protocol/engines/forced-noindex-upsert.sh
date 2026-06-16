#!/usr/bin/env bash
# forced-noindex-upsert.sh — D3 incremental scenario=upsert for forced-noindex (module=incremental).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROTO="$(cd "$HERE/.." && pwd)"
source "$HERE/_common.sh"
export PGCUVS_INC_SCENARIO=upsert
run_engine forced-noindex "${1:?cell_id}"
