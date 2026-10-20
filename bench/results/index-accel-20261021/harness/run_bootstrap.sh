#!/usr/bin/env bash
set -u
task_root=/home/ubuntu/index-accel-validation
if [ "$(hostname)" != "brev-v6nc08sjs" ]; then
  echo 'Refusing bootstrap on a host other than the new lane VM' >&2
  exit 1
fi
date -u +%FT%TZ > "$task_root/bootstrap.started"
CUDA_ARCH=sm_89 bash "$task_root/bootstrap-baseline.sh" > "$task_root/bootstrap-baseline.log" 2>&1
task_exit=$?
printf '%s\n' "$task_exit" > "$task_root/bootstrap.exit"
date -u +%FT%TZ > "$task_root/bootstrap.completed"
exit "$task_exit"
