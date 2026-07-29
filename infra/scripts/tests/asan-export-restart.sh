#!/bin/bash
# asan-export-restart.sh — #101/#114 regression guard: heap-buffer-overflow in
# handle_export_adjacency when a CAGRA index reloaded after a daemon restart is
# exported (cuvs_cagra_extract_adjacency reads dim from the deserialize/extend
# placeholder, which is intentionally zeroed — the real dim lives on the
# strided dataset view, dim 13 -> stride 16 measured).
#
# Why not a recall-style probe: pgvector's HNSW scan rechecks the heap tuple,
# so a corrupted export still yields exact self-matches (300/300 measured with
# the defect present, #101). Only ASAN on the real CUDA daemon reproduces it —
# ci.yml's ASAN job is not a substitute: it builds with PGCUVS_CPU_SHIM=1, and
# the shim has neither the deserialize path nor the placeholder.
#
#   make gpu-test-asan-export   (NOT part of gpu-test-all — ASAN daemon build +
#   two restarts + a slow first CUDA context runs 5-10 min; not worth paying on
#   every regression cycle for a defect class that is not frequent)
#
# Fixture: dim=13, 300 rows — matches #101's own hand-verification exactly.
# dim=8 (the common test dimension) is already stride-aligned and hides the
# row-stride half of the bug.
#
# This test runs its own ASAN daemon manually (not through the
# pg-cuvs-server systemd unit): ASAN needs ASAN_OPTIONS=protect_shadow_gap=0
# (without it the shadow mapping collides with the CUDA driver's reservations
# and the daemon dies at startup with "no CUDA GPUs detected"), and setting
# that on the shared unit would leak into the production daemon's environment.
# The production daemon is stopped for the duration and restored on exit via a
# trap, regardless of outcome, so a failed run does not leave gpu-test-all
# running under a sanitizer.
#
# Requires: pg_cuvs installed, pg-cuvs-server systemd unit present (stopped
# and restarted by this script), /tmp writable for a dedicated test index dir.

set -e

TESTIDX=/tmp/cuvs_indexes_asan_test
SOCK=/tmp/.s.pg_cuvs
DB=postgres
ASAN_LOG=/tmp/asan_export_test.log
BIN="$(pg_config --bindir)/pg_cuvs_server"
ASAN_ENV="ASAN_OPTIONS=protect_shadow_gap=0:detect_leaks=0:abort_on_error=1:print_stacktrace=1"

# Waits for the process to actually be gone rather than a fixed sleep --
# graceful_shutdown() serializes resident indexes before exiting, and a fixed
# sleep that's too short lets the next launch's binary-install / relaunch race
# a daemon that is still alive (measured: this cost a stuck run where a
# leftover process from an earlier iteration held the GPU/socket while a new
# one was started on top of it).
kill_test_daemon() {
    pkill -x pg_cuvs_server 2>/dev/null || true
    for _ in $(seq 1 60); do
        pgrep -x pg_cuvs_server >/dev/null || return 0
        sleep 0.5
    done
    echo "[asan-export] pg_cuvs_server did not exit after SIGTERM, sending SIGKILL"
    pkill -9 -x pg_cuvs_server 2>/dev/null || true
    sleep 1
}

# Checks liveness, not just the socket, so a daemon that dies during warm-up
# fails in seconds with a log tail instead of after the full 5-minute timeout
# (a real ASAN abort during startup would otherwise look identical to a slow
# machine still initializing).
wait_socket() {
    for _ in $(seq 1 600); do
        [ -S "$SOCK" ] && return 0
        pgrep -x pg_cuvs_server >/dev/null || {
            echo "[asan-export] daemon process exited during startup"
            return 1
        }
        sleep 0.5
    done
    return 1
}

# setsid + disown: a bare `&` dies with the ssh session this script runs
# under (PATTERNS.md). rm -f first: a stale special file from the previous
# instance would let `[ -S ... ]` match before the new daemon has bound it.
launch_asan_daemon() {
    rm -f "$SOCK"
    env $ASAN_ENV setsid "$BIN" --socket "$SOCK" --index-dir "$TESTIDX" --gpu-devices 0 \
        > "$ASAN_LOG" 2>&1 < /dev/null &
    disown
    wait_socket || { echo "[asan-export] FAIL: daemon socket never appeared"; tail -n 40 "$ASAN_LOG"; exit 1; }
    chmod 666 "$SOCK"
}

