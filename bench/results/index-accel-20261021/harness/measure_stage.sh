#!/usr/bin/env bash
set -euo pipefail
test "$(hostname)" = brev-v6nc08sjs
task_root=/home/ubuntu/index-accel-validation
task_mode=$1
task_label=$2
task_commit=$3
task_baseline=${4:-}
task_pool=${5:-}
. /home/ubuntu/.pg_cuvs_env
export PGHOST=/var/run/postgresql
export PATH="/opt/miniforge3/envs/cuvs_bench/bin:/usr/lib/postgresql/16/bin:$PATH"
task_args=("$task_mode" --repo /home/ubuntu/pg_cuvs --owner-file "$task_root/owner.json"
  --expected-commit "$task_commit" --dbname index_accel_smoke --data-dir /home/ubuntu/data
  --out "$task_root/results/$task_label" --queries 10000 --points 64,128
  --graph-degree 32 --intermediate-degree 128 --build-algo ivf_pq)
if [ -n "$task_baseline" ]; then task_args+=(--baseline "$task_root/results/$task_baseline"); fi
if [ -n "$task_pool" ]; then task_args+=(--expect-rmm-mb "$task_pool"); fi
if [ -n "${NOISE_REFERENCE:-}" ]; then task_args+=(--noise-reference "$NOISE_REFERENCE"); fi
if [ "${ENFORCE_NOISE_FLOOR:-0}" = 1 ]; then task_args+=(--enforce-noise-floor); fi
exec /opt/miniforge3/envs/cuvs_bench/bin/python "$task_root/feature_gpu_check.py" "${task_args[@]}"
