#!/bin/bash
# benchmark.sh -- parameterized large-dataset benchmark harness for pg_cuvs.
#
# Builds a cagra index over N random DIM-dimensional vectors via the live
# daemon, then measures build/planning/execution metrics, JIT presence,
# fallback counts, daemon reload time, and GPU VRAM/CUDA-context state.
# All metrics are emitted as parseable "metric: value" lines and collected
# into a final "[bench] SUMMARY" block. Run on the GPU VM:
#   make benchmark            (default sanity size 10000 x 384)
#   N=1000000 DIM=1536 make benchmark   (PLAN completion gate case)
#
# Requires: pg_cuvs installed, pg-cuvs-server systemd unit active, index dir
# matching the daemon --index-dir. The daemon is left running on exit.
#
# This harness is written to run ON the GPU VM; the local laptop has no
# toolchain/daemon, so the actual run is leader-verified on the VM.

set -e

# ---- parameters (env or default) ----------------------------------------
N=${N:-10000}                       # number of vectors / rows
DIM=${DIM:-384}                     # embedding dimension
K=${K:-10}                          # k-NN LIMIT
M=${M:-100}                         # query sample size for percentiles
IDX_DIR=${IDX_DIR:-/tmp/cuvs_indexes}
DB=${DB:-postgres}
RELOAD_TIMEOUT=${RELOAD_TIMEOUT:-120}   # seconds to wait for index requeryable

echo "[bench] params N=$N DIM=$DIM K=$K M=$M IDX_DIR=$IDX_DIR DB=$DB"

# psql wrapper: index_dir GUC set on every connection, stop on error.
PSQL="psql -d $DB -v ON_ERROR_STOP=1"

# Run a one-shot SQL query (index_dir prefixed), tuples-only unaligned.
q() {
    $PSQL -At -c "SET cuvs.index_dir='$IDX_DIR'; $1"
}

# epoch with sub-second resolution; falls back to whole seconds.
now() { date +%s.%N 2>/dev/null || date +%s; }

# elapsed seconds between two now() readings (awk for float math).
elapsed() { awk "BEGIN{printf \"%.3f\", $2 - $1}"; }

# ---- a. data generation --------------------------------------------------
# Server-side random vector generation: for each row i, build a DIM-element
# float4 array of random() values via an inner generate_series and cast to
# vector(DIM). No data is shipped from the client.
echo "[bench] generating $N x $DIM random vectors server-side"
GEN_START=$(now)
$PSQL >/dev/null <<SQL
SET cuvs.index_dir='$IDX_DIR';
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
DROP TABLE IF EXISTS bench_items;
CREATE TABLE bench_items (id bigint, embedding vector($DIM));
INSERT INTO bench_items
  SELECT i,
         (SELECT array_agg(random()::float4)::vector($DIM)
          FROM generate_series(1, $DIM))
  FROM generate_series(1, $N) AS i;
SQL
GEN_END=$(now)
GEN_TIME=$(elapsed "$GEN_START" "$GEN_END")
echo "data_generation_time_s: $GEN_TIME"

# ---- VRAM before build ---------------------------------------------------
# Optional measurement; guard so a missing/old nvidia-smi never aborts.
vram_used_mb() {
    nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>/dev/null \
        | head -1 | tr -d ' ' || echo "NA"
}
VRAM_BEFORE=$(vram_used_mb || echo "NA")
echo "vram_used_mb_before_build: $VRAM_BEFORE"

# ---- b. CREATE INDEX build time (wall clock) -----------------------------
echo "[bench] building cagra index"
BUILD_START=$(now)
$PSQL >/dev/null <<SQL
SET cuvs.index_dir='$IDX_DIR';
CREATE INDEX bench_cagra ON bench_items USING cagra (embedding vector_l2_ops);
SQL
BUILD_END=$(now)
BUILD_TIME=$(elapsed "$BUILD_START" "$BUILD_END")
echo "build_time_s: $BUILD_TIME"

VRAM_AFTER=$(vram_used_mb || echo "NA")
echo "vram_used_mb_after_build: $VRAM_AFTER"

