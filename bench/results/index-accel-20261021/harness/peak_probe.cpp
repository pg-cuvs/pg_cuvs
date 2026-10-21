#include "cuvs_wrapper.h"
#include <rmm/mr/statistics_resource_adaptor.hpp>
#include <rmm/mr/per_device_resource.hpp>
#include <nvtx3/nvToolsExt.h>
#include <cuda_runtime_api.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <vector>

using Stats=rmm::mr::statistics_resource_adaptor<rmm::mr::device_memory_resource>;
struct Matrix {
 int fd; void* map;size_t bytes;uint32_t n,dim;const float* data;
 explicit Matrix(const char* path){fd=open(path,O_RDONLY);if(fd<0)throw std::runtime_error(path);struct stat st;fstat(fd,&st);bytes=st.st_size;map=mmap(nullptr,bytes,PROT_READ,MAP_PRIVATE,fd,0);if(map==MAP_FAILED)throw std::runtime_error("mmap");auto* h=static_cast<const uint32_t*>(map);n=h[0];dim=h[1];data=reinterpret_cast<const float*>(h+2);if(bytes!=8+size_t(n)*dim*4)throw std::runtime_error("fbin shape");}
 ~Matrix(){munmap(map,bytes);close(fd);}
};
struct Scope {
 const char* name;Stats *client,*up;int64_t base;std::chrono::steady_clock::time_point start;
 Scope(const char* n,Stats* c,Stats* u):name(n),client(c),up(u){base=c->push_counters().first.value;u->push_counters();nvtxRangePushA(n);start=std::chrono::steady_clock::now();}
 ~Scope(){cudaDeviceSynchronize();auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();nvtxRangePop();auto c=client->pop_counters();auto u=up->pop_counters();std::printf("{\"phase\":\"%s\",\"client_start_bytes\":%lld,\"peak_client_bytes\":%lld,\"extra_client_peak_bytes\":%lld,\"client_end_bytes\":%lld,\"client_allocation_calls\":%lld,\"pool_growth_calls\":%lld,\"pool_growth_bytes\":%lld,\"seconds\":%.9f}\n",name,(long long)base,(long long)(base+c.first.peak),(long long)c.first.peak,(long long)(base+c.first.value),(long long)c.second.total,(long long)u.second.total,(long long)u.first.total,elapsed);std::fflush(stdout);}
};
int main(int argc,char** argv){
 try{
  if(argc!=3)throw std::runtime_error("usage: peak_probe search|ivf_pq|nn_descent initial_pool_mb");
  char host[128];gethostname(host,sizeof(host));if(std::string(host)!="brev-v6nc08sjs")throw std::runtime_error("wrong host");
  std::string mode=argv[1];size_t mb=std::stoul(argv[2]);cudaSetDevice(0);
  auto* original=rmm::mr::get_current_device_resource();
  auto* upstream=new Stats(original);rmm::mr::set_current_device_resource(upstream);
  nvtxRangePushA("pool_init");if(cuvs_rmm_pool_init(0,mb*1024*1024))throw std::runtime_error("pool init");nvtxRangePop();
  auto* pool_resource=rmm::mr::get_current_device_resource();auto* client=new Stats(pool_resource);rmm::mr::set_current_device_resource(client);
  std::printf("{\"setup\":true,\"pool_mb\":%zu,\"upstream_type\":\"%s\",\"initial_allocation_calls\":%lld}\n",mb,typeid(*original).name(),(long long)upstream->get_allocations_counter().total);std::fflush(stdout);
  CuvsCagraIndex index=nullptr;
  if(mode=="search"){
   {Scope phase("load_original_graph",client,upstream);index=cuvs_cagra_deserialize("/home/ubuntu/index-accel-validation/graph-snapshot/16390_1016847.cagra",768,0);}
   if(!index)throw std::runtime_error("deserialize");Matrix q("/home/ubuntu/data/queries_10k.fbin");if(q.n!=10000||q.dim!=768)throw std::runtime_error("queries");std::vector<CuvsSearchResult> result(size_t(10000)*64);
   {Scope phase("search_it512_q10000_k64",client,upstream);if(cuvs_cagra_search_batch(index,q.data,10000,768,64,result.data(),0))throw std::runtime_error("batch");}
   for(auto const& r:result)if(r.item_id<0||r.item_id>=1000000)throw std::runtime_error("short fill/id");
  }else if(mode=="ivf_pq"||mode=="nn_descent"){
   Matrix m("/home/ubuntu/data/corpus.fbin");if(m.n!=1000000||m.dim!=768)throw std::runtime_error("corpus");
   {Scope phase(mode.c_str(),client,upstream);index=cuvs_cagra_build(m.data,m.n,m.dim,CUVS_METRIC_L2,64,256,mode=="ivf_pq"?CUVS_CAGRA_BUILD_IVF_PQ:CUVS_CAGRA_BUILD_NN_DESCENT,0);}
   if(!index)throw std::runtime_error("build failed (see stderr; peak is observed lower bound on failure)");
  }else throw std::runtime_error("mode");
  cuvs_cagra_free(index,0);cudaDeviceSynchronize();rmm::mr::set_current_device_resource(pool_resource);
  std::printf("{\"success\":true,\"final_client_bytes\":%lld,\"lifetime_pool_allocations\":%lld,\"lifetime_pool_reserved_bytes\":%lld}\n",(long long)client->get_bytes_counter().value,(long long)upstream->get_allocations_counter().total,(long long)upstream->get_bytes_counter().value);
  return 0;
 }catch(std::exception const& e){std::fprintf(stderr,"peak_probe: %s\n",e.what());return 1;}
}
