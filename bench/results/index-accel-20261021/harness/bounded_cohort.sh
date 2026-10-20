#!/usr/bin/env bash
set -euo pipefail
test "$(hostname)" = brev-v6nc08sjs
. /home/ubuntu/.pg_cuvs_env
task_root=/home/ubuntu/index-accel-validation
task_candidate=$1
mkdir "$task_root/bounded-results"
sudo systemctl stop pg-cuvs-server
trap 'sudo systemctl start pg-cuvs-server' EXIT
for task_mode in search ivf_pq nn_descent; do
  task_exit=0
  "$task_root/bounded_probe" "$task_mode" "$task_candidate" > "$task_root/bounded-results/$task_mode.log" 2>&1 || task_exit=$?
  printf '%s\n' "$task_exit" > "$task_root/bounded-results/$task_mode.exit"
done
