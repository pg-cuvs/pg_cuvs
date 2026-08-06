#!/usr/bin/env bash
#
# pg_cuvs GPU dev/bench VM — zero-to-ready bootstrap.
#
# WHY THIS EXISTS
#   Brev A100 (massedcompute_A100_sxm4_80G) does NOT support `stop`, and the
#   instance has no persistent volume — deleting it loses everything, and there
#   is no snapshot/image feature in Brev. So "fast restart" = fast, reproducible
#   REBUILD. Every gotcha debugged in the 2026-07-23 session is baked in below
#   as a one-liner + comment, so a fresh instance goes to ready without redoing
#   the debugging (that was the slow part, not the downloads/compiles).
#
# USAGE (from the Mac, once a fresh instance is up and `ssh <name>` works):
#   scp bootstrap.sh <name>:/home/shadeform/
#   ssh <name> 'bash /home/shadeform/bootstrap.sh 2>&1 | tee /home/shadeform/bootstrap.log'
#   # ~10 min wall clock: setup ~80s, data ~75s (parallel), build ~2-3m, verify ~30s.
#
# ASSUMPTIONS (Brev / Massed Compute image, 2026-07):
#   - Ubuntu 22.04, user `shadeform`, passwordless sudo, systemd present.
#   - A100 with a working NVIDIA driver (nvidia-smi works out of the box).
#   - Public repo pg-cuvs/pg_cuvs reachable (no auth needed for clone).
# If the SSH user differs (RunPod uses root, no systemd), change USER/HOME and
# swap `systemctl` for `pg_ctlcluster` — see the notes at each such line.
# `-e` is load-bearing: without it a failed apt install walked all the way to
# "=== BOOTSTRAP DONE ===" and exit 0 with no PostgreSQL on the box at all
# (2026-07-28). Every line below that is *allowed* to fail carries an explicit
# `|| true` / `|| fallback`. Still NOT `set -u`: it kills conda's activation
# scripts.
set -exo pipefail
export DEBIAN_FRONTEND=noninteractive

USER_HOME=/home/shadeform
REPO="$USER_HOME/pg_cuvs"
# /tmp, matching what the regression suite hardcodes: 35 test/sql files do
# `SET cuvs.index_dir = '/tmp/cuvs_indexes'`. Pointing the daemon elsewhere used
# to mean the suite could not pass straight off a fresh bootstrap — it needed a
# manual daemon restart first. Volatility is fine here: a Brev VM is disposable
# (restart == rebuild), and nothing on it is a source of truth. On a long-lived
# host, note that systemd-tmpfiles can reap /tmp — the symptom is indexes
# silently vanishing; production deployments should use the path in
# docs/best-practices.md instead.
IDX=/tmp/cuvs_indexes
CONDA=/opt/miniforge3
DEV=$CONDA/envs/cuvs_dev            # C/C++ build toolchain (nvcc, libcuvs)
BENCH=$CONDA/envs/cuvs_bench        # python bench stack (separate: python 3.12)

# The Brev image runs its own apt on first boot. Racing it dies with "Could not
# get lock /var/lib/dpkg/lock-frontend", which is exactly how a bootstrap once
# finished "successfully" with zero PostgreSQL packages installed.
wait_for_apt() {
  set +x
  local i
  for i in $(seq 1 60); do
    sudo fuser /var/lib/dpkg/lock-frontend /var/lib/dpkg/lock \
               /var/lib/apt/lists/lock >/dev/null 2>&1 || { set -x; return 0; }
    echo "[wait_for_apt] another apt/dpkg holds the lock ($i/60), sleeping 10s"
    sleep 10
  done
  echo "[wait_for_apt] FATAL: dpkg lock still held after 10 minutes" >&2
  return 1
}

echo "=== [1/5] APT + PostgreSQL 16 =========================================="
wait_for_apt
sudo apt-get update -qq
# libcurl4-openssl-dev: curl/curl.h for the extension.
# g++ + libstdc++-12-dev: this image ships gcc=12 but g++=11, so the static
#   libstdc++.a the extension links (-Wl,-Bstatic -lstdc++) only existed for
#   gcc-11 and `ld: cannot find -lstdc++`. Installing the -12 dev package fixes it.
sudo apt-get install -y -qq curl ca-certificates gnupg lsb-release rsync cmake \
    postgresql-common libcurl4-openssl-dev libssl-dev g++ libstdc++-12-dev git
