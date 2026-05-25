#!/bin/bash
# integration-test.sh — fault-injection integration tests for pg_cuvs.
#
# Drives the daemon durability contract end to end against a SEPARATE test
# daemon built with -DCUVS_TEST_HOOKS, on a TEST socket + TEST index dir so
# the production pg-cuvs-server unit and its data are never touched. Runs on
# the GPU VM:
#   make gpu-test-daemon
#
# Scenarios:
#   1. daemon DOWN   -> CREATE INDEX USING cagra ERRORs (UNAVAILABLE),
#                       catalog rolls back, CPU-fallback search still works.
#   2. SERIALIZE fault -> CREATE INDEX ERRORs (PERSIST), catalog rolls back,
#                       no stray .cagra/.tids left in the test index dir.
#   3. TIDS_WRITE fault -> same contract as #2.
#   4. clean run     -> CREATE INDEX succeeds, NN search ordered correctly.
#
# Requires: pg_cuvs installed; the production pg-cuvs-server systemd unit
# (stopped during the run, restarted at cleanup); CONDA_ENV exported so the
# test daemon can be compiled with `make server-test`.

set -e

REPO=~/pg_cuvs
DB=postgres
TEST_SOCK=/tmp/.s.pg_cuvs_test
TEST_IDX=/tmp/cuvs_indexes_test
TEST_BIN="$REPO/pg_cuvs_server_test"
DAEMON_PID=""
FAILED=0

pass() { echo "[PASS] $1"; }
fail() { echo "[FAIL] $1"; FAILED=1; }

# Run a SQL snippet with the test socket + index dir GUCs set. Returns psql
# exit status. Captures stderr+stdout into the named global OUT for asserts.
run_sql() {
    OUT=$(psql -d "$DB" -v ON_ERROR_STOP=1 2>&1 <<SQL
SET cuvs.socket_path = '$TEST_SOCK';
SET cuvs.index_dir = '$TEST_IDX';
$1
SQL
)
    return $?
}

start_test_daemon() {
    # Extra env (fault vars) passed as "VAR=1" arguments before the binary.
    echo "[it] start test daemon ($*)"
    env "$@" "$TEST_BIN" \
        --socket "$TEST_SOCK" \
        --index-dir "$TEST_IDX" \
        --max-vram-mb 20480 \
        >/tmp/pg_cuvs_test_daemon.log 2>&1 &
    DAEMON_PID=$!
    sleep 2
    if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
        echo "[it] test daemon failed to start; log:"
        cat /tmp/pg_cuvs_test_daemon.log || true
        return 1
    fi
}

stop_test_daemon() {
    if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
        echo "[it] stop test daemon (pid $DAEMON_PID)"
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    DAEMON_PID=""
    rm -f "$TEST_SOCK"
}

cleanup() {
    echo "[it] cleanup"
    stop_test_daemon
    rm -rf "$TEST_IDX"
    psql -d "$DB" -c "DROP TABLE IF EXISTS it_items;" >/dev/null 2>&1 || true
    echo "[it] restart production pg-cuvs-server"
    sudo systemctl start pg-cuvs-server 2>/dev/null || true
}
trap cleanup EXIT

# Number of cagra index rows for it_items in the catalog (expect 0 on rollback).
count_cagra_index() {
    psql -d "$DB" -At -c \
        "SELECT count(*) FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid \
         JOIN pg_am a ON a.oid = c.relam \
         WHERE i.indrelid = 'it_items'::regclass AND a.amname = 'cagra';" \
        2>/dev/null || echo "ERR"
}

# Reset table + clean test index dir before each scenario.
fresh_fixture() {
    rm -rf "$TEST_IDX"
    mkdir -p "$TEST_IDX"
    psql -d "$DB" -v ON_ERROR_STOP=1 >/dev/null <<SQL
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
DROP TABLE IF EXISTS it_items;
CREATE TABLE it_items (id bigint, embedding vector(4));
INSERT INTO it_items VALUES
  (1,'[1,0,0,0]'),(2,'[0,1,0,0]'),(3,'[0,0,1,0]'),(4,'[0,0,0,1]'),
  (5,'[0.9,0.1,0,0]'),(6,'[0,0.9,0.1,0]'),(7,'[0,0,0.9,0.1]'),(8,'[0.8,0,0,0.2]');
SQL
}

# Assert no stray persisted index artifacts remain in the test index dir.
no_stray_artifacts() {
    local n
    n=$(find "$TEST_IDX" -maxdepth 1 \( -name '*.cagra' -o -name '*.tids' \) 2>/dev/null | wc -l | tr -d ' ')
    [ "$n" = "0" ]
}

echo "[it] === pg_cuvs fault-injection integration tests ==="

echo "[it] build CUVS_TEST_HOOKS daemon"
( cd "$REPO" && make server-test )
test -x "$TEST_BIN" || { echo "[it] FAIL: $TEST_BIN not built"; exit 1; }

echo "[it] stop production pg-cuvs-server (will be restarted at cleanup)"
sudo systemctl stop pg-cuvs-server 2>/dev/null || true
rm -f "$TEST_SOCK"
sleep 1

