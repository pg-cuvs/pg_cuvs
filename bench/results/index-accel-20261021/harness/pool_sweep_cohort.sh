#!/usr/bin/env bash
set -euo pipefail
test "$(hostname)" = brev-v6nc08sjs
task_root=/home/ubuntu/index-accel-validation
task_i=0
for task_pool in 0 1024 256 2048 512 4096 2048 0 512 1024 4096 256 512 4096 1024 256 0 2048; do
  task_i=$((task_i+1))
  bash "$task_root/run_job.sh" "pool_sweep_$task_i" bash "$task_root/pool_sweep_trial.sh" "$task_pool" "trial$task_i-pool$task_pool"
done