yes | sudo /usr/share/postgresql-common/pgdg/apt.postgresql.org.sh || true
wait_for_apt
sudo apt-get update -qq
wait_for_apt
sudo apt-get install -y -qq postgresql-16 postgresql-server-dev-16 postgresql-16-pgvector
# Hard gate. Everything downstream (PGXS, the .so, installcheck) needs pg_config;
# without `set -e` this line used to just print an error and let the run continue
# to a "DONE" that had never built the extension at all.
test -x /usr/lib/postgresql/16/bin/pg_config \
  || { echo "FATAL: postgresql-server-dev-16 missing — apt step failed" >&2; exit 1; }
/usr/lib/postgresql/16/bin/pg_config --version

# systemd path; on a container (RunPod) use: sudo pg_ctlcluster 16 main start
sudo systemctl enable --now postgresql@16-main 2>/dev/null || sudo pg_ctlcluster 16 main start || true
sleep 2
sudo -u postgres psql -tAc "SELECT 1 FROM pg_roles WHERE rolname='shadeform'" | grep -q 1 \
  || sudo -u postgres psql -c "CREATE ROLE shadeform SUPERUSER LOGIN"
sudo -u postgres psql -tAc "SELECT 1 FROM pg_database WHERE datname='shadeform'" | grep -q 1 \
  || sudo -u postgres createdb -O shadeform shadeform

echo "=== [2/5] miniforge + cuvs_dev (build) + cuvs_bench (python) ==========="
# In /opt, NOT ~ : the `postgres` OS user must be able to read libcuvs.so, and a
# 0700 home directory blocks that. chmod o+rX opens the read path.
if [ ! -d "$CONDA" ]; then
  wget -q https://github.com/conda-forge/miniforge/releases/latest/download/Miniforge3-Linux-x86_64.sh -O /tmp/mf.sh
  sudo bash /tmp/mf.sh -b -p "$CONDA"
fi
# Guarded so the script stays re-runnable: recovery from a half-finished run is
# "just run bootstrap again", and under `set -e` a bare `mamba create` over an
# existing prefix would abort instead.
[ -d "$DEV" ] || sudo "$CONDA/bin/mamba" create -y -n cuvs_dev -c rapidsai -c conda-forge -c nvidia \
  libcuvs=26.04 librmm cuda-version=12.4 cuda-nvcc cuda-cudart-dev
# python stack in a SEPARATE env: cuvs-bench pulls python 3.12, which would break
# the build env. psycopg is v3 (the backend imports `psycopg`, not psycopg2) and
# pgvector's python package are both required beyond psycopg2/numpy/pandas.
[ -d "$BENCH" ] || sudo "$CONDA/bin/mamba" create -y -n cuvs_bench -c rapidsai -c conda-forge -c nvidia \
  python=3.12 cuvs-bench=26.04 psycopg2 numpy pandas
sudo "$CONDA/bin/mamba" install -y -n cuvs_bench -c conda-forge psycopg
sudo "$BENCH/bin/pip" install -q pgvector
sudo chmod -R o+rX "$CONDA"
"$BENCH/bin/python" -c "import cuvs_bench,numpy,psycopg,pgvector.psycopg; print('BENCH ENV OK')"

# GLIBCXX: the PG backend loads pg_cuvs.so which needs conda's newer libstdc++.
# Symlink it where ldconfig sees it. Do NOT add conda's lib dir to ldconfig
# system-wide — it breaks sshd via an OpenSSL mismatch (documented lesson).
L="$DEV/lib"
sudo ln -sf "$L/libstdc++.so.6" /usr/local/lib/libstdc++.so.6
sudo ln -sf "$L/libgcc_s.so.1"  /usr/local/lib/libgcc_s.so.1
sudo ldconfig

