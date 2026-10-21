#!/usr/bin/env bash
set -euo pipefail
test "$(hostname)" = brev-v6nc08sjs
task_root=/home/ubuntu/index-accel-validation
. /home/ubuntu/.pg_cuvs_env
test "$(git -C /home/ubuntu/pg_cuvs rev-parse HEAD)" = "$(git -C /home/ubuntu/pg_cuvs rev-parse 0caa648)"
for task_label in issue144_run1 issue144_run2 issue144_run3; do
  test "$(cat "$task_root/$task_label.exit")" = 0
done
mkdir "$task_root/profiles"
sudo systemctl stop pg-cuvs-server
systemctl show pg-cuvs-server -p Result -p ExecMainStatus -p ActiveState
trap 'sudo systemctl start pg-cuvs-server' EXIT
nsys profile --trace=cuda,nvtx --sample=none --cpuctxsw=none --wait=all \
  --duration=180 --kill=sigterm --stats=true --force-overwrite=false \
  -o "$task_root/profiles/guc-ctamodes" bash "$task_root/profile_children.sh"
if [ ! -f "$task_root/profiles/guc-ctamodes.sqlite" ]; then
  nsys export --type=sqlite --force-overwrite=false \
    --output="$task_root/profiles/guc-ctamodes.sqlite" "$task_root/profiles/guc-ctamodes.nsys-rep"
fi
/opt/miniforge3/envs/cuvs_bench/bin/python "$task_root/extract_cta_modes.py" \
  "$task_root/profiles/guc-ctamodes.sqlite" --out "$task_root/profiles"
