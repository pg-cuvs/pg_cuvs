/*
 * pg_cuvs_server.c — GPU sidecar daemon for pg_cuvs.
 *
 * Standalone C program (NOT a PostgreSQL extension). Compiled separately
 * from the .so and linked against libcuvs + CUDA.
 *
 * Responsibilities:
 *   - Listen on UDS socket for SEARCH and BUILD commands
 *   - Manage CAGRA indexes in VRAM (LRU eviction)
 *   - Serialize indexes to disk on SIGTERM; reload on startup
 *   - Serve >=2 concurrent PG backends (pthread-per-connection)
 *
 * Usage:
 *   pg_cuvs_server --socket /tmp/.s.pg_cuvs.12345 \
 *                  --index-dir /var/lib/postgresql/16/main/cuvs_indexes \
 *                  --max-vram-mb 20480
 */

/* The Makefile builds with -D_POSIX_C_SOURCE=200809L, which hides glibc's
 * __USE_MISC extensions (e.g. MAP_ANONYMOUS, used by the ADR-059 multi-shard
 * fallback) and, on this glibc, also `struct ucred`/SO_PEERCRED (#87 finding
 * 1) — __USE_MISC alone (_DEFAULT_SOURCE) wasn't enough to expose those in
 * CI; _GNU_SOURCE is the strict superset that reliably does. Re-enable them;
 * additive alongside _POSIX_C_SOURCE. Must precede any system header. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "cuvs_ipc.h"
#include "cuvs_util.h"
#include "cuvs_cache_policy.h"
#include "cuvs_peak_meter.h"
#include <stdarg.h>
#include "cuvs_wrapper.h"
#include "cuvs_hnsw_build_common.h"
#include "cuvs_objstore.h"
#include "cuvs_build_corpus.h"   /* ADR-057: cuvs_fd_recv (SCM_RIGHTS build payload) */
#include "cuvs_lw_policy.h"      /* I4 / ADR-088: large_workspace overflow policy */
#include <curl/curl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Leveled logging (LOG_ERROR/WARN/INFO unconditional, LOG_DEBUG gated by
 * PG_CUVS_DEBUG) is provided by cuvs_util.h. Enable hot-path traces via
 * -DPG_CUVS_DEBUG=1 in the Makefile or env. */
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/random.h>   /* getrandom(2): CSPRNG suffix for reply shm key names, #87 */
#include <fcntl.h>
#include <time.h>
#include <stdint.h>
#include <poll.h>
#include <sys/statvfs.h>
#include <sys/file.h>

/* CSPRNG fill for reply-shm key suffixes (#87). getrandom(2) is Linux-only;
 * fall back to arc4random_buf() on macOS/BSD (never fails) so this file still
 * builds wherever `make` runs locally. Returns 0 on success, -1 if the CSPRNG
 * source itself failed (Linux only). #87 finding 2: callers must fail closed
 * on -1 (refuse to create the reply segment), not degrade to an all-zero,
 * predictable suffix. */
static int
shm_key_random(unsigned char *buf, size_t len)
{
#ifdef __linux__
    if (getrandom(buf, len, 0) != (ssize_t) len)
        return -1;
#else
    arc4random_buf(buf, len);
#endif
    return 0;
}

/* #87 finding 1: SO_PEERCRED owner verification.
 *
 * The daemon and the PG backend run as different uids, so the shm handoff
 * has to be 0644 (world-readable) for the daemon to read backend-created
 * segments at all (see the 0600->0644 fix above). On a non-sticky /dev/shm
 * host that reopens the CSPRNG-name/O_EXCL vuln from a different angle: an
 * attacker can readdir() /dev/shm, discover the (unguessable but not
 * secret-once-listed) segment name, unlink it, and recreate one under their
 * own uid with the same name — the daemon would then read attacker-chosen
 * bytes back as if they were the real backend's query/corpus/filter data.
 *
 * SO_PEERCRED gives the kernel-verified uid of whoever connect()ed this UDS
 * socket. Every PG backend and background worker is a postmaster fork, so
 * they all share postgres's uid — a segment the real backend created has
 * st_uid == g_peer_uid; an attacker-recreated one does not (different uid),
 * so the read is refused. Linux-only (SO_PEERCRED + struct ucred); this file
 * is daemon-only and Linux-only in production, so other platforms just skip
 * the check rather than fail closed. */
static __thread uid_t g_peer_uid = (uid_t) -1;

static int
shm_check_peer_owner(int fd, uid_t expected_uid)
{
#ifdef __linux__
    struct stat st;
    if (fstat(fd, &st) != 0)
        return 0;
    return st.st_uid == expected_uid;
#else
    (void) fd;
    (void) expected_uid;
    return 1;
#endif
}

/* ----------------------------------------------------------------
 * Index registry
 *
 * The registry is a soft LRU working-set cap, NOT a hard wall: up to
 * g_max_indexes indexes stay resident; beyond that the LRU is evicted to disk
 * (durable) and reloaded on demand by the search-miss path. Sized once at
 * startup from --max-indexes (default 1024, was a hard 64) — allocated with
 * calloc and never realloc'd, so &g_indexes[i] addresses and the compacting-shift
 * removal stay valid for the daemon's life.
 * ---------------------------------------------------------------- */
#define CUVS_DEFAULT_MAX_INDEXES 1024

/* Phase 3D: per-index warmup lifecycle state. */
typedef enum WarmupState {
    WARMUP_HOT          = 0,
    WARMUP_COLD         = 1,
    WARMUP_QUEUED       = 2,
    WARMUP_DOWNLOADING  = 3,
    WARMUP_LOADING      = 4,
    WARMUP_FAILED       = 5,
} WarmupState;

/* Phase 3F: one shard of a sharded logical CAGRA index. A shard is a standalone
 * CAGRA artifact resident on one GPU, covering the contiguous global TID range
 * [tid_offset, tid_offset + n_vecs). Shard-local item_ids map to global TIDs via
 * IndexEntry.tids[tid_offset + item_id]. */
typedef struct ShardEntry {
    uint32_t        shard_id;
    CuvsCagraIndex  handle;        /* cuVS opaque index for this shard */
    int64_t         tid_offset;    /* global TID start offset */
    int64_t         n_vecs;        /* vectors in this shard */
    uint32_t        gpu_device_id; /* CUDA device this shard lives on */
    size_t          vram_bytes;    /* estimated VRAM held by this shard */
    uint64_t        search_count;  /* OK searches dispatched to this shard */
    uint64_t        error_count;   /* failed shard searches */
    uint32_t        last_status;   /* CUVS_STATUS_* of the last shard search */
    int             valid;         /* 1 once the shard is built/loaded */

    /* Phase 3L: resident GPU brute-force index over this shard's vector range
     * [tid_offset, tid_offset + n_vecs). Lazily built on the first BF search;
     * NULL until then. Freed with the shard. */
    CuvsBfIndex     bf_idx;
    size_t          bf_vram_bytes; /* estimated VRAM held by bf_idx */
} ShardEntry;

typedef struct IndexEntry {
    uint32_t        db_oid;
    uint32_t        index_oid;
    uint32_t        dim;
    uint32_t        metric;
    int64_t         n_vecs;
    int             graph_degree;  /* CAGRA graph_degree for VRAM estimation; set at
                                    * build (cmd->graph_degree, 0 => default 64). Not
                                    * persisted: cold reload falls back to 64. */
    CuvsCagraIndex  handle;       /* cuVS opaque index (unsharded only; NULL if sharded) */
    uint64_t       *tids;         /* global TID array [n_vecs] (shared by all shards) */

    /* Phase 3F multi-GPU sharding. shard_count <= 1 => unsharded: use `handle`
     * and `gpu_device_id`. shard_count >= 2 => sharded: `handle` is NULL,
     * `shards`[shard_count] holds the per-GPU CAGRA artifacts, and
     * `gpu_device_id` is set to 0xFFFFFFFF (no single device) so LRU eviction
     * (which matches on exact device id) never partially evicts the index. */
    int             shard_count;  /* 0/1 = unsharded; >=2 = sharded */
    ShardEntry     *shards;       /* [shard_count] when sharded; NULL otherwise */
    int             inflight;     /* Phase 3G.4: in-flight lock-free sharded searches;
                                   * >0 blocks whole-unit eviction of this entry */
    size_t          vram_bytes;   /* estimated VRAM usage */
    time_t          last_search;  /* for LRU eviction; also stats last_search_at */
    int             valid;
    int             stale;        /* 1 if heap writes happened since build (REINDEX needed) */
    time_t          stale_since;  /* when first marked stale; 0 if fresh */

    /* ADR-073: this is a standalone `flat` index — GPU exact brute-force only,
     * NO CAGRA graph. handle == NULL and shard_count == 0 (breaks the old
     * "handle==NULL => sharded" assumption). Every search MUST take the
     * brute-force path (main_bf_idx over .vectors); the cagra `handle` is never
     * dereferenced. Set in handle_build_flat and load_index's flat branch; reset
     * to 0 by reset_entry_stats for every cagra/ivfpq/sharded (re)init. */
    int             is_flat;

    /* Phase 3B delta cache: resident GPU brute-force index over the pending
     * `.delta` vectors. Lazily (re)built in handle_search when the .delta file
     * appears/changes; freed on eviction. delta_idx==NULL means no GPU delta. */
    CuvsBfIndex     delta_idx;        /* resident brute-force index; NULL if none */
    uint64_t       *delta_tids;       /* host: delta item_id -> heap TID [n_delta] */
    int64_t         n_delta;          /* delta vectors in the cache */
    uint32_t        delta_generation; /* base .tids body_crc32 the cache was built on */
    int64_t         delta_mtime;      /* .delta st_mtime when the cache was built */
    size_t          delta_vram_bytes; /* estimated VRAM held by the delta cache */
    float          *delta_vecs_host; /* host copy for incremental rebuild (Phase 3A-4) */
    int64_t         delta_n_cached;  /* rows in current GPU BF + host buffer */

    /* Phase 3L: resident GPU brute-force index over the full base vector matrix
     * (the `.vectors` sidecar), used when a search request sets search_mode=BF.
     * Lazily (re)built in handle_search when `.vectors` appears/changes; freed
     * on eviction. main_bf_idx==NULL means no resident BF index. */
    CuvsBfIndex     main_bf_idx;        /* unsharded BF index; NULL if none */
    int64_t         main_bf_n;          /* vectors in the BF index */
    size_t          main_bf_vram_bytes; /* estimated VRAM held by main_bf_idx */
    int64_t         main_bf_mtime;      /* .vectors st_mtime when the BF was built */
    uint32_t        main_bf_generation; /* base .tids body_crc32 the BF was built on */
    uint32_t        bf_precision;       /* precision of the resident BF index(es):
                                         * 0=float32, 1=float16 (cuvs.bf_precision) */

    /* 3O pre-filter: TID-sorted reverse map for query-time BITSET construction.
     * rev_tids[i] is sorted ascending; rev_item_ids[i] is the corresponding item_id.
     * Built once at load time (O(n log n)); NULL until built or on malloc failure. */
    uint64_t       *rev_tids;           /* sorted TID values [n_vecs] */
    int32_t        *rev_item_ids;       /* item_id for rev_tids[i] [n_vecs] */

    /* Search stats (pg_stat_gpu_search). Reset on (re)build/load — they
     * describe the currently resident index instance, not a persisted total. */
    uint64_t        search_count;     /* CUVS_STATUS_OK searches */
    uint64_t        error_count;      /* attributable non-OK searches */
    uint64_t        total_latency_us; /* sum of OK latencies (for avg) */
    uint32_t        lat_buckets[CUVS_LAT_BUCKETS];
    uint32_t        last_status;       /* CUVS_STATUS_* of most recent search */
    uint32_t        last_requested_k;  /* top-k of last OK search (reflects cuvs.k) */
    uint32_t        last_returned_k;   /* rows last OK search returned */
    uint64_t        delta_merged_count; /* searches where daemon merged delta on GPU */
    uint64_t        bf_batch_count;     /* Phase 3L-9: coalesced BF batch dispatches */
    uint64_t        cagra_batch_count;  /* #160: coalesced CAGRA batch dispatches */
    uint64_t        prefilter_fallback_count; /* #133/ADR-083: 3O->gpu_bf_prefilter retries
                                                * after a short-fill (mean_returned<<k)
                                                * detection actually falls back (not merely
                                                * detects; see the handle_search comment) */
    char            last_error[128];
    WarmupState      warmup_state;     /* Phase 3D: WARMUP_HOT for resident indexes */
    /* Phase 3D: warmup observability — populated when an index is hydrated from
     * GCS by the warmup worker; stays 0 for locally-built indexes (never warmed). */
    time_t          last_warmup_at;
    uint32_t        warmup_duration_ms;
    uint64_t        download_count;
    uint64_t        cache_miss_count;
    uint32_t        gpu_device_id;    /* Phase 3E: which CUDA device this index lives on */

    /* Phase 3I-1: CPU HNSW fallback. Loaded lazily when use_cpu_hnsw=1. */
    CuvsHnswIndex   hnsw_idx;        /* NULL until first cpu_hnsw search request */
    uint32_t        last_search_mode; /* 0=gpu_cagra, 1=cpu_hnsw, 2=cpu_fallback, 3=gpu_bf, 4=cagra_prefilter, 5=ivfpq */

    /* 3P: IVF-PQ sidecar (mutually exclusive with CAGRA handle for same entry) */
    CuvsIvfPqIndex  ivfpq_handle;       /* NULL if not loaded */
    int64_t         ivfpq_n_vecs;       /* corpus size (mirrors n_vecs) */
    size_t          ivfpq_vram_bytes;   /* estimated VRAM for IVF-PQ PQ codes */
    /* 3Q: streaming update counters */
    int64_t         n_extended;         /* vectors added via EXTEND since last build/compact */
    /* 4C: compaction observability */
    uint64_t        compact_count;      /* cuvsCagraMerge compact ops since last build */
    time_t          last_compact_at;    /* epoch seconds of last compact; 0 if never */
    /* I3-B cache policy (v6 C1 EV-1/EV-2). `last_search` doubles as last_use:
     * 0 = never searched since registration (UC-1 3b). `inflight` is the
     * EV-1 (b) in-use count for every entry, not only sharded ones. */
    uint32_t        hits;               /* OK searches since registration */
    int             pinned;             /* daemon-lifetime pin; kept by REINDEX */
    time_t          loaded_at;          /* registration time (order key) */
    int             file_current;       /* I-FC: 1 iff graph/tids == .cagra/.tids */
} IndexEntry;

/* Zero just the stat counters of a (re)initialized entry. The slot may carry
 * stale stats from a previously evicted index, and a rebuild starts fresh. */
static void
reset_entry_stats(IndexEntry *e)
{
    e->search_count     = 0;
    e->error_count      = 0;
    e->total_latency_us = 0;
    memset(e->lat_buckets, 0, sizeof(e->lat_buckets));
    e->last_status      = 0;
    e->last_requested_k    = 0;
    e->last_returned_k    = 0;
    e->delta_merged_count = 0;
    e->bf_batch_count     = 0;
    e->cagra_batch_count  = 0;
    e->prefilter_fallback_count = 0;
    e->last_error[0]      = '\0';
    e->last_warmup_at     = 0;   /* Phase 3D warmup observability */
    e->warmup_duration_ms = 0;
    e->download_count     = 0;
    e->cache_miss_count   = 0;
    /* ADR-073: default to non-flat. Every cagra/ivfpq/sharded (re)init funnels
     * through reset_entry_stats, so this clears a reused slot's stale is_flat;
     * the flat build/load paths set is_flat = 1 AFTER calling reset_entry_stats. */
    e->is_flat            = 0;
    /* Default graph_degree to 0 (=> estimate_vram_bytes treats as cuVS default
     * 64), clearing a reused slot's stale value. CAGRA build paths set the real
     * cmd->graph_degree AFTER this call; flat/ivfpq/reload leave it 0. */
    e->graph_degree       = 0;
    /* I3-B: a fresh instance has no use history (UC-1 3b: last_use 0 is
     * eligible; order key falls back to loaded_at). `pinned` is deliberately
     * NOT cleared: REINDEX of the same OID keeps the pin (C7 7.2-N3). New
     * slots clear it in registry_new_slot(). file_current starts 0; callers
     * set 1 only after a successful save or load (I-FC). */
    e->hits               = 0;
    e->last_search        = 0;
    e->loaded_at          = time(NULL);
    e->file_current       = 0;
}

/* Record a completed search on an entry. Caller MUST hold g_index_mutex. */
static void
record_search_stat(IndexEntry *e, uint32_t status, uint32_t latency_us,
                   const char *err)
{
    e->last_status = status;
    e->last_search = time(NULL);
    if (status == CUVS_STATUS_OK)
    {
        e->search_count++;
        e->hits++;
        e->total_latency_us += latency_us;
        e->lat_buckets[cuvs_lat_bucket_index(latency_us)]++;
    }
    else
    {
        e->error_count++;
        if (err)
        {
            strncpy(e->last_error, err, sizeof(e->last_error) - 1);
            e->last_error[sizeof(e->last_error) - 1] = '\0';
        }
    }
}

static int         g_max_indexes = CUVS_DEFAULT_MAX_INDEXES;  /* registry capacity; --max-indexes */
static IndexEntry *g_indexes      = NULL;   /* calloc(g_max_indexes) at startup */
static int         g_n_indexes   = 0;
static pthread_mutex_t g_index_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Phase 3D: cold index registry — indexes known from .relfilenode sidecars but
 * not yet VRAM-resident. Protected by g_index_mutex. When warmup completes,
 * the entry is removed here and load_index() adds it to g_indexes. */
typedef struct ColdIndexEntry {
    uint32_t    db_oid;
    uint32_t    index_oid;
    uint32_t    relfilenode;
    uint32_t    table_oid;
    WarmupState warmup_state;
    time_t      last_warmup_at;
    uint32_t    warmup_duration_ms;
    uint64_t    download_count;
    uint64_t    cache_miss_count;
    uint32_t    gpu_device_id;
    int         valid;
} ColdIndexEntry;

static ColdIndexEntry *g_cold_indexes = NULL;   /* calloc(g_max_indexes) at startup */
static int            g_n_cold_indexes = 0;

/* Daemon-global VRAM cache counters (mutated under g_index_mutex). Exposed via
 * CUVS_OP_CACHE_STATS / pg_stat_gpu_cache. Per-index counters can't track this
 * because eviction destroys the IndexEntry. */
static uint64_t g_cache_hits[CUVS_MAX_GPUS];
static uint64_t g_cache_misses[CUVS_MAX_GPUS];
static uint64_t g_cache_evictions[CUVS_MAX_GPUS];
static uint64_t g_cache_reloads[CUVS_MAX_GPUS];
static uint64_t g_cache_persist_fail[CUVS_MAX_GPUS];

/* ADR-070 Bug #2: VRAM reserved by in-flight builds that have released
 * g_index_mutex for the (multi-minute) GPU build. The built index is not yet in
 * g_indexes, so total_vram_used() adds this so concurrent admission/eviction
 * still accounts for the build's VRAM. Mutated under g_index_mutex. */
static size_t   g_pending_build_vram[CUVS_MAX_GPUS];
static int      g_pending_build_count[CUVS_MAX_GPUS];   /* I3-B: solo flag */

/* Existing lock-free shard searches and HNSW exports may allocate GPU scratch
 * after dropping the registry lock. New HNSW admission drains these scopes.
 * The global count survives registry slot removal/replacement. Mutex protected. */
static unsigned g_unlocked_gpu_jobs;

/* Phase 3E: per-device GPU state. */
static CuvsGpuDeviceInfo g_gpus[CUVS_MAX_GPUS];
static int      g_n_gpus          = 0;
static int      g_allowed_gpus[CUVS_MAX_GPUS];
static int      g_n_allowed_gpus  = 0;   /* 0 = use all detected */
static size_t   g_max_vram_per_gpu[CUVS_MAX_GPUS];

/* ----------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------- */
static char   g_socket_path[256]  = "/tmp/.s.pg_cuvs.server";
static char   g_index_dir[256]    = "/tmp/pg_cuvs_indexes";
static size_t g_max_vram_bytes    = 0;   /* 0 = use default fraction; >0 = explicit --max-vram-mb */
#ifdef PGCUVS_CPU_SHIM
static uint32_t g_rmm_pool_mb     = 0;   /* CPU fixture has no GPU allocator */
#else
static uint32_t g_rmm_pool_mb     = 512; /* initial reservation per usable GPU */
#endif
static uint32_t g_rmm_pool_max_percent = 50;
/* C3 provisional coefficient; Q1-DERIVED.json supplies the calibrated
 * value at H_M. Keep this the only numeric default definition. */
#define CUVS_RMM_SCRATCH_PERMILLE_DEFAULT 7726
/* Provisional coefficient came from the I2 in-pool workspace measurements.
 * Q1 changes this binding together with the measured default coefficient. */
#define CUVS_RMM_SCRATCH_VALIDATED_OVERFLOW_POLICY CUVS_LW_OVERFLOW_MANAGED
static uint32_t g_scratch_permille = CUVS_RMM_SCRATCH_PERMILLE_DEFAULT;
/* ADR-087 (UC-5): build_algo=auto over the resident budget spills to ACE
 * (default) or fails with status 13. Startup-only; explicit ivf_pq/ace
 * never read it. */
static uint32_t g_hnsw_auto_overflow = CUVS_HNSW_AUTO_OVERFLOW_DEFAULT;
/* I3-D (UC-6 A): pooled RAFT resources own a dedicated RMM stream (on) or use
 * the per-thread default stream (off = I2 behaviour). Startup-only. */
static int g_rmm_dedicated_stream = CUVS_RMM_DEDICATED_STREAM_DEFAULT;
static size_t g_rmm_pool_limits[CUVS_MAX_GPUS];
/* Estimated quota reservations for unlocked builds, separate from logical
 * residency. Protected by g_index_mutex; allocator counters cannot see these
 * until the build allocates. Kept until return, conservatively even then. */
static size_t g_pending_rmm_build_vram[CUVS_MAX_GPUS];

/* I4 / ADR-088 knobs (KB-4 --cagra-build-overflow, KB-5 --managed-overcommit,
 * KB-6 --managed-lw-margin-permille, KB-24 --oom-score-adj). Startup flags
 * only: pool accounting reads them (UC-7), so no runtime change path. */
static uint32_t g_cagra_build_overflow       = CUVS_CAGRA_BUILD_OVERFLOW_DEFAULT;
static int      g_managed_overcommit         = CUVS_MANAGED_OVERCOMMIT_DEFAULT;
static uint32_t g_managed_lw_margin_permille = CUVS_MANAGED_LW_MARGIN_PERMILLE_DEFAULT;
static int      g_oom_score_adj              = CUVS_OOM_SCORE_ADJ_DEFAULT;
/* ADR-089 --rmm-build-headroom-permille: AR8 headroom over pool_need.
 * Startup flag only, like the I4 knobs above. */
static uint32_t g_rmm_build_headroom_permille = CUVS_RMM_BUILD_HEADROOM_PERMILLE_DEFAULT;
static int      g_oom_score_adj_effective    = -1;   /* -1 = not read */
static char     g_oom_score_adj_result[CUVS_LW_OOM_RESULT_LEN] = "oom_score_adj not evaluated";
/* Per GPU, under g_index_mutex: sum of lw_ub over admitted managed builds
 * (added at admission, removed at release), and spill counters (C8). */
static uint64_t g_lw_pending[CUVS_MAX_GPUS];
static uint64_t g_managed_spill_builds[CUVS_MAX_GPUS];
static uint64_t g_managed_spill_bytes_max[CUVS_MAX_GPUS];
/* Request-local victim notices; background reclaim never queues them. */
static __thread char t_evict_notices[CUVS_NOTICE_OUT_BYTES];
static void record_eviction_notice(uint32_t db, uint32_t idx, int64_t idle,
                                   int64_t protect, CuvsEvictReason why);


/* ADR-065: default per-device budget when --max-vram-mb is omitted — a fraction
 * of total VRAM, leaving headroom for the untracked cuVS workspace + CUDA
 * context so an unset budget cannot OOM the device. */
#define CUVS_DEFAULT_VRAM_FRACTION 0.90
static int    g_server_fd         = -1;
static volatile int g_shutdown    = 0;
static CuvsReclaimParams g_reclaim;  /* I3-B knobs; defined with the policy state */
/* index_oid of the request now being admitted ([evict] requester_idx; 0 for
 * reclaim and manual unload). Set under g_index_mutex by each requester. */
static uint32_t g_cur_requester_idx;
/* Provisional reason of the last ensure_vram reject (I3-A replaces this with
 * its AR classifier; the reclaim F log only needs AR0/AR7 vs the rest). */
static int      g_last_admit_reason = CUVS_ADMISSION_NONE;
static char   g_snapshot_uri[512] = ""; /* "gs://bucket[/prefix]"; empty = disabled */
static char   g_cluster_id[128]   = ""; /* multi-node identifier for GCS path */
static char   g_gcs_key_file[512] = ""; /* service account JSON; empty = instance metadata */

/* ----------------------------------------------------------------
 * Phase 3D: background warmup thread pool + queue
 * ---------------------------------------------------------------- */
typedef struct WarmupJob {
    uint32_t db_oid;
    uint32_t index_oid;
    uint32_t relfilenode;
    uint32_t table_oid;
    int      from_cache_miss;
} WarmupJob;

#define WARMUP_QUEUE_MAX 64

static WarmupJob       g_warmup_queue[WARMUP_QUEUE_MAX];
static int             g_warmup_head  = 0;
static int             g_warmup_tail  = 0;
static int             g_warmup_count = 0;
static pthread_mutex_t g_warmup_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_warmup_cond  = PTHREAD_COND_INITIALIZER;
static int             g_warmup_nthreads = 2;
static pthread_t       g_warmup_tids[8];

/* ----------------------------------------------------------------
 * Phase 3L-9 / #160: single-query micro-batch worker (single consumer).
 *
 * When a connection thread runs an unsharded search with a batch window set for
 * its path — cmd->bf_batch_wait_us for brute_force (3L-9), cmd->
 * cagra_batch_wait_us for cagra (#160) — it enqueues a request here (instead of
 * dispatching immediately) and blocks on the request's `done` flag. One
 * dedicated worker thread coalesces queued requests that share a
 * (db,index,precision,dim,mode) key into a single batched GPU dispatch —
 * cuvs_bf_search_batch or cuvs_cagra_search_batch — then wakes each producer.
 * The `mode` component of the key is what keeps the two algorithms in separate
 * dispatches while sharing one queue and one worker.
 *
 * Lock order (no thread ever holds both): a producer takes g_index_mutex for
 * the cheap stale/metric/dim preamble, RELEASES it, then takes g_batch_mtx to
 * enqueue; the worker moves the queue out under g_batch_mtx, RELEASES it, then
 * takes g_index_mutex for the GPU work. Gated entirely by a window > 0 — with
 * both defaults at 0 nothing is ever enqueued and this subsystem is inert.
 * ---------------------------------------------------------------- */
#define CUVS_BATCH_MAX 256          /* cap on concurrently queued requests */

typedef struct CuvsBatchRequest {
    CuvsBatchKey key;          /* db_oid, index_oid, precision, dim, mode */
    const float *query;        /* producer-owned, dim floats, valid until done */
    int          k;            /* requested top-k */
    uint32_t     wait_us;      /* this request's batch window hint */
    CuvsResult  *out;          /* producer-owned [k]; worker writes n_out results */
    int          n_out;        /* results written by the worker */
    int          status;       /* CUVS_STATUS_* set by the worker */
    int          delta_merged; /* #160: 1 if the worker folded the .delta in */
    uint32_t     latency_us;   /* #160: dispatch wall-clock, for the reply header */
    int          done;         /* 0 = pending, 1 = worker finished this request */
} CuvsBatchRequest;

static pthread_mutex_t   g_batch_mtx       = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    g_batch_cond      = PTHREAD_COND_INITIALIZER; /* signaled on enqueue */
static pthread_cond_t    g_batch_done_cond = PTHREAD_COND_INITIALIZER; /* signaled on completion */
static CuvsBatchRequest *g_batch_queue[CUVS_BATCH_MAX];
static int               g_batch_queue_n   = 0;
static pthread_t         g_batch_worker_tid;
static int               g_batch_worker_started = 0;

static int
warmup_enqueue(uint32_t db_oid, uint32_t index_oid,
               uint32_t relfilenode, uint32_t table_oid,
               int from_cache_miss)
{
    pthread_mutex_lock(&g_warmup_mutex);

    /* Duplicate check: skip if already queued or downloading. */
    for (int i = 0; i < g_warmup_count; i++)
    {
        int idx = (g_warmup_head + i) % WARMUP_QUEUE_MAX;
        if (g_warmup_queue[idx].db_oid == db_oid &&
            g_warmup_queue[idx].index_oid == index_oid)
        {
            pthread_mutex_unlock(&g_warmup_mutex);
            return -1;
        }
    }

    if (g_warmup_count >= WARMUP_QUEUE_MAX)
    {
        pthread_mutex_unlock(&g_warmup_mutex);
        return -1;
    }

    WarmupJob *j = &g_warmup_queue[g_warmup_tail];
    j->db_oid         = db_oid;
    j->index_oid      = index_oid;
    j->relfilenode    = relfilenode;
    j->table_oid      = table_oid;
    j->from_cache_miss = from_cache_miss;
    g_warmup_tail = (g_warmup_tail + 1) % WARMUP_QUEUE_MAX;
    g_warmup_count++;

    pthread_cond_signal(&g_warmup_cond);
    pthread_mutex_unlock(&g_warmup_mutex);
    return 0;
}

static int
warmup_dequeue(WarmupJob *out)
{
    pthread_mutex_lock(&g_warmup_mutex);
    while (g_warmup_count == 0 && !g_shutdown)
        pthread_cond_wait(&g_warmup_cond, &g_warmup_mutex);

    if (g_shutdown)
    {
        pthread_mutex_unlock(&g_warmup_mutex);
        return -1;
    }

    *out = g_warmup_queue[g_warmup_head];
    g_warmup_head = (g_warmup_head + 1) % WARMUP_QUEUE_MAX;
    g_warmup_count--;
    pthread_mutex_unlock(&g_warmup_mutex);
    return 0;
}

/* Forward declarations for warmup worker. */
static int  load_index(uint32_t db_oid, uint32_t index_oid, CuvsRequester rq);
static void build_rev_tid_map(IndexEntry *e);  /* 3O: TID→item_id reverse map */

static void *
warmup_worker_thread(void *arg)
{
    (void)arg;
    while (!g_shutdown)
    {
        WarmupJob job;
        if (warmup_dequeue(&job) != 0)
            break;

        LOG_INFO("warmup: downloading %u/%u from GCS\n",
                 job.db_oid, job.index_oid);

        /* Update cold entry state -> DOWNLOADING (under g_index_mutex). */
        pthread_mutex_lock(&g_index_mutex);
        for (int i = 0; i < g_n_cold_indexes; i++)
        {
            ColdIndexEntry *ce = &g_cold_indexes[i];
            if (ce->valid && ce->db_oid == job.db_oid &&
                ce->index_oid == job.index_oid)
            {
                ce->warmup_state = WARMUP_DOWNLOADING;
                break;
            }
        }
        pthread_mutex_unlock(&g_index_mutex);

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        int rc = cuvs_objstore_download(
            g_snapshot_uri, g_cluster_id, g_gcs_key_file,
            g_index_dir, job.db_oid, job.index_oid,
            job.relfilenode, NULL);

        if (rc != 0)
        {
            LOG_WARN("warmup: GCS download failed for %u/%u\n",
                     job.db_oid, job.index_oid);
            pthread_mutex_lock(&g_index_mutex);
            for (int i = 0; i < g_n_cold_indexes; i++)
            {
                ColdIndexEntry *ce = &g_cold_indexes[i];
                if (ce->valid && ce->db_oid == job.db_oid &&
                    ce->index_oid == job.index_oid)
                {
                    ce->warmup_state = WARMUP_FAILED;
                    break;
                }
            }
            pthread_mutex_unlock(&g_index_mutex);
            continue;
        }

        /* Download succeeded — load into VRAM. */
        pthread_mutex_lock(&g_index_mutex);

        /* Mark LOADING. */
        ColdIndexEntry *target_ce = NULL;
        for (int i = 0; i < g_n_cold_indexes; i++)
        {
            ColdIndexEntry *ce = &g_cold_indexes[i];
            if (ce->valid && ce->db_oid == job.db_oid &&
                ce->index_oid == job.index_oid)
            {
                ce->warmup_state = WARMUP_LOADING;
                target_ce = ce;
                break;
            }
        }

        if (load_index(job.db_oid, job.index_oid, CUVS_RQ_LOAD_WARMUP) != 0)
        {
            LOG_WARN("warmup: VRAM load failed for %u/%u\n",
                     job.db_oid, job.index_oid);
            if (target_ce)
                target_ce->warmup_state = WARMUP_FAILED;
            pthread_mutex_unlock(&g_index_mutex);
            continue;
        }

        clock_gettime(CLOCK_MONOTONIC, &t1);
        uint32_t dur_ms = (uint32_t)((t1.tv_sec - t0.tv_sec) * 1000 +
                                     (t1.tv_nsec - t0.tv_nsec) / 1000000);

        /* Propagate warmup stats to the hot entry. */
        IndexEntry *he = NULL;
        for (int i = 0; i < g_n_indexes; i++)
        {
            if (g_indexes[i].valid && g_indexes[i].db_oid == job.db_oid &&
                g_indexes[i].index_oid == job.index_oid)
            {
                he = &g_indexes[i];
                break;
            }
        }
        if (he)
        {
            he->warmup_state       = WARMUP_HOT;
            he->last_warmup_at     = time(NULL);
            he->warmup_duration_ms = dur_ms;
            he->download_count     = (target_ce ? target_ce->download_count : 0) + 1;
            he->cache_miss_count   = (target_ce ? target_ce->cache_miss_count : 0);
        }

        /* Remove from cold registry (shift down). */
        if (target_ce)
        {
            int ci = (int)(target_ce - g_cold_indexes);
            for (int i = ci; i < g_n_cold_indexes - 1; i++)
                g_cold_indexes[i] = g_cold_indexes[i + 1];
            g_n_cold_indexes--;
        }

        pthread_mutex_unlock(&g_index_mutex);

        /* Clean up stale delta/tombstone sidecars from a previous generation. */
        {
            char path[512];
            snprintf(path, sizeof(path), "%s/%u_%u.delta",
                     g_index_dir, job.db_oid, job.index_oid);
            unlink(path);
            snprintf(path, sizeof(path), "%s/%u_%u.tombstone",
                     g_index_dir, job.db_oid, job.index_oid);
            unlink(path);
            snprintf(path, sizeof(path), "%s/%u_%u.stale",
                     g_index_dir, job.db_oid, job.index_oid);
            unlink(path);
        }

        LOG_INFO("warmup: %u/%u loaded into VRAM in %u ms\n",
                 job.db_oid, job.index_oid, dur_ms);
    }
    return NULL;
}

/* ----------------------------------------------------------------
 * VRAM helpers
 * ---------------------------------------------------------------- */
static size_t
estimate_vram_bytes(int64_t n_vecs, int dim, int graph_degree)
{
    /* CAGRA graph: graph_degree edges × 4 bytes (uint32 adjacency) per node,
     * plus float vectors. graph_degree <= 0 means "cuVS default" (64) — mirrors
     * the >0 ? : 64 mapping in cuvs_wrapper.cu and the reloption default in
     * pg_cuvs.c. Reload/extend/compact paths that lack the per-index degree pass
     * 64 (the default); a non-default-degree index reloaded after a daemon
     * restart is estimated at 64 (graph_degree is not persisted in .tids/.shards). */
    /* I3 (C3, CK-30): the shared estimate rounds dim up to the 4-float stride
     * the wrapper allocates; equal to n*(dim*4 + gd*4) when dim % 4 == 0. */
    return (size_t) cuvs_admission_estimate_final_bytes(n_vecs, dim, graph_degree);
}

/* C3 admission value N = final + ceil(final x rho / 1000). Use it for
 * admission, reservation, release and GPU choice; residency uses final. */
static size_t
admission_needed_bytes(size_t final)
{
    return (size_t) cuvs_admission_needed_bytes(final, 0, g_scratch_permille, 0);
}

static size_t
total_vram_used(int device_id)
{
    size_t total = 0;
    for (int i = 0; i < g_n_indexes; i++)
    {
        IndexEntry *e = &g_indexes[i];
        if (!e->valid)
            continue;
        if (e->shard_count >= 2)
        {
            /* Sharded: sum only the shards resident on this device. Include the
             * per-shard resident BF index (Phase 3L): it is freed on eviction
             * (free_index_shards) so it must be accounted, else eviction
             * over-commits VRAM. */
            for (int s = 0; s < e->shard_count; s++)
                if (e->shards[s].valid &&
                    e->shards[s].gpu_device_id == (uint32_t)device_id)
                    total += e->shards[s].vram_bytes
                           + e->shards[s].bf_vram_bytes;
        }
        else if (e->gpu_device_id == (uint32_t)device_id)
        {
            /* Include the resident main BF index (Phase 3L). NOT ivfpq_vram_bytes:
             * an IVF-PQ entry sets both vram_bytes and ivfpq_vram_bytes to the
             * same `needed`, so counting the latter would double-count. */
            total += e->vram_bytes + e->delta_vram_bytes + e->main_bf_vram_bytes;
        }
    }
    /* ADR-070 Bug #2: include VRAM reserved by in-flight builds on this device
     * (released the mutex for the GPU build; not yet in g_indexes). */
    if (device_id >= 0 && device_id < CUVS_MAX_GPUS)
        total += g_pending_build_vram[device_id];
    return total;
}

/* All admission callers hold g_index_mutex. Include unallocated build claims
 * for cache/transient-BF admissions as well as ensure_vram(). The wrapper's
 * physical/pool counters alone cannot account for these reservations. */
static size_t
gpu_free_vram_bytes(int device_id)
{
    size_t available = cuvs_vram_free_bytes_on(device_id);
    size_t pending = g_rmm_pool_limits[device_id] > 0
        ? g_pending_rmm_build_vram[device_id] : 0;
    return pending < available ? available - pending : 0;
}

/* Phase 3E: return the allowed GPU index (in g_allowed_gpus) for a device_id,
 * or the device_id itself if no restriction is configured. */
static int
is_gpu_allowed(int device_id)
{
    if (g_n_allowed_gpus == 0)
        return 1;
    for (int i = 0; i < g_n_allowed_gpus; i++)
        if (g_allowed_gpus[i] == device_id)
            return 1;
    return 0;
}

static int
n_usable_gpus(void)
{
    return (g_n_allowed_gpus > 0) ? g_n_allowed_gpus : g_n_gpus;
}

static int
usable_gpu(int i)
{
    return (g_n_allowed_gpus > 0) ? g_allowed_gpus[i] : i;
}

/* ----------------------------------------------------------------
 * I3 admission (v6 r18 C3, C5, ADR-086): rejection reasons, err_out fields
 * and the [admit] / [release] / [build_oom] log lines. All helpers below run
 * with g_index_mutex held, like the admission callers.
 * ---------------------------------------------------------------- */
static uint64_t
admission_mono_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t) t.tv_sec * 1000000u + (uint64_t) t.tv_nsec / 1000u;
}

/* Bytes an entry holds on `dev`, in total_vram_used() terms. */
static size_t
admission_entry_bytes_on(const IndexEntry *e, int dev)
{
    size_t total = 0;
    if (!e->valid)
        return 0;
    if (e->shard_count >= 2)
    {
        for (int s = 0; s < e->shard_count; s++)
            if (e->shards[s].valid && e->shards[s].gpu_device_id == (uint32_t) dev)
                total += e->shards[s].vram_bytes + e->shards[s].bf_vram_bytes;
    }
    else if (e->gpu_device_id == (uint32_t) dev)
        total = e->vram_bytes + e->delta_vram_bytes + e->main_bf_vram_bytes;
    return total;
}

static size_t
admission_same_resident(uint32_t db, uint32_t idx, int dev)
{
    for (int i = 0; i < g_n_indexes; i++)
        if (g_indexes[i].db_oid == db && g_indexes[i].index_oid == idx)
            return admission_entry_bytes_on(&g_indexes[i], dev);
    return 0;
}

/* Resident total and count on `dev`, without in-flight build reservations. */
static void
admission_resident(int dev, size_t *bytes, uint32_t *count)
{
    *bytes = 0;
    *count = 0;
    for (int i = 0; i < g_n_indexes; i++)
    {
        size_t b = admission_entry_bytes_on(&g_indexes[i], dev);
        if (b > 0)
        {
            *bytes += b;
            (*count)++;
        }
    }
}

/* Single-build limit on `dev`: min(logical budget, pool quota), 0 = none. */
static size_t
admission_limit(int dev)
{
    size_t budget = g_max_vram_per_gpu[dev];
    size_t quota = g_rmm_pool_limits[dev];
    if (quota > 0 && (budget == 0 || quota < budget))
        return quota;
    return budget;
}

typedef struct AdmissionCacheTotals {
    uint64_t lru, eligible, pinned, busy, reclaimable;
    uint32_t lru_count, pinned_count, busy_count;
} AdmissionCacheTotals;
static void admission_cache_totals(int dev, AdmissionCacheTotals *out);

/* Resident candidates: oldest first, excluding pinned and in-use entries. */
static void
admission_candidates(int dev, CuvsAdmissionFields *f)
{
    int    picked[CUVS_ADMISSION_MAX_CANDIDATES];
    time_t now = time(NULL);

    f->n_cand = 0;
    while (f->n_cand < CUVS_ADMISSION_MAX_CANDIDATES)
    {
        int best = -1;
        for (int i = 0; i < g_n_indexes; i++)
        {
            const IndexEntry *e = &g_indexes[i];
            int dup = 0;
            if (admission_entry_bytes_on(e, dev) == 0 ||
                e->pinned || e->inflight > 0)
                continue;
            for (uint32_t k = 0; k < f->n_cand; k++)
                dup |= picked[k] == i;
            if (!dup && (best < 0 || e->last_search < g_indexes[best].last_search))
                best = i;
        }
        if (best < 0)
            break;
        {
            const IndexEntry *e = &g_indexes[best];
            CuvsAdmissionCandidate *c = &f->cand[f->n_cand];
            c->db_oid = e->db_oid;
            c->index_oid = e->index_oid;
            c->mib = cuvs_admission_mib(admission_entry_bytes_on(e, dev));
            if (e->last_search == 0)
                c->idle_s = CUVS_ADMISSION_IDLE_NEVER;
            else
            {
                double idle = difftime(now, e->last_search);
                c->idle_s = idle <= 0 ? 0
                          : idle >= (double) CUVS_ADMISSION_SCALAR_MAX
                            ? (int32_t) CUVS_ADMISSION_SCALAR_MAX : (int32_t) idle;
            }
            picked[f->n_cand++] = best;
        }
    }
}

/* Fill the C5 per-reason fields from the current B cache policy. */
static void
admission_fill(CuvsAdmissionFields *f, CuvsAdmissionReason reason, size_t needed,
               int dev)
{
    size_t   res_bytes = 0;
    uint32_t res_n = 0;

    memset(f, 0, sizeof(*f));
    f->reason = reason;
    f->n = cuvs_admission_mib(needed);
    f->present = CUVS_ADM_F_N;
    switch (reason)
    {
        case CUVS_ADMISSION_AR0:
        {
            size_t limit = 0;
            if (dev >= 0)
                limit = admission_limit(dev);
            else
                for (int i = 0; i < n_usable_gpus(); i++)
                {
                    size_t l = admission_limit(usable_gpu(i));
                    if (l > limit)
                        limit = l;
                }
            f->q = cuvs_admission_mib(limit);
            f->present |= CUVS_ADM_F_Q;
            break;
        }
        case CUVS_ADMISSION_AR1:
        case CUVS_ADMISSION_AR2:
        {
            size_t quota = g_rmm_pool_limits[dev] > 0 ? g_rmm_pool_limits[dev]
                                                      : g_max_vram_per_gpu[dev];
            f->q = cuvs_admission_mib(quota);
            f->p = cuvs_admission_mib(g_pending_rmm_build_vram[dev]);
            f->f = cuvs_admission_mib(gpu_free_vram_bytes(dev));
            f->present |= CUVS_ADM_F_Q | CUVS_ADM_F_P | CUVS_ADM_F_F;
            break;
        }
        case CUVS_ADMISSION_AR3:
        case CUVS_ADMISSION_AR4:
        case CUVS_ADMISSION_AR5:
            admission_resident(dev, &res_bytes, &res_n);
            f->f = cuvs_admission_mib(gpu_free_vram_bytes(dev));
            f->h_mib = cuvs_admission_mib(res_bytes);
            f->h_cnt = cuvs_admission_count(res_n);
            f->present |= CUVS_ADM_F_F | CUVS_ADM_F_H | CUVS_ADM_F_E | CUVS_ADM_F_K |
                          CUVS_ADM_F_R;
            {
                AdmissionCacheTotals totals;
                uint64_t budget = admission_limit(dev);
                uint64_t cap = budget / 100 * g_reclaim.f_cap_pct +
                    (budget % 100) * g_reclaim.f_cap_pct / 100;
                uint64_t floor = budget / 100 * g_reclaim.floor_pct +
                    (budget % 100) * g_reclaim.floor_pct / 100;
                admission_cache_totals(dev, &totals);
                f->e_mib = cuvs_admission_mib(totals.lru);
                f->e_cnt = cuvs_admission_count(totals.lru_count);
                f->k_mib = cuvs_admission_mib(totals.pinned);
                f->k_cnt = cuvs_admission_count(totals.pinned_count);
                f->r = cuvs_reclaim_wait_hint(g_reclaim.mode,
                    gpu_free_vram_bytes(dev), totals.reclaimable, needed,
                    cap, res_bytes, floor);
                if (reason == CUVS_ADMISSION_AR5)
                {
                    f->present &= ~CUVS_ADM_F_H;
                    f->present |= CUVS_ADM_F_S;
                }
            }
            admission_candidates(dev, f);
            break;
        case CUVS_ADMISSION_OOM:
            admission_resident(dev, &res_bytes, &res_n);
            f->f = cuvs_admission_mib(gpu_free_vram_bytes(dev));
            f->h_mib = cuvs_admission_mib(res_bytes);
            f->h_cnt = cuvs_admission_count(res_n);
            f->present |= CUVS_ADM_F_F | CUVS_ADM_F_H;
            break;
        default:   /* ATT and the I3-B/I4 reasons carry n only here */
            break;
    }
}

static void
admission_workspace_fields(CuvsAdmissionFields *f,
                           const CuvsAdmissionWorkspaceInput *ws)
{
    if (f->reason == CUVS_ADMISSION_AR6 || f->reason == CUVS_ADMISSION_AR7)
    {
        char raw[CUVS_ADMISSION_ERR_OUT_BYTES];
        CuvsAdmissionFields decoded;
        int n = cuvs_lw_reject_fields(ws, f->reason, raw, sizeof(raw));
        if (n >= 0 && (size_t)n < sizeof(raw) &&
            cuvs_admission_decode(raw, (size_t)n + 1, &decoded) == 0)
            *f = decoded;
    }
}

/* Last search-path reload rejection on this thread (reason NONE when the
 * last load_index failed for another cause or succeeded). */
static __thread CuvsAdmissionFields t_reload_rej;
/* LOAD shares the cache policy for search and manual operations, but C8
 * emits successful admission lines only for the explicit manual operation. */
static __thread int t_manual_loading;
static const char *
admission_load_path(CuvsRequester rq)
{
    if (rq == CUVS_RQ_LOAD_STARTUP) return "startup";
    if (rq == CUVS_RQ_LOAD_WARMUP) return "warmup";
    return t_manual_loading ? "load" : "reload";
}

typedef struct AdmitLine {
    const char *op, *path, *algo;
    const char *shape;           /* cuvs_build_shape_class(); NULL = na */
    int         gpu;
    uint32_t    db, idx, dim, gd, igd;
    int64_t     n;
    size_t      final, needed;
    size_t      pending_before;
    uint64_t    evictions_before, self_res;
    uint64_t    used_before, build_started_us, build_finished_us;
    AdmissionCacheTotals cache;
    int         cache_valid;
    CuvsAdmissionWorkspaceInput workspace;
    int         workspace_valid;
    /* ADR-089 AR8 inputs, set by ensure_vram for a CAGRA build. */
    int         pool_valid;
    uint64_t    pool_need, pool_headroom, pool_pending;
} AdmitLine;

/* AR8 err_out subset: n = pool_need (not N_m), q = pool quota, p = pending,
 * d = headroom (ADR-089). Other reasons are left as filled. */
static void
admission_pool_fields(CuvsAdmissionFields *f, const AdmitLine *a, int dev)
{
    if (f->reason != CUVS_ADMISSION_AR8 || !a->pool_valid)
        return;
    f->n = cuvs_admission_mib(a->pool_need);
    f->q = cuvs_admission_mib(g_rmm_pool_limits[dev]);
    f->p = cuvs_admission_mib(a->pool_pending);
    f->d = cuvs_admission_mib(a->pool_headroom);
    f->present = CUVS_ADM_F_N | CUVS_ADM_F_Q | CUVS_ADM_F_P | CUVS_ADM_F_D;
}

/* ---- I3-E observation (C8): rejection counters and in-flight builds ----
 * Every variable here is read and written under g_index_mutex. Counters are
 * cumulative and never reset by a read (C8); only a daemon restart clears
 * them. */
static uint64_t g_rejects[CUVS_MAX_GPUS][CUVS_I3E_N_REJECT_REASONS];
static uint64_t g_rejects_no_gpu[CUVS_I3E_N_REJECT_REASONS];
static int      g_last_reject_reason1[CUVS_MAX_GPUS];   /* reason + 1; 0 = none */
static time_t   g_last_reject_at[CUVS_MAX_GPUS];
static CuvsBuildStatusRow g_builds[CUVS_MAX_ACTIVE_BUILDS];
static int      g_builds_used[CUVS_MAX_ACTIVE_BUILDS];
static uint64_t g_builds_unlisted;

/* One count per emitted [admit] rejection (log_admit), never in
 * admission_fill, which also formats replies and rechecks. */
static void
observe_reject(int dev, CuvsAdmissionReason reason)
{
    if ((int)reason < (int)CUVS_ADMISSION_AR0 || (int)reason > (int)CUVS_ADMISSION_AR7)
        return;
    if (dev < 0 || dev >= CUVS_MAX_GPUS)
    {
        g_rejects_no_gpu[reason]++;
        return;
    }
    g_rejects[dev][reason]++;
    g_last_reject_reason1[dev] = (int)reason + 1;
    g_last_reject_at[dev] = time(NULL);
}

/* Registers an admitted unlocked CAGRA build in the critical section that
 * reserves its N (reserve_build_vram). The row copies the AdmitLine, so it
 * outlives the builder's stack frame. Returns the slot, or -1 when the table
 * is full (the build runs unlisted and builds_unlisted counts it). */
static int
build_status_register(const AdmitLine *a, uint32_t policy, uint64_t lw_ub)
{
    struct timespec ts;
    for (int slot = 0; slot < CUVS_MAX_ACTIVE_BUILDS; slot++)
    {
        CuvsBuildStatusRow *r = &g_builds[slot];
        if (g_builds_used[slot])
            continue;
        memset(r, 0, sizeof(*r));
        clock_gettime(CLOCK_REALTIME, &ts);
        r->gpu = a->gpu;
        r->db_oid = a->db;
        r->index_oid = a->idx;
        r->dim = a->dim;
        r->n = a->n;
        r->started_at_us = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
        r->final_bytes = a->final;
        r->scratch_bytes = a->needed > a->final ? a->needed - a->final : 0;
        r->reserved_n_bytes = a->needed;
        r->lw_ub_bytes = lw_ub;
        r->lw_ub_valid = (uint32_t)(a->workspace_valid != 0);
        r->policy = policy;
        snprintf(r->algo, sizeof(r->algo), "%s", a->algo ? a->algo : "");
        snprintf(r->path, sizeof(r->path), "%s", a->path ? a->path : "");
        snprintf(r->phase, sizeof(r->phase), "%s", "building");
        g_builds_used[slot] = 1;
        return slot;
    }
    g_builds_unlisted++;
    return -1;
}

/* In the critical section that releases the build's N (release_build_vram). */
static void
build_status_unregister(int slot)
{
    if (slot >= 0 && slot < CUVS_MAX_ACTIVE_BUILDS)
        g_builds_used[slot] = 0;
}

/* Pool status append (I3-E). Takes g_index_mutex for one snapshot. */
static void
i3e_fill_pool_status(CuvsRmmPoolStatus *status)
{
    pthread_mutex_lock(&g_index_mutex);
    status->builds_unlisted = g_builds_unlisted;
    memcpy(status->rejects_no_gpu, g_rejects_no_gpu, sizeof(status->rejects_no_gpu));
    for (int slot = 0; slot < CUVS_MAX_ACTIVE_BUILDS; slot++)
        if (g_builds_used[slot])
            status->builds[status->n_builds++] = g_builds[slot];
    pthread_mutex_unlock(&g_index_mutex);
}

/* B's device meter is observed under g_index_mutex. solo follows C8:
 * at most one in-flight build at the point the line is emitted. */

static void
log_build_begin(AdmitLine *a)
{
    CuvsRmmPoolStats st = {0};
    (void)cuvs_rmm_pool_stats(a->gpu, &st);
    a->used_before = st.used_bytes;
    if (g_pending_build_count[a->gpu] == 0)
        cuvs_rmm_pool_peak_reset(a->gpu);
    a->build_started_us = admission_mono_us();
}

static void
log_build_end(const AdmitLine *a, const CuvsLwBuildStats *lw, int attached,
              const char *outcome)
{
    CuvsRmmPoolStats st = {0};
    uint64_t lw_peak = lw ? lw->lw_peak : 0;
    int managed = a->workspace_valid && a->workspace.managed;
    int spill = lw ? cuvs_lw_spill_flag(managed ? CUVS_LW_OVERFLOW_MANAGED :
                                      CUVS_LW_OVERFLOW_ERROR,
                                      lw->lw_peak, lw->free_at_peak) : 0;
    const char *route = strcmp(a->op, "BUILD_IVFPQ") == 0 ? "ivfpq" :
                        managed ? "cagra_managed" : "cagra";
    (void)cuvs_rmm_pool_stats(a->gpu, &st);
    if (g_rmm_pool_limits[a->gpu] > 0 && g_reclaim.peak_meter)
        LOG_INFO("[build_peak] op=%s gpu=%d db=%u idx=%u final=%zu "
                 "used_before=%llu peak_used=%llu used_after=%llu reserved_after=%llu "
                 "solo=%d outcome=%s lw_peak=%llu spill=%d mono_us=%llu\n",
                 a->op, a->gpu, a->db, a->idx, a->final,
                 (unsigned long long)a->used_before,
                 (unsigned long long)cuvs_rmm_pool_peak_bytes(a->gpu),
                 (unsigned long long)st.used_bytes, (unsigned long long)st.reserved_bytes,
                 g_pending_build_count[a->gpu] <= 1,
                 outcome, (unsigned long long)lw_peak, spill,
                 (unsigned long long)admission_mono_us());
    LOG_INFO("[build_result] op=%s gpu=%d db=%u idx=%u algo=%s n=%lld dim=%u "
             "gd=%u igd=%u dataset_attached=%d route=%s build_ms=%.3f outcome=%s "
             "self_replace=%d unloaded=%llu mono_us=%llu\n",
             a->op, a->gpu, a->db, a->idx, a->algo, (long long)a->n, a->dim,
             a->gd, a->igd, attached, route,
             (double)(a->build_finished_us - a->build_started_us) / 1000.0,
             outcome, a->self_res > 0, (unsigned long long)a->self_res,
             (unsigned long long)admission_mono_us());
}

/* [admit] with the 41 v6 fields in schema order. I3-A tip: the I3-B/I4
 * fields consume B cache and I4 workspace observations. auto_probe remains
 * na (the v5 rule's "not implementable" value). */
static void
log_admit(const AdmitLine *a, CuvsAdmissionReason reason)
{
    int      dev = a->gpu;
    size_t   budget = 0, quota = 0, limit = 0, pending_after = 0;
    size_t   headroom_raw = 0, free_net = 0, logical_used = 0, res_bytes = 0;
    uint32_t res_n = 0;
    uint64_t pool_used = 0, reserved = 0, phys_free = 0;
    AdmissionCacheTotals totals = {0};
    int cagra = strcmp(a->op, "BUILD") == 0 && strcmp(a->path, "sharded") != 0;
    const char *policy = cagra ? cuvs_lw_overflow_name(g_cagra_build_overflow) : "na";
    const char *overcommit = cagra ? (g_managed_overcommit ? "on" : "off") : "na";
    uint64_t lw_ub = 0, lw_pending = 0, host_guard = 0;
    char shape[96];
    snprintf(shape, sizeof(shape), "%s%s", a->shape ? a->shape : "na",
             cagra && g_cagra_build_overflow == CUVS_LW_OVERFLOW_MANAGED &&
             !cuvs_lw_shape_checked(a->n, a->dim, a->igd) ? "+lw_extrapolated" : "");
    size_t   scratch = a->needed > a->final ? a->needed - a->final : 0;

    if (dev >= 0)
    {
        CuvsRmmPoolStats st;
        budget = g_max_vram_per_gpu[dev];
        quota = g_rmm_pool_limits[dev];
        limit = admission_limit(dev);
        pending_after = g_pending_rmm_build_vram[dev];
        headroom_raw = cuvs_vram_free_bytes_on(dev);
        free_net = gpu_free_vram_bytes(dev);
        logical_used = total_vram_used(dev);
        admission_resident(dev, &res_bytes, &res_n);
        if (a->cache_valid)
            totals = a->cache;
        else
            admission_cache_totals(dev, &totals);
        phys_free = cuvs_vram_physical_free_bytes_on(dev);
        if (quota > 0 && cuvs_rmm_pool_stats(dev, &st) == 0)
        {
            pool_used = st.used_bytes;
            reserved = st.reserved_bytes;
        }
    }
    if (dev < 0)
        for (int i = 0; i < n_usable_gpus(); i++)
        {
            int candidate = usable_gpu(i);
            size_t candidate_limit = admission_limit(candidate);
            if (candidate_limit > limit)
            {
                limit = candidate_limit;
                budget = g_max_vram_per_gpu[candidate];
                quota = g_rmm_pool_limits[candidate];
            }
        }
    if (a->workspace_valid)
    {
        reserved = a->workspace.reserved;
        pool_used = a->workspace.pool_used;
        phys_free = a->workspace.phys_free;
        lw_ub = a->workspace.lw_ub;
        lw_pending = a->workspace.lw_pending;
        host_guard = a->workspace.host_guard;
    }
    LOG_INFO("[admit] op=%s path=%s gpu=%d db=%u idx=%u algo=%s shape_class=%s "
             "auto_probe=na n=%lld dim=%u gd=%u igd=%u final=%zu scratch=%zu needed=%zu "
             "limit=%zu budget=%zu pending_before=%zu pending_after=%zu quota=%zu "
             "headroom_raw=%zu free_net=%zu pool_used=%llu logical_used=%zu evicted=%d "
             "verdict=%s reason=%s res_bytes=%zu res_n=%u lru_free=%llu elig_free=%llu "
             "pin_bytes=%llu policy=%s overcommit=%s reserved=%llu phys_free=%llu lw_ub=%llu "
             "lw_pending=%llu host_guard=%llu self_res=%llu mono_us=%llu "
             "pool_need=%llu headroom=%llu pool_quota=%zu pending=%llu\n",
             a->op, a->path, dev, a->db, a->idx, a->algo, shape,
             (long long) a->n, a->dim,
             a->gd, a->igd, a->final, scratch, a->needed, limit, budget,
             a->pending_before, pending_after, quota, headroom_raw, free_net,
             (unsigned long long) pool_used, logical_used, dev < 0 ? 0 : (int)(g_cache_evictions[dev] - a->evictions_before),
             reason == CUVS_ADMISSION_NONE ? "admit" : "reject",
             reason == CUVS_ADMISSION_NONE ? "none" : cuvs_admission_reason_name(reason),
             res_bytes, res_n, (unsigned long long)totals.lru,
             (unsigned long long)totals.eligible, (unsigned long long)totals.pinned,
             policy, overcommit, (unsigned long long)reserved,
             (unsigned long long)phys_free, (unsigned long long)lw_ub,
             (unsigned long long)lw_pending, (unsigned long long)host_guard,
             (unsigned long long)a->self_res, (unsigned long long)admission_mono_us(),
             (unsigned long long)(a->pool_valid ? a->pool_need : 0),
             (unsigned long long)(a->pool_valid ? a->pool_headroom : 0), quota,
             (unsigned long long)(a->pool_valid ? a->pool_pending : 0));
    if (reason != CUVS_ADMISSION_NONE)
        observe_reject(dev, reason);   /* I3-E: one count per emitted rejection */
    if (a->shape && strcmp(a->shape, "extrapolated") == 0)
        LOG_WARN("scratch model unvalidated: n=%lld dim=%u gd=%u igd=%u algo=%s\n",
                 (long long) a->n, a->dim, a->gd, a->igd, a->algo);
}

/* Pick the GPU with the most VRAM headroom for a new index of `needed` bytes.
 * Returns device_id or -1 if no device can fit even in principle (needed >
 * budget). Does NOT reject based on current usage -- ensure_vram handles
 * eviction to make room. */
static int
pick_gpu_for_index(size_t needed)
{
    int best = -1;
    size_t best_headroom = 0;
    int n = n_usable_gpus();
    for (int i = 0; i < n; i++)
    {
        int dev = usable_gpu(i);
        size_t budget = g_max_vram_per_gpu[dev];
        if (g_rmm_pool_limits[dev] > 0 &&
            (budget == 0 || g_rmm_pool_limits[dev] < budget))
            budget = g_rmm_pool_limits[dev];
        if (budget > 0 && needed > budget)
            continue;
        size_t used = total_vram_used(dev);
        size_t headroom = budget > 0 ? (budget > used ? budget - used : 0)
                                    : gpu_free_vram_bytes(dev);
        if (best < 0 || headroom > best_headroom) {
            best_headroom = headroom;
            best = dev;
        }
    }
    return best;
}

/* ----------------------------------------------------------------
 * Index persistence helpers
 * ---------------------------------------------------------------- */
static void
index_file_path(char *out, size_t outlen,
                const char *dir, uint32_t db_oid, uint32_t index_oid)
{
    snprintf(out, outlen, "%s/%u_%u.cagra", dir, db_oid, index_oid);
}

static void
tids_file_path(char *out, size_t outlen,
               const char *dir, uint32_t db_oid, uint32_t index_oid)
{
    snprintf(out, outlen, "%s/%u_%u.tids", dir, db_oid, index_oid);
}

/* Phase 3I-1: CPU HNSW fallback sidecar. */
static void
hnsw_file_path(char *out, size_t outlen,
               const char *dir, uint32_t db_oid, uint32_t index_oid)
{
    snprintf(out, outlen, "%s/%u_%u.hnsw", dir, db_oid, index_oid);
}

/* 3P: IVF-PQ serialized index. */
static void
ivfpq_file_path(char *out, size_t outlen,
                const char *dir, uint32_t db_oid, uint32_t index_oid)
{
    snprintf(out, outlen, "%s/%u_%u.ivfpq", dir, db_oid, index_oid);
}

/* Sidecar marking an index stale; persists staleness across daemon restarts. */
static void
stale_file_path(char *out, size_t outlen,
                const char *dir, uint32_t db_oid, uint32_t index_oid)
{
    snprintf(out, outlen, "%s/%u_%u.stale", dir, db_oid, index_oid);
}

/* Phase 3F: manifest sidecar marking a logical index as sharded; the commit
 * marker for a sharded build (renamed last). */
static void
shards_manifest_path(char *out, size_t outlen,
                     const char *dir, uint32_t db_oid, uint32_t index_oid)
{
    snprintf(out, outlen, "%s/%u_%u.shards", dir, db_oid, index_oid);
}

/* Phase 3F: per-shard CAGRA artifact, e.g. "<db>_<idx>.s000.cagra". */
static void
shard_cagra_path(char *out, size_t outlen,
                 const char *dir, uint32_t db_oid, uint32_t index_oid,
                 uint32_t shard_id)
{
    snprintf(out, outlen, "%s/%u_%u.s%03u.cagra", dir, db_oid, index_oid, shard_id);
}

/* Pending-insert delta sidecar (Phase 3B); written by the PG backend, replayed
 * here into a resident GPU brute-force cache. */
static void
delta_file_path(char *out, size_t outlen,
                const char *dir, uint32_t db_oid, uint32_t index_oid)
{
    snprintf(out, outlen, "%s/%u_%u.delta", dir, db_oid, index_oid);
}

/* Phase 3L: raw vector matrix sidecar for GPU brute-force search; written at
 * build time alongside `.cagra`/`.tids`, loaded into a resident CuvsBfIndex. */
static void
vectors_file_path(char *out, size_t outlen,
                  const char *dir, uint32_t db_oid, uint32_t index_oid)
{
    snprintf(out, outlen, "%s/%u_%u.vectors", dir, db_oid, index_oid);
}

/* Phase 3C: heap compatibility sidecar; written by the extension at build time.
 * Format: "<relfilenode> <table_oid>\n" */
static void
relfilenode_file_path(char *out, size_t outlen,
                      const char *dir, uint32_t db_oid, uint32_t index_oid)
{
    snprintf(out, outlen, "%s/%u_%u.relfilenode", dir, db_oid, index_oid);
}

static int
read_relfilenode_sidecar(const char *dir, uint32_t db_oid, uint32_t index_oid,
                         uint32_t *rfn_out, uint32_t *table_oid_out)
{
    char path[512];
    relfilenode_file_path(path, sizeof(path), dir, db_oid, index_oid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = (fscanf(f, "%u %u", rfn_out, table_oid_out) == 2);
    fclose(f);
    return ok ? 0 : -1;
}

/* ----------------------------------------------------------------
 * Phase 3B delta cache: resident GPU brute-force over the pending `.delta`.
 * ----------------------------------------------------------------
 * The merge comparator needs the metric; handle_search is serialized by
 * g_index_mutex, so a file-scope value set right before qsort is safe. */
static uint32_t g_merge_metric;

static int
delta_cand_cmp(const void *a, const void *b)
{
    const CuvsResult *x = (const CuvsResult *) a;
    const CuvsResult *y = (const CuvsResult *) b;

    if (g_merge_metric == CUVS_METRIC_IP)   /* larger inner product = nearer */
    {
        if (x->distance > y->distance) return -1;
        if (x->distance < y->distance) return 1;
        return 0;
    }
    if (x->distance < y->distance) return -1;   /* smaller distance = nearer */
    if (x->distance > y->distance) return 1;
    return 0;
}

/* GPU that hosts the delta brute-force cache for an entry. Unsharded indexes
 * use their single device; a sharded index has no single device (gpu_device_id
 * is the 0xFFFFFFFF sentinel), so the global delta cache lives on shard 0's GPU
 * (Phase 3G.3). Caller holds g_index_mutex (reads e->shards). */
static uint32_t
delta_gpu_of(const IndexEntry *e)
{
    if (e->shard_count >= 2 && e->shards)
        return e->shards[0].gpu_device_id;
    return e->gpu_device_id;
}

/* Release an entry's GPU delta cache. Caller holds g_index_mutex. */
static void
free_delta_cache(IndexEntry *e)
{
    if (e->delta_idx) { cuvs_bf_free(e->delta_idx, delta_gpu_of(e)); e->delta_idx = NULL; }
    if (e->delta_tids) { free(e->delta_tids); e->delta_tids = NULL; }
    if (e->delta_vecs_host) { free(e->delta_vecs_host); e->delta_vecs_host = NULL; }
    e->n_delta          = 0;
    e->delta_n_cached   = 0;
    e->delta_generation = 0;
    e->delta_mtime      = 0;
    e->delta_vram_bytes = 0;
}

/* Phase 3L: release an entry's resident GPU brute-force index over the full
 * base vector matrix. Unsharded only — sharded per-shard bf_idx is freed in
 * free_index_shards. Caller holds g_index_mutex. */
static void
free_main_bf_cache(IndexEntry *e)
{
    if (e->main_bf_idx) { cuvs_bf_free(e->main_bf_idx, delta_gpu_of(e)); e->main_bf_idx = NULL; }
    e->main_bf_n          = 0;
    e->main_bf_vram_bytes = 0;
    e->main_bf_mtime      = 0;
    e->main_bf_generation = 0;
}

/* Make e's GPU delta cache match the current `.delta` file. Cheap when nothing
 * changed (one stat + mtime compare). On a change it rebuilds the resident
 * brute-force index; any failure (corrupt / generation mismatch / no spare
 * VRAM) leaves the cache empty so the search runs base-only and the backend
 * CPU-merges. Best-effort, non-evicting: the delta must never evict a base
 * CAGRA index. Caller holds g_index_mutex. */
static void
refresh_delta_cache(IndexEntry *e)
{
    char            path[512], tids_path[512];
    struct stat     st;
    FILE           *f;
    CuvsDeltaHeader hdr;
    CuvsTidsHeader  th;
    uint32_t        base_crc;
    size_t          got, rec_bytes, needed;
    float          *vecs = NULL;
    uint64_t       *tids = NULL;
    char           *rec  = NULL;
    uint32_t        dg   = delta_gpu_of(e);   /* delta cache GPU (shard 0 if sharded) */

    delta_file_path(path, sizeof(path), g_index_dir, e->db_oid, e->index_oid);
    if (stat(path, &st) != 0)
    {
        free_delta_cache(e);            /* .delta gone (REINDEX) -> base-only */
        return;
    }
    if ((int64_t) st.st_mtime == e->delta_mtime)
        return;                         /* unchanged -> reuse current cache */

    /* The file changed. Save previous state for incremental check, then
     * selectively clear: free the GPU index but keep host buffers if the
     * generation might still match. Record new mtime up front so a persistent
     * failure below does not retry on every search. */
    uint32_t prev_gen    = e->delta_generation;
    int64_t  prev_cached = e->delta_n_cached;
    if (e->delta_idx) { cuvs_bf_free(e->delta_idx, dg); e->delta_idx = NULL; }
    e->n_delta          = 0;
    e->delta_vram_bytes = 0;
    e->delta_mtime      = (int64_t) st.st_mtime;

    f = fopen(path, "rb");
    if (!f)
        goto fail_cleanup;
    if (cuvs_delta_read_header(f, &hdr) != 0
        || cuvs_delta_validate(&hdr, (int64_t) st.st_size - (int64_t) sizeof(hdr)) != 0
        || hdr.dim != e->dim || hdr.metric != e->metric)
    {
        fclose(f);
        goto fail_cleanup;
    }

    /* Generation: the delta must belong to the current base build's .tids. */
    tids_file_path(tids_path, sizeof(tids_path), g_index_dir, e->db_oid, e->index_oid);
    {
        FILE *tf = fopen(tids_path, "rb");
        if (!tf) { fclose(f); goto fail_cleanup; }
        got = fread(&th, 1, sizeof(th), tf);
        fclose(tf);
    }
    if (got != sizeof(th) || th.magic != CUVS_TIDS_MAGIC
        || th.body_crc32 != hdr.base_tids_crc32)
    {
        fclose(f);
        goto fail_cleanup;              /* stale delta from an old base -> base-only */
    }
    e->delta_generation = hdr.base_tids_crc32;
    if (hdr.n_rows == 0)
    {
        fclose(f);
        goto fail_cleanup;              /* empty delta -> base-only */
    }

    /* VRAM: best-effort, NON-evicting (never evict a base index for the delta). */
    needed = (size_t) hdr.n_rows * ((size_t) hdr.dim * sizeof(float) + sizeof(uint64_t));
    if ((g_max_vram_per_gpu[dg] > 0
         && total_vram_used(dg) + needed > g_max_vram_per_gpu[dg])
        || needed > gpu_free_vram_bytes(dg))
    {
        fclose(f);
        goto fail_cleanup;              /* no spare VRAM -> backend CPU-merges */
    }

    rec_bytes = cuvs_delta_record_bytes(hdr.dim);

    /* Phase 3A-4 incremental path: if the generation matches, the file just
     * grew (append-only), and we still have the host-side buffers, read only
     * the new records and rebuild the GPU BF from the full host buffer. */
    if (hdr.base_tids_crc32 == prev_gen
        && hdr.n_rows > prev_cached
        && e->delta_vecs_host != NULL && e->delta_tids != NULL)
    {
        int64_t new_rows = hdr.n_rows - prev_cached;
        float    *new_vecs = realloc(e->delta_vecs_host,
                                     (size_t) hdr.n_rows * hdr.dim * sizeof(float));
        uint64_t *new_tids = realloc(e->delta_tids,
                                     (size_t) hdr.n_rows * sizeof(uint64_t));
        rec = malloc(rec_bytes);
        if (new_vecs && new_tids && rec)
        {
            e->delta_vecs_host = new_vecs;
            e->delta_tids      = new_tids;
            long seek_off = (long) sizeof(hdr) + (long)(prev_cached * rec_bytes);
            if (fseek(f, seek_off, SEEK_SET) == 0)
            {
                int ok = 1;
                for (int64_t i = 0; i < new_rows; i++)
                {
                    int64_t idx = prev_cached + i;
                    if (fread(rec, 1, rec_bytes, f) != rec_bytes) { ok = 0; break; }
                    memcpy(&e->delta_tids[idx], rec, sizeof(uint64_t));
                    memcpy(&e->delta_vecs_host[(size_t) idx * hdr.dim],
                           rec + sizeof(uint64_t),
                           (size_t) hdr.dim * sizeof(float));
                }
                if (ok)
                {
                    if (e->delta_idx) { cuvs_bf_free(e->delta_idx, dg); e->delta_idx = NULL; }
                    e->delta_idx = cuvs_bf_build(e->delta_vecs_host, hdr.n_rows,
                                                 (int) hdr.dim, hdr.metric,
                                                 0 /* float32: CPU-exact equivalence */, dg);
                    if (e->delta_idx)
                    {
                        e->n_delta          = hdr.n_rows;
                        e->delta_n_cached   = hdr.n_rows;
                        e->delta_vram_bytes = needed;
                        LOG_INFO("pg_cuvs_server: delta cache %u/%u incremental (%lld -> %lld rows)\n",
                                e->db_oid, e->index_oid,
                                (long long) prev_cached, (long long) hdr.n_rows);
                    }
                }
            }
        }
        free(rec);
        fclose(f);
        return;
    }

    /* Full rebuild path */
    vecs = malloc((size_t) hdr.n_rows * hdr.dim * sizeof(float));
    tids = malloc((size_t) hdr.n_rows * sizeof(uint64_t));
    rec  = malloc(rec_bytes);
    if (vecs && tids && rec && fseek(f, (long) sizeof(hdr), SEEK_SET) == 0)
    {
        int ok = 1;
        for (int64_t i = 0; i < hdr.n_rows; i++)
        {
            if (fread(rec, 1, rec_bytes, f) != rec_bytes) { ok = 0; break; }
            memcpy(&tids[i], rec, sizeof(uint64_t));
            memcpy(&vecs[(size_t) i * hdr.dim], rec + sizeof(uint64_t),
                   (size_t) hdr.dim * sizeof(float));
        }
        if (ok)
        {
            e->delta_idx = cuvs_bf_build(vecs, hdr.n_rows, (int) hdr.dim, hdr.metric,
                                         0 /* float32: CPU-exact equivalence */, dg);
            if (e->delta_idx)
            {
                e->delta_tids       = tids;  tids = NULL;
                e->delta_vecs_host  = vecs;  vecs = NULL;
                e->n_delta          = hdr.n_rows;
                e->delta_n_cached   = hdr.n_rows;
                e->delta_vram_bytes = needed;
                LOG_INFO("pg_cuvs_server: delta cache %u/%u built (%lld rows, %zu KB VRAM)\n",
                        e->db_oid, e->index_oid, (long long) e->n_delta, needed / 1024);
            }
        }
    }
    free(vecs);
    free(tids);
    free(rec);
    fclose(f);
    return;

fail_cleanup:
    if (e->delta_tids) { free(e->delta_tids); e->delta_tids = NULL; }
    if (e->delta_vecs_host) { free(e->delta_vecs_host); e->delta_vecs_host = NULL; }
    e->delta_n_cached   = 0;
    e->delta_generation = 0;
}

/* Phase 3L: make e's resident main brute-force index match the current
 * `.vectors` sidecar and the requested precision. Lazily (re)built on the first
 * brute_force search and whenever the sidecar's mtime or the precision changes.
 * Leaves main_bf_idx == NULL (so the caller fails closed with NO_VECTORS) when
 * the sidecar is missing, stale (generation mismatch with the current .tids),
 * corrupt, shape-mismatched, or cannot fit. Best-effort, NON-evicting:
 * brute_force must never evict a base CAGRA index. Caller holds g_index_mutex.
 * Unsharded only — sharded BF uses per-shard caches (refresh_shard_bf_caches). */
static void
refresh_main_bf_cache(IndexEntry *e, uint32_t want_precision)
{
    char              path[512], tids_path[512];
    struct stat       st;
    FILE             *f;
    CuvsVectorsHeader vh;
    CuvsTidsHeader    th;
    float            *vecs = NULL;
    size_t            got, needed;
    uint32_t          dg = delta_gpu_of(e);   /* unsharded: the index's GPU */

    vectors_file_path(path, sizeof(path), g_index_dir, e->db_oid, e->index_oid);
    if (stat(path, &st) != 0)
    {
        free_main_bf_cache(e);          /* no sidecar -> brute_force unavailable */
        return;
    }
    if (e->main_bf_idx
        && (int64_t) st.st_mtime == e->main_bf_mtime
        && e->bf_precision == want_precision)
        return;                         /* unchanged + same precision -> reuse */

    /* Sidecar changed or precision toggled: drop the old index and rebuild.
     * Record the new mtime/precision up front so a persistent failure below
     * does not retry the (re)build on every search. */
    free_main_bf_cache(e);
    e->main_bf_mtime = (int64_t) st.st_mtime;
    e->bf_precision  = want_precision;

    f = fopen(path, "rb");
    if (!f)
        return;
    if (cuvs_vectors_read(f, &vh, &vecs) != 0)
    {
        fclose(f);
        return;                         /* corrupt / short read -> unavailable */
    }
    fclose(f);
    if (vh.dim != e->dim || vh.metric != e->metric || vh.n_vecs != e->n_vecs)
    {
        free(vecs);
        return;                         /* shape mismatch -> unavailable */
    }

    /* Generation: the sidecar must belong to the current base build's .tids. */
    tids_file_path(tids_path, sizeof(tids_path), g_index_dir, e->db_oid, e->index_oid);
    {
        FILE *tf = fopen(tids_path, "rb");
        if (!tf) { free(vecs); return; }
        got = fread(&th, 1, sizeof(th), tf);
        fclose(tf);
    }
    if (got != sizeof(th) || th.magic != CUVS_TIDS_MAGIC
        || th.body_crc32 != vh.base_tids_crc32)
    {
        free(vecs);
        return;                         /* stale sidecar (torn build) -> unavailable */
    }

    /* VRAM: best-effort, NON-evicting (never evict a base index for BF). */
    needed = (size_t) vh.n_vecs * (size_t) vh.dim * (want_precision == 1 ? 2 : 4);
    if ((g_max_vram_per_gpu[dg] > 0
         && total_vram_used(dg) + needed > g_max_vram_per_gpu[dg])
        || needed > gpu_free_vram_bytes(dg))
    {
        free(vecs);
        return;                         /* no spare VRAM -> brute_force unavailable */
    }

    e->main_bf_idx = cuvs_bf_build(vecs, vh.n_vecs, (int) vh.dim, vh.metric,
                                   want_precision, dg);
    free(vecs);
    if (e->main_bf_idx)
    {
        e->main_bf_n          = vh.n_vecs;
        e->main_bf_generation = vh.base_tids_crc32;
        e->main_bf_vram_bytes = needed;
        LOG_INFO("pg_cuvs_server: main BF cache %u/%u built (%lld vecs, %s, %zu MB VRAM)\n",
                 e->db_oid, e->index_oid, (long long) vh.n_vecs,
                 want_precision == 1 ? "float16" : "float32", needed / (1024 * 1024));
    }
}

/* Phase 3L: free every shard's resident brute-force index. Caller holds the
 * mutex. */
static void
free_shard_bf_caches(IndexEntry *e)
{
    if (!e->shards) return;
    for (int s = 0; s < e->shard_count; s++)
        if (e->shards[s].bf_idx)
        {
            cuvs_bf_free(e->shards[s].bf_idx, e->shards[s].gpu_device_id);
            e->shards[s].bf_idx = NULL;
            e->shards[s].bf_vram_bytes = 0;
        }
}

/* Phase 3L: ensure every shard of e has a resident brute-force index over its
 * vector range, built at want_precision by slicing the single global `.vectors`
 * sidecar at each shard's tid_offset. Returns 1 if all shards are ready, 0 if
 * the sidecar is missing / stale (generation mismatch) / shape-mismatched / a
 * shard won't fit — in which case the caller fails closed with NO_VECTORS.
 * Lazily (re)builds when the sidecar mtime or the precision changes; reuses
 * main_bf_mtime/bf_precision as the generation tracker (main_bf_idx is unused
 * for sharded). Best-effort, NON-evicting, all-or-nothing. Caller holds mutex. */
static int
refresh_shard_bf_caches(IndexEntry *e, uint32_t want_precision)
{
    char              path[512], tids_path[512];
    struct stat       st;
    FILE             *f;
    CuvsVectorsHeader vh;
    CuvsTidsHeader    th;
    float            *vecs = NULL;
    size_t            got;

    if (e->shard_count < 2 || !e->shards)
        return 0;

    vectors_file_path(path, sizeof(path), g_index_dir, e->db_oid, e->index_oid);
    if (stat(path, &st) != 0)
    {
        free_shard_bf_caches(e);        /* sidecar gone -> brute_force unavailable */
        e->main_bf_mtime = 0;
        return 0;
    }
    if ((int64_t) st.st_mtime == e->main_bf_mtime && e->bf_precision == want_precision)
    {
        int ready = 1;
        for (int s = 0; s < e->shard_count; s++)
            if (!e->shards[s].bf_idx) { ready = 0; break; }
        if (ready)
            return 1;                   /* unchanged + same precision + all built */
    }

    /* (Re)build: drop existing per-shard BF, load once, slice, build each. */
    free_shard_bf_caches(e);
    e->main_bf_mtime = (int64_t) st.st_mtime;
    e->bf_precision  = want_precision;

    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (cuvs_vectors_read(f, &vh, &vecs) != 0) { fclose(f); return 0; }
    fclose(f);
    if (vh.dim != e->dim || vh.metric != e->metric || vh.n_vecs != e->n_vecs)
    {
        free(vecs);
        return 0;
    }

    /* Generation: the sidecar must belong to the current base build's .tids. */
    tids_file_path(tids_path, sizeof(tids_path), g_index_dir, e->db_oid, e->index_oid);
    {
        FILE *tf = fopen(tids_path, "rb");
        if (!tf) { free(vecs); return 0; }
        got = fread(&th, 1, sizeof(th), tf);
        fclose(tf);
    }
    if (got != sizeof(th) || th.magic != CUVS_TIDS_MAGIC
        || th.body_crc32 != vh.base_tids_crc32)
    {
        free(vecs);
        return 0;
    }

    /* Build each shard's BF over its contiguous [tid_offset, +n_vecs) range.
     * VRAM best-effort, NON-evicting per shard GPU; any failure rolls back all
     * shards and reports unavailable (fail closed). */
    int ok = 1;
    for (int s = 0; s < e->shard_count; s++)
    {
        ShardEntry *sh  = &e->shards[s];
        int         dev = (int) sh->gpu_device_id;
        size_t      needed = (size_t) sh->n_vecs * (size_t) vh.dim
                             * (want_precision == 1 ? 2 : 4);
        if ((g_max_vram_per_gpu[dev] > 0
             && total_vram_used(dev) + needed > g_max_vram_per_gpu[dev])
            || needed > gpu_free_vram_bytes(dev))
        {
            ok = 0;
            break;
        }
        sh->bf_idx = cuvs_bf_build(vecs + (size_t) sh->tid_offset * vh.dim,
                                   sh->n_vecs, (int) vh.dim, vh.metric,
                                   want_precision, dev);
        if (!sh->bf_idx) { ok = 0; break; }
        sh->bf_vram_bytes = needed;
    }
    free(vecs);

    if (!ok)
    {
        free_shard_bf_caches(e);
        return 0;
    }
    LOG_INFO("pg_cuvs_server: sharded BF cache %u/%u built (%d shards, %s)\n",
             e->db_oid, e->index_oid, e->shard_count,
             want_precision == 1 ? "float16" : "float32");
    return 1;
}

/* Forward decls used by save_index. */
static int crc32_file(const char *path, uint32_t *out);
static void free_index_shards(IndexEntry *e);
static IndexEntry *find_index(uint32_t db_oid, uint32_t index_oid);
static int write_tids_atomic(const char *tids_tmp,
                             int64_t n_vecs, uint32_t dim, uint32_t metric,
                             const uint64_t *tids);
static int fsync_path(const char *path);
static void fchmod_sidecar(FILE *f, const char *path);
static void chmod_sidecar(const char *path);
static CuvsAdmissionReason lw_admission_check(int device_id, uint64_t needed,
    int64_t n, uint32_t dim, int intermediate_graph_degree,
    CuvsAdmissionWorkspaceInput *ws);

static int ensure_vram(size_t needed, int device_id, CuvsRequester rq,
                       CuvsAdmissionFields *rej, AdmitLine *line); /* after the LRU section */
static size_t evict_lru(int device_id, CuvsRequester rq);
static int make_slot(int device_id, CuvsRequester rq);
static IndexEntry *registry_new_slot(void);
static size_t entry_bytes_on(const IndexEntry *e, int device_id);

/* save_index: persist a registry entry to disk atomically.
 * Same contract as handle_build's persistence path: tmp + rename + fsync.
 * Returns 0 on success, -1 on any failure (caller must NOT proceed to free
 * VRAM state, e.g., evict_lru must abort eviction on failure). */
static int
save_index(IndexEntry *e)
{
#ifdef CUVS_TEST_HOOKS
    if (cuvs_fault("CUVS_FAULT_SAVE_INDEX")) {
        LOG_ERROR("save_index: CUVS_FAULT_SAVE_INDEX -> forced failure for %u/%u\n",
                e->db_oid, e->index_oid);
        return -1;
    }
#endif
    /* Defensive (ADR-070): save_index serializes a CAGRA handle. An IVF-PQ or
     * sharded entry has e->handle == NULL; serializing it would deref NULL.
     * Callers must route those to their own (save-free) eviction paths; fail
     * closed here rather than crash if one ever reaches us. */
    if (e->handle == NULL) {
        LOG_ERROR("save_index: %u/%u has no CAGRA handle (ivfpq/sharded?); refusing\n",
                  e->db_oid, e->index_oid);
        return -1;
    }

    char idx_final[512],  idx_tmp[576];
    char tids_final[512], tids_tmp[576];
    index_file_path(idx_final,  sizeof(idx_final),  g_index_dir, e->db_oid, e->index_oid);
    tids_file_path(tids_final,  sizeof(tids_final), g_index_dir, e->db_oid, e->index_oid);
    snprintf(idx_tmp,  sizeof(idx_tmp),  "%s.tmp", idx_final);
    snprintf(tids_tmp, sizeof(tids_tmp), "%s.tmp", tids_final);

    if (write_tids_atomic(tids_tmp, e->n_vecs, e->dim, e->metric, e->tids) != 0)
        return -1;

    if (cuvs_cagra_serialize(e->handle, idx_tmp, e->gpu_device_id) != 0) {
        LOG_ERROR("save_index: cuvs_cagra_serialize FAILED for %u/%u\n",
                e->db_oid, e->index_oid);
        unlink(tids_tmp);
        return -1;
    }
    chmod_sidecar(idx_tmp);
    if (fsync_path(idx_tmp) != 0)
        LOG_WARN("save_index: fsync %s failed errno=%d\n", idx_tmp, errno);

    if (rename(tids_tmp, tids_final) != 0) {
        LOG_ERROR("save_index: rename %s -> %s FAILED errno=%d (%s)\n",
                tids_tmp, tids_final, errno, strerror(errno));
        unlink(tids_tmp);
        unlink(idx_tmp);
        return -1;
    }
    if (rename(idx_tmp, idx_final) != 0) {
        LOG_ERROR("save_index: rename %s -> %s FAILED errno=%d (%s); "
                  "unlinking tids to avoid mismatch\n",
                idx_tmp, idx_final, errno, strerror(errno));
        unlink(tids_final);
        unlink(idx_tmp);
        return -1;
    }
    int dir_fd = open(g_index_dir, O_RDONLY);
    if (dir_fd >= 0) { fsync(dir_fd); close(dir_fd); }

    e->file_current = 1;   /* I-FC (iii) */
    LOG_INFO("pg_cuvs_server: saved index %u/%u (%lld vecs)\n",
            e->db_oid, e->index_oid, (long long)e->n_vecs);
    return 0;
}

/* Phase 3F: reload a sharded logical index from its `.shards` manifest. The
 * caller has already read + validated the global `.tids` (thdr, tids). On
 * success registers a sharded IndexEntry, takes ownership of `tids`, and
 * returns 0. Returns 1 when no manifest exists (caller uses the unsharded
 * path; `tids` untouched). Returns -1 (corrupt/mismatch) or -2 (VRAM) on
 * failure WITHOUT consuming `tids` — fail closed, never a partial index.
 * Caller holds g_index_mutex. */
static int
load_index_sharded(uint32_t db_oid, uint32_t index_oid,
                   const CuvsTidsHeader *thdr, uint64_t *tids, CuvsRequester rq)
{
    /* A search-side reload of a sharded index is LOAD_SHARD (C1 RQ table). */
    if (rq == CUVS_RQ_LOAD)
        rq = CUVS_RQ_LOAD_SHARD;
    char manifest_path[512];
    shards_manifest_path(manifest_path, sizeof(manifest_path), g_index_dir, db_oid, index_oid);

    FILE *mf = fopen(manifest_path, "rb");
    if (!mf)
        return 1;   /* no manifest => unsharded */

    CuvsShardsHeader shdr;
    CuvsShardRecord *recs = NULL;
    if (cuvs_shards_read(mf, &shdr, &recs) != 0)
    {
        fclose(mf);
        LOG_ERROR("pg_cuvs_server: .shards validation failed for %u/%u, skip (fail closed)\n",
                  db_oid, index_oid);
        return -1;
    }
    fclose(mf);

    /* Generation + geometry must match the .tids we already validated. */
    if (shdr.base_tids_crc32 != thdr->body_crc32 ||
        shdr.n_vecs != thdr->n_vecs ||
        shdr.dim != thdr->dim ||
        shdr.metric != thdr->metric)
    {
        LOG_ERROR("pg_cuvs_server: .shards generation/geometry mismatch for %u/%u, skip\n",
                  db_oid, index_oid);
        free(recs);
        return -1;
    }

    /* Soft cap: evict the LRU to free a slot so a sharded reload succeeds. */
    if (g_n_indexes >= g_max_indexes)
        (void) make_slot(usable_gpu(0), rq);
    if (g_n_indexes >= g_max_indexes)
    {
        free(recs);
        return -1;
    }

    int         sc      = (int)shdr.shard_count;
    ShardEntry *shards  = calloc((size_t)sc, sizeof(ShardEntry));
    if (!shards)
    {
        free(recs);
        return -1;
    }

    int n_gpus  = n_usable_gpus();
    int ok      = 1;
    int rc_fail = -1;
    int i;
    for (i = 0; i < sc; i++)
    {
        /* cold reload: graph_degree not persisted in .shards -> default 64 */
        size_t needed = estimate_vram_bytes(recs[i].n_vecs, (int)recs[i].dim, 64);
        int    dev    = usable_gpu(i % n_gpus);   /* re-place on reload */
        AdmitLine al = {.op = "LOAD_SHARD", .path = admission_load_path(rq),
                        .algo = "cagra", .gpu = dev, .db = db_oid, .idx = index_oid,
                        .dim = recs[i].dim, .gd = 64, .igd = 128, .n = recs[i].n_vecs,
                        .final = needed, .needed = needed,
                        .pending_before = g_pending_rmm_build_vram[dev],
                        .evictions_before = g_cache_evictions[dev]};
        if (ensure_vram(needed, dev, rq, NULL, &al) != 0)
        {
            int alt = pick_gpu_for_index(needed);
            al.gpu = alt;
            al.cache_valid = 0;
            if (alt >= 0)
            {
                al.pending_before = g_pending_rmm_build_vram[alt];
                al.evictions_before = g_cache_evictions[alt];
            }
            else
                admission_fill(&t_reload_rej, CUVS_ADMISSION_AR0, needed, -1);
            if (alt < 0 || ensure_vram(needed, alt, rq, &t_reload_rej, &al) != 0)
            {
                log_admit(&al, t_reload_rej.reason);
                rc_fail = -2;
                ok = 0;
                break;
            }
            dev = alt;
        }
        if (t_manual_loading)
            log_admit(&al, CUVS_ADMISSION_NONE);

        char sp[512];
        shard_cagra_path(sp, sizeof(sp), g_index_dir, db_oid, index_oid, recs[i].shard_id);

        /* Verify shard artifact CRC before deserialize (fail closed). */
        uint32_t acrc = 0;
        if (crc32_file(sp, &acrc) != 0 || acrc != recs[i].artifact_crc32)
        {
            LOG_ERROR("pg_cuvs_server: shard %u artifact crc mismatch for %u/%u, skip\n",
                      recs[i].shard_id, db_oid, index_oid);
            ok = 0;
            break;
        }

        CuvsCagraIndex h = cuvs_cagra_deserialize(sp, (int)recs[i].dim, dev);
        if (!h)
        {
            ok = 0;
            break;
        }

        shards[i].shard_id      = recs[i].shard_id;
        shards[i].handle        = h;
        shards[i].tid_offset    = recs[i].tid_offset;
        shards[i].n_vecs        = recs[i].n_vecs;
        shards[i].gpu_device_id = (uint32_t)dev;
        shards[i].vram_bytes    = needed;
        shards[i].valid         = 1;
    }

    if (!ok)
    {
        for (int j = 0; j < i && j < sc; j++)
            if (shards[j].valid && shards[j].handle)
                cuvs_cagra_free(shards[j].handle, shards[j].gpu_device_id);
        free(shards);
        free(recs);
        return rc_fail;   /* fail closed: no partial sharded index registered */
    }

    IndexEntry *e = registry_new_slot();
    memset(e, 0, sizeof(*e));
    e->db_oid        = db_oid;
    e->index_oid     = index_oid;
    e->dim           = thdr->dim;
    e->metric        = thdr->metric;
    e->n_vecs        = thdr->n_vecs;
    e->handle        = NULL;
    e->tids          = tids;          /* take ownership */
    build_rev_tid_map(e);             /* 3O: TID → item_id reverse map */
    e->vram_bytes    = 0;
    e->last_search   = time(NULL);
    e->valid         = 1;
    e->warmup_state  = WARMUP_HOT;
    e->gpu_device_id = 0xFFFFFFFFu;    /* sharded: not LRU-evictable */
    e->shard_count   = sc;
    e->shards        = shards;
    reset_entry_stats(e);

    {
        char stale_path[512];
        struct stat st;
        stale_file_path(stale_path, sizeof(stale_path), g_index_dir, db_oid, index_oid);
        if (stat(stale_path, &st) == 0) { e->stale = 1; e->stale_since = st.st_mtime; }
        else { e->stale = 0; e->stale_since = 0; }
    }

    free(recs);
    LOG_INFO("pg_cuvs_server: loaded sharded index %u/%u (%lld vecs, %d shards)\n",
             db_oid, index_oid, (long long)thdr->n_vecs, sc);
    for (int j = 0; j < sc; j++)
        LOG_INFO("  shard %d -> GPU %u (%lld vecs)\n",
                 j, e->shards[j].gpu_device_id, (long long)e->shards[j].n_vecs);
    return 0;
}

/* ----------------------------------------------------------------
 * 3O: Build sorted (TID → item_id) reverse map for BITSET prefilter.
 * Called after e->tids is populated; safe to call with n_vecs==0.
 * ---------------------------------------------------------------- */
typedef struct { uint64_t tid; int32_t item_id; } RevTidPair;
static int
cmp_rev_tid(const void *a, const void *b)
{
    uint64_t ta = ((const RevTidPair *)a)->tid;
    uint64_t tb = ((const RevTidPair *)b)->tid;
    return (ta > tb) - (ta < tb);
}

static void
build_rev_tid_map(IndexEntry *e)
{
    if (!e->tids || e->n_vecs <= 0)
        return;

    RevTidPair *pairs   = malloc((size_t)e->n_vecs * sizeof(RevTidPair));
    uint64_t   *rt      = malloc((size_t)e->n_vecs * sizeof(uint64_t));
    int32_t    *ri      = malloc((size_t)e->n_vecs * sizeof(int32_t));

    if (!pairs || !rt || !ri) {
        free(pairs); free(rt); free(ri);
        e->rev_tids     = NULL;
        e->rev_item_ids = NULL;
        LOG_WARN("[3O] build_rev_tid_map: malloc failed for index %u/%u\n",
                 e->db_oid, e->index_oid);
        return;
    }

    for (int64_t i = 0; i < e->n_vecs; i++) {
        pairs[i].tid     = e->tids[i];
        pairs[i].item_id = (int32_t)i;
    }
    qsort(pairs, (size_t)e->n_vecs, sizeof(RevTidPair), cmp_rev_tid);
    for (int64_t i = 0; i < e->n_vecs; i++) {
        rt[i] = pairs[i].tid;
        ri[i] = pairs[i].item_id;
    }
    free(pairs);

    e->rev_tids     = rt;
    e->rev_item_ids = ri;
}

static int
load_index(uint32_t db_oid, uint32_t index_oid, CuvsRequester rq)
{
    g_cur_requester_idx = index_oid;
    g_last_admit_reason = CUVS_ADMISSION_NONE;   /* never report a stale reason */
    char idx_path[512], tids_path[512];

    memset(&t_reload_rej, 0, sizeof(t_reload_rej));
    t_reload_rej.reason = CUVS_ADMISSION_NONE;
    index_file_path(idx_path, sizeof(idx_path), g_index_dir, db_oid, index_oid);
    tids_file_path(tids_path, sizeof(tids_path), g_index_dir, db_oid, index_oid);

    /* Read + validate the versioned, checksummed .tids sidecar. A reject
     * (bad magic/version/range/crc, legacy headerless format) means this
     * pair must be skipped; the index must be REINDEXed. */
    FILE *f = fopen(tids_path, "rb");
    if (!f)
        return -1;

    CuvsTidsHeader thdr;
    uint64_t *tids = NULL;
    if (cuvs_tids_read(f, &thdr, &tids) != 0)
    {
        fclose(f);
        LOG_ERROR("pg_cuvs_server: .tids validation failed for %u/%u, skip\n",
                db_oid, index_oid);
        return -1;
    }
    fclose(f);

    int64_t  n_vecs = thdr.n_vecs;
    uint32_t dim    = thdr.dim;
    uint32_t metric = thdr.metric;

    /* Header sanity: n_vecs already validated > 0 by cuvs_tids_read; require
     * a non-zero dimension before trusting the cagra side. */
    if (dim == 0)
    {
        LOG_ERROR("pg_cuvs_server: .tids header dim=0 for %u/%u, skip\n",
                db_oid, index_oid);
        free(tids);
        return -1;
    }

    /* Phase 3F: if a `.shards` manifest exists, this is a sharded logical index.
     * Load via the shard path (which takes ownership of tids on success). A
     * present-but-invalid manifest fails closed (no unsharded fallback — there
     * is no single .cagra to fall back to). */
    {
        int src = load_index_sharded(db_oid, index_oid, &thdr, tids, rq);
        if (src == 0)
            return 0;            /* sharded load OK; tids ownership transferred */
        if (src < 0)
        {
            free(tids);
            return src;          /* -1 corrupt/mismatch, -2 VRAM: fail closed */
        }
        /* src == 1: no manifest -> unsharded path below (tids still owned here) */
    }

    /* ADR-073: flat index — .tids + .vectors present, but NO .cagra and NO
     * .ivfpq. Register brute-force-only (handle == NULL, is_flat = 1). The
     * resident BF over .vectors is (re)built lazily + non-evictingly on the first
     * search (refresh_main_bf_cache), so we reserve no graph VRAM here. Detected
     * before the cagra VRAM make-room so a flat load never over-reserves or evicts
     * a cagra index for a graph it will not allocate. */
    {
        char fcagra[512], fivfpq[512], fvecs[512];
        index_file_path(fcagra, sizeof(fcagra), g_index_dir, db_oid, index_oid);
        ivfpq_file_path(fivfpq, sizeof(fivfpq), g_index_dir, db_oid, index_oid);
        vectors_file_path(fvecs, sizeof(fvecs), g_index_dir, db_oid, index_oid);
        if (access(fcagra, F_OK) != 0 && access(fivfpq, F_OK) != 0
            && access(fvecs, F_OK) == 0)
        {
            int fgpu = pick_gpu_for_index(0);
            if (fgpu < 0) fgpu = usable_gpu(0);

            /* Soft cap: free a slot if the registry is full (search-miss reload). */
            if (g_n_indexes >= g_max_indexes)
                (void) make_slot(fgpu, rq);
            if (g_n_indexes >= g_max_indexes)
            {
                free(tids);
                return -1;
            }

            IndexEntry *e = registry_new_slot();
            e->db_oid       = db_oid;
            e->index_oid    = index_oid;
            e->dim          = dim;
            e->metric       = metric;
            e->n_vecs       = n_vecs;
            e->handle       = NULL;          /* no CAGRA graph — brute-force only */
            e->ivfpq_handle = NULL;
            e->ivfpq_n_vecs = 0;
            e->ivfpq_vram_bytes = 0;
            e->tids         = tids;          /* ownership transferred */
            e->rev_tids     = NULL;
            e->rev_item_ids = NULL;
            build_rev_tid_map(e);            /* 3O: TID -> item_id reverse map */
            e->vram_bytes   = 0;             /* BF is lazy; tracked in main_bf_vram_bytes */
            e->last_search  = time(NULL);
            e->valid        = 1;
            e->gpu_device_id = (uint32_t) fgpu;
            e->shard_count  = 0;
            e->shards       = NULL;
            e->inflight     = 0;
            e->delta_idx = NULL; e->delta_tids = NULL; e->n_delta = 0;
            e->delta_generation = 0; e->delta_mtime = 0; e->delta_vram_bytes = 0;
            e->delta_vecs_host = NULL; e->delta_n_cached = 0;
            e->main_bf_idx = NULL; e->main_bf_n = 0; e->main_bf_vram_bytes = 0;
            e->main_bf_mtime = 0; e->main_bf_generation = 0; e->bf_precision = 0;
            e->hnsw_idx     = NULL;
            e->n_extended   = 0;
            e->compact_count = 0;
            e->last_compact_at = 0;
            e->warmup_state = WARMUP_HOT;
            reset_entry_stats(e);
            e->is_flat      = 1;             /* MUST be after reset_entry_stats */
            e->last_search_mode = 3;         /* gpu_bf */

            /* Restore staleness from the .stale sidecar (same as cagra). */
            {
                char stale_path[512];
                struct stat st;
                stale_file_path(stale_path, sizeof(stale_path), g_index_dir, db_oid, index_oid);
                if (stat(stale_path, &st) == 0) { e->stale = 1; e->stale_since = st.st_mtime; }
                else { e->stale = 0; e->stale_since = 0; }
            }

            if (t_manual_loading)
            {
                AdmitLine al = {.op = "LOAD", .path = "load", .algo = "flat",
                    .gpu = fgpu, .db = db_oid, .idx = index_oid,
                    .dim = dim, .n = n_vecs,
                    .pending_before = g_pending_rmm_build_vram[fgpu],
                    .evictions_before = g_cache_evictions[fgpu]};
                log_admit(&al, CUVS_ADMISSION_NONE);
            }
            LOG_INFO("pg_cuvs_server: loaded flat index %u/%u (%lld vecs, BF built lazily)\n",
                     db_oid, index_oid, (long long) n_vecs);
            return 0;
        }
    }

    /* VRAM make-room: evict LRU resident indexes to fit (tiered cache). Same
     * path as build, so a search miss on a non-resident index reloads it by
     * evicting the least-recently-used one. If it still won't fit after full
     * eviction, skip (caller falls back to CPU). */
    /* cold reload: graph_degree not persisted in .tids -> default 64 */
    size_t needed = estimate_vram_bytes(n_vecs, (int)dim, 64);
    int target_gpu = pick_gpu_for_index(needed);
    if (target_gpu < 0)
    {
        AdmitLine al = {.op = "LOAD", .path = admission_load_path(rq), .algo = "cagra", .gpu = -1,
                        .db = db_oid, .idx = index_oid, .dim = dim, .gd = 64, .igd = 128,
                        .n = n_vecs, .final = needed, .needed = needed};
        admission_fill(&t_reload_rej, CUVS_ADMISSION_AR0, needed, -1);
        log_admit(&al, CUVS_ADMISSION_AR0);
        LOG_WARN("pg_cuvs_server: no GPU can fit %u/%u (%zu bytes), skip\n",
                 db_oid, index_oid, needed);
        free(tids);
        return -2;  /* VRAM: index too large for any GPU budget */
    }
    AdmitLine al = {.op = "LOAD",
                    .path = admission_load_path(rq),
                    .algo = "cagra", .gpu = target_gpu, .db = db_oid, .idx = index_oid,
                    .dim = dim, .gd = 64, .igd = 128, .n = n_vecs,
                    .final = needed, .needed = needed,
                    .pending_before = g_pending_rmm_build_vram[target_gpu],
                    .evictions_before = g_cache_evictions[target_gpu]};
    if (ensure_vram(needed, target_gpu, rq, &t_reload_rej, &al) != 0)
    {
        log_admit(&al, t_reload_rej.reason);
        LOG_WARN("pg_cuvs_server: VRAM exhausted on GPU %d loading %u/%u "
                 "(budget %zu MB, used %zu MB, needed %zu MB)\n",
                 target_gpu, db_oid, index_oid,
                 g_max_vram_per_gpu[target_gpu] / (1024*1024),
                 total_vram_used(target_gpu) / (1024*1024),
                 needed / (1024*1024));
        free(tids);
        return -2;  /* VRAM: exhausted after eviction */
    }

    if (t_manual_loading)
        log_admit(&al, CUVS_ADMISSION_NONE);

    CuvsCagraIndex handle       = cuvs_cagra_deserialize(idx_path, (int)dim, target_gpu);
    CuvsIvfPqIndex ivfpq_handle = NULL;
    if (!handle)
    {
        /* No .cagra sidecar — try .ivfpq (3P IVF-PQ index). */
        char ivfpq_path[512];
        ivfpq_file_path(ivfpq_path, sizeof(ivfpq_path), g_index_dir, db_oid, index_oid);
        if (access(ivfpq_path, F_OK) == 0)
            ivfpq_handle = cuvs_ivfpq_deserialize(ivfpq_path, target_gpu);
        if (!ivfpq_handle)
        {
            free(tids);
            return -1;
        }
    }

    /* Soft cap: if the registry is full, evict the LRU to free a slot so this
     * reload (search-miss auto-reload) succeeds. ensure_vram above only evicts
     * under VRAM pressure; with spare VRAM but full slots it would not, leaving
     * an evicted index unable to return. */
    if (g_n_indexes >= g_max_indexes)
        (void) make_slot(target_gpu, rq);
    if (g_n_indexes >= g_max_indexes)
    {
        if (handle)       cuvs_cagra_free(handle, target_gpu);
        if (ivfpq_handle) cuvs_ivfpq_free(ivfpq_handle, target_gpu);
        free(tids);
        return -1;
    }

    IndexEntry *e = registry_new_slot();
    e->db_oid       = db_oid;
    e->index_oid    = index_oid;
    e->dim          = dim;
    e->metric       = metric;
    e->n_vecs       = n_vecs;
    e->handle       = handle;
    e->ivfpq_handle = ivfpq_handle;
    e->ivfpq_n_vecs = ivfpq_handle ? n_vecs : 0;
    e->tids         = tids;
    build_rev_tid_map(e);   /* 3O: TID → item_id reverse map */
    e->vram_bytes  = needed;
    e->last_search = time(NULL);
    e->valid       = 1;
    e->gpu_device_id = (uint32_t)target_gpu;
    e->shard_count = 0;     /* unsharded; slot may be reused post-eviction */
    e->shards      = NULL;
    /* Delta cache starts empty; this slot may carry stale delta fields from a
     * previously evicted index. It is lazily (re)built in handle_search. */
    e->delta_idx = NULL; e->delta_tids = NULL; e->n_delta = 0;
    e->delta_generation = 0; e->delta_mtime = 0; e->delta_vram_bytes = 0;
    e->delta_vecs_host = NULL; e->delta_n_cached = 0;
    /* Phase 3L: main BF cache starts empty; lazily built on a BF search. */
    e->main_bf_idx = NULL; e->main_bf_n = 0; e->main_bf_vram_bytes = 0;
    e->main_bf_mtime = 0; e->main_bf_generation = 0; e->bf_precision = 0;
    /* 3P: ivfpq_vram_bytes already set above; ensure it's 0 for CAGRA entries */
    if (!e->ivfpq_handle) e->ivfpq_vram_bytes = 0;
    e->warmup_state = WARMUP_HOT;
    reset_entry_stats(e);
    /* I3-B defect 3 (FACT-47): a reused slot must not carry the previous
     * occupant's EXTEND/compact counters into the reloaded instance. */
    e->n_extended      = 0;
    e->compact_count   = 0;
    e->last_compact_at = 0;
    /* I-FC (ii): just loaded from the files. Only CAGRA single is ever saved
     * by eviction; other kinds keep 0 (unused). */
    e->file_current = handle != NULL;

    /* Restore staleness from the .stale sidecar so a daemon restart does not
     * silently resurrect a write-stale index as fresh. */
    {
        char stale_path[512];
        struct stat st;
        stale_file_path(stale_path, sizeof(stale_path), g_index_dir, db_oid, index_oid);
        if (stat(stale_path, &st) == 0)
        {
            e->stale = 1;
            e->stale_since = st.st_mtime;
        }
        else
        {
            e->stale = 0;
            e->stale_since = 0;
        }
    }

    LOG_INFO("pg_cuvs_server: loaded index %u/%u (%lld vecs, %zu MB VRAM)\n",
            db_oid, index_oid, (long long)n_vecs, needed / (1024*1024));
    return 0;
}

/* I3-B usage stats file (UC-1 4c): beside the index files, a dot-name so
 * cuvs_parse_index_filename, gc_orphans and the not_resident enumeration
 * never see it as an index artifact. Written by the reclaim thread each
 * tick (tmp + fsync + rename), read once at startup. */
static void
usage_stats_path(char *out, size_t outlen, int tmp)
{
    snprintf(out, outlen, "%s/.pg_cuvs_usage_stats%s", g_index_dir, tmp ? ".tmp" : "");
}

/* Read the previous run's usage stats. Returns the count, or -1 (missing,
 * unreadable or corrupt: caller falls back to OID order) with why set. */
static int
usage_stats_load(CuvsUsageStat *out, int max, char *why, size_t whylen)
{
    char path[512];
    usage_stats_path(path, sizeof(path), 0);
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        snprintf(why, whylen, "no usage stats file");
        return -1;
    }
    size_t cap = (size_t)CUVS_USAGE_STATS_MAX * 64 + 128, len = 0;
    char *buf = malloc(cap);
    if (!buf)
    {
        fclose(f);
        snprintf(why, whylen, "out of memory");
        return -1;
    }
    len = fread(buf, 1, cap, f);
    int too_big = !feof(f);
    fclose(f);
    int n = 0;
    if (too_big)
    {
        snprintf(why, whylen, "usage stats file too large");
        free(buf);
        return -1;
    }
    if (cuvs_usage_stats_parse(buf, len, out, max, &n, why, whylen) != 0)
    {
        free(buf);
        return -1;
    }
    free(buf);
    return n;
}

/* Load the keys of one startup pass in the C1 order (hot first from the
 * usage stats, else (db, idx) ascending), never evicting (LOAD_STARTUP). */
static void
startup_load_ordered(CuvsLoadKey *keys, int n, const CuvsUsageStat *st,
                     int n_st, int *loaded, int *skipped)
{
    if (n <= 0)
        return;
    int *perm = calloc((size_t)n, sizeof(int));
    if (!perm)
        return;
    cuvs_startup_order(keys, n, st, n_st, perm);
    for (int i = 0; i < n; i++)
    {
        const CuvsLoadKey *k = &keys[perm[i]];
        if (find_index(k->db_oid, k->index_oid))
            continue;
        if (load_index(k->db_oid, k->index_oid, CUVS_RQ_LOAD_STARTUP) == 0)
            (*loaded)++;
        else
        {
            (*skipped)++;
            LOG_INFO("[startup] %u/%u not loaded (no room without eviction or "
                     "invalid files); first query reloads it\n",
                     k->db_oid, k->index_oid);
        }
    }
    free(perm);
}

/* Collect "<db>_<idx>.<ext>" keys of one pass. */
static int
startup_collect(const char *ext, CuvsLoadKey **keys_out)
{
    DIR *dir = opendir(g_index_dir);
    int n = 0, cap = 64;
    CuvsLoadKey *keys = malloc(sizeof(CuvsLoadKey) * (size_t)cap);
    *keys_out = keys;
    if (!dir || !keys)
    {
        if (dir) closedir(dir);
        return 0;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL)
    {
        uint32_t db_oid, index_oid;
        int ok;
        if (strcmp(ext, "cagra") == 0)
            /* "<db_oid>_<index_oid>.cagra" only (rejects .tids and shard
             * artifacts). See cuvs_parse_index_filename. */
            ok = cuvs_parse_index_filename(ent->d_name, &db_oid, &index_oid) == 0;
        else
        {
            char tail[16] = {0};
            ok = sscanf(ent->d_name, "%u_%u.%15s", &db_oid, &index_oid, tail) == 3
                 && strcmp(tail, ext) == 0;
        }
        if (!ok)
            continue;
        if (n == cap)
        {
            CuvsLoadKey *nk = realloc(keys, sizeof(CuvsLoadKey) * (size_t)cap * 2);
            if (!nk)
                break;
            keys = nk;
            cap *= 2;
        }
        keys[n].db_oid = db_oid;
        keys[n].index_oid = index_oid;
        n++;
    }
    closedir(dir);
    *keys_out = keys;
    return n;
}

/* Scan index_dir on startup and load valid indexes without evicting any
 * (LOAD_STARTUP). The three passes keep their order (.cagra, .shards,
 * .vectors); inside a pass the previous run's usage stats decide (hot
 * first), else (db_oid, index_oid) ascending, so the resident set depends on
 * the order key and capacity only, never on readdir order (CK-36 (e)). */
static void
startup_load_indexes(void)
{
    mkdir(g_index_dir, 0700);

    CuvsUsageStat *st = NULL;
    int n_st = -1;
    char why[128] = "usage stats file disabled";
    if (g_reclaim.usage_file)
    {
        st = calloc(CUVS_USAGE_STATS_MAX, sizeof(CuvsUsageStat));
        if (st)
            n_st = usage_stats_load(st, CUVS_USAGE_STATS_MAX, why, sizeof(why));
    }
    if (n_st >= 0)
        LOG_INFO("[startup] load order: usage stats file (%d entries), hot first\n", n_st);
    else
        LOG_INFO("[startup] load order: (db_oid, index_oid) ascending (%s)\n", why);

    /* Phase 3F: sharded indexes have no base "<db>_<idx>.cagra" — discover
     * them via the ".shards" manifest. ADR-073: flat indexes have only
     * ".vectors" + ".tids". load_index branches on the files present. */
    static const char *passes[] = { "cagra", "shards", "vectors" };
    int loaded = 0, skipped = 0;
    for (int p = 0; p < 3; p++)
    {
        CuvsLoadKey *keys = NULL;
        int n = startup_collect(passes[p], &keys);
        startup_load_ordered(keys, n, st, n_st, &loaded, &skipped);
        free(keys);
    }
    free(st);
    LOG_INFO("[startup] loaded %d index(es), %d left not resident\n", loaded, skipped);

    DIR *dir;
    struct dirent *ent;

    /* Phase 3D: second pass — for indexes that have a .relfilenode sidecar
     * but no local .cagra, register as COLD and enqueue background download.
     * The daemon opens its UDS socket before these downloads complete, so
     * queries for COLD indexes get NOT_FOUND -> CPU fallback until warmup
     * finishes. */
    if (g_snapshot_uri[0] == '\0')
        return;

    dir = opendir(g_index_dir);
    if (!dir)
        return;

    while ((ent = readdir(dir)) != NULL)
    {
        uint32_t db_oid, index_oid;
        if (sscanf(ent->d_name, "%u_%u.relfilenode", &db_oid, &index_oid) != 2)
            continue;

        int already_loaded = 0;
        for (int i = 0; i < g_n_indexes; i++)
        {
            if (g_indexes[i].valid &&
                g_indexes[i].db_oid    == db_oid &&
                g_indexes[i].index_oid == index_oid)
            {
                already_loaded = 1;
                break;
            }
        }
        if (already_loaded)
            continue;

        uint32_t local_rfn = 0, local_table_oid = 0;
        read_relfilenode_sidecar(g_index_dir, db_oid, index_oid,
                                 &local_rfn, &local_table_oid);

        if (g_n_cold_indexes < g_max_indexes)
        {
            ColdIndexEntry *ce = &g_cold_indexes[g_n_cold_indexes++];
            ce->db_oid            = db_oid;
            ce->index_oid         = index_oid;
            ce->relfilenode       = local_rfn;
            ce->table_oid         = local_table_oid;
            ce->warmup_state      = WARMUP_QUEUED;
            ce->last_warmup_at    = 0;
            ce->warmup_duration_ms = 0;
            ce->download_count    = 0;
            ce->cache_miss_count  = 0;
            ce->valid             = 1;
        }

        warmup_enqueue(db_oid, index_oid, local_rfn, local_table_oid, 0);
        LOG_INFO("warmup: enqueued background download for %u/%u\n",
                 db_oid, index_oid);
    }
    closedir(dir);
}

/* ----------------------------------------------------------------
 * LRU eviction
 * ---------------------------------------------------------------- */
/* Is `e` (partly) resident on device_id? Unsharded: its single GPU. Sharded: any
 * of its shards (Phase 3G.4). */
static int
entry_on_device(const IndexEntry *e, int device_id)
{
    if (e->shard_count >= 2)
    {
        for (int s = 0; s < e->shard_count; s++)
            if (e->shards[s].valid &&
                e->shards[s].gpu_device_id == (uint32_t)device_id)
                return 1;
        return 0;
    }
    return e->gpu_device_id == (uint32_t)device_id;
}

/* ----------------------------------------------------------------
 * I3-B cache policy state (all under g_index_mutex unless noted).
 * ---------------------------------------------------------------- */
static CuvsReclaimParams g_reclaim;               /* flags + pg_cuvs_reclaim_set (tentative) */
static char     g_reclaim_src[16][16];            /* per runtime knob: flag/function */
static time_t   g_reclaim_changed_at[16];
static CuvsAdmitLog g_admit_log;                  /* BUILD_* verdicts driving F */
static uint64_t g_evict_by_reason[CUVS_MAX_GPUS][CUVS_EVICT__COUNT];
static uint64_t g_cache_drops;                    /* DROP INDEX cleanups (not evictions) */
static uint64_t g_reclaim_runs[CUVS_MAX_GPUS];
static uint64_t g_reclaim_skip_sidecar[CUVS_MAX_GPUS];
static time_t   g_last_reclaim_at[CUVS_MAX_GPUS];
static CuvsReclaimTrigger g_last_reclaim_trigger[CUVS_MAX_GPUS];
static CuvsFreeTarget g_reclaim_f[CUVS_MAX_GPUS];

static uint64_t
mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* Eviction history (C8): survives the registry entry, dropped with DROP
 * INDEX, bounded; oldest key dropped first (history_dropped). */
#define CUVS_EVICT_HISTORY_MAX 65536
typedef struct EvictHistory {
    uint32_t db_oid, index_oid;
    uint64_t evict_count;
    time_t   last_evict_at;
    int      last_evict_reason;      /* CuvsEvictReason, -1 none */
    int      last_build_self_replace;/* -1 no build recorded */
    uint64_t last_build_unloaded;
    uint64_t seq;                    /* insertion order for eviction */
} EvictHistory;
static EvictHistory *g_evict_hist;
static int      g_n_evict_hist;
static uint64_t g_evict_hist_seq;
static uint64_t g_history_dropped;

static EvictHistory *
evict_history(uint32_t db_oid, uint32_t index_oid, int create)
{
    for (int i = 0; i < g_n_evict_hist; i++)
        if (g_evict_hist[i].db_oid == db_oid && g_evict_hist[i].index_oid == index_oid)
            return &g_evict_hist[i];
    if (!create)
        return NULL;
    if (!g_evict_hist)
    {
        g_evict_hist = calloc(CUVS_EVICT_HISTORY_MAX, sizeof(EvictHistory));
        if (!g_evict_hist)
            return NULL;
    }
    int slot = g_n_evict_hist;
    if (g_n_evict_hist == CUVS_EVICT_HISTORY_MAX)
    {
        slot = 0;
        for (int i = 1; i < g_n_evict_hist; i++)
            if (g_evict_hist[i].seq < g_evict_hist[slot].seq)
                slot = i;
        g_history_dropped++;
    }
    else
        g_n_evict_hist++;
    EvictHistory *h = &g_evict_hist[slot];
    memset(h, 0, sizeof(*h));
    h->db_oid = db_oid;
    h->index_oid = index_oid;
    h->last_evict_reason = -1;
    h->last_build_self_replace = -1;
    h->seq = ++g_evict_hist_seq;
    return h;
}

static void
evict_history_drop(uint32_t db_oid, uint32_t index_oid)
{
    EvictHistory *h = evict_history(db_oid, index_oid, 0);
    if (!h)
        return;
    *h = g_evict_hist[--g_n_evict_hist];
}

/* Registry slot for a NEW entry: the slot may hold a shifted copy of another
 * index, so per-index policy state must not leak (pin, in-use count). */
static IndexEntry *
registry_new_slot(void)
{
    IndexEntry *e = &g_indexes[g_n_indexes++];
    /* After compaction this slot holds a byte copy of the entry shifted
     * down: never inherit its owned pointers (the CAGRA reload path does
     * not assign them all; a stale hnsw_idx is a wrong-result + UAF). */
    e->hnsw_idx     = NULL;
    e->rev_tids     = NULL;
    e->rev_item_ids = NULL;
    e->pinned       = 0;
    e->inflight     = 0;
    e->file_current = 0;
    e->hits         = 0;
    e->last_search  = 0;
    e->loaded_at    = time(NULL);
    return e;
}

/* Bytes evicting `e` frees on device_id: the same terms as total_vram_used. */
static size_t
entry_bytes_on(const IndexEntry *e, int device_id)
{
    size_t b = 0;
    if (e->shard_count >= 2)
    {
        for (int s = 0; s < e->shard_count; s++)
            if (e->shards[s].valid &&
                e->shards[s].gpu_device_id == (uint32_t)device_id)
                b += e->shards[s].vram_bytes + e->shards[s].bf_vram_bytes;
        return b;
    }
    if (e->gpu_device_id == (uint32_t)device_id)
        b = e->vram_bytes + e->delta_vram_bytes + e->main_bf_vram_bytes;
    return b;
}

/* EV-5 resident_bytes: registry only, build reservations excluded (R1). */
static size_t
resident_bytes(int device_id)
{
    size_t t = 0;
    for (int i = 0; i < g_n_indexes; i++)
        if (g_indexes[i].valid)
            t += entry_bytes_on(&g_indexes[i], device_id);
    return t;
}

/* Effective budget the reclaim fences use: min(logical budget, quota). */
static size_t
budget_eff_bytes(int device_id)
{
    size_t b = g_max_vram_per_gpu[device_id];
    size_t q = g_rmm_pool_limits[device_id];
    if (q > 0 && (b == 0 || q < b))
        b = q;
    return b;
}

static int
entry_is_cagra_single(const IndexEntry *e)
{
    return e->shard_count < 2 && e->handle != NULL && !e->ivfpq_handle && !e->is_flat;
}

static CuvsEvictParams
evict_params_now(void)
{
    CuvsEvictParams p = { g_reclaim.t_min_s, g_reclaim.bonus_s, g_reclaim.hits_cap };
    return p;
}

static CuvsEvictCand
entry_cand(const IndexEntry *e, int device_id)
{
    CuvsEvictCand c;
    c.db_oid    = e->db_oid;
    c.index_oid = e->index_oid;
    c.last_use  = (int64_t)e->last_search;
    c.loaded_at = (int64_t)e->loaded_at;
    c.hits      = e->hits;
    c.pinned    = e->pinned;
    c.in_use    = e->inflight;
    c.bytes     = entry_bytes_on(e, device_id);
    return c;
}

/* Bind C5/C8 observations to the same B policy state; no policy copy. */
static void
admission_cache_totals(int dev, AdmissionCacheTotals *out)
{
    CuvsEvictParams params = evict_params_now();
    int64_t now = (int64_t)time(NULL);
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < g_n_indexes; i++)
    {
        IndexEntry *e = &g_indexes[i];
        if (!e->valid || !entry_on_device(e, dev))
            continue;
        CuvsEvictCand c = entry_cand(e, dev);
        if (c.pinned)
        {
            out->pinned += c.bytes;
            out->pinned_count++;
        }
        else if (c.in_use)
        {
            out->busy += c.bytes;
            out->busy_count++;
        }
        if (!cuvs_evict_protected(&c))
        {
            out->lru += c.bytes;
            out->lru_count++;
        }
        if (cuvs_evict_eligible(&c, now, &params))
        {
            out->eligible += c.bytes;
            if (!cuvs_reclaim_skip_sidecar(cuvs_entry_sidecar_kind(g_index_dir,
                    e->db_oid, e->index_oid), entry_is_cagra_single(e), e->file_current))
                out->reclaimable += c.bytes;
        }
    }
}

/* Free `e` (index into g_indexes is compacted). Returns bytes freed on
 * device_id, or 0 with *persist_failed = 1 when its save failed (the entry
 * stays resident). Writes the [evict] line and the counters (EV-7). */
static size_t
evict_entry(IndexEntry *e, int device_id, CuvsEvictReason why, int self_replace,
            CuvsRequester rq, int *persist_failed)
{
    CuvsEvictParams pp = evict_params_now();
    CuvsEvictCand   c = entry_cand(e, device_id);
    time_t now = time(NULL);
    int eligible = cuvs_evict_eligible(&c, (int64_t)now, &pp);
    int saved = 0;
    uint64_t saved_ms = 0;
    uint32_t db = e->db_oid, idx = e->index_oid;
    size_t freed;

    if (persist_failed)
        *persist_failed = 0;
    freed = entry_bytes_on(e, device_id);

    if (e->shard_count >= 2)
    {
        /* Sharded: every artifact (.tids/.shards/.sNNN.cagra/.delta/.tombstone)
         * is already durable on disk, so eviction needs no save — just free the
         * whole logical index (all shards across their GPUs) as a unit. A later
         * query reloads it from the .shards manifest. `freed` counts only this
         * device's shards; freeing also releases the index's other shards. */
        free_delta_cache(e);
        free_main_bf_cache(e);
        free_index_shards(e);
        free(e->tids);
        free(e->rev_tids);     e->rev_tids     = NULL;
        free(e->rev_item_ids); e->rev_item_ids = NULL;
    }
    else if (e->ivfpq_handle)
    {
        /* 3P IVF-PQ: `.ivfpq`/`.tids` are durable on disk, so eviction needs
         * no save (save_index assumes a CAGRA handle: ADR-070). The
         * ivfpq_handle itself is freed by the shared cleanup below. */
        free_delta_cache(e);
        free_main_bf_cache(e);
        free(e->tids);
        free(e->rev_tids);     e->rev_tids     = NULL;
        free(e->rev_item_ids); e->rev_item_ids = NULL;
    }
    else if (e->is_flat)
    {
        /* ADR-073: flat's .tids + .vectors are durable on disk; MUST NOT call
         * save_index (handle == NULL -> refuses -> un-evictable). */
        free_delta_cache(e);
        free_main_bf_cache(e);
        free(e->tids);
        free(e->rev_tids);     e->rev_tids     = NULL;
        free(e->rev_item_ids); e->rev_item_ids = NULL;
    }
    else
    {
        /* I-FC (v6 C1 impact 3): the files already equal the graph only when
         * file_current == 1; then the save is skipped. This is the ONLY place
         * the skip is decided (SIGTERM always saves). */
        if (!e->file_current)
        {
            uint64_t t0 = mono_us();
            int src = save_index(e);
            saved_ms = (mono_us() - t0) / 1000u;
            if (src != 0) {
                LOG_ERROR("[evict] save_index FAILED for %u/%u on GPU %u; skipping victim "
                          "(VRAM still holds index)\n",
                        e->db_oid, e->index_oid, e->gpu_device_id);
                g_cache_persist_fail[device_id]++;
                if (persist_failed)
                    *persist_failed = 1;
                return 0;
            }
            saved = 1;
        }
        free_delta_cache(e);
        free_main_bf_cache(e);
        cuvs_cagra_free(e->handle, e->gpu_device_id);
        free(e->tids);
        free(e->rev_tids);     e->rev_tids     = NULL;
        free(e->rev_item_ids); e->rev_item_ids = NULL;
    }

    /* Phase 3I-1: free CPU HNSW sidecar (RAM-only; no VRAM). */
    if (e->hnsw_idx) { cuvs_hnsw_free(e->hnsw_idx); e->hnsw_idx = NULL; }
    /* 3P: free IVF-PQ GPU index. */
    if (e->ivfpq_handle) { cuvs_ivfpq_free(e->ivfpq_handle, e->gpu_device_id); e->ivfpq_handle = NULL; }

    /* ADR-086 [evict] schema (13): op gpu db idx reason requester_idx idle_s
     * protect_s hits vram_bytes saved_ms mono_us self_replace. idle_s = -1
     * for a never-searched victim. op = requester kind. */
    LOG_INFO("[evict] op=%s gpu=%d db=%u idx=%u reason=%s requester_idx=%u idle_s=%lld "
             "protect_s=%lld hits=%u vram_bytes=%zu saved_ms=%llu mono_us=%llu "
             "self_replace=%d\n",
             cuvs_requester_name(rq), device_id, db, idx, cuvs_evict_reason_name(why),
             (rq == CUVS_RQ_RECLAIM || rq == CUVS_RQ_MANUAL) ? 0u : g_cur_requester_idx,
             c.last_use ? (long long)(now - c.last_use) : -1LL,
             (long long)cuvs_evict_protect_s(c.hits, pp.t_min_s, pp.bonus_s, pp.hits_cap),
             c.hits, freed, (unsigned long long)saved_ms,
             (unsigned long long)mono_us(), self_replace);
    (void) eligible;
    (void) saved;

    if (!self_replace && rq != CUVS_RQ_RECLAIM && rq != CUVS_RQ_MANUAL)
        record_eviction_notice(db, idx, c.last_use ? (int64_t)(now - c.last_use) : -1,
            cuvs_evict_protect_s(c.hits, pp.t_min_s, pp.bonus_s, pp.hits_cap), why);

    int idx_pos = (int)(e - g_indexes);
    for (int i = idx_pos; i < g_n_indexes - 1; i++)
        g_indexes[i] = g_indexes[i+1];
    g_n_indexes--;

    g_cache_evictions[device_id]++;
    g_evict_by_reason[device_id][why]++;
    {
        EvictHistory *h = evict_history(db, idx, 1);
        if (h)
        {
            h->evict_count++;
            h->last_evict_at = now;
            h->last_evict_reason = (int)why;
        }
    }
    return freed;
}

/* Keys a make-room pass must not pick again (their save failed). */
typedef struct EvictSkip { uint32_t db_oid, index_oid; } EvictSkip;

static int
evict_skipped(const EvictSkip *sk, int n, const IndexEntry *e)
{
    for (int i = 0; i < n; i++)
        if (sk[i].db_oid == e->db_oid && sk[i].index_oid == e->index_oid)
            return 1;
    return 0;
}

/*
 * Free `need` bytes on device_id for requester rq under the C1 RQ table:
 * one pre-summed pick (nothing evicted on a reject), then victims in order.
 * A victim whose save fails is skipped and the pick is redone for the
 * remainder (EV-3/EV-4 save-failure rule). slot_mode treats every entry as
 * 1 "byte" so the same rules choose a registry slot (reason `slot`).
 * Returns 0 when satisfied, -1 otherwise; *pick_out gets the last pick.
 */
static int
make_room(int device_id, CuvsRequester rq, uint64_t need, int slot_mode,
          CuvsEvictPick *pick_out)
{
    EvictSkip skip[64];
    int n_skip = 0;
    CuvsEvictPick dummy;
    if (!pick_out)
        pick_out = &dummy;
    memset(pick_out, 0, sizeof(*pick_out));

    for (int round = 0; round < 64 && need > 0; round++)
    {
        int n = 0;
        CuvsEvictCand *cands = calloc((size_t)(g_n_indexes > 0 ? g_n_indexes : 1), sizeof(CuvsEvictCand));
        int *victims = calloc((size_t)(g_n_indexes > 0 ? g_n_indexes : 1), sizeof(int));
        CuvsEvictReason *reasons = calloc((size_t)(g_n_indexes > 0 ? g_n_indexes : 1), sizeof(CuvsEvictReason));
        EvictSkip *keys = calloc((size_t)(g_n_indexes > 0 ? g_n_indexes : 1), sizeof(EvictSkip));
        if (!cands || !victims || !reasons || !keys)
        {
            free(cands); free(victims); free(reasons); free(keys);
            return -1;
        }
        for (int i = 0; i < g_n_indexes; i++)
        {
            IndexEntry *e = &g_indexes[i];
            if (!e->valid || !entry_on_device(e, device_id) || evict_skipped(skip, n_skip, e))
                continue;
            cands[n] = entry_cand(e, device_id);
            if (slot_mode)
                cands[n].bytes = 1;
            else if (cands[n].bytes == 0)
                continue;   /* evicting it frees no VRAM on this device */
            keys[n].db_oid = e->db_oid;
            keys[n].index_oid = e->index_oid;
            n++;
        }
        CuvsEvictRequest req;
        memset(&req, 0, sizeof(req));
        req.rq = rq;
        req.scope = g_reclaim.scope;
        req.soft_evict = g_reclaim.soft_evict;
        req.need = need;
        req.now = (int64_t)time(NULL);
        CuvsEvictParams pp = evict_params_now();
        cuvs_evict_pick(cands, n, &pp, &req, victims, reasons, pick_out);
        if (pick_out->verdict != CUVS_EVICT_OK)
        {
            free(cands); free(victims); free(reasons); free(keys);
            return -1;
        }
        int failed = 0;
        for (int v = 0; v < pick_out->n_victims; v++)
        {
            EvictSkip k = keys[victims[v]];
            IndexEntry *e = find_index(k.db_oid, k.index_oid);
            int pf = 0;
            if (!e)
                continue;
            CuvsEvictReason why = reasons[v];
            if (slot_mode && why != CUVS_EVICT_SOFT)
                why = CUVS_EVICT_SLOT;
            uint64_t unit = slot_mode ? 1 : cands[victims[v]].bytes;
            if (evict_entry(e, device_id, why, 0, rq, &pf) == 0 && pf)
            {
                /* Stop here and re-pick the remainder: victims not yet
                 * evicted stay candidates, and a re-pick that rejects leaves
                 * them resident (nothing evicted on a reject). */
                if (n_skip < 64)
                    skip[n_skip++] = k;
                failed = 1;
                break;
            }
            need = unit >= need ? 0 : need - unit;
        }
        free(cands); free(victims); free(reasons); free(keys);
        if (!failed)
            return need == 0 ? 0 : -1;
    }
    return need == 0 ? 0 : -1;
}

/* Registry slot cap (soft LRU working-set cap). Returns 1 if a slot is free
 * afterwards. BUILD_* with scope none never evicts (registry-full fallback). */
static int
make_slot(int device_id, CuvsRequester rq)
{
    if (g_n_indexes < g_max_indexes)
        return 1;
    (void) make_room(device_id, rq, 1, 1, NULL);
    return g_n_indexes < g_max_indexes;
}

/* Evict one victim the rules allow for rq on device_id; bytes freed or 0.
 * Kept for the post-admission OOM retry loops I3-A removes: with build
 * scope none it frees nothing, so those loops no longer evict residents. */
static size_t
evict_lru(int device_id, CuvsRequester rq)
{
    size_t before = resident_bytes(device_id);
    if (make_room(device_id, rq, 1, 0, NULL) != 0)
        return 0;
    size_t after = resident_bytes(device_id);
    return before > after ? before - after : 0;
}

/* Call under g_index_mutex, immediately after successful admission and before
 * unlocking for a build; release under the same mutex on success or failure. */
static void
reserve_build_vram(size_t needed, int device_id)
{
    g_pending_build_vram[device_id] += needed;
    g_pending_build_count[device_id]++;
    if (g_rmm_pool_limits[device_id] > 0)
        g_pending_rmm_build_vram[device_id] += needed;
}

static void
release_build_vram(size_t needed, int device_id)
{
    g_pending_build_vram[device_id] -= needed;
    if (g_pending_build_count[device_id] > 0)
        g_pending_build_count[device_id]--;
    if (g_rmm_pool_limits[device_id] > 0)
        g_pending_rmm_build_vram[device_id] -= needed;
}

/* Map a failed pick to the provisional reject reason (I3-A owns the final
 * AR classifier and the err_out payload). */
static int
pick_reason(const CuvsEvictPick *pk, int physical_phase)
{
    if (pk->verdict == CUVS_EVICT_AR5)
        return CUVS_ADMISSION_AR5;
    return physical_phase ? CUVS_ADMISSION_AR4 : CUVS_ADMISSION_AR3;
}

/* Ensure there is enough VRAM on `device_id` for `needed` bytes, evicting
 * only what the requester's rules allow (v6 C1 RQ table). The requester is
 * mandatory: a caller that omits it does not compile (CK-36 (a)). */
static int
ensure_vram(size_t needed, int device_id, CuvsRequester rq, CuvsAdmissionFields *rej,
            AdmitLine *line)
{
    size_t budget = g_max_vram_per_gpu[device_id];
    size_t quota = g_rmm_pool_limits[device_id];
    size_t pending = quota > 0 ? g_pending_rmm_build_vram[device_id] : 0;
    size_t beff = budget_eff_bytes(device_id);
    if (line && !line->cache_valid)
    {
        admission_cache_totals(device_id, &line->cache);
        line->cache_valid = 1;
    }
    /* UC-1 6b / r18 B-9: a reload softly evicts even when another build's
     * pending reservation is what makes it short. */
    int soft_load = (rq == CUVS_RQ_LOAD || rq == CUVS_RQ_LOAD_SHARD) &&
                    g_reclaim.soft_evict;
    CuvsEvictPick pk;
    memset(&pk, 0, sizeof(pk));

    g_last_admit_reason = CUVS_ADMISSION_NONE;
    /* Cannot fit in any state: evict nothing (AR0). */
    if (beff > 0 && needed > beff)
    {
        g_last_admit_reason = CUVS_ADMISSION_AR0;
        goto admission_rejected;
    }
    /* Reject impossible concurrent reservations before touching healthy LRUs.
     * Allocation may already reflect part of pending: retaining it until the
     * build returns deliberately makes admission conservative, never looser. */
    if (quota > 0 && (pending > quota || needed > quota - pending))
    {
        g_last_admit_reason = CUVS_ADMISSION_AR1;
        goto admission_rejected;
    }
    /* Both shortfalls shrink together when an index is evicted, so one
     * pre-summed pick covers the larger of them: no partial eviction followed
     * by a reject from a later check (ADR-086 "nothing evicted on reject"). */
    size_t used = total_vram_used(device_id);
    uint64_t budget_short = (budget > 0 && used + needed > budget) ? used + needed - budget : 0;
    size_t fr = gpu_free_vram_bytes(device_id);
    uint64_t phys_short = needed > fr ? needed - fr : 0;
    /* An unlocked builder can allocate between observations: do not evict on
     * that transient pressure, except for a soft reload (UC-1 6b, r18 B-9). */
    if (phys_short > 0 && pending > 0 && !soft_load)
    {
        g_last_admit_reason = CUVS_ADMISSION_AR2;
        goto admission_rejected;
    }
    /* AR6/7 consume the original pre-eviction observation once per build.
     * A self-replace second admission reuses it, never a more permissive one. */
    if (rq == CUVS_RQ_BUILD_CAGRA && line)
    {
        CuvsAdmissionReason wr;
        if (!line->workspace_valid)
        {
            wr = lw_admission_check(device_id, needed, line->n, line->dim,
                                     (int)line->igd, &line->workspace);
            line->workspace_valid = 1;
        }
        else
            wr = cuvs_admission_workspace_check(&line->workspace);
        /* AR8 (ADR-089): the pool must hold N_m plus large_workspace plus
         * headroom. A premise about the pool, so it is decided before AR6/7
         * and before make_room: a refusal evicts and reserves nothing. */
        line->pool_need = cuvs_admission_pool_need(needed, line->workspace.lw_ub);
        line->pool_headroom = cuvs_admission_build_headroom(line->pool_need,
                                                            g_rmm_build_headroom_permille);
        line->pool_pending = pending;
        line->pool_valid = 1;
        if (cuvs_admission_pool_refuses(quota, pending, line->pool_need,
                                        line->pool_headroom))
        {
            g_last_admit_reason = CUVS_ADMISSION_AR8;
            goto admission_rejected;
        }
        if (wr != CUVS_ADMISSION_NONE)
        {
            g_last_admit_reason = wr;
            goto admission_rejected;
        }
    }
    uint64_t need = budget_short > phys_short ? budget_short : phys_short;
    if (need > 0 && make_room(device_id, rq, need, 0, &pk) != 0)
    {
        g_last_admit_reason = pick_reason(&pk, phys_short > budget_short);
        goto admission_rejected;
    }
    if (budget > 0 && total_vram_used(device_id) + needed > budget)
    {
        g_last_admit_reason = CUVS_ADMISSION_AR3;
        goto admission_rejected;
    }
    if (needed > gpu_free_vram_bytes(device_id))
    {
        g_last_admit_reason = CUVS_ADMISSION_AR4;
        goto admission_rejected;
    }
    return 0;
admission_rejected:
    if (rej)
    {
        admission_fill(rej, (CuvsAdmissionReason)g_last_admit_reason,
                       needed, device_id);
        if (line && line->workspace_valid)
            admission_workspace_fields(rej, &line->workspace);
        if (line)
            admission_pool_fields(rej, line, device_id);
        if (g_last_admit_reason == CUVS_ADMISSION_AR5 &&
            (rq == CUVS_RQ_LOAD || rq == CUVS_RQ_LOAD_SHARD))
        {
            AdmissionCacheTotals totals;
            admission_cache_totals(device_id, &totals);
            rej->present &= ~(CUVS_ADM_F_S | CUVS_ADM_F_R);
            rej->present |= CUVS_ADM_F_W;
            rej->w = pk.wait;
            rej->e_mib = cuvs_admission_mib(totals.pinned + totals.busy);
            rej->e_cnt = cuvs_admission_count(totals.pinned_count + totals.busy_count);
        }
    }
    return -1;
}

static void note_self_replace_failure(uint32_t index_oid, uint64_t self_unloaded);

/* NOTICE text for REINDEX self-replace (decision UC-1 2a, exact wording).
 * Delivered through I3-A's notice_out; until that lands it is logged. */
static __thread char g_build_notice[512];
static __thread char g_build_notice_detail[512];
static __thread char t_lw_notice[512];
static __thread uint32_t t_notice_index_oid;
static __thread uint64_t t_notice_self_unloaded;
static __thread int t_self_replace_failure_noted;

static void
clear_build_notices(void)
{
    g_build_notice[0] = g_build_notice_detail[0] = t_lw_notice[0] = '\0';
    t_evict_notices[0] = '\0';
    t_notice_index_oid = 0;
    t_notice_self_unloaded = 0;
    t_self_replace_failure_noted = 0;
}


/*
 * BUILD_CAGRA admission with REINDEX self-replace (UC-1 2a, KB-26): when the
 * build does not fit, the same (db, index) resident copy on this GPU counts
 * as free (self_free) if it is not pinned or in use; the copy is then
 * unloaded (saved first unless file_current) and the build admitted. Other
 * indexes are never evicted by this path. *unloaded = bytes freed (0 = no
 * self-replace).
 */
static int
admit_build_cagra(size_t needed, int device_id, uint32_t db_oid,
                  uint32_t index_oid, uint64_t *unloaded, AdmitLine *line)
{
    *unloaded = 0;
    line->workspace_valid = 0;
    line->pool_valid = 0;
    line->cache_valid = 0;
    line->self_res = 0;
    g_build_notice[0] = '\0';
    g_build_notice_detail[0] = '\0';
    t_notice_index_oid = index_oid;
    g_cur_requester_idx = index_oid;
    if (ensure_vram(needed, device_id, CUVS_RQ_BUILD_CAGRA, NULL, line) == 0)
        return 0;
    int reason = g_last_admit_reason;
    IndexEntry *old = find_index(db_oid, index_oid);
    int has_old = old && old->valid && entry_on_device(old, device_id) &&
                  old->inflight == 0;
    uint64_t old_bytes = has_old ? entry_bytes_on(old, device_id) : 0;

    /* Shortfall without the old copy, under the same three checks as
     * ensure_vram (quota/pending are unaffected by residents). */
    size_t budget = g_max_vram_per_gpu[device_id];
    size_t used = total_vram_used(device_id);
    size_t fr = gpu_free_vram_bytes(device_id);
    uint64_t shortfall = 0;
    if (budget > 0 && used + needed > budget)
        shortfall = used + needed - budget;
    if (needed > fr && needed - fr > shortfall)
        shortfall = needed - fr;
    if (reason == CUVS_ADMISSION_AR0 || reason == CUVS_ADMISSION_AR1 ||
        reason == CUVS_ADMISSION_AR2 || reason == CUVS_ADMISSION_AR6 ||
        reason == CUVS_ADMISSION_AR7 || reason == CUVS_ADMISSION_AR8 ||
        cuvs_self_replace_decide(shortfall, has_old, has_old && old->pinned,
                                 old_bytes, g_reclaim.self_replace)
            != CUVS_SELF_REPLACE)
    {
        g_last_admit_reason = reason;
        return -1;
    }
    int pf = 0;
    size_t freed = evict_entry(old, device_id, CUVS_EVICT_ADMIT_BUILD, 1,
                               CUVS_RQ_BUILD_CAGRA, &pf);
    if (pf)
    {
        g_last_admit_reason = reason;
        return -1;
    }
    *unloaded = freed;
    t_notice_self_unloaded = freed;
    line->self_res = freed;
    {
        char label[32];
        /* Room for the build before the old copy was counted: the smaller of
         * the logical-budget room and the physical free bytes. */
        uint64_t room = fr;
        if (budget > 0)
        {
            uint64_t lroom = budget > used ? budget - used : 0;
            if (lroom < room)
                room = lroom;
        }
        snprintf(label, sizeof(label), "index %u", index_oid);
        (void) cuvs_self_replace_notice(g_build_notice, sizeof(g_build_notice),
                                        g_build_notice_detail, sizeof(g_build_notice_detail),
                                        label, needed, room);
    }
    LOG_INFO("[self_replace] db=%u idx=%u unloaded=%zu notice=\"%s\" detail=\"%s\"\n",
             db_oid, index_oid, freed, g_build_notice, g_build_notice_detail);
    if (ensure_vram(needed, device_id, CUVS_RQ_BUILD_CAGRA, NULL, line) == 0)
        return 0;
    /* The estimate said the old copy was enough but the device disagrees:
     * the copy is gone and the build is refused. Say so (the next query
     * reloads it) and keep the receipt. */
    note_self_replace_failure(index_oid, freed);
    {
        EvictHistory *h = evict_history(db_oid, index_oid, 1);
        if (h)
        {
            h->last_build_self_replace = 1;
            h->last_build_unloaded = freed;
        }
    }
    return -1;
}

/* UC-1 2a: a build that fails after its old copy was unloaded says so and
 * that the next query reloads the old copy (notice_out text; logged until
 * I3-A's notice_out carries it). */
static void
note_self_replace_failure(uint32_t index_oid, uint64_t self_unloaded)
{
    char label[32];
    if (self_unloaded == 0 || t_self_replace_failure_noted)
        return;
    t_self_replace_failure_noted = 1;
    snprintf(label, sizeof(label), "index %u", index_oid);
    (void) cuvs_self_replace_fail_notice(g_build_notice, sizeof(g_build_notice),
                                         g_build_notice_detail, sizeof(g_build_notice_detail),
                                         label);
    LOG_INFO("[self_replace] idx=%u build_failed=1 notice=\"%s\" detail=\"%s\"\n",
             index_oid, g_build_notice, g_build_notice_detail);
}

/* Record a BUILD_* verdict for the reclaim free target F (fences F-1, F-3)
 * and the last-build receipt fields. Caller holds g_index_mutex. */
static void
note_build_admission(uint32_t db_oid, uint32_t index_oid, size_t needed,
                     int device_id, int admitted, uint64_t self_unloaded)
{
    CuvsAdmitRecord r;
    r.db_oid = db_oid;
    r.index_oid = index_oid;
    r.needed = needed;
    r.reason = admitted ? CUVS_ADMISSION_NONE : g_last_admit_reason;
    r.gpu = device_id;
    r.at = (int64_t)time(NULL);
    cuvs_admit_log_add(&g_admit_log, &r);
    if (admitted)
    {
        EvictHistory *h = evict_history(db_oid, index_oid, 1);
        if (h)
        {
            h->last_build_self_replace = self_unloaded > 0;
            h->last_build_unloaded = self_unloaded;
        }
    }
}

/* ----------------------------------------------------------------
 * I4 / ADR-088: large_workspace managed accounting. The admission formula
 * (ensure_vram, N_m) belongs to I3-A; these helpers only add the managed
 * part. Call every function here under g_index_mutex.
 * ---------------------------------------------------------------- */

/* lw_ub of one cagra build; 0 under policy error (no lw_pending is held). */
static uint64_t
lw_ub_for_build(int64_t n, uint32_t dim, int intermediate_graph_degree)
{
    if (g_cagra_build_overflow != CUVS_LW_OVERFLOW_MANAGED)
        return 0;
    return cuvs_lw_ub_bytes(cuvs_lw_shape_bytes(n, dim, intermediate_graph_degree),
                            g_managed_lw_margin_permille);
}

/* Admission-time WARN for shapes outside the LW-8 grid (C3). The [admit]
 * shape_class "+lw_extrapolated" suffix is appended by the I3-A line. */
static void
lw_warn_shape(int64_t n, uint32_t dim, int intermediate_graph_degree,
              uint32_t db_oid, uint32_t index_oid)
{
    if (g_cagra_build_overflow != CUVS_LW_OVERFLOW_MANAGED ||
        cuvs_lw_shape_checked(n, dim, intermediate_graph_degree))
        return;
    LOG_WARN("[managed_lw] %u/%u shape_class=+lw_extrapolated (n=%lld dim=%u igd=%d): "
             "lw_margin_permille=%u is not known to cover lw_peak outside the LW-8 grid\n",
             db_oid, index_oid, (long long)n, dim,
             intermediate_graph_degree > 0 ? intermediate_graph_degree : (int)CUVS_LW_DEFAULT_IGD,
             g_managed_lw_margin_permille);
}

static void
lw_pending_reserve(uint64_t lw_ub, int device_id)
{
    g_lw_pending[device_id] = cuvs_lw_sat_add(g_lw_pending[device_id], lw_ub);
}

static void
lw_pending_release(uint64_t lw_ub, int device_id)
{
    g_lw_pending[device_id] = lw_ub < g_lw_pending[device_id]
        ? g_lw_pending[device_id] - lw_ub : 0;
}

static uint64_t hnsw_host_headroom(uint64_t requested, char *error, size_t error_len);
/* host_guard = min(MemAvailable/2, each cgroup ancestor's remaining/2); 0 on a
 * cgroup v1 memory controller or any read failure (ADR-088 decision 5). */
static uint64_t
lw_host_guard_bytes(void)
{
    char error[160] = "";
    return hnsw_host_headroom(UINT64_MAX, error, sizeof(error));
}
/* AR7 reads the guard through this pointer; only unit tests repoint it. */
static uint64_t (*g_lw_host_guard_fn)(void) = lw_host_guard_bytes;

/* AR6/AR7 pre-admission (ADR-088 decisions 3 and 5): the one call the I3-A
 * admission function makes for a CAGRA build, after AR2 and before scope
 * eviction, with pre-eviction snapshots (not re-run after eviction). needed
 * is the in-pool N_m I3-A computed. Fills *ws (the [admit]/err_out inputs;
 * ws->lw_ub is what the caller reserves with lw_pending_reserve once the
 * build is admitted) and returns NONE, AR6 or AR7 from the shared hook. */
static __attribute__((unused)) CuvsAdmissionReason   /* called by the I3-A admission */
lw_admission_check(int device_id, uint64_t needed, int64_t n, uint32_t dim,
                   int intermediate_graph_degree, CuvsAdmissionWorkspaceInput *ws)
{
    CuvsRmmPoolStats pool;
    memset(ws, 0, sizeof(*ws));
    ws->managed = g_cagra_build_overflow == CUVS_LW_OVERFLOW_MANAGED;
    ws->overcommit = (uint32_t)g_managed_overcommit;
    ws->needed = needed;
    if (cuvs_rmm_pool_stats(device_id, &pool) == 0)
    {
        ws->reserved = pool.reserved_bytes;
        ws->pool_used = pool.used_bytes;
    }
    ws->phys_free = cuvs_vram_physical_free_bytes_on(device_id);
    ws->lw_ub = lw_ub_for_build(n, dim, intermediate_graph_degree);
    ws->lw_pending = g_lw_pending[device_id];
    ws->host_guard = g_lw_host_guard_fn();
    return cuvs_admission_workspace_check(ws);
}

/* Build end (success or failure), after lw_pending_release. Updates the C8
 * spill counters, logs the I4 line and returns 1 with the requester NOTICE
 * text when the build spilled (delivered through notice_out by I3-A). */
static int
lw_build_finish(const char *op, int device_id, uint32_t db_oid, uint32_t index_oid,
                uint32_t policy, uint64_t lw_ub, const CuvsLwBuildStats *st,
                char *notice, size_t notice_len)
{
    int spill = cuvs_lw_spill_flag(policy, st->lw_peak, st->free_at_peak);
    if (spill)
    {
        g_managed_spill_builds[device_id]++;
        if (st->lw_peak > g_managed_spill_bytes_max[device_id])
            g_managed_spill_bytes_max[device_id] = st->lw_peak;
    }
    LOG_INFO("[managed_lw] op=%s gpu=%d db=%u idx=%u policy=%s lw_ub=%llu lw_peak=%llu "
             "spill=%d free_at_peak=%llu lw_pending_after=%llu applied=%u restored=%u "
             "live_end=%llu\n",
             op, device_id, db_oid, index_oid, cuvs_lw_overflow_name(policy),
             (unsigned long long)lw_ub, (unsigned long long)st->lw_peak, spill,
             (unsigned long long)(st->free_at_peak == UINT64_MAX ? 0 : st->free_at_peak),
             (unsigned long long)g_lw_pending[device_id], st->applied, st->restored,
             (unsigned long long)st->lw_live_end);
    if (st->applied && !st->restored)
        LOG_WARN("[managed_lw] GPU %d: large_workspace default not restored; resources poisoned\n",
                 device_id);
    if (st->lw_live_end != 0)
        LOG_WARN("[managed_lw] GPU %d: %llu managed bytes still live at restore\n",
                 device_id, (unsigned long long)st->lw_live_end);
    if (!cuvs_lw_spill_notice(spill, st->lw_peak, st->free_at_peak, notice, notice_len))
        return 0;
    snprintf(t_lw_notice, sizeof(t_lw_notice), "%s", notice);
    LOG_INFO("[managed_lw] NOTICE %u/%u: %s\n", db_oid, index_oid, notice);
    return 1;
}

/* Pool status append (I4 keys). Takes g_index_mutex for the counters. */
static void
lw_fill_pool_status(CuvsRmmPoolStatus *status)
{
    _Static_assert(sizeof(status->oom_score_adj_result) >= CUVS_LW_OOM_RESULT_LEN,
                   "oom_score_adj_result must hold the startup line");
    status->cagra_build_overflow = g_cagra_build_overflow;
    status->managed_overcommit = (uint32_t)g_managed_overcommit;
    status->lw_margin_permille = g_managed_lw_margin_permille;
    status->oom_score_adj = g_oom_score_adj;
    status->oom_score_adj_effective = g_oom_score_adj_effective;
    snprintf(status->oom_score_adj_result, sizeof(status->oom_score_adj_result), "%s",
             g_oom_score_adj_result);
    pthread_mutex_lock(&g_index_mutex);
    for (int i = 0; i < n_usable_gpus(); i++)
    {
        int dev = usable_gpu(i);
        CuvsLwDeviceStatus *d = &status->lw_devices[status->lw_n_devices++];
        d->device_id = (uint32_t)dev;
        d->lw_pending_bytes = g_lw_pending[dev];
        d->managed_spill_builds = g_managed_spill_builds[dev];
        d->managed_spill_bytes_max = g_managed_spill_bytes_max[dev];
    }
    pthread_mutex_unlock(&g_index_mutex);
}

/* KB-4/5/6/24 parser. Returns 1 when argv[*i] is an I4 flag (consuming its
 * value), 0 when it is not, -1 on a missing or out-of-range value (startup
 * refuses). */
static int
parse_lw_flag(int argc, char **argv, int *i)
{
    const char *flag = argv[*i];
    const char *value = *i + 1 < argc ? argv[*i + 1] : NULL;
    int ok;
    if (strcmp(flag, "--cagra-build-overflow") == 0)
        ok = cuvs_lw_parse_overflow(value, &g_cagra_build_overflow) == 0;
    else if (strcmp(flag, "--managed-overcommit") == 0)
        ok = cuvs_lw_parse_on_off(value, &g_managed_overcommit) == 0;
    else if (strcmp(flag, "--managed-lw-margin-permille") == 0)
        ok = cuvs_lw_parse_margin(value, &g_managed_lw_margin_permille) == 0;
    else if (strcmp(flag, "--oom-score-adj") == 0)
        ok = cuvs_lw_parse_oom_score_adj(value, &g_oom_score_adj) == 0;
    else
        return 0;
    if (!ok)
    {
        LOG_ERROR("%s: invalid value '%s' (--cagra-build-overflow error|managed, "
                  "--managed-overcommit on|off, --managed-lw-margin-permille %u..%u, "
                  "--oom-score-adj 0..%d)\n", flag, value ? value : "",
                  CUVS_MANAGED_LW_MARGIN_PERMILLE_MIN, CUVS_MANAGED_LW_MARGIN_PERMILLE_MAX,
                  CUVS_OOM_SCORE_ADJ_MAX);
        return -1;
    }
    (*i)++;
    return 1;
}

/* KB-24 / UC-3 (d): raise the daemon's oom_score_adj once at startup so the
 * kernel OOM killer prefers it over PostgreSQL under host RAM pressure. Never
 * lowers (kept current N), never writes when the setting is 0, and a failure
 * never stops the daemon: the result text goes to the log and pool status. */
static void
apply_oom_score_adj(const char *path)
{
    int current = -1, err = 0;
    CuvsOomAdjAction action = cuvs_oom_adj_action(g_oom_score_adj, INT32_MIN);
#ifdef __linux__
    if (g_oom_score_adj != 0)
    {
        FILE *f;
        errno = 0;
        f = fopen(path, "r");
        if (!f || fscanf(f, "%d", &current) != 1)
            err = errno ? errno : EIO;
        if (f) fclose(f);
        if (!err)
        {
            action = cuvs_oom_adj_action(g_oom_score_adj, current);
            if (action == CUVS_OOM_ADJ_WRITE)
            {
                char text[16];
                int len = snprintf(text, sizeof(text), "%d", g_oom_score_adj);
                int fd = open(path, O_WRONLY);
                if (fd < 0 || write(fd, text, (size_t)len) != len)
                    err = errno ? errno : EIO;
                if (fd >= 0) close(fd);
            }
        }
        else
            action = CUVS_OOM_ADJ_WRITE;   /* describe the read failure */
    }
    g_oom_score_adj_effective = (action == CUVS_OOM_ADJ_WRITE && !err) ? g_oom_score_adj : current;
    cuvs_oom_adj_describe(action, g_oom_score_adj, current, err, 1,
                          g_oom_score_adj_result, sizeof(g_oom_score_adj_result));
#else
    (void)path;
    cuvs_oom_adj_describe(action, g_oom_score_adj, current, err, 0,
                          g_oom_score_adj_result, sizeof(g_oom_score_adj_result));
#endif
    LOG_INFO("pg_cuvs_server: %s (--oom-score-adj %d)\n", g_oom_score_adj_result, g_oom_score_adj);
}

/* ----------------------------------------------------------------
 * Index lookup
 * ---------------------------------------------------------------- */
static IndexEntry *
find_index(uint32_t db_oid, uint32_t index_oid)
{
    for (int i = 0; i < g_n_indexes; i++)
    {
        if (g_indexes[i].valid &&
            g_indexes[i].db_oid    == db_oid &&
            g_indexes[i].index_oid == index_oid)
            return &g_indexes[i];
    }
    return NULL;
}

/* Phase 3F: bump a per-GPU cache counter for every GPU a logical index
 * occupies — each shard's GPU for a sharded index, or the single GPU for an
 * unsharded one. Guards the sharded sentinel gpu_device_id (0xFFFFFFFF) so it
 * never indexes the per-GPU counter arrays out of bounds. Caller holds the
 * mutex. */
static void
cache_counter_bump(uint64_t *counters, const IndexEntry *e)
{
    if (e->shard_count >= 2)
    {
        for (int s = 0; s < e->shard_count; s++)
        {
            uint32_t g = e->shards[s].gpu_device_id;
            if (g < CUVS_MAX_GPUS)
                counters[g]++;
        }
    }
    else if (e->gpu_device_id < CUVS_MAX_GPUS)
    {
        counters[e->gpu_device_id]++;
    }
}

/* ----------------------------------------------------------------
 * Socket I/O helpers (same as client)
 * ---------------------------------------------------------------- */
static int
send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = write(fd, p + sent, len - sent);
        if (n <= 0)
            return -1;
        sent += n;
    }
    return 0;
}

static int
recv_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    size_t received = 0;
    while (received < len)
    {
        ssize_t n = read(fd, p + received, len - received);
        if (n <= 0)
            return -1;
        received += n;
    }
    return 0;
}

static void
append_notice_logged(CuvsReplyHeader *hdr, const char *msg, const char *detail)
{
    int rc;
    if (!msg || !msg[0])
        return;
    LOG_INFO("[notice_full] idx=%u message=\"%s\" detail=\"%s\"\n",
             t_notice_index_oid, msg, detail ? detail : "");
    rc = cuvs_notice_append(hdr->notice_out, sizeof(hdr->notice_out), msg, detail);
    if (rc != 0)
        LOG_WARN("[notice_overflow] idx=%u wire=\"%s\" complete_pairs=daemon_journal\n",
                 t_notice_index_oid, hdr->notice_out);
}

static void
record_eviction_notice(uint32_t db, uint32_t idx, int64_t idle,
                       int64_t protect, CuvsEvictReason why)
{
    char message[256], idle_text[48];
    if (idle < 0)
        snprintf(idle_text, sizeof(idle_text), "never");
    else
        snprintf(idle_text, sizeof(idle_text), "%lld s", (long long)idle);
    snprintf(message, sizeof(message), "pg_cuvs: evicted index %u:%u "
             "(idle %s, P %lld s, reason %s) to make room", db, idx,
             idle_text, (long long)protect, cuvs_evict_reason_name(why));
    LOG_INFO("[notice_full] db=%u idx=%u message=\"%s\" detail=\"\"\n", db, idx, message);
    (void)cuvs_notice_append(t_evict_notices, sizeof(t_evict_notices), message, "");
}

static void
attach_build_notices(CuvsReplyHeader *hdr)
{
    char existing[CUVS_NOTICE_OUT_BYTES];
    snprintf(existing, sizeof(existing), "%s", hdr->notice_out);
    snprintf(hdr->notice_out, sizeof(hdr->notice_out), "%s", t_evict_notices);
    if (hdr->status != CUVS_STATUS_OK && t_notice_self_unloaded)
        note_self_replace_failure(t_notice_index_oid, t_notice_self_unloaded);
    append_notice_logged(hdr, g_build_notice, g_build_notice_detail);
    append_notice_logged(hdr, t_lw_notice, "");
    append_notice_logged(hdr, existing, "");
}

static void
send_error_code(int client_fd, int status, const char *msg)
{
    CuvsReplyHeader hdr = {0};
    hdr.status = status;
    hdr.n_results = 0;
    strncpy(hdr.error, msg, sizeof(hdr.error) - 1);
    attach_build_notices(&hdr);
    send_all(client_fd, &hdr, sizeof(hdr));
}

static void
send_error(int client_fd, const char *msg)
{
    send_error_code(client_fd, CUVS_STATUS_ERROR, msg);
}

static const char *
admission_algo_name(uint32_t build_algo)
{
    return build_algo == CUVS_CAGRA_BUILD_IVF_PQ ? "ivf_pq"
         : build_algo == CUVS_CAGRA_BUILD_NN_DESCENT ? "nn_descent" : "auto";
}

/* [release] (9 fields), after both reservations have been released. */
static void
log_release(const char *op, int gpu, uint32_t db, uint32_t idx, size_t needed,
            const char *outcome)
{
    LOG_INFO("[release] op=%s gpu=%d db=%u idx=%u needed=%zu pending_after=%zu "
             "outcome=%s lw_pending_after=%llu mono_us=%llu\n", op, gpu, db, idx, needed,
             g_pending_rmm_build_vram[gpu], outcome, (unsigned long long)g_lw_pending[gpu],
             (unsigned long long) admission_mono_us());
}

/* [build_oom] (8 fields): after admission the build ran out of GPU memory and
 * no resident index is evicted. failed_bytes is the final failing upstream
 * growth request, not needed or the logical suballocation; unknown is -1. */
static void
log_build_oom(const char *op, int gpu, uint32_t db, uint32_t idx, size_t needed)
{
    int64_t failed_bytes = cuvs_last_failed_allocation_bytes();
    LOG_WARN("[build_oom] op=%s gpu=%d db=%u idx=%u needed=%zu action=fail_no_evict "
             "mono_us=%llu failed_bytes=%lld\n", op, gpu, db, idx, needed,
             (unsigned long long) admission_mono_us(), (long long)failed_bytes);
}

/* Reply with an admission rejection: legacy status/error plus err_out. */
static void
send_admission_reject(int client_fd, int status, const char *legacy_error,
                      const CuvsAdmissionFields *f)
{
    CuvsReplyHeader hdr = {0};
    hdr.status = (uint32_t) status;
    snprintf(hdr.error, sizeof(hdr.error), "%s", legacy_error);
    if (cuvs_admission_encode(f, hdr.err_out, sizeof(hdr.err_out)) < 0)
        hdr.err_out[0] = '\0';
    LOG_WARN("[admit_reject] status=%d err_out=\"%s\"\n", status, hdr.err_out);
    attach_build_notices(&hdr);
    send_all(client_fd, &hdr, sizeof(hdr));
}


/* Phase 3G: one shard's search, run on its own thread for parallel fanout.
 * Each shard lives on a distinct GPU and uses the per-device cuVS resource pool
 * (cuvs_wrapper.cu), which is independently locked per device — concurrent
 * dispatch across devices is safe, and two shards on the same device serialize
 * correctly on that device's pool mutex. `cudaSetDevice` is per-thread, set
 * inside cuvs_cagra_search via PooledRes, so each worker binds its own device. */
typedef struct ShardSearchArg {
    CuvsCagraIndex    handle;
    CuvsBfIndex       bf_idx;      /* Phase 3L: per-shard BF index (used when use_bf) */
    int               use_bf;      /* Phase 3L: 1 = brute_force, 0 = CAGRA */
    const float      *query;
    int               dim;
    int               sk;          /* per-shard request k (k + overfetch, clamped to n_vecs) */
    uint32_t          gpu_device_id;
    int64_t           tid_offset;  /* snapshot: global TID start of this shard */
    uint32_t          itopk_size;
    uint32_t          search_width;
    int64_t           shard_n_vecs;/* snapshot: vectors in this shard */
    CuvsSearchResult *out;         /* caller-allocated [sk]; NULL when sk==0 */
    int               rc;          /* 0 = OK (also for skipped sk==0 shards) */
    int               threaded;    /* 1 if dispatched on its own pthread */
} ShardSearchArg;

static void *
shard_search_worker(void *p)
{
    ShardSearchArg *a = (ShardSearchArg *) p;
    if (a->use_bf)
        a->rc = cuvs_bf_search(a->bf_idx, a->query, a->dim, a->sk, a->out,
                               (int) a->gpu_device_id);
    else
        a->rc = cuvs_cagra_search(a->handle, a->query, a->dim, a->sk, a->out,
                                  (int) a->gpu_device_id, a->itopk_size, a->search_width);
    return NULL;
}

/* ----------------------------------------------------------------
 * ADR-064: Streaming / out-of-core filtered brute force.
 *
 * Reuses the 3O reverse map (heapTID -> item_id) to gather ONLY the
 * filter-passing vectors from the `.vectors` sidecar via pread (item_id order,
 * random access: offset = sizeof(header) + item_id*dim*4), runs GPU brute force
 * in chunks of `cmd->n_vecs` vectors, and merges a running exact top-k. The
 * whole sidecar is never resident — only one chunk at a time. Chunk size is a
 * footprint knob only; the merged result is identical for any chunking.
 * ---------------------------------------------------------------- */

/* pread `chunk_n` rows (item_ids[0..chunk_n)) from the sidecar body into the
 * contiguous host buffer `out` (row j = item_ids[j]). 0 on success, -1 on any
 * short read / pread error. */
static int
gather_chunk_pread(int fd, uint32_t dim, const int32_t *item_ids,
                   int chunk_n, float *out)
{
    const size_t row_bytes = (size_t)dim * sizeof(float);
    for (int j = 0; j < chunk_n; j++)
    {
        off_t  off = (off_t)sizeof(CuvsVectorsHeader)
                   + (off_t)item_ids[j] * (off_t)row_bytes;
        char  *dst = (char *)(out + (size_t)j * dim);
        size_t got = 0;
        while (got < row_bytes)
        {
            ssize_t r = pread(fd, dst + got, row_bytes - got, off + (off_t)got);
            if (r <= 0)
                return -1;
            got += (size_t)r;
        }
    }
    return 0;
}

/* Insert (tid, dist) into a running top-k kept ascending by distance (smaller =
 * better, matching cuVS L2/IP-as-distance). `*pn` is the current fill (<= k). */
static void
topk_insert(CuvsResult *top, int *pn, int k, uint64_t tid, float dist)
{
    if (*pn >= k && dist >= top[k - 1].distance)
        return;                                /* not better than current worst */
    int pos = (*pn < k) ? (*pn)++ : k - 1;     /* append, or evict worst if full */
    while (pos > 0 && top[pos - 1].distance > dist)
    {
        top[pos] = top[pos - 1];
        pos--;
    }
    top[pos].tid      = tid;
    top[pos].distance = dist;
}

static void *
map_shm_readonly(const char *key, size_t bytes, uid_t expected_uid)
{
    int fd;
    struct stat st;
    void *mem;

    if (!key || key[0] == '\0' || bytes == 0)
        return MAP_FAILED;
    fd = shm_open(key, O_RDONLY, 0);
    if (fd < 0)
        return MAP_FAILED;
    if (!shm_check_peer_owner(fd, expected_uid))   /* #87 finding 1 */
    {
        close(fd);
        return MAP_FAILED;
    }
    if (fstat(fd, &st) != 0 || st.st_size < 0
        || (uint64_t)st.st_size < (uint64_t)bytes)
    {
        close(fd);
        return MAP_FAILED;
    }
    mem = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    return mem;
}

static int
search_filter_exact(IndexEntry *e, const float *query,
                    const uint64_t *filter_tids, uint32_t n_filter,
                    int k, int chunk_cap, CuvsResult *top, int *pn)
{
    int32_t *item_ids = NULL;
    int n_items = 0;
    int vfd = -1;
    float *chunkbuf = NULL;
    CuvsSearchResult *chunk_res = NULL;
    int status = CUVS_STATUS_ERROR;

    *pn = 0;
    if (!e->rev_tids || !e->rev_item_ids || e->n_vecs <= 0)
        return CUVS_STATUS_NO_VECTORS;

    item_ids = malloc((size_t)n_filter * sizeof(*item_ids));
    if (!item_ids)
        goto cleanup;

    for (uint32_t fi = 0; fi < n_filter; fi++)
    {
        uint64_t want = filter_tids[fi];
        int64_t lo = 0;
        int64_t hi = e->n_vecs - 1;

        if (fi > 0 && want == filter_tids[fi - 1])
            continue;
        while (lo <= hi)
        {
            int64_t mid = lo + (hi - lo) / 2;

            if (e->rev_tids[mid] == want)
            {
                int32_t item_id = e->rev_item_ids[mid];

                if (item_id >= 0 && (int64_t)item_id < e->n_vecs)
                    item_ids[n_items++] = item_id;
                break;
            }
            if (e->rev_tids[mid] < want)
                lo = mid + 1;
            else
                hi = mid - 1;
        }
    }

    if (n_items == 0)
    {
        status = CUVS_STATUS_OK;
        goto cleanup;
    }

    char vpath[512];
    CuvsVectorsHeader vh;
    vectors_file_path(vpath, sizeof(vpath), g_index_dir, e->db_oid, e->index_oid);
    vfd = open(vpath, O_RDONLY);
    if (vfd < 0
        || pread(vfd, &vh, sizeof(vh), 0) != (ssize_t)sizeof(vh)
        || vh.magic != CUVS_VECTORS_MAGIC
        || vh.dim != e->dim
        || vh.metric != e->metric
        || vh.n_vecs != e->n_vecs)
    {
        status = CUVS_STATUS_NO_VECTORS;
        goto cleanup;
    }

    if (chunk_cap < 1)
        chunk_cap = 1;
    int cap = chunk_cap < n_items ? chunk_cap : n_items;
    int result_cap = k < cap ? k : cap;
    chunkbuf = malloc((size_t)cap * (size_t)e->dim * sizeof(*chunkbuf));
    chunk_res = malloc((size_t)result_cap * sizeof(*chunk_res));
    if (!chunkbuf || !chunk_res)
        goto cleanup;

    for (int base = 0; base < n_items; base += cap)
    {
        int chunk_n = n_items - base < cap ? n_items - base : cap;
        int search_k = k < chunk_n ? k : chunk_n;

        if (gather_chunk_pread(vfd, e->dim, item_ids + base,
                               chunk_n, chunkbuf) != 0
            || cuvs_brute_force_search(chunkbuf, query, chunk_n, (int)e->dim,
                                       search_k, e->metric, chunk_res,
                                       delta_gpu_of(e)) != 0)
            goto cleanup;

        for (int j = 0; j < search_k; j++)
        {
            int64_t row = chunk_res[j].item_id;

            if (row >= 0 && row < chunk_n)
            {
                int32_t item_id = item_ids[base + row];

                if (item_id >= 0 && (int64_t)item_id < e->n_vecs)
                    topk_insert(top, pn, k, e->tids[item_id],
                                chunk_res[j].distance);
            }
        }
    }
    status = CUVS_STATUS_OK;

cleanup:
    if (vfd >= 0)
        close(vfd);
    free(item_ids);
    free(chunkbuf);
    free(chunk_res);
    return status;
}

static void
handle_search_stream_bf(int client_fd, const CuvsCmdFrame *cmd)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_mutex_lock(&g_index_mutex);

    IndexEntry *e = find_index(cmd->db_oid, cmd->index_oid);
    if (!e && load_index(cmd->db_oid, cmd->index_oid, CUVS_RQ_LOAD) == 0)
        e = find_index(cmd->db_oid, cmd->index_oid);
    if (!e)
    {
        pthread_mutex_unlock(&g_index_mutex);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_NOT_FOUND;
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }
    e->last_search = time(NULL);

    /* Stale (heap writes since build): defer to the backend CPU path, same as
     * the in-VRAM filtered path does. */
    if (e->stale)
    {
        record_search_stat(e, CUVS_STATUS_STALE, 0, "index stale (writes since build)");
        pthread_mutex_unlock(&g_index_mutex);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_STALE;
        strncpy(hdr.error, "index stale (writes since build)", sizeof(hdr.error) - 1);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    /* Need the 3O reverse map (heapTID -> item_id), built at build/load time.
     * Genuinely absent only if build_rev_tid_map hit a malloc failure -> the
     * backend falls back to its in-VRAM filtered path. */
    if (e->rev_tids == NULL || e->rev_item_ids == NULL || e->n_vecs <= 0)
    {
        pthread_mutex_unlock(&g_index_mutex);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_NO_VECTORS;
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }
    if (cmd->metric != e->metric || cmd->dim != e->dim)
    {
        pthread_mutex_unlock(&g_index_mutex);
        CuvsReplyHeader hdr = {0};
        hdr.status = (cmd->metric != e->metric) ? CUVS_STATUS_METRIC_MISMATCH
                                                : CUVS_STATUS_DIM_MISMATCH;
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    /* Map shm query vector. */
    size_t vec_bytes = (size_t)cmd->dim * sizeof(float);
    int qfd = shm_open(cmd->shm_key, O_RDONLY, 0);
    if (qfd < 0)
    {
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "shm_open failed");
        return;
    }
    if (!shm_check_peer_owner(qfd, g_peer_uid))   /* #87 finding 1 */
    {
        close(qfd);
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "shm owner mismatch");
        return;
    }
    float *query = mmap(NULL, vec_bytes, PROT_READ, MAP_SHARED, qfd, 0);
    close(qfd);
    if (query == MAP_FAILED)
    {
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "mmap failed");
        return;
    }

    /* Map filter TIDs. */
    uint64_t *flt_tids = NULL;
    void     *flt_mem  = MAP_FAILED;
    size_t    flt_bytes = (size_t)cmd->n_filter_tids * sizeof(uint64_t);
    if (cmd->n_filter_tids > 0)
        flt_mem = map_shm_readonly(cmd->filter_shm_key, flt_bytes, g_peer_uid);
    if (flt_mem != MAP_FAILED)
        flt_tids = (uint64_t *)flt_mem;
    if (cuvs_filter_frame_refuses(cmd->n_filter_tids, flt_tids != NULL))
    {
        munmap(query, vec_bytes);
        pthread_mutex_unlock(&g_index_mutex);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_NO_VECTORS;   /* nothing to gather */
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    int k = (int)cmd->k;
    if (k < 1)
        k = 1;
    int chunk_cap = (cmd->n_vecs > 0) ? (int)cmd->n_vecs : 1;
    uint64_t *sorted_tids = malloc(flt_bytes);
    CuvsResult *top = malloc((size_t)k * sizeof(*top));
    int pn = 0;
    int status = CUVS_STATUS_ERROR;
    if (sorted_tids && top)
    {
        memcpy(sorted_tids, flt_tids, flt_bytes);
        cuvs_u64_sort(sorted_tids, cmd->n_filter_tids);
        status = search_filter_exact(e, query, sorted_tids,
                                     cmd->n_filter_tids, k, chunk_cap,
                                     top, &pn);
    }
    if (flt_mem != MAP_FAILED)
        munmap(flt_mem, flt_bytes);
    munmap(query, vec_bytes);
    free(sorted_tids);

    if (status != CUVS_STATUS_OK)
    {
        free(top);
        if (status == CUVS_STATUS_ERROR)
            record_search_stat(e, status, 0, "stream_bf failed");
        pthread_mutex_unlock(&g_index_mutex);
        CuvsReplyHeader hdr = {0};
        hdr.status = (uint32_t)status;
        if (status == CUVS_STATUS_ERROR)
            strncpy(hdr.error, "stream_bf failed", sizeof(hdr.error) - 1);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint32_t lat = (uint32_t)((t1.tv_sec - t0.tv_sec) * 1000000 +
                              (t1.tv_nsec - t0.tv_nsec) / 1000);
    record_search_stat(e, CUVS_STATUS_OK, lat, NULL);
    e->last_requested_k = cmd->k;
    e->last_returned_k  = (uint32_t)pn;
    e->last_search_mode = 6;   /* stream_bf */
    pthread_mutex_unlock(&g_index_mutex);

    CuvsReplyHeader hdr = {0};
    hdr.status     = CUVS_STATUS_OK;
    hdr.n_results  = (uint32_t)pn;
    hdr.latency_us = lat;
    send_all(client_fd, &hdr, sizeof(hdr));
    if (pn > 0)
        send_all(client_fd, top, (size_t)pn * sizeof(CuvsResult));
    free(top);
}

/* ----------------------------------------------------------------
 * Handle SEARCH_BF_TRANSIENT command (CUVS_OP_SEARCH_BF_TRANSIENT, ADR-073 B)
 *
 * One-shot exact GPU brute force over a per-query corpus the backend supplies —
 * NO resident IndexEntry. The corpus [float32 vecs n×dim][uint64_t tids n] arrives
 * as an SCM_RIGHTS memfd (or a named shm in filter_shm_key); the query is in
 * shm_key. VRAM admission is NON-EVICTING: a transient corpus must NEVER evict a
 * resident flat/cagra index. The short search runs under g_index_mutex so
 * total_vram_used() is consistent without a reservation counter. Touches no
 * IndexEntry (no find_index/load_index/record_search_stat).
 * ---------------------------------------------------------------- */
static void
handle_search_bf_transient(int client_fd, const CuvsCmdFrame *cmd)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* Second frame: corpus memfd via SCM_RIGHTS (passed_fd), else shm by name. */
    char dir_buf[256] = {0};
    int  passed_fd = -1;
    if (cuvs_fd_recv(client_fd, dir_buf, sizeof(dir_buf), &passed_fd) < 0)
    {
        send_error(client_fd, "recv corpus frame failed");
        return;
    }

    if (cmd->n_vecs < 1 || cmd->dim == 0)
    {
        if (passed_fd >= 0) close(passed_fd);
        send_error_code(client_fd, CUVS_STATUS_ERROR, "transient BF needs >= 1 vector");
        return;
    }
    {
        size_t per_vec = (size_t)cmd->dim * sizeof(float) + sizeof(uint64_t);
        if ((size_t)cmd->n_vecs > SIZE_MAX / per_vec)
        {
            if (passed_fd >= 0) close(passed_fd);
            send_error_code(client_fd, CUVS_STATUS_ERROR, "transient BF payload overflow");
            return;
        }
    }

    size_t vec_bytes = (size_t)cmd->n_vecs * cmd->dim * sizeof(float);
    size_t tid_bytes = (size_t)cmd->n_vecs * sizeof(uint64_t);
    size_t total     = vec_bytes + tid_bytes;

    /* Map the corpus (memfd tier via SCM_RIGHTS; else named shm). */
    void *mem;
    if (passed_fd >= 0)
    {
        mem = mmap(NULL, total, PROT_READ, MAP_SHARED, passed_fd, 0);
        close(passed_fd);
    }
    else
    {
        int cfd = shm_open(cmd->filter_shm_key, O_RDONLY, 0);
        if (cfd < 0) { send_error(client_fd, "corpus shm_open failed"); return; }
        if (!shm_check_peer_owner(cfd, g_peer_uid)) { close(cfd); send_error(client_fd, "corpus shm owner mismatch"); return; }   /* #87 finding 1 */
        mem = mmap(NULL, total, PROT_READ, MAP_SHARED, cfd, 0);
        close(cfd);
    }
    if (mem == MAP_FAILED) { send_error(client_fd, "corpus mmap failed"); return; }

    const float    *vecs = (const float *)mem;
    const uint64_t *tids = (const uint64_t *)((const char *)mem + vec_bytes);

    /* Map the query shm. */
    size_t q_bytes = (size_t)cmd->dim * sizeof(float);
    int qfd = shm_open(cmd->shm_key, O_RDONLY, 0);
    if (qfd < 0) { munmap(mem, total); send_error(client_fd, "query shm_open failed"); return; }
    if (!shm_check_peer_owner(qfd, g_peer_uid)) { close(qfd); munmap(mem, total); send_error(client_fd, "query shm owner mismatch"); return; }   /* #87 finding 1 */
    float *query = mmap(NULL, q_bytes, PROT_READ, MAP_SHARED, qfd, 0);
    close(qfd);
    if (query == MAP_FAILED) { munmap(mem, total); send_error(client_fd, "query mmap failed"); return; }

    int k  = (int)cmd->k; if (k < 1) k = 1;
    int sk = (k < (int)cmd->n_vecs) ? k : (int)cmd->n_vecs;

    /* --- VRAM admission (NON-EVICTING). Hold g_index_mutex across the short
     * search so total_vram_used() stays consistent (no reservation counter). --- */
    pthread_mutex_lock(&g_index_mutex);

    size_t needed = vec_bytes;   /* corpus uploaded to the GPU as float32 */
    int    dev    = pick_gpu_for_index(needed);
    if (dev < 0
        || (g_max_vram_per_gpu[dev] > 0
            && total_vram_used(dev) + needed > g_max_vram_per_gpu[dev])
        || needed > gpu_free_vram_bytes(dev))
    {
        pthread_mutex_unlock(&g_index_mutex);
        munmap(query, q_bytes);
        munmap(mem, total);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_OOM_FALLBACK;
        snprintf(hdr.error, sizeof(hdr.error),
                 "transient BF corpus %zu MB exceeds free GPU VRAM; reduce the WHERE "
                 "selectivity, build a flat index, or use a cagra/ivfpq index",
                 needed / (1024 * 1024));
        LOG_WARN("[handle_search_bf_transient] db=%u %s\n", cmd->db_oid, hdr.error);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    CuvsSearchResult *res = malloc((size_t)sk * sizeof(CuvsSearchResult));
    CuvsResult       *out = malloc((size_t)sk * sizeof(CuvsResult));
    int rc = (!res || !out)
           ? -1
           : cuvs_brute_force_search(vecs, query, cmd->n_vecs, (int)cmd->dim,
                                     sk, cmd->metric, res, dev);

    pthread_mutex_unlock(&g_index_mutex);

    munmap(query, q_bytes);

    if (rc != 0)
    {
        free(res); free(out);
        munmap(mem, total);
        send_error(client_fd, "transient BF search failed");
        return;
    }

    /* Map corpus rows -> TIDs (tids still mapped). */
    int pn = 0;
    for (int j = 0; j < sk; j++)
    {
        int64_t row = res[j].item_id;
        if (row < 0 || row >= cmd->n_vecs)
            continue;
        out[pn].tid      = tids[row];
        out[pn].distance = res[j].distance;
        pn++;
    }
    munmap(mem, total);
    free(res);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint32_t lat = (uint32_t)((t1.tv_sec - t0.tv_sec) * 1000000 +
                              (t1.tv_nsec - t0.tv_nsec) / 1000);

    CuvsReplyHeader hdr = {0};
    hdr.status     = CUVS_STATUS_OK;
    hdr.n_results  = (uint32_t)pn;
    hdr.latency_us = lat;
    send_all(client_fd, &hdr, sizeof(hdr));
    if (pn > 0)
        send_all(client_fd, out, (size_t)pn * sizeof(CuvsResult));
    free(out);
}

/* ----------------------------------------------------------------
 * #160: finish ONE unsharded CAGRA single-query search.
 *
 * Maps the graph's item_ids to heap TIDs and, when the resident GPU delta cache
 * produced candidates, merges them into the same metric-aware distance order —
 * the post-processing the immediate path has always done. Extracted so the
 * inline dispatch in handle_search and the batch worker share one implementation
 * and cannot drift on delta semantics, id-range filtering, or the merge order.
 *
 * The id-range test (`id < 0 || id >= n`) is also what discards the TAIL
 * CONTRACT padding (CUVS_PAD_ITEM_ID == -1) that both the single-query and the
 * batch kernel write into slots the corpus could not fill.
 *
 *   base / k  — the k candidates from cuvs_cagra_search[_batch] for THIS request
 *   draw / n_dc — delta-cache candidates for this request, or NULL/0 if there is
 *                 no usable delta (absent, or its search failed — caller decides)
 *   out       — caller-owned [k]
 *
 * Returns the number of results written; sets *delta_merged_out. Caller holds
 * g_index_mutex (g_merge_metric and the entry's counters are read/written here).
 * ---------------------------------------------------------------- */
static int
cagra_finish_one(IndexEntry *e, const CuvsSearchResult *base, int k,
                 const CuvsSearchResult *draw, int n_dc,
                 CuvsResult *out, int *delta_merged_out)
{
    int n_valid = 0;
    int merged  = 0;

    if (draw && n_dc > 0)
    {
        CuvsResult *cand = malloc((size_t) (k + n_dc) * sizeof(CuvsResult));
        if (cand)
        {
            int n_total = 0;
            for (int i = 0; i < k; i++)              /* base candidates */
            {
                int64_t id = base[i].item_id;
                if (id < 0 || id >= e->n_vecs) continue;
                cand[n_total].tid      = e->tids[id];
                cand[n_total].distance = base[i].distance;
                n_total++;
            }
            for (int i = 0; i < n_dc; i++)           /* delta candidates */
            {
                int64_t id = draw[i].item_id;
                if (id < 0 || id >= e->n_delta) continue;
                cand[n_total].tid      = e->delta_tids[id];
                cand[n_total].distance = draw[i].distance;
                n_total++;
            }
            g_merge_metric = e->metric;   /* read by delta_cand_cmp under the mutex */
            qsort(cand, (size_t) n_total, sizeof(CuvsResult), delta_cand_cmp);
            n_valid = (n_total < k) ? n_total : k;
            memcpy(out, cand, (size_t) n_valid * sizeof(CuvsResult));
            merged = 1;
            e->delta_merged_count++;
            free(cand);
        }
    }

    if (!merged)
    {
        /* Base-only: no usable delta cache — map item_ids to TIDs. */
        for (int i = 0; i < k; i++)
        {
            int64_t id = base[i].item_id;
            if (id < 0 || id >= e->n_vecs) continue;
            out[n_valid].tid      = e->tids[id];
            out[n_valid].distance = base[i].distance;
            n_valid++;
        }
    }

    if (delta_merged_out)
        *delta_merged_out = merged;
    return n_valid;
}

/* ----------------------------------------------------------------
 * Handle SEARCH command
 * ---------------------------------------------------------------- */
static void
handle_search(int client_fd, const CuvsCmdFrame *cmd)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

#ifdef CUVS_TEST_HOOKS
    /* 3S test seam: delay the reply (before taking the index lock so other
     * searches are unaffected) to exercise backend cancel + daemon disconnect. */
    {
        const char *d = getenv("CUVS_FAULT_SEARCH_DELAY_MS");
        long ms = (d && d[0]) ? atol(d) : 0;
        if (ms > 0)
        {
            LOG_INFO("[handle_search] fault: delaying reply %ld ms\n", ms);
            usleep((useconds_t) ms * 1000);
        }
    }
#endif

    pthread_mutex_lock(&g_index_mutex);

    IndexEntry *e = find_index(cmd->db_oid, cmd->index_oid);
    if (e)
    {
        cache_counter_bump(g_cache_hits, e);
    }
    else
    {
        int load_rc = load_index(cmd->db_oid, cmd->index_oid, CUVS_RQ_LOAD);
        if (load_rc == 0)
        {
            e = find_index(cmd->db_oid, cmd->index_oid);
            if (e)
            {
                cache_counter_bump(g_cache_reloads, e);
                cache_counter_bump(g_cache_misses, e);
            }
        }
        else
        {
            g_cache_misses[usable_gpu(0)]++;
        }

        if (!e)
        {
            /* Distinguish VRAM exhaustion (-2) from file-not-found (-1). */
            int reply_status;
            if (load_rc == -2)
            {
                reply_status = CUVS_STATUS_OOM_FALLBACK;
            }
            else
            {
                reply_status = CUVS_STATUS_NOT_FOUND;
                /* Phase 3D: check cold registry for GCS download. */
                if (g_snapshot_uri[0] != '\0')
                {
                    for (int i = 0; i < g_n_cold_indexes; i++)
                    {
                        ColdIndexEntry *ce = &g_cold_indexes[i];
                        if (ce->valid && ce->db_oid == cmd->db_oid &&
                            ce->index_oid == cmd->index_oid)
                        {
                            ce->cache_miss_count++;
                            if (ce->warmup_state == WARMUP_COLD ||
                                ce->warmup_state == WARMUP_FAILED)
                            {
                                ce->warmup_state = WARMUP_QUEUED;
                                warmup_enqueue(ce->db_oid, ce->index_oid,
                                               ce->relfilenode, ce->table_oid, 1);
                            }
                            break;
                        }
                    }
                }
            }

            pthread_mutex_unlock(&g_index_mutex);
            CuvsReplyHeader hdr = {0};
            hdr.status = (uint32_t)reply_status;
            if (load_rc == -2 && t_reload_rej.reason != CUVS_ADMISSION_NONE &&
                cuvs_admission_encode(&t_reload_rej, hdr.err_out, sizeof(hdr.err_out)) < 0)
                hdr.err_out[0] = '\0';
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }
    }

    e->last_search = time(NULL);

    /* Stale: heap writes happened since the build, so the CAGRA graph is missing
     * rows. Don't search a stale graph — tell the backend to use the CPU path
     * (correct, current results) until a REINDEX rebuilds the graph. */
    if (e->stale)
    {
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_STALE;
        strncpy(hdr.error, "index stale (writes since build)", sizeof(hdr.error) - 1);
        record_search_stat(e, CUVS_STATUS_STALE, 0, hdr.error);
        pthread_mutex_unlock(&g_index_mutex);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    /* The metric is baked into the CAGRA graph at build time. A query carrying
     * a different opclass metric means this index was built before the
     * build-metric fix (always-L2) and must be REINDEXed — otherwise we'd
     * silently return results from a wrong-metric graph. Reject it. */
    if (cmd->metric != e->metric)
    {
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_METRIC_MISMATCH;
        snprintf(hdr.error, sizeof(hdr.error),
                 "index built with metric %u but queried with metric %u; REINDEX required",
                 e->metric, cmd->metric);
        record_search_stat(e, CUVS_STATUS_METRIC_MISMATCH, 0, hdr.error);
        pthread_mutex_unlock(&g_index_mutex);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    /* Map shm query vector */
    size_t vec_bytes = (size_t)cmd->dim * sizeof(float);
    int shm_fd = shm_open(cmd->shm_key, O_RDONLY, 0);
    if (shm_fd < 0)
    {
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "shm_open failed");
        return;
    }
    if (!shm_check_peer_owner(shm_fd, g_peer_uid))   /* #87 finding 1 */
    {
        close(shm_fd);
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "shm owner mismatch");
        return;
    }

    float *query = mmap(NULL, vec_bytes, PROT_READ, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    if (query == MAP_FAILED)
    {
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "mmap failed");
        return;
    }

    /* Reject a dimension-mismatched query BEFORE it reaches cuVS. cuVS throws
     * a RAFT failure on mismatch, and the resulting sticky CUDA error has
     * aborted the entire daemon (SIGABRT), taking down every backend. This is
     * a user error: distinct status, no CPU fallback. */
    if (cmd->dim != e->dim)
    {
        munmap(query, vec_bytes);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_DIM_MISMATCH;
        snprintf(hdr.error, sizeof(hdr.error),
                 "query dim %u does not match index dim %u", cmd->dim, e->dim);
        record_search_stat(e, CUVS_STATUS_DIM_MISMATCH, 0, hdr.error);
        pthread_mutex_unlock(&g_index_mutex);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    int k = (int)cmd->k;

    /* Phase 3L: GPU brute-force (exact) search, unsharded. The resident BF index
     * over the `.vectors` sidecar is (re)built lazily at the requested precision.
     * If it is unavailable (sidecar missing / stale generation / won't fit) we
     * fail closed with NO_VECTORS so the backend raises a clear "REINDEX to
     * enable brute_force" error rather than silently returning ANN results.
     * Delta is NOT merged here — `.vectors` already covers the full base corpus
     * exactly. Sharded BF runs in the fanout block below.
     *
     * ADR-073: a flat index (e->is_flat) FORCES this brute-force path regardless
     * of cmd->search_mode. flat has handle == NULL, so the cagra dispatch below
     * would deref NULL; the backend's flat_gettuple already sends search_mode=1,
     * and this is the daemon-side defense-in-depth that also covers any other
     * caller (e.g. a batch path that did not set search_mode). */
    if ((cmd->search_mode == 1 || e->is_flat) && e->shard_count < 2)
    {
        /* Phase 3L-9: micro-batched path. When bf_batch_wait_us>0 and the batch
         * worker is up, hand this request to the worker instead of dispatching
         * inline: release g_index_mutex (lock order is index-then-bf), enqueue,
         * and block until the worker fills our result. The query shm stays mapped
         * (req.query points at it) until we reply. On a full queue we degrade to
         * the immediate path below by re-acquiring the lock and re-finding e. */
        if (cmd->bf_batch_wait_us > 0 && g_batch_worker_started
            && !(cmd->use_prefilter && cmd->n_filter_tids > 0
                 && e->handle != NULL && e->rev_tids != NULL
                 && e->rev_item_ids != NULL))
        {
            CuvsBatchKey     key  = { cmd->db_oid, cmd->index_oid, cmd->bf_precision,
                                      cmd->dim, 1 /* mode: brute_force */,
                                      0, 0 /* CAGRA tuning is unused */ };
            CuvsResult   *rout = malloc((size_t) k * sizeof(CuvsResult));
            int           enqueued = 0;
            CuvsBatchRequest req;

            pthread_mutex_unlock(&g_index_mutex);   /* release BEFORE taking g_batch_mtx */

            if (rout)
            {
                req.key     = key;   req.query  = query;  req.k = k;
                req.wait_us = cmd->bf_batch_wait_us;
                req.out     = rout;  req.n_out  = 0;
                req.status  = CUVS_STATUS_ERROR; req.done = 0;

                pthread_mutex_lock(&g_batch_mtx);
                if (g_batch_queue_n < CUVS_BATCH_MAX)
                {
                    g_batch_queue[g_batch_queue_n++] = &req;
                    enqueued = 1;
                    pthread_cond_signal(&g_batch_cond);
                    while (!req.done)                       /* spurious-wakeup safe */
                        pthread_cond_wait(&g_batch_done_cond, &g_batch_mtx);
                }
                pthread_mutex_unlock(&g_batch_mtx);
            }

            if (enqueued)
            {
                munmap(query, vec_bytes);
                CuvsReplyHeader hdr = {0};
                hdr.status = (uint32_t) req.status;
                if (req.status == CUVS_STATUS_OK)
                {
                    hdr.n_results = (uint32_t) req.n_out;
                    send_all(client_fd, &hdr, sizeof(hdr));
                    if (req.n_out > 0)
                        send_all(client_fd, rout, (size_t) req.n_out * sizeof(CuvsResult));
                }
                else
                {
                    if (req.status == CUVS_STATUS_NO_VECTORS)
                        strncpy(hdr.error,
                                "brute_force requested but the .vectors sidecar is "
                                "missing or stale; REINDEX to enable it",
                                sizeof(hdr.error) - 1);
                    send_all(client_fd, &hdr, sizeof(hdr));
                }
                free(rout);
                return;
            }

            /* Could not enqueue (queue full / malloc fail): degrade to the
             * immediate path. Re-acquire the lock and re-find the entry. */
            free(rout);
            pthread_mutex_lock(&g_index_mutex);
            e = find_index(cmd->db_oid, cmd->index_oid);
            if (!e)
            {
                munmap(query, vec_bytes);
                pthread_mutex_unlock(&g_index_mutex);
                CuvsReplyHeader hdr = {0};
                hdr.status = CUVS_STATUS_NOT_FOUND;
                send_all(client_fd, &hdr, sizeof(hdr));
                return;
            }
            /* fall through to the immediate path with e + the still-mapped query */
        }

        refresh_main_bf_cache(e, cmd->bf_precision);
        if (!e->main_bf_idx
            && !(cmd->use_prefilter && cmd->n_filter_tids > 0
                 && e->handle != NULL && e->rev_tids != NULL
                 && e->rev_item_ids != NULL))
        {
            munmap(query, vec_bytes);
            CuvsReplyHeader hdr = {0};
            hdr.status = CUVS_STATUS_NO_VECTORS;
            strncpy(hdr.error,
                    "brute_force requested but the .vectors sidecar is missing or "
                    "stale; REINDEX to enable it",
                    sizeof(hdr.error) - 1);
            record_search_stat(e, CUVS_STATUS_NO_VECTORS, 0, hdr.error);
            pthread_mutex_unlock(&g_index_mutex);
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }

        /* 3O: GPU BITSET prefilter path.
         * Converts filter TIDs → bitset via rev map, then calls cuVS filtered BF.
         * Falls through to D-wedge below on any allocation or search failure. */
        if (cmd->use_prefilter && cmd->n_filter_tids > 0
            && e->rev_tids != NULL && e->rev_item_ids != NULL)
        {
            size_t    flt_bytes  = (size_t)cmd->n_filter_tids * sizeof(uint64_t);
            void     *flt_mem    = MAP_FAILED;
            uint64_t *flt_tids   = NULL;
            uint32_t *bitset     = NULL;
            CuvsSearchResult *praw     = NULL;
            CuvsResult       *presults = NULL;
            int pn = 0, did_prefilter = 0, used_cagra = 0;

            flt_mem = map_shm_readonly(cmd->filter_shm_key, flt_bytes, g_peer_uid);
            if (flt_mem != MAP_FAILED)
                flt_tids = (uint64_t *)flt_mem;

            if (flt_tids) {
                int64_t  nv      = e->n_vecs;
                uint32_t nwords  = (uint32_t)((nv + 31) / 32);
                bitset   = malloc((size_t)nwords * sizeof(uint32_t));
                praw     = malloc((size_t)(k > 0 ? k : 1) * sizeof(CuvsSearchResult));
                presults = malloc((size_t)(k > 0 ? k : 1) * sizeof(CuvsResult));

                if (bitset && praw && presults) {
                    /* The wrappers now pad slots the corpus cannot fill (TAIL
                     * CONTRACT in cuvs_wrapper.h, #103). Seeding the same
                     * sentinel the loop stops on keeps this loop safe against a
                     * wrapper that returns non-zero after a partial write. */
                    for (int pi = 0; pi < k; pi++)
                        praw[pi].item_id = -1;
                    /* pg_cuvs convention: bit=1 = exclude. Start all excluded;
                     * the cuVS wrapper inverts this to cuVS's bit=1 = include. */
                    memset(bitset, 0xFF, (size_t)nwords * sizeof(uint32_t));
                    /* clear bits for filter items (include them). n_included
                     * counts bits actually cleared -- cmd->n_filter_tids is
                     * the backend's filter TID count, but the binary search
                     * above silently skips a TID this index's rev map can't
                     * find (e.g. a post-build delta row, a tombstoned row),
                     * so n_included <= cmd->n_filter_tids in general (#133
                     * review F2). Used below to size the short-fill guard. */
                    int64_t n_included = 0;
                    for (uint32_t fi = 0; fi < cmd->n_filter_tids; fi++) {
                        uint64_t want = flt_tids[fi];
                        int64_t lo = 0, hi = nv - 1;
                        while (lo <= hi) {
                            int64_t mid = lo + (hi - lo) / 2;
                            if (e->rev_tids[mid] == want) {
                                int32_t iid = e->rev_item_ids[mid];
                                if (iid >= 0 && (int64_t)iid < nv) {
                                    bitset[iid >> 5] &= ~(1u << (iid & 31));
                                    n_included++;
                                }
                                break;
                            }
                            if (e->rev_tids[mid] < want) lo = mid + 1;
                            else                          hi = mid - 1;
                        }
                    }

                    /* CAGRA prefilter first (approx, faster); fall back to BF. */
                    int pret = 1;
                    int short_fill_detected = 0;
                    if (e->handle != NULL) {
                        pret = cuvs_cagra_search_filtered(
                            e->handle, query, (int)cmd->dim, k,
                            bitset, nv, praw, e->gpu_device_id,
                            cmd->itopk_size, cmd->search_width);
                        if (pret == 0) {
                            used_cagra = 1;
                            /* #133 / ADR-083: 3O's BITSET-masked CAGRA traversal
                             * can collapse on anti-correlated filters -- selectivity
                             * does not predict it (measured recall 0.0 across
                             * 0.0001-0.5 selectivity), but the traversal never
                             * reaching the passing rows does: it comes back with
                             * far fewer than k filled slots. Healthy cells returned
                             * exactly min(k, |included filter|); every measured
                             * collapsed cell returned <=2.02 out of k=10. TAIL
                             * CONTRACT (cuvs_wrapper.h, #103) pads unfilled slots
                             * with CUVS_PAD_ITEM_ID; the documented contract says
                             * padding is trailing-only, but cuvs_wrapper.cu's own
                             * comment on store_result() only guarantees the
                             * sentinel lands somewhere in [0, bk) -- so count every
                             * slot rather than stop at the first pad, which stays
                             * correct even if that ever isn't strictly trailing.
                             *
                             * `expect` is what a correct answer could return: k,
                             * or the whole INCLUDED filter set if that is smaller
                             * than k (n_included, not cmd->n_filter_tids -- see
                             * above; a short-but-correct answer must not be
                             * mistaken for a collapse). Strict `filled < expect`
                             * (#133 review F1): every measured healthy cell
                             * returned exactly `expect`, with no observed
                             * in-between case, and the measured collapse ceiling
                             * (2.02/10) is far below even a `expect - 1` floor, so
                             * tightening from the original half-of-expect margin
                             * does not risk misfiring on a real partial fill.
                             *
                             * Gated on e->main_bf_idx != NULL (#133 review F3):
                             * detecting a collapse is only actionable if a BF
                             * fallback target exists. Without this gate, an index
                             * with no `.vectors` sidecar would have pret forced
                             * back to 1 with nothing to retry into, and the
                             * request would fail with CUVS_STATUS_NO_VECTORS
                             * instead of returning 3O's (possibly degraded but
                             * still approximate) results -- turning a recall
                             * problem into an availability regression. */
                            int filled = 0;
                            for (int fi = 0; fi < k; fi++)
                                if (praw[fi].item_id != CUVS_PAD_ITEM_ID)
                                    filled++;
                            int expect = k;
                            if (n_included < (int64_t)expect)
                                expect = (int)n_included;
                            if (filled < expect && e->main_bf_idx != NULL) {
                                pret = 1;
                                used_cagra = 0;
                                short_fill_detected = 1;
                            }
                        }
                    }
                    if (pret != 0 && e->main_bf_idx != NULL) {
                        pret = cuvs_bf_search_filtered(
                            e->main_bf_idx, query, (int)cmd->dim, k,
                            bitset, nv, praw, delta_gpu_of(e));
                        /* Count the retry only once it actually ran (#133
                         * review F3), not merely detected -- a search error
                         * unrelated to the short-fill collapse (e.g. a
                         * concurrent OOM) must not be attributed to it. */
                        if (short_fill_detected)
                            e->prefilter_fallback_count++;
                    }
                    if (pret == 0) {
                        did_prefilter = 1;
                        munmap(query, vec_bytes);  /* unmap before D-wedge path runs */
                        for (int i = 0; i < k; i++) {
                            int64_t id = praw[i].item_id;
                            if (id < 0 || id >= e->n_vecs) break;
                            presults[pn].tid      = e->tids[id];
                            presults[pn].distance = praw[i].distance;
                            pn++;
                        }
                    }
                }
            }

            free(bitset);
            free(praw);
            if (flt_mem != MAP_FAILED) munmap(flt_mem, flt_bytes);

            if (did_prefilter) {
                clock_gettime(CLOCK_MONOTONIC, &t1);
                uint32_t lat3o = (uint32_t)(
                    (t1.tv_sec - t0.tv_sec) * 1000000 +
                    (t1.tv_nsec - t0.tv_nsec) / 1000);
                record_search_stat(e, CUVS_STATUS_OK, lat3o, NULL);
                e->last_requested_k = cmd->k;
                e->last_returned_k  = (uint32_t) pn;
                e->last_search_mode = used_cagra ? 4 : 3; /* 4=cagra_prefilter, 3=gpu_bf_prefilter */
                pthread_mutex_unlock(&g_index_mutex);
                CuvsReplyHeader hdr3o = {0};
                hdr3o.status     = CUVS_STATUS_OK;
                hdr3o.n_results  = (uint32_t) pn;
                hdr3o.latency_us = lat3o;
                send_all(client_fd, &hdr3o, sizeof(hdr3o));
                if (pn > 0)
                    send_all(client_fd, presults, (size_t) pn * sizeof(CuvsResult));
                free(presults);
                return;
            }
            free(presults);
            /* 3O failed (search error or malloc); fall through to D-wedge. */
        }

        if (!e->main_bf_idx)
        {
            munmap(query, vec_bytes);
            CuvsReplyHeader hdr = {0};
            hdr.status = CUVS_STATUS_NO_VECTORS;
            strncpy(hdr.error,
                    "prefilter failed and no brute-force vectors are available",
                    sizeof(hdr.error) - 1);
            record_search_stat(e, hdr.status, 0, hdr.error);
            pthread_mutex_unlock(&g_index_mutex);
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }

        /* D-wedge: exact BF over the whole corpus, then post-filter down to the
         * whitelist -- so the fetch depth must be deep enough that ~k filtered
         * rows survive. For a filter uncorrelated with distance the j-th
         * filtered neighbour sits at unfiltered rank ~ j/sel, so the depth has
         * to scale with 1/sel. A fixed 4x ignores selectivity and collapses:
         * ADR-082 measured recall 0.011 with 0.11 rows returned at sel=0.001.
         * The 2x factor covers the variance of Binomial(bk, sel) so ~k rows
         * survive rather than exactly k in expectation. bk is clamped to the
         * corpus and to the exact-fallback chunk cap below. If that prefix
         * comes up short, the sidecar path scans only whitelist vectors in
         * bounded chunks. */
        int64_t bk_target = (int64_t) k;
        if (cmd->n_filter_tids > 0)
        {
            double sel = (e->main_bf_n > 0)
                ? (double) cmd->n_filter_tids / (double) e->main_bf_n
                : 0.0;
            bk_target = (int64_t) k * 4;
            if (sel > 0.0)
            {
                /* Clamp in double space before converting: a pathologically
                 * small selectivity (a stale reltuples, say) would otherwise
                 * overflow the int64_t conversion, which is undefined. The
                 * corpus is the only meaningful ceiling anyway. */
                double want = (double) k / sel * 2.0;
                double cap  = (double) e->main_bf_n;
                if (want > cap)
                    want = cap;
                if (want > (double) bk_target)
                    bk_target = (int64_t) want;
            }
        }
        int exact_chunk_cap = (cmd->n_vecs > 0 && cmd->n_vecs <= INT32_MAX)
            ? (int)cmd->n_vecs : 1;
        int64_t bk_limit = exact_chunk_cap > k ? exact_chunk_cap : k;
        int64_t bk_value = bk_target < e->main_bf_n ? bk_target : e->main_bf_n;
        if (bk_value > bk_limit)
            bk_value = bk_limit;
        int bk = (int)bk_value;
        CuvsSearchResult *raw     = malloc((size_t)(bk > 0 ? bk : 1) * sizeof(CuvsSearchResult));
        CuvsResult       *results = malloc((size_t)(k  > 0 ? k  : 1) * sizeof(CuvsResult));
        if (!raw || !results)
        {
            free(raw); free(results);
            munmap(query, vec_bytes);
            pthread_mutex_unlock(&g_index_mutex);
            send_error(client_fd, "malloc failed");
            return;
        }

        int ret = (bk > 0)
            ? cuvs_bf_search(e->main_bf_idx, query, (int) cmd->dim, bk, raw, delta_gpu_of(e))
            : 0;
        if (ret != 0)
        {
            free(raw); free(results);
            munmap(query, vec_bytes);
            CuvsReplyHeader hdr = {0};
            hdr.status = (ret == 2) ? CUVS_STATUS_DIM_MISMATCH : CUVS_STATUS_OOM_FALLBACK;
            strncpy(hdr.error,
                    (ret == 2) ? "query dim != index dim" : "brute_force search failed",
                    sizeof(hdr.error) - 1);
            record_search_stat(e, hdr.status, 0, hdr.error);
            pthread_mutex_unlock(&g_index_mutex);
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }

        /* D-wedge spike: open filter shm and mmap the sorted TID array. */
        uint64_t *filter_tids = NULL;
        void     *filter_mem  = MAP_FAILED;
        size_t    filter_bytes = 0;
        if (cmd->n_filter_tids > 0)
        {
            filter_bytes = (size_t)cmd->n_filter_tids * sizeof(uint64_t);
            filter_mem = map_shm_readonly(cmd->filter_shm_key, filter_bytes, g_peer_uid);
            if (filter_mem != MAP_FAILED)
            {
                filter_tids = malloc(filter_bytes);
                if (filter_tids)
                {
                    memcpy(filter_tids, filter_mem, filter_bytes);
                    cuvs_u64_sort(filter_tids, cmd->n_filter_tids);
                }
            }
        }
        if (filter_mem != MAP_FAILED)
        {
            munmap(filter_mem, filter_bytes);
            filter_mem = MAP_FAILED;
        }

        /* Fail closed, keyed on the REQUEST (n_filter_tids) rather than on the
         * shm key. This path exists to confine results to the caller's TID
         * whitelist (tenant isolation, ADR-063); returning the unfiltered top-k
         * with CUVS_STATUS_OK silently hands back rows the caller excluded.
         * Note an empty filter_shm_key with n_filter_tids > 0 must land here
         * too: an earlier version tested only the mmap result *inside* the
         * "key is non-empty" branch, so a frame with no key skipped the check
         * entirely and still produced unfiltered rows. The streaming-BF path
         * already refuses in the same situation — match it. */
        if (cuvs_filter_frame_refuses(cmd->n_filter_tids, filter_tids != NULL))
        {
            free(raw); free(results);
            free(filter_tids);
            munmap(query, vec_bytes);
            CuvsReplyHeader hdr = {0};
            hdr.status = CUVS_STATUS_ERROR;
            strncpy(hdr.error,
                    "filter TID set unavailable; refusing to return unfiltered rows",
                    sizeof(hdr.error) - 1);
            record_search_stat(e, hdr.status, 0, hdr.error);
            pthread_mutex_unlock(&g_index_mutex);
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }

        int n_valid = 0;
        for (int i = 0; i < bk && n_valid < k; i++)
        {
            int64_t id = raw[i].item_id;
            if (id < 0 || id >= e->n_vecs)
                continue;
            uint64_t tid = e->tids[id];
            if (filter_tids
                && !cuvs_u64_contains(filter_tids, cmd->n_filter_tids, tid))
                continue;
            results[n_valid].tid = tid;
            results[n_valid].distance = raw[i].distance;
            n_valid++;
        }
        free(raw);

        int exact_status = CUVS_STATUS_OK;
        int used_exact = 0;
        if (filter_tids && n_valid < k && (int64_t)bk < e->main_bf_n)
        {
            used_exact = 1;
            exact_status = search_filter_exact(
                e, query, filter_tids, cmd->n_filter_tids,
                k, exact_chunk_cap, results, &n_valid);
        }
        free(filter_tids);
        munmap(query, vec_bytes);

        if (exact_status != CUVS_STATUS_OK)
        {
            free(results);
            CuvsReplyHeader hdr = {0};
            hdr.status = (uint32_t)exact_status;
            strncpy(hdr.error,
                    "filtered search could not complete exact fallback",
                    sizeof(hdr.error) - 1);
            record_search_stat(e, hdr.status, 0, hdr.error);
            pthread_mutex_unlock(&g_index_mutex);
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }

        clock_gettime(CLOCK_MONOTONIC, &t1);
        uint32_t latency_us = (uint32_t)(
            (t1.tv_sec - t0.tv_sec) * 1000000 +
            (t1.tv_nsec - t0.tv_nsec) / 1000);

        record_search_stat(e, CUVS_STATUS_OK, latency_us, NULL);
        e->last_requested_k = cmd->k;
        e->last_returned_k  = (uint32_t) n_valid;
        e->last_search_mode = used_exact ? 6 : 3;

        pthread_mutex_unlock(&g_index_mutex);

        CuvsReplyHeader hdr = {0};
        hdr.status       = CUVS_STATUS_OK;
        hdr.n_results    = (uint32_t) n_valid;
        hdr.latency_us   = latency_us;
        hdr.delta_merged = 0;
        send_all(client_fd, &hdr, sizeof(hdr));
        if (n_valid > 0)
            send_all(client_fd, results, (size_t) n_valid * sizeof(CuvsResult));
        free(results);
        return;
    }

    /* Phase 3F/3G: sharded fanout. Search every shard for its top-(k+overfetch),
     * map each shard-local id to a global TID via the shard's tid_offset, merge
     * all candidates by distance (metric-aware), and return the global top-k.
     * One IPC request per logical index; the reply format is unchanged. Phase 3G
     * dispatches the per-shard searches concurrently (one pthread per shard) when
     * cuvs.parallel_fanout is on, cutting latency from sum(shard_i) toward
     * max(shard_i); off runs them sequentially (A/B baseline / kill switch).
     * cuvs.shard_overfetch widens each shard's request for recall at scale.
     * Delta is left to the backend CPU merge (delta_merged=0). Fail closed: any
     * shard search failure returns OOM_FALLBACK (CPU), never a partial result. */
    if (e->shard_count >= 2)
    {
        int sc        = e->shard_count;
        int overfetch = (int) cmd->shard_overfetch;
        int eff_k     = k + overfetch;
        if (eff_k < k) eff_k = k;            /* guard against overflow */
        int parallel  = (cmd->parallel_fanout != 0);
        int use_bf    = (cmd->search_mode == 1);   /* Phase 3L: brute_force over shards */
        uint32_t snap_db  = e->db_oid;
        uint32_t snap_idx = e->index_oid;

        /* The sharded fanout below never consumes cmd->filter_shm_key, and the
         * merge does not re-apply the whitelist, so a filtered request answered
         * here would silently return rows the caller excluded. cuvs_ipc.h used
         * to promise a per-shard post-filter that was never implemented; until
         * it is, refuse rather than widen the result set. */
        if (cmd->n_filter_tids > 0)
        {
            munmap(query, vec_bytes);
            CuvsReplyHeader hdr = {0};
            hdr.status = CUVS_STATUS_ERROR;
            strncpy(hdr.error,
                    "filtered search is not implemented for sharded indexes",
                    sizeof(hdr.error) - 1);
            record_search_stat(e, hdr.status, 0, hdr.error);
            pthread_mutex_unlock(&g_index_mutex);
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }

        /* Phase 3L: brute_force over a sharded index — ensure every shard has a
         * resident BF index over its range. Fail closed (NO_VECTORS) if the
         * global .vectors sidecar is missing/stale, exactly like the unsharded
         * path. Done under the lock before the in-flight window opens. */
        if (use_bf && !refresh_shard_bf_caches(e, cmd->bf_precision))
        {
            munmap(query, vec_bytes);
            CuvsReplyHeader hdr = {0};
            hdr.status = CUVS_STATUS_NO_VECTORS;
            strncpy(hdr.error,
                    "brute_force requested but the .vectors sidecar is missing or "
                    "stale; REINDEX to enable it",
                    sizeof(hdr.error) - 1);
            record_search_stat(e, CUVS_STATUS_NO_VECTORS, 0, hdr.error);
            pthread_mutex_unlock(&g_index_mutex);
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }

        ShardSearchArg *args    = calloc((size_t) sc, sizeof(ShardSearchArg));
        pthread_t      *threads = calloc((size_t) sc, sizeof(pthread_t));
        if (!args || !threads)
        {
            free(args); free(threads);
            munmap(query, vec_bytes);
            pthread_mutex_unlock(&g_index_mutex);
            send_error(client_fd, "malloc failed");
            return;
        }

        /* Snapshot each shard's descriptor and allocate its result buffer, then
         * drop the lock for the GPU dispatch. The shard handles and the `tids`
         * block stay valid lock-free by construction (ADR-022): sharded entries
         * are non-evictable, and same-index REINDEX/DROP is serialized against
         * this search by PostgreSQL's AccessExclusiveLock, so nothing frees them
         * mid-search. We must NOT reuse `e` after unlocking, though: a concurrent
         * eviction of some other index compacts g_indexes[] by value and can move
         * this struct — so we re-find by OID after the parallel window. */
        int64_t total_sk = 0;
        int     alloc_ok = 1;
        for (int s = 0; s < sc; s++)
        {
            ShardEntry *se = &e->shards[s];
            int sk = (int) (se->n_vecs < (int64_t) eff_k ? se->n_vecs : eff_k);
            if (sk < 0) sk = 0;
            args[s].handle        = se->handle;
            args[s].bf_idx        = se->bf_idx;   /* Phase 3L: NULL unless use_bf */
            args[s].use_bf        = use_bf;
            args[s].query         = query;
            args[s].dim           = (int) cmd->dim;
            args[s].sk            = sk;
            args[s].gpu_device_id = se->gpu_device_id;
            args[s].itopk_size    = cmd->itopk_size;
            args[s].search_width  = cmd->search_width;
            args[s].tid_offset    = se->tid_offset;
            args[s].shard_n_vecs  = se->n_vecs;
            args[s].rc            = 0;       /* skipped (sk==0) shards = OK / no candidates */
            if (sk > 0)
            {
                args[s].out = malloc((size_t) sk * sizeof(CuvsSearchResult));
                if (!args[s].out) { alloc_ok = 0; break; }
                total_sk += sk;
            }
        }
        if (!alloc_ok)
        {
            for (int s = 0; s < sc; s++) free(args[s].out);
            free(args); free(threads);
            munmap(query, vec_bytes);
            pthread_mutex_unlock(&g_index_mutex);
            send_error(client_fd, "malloc failed");
            return;
        }

        /* Phase 3G.4: mark this entry in-flight so a concurrent eviction (now
         * that sharded indexes are evictable) cannot free our shard handles
         * while we search them lock-free. Decremented after the re-lock below. */
        e->inflight++;
        g_unlocked_gpu_jobs++;

        pthread_mutex_unlock(&g_index_mutex);   /* ---- lock-free GPU dispatch ---- */
#ifdef CUVS_TEST_HOOKS
        {
            const char *pause = getenv("CUVS_TEST_SHARD_PAUSE_MS");
            if (pause)
            {
                long ms = strtol(pause, NULL, 10);
                if (ms > 0 && ms <= 10000)
                {
                    LOG_INFO("[hnsw_admission_test] phase=shard-registered mono_us=%llu\n",
                        (unsigned long long)cuvs_hnsw_monotonic_us());
                    (void)poll(NULL, 0, (int)ms);
                }
            }
        }
#endif

        /* Parallel spawns one worker per non-empty shard; sequential runs them
         * inline (cuvs.parallel_fanout = off). A pthread_create failure degrades
         * that shard to inline execution (no leak, still correct). */
        for (int s = 0; s < sc; s++)
        {
            if (args[s].sk <= 0) continue;
            if (parallel &&
                pthread_create(&threads[s], NULL, shard_search_worker, &args[s]) == 0)
                args[s].threaded = 1;
            else
                shard_search_worker(&args[s]);
        }
        for (int s = 0; s < sc; s++)
            if (args[s].threaded)
                pthread_join(threads[s], NULL);

        /* Keep `query` mapped past the join: the GPU delta search below
         * (Phase 3G.3) still reads it; munmap after that. Re-acquire the lock to
         * update counters, refresh+search the delta cache, and merge. Re-find by
         * OID since the struct may have moved (eviction compaction) while we were
         * unlocked. The merge (g_merge_metric + qsort) and the small delta BF
         * search run here under the lock; only the base shard fanout was lock-free. */
        pthread_mutex_lock(&g_index_mutex);
        g_unlocked_gpu_jobs--;
#ifdef CUVS_TEST_HOOKS
        LOG_INFO("[hnsw_admission_test] phase=shard-return mono_us=%llu\n",
            (unsigned long long)cuvs_hnsw_monotonic_us());
#endif
        e = find_index(snap_db, snap_idx);
        if (!e)
        {
            /* Cannot happen for the supported workload (ADR-022 invariant);
             * fail closed to CPU rather than risk touching freed memory. */
            for (int s = 0; s < sc; s++) free(args[s].out);
            free(args); free(threads);
            munmap(query, vec_bytes);
            pthread_mutex_unlock(&g_index_mutex);
            CuvsReplyHeader hdr = {0};
            hdr.status = CUVS_STATUS_OOM_FALLBACK;
            strncpy(hdr.error, "sharded index vanished mid-search; CPU fallback",
                    sizeof(hdr.error) - 1);
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }

        /* Lock-free window closed: we hold the mutex again and the entry can't be
         * evicted under us now, so drop the in-flight refcount here (covers every
         * exit path below). */
        if (e->inflight > 0) e->inflight--;

        /* Record per-shard outcomes; any shard failure fails the whole query
         * closed (CPU fallback), never a partial ANN result. */
        int shard_err = 0;
        for (int s = 0; s < sc; s++)
        {
            if (args[s].rc != 0)
            {
                e->shards[s].error_count++;
                e->shards[s].last_status = CUVS_STATUS_OOM_FALLBACK;
                shard_err = 1;
            }
            else if (args[s].sk > 0)
            {
                e->shards[s].search_count++;
                e->shards[s].last_status = CUVS_STATUS_OK;
            }
        }

        if (shard_err)
        {
            for (int s = 0; s < sc; s++) free(args[s].out);
            free(args); free(threads);
            munmap(query, vec_bytes);
            CuvsReplyHeader hdr = {0};
            hdr.status = CUVS_STATUS_OOM_FALLBACK;
            strncpy(hdr.error, "sharded search failed on a shard; CPU fallback",
                    sizeof(hdr.error) - 1);
            record_search_stat(e, CUVS_STATUS_OOM_FALLBACK, 0, hdr.error);
            pthread_mutex_unlock(&g_index_mutex);
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }

        /* cand holds base candidates (<= total_sk) plus up to k delta candidates. */
        CuvsResult *cand = malloc((size_t) (total_sk + (int64_t) k) * sizeof(CuvsResult));
        CuvsResult *out  = malloc((size_t) k * sizeof(CuvsResult));
        if (!cand || !out)
        {
            for (int s = 0; s < sc; s++) free(args[s].out);
            free(args); free(threads); free(cand); free(out);
            munmap(query, vec_bytes);
            pthread_mutex_unlock(&g_index_mutex);
            send_error(client_fd, "malloc failed");
            return;
        }

        /* Collect candidates, mapping each shard-local id to a global TID. Uses
         * the snapshot tid_offset/n_vecs (stable per the invariant) and the
         * re-found entry's tids/n_vecs. */
        int n_total = 0;
        for (int s = 0; s < sc; s++)
        {
            int64_t toff = args[s].tid_offset;
            for (int j = 0; j < args[s].sk; j++)
            {
                int64_t id = args[s].out[j].item_id;
                if (id < 0 || id >= args[s].shard_n_vecs) continue;
                int64_t gid = toff + id;
                if (gid < 0 || gid >= e->n_vecs) continue;
                cand[n_total].tid      = e->tids[gid];
                cand[n_total].distance = args[s].out[j].distance;
                n_total++;
            }
            free(args[s].out);
        }
        free(args); free(threads);

        /* Phase 3G.3: shard-aware GPU delta cache. Refresh the resident delta
         * brute-force index (on shard 0's GPU) from the current `.delta`; if
         * present, search it and fold its candidates into the same global merge,
         * so the daemon serves INSERT/UPDATE rows on the GPU path (delta_merged=1)
         * instead of forcing the backend CPU merge. If the cache is unavailable
         * (no VRAM / corrupt / generation mismatch), delta_merged stays 0 and the
         * backend CPU-merges as before — fail-open to correctness. */
        int delta_merged = 0;
        /* Phase 3L: brute_force already searches the full base corpus exactly via
         * the per-shard .vectors caches; the pending .delta is a separate concern
         * and is NOT merged in BF mode (consistent with the unsharded BF path). */
        if (!use_bf)
            refresh_delta_cache(e);
        if (!use_bf && e->delta_idx && e->n_delta > 0)
        {
            int eff_dk = (int) ((int64_t) k < e->n_delta ? (int64_t) k : e->n_delta);
            CuvsSearchResult *draw = malloc((size_t) eff_dk * sizeof(CuvsSearchResult));
            if (draw && cuvs_bf_search(e->delta_idx, query, (int) cmd->dim, eff_dk,
                                       draw, delta_gpu_of(e)) == 0)
            {
                for (int i = 0; i < eff_dk; i++)
                {
                    int64_t id = draw[i].item_id;
                    if (id < 0 || id >= e->n_delta) continue;
                    cand[n_total].tid      = e->delta_tids[id];
                    cand[n_total].distance = draw[i].distance;
                    n_total++;
                }
                delta_merged = 1;
                e->delta_merged_count++;
            }
            free(draw);
        }
        munmap(query, vec_bytes);            /* base + delta searches done with query */

        g_merge_metric = e->metric;          /* read by delta_cand_cmp under the lock */
        qsort(cand, (size_t) n_total, sizeof(CuvsResult), delta_cand_cmp);
        int n_valid = (n_total < k) ? n_total : k;
        memcpy(out, cand, (size_t) n_valid * sizeof(CuvsResult));
        free(cand);

        clock_gettime(CLOCK_MONOTONIC, &t1);
        uint32_t latency_us = (uint32_t)(
            (t1.tv_sec - t0.tv_sec) * 1000000 +
            (t1.tv_nsec - t0.tv_nsec) / 1000);
        record_search_stat(e, CUVS_STATUS_OK, latency_us, NULL);
        e->last_requested_k = cmd->k;
        e->last_returned_k  = (uint32_t) n_valid;
        e->last_search_mode = use_bf ? 3 : 0;   /* Phase 3L: 3=gpu_bf, 0=gpu_cagra */
        pthread_mutex_unlock(&g_index_mutex);

        CuvsReplyHeader hdr = {0};
        hdr.status       = CUVS_STATUS_OK;
        hdr.n_results    = (uint32_t) n_valid;
        hdr.latency_us   = latency_us;
        hdr.delta_merged = (uint32_t) delta_merged;
        send_all(client_fd, &hdr, sizeof(hdr));
        if (n_valid > 0)
            send_all(client_fd, out, (size_t) n_valid * sizeof(CuvsResult));
        free(out);
        return;
    }

    /* Phase 3I-1: CPU HNSW path — bypass GPU CAGRA when requested. */
    if (cmd->use_cpu_hnsw)
    {
        /* Lazy-load the .hnsw sidecar into RAM (held in e->hnsw_idx). */
        if (!e->hnsw_idx)
        {
            char hp[512];
            hnsw_file_path(hp, sizeof(hp), g_index_dir, e->db_oid, e->index_oid);
            e->hnsw_idx = cuvs_hnsw_deserialize(hp, (int)e->dim, e->metric,
                                                 (int)e->gpu_device_id);
            if (!e->hnsw_idx)
                LOG_WARN("[handle_search] HNSW sidecar not loadable for %u/%u; "
                         "using GPU CAGRA\n", e->db_oid, e->index_oid);
        }
        if (e->hnsw_idx)
        {
            CuvsSearchResult *hraw = malloc((size_t)k * sizeof(CuvsSearchResult));
            if (!hraw)
            {
                munmap(query, vec_bytes);
                pthread_mutex_unlock(&g_index_mutex);
                send_error(client_fd, "malloc failed");
                return;
            }
            int hret = cuvs_hnsw_search(e->hnsw_idx, query, (int)cmd->dim,
                                         k, 0, hraw);
            if (hret == 0)
            {
                CuvsResult *hresults = malloc((size_t)k * sizeof(CuvsResult));
                int hn_valid = 0;
                if (hresults)
                {
                    for (int i = 0; i < k; i++)
                    {
                        int64_t id = hraw[i].item_id;
                        if (id < 0 || id >= e->n_vecs) continue;
                        hresults[hn_valid].tid      = e->tids[id];
                        hresults[hn_valid].distance = hraw[i].distance;
                        hn_valid++;
                    }
                }
                free(hraw);
                munmap(query, vec_bytes);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                uint32_t hlat = (uint32_t)((t1.tv_sec  - t0.tv_sec)  * 1000000 +
                                           (t1.tv_nsec - t0.tv_nsec) / 1000);
                record_search_stat(e, CUVS_STATUS_OK, hlat, NULL);
                e->last_requested_k  = cmd->k;
                e->last_returned_k   = (uint32_t)hn_valid;
                e->last_search_mode  = 1; /* cpu_hnsw */
                pthread_mutex_unlock(&g_index_mutex);
                CuvsReplyHeader hdr = {0};
                hdr.status     = CUVS_STATUS_OK;
                hdr.n_results  = (uint32_t)hn_valid;
                hdr.latency_us = hlat;
                send_all(client_fd, &hdr, sizeof(hdr));
                if (hn_valid > 0 && hresults)
                    send_all(client_fd, hresults,
                             (size_t)hn_valid * sizeof(CuvsResult));
                free(hresults);
                return;
            }
            /* HNSW search failed -> fall through to GPU CAGRA. */
            free(hraw);
            LOG_WARN("[handle_search] cuvs_hnsw_search failed for %u/%u; "
                     "retrying with GPU CAGRA\n", e->db_oid, e->index_oid);
        }
    }

    /* #160: micro-batched cagra path, the twin of the 3L-9 block above. Every
     * other unsharded shape has already returned by here (brute_force/flat,
     * sharded fanout, a successful CPU HNSW), so this is exactly the single
     * cagra dispatch point. Filtered requests are excluded: the cagra path's
     * filtering lives in the 3O/D-wedge blocks above, and a coalesced dispatch
     * has no per-request bitset — batching one here would silently drop the
     * whitelist. handle==NULL is left to the ADR-073 guard just below, which
     * fails closed with a clearer status than the worker could.
     *
     * Mechanically identical to the BF enqueue: release g_index_mutex (lock
     * order is index-then-batch), enqueue, block until the worker fills our
     * result. The query shm stays mapped (req.query points at it) until we
     * reply. On a full queue we degrade to the immediate path below by
     * re-acquiring the lock and re-finding e. */
    if (cmd->cagra_batch_wait_us > 0 && g_batch_worker_started
        && cmd->n_filter_tids == 0 && e->handle != NULL)
    {
        CuvsBatchKey     key = { cmd->db_oid, cmd->index_oid, 0 /* n/a */,
                                 cmd->dim, 0 /* mode: cagra */,
                                 cmd->itopk_size, cmd->search_width };
        CuvsResult      *rout = malloc((size_t) k * sizeof(CuvsResult));
        int              enqueued = 0;
        CuvsBatchRequest req;

        pthread_mutex_unlock(&g_index_mutex);   /* release BEFORE taking g_batch_mtx */

        if (rout)
        {
            req.key     = key;   req.query  = query;  req.k = k;
            req.wait_us = cmd->cagra_batch_wait_us;
            req.out     = rout;  req.n_out  = 0;
            req.status  = CUVS_STATUS_ERROR;
            req.delta_merged = 0; req.latency_us = 0; req.done = 0;

            pthread_mutex_lock(&g_batch_mtx);
            if (g_batch_queue_n < CUVS_BATCH_MAX)
            {
                g_batch_queue[g_batch_queue_n++] = &req;
                enqueued = 1;
                pthread_cond_signal(&g_batch_cond);
                while (!req.done)                       /* spurious-wakeup safe */
                    pthread_cond_wait(&g_batch_done_cond, &g_batch_mtx);
            }
            pthread_mutex_unlock(&g_batch_mtx);
        }

        if (enqueued)
        {
            munmap(query, vec_bytes);
            CuvsReplyHeader hdr = {0};
            hdr.status = (uint32_t) req.status;
            if (req.status == CUVS_STATUS_OK)
            {
                hdr.n_results    = (uint32_t) req.n_out;
                hdr.latency_us   = req.latency_us;
                hdr.delta_merged = (uint32_t) req.delta_merged;
                send_all(client_fd, &hdr, sizeof(hdr));
                if (req.n_out > 0)
                    send_all(client_fd, rout, (size_t) req.n_out * sizeof(CuvsResult));
            }
            else
            {
                if (req.status == CUVS_STATUS_STALE)
                    strncpy(hdr.error, "index stale (writes since build)",
                            sizeof(hdr.error) - 1);
                send_all(client_fd, &hdr, sizeof(hdr));
            }
            free(rout);
            return;
        }

        /* Could not enqueue (queue full / malloc fail): degrade to the
         * immediate path. Re-acquire the lock and re-find the entry. */
        free(rout);
        pthread_mutex_lock(&g_index_mutex);
        e = find_index(cmd->db_oid, cmd->index_oid);
        if (!e)
        {
            munmap(query, vec_bytes);
            pthread_mutex_unlock(&g_index_mutex);
            CuvsReplyHeader hdr = {0};
            hdr.status = CUVS_STATUS_NOT_FOUND;
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }
        /* fall through to the immediate path with e + the still-mapped query */
    }

    CuvsSearchResult *raw = malloc(k * sizeof(CuvsSearchResult));
    if (!raw)
    {
        munmap(query, vec_bytes);
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "malloc failed");
        return;
    }

    /* ADR-073 defense-in-depth: the cagra dispatch requires a non-NULL graph
     * handle. A flat index (is_flat, handle==NULL) must never reach here — the
     * brute-force gate above returns first — but guard structurally so any future
     * path landing here with handle==NULL fails closed instead of dereferencing. */
    if (e->handle == NULL)
    {
        free(raw);
        munmap(query, vec_bytes);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_NO_VECTORS;
        strncpy(hdr.error,
                "index has no CAGRA graph (flat/brute-force only)",
                sizeof(hdr.error) - 1);
        record_search_stat(e, CUVS_STATUS_NO_VECTORS, 0, hdr.error);
        pthread_mutex_unlock(&g_index_mutex);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    LOG_DEBUG("[handle_search] calling cuvs_cagra_search k=%d dim=%u...\n",
            k, cmd->dim);
    int ret = cuvs_cagra_search(e->handle, query, (int)cmd->dim, k, raw,
                                e->gpu_device_id, cmd->itopk_size, cmd->search_width);
    LOG_DEBUG("[handle_search] cuvs_cagra_search rc=%d\n", ret);

    if (ret != 0)
    {
        munmap(query, vec_bytes);
        free(raw);
        CuvsReplyHeader hdr = {0};
        /* ret==2 is the wrapper's own dim-mismatch guard (defense in depth in
         * case the pre-check above is ever bypassed). Everything else maps to
         * OOM_FALLBACK so the backend retries on CPU. */
        hdr.status = (ret == 2) ? CUVS_STATUS_DIM_MISMATCH : CUVS_STATUS_OOM_FALLBACK;
        strncpy(hdr.error,
                (ret == 2) ? "query dim != index dim" : "CAGRA search failed",
                sizeof(hdr.error) - 1);
        record_search_stat(e, hdr.status, 0, hdr.error);
        pthread_mutex_unlock(&g_index_mutex);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    CuvsResult *results = malloc(k * sizeof(CuvsResult));
    if (!results)
    {
        munmap(query, vec_bytes);
        free(raw);
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "malloc failed");
        return;
    }

    /* Phase 3B: refresh the resident GPU delta cache from the current .delta;
     * if present, brute-force it on the GPU so cagra_finish_one can merge it
     * with the base candidates by distance (same cuVS scale, metric-aware
     * order). The query stays mmap'd until after the delta search. */
    int delta_merged = 0;
    int n_valid      = 0;
    CuvsSearchResult *draw = NULL;
    int               n_dc = 0;
    refresh_delta_cache(e);
    if (e->delta_idx && e->n_delta > 0)
    {
        n_dc = (int) ((int64_t) k < e->n_delta ? (int64_t) k : e->n_delta);
        draw = malloc((size_t) n_dc * sizeof(CuvsSearchResult));
        if (!draw
            || cuvs_bf_search(e->delta_idx, query, (int) cmd->dim, n_dc, draw,
                              e->gpu_device_id) != 0)
        {
            free(draw);             /* no usable delta -> base-only, as before */
            draw = NULL;
            n_dc = 0;
        }
    }
    munmap(query, vec_bytes);

    n_valid = cagra_finish_one(e, raw, k, draw, n_dc, results, &delta_merged);
    free(draw);
    free(raw);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint32_t latency_us = (uint32_t)(
        (t1.tv_sec - t0.tv_sec) * 1000000 +
        (t1.tv_nsec - t0.tv_nsec) / 1000);

    record_search_stat(e, CUVS_STATUS_OK, latency_us, NULL);
    e->last_requested_k = cmd->k;
    e->last_returned_k  = (uint32_t)n_valid;
    e->last_search_mode = 0; /* gpu_cagra */

    pthread_mutex_unlock(&g_index_mutex);

    CuvsReplyHeader hdr = {0};
    hdr.status       = CUVS_STATUS_OK;
    hdr.n_results    = (uint32_t)n_valid;
    hdr.latency_us   = latency_us;
    hdr.delta_merged = (uint32_t)delta_merged;

    send_all(client_fd, &hdr, sizeof(hdr));
    if (n_valid > 0)
        send_all(client_fd, results, (size_t)n_valid * sizeof(CuvsResult));

    free(results);
}

/* ----------------------------------------------------------------
 * Handle SEARCH_BATCH command (CUVS_OP_SEARCH_BATCH, Phase 3M)
 *
 * Q queries arrive in one request shm ([Q][dim][Q*dim f32]); the daemon runs
 * one batched GPU dispatch (cuvs_*_search_batch) for unsharded indexes, or a
 * per-query shard fanout+merge for sharded indexes, and returns Q*K results
 * (K = min(k, n_vecs)) in a daemon-allocated reply shm
 * ([Q][K][tids Q*K][dists Q*K]) whose key travels back in reply.error[]. The
 * single-query handle_search path is unchanged. Held under g_index_mutex for
 * the whole call (no lock-free window): correctness over latency for batches.
 * ---------------------------------------------------------------- */
static void
handle_search_batch(int client_fd, const CuvsCmdFrame *cmd)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_mutex_lock(&g_index_mutex);

    /* --- Resolve the index (find or reload), mirroring handle_search. --- */
    IndexEntry *e = find_index(cmd->db_oid, cmd->index_oid);
    if (e)
    {
        cache_counter_bump(g_cache_hits, e);
    }
    else
    {
        int load_rc = load_index(cmd->db_oid, cmd->index_oid, CUVS_RQ_LOAD);
        if (load_rc == 0)
            e = find_index(cmd->db_oid, cmd->index_oid);
        if (!e)
        {
            int st = (load_rc == -2) ? CUVS_STATUS_OOM_FALLBACK : CUVS_STATUS_NOT_FOUND;
            pthread_mutex_unlock(&g_index_mutex);
            CuvsReplyHeader hdr = {0};
            hdr.status = (uint32_t) st;
            if (load_rc == -2 && t_reload_rej.reason != CUVS_ADMISSION_NONE &&
                cuvs_admission_encode(&t_reload_rej, hdr.err_out, sizeof(hdr.err_out)) < 0)
                hdr.err_out[0] = '\0';
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }
        cache_counter_bump(g_cache_reloads, e);
        cache_counter_bump(g_cache_misses, e);
    }
    e->last_search = time(NULL);

    /* --- Stale / metric / dim gates (same statuses as handle_search). --- */
    if (e->stale)
    {
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_STALE;
        strncpy(hdr.error, "index stale (writes since build)", sizeof(hdr.error) - 1);
        record_search_stat(e, CUVS_STATUS_STALE, 0, hdr.error);
        pthread_mutex_unlock(&g_index_mutex);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }
    if (cmd->metric != e->metric)
    {
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_METRIC_MISMATCH;
        snprintf(hdr.error, sizeof(hdr.error),
                 "index built with metric %u but queried with metric %u; REINDEX required",
                 e->metric, cmd->metric);
        record_search_stat(e, CUVS_STATUS_METRIC_MISMATCH, 0, hdr.error);
        pthread_mutex_unlock(&g_index_mutex);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }
    if (cmd->dim != e->dim)
    {
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_DIM_MISMATCH;
        snprintf(hdr.error, sizeof(hdr.error),
                 "query dim %u does not match index dim %u", cmd->dim, e->dim);
        record_search_stat(e, CUVS_STATUS_DIM_MISMATCH, 0, hdr.error);
        pthread_mutex_unlock(&g_index_mutex);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    uint32_t Q      = (uint32_t) cmd->n_vecs;   /* SEARCH_BATCH: Q in n_vecs */
    int      dim    = (int) cmd->dim;
    int      k      = (int) cmd->k;
    /* ADR-073 FATAL fix: a flat index (handle==NULL) MUST use brute-force here.
     * Without the e->is_flat term a flat batch search falls to the else branch
     * below — cuvs_cagra_search_batch(e->handle, ...) — and dereferences NULL,
     * crashing the daemon for every backend. */
    int      use_bf = (cmd->search_mode == 1 || e->is_flat);
    int      K      = (int) ((int64_t) k < e->n_vecs ? (int64_t) k : e->n_vecs);

    if (Q == 0 || K <= 0)
    {
        pthread_mutex_unlock(&g_index_mutex);
        send_error_code(client_fd, CUVS_STATUS_ERROR, "empty batch or empty index");
        return;
    }

    /* --- Map the request shm: [Q][dim][Q*dim f32]. --- */
    size_t qhdr_bytes = 2 * sizeof(uint32_t);
    size_t qtotal     = qhdr_bytes + (size_t) Q * (size_t) dim * sizeof(float);
    int    qfd        = shm_open(cmd->shm_key, O_RDONLY, 0666);
    if (qfd < 0)
    {
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "shm_open failed for batch queries");
        return;
    }
    if (!shm_check_peer_owner(qfd, g_peer_uid))   /* #87 finding 1 */
    {
        close(qfd);
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "shm owner mismatch for batch queries");
        return;
    }
    void *qmem = mmap(NULL, qtotal, PROT_READ, MAP_SHARED, qfd, 0);
    close(qfd);
    if (qmem == MAP_FAILED)
    {
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "mmap failed for batch queries");
        return;
    }
    const float *queries = (const float *) ((const char *) qmem + qhdr_bytes);

    /* --- brute_force: ensure the resident BF cache(s); fail closed if absent. --- */
    if (use_bf)
    {
        int ready;
        if (e->shard_count >= 2)
            ready = refresh_shard_bf_caches(e, cmd->bf_precision);
        else
        {
            refresh_main_bf_cache(e, cmd->bf_precision);
            ready = (e->main_bf_idx != NULL);
        }
        if (!ready)
        {
            munmap(qmem, qtotal);
            CuvsReplyHeader hdr = {0};
            hdr.status = CUVS_STATUS_NO_VECTORS;
            strncpy(hdr.error,
                    "brute_force requested but the .vectors sidecar is missing or "
                    "stale; REINDEX to enable it",
                    sizeof(hdr.error) - 1);
            record_search_stat(e, CUVS_STATUS_NO_VECTORS, 0, hdr.error);
            pthread_mutex_unlock(&g_index_mutex);
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }
    }

    /* --- Run the batched search into out[Q*K] (global TIDs). --- */
    CuvsResult *out = malloc((size_t) Q * (size_t) K * sizeof(CuvsResult));
    if (!out)
    {
        munmap(qmem, qtotal);
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "malloc failed");
        return;
    }
    int ok = 1;

    if (e->shard_count < 2)
    {
        /* Unsharded: one GPU dispatch for the whole batch. */
        CuvsSearchResult *raw = malloc((size_t) Q * (size_t) K * sizeof(CuvsSearchResult));
        if (!raw)
        {
            ok = 0;
        }
        else
        {
            int ret = use_bf
                ? cuvs_bf_search_batch(e->main_bf_idx, queries, (int) Q, dim, K, raw, delta_gpu_of(e))
                : cuvs_cagra_search_batch(e->handle, queries, (int) Q, dim, K, raw,
                                           e->gpu_device_id, cmd->itopk_size, cmd->search_width);
            if (ret != 0)
                ok = 0;
            else
                for (size_t i = 0; i < (size_t) Q * (size_t) K; i++)
                {
                    int64_t id = raw[i].item_id;
                    out[i].tid      = (id < 0 || id >= e->n_vecs) ? 0 : e->tids[id];
                    out[i].distance = raw[i].distance;
                }
            free(raw);
        }
    }
    else
    {
        /* Sharded: per-query fanout + global merge, sequential under the lock. */
        int sc        = e->shard_count;
        int overfetch = (int) cmd->shard_overfetch;
        int eff_k     = k + overfetch;
        if (eff_k < k) eff_k = k;
        CuvsSearchResult *sraw = malloc((size_t) eff_k * sizeof(CuvsSearchResult));
        CuvsResult       *cand = malloc((size_t) sc * (size_t) eff_k * sizeof(CuvsResult));
        if (!sraw || !cand)
        {
            ok = 0;
        }
        else
        {
            for (uint32_t q = 0; q < Q && ok; q++)
            {
                const float *query = queries + (size_t) q * dim;
                int n_total = 0;
                for (int s = 0; s < sc; s++)
                {
                    ShardEntry *sh = &e->shards[s];
                    int sk = (int) (sh->n_vecs < (int64_t) eff_k ? sh->n_vecs : eff_k);
                    if (sk <= 0) continue;
                    int ret = use_bf
                        ? cuvs_bf_search(sh->bf_idx, query, dim, sk, sraw, sh->gpu_device_id)
                        : cuvs_cagra_search(sh->handle, query, dim, sk, sraw,
                                             sh->gpu_device_id, cmd->itopk_size, cmd->search_width);
                    if (ret != 0) { ok = 0; break; }
                    for (int j = 0; j < sk; j++)
                    {
                        int64_t id = sraw[j].item_id;
                        if (id < 0 || id >= sh->n_vecs) continue;
                        int64_t gid = sh->tid_offset + id;
                        if (gid < 0 || gid >= e->n_vecs) continue;
                        cand[n_total].tid      = e->tids[gid];
                        cand[n_total].distance = sraw[j].distance;
                        n_total++;
                    }
                }
                if (!ok) break;
                g_merge_metric = e->metric;
                qsort(cand, (size_t) n_total, sizeof(CuvsResult), delta_cand_cmp);
                int nv = (n_total < K) ? n_total : K;
                for (int j = 0; j < K; j++)
                {
                    if (j < nv)
                        out[(size_t) q * K + j] = cand[j];
                    else
                    {
                        out[(size_t) q * K + j].tid      = 0;
                        out[(size_t) q * K + j].distance = 3.402823466e+38f;
                    }
                }
            }
        }
        free(sraw);
        free(cand);
    }

    munmap(qmem, qtotal);

    if (!ok)
    {
        free(out);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_OOM_FALLBACK;
        strncpy(hdr.error, "batch search failed on the GPU; retry on CPU",
                sizeof(hdr.error) - 1);
        record_search_stat(e, CUVS_STATUS_OOM_FALLBACK, 0, hdr.error);
        pthread_mutex_unlock(&g_index_mutex);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint32_t latency_us = (uint32_t) (
        (t1.tv_sec - t0.tv_sec) * 1000000 +
        (t1.tv_nsec - t0.tv_nsec) / 1000);
    record_search_stat(e, CUVS_STATUS_OK, latency_us, NULL);
    e->last_requested_k = cmd->k;
    e->last_returned_k  = (uint32_t) K;
    e->last_search_mode = use_bf ? 3 : 0;

    /* --- Allocate the reply shm: [Q][K][tids Q*K][dists Q*K]. --- */
    static int bsr_seq = 0;
    char rkey[64];
    {
        unsigned char rnd[8];
        char hex[17];
        if (shm_key_random(rnd, sizeof(rnd)) != 0)   /* #87 finding 2: fail closed */
        {
            free(out);
            pthread_mutex_unlock(&g_index_mutex);
            send_error_code(client_fd, CUVS_STATUS_ERROR, "csprng failed");
            return;
        }
        for (size_t i = 0; i < sizeof(rnd); i++)
            snprintf(hex + i * 2, 3, "%02x", rnd[i]);
        snprintf(rkey, sizeof(rkey), "/pg_cuvs_bsr_%d_%d_%s",
                 (int) getpid(), __atomic_fetch_add(&bsr_seq, 1, __ATOMIC_RELAXED), hex);
    }
    size_t rhdr   = 2 * sizeof(uint32_t);
    size_t rtids  = (size_t) Q * (size_t) K * sizeof(uint64_t);
    size_t rdists = (size_t) Q * (size_t) K * sizeof(float);
    size_t rtotal = rhdr + rtids + rdists;

    /*
     * #165: handed over as an SCM_RIGHTS fd with the name unlinked before the
     * reply, so nothing survives this call. The backend cannot unlink a
     * daemon-owned name (sticky /dev/shm, different uid) — and this is the
     * per-query path, so the old scheme leaked on every batch search.
     * Second RDONLY descriptor keeps the consumer read-only, as before.
     */
    int rfd = shm_open(rkey, O_CREAT | O_EXCL | O_RDWR, 0644);
    if (rfd < 0)
    {
        free(out);
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "shm_open failed for batch reply");
        return;
    }
    if (ftruncate(rfd, (off_t) rtotal) != 0)
    {
        close(rfd); shm_unlink(rkey); free(out);
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "ftruncate failed for batch reply");
        return;
    }
    int rpass_fd = shm_open(rkey, O_RDONLY, 0);
    if (rpass_fd < 0)
    {
        close(rfd); shm_unlink(rkey); free(out);
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "shm_open(RDONLY) failed for batch reply");
        return;
    }
    shm_unlink(rkey);          /* name gone; both fds remain valid */
    void *rmem = mmap(NULL, rtotal, PROT_WRITE, MAP_SHARED, rfd, 0);
    close(rfd);
    if (rmem == MAP_FAILED)
    {
        close(rpass_fd); free(out);
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "mmap failed for batch reply");
        return;
    }
    ((uint32_t *) rmem)[0] = Q;
    ((uint32_t *) rmem)[1] = (uint32_t) K;
    {
        uint64_t *tp = (uint64_t *) ((char *) rmem + rhdr);
        float    *dp = (float *)    ((char *) rmem + rhdr + rtids);
        for (size_t i = 0; i < (size_t) Q * (size_t) K; i++)
        {
            tp[i] = out[i].tid;
            dp[i] = out[i].distance;
        }
    }
    munmap(rmem, rtotal);
    free(out);

    pthread_mutex_unlock(&g_index_mutex);

    CuvsReplyHeader hdr = {0};
    hdr.status       = CUVS_STATUS_OK;
    hdr.n_results    = Q;
    hdr.latency_us   = latency_us;
    hdr.delta_merged = (uint32_t) K;   /* per-query result stride */
    /* error[] carries the former name for log correlation only (#165). */
    strncpy(hdr.error, rkey, sizeof(hdr.error) - 1);
    if (cuvs_fd_send(client_fd, rpass_fd, &hdr, sizeof(hdr)) != 0)
        LOG_ERROR("[batch_search] reply fd handoff failed\n");
    close(rpass_fd);
}

/* ----------------------------------------------------------------
 * Atomic tids file write helper.
 * Writes to <tids_tmp> path, fsyncs, returns 0 on success, -1 on failure.
 * On failure the tmp file is unlinked.
 * ---------------------------------------------------------------- */
static int
write_tids_atomic(const char *tids_tmp,
                  int64_t n_vecs, uint32_t dim, uint32_t metric,
                  const uint64_t *tids)
{
    FILE *f = fopen(tids_tmp, "wb");
    if (!f) {
        LOG_ERROR("write_tids_atomic: fopen %s FAILED errno=%d (%s)\n",
                tids_tmp, errno, strerror(errno));
        return -1;
    }
    fchmod_sidecar(f, tids_tmp);
    int ok = (cuvs_tids_write(f, n_vecs, dim, metric, tids) == 0);
    if (ok && fflush(f) != 0) {
        LOG_ERROR("write_tids_atomic: fflush %s FAILED errno=%d\n",
                tids_tmp, errno);
        ok = 0;
    }
    if (ok && fsync(fileno(f)) != 0) {
        LOG_ERROR("write_tids_atomic: fsync %s FAILED errno=%d\n",
                tids_tmp, errno);
        ok = 0;
    }
    if (fclose(f) != 0) {
        LOG_ERROR("write_tids_atomic: fclose %s FAILED errno=%d\n",
                tids_tmp, errno);
        ok = 0;
    }
    if (!ok) {
        unlink(tids_tmp);
        return -1;
    }
    return 0;
}

/* fsync a file by path (re-opens it RDONLY). Best-effort; returns 0/-1. */
static int
fsync_path(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    close(fd);
    return rc;
}

/*
 * #167: force persistent index_dir sidecars (.tids/.vectors/.shards/.cagra/
 * .hnsw) to 0644 regardless of the daemon's umask.
 *
 * fopen(path, "wb") and the cuVS serialize calls both leave the mode at
 * 0666 & ~umask. Under the default umask (022) that is already 0644, so this
 * is a no-op there. Under a hardened daemon (systemd UMask=077, a restrictive
 * launcher, some container defaults) it is 0600 owned by the daemon uid, and
 * the PG backend — a different uid, reading these files by path
 * (hnsw_export.c, pg_cuvs.c) — gets EACCES. #166 closed the equivalent gap
 * for the transient SCM_RIGHTS handoff; these are the persistent files that
 * fd-passing cannot help with, because they must still exist as files after
 * a restart.
 *
 * 0644 matches the existing shm_open(..., 0644) precedent (cuvs_ipc.c) for
 * the transient path, and the current index_dir deployment shape (bootstrap.sh
 * chmod 777s the directory itself — see #167's follow-up on directory
 * permissions, which is what actually gates read/replace access here).
 *
 * Best-effort: a chmod failure is logged, not fatal. The write already
 * succeeded and fsynced; refusing to persist over a chmod problem would
 * trade a mode bug for a durability regression.
 */
static void
fchmod_sidecar(FILE *f, const char *path)
{
    if (fchmod(fileno(f), 0644) != 0)
        LOG_WARN("fchmod_sidecar: fchmod(%s, 0644) failed errno=%d\n", path, errno);
}

static void
chmod_sidecar(const char *path)
{
    if (chmod(path, 0644) != 0)
        LOG_WARN("chmod_sidecar: chmod(%s, 0644) failed errno=%d\n", path, errno);
}

/* Phase 3F: streaming CRC-32 of a whole file (chunked; never loads the entire
 * artifact into RAM). Returns 0 and sets *out on success, -1 on open/read error. */
static int
crc32_file(const char *path, uint32_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    uint32_t crc = cuvs_crc32_stream_begin();
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        crc = cuvs_crc32_stream_update(crc, buf, n);
    int err = ferror(f);
    fclose(f);
    if (err)
        return -1;
    *out = cuvs_crc32_stream_end(crc);
    return 0;
}

/* Phase 3F: free all GPU shard handles of a sharded entry and the shards array.
 * Caller holds g_index_mutex. Leaves shard_count=0/shards=NULL so the slot can
 * be reused as an unsharded entry. Does NOT free e->tids (caller owns that). */
static void
free_index_shards(IndexEntry *e)
{
    if (!e->shards)
    {
        e->shard_count = 0;
        return;
    }
    for (int s = 0; s < e->shard_count; s++)
    {
        if (e->shards[s].valid && e->shards[s].handle)
            cuvs_cagra_free(e->shards[s].handle, e->shards[s].gpu_device_id);
        /* Phase 3L: release this shard's resident brute-force index, if any. */
        if (e->shards[s].bf_idx)
            cuvs_bf_free(e->shards[s].bf_idx, e->shards[s].gpu_device_id);
    }
    free(e->shards);
    e->shards = NULL;
    e->shard_count = 0;
}

/* ----------------------------------------------------------------
 * Phase 3C: detached upload thread — spawned after OK reply so that
 * CREATE INDEX returns immediately. Upload failure is non-fatal; the
 * local artifact is already durable at this point.
 * ---------------------------------------------------------------- */
typedef struct {
    char     snapshot_uri[512];
    char     cluster_id[128];
    char     gcs_key_file[512];
    char     cagra_path[512];
    char     tids_path[512];
    uint32_t db_oid;
    uint32_t table_oid;
    uint32_t index_oid;
    uint32_t relfilenode;
    uint32_t metric;
    uint32_t dim;
    int64_t  vector_count;
    uint32_t base_generation;   /* this build's .tids body_crc32 (3C manifest) */
} UploadThreadArgs;

static void *
objstore_upload_thread(void *arg)
{
    UploadThreadArgs *a = (UploadThreadArgs *)arg;
    int rc = cuvs_objstore_upload(
        a->snapshot_uri,
        a->cluster_id,
        a->gcs_key_file,
        a->cagra_path,
        a->tids_path,
        a->db_oid,
        a->table_oid,
        a->index_oid,
        a->relfilenode,
        a->metric,
        a->dim,
        a->vector_count,
        a->base_generation   /* this build's .tids body_crc32 */
    );
    if (rc == 0)
        LOG_INFO("objstore: uploaded index %u/%u to %s\n",
                 a->db_oid, a->index_oid, a->snapshot_uri);
    else
        LOG_WARN("objstore: upload FAILED for index %u/%u (non-fatal, local artifact intact)\n",
                 a->db_oid, a->index_oid);
    free(a);
    return NULL;
}

/* Phase 3G.2: sharded variant — uploads the whole artifact set (.tids + .shards
 * + N .sNNN.cagra) located under index_dir. Detached + non-fatal, like the
 * unsharded path above. */
typedef struct {
    char     snapshot_uri[512];
    char     cluster_id[128];
    char     gcs_key_file[512];
    char     index_dir[512];
    uint32_t db_oid;
    uint32_t table_oid;
    uint32_t index_oid;
    uint32_t relfilenode;
    uint32_t metric;
    uint32_t dim;
    int64_t  vector_count;
    uint32_t shard_count;
    uint32_t base_generation;   /* this build's .tids body_crc32 (3C manifest) */
} ShardedUploadArgs;

static void *
objstore_upload_sharded_thread(void *arg)
{
    ShardedUploadArgs *a = (ShardedUploadArgs *)arg;
    int rc = cuvs_objstore_upload_sharded(
        a->snapshot_uri, a->cluster_id, a->gcs_key_file, a->index_dir,
        a->db_oid, a->table_oid, a->index_oid, a->relfilenode,
        a->metric, a->dim, a->vector_count,
        a->base_generation,   /* this build's .tids body_crc32 */
        a->shard_count);
    if (rc == 0)
        LOG_INFO("objstore: uploaded sharded index %u/%u (%u shards) to %s\n",
                 a->db_oid, a->index_oid, a->shard_count, a->snapshot_uri);
    else
        LOG_WARN("objstore: sharded upload FAILED for %u/%u (non-fatal, local artifacts intact)\n",
                 a->db_oid, a->index_oid);
    free(a);
    return NULL;
}

/* ----------------------------------------------------------------
 * Phase 3F: sharded BUILD path (cuvs.shard_count >= 2).
 *
 * Splits the corpus into `shard_count` contiguous build-order ranges, builds a
 * standalone CAGRA artifact per shard placed round-robin across usable GPUs,
 * and commits all-or-nothing: every shard `.cagra.tmp` + global `.tids.tmp` +
 * `.shards.tmp` are fsynced, then renamed (shard cagras, then .tids, then the
 * `.shards` manifest LAST as the commit marker). Self-contained: owns `mem`
 * (munmap) and the client reply. Fail-closed: any failure rolls back all
 * artifacts and resources and returns an error (CREATE INDEX/REINDEX aborts).
 *
 * Builds into LOCAL arrays and registers the entry only on full success — this
 * avoids holding an IndexEntry* across ensure_vram, whose eviction can shift
 * g_indexes[] and invalidate the pointer.
 * ---------------------------------------------------------------- */
static void
build_sharded(int client_fd, const CuvsCmdFrame *cmd, const char *index_dir,
              const float *vecs, const uint64_t *tids_in,
              size_t total, void *mem, int shard_count)
{
    int64_t  n_vecs    = cmd->n_vecs;
    uint32_t dim       = cmd->dim;
    uint32_t metric    = cmd->metric;
    size_t   tid_bytes = (size_t)n_vecs * sizeof(uint64_t);

    /* Clamp shard count so every shard holds >= 2 vectors (CAGRA aborts on 1). */
    int sc = shard_count;
    if (sc > CUVS_SHARDS_MAX)
        sc = CUVS_SHARDS_MAX;
    if ((int64_t)sc > n_vecs / 2)
        sc = (int)(n_vecs / 2);
    if (sc < 2)
    {
        munmap(mem, total);
        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED,
                        "shard_count too high for corpus (need >= 2 vectors per shard)");
        return;
    }
    if (sc != shard_count)
        LOG_WARN("[build_sharded] clamped shard_count %d -> %d for %lld vecs\n",
                 shard_count, sc, (long long)n_vecs);

    const char *save_dir = (index_dir[0] != '\0') ? index_dir : g_index_dir;
    mkdir(save_dir, 0700);

    /* Host copy of the global TID array (shared by all shards), taken before
     * we drop the shm mapping. */
    uint64_t        *new_tids    = malloc(tid_bytes);
    ShardEntry      *shards      = calloc((size_t)sc, sizeof(ShardEntry));
    CuvsShardRecord *recs        = calloc((size_t)sc, sizeof(CuvsShardRecord));
    char           (*shard_tmp)[576]   = malloc((size_t)sc * sizeof(*shard_tmp));
    char           (*shard_final)[512] = malloc((size_t)sc * sizeof(*shard_final));
    if (!new_tids || !shards || !recs || !shard_tmp || !shard_final)
    {
        free(new_tids); free(shards); free(recs); free(shard_tmp); free(shard_final);
        munmap(mem, total);
        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED, "sharded build alloc failed");
        return;
    }
    memcpy(new_tids, tids_in, tid_bytes);

    pthread_mutex_lock(&g_index_mutex);

    /* --- Build + place each shard over a contiguous range. --- */
    int64_t base   = n_vecs / sc;
    int64_t rem    = n_vecs % sc;
    int64_t off    = 0;
    int     n_gpus = n_usable_gpus();
    int     ok     = 1;
    int     i      = 0;
    int     n_reserved = 0;   /* ADR-070 Bug #2: shards with an active VRAM reservation */
    CuvsAdmissionFields shard_rej;   /* I3: per-shard admission rejection */
    int     shard_rejected = 0;
    for (i = 0; i < sc; i++)
    {
        int64_t shard_n = base + (i < rem ? 1 : 0);
        /* C3: shards have no reservation; only their admission value is N. */
        size_t  final   = estimate_vram_bytes(shard_n, (int)dim, (int)cmd->graph_degree);
        size_t  needed  = admission_needed_bytes(final);
        int     dev     = usable_gpu(i % n_gpus);   /* round-robin spread */

        shard_cagra_path(shard_final[i], 512, save_dir,
                         cmd->db_oid, cmd->index_oid, (uint32_t)i);
        snprintf(shard_tmp[i], 576, "%s.tmp", shard_final[i]);

        g_cur_requester_idx = cmd->index_oid;
        AdmitLine al = {.op = "BUILD", .path = "sharded",
                        .algo = admission_algo_name(cmd->build_algo), .gpu = dev,
                        .db = cmd->db_oid, .idx = cmd->index_oid, .dim = dim,
                        .gd = cmd->graph_degree ? cmd->graph_degree : 64,
                        .igd = cmd->intermediate_graph_degree
                               ? cmd->intermediate_graph_degree : 128,
                        .n = shard_n, .final = final, .needed = needed,
                        .shape = cuvs_build_shape_class(shard_n, dim, metric,
                                                        cmd->graph_degree,
                                                        cmd->intermediate_graph_degree,
                                                        cmd->build_algo)};
        al.pending_before = g_pending_rmm_build_vram[dev];
        al.evictions_before = g_cache_evictions[dev];
        if (ensure_vram(needed, dev, CUVS_RQ_BUILD_SHARD, NULL, &al) != 0)
        {
            int alt = pick_gpu_for_index(needed);
            al.gpu = alt;
            if (alt < 0)
                admission_fill(&shard_rej, CUVS_ADMISSION_AR0, needed, -1);
            else
            {
                al.pending_before = g_pending_rmm_build_vram[alt];
                al.evictions_before = g_cache_evictions[alt];
            }
            if (alt < 0 || ensure_vram(needed, alt, CUVS_RQ_BUILD_SHARD, &shard_rej, &al) != 0)
            {
                note_build_admission(cmd->db_oid, cmd->index_oid, needed,
                                     alt >= 0 ? alt : dev, 0, 0);
                LOG_WARN("[build_sharded] shard %d (%lld vecs) won't fit on any GPU\n",
                         i, (long long)shard_n);
                log_admit(&al, shard_rej.reason);
                shard_rej.i = (uint32_t) i;
                shard_rej.present |= CUVS_ADM_F_I;
                shard_rejected = 1;
                ok = 0;
                break;
            }
            dev = alt;
        }
        if (i == 0)   /* F-3: an admitted build drops its earlier rejects */
            note_build_admission(cmd->db_oid, cmd->index_oid, needed, dev, 1, 0);
        log_admit(&al, CUVS_ADMISSION_NONE);   /* one [admit] line per shard (v5) */

        /* I4 / CK-28: shards are not admitted through N_m/AR6/AR7, so they
         * never get the managed large_workspace, whatever the active policy. */
        log_build_begin(&al);
        al.build_started_us = admission_mono_us();
        CuvsCagraIndex h = cuvs_cagra_build(vecs + (size_t)off * dim, shard_n,
                                            (int)dim, metric,
                                            (int)cmd->graph_degree,
                                            (int)cmd->intermediate_graph_degree,
                                            cmd->build_algo, dev,
                                            CUVS_LW_OVERFLOW_ERROR, NULL);
        al.build_finished_us = admission_mono_us();
        log_build_end(&al, NULL, h != NULL,
                      h ? "built" : cuvs_last_build_was_oom() ? "oom" : "failed");
        if (!h)
        {
            LOG_ERROR("[build_sharded] cuvs_cagra_build failed for shard %d\n", i);
            ok = 0;
            break;
        }

        shards[i].shard_id      = (uint32_t)i;
        shards[i].handle        = h;
        shards[i].tid_offset    = off;
        shards[i].n_vecs        = shard_n;
        shards[i].gpu_device_id = (uint32_t)dev;
        shards[i].vram_bytes    = final;
        shards[i].valid         = 1;

        int shard_serialized = (cuvs_cagra_serialize(h, shard_tmp[i], dev) == 0);
        if (shard_serialized)
            chmod_sidecar(shard_tmp[i]);
        if (!shard_serialized || fsync_path(shard_tmp[i]) != 0)
        {
            LOG_ERROR("[build_sharded] serialize/fsync failed for shard %d\n", i);
            ok = 0;
            break;
        }

        uint32_t acrc = 0;
        if (crc32_file(shard_tmp[i], &acrc) != 0)
        {
            ok = 0;
            break;
        }
        recs[i].shard_id       = (uint32_t)i;
        recs[i].gpu_device_id  = (uint32_t)dev;
        recs[i].tid_offset     = off;
        recs[i].n_vecs         = shard_n;
        recs[i].dim            = dim;
        recs[i].metric         = metric;
        recs[i].artifact_crc32 = acrc;
        recs[i].reserved       = 0;

        off += shard_n;
    }

    /* ADR-070 Bug #2: builds done (or aborted) — release every shard reservation.
     * The lock is held continuously from here through the registry insert below,
     * so the shards' VRAM is accounted by the registry entry (success) or freed
     * (error paths) at the next unlock; no reservation must outlive this point.
     * shards[r].gpu_device_id/vram_bytes were set before each build, so this also
     * covers a shard whose build failed. */
    for (int r = 0; r < n_reserved; r++)
        g_pending_build_vram[shards[r].gpu_device_id] -= shards[r].vram_bytes;
    n_reserved = 0;

    /* Phase 3L: persist one global `.vectors` sidecar (same build-order layout
     * as the global `.tids`) while `vecs` is still mapped. Per-shard BF indexes
     * are sliced from this at load time by tid_offset. Best-effort: a failure
     * leaves brute_force unavailable but never fails the sharded build. */
    char vecs_final[512], vecs_tmp[576];
    int  vectors_committed = 0;
    vectors_file_path(vecs_final, sizeof(vecs_final), save_dir, cmd->db_oid, cmd->index_oid);
    snprintf(vecs_tmp, sizeof(vecs_tmp), "%s.tmp", vecs_final);
    uint32_t base_gen = cuvs_crc32(new_tids, tid_bytes);   /* .tids body_crc32 — also the 3C manifest base_generation */
    if (ok)
    {
        FILE *vf = fopen(vecs_tmp, "wb");
        int vok = (vf != NULL);
        if (vok) fchmod_sidecar(vf, vecs_tmp);
        if (vok && cuvs_vectors_write(vf, n_vecs, dim, metric, base_gen, vecs) != 0) vok = 0;
        if (vok && fflush(vf) != 0) vok = 0;
        if (vok && fsync(fileno(vf)) != 0) vok = 0;
        if (vf && fclose(vf) != 0) vok = 0;
        if (vok) {
            vectors_committed = 1;
        } else {
            if (vf) unlink(vecs_tmp);
            unlink(vecs_final);   /* drop any stale prior sidecar */
            LOG_WARN("[build_sharded] .vectors sidecar not written for %u/%u; "
                     "brute_force unavailable until REINDEX\n",
                     cmd->db_oid, cmd->index_oid);
        }
    }

    munmap(mem, total);   /* vecs no longer needed past the build loop */

    if (!ok)
    {
        for (int j = 0; j <= i && j < sc; j++)
        {
            if (shards[j].valid && shards[j].handle)
                cuvs_cagra_free(shards[j].handle, shards[j].gpu_device_id);
            unlink(shard_tmp[j]);
        }
        pthread_mutex_unlock(&g_index_mutex);
        free(new_tids); free(shards); free(recs); free(shard_tmp); free(shard_final);
        cuvs_set_inject_build_oom(0);   /* #82: see handle_build */
        if (shard_rejected)   /* C5: shard admission rejection, not a build failure */
            send_admission_reject(client_fd, CUVS_STATUS_OOM_FALLBACK,
                                  "sharded build: a shard does not fit", &shard_rej);
        else
            send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED, "sharded build failed");
        return;
    }

    /* --- Persist global .tids.tmp + .shards.tmp (fsynced). --- */
    char tids_final[512], tids_tmp[576], shards_finalp[512], shards_tmp[576];
    tids_file_path(tids_final, sizeof(tids_final), save_dir, cmd->db_oid, cmd->index_oid);
    shards_manifest_path(shards_finalp, sizeof(shards_finalp), save_dir,
                         cmd->db_oid, cmd->index_oid);
    snprintf(tids_tmp,   sizeof(tids_tmp),   "%s.tmp", tids_final);
    snprintf(shards_tmp, sizeof(shards_tmp), "%s.tmp", shards_finalp);

    uint32_t base_crc = cuvs_crc32(new_tids, tid_bytes);

    int persisted = 0;
    if (write_tids_atomic(tids_tmp, n_vecs, dim, metric, new_tids) == 0)
    {
        FILE *mf = fopen(shards_tmp, "wb");
        if (mf)
        {
            fchmod_sidecar(mf, shards_tmp);
            int wok = (cuvs_shards_write(mf, (uint32_t)sc, n_vecs, dim, metric,
                                         base_crc, recs) == 0);
            if (wok && fflush(mf) != 0) wok = 0;
            if (wok && fsync(fileno(mf)) != 0) wok = 0;
            if (fclose(mf) != 0) wok = 0;
            if (wok) persisted = 1;
            else unlink(shards_tmp);
        }
        if (!persisted)
            unlink(tids_tmp);
    }

    if (!persisted)
    {
        for (int j = 0; j < sc; j++)
        {
            cuvs_cagra_free(shards[j].handle, shards[j].gpu_device_id);
            unlink(shard_tmp[j]);
        }
        unlink(vecs_tmp);   /* Phase 3L: drop our un-committed sidecar tmp */
        pthread_mutex_unlock(&g_index_mutex);
        free(new_tids); free(shards); free(recs); free(shard_tmp); free(shard_final);
        send_error_code(client_fd, CUVS_STATUS_PERSIST_FAILED, "sharded persist failed");
        return;
    }

    /* --- Commit: rename shard cagras, then .tids, then .shards LAST. --- */
    int commit_ok = 1, renamed = 0;
    for (int j = 0; j < sc; j++)
    {
        if (rename(shard_tmp[j], shard_final[j]) != 0) { commit_ok = 0; break; }
        renamed++;
    }
    if (commit_ok && rename(tids_tmp, tids_final) != 0) commit_ok = 0;
    if (commit_ok && rename(shards_tmp, shards_finalp) != 0) commit_ok = 0;

    if (!commit_ok)
    {
        for (int j = 0; j < renamed; j++) unlink(shard_final[j]);
        unlink(tids_final); unlink(shards_finalp);
        unlink(tids_tmp);   unlink(shards_tmp);
        unlink(vecs_tmp);   /* Phase 3L: .vectors not yet renamed; drop tmp */
        for (int j = renamed; j < sc; j++) unlink(shard_tmp[j]);
        for (int j = 0; j < sc; j++)
            cuvs_cagra_free(shards[j].handle, shards[j].gpu_device_id);
        pthread_mutex_unlock(&g_index_mutex);
        free(new_tids); free(shards); free(recs); free(shard_tmp); free(shard_final);
        send_error_code(client_fd, CUVS_STATUS_PERSIST_FAILED, "sharded commit rename failed");
        return;
    }

    int dir_fd = open(save_dir, O_RDONLY);
    if (dir_fd >= 0) { fsync(dir_fd); close(dir_fd); }

    /* Phase 3L: commit the global .vectors sidecar (non-fatal, like unsharded).
     * The generation token guards a torn post-commit, pre-vectors crash. */
    if (vectors_committed) {
        if (rename(vecs_tmp, vecs_final) != 0) {
            LOG_WARN("[build_sharded] rename %s -> %s failed errno=%d; brute_force unavailable\n",
                     vecs_tmp, vecs_final, errno);
            unlink(vecs_tmp); unlink(vecs_final);
            vectors_committed = 0;
        } else {
            int vdir_fd = open(save_dir, O_RDONLY);
            if (vdir_fd >= 0) { fsync(vdir_fd); close(vdir_fd); }
        }
    }

    /* Clear staleness marker and any legacy unsharded .cagra for this OID so a
     * reload sees only the sharded artifacts. Also remove orphan shard files
     * left by a prior build with a HIGHER shard count (e.g. REINDEX 4->2). */
    {
        char stale_path[512], legacy_cagra[512];
        stale_file_path(stale_path, sizeof(stale_path), save_dir, cmd->db_oid, cmd->index_oid);
        unlink(stale_path);
        index_file_path(legacy_cagra, sizeof(legacy_cagra), save_dir,
                        cmd->db_oid, cmd->index_oid);
        unlink(legacy_cagra);
        for (uint32_t s = (uint32_t)sc; s < CUVS_SHARDS_MAX; s++)
        {
            char sp[512];
            shard_cagra_path(sp, sizeof(sp), save_dir, cmd->db_oid, cmd->index_oid, s);
            if (unlink(sp) != 0)
                break;   /* contiguous ids: first miss => no more orphans */
        }
    }

    /* --- Register the entry (no more eviction past this point). --- */
    IndexEntry *e = find_index(cmd->db_oid, cmd->index_oid);
    if (e)
    {
        free_delta_cache(e);
        if (e->shard_count >= 2) free_index_shards(e);
        else if (e->handle) cuvs_cagra_free(e->handle, e->gpu_device_id);
        free(e->tids);
    }
    else
    {
        g_cur_requester_idx = cmd->index_oid;
        if (g_n_indexes >= g_max_indexes)
            (void) make_slot(usable_gpu(0), CUVS_RQ_BUILD_SHARD);
        if (g_n_indexes >= g_max_indexes)
        {
            /* Registry full but artifacts are durable on disk: a later search
             * reloads from the .shards manifest. Free GPU resources now. */
            for (int j = 0; j < sc; j++)
                cuvs_cagra_free(shards[j].handle, shards[j].gpu_device_id);
            pthread_mutex_unlock(&g_index_mutex);
            free(new_tids); free(shards); free(recs); free(shard_tmp); free(shard_final);
            LOG_WARN("[build_sharded] registry full; %u/%u persisted, will reload on demand\n",
                     cmd->db_oid, cmd->index_oid);
            CuvsReplyHeader hdr_ok = {0};
            hdr_ok.status = CUVS_STATUS_OK;
            send_all(client_fd, &hdr_ok, sizeof(hdr_ok));
            return;
        }
        e = registry_new_slot();
        memset(e, 0, sizeof(*e));
    }

    e->db_oid       = cmd->db_oid;
    e->index_oid    = cmd->index_oid;
    e->dim          = dim;
    e->metric       = metric;
    e->n_vecs       = n_vecs;
    e->handle       = NULL;
    e->tids         = new_tids;
    e->vram_bytes   = 0;
    e->last_search  = time(NULL);
    e->valid        = 1;
    e->stale        = 0;
    e->stale_since  = 0;
    e->delta_idx = NULL; e->delta_tids = NULL; e->n_delta = 0;
    e->delta_generation = 0; e->delta_mtime = 0; e->delta_vram_bytes = 0;
    e->delta_vecs_host = NULL; e->delta_n_cached = 0;
    /* Phase 3L: main BF cache starts empty; sharded uses per-shard bf_idx. */
    e->main_bf_idx = NULL; e->main_bf_n = 0; e->main_bf_vram_bytes = 0;
    e->main_bf_mtime = 0; e->main_bf_generation = 0; e->bf_precision = 0;
    e->warmup_state  = WARMUP_HOT;
    e->gpu_device_id = 0xFFFFFFFFu;   /* sharded: no single device, not LRU-evictable */
    e->shard_count   = sc;
    e->shards        = shards;        /* transfer ownership of the local array */
    e->inflight      = 0;             /* Phase 3G.4 */
    reset_entry_stats(e);

    LOG_INFO("pg_cuvs_server: built sharded index %u/%u (%lld vecs, %d shards)\n",
             cmd->db_oid, cmd->index_oid, (long long)n_vecs, sc);
    for (int j = 0; j < sc; j++)
        LOG_INFO("  shard %d -> GPU %u (%lld vecs, %zu MB)\n",
                 j, e->shards[j].gpu_device_id, (long long)e->shards[j].n_vecs,
                 e->shards[j].vram_bytes / (1024*1024));

    pthread_mutex_unlock(&g_index_mutex);

    /* new_tids ownership moved to e->tids; do not free it here. */
    free(recs); free(shard_tmp); free(shard_final);

    CuvsReplyHeader hdr_ok = {0};
    hdr_ok.status = CUVS_STATUS_OK;
    send_all(client_fd, &hdr_ok, sizeof(hdr_ok));

    /* Phase 3G.2: snapshot the whole sharded artifact set to GCS (detached,
     * non-fatal). The local artifacts are already durable; upload failure only
     * costs a replica the warmup fast-path. */
    if (g_snapshot_uri[0] != '\0' && cmd->relfilenode != 0)
    {
        ShardedUploadArgs *sa = malloc(sizeof(*sa));
        if (sa)
        {
            strncpy(sa->snapshot_uri, g_snapshot_uri, sizeof(sa->snapshot_uri) - 1);
            strncpy(sa->cluster_id,   g_cluster_id,   sizeof(sa->cluster_id)   - 1);
            strncpy(sa->gcs_key_file, g_gcs_key_file, sizeof(sa->gcs_key_file) - 1);
            strncpy(sa->index_dir,    save_dir,        sizeof(sa->index_dir)    - 1);
            sa->snapshot_uri[sizeof(sa->snapshot_uri) - 1] = '\0';
            sa->cluster_id[sizeof(sa->cluster_id) - 1]     = '\0';
            sa->gcs_key_file[sizeof(sa->gcs_key_file) - 1] = '\0';
            sa->index_dir[sizeof(sa->index_dir) - 1]       = '\0';
            sa->db_oid       = cmd->db_oid;
            sa->table_oid    = cmd->table_oid;
            sa->index_oid    = cmd->index_oid;
            sa->relfilenode  = cmd->relfilenode;
            sa->metric       = cmd->metric;
            sa->dim          = cmd->dim;
            sa->vector_count = cmd->n_vecs;
            sa->shard_count  = (uint32_t) sc;
            sa->base_generation = base_gen;

            pthread_t up_tid;
            pthread_attr_t up_attr;
            pthread_attr_init(&up_attr);
            pthread_attr_setdetachstate(&up_attr, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&up_tid, &up_attr, objstore_upload_sharded_thread, sa) != 0)
            {
                LOG_WARN("objstore: failed to spawn sharded upload thread for %u/%u\n",
                         cmd->db_oid, cmd->index_oid);
                free(sa);
            }
            pthread_attr_destroy(&up_attr);
        }
    }
}

/* ----------------------------------------------------------------
 * Handle BUILD command
 *
 * Atomicity contract (B-1 + B-2 + B-3):
 *   Either CREATE INDEX succeeds with VRAM + disk both committed, or it
 *   fails with no state change (existing index, if any, untouched).
 *
 * Sequence:
 *   1. Build new VRAM index (do NOT drop existing yet).
 *   2. Write tids.tmp atomically (fwrite count + fsync + fclose checked).
 *   3. Serialize cagra.tmp and fsync.
 *   4. Atomic rename: tmp -> final.
 *   5. fsync directory.
 *   6. Swap registry: free existing, install new.
 *
 * Any failure before step 4 → unlink tmps, free new resources, error reply.
 * Failure between renames is logged; partial state may persist on disk but
 * registry is rolled back so memory state stays consistent.
 * ---------------------------------------------------------------- */
/* ADR-059: upper bound on parallel-build worker partials accepted from a single
 * BUILD frame (defensive against a malformed n_partials). A real build has at
 * most max_parallel_maintenance_workers + 1 participants. */
#define CUVS_BUILD_MAX_PARTIALS 256

static void handle_build_multi(int client_fd, const CuvsCmdFrame *cmd,
                               const char *index_dir);
static void finish_build_commit(int client_fd, const CuvsCmdFrame *cmd,
                                const char *save_dir, CuvsCagraIndex new_handle,
                                uint64_t *new_tids, int target_gpu, size_t needed,
                                int vectors_committed, const char *vecs_tmp,
                                const char *vecs_final, uint32_t base_generation);

/* Auto-shard refusal happens before the ordinary admission path. Keep its
 * permanent AR0 payload and log on both corpus transports (C5). */
static void
admission_auto_shard_rejection(const CuvsCmdFrame *cmd, const char *path,
                                CuvsAdmissionFields *rejection)
{
    size_t final = estimate_vram_bytes(cmd->n_vecs, (int)cmd->dim,
                                       (int)cmd->graph_degree);
    size_t needed = admission_needed_bytes(final);
    AdmitLine al = {.op = "BUILD", .path = path,
        .algo = admission_algo_name(cmd->build_algo), .gpu = -1,
        .db = cmd->db_oid, .idx = cmd->index_oid, .dim = cmd->dim,
        .gd = cmd->graph_degree ? cmd->graph_degree : 64,
        .igd = cmd->intermediate_graph_degree ? cmd->intermediate_graph_degree : 128,
        .n = cmd->n_vecs, .final = final, .needed = needed};
    pthread_mutex_lock(&g_index_mutex);
    admission_fill(rejection, CUVS_ADMISSION_AR0, needed, -1);
    log_admit(&al, CUVS_ADMISSION_AR0);
    pthread_mutex_unlock(&g_index_mutex);
}

static void
handle_build(int client_fd, const CuvsCmdFrame *cmd)
{
    clear_build_notices();
    LOG_DEBUG("[handle_build] reading index_dir...\n");
    char index_dir[256] = {0};
    /* ADR-057: the backend may pass the corpus as an SCM_RIGHTS memfd alongside
     * index_dir (memfd tier); passed_fd is -1 for the shm/heap tiers. */
    int  passed_fd = -1;
    if (cuvs_fd_recv(client_fd, index_dir, sizeof(index_dir), &passed_fd) < 0)
    {
        LOG_ERROR("[handle_build] recv index_dir FAILED errno=%d\n", errno);
        send_error(client_fd, "recv index_dir failed");
        return;
    }
    LOG_DEBUG("[handle_build] got index_dir=%s passed_fd=%d\n", index_dir, passed_fd);

    /* Reject degenerate or overflowing payload sizes before any allocation.
     * n_vecs*dim*4 can wrap size_t on a 32-bit-ish product (e.g. n_vecs ~2^31,
     * dim ~2^20), producing a tiny mmap that the build then reads past. */
    /* CAGRA cannot build a neighborhood graph from a single point; cuVS
     * ABORTS the process (uncatchable) on n_vecs==1, taking down the daemon
     * for every backend. Require >= 2 and reject cleanly here. (The backend
     * already short-circuits n_vecs==0 without an IPC call.) */
    if (cmd->n_vecs < 2 || cmd->dim == 0)
    {
        LOG_ERROR("[handle_build] invalid payload n_vecs=%lld dim=%u\n",
                  (long long)cmd->n_vecs, cmd->dim);
        if (passed_fd >= 0) close(passed_fd);
        send_error(client_fd, "CAGRA build needs at least 2 vectors");
        return;
    }
    {
        size_t per_vec = (size_t)cmd->dim * sizeof(float) + sizeof(uint64_t);
        if ((size_t)cmd->n_vecs > SIZE_MAX / per_vec)
        {
            LOG_ERROR("[handle_build] payload size overflow n_vecs=%lld dim=%u\n",
                      (long long)cmd->n_vecs, cmd->dim);
            if (passed_fd >= 0) close(passed_fd);
            send_error(client_fd, "build payload size overflow");
            return;
        }
    }

    /* ADR-059: the parallel-build leader can hand off N worker named-shm
     * partials instead of one merged corpus. That path is self-contained
     * (receives the descriptor list, mmaps each partial, builds via direct
     * multi-H2D). The multi path never carries an SCM_RIGHTS fd. */
    if (cmd->n_partials > 0)
    {
        if (passed_fd >= 0)
            close(passed_fd);
        handle_build_multi(client_fd, cmd, index_dir);
        return;
    }

    size_t vec_bytes = (size_t)cmd->n_vecs * cmd->dim * sizeof(float);
    size_t tid_bytes = (size_t)cmd->n_vecs * sizeof(uint64_t);
    size_t total     = vec_bytes + tid_bytes;

    /* ADR-057: memfd tier hands over the corpus as a passed fd (no /dev/shm
     * name, so a crashed build can never leave an orphan); the shm/heap tiers
     * name it in cmd->shm_key. Either way the mapping outlives the fd close. */
    void *mem;
    if (passed_fd >= 0)
    {
        LOG_INFO("[handle_build] corpus via memfd(SCM_RIGHTS) fd=%d total=%zu\n", passed_fd, total);
        mem = mmap(NULL, total, PROT_READ, MAP_SHARED, passed_fd, 0);
        close(passed_fd);   /* mapping holds the memory; daemon's ref is dropped here */
    }
    else
    {
        LOG_INFO("[handle_build] corpus via shm_open(%s)\n", cmd->shm_key);
        int shm_fd = shm_open(cmd->shm_key, O_RDONLY, 0);
        if (shm_fd < 0)
        {
            LOG_ERROR("[handle_build] shm_open FAILED errno=%d (%s)\n", errno, strerror(errno));
            send_error(client_fd, "shm_open failed");
            return;
        }
        if (!shm_check_peer_owner(shm_fd, g_peer_uid))   /* #87 finding 1 */
        {
            LOG_ERROR("[handle_build] shm owner mismatch for %s\n", cmd->shm_key);
            close(shm_fd);
            send_error(client_fd, "shm owner mismatch");
            return;
        }
        LOG_DEBUG("[handle_build] shm_open OK fd=%d\n", shm_fd);
        mem = mmap(NULL, total, PROT_READ, MAP_SHARED, shm_fd, 0);
        close(shm_fd);
    }
    if (mem == MAP_FAILED)
    {
        send_error(client_fd, "mmap failed");
        return;
    }

    const float    *vecs    = (const float *)mem;
    const uint64_t *tids_in = (const uint64_t *)((const char *)mem + vec_bytes);

    /* Phase 3F/3G: resolve the shard count, then dispatch.
     *   cmd->shard_count == 0 -> Phase 3G auto: derive from VRAM. Resolves to 1
     *      (unsharded) whenever the index fits one GPU, so small-index behavior
     *      is byte-identical to pre-3G. 0 from the helper means it won't fit even
     *      maximally sharded -> fail closed (CREATE INDEX aborts).
     *   cmd->shard_count == 1 -> forced unsharded.
     *   cmd->shard_count >= 2 -> forced N (build_sharded clamps to the corpus).
     * The build_sharded path is self-contained (owns munmap + reply); the
     * unsharded fall-through below is byte-identical to pre-3F behavior. */
    int resolved_sc = (int)cmd->shard_count;
    if (cmd->shard_count == 0)
    {
        /* Per-GPU budget: the smallest positive budget across usable GPUs (most
         * conservative; 0 if all are unlimited, which the helper treats as
         * "don't auto-shard"). */
        size_t budget = 0;
        int ng = n_usable_gpus();
        for (int i = 0; i < ng; i++)
        {
            size_t b = g_max_vram_per_gpu[usable_gpu(i)];
            if (b > 0 && (budget == 0 || b < budget))
                budget = b;
        }
        resolved_sc = cuvs_auto_shard_count(cmd->n_vecs, (int)cmd->dim, budget,
                                            ng, CUVS_SHARDS_MAX);
        if (resolved_sc == 0)
        {
            CuvsAdmissionFields rejection;
            admission_auto_shard_rejection(cmd, "single", &rejection);
            munmap(mem, total);
            send_admission_reject(client_fd, CUVS_STATUS_OOM_FALLBACK,
                "index too large for GPU VRAM even when fully sharded", &rejection);
            return;
        }
        LOG_INFO("[handle_build] auto shard count: %u/%u %lld vecs dim %u -> %d shard(s)\n",
                 cmd->db_oid, cmd->index_oid, (long long)cmd->n_vecs, cmd->dim, resolved_sc);
    }

    if (resolved_sc >= 2)
    {
        build_sharded(client_fd, cmd, index_dir, vecs, tids_in, total, mem, resolved_sc);
        return;
    }

    pthread_mutex_lock(&g_index_mutex);

    /* VRAM accounting: cuvs_cagra_build allocates BEFORE we free the old
     * index, so peak VRAM = existing + new. Ask for full 'needed' from
     * ensure_vram regardless of same-OID replacement. */
    /* I3 (C3): admit, reserve and release N = final + scratch; the registry
     * records final (finish_build_commit). */
    size_t final  = estimate_vram_bytes(cmd->n_vecs, (int)cmd->dim, (int)cmd->graph_degree);
    size_t needed = admission_needed_bytes(final);
    CuvsAdmissionFields rej, oom_rej;
    AdmitLine al = {.op = "BUILD", .path = "single",
                    .algo = admission_algo_name(cmd->build_algo), .gpu = -1,
                    .db = cmd->db_oid, .idx = cmd->index_oid, .dim = cmd->dim,
                    .gd = cmd->graph_degree ? cmd->graph_degree : 64,
                    .igd = cmd->intermediate_graph_degree ? cmd->intermediate_graph_degree : 128,
                    .n = cmd->n_vecs, .final = final, .needed = needed,
                    .shape = cuvs_build_shape_class(cmd->n_vecs, cmd->dim, cmd->metric,
                                                    cmd->graph_degree,
                                                    cmd->intermediate_graph_degree,
                                                    cmd->build_algo)};
    int target_gpu = pick_gpu_for_index(needed);
    if (target_gpu < 0)
    {
        LOG_WARN("[handle_build] index %u/%u (%zu bytes) too large for any GPU budget\n",
                 cmd->db_oid, cmd->index_oid, needed);
        admission_fill(&rej, CUVS_ADMISSION_AR0, needed, -1);
        log_admit(&al, CUVS_ADMISSION_AR0);
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        send_admission_reject(client_fd, CUVS_STATUS_OOM_FALLBACK,
                              "index too large for any GPU VRAM budget", &rej);
        return;
    }
    al.gpu = target_gpu;
    al.pending_before = g_pending_rmm_build_vram[target_gpu];
    al.evictions_before = g_cache_evictions[target_gpu];
    uint64_t self_unloaded = 0;
    int admit_rc = admit_build_cagra(needed, target_gpu, cmd->db_oid, cmd->index_oid,
                                     &self_unloaded, &al);
    note_build_admission(cmd->db_oid, cmd->index_oid, needed, target_gpu,
                         admit_rc == 0, self_unloaded);
    if (admit_rc < 0)
    {
        admission_fill(&rej, (CuvsAdmissionReason)g_last_admit_reason,
                       needed, target_gpu);
        if (al.workspace_valid)
            admission_workspace_fields(&rej, &al.workspace);
        admission_pool_fields(&rej, &al, target_gpu);
        if (rej.reason == CUVS_ADMISSION_AR5)
            rej.s = cuvs_admission_mib(admission_same_resident(cmd->db_oid, cmd->index_oid, target_gpu));
    }
    if (admit_rc < 0)
    {
        char legacy[128];
        LOG_WARN("[handle_build] VRAM exhausted on GPU %d for %u/%u "
                 "(budget %zu MB, used %zu MB, needed %zu MB)\n",
                 target_gpu, cmd->db_oid, cmd->index_oid,
                 g_max_vram_per_gpu[target_gpu] / (1024*1024),
                 total_vram_used(target_gpu) / (1024*1024),
                 needed / (1024*1024));
        log_admit(&al, rej.reason);
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        snprintf(legacy, sizeof(legacy), "VRAM exhausted on GPU %d after eviction",
                 target_gpu);
        send_admission_reject(client_fd, CUVS_STATUS_OOM_FALLBACK, legacy, &rej);
        return;
    }

    /* Snapshot for a later out-of-memory reply: free and resident at admission. */
    admission_fill(&oom_rej, CUVS_ADMISSION_OOM, needed, target_gpu);

    /* ADR-070 Bug #2: reserve the VRAM, then release g_index_mutex for the
     * multi-minute GPU build so concurrent searches/stats/drops are not blocked.
     * total_vram_used() counts g_pending_build_vram, so concurrent admission and
     * eviction stay correct while we build unlocked. vecs/mem are per-request
     * (not shared state), so reading them unlocked is safe. */
    log_build_begin(&al);
    reserve_build_vram(needed, target_gpu);
    /* I4: the active policy is passed only here and in handle_build_multi. */
    uint32_t lw_policy = g_cagra_build_overflow;
    uint64_t lw_ub = al.workspace.lw_ub;
    CuvsLwBuildStats lw_stats;
    char lw_notice[CUVS_ADMISSION_ERR_OUT_BYTES];
    lw_pending_reserve(lw_ub, target_gpu);
    int build_slot = build_status_register(&al, lw_policy, lw_ub);   /* I3-E */
    lw_warn_shape(cmd->n_vecs, cmd->dim, (int)cmd->intermediate_graph_degree,
                  cmd->db_oid, cmd->index_oid);
    log_admit(&al, CUVS_ADMISSION_NONE);
    pthread_mutex_unlock(&g_index_mutex);

    LOG_DEBUG("[handle_build] calling cuvs_cagra_build n_vecs=%lld dim=%u gpu=%d...\n",
            (long long)cmd->n_vecs, cmd->dim, target_gpu);
    al.build_started_us = admission_mono_us();
    CuvsCagraIndex new_handle = cuvs_cagra_build(vecs, cmd->n_vecs, (int)cmd->dim, cmd->metric,
                                                 (int)cmd->graph_degree,
                                                 (int)cmd->intermediate_graph_degree,
                                                 cmd->build_algo, target_gpu, lw_policy, &lw_stats);
    al.build_finished_us = admission_mono_us();
    /* ADR-086 (I3): an out-of-memory build after admission fails without
     * evicting any resident index; ADR-070's evict-to-fit retry is removed. */
    int build_oom = !new_handle && cuvs_last_build_was_oom();
    int build_att = !new_handle && !build_oom && cuvs_last_build_attach_failed();

    /* Reacquire for the durable-commit + registry tail (finish_build_commit
     * expects the lock held). Release the build reservation now: on success the
     * registry entry below carries vram_bytes; on failure the VRAM is freed. */
    pthread_mutex_lock(&g_index_mutex);
    log_build_end(&al, &lw_stats, new_handle != NULL && !build_att,
                  new_handle ? "built" : build_oom ? "oom" : build_att ? "att" : "failed");
    if (!new_handle)
        note_self_replace_failure(cmd->index_oid, self_unloaded);
    release_build_vram(needed, target_gpu);
    lw_pending_release(lw_ub, target_gpu);
    build_status_unregister(build_slot);   /* I3-E */
    (void)lw_build_finish("build", target_gpu, cmd->db_oid, cmd->index_oid, lw_policy,
                          lw_ub, &lw_stats, lw_notice, sizeof(lw_notice));
    log_release("BUILD", target_gpu, cmd->db_oid, cmd->index_oid, needed,
                new_handle ? "built" : build_oom ? "oom" : build_att ? "att" : "failed");

    if (build_att)
    {
        CuvsAdmissionFields att;
        admission_fill(&att, CUVS_ADMISSION_ATT, needed, target_gpu);
        cuvs_set_inject_build_oom(0);
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        send_admission_reject(client_fd, CUVS_STATUS_OOM_FALLBACK,
                              "dataset not attached to the CAGRA index", &att);
        return;
    }
    if (!new_handle && build_oom)
    {
        log_build_oom("BUILD", target_gpu, cmd->db_oid, cmd->index_oid, needed);
        cuvs_set_inject_build_oom(0);   /* #82: see the failure branch below */
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        send_admission_reject(client_fd, CUVS_STATUS_OOM_FALLBACK,
                              "GPU memory exhausted during build; no index evicted",
                              &oom_rej);
        return;
    }
    if (!new_handle)
    {
        LOG_ERROR("[handle_build] cuvs_cagra_build returned NULL\n");
        note_self_replace_failure(cmd->index_oid, self_unloaded);
        /* #82: the build-OOM injection is daemon-global and outlives the test
         * that armed it — a test that arms N failures then aborts under
         * ON_ERROR_STOP never reaches its disarm call, and the leftover arm
         * kills the next cagra build in an unrelated test. Clear it where a
         * cagra build gives up for good. Deliberately NOT in send_error_code():
         * EXTEND and COMPACT also report BUILD_FAILED, and a generic serializer
         * must not disarm a scenario that belongs to a different operation. */
        cuvs_set_inject_build_oom(0);
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED, "cuvs_cagra_build failed");
        return;
    }
    LOG_DEBUG("[handle_build] cuvs_cagra_build OK\n");

    uint64_t *new_tids = malloc(tid_bytes);
    if (!new_tids)
    {
        cuvs_cagra_free(new_handle, target_gpu);
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED, "malloc tids failed");
        return;
    }
    memcpy(new_tids, tids_in, tid_bytes);

    /* --- Disk persistence: tmp -> rename pattern --- */
    const char *save_dir = (index_dir[0] != '\0') ? index_dir : g_index_dir;
    mkdir(save_dir, 0700);

    /* Phase 3L: persist the raw vector matrix as a `.vectors` sidecar for GPU
     * brute-force search, written from `vecs` (the shm payload) while it is
     * still mapped — munmap follows. Best-effort: a write failure leaves
     * brute_force unavailable for this index but never fails the CAGRA build.
     * The generation token (this build's .tids body_crc32) lets the daemon
     * reject a stale sidecar after a torn build. Renamed at the commit point. */
    uint32_t tids_gen = cuvs_crc32(new_tids, (size_t)cmd->n_vecs * sizeof(uint64_t));
    char vecs_final[512], vecs_tmp[576];
    int  vectors_committed = 0;
    vectors_file_path(vecs_final, sizeof(vecs_final), save_dir, cmd->db_oid, cmd->index_oid);
    snprintf(vecs_tmp, sizeof(vecs_tmp), "%s.tmp", vecs_final);
    {
        FILE *vf = fopen(vecs_tmp, "wb");
        int vok = (vf != NULL);
        if (vok) fchmod_sidecar(vf, vecs_tmp);
        if (vok && cuvs_vectors_write(vf, cmd->n_vecs, cmd->dim, cmd->metric, tids_gen, vecs) != 0)
            vok = 0;
        if (vok && fflush(vf) != 0) vok = 0;
        if (vok && fsync(fileno(vf)) != 0) vok = 0;
        if (vf && fclose(vf) != 0) vok = 0;
        if (vok) {
            vectors_committed = 1;   /* tmp ready; renamed at the commit point */
        } else {
            if (vf) unlink(vecs_tmp);
            unlink(vecs_final);      /* drop any stale prior sidecar */
            LOG_WARN("[handle_build] .vectors sidecar not written for %u/%u; "
                     "brute_force unavailable until REINDEX\n",
                     cmd->db_oid, cmd->index_oid);
        }
    }
    munmap(mem, total);

    /* ADR-059: the persist/commit/registry/reply sequence is shared with the
     * multi-partial build path (handle_build_multi). */
    finish_build_commit(client_fd, cmd, save_dir, new_handle, new_tids,
                        target_gpu, final, vectors_committed, vecs_tmp, vecs_final,
                        tids_gen);
}

/* Persist a freshly built CAGRA index (tmp+rename), swap it into the registry,
 * and reply OK — the durable commit tail shared by the single-corpus and ADR-059
 * multi-partial build paths. On entry the caller holds g_index_mutex, has built
 * new_handle, assembled the contiguous new_tids, and staged the .vectors sidecar
 * (vectors_committed + vecs_tmp/vecs_final). This releases g_index_mutex. */
static void
finish_build_commit(int client_fd, const CuvsCmdFrame *cmd, const char *save_dir,
                    CuvsCagraIndex new_handle, uint64_t *new_tids, int target_gpu,
                    size_t needed, int vectors_committed, const char *vecs_tmp,
                    const char *vecs_final, uint32_t base_generation)
{
    char idx_final[512],  idx_tmp[576];
    char tids_final[512], tids_tmp[576];
    index_file_path(idx_final,  sizeof(idx_final),  save_dir, cmd->db_oid, cmd->index_oid);
    tids_file_path(tids_final,  sizeof(tids_final), save_dir, cmd->db_oid, cmd->index_oid);
    snprintf(idx_tmp,  sizeof(idx_tmp),  "%s.tmp", idx_final);
    snprintf(tids_tmp, sizeof(tids_tmp), "%s.tmp", tids_final);

#ifdef CUVS_TEST_HOOKS
    if (cuvs_fault("CUVS_FAULT_TIDS_WRITE")) {
        LOG_ERROR("[handle_build] fault injection: CUVS_FAULT_TIDS_WRITE\n");
        goto persist_fail;
    }
#endif
    if (write_tids_atomic(tids_tmp, cmd->n_vecs, cmd->dim, cmd->metric, new_tids) != 0)
        goto persist_fail;
    LOG_DEBUG("[handle_build] tids.tmp written + fsynced\n");

#ifdef CUVS_TEST_HOOKS
    if (cuvs_fault("CUVS_FAULT_SERIALIZE")) {
        LOG_ERROR("[handle_build] fault injection: CUVS_FAULT_SERIALIZE\n");
        goto persist_fail;
    }
#endif
    LOG_DEBUG("[handle_build] cuvs_cagra_serialize(%s)...\n", idx_tmp);
    if (cuvs_cagra_serialize(new_handle, idx_tmp, target_gpu) != 0) {
        LOG_ERROR("[handle_build] cuvs_cagra_serialize FAILED (path=%s)\n", idx_tmp);
        goto persist_fail;
    }
    chmod_sidecar(idx_tmp);
    if (fsync_path(idx_tmp) != 0) {
        LOG_ERROR("[handle_build] fsync %s failed errno=%d; aborting commit\n", idx_tmp, errno);
        goto persist_fail;
    }
    LOG_DEBUG("[handle_build] cagra.tmp written + fsynced\n");

    /* Commit point: atomic renames. tids first, then cagra (the file
     * startup_load_indexes scans for). If second rename fails, log and
     * unlink whatever we renamed to leave consistent state. */
#ifdef CUVS_TEST_HOOKS
    if (cuvs_fault("CUVS_FAULT_RENAME_TIDS")) {
        LOG_ERROR("[handle_build] fault injection: CUVS_FAULT_RENAME_TIDS\n");
        goto persist_fail;
    }
#endif
    if (rename(tids_tmp, tids_final) != 0) {
        LOG_ERROR("[handle_build] rename %s -> %s FAILED errno=%d (%s)\n",
                tids_tmp, tids_final, errno, strerror(errno));
        goto persist_fail;
    }
#ifdef CUVS_TEST_HOOKS
    if (cuvs_fault("CUVS_FAULT_RENAME_CAGRA")) {
        LOG_ERROR("[handle_build] fault injection: CUVS_FAULT_RENAME_CAGRA\n");
        unlink(tids_final);
        goto persist_fail;
    }
#endif
    if (rename(idx_tmp, idx_final) != 0) {
        LOG_ERROR("[handle_build] rename %s -> %s FAILED errno=%d (%s); "
                  "unlinking tids to avoid mismatch\n",
                idx_tmp, idx_final, errno, strerror(errno));
        unlink(tids_final);
        goto persist_fail;
    }
    /* fsync directory so the rename(s) are durable. */
    int dir_fd = open(save_dir, O_RDONLY);
    if (dir_fd >= 0) { fsync(dir_fd); close(dir_fd); }

    /* Phase 3L: commit the .vectors sidecar. Non-fatal — the index is already
     * durably committed above (cagra + tids renamed); a rename failure only
     * disables brute_force. The generation token guards a torn post-cagra,
     * pre-vectors crash: a stale sidecar is rejected at load. */
    if (vectors_committed) {
        if (rename(vecs_tmp, vecs_final) != 0) {
            LOG_WARN("[handle_build] rename %s -> %s failed errno=%d; brute_force unavailable\n",
                     vecs_tmp, vecs_final, errno);
            unlink(vecs_tmp);
            unlink(vecs_final);
            vectors_committed = 0;
        } else {
            int vdir_fd = open(save_dir, O_RDONLY);
            if (vdir_fd >= 0) { fsync(vdir_fd); close(vdir_fd); }
        }
    }

    /* A rebuild produces a fresh graph reflecting the current heap — clear any
     * persisted staleness marker. */
    {
        char stale_path[512];
        stale_file_path(stale_path, sizeof(stale_path), save_dir,
                        cmd->db_oid, cmd->index_oid);
        unlink(stale_path);
    }

    /* Phase 3F: this unsharded build replaces any prior sharded artifacts for
     * the same OID. Remove the .shards manifest FIRST (so a reload can never
     * take the sharded path), then the contiguous shard .cagra files. */
    {
        char shards_path[512];
        shards_manifest_path(shards_path, sizeof(shards_path), save_dir,
                             cmd->db_oid, cmd->index_oid);
        unlink(shards_path);
        for (uint32_t s = 0; s < CUVS_SHARDS_MAX; s++)
        {
            char sp[512];
            shard_cagra_path(sp, sizeof(sp), save_dir, cmd->db_oid, cmd->index_oid, s);
            if (unlink(sp) != 0)
                break;   /* contiguous ids: first miss => no more shards */
        }
    }

    LOG_DEBUG("[handle_build] disk commit OK\n");

    /* Phase 3I-1: write CPU HNSW fallback sidecar — only when the backend
     * requested it (cuvs.cpu_hnsw_fallback=on or a 3I import_hnsw flow).
     * Skipping this for CAGRA-only builds avoids ~30s of from_cagra() CPU
     * work that would never be used. */
    if (cmd->use_cpu_hnsw)
    {
        char hnsw_path[512];
        hnsw_file_path(hnsw_path, sizeof(hnsw_path), save_dir,
                       cmd->db_oid, cmd->index_oid);
        int hrc = cuvs_hnsw_serialize(new_handle, hnsw_path, target_gpu);
        if (hrc == CUVS_HNSW_SERIALIZE_SKIPPED)
            /* #106: no file was written — say so, so the log does not claim a
             * sidecar that a later cpu_hnsw search will fail to open. */
            LOG_WARN("[handle_build] no HNSW fallback sidecar for %u/%u: "
                     "fewer than %d vectors; cpu_hnsw searches will use GPU CAGRA\n",
                     cmd->db_oid, cmd->index_oid, CUVS_HNSW_MIN_ELEMENTS);
        else if (hrc != CUVS_HNSW_SERIALIZE_OK)
            LOG_WARN("[handle_build] HNSW fallback sidecar not saved for %u/%u\n",
                     cmd->db_oid, cmd->index_oid);
        else
        {
            chmod_sidecar(hnsw_path);
            LOG_INFO("pg_cuvs_server: saved HNSW fallback %u/%u\n",
                     cmd->db_oid, cmd->index_oid);
        }
    }

    /* --- Swap into registry --- */
    IndexEntry *existing = find_index(cmd->db_oid, cmd->index_oid);
    if (existing) {
        /* Phase 3F: a prior sharded instance is replaced by this unsharded
         * build — free all its shard handles, not a (NULL) single handle. */
        if (existing->shard_count >= 2)
            free_index_shards(existing);
        else
            cuvs_cagra_free(existing->handle, existing->gpu_device_id);
        free(existing->tids);
        free(existing->rev_tids);     existing->rev_tids     = NULL;
        free(existing->rev_item_ids); existing->rev_item_ids = NULL;
        existing->shard_count = 0;
        existing->shards      = NULL;
        existing->gpu_device_id = (uint32_t)target_gpu;
        existing->dim         = cmd->dim;
        existing->metric      = cmd->metric;
        existing->n_vecs      = cmd->n_vecs;
        existing->handle      = new_handle;
        existing->tids        = new_tids;
        existing->vram_bytes  = needed;
        existing->last_search = time(NULL);
        existing->valid       = 1;
        existing->stale       = 0;     /* rebuilt -> fresh */
        existing->stale_since = 0;
        /* The rebuilt base obsoletes the old delta cache (the backend unlinks
         * .delta on a successful build); drop the resident GPU delta now. */
        free_delta_cache(existing);
        /* Phase 3I-1: drop cached CPU HNSW (new sidecar was just written). */
        if (existing->hnsw_idx) { cuvs_hnsw_free(existing->hnsw_idx); existing->hnsw_idx = NULL; }
        /* 3P: CAGRA rebuild clears any prior IVF-PQ handle on the same slot. */
        if (existing->ivfpq_handle) { cuvs_ivfpq_free(existing->ivfpq_handle, existing->gpu_device_id); existing->ivfpq_handle = NULL; }
        existing->last_search_mode = 0;
        reset_entry_stats(existing);   /* fresh index instance */
        existing->graph_degree = (int)cmd->graph_degree;  /* for extend/compact VRAM est */
        existing->n_extended = 0;      /* REINDEX resets the extend counter */
        /* I-FC (i): build saved its files first, but eviction saves/reloads
         * through g_index_dir only, so a per-index index_dir is never current. */
        existing->file_current = strcmp(save_dir, g_index_dir) == 0;
        existing->compact_count++;     /* every rebuild counts as a compaction */
        existing->last_compact_at = time(NULL);
        build_rev_tid_map(existing);   /* rebuild map for new tids/n_vecs */
    } else {
        int registry_full;
        g_cur_requester_idx = cmd->index_oid;
        if (g_n_indexes >= g_max_indexes)
            (void) make_slot(target_gpu, CUVS_RQ_BUILD_CAGRA);
        registry_full = g_n_indexes >= g_max_indexes;
#ifdef CUVS_TEST_HOOKS
        registry_full |= cuvs_fault("CUVS_FAULT_REGISTRY_FULL");
#endif
        if (registry_full) {
            /* Registry full and the LRU couldn't be evicted (e.g. all entries
             * inflight on other GPUs). The artifacts are already durable on
             * disk, so DON'T roll back — free GPU resources and reply OK; the
             * first query reloads from disk (load_index evicts for a slot).
             * Mirrors build_sharded's defer-to-reload. I3 (v6 1.4 (g)): the
             * requester gets a NOTICE through notice_out instead of silence. */
            LOG_WARN("[handle_build] registry full; %u/%u persisted, will reload on demand\n",
                     cmd->db_oid, cmd->index_oid);
            (void) vectors_committed;   /* .vectors stays on disk for the reload */
            cuvs_cagra_free(new_handle, target_gpu);
            free(new_tids);
            pthread_mutex_unlock(&g_index_mutex);
            CuvsReplyHeader hdr_ok = {0};
            hdr_ok.status = CUVS_STATUS_OK;
            snprintf(hdr_ok.notice_out, sizeof(hdr_ok.notice_out),
                     "pg_cuvs: the GPU index registry is full (%d slots); the index "
                     "was saved to disk and loads into GPU memory on its first query",
                     g_max_indexes);
            attach_build_notices(&hdr_ok);
            send_all(client_fd, &hdr_ok, sizeof(hdr_ok));
            return;
        }
        IndexEntry *e = registry_new_slot();
        e->db_oid      = cmd->db_oid;
        e->index_oid   = cmd->index_oid;
        e->dim         = cmd->dim;
        e->metric      = cmd->metric;
        e->n_vecs      = cmd->n_vecs;
        e->handle      = new_handle;
        e->tids        = new_tids;
        e->vram_bytes  = needed;
        e->last_search = time(NULL);
        e->valid       = 1;
        e->stale       = 0;
        e->stale_since = 0;
        e->delta_idx = NULL; e->delta_tids = NULL; e->n_delta = 0;
        e->delta_generation = 0; e->delta_mtime = 0; e->delta_vram_bytes = 0;
        e->delta_vecs_host = NULL; e->delta_n_cached = 0;
        /* Phase 3L: main BF cache starts empty; lazily built on a BF search. */
        e->main_bf_idx = NULL; e->main_bf_n = 0; e->main_bf_vram_bytes = 0;
        e->main_bf_mtime = 0; e->main_bf_generation = 0; e->bf_precision = 0;
        e->warmup_state = WARMUP_HOT;
        e->gpu_device_id = (uint32_t)target_gpu;
        e->shard_count = 0;     /* unsharded; slot may be reused post-eviction */
        e->shards      = NULL;
        e->hnsw_idx     = NULL;  /* Phase 3I-1: loaded lazily on first cpu_hnsw request */
        e->ivfpq_handle = NULL;  /* 3P: CAGRA build never sets an IVF-PQ handle */
        e->ivfpq_n_vecs = 0;
        e->ivfpq_vram_bytes = 0;
        e->last_search_mode = 0;
        e->n_extended        = 0;   /* slot may be reused; must not carry stale counter */
        e->compact_count     = 0;
        e->last_compact_at   = 0;
        e->rev_tids = NULL;  /* evicted slot may have stale freed ptr; NULL it */
        e->rev_item_ids = NULL;
        /* 3O: build the heapTID->item_id reverse map at build time (not only on
         * load). Without this a freshly-built resident index has no map, so 3O
         * prefilter and ADR-064 streaming BF silently fall back until a restart
         * or in-place REINDEX. e->tids/e->n_vecs are set above. This covers both
         * handle_build and handle_build_multi (both finalize through here). */
        build_rev_tid_map(e);
        reset_entry_stats(e);
        e->graph_degree = (int)cmd->graph_degree;  /* for extend/compact VRAM est */
        e->file_current = strcmp(save_dir, g_index_dir) == 0;   /* I-FC (i), as above */
    }

    pthread_mutex_unlock(&g_index_mutex);
    LOG_DEBUG("[handle_build] sending OK reply\n");

    LOG_INFO("pg_cuvs_server: built index %u/%u (%lld vecs, %zu MB VRAM)\n",
            cmd->db_oid, cmd->index_oid, (long long)cmd->n_vecs, needed / (1024*1024));

    CuvsReplyHeader hdr_ok = {0};
    hdr_ok.status = CUVS_STATUS_OK;
    attach_build_notices(&hdr_ok);
    send_all(client_fd, &hdr_ok, sizeof(hdr_ok));

    /* Phase 3C: upload to GCS in a detached thread after replying OK.
     * CREATE INDEX has already returned; upload failure is non-fatal. */
    if (g_snapshot_uri[0] != '\0' && cmd->relfilenode != 0)
    {
        UploadThreadArgs *ua = malloc(sizeof(*ua));
        if (ua)
        {
            strncpy(ua->snapshot_uri, g_snapshot_uri, sizeof(ua->snapshot_uri) - 1);
            strncpy(ua->cluster_id,   g_cluster_id,   sizeof(ua->cluster_id)   - 1);
            strncpy(ua->gcs_key_file, g_gcs_key_file, sizeof(ua->gcs_key_file) - 1);
            strncpy(ua->cagra_path,   idx_final,       sizeof(ua->cagra_path)   - 1);
            strncpy(ua->tids_path,    tids_final,      sizeof(ua->tids_path)    - 1);
            ua->db_oid      = cmd->db_oid;
            ua->table_oid   = cmd->table_oid;
            ua->index_oid   = cmd->index_oid;
            ua->relfilenode = cmd->relfilenode;
            ua->metric      = cmd->metric;
            ua->dim         = cmd->dim;
            ua->vector_count = cmd->n_vecs;
            ua->base_generation = base_generation;

            pthread_t upload_tid;
            pthread_attr_t upload_attr;
            pthread_attr_init(&upload_attr);
            pthread_attr_setdetachstate(&upload_attr, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&upload_tid, &upload_attr, objstore_upload_thread, ua) != 0)
            {
                LOG_WARN("objstore: failed to spawn upload thread for %u/%u\n",
                         cmd->db_oid, cmd->index_oid);
                free(ua);
            }
            pthread_attr_destroy(&upload_attr);
        }
    }
    return;

persist_fail:
    /* Disk persistence failed before commit. Existing entry (if any) remains
     * intact in registry. Clean up new resources and tmp files. The prior
     * `.vectors` (if any) is left intact — it still matches the surviving
     * (un-renamed) prior `.tids`/`.cagra`; only our new tmp is removed. */
    unlink(idx_tmp);
    unlink(tids_tmp);
    unlink(vecs_tmp);
    cuvs_cagra_free(new_handle, target_gpu);
    free(new_tids);
    pthread_mutex_unlock(&g_index_mutex);
    send_error_code(client_fd, CUVS_STATUS_PERSIST_FAILED, "disk persistence failed");
}

/* ----------------------------------------------------------------
 * ADR-059: multi-partial CAGRA build.
 *
 * The parallel-build leader hands off N worker named-shm partials (each
 * [vectors][tids] for its scanned share) instead of one merged corpus. We mmap
 * each partial and, for the common single-shard case, build via
 * cuvs_cagra_build_multi — streaming each partial straight to the GPU with no
 * host-side corpus copy (eliminating the leader merge that capped ADR-058). The
 * global TID array (small) is assembled host-side for persistence; the .vectors
 * sidecar is streamed from the partials. Multi-shard falls back to a contiguous
 * host assembly + build_sharded. The leader owns the partials' lifetime and
 * unlinks them after we reply.
 * ---------------------------------------------------------------- */
static void
handle_build_multi(int client_fd, const CuvsCmdFrame *cmd, const char *index_dir)
{
    clear_build_notices();
    uint32_t       n_parts   = cmd->n_partials;
    CuvsPartialDesc *descs    = NULL;
    const float  **part_vecs  = NULL;
    int64_t       *n_each     = NULL;
    void         **maps       = NULL;
    size_t        *map_lens   = NULL;
    uint64_t      *new_tids   = NULL;
    int            nmapped    = 0;
    int64_t        total      = cmd->n_vecs;
    size_t         per_vec    = (size_t)cmd->dim * sizeof(float);
    size_t         vec_bytes  = (size_t)total * per_vec;
    size_t         tid_bytes  = (size_t)total * sizeof(uint64_t);
    const char    *err        = NULL;
    int            status     = CUVS_STATUS_ERROR;
    int            resolved_sc;
    const char    *save_dir;
    CuvsAdmissionFields rej;          /* I3: err_out of a rejection reply */
    int            rej_set    = 0;

    if (n_parts == 0 || n_parts > CUVS_BUILD_MAX_PARTIALS)
    {
        LOG_ERROR("[handle_build_multi] invalid n_partials=%u\n", n_parts);
        send_error(client_fd, "invalid n_partials");
        return;
    }

    descs     = malloc((size_t)n_parts * sizeof(*descs));
    part_vecs = malloc((size_t)n_parts * sizeof(*part_vecs));
    n_each    = malloc((size_t)n_parts * sizeof(*n_each));
    maps      = malloc((size_t)n_parts * sizeof(*maps));
    map_lens  = malloc((size_t)n_parts * sizeof(*map_lens));
    if (!descs || !part_vecs || !n_each || !maps || !map_lens)
    {
        err = "malloc (partial bookkeeping) failed";
        goto fail;
    }

    if (recv_all(client_fd, descs, (size_t)n_parts * sizeof(*descs)) < 0)
    {
        err = "recv partial descriptors failed";
        goto fail;
    }

    /* Validate the list before touching shm: name format + Σ n_vecs == corpus
     * size (so positional (vector,tid) pairing across partials is complete). */
    {
        int64_t sum = 0;
        for (uint32_t i = 0; i < n_parts; i++)
        {
            if (descs[i].n_vecs < 0)  { err = "negative partial n_vecs"; goto fail; }
            if (descs[i].n_vecs == 0) continue;
            if (strncmp(descs[i].shm_name, "/pg_cuvs_bld_", 13) != 0)
            {
                err = "partial shm name not /pg_cuvs_bld_*";
                goto fail;
            }
            sum += descs[i].n_vecs;
        }
        if (sum != total)
        {
            LOG_ERROR("[handle_build_multi] sum partial n_vecs %lld != total %lld\n",
                      (long long)sum, (long long)total);
            err = "partial n_vecs sum != corpus size";
            goto fail;
        }
    }

    /* mmap each non-empty partial (host) after verifying its exact size. */
    for (uint32_t i = 0; i < n_parts; i++)
    {
        size_t pvb, ptb, plen;
        int    pfd;
        struct stat st;
        void  *pm;

        if (descs[i].n_vecs == 0)
            continue;
        pvb  = (size_t)descs[i].n_vecs * per_vec;
        ptb  = (size_t)descs[i].n_vecs * sizeof(uint64_t);
        plen = pvb + ptb;

        pfd = shm_open(descs[i].shm_name, O_RDONLY, 0);
        if (pfd < 0)              { err = "shm_open partial failed"; goto fail; }
        if (!shm_check_peer_owner(pfd, g_peer_uid)) { close(pfd); err = "shm owner mismatch (partial)"; goto fail; }   /* #87 finding 1 */
        if (fstat(pfd, &st) != 0) { close(pfd); err = "fstat partial failed"; goto fail; }
        if ((size_t)st.st_size != plen)
        {
            LOG_ERROR("[handle_build_multi] partial %s size %lld != expected %zu\n",
                      descs[i].shm_name, (long long)st.st_size, plen);
            close(pfd);
            err = "partial size mismatch";
            goto fail;
        }
        pm = mmap(NULL, plen, PROT_READ, MAP_SHARED, pfd, 0);
        close(pfd);
        if (pm == MAP_FAILED)     { err = "mmap partial failed"; goto fail; }

        maps[nmapped]      = pm;
        map_lens[nmapped]  = plen;
        part_vecs[nmapped] = (const float *)pm;
        n_each[nmapped]    = descs[i].n_vecs;
        nmapped++;
    }
    if (nmapped == 0)             { err = "no non-empty partials"; goto fail; }

    LOG_INFO("[handle_build_multi] %d partial(s), total=%lld dim=%u (direct multi-H2D)\n",
             nmapped, (long long)total, cmd->dim);

    /* Assemble the global TID array (small: total*8 bytes). */
    new_tids = malloc(tid_bytes);
    if (!new_tids)                { err = "malloc tids failed"; goto fail; }
    {
        size_t toff = 0;
        for (int j = 0; j < nmapped; j++)
        {
            size_t pvb = (size_t)n_each[j] * per_vec;
            size_t ptb = (size_t)n_each[j] * sizeof(uint64_t);
            memcpy((char *)new_tids + toff, (const char *)maps[j] + pvb, ptb);
            toff += ptb;
        }
    }

    /* Resolve shard count — identical policy to the single-corpus path. */
    resolved_sc = (int)cmd->shard_count;
    if (cmd->shard_count == 0)
    {
        size_t budget = 0;
        int ng = n_usable_gpus();
        for (int i = 0; i < ng; i++)
        {
            size_t b = g_max_vram_per_gpu[usable_gpu(i)];
            if (b > 0 && (budget == 0 || b < budget))
                budget = b;
        }
        resolved_sc = cuvs_auto_shard_count(total, (int)cmd->dim, budget, ng, CUVS_SHARDS_MAX);
        if (resolved_sc == 0)
        {
            admission_auto_shard_rejection(cmd, "multi", &rej);
            rej_set = 1;
            status = CUVS_STATUS_OOM_FALLBACK;
            err = "index too large for GPU VRAM even when fully sharded";
            goto fail;
        }
    }

    save_dir = (index_dir[0] != '\0') ? index_dir : g_index_dir;
    mkdir(save_dir, 0700);

    /* Multi-shard fallback: partials cross shard boundaries, so assemble a
     * contiguous [vecs][tids] corpus (anonymous mmap, released by build_sharded's
     * munmap) and reuse the existing sharded build. Rare large-index path. */
    if (resolved_sc >= 2)
    {
        size_t corpus_bytes = vec_bytes + tid_bytes;
        void  *mem = mmap(NULL, corpus_bytes, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        size_t voff = 0;
        if (mem == MAP_FAILED)    { err = "mmap merged corpus failed"; goto fail; }
        for (int j = 0; j < nmapped; j++)
        {
            size_t pvb = (size_t)n_each[j] * per_vec;
            memcpy((char *)mem + voff, maps[j], pvb);
            voff += pvb;
        }
        memcpy((char *)mem + vec_bytes, new_tids, tid_bytes);

        for (int j = 0; j < nmapped; j++)
            munmap(maps[j], map_lens[j]);    /* copied out; release partials */
        free(new_tids); new_tids = NULL;
        free(descs); free(part_vecs); free(n_each); free(maps); free(map_lens);

        build_sharded(client_fd, cmd, index_dir,
                      (const float *)mem,
                      (const uint64_t *)((char *)mem + vec_bytes),
                      corpus_bytes, mem, resolved_sc);
        return;   /* build_sharded owns the reply + munmap(mem) */
    }

    /* Single shard: direct multi-H2D — the ADR-059 win (no host corpus copy). */
    pthread_mutex_lock(&g_index_mutex);
    {
        size_t final  = estimate_vram_bytes(total, (int)cmd->dim, (int)cmd->graph_degree);
        size_t needed = admission_needed_bytes(final);
        int    target_gpu = pick_gpu_for_index(needed);
        CuvsCagraIndex new_handle;
        CuvsAdmissionFields oom_rej;
        uint32_t tids_gen;
        char vecs_final[512], vecs_tmp[576];
        int  vectors_committed = 0;
        int  build_oom, build_att;
        AdmitLine al = {.op = "BUILD", .path = "multi",
                        .algo = admission_algo_name(cmd->build_algo), .gpu = target_gpu,
                        .db = cmd->db_oid, .idx = cmd->index_oid, .dim = cmd->dim,
                        .gd = cmd->graph_degree ? cmd->graph_degree : 64,
                        .igd = cmd->intermediate_graph_degree ? cmd->intermediate_graph_degree : 128,
                        .n = total, .final = final, .needed = needed,
                        .shape = cuvs_build_shape_class(total, cmd->dim, cmd->metric,
                                                        cmd->graph_degree,
                                                        cmd->intermediate_graph_degree,
                                                        cmd->build_algo)};

        if (target_gpu < 0)
        {
            admission_fill(&rej, CUVS_ADMISSION_AR0, needed, -1);
            rej_set = 1;
            log_admit(&al, CUVS_ADMISSION_AR0);
            pthread_mutex_unlock(&g_index_mutex);
            status = CUVS_STATUS_OOM_FALLBACK;
            err = "index too large for any GPU VRAM budget";
            goto fail;
        }
        al.pending_before = g_pending_rmm_build_vram[target_gpu];
        al.evictions_before = g_cache_evictions[target_gpu];
        uint64_t self_unloaded = 0;
        int admit_rc = admit_build_cagra(needed, target_gpu, cmd->db_oid,
                                         cmd->index_oid, &self_unloaded, &al);
        note_build_admission(cmd->db_oid, cmd->index_oid, needed, target_gpu,
                             admit_rc == 0, self_unloaded);
        if (admit_rc < 0)
        {
            admission_fill(&rej, (CuvsAdmissionReason)g_last_admit_reason,
                           needed, target_gpu);
            if (al.workspace_valid)
                admission_workspace_fields(&rej, &al.workspace);
            admission_pool_fields(&rej, &al, target_gpu);
            if (rej.reason == CUVS_ADMISSION_AR5)
                rej.s = cuvs_admission_mib(admission_same_resident(cmd->db_oid, cmd->index_oid, target_gpu));
        }
        if (admit_rc < 0)
        {
            rej_set = 1;
            log_admit(&al, rej.reason);
            pthread_mutex_unlock(&g_index_mutex);
            status = CUVS_STATUS_OOM_FALLBACK;
            err = "VRAM exhausted after eviction";
            goto fail;
        }
        admission_fill(&oom_rej, CUVS_ADMISSION_OOM, needed, target_gpu);

        /* ADR-070 Bug #2: reserve VRAM, then release the lock for the multi-H2D
         * GPU build so concurrent searches/stats/drops aren't blocked. Partials
         * (part_vecs/maps) are per-request, so reading them unlocked is safe;
         * total_vram_used() counts g_pending_build_vram during the window. */
        log_build_begin(&al);
    reserve_build_vram(needed, target_gpu);
        /* I4: the active policy is passed only here and in handle_build. */
        uint32_t lw_policy = g_cagra_build_overflow;
        uint64_t lw_ub = al.workspace.lw_ub;
        CuvsLwBuildStats lw_stats;
        char lw_notice[CUVS_ADMISSION_ERR_OUT_BYTES];
        lw_pending_reserve(lw_ub, target_gpu);
        int build_slot = build_status_register(&al, lw_policy, lw_ub);   /* I3-E */
        lw_warn_shape(total, cmd->dim, (int)cmd->intermediate_graph_degree,
                      cmd->db_oid, cmd->index_oid);
        log_admit(&al, CUVS_ADMISSION_NONE);
        pthread_mutex_unlock(&g_index_mutex);

        al.build_started_us = admission_mono_us();
        new_handle = cuvs_cagra_build_multi(part_vecs, n_each, nmapped, total,
                                            (int)cmd->dim, cmd->metric,
                                            (int)cmd->graph_degree,
                                            (int)cmd->intermediate_graph_degree,
                                            cmd->build_algo, target_gpu, lw_policy, &lw_stats);
    al.build_finished_us = admission_mono_us();
        /* ADR-086 (I3): no evict-to-fit; an OOM after admission is a failure. */
        build_oom = !new_handle && cuvs_last_build_was_oom();
        build_att = !new_handle && !build_oom && cuvs_last_build_attach_failed();

        /* Reacquire for the durable-commit + registry tail (finish_build_commit
         * expects the lock held). Release the reservation: on success the registry
         * entry carries vram_bytes; on failure the VRAM is freed. */
        pthread_mutex_lock(&g_index_mutex);
        log_build_end(&al, &lw_stats, new_handle != NULL && !build_att,
                      new_handle ? "built" : build_oom ? "oom" : build_att ? "att" : "failed");
        if (!new_handle)
            note_self_replace_failure(cmd->index_oid, self_unloaded);
        release_build_vram(needed, target_gpu);
        lw_pending_release(lw_ub, target_gpu);
        build_status_unregister(build_slot);   /* I3-E */
        (void)lw_build_finish("build_multi", target_gpu, cmd->db_oid, cmd->index_oid, lw_policy,
                              lw_ub, &lw_stats, lw_notice, sizeof(lw_notice));
        log_release("BUILD", target_gpu, cmd->db_oid, cmd->index_oid, needed,
                    new_handle ? "built" : build_oom ? "oom" : build_att ? "att" : "failed");

        if (build_att)
        {
            cuvs_set_inject_build_oom(0);
            admission_fill(&rej, CUVS_ADMISSION_ATT, needed, target_gpu);   /* under the mutex */
            pthread_mutex_unlock(&g_index_mutex);
            rej_set = 1;
            status = CUVS_STATUS_OOM_FALLBACK;
            err = "dataset not attached to the CAGRA index";
            goto fail;
        }
        if (!new_handle && build_oom)
        {
            log_build_oom("BUILD", target_gpu, cmd->db_oid, cmd->index_oid, needed);
            cuvs_set_inject_build_oom(0);   /* #82: see handle_build */
            pthread_mutex_unlock(&g_index_mutex);
            rej = oom_rej;
            rej_set = 1;
            status = CUVS_STATUS_OOM_FALLBACK;
            err = "GPU memory exhausted during build; no index evicted";
            goto fail;
        }
        if (!new_handle)
        {
            note_self_replace_failure(cmd->index_oid, self_unloaded);
            cuvs_set_inject_build_oom(0);   /* #82: see handle_build */
            pthread_mutex_unlock(&g_index_mutex);
            status = CUVS_STATUS_BUILD_FAILED;
            err = "cuvs_cagra_build_multi failed";
            goto fail;
        }

        /* .vectors sidecar streamed from the partials (no contiguous host copy). */
        tids_gen = cuvs_crc32(new_tids, tid_bytes);
        vectors_file_path(vecs_final, sizeof(vecs_final), save_dir, cmd->db_oid, cmd->index_oid);
        snprintf(vecs_tmp, sizeof(vecs_tmp), "%s.tmp", vecs_final);
        {
            FILE *vf = fopen(vecs_tmp, "wb");
            int vok = (vf != NULL);
            if (vok) fchmod_sidecar(vf, vecs_tmp);
            if (vok && cuvs_vectors_write_multi(vf, n_each, nmapped, cmd->dim,
                                                cmd->metric, tids_gen, part_vecs) != 0)
                vok = 0;
            if (vok && fflush(vf) != 0) vok = 0;
            if (vok && fsync(fileno(vf)) != 0) vok = 0;
            if (vf && fclose(vf) != 0) vok = 0;
            if (vok)
                vectors_committed = 1;
            else
            {
                if (vf) unlink(vecs_tmp);
                unlink(vecs_final);
                LOG_WARN("[handle_build_multi] .vectors sidecar not written for %u/%u; "
                         "brute_force unavailable until REINDEX\n",
                         cmd->db_oid, cmd->index_oid);
            }
        }

        /* Done with the partials; the GPU build holds its own device copy. */
        for (int j = 0; j < nmapped; j++)
            munmap(maps[j], map_lens[j]);
        free(descs); free(part_vecs); free(n_each); free(maps); free(map_lens);

        finish_build_commit(client_fd, cmd, save_dir, new_handle, new_tids,
                            target_gpu, final, vectors_committed, vecs_tmp, vecs_final,
                            tids_gen);
        return;   /* new_tids ownership passed to the registry */
    }

fail:
    for (int j = 0; j < nmapped; j++)
        munmap(maps[j], map_lens[j]);
    free(new_tids);
    free(descs); free(part_vecs); free(n_each); free(maps); free(map_lens);
    if (rej_set)
        send_admission_reject(client_fd, status, err ? err : "multi-partial build failed",
                              &rej);
    else if (status == CUVS_STATUS_ERROR)
        send_error(client_fd, err ? err : "multi-partial build failed");
    else
        send_error_code(client_fd, status, err ? err : "multi-partial build failed");
}

/* ----------------------------------------------------------------
 * Handle STATS command (CUVS_OP_STATUS)
 *
 * Returns per-index search statistics for the requesting database. The
 * daemon is the source of truth: counters are cross-backend and survive
 * individual PG sessions (but reset on index rebuild/reload). With
 * cmd->index_oid == 0, every resident index in cmd->db_oid is returned;
 * otherwise just that one. Reply is the standard header (n_results = count)
 * followed by n_results × CuvsIndexStats.
 * ---------------------------------------------------------------- */
static void
handle_stats(int client_fd, const CuvsCmdFrame *cmd)
{
    pthread_mutex_lock(&g_index_mutex);

    /* Bounded by g_max_indexes; gather under the lock, send after unlocking.
     * Heap-allocated — g_max_indexes can be large, so never a stack VLA. */
    CuvsIndexStats *stats = malloc((size_t)g_max_indexes * sizeof(CuvsIndexStats));
    uint32_t n = 0;

    if (!stats)
    {
        pthread_mutex_unlock(&g_index_mutex);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_OK;   /* empty stats, not an error */
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    for (int i = 0; i < g_n_indexes && n < (uint32_t)g_max_indexes; i++)
    {
        IndexEntry *e = &g_indexes[i];
        if (!e->valid)
            continue;
        if (e->db_oid != cmd->db_oid)
            continue;
        if (cmd->index_oid != 0 && e->index_oid != cmd->index_oid)
            continue;

        CuvsIndexStats *s = &stats[n++];
        memset(s, 0, sizeof(*s));
        s->db_oid           = e->db_oid;
        s->index_oid        = e->index_oid;
        s->dim              = e->dim;
        s->metric           = e->metric;
        s->n_vecs           = e->n_vecs;
        if (e->shard_count >= 2)
        {
            uint64_t vb = 0;
            for (int s2 = 0; s2 < e->shard_count; s2++)
                vb += e->shards[s2].vram_bytes;
            s->vram_bytes = vb;     /* logical index VRAM = sum of shards */
        }
        else
            s->vram_bytes = e->vram_bytes;
        s->resident         = 1;
        s->last_status      = e->last_status;
        s->last_requested_k = e->last_requested_k;
        s->last_returned_k  = e->last_returned_k;
        s->search_count     = e->search_count;
        s->error_count      = e->error_count;
        s->total_latency_us = e->total_latency_us;
        s->p50_us = cuvs_lat_percentile(e->lat_buckets, CUVS_LAT_BUCKETS, 0.50);
        s->p95_us = cuvs_lat_percentile(e->lat_buckets, CUVS_LAT_BUCKETS, 0.95);
        s->p99_us = cuvs_lat_percentile(e->lat_buckets, CUVS_LAT_BUCKETS, 0.99);
        s->last_search_at   = (int64_t)e->last_search;
        s->stale            = (uint32_t)e->stale;
        s->stale_since      = (int64_t)e->stale_since;
        strncpy(s->last_error, e->last_error, sizeof(s->last_error) - 1);
        s->delta_rows         = e->n_delta;
        s->delta_generation   = e->delta_generation;
        s->delta_vram_bytes   = e->delta_vram_bytes;
        s->delta_merged_count = e->delta_merged_count;
        s->delta_search_mode  = (e->delta_idx && e->n_delta > 0) ? 2 : 0;
        s->warmup_state       = (uint32_t)e->warmup_state;
        s->last_warmup_at     = (int64_t)e->last_warmup_at;       /* Phase 3D: retained post-hydration */
        s->warmup_duration_ms = e->warmup_duration_ms;
        s->download_count     = (uint32_t)e->download_count;
        s->cache_miss_count   = e->cache_miss_count;
        s->gpu_device_id      = e->gpu_device_id;   /* 0xFFFFFFFF when sharded */
        s->shard_count        = (uint32_t)e->shard_count;
        s->search_mode        = e->last_search_mode; /* Phase 3I-1 */
        s->bf_batch_count     = e->bf_batch_count;   /* Phase 3L-9 */
        s->cagra_batch_count  = e->cagra_batch_count; /* #160 */
        s->prefilter_fallback_count = e->prefilter_fallback_count; /* #133 */
        s->n_extended         = e->n_extended;
        s->compact_count      = e->compact_count;
        s->last_compact_at    = (int64_t)e->last_compact_at;
    }

    /* Phase 3D: also emit cold (not-yet-resident) entries so operators can
     * see warmup progress in pg_stat_gpu_search. */
    for (int i = 0; i < g_n_cold_indexes && n < (uint32_t)g_max_indexes; i++)
    {
        ColdIndexEntry *ce = &g_cold_indexes[i];
        if (!ce->valid)
            continue;
        if (ce->db_oid != cmd->db_oid)
            continue;
        if (cmd->index_oid != 0 && ce->index_oid != cmd->index_oid)
            continue;

        CuvsIndexStats *s = &stats[n++];
        memset(s, 0, sizeof(*s));
        s->db_oid             = ce->db_oid;
        s->index_oid          = ce->index_oid;
        s->resident           = 0;
        s->warmup_state       = (uint32_t)ce->warmup_state;
        s->last_warmup_at     = (int64_t)ce->last_warmup_at;
        s->warmup_duration_ms = ce->warmup_duration_ms;
        s->download_count     = (uint32_t)ce->download_count;
        s->cache_miss_count   = ce->cache_miss_count;
        s->gpu_device_id      = 0xFFFFFFFF;
        s->shard_count        = 0;   /* cold entries are reported unsharded */
    }

    pthread_mutex_unlock(&g_index_mutex);

    CuvsReplyHeader hdr = {0};
    hdr.status    = CUVS_STATUS_OK;
    hdr.n_results = n;
    send_all(client_fd, &hdr, sizeof(hdr));
    if (n > 0)
        send_all(client_fd, stats, (size_t)n * sizeof(CuvsIndexStats));
    free(stats);
}

/* ----------------------------------------------------------------
 * Handle MARK_STALE command (CUVS_OP_MARK_STALE)
 *
 * A backend write hook (aminsert/ambulkdelete) flags an index stale after a
 * heap write. We set the in-memory flag AND touch a .stale sidecar so the
 * staleness survives a daemon restart. Idempotent.
 * ---------------------------------------------------------------- */
static void
handle_mark_stale(int client_fd, const CuvsCmdFrame *cmd)
{
    char stale_path[512];
    time_t now = time(NULL);

    pthread_mutex_lock(&g_index_mutex);

    IndexEntry *e = find_index(cmd->db_oid, cmd->index_oid);
    if (e && !e->stale)
    {
        e->stale = 1;
        e->stale_since = now;
    }

    /* Persist the marker even if the index is not currently resident, so a
     * later load_index picks it up. */
    stale_file_path(stale_path, sizeof(stale_path), g_index_dir,
                    cmd->db_oid, cmd->index_oid);
    int fd = open(stale_path, O_WRONLY | O_CREAT, 0600);
    if (fd >= 0)
        close(fd);

    pthread_mutex_unlock(&g_index_mutex);

    CuvsReplyHeader hdr = {0};
    hdr.status = CUVS_STATUS_OK;
    send_all(client_fd, &hdr, sizeof(hdr));
}

/* ----------------------------------------------------------------
 * Handle DROP_INDEX command (CUVS_OP_DROP_INDEX, Phase 3G.1)
 *
 * Fired by the backend's object_access_hook when a DROP INDEX commits. Free the
 * whole logical index from VRAM (sharded or not), remove it from the resident +
 * cold registries, and unlink ALL of its on-disk artifacts so a daemon restart
 * does not reload a dropped index as a zombie. DROP holds AccessExclusiveLock on
 * the index, so no search can be in-flight on it — safe to free under the mutex
 * (this is why sharded indexes need no inflight guard here; see ADR-022/023).
 * Idempotent: a missing index or missing files are not errors.
 * ---------------------------------------------------------------- */
static void
handle_drop(int client_fd, const CuvsCmdFrame *cmd)
{
    uint32_t db  = cmd->db_oid;
    uint32_t idx = cmd->index_oid;
    char     p[512];

    pthread_mutex_lock(&g_index_mutex);

    /* Free the resident entry (if any) and compact the registry. */
    IndexEntry *e = find_index(db, idx);
    if (e)
    {
        free_delta_cache(e);
        /* ADR-073: free the resident main BF cache — flat's ONLY GPU allocation
         * (handle is NULL), and a cagra index's secondary BF cache. NULL-safe.
         * (Note: e->ivfpq_handle is still not freed here — a pre-existing IVF-PQ
         * VRAM-on-drop leak, out of scope for ADR-073.) */
        free_main_bf_cache(e);
        if (e->shard_count >= 2) free_index_shards(e);
        else if (e->handle)      cuvs_cagra_free(e->handle, e->gpu_device_id);
        free(e->tids);
        free(e->rev_tids);     e->rev_tids     = NULL;
        free(e->rev_item_ids); e->rev_item_ids = NULL;

        int slot = (int)(e - g_indexes);
        for (int i = slot; i < g_n_indexes - 1; i++)
            g_indexes[i] = g_indexes[i + 1];
        g_n_indexes--;
        g_cache_drops++;   /* EV-7: DROP cleanup is not a policy eviction */
    }
    evict_history_drop(db, idx);   /* C8: history goes with the index */
    cuvs_admit_log_forget(&g_admit_log, db, idx);  /* its rejects stop driving F */

    /* Drop a cold-registry entry too (Phase 3D), if present. */
    for (int i = 0; i < g_n_cold_indexes; i++)
        if (g_cold_indexes[i].valid &&
            g_cold_indexes[i].db_oid == db && g_cold_indexes[i].index_oid == idx)
        {
            for (int j = i; j < g_n_cold_indexes - 1; j++)
                g_cold_indexes[j] = g_cold_indexes[j + 1];
            g_n_cold_indexes--;
            break;
        }

    /* Unlink every per-index artifact so a restart won't reload a zombie. */
    index_file_path(p, sizeof(p), g_index_dir, db, idx);       unlink(p); /* .cagra */
    tids_file_path(p, sizeof(p), g_index_dir, db, idx);        unlink(p); /* .tids */
    shards_manifest_path(p, sizeof(p), g_index_dir, db, idx);  unlink(p); /* .shards */
    delta_file_path(p, sizeof(p), g_index_dir, db, idx);       unlink(p); /* .delta */
    vectors_file_path(p, sizeof(p), g_index_dir, db, idx);     unlink(p); /* .vectors */
    stale_file_path(p, sizeof(p), g_index_dir, db, idx);       unlink(p); /* .stale */
    relfilenode_file_path(p, sizeof(p), g_index_dir, db, idx); unlink(p); /* .relfilenode */
    snprintf(p, sizeof(p), "%s/%u_%u.tombstone", g_index_dir, db, idx); unlink(p);
    /* Shard artifacts have contiguous ids; stop at the first missing one. */
    for (uint32_t s = 0; s < CUVS_SHARDS_MAX; s++)
    {
        shard_cagra_path(p, sizeof(p), g_index_dir, db, idx, s);
        if (unlink(p) != 0)
            break;
    }

    LOG_INFO("pg_cuvs_server: dropped index %u/%u (freed + artifacts unlinked)\n", db, idx);

    pthread_mutex_unlock(&g_index_mutex);

    CuvsReplyHeader hdr = {0};
    hdr.status = CUVS_STATUS_OK;
    send_all(client_fd, &hdr, sizeof(hdr));
}

/* ----------------------------------------------------------------
 * Handle EXPORT_ADJACENCY command (CUVS_OP_EXPORT_ADJACENCY, Phase 3J).
 *
 * Copies the CAGRA graph adjacency list + corpus vectors from GPU VRAM
 * to a shared memory segment, then replies with the key so the backend
 * can read the data and write pgvector HNSW pages directly (no .hnsw file).
 *
 * Reply header reuse:
 *   n_results  = n_vecs
 *   latency_us = graph_degree
 *   delta_merged = dim
 *   error[]    = shm_key (null-terminated)
 * ---------------------------------------------------------------- */
static void
handle_export_adjacency(int client_fd, const CuvsCmdFrame *cmd)
{
    uint32_t db  = cmd->db_oid;
    uint32_t idx = cmd->index_oid;

    pthread_mutex_lock(&g_index_mutex);
    IndexEntry *e = find_index(db, idx);
    if (!e || !e->valid || !e->handle || e->shard_count >= 2)
    {
        pthread_mutex_unlock(&g_index_mutex);
        send_error_code(client_fd, CUVS_STATUS_NOT_FOUND,
                        "index not loaded or sharded (call after CREATE INDEX USING cagra)");
        return;
    }

    /* Hold the mutex while extracting from VRAM to prevent eviction. */
    uint32_t *adj  = NULL;
    float    *vecs = NULL;
    size_t    n_vecs;
    int       graph_degree;
    int       gpu  = (int)e->gpu_device_id;

    if (cuvs_cagra_extract_adjacency(e->handle, &adj, &vecs, &n_vecs,
                                      &graph_degree, gpu) != 0)
    {
        pthread_mutex_unlock(&g_index_mutex);
        free(adj); free(vecs);
        send_error(client_fd, "cuvs_cagra_extract_adjacency failed");
        return;
    }

    uint32_t dim     = e->dim;
    int64_t   n_vecs_i = e->n_vecs;
    /* I3-B: copy the TIDs under the lock; the registry's array can be freed
     * by an eviction (e.g. the reclaim thread) once the lock is released. */
    uint64_t *tids   = malloc((size_t)n_vecs_i * sizeof(uint64_t));
    if (!tids)
    {
        pthread_mutex_unlock(&g_index_mutex);
        free(adj); free(vecs);
        send_error(client_fd, "out of memory for export");
        return;
    }
    memcpy(tids, e->tids, (size_t)n_vecs_i * sizeof(uint64_t));
    pthread_mutex_unlock(&g_index_mutex);

    /* Pack into shared memory:
     *   [uint32_t n_vecs][uint32_t graph_degree][uint32_t dim][uint32_t pad]
     *   [uint32_t adj[n_vecs * graph_degree]]
     *   [float    vecs[n_vecs * dim]]
     *   [uint64_t tids[n_vecs]]
     */
    size_t adj_bytes  = (size_t)n_vecs * (size_t)graph_degree * sizeof(uint32_t);
    size_t vecs_bytes = (size_t)n_vecs * (size_t)dim           * sizeof(float);
    size_t tids_bytes = (size_t)n_vecs * sizeof(uint64_t);
    size_t hdr_bytes  = 4 * sizeof(uint32_t);
    size_t total      = hdr_bytes + adj_bytes + vecs_bytes + tids_bytes;

    /* Generate a unique, unguessable shm key for this reply (#87). */
    static int adj_seq = 0;
    char shm_key[64];
    {
        unsigned char rnd[8];
        char hex[17];
        if (shm_key_random(rnd, sizeof(rnd)) != 0)   /* #87 finding 2: fail closed */
        {
            free(adj); free(vecs); free(tids);
            send_error_code(client_fd, CUVS_STATUS_ERROR, "csprng failed");
            return;
        }
        for (size_t i = 0; i < sizeof(rnd); i++)
            snprintf(hex + i * 2, 3, "%02x", rnd[i]);
        snprintf(shm_key, sizeof(shm_key), "/pg_cuvs_adj_%d_%d_%s",
                 (int)getpid(), __atomic_fetch_add(&adj_seq, 1, __ATOMIC_RELAXED), hex);
    }

    /*
     * #165: the segment is handed over as an SCM_RIGHTS fd, and its name is
     * unlinked before the reply is sent — so no /dev/shm entry outlives this
     * call. The previous design left the name for the backend to shm_unlink(),
     * which cannot work: /dev/shm is sticky (drwxrwxrwt) and the daemon (its
     * owner) and the PG backend run as different uids, so that unlink failed
     * with EPERM and the segments accumulated until tmpfs was exhausted and
     * the next mmap write took down the cluster with SIGBUS.
     *
     * Two descriptors: RDWR to fill the segment here, RDONLY to hand out, so
     * the consumer keeps the read-only access it had under the named scheme.
     */
    int shm_fd = shm_open(shm_key, O_CREAT | O_EXCL | O_RDWR, 0644);
    if (shm_fd < 0)
    {
        free(adj); free(vecs); free(tids);
        send_error(client_fd, "shm_open failed for export");
        return;
    }
    if (ftruncate(shm_fd, (off_t)total) != 0)
    {
        close(shm_fd); shm_unlink(shm_key); free(adj); free(vecs); free(tids);
        send_error(client_fd, "ftruncate failed for export");
        return;
    }
    int pass_fd = shm_open(shm_key, O_RDONLY, 0);
    if (pass_fd < 0)
    {
        close(shm_fd); shm_unlink(shm_key); free(adj); free(vecs); free(tids);
        send_error(client_fd, "shm_open(RDONLY) failed for export");
        return;
    }
    /* Name is no longer needed by anyone: both fds stay valid without it.
     * #166 review 3: if the unlink fails the name survives as daemon-owned
     * residue that nothing else reclaims — exactly the condition #165 exists
     * to make observable, so it must not fail silently. */
    if (shm_unlink(shm_key) != 0)
        LOG_WARN("export_adjacency: shm_unlink(%s) failed errno=%d — "
                 "segment name leaked\n", shm_key, errno);
    void *mem = mmap(NULL, total, PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    if (mem == MAP_FAILED)
    {
        close(pass_fd); free(adj); free(vecs); free(tids);
        send_error(client_fd, "mmap failed for export");
        return;
    }

    /* Write header: [n_vecs][graph_degree][dim][metric] */
    uint32_t *hp = (uint32_t *)mem;
    hp[0] = (uint32_t)n_vecs;
    hp[1] = (uint32_t)graph_degree;
    hp[2] = (uint32_t)dim;
    hp[3] = e->metric;  /* CUVS_METRIC_* for source metric validation */

    char *dp = (char *)mem + hdr_bytes;
    memcpy(dp, adj,  adj_bytes);   dp += adj_bytes;
    memcpy(dp, vecs, vecs_bytes);  dp += vecs_bytes;
    memcpy(dp, tids, tids_bytes);

    munmap(mem, total);
    free(adj);
    free(vecs);
    free(tids);

    LOG_INFO("[handle_export_adjacency] %u/%u N=%zu D=%d dim=%u fd-passed (was %s)\n",
             db, idx, n_vecs, graph_degree, dim, shm_key);

    /* Reply: n_vecs/graph_degree/dim in header fields; the segment itself rides
     * along as an SCM_RIGHTS fd (#165). error[] carries the former name for log
     * correlation only — the backend must not open by name (it is unlinked). */
    CuvsReplyHeader reply = {0};
    reply.status       = CUVS_STATUS_OK;
    reply.n_results    = (uint32_t)n_vecs;
    reply.latency_us   = (uint32_t)graph_degree;
    reply.delta_merged = (uint32_t)dim;
    strncpy(reply.error, shm_key, sizeof(reply.error) - 1);
    if (cuvs_fd_send(client_fd, pass_fd, &reply, sizeof(reply)) != 0)
        LOG_ERROR("[handle_export_adjacency] fd handoff failed for %u/%u\n", db, idx);
    close(pass_fd);
    (void)n_vecs_i;
}

/* ----------------------------------------------------------------
 * Handle EXPORT_HNSW_SHM command (CUVS_OP_EXPORT_HNSW_SHM, Phase 3J).
 *
 * Runs from_cagra() on the loaded CAGRA index and writes the resulting
 * multi-level HNSW to /dev/shm/ (RAM-backed, no disk I/O).  Returns the
 * /dev/shm path in reply.error[] so the backend can open and parse it,
 * then unlink after reading.
 * ---------------------------------------------------------------- */
static void
handle_export_hnsw_shm(int client_fd, const CuvsCmdFrame *cmd)
{
    uint32_t db  = cmd->db_oid;
    uint32_t idx = cmd->index_oid;

    pthread_mutex_lock(&g_index_mutex);
    IndexEntry *e = find_index(db, idx);
    if (!e || !e->valid || !e->handle || e->shard_count >= 2)
    {
        pthread_mutex_unlock(&g_index_mutex);
        send_error_code(client_fd, CUVS_STATUS_NOT_FOUND,
                        "index not loaded or sharded");
        return;
    }
    CuvsCagraIndex handle = e->handle;
    int            gpu    = (int)e->gpu_device_id;
    pthread_mutex_unlock(&g_index_mutex);

    /*
     * #166 review 3: the sidecar is an anonymous memfd, never a name in
     * /dev/shm.
     *
     * The previous scheme ("/dev/shm/pg_cuvs_hnsw_<pid>_<seq>") was fully
     * predictable, and /dev/shm is world-writable (sticky only stops *deletion*
     * of another uid's entry, not creation of a new name). The serializers open
     * that path themselves and follow symlinks with no O_EXCL — the CPU shim
     * through fopen(path, "wb") (cuvs_wrapper_shim_cpu.c), the GPU wrapper
     * through cuVS's own stream — so a local attacker could pre-create the name
     * and have the daemon write through it. Unlinking afterwards also could not
     * close the crash window: a daemon killed mid-serialize left the inode
     * behind, contradicting #165's "no residue" contract.
     *
     * memfd has no name to squat and no residue to reclaim — the inode dies
     * with the last descriptor, including on SIGKILL. The serializers still
     * take a path, so they get /proc/self/fd/N. That is safe *here* and unsafe
     * on the consumer side (which review 2 fixed) for the same reason: opening
     * the procfs link re-checks inode permissions, which succeeds only for the
     * process and uid that owns the file — which is exactly this one.
     *
     * No fallback to the old path scheme: it is the vulnerability. memfd_create
     * is Linux 3.17+ and already required by the ADR-057 build corpus.
     */
    int mfd = memfd_create("pg_cuvs_hnsw", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (mfd < 0)
    {
        LOG_ERROR("[handle_export_hnsw_shm] memfd_create failed errno=%d\n", errno);
        send_error(client_fd, "memfd_create failed for hnsw sidecar");
        return;
    }

    char shm_path[64];
    snprintf(shm_path, sizeof(shm_path), "/proc/self/fd/%d", mfd);

    /* Serialize HNSW into the memfd (reuses existing cuvs_hnsw_serialize).
     * I3-B EV-1 (b): the entry is in use while the handle is read without
     * the lock, so no eviction path (notably the reclaim thread) frees it;
     * re-check it is still the same resident handle first. */
    pthread_mutex_lock(&g_index_mutex);
    e = find_index(db, idx);
    if (!e || e->handle != handle)
    {
        pthread_mutex_unlock(&g_index_mutex);
        close(mfd);
        send_error_code(client_fd, CUVS_STATUS_NOT_FOUND, "index evicted during export");
        return;
    }
    e->inflight++;
    g_unlocked_gpu_jobs++;
    pthread_mutex_unlock(&g_index_mutex);
    int hrc = cuvs_hnsw_serialize(handle, shm_path, gpu);
    pthread_mutex_lock(&g_index_mutex);
    g_unlocked_gpu_jobs--;
    e = find_index(db, idx);
    if (e && e->inflight > 0)
        e->inflight--;
    pthread_mutex_unlock(&g_index_mutex);
    if (hrc != CUVS_HNSW_SERIALIZE_OK)
    {
        /*
         * A failed serialize may have written into the memfd — the CPU shim
         * opens before it can fail on write or close, and the GPU wrapper
         * catches serialization exceptions after the stream exists. There is
         * nothing to unlink: closing the descriptor frees the inode.
         */
        close(mfd);
        if (hrc == CUVS_HNSW_SERIALIZE_SKIPPED)
            send_error_code(client_fd, CUVS_STATUS_NOT_FOUND,
                            "index too small for an HNSW sidecar");
        else
            send_error(client_fd, "from_cagra/serialize failed");
        return;
    }

    /*
     * Seal the sidecar read-only before it leaves this process.
     *
     * memfd_create returns an O_RDWR description, and SCM_RIGHTS hands the
     * receiver that same description — so without this the PG backend (a
     * different uid) would gain write access the previous scheme never gave
     * it: the old code passed a descriptor from open(path, O_RDONLY). Sealing
     * is stronger than restoring O_RDONLY, because it makes the bytes
     * immutable for *every* holder rather than for one descriptor, so neither
     * side can mutate the buffer between the importer's two read passes.
     *
     * F_SEAL_WRITE can only fail with EBUSY if a writable mapping exists; the
     * serializers write through stdio/ofstream, never mmap. Treat a failure as
     * fatal rather than shipping a writable descriptor.
     */
    if (fcntl(mfd, F_ADD_SEALS,
              F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0)
    {
        LOG_ERROR("[handle_export_hnsw_shm] F_ADD_SEALS failed errno=%d\n", errno);
        close(mfd);
        send_error(client_fd, "cannot seal hnsw sidecar");
        return;
    }

    LOG_INFO("[handle_export_hnsw_shm] %u/%u → memfd (sealed, fd-passed)\n", db, idx);

    /*
     * #165: hand the serialized sidecar over as an SCM_RIGHTS fd. There is no
     * path to unlink and no reopen to fail — the memfd created above *is* what
     * gets passed. The serializer wrote through its own file description, so
     * this descriptor's offset is untouched; the consumer dup()s it and
     * rewind()s the dup anyway (hnsw_open_stream).
     */
    CuvsReplyHeader reply = {0};
    reply.status = CUVS_STATUS_OK;
    /* error[] carries a label for log correlation only (#165); there is no
     * longer a filesystem path to report. */
    snprintf(reply.error, sizeof(reply.error), "memfd:pg_cuvs_hnsw %u/%u", db, idx);
    if (cuvs_fd_send(client_fd, mfd, &reply, sizeof(reply)) != 0)
        LOG_ERROR("[handle_export_hnsw_shm] fd handoff failed for %u/%u\n", db, idx);
    close(mfd);
}

/* ----------------------------------------------------------------
 * Handle CACHE_STATS command (CUVS_OP_CACHE_STATS): per-GPU counters.
 * Phase 3E: one CuvsCacheStats row per usable GPU device.
 * ---------------------------------------------------------------- */
static void
handle_cache_stats(int client_fd)
{
    CuvsCacheStats rows[CUVS_MAX_GPUS];
    int n_rows = 0;

    pthread_mutex_lock(&g_index_mutex);
    int n = n_usable_gpus();
    for (int gi = 0; gi < n && n_rows < CUVS_MAX_GPUS; gi++)
    {
        int dev = usable_gpu(gi);
        CuvsCacheStats *cs = &rows[n_rows++];
        memset(cs, 0, sizeof(*cs));
        cs->gpu_device_id    = (uint32_t)dev;
        cs->hits             = g_cache_hits[dev];
        cs->misses           = g_cache_misses[dev];
        cs->evictions        = g_cache_evictions[dev];
        cs->reloads          = g_cache_reloads[dev];
        cs->persist_failures = g_cache_persist_fail[dev];
        cs->resident_count   = 0;
        for (int j = 0; j < g_n_indexes; j++)
        {
            IndexEntry *ej = &g_indexes[j];
            if (!ej->valid)
                continue;
            if (ej->shard_count >= 2)
            {
                /* Count each shard resident on this device. */
                for (int s = 0; s < ej->shard_count; s++)
                    if (ej->shards[s].valid &&
                        ej->shards[s].gpu_device_id == (uint32_t)dev)
                    {
                        cs->resident_count++;
                        /* Phase 3L: per-shard resident brute-force index VRAM. */
                        if (ej->shards[s].bf_idx)
                        {
                            cs->bf_vram_bytes += ej->shards[s].bf_vram_bytes;
                            cs->bf_precision = ej->bf_precision;
                        }
                    }
            }
            else if (ej->gpu_device_id == (uint32_t)dev)
            {
                cs->resident_count++;
                /* Phase 3L: unsharded resident brute-force index VRAM. */
                if (ej->main_bf_idx)
                {
                    cs->bf_vram_bytes += ej->main_bf_vram_bytes;
                    cs->bf_precision = ej->bf_precision;
                }
            }
        }
        cs->vram_used_bytes  = total_vram_used(dev);
        cs->vram_budget_bytes = g_max_vram_per_gpu[dev];
    }
    pthread_mutex_unlock(&g_index_mutex);

    CuvsReplyHeader hdr = {0};
    hdr.status    = CUVS_STATUS_OK;
    hdr.n_results = (uint32_t)n_rows;
    send_all(client_fd, &hdr, sizeof(hdr));
    if (n_rows > 0)
        send_all(client_fd, rows, (size_t)n_rows * sizeof(CuvsCacheStats));
}

/* ----------------------------------------------------------------
 * Handle SHARD_STATS command (CUVS_OP_SHARD_STATS): per-shard rows for every
 * resident sharded index in the requesting database (Phase 3F).
 * ---------------------------------------------------------------- */
static void
handle_shard_stats(int client_fd, const CuvsCmdFrame *cmd)
{
    pthread_mutex_lock(&g_index_mutex);

    /* Count shard rows first so we can size the reply buffer. */
    int total = 0;
    for (int i = 0; i < g_n_indexes; i++)
    {
        IndexEntry *e = &g_indexes[i];
        if (!e->valid || e->shard_count < 2)
            continue;
        if (e->db_oid != cmd->db_oid)
            continue;
        if (cmd->index_oid != 0 && e->index_oid != cmd->index_oid)
            continue;
        total += e->shard_count;
    }

    CuvsShardStats *rows = (total > 0) ? malloc((size_t)total * sizeof(CuvsShardStats))
                                       : NULL;
    int n = 0;
    if (rows)
    {
        for (int i = 0; i < g_n_indexes; i++)
        {
            IndexEntry *e = &g_indexes[i];
            if (!e->valid || e->shard_count < 2)
                continue;
            if (e->db_oid != cmd->db_oid)
                continue;
            if (cmd->index_oid != 0 && e->index_oid != cmd->index_oid)
                continue;
            for (int s = 0; s < e->shard_count; s++)
            {
                ShardEntry *se = &e->shards[s];
                CuvsShardStats *r = &rows[n++];
                r->db_oid        = e->db_oid;
                r->index_oid     = e->index_oid;
                r->shard_id      = se->shard_id;
                r->gpu_device_id = se->gpu_device_id;
                r->n_vecs        = se->n_vecs;
                r->tid_offset    = se->tid_offset;
                r->vram_bytes    = se->vram_bytes;
                r->search_count  = se->search_count;
                r->error_count   = se->error_count;
                r->resident      = (uint32_t)(se->valid ? 1 : 0);
                r->last_status   = se->last_status;
            }
        }
    }

    pthread_mutex_unlock(&g_index_mutex);

    CuvsReplyHeader hdr = {0};
    hdr.status    = CUVS_STATUS_OK;
    hdr.n_results = (uint32_t)n;
    send_all(client_fd, &hdr, sizeof(hdr));
    if (n > 0)
        send_all(client_fd, rows, (size_t)n * sizeof(CuvsShardStats));
    free(rows);
}

/* ----------------------------------------------------------------
 * ADR-073: handle_build_flat — build a standalone flat (vectors-only) index.
 *
 * Persists .tids + .vectors ONLY (no .cagra graph) and registers a brute-force-
 * only entry (handle == NULL, is_flat = 1). It does NOT call finish_build_commit
 * (which serializes a CAGRA handle). The corpus handoff mirrors handle_build
 * (memfd via SCM_RIGHTS, or named shm) because the backend builds the flat corpus
 * with the same cuvs_corpus tiers. The resident GPU BF over .vectors is built
 * lazily on the first search (refresh_main_bf_cache). n_vecs >= 1 is allowed
 * (brute-force needs no neighborhood graph), reconciled with the load validator
 * (cuvs_tids_read requires n > 0).
 * ---------------------------------------------------------------- */
static void
handle_build_flat(int client_fd, const CuvsCmdFrame *cmd)
{
    char index_dir[256] = {0};
    int  passed_fd = -1;
    if (cuvs_fd_recv(client_fd, index_dir, sizeof(index_dir), &passed_fd) < 0)
    {
        send_error(client_fd, "recv index_dir failed");
        return;
    }

    if (cmd->n_vecs < 1 || cmd->dim == 0)
    {
        if (passed_fd >= 0) close(passed_fd);
        send_error(client_fd, "flat build needs at least 1 vector");
        return;
    }
    {
        size_t per_vec = (size_t)cmd->dim * sizeof(float) + sizeof(uint64_t);
        if ((size_t)cmd->n_vecs > SIZE_MAX / per_vec)
        {
            if (passed_fd >= 0) close(passed_fd);
            send_error(client_fd, "build payload size overflow");
            return;
        }
    }

    size_t vec_bytes = (size_t)cmd->n_vecs * cmd->dim * sizeof(float);
    size_t tid_bytes = (size_t)cmd->n_vecs * sizeof(uint64_t);
    size_t total     = vec_bytes + tid_bytes;

    /* Corpus handoff (memfd tier via SCM_RIGHTS, else named shm) — same as
     * handle_build. */
    void *mem;
    if (passed_fd >= 0)
    {
        LOG_INFO("[handle_build_flat] corpus via memfd(SCM_RIGHTS) fd=%d total=%zu\n",
                 passed_fd, total);
        mem = mmap(NULL, total, PROT_READ, MAP_SHARED, passed_fd, 0);
        close(passed_fd);
    }
    else
    {
        LOG_INFO("[handle_build_flat] corpus via shm_open(%s)\n", cmd->shm_key);
        int shm_fd = shm_open(cmd->shm_key, O_RDONLY, 0);
        if (shm_fd < 0)
        {
            send_error(client_fd, "shm_open failed");
            return;
        }
        if (!shm_check_peer_owner(shm_fd, g_peer_uid))   /* #87 finding 1 */
        {
            close(shm_fd);
            send_error(client_fd, "shm owner mismatch");
            return;
        }
        mem = mmap(NULL, total, PROT_READ, MAP_SHARED, shm_fd, 0);
        close(shm_fd);
    }
    if (mem == MAP_FAILED)
    {
        send_error(client_fd, "mmap failed");
        return;
    }

    const float    *vecs    = (const float *)mem;
    const uint64_t *tids_in = (const uint64_t *)((const char *)mem + vec_bytes);

    /* ADR-073 VRAM no-fit gate: the resident BF over .vectors needs N*dim*bytes of
     * VRAM at search time (non-evicting). If the corpus cannot fit ANY GPU's
     * configured budget even as float16, fail the build with a DISTINCT, actionable
     * message — far clearer than letting every later search fail with the generic
     * "sidecar missing or stale". A float32-only overflow is a non-fatal advisory.
     * Budget-only check (config, not live free VRAM); dynamic shortfalls still
     * surface at search time via refresh_main_bf_cache. */
    {
        size_t need_f32 = vec_bytes;
        size_t need_f16 = vec_bytes / 2;
        size_t max_budget = 0;
        int    unlimited = 0;
        int    ng = n_usable_gpus();
        for (int i = 0; i < ng; i++)
        {
            size_t b = g_max_vram_per_gpu[usable_gpu(i)];
            if (b == 0) { unlimited = 1; break; }   /* an unlimited GPU can hold it */
            if (b > max_budget) max_budget = b;
        }
        if (!unlimited && max_budget > 0 && need_f16 > max_budget)
        {
            munmap(mem, total);
            CuvsReplyHeader hdr = {0};
            hdr.status = CUVS_STATUS_OOM_FALLBACK;
            snprintf(hdr.error, sizeof(hdr.error),
                     "flat corpus %zu MB (float16 %zu MB) exceeds the GPU VRAM budget "
                     "(%zu MB); shard the table or use a cagra/ivfpq index",
                     need_f32 / (1024*1024), need_f16 / (1024*1024),
                     max_budget / (1024*1024));
            LOG_WARN("[handle_build_flat] %u/%u %s\n", cmd->db_oid, cmd->index_oid, hdr.error);
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }
        if (!unlimited && max_budget > 0 && need_f32 > max_budget)
            LOG_WARN("[handle_build_flat] %u/%u corpus %zu MB exceeds VRAM budget as "
                     "float32; use WITH (precision='float16') for brute-force search\n",
                     cmd->db_oid, cmd->index_oid, need_f32 / (1024*1024));
    }

    /* --- Disk persistence: .tids + .vectors (tmp + rename). Our own commit,
     * NOT finish_build_commit (which serializes a .cagra graph). --- */
    const char *save_dir = (index_dir[0] != '\0') ? index_dir : g_index_dir;
    mkdir(save_dir, 0700);

    uint64_t *new_tids = malloc(tid_bytes);
    if (!new_tids)
    {
        munmap(mem, total);
        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED, "malloc tids failed");
        return;
    }
    memcpy(new_tids, tids_in, tid_bytes);

    /* Generation token: this build's .tids body CRC, embedded in .vectors so the
     * loader rejects a torn (stale) sidecar after a crash. */
    uint32_t tids_gen = cuvs_crc32(new_tids, (size_t)cmd->n_vecs * sizeof(uint64_t));

    char tids_final[512], tids_tmp[576];
    char vecs_final[512], vecs_tmp[576];
    tids_file_path(tids_final, sizeof(tids_final), save_dir, cmd->db_oid, cmd->index_oid);
    vectors_file_path(vecs_final, sizeof(vecs_final), save_dir, cmd->db_oid, cmd->index_oid);
    snprintf(tids_tmp, sizeof(tids_tmp), "%s.tmp", tids_final);
    snprintf(vecs_tmp, sizeof(vecs_tmp), "%s.tmp", vecs_final);

    /* .vectors (float32 body + generation token) while the corpus is still mapped. */
    {
        FILE *vf = fopen(vecs_tmp, "wb");
        int vok = (vf != NULL);
        if (vok) fchmod_sidecar(vf, vecs_tmp);
        if (vok && cuvs_vectors_write(vf, cmd->n_vecs, cmd->dim, cmd->metric, tids_gen, vecs) != 0)
            vok = 0;
        if (vok && fflush(vf) != 0) vok = 0;
        if (vok && fsync(fileno(vf)) != 0) vok = 0;
        if (vf && fclose(vf) != 0) vok = 0;
        if (!vok)
        {
            unlink(vecs_tmp);
            free(new_tids);
            munmap(mem, total);
            send_error_code(client_fd, CUVS_STATUS_PERSIST_FAILED, "flat .vectors persist failed");
            return;
        }
    }
    munmap(mem, total);

    if (write_tids_atomic(tids_tmp, cmd->n_vecs, cmd->dim, cmd->metric, new_tids) != 0)
    {
        unlink(vecs_tmp);
        free(new_tids);
        send_error_code(client_fd, CUVS_STATUS_PERSIST_FAILED, "flat .tids persist failed");
        return;
    }

    /* Commit: rename .vectors then .tids. On a failure, unlink both so a partial
     * (e.g. .vectors but no .tids) never registers — load_index needs .tids. */
    if (rename(vecs_tmp, vecs_final) != 0)
    {
        unlink(vecs_tmp); unlink(tids_tmp);
        free(new_tids);
        send_error_code(client_fd, CUVS_STATUS_PERSIST_FAILED, "flat .vectors rename failed");
        return;
    }
    if (rename(tids_tmp, tids_final) != 0)
    {
        unlink(tids_tmp); unlink(vecs_final);
        free(new_tids);
        send_error_code(client_fd, CUVS_STATUS_PERSIST_FAILED, "flat .tids rename failed");
        return;
    }
    {
        int dir_fd = open(save_dir, O_RDONLY);
        if (dir_fd >= 0) { fsync(dir_fd); close(dir_fd); }
    }

    /* Remove any stale CAGRA / IVF-PQ / .shards / .stale artifacts for this OID so
     * load_index unambiguously takes the flat branch. */
    {
        char p[512];
        index_file_path(p, sizeof(p), save_dir, cmd->db_oid, cmd->index_oid);   unlink(p);
        ivfpq_file_path(p, sizeof(p), save_dir, cmd->db_oid, cmd->index_oid);   unlink(p);
        stale_file_path(p, sizeof(p), save_dir, cmd->db_oid, cmd->index_oid);   unlink(p);
        shards_manifest_path(p, sizeof(p), save_dir, cmd->db_oid, cmd->index_oid); unlink(p);
    }

    /* Register (or replace) in the hot registry — handle == NULL, is_flat = 1. */
    pthread_mutex_lock(&g_index_mutex);
    {
        int fgpu = pick_gpu_for_index(0);
        if (fgpu < 0) fgpu = usable_gpu(0);
        IndexEntry *existing = find_index(cmd->db_oid, cmd->index_oid);
        if (existing)
        {
            if (existing->handle)       cuvs_cagra_free(existing->handle, existing->gpu_device_id);
            if (existing->ivfpq_handle) cuvs_ivfpq_free(existing->ivfpq_handle, existing->gpu_device_id);
            free(existing->tids);
            free_delta_cache(existing);
            free_main_bf_cache(existing);
            if (existing->hnsw_idx) { cuvs_hnsw_free(existing->hnsw_idx); existing->hnsw_idx = NULL; }
            existing->handle         = NULL;
            existing->ivfpq_handle   = NULL;
            existing->ivfpq_n_vecs   = 0;
            existing->ivfpq_vram_bytes = 0;
            existing->tids           = new_tids;
            existing->dim            = cmd->dim;
            existing->metric         = cmd->metric;
            existing->n_vecs         = cmd->n_vecs;
            existing->vram_bytes     = 0;
            existing->shard_count    = 0;
            existing->shards         = NULL;
            existing->inflight       = 0;
            existing->gpu_device_id  = (uint32_t) fgpu;
            existing->last_search    = time(NULL);
            existing->valid          = 1;
            existing->stale          = 0;
            existing->stale_since    = 0;
            existing->n_extended     = 0;
            existing->compact_count  = 0;
            existing->last_compact_at = 0;
            existing->warmup_state   = WARMUP_HOT;
            existing->last_search_mode = 3; /* gpu_bf */
            free(existing->rev_tids);     existing->rev_tids     = NULL;
            free(existing->rev_item_ids); existing->rev_item_ids = NULL;
            build_rev_tid_map(existing);
            reset_entry_stats(existing);
            existing->is_flat        = 1;   /* MUST be after reset_entry_stats */
        }
        else
        {
            g_cur_requester_idx = cmd->index_oid;
            if (g_n_indexes >= g_max_indexes)
                (void) make_slot(fgpu, CUVS_RQ_BUILD_FLAT);
            if (g_n_indexes >= g_max_indexes)
            {
                /* Soft cap: artifacts durable on disk; reload on demand. */
                LOG_WARN("[handle_build_flat] registry full; %u/%u persisted, reload on demand\n",
                         cmd->db_oid, cmd->index_oid);
                free(new_tids);
                pthread_mutex_unlock(&g_index_mutex);
                CuvsReplyHeader hdr_ok = {0};
                hdr_ok.status = CUVS_STATUS_OK;
                send_all(client_fd, &hdr_ok, sizeof(hdr_ok));
                return;
            }
            IndexEntry *e = registry_new_slot();
            e->db_oid          = cmd->db_oid;
            e->index_oid       = cmd->index_oid;
            e->dim             = cmd->dim;
            e->metric          = cmd->metric;
            e->n_vecs          = cmd->n_vecs;
            e->handle          = NULL;
            e->ivfpq_handle    = NULL;
            e->ivfpq_n_vecs    = 0;
            e->ivfpq_vram_bytes = 0;
            e->tids            = new_tids;
            e->rev_tids        = NULL;
            e->rev_item_ids    = NULL;
            build_rev_tid_map(e);
            e->vram_bytes      = 0;
            e->last_search     = time(NULL);
            e->valid           = 1;
            e->stale           = 0;
            e->stale_since     = 0;
            e->shard_count     = 0;
            e->shards          = NULL;
            e->inflight        = 0;
            e->gpu_device_id   = (uint32_t) fgpu;
            e->delta_idx = NULL; e->delta_tids = NULL; e->n_delta = 0;
            e->delta_generation = 0; e->delta_mtime = 0; e->delta_vram_bytes = 0;
            e->delta_vecs_host = NULL; e->delta_n_cached = 0;
            e->main_bf_idx = NULL; e->main_bf_n = 0; e->main_bf_vram_bytes = 0;
            e->main_bf_mtime = 0; e->main_bf_generation = 0; e->bf_precision = 0;
            e->hnsw_idx        = NULL;
            e->n_extended      = 0;
            e->compact_count   = 0;
            e->last_compact_at = 0;
            e->warmup_state    = WARMUP_HOT;
            e->last_search_mode = 3; /* gpu_bf */
            reset_entry_stats(e);
            e->is_flat         = 1;   /* MUST be after reset_entry_stats */
        }
    }
    pthread_mutex_unlock(&g_index_mutex);

    LOG_INFO("pg_cuvs_server: built flat index %u/%u (%lld vecs, %zu MB on disk)\n",
             cmd->db_oid, cmd->index_oid, (long long)cmd->n_vecs, vec_bytes / (1024*1024));

    CuvsReplyHeader ok = {0};
    ok.status = CUVS_STATUS_OK;
    send_all(client_fd, &ok, sizeof(ok));
}

/* ----------------------------------------------------------------
 * 3P: handle_build_ivfpq — build an IVF-PQ index from the corpus in shm.
 *
 * Follows the same mmap-corpus / write_tids_atomic / serialize / registry
 * pattern as handle_build's unsharded path, but produces .tids + .ivfpq
 * instead of .tids + .cagra. No shard/hnsw/delta support in initial release.
 * ---------------------------------------------------------------- */
static void
handle_build_ivfpq(int client_fd, const CuvsCmdFrame *cmd)
{
    clear_build_notices();
    char index_dir[256] = {0};
    int  passed_fd = -1;
    if (cuvs_fd_recv(client_fd, index_dir, sizeof(index_dir), &passed_fd) < 0)
    {
        send_error(client_fd, "recv index_dir failed");
        return;
    }
    if (passed_fd >= 0) close(passed_fd);   /* IVF-PQ always uses shm tier */

    if (cmd->n_vecs < 1 || cmd->dim == 0)
    {
        send_error(client_fd, "IVF-PQ build needs at least 1 vector");
        return;
    }

    size_t vec_bytes = (size_t)cmd->n_vecs * cmd->dim * sizeof(float);
    size_t tid_bytes = (size_t)cmd->n_vecs * sizeof(uint64_t);
    size_t total     = vec_bytes + tid_bytes;

    int shm_fd = shm_open(cmd->shm_key, O_RDONLY, 0);
    if (shm_fd < 0)
    {
        send_error(client_fd, "shm_open failed");
        return;
    }
    if (!shm_check_peer_owner(shm_fd, g_peer_uid))   /* #87 finding 1 */
    {
        close(shm_fd);
        send_error(client_fd, "shm owner mismatch");
        return;
    }
    void *mem = mmap(NULL, total, PROT_READ, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    if (mem == MAP_FAILED)
    {
        send_error(client_fd, "mmap failed");
        return;
    }

    const float    *vecs    = (const float *)mem;
    const uint64_t *tids_in = (const uint64_t *)((const char *)mem + vec_bytes);

    uint32_t pq_dim  = cmd->pq_dim  ? cmd->pq_dim  : (uint32_t)((cmd->dim + 1) / 2);
    uint32_t pq_bits = cmd->pq_bits ? cmd->pq_bits : 8;
    /* IVF-PQ VRAM ~ n_vecs * pq_dim * pq_bits/8 (PQ codes only, no raw vecs) */
    size_t needed = (size_t)cmd->n_vecs * pq_dim * (pq_bits / 8 + 1);

    pthread_mutex_lock(&g_index_mutex);

    /* I3 (C5, UC-2 (a)): IVF-PQ admission rejections carry err_out; its own
     * estimate is unchanged (C3's N applies to CAGRA). */
    CuvsAdmissionFields rej;
    AdmitLine al = {.op = "BUILD_IVFPQ", .path = "ivfpq", .algo = "ivf_pq", .gpu = -1,
                    .db = cmd->db_oid, .idx = cmd->index_oid, .dim = cmd->dim,
                    .n = cmd->n_vecs, .final = needed, .needed = needed};
    int target_gpu = pick_gpu_for_index(needed);
    if (target_gpu < 0)
    {
        admission_fill(&rej, CUVS_ADMISSION_AR0, needed, -1);
        log_admit(&al, CUVS_ADMISSION_AR0);
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        send_admission_reject(client_fd, CUVS_STATUS_OOM_FALLBACK,
                              "IVF-PQ index too large for any GPU VRAM budget", &rej);
        return;
    }
    g_cur_requester_idx = cmd->index_oid;
    al.gpu = target_gpu;
    al.pending_before = g_pending_rmm_build_vram[target_gpu];
    al.evictions_before = g_cache_evictions[target_gpu];
    int admit_rc = ensure_vram(needed, target_gpu, CUVS_RQ_BUILD_IVFPQ, &rej, &al);
    note_build_admission(cmd->db_oid, cmd->index_oid, needed, target_gpu,
                         admit_rc == 0, 0);
    if (admit_rc < 0)
    {
        char legacy[128];
        log_admit(&al, rej.reason);
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        snprintf(legacy, sizeof(legacy), "VRAM exhausted on GPU %d after eviction",
                 target_gpu);
        send_admission_reject(client_fd, CUVS_STATUS_OOM_FALLBACK, legacy, &rej);
        return;
    }
    log_admit(&al, CUVS_ADMISSION_NONE);

    LOG_DEBUG("[handle_build_ivfpq] building n_vecs=%lld dim=%u n_lists=%u pq_bits=%u pq_dim=%u gpu=%d\n",
              (long long)cmd->n_vecs, cmd->dim, cmd->n_lists, pq_bits, pq_dim, target_gpu);

    log_build_begin(&al);
    al.build_started_us = admission_mono_us();
    CuvsIvfPqIndex new_handle = NULL;
    int build_rc = cuvs_ivfpq_build(vecs, cmd->n_vecs, (int)cmd->dim, cmd->metric,
                                    cmd->n_lists, pq_bits, pq_dim,
                                    target_gpu, &new_handle);
    al.build_finished_us = admission_mono_us();
    log_build_end(&al, NULL, build_rc == 0 && new_handle != NULL,
                  build_rc == 0 && new_handle ? "built" : "failed");
    if (build_rc != 0 || !new_handle)
    {
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED, "cuvs_ivfpq_build failed");
        return;
    }

    uint64_t *new_tids = malloc(tid_bytes);
    if (!new_tids)
    {
        cuvs_ivfpq_free(new_handle, target_gpu);
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED, "malloc tids failed");
        return;
    }
    memcpy(new_tids, tids_in, tid_bytes);
    munmap(mem, total);

    const char *save_dir = (index_dir[0] != '\0') ? index_dir : g_index_dir;
    mkdir(save_dir, 0700);

    char tids_final[512], tids_tmp[576];
    char ivfpq_final[512], ivfpq_tmp[576];
    tids_file_path(tids_final, sizeof(tids_final), save_dir, cmd->db_oid, cmd->index_oid);
    ivfpq_file_path(ivfpq_final, sizeof(ivfpq_final), save_dir, cmd->db_oid, cmd->index_oid);
    snprintf(tids_tmp,  sizeof(tids_tmp),  "%s.tmp", tids_final);
    snprintf(ivfpq_tmp, sizeof(ivfpq_tmp), "%s.tmp", ivfpq_final);

    if (write_tids_atomic(tids_tmp, cmd->n_vecs, cmd->dim, cmd->metric, new_tids) != 0)
        goto persist_fail;

    if (cuvs_ivfpq_serialize(new_handle, ivfpq_tmp, target_gpu) != 0)
    {
        LOG_ERROR("[handle_build_ivfpq] cuvs_ivfpq_serialize FAILED\n");
        goto persist_fail;
    }

    if (rename(tids_tmp, tids_final) != 0)
        goto persist_fail;
    if (rename(ivfpq_tmp, ivfpq_final) != 0)
    {
        unlink(tids_final);
        goto persist_fail;
    }

    {
        int dir_fd = open(save_dir, O_RDONLY);
        if (dir_fd >= 0) { fsync(dir_fd); close(dir_fd); }
    }

    /* Remove any stale CAGRA artifacts for this OID so load_index takes the IVF-PQ path. */
    {
        char cagra_path[512];
        index_file_path(cagra_path, sizeof(cagra_path), save_dir, cmd->db_oid, cmd->index_oid);
        unlink(cagra_path);
        char stale_path[512];
        stale_file_path(stale_path, sizeof(stale_path), save_dir, cmd->db_oid, cmd->index_oid);
        unlink(stale_path);
    }

    /* Register (or replace) in the hot index registry. */
    {
        IndexEntry *existing = find_index(cmd->db_oid, cmd->index_oid);
        if (existing)
        {
            if (existing->handle)       cuvs_cagra_free(existing->handle, existing->gpu_device_id);
            if (existing->ivfpq_handle) cuvs_ivfpq_free(existing->ivfpq_handle, existing->gpu_device_id);
            free(existing->tids);
            free_delta_cache(existing);
            free_main_bf_cache(existing);
            if (existing->hnsw_idx) { cuvs_hnsw_free(existing->hnsw_idx); existing->hnsw_idx = NULL; }
            existing->handle         = NULL;
            existing->ivfpq_handle   = new_handle;
            existing->ivfpq_n_vecs   = cmd->n_vecs;
            existing->ivfpq_vram_bytes = needed;
            existing->tids           = new_tids;
            existing->dim            = cmd->dim;
            existing->metric         = cmd->metric;
            existing->n_vecs         = cmd->n_vecs;
            existing->vram_bytes     = needed;
            existing->gpu_device_id  = (uint32_t)target_gpu;
            existing->last_search    = time(NULL);
            existing->valid          = 1;
            existing->stale          = 0;
            existing->stale_since    = 0;
            existing->last_search_mode = 5; /* ivfpq */
            reset_entry_stats(existing);
            existing->n_extended = 0;
            free(existing->rev_tids);     existing->rev_tids     = NULL;
            free(existing->rev_item_ids); existing->rev_item_ids = NULL;
            build_rev_tid_map(existing);
        }
        else
        {
            g_cur_requester_idx = cmd->index_oid;
            if (g_n_indexes >= g_max_indexes)
                (void) make_slot(target_gpu, CUVS_RQ_BUILD_IVFPQ);
            if (g_n_indexes >= g_max_indexes)
            {
                /* Soft cap: artifacts durable on disk, defer to reload-on-demand
                 * (load_index reloads IVF-PQ). Don't roll back; reply OK. */
                LOG_WARN("[handle_build_ivfpq] registry full; %u/%u persisted, will reload on demand\n",
                         cmd->db_oid, cmd->index_oid);
                cuvs_ivfpq_free(new_handle, target_gpu);
                free(new_tids);
                pthread_mutex_unlock(&g_index_mutex);
                {
                    CuvsReplyHeader hdr_ok = {0};
                    hdr_ok.status = CUVS_STATUS_OK;
                    send_all(client_fd, &hdr_ok, sizeof(hdr_ok));
                }
                return;
            }
            IndexEntry *e = registry_new_slot();
            e->db_oid          = cmd->db_oid;
            e->index_oid       = cmd->index_oid;
            e->dim             = cmd->dim;
            e->metric          = cmd->metric;
            e->n_vecs          = cmd->n_vecs;
            e->handle          = NULL;
            e->ivfpq_handle    = new_handle;
            e->ivfpq_n_vecs    = cmd->n_vecs;
            e->ivfpq_vram_bytes = needed;
            e->tids            = new_tids;
            e->vram_bytes      = needed;
            e->last_search     = time(NULL);
            e->valid           = 1;
            e->stale           = 0;
            e->stale_since     = 0;
            e->shard_count     = 0;
            e->shards          = NULL;
            e->gpu_device_id   = (uint32_t)target_gpu;
            e->delta_idx = NULL; e->delta_tids = NULL; e->n_delta = 0;
            e->delta_generation = 0; e->delta_mtime = 0; e->delta_vram_bytes = 0;
            e->delta_vecs_host = NULL; e->delta_n_cached = 0;
            e->main_bf_idx = NULL; e->main_bf_n = 0; e->main_bf_vram_bytes = 0;
            e->main_bf_mtime = 0; e->main_bf_generation = 0; e->bf_precision = 0;
            e->hnsw_idx        = NULL;
            e->warmup_state    = WARMUP_HOT;
            e->last_search_mode = 5; /* ivfpq */
            reset_entry_stats(e);
            e->n_extended      = 0;   /* I3-B defect 3: reused slot */
            e->compact_count   = 0;
            e->last_compact_at = 0;
            build_rev_tid_map(e);
        }
    }

    pthread_mutex_unlock(&g_index_mutex);

    LOG_INFO("pg_cuvs_server: built IVF-PQ index %u/%u (%lld vecs, %zu MB VRAM)\n",
             cmd->db_oid, cmd->index_oid, (long long)cmd->n_vecs, needed / (1024*1024));

    CuvsReplyHeader ok = {0};
    ok.status = CUVS_STATUS_OK;
    send_all(client_fd, &ok, sizeof(ok));
    return;

persist_fail:
    cuvs_ivfpq_free(new_handle, target_gpu);
    free(new_tids);
    unlink(tids_tmp);
    unlink(ivfpq_tmp);
    pthread_mutex_unlock(&g_index_mutex);
    send_error_code(client_fd, CUVS_STATUS_PERSIST_FAILED, "IVF-PQ disk persist failed");
}

/* ----------------------------------------------------------------
 * 3P: handle_search_ivfpq — search an IVF-PQ index.
 * ---------------------------------------------------------------- */
static void
handle_search_ivfpq(int client_fd, const CuvsCmdFrame *cmd)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_mutex_lock(&g_index_mutex);

    IndexEntry *e = find_index(cmd->db_oid, cmd->index_oid);
    if (!e)
    {
        int load_rc = load_index(cmd->db_oid, cmd->index_oid, CUVS_RQ_LOAD);
        if (load_rc == 0)
            e = find_index(cmd->db_oid, cmd->index_oid);
    }

    if (!e || !e->ivfpq_handle)
    {
        pthread_mutex_unlock(&g_index_mutex);
        CuvsReplyHeader hdr = {0};
        hdr.status = e ? CUVS_STATUS_NOT_FOUND : CUVS_STATUS_NOT_FOUND;
        snprintf(hdr.error, sizeof(hdr.error), "IVF-PQ index %u/%u not found",
                 cmd->db_oid, cmd->index_oid);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    if (cmd->metric != e->metric)
    {
        pthread_mutex_unlock(&g_index_mutex);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_METRIC_MISMATCH;
        snprintf(hdr.error, sizeof(hdr.error),
                 "index built with metric %u but queried with metric %u; REINDEX required",
                 e->metric, cmd->metric);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    size_t vec_bytes = (size_t)cmd->dim * sizeof(float);
    int shm_fd = shm_open(cmd->shm_key, O_RDONLY, 0);
    if (shm_fd < 0)
    {
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "shm_open failed");
        return;
    }
    if (!shm_check_peer_owner(shm_fd, g_peer_uid))   /* #87 finding 1 */
    {
        close(shm_fd);
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "shm owner mismatch");
        return;
    }
    float *query = mmap(NULL, vec_bytes, PROT_READ, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    if (query == MAP_FAILED)
    {
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "mmap failed");
        return;
    }

    if (cmd->dim != e->dim)
    {
        munmap(query, vec_bytes);
        pthread_mutex_unlock(&g_index_mutex);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_DIM_MISMATCH;
        snprintf(hdr.error, sizeof(hdr.error),
                 "query dim %u does not match index dim %u", cmd->dim, e->dim);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    int k = (int)cmd->k;
    if (k <= 0) k = 1;

    CuvsSearchResult *raw = malloc((size_t)(k > 0 ? k : 1) * sizeof(CuvsSearchResult));
    CuvsResult       *results = malloc((size_t)(k > 0 ? k : 1) * sizeof(CuvsResult));
    if (raw)
    {
        /* cuvs_ivfpq_search now pads slots the corpus cannot fill (TAIL
         * CONTRACT in cuvs_wrapper.h, #103). The loop below uses `continue`,
         * not `break`, so seed the same sentinel it rejects: that keeps it safe
         * against a wrapper that returns non-zero after a partial write. */
        for (int i = 0; i < k; i++)
            raw[i].item_id = -1;
    }
    if (!raw || !results)
    {
        free(raw); free(results);
        munmap(query, vec_bytes);
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "malloc failed");
        return;
    }

    uint32_t n_probes = cmd->n_probes ? cmd->n_probes : 64;
    int sret = cuvs_ivfpq_search(e->ivfpq_handle, query, (int)cmd->dim, k,
                                  n_probes, raw, (int)e->gpu_device_id);
    munmap(query, vec_bytes);

    int n_valid = 0;
    if (sret == 0)
    {
        for (int i = 0; i < k; i++)
        {
            int64_t item_id = raw[i].item_id;
            if (item_id < 0 || item_id >= e->n_vecs)
                continue;
            results[n_valid].tid      = e->tids[item_id];
            results[n_valid].distance = raw[i].distance;
            n_valid++;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint32_t latency_us = (uint32_t)(
        (t1.tv_sec - t0.tv_sec) * 1000000 +
        (t1.tv_nsec - t0.tv_nsec) / 1000);

    e->last_search      = time(NULL);
    e->last_search_mode = 5; /* ivfpq */
    record_search_stat(e, sret == 0 ? CUVS_STATUS_OK : CUVS_STATUS_ERROR, latency_us, NULL);
    e->last_requested_k = cmd->k;
    e->last_returned_k  = (uint32_t)n_valid;

    pthread_mutex_unlock(&g_index_mutex);

    free(raw);

    if (sret != 0)
    {
        free(results);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_ERROR;
        strncpy(hdr.error, "IVF-PQ search failed", sizeof(hdr.error) - 1);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    CuvsReplyHeader hdr = {0};
    hdr.status     = CUVS_STATUS_OK;
    hdr.n_results  = (uint32_t)n_valid;
    hdr.latency_us = latency_us;
    send_all(client_fd, &hdr, sizeof(hdr));
    if (n_valid > 0)
        send_all(client_fd, results, (size_t)n_valid * sizeof(CuvsResult));
    free(results);
}

/* ----------------------------------------------------------------
 * 3Q: handle_extend — extend a loaded CAGRA index in-place with new vectors.
 *
 * shm layout: same as BUILD — [float32 vecs: n_vecs×dim][uint64_t tids: n_vecs].
 * After cuvs_cagra_extend, VRAM graph includes the new vectors; no disk write
 * (extend_sync=false default). Rev_tids rebuilt so 3O prefilter stays correct.
 * ---------------------------------------------------------------- */
static void
handle_extend(int client_fd, const CuvsCmdFrame *cmd)
{
    char index_dir[256] = {0};
    int  passed_fd = -1;

    if (cuvs_fd_recv(client_fd, index_dir, sizeof(index_dir), &passed_fd) < 0)
    {
        send_error(client_fd, "recv index_dir failed");
        return;
    }
    if (passed_fd >= 0) close(passed_fd);

    if (cmd->n_vecs < 1 || cmd->dim == 0)
    {
        send_error(client_fd, "EXTEND needs at least 1 vector");
        return;
    }

    size_t vec_bytes = (size_t)cmd->n_vecs * cmd->dim * sizeof(float);
    size_t tid_bytes = (size_t)cmd->n_vecs * sizeof(uint64_t);
    size_t total     = vec_bytes + tid_bytes;

    int shm_fd = shm_open(cmd->shm_key, O_RDONLY, 0);
    if (shm_fd < 0)
    {
        send_error(client_fd, "shm_open failed");
        return;
    }
    if (!shm_check_peer_owner(shm_fd, g_peer_uid))   /* #87 finding 1 */
    {
        close(shm_fd);
        send_error(client_fd, "shm owner mismatch");
        return;
    }
    void *mem = mmap(NULL, total, PROT_READ, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    if (mem == MAP_FAILED)
    {
        send_error(client_fd, "mmap failed");
        return;
    }

    const float    *new_vecs = (const float *)mem;
    const uint64_t *new_tids_src = (const uint64_t *)((const char *)mem + vec_bytes);

    pthread_mutex_lock(&g_index_mutex);

    IndexEntry *e = find_index(cmd->db_oid, cmd->index_oid);
    if (!e || !e->handle)
    {
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_NOT_FOUND;
        /* EXTEND is cagra-only (needs the graph handle). flat/ivfpq have
         * handle==NULL; the backend skips extend for them (pg_cuvs.c aminsert),
         * so reaching here means a not-loaded or non-extendable index. */
        snprintf(hdr.error, sizeof(hdr.error),
                 "index %u/%u not loaded or not extendable (extend is cagra-only)",
                 cmd->db_oid, cmd->index_oid);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    int64_t  old_n = e->n_vecs;
    int64_t  n_new = cmd->n_vecs;

    /* Pre-flight VRAM budget check.
     * Cannot call ensure_vram here: evict_lru shifts g_indexes[], invalidating
     * this IndexEntry *e pointer.  A budget-only check is sufficient — the
     * backend falls through to delta on BUILD_FAILED (pg_cuvs.c:3029). */
    size_t new_vram   = estimate_vram_bytes(old_n + n_new, (int)e->dim, e->graph_degree);
    size_t delta_vram = (new_vram > e->vram_bytes) ? new_vram - e->vram_bytes : 0;
    int    dev        = (int)e->gpu_device_id;
    if (delta_vram > 0 &&
        g_max_vram_per_gpu[dev] > 0 &&
        total_vram_used(dev) + delta_vram > g_max_vram_per_gpu[dev])
    {
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED, "not enough VRAM for extend");
        return;
    }

    /* Grow TIDs array before the GPU call; failure here is cheap to handle. */
    uint64_t *grown = realloc(e->tids, (size_t)(old_n + n_new) * sizeof(uint64_t));
    if (!grown)
    {
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        send_error_code(client_fd, CUVS_STATUS_ERROR, "realloc tids failed");
        return;
    }
    e->tids = grown;
    memcpy(e->tids + old_n, new_tids_src, (size_t)n_new * sizeof(uint64_t));

    int rc = cuvs_cagra_extend(e->handle, new_vecs, n_new, (int)cmd->dim,
                               cmd->max_chunk_size, (int)e->gpu_device_id);
    if (rc != 0)
    {
        /* Rollback TIDs to old_n: shrink the buffer so the invariant
         * e->tids[0..n_vecs-1] == valid holds again. realloc to a smaller
         * size is guaranteed to succeed on all POSIX malloc implementations;
         * on failure (defensive) the oversized buffer is still safe because
         * n_vecs was not updated. */
        if (old_n > 0)
        {
            uint64_t *shrunk = realloc(e->tids, (size_t)old_n * sizeof(uint64_t));
            if (shrunk)
                e->tids = shrunk;
        }
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED, "cuvs_cagra_extend failed");
        return;
    }

    e->n_vecs     += n_new;
    e->n_extended += n_new;
    e->file_current = 0;   /* I-FC (1): graph now differs from the files */
    e->vram_bytes  = estimate_vram_bytes(e->n_vecs, (int)e->dim, e->graph_degree);

    /* Rebuild rev_tids to include new item_ids (needed for 3O prefilter). */
    free(e->rev_tids);     e->rev_tids     = NULL;
    free(e->rev_item_ids); e->rev_item_ids = NULL;
    build_rev_tid_map(e);

    pthread_mutex_unlock(&g_index_mutex);
    munmap(mem, total);

    LOG_INFO("[handle_extend] %u/%u: added %lld vecs (total %lld, n_extended %lld)\n",
             cmd->db_oid, cmd->index_oid,
             (long long)n_new, (long long)e->n_vecs, (long long)e->n_extended);

    CuvsReplyHeader ok = {0};
    ok.status = CUVS_STATUS_OK;
    send_all(client_fd, &ok, sizeof(ok));
}

/* Comparator for qsort/bsearch on raw uint64_t arrays. */
static int
cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* ----------------------------------------------------------------
 * 3Q: handle_compact — remove tombstoned vectors via cagra::merge.
 *
 * No extra shm payload; daemon reads .tombstone sidecar from save_dir.
 * After merge: new handle + compacted tids + rebuilt rev_tids + save_index.
 * ---------------------------------------------------------------- */
static void
handle_compact(int client_fd, const CuvsCmdFrame *cmd)
{
    char index_dir[256] = {0};
    int  passed_fd = -1;

    if (cuvs_fd_recv(client_fd, index_dir, sizeof(index_dir), &passed_fd) < 0)
    {
        send_error(client_fd, "recv index_dir failed");
        return;
    }
    if (passed_fd >= 0) close(passed_fd);

    const char *save_dir = (index_dir[0] != '\0') ? index_dir : g_index_dir;

    /* Read tombstone file before taking the mutex (disk I/O only). */
    char tombstone_path[512];
    snprintf(tombstone_path, sizeof(tombstone_path), "%s/%u_%u.tombstone",
             save_dir, cmd->db_oid, cmd->index_oid);

    FILE *tf = fopen(tombstone_path, "rb");
    if (!tf)
    {
        /* No tombstone: nothing to compact. Reply OK. */
        CuvsReplyHeader ok = {0};
        ok.status = CUVS_STATUS_OK;
        send_all(client_fd, &ok, sizeof(ok));
        return;
    }

    CuvsTombstoneHeader thdr;
    if (fread(&thdr, sizeof(thdr), 1, tf) != 1 ||
        thdr.magic != CUVS_TOMBSTONE_MAGIC || thdr.n_entries <= 0)
    {
        fclose(tf);
        send_error_code(client_fd, CUVS_STATUS_ERROR, "invalid tombstone file");
        return;
    }

    int64_t  n_dead   = thdr.n_entries;
    uint64_t *dead_tids = malloc((size_t)n_dead * sizeof(uint64_t));
    if (!dead_tids)
    {
        fclose(tf);
        send_error_code(client_fd, CUVS_STATUS_ERROR, "malloc dead_tids failed");
        return;
    }

    for (int64_t i = 0; i < n_dead; i++)
    {
        CuvsTombstoneRecord rec;
        if (fread(&rec, sizeof(rec), 1, tf) != 1)
        {
            fclose(tf);
            free(dead_tids);
            send_error_code(client_fd, CUVS_STATUS_ERROR, "tombstone read failed");
            return;
        }
        dead_tids[i] = rec.tid;
    }
    fclose(tf);
    qsort(dead_tids, (size_t)n_dead, sizeof(uint64_t), cmp_u64);

    pthread_mutex_lock(&g_index_mutex);

    IndexEntry *e = find_index(cmd->db_oid, cmd->index_oid);
    if (!e || !e->handle)
    {
        pthread_mutex_unlock(&g_index_mutex);
        free(dead_tids);
        send_error_code(client_fd, CUVS_STATUS_NOT_FOUND,
                        "CAGRA index not loaded for compact");
        return;
    }

    /* Build keep_bits: bit[i]=1 → keep vector i (not tombstoned). */
    int64_t   n_vecs   = e->n_vecs;
    int64_t   n_words  = (n_vecs + 31) / 32;
    uint32_t *keep_bits = calloc((size_t)n_words, sizeof(uint32_t));
    if (!keep_bits)
    {
        pthread_mutex_unlock(&g_index_mutex);
        free(dead_tids);
        send_error_code(client_fd, CUVS_STATUS_ERROR, "calloc keep_bits failed");
        return;
    }

    int64_t dead_count = 0;
    for (int64_t i = 0; i < n_vecs; i++)
    {
        if (!bsearch(&e->tids[i], dead_tids, (size_t)n_dead,
                     sizeof(uint64_t), cmp_u64))
        {
            keep_bits[i / 32] |= (1u << (i % 32));
        }
        else
        {
            dead_count++;
        }
    }
    free(dead_tids);

    if (dead_count == 0)
    {
        pthread_mutex_unlock(&g_index_mutex);
        free(keep_bits);
        unlink(tombstone_path);
        CuvsReplyHeader ok = {0};
        ok.status = CUVS_STATUS_OK;
        send_all(client_fd, &ok, sizeof(ok));
        return;
    }

    LOG_INFO("[handle_compact] %u/%u: removing %lld of %lld vectors\n",
             cmd->db_oid, cmd->index_oid, (long long)dead_count, (long long)n_vecs);

    int64_t   new_n    = n_vecs - dead_count;

    /* Build compacted TIDs BEFORE the GPU call — both operations need keep_bits.
     * Free keep_bits only after both consumers are done. */
    uint64_t *new_tids = malloc((size_t)new_n * sizeof(uint64_t));
    if (!new_tids)
    {
        pthread_mutex_unlock(&g_index_mutex);
        free(keep_bits);
        send_error_code(client_fd, CUVS_STATUS_ERROR, "malloc new_tids failed");
        return;
    }
    {
        int64_t j = 0;
        for (int64_t i = 0; i < n_vecs; i++)
            if (keep_bits[i / 32] & (1u << (i % 32)))
                new_tids[j++] = e->tids[i];
    }

    CuvsCagraIndex new_handle = cuvs_cagra_compact(
        e->handle, keep_bits, n_vecs, e->metric, (int)e->gpu_device_id);
    free(keep_bits);

    if (!new_handle)
    {
        pthread_mutex_unlock(&g_index_mutex);
        free(new_tids);
        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED,
                        "cuvs_cagra_compact failed");
        return;
    }

    /* Swap handle + tids. */
    CuvsCagraIndex old_handle = e->handle;
    e->handle = new_handle;
    free(e->tids);
    e->tids = new_tids;
    e->n_vecs      = new_n;
    e->file_current = 0;   /* I-FC: merged in memory; save_index sets 1 */
    e->n_extended       = 0;
    e->compact_count++;
    e->last_compact_at  = time(NULL);
    e->vram_bytes  = estimate_vram_bytes(new_n, (int)e->dim, e->graph_degree);

    cuvs_cagra_free(old_handle, (int)e->gpu_device_id);

    /* Rebuild rev_tids from scratch. */
    free(e->rev_tids);     e->rev_tids     = NULL;
    free(e->rev_item_ids); e->rev_item_ids = NULL;
    build_rev_tid_map(e);

    /* Persist to disk. */
    if (save_index(e) != 0)
    {
        pthread_mutex_unlock(&g_index_mutex);
        send_error_code(client_fd, CUVS_STATUS_PERSIST_FAILED,
                        "save_index failed after compact");
        return;
    }

    unlink(tombstone_path);

    pthread_mutex_unlock(&g_index_mutex);

    LOG_INFO("[handle_compact] %u/%u: compact OK, %lld vecs remaining\n",
             cmd->db_oid, cmd->index_oid, (long long)new_n);

    CuvsReplyHeader ok = {0};
    ok.status = CUVS_STATUS_OK;
    send_all(client_fd, &ok, sizeof(ok));
}

/* ----------------------------------------------------------------
 * handle_set_vram_budget — runtime VRAM budget override (test/admin).
 *
 * cmd->n_vecs = new budget in bytes; 0 = unlimited.
 * No extra payload — just update g_max_vram_per_gpu for all GPUs.
 * ---------------------------------------------------------------- */
static void
handle_set_vram_budget(int client_fd, const CuvsCmdFrame *cmd)
{
    size_t new_budget = (size_t)(uint64_t)cmd->n_vecs;  /* 0 = unlimited */

    pthread_mutex_lock(&g_index_mutex);
    g_max_vram_bytes = new_budget;
    for (int d = 0; d < CUVS_MAX_GPUS; d++)
        g_max_vram_per_gpu[d] = new_budget;
    pthread_mutex_unlock(&g_index_mutex);

    LOG_INFO("[set_vram_budget] budget set to %zu bytes\n", new_budget);

    CuvsReplyHeader ok = {0};
    ok.status = CUVS_STATUS_OK;
    send_all(client_fd, &ok, sizeof(ok));
}

/* ----------------------------------------------------------------
 * handle_eat_vram / handle_free_vram — test helpers.
 *
 * EAT:  cmd->n_vecs = leave_bytes, cmd->dim = device_id
 * FREE: cmd->dim = device_id
 * ---------------------------------------------------------------- */
static void
handle_eat_vram(int client_fd, const CuvsCmdFrame *cmd)
{
    int64_t leave_bytes = cmd->n_vecs;
    int     dev         = ((int)cmd->dim >= 0 && (int)cmd->dim < CUVS_MAX_GPUS)
                          ? (int)cmd->dim : 0;

    pthread_mutex_lock(&g_index_mutex);
    int rc = cuvs_eat_vram(leave_bytes, dev);
    pthread_mutex_unlock(&g_index_mutex);

    CuvsReplyHeader ok = {0};
    ok.status = (rc == 0) ? CUVS_STATUS_OK : CUVS_STATUS_ERROR;
    send_all(client_fd, &ok, sizeof(ok));
}

static void
handle_free_vram(int client_fd, const CuvsCmdFrame *cmd)
{
    int dev = ((int)cmd->dim >= 0 && (int)cmd->dim < CUVS_MAX_GPUS)
              ? (int)cmd->dim : 0;

    pthread_mutex_lock(&g_index_mutex);
    cuvs_free_vram(dev);
    pthread_mutex_unlock(&g_index_mutex);

    CuvsReplyHeader ok = {0};
    ok.status = CUVS_STATUS_OK;
    send_all(client_fd, &ok, sizeof(ok));
}

/* #124: handle_inject_extend_oom/handle_inject_build_oom (and their dispatch
 * cases below) are test-only fault-injection hooks — a client that can reach
 * the daemon's socket could otherwise arm a synthetic OOM against ANY
 * resident index in a production deployment. Unlike the CUVS_FAULT_* hooks
 * elsewhere in this file (gated by CUVS_TEST_HOOKS at each call site, e.g.
 * around line 5089), these two were dispatched unconditionally. Guarding the
 * handler bodies too (not just the dispatch, below) avoids an unused-function
 * warning in a production (non-CUVS_TEST_HOOKS) build. */
#ifdef CUVS_TEST_HOOKS
/* handle_inject_extend_oom — test helper: arm/disarm synthetic OOM in cuvs_cagra_extend.
 * cmd->dim == 1 arms; == 0 disarms. */
static void
handle_inject_extend_oom(int client_fd, const CuvsCmdFrame *cmd)
{
    cuvs_set_inject_extend_oom((int)cmd->dim);

    CuvsReplyHeader ok = {0};
    ok.status = CUVS_STATUS_OK;
    send_all(client_fd, &ok, sizeof(ok));
}

/* handle_inject_build_oom — test helper (ADR-070 Bug #3): arm synthetic OOM for
 * the next cmd->dim cuvs_cagra_build calls, so a test can exercise the daemon's
 * evict-and-retry path deterministically. cmd->dim == 0 disarms. */
static void
handle_inject_build_oom(int client_fd, const CuvsCmdFrame *cmd)
{
    cuvs_set_inject_build_oom((int)cmd->dim);

    CuvsReplyHeader ok = {0};
    ok.status = CUVS_STATUS_OK;
    send_all(client_fd, &ok, sizeof(ok));
}
#endif /* CUVS_TEST_HOOKS */

/* Temporary HNSW builds are serialized and accounted independently of the
 * permanent registry. Holding g_index_mutex during the cuVS call also excludes
 * legacy GPU builders/search work, whose memory is otherwise not a reservation
 * in this admission estimate. Cancel/owner EOF remains observable on its FD. */
/* Host RAM headroom reader with injectable roots (tests use a fake tree).
 * hnsw_host_headroom() below is the production entry with the real paths. */
typedef struct HostHeadroomPaths {
    const char *meminfo;      /* /proc/meminfo */
    const char *self_cgroup;  /* /proc/self/cgroup */
    const char *cgroup_root;  /* /sys/fs/cgroup */
} HostHeadroomPaths;

static uint64_t
host_headroom_at(const HostHeadroomPaths *paths, uint64_t requested,
                 char *error, size_t error_len)
{
    FILE *f = fopen(paths->meminfo, "r");
    char line[4096];
    unsigned long long available = 0;
    if (!f) return 0;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "MemAvailable: %llu kB", &available) == 1) break;
    fclose(f);
    if (!available || available > UINT64_MAX / 1024) return 0;
    uint64_t budget = available * 1024 / 2;
    if (requested < budget) budget = requested;
    /* Unified cgroup: include every ancestor, since a parent's remaining
     * memory limit constrains us even when the leaf says max. */
    f = fopen(paths->self_cgroup, "r");
    if (!f) return 0; // an unreadable cgroup limit is not unlimited
    if (f)
    {
        while (fgets(line, sizeof(line), f))
        {
            if (strncmp(line, "0::", 3))
            {
                // Do not silently ignore a legacy memory-controller limit.
                // The bounded path currently resolves unified cgroup v2 only.
                char *first = strchr(line, ':'), *last;
                if (first && (last = strchr(first + 1, ':')) != NULL)
                {
                    *last = '\0';
                    char *save = NULL;
                    char *controller = strtok_r(first + 1, ",", &save);
                    while (controller)
                    {
                        if (strcmp(controller, "memory") == 0)
                        {
                            snprintf(error, error_len, "HNSW capacity: cgroup v1 memory controller is unsupported; use verified cgroup v2");
                            fclose(f); return 0;
                        }
                        controller = strtok_r(NULL, ",", &save);
                    }
                }
                continue;
            }
            char group[4096], path[8192 + 512];
            snprintf(group, sizeof(group), "%s", line + 3);
            group[strcspn(group, "\n")] = '\0';
            if (strstr(group, "..")) { fclose(f); return 0; }
            for (;;)
            {
                unsigned long long limit, current;
                FILE *m;
                snprintf(path, sizeof(path), "%s%s/memory.max", paths->cgroup_root, group);
                m = fopen(path, "r");
                if (m)
                {
                    int numeric = fscanf(m, "%llu", &limit) == 1;
                    fclose(m);
                    if (numeric)
                    {
                        snprintf(path, sizeof(path), "%s%s/memory.current", paths->cgroup_root, group);
                        m = fopen(path, "r");
                        if (!m) { fclose(f); return 0; }
                        int ok = fscanf(m, "%llu", &current) == 1;
                        fclose(m);
                        if (!ok) { fclose(f); return 0; }
                        uint64_t remaining = current < limit ? (limit - current) / 2 : 0;
                        if (remaining < budget) budget = remaining;
                    }
                }
                char *slash = strrchr(group, '/');
                if (!slash) break;
                *slash = '\0';
            }
        }
        fclose(f);
    }
    return budget;
}

static uint64_t
hnsw_host_headroom(uint64_t requested, char *error, size_t error_len)
{
    static const HostHeadroomPaths system_paths = {
        "/proc/meminfo", "/proc/self/cgroup", "/sys/fs/cgroup"};
    return host_headroom_at(&system_paths, requested, error, error_len);
}

#ifdef CUVS_TEST_HOOKS
typedef struct HnswTestPhase {
    int owner_fd;
    const char *workdir;
} HnswTestPhase;

/* Test daemon only. The production daemon never installs this callback, and
 * no wire field can supply an executable address or a pause configuration. */
static int
hnsw_test_phase(void *arg, const char *phase)
{
    HnswTestPhase *test = arg;
    const char *pause = !strcmp(phase, "after-graph")
        ? getenv("CUVS_TEST_HNSW_POST_GRAPH_PAUSE_MS") : NULL;
    long ms = pause ? strtol(pause, NULL, 10) : 0;
    if (ms > 0 && ms <= 10000)
    {
        uint64_t until = cuvs_hnsw_monotonic_us() + (uint64_t)ms * 1000;
        LOG_INFO("[hnsw_diskfull_test] job=%s phase=after-graph mono_us=%llu pause_ms=%ld\n",
            test->workdir, (unsigned long long)cuvs_hnsw_monotonic_us(), ms);
        while (cuvs_hnsw_monotonic_us() < until)
        {
            struct pollfd owner = {.fd = test->owner_fd, .events = POLLIN};
            if (cuvs_hnsw_owner_cancelled(test->owner_fd)) return 1;
            (void)poll(&owner, 1, 100);
        }
    }
    return cuvs_hnsw_owner_cancelled(test->owner_fd);
}
#endif

/* Called under g_index_mutex; never wait here while holding that lock. */
static int
hnsw_gpu_work_pending(void *unused)
{
    int busy = g_unlocked_gpu_jobs != 0;
    (void)unused;
    for (int gpu = 0; gpu < CUVS_MAX_GPUS; gpu++)
        if (g_pending_build_vram[gpu]) busy = 1;
    for (int i = 0; i < g_n_indexes; i++)
        if (g_indexes[i].inflight > 0) busy = 1;
#ifdef CUVS_TEST_HOOKS
    if (busy)
        LOG_INFO("[hnsw_admission_test] phase=wait mono_us=%llu unlocked_gpu_jobs=%u\n",
            (unsigned long long)cuvs_hnsw_monotonic_us(), g_unlocked_gpu_jobs);
#endif
    return busy;
}

/* One GPU budget formula for the build decision and the preflight. With the
 * registry lock held (build) residents are subtracted from the logical
 * budget; the preflight passes locked=0 when the lock is busy and then uses
 * the static logical budget (ADR-087 decision 4: GPU budget static at the
 * pre-spool notice; the build re-reads it after wait_for_idle). */
static uint64_t
hnsw_gpu_budget(int device, int locked)
{
    uint64_t budget = cuvs_vram_free_bytes_on(device);
    if (g_max_vram_per_gpu[device])
    {
        uint64_t used = locked ? total_vram_used(device) : 0;
        uint64_t remaining = used < g_max_vram_per_gpu[device]
            ? g_max_vram_per_gpu[device] - used : 0;
        if (remaining < budget) budget = remaining;
    }
    return budget;
}

/* [hnsw_route]: one line per decision, pre-spool forecast or build. Q4-5/Q4-6
 * read est_host and route from it; route must equal the receipt's route. */
static void
hnsw_route_log(const char *phase, uint32_t algo, const CuvsHnswForecast *f,
               const char *error)
{
    LOG_INFO("[hnsw_route] phase=%s requested_algo=%s route=%s overflow=%s overflowed=%u status=%u rows=%llu est_host=%llu est_gpu=%llu host_budget=%llu gpu_budget=%llu partitions=%u disk_need=%llu%s%s\n",
        phase, cuvs_hnsw_algo_name(algo),
        f->status == CUVS_STATUS_OK ? cuvs_hnsw_route_name(f->decision.route) : "none",
        cuvs_hnsw_overflow_name(g_hnsw_auto_overflow), f->decision.overflowed, f->status,
        (unsigned long long)f->n, (unsigned long long)f->host_estimate,
        (unsigned long long)f->gpu_estimate, (unsigned long long)f->host_budget,
        (unsigned long long)f->gpu_budget, f->decision.partitions,
        (unsigned long long)f->disk_need,
        error && error[0] ? " error=" : "", error && error[0] ? error : "");
}

/* Regular-disk probe of an existing directory (index dir or private
 * workdir). tmpfs/ramfs are rejected like the input spool. */
static int
hnsw_dir_disk(const char *dir, uint64_t *avail)
{
    struct statvfs disk;
    int regular = 0;
    int probe = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
#ifdef __linux__
    struct statfs fs;
    regular = probe >= 0 && fstatfs(probe, &fs) == 0
        && fs.f_type != 0x01021994 && fs.f_type != 0x858458f6;
#else
    regular = probe >= 0;
#endif
    if (probe >= 0) close(probe);
    *avail = 0;
    if (statvfs(dir, &disk) == 0)
    {
        long double bytes = (long double)disk.f_bavail * disk.f_frsize;
        *avail = bytes > (long double)UINT64_MAX ? UINT64_MAX : (uint64_t)bytes;
    }
    return regular;
}

/* Op 40: read-only forecast from table statistics. No wait_for_idle, no
 * workdir, no reservation; the backend uses it only to announce the route
 * (PN-1) or to fail before its heap scan. */
static void
handle_hnsw_route_preflight(int client_fd)
{
    CuvsHnswPreflightRequest request;
    CuvsHnswPreflightReply reply;
    CuvsHnswForecast f;
    int locked = 0;
    memset(&reply, 0, sizeof(reply));
    memset(&f, 0, sizeof(f));
    reply.version = CUVS_HNSW_PREFLIGHT_VERSION;
    reply.auto_overflow = g_hnsw_auto_overflow;
    reply.header.status = CUVS_STATUS_BUILD_FAILED;
    if (recv_all(client_fd, &request, sizeof(request)) < 0) return;
    reply.n_estimate = request.n_estimate;
    snprintf(reply.index_dir, sizeof(reply.index_dir), "%s", g_index_dir);
    if (request.version != CUVS_HNSW_PREFLIGHT_VERSION || request.hierarchy > 1
        || request.after_scan > 1 || !request.host_budget || !request.n_estimate
        || request.n_estimate > CUVS_TIDS_MAX_VECS || !request.dim
        || request.dim > CUVS_VECTORS_MAX_DIM)
    {
        snprintf(reply.header.error, sizeof(reply.header.error), "invalid HNSW preflight request");
        goto send;
    }
    for (int tries = 0; tries < 10 && !locked; tries++)
    {
        if (pthread_mutex_trylock(&g_index_mutex) == 0) locked = 1;
        else (void)poll(NULL, 0, 100);
    }
    int device = usable_gpu(0);
    reply.header.error[0] = '\0';
    uint64_t host = hnsw_host_headroom(request.host_budget, reply.header.error, sizeof(reply.header.error));
    uint64_t gpu = hnsw_gpu_budget(device, locked);
    if (locked) pthread_mutex_unlock(&g_index_mutex);
    reply.gpu_static = !locked;
    if (!host || !gpu)
    {
        if (!reply.header.error[0])
            snprintf(reply.header.error, sizeof(reply.header.error), "HNSW host/GPU/cgroup headroom unavailable");
        f.n = request.n_estimate; f.host_budget = host; f.gpu_budget = gpu;
        f.status = CUVS_STATUS_BUILD_FAILED;
    }
    else if (cuvs_hnsw_forecast_route(request.n_estimate, request.dim, request.graph_degree,
                 request.intermediate_degree, request.hierarchy, request.build_algo,
                 request.ef_construction, g_hnsw_auto_overflow, host, gpu, &f,
                 reply.header.error, sizeof(reply.header.error)) == CUVS_STATUS_OK)
    {
        uint64_t avail;
        reply.disk_regular = (uint32_t)hnsw_dir_disk(g_index_dir, &avail);
        (void)cuvs_hnsw_forecast_disk(&f, reply.disk_regular, avail,
            reply.header.error, sizeof(reply.header.error));
    }
    hnsw_route_log(request.after_scan ? "postscan" : "prespool", request.build_algo, &f, reply.header.error);
    reply.header.status = f.status;
    reply.route = f.status == CUVS_STATUS_OK ? f.decision.route : 0;
    reply.partitions = f.decision.partitions;
    reply.overflowed = f.decision.overflowed;
    reply.disk_ok = f.disk_ok;
    reply.host_estimate = f.host_estimate; reply.gpu_estimate = f.gpu_estimate;
    reply.host_budget = f.host_budget; reply.gpu_budget = f.gpu_budget;
    reply.disk_need = f.disk_need; reply.disk_avail = f.disk_avail;
 send:
    send_all(client_fd, &reply, sizeof(reply));
}

static void
handle_build_hnsw_file(int client_fd, const CuvsCmdFrame *cmd)
{
    CuvsHnswFileRequest request = {0};
    CuvsHnswFileBuild params = {0};
    CuvsHnswFileReply wire = {0};
    CuvsReplyHeader reply = {0};
    struct stat st;
    int vectors_fd = -1, owner_fd = -1, result_fd = -1;
    int locked = 0, lease_fd = -1;
    char token;
    char workdir[4096] = {0};
    reply.status = CUVS_STATUS_BUILD_FAILED;
    snprintf(reply.error, sizeof(reply.error), "invalid HNSW file request");
    int request_status = cuvs_hnsw_receive_file_request(client_fd, &request,
        &vectors_fd, &owner_fd, reply.error, sizeof(reply.error));
    if (request_status != CUVS_STATUS_OK)
    {
        reply.status = request_status;
        goto done;
    }
    if (cmd->n_vecs <= 0 || cmd->n_vecs > CUVS_TIDS_MAX_VECS
        || cmd->dim == 0 || cmd->dim > CUVS_VECTORS_MAX_DIM
        || cmd->metric > CUVS_METRIC_IP || !cuvs_hnsw_regular_disk(vectors_fd)
        || !shm_check_peer_owner(vectors_fd, g_peer_uid)
        || fstat(vectors_fd, &st) || st.st_size < 0
        || (uint64_t)st.st_size != (uint64_t)cmd->n_vecs * cmd->dim * sizeof(float)) goto done;
    CuvsHnswCapacity checked;
    if (cuvs_hnsw_capacity((uint64_t)cmd->n_vecs, cmd->dim,
        cmd->graph_degree, cmd->intermediate_graph_degree, request.hierarchy, &checked)) goto done;
    if (owner_fd < 0 || fstat(owner_fd, &st) || !S_ISSOCK(st.st_mode)) goto done;
    fcntl(owner_fd, F_SETFD, FD_CLOEXEC);
    fcntl(vectors_fd, F_SETFD, FD_CLOEXEC);
    params.n_vecs = (uint64_t)cmd->n_vecs;
    params.dim = cmd->dim;
    params.metric = cmd->metric;
    params.graph_degree = cmd->graph_degree;
    params.intermediate_degree = cmd->intermediate_graph_degree;
    params.hierarchy = request.hierarchy;
    params.build_algo = request.build_algo;
    params.ef_construction = request.ef_construction;
    params.auto_overflow = g_hnsw_auto_overflow;
    params.policy = &wire.policy;
    if (cuvs_hnsw_wait_for_idle(&g_index_mutex, owner_fd, hnsw_gpu_work_pending, NULL))
        goto cancelled;
    locked = 1;
    reply.error[0] = '\0';
    params.host_budget = hnsw_host_headroom(request.host_budget, reply.error, sizeof(reply.error));
    int device = usable_gpu(0);
    params.gpu_budget = hnsw_gpu_budget(device, 1);
    if (!params.host_budget || !params.gpu_budget)
    {
        if (!reply.error[0])
            snprintf(reply.error, sizeof(reply.error), "HNSW host/GPU/cgroup headroom unavailable");
        goto done;
    }
    /* ADR-087 decision 4: route decided right after the lock and budgets,
     * before mkdtemp and the disk check, by the same pure resolver the
     * wrapper re-runs with these exact inputs. */
    CuvsHnswForecast route;
    reply.status = cuvs_hnsw_forecast_route(params.n_vecs, params.dim,
        params.graph_degree, params.intermediate_degree, params.hierarchy,
        params.build_algo, params.ef_construction, params.auto_overflow,
        params.host_budget, params.gpu_budget, &route, reply.error, sizeof(reply.error));
    hnsw_route_log("build", params.build_algo, &route, reply.error);
    if (reply.status != CUVS_STATUS_OK) goto done;
    reply.status = CUVS_STATUS_BUILD_FAILED;
    (void)cuvs_hnsw_reap_workdirs(g_index_dir);
    if (snprintf(workdir, sizeof(workdir), "%s/.hnsw-job-XXXXXX", g_index_dir)
            >= (int)sizeof(workdir) || !mkdtemp(workdir))
    {
        workdir[0] = '\0';
        snprintf(reply.error, sizeof(reply.error), "cannot create private HNSW disk directory");
        goto done;
    }
    char lease_path[8192];
    snprintf(lease_path, sizeof(lease_path), "%s/.lease", workdir);
    lease_fd = open(lease_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (lease_fd < 0 || flock(lease_fd, LOCK_EX | LOCK_NB))
    {
        snprintf(reply.error, sizeof(reply.error), "cannot acquire HNSW temporary directory lease");
        goto done;
    }
    /* Reject tmpfs even if configured as the daemon's index directory.
     * Same formula as the pre-spool check (UC-5 sub-decision 1(a)). */
    {
        uint64_t avail;
        int regular = hnsw_dir_disk(workdir, &avail);
        if (cuvs_hnsw_forecast_disk(&route, regular, avail, reply.error,
                                    sizeof(reply.error)) != CUVS_STATUS_OK)
            goto done;
    }
    (void)fstat(vectors_fd, &st);
    LOG_INFO("[hnsw_job] job=%s phase=start mono_us=%llu rows=%llu dim=%u input_dev=%llu input_ino=%llu\n",
        workdir, (unsigned long long)cuvs_hnsw_monotonic_us(),
        (unsigned long long)params.n_vecs, params.dim,
        (unsigned long long)st.st_dev, (unsigned long long)st.st_ino);
#ifdef CUVS_TEST_HOOKS
    {
        const char *pause = getenv("CUVS_TEST_HNSW_PRE_GPU_PAUSE_MS");
        if (pause)
        {
            long ms = strtol(pause, NULL, 10);
            if (ms > 0 && ms <= 10000)
            {
                uint64_t until = cuvs_hnsw_monotonic_us() + (uint64_t)ms * 1000;
                LOG_INFO("[hnsw_oom_test] job=%s phase=admitted-before-cuvs mono_us=%llu pause_ms=%ld\n",
                    workdir, (unsigned long long)cuvs_hnsw_monotonic_us(), ms);
                while (cuvs_hnsw_monotonic_us() < until)
                {
                    struct pollfd owner = {.fd = owner_fd, .events = POLLIN};
                    if (cuvs_hnsw_owner_cancelled(owner_fd)) goto cancelled;
                    (void)poll(&owner, 1, 100);
                }
            }
        }
    }
#endif
#ifdef CUVS_TEST_HOOKS
    HnswTestPhase phase = {.owner_fd = owner_fd, .workdir = workdir};
    params.phase_hook = hnsw_test_phase;
    params.phase_arg = &phase;
#endif
    reply.status = cuvs_hnsw_build_file(vectors_fd, owner_fd, &params, workdir,
        device, &result_fd, reply.error, sizeof(reply.error));
    /* [hnsw_route] and the receipt must name the same route (Q4-6 (iii)). */
    if (reply.status == CUVS_STATUS_OK
        && (wire.policy.route != route.decision.route
            || !!(wire.policy.route_flags & CUVS_HNSW_ROUTE_FLAG_AUTO_OVERFLOW)
               != !!route.decision.overflowed))
    {
        reply.status = CUVS_STATUS_BUILD_FAILED;
        snprintf(reply.error, sizeof(reply.error), "HNSW receipt route %s differs from decided route %s",
                 cuvs_hnsw_route_name(wire.policy.route), cuvs_hnsw_route_name(route.decision.route));
    }
    LOG_INFO("[hnsw_job] job=%s phase=wrapper-return mono_us=%llu status=%u owner_cancelled=%d gpu_free_now=%zu resident_now=%zu\n",
        workdir, (unsigned long long)cuvs_hnsw_monotonic_us(), reply.status,
        cuvs_hnsw_owner_cancelled(owner_fd), cuvs_vram_free_bytes_on(device), total_vram_used(device));
    goto done;
 cancelled:
    reply.status = CUVS_STATUS_CANCELED;
    snprintf(reply.error, sizeof(reply.error), "HNSW owner cancelled or disconnected");
 done:
    if (locked) pthread_mutex_unlock(&g_index_mutex);
    if (vectors_fd >= 0) close(vectors_fd);
    if (workdir[0])
    {
        char lease_path[8192];
        snprintf(lease_path, sizeof(lease_path), "%s/.lease", workdir);
        unlink(lease_path);
        if (rmdir(workdir) != 0 && errno != ENOENT)
        {
            // Keep an unlocked lease marker if cleanup was incomplete, so a
            // later startup/build can reap the private directory safely.
            int marker = open(lease_path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
            if (marker >= 0) close(marker);
            LOG_WARN("[hnsw_build] deferred temporary cleanup: %s\n", workdir);
        }
    }
    if (lease_fd >= 0) close(lease_fd);
    if (owner_fd >= 0 && cuvs_hnsw_owner_cancelled(owner_fd))
        reply.status = CUVS_STATUS_CANCELED;
    if (reply.status != CUVS_STATUS_OK && result_fd >= 0)
    { close(result_fd); result_fd = -1; }
    wire.header = reply;
    token = 'R';
    if (send_all(client_fd, &token, 1) == 0
        && cuvs_fd_send(client_fd, result_fd, &wire, sizeof(wire)) == 0
        && reply.status == CUVS_STATUS_OK)
    {
        /* The backend ACKs receipt of its own kernel reference; it then owns
         * the temporary file through PG conversion and closes it on ERROR. */
        struct pollfd pfd = {.fd = owner_fd, .events = POLLIN};
        if (poll(&pfd, 1, 30000) > 0) (void)recv(owner_fd, &token, 1, 0);
    }
    if (result_fd >= 0) close(result_fd);
    if (owner_fd >= 0) close(owner_fd);
}

/* ----------------------------------------------------------------
 * Per-connection thread
 * ---------------------------------------------------------------- */
/* ----------------------------------------------------------------
 * I3-B: manual cache ops, status text, reclaim thread (v6 C7, C8, EV-5)
 * ---------------------------------------------------------------- */

/* A text buffer for the status ops. */
typedef struct TextBuf { char *p; size_t len, cap; int oom; } TextBuf;

static void
tb_printf(TextBuf *b, const char *fmt, ...)
{
    va_list ap;
    if (b->oom)
        return;
    for (;;)
    {
        size_t room = b->cap - b->len;
        va_start(ap, fmt);
        int w = vsnprintf(b->p ? b->p + b->len : NULL, b->p ? room : 0, fmt, ap);
        va_end(ap);
        if (w < 0) { b->oom = 1; return; }
        if (b->p && (size_t)w < room) { b->len += (size_t)w; return; }
        size_t ncap = b->cap ? b->cap * 2 : 4096;
        while (ncap - b->len <= (size_t)w)
            ncap *= 2;
        if (ncap > CUVS_IPC_TEXT_MAX + 1) { b->oom = 1; return; }
        char *np = realloc(b->p, ncap);
        if (!np) { b->oom = 1; return; }
        b->p = np;
        b->cap = ncap;
    }
}

static void
send_text(int client_fd, TextBuf *b)
{
    CuvsReplyHeader hdr = {0};
    if (b->oom)
    {
        hdr.status = CUVS_STATUS_ERROR;
        snprintf(hdr.error, sizeof(hdr.error), "status text too large");
        send_all(client_fd, &hdr, sizeof(hdr));
    }
    else
    {
        hdr.status = CUVS_STATUS_OK;
        hdr.n_results = (uint32_t)b->len;
        send_all(client_fd, &hdr, sizeof(hdr));
        if (b->len)
            send_all(client_fd, b->p, b->len);
    }
    free(b->p);
}

static void
reply_msg(int client_fd, int status, const char *fmt, ...)
{
    CuvsReplyHeader hdr = {0};
    va_list ap;
    hdr.status = (uint32_t)status;
    va_start(ap, fmt);
    vsnprintf(hdr.error, sizeof(hdr.error), fmt, ap);
    va_end(ap);
    attach_build_notices(&hdr);
    send_all(client_fd, &hdr, sizeof(hdr));
}

/* GPU an entry is accounted on for single-GPU purposes (sharded: shard 0). */
static int
entry_home_gpu(const IndexEntry *e)
{
    if (e->shard_count >= 2 && e->shards)
        return (int)e->shards[0].gpu_device_id;
    return (int)e->gpu_device_id;
}

static size_t
entry_total_bytes(const IndexEntry *e)
{
    size_t t = 0;
    for (int i = 0; i < n_usable_gpus(); i++)
        t += entry_bytes_on(e, usable_gpu(i));
    return t;
}

static const char *k_unload_warning =
    "unloaded; warning: delta or tombstone file present, GPU path switches to "
    "CPU replan until REINDEX";

static void
handle_manual(int client_fd, const CuvsCmdFrame *cmd)
{
    uint32_t db = cmd->db_oid, idx = cmd->index_oid;
    pthread_mutex_lock(&g_index_mutex);
    IndexEntry *e = find_index(db, idx);

    switch (cmd->op)
    {
        case CUVS_OP_UNLOAD:
        {
            if (!e)
            {
                pthread_mutex_unlock(&g_index_mutex);
                reply_msg(client_fd, CUVS_STATUS_OK, "not resident");
                return;
            }
            if (e->pinned)
            {
                pthread_mutex_unlock(&g_index_mutex);
                reply_msg(client_fd, CUVS_STATUS_ERROR,
                          "pinned: run pg_cuvs_unpin first");
                return;
            }
            if (e->inflight > 0)
            {
                pthread_mutex_unlock(&g_index_mutex);
                reply_msg(client_fd, CUVS_STATUS_ERROR,
                          "in use: retry after current queries finish");
                return;
            }
            /* Warn when this save rewrites .tids under a delta/tombstone
             * sidecar of a CAGRA index that changed since it was saved. */
            int warn = entry_is_cagra_single(e) && !e->file_current &&
                       cuvs_entry_sidecar_kind(g_index_dir, db, idx) != CUVS_SIDECAR_NONE;
            int pf = 0;
            (void) evict_entry(e, entry_home_gpu(e), CUVS_EVICT_MANUAL_UNLOAD, 0,
                               CUVS_RQ_MANUAL, &pf);
            pthread_mutex_unlock(&g_index_mutex);
            if (pf)
                reply_msg(client_fd, CUVS_STATUS_ERROR,
                          "save failed: index stays resident (see daemon log)");
            else
                reply_msg(client_fd, CUVS_STATUS_OK, "%s",
                          warn ? k_unload_warning : "unloaded");
            return;
        }
        case CUVS_OP_LOAD:
        case CUVS_OP_PIN:
        {
            int loaded_now = 0;
            if (!e)
            {
                t_manual_loading = 1;
                int load_rc = load_index(db, idx, CUVS_RQ_LOAD);
                t_manual_loading = 0;
                if (load_rc != 0)
                {
                    int r = g_last_admit_reason;
                    pthread_mutex_unlock(&g_index_mutex);
                    if (t_reload_rej.reason != CUVS_ADMISSION_NONE)
                    {
                        send_admission_reject(client_fd, CUVS_STATUS_OOM_FALLBACK,
                            "not loaded: GPU memory admission refused", &t_reload_rej);
                        return;
                    }
                    if (r == CUVS_ADMISSION_AR5)
                        reply_msg(client_fd, CUVS_STATUS_ERROR,
                                  "not loaded: the indexes that could make room are pinned, in use, or still protected");
                    else if (r == CUVS_ADMISSION_NONE)
                        reply_msg(client_fd, CUVS_STATUS_ERROR,
                                  "not loaded: no usable index files for %u/%u", db, idx);
                    else
                        reply_msg(client_fd, CUVS_STATUS_ERROR,
                                  "not loaded: GPU memory is insufficient even after "
                                  "unloading every unpinned index");
                    return;
                }
                loaded_now = 1;
                e = find_index(db, idx);
            }
            if (cmd->op == CUVS_OP_LOAD)
            {
                pthread_mutex_unlock(&g_index_mutex);
                reply_msg(client_fd, CUVS_STATUS_OK, loaded_now ? "loaded" : "already resident");
                return;
            }
            if (e->pinned)
            {
                pthread_mutex_unlock(&g_index_mutex);
                reply_msg(client_fd, CUVS_STATUS_OK, "already pinned");
                return;
            }
            /* EV-6: per-GPU pinned total <= pin-max-pct of budget_eff. */
            for (int i = 0; i < n_usable_gpus(); i++)
            {
                int dev = usable_gpu(i);
                size_t mine = entry_bytes_on(e, dev);
                size_t beff = budget_eff_bytes(dev), pinned = 0;
                if (mine == 0 || beff == 0)
                    continue;
                for (int j = 0; j < g_n_indexes; j++)
                    if (g_indexes[j].valid && g_indexes[j].pinned)
                        pinned += entry_bytes_on(&g_indexes[j], dev);
                if (pinned + mine > beff / 100 * g_reclaim.pin_max_pct)
                {
                    pthread_mutex_unlock(&g_index_mutex);
                    reply_msg(client_fd, CUVS_STATUS_ERROR,
                              "pin limit: pinned bytes on GPU %d would exceed --pin-max-pct %u%%",
                              dev, g_reclaim.pin_max_pct);
                    return;
                }
            }
            e->pinned = 1;
            pthread_mutex_unlock(&g_index_mutex);
            reply_msg(client_fd, CUVS_STATUS_OK, loaded_now ? "loaded and pinned" : "pinned");
            return;
        }
        case CUVS_OP_UNPIN:
        default:
        {
            const char *m = !e ? "not resident" : (!e->pinned ? "not pinned" : "unpinned");
            if (e)
                e->pinned = 0;
            pthread_mutex_unlock(&g_index_mutex);
            reply_msg(client_fd, CUVS_STATUS_OK, "%s", m);
            return;
        }
    }
}

/* Runtime-settable knob table (index into g_reclaim_src/changed_at). */
static const char *k_rt_names[] = {
    "reclaim-mode", "reclaim-free-bytes", "evict-min-idle-s", "evict-hit-bonus-s",
    "evict-hit-cap", "reclaim-high-pct", "reclaim-low-pct", "reclaim-tick-s",
};
#define K_RT_N ((int)(sizeof(k_rt_names) / sizeof(k_rt_names[0])))

static int
rt_index(const char *name)
{
    for (int i = 0; i < K_RT_N; i++)
        if (strcmp(k_rt_names[i], name) == 0)
            return i;
    return -1;
}

static void
handle_reclaim_set(int client_fd, const CuvsCmdFrame *cmd)
{
    char name[sizeof(cmd->shm_key)], value[sizeof(cmd->filter_shm_key)], err[160];
    memcpy(name, cmd->shm_key, sizeof(name));
    name[sizeof(name) - 1] = '\0';
    memcpy(value, cmd->filter_shm_key, sizeof(value));
    value[sizeof(value) - 1] = '\0';
    pthread_mutex_lock(&g_index_mutex);
    if (cuvs_reclaim_runtime_set(&g_reclaim, name, value, err, sizeof(err)) != 0)
    {
        pthread_mutex_unlock(&g_index_mutex);
        reply_msg(client_fd, CUVS_STATUS_ERROR, "%s", err);
        return;
    }
    int k = rt_index(name);
    if (k >= 0)
    {
        snprintf(g_reclaim_src[k], sizeof(g_reclaim_src[k]), "function");
        g_reclaim_changed_at[k] = time(NULL);
    }
    pthread_mutex_unlock(&g_index_mutex);
    LOG_INFO("[reclaim_set] %s = %s (takes effect at the next reclaim tick)\n", name, value);
    reply_msg(client_fd, CUVS_STATUS_OK, "%s = %s", name, value);
}

/* Current value of a runtime knob as JSON. Caller holds the mutex. */
static void
rt_value_json(TextBuf *b, int k)
{
    const CuvsReclaimParams *p = &g_reclaim;
    switch (k)
    {
        case 0: tb_printf(b, "\"%s\"", cuvs_reclaim_mode_name(p->mode)); break;
        case 1: tb_printf(b, "%llu", (unsigned long long)p->f_fixed); break;
        case 2: tb_printf(b, "%lld", (long long)p->t_min_s); break;
        case 3: tb_printf(b, "%lld", (long long)p->bonus_s); break;
        case 4: tb_printf(b, "%u", p->hits_cap); break;
        case 5: tb_printf(b, "%u", p->hi_pct); break;
        case 6: tb_printf(b, "%u", p->lo_pct); break;
        default: tb_printf(b, "%lld", (long long)p->tick_s); break;
    }
}

static void
handle_reclaim_status(int client_fd)
{
    TextBuf b = {0};
    char scope[32];
    pthread_mutex_lock(&g_index_mutex);
    cuvs_format_evict_scope(&g_reclaim.scope, scope, sizeof(scope));
    tb_printf(&b, "{\"build_evict_scope\":\"%s\",\"pin_max_pct\":%u,"
                  "\"reclaim_free_max_pct\":%u,\"reclaim_free_window_min\":%u,"
                  "\"reclaim_floor_pct\":%u,\"load_soft_evict\":\"%s\","
                  "\"reindex_self_replace\":\"%s\",\"usage_stats_file\":\"%s\","
                  "\"rmm_peak_meter\":\"%s\",\"reclaim_params\":{",
              scope, g_reclaim.pin_max_pct, g_reclaim.f_cap_pct, g_reclaim.window_min,
              g_reclaim.floor_pct, g_reclaim.soft_evict ? "on" : "off",
              g_reclaim.self_replace ? "on" : "off", g_reclaim.usage_file ? "on" : "off",
              g_reclaim.peak_meter ? "on" : "off");
    for (int k = 0; k < K_RT_N; k++)
    {
        tb_printf(&b, "%s\"%s\":{\"value\":", k ? "," : "", k_rt_names[k]);
        rt_value_json(&b, k);
        tb_printf(&b, ",\"source\":\"%s\",\"changed_at\":",
                  g_reclaim_src[k][0] ? g_reclaim_src[k] : "default");
        if (g_reclaim_changed_at[k])
            tb_printf(&b, "%lld}", (long long)g_reclaim_changed_at[k]);
        else
            tb_printf(&b, "null}");
    }
    tb_printf(&b, "}}");
    pthread_mutex_unlock(&g_index_mutex);
    send_text(client_fd, &b);
}

static const char *
warmup_state_name(WarmupState w)
{
    switch (w)
    {
        case WARMUP_HOT:         return "hot";
        case WARMUP_COLD:        return "cold";
        case WARMUP_QUEUED:      return "queued";
        case WARMUP_DOWNLOADING: return "downloading";
        case WARMUP_LOADING:     return "loading";
        case WARMUP_FAILED:      return "failed";
    }
    return "?";
}

static void
index_status_row(TextBuf *b, uint32_t db, uint32_t idx, const char *state,
                 const char *kind, const IndexEntry *e, const char *warm)
{
    const EvictHistory *h = evict_history(db, idx, 0);
    CuvsEvictParams pp = evict_params_now();
    time_t now = time(NULL);
    long long protect_until = 0;
    int eligible = 0;
    if (e)
    {
        CuvsEvictCand c = entry_cand(e, entry_home_gpu(e));
        eligible = cuvs_evict_eligible(&c, (int64_t)now, &pp);
        if (e->last_search)
            protect_until = (long long)e->last_search +
                cuvs_evict_protect_s(e->hits, pp.t_min_s, pp.bonus_s, pp.hits_cap);
    }
    tb_printf(b, "%u\t%u\t%s\t%s\t%d\t%zu\t%lld\t%lld\t%u\t%d\t%d\t%lld\t%d\t%s\t%d\t%lld\t%s\t"
                 "%llu\t%lld\t%s\t%d\t%llu"
                 /* I3-E: dataset_attached = -1 (NULL). No wrapper API reads a
                  * loaded CAGRA handle's attach state yet (later stage). */
                 "\t-1\n",
              db, idx, state, kind,
              e ? entry_home_gpu(e) : -1,
              e ? entry_total_bytes(e) : (size_t)0,
              e ? (long long)e->last_search : 0LL,
              e ? (long long)e->loaded_at : 0LL,
              e ? e->hits : 0u,
              e ? e->pinned : 0,
              e ? e->inflight : 0,
              protect_until, eligible,
              cuvs_sidecar_kind_name(cuvs_entry_sidecar_kind(g_index_dir, db, idx)),
              e ? e->file_current : 0,
              e ? (long long)e->n_extended : 0LL,
              warm ? warm : "",
              h ? (unsigned long long)h->evict_count : 0ULL,
              h ? (long long)h->last_evict_at : 0LL,
              h && h->last_evict_reason >= 0
                  ? cuvs_evict_reason_name((CuvsEvictReason)h->last_evict_reason) : "",
              h ? h->last_build_self_replace : -1,
              h ? (unsigned long long)h->last_build_unloaded : 0ULL);
}

static const char *
entry_kind(const IndexEntry *e)
{
    if (e->shard_count >= 2) return "sharded";
    if (e->ivfpq_handle)     return "ivfpq";
    if (e->is_flat)          return "flat";
    return "cagra";
}

static int
key_known(uint32_t db, uint32_t idx)
{
    if (find_index(db, idx))
        return 1;
    for (int i = 0; i < g_n_cold_indexes; i++)
        if (g_cold_indexes[i].valid && g_cold_indexes[i].db_oid == db &&
            g_cold_indexes[i].index_oid == idx)
            return 1;
    return 0;
}

static void
handle_index_status(int client_fd)
{
    TextBuf b = {0};
    tb_printf(&b, "%s%s\n", CUVS_INDEX_STATUS_COLS, CUVS_INDEX_STATUS_COLS_I3E);
    pthread_mutex_lock(&g_index_mutex);
    for (int i = 0; i < g_n_indexes; i++)
    {
        const IndexEntry *e = &g_indexes[i];
        if (e->valid)
            index_status_row(&b, e->db_oid, e->index_oid, "resident", entry_kind(e), e,
                             warmup_state_name(e->warmup_state));
    }
    for (int i = 0; i < g_n_cold_indexes; i++)
    {
        const ColdIndexEntry *ce = &g_cold_indexes[i];
        if (ce->valid)
            index_status_row(&b, ce->db_oid, ce->index_oid, "cold", "cagra", NULL,
                             warmup_state_name(ce->warmup_state));
    }
    /* not_resident rows come from the index files (C8): .cagra, .shards,
     * .vectors, .ivfpq keys that are neither resident nor cold. The usage
     * stats file has a dot-name and never parses as a key. */
    DIR *dir = opendir(g_index_dir);
    if (dir)
    {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL)
        {
            uint32_t db, idx;
            char tail[16] = {0};
            if (sscanf(ent->d_name, "%u_%u.%15s", &db, &idx, tail) != 3)
                continue;
            const char *kind = strcmp(tail, "cagra") == 0 ? "cagra"
                             : strcmp(tail, "shards") == 0 ? "sharded"
                             : strcmp(tail, "ivfpq") == 0 ? "ivfpq"
                             : strcmp(tail, "vectors") == 0 ? "flat" : NULL;
            if (!kind || key_known(db, idx))
                continue;
            /* a cagra index also writes .vectors: report it once, as cagra */
            if (strcmp(kind, "flat") == 0)
            {
                char p[512];
                struct stat st;
                index_file_path(p, sizeof(p), g_index_dir, db, idx);
                if (stat(p, &st) == 0)
                    continue;
                shards_manifest_path(p, sizeof(p), g_index_dir, db, idx);
                if (stat(p, &st) == 0)
                    continue;
            }
            index_status_row(&b, db, idx, "not_resident", kind, NULL, "");
        }
        closedir(dir);
    }
    pthread_mutex_unlock(&g_index_mutex);
    send_text(client_fd, &b);
}

/* I3-E tail of one memory_status row (CUVS_MEMORY_STATUS_COLS_I3E order).
 * pool_used / pool_reserved are -1 (NULL) when the GPU has no RMM pool quota
 * or the pool cannot be read. Caller holds g_index_mutex. */
/* Byte counts go out as signed int64 ('l'/'n' kinds): a saturated
 * uint64 counter (cuvs_lw_sat_add) is clamped, not sent as a value the
 * backend refuses to parse. */
static long long
i3e_i64(uint64_t v)
{
    return v > (uint64_t)INT64_MAX ? (long long)INT64_MAX : (long long)v;
}

static void
memory_status_row_i3e(TextBuf *b, int dev)
{
    CuvsRmmPoolStats ps;
    int pool_ok = g_rmm_pool_limits[dev] > 0 && cuvs_rmm_pool_stats(dev, &ps) == 0;
    int last = g_last_reject_reason1[dev] - 1;
    tb_printf(b, "\t%zu\t%zu\t%lld\t%lld\t%zu\t%lld\t%d",
              g_max_vram_per_gpu[dev], g_rmm_pool_limits[dev],
              pool_ok ? (long long)ps.used_bytes : -1LL,
              pool_ok ? (long long)ps.reserved_bytes : -1LL,
              g_pending_rmm_build_vram[dev],
              i3e_i64(g_lw_pending[dev]),
              g_pending_build_count[dev]);
    for (int r = 0; r < CUVS_I3E_N_REJECT_REASONS; r++)
        tb_printf(b, "\t%lld", i3e_i64(g_rejects[dev][r]));
    tb_printf(b, "\t%s\t%lld\t%lld\t%lld\n",
              last >= 0 ? cuvs_admission_reason_name((CuvsAdmissionReason)last) : "",
              (long long)g_last_reject_at[dev],
              i3e_i64(g_managed_spill_builds[dev]),
              i3e_i64(g_managed_spill_bytes_max[dev]));
}

static void
handle_memory_status(int client_fd)
{
    TextBuf b = {0};
    tb_printf(&b, "%s%s\n", CUVS_MEMORY_STATUS_COLS, CUVS_MEMORY_STATUS_COLS_I3E);
    pthread_mutex_lock(&g_index_mutex);
    for (int i = 0; i < n_usable_gpus(); i++)
    {
        int dev = usable_gpu(i);
        size_t pinned = 0;
        int n_res = 0, n_pin = 0;
        uint64_t total_ev = 0;
        char fsrc[48];
        for (int j = 0; j < g_n_indexes; j++)
        {
            const IndexEntry *e = &g_indexes[j];
            size_t on = e->valid ? entry_bytes_on(e, dev) : 0;
            if (!e->valid || !entry_on_device(e, dev))
                continue;
            n_res++;
            if (e->pinned) { pinned += on; n_pin++; }
        }
        for (int r = 0; r < CUVS_EVICT__COUNT; r++)
            total_ev += g_evict_by_reason[dev][r];
        cuvs_format_free_src(&g_reclaim_f[dev], fsrc, sizeof(fsrc));
        CuvsRmmPoolStats ps;
        memset(&ps, 0, sizeof(ps));
        tb_printf(&b, "%d\t%zu\t%zu\t%zu\t%zu\t%d\t%d\t%s\t%llu\t%s\t%lld\t%s\t%llu\t%llu\t%llu\t"
                      "%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu",
                  dev, budget_eff_bytes(dev), resident_bytes(dev),
                  g_pending_build_vram[dev], pinned, n_res, n_pin,
                  cuvs_reclaim_mode_name(g_reclaim.mode),
                  g_reclaim.mode == CUVS_RECLAIM_HEADROOM
                      ? (unsigned long long)g_reclaim_f[dev].f_bytes : 0ULL,
                  g_reclaim.mode == CUVS_RECLAIM_HEADROOM ? fsrc : "none",
                  (long long)g_last_reclaim_at[dev],
                  cuvs_reclaim_trigger_name(g_last_reclaim_trigger[dev]),
                  (unsigned long long)g_reclaim_runs[dev],
                  (unsigned long long)g_reclaim_skip_sidecar[dev],
                  (unsigned long long)g_cache_persist_fail[dev],
                  (unsigned long long)g_evict_by_reason[dev][CUVS_EVICT_ADMIT_BUILD],
                  (unsigned long long)g_evict_by_reason[dev][CUVS_EVICT_RELOAD],
                  (unsigned long long)g_evict_by_reason[dev][CUVS_EVICT_SOFT],
                  (unsigned long long)g_evict_by_reason[dev][CUVS_EVICT_RECLAIM],
                  (unsigned long long)g_evict_by_reason[dev][CUVS_EVICT_MANUAL_UNLOAD],
                  (unsigned long long)g_evict_by_reason[dev][CUVS_EVICT_SLOT],
                  (unsigned long long)total_ev,
                  (unsigned long long)g_cache_drops,
                  (unsigned long long)g_history_dropped,
                  (unsigned long long)cuvs_rmm_pool_peak_bytes(dev));
        memory_status_row_i3e(&b, dev);   /* I3-E columns, then '\n' */
    }
    pthread_mutex_unlock(&g_index_mutex);
    send_text(client_fd, &b);
}

/* ---- reclaim thread (EV-5) ---- */

static uint64_t g_reclaim_ticks;
static pthread_t g_reclaim_tid;
static int      g_reclaim_started;
static int      g_reclaim_last_quiet_skips[CUVS_MAX_GPUS];

/* One reclaim pass on device_id. Caller holds g_index_mutex. */
static void
reclaim_gpu(int dev, time_t now)
{
    size_t beff = budget_eff_bytes(dev);
    int64_t window_s = (int64_t)g_reclaim.window_min * 60;
    cuvs_reclaim_free_target(&g_admit_log, dev, (int64_t)now, window_s, beff,
                             g_reclaim.f_cap_pct, g_reclaim.f_fixed, &g_reclaim_f[dev]);
    CuvsReclaimGoal g = { g_reclaim.mode, beff, g_reclaim.hi_pct, g_reclaim.lo_pct,
                          g_reclaim.floor_pct,
                          g_reclaim.mode == CUVS_RECLAIM_HEADROOM ? g_reclaim_f[dev].f_bytes : 0 };
    size_t resident = resident_bytes(dev), before = resident;
    CuvsReclaimTrigger trig = cuvs_reclaim_trigger(&g, resident);
    if (trig == CUVS_TRIG_NONE || beff == 0)
        return;   /* below the lines: not a run (r13 13.2-N7) */

    g_reclaim_runs[dev]++;
    g_last_reclaim_at[dev] = now;
    g_last_reclaim_trigger[dev] = trig;

    int n = 0, evicted = 0, skipped = 0;
    CuvsEvictCand *c = calloc((size_t)(g_n_indexes > 0 ? g_n_indexes : 1), sizeof(*c));
    int *order = calloc((size_t)(g_n_indexes > 0 ? g_n_indexes : 1), sizeof(int));
    EvictSkip *keys = calloc((size_t)(g_n_indexes > 0 ? g_n_indexes : 1), sizeof(EvictSkip));
    if (!c || !order || !keys)
    {
        free(c); free(order); free(keys);
        return;
    }
    for (int i = 0; i < g_n_indexes; i++)
    {
        IndexEntry *e = &g_indexes[i];
        if (!e->valid || !entry_on_device(e, dev))
            continue;
        c[n] = entry_cand(e, dev);
        keys[n].db_oid = e->db_oid;
        keys[n].index_oid = e->index_oid;
        n++;
    }
    CuvsEvictParams pp = evict_params_now();
    int k = cuvs_reclaim_plan(c, n, &pp, (int64_t)now, order);
    for (int j = 0; j < k && cuvs_reclaim_need_more(&g, trig, resident); j++)
    {
        const CuvsEvictCand *v = &c[order[j]];
        if (!cuvs_reclaim_take(&g, trig, resident, v->bytes))
            continue;
        IndexEntry *e = find_index(keys[order[j]].db_oid, keys[order[j]].index_oid);
        if (!e)
            continue;
        /* Sidecar exclusion (track-split decision 4, UC-1 7b), decided under
         * the lock right before the save from the files themselves. */
        if (cuvs_reclaim_skip_sidecar(cuvs_entry_sidecar_kind(g_index_dir, e->db_oid,
                                                              e->index_oid),
                                      entry_is_cagra_single(e), e->file_current))
        {
            skipped++;
            continue;
        }
        int pf = 0;
        size_t freed = evict_entry(e, dev, CUVS_EVICT_RECLAIM, 0, CUVS_RQ_RECLAIM, &pf);
        if (pf)
            continue;   /* persist failure: skip this victim (persist_failures) */
        resident = freed > resident ? 0 : resident - freed;
        evicted++;
    }
    free(c); free(order); free(keys);

    g_reclaim_skip_sidecar[dev] += (uint64_t)skipped;
    /* Repeat suppression: an all-sidecar tick that changes nothing logs on
     * the first tick, every 12th tick, and whenever the skipped set size
     * changes; the counter above still counts every tick. */
    int quiet = evicted == 0 && skipped > 0 && skipped == g_reclaim_last_quiet_skips[dev];
    g_reclaim_last_quiet_skips[dev] = evicted == 0 ? skipped : 0;
    if (!quiet || g_reclaim_ticks % 12 == 0)
    {
        char fsrc[48];
        cuvs_format_free_src(&g_reclaim_f[dev], fsrc, sizeof(fsrc));
        /* ADR-086 [reclaim] schema (13): op gpu tick resident_before
         * resident_after hi_bytes lo_bytes evicted skip_sidecar trigger
         * f_bytes f_src mono_us. */
        LOG_INFO("[reclaim] op=%s gpu=%d tick=%llu resident_before=%zu resident_after=%zu "
                 "hi_bytes=%zu lo_bytes=%zu evicted=%d skip_sidecar=%d trigger=%s "
                 "f_bytes=%llu f_src=%s mono_us=%llu\n",
                 cuvs_reclaim_mode_name(g.mode), dev, (unsigned long long)g_reclaim_ticks,
                 before, resident, beff / 100 * g.hi_pct, beff / 100 * g.lo_pct,
                 evicted, skipped, cuvs_reclaim_trigger_name(trig),
                 (unsigned long long)g.f_bytes, g.f_bytes ? fsrc : "none",
                 (unsigned long long)mono_us());
    }
}

/* Snapshot of the registry for the usage stats file (no lock while writing). */
static void
usage_stats_write(void)
{
    CuvsUsageStat *s;
    int n = 0;
    pthread_mutex_lock(&g_index_mutex);
    s = calloc((size_t)(g_n_indexes > 0 ? g_n_indexes : 1), sizeof(*s));
    if (s)
        for (int i = 0; i < g_n_indexes && n < CUVS_USAGE_STATS_MAX; i++)
            if (g_indexes[i].valid)
            {
                s[n].db_oid = g_indexes[i].db_oid;
                s[n].index_oid = g_indexes[i].index_oid;
                s[n].last_use = (int64_t)g_indexes[i].last_search;
                s[n].hits = g_indexes[i].hits;
                s[n].pinned = g_indexes[i].pinned;
                n++;
            }
    pthread_mutex_unlock(&g_index_mutex);
    if (!s)
        return;
    size_t cap = (size_t)n * 64 + 128;
    char *buf = malloc(cap);
    long len = buf ? cuvs_usage_stats_format(s, n, buf, cap) : -1;
    free(s);
    if (len < 0)
    {
        free(buf);
        return;
    }
    char tmp[512], fin[512];
    usage_stats_path(tmp, sizeof(tmp), 1);
    usage_stats_path(fin, sizeof(fin), 0);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
    {
        free(buf);
        return;
    }
    ssize_t w = write(fd, buf, (size_t)len);
    int ok = w == len && fsync(fd) == 0;
    close(fd);
    free(buf);
    /* rename is atomic: readers see the old complete file or the new one. */
    if (!ok || rename(tmp, fin) != 0)
        unlink(tmp);
}

static void *
reclaim_thread(void *arg)
{
    (void) arg;
    while (!g_shutdown)
    {
        int64_t tick;
        pthread_mutex_lock(&g_index_mutex);
        tick = g_reclaim.tick_s;
        pthread_mutex_unlock(&g_index_mutex);
        for (int64_t t = 0; t < tick && !g_shutdown; t++)
            sleep(1);
        if (g_shutdown)
            break;
        time_t now = time(NULL);
        pthread_mutex_lock(&g_index_mutex);
        g_reclaim_ticks++;
        cuvs_admit_log_expire(&g_admit_log, (int64_t)now,
                              (int64_t)g_reclaim.window_min * 60);
        if (g_reclaim.mode != CUVS_RECLAIM_OFF)
            for (int i = 0; i < n_usable_gpus(); i++)
                reclaim_gpu(usable_gpu(i), now);
        pthread_mutex_unlock(&g_index_mutex);
        /* UC-1 4c: written every tick whatever the mode (CK-24). */
        if (g_reclaim.usage_file)
            usage_stats_write();
    }
    return NULL;
}

static void *
connection_thread(void *arg)
{
    int client_fd = *(int *)arg;
    free(arg);
    clear_build_notices();

#ifdef __linux__
    /* #87 finding 1: latch the kernel-verified peer uid for this connection's
     * lifetime before touching any shm the peer named. Fail closed — a
     * connection whose credentials we can't read gets no service. */
    {
        struct ucred cred;
        socklen_t    cred_len = sizeof(cred);
        if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0)
        {
            LOG_ERROR("pg_cuvs_server: SO_PEERCRED failed (errno=%d); refusing connection (#87)\n", errno);
            close(client_fd);
            return NULL;
        }
        g_peer_uid = cred.uid;
    }
#endif

    LOG_DEBUG("pg_cuvs_server: client connected (fd=%d)\n", client_fd);
    fflush(stderr);

    CuvsCmdFrame cmd;

    /* #77: read the protocol prologue (magic + version) first, then the rest.
     * An extension built from an older header sends a SHORTER frame, so reading
     * sizeof(cmd) in one call would block forever on bytes that never arrive.
     * Validating the prologue turns a wire-protocol skew into an immediate,
     * named error instead of a silent struct misinterpretation. */
    const size_t prologue = offsetof(CuvsCmdFrame, op);

    if (recv_all(client_fd, &cmd, prologue) < 0)
    {
        LOG_ERROR("pg_cuvs_server: recv_all CMD prologue failed (errno=%d)\n", errno);
        fflush(stderr);
        close(client_fd);
        return NULL;
    }

    if (cmd.proto_magic != CUVS_PROTO_MAGIC
        || cmd.proto_version != CUVS_PROTO_VERSION)
    {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "IPC protocol mismatch: daemon speaks magic 0x%08x v%u, "
                 "client sent 0x%08x v%u; co-deploy extension and daemon",
                 CUVS_PROTO_MAGIC, CUVS_PROTO_VERSION,
                 cmd.proto_magic, cmd.proto_version);
        LOG_ERROR("pg_cuvs_server: %s\n", msg);
        fflush(stderr);
        send_error_code(client_fd, CUVS_STATUS_PROTO_MISMATCH, msg);
        close(client_fd);
        return NULL;
    }

    if (recv_all(client_fd, (char *)&cmd + prologue,
                 sizeof(cmd) - prologue) < 0)
    {
        LOG_ERROR("pg_cuvs_server: recv_all CMD body failed (errno=%d)\n", errno);
        fflush(stderr);
        close(client_fd);
        return NULL;
    }

    LOG_DEBUG("pg_cuvs_server: received cmd op=%u db=%u idx=%u dim=%u n_vecs=%lld shm=%s\n",
            cmd.op, cmd.db_oid, cmd.index_oid, cmd.dim, (long long)cmd.n_vecs, cmd.shm_key);
    fflush(stderr);

    switch (cmd.op)
    {
        case CUVS_OP_SEARCH:
            handle_search(client_fd, &cmd);
            break;
        case CUVS_OP_SEARCH_BATCH:
            handle_search_batch(client_fd, &cmd);
            break;
        case CUVS_OP_BUILD_HNSW_FILE:
            handle_build_hnsw_file(client_fd, &cmd);
            break;
        case CUVS_OP_HNSW_ROUTE_PREFLIGHT:
            handle_hnsw_route_preflight(client_fd);
            break;
        case CUVS_OP_CAGRA_AR0_PREFLIGHT:
        {
            /* Static startup values; same pair as pick_gpu_for_index. */
            CuvsReplyHeader hdr = {.status = CUVS_STATUS_OK, .n_results = 1};
            CuvsCagraPreflightReply ar0 = {.version = CUVS_CAGRA_PREFLIGHT_VERSION};
            int unlimited = 0;
            for (int i = 0; i < n_usable_gpus(); i++)
            {
                int dev = usable_gpu(i);
                uint64_t limit = g_max_vram_per_gpu[dev];
                if (g_rmm_pool_limits[dev] > 0 && (limit == 0 || g_rmm_pool_limits[dev] < limit))
                    limit = g_rmm_pool_limits[dev];
                if (limit == 0) unlimited = 1;
                if (limit > ar0.max_limit) ar0.max_limit = limit;
                ar0.n_devices++;
            }
            if (unlimited || ar0.n_devices == 0) ar0.max_limit = 0;
            send_all(client_fd, &hdr, sizeof(hdr));
            send_all(client_fd, &ar0, sizeof(ar0));
            break;
        }
        case CUVS_OP_BUILD:
            handle_build(client_fd, &cmd);
            break;
        case CUVS_OP_STATUS:
            handle_stats(client_fd, &cmd);
            break;
        case CUVS_OP_MARK_STALE:
            handle_mark_stale(client_fd, &cmd);
            break;
        case CUVS_OP_CACHE_STATS:
            handle_cache_stats(client_fd);
            break;
        case CUVS_OP_SERVER_SETTINGS:
        {
            CuvsReplyHeader hdr = {.status = CUVS_STATUS_OK, .n_results = 1};
            CuvsServerSettings settings = {.rmm_pool_mb = g_rmm_pool_mb};
            send_all(client_fd, &hdr, sizeof(hdr));
            send_all(client_fd, &settings, sizeof(settings));
            break;
        }
        case CUVS_OP_RMM_POOL_STATUS:
        {
            CuvsReplyHeader hdr = {.status = CUVS_STATUS_OK, .n_results = 1};
            CuvsRmmPoolStatus status = {
                .version = CUVS_RMM_POOL_STATUS_VERSION,
                .initial_mb = g_rmm_pool_mb,
                .maximum_percent = g_rmm_pool_mb > 0 ? g_rmm_pool_max_percent : 0,
                .scratch_permille = g_scratch_permille,
                .hnsw_auto_overflow = g_hnsw_auto_overflow
            };
            if (g_rmm_pool_mb > 0)
                for (int i = 0; i < n_usable_gpus(); i++)
                {
                    if (cuvs_rmm_pool_stats(usable_gpu(i), &status.devices[i]) == 0)
                    {
                        pthread_mutex_lock(&g_index_mutex);
                        status.devices[i].solo = g_pending_build_count[usable_gpu(i)] <= 1;
                        pthread_mutex_unlock(&g_index_mutex);
                    }
                    else
                    {
                        hdr.status = CUVS_STATUS_UNAVAILABLE;
                        hdr.n_results = 0;
                        break;
                    }
                    status.n_devices++;
                }
            if (hdr.status == CUVS_STATUS_OK)
                lw_fill_pool_status(&status);
            if (hdr.status == CUVS_STATUS_OK)
                i3e_fill_pool_status(&status);   /* I3-E append */
            send_all(client_fd, &hdr, sizeof(hdr));
            if (hdr.status == CUVS_STATUS_OK)
                send_all(client_fd, &status, sizeof(status));
            break;
        }
        case CUVS_OP_SHARD_STATS:
            handle_shard_stats(client_fd, &cmd);
            break;
        case CUVS_OP_DROP_INDEX:
            handle_drop(client_fd, &cmd);
            break;
        case CUVS_OP_EXPORT_ADJACENCY:
            handle_export_adjacency(client_fd, &cmd);
            break;
        case CUVS_OP_EXPORT_HNSW_SHM:
            handle_export_hnsw_shm(client_fd, &cmd);
            break;
        case CUVS_OP_BUILD_IVFPQ:
            handle_build_ivfpq(client_fd, &cmd);
            break;
        case CUVS_OP_BUILD_FLAT:
            handle_build_flat(client_fd, &cmd);   /* ADR-073 */
            break;
        case CUVS_OP_SEARCH_IVFPQ:
            handle_search_ivfpq(client_fd, &cmd);
            break;
        case CUVS_OP_EXTEND:
            handle_extend(client_fd, &cmd);
            break;
        case CUVS_OP_COMPACT:
            handle_compact(client_fd, &cmd);
            break;
        case CUVS_OP_SET_VRAM_BUDGET:
            handle_set_vram_budget(client_fd, &cmd);
            break;
        case CUVS_OP_EAT_VRAM:
            handle_eat_vram(client_fd, &cmd);
            break;
        case CUVS_OP_FREE_VRAM:
            handle_free_vram(client_fd, &cmd);
            break;
#ifdef CUVS_TEST_HOOKS
        case CUVS_OP_INJECT_EXTEND_OOM:
            handle_inject_extend_oom(client_fd, &cmd);
            break;
        case CUVS_OP_INJECT_BUILD_OOM:
            handle_inject_build_oom(client_fd, &cmd);
            break;
#endif /* CUVS_TEST_HOOKS */
        case CUVS_OP_SEARCH_STREAM_BF:
            handle_search_stream_bf(client_fd, &cmd);
            break;
        case CUVS_OP_SEARCH_BF_TRANSIENT:
            handle_search_bf_transient(client_fd, &cmd);
            break;
        case CUVS_OP_UNLOAD:
        case CUVS_OP_LOAD:
        case CUVS_OP_PIN:
        case CUVS_OP_UNPIN:
            handle_manual(client_fd, &cmd);
            break;
        case CUVS_OP_RECLAIM_SET:
            handle_reclaim_set(client_fd, &cmd);
            break;
        case CUVS_OP_INDEX_STATUS:
            handle_index_status(client_fd);
            break;
        case CUVS_OP_MEMORY_STATUS:
            handle_memory_status(client_fd);
            break;
        case CUVS_OP_RECLAIM_STATUS:
            handle_reclaim_status(client_fd);
            break;
        default:
            send_error(client_fd, "unknown op");
            break;
    }

    close(client_fd);
    return NULL;
}

/* ----------------------------------------------------------------
 * Signal handling — async-signal-safe flag + deferred graceful shutdown
 *
 * sigterm_handler does the minimum legal in a signal handler: set the
 * volatile flag. All heavy work (mutex, CUDA serialize, file I/O) is
 * deferred to graceful_shutdown(), run from the main thread after the
 * accept loop is interrupted. This avoids deadlock (the handler could
 * fire while a connection thread holds g_index_mutex) and undefined
 * behavior from non-async-signal-safe calls.
 * ---------------------------------------------------------------- */
/* Phase 3L-9: mark every queued request done with `status` and wake its
 * producer. Caller holds g_batch_mtx. */
static void
batch_fail_all_locked(int status)
{
    for (int i = 0; i < g_batch_queue_n; i++)
    {
        g_batch_queue[i]->status = status;
        g_batch_queue[i]->done   = 1;
    }
    g_batch_queue_n = 0;
    pthread_cond_broadcast(&g_batch_done_cond);
}

/* Phase 3L-9: run one coalesced group (requests in `batch` with gid[i]==g, all
 * sharing a (db,index,precision,dim) key) as a single cuvs_bf_search_batch
 * dispatch. Acquires g_index_mutex for the GPU work (eviction-safe, mirrors the
 * immediate path); the caller must NOT hold g_batch_mtx here. Writes each member's
 * out/n_out/status (but not `done` — the caller sets that after all groups). */
static void
bf_batch_run_group(CuvsBatchRequest **batch, const int *gid, int n, int g)
{
    int idx[CUVS_BATCH_MAX], Q = 0;
    for (int i = 0; i < n; i++)
        if (gid[i] == g)
            idx[Q++] = i;
    if (Q == 0)
        return;

    CuvsBatchRequest *r0   = batch[idx[0]];
    int            dim  = (int) r0->key.dim;
    int            maxk = 0;
    for (int i = 0; i < Q; i++)
        if (batch[idx[i]]->k > maxk)
            maxk = batch[idx[i]]->k;

    pthread_mutex_lock(&g_index_mutex);
    IndexEntry *e = find_index(r0->key.db_oid, r0->key.index_oid);
    if (!e && load_index(r0->key.db_oid, r0->key.index_oid, CUVS_RQ_LOAD) == 0)
        e = find_index(r0->key.db_oid, r0->key.index_oid);

    int status = CUVS_STATUS_OK;
    if (!e)
        status = CUVS_STATUS_NOT_FOUND;
    else if (e->stale)
        status = CUVS_STATUS_STALE;
    else
    {
        refresh_main_bf_cache(e, r0->key.precision);
        if (!e->main_bf_idx)
            status = CUVS_STATUS_NO_VECTORS;
    }
    if (status != CUVS_STATUS_OK)
    {
        if (e)
            record_search_stat(e, status, 0, NULL);
        pthread_mutex_unlock(&g_index_mutex);
        for (int i = 0; i < Q; i++) { batch[idx[i]]->status = status; batch[idx[i]]->n_out = 0; }
        return;
    }

    int K = (int) ((int64_t) maxk < e->n_vecs ? (int64_t) maxk : e->n_vecs);
    float            *queries = malloc((size_t) Q * (size_t) dim * sizeof(float));
    CuvsSearchResult *raw     = malloc((size_t) Q * (size_t) K * sizeof(CuvsSearchResult));
    if (!queries || !raw)
    {
        free(queries); free(raw);
        pthread_mutex_unlock(&g_index_mutex);
        for (int i = 0; i < Q; i++) { batch[idx[i]]->status = CUVS_STATUS_OOM_FALLBACK; batch[idx[i]]->n_out = 0; }
        return;
    }
    for (int i = 0; i < Q; i++)
        memcpy(queries + (size_t) i * dim, batch[idx[i]]->query, (size_t) dim * sizeof(float));

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int ret = cuvs_bf_search_batch(e->main_bf_idx, queries, Q, dim, K, raw, delta_gpu_of(e));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint32_t latency_us = (uint32_t) ((t1.tv_sec - t0.tv_sec) * 1000000 +
                                      (t1.tv_nsec - t0.tv_nsec) / 1000);

    if (ret != 0)
    {
        record_search_stat(e, CUVS_STATUS_OOM_FALLBACK, 0, NULL);
        pthread_mutex_unlock(&g_index_mutex);
        free(queries); free(raw);
        for (int i = 0; i < Q; i++) { batch[idx[i]]->status = CUVS_STATUS_OOM_FALLBACK; batch[idx[i]]->n_out = 0; }
        return;
    }

    for (int i = 0; i < Q; i++)
    {
        CuvsBatchRequest *r  = batch[idx[i]];
        int            ki = (r->k < K) ? r->k : K;
        int            nv = 0;
        for (int j = 0; j < ki; j++)
        {
            int64_t id = raw[(size_t) i * K + j].item_id;
            if (id < 0 || id >= e->n_vecs) continue;
            r->out[nv].tid      = e->tids[id];
            r->out[nv].distance = raw[(size_t) i * K + j].distance;
            nv++;
        }
        r->n_out  = nv;
        r->status = CUVS_STATUS_OK;
        record_search_stat(e, CUVS_STATUS_OK, latency_us, NULL);   /* per request */
    }
    e->bf_batch_count++;            /* one coalesced GPU dispatch served Q requests */
    e->last_search_mode = 3;        /* gpu_bf */
    e->last_requested_k = (uint32_t) r0->k;
    e->last_returned_k  = (uint32_t) batch[idx[0]]->n_out;
    pthread_mutex_unlock(&g_index_mutex);
    free(queries);
    free(raw);
}

/* #160: run one coalesced CAGRA group (requests in `batch` with gid[i]==g, all
 * sharing a (db,index,dim,mode=cagra) key) as a single cuvs_cagra_search_batch
 * dispatch. Mirrors bf_batch_run_group: acquires g_index_mutex for the GPU work
 * (the caller must NOT hold g_batch_mtx), writes each member's
 * out/n_out/status/delta_merged/latency_us but not `done`.
 *
 * Requests may ask for different k, so the dispatch uses max(k) and each
 * request reads back only its own prefix — the batch kernel writes results
 * row-major [q*maxk + j] with TAIL CONTRACT padding beyond what the corpus
 * could fill, and cagra_finish_one drops the padding by id range.
 *
 * The .delta is folded in exactly as the immediate path does, via one extra
 * batched brute-force dispatch over the resident delta cache shared by the
 * whole group (not one per request), then cagra_finish_one merges per request.
 * All per-request post-processing — delta merge, id-range filtering, metric-
 * aware ordering, per-request stat recording — goes through the same helper the
 * immediate path uses, so the batched answer is the immediate answer. */
static void
cagra_batch_run_group(CuvsBatchRequest **batch, const int *gid, int n, int g)
{
    int idx[CUVS_BATCH_MAX], Q = 0;
    for (int i = 0; i < n; i++)
        if (gid[i] == g)
            idx[Q++] = i;
    if (Q == 0)
        return;

    CuvsBatchRequest *r0   = batch[idx[0]];
    int               dim  = (int) r0->key.dim;
    int               maxk = 0;
    for (int i = 0; i < Q; i++)
        if (batch[idx[i]]->k > maxk)
            maxk = batch[idx[i]]->k;

    pthread_mutex_lock(&g_index_mutex);
    IndexEntry *e = find_index(r0->key.db_oid, r0->key.index_oid);
    if (!e && load_index(r0->key.db_oid, r0->key.index_oid, CUVS_RQ_LOAD) == 0)
        e = find_index(r0->key.db_oid, r0->key.index_oid);

    int status = CUVS_STATUS_OK;
    if (!e)
        status = CUVS_STATUS_NOT_FOUND;
    else if (e->stale)
        status = CUVS_STATUS_STALE;
    else if (e->handle == NULL)
        status = CUVS_STATUS_NO_VECTORS;   /* ADR-073: flat/BF-only index */
    if (status != CUVS_STATUS_OK)
    {
        if (e)
            record_search_stat(e, status, 0, NULL);
        pthread_mutex_unlock(&g_index_mutex);
        for (int i = 0; i < Q; i++) { batch[idx[i]]->status = status; batch[idx[i]]->n_out = 0; }
        return;
    }

    float            *queries = malloc((size_t) Q * (size_t) dim * sizeof(float));
    CuvsSearchResult *raw     = malloc((size_t) Q * (size_t) maxk * sizeof(CuvsSearchResult));
    if (!queries || !raw)
    {
        free(queries); free(raw);
        pthread_mutex_unlock(&g_index_mutex);
        for (int i = 0; i < Q; i++) { batch[idx[i]]->status = CUVS_STATUS_OOM_FALLBACK; batch[idx[i]]->n_out = 0; }
        return;
    }
    for (int i = 0; i < Q; i++)
        memcpy(queries + (size_t) i * dim, batch[idx[i]]->query, (size_t) dim * sizeof(float));

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int ret = cuvs_cagra_search_batch(e->handle, queries, Q, dim, maxk, raw,
                                      e->gpu_device_id, r0->key.itopk_size,
                                      r0->key.search_width);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint32_t latency_us = (uint32_t) ((t1.tv_sec - t0.tv_sec) * 1000000 +
                                      (t1.tv_nsec - t0.tv_nsec) / 1000);

    if (ret != 0)
    {
        record_search_stat(e, CUVS_STATUS_OOM_FALLBACK, 0, NULL);
        pthread_mutex_unlock(&g_index_mutex);
        free(queries); free(raw);
        for (int i = 0; i < Q; i++) { batch[idx[i]]->status = CUVS_STATUS_OOM_FALLBACK; batch[idx[i]]->n_out = 0; }
        return;
    }

    /* Phase 3B delta, batched: one dispatch for the group instead of one per
     * request. A failure here is not fatal — the group falls back to base-only
     * results with delta_merged=0, the same fail-open the immediate path has. */
    CuvsSearchResult *draw = NULL;
    int               n_dc = 0;
    refresh_delta_cache(e);
    if (e->delta_idx && e->n_delta > 0)
    {
        n_dc = (int) ((int64_t) maxk < e->n_delta ? (int64_t) maxk : e->n_delta);
        draw = malloc((size_t) Q * (size_t) n_dc * sizeof(CuvsSearchResult));
        if (!draw
            || cuvs_bf_search_batch(e->delta_idx, queries, Q, dim, n_dc, draw,
                                    e->gpu_device_id) != 0)
        {
            free(draw);
            draw = NULL;
            n_dc = 0;
        }
    }

    for (int i = 0; i < Q; i++)
    {
        CuvsBatchRequest *r = batch[idx[i]];
        /* This request's delta depth is min(its k, n_delta) — what the
         * immediate path would have used for the same k. */
        int di = (n_dc < r->k) ? n_dc : r->k;
        int dm = 0;

        r->n_out = cagra_finish_one(e, raw + (size_t) i * maxk, r->k,
                                    draw ? draw + (size_t) i * n_dc : NULL, di,
                                    r->out, &dm);
        r->delta_merged = dm;
        r->latency_us   = latency_us;
        r->status       = CUVS_STATUS_OK;
        record_search_stat(e, CUVS_STATUS_OK, latency_us, NULL);   /* per request */
    }
    e->cagra_batch_count++;         /* one coalesced GPU dispatch served Q requests */
    e->last_search_mode = 0;        /* gpu_cagra */
    e->last_requested_k = (uint32_t) r0->k;
    e->last_returned_k  = (uint32_t) batch[idx[0]]->n_out;
    pthread_mutex_unlock(&g_index_mutex);
    free(queries);
    free(raw);
    free(draw);
}

/* Phase 3L-9 / #160: process the currently queued requests. Caller holds
 * g_batch_mtx; returns holding it. Snapshots + clears the queue (so producers
 * can fill the next batch), releases g_batch_mtx, groups by key, runs one
 * batched dispatch per group — cuvs_bf_search_batch or cuvs_cagra_search_batch
 * depending on the group's mode, which the key already separated — then
 * re-locks and wakes every producer in this batch. */
static void
batch_process_locked(void)
{
    int n = g_batch_queue_n;
    if (n == 0)
        return;

    CuvsBatchRequest *batch[CUVS_BATCH_MAX];
    for (int i = 0; i < n; i++)
        batch[i] = g_batch_queue[i];
    g_batch_queue_n = 0;
    pthread_mutex_unlock(&g_batch_mtx);

    CuvsBatchKey keys[CUVS_BATCH_MAX];
    int       gid[CUVS_BATCH_MAX];
    int       ng = 0;
    for (int i = 0; i < n; i++)
        keys[i] = batch[i]->key;
    cuvs_batch_group(keys, n, gid, &ng);

    for (int g = 0; g < ng; g++)
    {
        /* Every member of a group shares the key, so the first member's mode
         * decides the kernel for the whole group. */
        int gm = 1;
        for (int i = 0; i < n; i++)
            if (gid[i] == g) { gm = (int) batch[i]->key.mode; break; }
        if (gm == 0)
            cagra_batch_run_group(batch, gid, n, g);
        else
            bf_batch_run_group(batch, gid, n, g);
    }

    pthread_mutex_lock(&g_batch_mtx);
    for (int i = 0; i < n; i++)
        batch[i]->done = 1;
    pthread_cond_broadcast(&g_batch_done_cond);
}

/* Phase 3L-9 / #160: the single micro-batch consumer thread, shared by both
 * algorithms. Idle (1s poll) until a request is enqueued or shutdown. On the
 * first queued request it waits that request's window (lock released, so more
 * accumulate) then coalesces everything queued into one batch. */
static void *
batch_worker_thread(void *arg)
{
    (void) arg;
    pthread_mutex_lock(&g_batch_mtx);
    while (!g_shutdown)
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;                 /* 1s poll so shutdown stays responsive */
        while (g_batch_queue_n == 0 && !g_shutdown)
            if (pthread_cond_timedwait(&g_batch_cond, &g_batch_mtx, &ts) == ETIMEDOUT)
                break;
        if (g_shutdown)
            break;
        if (g_batch_queue_n == 0)
            continue;

        /* Accumulation window: let concurrent requests pile up before the GPU
         * dispatch. Released lock during the sleep so producers can enqueue. */
        uint32_t window = g_batch_queue[0]->wait_us;
        if (window > 10000) window = 10000;   /* GUC max; defensive */
        if (window > 0)
        {
            /* window us -> ns; window<=10000 keeps tv_nsec well under 1e9. */
            struct timespec ws = { 0, (long) window * 1000 };
            pthread_mutex_unlock(&g_batch_mtx);
            nanosleep(&ws, NULL);
            pthread_mutex_lock(&g_batch_mtx);
        }
        batch_process_locked();
    }
    /* Shutdown: fail any still-queued requests so producers never hang. */
    batch_fail_all_locked(CUVS_STATUS_UNAVAILABLE);
    pthread_mutex_unlock(&g_batch_mtx);
    return NULL;
}

static void
sigterm_handler(int sig)
{
    (void)sig;
    g_shutdown = 1;
}

/* Graceful shutdown — main-thread context, safe to use mutex/CUDA/file I/O.
 * Locking g_index_mutex waits for any in-flight connection thread to finish
 * its critical section before we serialize. Does NOT call exit(); the caller
 * returns from main() normally. */
static void
graceful_shutdown(void)
{
    LOG_INFO("pg_cuvs_server: SIGTERM received, serializing indexes...\n");

    /* I3-B: stop the reclaim thread before the final saves so no tick
     * evicts concurrently with them (it checks g_shutdown every second). */
    if (g_reclaim_started)
    {
        pthread_join(g_reclaim_tid, NULL);
        g_reclaim_started = 0;
    }

    /* Phase 3L-9: wake the batch worker (g_shutdown is already set) so it
     * fails any queued requests and exits, then join it. Always spawned. */
    if (g_batch_worker_started)
    {
        pthread_mutex_lock(&g_batch_mtx);
        pthread_cond_broadcast(&g_batch_cond);
        pthread_mutex_unlock(&g_batch_mtx);
        pthread_join(g_batch_worker_tid, NULL);
        LOG_INFO("batch: worker thread joined\n");
    }

    /* Phase 3D: signal warmup workers to exit and wait for them. */
    if (g_snapshot_uri[0] != '\0')
    {
        pthread_mutex_lock(&g_warmup_mutex);
        pthread_cond_broadcast(&g_warmup_cond);
        pthread_mutex_unlock(&g_warmup_mutex);

        for (int i = 0; i < g_warmup_nthreads; i++)
            pthread_join(g_warmup_tids[i], NULL);
        LOG_INFO("warmup: all worker threads joined\n");
    }

    pthread_mutex_lock(&g_index_mutex);
    int saved = 0, failed = 0;
    for (int i = 0; i < g_n_indexes; i++)
    {
        if (!g_indexes[i].valid)
            continue;
        if (save_index(&g_indexes[i]) == 0) {
            saved++;
        } else {
            failed++;
            LOG_ERROR("sigterm: save_index FAILED for %u/%u "
                      "(operator must REINDEX after restart)\n",
                    g_indexes[i].db_oid, g_indexes[i].index_oid);
        }
    }
    pthread_mutex_unlock(&g_index_mutex);
    LOG_INFO("sigterm: %d indexes saved, %d failed\n", saved, failed);

    if (g_server_fd >= 0)
    {
        close(g_server_fd);
        unlink(g_socket_path);
    }

    LOG_INFO("pg_cuvs_server: shutdown complete\n");
}

/* ----------------------------------------------------------------
 * ADR-075 Phase 1: probe hardware constants once at boot and write the GLOBAL
 * cuvs_hw_profile (CRC) + cuvs_daemon_identity (text) sidecars atomically. The
 * planner reads these cheaply; failure is non-fatal (planner falls back to
 * compiled DEFAULTs). NOT consumed by any cost decision yet (Phase 2).
 * ---------------------------------------------------------------- */
/* ADR-075 Phase 2: host-side coefficient probes (no CUDA). Both best-effort and
 * return -1 on failure so the caller leaves the field at DEFAULT with its bit
 * clear (→ planner falls back to the legacy cost path for that coefficient). */

/* Local AF_UNIX round-trip latency floor (microseconds) — the IPC cost a
 * backend<->daemon request pays before any work. */
static double
probe_ipc_rtt_us(void)
{
    int    sv[2];
    char   b = 0;
    double best = 1e30;
    int    i;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return -1.0;
    /* warm */
    if (write(sv[0], &b, 1) == 1 && read(sv[1], &b, 1) == 1 &&
        write(sv[1], &b, 1) == 1 && read(sv[0], &b, 1) == 1) { /* ok */ }
    for (i = 0; i < 50; i++)
    {
        struct timespec t0, t1;
        double us;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (write(sv[0], &b, 1) != 1 || read(sv[1], &b, 1) != 1 ||
            write(sv[1], &b, 1) != 1 || read(sv[0], &b, 1) != 1) { best = 1e30; break; }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
        if (us > 0.0 && us < best) best = us;
    }
    close(sv[0]); close(sv[1]);
    return (best < 1e29) ? best : -1.0;
}

/* CPU L2-distance throughput, (vectors*dim) per microsecond — the anchor that
 * converts GPU microseconds into PG cost units. Clamped to a plausible band;
 * outside it returns -1 (probe distrusted → DEFAULT/legacy). */
static double
probe_cpu_dist_tput(void)
{
    const int64_t   n = 100000; const int dim = 128;
    float          *corpus = (float *) malloc((size_t) n * dim * sizeof(float));
    float           q[128];
    struct timespec t0, t1;
    volatile double sink = 0.0;
    double          us, tput;
    int64_t         i; int j;

    if (!corpus)
        return -1.0;
    for (i = 0; i < n * dim; i++)
        corpus[i] = (float) ((uint64_t) i * 2654435761u % 1000) / 1000.0f;
    for (j = 0; j < dim; j++) q[j] = 0.1f;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < n; i++)
    {
        const float *v = corpus + i * dim;
        double s = 0.0;
        for (j = 0; j < dim; j++) { double d = (double) v[j] - q[j]; s += d * d; }
        sink += s;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    (void) sink;
    free(corpus);

    us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
    if (us <= 0.0)
        return -1.0;
    tput = (double) n * dim / us;            /* (vec*dim)/us */
    if (tput < 1.0 || tput > 1.0e6)          /* implausible → distrust */
        return -1.0;
    return tput;
}

static void
write_hw_profile(void)
{
    int           dev = (g_n_allowed_gpus > 0) ? g_allowed_gpus[0] : 0;
    CuvsHwProfile p;
    char          final[512], tmp[576];
    FILE         *f;

    mkdir(g_index_dir, 0700);   /* may not exist yet if no index built */

    memset(&p, 0, sizeof(p));
    if (dev >= 0 && dev < g_n_gpus)
    {
        strncpy(p.gpu_name, g_gpus[dev].name, sizeof(p.gpu_name) - 1);
        p.total_vram_bytes = (int64_t) g_gpus[dev].total_vram_bytes;
    }
    p.n_gpus      = (uint32_t) g_n_gpus;
    p.measured_at = (int64_t) time(NULL);
    /* Conservative DEFAULTs (shared with the planner fallback); probes overwrite. */
    p.link_bw_bpus    = CUVS_HWP_DEFAULT_LINK_BW;
    p.hbm_bw_bpus     = CUVS_HWP_DEFAULT_HBM_BW;
    p.gpu_bf_tput     = CUVS_HWP_DEFAULT_BF_TPUT;
    p.ipc_rtt_us      = CUVS_HWP_DEFAULT_IPC_RTT;
    p.cpu_dist_tput   = CUVS_HWP_DEFAULT_CPU_DIST;   /* v2; probed in Stage 3 */
    p.gpu_cagra_lat_us = CUVS_HWP_DEFAULT_CAGRA_LAT; /* v2; probed in Stage 3 */
    p.probe_status    = 0;

    cuvs_probe_hw(dev, &p.link_bw_bpus, &p.hbm_bw_bpus, &p.gpu_bf_tput,
                  &p.gpu_cagra_lat_us, &p.probe_status);

    /* Host-side coefficients (no CUDA): IPC RTT floor + CPU distance throughput. */
    {
        double rtt  = probe_ipc_rtt_us();
        double cput = probe_cpu_dist_tput();
        if (rtt  > 0.0) { p.ipc_rtt_us    = rtt;  p.probe_status |= CUVS_HWPROBE_IPC_RTT; }
        if (cput > 0.0) { p.cpu_dist_tput = cput; p.probe_status |= CUVS_HWPROBE_CPU_DIST; }
    }

    /* atomic profile write */
    snprintf(final, sizeof(final), "%s/cuvs_hw_profile", g_index_dir);
    snprintf(tmp,   sizeof(tmp),   "%s.tmp", final);
    f = fopen(tmp, "wb");
    if (f)
    {
        int ok = (cuvs_hw_profile_write(f, &p) == 0);
        if (ok && fflush(f) != 0) ok = 0;
        if (ok && fsync(fileno(f)) != 0) ok = 0;
        if (fclose(f) != 0) ok = 0;
        if (ok && rename(tmp, final) != 0) ok = 0;
        if (!ok) { unlink(tmp); LOG_WARN("pg_cuvs_server: hw_profile write failed errno=%d\n", errno); }
    }
    else
        LOG_WARN("pg_cuvs_server: hw_profile fopen(%s) failed errno=%d\n", tmp, errno);

    /* atomic daemon-identity write (text: gpu_name / pid / boot epoch) */
    snprintf(final, sizeof(final), "%s/cuvs_daemon_identity", g_index_dir);
    snprintf(tmp,   sizeof(tmp),   "%s.tmp", final);
    f = fopen(tmp, "w");
    if (f)
    {
        int ok = (fprintf(f, "%s\n%d\n%lld\n", p.gpu_name, (int) getpid(),
                          (long long) p.measured_at) > 0);
        if (ok && fflush(f) != 0) ok = 0;
        if (ok && fsync(fileno(f)) != 0) ok = 0;
        if (fclose(f) != 0) ok = 0;
        if (ok && rename(tmp, final) != 0) ok = 0;
        if (!ok) unlink(tmp);
    }

    {
        int dir_fd = open(g_index_dir, O_RDONLY);
        if (dir_fd >= 0) { fsync(dir_fd); close(dir_fd); }
    }

    LOG_INFO("pg_cuvs_server: hw_profile gpu='%s' link_bw=%.0f hbm_bw=%.0f bf_tput=%.0f "
             "cpu_dist=%.0f cagra_lat=%.1f ipc_rtt=%.1f probe_status=0x%x\n",
             p.gpu_name, p.link_bw_bpus, p.hbm_bw_bpus, p.gpu_bf_tput,
             p.cpu_dist_tput, p.gpu_cagra_lat_us, p.ipc_rtt_us, p.probe_status);
}

/* ----------------------------------------------------------------
 * main
 * ---------------------------------------------------------------- */
int
main(int argc, char **argv)
{
    cuvs_reclaim_params_default(&g_reclaim);
    /* Parse arguments */
    int lw_flag;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--socket") == 0 && i+1 < argc)
            strncpy(g_socket_path, argv[++i], sizeof(g_socket_path) - 1);
        else if (strcmp(argv[i], "--index-dir") == 0 && i+1 < argc)
            strncpy(g_index_dir, argv[++i], sizeof(g_index_dir) - 1);
        else if (strcmp(argv[i], "--max-vram-mb") == 0 && i+1 < argc)
            g_max_vram_bytes = (size_t)atol(argv[++i]) * 1024 * 1024;
        else if (strcmp(argv[i], "--rmm-pool-mb") == 0)
        {
            if (i + 1 >= argc || cuvs_parse_rmm_pool_mb(argv[++i], &g_rmm_pool_mb) != 0)
            {
                LOG_ERROR("--rmm-pool-mb requires a nonnegative integer MiB value\n");
                return 1;
            }
        }
        else if (strcmp(argv[i], "--rmm-scratch-permille") == 0)
        {
            if (i + 1 >= argc ||
                cuvs_parse_scratch_permille(argv[++i], &g_scratch_permille) != 0)
            {
                LOG_ERROR("--rmm-scratch-permille requires an integer from 0 to %u\n",
                          CUVS_ADMISSION_RHO_PERMILLE_MAX);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--rmm-build-headroom-permille") == 0)
        {
            if (i + 1 >= argc ||
                cuvs_lw_parse_build_headroom(argv[++i], &g_rmm_build_headroom_permille) != 0)
            {
                LOG_ERROR("--rmm-build-headroom-permille requires an integer from 0 to %u\n",
                          CUVS_RMM_BUILD_HEADROOM_PERMILLE_MAX);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--rmm-pool-max-percent") == 0)
        {
            if (i + 1 >= argc ||
                cuvs_parse_rmm_pool_percent(argv[++i], &g_rmm_pool_max_percent) != 0)
            {
                LOG_ERROR("--rmm-pool-max-percent requires an integer from 1 to 100\n");
                return 1;
            }
        }
        else if (strcmp(argv[i], "--rmm-dedicated-stream") == 0)
        {
            const char *v = i + 1 < argc ? argv[++i] : "";
            if (!strcmp(v, "on")) g_rmm_dedicated_stream = 1;
            else if (!strcmp(v, "off")) g_rmm_dedicated_stream = 0;
            else
            {
                LOG_ERROR("--rmm-dedicated-stream requires on or off\n");
                return 1;
            }
        }
        else if (strcmp(argv[i], "--hnsw-auto-overflow") == 0)
        {
            if (i + 1 >= argc
                || (g_hnsw_auto_overflow = cuvs_hnsw_overflow_from_name(argv[++i])) == UINT32_MAX)
            {
                LOG_ERROR("--hnsw-auto-overflow requires ace or error\n");
                return 1;
            }
        }
        else if (strcmp(argv[i], "--max-indexes") == 0 && i+1 < argc)
        {
            int n = atoi(argv[++i]);
            if (n >= 1)
                g_max_indexes = n;   /* registry working-set capacity */
        }
        else if (strncmp(argv[i], "--", 2) == 0 && cuvs_reclaim_flag_known(argv[i] + 2))
        {
            /* I3-B knobs KB-7..KB-27 (C6): strict values, startup refusal. */
            char err[256];
            const char *name = argv[i] + 2;
            if (i + 1 >= argc)
            {
                LOG_ERROR("--%s requires a value\n", name);
                return 1;
            }
            if (cuvs_reclaim_flag_set(&g_reclaim, name, argv[++i], err, sizeof(err)) != 0)
            {
                LOG_ERROR("%s\n", err);
                return 1;
            }
            int k = rt_index(name);
            if (k >= 0)
                snprintf(g_reclaim_src[k], sizeof(g_reclaim_src[k]), "flag");
        }
        else if (strcmp(argv[i], "--snapshot-uri") == 0 && i+1 < argc)
            strncpy(g_snapshot_uri, argv[++i], sizeof(g_snapshot_uri) - 1);
        else if (strcmp(argv[i], "--cluster-id") == 0 && i+1 < argc)
            strncpy(g_cluster_id, argv[++i], sizeof(g_cluster_id) - 1);
        else if (strcmp(argv[i], "--gcs-key-file") == 0 && i+1 < argc)
            strncpy(g_gcs_key_file, argv[++i], sizeof(g_gcs_key_file) - 1);
        else if (strcmp(argv[i], "--warmup-threads") == 0 && i+1 < argc)
        {
            int n = atoi(argv[++i]);
            if (n >= 1 && n <= 8)
                g_warmup_nthreads = n;
        }
        else if ((lw_flag = parse_lw_flag(argc, argv, &i)) != 0)
        {
            if (lw_flag < 0)
                return 1;   /* I4 knobs: out-of-range value refuses startup */
        }
        else if (strcmp(argv[i], "--gpu-devices") == 0 && i+1 < argc)
        {
            char buf[256];
            strncpy(buf, argv[++i], sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            char *tok = strtok(buf, ",");
            while (tok && g_n_allowed_gpus < CUVS_MAX_GPUS)
            {
                g_allowed_gpus[g_n_allowed_gpus++] = atoi(tok);
                tok = strtok(NULL, ",");
            }
        }
    }

    {
        char err[256], scope[32];
        if (cuvs_reclaim_params_check(&g_reclaim, err, sizeof(err)) != 0)
        {
            LOG_ERROR("%s\n", err);
            return 1;
        }
        cuvs_format_evict_scope(&g_reclaim.scope, scope, sizeof(scope));
        cuvs_peak_meter_set_enabled(g_reclaim.peak_meter);   /* KB-17, before the pool */
        LOG_INFO("pg_cuvs_server: rmm_peak_meter=%s\n", g_reclaim.peak_meter ? "on" : "off");
        LOG_INFO("pg_cuvs_server: cache policy build_evict_scope=%s reclaim_mode=%s "
                 "evict_min_idle_s=%lld evict_hit_bonus_s=%lld evict_hit_cap=%u "
                 "reclaim_high_pct=%u reclaim_low_pct=%u reclaim_tick_s=%lld pin_max_pct=%u "
                 "reclaim_free_bytes=%llu reclaim_free_max_pct=%u reclaim_free_window_min=%u "
                 "reclaim_floor_pct=%u load_soft_evict=%s reindex_self_replace=%s "
                 "usage_stats_file=%s\n",
                 scope, cuvs_reclaim_mode_name(g_reclaim.mode),
                 (long long)g_reclaim.t_min_s, (long long)g_reclaim.bonus_s,
                 g_reclaim.hits_cap, g_reclaim.hi_pct, g_reclaim.lo_pct,
                 (long long)g_reclaim.tick_s, g_reclaim.pin_max_pct,
                 (unsigned long long)g_reclaim.f_fixed, g_reclaim.f_cap_pct,
                 g_reclaim.window_min, g_reclaim.floor_pct,
                 g_reclaim.soft_evict ? "on" : "off", g_reclaim.self_replace ? "on" : "off",
                 g_reclaim.usage_file ? "on" : "off");
    }

    LOG_INFO("pg_cuvs_server: starting (socket=%s index-dir=%s snapshot=%s)\n",
            g_socket_path, g_index_dir,
            g_snapshot_uri[0] ? g_snapshot_uri : "disabled");

#ifdef __linux__
    /* #87 (DiD): SO_PEERCRED owner verification (finding 1) closes the
     * unlink-and-resquat attack, but only because every real segment's name
     * is CSPRNG-random and every fake one has the wrong uid. Neither defense
     * needs /dev/shm to be sticky (mode 1777) — but sticky is the standard
     * hardening for a shared tmpfs and worth a loud warning if it's missing,
     * since it also stops other unrelated tenants from touching each
     * other's files by name. */
    {
        struct stat st;
        if (stat("/dev/shm", &st) == 0 && !(st.st_mode & S_ISVTX))
            LOG_WARN("pg_cuvs_server: /dev/shm is not sticky (mode %o) — "
                      "any local user can unlink/replace another user's shm "
                      "segments; SO_PEERCRED owner checks (#87) still block "
                      "the daemon from trusting a resquatted segment, but "
                      "sticky /dev/shm (chmod +t) is recommended defense in depth\n",
                      (unsigned)(st.st_mode & 07777));
    }
#endif

    /* Install SIGTERM/SIGINT via sigaction WITHOUT SA_RESTART so a signal
     * interrupts a blocked accept() (returns -1/EINTR) and we can break out
     * of the loop to run graceful_shutdown() in the main thread. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  /* no SA_RESTART */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* 3S: ignore SIGPIPE so a client that disconnects mid-request (e.g. a backend
     * aborting a search on statement_timeout / query cancel) makes the reply
     * write() fail with EPIPE — the per-connection thread cleans up — instead of
     * killing the whole daemon (and every backend's GPU index). */
    signal(SIGPIPE, SIG_IGN);

    /* Phase 3C: initialize libcurl once for the process lifetime. Must be called
     * before any curl_easy_* or download/upload operations. */
    if (g_snapshot_uri[0] != '\0')
        curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Phase 3E: GPU detection */
    g_n_gpus = cuvs_detect_gpus(g_gpus, CUVS_MAX_GPUS);
    if (g_n_gpus == 0)
    {
        LOG_ERROR("pg_cuvs_server: no CUDA GPUs detected\n");
        return 1;
    }
    /* Validate and deduplicate --gpu-devices against detected GPUs */
    for (int i = 0; i < g_n_allowed_gpus; i++)
    {
        if (g_allowed_gpus[i] >= g_n_gpus)
        {
            LOG_ERROR("pg_cuvs_server: --gpu-devices references GPU %d but only %d detected\n",
                      g_allowed_gpus[i], g_n_gpus);
            return 1;
        }
        for (int j = i + 1; j < g_n_allowed_gpus; j++)
        {
            if (g_allowed_gpus[i] == g_allowed_gpus[j])
            {
                for (int k = j; k < g_n_allowed_gpus - 1; k++)
                    g_allowed_gpus[k] = g_allowed_gpus[k + 1];
                g_n_allowed_gpus--;
                j--;
            }
        }
    }

    /* Allocate the index registry now that --max-indexes is known (calloc =
     * zero-initialized, matching the old static arrays). Done before
     * startup_load_indexes() and any registry use; never realloc'd afterward. */
    g_indexes      = calloc((size_t)g_max_indexes, sizeof(IndexEntry));
    g_cold_indexes = calloc((size_t)g_max_indexes, sizeof(ColdIndexEntry));
    if (!g_indexes || !g_cold_indexes)
    {
        LOG_ERROR("pg_cuvs_server: failed to allocate index registry (--max-indexes %d)\n",
                  g_max_indexes);
        return 1;
    }
    LOG_INFO("index registry capacity: %d (--max-indexes)\n", g_max_indexes);

    /* Install the per-device allocator before probes, persisted-index reloads,
     * warmup or worker threads can make the first RMM allocation. An explicit
     * zero keeps the old allocation path. Any failed init aborts startup. */
    /* Before the pool, probes, loads and warmup acquire any RAFT resource. */
    if (cuvs_rmm_set_dedicated_stream(g_rmm_dedicated_stream) != 0)
    {
        LOG_ERROR("RMM dedicated stream mode could not be applied before first GPU use\n");
        return 1;
    }
    LOG_INFO("RMM dedicated stream: %s (--rmm-dedicated-stream; default %s)\n",
             g_rmm_dedicated_stream ? "on" : "off",
             CUVS_RMM_DEDICATED_STREAM_DEFAULT ? "on" : "off");
    if (g_rmm_pool_mb > 0)
    {
        for (int i = 0; i < n_usable_gpus(); i++)
        {
            int dev = usable_gpu(i);
            CuvsRmmPoolStats stats;
            if (cuvs_rmm_pool_init(dev, (size_t)g_rmm_pool_mb * 1024 * 1024,
                                  g_rmm_pool_max_percent) != 0 ||
                cuvs_rmm_pool_stats(dev, &stats) != 0)
            {
                LOG_ERROR("GPU %d: RMM pool initialization failed (%u MiB)\n",
                          dev, g_rmm_pool_mb);
                return 1;
            }
            g_rmm_pool_limits[dev] = (size_t)stats.maximum_bytes;
        }
    }
    LOG_INFO("RMM initial pool reservation: %u MiB per GPU (0=disabled)\n", g_rmm_pool_mb);
    LOG_INFO("RMM maximum pool quota: %u%% of total device memory\n",
             g_rmm_pool_mb > 0 ? g_rmm_pool_max_percent : 0);
    LOG_INFO("CAGRA build scratch: rmm_scratch_permille=%u (admission N = final + "
             "ceil(final x %u / 1000))\n", g_scratch_permille, g_scratch_permille);
    LOG_INFO("CAGRA build pool headroom: rmm_build_headroom_permille=%u (AR8 refuses "
             "pending + N + lw_ub + headroom > pool quota)\n", g_rmm_build_headroom_permille);
    LOG_INFO("HNSW auto overflow: %s (--hnsw-auto-overflow; default %s)\n",
             cuvs_hnsw_overflow_name(g_hnsw_auto_overflow),
             cuvs_hnsw_overflow_name(CUVS_HNSW_AUTO_OVERFLOW_DEFAULT));
    /* I4: effective large_workspace knobs, then the host protection line. */
    LOG_INFO("managed large_workspace: cagra_build_overflow=%s managed_overcommit=%s "
             "lw_margin_permille=%u\n", cuvs_lw_overflow_name(g_cagra_build_overflow),
             g_managed_overcommit ? "on" : "off", g_managed_lw_margin_permille);
    apply_oom_score_adj("/proc/self/oom_score_adj");

    /* ADR-075 Phase 1: probe hardware + write the global cost-model profile. */
    write_hw_profile();

    /* Initialize per-device VRAM budgets.
     *
     * ADR-065: when --max-vram-mb is omitted, default to a conservative fraction
     * of each device's total VRAM rather than "unlimited" — an external operator
     * who doesn't set a budget should NOT be able to OOM the device. The budget
     * is enforced against the daemon's own per-index accounting (total_vram_used),
     * which is reliable (it knows what it allocated); the headroom fraction
     * reserves room for the untracked cuVS workspace + CUDA context. (We do NOT
     * trust raw cudaMemGetInfo for the budget — cuVS's async mempool caches freed
     * memory so the "free" figure understates real availability.) Explicit
     * runtime-unlimited is still available via pg_cuvs_set_vram_budget(0). */
    {
        int n = n_usable_gpus();
        for (int i = 0; i < n; i++)
        {
            int dev = usable_gpu(i);
            if (g_max_vram_bytes > 0)
                g_max_vram_per_gpu[dev] = g_max_vram_bytes;          /* explicit --max-vram-mb */
            else
                g_max_vram_per_gpu[dev] =
                    (size_t)(g_gpus[dev].total_vram_bytes * CUVS_DEFAULT_VRAM_FRACTION);
            LOG_INFO("GPU %d (%s): %.0f MB total, budget %.0f MB (%s)\n",
                     dev, g_gpus[dev].name,
                     g_gpus[dev].total_vram_bytes / (1024.0 * 1024),
                     g_max_vram_per_gpu[dev] / (1024.0 * 1024),
                     g_max_vram_bytes ? "explicit --max-vram-mb"
                                      : "default fraction of total");
        }
    }

    /* C6/CK-27: one effective model line after every flag and GPU budget
     * has been resolved. This reports provenance; it never changes policy. */
    {
        char scope[32], budgets[CUVS_MAX_GPUS * 24];
        size_t at = 0;
        cuvs_format_evict_scope(&g_reclaim.scope, scope, sizeof(scope));
        budgets[0] = '\0';
        for (int i = 0; i < n_usable_gpus(); i++)
            at += (size_t)snprintf(budgets + at, sizeof(budgets) - at, "%s%zu",
                i ? "," : "", g_max_vram_per_gpu[usable_gpu(i)] / (1024u * 1024u));
        LOG_INFO("build admission model: scratch_permille=%u validated_overflow_policy=%s "
                 "validated_rows=999000 validated_dim=768 validated_gpu=\"NVIDIA A100 80GB PCIe\" "
                 "validated_quota_percent=50 overflow=%s overcommit=%s lw_margin_permille=%u "
                 "evict_scope=%s hnsw_auto_overflow=%s dedicated_stream=%s peak_meter=%s "
                 "rmm_pool_mb=%u rmm_pool_max_percent=%u evict_min_idle_s=%lld "
                 "evict_hit_bonus_s=%lld evict_hit_cap=%u reclaim_high_pct=%u "
                 "reclaim_low_pct=%u reclaim_tick_s=%lld reclaim_mode=%s reclaim_free_bytes=%llu "
                 "reclaim_free_max_pct=%u reclaim_free_window_min=%u reclaim_floor_pct=%u "
                 "load_soft_evict=%s reindex_self_replace=%s usage_stats_file=%s pin_max_pct=%u "
                 "max_vram_mb=%s max_indexes=%d\n",
                 g_scratch_permille, cuvs_lw_overflow_name(CUVS_RMM_SCRATCH_VALIDATED_OVERFLOW_POLICY),
                 cuvs_lw_overflow_name(g_cagra_build_overflow), g_managed_overcommit ? "on" : "off",
                 g_managed_lw_margin_permille, scope, cuvs_hnsw_overflow_name(g_hnsw_auto_overflow),
                 g_rmm_dedicated_stream ? "on" : "off", g_reclaim.peak_meter ? "on" : "off",
                 g_rmm_pool_mb, g_rmm_pool_max_percent, (long long)g_reclaim.t_min_s,
                 (long long)g_reclaim.bonus_s, g_reclaim.hits_cap, g_reclaim.hi_pct,
                 g_reclaim.lo_pct, (long long)g_reclaim.tick_s, cuvs_reclaim_mode_name(g_reclaim.mode),
                 (unsigned long long)g_reclaim.f_fixed, g_reclaim.f_cap_pct,
                 g_reclaim.window_min, g_reclaim.floor_pct, g_reclaim.soft_evict ? "on" : "off",
                 g_reclaim.self_replace ? "on" : "off", g_reclaim.usage_file ? "on" : "off",
                 g_reclaim.pin_max_pct, budgets, g_max_indexes);
        if (g_cagra_build_overflow != CUVS_RMM_SCRATCH_VALIDATED_OVERFLOW_POLICY)
            LOG_WARN("scratch model unvalidated: overflow=%s validated_overflow_policy=%s; "
                     "recalibrate scratch_permille for the active policy\n",
                     cuvs_lw_overflow_name(g_cagra_build_overflow),
                     cuvs_lw_overflow_name(CUVS_RMM_SCRATCH_VALIDATED_OVERFLOW_POLICY));
    }

    /* Load persisted indexes */
    (void)cuvs_hnsw_reap_workdirs(g_index_dir);
    startup_load_indexes();

    /* Per-device GPU warm-up so the first client query doesn't pay the
     * one-time CUDA context / RMM / cuBLAS / kernel init (hundreds of ms). */
    {
        int n = n_usable_gpus();
        for (int gi = 0; gi < n; gi++)
        {
            int dev = usable_gpu(gi);
            struct timespec w0, w1;
            clock_gettime(CLOCK_MONOTONIC, &w0);
            cuvs_warmup_device(dev);
            clock_gettime(CLOCK_MONOTONIC, &w1);
            double ms = (w1.tv_sec - w0.tv_sec) * 1000.0 +
                        (w1.tv_nsec - w0.tv_nsec) / 1e6;
            LOG_INFO("GPU %d warm-up done in %.0f ms\n", dev, ms);
        }
    }

    /* Create UDS socket */
    g_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_server_fd < 0)
    {
        perror("socket");
        return 1;
    }

    unlink(g_socket_path);  /* remove stale socket if present */

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_socket_path, sizeof(addr.sun_path) - 1);

    if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        return 1;
    }

    chmod(g_socket_path, 0660);

    if (listen(g_server_fd, 32) < 0)
    {
        perror("listen");
        return 1;
    }

    LOG_INFO("pg_cuvs_server: listening on %s\n", g_socket_path);

    /* Phase 3D: spawn warmup worker threads. They process the queue populated
     * by startup_load_indexes() Pass 2 and by on-demand cache miss triggers.
     * Workers download from GCS and load into VRAM in background while the
     * daemon is already accepting queries. */
    if (g_snapshot_uri[0] != '\0')
    {
        for (int i = 0; i < g_warmup_nthreads; i++)
            pthread_create(&g_warmup_tids[i], NULL, warmup_worker_thread, NULL);
        LOG_INFO("warmup: spawned %d worker threads\n", g_warmup_nthreads);
    }

    /* Phase 3L-9: spawn the single BF micro-batch consumer. Always running but
     * inert until a request with a non-zero batch window is enqueued. */
    if (pthread_create(&g_reclaim_tid, NULL, reclaim_thread, NULL) == 0)
        g_reclaim_started = 1;
    else
        LOG_WARN("pg_cuvs_server: reclaim thread failed to start; no background reclaim\n");
    if (pthread_create(&g_batch_worker_tid, NULL, batch_worker_thread, NULL) == 0)
        g_batch_worker_started = 1;
    else
        LOG_ERROR("pg_cuvs_server: failed to spawn BF batch worker; "
                  "brute_force micro-batching disabled (immediate dispatch only)\n");

    /* Accept loop */
    while (!g_shutdown)
    {
        int client_fd = accept(g_server_fd, NULL, NULL);
        if (client_fd < 0)
        {
            /* sigaction installed handlers WITHOUT SA_RESTART, so a SIGTERM/
             * SIGINT during accept() returns EINTR. We were signalled (or
             * g_shutdown was set between iterations) -> stop accepting. */
            if (errno == EINTR)
                break;
            if (!g_shutdown)
                perror("accept");
            break;
        }

        int *fd_ptr = malloc(sizeof(int));
        *fd_ptr = client_fd;

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, connection_thread, fd_ptr);
        pthread_attr_destroy(&attr);
    }

    /* Deferred graceful shutdown in the main thread (safe for mutex / CUDA
     * serialize / file I/O). In-flight detached connection threads holding
     * g_index_mutex are waited on by the lock inside graceful_shutdown. */
    graceful_shutdown();

    return 0;
}
