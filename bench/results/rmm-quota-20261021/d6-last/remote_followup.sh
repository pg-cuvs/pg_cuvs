#!/usr/bin/env bash
set -euo pipefail
test "$(hostname)" = brev-v6nc08sjs
task_pin=$1
task_stage=$2
task_out=/home/ubuntu/index-accel-validation/rmm-quota-last-20261021
task_repo=/home/ubuntu/pg_cuvs
task_bin=/usr/lib/postgresql/16/bin/pg_cuvs_server
task_override=/etc/systemd/system/pg-cuvs-server.service.d/index-accel-validation.conf
. /home/ubuntu/.pg_cuvs_env
export PGHOST=/var/run/postgresql
case "$task_stage" in
  prepare)
    test -d "$task_out/connect-zero"
    test ! -e "$task_out/original-override.conf"
    mkdir -p "$task_out"
    cp "$task_override" "$task_out/original-override.conf"
    sudo systemctl stop postgresql@16-main pg-cuvs-server
    test -z "$(nvidia-smi --query-compute-apps=pid --format=csv,noheader)"
    nvidia-smi --query-gpu=name,uuid,driver_version,compute_cap,memory.total --format=csv > "$task_out/gpu.csv"
    nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv > "$task_out/gpu-before.csv"
    python3 /home/ubuntu/index-accel-validation/rmm-production-tools-last-20261021/graph_check.py prepare
    cd "$task_repo"
    test -z "$(git status --porcelain --untracked-files=no)"
    git fetch origin ysys143/index-rmm-pool
    git checkout --detach "$task_pin"
    test "$(git rev-parse HEAD)" = "$task_pin"
    git diff --exit-code
    git ls-tree -r HEAD > "$task_out/source-tree.txt"
    git rev-parse HEAD HEAD^{tree} > "$task_out/source-pin.txt"
    make clean > "$task_out/build.log" 2>&1
    rm -f src/*_server.o pg_cuvs_server
    make -j2 >> "$task_out/build.log" 2>&1
    make -j2 server >> "$task_out/build.log" 2>&1
    sudo -E env PATH="$PATH" CONDA_PREFIX="$CONDA_PREFIX" make install install-server > "$task_out/install.log" 2>&1
    sha256sum pg_cuvs.so pg_cuvs_server "$(pg_config --pkglibdir)/pg_cuvs.so" "$task_bin" > "$task_out/artifact-sha.txt"
    ;;
  zero|default|admission)
    sudo systemctl stop postgresql@16-main pg-cuvs-server
    task_args=
    if [ "$task_stage" = zero ]; then task_args='--rmm-pool-mb 0 --rmm-pool-max-percent 50'; fi
    if [ "$task_stage" = admission ]; then task_args='--rmm-pool-mb 64 --rmm-pool-max-percent 1'; fi
    printf '[Service]\nExecStart=\nExecStart=%s --socket /tmp/.s.pg_cuvs --index-dir /tmp/cuvs_indexes --gpu-devices 0 %s\n' "$task_bin" "$task_args" | sudo tee "$task_override" > /dev/null
    sudo systemctl daemon-reload
    sudo systemctl start postgresql@16-main
    sudo systemctl start pg-cuvs-server
    sudo journalctl -u pg-cuvs-server -n 75 --no-pager > "$task_out/$task_stage-journal.log"
    if [ "$task_stage" = admission ]; then
      grep -F 'no GPU can fit 16390/1016847' "$task_out/$task_stage-journal.log"
    fi
    ;;
  reject)
    sudo systemctl stop postgresql@16-main pg-cuvs-server
    systemctl show pg-cuvs-server -p Result -p ExecMainStatus > "$task_out/default-shutdown.txt"
    grep -qx Result=success "$task_out/default-shutdown.txt"
    grep -qx ExecMainStatus=0 "$task_out/default-shutdown.txt"
    mkdir "$task_out/reject-index"
    set +e
    timeout --signal=TERM --kill-after=5s 35s "$task_bin" --socket "$task_out/reject.sock" --index-dir "$task_out/reject-index" --gpu-devices 0 --rmm-pool-mb 512 --rmm-pool-max-percent 1 > "$task_out/initial-rejection.log" 2>&1
    task_rc=$?
    set -e
    printf '%s\n' "$task_rc" > "$task_out/initial-rejection.exit"
    test "$task_rc" = 1
    test ! -S "$task_out/reject.sock"
    grep -F 'exceeds quota' "$task_out/initial-rejection.log"
    ;;
  cleanup)
    sudo systemctl stop postgresql@16-main pg-cuvs-server
    if [ -f "$task_out/original-override.conf" ]; then
      sudo cp "$task_out/original-override.conf" "$task_override"
      sudo systemctl daemon-reload
    fi
    systemctl show pg-cuvs-server -p Result -p ExecMainStatus > "$task_out/final-shutdown.txt"
    grep -qx Result=success "$task_out/final-shutdown.txt"
    grep -qx ExecMainStatus=0 "$task_out/final-shutdown.txt"
    python3 /home/ubuntu/index-accel-validation/rmm-production-tools-last-20261021/graph_check.py final
    systemctl show postgresql@16-main pg-cuvs-server -p Id -p Result -p ExecMainStatus -p ActiveState > "$task_out/services-stopped.txt"
    nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv > "$task_out/gpu-after.csv"
    test -z "$(nvidia-smi --query-compute-apps=pid --format=csv,noheader)"
    ;;
  *) exit 2;;
esac
