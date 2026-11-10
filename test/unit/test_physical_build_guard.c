/* Exercise real daemon handlers with only raw physical free replaced.
 * The shim still reports ample pooled/reclaimable memory, reproducing the
 * admission blind spot. CUDA/HMM safety itself is verified on the GPU. */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
static uint64_t raw_free = UINT64_C(64) << 30;
size_t fixture_raw_free(int dev);
#define cuvs_vram_raw_free_bytes_on fixture_raw_free
#define main pg_cuvs_server_program_main
#include "../../src/pg_cuvs_server.c"
#undef main
#undef cuvs_vram_raw_free_bytes_on
#include <assert.h>
#include <sys/un.h>
size_t fixture_raw_free(int dev) { (void)dev; return raw_free; }
extern void cuvs_shim_lw_reset(void);
extern uint32_t cuvs_shim_lw_calls(uint32_t);
#define N 32
#define DIM 8
static char tmpdir[256];
static void
fill_corpus(char *mem)
{
    float *v = (float *)mem;
    uint64_t *t = (uint64_t *)(mem + (size_t)N * DIM * sizeof(float));
    for (int64_t i = 0; i < N * (int64_t)DIM; i++) v[i] = (float)((i * 2654435761u) % 997) / 997.0f;
    for (int64_t i = 0; i < N; i++) t[i] = (uint64_t)i + 1;
}

static CuvsCmdFrame
build_cmd(uint32_t index_oid, uint32_t shard_count)
{
    CuvsCmdFrame cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = CUVS_OP_BUILD;
    cmd.db_oid = 1;
    cmd.index_oid = index_oid;
    cmd.n_vecs = N;
    cmd.dim = DIM;
    cmd.metric = CUVS_METRIC_L2;
    cmd.graph_degree = 4;
    cmd.intermediate_graph_degree = 8;
    cmd.shard_count = shard_count;
    return cmd;
}

static CuvsReplyHeader last_reply;

static int
reply_status(int fd)
{
    CuvsReplyHeader hdr;
    if (recv(fd, &hdr, sizeof(hdr), MSG_WAITALL) != (ssize_t)sizeof(hdr)) return -1;
    last_reply = hdr;
    return (int)hdr.status;
}

static int
run_handle_build(uint32_t index_oid, uint32_t shard_count)
{
    int sv[2], st;
    size_t total = (size_t)N * DIM * sizeof(float) + (size_t)N * sizeof(uint64_t);
    char dir[256];
    CuvsCmdFrame cmd = build_cmd(index_oid, shard_count);
    int mfd = memfd_create("lw_test", MFD_CLOEXEC);
    assert(mfd >= 0 && ftruncate(mfd, (off_t)total) == 0);
    char *mem = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    assert(mem != MAP_FAILED);
    fill_corpus(mem);
    munmap(mem, total);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    memset(dir, 0, sizeof(dir));
    snprintf(dir, sizeof(dir), "%s/idx", tmpdir);
    assert(cuvs_fd_send(sv[0], mfd, dir, sizeof(dir)) == 0);
    close(mfd);
    handle_build(sv[1], &cmd);
    st = reply_status(sv[0]);
    close(sv[0]); close(sv[1]);
    return st;
}

