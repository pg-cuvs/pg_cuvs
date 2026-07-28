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
IDX=/var/lib/pg_cuvs_indexes
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
ENV_FILE="$USER_HOME/.pg_cuvs_env"
cat > "$ENV_FILE" <<EOF
export CONDA_PREFIX="$DEV"
export PATH="$DEV/bin:/usr/lib/postgresql/16/bin:\$PATH"
export LD_LIBRARY_PATH="$DEV/lib"
export NVCC="nvcc -ccbin /usr/bin/g++"
export PGHOST=/var/run/postgresql
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
sudo mkdir -p "$IDX"; sudo chmod 777 "$IDX"
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
sudo -u postgres psql -c "SELECT pg_reload_conf()"
# `|| true`: pkill exits 1 when nothing matched, which is the normal case on a
# fresh box and would abort the script under `set -e`.
pkill -9 -f pg_cuvs_server 2>/dev/null || true; sleep 1
nohup "$REPO/pg_cuvs_server" --socket /tmp/.s.pg_cuvs --index-dir "$IDX" --gpu-devices 0 \
  > "$USER_HOME/daemon.log" 2>&1 &
# Poll, don't guess. The socket appears only after the daemon's first CUDA
# context is built, and that is a property of the machine, not of the build:
# ~12s on Massed Compute but over 3 minutes on a Paperspace A100. The former
# fixed `sleep 12` turned the slower machine into a bogus "SOCKET MISSING"
# failure for a daemon that was still initialising normally.
set +x
for _ in $(seq 1 600); do
  [ -S /tmp/.s.pg_cuvs ] && break
  sleep 0.5
done
set -x
test -S /tmp/.s.pg_cuvs && sudo chmod 666 /tmp/.s.pg_cuvs && echo "SOCKET OK" \
  || { echo "SOCKET MISSING after 5 min"; tail -30 "$USER_HOME/daemon.log"; exit 1; }

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
