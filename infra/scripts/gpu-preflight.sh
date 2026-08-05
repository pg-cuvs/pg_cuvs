#!/usr/bin/env bash
# gpu-preflight.sh — refuse to measure against stale artifacts.
#
# Why this exists: a measurement run against a daemon that was never restarted
# after `make install-server` reports the OLD binary's behaviour. That produces a
# false negative ("the fix changed nothing") that looks exactly like a real
# result. This happened; the daemon binary was 5 minutes newer than the running
# process and nobody noticed until the timestamps were compared by hand.
#
# Checks (all fail loud, none are advisory):
#   1. running daemon process older than the installed daemon binary
#   2. installed extension .so newer than the postmaster that loaded it
#   3. DB extension version != pg_cuvs.control default_version
#   4. daemon socket present
#
# Usage: infra/scripts/gpu-preflight.sh [dbname]
#        VM host comes from gpu.conf / .env.gpu (VM_SSH_HOST, falls back to GCP_VM).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DB="${1:-shadeform}"

# Same config resolution the Makefile uses (gpu.conf first, legacy .env.gpu
# second) — but PARSED, not sourced.
#
# #169: sourcing was wrong and made this whole guard unreachable. gpu.conf is
# read by the Makefile via `-include`, so `VM_SSH_HOST = host` (spaces around
# `=`, Makefile style) is a legal and natural thing to write there. `.` on that
# line runs `VM_SSH_HOST` as a command, returns 127, and `set -e` kills the
# script before a single check runs. gpu-run.sh — the runner the PreToolUse hook
# tells everyone to use — inherited the same failure, so its preflight never
# ran either.
conf_get() {
    local key="$1" f v
    for f in "$REPO_ROOT/gpu.conf" "$REPO_ROOT/.env.gpu"; do
        [ -f "$f" ] || continue
        v=$(sed -n "s/^[[:space:]]*$key[[:space:]]*=[[:space:]]*//p" "$f" | tail -1)
        v=${v%%#*}                                  # trailing comment
        v="$(printf '%s' "$v" | tr -d '"'\''' | xargs 2>/dev/null || true)"
        [ -n "$v" ] && { printf '%s' "$v"; return 0; }
    done
    return 0
}

VM_SSH_HOST="${VM_SSH_HOST:-$(conf_get VM_SSH_HOST)}"
[ -n "$VM_SSH_HOST" ] || VM_SSH_HOST="$(conf_get GCP_VM)"
if [ -z "$VM_SSH_HOST" ]; then
    echo "[preflight] FAIL: VM_SSH_HOST unset (set it in gpu.conf)" >&2
    exit 1
fi

ssh -o BatchMode=yes "$VM_SSH_HOST" "DB='$DB' bash -s" <<'REMOTE'
set -uo pipefail
fail=0
say()  { printf '[preflight] %s\n' "$*"; }
bad()  { printf '[preflight] FAIL: %s\n' "$*" >&2; fail=1; }

PGBIN=/usr/lib/postgresql/16/bin
export PGHOST=/var/run/postgresql

# 1. daemon process vs installed binary -------------------------------------
dpid=$(pgrep -f "bin/pg_cuvs_server" | head -1 || true)
if [ -z "$dpid" ]; then
    bad "pg_cuvs_server is not running"
else
    bin=$PGBIN/pg_cuvs_server
    bin_mtime=$(stat -c %Y "$bin" 2>/dev/null || echo 0)
    # process start as epoch seconds
    proc_start=$(date -d "$(ps -o lstart= -p "$dpid")" +%s 2>/dev/null || echo 0)
    if [ "$bin_mtime" -gt "$proc_start" ]; then
        bad "daemon binary is NEWER than the running daemon
             binary  $(date -d @"$bin_mtime" '+%F %T')
             process $(date -d @"$proc_start" '+%F %T')
             → sudo systemctl restart pg-cuvs-server   (measuring now reads the OLD code)"
    else
        say "daemon fresh (started $(date -d @"$proc_start" '+%T') >= binary $(date -d @"$bin_mtime" '+%T'))"
    fi
fi

# 2. extension .so vs postmaster --------------------------------------------
so=$($PGBIN/pg_config --pkglibdir)/pg_cuvs.so
if [ -f "$so" ]; then
    so_mtime=$(stat -c %Y "$so")
    ppid_=$(pgrep -x postgres | head -1 || true)
    if [ -n "$ppid_" ]; then
        pm_start=$(date -d "$(ps -o lstart= -p "$ppid_")" +%s 2>/dev/null || echo 0)
        if [ "$so_mtime" -gt "$pm_start" ]; then
            bad "pg_cuvs.so is NEWER than the postmaster that loaded it
             → sudo systemctl restart postgresql@16-main"
        else
            say "extension .so fresh"
        fi
    fi
fi

# 3. DB extension version vs control default --------------------------------
ctl=$($PGBIN/pg_config --sharedir)/extension/pg_cuvs.control
want=$(sed -n "s/^default_version[[:space:]]*=[[:space:]]*'\([^']*\)'.*/\1/p" "$ctl" 2>/dev/null || true)
have=$($PGBIN/psql -d "$DB" -tAc \
        "SELECT extversion FROM pg_extension WHERE extname='pg_cuvs'" 2>/dev/null || true)
if [ -n "$want" ] && [ -n "$have" ] && [ "$want" != "$have" ]; then
    bad "extension version drift in db '$DB': installed=$have control=$want
             → psql -d $DB -c 'ALTER EXTENSION pg_cuvs UPDATE'"
elif [ -n "$have" ]; then
    say "extension $have (matches control)"
fi

# 4. socket ------------------------------------------------------------------
[ -S /tmp/.s.pg_cuvs ] || bad "daemon socket /tmp/.s.pg_cuvs missing"

exit $fail
REMOTE
