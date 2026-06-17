#!/usr/bin/env bash
# forced-flat-concurrent.sh — D3 query-QPS-under-ingest for forced-flat (module=incremental).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROTO="$(cd "$HERE/.." && pwd)"
source "$HERE/_common.sh"
export PGCUVS_INC_SCENARIO=concurrent
run_engine forced-flat "${1:?cell_id}"