static int
run_handle_build_multi(uint32_t index_oid, uint32_t shards)
{
    int sv[2], st;
    char dir[256];
    CuvsPartialDesc desc[2];
    CuvsCmdFrame cmd = build_cmd(index_oid, shards);
    const int64_t half = N / 2;
    size_t per_vec = DIM * sizeof(float);
    char *whole = malloc((size_t)N * (per_vec + sizeof(uint64_t)));
    fill_corpus(whole);
    cmd.n_partials = 2;
    for (int p = 0; p < 2; p++)
    {
        size_t len = (size_t)half * (per_vec + sizeof(uint64_t));
        snprintf(desc[p].shm_name, sizeof(desc[p].shm_name), "/pg_cuvs_bld_lwtest_%d_%d", (int)getpid(), p);
        desc[p].n_vecs = half;
        shm_unlink(desc[p].shm_name);
        int fd = shm_open(desc[p].shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
        assert(fd >= 0 && ftruncate(fd, (off_t)len) == 0);
        char *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        assert(m != MAP_FAILED);
        memcpy(m, whole + (size_t)p * half * per_vec, (size_t)half * per_vec);
        memcpy(m + (size_t)half * per_vec,
               whole + (size_t)N * per_vec + (size_t)p * half * sizeof(uint64_t),
               (size_t)half * sizeof(uint64_t));
        munmap(m, len);
        close(fd);
    }
    free(whole);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(send(sv[0], desc, sizeof(desc), 0) == (ssize_t)sizeof(desc));
    memset(dir, 0, sizeof(dir));
    snprintf(dir, sizeof(dir), "%s/idx", tmpdir);
    g_peer_uid = getuid();
    handle_build_multi(sv[1], &cmd, dir);
    st = reply_status(sv[0]);
    close(sv[0]); close(sv[1]);
    for (int p = 0; p < 2; p++) shm_unlink(desc[p].shm_name);
    return st;
}

static int
run_build_sharded(uint32_t index_oid)
{
    int sv[2], st;
    size_t total = (size_t)N * DIM * sizeof(float) + (size_t)N * sizeof(uint64_t);
    char dir[256];
    CuvsCmdFrame cmd = build_cmd(index_oid, 2);
    char *mem = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mem != MAP_FAILED);
    fill_corpus(mem);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    snprintf(dir, sizeof(dir), "%s/idx", tmpdir);
    build_sharded(sv[1], &cmd, dir, (const float *)mem,
                  (const uint64_t *)(mem + (size_t)N * DIM * sizeof(float)), total, mem, 2);
    st = reply_status(sv[0]);
    close(sv[0]); close(sv[1]);
    return st;
}


static void check_refusal(void)
{
    CuvsAdmissionFields f;
    CuvsNoticeBatch notices;
    CuvsAdmissionCatalog cat = {0};
    char msg[1024], detail[1024], hint[1024];
    assert(last_reply.status == CUVS_STATUS_OOM_FALLBACK);
    assert(cuvs_admission_decode(last_reply.err_out, sizeof(last_reply.err_out), &f) == 0);
    assert(f.reason == CUVS_ADMISSION_PHYS && f.n == 32 && f.u == (raw_free >> 20));
    assert(!strcmp(cuvs_admission_format(&f, CUVS_ADMISSION_OBJ_CAGRA, NULL, &cat,
        msg,sizeof(msg),detail,sizeof(detail),hint,sizeof(hint)), "53200"));
    assert(strstr(msg,"deferred before cuVS") && strstr(hint,"Retry after other GPU users"));
    assert(cuvs_admission_reason_transient(f.reason));
    assert(cuvs_circuit_reason_is_transient(last_reply.status, f.reason));
    assert(cuvs_notice_decode(last_reply.notice_out,sizeof(last_reply.notice_out),&notices) == 0);
    assert(notices.count == 1 && strstr(notices.pairs[0].message,"physical memory pressure"));
    assert(cuvs_shim_lw_calls(CUVS_LW_OVERFLOW_MANAGED) == 0);
    assert(cuvs_shim_lw_calls(CUVS_LW_OVERFLOW_ERROR) == 0);
    assert(g_pending_rmm_build_vram[0] == 0 && g_lw_pending[0] == 0);
    assert(g_cache_evictions[0] == 0);
}

