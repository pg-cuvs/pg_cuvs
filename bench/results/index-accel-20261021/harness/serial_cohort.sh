#!/usr/bin/env bash
set -euo pipefail
test "$(hostname)" = brev-v6nc08sjs
. /home/ubuntu/.pg_cuvs_env
task_root=/home/ubuntu/index-accel-validation
mkdir "$task_root/serial-profiles"
sudo systemctl stop pg-cuvs-server
trap 'sudo systemctl start pg-cuvs-server' EXIT
for task_pool in 0 256 512 1024; do
  nsys profile --trace=cuda,nvtx --sample=none --cpuctxsw=none --duration=180 --kill=sigterm --stats=true \
    --force-overwrite=false -o "$task_root/serial-profiles/pool$task_pool" "$task_root/serial_probe" serial100 "$task_pool" \
    > "$task_root/serial-profiles/pool$task_pool.log" 2>&1
done
