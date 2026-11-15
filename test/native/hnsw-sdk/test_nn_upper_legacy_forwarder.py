#!/usr/bin/env python3
"""Exercise SDK six-argument forwarder with four CPU type stubs."""
import argparse
from pathlib import Path
import subprocess
import tempfile

p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--mutant',action='store_true');a=p.parse_args()
source=(a.source/'neighbors/detail/hnsw.hpp').read_text()
anchor='__attribute__((noinline, used)) void all_neighbors_graph('
if source.count(anchor)!=1:
 print('legacy six-argument forwarder absent')
 raise SystemExit(2)
start=source.rfind('template <typename T>',0,source.index(anchor));brace=source.index('{',start);depth=1;end=brace+1
while depth:
 if source[end]=='{':depth+=1
 if source[end]=='}':depth-=1
 end+=1
wrapper=source[start:end]
if a.mutant:
 old='host_budget, gpu_budget, size_t{0});';assert wrapper.count(old)==1
 wrapper=wrapper.replace(old,'host_budget, gpu_budget, size_t{1});')
prefix=r'''
#include <cassert>
#include <cstdint>
#include <optional>
#include <cstdio>
namespace raft { struct resources {}; struct row_major {};
 template<class T,class I,class L> struct host_matrix_view { int marker; };
}
namespace cuvs::distance { enum class DistanceType { L2, IP }; }
struct Half {};
struct Seen { int data, neighbors; size_t dtype; const std::optional<size_t> *host,*gpu; bool host_set,gpu_set; cuvs::distance::DistanceType metric; } seen;
template<class T> void all_neighbors_graph(raft::resources const&,
 raft::host_matrix_view<const T,int64_t,raft::row_major> dataset,
 raft::host_matrix_view<uint32_t,int64_t,raft::row_major> neighbors,
 cuvs::distance::DistanceType metric,const std::optional<size_t>& host,
 const std::optional<size_t>& gpu,size_t dtype) {
 seen={dataset.marker,neighbors.marker,dtype,&host,&gpu,host.has_value(),gpu.has_value(),metric};
}
'''
suffix=r'''
template<class T> void legacy(const std::optional<size_t>& host,const std::optional<size_t>& gpu,int id) {
 raft::resources res;raft::host_matrix_view<const T,int64_t,raft::row_major> data{id};
 raft::host_matrix_view<uint32_t,int64_t,raft::row_major> neighbors{id+10};
 all_neighbors_graph(res,data,neighbors,cuvs::distance::DistanceType::IP,host,gpu);
 assert(seen.data==id && seen.neighbors==id+10 && seen.dtype==0);
 assert(seen.host_set && seen.gpu_set);
 assert(seen.host==&host && seen.gpu==&gpu && seen.metric==cuvs::distance::DistanceType::IP);
}
int main() {
 std::optional<size_t> host=1000,gpu=2000;
 legacy<float>(host,gpu,1);legacy<Half>(host,gpu,2);legacy<int8_t>(host,gpu,3);legacy<uint8_t>(host,gpu,4);
 raft::resources res;raft::host_matrix_view<const float,int64_t,raft::row_major> data{9};
 raft::host_matrix_view<uint32_t,int64_t,raft::row_major> neighbors{19};
 all_neighbors_graph(res,data,neighbors,cuvs::distance::DistanceType::IP,host,gpu,SIZE_MAX-1);
 assert(seen.dtype==SIZE_MAX-1 && seen.host==&host && seen.gpu==&gpu);
 all_neighbors_graph(res,data,neighbors,cuvs::distance::DistanceType::IP);
 assert(seen.dtype==0 && !seen.host_set && !seen.gpu_set);
 puts("four legacy type forwarders and new NN sentinel: PASS (CPU mock)");
}
'''
with tempfile.TemporaryDirectory(prefix='pgcuvs-upper-forwarder-') as tmp:
 d=Path(tmp);(d/'fixture.cpp').write_text(prefix+wrapper+suffix)
 b=subprocess.run(['c++','-std=c++17','-O2','-Wall','-Wextra','-Werror',str(d/'fixture.cpp'),'-o',str(d/'fixture')],capture_output=True,text=True)
 if b.returncode:raise RuntimeError('compile failure: '+b.stderr)
 r=subprocess.run([str(d/'fixture')],capture_output=True,text=True)
 if a.mutant:
  assert r.returncode!=0 and 'seen.dtype==0' in r.stderr, r.stderr
  print('legacy dtype mutation detected (CPU mock)')
 else:
  if r.returncode:raise RuntimeError('fixture runtime failure: '+r.stderr)
  print(r.stdout,end='')
