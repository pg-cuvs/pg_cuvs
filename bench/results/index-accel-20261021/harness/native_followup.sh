#!/usr/bin/env bash
set -euo pipefail
test "$(hostname)" = brev-v6nc08sjs
task_root=/home/ubuntu/index-accel-validation
test "$(cat "$task_root/build_timing_cohort_r2.exit")" = 0
bash "$task_root/run_job.sh" compile_bounded_probe bash "$task_root/compile_bounded_probe.sh"
bash "$task_root/run_job.sh" serial_cohort bash "$task_root/serial_cohort.sh"
bash "$task_root/run_job.sh" bounded_cohort bash "$task_root/bounded_cohort.sh" 512
