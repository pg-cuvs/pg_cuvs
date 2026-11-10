from pathlib import Path
import subprocess, sys, tempfile
ROOT = Path(__file__).resolve().parents[2]
src = (ROOT / "src/cuvs_wrapper.cu").read_text()
def block(text, needle):
    start=text.index(needle); pos=text.index('{',start)+1; depth=1
    while depth:
        depth += (text[pos]=='{')-(text[pos]=='}'); pos += 1
    return text[start:pos]
prefix = r'''
#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <variant>
#include <vector>
#include "cuvs_ipc.h"
#include "cuvs_hnsw_policy.h"
#include "cuvs_hnsw_build_common.h"
using std::isfinite;
#define __device__
constexpr int CUDA_R_16F=16, CUDA_R_32F=32;
struct Policy { bool rmm; int stream; };
namespace raft {
struct device_resources { int get_stream() { return 17; } void sync_stream() {} };
struct logic_error : std::runtime_error { using std::runtime_error::runtime_error; };
template<class T> struct matrix_extent { T n,d; T extent(int i) const {return i ? d : n;} };
template<class T> matrix_extent<T> make_extents(T n,T d) { return {n,d}; }
namespace resource { Policy get_thrust_policy(device_resources &r) {return {true,r.get_stream()};} }
}
namespace thrust {
namespace cuda {struct Par {Policy on(int s) const {return {false,s};}}; static const Par par;}
template<class T> struct counting_iterator {T value;counting_iterator(T v):value(v){}};
template<class T> struct maximum {T operator()(T a,T b){return a>b?a:b;}};
template<class I,class F,class T,class O> T transform_reduce(Policy p,I a,I b,F f,T v,O op){
 assert(p.rmm && p.stream==17);for(auto i=a.value;i<b.value;++i)v=op(v,f(i));return v;
}
}
namespace cuvs::neighbors::nn_descent {
bool has_enough_device_memory(raft::device_resources&,raft::matrix_extent<int64_t>,size_t){return true;}
}
namespace cuvs::neighbors::cagra {
namespace graph_build_params {
struct nn_descent_params { nn_descent_params(size_t,int=0){} };
struct ivf_pq_params {
 struct {uint32_t n_lists;double kmeans_trainset_fraction;} build_params;
 struct {int lut_dtype=16,internal_distance_dtype=16,coarse_search_dtype=16;uint32_t max_internal_batch_size=131072;} search_params;
 ivf_pq_params(raft::matrix_extent<int64_t> shape,int=0) {
  auto n=shape.extent(0);build_params.n_lists=std::max<uint32_t>(1,n/2000);
  build_params.kmeans_trainset_fraction=std::clamp(1.0/std::sqrt(n*1e-5),std::min(1.0,32.0*build_params.n_lists/n),1.0);
 }
};
}
struct index_params {int metric=0;size_t intermediate_graph_degree=128;
 std::variant<std::monostate,graph_build_params::nn_descent_params,graph_build_params::ivf_pq_params> graph_build_params;};
}
struct PR {void fence_check(){}};
struct DS {std::vector<float> rows;explicit DS(int64_t n):rows(n,150){}const float* data_handle()const{return rows.data();}int64_t extent(int i)const{return i?1:rows.size();}int64_t stride(int)const{return 1;}};
static int calls, expected_dtype;
static int64_t expected_n;
namespace cuvs::neighbors::cagra {
int merge(raft::device_resources&,index_params &p,int,int){
 if(++calls==1)throw std::bad_alloc();
 auto &pq=std::get<graph_build_params::ivf_pq_params>(p.graph_build_params);
 assert(pq.search_params.lut_dtype==expected_dtype);
 assert(pq.search_params.internal_distance_dtype==expected_dtype);
 assert(pq.search_params.coarse_search_dtype==expected_dtype);
 assert(pq.search_params.max_internal_batch_size==4096);
 assert(pq.build_params.kmeans_trainset_fraction*expected_n+0.01>=32.0*pq.build_params.n_lists);
 return 42;
}
}
'''


mutation=sys.argv[1] if len(sys.argv)>1 else "baseline"
mutations={"retry_metric_is_cosine":("metric, CUVS_CAGRA_BUILD_IVF_PQ,","1u, CUVS_CAGRA_BUILD_IVF_PQ,"),
"retry_training_floor_dropped":("32ULL * pq.build_params.n_lists","0ULL * pq.build_params.n_lists")}
text=src
if mutation!="baseline":
    old,new=mutations[mutation]; assert text.count(old)==1; text=text.replace(old,new)
norm=text[text.index("struct CuvsRowSqNorm {"):text.index("/* fp16 kNN bound")]
helper=block(text,"static void cagra_knn_precision(")
classifier=block(text,"static bool cagra_compact_is_oom(")
retry=block(text,"auto new_idx = [&]")+"();"
body="""
static int run(uint32_t metric,int64_t n_keep,int *ivfpq_retry) {
 raft::device_resources res;PR _pr;DS ds(n_keep+1);int indices=0,filter=0;
 cuvs::neighbors::cagra::index_params params;params.metric=metric;
 cagra_knn_precision(res,ds.data_handle(),ds.extent(0),ds.extent(1),ds.stride(0),
  raft::make_extents<int64_t>(n_keep,ds.extent(1)),metric,CUVS_CAGRA_BUILD_AUTO,params,"first");
"""+retry+"""
 return new_idx;
}
int main() {
 for (uint32_t met : {uint32_t(CUVS_METRIC_L2),uint32_t(CUVS_METRIC_IP)})
  for (int64_t n : {880000LL,8800000LL}) {
   calls=0;expected_n=n;expected_dtype=met==CUVS_METRIC_L2?CUDA_R_32F:CUDA_R_16F;
   int observed=0; assert(run(met,n,&observed)==42 && calls==2);
   assert(observed==(expected_dtype==CUDA_R_32F?2:1));
  }
 puts("PASS actual norm + precision helper + retry: L2/IP and 0.88M/8.8M");
}
"""
with tempfile.TemporaryDirectory(prefix="compact-retry-integrated-") as d:
    work=Path(d); cpp=work/"test.cpp"; binary=work/"test"
    cpp.write_text(prefix + norm + helper + classifier + body)
    c=subprocess.run(["c++","-std=c++17","-Wall","-Wextra","-I",str(ROOT/"src"),str(cpp),"-o",str(binary)],capture_output=True)
    if c.returncode: sys.stderr.write(c.stderr.decode()); sys.exit(2)
    r=subprocess.run([str(binary)],capture_output=True);sys.stdout.buffer.write(r.stdout);sys.stderr.buffer.write(r.stderr);sys.exit(r.returncode)
