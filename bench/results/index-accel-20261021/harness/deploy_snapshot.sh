#!/usr/bin/env bash
set -euo pipefail
test "$(hostname)" = brev-v6nc08sjs
task_commit=$1
task_arm=$2
task_pool=${3:-0}
case "$task_arm" in baseline|guc|rmm) ;; *) exit 1;; esac
case "$task_pool" in 0|1024) ;; *) exit 1;; esac
. /home/ubuntu/.pg_cuvs_env
cd /home/ubuntu/pg_cuvs
test -z "$(git status --porcelain --untracked-files=no)"
git fetch origin \
  ysys143/index-accel:refs/remotes/origin/ysys143/index-accel \
  ysys143/index-rmm-pool:refs/remotes/origin/ysys143/index-rmm-pool
git checkout --detach "$task_commit"
make clean
rm -f src/*_server.o pg_cuvs_server
make -j2
make -j2 server

# Install only after the old processes have stopped using their artifacts.
sudo systemctl stop postgresql@16-main
sudo systemctl stop pg-cuvs-server
sudo -E env PATH="$PATH" CONDA_PREFIX="$CONDA_PREFIX" make install install-server

task_bin=$(pg_config --bindir)/pg_cuvs_server
task_pool_arg=
if [ "$task_arm" = rmm ]; then task_pool_arg="--rmm-pool-mb $task_pool"; fi
sudo mkdir -p /etc/systemd/system/pg-cuvs-server.service.d
printf '[Service]\nExecStart=\nExecStart=%s --socket /tmp/.s.pg_cuvs --index-dir /tmp/cuvs_indexes --gpu-devices 0 %s\n' \
  "$task_bin" "$task_pool_arg" | sudo tee /etc/systemd/system/pg-cuvs-server.service.d/index-accel-validation.conf
sudo systemctl daemon-reload
sudo systemctl start postgresql@16-main
sudo systemctl start pg-cuvs-server
systemctl is-active postgresql@16-main pg-cuvs-server
git rev-parse HEAD
sha256sum "$(pg_config --pkglibdir)/pg_cuvs.so" "$task_bin"
psql -XAt -d index_accel_smoke -c 'SELECT pg_postmaster_start_time()'
