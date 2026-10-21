#!/usr/bin/env bash
set -euo pipefail
test "$(hostname)" = brev-v6nc08sjs
task_root=/home/ubuntu/index-accel-validation
for task_label in build_timing_cohort_r2 serial_cohort bounded_cohort; do
  test "$(cat "$task_root/$task_label.exit")" = 0
done
sudo systemctl stop postgresql@16-main
sudo systemctl stop pg-cuvs-server
systemctl show postgresql@16-main pg-cuvs-server -p Id -p Result -p ExecMainStatus -p ActiveState > "$task_root/services-stopped.txt"
printf '[Service]\nExecStart=\nExecStart=/usr/lib/postgresql/16/bin/pg_cuvs_server --socket /tmp/.s.pg_cuvs --index-dir /tmp/cuvs_indexes --gpu-devices 0 --rmm-pool-mb 0\n' | sudo tee /etc/systemd/system/pg-cuvs-server.service.d/index-accel-validation.conf
sudo systemctl daemon-reload
nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv,noheader > "$task_root/gpu-processes-after-stop.txt"
diff -u "$task_root/native-peak-source/src/cuvs_wrapper.cu" "$task_root/native-peak-source/src/cuvs_wrapper_bounded.cu" > "$task_root/bounded-quota.patch" || test "$?" = 1
/opt/miniforge3/envs/cuvs_bench/bin/python "$task_root/finalize_evidence.py"
