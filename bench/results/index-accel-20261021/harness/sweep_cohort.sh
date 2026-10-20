#!/usr/bin/env bash
set -euo pipefail
test "$(hostname)" = brev-v6nc08sjs
task_root=/home/ubuntu/index-accel-validation
for task_label in issue144_run1 issue144_run2 issue144_run3; do
  bash "$task_root/run_job.sh" "$task_label" bash "$task_root/restart_sweep.sh" "$task_label"
done
