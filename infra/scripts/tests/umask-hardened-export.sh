#!/bin/bash
# #166 review P1: the HNSW export must survive a hardened daemon umask.
#
# The daemon hands the serialized sidecar to the backend as an SCM_RIGHTS fd.
# Reads through that descriptor carry the access rights granted at open time, so
# they work regardless of the file's mode. Reaching the same inode by *path*
# does not: opening "/proc/self/fd/N" re-checks inode permissions, and with
# umask 077 the segment lands 0600 owned by the daemon uid — the PG backend
# (a different uid) then gets EACCES. That is why the importer consumes the
# descriptor directly instead of reopening a path.
#
# Pre-fix reproduction on this harness:
#   ERROR:  pg_cuvs: cannot open .hnsw sidecar "/proc/self/fd/48": Permission denied
#
# ---------------------------------------------------------------------------
# ISOLATION — why this runs its own daemon (#166 review 3)
#
# Revision 1 hardened the *system* daemon with a systemd drop-in and then ran
# `sudo chmod 644 "$IDIR"/*` over the shared, world-writable /tmp/cuvs_indexes
# to undo the damage. Two problems: a planted symlink there turns that chmod
# into an arbitrary root-level permission change, and it permanently widened
# persistent sidecars to 0644.
#
# Revision 2 dropped the chmod and pointed `cuvs.index_dir` at a private
# directory — which was not enough, and the harness proved it. That GUC only
# tells the *backend* where to look; the daemon keeps writing to the
# --index-dir it was started with. Hardening the system daemon therefore
# re-persisted the shared 1M fixture at 0600 and broke the next test in the
# suite:
#   ERROR:  pg_cuvs: cannot open .tids sidecar
#           "/tmp/cuvs_indexes/16385_3029972.tids": Permission denied
#
# So this revision never touches the system daemon or the shared directory at
# all. It stops the systemd unit, runs its own daemon against a private
# --index-dir with the umask set on the process itself, and restores the unit
# on exit. Same pattern as asan-export-restart.sh.
#
# SCOPE — transient handoff (#166) and persistent sidecars (#167).
#
# Persistent sidecars inherit the daemon's umask too, and the backend reads
# .tids by path (hnsw_export.c) — a name, not an fd, so #166's fix cannot help
# it. That was #167: a daemon hardened for its whole lifetime wrote .tids/.cagra
# at 0600, and the phase-2 export below (which reads .tids by path to build the
# pgvector HNSW) failed with EACCES even though the transient .hnsw handoff
# itself was fine.
#
# Fixed by chmod'ing every index_dir sidecar to 0644 right after it is written
# (pg_cuvs_server.c fchmod_sidecar/chmod_sidecar), independent of the writing
# process's umask — same reasoning as #166, applied to the files fd-passing
# cannot reach because they must still exist after a restart.
#
# Phase 1 now builds the fixture under the SAME hardened umask as phase 2, so
# .tids/.cagra are also written hardened — this is what proves #167, since
# phase 2's export reads .tids by path regardless of #166's fix.
#
# Run on the GPU VM; needs sudo (systemctl only) and a built pg_cuvs_server.
set -u
DB="${PGDATABASE:-shadeform}"
TESTIDX=/tmp/cuvs_indexes_umask_test
SOCK=/tmp/.s.pg_cuvs
BIN="$(pg_config --bindir)/pg_cuvs_server"
DAEMON_LOG=/tmp/umask_daemon.log
PSQL="psql -d ${DB} -qtA -v ON_ERROR_STOP=1"
PASS=0; FAIL=0
ok(){  PASS=$((PASS+1)); echo "  [PASS] $1"; }
bad(){ FAIL=$((FAIL+1)); echo "  [FAIL] $1"; }

# psql writes NOTICEs to the same stream, and DROP INDEX IF EXISTS emits one
# before the statement that actually fails — so `head -1` reported
#   NOTICE: index "umask_t_hnsw" does not exist, skipping
# as the cause. Surface the ERROR line instead, which is what a negative
# control needs to be readable at all.
why(){ grep -m1 '^ERROR' "$1" 2>/dev/null || head -1 "$1" 2>/dev/null; }

