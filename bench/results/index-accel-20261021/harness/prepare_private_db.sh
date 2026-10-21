#!/usr/bin/env bash
set -euo pipefail
test "$(hostname)" = brev-v6nc08sjs
test "$(cat /home/ubuntu/index-accel-validation/bootstrap.exit)" = 0
. /home/ubuntu/.pg_cuvs_env
if ! psql -XAt -d postgres -c "SELECT 1 FROM pg_database WHERE datname='index_accel_smoke'" | grep -q 1; then
  createdb index_accel_smoke
fi
psql -X -v ON_ERROR_STOP=1 -d index_accel_smoke \
  -c 'CREATE EXTENSION IF NOT EXISTS vector' \
  -c 'CREATE EXTENSION IF NOT EXISTS pg_cuvs' \
  -c 'CREATE EXTENSION IF NOT EXISTS pg_prewarm'
git -C /home/ubuntu/pg_cuvs rev-parse HEAD
nvcc --version
pg_config --version
systemctl is-active postgresql@16-main pg-cuvs-server
/opt/miniforge3/bin/conda list -n cuvs_dev --json | /usr/bin/python3 -c \
  'import json,sys; d=json.load(sys.stdin); print(json.dumps([{k:p.get(k) for k in ("name","version","build_string")} for p in d if p["name"] in ("libcuvs","librmm","cuda-version","cuda-nvcc","cuda-cudart")],indent=2))'
