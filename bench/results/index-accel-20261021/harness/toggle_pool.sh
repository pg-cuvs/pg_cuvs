#!/usr/bin/env bash
set -euo pipefail
test "$(hostname)" = brev-v6nc08sjs
task_pool=$1
case "$task_pool" in 0|1024) ;; *) exit 1;; esac
. /home/ubuntu/.pg_cuvs_env
test "$(git -C /home/ubuntu/pg_cuvs rev-parse HEAD)" = "$(git -C /home/ubuntu/pg_cuvs rev-parse fded8ce)"
sudo systemctl stop postgresql@16-main
sudo systemctl stop pg-cuvs-server
systemctl show pg-cuvs-server -p Result -p ExecMainCode -p ExecMainStatus -p ActiveState
task_bin=$(pg_config --bindir)/pg_cuvs_server
printf '[Service]\nExecStart=\nExecStart=%s --socket /tmp/.s.pg_cuvs --index-dir /tmp/cuvs_indexes --gpu-devices 0 --rmm-pool-mb %s\n' \
  "$task_bin" "$task_pool" | sudo tee /etc/systemd/system/pg-cuvs-server.service.d/index-accel-validation.conf
sudo systemctl daemon-reload
sudo systemctl start postgresql@16-main
sudo systemctl start pg-cuvs-server
systemctl is-active postgresql@16-main pg-cuvs-server
psql -XAt -d index_accel_smoke -c 'SHOW cuvs.rmm_pool_mb'
export NOISE_REFERENCE=/home/ubuntu/index-accel-validation/results/baseline-noise.json
export ENFORCE_NOISE_FLOOR=0
exec bash /home/ubuntu/index-accel-validation/measure_stage.sh measure "rmm_$task_pool" fded8ce baseline_control1 "$task_pool"
