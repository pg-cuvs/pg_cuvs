#!/usr/bin/env bash
# selftest.sh — CPU-shim plumbing smoke for the protocol harness (no GPU/PG).
# Verifies CONTRACT §7: cell enumeration, dry-run plan, CSV shape, resume idempotency.
# Self-validating (FIRST): exits non-zero on any mismatch.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROTO="$(cd "$HERE/.." && pwd)"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

export PGCUVS_RESULTS_ROOT="$tmp/results"
export PGCUVS_STAGE=A PGCUVS_MODULE=physics PGCUVS_CPU_SHIM=1
export PGCUVS_CELLS="N=1k,100k;dim=1024;k=10;recall=0.95,0.99"
export PGCUVS_CONFIGS="forced-hnsw,forced-cuvs,auto"
export PGCUVS_RUN_ID=selftest

fail(){ echo "FAIL: $*"; exit 1; }

# 1. resolve_cells: 2 N × 1 dim × 1 k × 2 recall = 4 cells
cells=$("$PROTO/lib/resolve_cells.sh" "$PGCUVS_CELLS" | wc -l)
[ "$cells" -eq 4 ] || fail "resolve_cells expected 4, got $cells"

# 2. dry-run: 4 cells × 3 configs = 12 measurements
n=$(PGCUVS_DRY_RUN=1 "$PROTO/run.sh" 2>/dev/null | grep -c '|') || true
[ "$n" -eq 12 ] || fail "dry-run expected 12 measurements, got $n"

# 3. full shim run: 12 rows + 1 header
"$PROTO/run.sh" >/dev/null
rows=$(( $(wc -l < "$tmp/results/A/selftest.csv") - 1 ))
[ "$rows" -eq 12 ] || fail "shim run expected 12 rows, got $rows"

# 3b. header matches the column count of a data row
hcols=$(head -1 "$tmp/results/A/selftest.csv" | awk -F, '{print NF}')
dcols=$(sed -n '2p' "$tmp/results/A/selftest.csv" | awk -F, '{print NF}')
[ "$hcols" -eq "$dcols" ] || fail "header cols ($hcols) != data cols ($dcols)"

# 4. resume idempotency: re-run adds 0 rows
PGCUVS_RESUME=1 "$PROTO/run.sh" >/dev/null
rows2=$(( $(wc -l < "$tmp/results/A/selftest.csv") - 1 ))
[ "$rows2" -eq 12 ] || fail "resume expected 12 rows (idempotent), got $rows2"

# 5. progress ledger has 12 entries
p=$(grep -c '|' "$tmp/results/A/selftest.progress")
[ "$p" -eq 12 ] || fail "progress expected 12 entries, got $p"

echo "PASS — plumbing smoke: resolve(4) × configs(3) = 12, dry-run, run, resume idempotency, header/row width"