echo "=== [3/5] clone + build (.so + daemon) ================================="
[ -d "$REPO/.git" ] || git clone https://github.com/pg-cuvs/pg_cuvs.git "$REPO"
cd "$REPO"
git pull --ff-only || true
# The build environment is written to a file FIRST and then sourced, so the
# environment this script exports is by construction the exact one it built with.
# The Makefile's gpu-* targets source the same file over ssh; without it they
# fall back to GCP-era guesses (~/miniforge3, no PG bin on PATH) and all fail.
#   NVCC: without `conda activate`, conda's nvcc can't find its own cc1plus
#     (activation sets GCC_EXEC_PREFIX). Pointing it at the system g++ sidesteps
#     that; the Makefile's `NVCC ?= nvcc` makes the override clean.
#   PGHOST: conda's libpq defaults to a socket dir the Debian server doesn't use.
#   \$PATH is left unexpanded on purpose — the sourcing shell keeps its own PATH.
# #175: detect the GPU's compute capability so the Makefile's `CUDA_ARCH ?=
# sm_80` default cannot silently apply on a non-A100 box. Local gpu.conf never
# reaches this remote build (REMOTE_ENV only sources this file, never gpu.conf),
# and this script never referenced CUDA_ARCH at all — so every `make gpu-build`
# on an L40/L40S/H100/etc. instance built sm_80 SASS that fails to load at
# runtime ("no kernel image available for execution on the device"), far from
# the actual cause. `nvidia-smi --query-gpu=compute_cap` reports e.g. "8.9" for
# an L40S; strip the dot and prefix "sm_". CUDA_ARCH set in this script's own
# environment (e.g. `CUDA_ARCH=sm_90 bash bootstrap.sh` for a GPU model this
# detection doesn't handle correctly) wins over detection.
if [ -z "${CUDA_ARCH:-}" ]; then
    cap=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -1 | tr -d '[:space:]')
    if [ -n "$cap" ]; then
        CUDA_ARCH="sm_$(printf '%s' "$cap" | tr -d '.')"
    else
        echo "WARNING: nvidia-smi compute_cap detection failed; defaulting CUDA_ARCH=sm_80" >&2
        CUDA_ARCH="sm_80"
    fi
fi
echo "  CUDA_ARCH        : $CUDA_ARCH"

ENV_FILE="$USER_HOME/.pg_cuvs_env"
cat > "$ENV_FILE" <<EOF
export CONDA_PREFIX="$DEV"
export PATH="$DEV/bin:/usr/lib/postgresql/16/bin:\$PATH"
export LD_LIBRARY_PATH="$DEV/lib"
export NVCC="nvcc -ccbin /usr/bin/g++"
export PGHOST=/var/run/postgresql
export CUDA_ARCH="$CUDA_ARCH"
EOF
# Hard gate: a truncated or unparseable env file would make every remote build
# fail far from here, with an error that points at the Makefile instead.
test -f "$ENV_FILE" \
  || { echo "FATAL: $ENV_FILE was not created" >&2; exit 1; }
sh -n "$ENV_FILE" \
  || { echo "FATAL: $ENV_FILE is not valid POSIX sh" >&2; exit 1; }
# shellcheck source=/dev/null
. "$ENV_FILE"
make
sudo -E env PATH="$PATH" CONDA_PREFIX="$CONDA_PREFIX" make install
make server
sudo -E env PATH="$PATH" CONDA_PREFIX="$CONDA_PREFIX" make install-server
# Gate on the artifacts, not on make's exit code: when pg_config is missing PGXS
# silently degrades and `make install` has been observed writing the daemon to /
# (empty --bindir) while never producing the extension .so at all.
PKGLIB=$(pg_config --pkglibdir)
test -f "$PKGLIB/pg_cuvs.so" \
  || { echo "FATAL: pg_cuvs.so not installed under $PKGLIB" >&2; exit 1; }
test -x "$(pg_config --bindir)/pg_cuvs_server" \
  || { echo "FATAL: pg_cuvs_server not installed under $(pg_config --bindir)" >&2; exit 1; }

