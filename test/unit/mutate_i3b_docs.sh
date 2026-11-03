#!/bin/sh
# Mutations for check_i3b_docs.py: each edit must turn the check RED.
# Works on temp copies; the tree is restored on exit.
set -u
cd "$(dirname "$0")/../.."
T=$(mktemp -d); cp docs/reference.md "$T/ref"; cp src/cuvs_cache_policy.h "$T/hdr"
trap 'cp "$T/ref" docs/reference.md; cp "$T/hdr" src/cuvs_cache_policy.h; rm -rf "$T"' EXIT
bad=0
run() { # file sed-expr label
    cp "$T/ref" docs/reference.md; cp "$T/hdr" src/cuvs_cache_policy.h
    sed -i.bak "$2" "$1" && rm -f "$1.bak"
    if python3 test/unit/check_i3b_docs.py >"$T/o" 2>&1; then echo "MISS $3"; bad=$((bad+1));
    else echo "RED  $3: $(grep FAIL "$T/o" | head -1)"; fi
}
run docs/reference.md 's/pin it with `pg_cuvs_pin` or keep enough free room for both copies/pin it with `pg_cuvs_pin`/' "REINDEX sentence loses an element"
run docs/reference.md 's/when the daemon stops/at shutdown/' "entry route dropped"
run docs/reference.md 's/only the current database and at most 64 rows/only the current database/' "search_stats limit dropped"
run docs/reference.md 's/| `--reclaim-floor-pct` | 0-100 | `10` |/| `--reclaim-floor-pct` | 0-100 | `5` |/' "documented default drifts"
run docs/reference.md 's/The insert-durability track handles this (in design)/The I2b track removes this when it merges/' "internal track and timing named"
run src/cuvs_cache_policy.h 's/#define CUVS_RECLAIM_HIGH_PCT_DEFAULT     85/#define CUVS_RECLAIM_HIGH_PCT_DEFAULT     90/' "source default drifts"
echo "mutations: 6, survived: $bad"
[ $bad -eq 0 ]
