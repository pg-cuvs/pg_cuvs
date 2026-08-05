#!/bin/bash
# #169: restart both halves of the deployment so the RUNNING processes are the
# ones that were just installed.
#
# Neither half picks up a new artifact on its own:
#   - the extension .so is loaded at postmaster start (shared_preload_libraries)
#   - the daemon is long-lived; installing its binary does not respawn it
#
# Order: PostgreSQL first, then the daemon. The daemon's first CUDA context
# takes 12s-3min on this class of machine (the unit allows TimeoutStartSec=600),
# and systemd reports `activating` throughout — so wait for `active` rather than
# sleeping a fixed amount. A fixed sleep is what lets a test start against a
# daemon that has not bound its socket yet.
set -u

PG_UNIT="${PG_UNIT:-postgresql@16-main}"
DAEMON_UNIT="${DAEMON_UNIT:-pg-cuvs-server}"
WAIT_SECS="${WAIT_SECS:-360}"

echo "== #169 reload: $PG_UNIT, $DAEMON_UNIT =="

sudo systemctl restart "$PG_UNIT"   || { echo "FATAL: $PG_UNIT restart failed"; exit 1; }
sudo systemctl restart "$DAEMON_UNIT" || { echo "FATAL: $DAEMON_UNIT restart failed"; exit 1; }

deadline=$(( $(date +%s) + WAIT_SECS ))
while :; do
    state=$(systemctl is-active "$DAEMON_UNIT" 2>/dev/null)
    case "$state" in
        active) break ;;
        failed)
            echo "FATAL: $DAEMON_UNIT entered failed state"
            sudo journalctl -u "$DAEMON_UNIT" -n 40 --no-pager
            exit 1 ;;
    esac
    if [ "$(date +%s)" -ge "$deadline" ]; then
        echo "FATAL: $DAEMON_UNIT still '$state' after ${WAIT_SECS}s"
        sudo journalctl -u "$DAEMON_UNIT" -n 40 --no-pager
        exit 1
    fi
    sleep 2
done

pg_state=$(systemctl is-active "$PG_UNIT")
[ "$pg_state" = active ] || { echo "FATAL: $PG_UNIT is '$pg_state'"; exit 1; }

echo "  daemon=$(systemctl is-active "$DAEMON_UNIT")  postgres=$pg_state"
