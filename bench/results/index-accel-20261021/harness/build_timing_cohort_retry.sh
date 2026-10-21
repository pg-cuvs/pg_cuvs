#!/usr/bin/env bash
set -euo pipefail
test "$(hostname)" = brev-v6nc08sjs
. /home/ubuntu/.pg_cuvs_env
task_root=/home/ubuntu/index-accel-validation
task_candidate=$1
case "$task_candidate" in 256|512|1024|2048|4096) ;; *) exit 1;; esac
test "$(git -C /home/ubuntu/pg_cuvs rev-parse HEAD)" = "$(git -C /home/ubuntu/pg_cuvs rev-parse fded8ce)"
test "$(cat "$task_root/build_timing_cohort.exit")" = 1
psql -X -v ON_ERROR_STOP=1 -d index_accel_pool_build -c 'DROP INDEX IF EXISTS t_pool_bench' -c 'ANALYZE t' -c 'SELECT count(*),min(id),max(id) FROM t'
task_i=0
for task_pool in 0 "$task_candidate" 0 "$task_candidate" 0 "$task_candidate"; do
  task_i=$((task_i+1));task_label="build_r2_$task_i-pool$task_pool";task_dir="$task_root/build-artifacts/$task_label"
  mkdir -p "$task_dir"
  # PostgreSQL writes vector sidecars as postgres; the daemon runs as ubuntu.
  chmod 1777 "$task_dir"
  sudo systemctl stop postgresql@16-main
  sudo systemctl stop pg-cuvs-server
  printf '[Service]\nExecStart=\nExecStart=/usr/lib/postgresql/16/bin/pg_cuvs_server --socket /tmp/.s.pg_cuvs --index-dir %s --gpu-devices 0 --rmm-pool-mb %s\n' "$task_dir" "$task_pool" | sudo tee /etc/systemd/system/pg-cuvs-server.service.d/index-accel-validation.conf
  sudo systemctl daemon-reload
  sudo systemctl start postgresql@16-main
  sudo systemctl start pg-cuvs-server
  /opt/miniforge3/envs/cuvs_bench/bin/python "$task_root/build_timing_measure.py" --pool "$task_pool" --label "$task_label" --index-dir "$task_dir"
done
