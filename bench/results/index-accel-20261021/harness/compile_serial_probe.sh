#!/usr/bin/env bash
set -euo pipefail
test "$(hostname)" = brev-v6nc08sjs
. /home/ubuntu/.pg_cuvs_env
task_root=/home/ubuntu/index-accel-validation
cd "$task_root/native-peak-source"
nvcc -ccbin /usr/bin/g++ -x cu -std=c++17 -arch=sm_89 --extended-lambda \
  -Isrc -I"$CONDA_PREFIX/include" -I"$CONDA_PREFIX/include/rapids" \
  -c "$task_root/serial_probe.cpp" -o "$task_root/serial_probe.o"
nvcc -ccbin /usr/bin/g++ "$task_root/serial_probe.o" src/cuvs_wrapper.o /home/ubuntu/pg_cuvs/src/cuvs_util_server.o \
  -L"$CONDA_PREFIX/lib" -Xlinker -rpath -Xlinker "$CONDA_PREFIX/lib" \
  -lcuvs -lrmm -lrapids_logger -lcudart -ldl -lpthread -o "$task_root/serial_probe"
sha256sum "$task_root/serial_probe" "$task_root/serial_probe.cpp"
