#!/usr/bin/env bash
set -euo pipefail
test "$(hostname)" = brev-v6nc08sjs
task_root=/home/ubuntu/index-accel-validation
. /home/ubuntu/.pg_cuvs_env
task_daemon_pid=
cleanup() {
  if [ -n "$task_daemon_pid" ]; then
    kill -TERM "$task_daemon_pid" 2>/dev/null || true
    wait "$task_daemon_pid" || true
  fi
}
trap cleanup EXIT
test ! -S /tmp/.s.pg_cuvs
/usr/lib/postgresql/16/bin/pg_cuvs_server --socket /tmp/.s.pg_cuvs --index-dir /tmp/cuvs_indexes --gpu-devices 0 \
  > "$task_root/profiles/profile-daemon.log" 2>&1 9>&- &
task_daemon_pid=$!
for task_attempt in $(seq 1 90); do
  kill -0 "$task_daemon_pid" || { cat "$task_root/profiles/profile-daemon.log"; exit 1; }
  [ -S /tmp/.s.pg_cuvs ] && break
  sleep 1
done
test -S /tmp/.s.pg_cuvs
chmod 666 /tmp/.s.pg_cuvs
/opt/miniforge3/envs/cuvs_bench/bin/python "$task_root/profile_probes.py"
cleanup
task_daemon_pid=