# --- Scenario 1: daemon DOWN --------------------------------------------
echo "[it] --- scenario 1: daemon DOWN ---"
fresh_fixture
# No test daemon started; socket_path points at a dead socket.
if run_sql "CREATE INDEX it_cagra ON it_items USING cagra (embedding vector_l2_ops);"; then
    fail "daemon-down: CREATE INDEX unexpectedly succeeded"
else
    if echo "$OUT" | grep -q "BUILD failed (status"; then
        pass "daemon-down: CREATE INDEX ERRORed (status reported)"
    else
        fail "daemon-down: CREATE INDEX failed but error text unexpected: $OUT"
    fi
fi
if [ "$(count_cagra_index)" = "0" ]; then
    pass "daemon-down: catalog rolled back (no cagra pg_index row)"
else
    fail "daemon-down: stray cagra index in pg_index"
fi
# CPU fallback search still works with no cagra index present.
if run_sql "SET enable_cuvs = off; SELECT id FROM it_items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 3;"; then
    if echo "$OUT" | grep -q "^[[:space:]]*1$"; then
        pass "daemon-down: CPU-fallback search returned rows (nearest id=1)"
    else
        fail "daemon-down: CPU-fallback search output unexpected: $OUT"
    fi
else
    fail "daemon-down: CPU-fallback search errored: $OUT"
fi

# --- Scenario 2: SERIALIZE fault ----------------------------------------
echo "[it] --- scenario 2: CUVS_FAULT_SERIALIZE ---"
fresh_fixture
start_test_daemon CUVS_FAULT_SERIALIZE=1
if run_sql "CREATE INDEX it_cagra ON it_items USING cagra (embedding vector_l2_ops);"; then
    fail "serialize-fault: CREATE INDEX unexpectedly succeeded"
else
    if echo "$OUT" | grep -q "BUILD failed (status"; then
        pass "serialize-fault: CREATE INDEX ERRORed (persist failure)"
    else
        fail "serialize-fault: error text unexpected: $OUT"
    fi
fi
if [ "$(count_cagra_index)" = "0" ]; then
    pass "serialize-fault: catalog rolled back (no cagra pg_index row)"
else
    fail "serialize-fault: stray cagra index in pg_index"
fi
if no_stray_artifacts; then
    pass "serialize-fault: no stray .cagra/.tids in test index dir"
else
    fail "serialize-fault: stray artifacts left: $(ls -1 "$TEST_IDX")"
fi
stop_test_daemon

# --- Scenario 3: TIDS_WRITE fault ---------------------------------------
echo "[it] --- scenario 3: CUVS_FAULT_TIDS_WRITE ---"
fresh_fixture
start_test_daemon CUVS_FAULT_TIDS_WRITE=1
if run_sql "CREATE INDEX it_cagra ON it_items USING cagra (embedding vector_l2_ops);"; then
    fail "tids-fault: CREATE INDEX unexpectedly succeeded"
else
    if echo "$OUT" | grep -q "BUILD failed (status"; then
        pass "tids-fault: CREATE INDEX ERRORed (persist failure)"
    else
        fail "tids-fault: error text unexpected: $OUT"
    fi
fi
if [ "$(count_cagra_index)" = "0" ]; then
    pass "tids-fault: catalog rolled back (no cagra pg_index row)"
else
    fail "tids-fault: stray cagra index in pg_index"
fi
if no_stray_artifacts; then
    pass "tids-fault: no stray .cagra/.tids in test index dir"
else
    fail "tids-fault: stray artifacts left: $(ls -1 "$TEST_IDX")"
fi
stop_test_daemon

# --- Scenario 4: clean run ----------------------------------------------
echo "[it] --- scenario 4: clean build + search ---"
fresh_fixture
start_test_daemon
if run_sql "CREATE INDEX it_cagra ON it_items USING cagra (embedding vector_l2_ops);"; then
    pass "clean: CREATE INDEX succeeded"
else
    fail "clean: CREATE INDEX errored: $OUT"
fi
if [ "$(count_cagra_index)" = "1" ]; then
    pass "clean: cagra index present in pg_index"
else
    fail "clean: expected 1 cagra index, found $(count_cagra_index)"
fi
if run_sql "SELECT id FROM it_items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 3;"; then
    # Nearest to [1,0,0,0]: id 1 ([1,0,0,0]) then 5 ([0.9,0.1,0,0]).
    NN=$(echo "$OUT" | grep -E "^[[:space:]]*[0-9]+$" | head -1 | tr -d ' ')
    if [ "$NN" = "1" ]; then
        pass "clean: GPU search returned nearest id=1"
    else
        fail "clean: nearest neighbor was '$NN', expected 1 (out: $OUT)"
    fi
else
    fail "clean: search errored: $OUT"
fi
stop_test_daemon

echo "[it] === summary ==="
if [ "$FAILED" = "0" ]; then
    echo "[PASS] all integration scenarios passed"
    exit 0
else
    echo "[FAIL] one or more integration scenarios failed"
    exit 1
fi
