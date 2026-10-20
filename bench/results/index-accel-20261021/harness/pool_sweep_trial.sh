#!/usr/bin/env bash
set -euo pipefail
test "$(hostname)" = brev-v6nc08sjs
task_pool=$1
task_label=$2
. /home/ubuntu/.pg_cuvs_env
test "$(git -C /home/ubuntu/pg_cuvs rev-parse HEAD)" = "$(git -C /home/ubuntu/pg_cuvs rev-parse fded8ce)"
for task_k in 64 128; do
  sudo systemctl stop postgresql@16-main
  sudo systemctl stop pg-cuvs-server
  printf '[Service]\nExecStart=\nExecStart=/usr/lib/postgresql/16/bin/pg_cuvs_server --socket /tmp/.s.pg_cuvs --index-dir /tmp/cuvs_indexes --gpu-devices 0 --rmm-pool-mb %s\n' "$task_pool" | sudo tee /etc/systemd/system/pg-cuvs-server.service.d/index-accel-validation.conf
  sudo systemctl daemon-reload
  sudo systemctl start postgresql@16-main
  sudo systemctl start pg-cuvs-server
  /opt/miniforge3/envs/cuvs_bench/bin/python /home/ubuntu/index-accel-validation/pool_sweep_measure.py --pool "$task_pool" --k "$task_k" --label "$task_label-k$task_k"
done
