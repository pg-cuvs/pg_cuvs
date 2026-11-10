"""Execute the product norm helper with CPU Thrust/RAFT resource fixtures.

This checks resource/stream dispatch and row semantics, not CUDA allocation.
The real RMM allocator under physical GPU pressure is a separate GPU test.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "src/cuvs_wrapper.cu").read_text()
start = source.index("struct CuvsRowSqNorm {")
end = source.index("/* fp16 kNN bound", start)
code = r'''
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
using std::isfinite;
#define __device__
struct Policy { bool rmm; int stream; };
static int calls = 0;
namespace raft {
struct device_resources { int stream = 17; int get_stream() { return stream; } };
namespace resource {
Policy get_thrust_policy(device_resources &res) { return {true, res.stream}; }
}
}
namespace thrust {
namespace cuda {
struct Par { Policy on(int stream) const { return {false, stream}; } };
static const Par par;
}
template<class T> struct counting_iterator { T value; counting_iterator(T v): value(v) {} };
template<class T> struct maximum { T operator()(T a, T b) { return a > b ? a : b; } };
template<class I, class F, class T, class Op>
T transform_reduce(Policy policy, I begin, I end, F f, T acc, Op op) {
    assert(policy.rmm && policy.stream == 17);
    ++calls;
    for (auto i = begin.value; i < end.value; ++i) acc = op(acc, f(i));
    return acc;
}
}
'''
code += source[start:end]
code += r'''
int main() {
    raft::device_resources res;
    const float rows[] = {3, 4, 9999, 0, 2, 9999};
    assert(device_max_sq_norm(res, rows, 2, 2, 3) == 25);
    assert(calls == 1);
    assert(device_max_sq_norm(res, rows, 0, 2, 3) == 0);
    assert(device_max_sq_norm(res, nullptr, 2, 2, 3) == 0);
    assert(calls == 1);
    const float bad[] = {1, std::numeric_limits<float>::quiet_NaN()};
    assert(std::isinf(device_max_sq_norm(res, bad, 1, 2, 2)));
    puts("PASS device norm: RMM policy/request stream, stride, empty/null, nonfinite");
}
'''
with tempfile.TemporaryDirectory(prefix="device-norm-") as tmp:
    cpp, binary = Path(tmp) / "test.cpp", Path(tmp) / "test"
    cpp.write_text(code)
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                    str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], cwd=tmp, check=True)