echo "=== [4/5] runtime: preload, GUCs, environment, daemon =================="
sudo mkdir -p "$IDX"; sudo chmod 1777 "$IDX"
# Debian's /etc/postgresql/16/main/environment is parsed by pg_ctlcluster (perl)
# and REQUIRES quotes around the value — bare LD_LIBRARY_PATH=... is rejected and
# PG fails to restart. (RunPod used pg_ctl directly and never hit this.)
echo "LD_LIBRARY_PATH='$DEV/lib'" | sudo tee /etc/postgresql/16/main/environment
# ORDER MATTERS: shared_preload_libraries must be set and PG restarted FIRST —
# the custom cuvs.* GUCs don't exist until the extension is preloaded, so setting
# them before the restart fails with "unrecognized configuration parameter".
sudo -u postgres psql -c "ALTER SYSTEM SET shared_preload_libraries='pg_cuvs'"
sudo systemctl restart postgresql@16-main; sleep 3
sudo -u postgres psql -c "ALTER SYSTEM SET cuvs.socket_path='/tmp/.s.pg_cuvs'"
sudo -u postgres psql -c "ALTER SYSTEM SET cuvs.index_dir='$IDX'"
# #119: the daemon runs as this script's own user (systemd unit's User=$(id -un)
# below), so its uid is known here — set cuvs.daemon_uid so the backend verifies
# the socket owner before connecting on every VM this bootstrap produces, not
# only ones someone remembers to configure by hand. Default is -1 (off), so this
# is additive: it does not change behavior on any existing deployment that
# doesn't run this bootstrap.
sudo -u postgres psql -c "ALTER SYSTEM SET cuvs.daemon_uid=$(id -u)"
sudo -u postgres psql -c "SELECT pg_reload_conf()"
# Run the daemon under systemd, not nohup. 34 playbooks and scripts in this repo
# already say `systemctl restart pg-cuvs-server` / `journalctl -u pg-cuvs-server`
# (delta-restart-e2e.sh among them); with a bare nohup none of that was true, and
# a daemon started over ssh died with the session. This unit is the SSOT for the
# definition — references/quick-start.md carries the GCP-era copy for history.
#
# `|| true`: pkill exits 1 when nothing matched, which is the normal case on a
# fresh box and would abort the script under `set -e`. `-x` not `-f`: `-f`
# matches this script's own command line (PATTERNS.md — that trap cost real time).
pkill -9 -x pg_cuvs_server 2>/dev/null || true; sleep 1

# The socket only appears after the daemon's first CUDA context, and how long
# that takes is a property of the machine, not the build: ~12s on Massed Compute
# but over 3 minutes on a Paperspace A100. So every wait here polls; nothing
# sleeps a fixed amount. The chmod is what this buys us — the daemon creates the
# socket 0660 and the `postgres` OS user cannot reach it until it is widened.
# Doing that by hand is the step that gets forgotten, and forgetting it fails the
# whole regression run with an opaque `BUILD failed (status 4)`.
#
# It lives in a file rather than inline in the unit because systemd does its own
# `$`-expansion on Exec lines, so an inline `$(seq ...)` depends on systemd
# declining to touch a `$` that is not a variable name. Not worth the bet.
#
# Always exits 0. ExecStartPost failure would mark the unit failed, and with
# Restart=on-failure and each attempt taking the full 5 minutes, a genuinely
# broken GPU would sit in a permanent restart loop (the default 10s
# StartLimitIntervalSec never sees two failures close enough together to trip).
# Verifying the socket is the bootstrap's job, below, where failing is useful.
sudo tee /usr/local/bin/pg-cuvs-wait-socket > /dev/null <<'WAIT'
#!/bin/sh
n=0
while [ ! -S /tmp/.s.pg_cuvs ] && [ "$n" -lt 600 ]; do
  n=$((n + 1))
  sleep 0.5
done
[ -S /tmp/.s.pg_cuvs ] && chmod 666 /tmp/.s.pg_cuvs
exit 0
WAIT
sudo chmod 755 /usr/local/bin/pg-cuvs-wait-socket

# $(pg_config --bindir) is expanded here, at write time, on purpose: systemd has
# none of the conda PATH this script is running under, so the unit needs the
# absolute path — and it is the same pg_config that `make install-server` and the
# gate above used, so the unit points at the binary that was actually installed.
# TimeoutStartSec covers the CUDA-context wait described above; the unit sitting
# in `activating` for minutes on a slow machine is normal, not a hang.
#
# ExecStartPre removes the socket path before ExecStart runs. Without it, a
# restart racing a stale file gives a false "ready": on SIGKILL (this script's
# earlier pkill -9, or systemd falling back to SIGKILL after TimeoutStopSec)
# the old process cannot run its own unlink(), so the special file from the
# PREVIOUS instance is still on disk when the new one starts. The daemon binds
# the real socket only after startup_load_indexes() + per-GPU warm-up (the same
# CUDA-context wait TimeoutStartSec covers — up to minutes on a slow machine),
# so pg-cuvs-wait-socket's `[ -S ... ]` poll below would match that leftover
# file immediately and report the daemon ready long before it is listening.
# Deleting it first makes a later `[ -S ... ]` unambiguous: the path can only
# exist again once the new process has actually bound it. `-` prefix: a
# missing file (first-ever start) must not fail the unit.
#
# TimeoutStopSec: on `systemctl restart`, systemd stops the running instance
# before this ExecStartPre even runs. Shutdown serializes every resident index
# to disk one at a time (graceful_shutdown() in pg_cuvs_server.c) before it
# unlinks the socket — with several/large indexes this can outrun systemd's
# default stop timeout (90s), and the ensuing SIGKILL both loses whatever
# indexes had not yet been saved and reintroduces the stale-socket race above.
sudo tee /etc/systemd/system/pg-cuvs-server.service > /dev/null <<UNIT
[Unit]
Description=pg_cuvs GPU sidecar daemon
After=network.target

