#!/usr/bin/env bash
set -euo pipefail
test "$(hostname)" = brev-v6nc08sjs
. /home/ubuntu/.pg_cuvs_env
task_root=/home/ubuntu/index-accel-validation
cd "$task_root/native-peak-source"
python3 - <<'PY'
from pathlib import Path
s=Path('src/cuvs_wrapper.cu').read_text()
old = """auto counted_upstream = std::make_unique<RmmUpstream>(
            upstream, std::numeric_limits<size_t>::max());"""
new = r'''size_t private_free = 0, private_total = 0;
        if (cudaMemGetInfo(&private_free, &private_total) != cudaSuccess) return 1;
        size_t private_maximum = (private_total / 2 / 256) * 256;
        fprintf(stderr, "{\"private_maximum_pool_bytes\":%zu,\"total_cuda_bytes\":%zu}\n", private_maximum, private_total);
        auto counted_upstream = std::make_unique<RmmUpstream>(upstream, private_maximum);'''
assert s.count(old)==1
Path('src/cuvs_wrapper_bounded.cu').write_text(s.replace(old,new))
PY
nvcc -ccbin /usr/bin/g++ -O3 --compiler-options '-fPIC' -arch=sm_89 -std=c++17 --extended-lambda \
  -DRAFT_SYSTEM_LITTLE_ENDIAN=1 -DCUVS_BUILD_CAGRA_HNSWLIB \
  -I"$CONDA_PREFIX/include" -I"$CONDA_PREFIX/include/rapids" -Isrc \
  -c src/cuvs_wrapper_bounded.cu -o "$task_root/cuvs_wrapper_bounded.o"
nvcc -ccbin /usr/bin/g++ "$task_root/peak_probe.o" "$task_root/cuvs_wrapper_bounded.o" /home/ubuntu/pg_cuvs/src/cuvs_util_server.o \
  -L"$CONDA_PREFIX/lib" -Xlinker -rpath -Xlinker "$CONDA_PREFIX/lib" \
  -lcuvs -lrmm -lrapids_logger -lcudart -ldl -lpthread -o "$task_root/bounded_probe"
sha256sum "$task_root/bounded_probe" src/cuvs_wrapper_bounded.cu
