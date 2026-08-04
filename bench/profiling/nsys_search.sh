#!/bin/bash
# PR-E search-path nsys capture.
#   $1 = output label (report goes to /tmp/<label>.nsys-rep)
#   $2 = 1 to run the workload, 0 for a startup-only baseline
#
# Two captures are taken and subtracted: the daemon copies the whole 3.2 GB
# CAGRA index H2D during startup_load_indexes(), which would otherwise be
# counted as "search memcpy". baseline = startup only; the difference is the
# workload's own kernel/memcpy cost.
#
# Finalize is SIGTERM to pg_cuvs_server (in-process). --duration/SIGINT are
# known to corrupt the report (profiling-results.md, 2026-06).
set -u
LABEL="$1"
RUN_WORKLOAD="${2:-1}"
ARGV="--socket /tmp/.s.pg_cuvs --index-dir /tmp/cuvs_indexes --gpu-devices 0"
BIN=/usr/lib/postgresql/16/bin/pg_cuvs_server
export PGHOST=/var/run/postgresql
export PATH=/usr/lib/postgresql/16/bin:$PATH

echo "=== [$LABEL] stopping systemd unit ==="
sudo systemctl stop pg-cuvs-server
sleep 2
pgrep -f 'pg_cuvs_server --socket' && { echo "FATAL: daemon still running"; exit 1; }
sudo rm -f /tmp/.s.pg_cuvs

echo "=== [$LABEL] launching daemon under nsys (no LD_LIBRARY_PATH: RUNPATH is baked in) ==="
nsys profile --trace=cuda --output="/tmp/$LABEL" --force-overwrite=true \
     $BIN $ARGV > "/tmp/$LABEL.daemon.log" 2>&1 &
NSYS_PID=$!
echo "nsys wrapper pid=$NSYS_PID"

echo "=== [$LABEL] waiting for socket (CUDA context + 3.2GB index load) ==="
for i in $(seq 1 600); do
    [ -S /tmp/.s.pg_cuvs ] && break
    kill -0 $NSYS_PID 2>/dev/null || { echo "FATAL: nsys died early"; cat "/tmp/$LABEL.daemon.log"; exit 1; }
    sleep 0.5
done
[ -S /tmp/.s.pg_cuvs ] || { echo "FATAL: no socket after 300s"; cat "/tmp/$LABEL.daemon.log"; exit 1; }
sudo chmod 666 /tmp/.s.pg_cuvs
echo "socket up after ${i} polls (~$((i/2))s); perms: $(stat -c %a /tmp/.s.pg_cuvs)"

DPID=$(pgrep -f 'pg_cuvs_server --socket' | head -1)
echo "daemon pid=$DPID"

echo "=== [$LABEL] workload mode=$RUN_WORKLOAD ==="
/opt/miniforge3/envs/cuvs_bench/bin/python ~/pg98_nsys_workload.py "$RUN_WORKLOAD"
echo "workload exit=$?"

echo "=== [$LABEL] SIGTERM -> in-process finalize ==="
kill -TERM "$DPID"
for i in $(seq 1 600); do
    kill -0 $NSYS_PID 2>/dev/null || break
    sleep 0.5
done
wait $NSYS_PID 2>/dev/null
echo "nsys wrapper exited after ~$((i/2))s"
tail -5 "/tmp/$LABEL.daemon.log"
ls -la "/tmp/$LABEL".* 2>&1
