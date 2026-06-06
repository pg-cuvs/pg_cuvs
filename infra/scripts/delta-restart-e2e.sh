#!/bin/bash
# delta-restart-e2e.sh — Phase 3A pending-delta durability + fail-closed across a
# daemon restart. pg_regress cannot restart the daemon or corrupt an artifact, so
# this runs as a manual/e2e harness on the GPU VM:
#   make gpu-test-delta-restart   (or via gpu-test-all)
#
# Two properties (PLAN.md 3A: "daemon restart 후 delta 유실/손상 시 incomplete GPU
# 결과를 서빙하지 않는다"):
#   (1) DURABILITY — a VALID pending .delta survives a daemon restart: the daemon
#       reloads the base index and rebuilds the resident GPU delta cache from the
#       persisted .delta, so the merged top-k is unchanged.
#   (2) FAIL-CLOSED — a CORRUPT .delta must never yield an incomplete base-only
#       GPU result: the plan-time validity gate (cuvs_index_delta_unusable)
#       rejects it and the query CPU-reroutes, so the delta-inclusive answer is
#       still correct.
#
# Determinism: id 999 sits at a far extremum '[500,0,0,0]' reachable ONLY via the
# delta merge (the base graph, built before the INSERT, cannot return it). So the
# probe returning 999 proves the delta was honored; returning a base id (<=16)
# proves an incomplete/wrong result was served.
#
# Requires: pg_cuvs installed, pg-cuvs-server systemd unit, index dir
# /tmp/cuvs_indexes (matching the daemon --index-dir). The index dir is owned by
# postgres (0700), so artifact tampering uses sudo.

set -e

IDX_DIR=/tmp/cuvs_indexes
DB=postgres
SOCK=/tmp/.s.pg_cuvs

wait_daemon_ready() {
    for _ in $(seq 1 60); do
        [ -S "$SOCK" ] && return 0
        sleep 0.5
    done
    echo "[delta-e2e] FAIL: daemon socket $SOCK never appeared"; return 1
}

# Force the GPU path (enable_seqscan=off) so a probe that returns 999 proves the
# GPU+delta merge — not a CPU seqscan that would trivially see the heap row.
# -q suppresses SET command tags so only the SELECT result is captured.
qgpu() { psql -qd "$DB" -At -c "SET cuvs.index_dir='$IDX_DIR'; SET enable_seqscan=off; $1"; }
# Natural planner choice: the fail-closed reroute path is CPU, so let the planner
# pick seqscan when the GPU path is gated off as unusable.
qauto() { psql -qd "$DB" -At -c "SET cuvs.index_dir='$IDX_DIR'; $1"; }

PROBE="SELECT id FROM de_items ORDER BY embedding <-> '[500,0,0,0]'::vector LIMIT 1;"

echo "[delta-e2e] restart clean"
sudo systemctl restart pg-cuvs-server
wait_daemon_ready
sudo systemctl is-active pg-cuvs-server

echo "[delta-e2e] build base + append a pending-delta extremum (id 999)"
psql -d "$DB" -v ON_ERROR_STOP=1 <<SQL
SET cuvs.index_dir='$IDX_DIR';
DROP TABLE IF EXISTS de_items;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
CREATE TABLE de_items (id bigint, embedding vector(4));
INSERT INTO de_items SELECT g, ('['||g||',0,0,0]')::vector FROM generate_series(1,16) g;
CREATE INDEX de_cagra ON de_items USING cagra (embedding vector_l2_ops);
-- pending delta extremum: reachable only via the delta merge.
INSERT INTO de_items VALUES (999, '[500,0,0,0]');
SQL

# Warm so the daemon builds the resident GPU delta cache, then probe.
qgpu "$PROBE" >/dev/null
B1=$(qgpu "$PROBE")
echo "[delta-e2e] before restart (GPU+delta): id=$B1 (expect 999)"

echo "[delta-e2e] (1) restart with VALID .delta -> must survive"
sudo systemctl restart pg-cuvs-server
wait_daemon_ready
qgpu "$PROBE" >/dev/null   # re-warm the rebuilt delta cache
B2=$(qgpu "$PROBE")
echo "[delta-e2e] after restart (valid delta, GPU+delta): id=$B2 (expect 999)"

echo "[delta-e2e] (2) corrupt .delta (truncate body) + restart -> must fail closed"
DELTA=$(sudo sh -c "ls -t '$IDX_DIR'/*.delta 2>/dev/null" | head -1)
if [ -z "$DELTA" ]; then
    echo "[delta-e2e] FAIL: no .delta artifact found in $IDX_DIR"; exit 1
fi
# Keep the 32-byte header, drop the record body: cuvs_delta_validate then sees
# body_bytes=0 with n_rows>0 -> mismatch -> the gate marks the delta unusable.
sudo truncate -s 32 "$DELTA"
sudo systemctl restart pg-cuvs-server
wait_daemon_ready
# Let the planner reroute to CPU (the fail-closed path). The answer must remain
# correct (999) — never an incomplete base-only result.
B3=$(qauto "$PROBE")
echo "[delta-e2e] after corrupt delta (auto plan / CPU reroute): id=$B3 (expect 999)"

echo "[delta-e2e] cleanup"
psql -d "$DB" -c "DROP TABLE IF EXISTS de_items;" >/dev/null

if [ "$B1" = "999" ] && [ "$B2" = "999" ] && [ "$B3" = "999" ]; then
    echo "[delta-e2e] PASS: delta survives restart (valid) + corrupt delta fails closed to the correct result"
else
    echo "[delta-e2e] FAIL: before=$B1 after_valid=$B2 after_corrupt=$B3 (expect all 999)"
    exit 1
fi
echo "[delta-e2e] DONE"