# ---- artifact sizes (newest .cagra/.tids pair in IDX_DIR) ----------------
CAGRA_FILE=$(ls -t "$IDX_DIR"/*.cagra 2>/dev/null | head -1 || true)
TIDS_FILE=$(ls -t "$IDX_DIR"/*.tids 2>/dev/null | head -1 || true)
CAGRA_BYTES=NA
TIDS_BYTES=NA
if [ -n "$CAGRA_FILE" ]; then
    CAGRA_BYTES=$(ls -l "$CAGRA_FILE" | awk '{print $5}')
fi
if [ -n "$TIDS_FILE" ]; then
    TIDS_BYTES=$(ls -l "$TIDS_FILE" | awk '{print $5}')
fi
echo "cagra_artifact: ${CAGRA_FILE:-NA}"
echo "cagra_bytes: $CAGRA_BYTES"
echo "tids_artifact: ${TIDS_FILE:-NA}"
echo "tids_bytes: $TIDS_BYTES"

# ---- helper: a random query vector literal of dimension DIM --------------
# Generated server-side; returns a bracketed vector literal string.
rand_qvec() {
    q "SELECT (SELECT array_agg(random()::float4)::vector($DIM) FROM generate_series(1,$DIM))::text;"
}

QVEC=$(rand_qvec)

# ---- c. cold vs warm planning time --------------------------------------
# Cold = first EXPLAIN (no analyze) on a fresh connection. Warm = second
# EXPLAIN reusing the same connection (cached plan/relcache). Capture the
# "Planning Time" line from each.
echo "[bench] planning time cold/warm"
PLAN_OUT=$($PSQL -X <<SQL
SET cuvs.index_dir='$IDX_DIR';
EXPLAIN SELECT id FROM bench_items ORDER BY embedding <-> '$QVEC'::vector LIMIT $K;
EXPLAIN SELECT id FROM bench_items ORDER BY embedding <-> '$QVEC'::vector LIMIT $K;
SQL
)
COLD_PLAN=$(echo "$PLAN_OUT" | grep -i "Planning Time" | sed -n '1p' | grep -oE '[0-9.]+' | head -1)
WARM_PLAN=$(echo "$PLAN_OUT" | grep -i "Planning Time" | sed -n '2p' | grep -oE '[0-9.]+' | head -1)
echo "cold_planning_ms: ${COLD_PLAN:-NA}"
echo "warm_planning_ms: ${WARM_PLAN:-NA}"

# ---- d. execution latency percentiles + JIT + fallbacks ------------------
# Run M EXPLAIN (ANALYZE) queries with a fresh random vector each time.
# Parse "Execution Time" per query into a list, then compute p50/p95/p99 in
# awk. Detect JIT section (grep '^ *JIT:') from the first plan. Count
# "falling back" WARNINGs emitted on psql stderr across the sample.
echo "[bench] running $M analyze queries (percentiles, jit, fallbacks)"
LATENCIES=""
JIT_SECTION=no
FALLBACKS=0
FIRST=1
for i in $(seq 1 "$M"); do
    QV=$(rand_qvec)
    # stderr captured separately so we can count fallback WARNINGs.
    OUT=$($PSQL -X 2>/tmp/bench_stderr.$$ <<SQL || true
SET cuvs.index_dir='$IDX_DIR';
EXPLAIN (ANALYZE) SELECT id FROM bench_items ORDER BY embedding <-> '$QV'::vector LIMIT $K;
SQL
)
    ET=$(echo "$OUT" | grep -i "Execution Time" | grep -oE '[0-9.]+' | head -1)
    if [ -n "$ET" ]; then
        LATENCIES="$LATENCIES$ET
"
    fi
    # JIT detection on the first query's plan is sufficient (plan is stable).
    if [ "$FIRST" = "1" ]; then
        if echo "$OUT" | grep -qE '^ *JIT:'; then
            JIT_SECTION=yes
        fi
        FIRST=0
    fi
    # Count fallback warnings from this query's stderr.
    FB=$(grep -ic "falling back" /tmp/bench_stderr.$$ 2>/dev/null || echo 0)
    FALLBACKS=$((FALLBACKS + FB))
done
rm -f /tmp/bench_stderr.$$

# percentiles via awk (nearest-rank on sorted ascending list)
pctl() {
    local p=$1
    echo "$LATENCIES" | grep -E '[0-9]' | sort -g | awk -v p="$p" '
        { a[NR]=$1 }
        END {
            if (NR==0) { print "NA"; exit }
            r = p/100.0*NR; idx = int(r); if (idx < 1) idx=1; if (idx > NR) idx=NR;
            printf "%.3f", a[idx]
        }'
}
P50=$(pctl 50)
P95=$(pctl 95)
P99=$(pctl 99)
echo "exec_p50_ms: $P50"
echo "exec_p95_ms: $P95"
echo "exec_p99_ms: $P99"
echo "jit_section: $JIT_SECTION"
echo "fallbacks: $FALLBACKS"

# Last GPU search latency reported by the daemon (micro-seconds).
LAST_US=$(q "SELECT pg_cuvs_last_search_latency_us();" 2>/dev/null || echo "NA")
echo "last_search_latency_us: ${LAST_US:-NA}"

# ---- per-backend CUDA context note (ADR-002 confirmation) ----------------
# Only the daemon should hold a CUDA context; PG backends must hold none.
# --query-compute-apps flags vary by driver version; guard against failure.
echo "[bench] CUDA compute apps (expect only pg_cuvs_server)"
COMPUTE_APPS=$(nvidia-smi --query-compute-apps=pid,used_memory --format=csv,noheader 2>/dev/null \
    || echo "UNSUPPORTED: nvidia-smi --query-compute-apps not available on this driver")
echo "compute_apps: $(echo "$COMPUTE_APPS" | tr '\n' ';')"

# ---- daemon restart reload time ------------------------------------------
# Restart the daemon, then loop a quick k-NN query until it succeeds (index
# reloaded and queryable) or RELOAD_TIMEOUT elapses.
echo "[bench] restart daemon and measure reload time"
sudo systemctl restart pg-cuvs-server
RELOAD_START=$(now)
RELOAD_TIME=NA
RQV=$(echo "$QVEC")
deadline=$(awk "BEGIN{print $RELOAD_START + $RELOAD_TIMEOUT}")
while :; do
    if q "SELECT id FROM bench_items ORDER BY embedding <-> '$RQV'::vector LIMIT $K;" >/dev/null 2>&1; then
        RELOAD_END=$(now)
        RELOAD_TIME=$(elapsed "$RELOAD_START" "$RELOAD_END")
        break
    fi
    nowt=$(now)
    if awk "BEGIN{exit !($nowt > $deadline)}"; then
        echo "[bench] WARN: index not requeryable within ${RELOAD_TIMEOUT}s"
        break
    fi
    sleep 1
done
echo "reload_time_s: $RELOAD_TIME"

# ---- final SUMMARY block (greppable key: value lines) --------------------
echo ""
echo "[bench] SUMMARY"
echo "n: $N"
echo "dim: $DIM"
echo "k: $K"
echo "query_sample_m: $M"
echo "data_generation_time_s: $GEN_TIME"
echo "build_time_s: $BUILD_TIME"
echo "cagra_bytes: $CAGRA_BYTES"
echo "tids_bytes: $TIDS_BYTES"
echo "vram_used_mb_before_build: $VRAM_BEFORE"
echo "vram_used_mb_after_build: $VRAM_AFTER"
echo "cold_planning_ms: ${COLD_PLAN:-NA}"
echo "warm_planning_ms: ${WARM_PLAN:-NA}"
echo "exec_p50_ms: $P50"
echo "exec_p95_ms: $P95"
echo "exec_p99_ms: $P99"
echo "jit_section: $JIT_SECTION"
echo "fallbacks: $FALLBACKS"
echo "last_search_latency_us: ${LAST_US:-NA}"
echo "reload_time_s: $RELOAD_TIME"
echo "compute_apps: $(echo "$COMPUTE_APPS" | tr '\n' ';')"

# ---- cleanup (leave daemon running) --------------------------------------
echo "[bench] cleanup: DROP TABLE bench_items"
$PSQL -c "DROP TABLE IF EXISTS bench_items;" >/dev/null
echo "[bench] DONE"