# Waits for the process to be gone, not a fixed sleep: graceful shutdown
# serializes resident indexes first (asan-export-restart.sh learned this).
kill_test_daemon() {
    pkill -x pg_cuvs_server 2>/dev/null || true
    for _ in $(seq 1 60); do
        pgrep -x pg_cuvs_server >/dev/null || return 0
        sleep 0.5
    done
    pkill -9 -x pg_cuvs_server 2>/dev/null || true
    sleep 1
}

wait_socket() {
    for _ in $(seq 1 600); do
        [ -S "$SOCK" ] && return 0
        pgrep -x pg_cuvs_server >/dev/null || {
            echo "  daemon exited during startup:"; tail -n 20 "$DAEMON_LOG"; return 1
        }
        sleep 0.5
    done
    return 1
}

# $1 = umask for the daemon process
launch_daemon() {
    rm -f "$SOCK"
    ( umask "$1"; setsid "$BIN" --socket "$SOCK" --index-dir "$TESTIDX" \
        --gpu-devices 0 > "$DAEMON_LOG" 2>&1 < /dev/null & )
    wait_socket || return 1
    # Production gets this from the unit's ExecStartPost; a bare daemon under
    # umask 077 would bind the socket 0700 and the backend (other uid) could
    # not connect at all — which would fail this test for the wrong reason.
    chmod 666 "$SOCK"
}

cleanup(){
    psql -qd "$DB" -c "DROP TABLE IF EXISTS umask_t CASCADE;" >/dev/null 2>&1 || true
    kill_test_daemon
    rm -rf "$TESTIDX"
    sudo systemctl start pg-cuvs-server || true
    sleep 4
}
trap cleanup EXIT

echo "== #166/#167 hardened-umask export =="

sudo systemctl stop pg-cuvs-server || true
kill_test_daemon
rm -rf "$TESTIDX"; mkdir -p "$TESTIDX"; chmod 755 "$TESTIDX"

# ---- phase 1: fixture, HARDENED umask -> .tids/.cagra written 0600 (#167) --
launch_daemon 077 || { bad "daemon launch (umask 077, fixture)"; exit 1; }

# 2000 rows clears CUVS_HNSW_MIN_ELEMENTS (16) with room for the CAGRA graph.
if $PSQL <<SQL >/dev/null 2>/tmp/umask_setup.txt
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
RESET client_min_messages;
SET cuvs.index_dir='$TESTIDX';
DROP TABLE IF EXISTS umask_t CASCADE;
CREATE TABLE umask_t (id bigint, embedding vector(16));
INSERT INTO umask_t
    SELECT g, ('[' || array_to_string(ARRAY(
        SELECT (random())::numeric(6,4) FROM generate_series(1,16)), ',') || ']')::vector
    FROM generate_series(1, 2000) g;
CREATE INDEX umask_t_cagra ON umask_t USING cagra (embedding vector_l2_ops);
SQL
then
  ok "fixture built under hardened umask (.tids/.cagra at 077)"
else
  bad "fixture build: $(why /tmp/umask_setup.txt)"; exit 1
fi

# ---- phase 2: same index_dir, hardened daemon ------------------------------
kill_test_daemon
rm -f /dev/shm/pg_cuvs_*
launch_daemon 077 || { bad "daemon launch (umask 077)"; exit 1; }

# Read the umask off the process itself rather than trusting the subshell.
dpid=$(pgrep -x pg_cuvs_server | head -1)
actual=$(grep -i '^Umask:' "/proc/${dpid}/status" 2>/dev/null | awk '{print $2}')
[ "$actual" = "0077" ] && ok "daemon running with umask 0077" \
                       || bad "expected umask 0077, got '${actual:-unknown}'"

for mode in hnswlib nsw; do
  if $PSQL -c "SET cuvs.index_dir='${TESTIDX}';
               DROP INDEX IF EXISTS umask_t_hnsw;
               CREATE INDEX umask_t_hnsw ON umask_t USING pg_cuvs_hnsw (embedding vector_l2_ops)
                      WITH (source='umask_t_cagra', mode='${mode}');" >/dev/null 2>/tmp/umask_err.txt; then
    ok "export mode=${mode} under umask 077"
  else
    bad "export mode=${mode} under umask 077: $(why /tmp/umask_err.txt)"
  fi
done

r=$(ls /dev/shm 2>/dev/null | grep -c '^pg_cuvs_')
[ "$r" = "0" ] && ok "no pg_cuvs_* residue under umask 077" \
               || bad "pg_cuvs_* residue=$r under umask 077"

echo "== $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
