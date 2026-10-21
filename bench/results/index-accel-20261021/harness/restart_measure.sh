#!/usr/bin/env bash
set -euo pipefail
test "$(hostname)" = brev-v6nc08sjs
task_label=$1
task_commit=$2
task_pool=$3
. /home/ubuntu/.pg_cuvs_env
test "$(git -C /home/ubuntu/pg_cuvs rev-parse HEAD)" = "$(git -C /home/ubuntu/pg_cuvs rev-parse "$task_commit")"
sudo systemctl stop postgresql@16-main
sudo systemctl restart pg-cuvs-server
sudo systemctl start postgresql@16-main
systemctl is-active postgresql@16-main pg-cuvs-server
export NOISE_REFERENCE=/home/ubuntu/index-accel-validation/results/baseline-noise.json
export ENFORCE_NOISE_FLOOR=0
exec bash /home/ubuntu/index-accel-validation/measure_stage.sh measure "$task_label" "$task_commit" baseline_control1 "$task_pool"
