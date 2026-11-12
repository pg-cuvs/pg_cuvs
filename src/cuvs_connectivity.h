#ifndef CUVS_CONNECTIVITY_H
#define CUVS_CONNECTIVITY_H
/* CPU structural reachability. A state (node, level) can follow edges at that
 * level or descend, never ascend. This is a possible-path upper bound on a
 * query's greedy descent, NOT a finite-ef recall guarantee. No graph is changed.
 * Callback buffers may be unaligned; count UINT32_MAX marks a nonexistent level. Caller validates file framing beforehand. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
typedef struct CuvsConnectivity {
    uint32_t magic, version, available, spilled;
    uint64_t nodes, unreachable, unreachable_l0, wcc_l0, wcc_all, elapsed_us;
} CuvsConnectivity;
#define CUVS_CONNECTIVITY_MAGIC 0x434F4E31u
typedef const unsigned char *(*CuvsLinks)(void *, uint32_t, uint32_t, uint32_t *);
typedef int (*CuvsConnectivityCancel)(void *);
static uint32_t cuvs_conn_root(uint32_t *p, uint32_t a) {
    while (p[a]!=a) { p[a]=p[p[a]]; a=p[a]; } return a;
}
static int cuvs_conn_reach(uint32_t n,uint32_t entry,uint32_t level,
    CuvsLinks links,void *ctx,int32_t *seen,unsigned char *queued,uint32_t *stack,
    CuvsConnectivityCancel cancel,uint64_t *missing) {
    uint32_t used=0; uint64_t steps=0; *missing=n;
    for(uint32_t i=0;i<n;i++)seen[i]=-1;
    memset(queued,0,n); seen[entry]=(int32_t)level;stack[used++]=entry;queued[entry]=1;
    while(used) {
        uint32_t u=stack[--used];queued[u]=0;
        if ((++steps&4095)==0 && cancel && cancel(ctx)) return 0;
        for(int32_t l=0;l<=seen[u];l++) {
            uint32_t count=0;const unsigned char *p=links(ctx,u,(uint32_t)l,&count);
            if(count==UINT32_MAX)break;
            if (!p && count) return 0;
            for(uint32_t k=0;k<count;k++) {
                uint32_t v;memcpy(&v,p+4*(size_t)k,4);
                if(v>=n)return 0;
                if(l){uint32_t nc=0;(void)links(ctx,v,(uint32_t)l,&nc);if(nc==UINT32_MAX)return 0;}
                if(seen[v]<l) {seen[v]=l;if(!queued[v]){stack[used++]=v;queued[v]=1;}}
            }
        }
    }
    for(uint32_t i=0;i<n;i++)if(seen[i]>=0)(*missing)--;
    return 1;
}
static int cuvs_connectivity_scratch(uint32_t n,uint32_t entry,uint32_t entry_level,uint32_t maxlevel,
    CuvsLinks links,void *ctx,CuvsConnectivityCancel cancel,void *scratch,CuvsConnectivity *out) {
    uint32_t *parents=NULL,*stack=NULL; int32_t *seen=NULL;unsigned char *queued=NULL;
    int ok=0;memset(out,0,sizeof(*out));out->magic=CUVS_CONNECTIVITY_MAGIC;out->version=1;out->nodes=n;
    if(!n || entry>=n || maxlevel>INT32_MAX || entry_level>maxlevel || (uint64_t)n*8>SIZE_MAX)return 0;
    if(scratch) {
        parents=(uint32_t*)scratch;stack=parents+(size_t)n*2;
        seen=(int32_t*)(stack+n);queued=(unsigned char*)(seen+n);
    } else {
        parents=(uint32_t*)malloc((size_t)n*8);stack=(uint32_t*)malloc((size_t)n*4);
        seen=(int32_t*)malloc((size_t)n*4);queued=(unsigned char*)malloc(n);
    }
    if(!parents||!stack||!seen||!queued)goto done;
    for(uint64_t i=0;i<(uint64_t)n*2;i++)parents[i]=(uint32_t)(i%n);
    out->wcc_l0=out->wcc_all=n;
    for(uint32_t u=0;u<n;u++) {
        if((u&4095)==0 && cancel && cancel(ctx))goto done;
        /* Callback returns no links for levels this node does not have. */
        for(uint32_t l=0;l<=maxlevel;l++) {
            uint32_t count=0;const unsigned char *p=links(ctx,u,l,&count);
            if(count==UINT32_MAX)break;
            if(!p && count)goto done;
            for(uint32_t k=0;k<count;k++) {
                uint32_t v;memcpy(&v,p+4*(size_t)k,4);if(v>=n)goto done;
                if(l){uint32_t nc=0;(void)links(ctx,v,l,&nc);if(nc==UINT32_MAX)goto done;}
                for(int which=(l?1:0);which<2;which++) {
                    uint32_t *d=parents+(size_t)which*n;
                    uint32_t a=cuvs_conn_root(d,u),b=cuvs_conn_root(d,v);
                    if(a!=b){d[a]=b;if(which)out->wcc_all--;else out->wcc_l0--;}
                }
            }
        }
    }
    if(!cuvs_conn_reach(n,entry,0,links,ctx,seen,queued,stack,cancel,&out->unreachable_l0) ||
       !cuvs_conn_reach(n,entry,entry_level,links,ctx,seen,queued,stack,cancel,&out->unreachable))goto done;
    out->available=1;ok=1;
done:
    if(!scratch){free(parents);free(stack);free(seen);free(queued);}
    return ok;
}
static inline int cuvs_connectivity(uint32_t n,uint32_t entry,uint32_t entry_level,uint32_t maxlevel,
    CuvsLinks links,void *ctx,CuvsConnectivityCancel cancel,CuvsConnectivity *out) {
    return cuvs_connectivity_scratch(n,entry,entry_level,maxlevel,links,ctx,cancel,NULL,out);
}
#endif
