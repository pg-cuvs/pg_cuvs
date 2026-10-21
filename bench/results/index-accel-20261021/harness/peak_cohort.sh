#!/usr/bin/env bash
set -euo pipefail
test "$(hostname)" = brev-v6nc08sjs
. /home/ubuntu/.pg_cuvs_env
task_root=/home/ubuntu/index-accel-validation
mkdir -p "$task_root/peak-profiles"
sudo systemctl stop pg-cuvs-server
trap 'sudo systemctl start pg-cuvs-server' EXIT
for task_mode in search ivf_pq nn_descent; do
  task_status=0
  nsys profile --trace=cuda,nvtx --cuda-memory-usage=true --sample=none --cpuctxsw=none --duration=900 --kill=sigterm --stats=true \
    --force-overwrite=false -o "$task_root/peak-profiles/$task_mode" "$task_root/peak_probe" "$task_mode" 1024 \
    > "$task_root/peak-profiles/$task_mode.log" 2>&1 || task_status=$?
  printf '%s\n' "$task_status" > "$task_root/peak-profiles/$task_mode.exit"
done
