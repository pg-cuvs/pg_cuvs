#!/usr/bin/env bash
set -euo pipefail
test "$(hostname)" = brev-v6nc08sjs
task_label=$1
. /home/ubuntu/.pg_cuvs_env
test "$(git -C /home/ubuntu/pg_cuvs rev-parse HEAD)" = "$(git -C /home/ubuntu/pg_cuvs rev-parse 0caa648)"
sudo systemctl stop postgresql@16-main
sudo systemctl restart pg-cuvs-server
sudo systemctl start postgresql@16-main
systemctl is-active postgresql@16-main pg-cuvs-server
exec bash /home/ubuntu/index-accel-validation/measure_stage.sh sweep "$task_label" 0caa648
