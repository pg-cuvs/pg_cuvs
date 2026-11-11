/* Actual daemon admission/reservation with only physical GPU free bytes faked.
 * No provider/network/allocator, registry mutex and guards remain production. */
#define main pg_cuvs_server_program_main
#define cuvs_vram_free_bytes_on fixture_physical_headroom
#include "../../src/pg_cuvs_server.c"
#undef cuvs_vram_free_bytes_on
#undef main
#include <assert.h>
#include <stdatomic.h>

static size_t physical[CUVS_MAX_GPUS];
size_t fixture_physical_headroom(int device) { return physical[device]; }
#define GiB ((size_t)1024 * 1024 * 1024)
static void reset(void) {
    cuvs_reclaim_params_default(&g_reclaim);
    memset(g_pending_build_vram, 0, sizeof(g_pending_build_vram));
    memset(g_pending_rmm_build_vram, 0, sizeof(g_pending_rmm_build_vram));
    memset(g_rmm_pool_limits, 0, sizeof(g_rmm_pool_limits));
    memset(g_max_vram_per_gpu, 0, sizeof(g_max_vram_per_gpu));
    memset(g_cache_evictions, 0, sizeof(g_cache_evictions));
    g_n_indexes = 0;
    for (int i=0;i<2;i++) {
        g_rmm_pool_limits[i]=40*GiB;g_max_vram_per_gpu[i]=72*GiB;physical[i]=40*GiB;
    }
}
static atomic_int admitted, refused, arrived, finish;
static void *competing_build(void *unused) {
    (void)unused;
    pthread_mutex_lock(&g_index_mutex);
    int ok=ensure_vram(24*GiB,0,CUVS_RQ_BUILD_CAGRA,NULL)==0;
    if (ok) { reserve_build_vram(24*GiB,0);atomic_fetch_add(&admitted,1); }
    else atomic_fetch_add(&refused,1);
    pthread_mutex_unlock(&g_index_mutex);
    atomic_fetch_add(&arrived,1);
    while (!atomic_load(&finish)) usleep(1000);
    if (ok) {
        pthread_mutex_lock(&g_index_mutex);
        release_build_vram(24*GiB,0);
        pthread_mutex_unlock(&g_index_mutex);
    }
    return NULL;
}
int main(void) {
    reset();pthread_t a,b;
    pthread_create(&a,NULL,competing_build,NULL);pthread_create(&b,NULL,competing_build,NULL);
    while(atomic_load(&arrived)<2) usleep(1000);
    assert(atomic_load(&admitted)==1 && atomic_load(&refused)==1);
    assert(g_pending_rmm_build_vram[0]==24*GiB && g_pending_build_vram[0]==24*GiB);
    assert(g_cache_evictions[0]==0);
    atomic_store(&finish,1);pthread_join(a,NULL);pthread_join(b,NULL);
    assert(g_pending_rmm_build_vram[0]==0 && g_pending_build_vram[0]==0);
    /* Releasing either successful or failed build opens the same next admission. */
    pthread_mutex_lock(&g_index_mutex);
    assert(ensure_vram(24*GiB,0,CUVS_RQ_BUILD_CAGRA,NULL)==0);reserve_build_vram(24*GiB,0);
    assert(gpu_free_vram_bytes(0)==16*GiB); /* shared BF/cache admission also sees claims */
    assert(ensure_vram(16*GiB,0,CUVS_RQ_BUILD_CAGRA,NULL)==0); /* exact sum boundary */
    assert(ensure_vram(16*GiB+1,0,CUVS_RQ_BUILD_CAGRA,NULL)<0);
    assert(ensure_vram(24*GiB,1,CUVS_RQ_BUILD_CAGRA,NULL)==0); /* distinct GPU */
    release_build_vram(24*GiB,0);
    /* Counter corruption and reservation already reflected by allocator close.
     * Conservative accounting may refuse but never evicts for pending pressure. */
    g_pending_rmm_build_vram[0]=41*GiB;assert(ensure_vram(1,0,CUVS_RQ_BUILD_CAGRA,NULL)<0);
    IndexEntry healthy = {.valid=1,.is_flat=1,.gpu_device_id=0,
                          .main_bf_vram_bytes=GiB};
    g_indexes=&healthy;g_n_indexes=1;
    g_pending_rmm_build_vram[0]=24*GiB;physical[0]=16*GiB;
    assert(ensure_vram(1,0,CUVS_RQ_BUILD_CAGRA,NULL)<0);assert(g_cache_evictions[0]==0 && g_n_indexes==1);
    g_pending_rmm_build_vram[0]=0;physical[0]=40*GiB;
    assert(ensure_vram(40*GiB,0,CUVS_RQ_BUILD_CAGRA,NULL)==0);assert(ensure_vram(40*GiB+1,0,CUVS_RQ_BUILD_CAGRA,NULL)<0);
    /* Disabled pool preserves the original logical residency admission. */
    g_n_indexes=0; /* isolate legacy budget from the later LRU control */
    g_rmm_pool_limits[0]=0;physical[0]=80*GiB;
    assert(ensure_vram(48*GiB,0,CUVS_RQ_BUILD_CAGRA,NULL)==0);reserve_build_vram(48*GiB,0);
    assert(g_pending_rmm_build_vram[0]==0 && g_pending_build_vram[0]==48*GiB);
    assert(ensure_vram(25*GiB,0,CUVS_RQ_BUILD_CAGRA,NULL)<0);release_build_vram(48*GiB,0);
    /* The actual LRU path remains active without pending pressure. A physical
     * free fixture stays zero, so eviction occurs but allocation still refuses. */
    g_n_indexes=1;
    g_rmm_pool_limits[0]=40*GiB;physical[0]=0;
    assert(ensure_vram(1,0,CUVS_RQ_LOAD,NULL)<0);
    assert(g_n_indexes==0 && g_cache_evictions[0]==1);
    g_indexes=NULL;
    pthread_mutex_unlock(&g_index_mutex);
    puts("PASS actual ensure_vram/pending reservation: concurrent40/24+24 rejection, exact boundary, success/failure release, device isolation, corrupt/allocated pending, zero-pool legacy, no eviction");
}