[Service]
Type=simple
User=$(id -un)
Group=$(id -gn)
ExecStartPre=/bin/mkdir -p $IDX
ExecStartPre=-/bin/rm -f /tmp/.s.pg_cuvs
ExecStart=$(pg_config --bindir)/pg_cuvs_server --socket /tmp/.s.pg_cuvs --index-dir $IDX --gpu-devices 0
ExecStartPost=/usr/local/bin/pg-cuvs-wait-socket
TimeoutStartSec=600
TimeoutStopSec=300
Restart=on-failure

[Install]
WantedBy=multi-user.target
UNIT
# Idempotent: re-running the bootstrap overwrites the unit and re-enables it.
sudo systemctl daemon-reload
sudo systemctl enable pg-cuvs-server
# `|| true` under `set -e`: this blocks until ExecStartPost returns, so a machine
# slower than TimeoutStartSec makes systemctl exit non-zero. Aborting here would
# skip the diagnostic block below, which is the only thing that says why.
sudo systemctl restart pg-cuvs-server || true
# Type=simple returns as soon as the process is spawned, so still gate on the
# socket here rather than trusting `systemctl start` to mean "ready".
set +x
for _ in $(seq 1 600); do
  [ -S /tmp/.s.pg_cuvs ] && break
  sleep 0.5
done
set -x
# The unit's ExecStartPost already chmods; this is the hard gate, and the chmod
# is repeated only so a partially-started unit still leaves a usable socket.
# Diagnostics come from the journal now that the daemon is a unit, not a nohup.
test -S /tmp/.s.pg_cuvs && sudo chmod 666 /tmp/.s.pg_cuvs && echo "SOCKET OK" \
  || { echo "SOCKET MISSING after 5 min"
       systemctl is-active pg-cuvs-server || true
       sudo journalctl -u pg-cuvs-server --no-pager -n 30 || true
       exit 1; }

echo "=== [5/5] dataset: wiki_all_1M -> corpus.fbin/queries_10k.fbin/gt.npy ==="
D="$USER_HOME/data"; mkdir -p "$D/raw"
if [ ! -f "$D/corpus.fbin" ]; then
  curl -sSL -o "$D/raw/wiki_all_1M.tar" \
    https://data.rapids.ai/raft/datasets/wiki_all_1M/wiki_all_1M.tar
  tar -xf "$D/raw/wiki_all_1M.tar" -C "$D/raw"
  # cuvs-bench backend wants: corpus.fbin, queries_10k.fbin, gt_<n>.npy
  ln -f "$D/raw/base.1M.fbin"  "$D/corpus.fbin"
  ln -f "$D/raw/queries.fbin"  "$D/queries_10k.fbin"
  "$BENCH/bin/python" - <<PY
import numpy as np
D="$D"; n,d = np.fromfile(f"{D}/raw/groundtruth.1M.neighbors.ibin", dtype=np.uint32, count=2)
gt = np.fromfile(f"{D}/raw/groundtruth.1M.neighbors.ibin", dtype=np.int32, offset=8).reshape(int(n),int(d))
np.save(f"{D}/gt_1000000.npy", gt); print("gt", gt.shape)
PY
fi

echo "=== BOOTSTRAP DONE ==="
echo "  build/bench env : $DEV | $BENCH"
echo "  daemon socket   : /tmp/.s.pg_cuvs   (index dir $IDX)"
echo "  build env file  : $ENV_FILE  (\`. $ENV_FILE\` — what this build used; make gpu-* sources it)"
echo "  dataset         : $D/{corpus.fbin,queries_10k.fbin,gt_1000000.npy}"
echo "  bench:  export PGHOST=/var/run/postgresql PATH=$BENCH/bin:/usr/lib/postgresql/16/bin:\$PATH LD_LIBRARY_PATH=$DEV/lib"
echo "          python bench/adr079_3o_recall.py --data-dir $D --n 1000000 --queries 100 --k 10 \\"
echo "                 --dbname shadeform --index-dir $IDX --selectivities 0.05,0.01,0.001,0.0002 --reuse-table --out /tmp/x.csv"
