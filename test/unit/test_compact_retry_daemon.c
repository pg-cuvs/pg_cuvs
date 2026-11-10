/* Reviewer-only proof: real handle_compact, file persistence and notice bytes.
 * CUDA merge results are controlled; this is not GPU execution. */
#define _GNU_SOURCE
#define main pg_cuvs_server_program_main
#define cuvs_cagra_compact fixture_compact
#define cuvs_cagra_serialize fixture_serialize
#include "../../src/pg_cuvs_server.c"
#undef main
#undef cuvs_cagra_compact
#undef cuvs_cagra_serialize
#include <assert.h>

extern CuvsCagraIndex cuvs_cagra_compact(CuvsCagraIndex,const uint32_t*,int64_t,uint32_t,int,int*);
extern int cuvs_cagra_serialize(CuvsCagraIndex,const char*,int);
static int retry_dtype, merge_fails, persist_fails, merge_calls;

CuvsCagraIndex fixture_compact(CuvsCagraIndex ix,const uint32_t *bits,
    int64_t n,uint32_t metric,int dev,int *retry)
{
    ++merge_calls;
    assert(n==4 && bits[0]==13);
    *retry=retry_dtype;
    if (merge_fails) return NULL;
    return cuvs_cagra_compact(ix,bits,n,metric,dev,NULL);
}
int fixture_serialize(CuvsCagraIndex ix,const char *path,int dev)
{
    return persist_fails ? 1 : cuvs_cagra_serialize(ix,path,dev);
}

static size_t file_bytes(const char *path,unsigned char *buf,size_t cap)
{
    FILE *f=fopen(path,"rb");assert(f);
    size_t n=fread(buf,1,cap,f);assert(feof(f));fclose(f);return n;
}

static void run_case(int dtype,int fail_merge,int fail_persist)
{
    char dir[]="/tmp/r3-compact-XXXXXX";assert(mkdtemp(dir));
    snprintf(g_index_dir,sizeof(g_index_dir),"%s",dir);
    g_indexes=calloc(1,sizeof(IndexEntry));g_n_indexes=1;
    IndexEntry *e=&g_indexes[0];
    float vecs[]={1,0,2,0,3,0,4,0};
    e->valid=1;e->db_oid=1;e->index_oid=2;e->dim=2;e->n_vecs=4;
    e->graph_degree=2;e->metric=CUVS_METRIC_L2;
    e->handle=cuvs_cagra_build(vecs,4,2,CUVS_METRIC_L2,2,3,CUVS_CAGRA_BUILD_AUTO,0,0,NULL);
    assert(e->handle);e->tids=malloc(4*sizeof(uint64_t));
    for(int i=0;i<4;++i)e->tids[i]=101+i;
    persist_fails=0;assert(save_index(e)==0);
    char tomb[512],index_path[512];
    snprintf(tomb,sizeof(tomb),"%s/1_2.tombstone",dir);
    index_file_path(index_path,sizeof(index_path),dir,1,2);
    CuvsTombstoneHeader th={.magic=CUVS_TOMBSTONE_MAGIC,.version=1,.n_entries=1};
    CuvsTombstoneRecord tr={.tid=102,.delete_xid=1};
    FILE *tf=fopen(tomb,"wb");assert(tf);
    assert(fwrite(&th,sizeof(th),1,tf)==1);assert(fwrite(&tr,sizeof(tr),1,tf)==1);fclose(tf);
    unsigned char old_disk[1024],new_disk[1024],old_tomb[128],new_tomb[128];
    size_t old_size=file_bytes(index_path,old_disk,sizeof(old_disk));
    size_t tomb_size=file_bytes(tomb,old_tomb,sizeof(old_tomb));
    CuvsCagraIndex old_handle=e->handle;uint64_t *old_tids=e->tids;
    retry_dtype=dtype;merge_fails=fail_merge;persist_fails=fail_persist;merge_calls=0;
    int sv[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,sv)==0);
    char dirbuf[256]={0};snprintf(dirbuf,sizeof(dirbuf),"%s",dir);
    assert(cuvs_fd_send(sv[0],-1,dirbuf,sizeof(dirbuf))==0);
    CuvsCmdFrame cmd={.op=CUVS_OP_COMPACT,.db_oid=1,.index_oid=2};
    handle_compact(sv[1],&cmd);
    CuvsReplyHeader hdr;assert(recv_all(sv[0],&hdr,sizeof(hdr))==0);
    assert(merge_calls==1);
    if(fail_merge) {
        assert(hdr.status==CUVS_STATUS_BUILD_FAILED);
        assert(e->handle==old_handle && e->tids==old_tids && e->n_vecs==4 && e->compact_count==0);
        assert(file_bytes(index_path,new_disk,sizeof(new_disk))==old_size && !memcmp(old_disk,new_disk,old_size));
        assert(file_bytes(tomb,new_tomb,sizeof(new_tomb))==tomb_size && !memcmp(old_tomb,new_tomb,tomb_size));
    } else if(fail_persist) {
        assert(hdr.status==CUVS_STATUS_PERSIST_FAILED);
        assert(e->handle!=old_handle && e->n_vecs==3 && e->compact_count==1 && !e->file_current);
        assert(access(tomb,F_OK)==0);
        printf("OBSERVATION persist failure after retry: notice_empty=%d old_handle_retained=%d tombstone_retained=1\n",!hdr.notice_out[0],e->handle==old_handle);
    } else {
        assert(hdr.status==CUVS_STATUS_OK && e->n_vecs==3 && e->compact_count==1 && e->file_current);
        assert(e->tids[0]==101 && e->tids[1]==103 && e->tids[2]==104);
        assert(access(tomb,F_OK)!=0);
    }
    if(dtype && !fail_persist) {
        CuvsNoticeBatch batch;assert(cuvs_notice_decode(hdr.notice_out,sizeof(hdr.notice_out),&batch)==0);
        assert(batch.count==1);
        assert(strstr(batch.pairs[0].message,dtype==2?"fp32":"fp16"));
        assert(strstr(batch.pairs[0].message,fail_merge?"failed":"succeeded"));
        if(fail_merge) assert(strstr(batch.pairs[0].detail,"retained"));
    } else assert(!hdr.notice_out[0]);
    printf("PASS daemon fixture dtype=%d merge_fail=%d persist_fail=%d status=%u\n",dtype,fail_merge,fail_persist,hdr.status);
    close(sv[0]);close(sv[1]);cuvs_cagra_free(e->handle,0);free(e->tids);
    free(e->rev_tids);free(e->rev_item_ids);free(g_indexes);g_indexes=NULL;g_n_indexes=0;
}
int main(void)
{
    run_case(0,0,0);run_case(1,0,0);run_case(2,0,0);
    run_case(2,1,0);run_case(2,0,1);
    return 0;
}