# Always restores the production daemon, success or failure -- an ASAN build
# left running under gpu-test-all would make every later request pay the
# sanitizer's overhead and could mask or misattribute unrelated failures.
cleanup() {
    echo "[asan-export] cleanup: restoring production (non-ASAN, systemd) daemon"
    psql -qd "$DB" -c "DROP TABLE IF EXISTS asan_export_t;" 2>/dev/null || true
    kill_test_daemon
    sudo rm -rf "$TESTIDX"
    # Same mtime trap as the ASAN build above, in reverse: the objects on disk
    # are now the ASAN ones, so a plain `make server` sees them as up to date
    # and does nothing -- install-server would silently reinstall the ASAN
    # binary as "production". Confirmed the hard way: the systemd unit doesn't
    # set ASAN_OPTIONS, so that binary fails startup with "no CUDA GPUs
    # detected" after the same ~3min CUDA-context wait, and systemd's
    # Restart=on-failure retries it forever, each attempt paying the full
    # timeout. Force the rebuild and verify the sanitizer is gone before
    # trusting it, exactly as the ASAN build verified it was present.
    rm -f src/pg_cuvs_server.o src/cuvs_ipc_server.o src/cuvs_util_server.o \
          src/cuvs_objstore_server.o src/cuvs_build_corpus_server.o \
          src/cuvs_wrapper.o pg_cuvs_server
    make server 2>&1 | tail -n 5
    nm -D pg_cuvs_server 2>/dev/null | grep -q '__asan_init' \
        && { echo "[asan-export] FAIL: cleanup rebuild is still ASAN-linked, refusing to install"; return 1; }
    sudo make install-server
    sudo systemctl start pg-cuvs-server
    for _ in $(seq 1 600); do
        systemctl is-active --quiet pg-cuvs-server && break
        sleep 1
    done
    systemctl is-active pg-cuvs-server
}
trap cleanup EXIT

echo "[asan-export] stopping production daemon (this test runs its own)"
sudo systemctl stop pg-cuvs-server

echo "[asan-export] building ASAN daemon"
# make's staleness check is mtime-only -- it has no notion of "compiler flags
# changed", so a `server` target already up to date from a prior plain build
# is left alone and EXTRA_SERVER_CFLAGS silently does nothing (confirmed: a
# run against already-built objects logged "Nothing to be done for 'server'"
# and launched a daemon with no sanitizer at all). Force every object feeding
# the binary, matching $(SERVER_BIN)'s prerequisite list (Makefile:169).
rm -f src/pg_cuvs_server.o src/cuvs_ipc_server.o src/cuvs_util_server.o \
      src/cuvs_objstore_server.o src/cuvs_build_corpus_server.o \
      src/cuvs_wrapper.o pg_cuvs_server
make server \
    EXTRA_SERVER_CFLAGS="-fsanitize=address -fno-omit-frame-pointer -O1" \
    EXTRA_SERVER_LDFLAGS="-fsanitize=address" 2>&1 | tail -n 10
# Verify the sanitizer is actually linked in before trusting the rest of the
# run -- silently testing a plain build would report a false PASS.
nm -D pg_cuvs_server 2>/dev/null | grep -q '__asan_init' \
    || { echo "[asan-export] FAIL: pg_cuvs_server was not built with ASAN"; exit 1; }
sudo make install-server

sudo mkdir -p "$TESTIDX"
sudo chmod 777 "$TESTIDX"

echo "[asan-export] launch 1: build the CAGRA index"
launch_asan_daemon
psql -d "$DB" -v ON_ERROR_STOP=1 <<SQL
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
RESET client_min_messages;
SET cuvs.index_dir='$TESTIDX';
DROP TABLE IF EXISTS asan_export_t;
CREATE TABLE asan_export_t (id bigint, embedding vector(13));
INSERT INTO asan_export_t
    SELECT g, ('[' || array_to_string(ARRAY(
        SELECT (random())::numeric(6,4) FROM generate_series(1,13)), ',') || ']')::vector
    FROM generate_series(1, 300) g;
CREATE INDEX asan_export_cagra ON asan_export_t USING cagra (embedding vector_l2_ops);
SQL

echo "[asan-export] restart: the only thing that puts the index on the deserialize path"
kill_test_daemon
launch_asan_daemon

echo "[asan-export] export: pg_cuvs_build_hnsw() must not ASAN-fault"
set +e
psql -d "$DB" -c "SET cuvs.index_dir='$TESTIDX'; SELECT pg_cuvs_build_hnsw('asan_export_cagra'::regclass);"
PSQL_RC=$?
set -e

echo "[asan-export] daemon log tail:"
tail -n 40 "$ASAN_LOG"

if grep -q "ERROR: AddressSanitizer" "$ASAN_LOG"; then
    echo "[asan-export] FAIL: AddressSanitizer report detected (regression of #101)"
    exit 1
fi
if [ "$PSQL_RC" -ne 0 ]; then
    echo "[asan-export] FAIL: export query failed (rc=$PSQL_RC) with no ASAN report -- investigate"
    exit 1
fi

echo "[asan-export] PASS: no AddressSanitizer report, export succeeded"
