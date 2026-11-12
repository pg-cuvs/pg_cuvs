#include "cuvs_connectivity.h"
typedef struct {const uint32_t *u,*count;uint32_t width;} Input;
static const unsigned char *edges(void *ctx,uint32_t node,uint32_t level,uint32_t *n) {
 Input *in=ctx;if(level){*n=UINT32_MAX;return NULL;}*n=in->count[node];return (const unsigned char*)(in->u+(size_t)node*in->width);
}
int conn203_check(uint32_t n,uint32_t width,uint32_t root,const uint32_t *u,const uint32_t *counts,CuvsConnectivity *out) {
 Input in={u,counts,width};return cuvs_connectivity(n,root,0,0,edges,&in,NULL,out);
}

int conn203_check_mask(uint32_t n,uint32_t width,uint32_t root,const uint32_t *u,const uint32_t *counts,CuvsConnectivity *out,unsigned char *mask) {
 Input in={u,counts,width};void *scratch=malloc((size_t)n*17);int ok;
 if(!scratch)return 0;
 ok=cuvs_connectivity_scratch(n,root,0,0,edges,&in,NULL,scratch,out);
 if(ok){int32_t *seen=(int32_t*)((char*)scratch+(size_t)n*12);for(uint32_t i=0;i<n;i++)mask[i]=seen[i]>=0;}
 free(scratch);return ok;
}
