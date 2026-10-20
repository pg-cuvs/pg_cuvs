#!/usr/bin/env bash
set -euo pipefail
test "$(hostname)" = brev-v6nc08sjs
. /home/ubuntu/.pg_cuvs_env
task_root=/home/ubuntu/index-accel-validation
git clone --shared /home/ubuntu/pg_cuvs "$task_root/native-peak-source"
cd "$task_root/native-peak-source"
git checkout --detach fded8ce
python3 - <<'PY'
from pathlib import Path
p=Path('src/cuvs_wrapper.cu');s=p.read_text();start=s.index('cuvs_cagra_search_batch(');pos=s.index('        sparams.itopk_size = itopk;',start)
s=s[:pos]+s[pos:].replace('        sparams.itopk_size = itopk;','        sparams.itopk_size = 512; /* private peak probe only; returned k remains64 */\n        sparams.search_width = 1;',1);p.write_text(s)
PY
git diff > "$task_root/native-peak-wrapper.patch"
make src/cuvs_wrapper.o
g++ -std=c++17 -O2 -Isrc -I"$CONDA_PREFIX/include" -I"$CONDA_PREFIX/include/rapids" \
  "$task_root/peak_probe.cpp" src/cuvs_wrapper.o /home/ubuntu/pg_cuvs/src/cuvs_util_server.o \
  -L"$CONDA_PREFIX/lib" -Wl,-rpath,"$CONDA_PREFIX/lib" -lcuvs -lrmm -lrapids_logger -lcudart -ldl -lpthread -o "$task_root/peak_probe"
sha256sum "$task_root/peak_probe" "$task_root/peak_probe.cpp" src/cuvs_wrapper.cu
