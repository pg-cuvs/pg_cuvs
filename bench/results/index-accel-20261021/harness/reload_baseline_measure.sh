#!/usr/bin/env bash
set -euo pipefail
test "$(hostname)" = brev-v6nc08sjs
. /home/ubuntu/.pg_cuvs_env
task_label=${1:-baseline_reload}
task_baseline=${2:-baseline_initial}
test "$(git -C /home/ubuntu/pg_cuvs rev-parse HEAD)" = 8b73d65fb8a3d6f66cdfe69eec4969c1f2fc753c
sudo systemctl stop postgresql@16-main
sudo systemctl restart pg-cuvs-server
sudo systemctl start postgresql@16-main
systemctl is-active postgresql@16-main pg-cuvs-server
exec bash /home/ubuntu/index-accel-validation/measure_stage.sh measure "$task_label" 8b73d65 "$task_baseline"
