"""Execute the product compaction retry block with controlled merge failures.

CUDA/cuVS operations are fixtures; the retry control flow is copied verbatim
from the wrapper. GPU tests separately cover allocation and graph quality.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "src/cuvs_wrapper.cu").read_text()

def block(needle):
    start = source.index(needle)
    begin = source.index("{", start)
    end, depth = begin + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

try:
    classifier = block("static bool cagra_compact_is_oom(")
    retry = block("auto new_idx = [&]") + "();"
except ValueError:
    sys.exit("FAIL: compaction has no OOM retry path")

mutants = {
    "no_retry": ("if (!cagra_compact_is_oom(e)", "if (true || !cagra_compact_is_oom(e)"),
    "retry_any_error": ("!cagra_compact_is_oom(e)", "false"),
    "retry_existing_ivfpq": ("std::holds_alternative<gbp::ivf_pq_params>(params.graph_build_params)", "false"),
    "retry_uses_auto": ("metric, CUVS_CAGRA_BUILD_IVF_PQ,", "metric, CUVS_CAGRA_BUILD_AUTO,"),
    "retry_no_sync": ("                res.sync_stream();", "                ;"),
    "retry_no_fence": ("                _pr.fence_check();", "                ;"),
    "retry_not_observed": ("if (ivfpq_retry) *ivfpq_retry =", "if (false) *ivfpq_retry ="),
    "retry_training_unbounded": ("pq.build_params.kmeans_trainset_fraction = std::min(\n                            pq.build_params.kmeans_trainset_fraction, (double)train_rows / n_keep);", "pq.build_params.kmeans_trainset_fraction = 0.337;"),
    "retry_batch_not_capped": ("pq.search_params.max_internal_batch_size =\n                        std::min(pq.search_params.max_internal_batch_size, 4096u);", ""),
    "masked_oom_not_recognized": ("dynamic_cast<const raft::logic_error *>(&e)", "false"),
}
if len(sys.argv) > 1:
    old, new = mutants[sys.argv[1]]
    assert (classifier + retry).count(old) >= 1
    classifier, retry = classifier.replace(old, new), retry.replace(old, new)

code = r'''
#include <cassert>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <stdexcept>
#include <variant>
namespace raft { struct logic_error : std::runtime_error { using std::runtime_error::runtime_error; }; }
namespace gbp { struct ivf_pq_params { struct { uint32_t n_lists=440; double kmeans_trainset_fraction=0.337; } build_params; struct { int lut_dtype = 0; uint32_t max_internal_batch_size=131072; } search_params; }; }
namespace cuvs::neighbors::cagra { namespace graph_build_params = ::gbp; }
[[maybe_unused]] constexpr int CUDA_R_32F=1, CUVS_CAGRA_BUILD_IVF_PQ=1, CUVS_CAGRA_BUILD_AUTO=0;
struct Params { std::variant<int, gbp::ivf_pq_params> graph_build_params; };
struct DS { int data_handle() { return 1; } int extent(int i) { return i ? 128 : 1000000; } int stride(int) { return 128; } };
namespace raft { template<class T> int make_extents(int a, int b) { return a+b; } }
static int calls, syncs, fences, prepared, fail_first, fail_second;
static bool unsafe_data, dead_stream, dead_fence;
struct Res { void sync_stream() { ++syncs; if (dead_stream) throw std::runtime_error("stream"); } };
struct PR { void fence_check() { ++fences; if (dead_fence) throw std::runtime_error("fence"); } };
static void cagra_knn_precision(Res &, int, int, int, int, int, unsigned, int algo, Params &p, const char *) {
    ++prepared;
    assert(algo == CUVS_CAGRA_BUILD_IVF_PQ);
    gbp::ivf_pq_params pq; pq.search_params.lut_dtype = unsafe_data ? CUDA_R_32F : 0;
    p.graph_build_params = pq;
}
namespace cuvs::neighbors::cagra {
static int merge(Res &, Params &params, int, int) {
    ++calls;
    if (calls==2) assert(std::get<gbp::ivf_pq_params>(params.graph_build_params).search_params.max_internal_batch_size<=4096);
    if (calls==2) assert(std::get<gbp::ivf_pq_params>(params.graph_build_params).build_params.kmeans_trainset_fraction*880000<=65536.01);
    int failure = calls == 1 ? fail_first : fail_second;
    if (failure == 1) throw std::bad_alloc();
    if (failure == 2) throw raft::logic_error("Filtered merge isn't available on cpu memory");
    if (failure == 3) throw raft::logic_error("invalid dataset");
    return 42;
}
}
'''
code += classifier
code += r'''
static int run(bool already_ivfpq, int *ivfpq_retry) {
    Res res; PR _pr; Params params; DS ds;
    int n_keep=880000, indices=0, filter=0; unsigned metric=0;
    if (already_ivfpq) params.graph_build_params = gbp::ivf_pq_params{};
'''
code += retry + "\nreturn new_idx;\n}\n"
code += r'''
static void reset(int first, int second=0) {
    calls=syncs=fences=prepared=0; fail_first=first; fail_second=second;
    dead_stream=dead_fence=false;
}
int main() {
    int observed;
    reset(0); observed=0;
    assert(run(false,&observed)==42 && calls==1 && !prepared && observed==0);
    for (bool unsafe : {false,true}) for (int failure : {1,2}) {
        reset(failure); unsafe_data=unsafe; observed=0;
        assert(run(false,&observed)==42 && calls==2 && prepared==1);
        assert(syncs==2 && fences==2 && observed==(unsafe ? 2 : 1));
    }
    reset(2); unsafe_data=true;
    assert(run(false,nullptr)==42 && calls==2);
    for (int failure : {1,2,3}) {
        reset(failure); observed=0; bool caught=false;
        try { (void)run(true,&observed); } catch (const std::exception &) { caught=true; }
        assert(caught && calls==1 && !prepared && observed==0);
    }
    reset(3); observed=0;
    try { (void)run(false,&observed); assert(false); } catch (const raft::logic_error &) {}
    assert(calls==1 && !prepared);
    reset(1,2); observed=0;
    try { (void)run(false,&observed); assert(false); } catch (const raft::logic_error &) {}
    assert(calls==2 && prepared==1); // never a third merge
    for (bool stream : {false,true}) {
        reset(1); dead_stream=stream; dead_fence=!stream;
        try { (void)run(false,&observed); assert(false); } catch (const std::runtime_error &) {}
        assert(calls==1 && !prepared);
    }
    puts("PASS compaction retry: OOM/raw+SDK, fp16/fp32, one retry, non-OOM, IVF, stream/fence, observation");
}
'''
with tempfile.TemporaryDirectory(prefix="compact-retry-") as tmp:
    cpp, binary = Path(tmp) / "test.cpp", Path(tmp) / "test"
    cpp.write_text(code)
    compiled = subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                               str(cpp), "-o", str(binary)])
    if compiled.returncode:
        sys.exit(2)
    result = subprocess.run([str(binary)], cwd=tmp)
    sys.exit(0 if result.returncode == 0 else 1)
