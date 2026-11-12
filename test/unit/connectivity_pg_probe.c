/* Private PostgreSQL test module. Include the production adapters verbatim;
 * wrap only allocation accounting and the kernel entry synchronization point. */
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include "cuvs_connectivity.h"

static void *heaps[16], *maps[16];
static unsigned heap_live, map_live;
static int probe_action;
static void *probe_malloc(size_t n)
{
    void *p=malloc(n);
    if(p){for(unsigned i=0;i<16;i++)if(!heaps[i]){heaps[i]=p;heap_live++;break;}}
    return p;
}
static void probe_free(void *p)
{
    for(unsigned i=0;i<16;i++)if(p && heaps[i]==p){heaps[i]=NULL;heap_live--;break;}
    free(p);
}
static void *probe_mmap(void *a,size_t n,int prot,int flags,int fd,off_t off)
{
    void *p=mmap(a,n,prot,flags,fd,off);
    if(p!=MAP_FAILED){for(unsigned i=0;i<16;i++)if(!maps[i]){maps[i]=p;map_live++;break;}}
    return p;
}
static int probe_munmap(void *p,size_t n)
{
    int rc=munmap(p,n);
    if(!rc){for(unsigned i=0;i<16;i++)if(maps[i]==p){maps[i]=NULL;map_live--;break;}}
    return rc;
}
static int probe_scratch(uint32_t n,uint32_t entry,uint32_t level,uint32_t maxlevel,
                        CuvsLinks links,void *ctx,CuvsConnectivityCancel cancel,
                        void *scratch,CuvsConnectivity *out)
{
    if(probe_action==1)InterruptPending=1; /* benign pending flag, no cancel */
    if(probe_action>=2){
        struct timespec start,now;
        bool ready=false;
        ereport(NOTICE,(errmsg("CONN203_WORKSPACE_READY heap=%u maps=%u",heap_live,map_live)));
        clock_gettime(CLOCK_MONOTONIC,&start);
        /* Unrelated signals may wake pg_usleep. Wait for the requested flag,
         * without servicing it until the production callback is entered. */
        while(!ready){
            ready=probe_action==2 ? LogMemoryContextPending :
                  probe_action==5 ? ProcDiePending : QueryCancelPending;
            if(ready)break;
            pg_usleep(10000L);
            clock_gettime(CLOCK_MONOTONIC,&now);
            if(now.tv_sec-start.tv_sec>5)elog(ERROR,"expected PG test signal missing");
        }
    }
    return cuvs_connectivity_scratch(n,entry,level,maxlevel,links,ctx,cancel,scratch,out);
}
#define malloc probe_malloc
#define free probe_free
#define mmap probe_mmap
#define munmap probe_munmap
#define cuvs_connectivity_scratch probe_scratch
#include "hnsw_export.c"
#undef cuvs_connectivity_scratch
#undef malloc
#undef free
#undef mmap
#undef munmap

PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(conn203_probe);
PG_FUNCTION_INFO_V1(conn203_probe_state);

Datum conn203_probe_state(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text(psprintf("{\"heap_live\":%u,\"map_live\":%u}",heap_live,map_live)));
}

Datum conn203_probe(PG_FUNCTION_ARGS)
{
    const uint32_t n=131072;
    bool file=PG_GETARG_BOOL(0);
    CuvsConnectivity c;
    StringInfoData s;
    probe_action=PG_GETARG_INT32(1);
    if(file){
        HnswlibHeader h={0};
        char path[]="/tmp/conn203-test-input-XXXXXX";
        int fd=mkstemp(path);
        FILE *f;
        uint32_t row[6]={2,0,0,0,0,0},zero=0;
        if(fd<0)elog(ERROR,"test input creation failed");
        unlink(path);
        f=fdopen(fd,"wb");
        if(!f){close(fd);elog(ERROR,"test input stream failed");}
        h.curElementCount=n;h.maxElements=n;h.enterpointNode=0;h.maxlevel=0;
        h.sizeDataPerElement=sizeof(row);h.maxM=2;h.maxM0=2;
        h.offsetData=12;h.labelOffset=16;
        if(fwrite(&h,sizeof(h),1,f)!=1)elog(ERROR,"test header write failed");
        for(uint32_t i=0;i<n;i++){
            row[1]=(i+1)%n;row[2]=(i+2)%n;row[4]=i;
            if(fwrite(row,sizeof(row),1,f)!=1)elog(ERROR,"test row write failed");
        }
        for(uint32_t i=0;i<n;i++)if(fwrite(&zero,4,1,f)!=1)elog(ERROR,"test upper write failed");
        if(fflush(f))elog(ERROR,"test input flush failed");
        PG_TRY();{hnsw_check_connectivity(fd,&h,&c);}
        PG_FINALLY();{fclose(f);}
        PG_END_TRY();
    }else{
        ElemDesc *e=palloc0((size_t)n*sizeof(*e));
        uint32_t *edges=palloc((size_t)n*8);
        for(uint32_t i=0;i<n;i++){
            edges[2*i]=(i+1)%n;edges[2*i+1]=(i+2)%n;
            e[i].lv0_count=2;e[i].lv0_links=edges+2*i;
        }
        c=hnsw_check_elements(e,n,0);
        pfree(edges);pfree(e);
    }
    initStringInfo(&s);appendStringInfoString(&s,"{");
    /* The production JSON helper appends a comma to an existing object. */
    appendStringInfo(&s,"\"heap_live\":%u,\"map_live\":%u",heap_live,map_live);
    hnsw_append_connectivity(&s,&c);appendStringInfoChar(&s,'}');
    PG_RETURN_TEXT_P(cstring_to_text(s.data));
}