static void run_ivfpq(uint32_t idx)
{
    int sv[2]; char dir[256] = {0};
    CuvsCmdFrame cmd = build_cmd(idx,1);
    cmd.op = CUVS_OP_BUILD_IVFPQ; cmd.n_lists = 2; cmd.pq_bits = 4; cmd.pq_dim = 2;
    snprintf(cmd.shm_key,sizeof(cmd.shm_key),"/pg_cuvs_phys_%d",getpid());
    size_t size = N * (DIM * sizeof(float) + sizeof(uint64_t));
    int fd = shm_open(cmd.shm_key,O_RDWR|O_CREAT|O_EXCL,0600);
    assert(fd>=0 && ftruncate(fd,size)==0);
    char *p=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);assert(p!=MAP_FAILED);
    fill_corpus(p);munmap(p,size);close(fd);
    snprintf(dir,sizeof(dir),"%s/idx",tmpdir);
    assert(socketpair(AF_UNIX,SOCK_STREAM,0,sv)==0);
    assert(cuvs_fd_send(sv[0],-1,dir,sizeof(dir))==0);
    g_peer_uid=getuid();handle_build_ivfpq(sv[1],&cmd);reply_status(sv[0]);
    close(sv[0]);close(sv[1]);shm_unlink(cmd.shm_key);
}

/* A real op-23 client verifies SCM_RIGHTS and thread-local error/NOTICE
 * retention, including clearing the previous reply on the next request. */
