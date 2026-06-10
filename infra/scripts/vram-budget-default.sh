#!/bin/bash
# vram-budget-default.sh — ADR-065: a daemon started WITHOUT --max-vram-mb must
# default to a sane VRAM budget (a fraction of total VRAM), NOT unlimited, so an
# external operator who forgets the flag cannot OOM the device.
#
#   make gpu-test-vram
#
# Runs a DEDICATED daemon (no --max-vram-mb) on a test socket/index-dir, builds a
# tiny index, and asserts the per-GPU budget reported by pg_stat_gpu_cache is
# >0 and < total (a fraction), plus the startup log shows the default path. The
# production pg-cuvs-server unit is stopped for the run and restarted at cleanup.

set -e

DB=postgres
SOCK=/tmp/.s.pg_cuvs_vram
IDX=/tmp/cuvs_indexes_vram
BIN=/usr/lib/postgresql/16/bin/pg_cuvs_server
LOG=/tmp/pg_cuvs_vram_daemon.log
DAEMON_PID=""
FAILED=0

pass() { echo "[PASS] $1"; }
fail() { echo "[FAIL] $1"; FAILED=1; }

cleanup() {
    echo "[vram] cleanup"
    [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null || true
    [ -n "$DAEMON_PID" ] && wait "$DAEMON_PID" 2>/dev/null || true
    rm -f "$SOCK"; rm -rf "$IDX"
    psql -d "$DB" -c "DROP TABLE IF EXISTS vb;" >/dev/null 2>&1 || true
    echo "[vram] restart production pg-cuvs-server"
    sudo systemctl start pg-cuvs-server 2>/dev/null || true
}
trap cleanup EXIT

run_sql() {
    psql -d "$DB" -tA -v ON_ERROR_STOP=1 2>/dev/null <<SQL | grep -v '^SET$'
SET cuvs.socket_path = '$SOCK';
SET cuvs.index_dir = '$IDX';
$1
SQL
}

echo "[vram] stop production daemon for the duration"
sudo systemctl stop pg-cuvs-server 2>/dev/null || true
rm -rf "$IDX"; mkdir -p "$IDX"; chmod 0777 "$IDX"

echo "[vram] start daemon WITHOUT --max-vram-mb (default-budget path)"
"$BIN" --socket "$SOCK" --index-dir "$IDX" >"$LOG" 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 60); do
    [ -S "$SOCK" ] && break
    kill -0 "$DAEMON_PID" 2>/dev/null || { echo "[vram] daemon died:"; cat "$LOG"; exit 1; }
    sleep 0.5
done
[ -S "$SOCK" ] || { echo "[vram] socket never appeared:"; cat "$LOG"; exit 1; }
chmod 666 "$SOCK" 2>/dev/null || true
chmod 0777 "$IDX" 2>/dev/null || true

# Build a tiny index so pg_stat_gpu_cache reports a row for the active GPU.
run_sql "
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
DROP TABLE IF EXISTS vb;
CREATE TABLE vb (id int, v vector(8));
INSERT INTO vb SELECT g, ('[' || array_to_string(array_fill((g%10)::real/10, ARRAY[8]), ',') || ']')::vector
FROM generate_series(1, 100) g;
CREATE INDEX vb_cagra ON vb USING cagra (v vector_l2_ops);
" >/dev/null || { echo "[vram] build failed"; cat "$LOG"; exit 1; }

# ---- Assert: pg_stat_gpu_cache budget is a sane fraction, not unlimited -------
BUDGET=$(run_sql "SELECT vram_budget_mb FROM pg_stat_gpu_cache ORDER BY gpu_device_id LIMIT 1;" | tail -1)
TOTAL_LINE=$(grep -m1 "default fraction of total" "$LOG" || true)
TOTAL=$(echo "$TOTAL_LINE" | sed -n 's/.*: \([0-9]*\) MB total.*/\1/p')
LOG_BUDGET=$(echo "$TOTAL_LINE" | sed -n 's/.*budget \([0-9]*\) MB.*/\1/p')
echo "[vram] pg_stat budget=${BUDGET} MB; log total=${TOTAL} MB log budget=${LOG_BUDGET} MB"

if [ -n "$BUDGET" ] && [ "$BUDGET" -gt 0 ] 2>/dev/null && \
   [ -n "$TOTAL" ] && [ "$BUDGET" -lt "$TOTAL" ] 2>/dev/null; then
    pass "1. default budget is a sane fraction of total (not unlimited): ${BUDGET} MB < ${TOTAL} MB"
else
    fail "1. default budget wrong (want >0 and < total ${TOTAL}): ${BUDGET}"
fi

# ---- Assert: the startup log took the default-fraction path -------------------
if [ -n "$TOTAL_LINE" ] && [ -n "$LOG_BUDGET" ] && [ "$LOG_BUDGET" -gt 0 ] 2>/dev/null; then
    pass "2. daemon logged default-fraction budget (${LOG_BUDGET} MB of ${TOTAL} MB)"
else
    fail "2. daemon did not log the default-fraction path"; grep -i "budget" "$LOG" | tail -3
fi

if [ "$FAILED" -eq 0 ]; then
    echo "[vram] ALL PASS — default VRAM budget is bounded (ADR-065)"
else
    echo "[vram] FAILURES present"
fi
exit $FAILED
