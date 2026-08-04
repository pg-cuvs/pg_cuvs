#!/bin/bash
# PR-E build-path nsys capture (MANDATORY ATTEMPT).
#   $1 = label, $2 = extra nsys flags (escalation ladder), $3 = build N hint (unused, informational)
#
# Prior failure: nsys 2023.4.4 could not convert qdstrm->nsys-rep for the
# multi-stream build ("Wrong event order has been detected"). Retried here on
# nsys 2026.1.3. Escalation, each attempt recorded:
#   (a) --sample=none --backtrace=none   (b) smaller N   (c) nsys export on qdstrm
#   (d) fewer build streams if a knob exists
set -u
LABEL="$1"
EXTRA="${2:-}"
ARGV="--socket /tmp/.s.pg_cuvs --index-dir /tmp/cuvs_indexes --gpu-devices 0"
BIN=/usr/lib/postgresql/16/bin/pg_cuvs_server
export PGHOST=/var/run/postgresql
export PATH=/usr/lib/postgresql/16/bin:$PATH

echo "=== [$LABEL] nsys flags: --trace=cuda $EXTRA ==="
sudo systemctl stop pg-cuvs-server
sleep 2
pgrep -f 'pg_cuvs_server --socket' && { echo "FATAL: daemon still running"; exit 1; }
sudo rm -f /tmp/.s.pg_cuvs

nsys profile --trace=cuda $EXTRA --output="/tmp/$LABEL" --force-overwrite=true \
     $BIN $ARGV > "/tmp/$LABEL.daemon.log" 2>&1 &
NSYS_PID=$!

for i in $(seq 1 600); do
    [ -S /tmp/.s.pg_cuvs ] && break
    kill -0 $NSYS_PID 2>/dev/null || { echo "FATAL: nsys died early"; cat "/tmp/$LABEL.daemon.log"; exit 1; }
    sleep 0.5
done
[ -S /tmp/.s.pg_cuvs ] || { echo "FATAL: no socket"; exit 1; }
sudo chmod 666 /tmp/.s.pg_cuvs
DPID=$(pgrep -f 'pg_cuvs_server --socket' | head -1)
echo "socket up (~$((i/2))s), daemon pid=$DPID"

echo "=== [$LABEL] CREATE INDEX (build path) ==="
# Epoch markers: nsys stores its session start as utcEpochNs, so these let the
# CREATE INDEX window be located in the capture timeline exactly rather than
# inferred from the socket-up poll count.
echo "EPOCH_BEFORE_CREATE=$(date +%s.%N)"
psql -d postgres -v ON_ERROR_STOP=1 <<'SQL'
\timing on
SET cuvs.index_dir = '/tmp/cuvs_indexes';
SET maintenance_work_mem = '8GB';
DROP INDEX IF EXISTS t_cagra_nsys;
CREATE INDEX t_cagra_nsys ON t USING cagra (embedding vector_l2_ops)
  WITH (graph_degree=32, intermediate_graph_degree=128, build_algo='ivf_pq');
SQL
echo "create exit=$?"
echo "EPOCH_AFTER_CREATE=$(date +%s.%N)"

echo "=== [$LABEL] journal GPU build timestamps ==="
sudo journalctl -u pg-cuvs-server --since "-10 min" --no-pager 2>/dev/null | tail -5
grep -iE 'handle_build|built index|corpus via' "/tmp/$LABEL.daemon.log" | tail -10

echo "=== [$LABEL] SIGTERM -> finalize ==="
kill -TERM "$DPID"
for i in $(seq 1 900); do kill -0 $NSYS_PID 2>/dev/null || break; sleep 0.5; done
wait $NSYS_PID 2>/dev/null
echo "=== [$LABEL] artifacts ==="
ls -la /tmp/$LABEL.* 2>&1
echo "=== [$LABEL] daemon.log tail (conversion errors appear here) ==="
tail -20 "/tmp/$LABEL.daemon.log"