typedef struct HnswCall { const char *sock; int fd; } HnswCall;
static void *hnsw_client(void *arg)
{
    HnswCall *c=arg;int result=-1;char error[128];CuvsHnswPolicy policy;
    CuvsHnswBudgetSnapshot budget;CuvsHnswAceReadback ace;
    int rc=cuvs_ipc_build_hnsw_file(c->sock,c->fd,N,DIM,CUVS_METRIC_L2,4,8,0,
        UINT64_C(1)<<30,CUVS_HNSW_ALGO_IVF_PQ,64,&policy,&budget,&ace,&result,error,sizeof(error));
    assert(rc==CUVS_STATUS_OOM_FALLBACK && result==-1);
    last_reply.status=rc;
    snprintf(last_reply.err_out,sizeof(last_reply.err_out),"%s",cuvs_ipc_last_err_out());
    snprintf(last_reply.notice_out,sizeof(last_reply.notice_out),"%s",cuvs_ipc_last_notice_out());
    check_refusal();
    rc=cuvs_ipc_build_hnsw_file(c->sock,c->fd,0,DIM,CUVS_METRIC_L2,4,8,0,
        UINT64_C(1)<<30,CUVS_HNSW_ALGO_IVF_PQ,64,&policy,&budget,&ace,&result,error,sizeof(error));
    assert(rc!=0 && !cuvs_ipc_last_err_out()[0] && !cuvs_ipc_last_notice_out()[0]);
    return NULL;
}
static void run_hnsw(void)
{
    char sock[108],file[300];snprintf(sock,sizeof(sock),"%s/socket",tmpdir);
    snprintf(file,sizeof(file),"%s/vectors",tmpdir);
    int fd=open(file,O_CREAT|O_RDWR|O_EXCL,0600);assert(fd>=0);
    assert(ftruncate(fd,N*DIM*sizeof(float))==0);
    struct sockaddr_un sa={.sun_family=AF_UNIX};snprintf(sa.sun_path,sizeof(sa.sun_path),"%s",sock);
    int listener=socket(AF_UNIX,SOCK_STREAM,0);
    assert(listener>=0 && bind(listener,(struct sockaddr*)&sa,sizeof(sa))==0 && listen(listener,1)==0);
    HnswCall c={sock,fd};pthread_t thread;
    cuvs_ipc_set_daemon_uid(getuid());
    assert(pthread_create(&thread,NULL,hnsw_client,&c)==0);
    int peer=accept(listener,NULL,NULL);CuvsCmdFrame cmd;
    assert(peer>=0 && recv_all(peer,&cmd,sizeof(cmd))==0);
    g_peer_uid=getuid();handle_build_hnsw_file(peer,&cmd);close(peer);
    assert(pthread_join(thread,NULL)==0);close(listener);close(fd);unlink(sock);unlink(file);
}
static void run_compact(void)
{
    IndexEntry *e=find_index(1,101);assert(e && e->handle);
    CuvsCagraIndex before=e->handle;uint64_t *before_tids=e->tids;
    char tomb[512],dir[256]={0};snprintf(dir,sizeof(dir),"%s/idx",tmpdir);
    snprintf(tomb,sizeof(tomb),"%s/1_101.tombstone",dir);
    CuvsTombstoneHeader h={.magic=CUVS_TOMBSTONE_MAGIC,.version=1,.n_entries=1};
    CuvsTombstoneRecord r={.tid=e->tids[0],.delete_xid=1};
    FILE *f=fopen(tomb,"wb");assert(f && fwrite(&h,sizeof(h),1,f)==1 && fwrite(&r,sizeof(r),1,f)==1);fclose(f);
    int sv[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,sv)==0);
    assert(cuvs_fd_send(sv[0],-1,dir,sizeof(dir))==0);
    CuvsCmdFrame cmd={.op=CUVS_OP_COMPACT,.db_oid=1,.index_oid=101};
    handle_compact(sv[1],&cmd);reply_status(sv[0]);close(sv[0]);close(sv[1]);
    check_refusal();
    assert(e->handle==before && e->tids==before_tids && e->n_vecs==N && !e->compact_count);
    f=fopen(tomb,"rb");assert(f);CuvsTombstoneHeader after;CuvsTombstoneRecord rec;
    assert(fread(&after,sizeof(after),1,f)==1 && fread(&rec,sizeof(rec),1,f)==1);
    assert(!memcmp(&h,&after,sizeof(h)) && !memcmp(&r,&rec,sizeof(r)));fclose(f);
}
static void check_status(void)
{
    int sv[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,sv)==0);
    handle_reclaim_status(sv[1]);CuvsReplyHeader reply;
    assert(recv_all(sv[0],&reply,sizeof(reply))==0 && reply.status==0);
    char buf[16384]={0};assert(reply.n_results<sizeof(buf));
    assert(recv_all(sv[0],buf,reply.n_results)==0);
    assert(strstr(buf,"\"build_physical_floor_bytes\":33554432"));
    assert(strstr(buf,"\"physical_build_guard\":[{\"device_id\":0,\"rejects\":6,\"last_reject_free_bytes\":0}]"));
    close(sv[0]);close(sv[1]);
}
int main(void)
{
    alarm(60);
    assert(cuvs_build_physical_refuse(0));
    assert(cuvs_build_physical_refuse(CUVS_BUILD_PHYSICAL_FLOOR_BYTES-1));
    assert(!cuvs_build_physical_refuse(CUVS_BUILD_PHYSICAL_FLOOR_BYTES));
    assert(!cuvs_build_physical_refuse(UINT64_MAX));
    snprintf(tmpdir,sizeof(tmpdir),"/tmp/physical-build-XXXXXX");assert(mkdtemp(tmpdir));
    snprintf(g_index_dir,sizeof(g_index_dir),"%s/idx",tmpdir);
    g_n_gpus=1;g_max_vram_per_gpu[0]=(size_t)64<<30;
    g_indexes=calloc(g_max_indexes,sizeof(IndexEntry));g_cold_indexes=calloc(g_max_indexes,sizeof(ColdIndexEntry));
    assert(g_indexes && g_cold_indexes);
    assert(g_cagra_build_overflow==CUVS_LW_OVERFLOW_MANAGED && g_managed_overcommit==1);
    assert(run_handle_build(101,1)==0); /* old index must survive refused replacement */
    IndexEntry *e=find_index(1,101);assert(e);CuvsCagraIndex old_handle=e->handle;
    raw_free=0;cuvs_shim_lw_reset();
    run_handle_build(101,1);check_refusal();assert(e->handle==old_handle);
    clear_build_notices();run_handle_build_multi(102,1);check_refusal();
    clear_build_notices();run_build_sharded(103);check_refusal();
    clear_build_notices();run_ivfpq(104);check_refusal();
    run_compact();run_hnsw();check_status();
    raw_free=CUVS_BUILD_PHYSICAL_FLOOR_BYTES;
    assert(run_handle_build(105,1)==0 && !last_reply.notice_out[0]);
    assert(cuvs_shim_lw_calls(CUVS_LW_OVERFLOW_MANAGED)==1);
    assert(g_physical_build_rejects[0]==6);
    puts("PASS raw-free boundary, six real handlers, retry 53200/NOTICE/status, old index/tombstone retention and next build");
    return 0;
}
