/*
 * cuvs_wrapper.cu — C++ / CUDA bridge to NVIDIA cuVS.
 *
 * Compiled as CUDA C++ (nvcc). Exposes a pure-C API declared in
 * cuvs_wrapper.h so that pg_cuvs.c (plain C) can call it without
 * triggering the float4 typedef collision between PostgreSQL headers
 * and CUDA headers.
 *
 * Dependency: libcuvs (RAPIDS cuVS). Install via conda:
 *   mamba install -c rapidsai -c nvidia libcuvs=24.12 cuda-toolkit=12.4
 */

#include "cuvs_wrapper.h"
#include "cuvs_hnsw_build_common.h"
#include "cuvs_ipc.h"   /* CUVS_METRIC_* (PG-free, CUDA-free defines) */
#include "cuvs_util.h"  /* ADR-075: CUVS_HWPROBE_* bits (PG-free, CUDA-free) */

#include <cuvs/neighbors/brute_force.hpp>
#include <cuvs/neighbors/cagra.hpp>
#include "cuvs_hnsw_sdk_plan.cuh"  /* serialize/deserialize merged here in cuVS 25.x+ */
#include <cuvs/neighbors/hnsw.hpp>   /* Phase 3I-1: CPU HNSW fallback */
#include <cuvs/neighbors/ivf_pq.hpp> /* 3P: IVF-PQ index */
#include <cuvs/neighbors/nn_descent.hpp> /* has_enough_device_memory (CAGRA AUTO kNN choice) */
#include <thrust/execution_policy.h>
#include <thrust/functional.h>
#include <thrust/iterator/counting_iterator.h>
#include <thrust/transform_reduce.h>
#include <cuvs/core/bitset.hpp>      /* 3O: cuvs::core::bitset_view */
#include <raft/core/device_resources.hpp>
#include <raft/core/device_mdarray.hpp>
#include <raft/core/host_mdarray.hpp>
#include <raft/core/bitset.cuh>      /* 3O: raft::core::bitset device ops */

#include <rmm/mr/pool_memory_resource.hpp>
#include <rmm/mr/limiting_resource_adaptor.hpp>
#include "cuvs_rmm_failure_resource.hpp"
#include <rmm/mr/per_device_resource.hpp>
#include <rmm/aligned.hpp>
#include <rmm/resource_ref.hpp>
#include "cuvs_peak_meter.h"
#include <rmm/cuda_stream.hpp>
#include "cuvs_res_pool.hpp"
#include <rmm/mr/managed_memory_resource.hpp>          /* I4: build-only large_workspace */
#include <raft/core/resource/device_memory_resource.hpp>
#include "cuvs_lw_guard.hpp"
#include "cuvs_host_free_fence.hpp"        /* Q6: fenced RAFT host frees */
#include <raft/mr/host_memory_resource.hpp>

#include <cuda_runtime.h>
#include <cuda_fp16.h>   /* Phase 3L: half precision for brute-force (cuvs.bf_precision) */
#include <cstring>
#include <memory>
#include <vector>
#include <chrono>   /* ADR-075: host wall-clock for the BF throughput probe */
#include <fstream>
#include <stdexcept>
#include <mutex>
#include <atomic>
#include <limits>
#include <optional>
#include <utility>
#include <filesystem>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <ctime>
#include <fcntl.h>
#include <dlfcn.h>

/* ----------------------------------------------------------------
 * Per-device device_resources pool (Phase 3E)
 *
 * Each cuVS call needs a raft::device_resources (a CUDA stream + lazily
 * created cuBLAS/cuSOLVER handles). Creating one per call repeats that setup,
 * and the daemon spawns a NEW thread per request (so thread_local would never
 * be reused). Instead we keep a per-device free-list and hand resources
 * out via RAII: a request borrows one (creating lazily if none free) and
 * returns it on scope exit. Sequential reuse across threads is safe because
 * the streams/handles are bound to a specific CUDA device context, and the
 * free-list guarantees only one thread uses a given object at a time.
 *
 * raft::device_resources objects are permanently bound to the CUDA device
 * they were created on. A resource created on GPU-0 cannot be used for
 * GPU-2 operations. Each device gets its own pool.
 *
 * device_resources has a deleted move ctor in cuVS 25.x+, so the pool holds
 * unique_ptr (movable) rather than the objects by value.
 * ---------------------------------------------------------------- */
namespace {

/* I3-D: a slot owns rmm::cuda_stream when --rmm-dedicated-stream is on and
 * builds device_resources on that stream; off keeps the per-thread default. */
struct RaftResTraits {
    using Res = raft::device_resources;
    using Stream = rmm::cuda_stream;
    static std::unique_ptr<Stream> make_stream() { return std::make_unique<Stream>(); }
    static std::unique_ptr<Res> make_res(Stream *stream)
    {
        return stream ? std::make_unique<Res>(stream->view()) : std::make_unique<Res>();
    }
};
using DevicePool = CuvsResPool<RaftResTraits>;
using ResSlot = DevicePool::Slot;

static DevicePool g_device_pools[CUVS_MAX_GPUS];

/* Limiting adaptors provide atomic, allocation-aligned byte counters. The
 * upstream enforces the quota; the usage adaptor only counts live allocations. */
using RmmUpstream = CuvsRmmFailureUpstream;
using RmmPool = rmm::mr::pool_memory_resource<RmmUpstream>;
/* I3-B (UC-6 A): the live-usage adaptor plus an always-on lock-free peak
 * (CAS max on the same atomic counter; no shared_mutex as in
 * statistics_resource_adaptor). Mirrors rmm 26.04 limiting_resource_adaptor
 * (do_allocate(bytes, stream), upstream ref allocate(stream, bytes)) without
 * a limit: the upstream adaptor enforces the quota. */
class PeakUsage final : public rmm::mr::device_memory_resource {
public:
    PeakUsage(RmmPool *upstream, int dev) : upstream_{*upstream}, dev_{dev} {}
    std::size_t get_allocated_bytes() const { return cuvs_peak_meter_live(dev_); }
    std::size_t get_peak_bytes() const { return cuvs_peak_meter_peak(dev_); }
    void reset_peak() { cuvs_peak_meter_reset(dev_); }
private:
    /* Live bytes and the lock-free CAS peak live in cuvs_peak_meter.c (CK-34
     * tests it on the CPU); --rmm-peak-meter off skips the peak update. */
    void *do_allocate(std::size_t bytes, rmm::cuda_stream_view stream) override
    {
        std::size_t sz = rmm::align_up(bytes, rmm::CUDA_ALLOCATION_ALIGNMENT);
        std::size_t level = cuvs_peak_meter_reserve(dev_, sz);
        try {
            void *p = cuvs_failed_allocation::outer([&] {
                return upstream_.allocate(stream, bytes);
            });
            cuvs_peak_meter_commit(dev_, level);   /* only a successful allocation */
            return p;
        } catch (const rmm::out_of_memory &) {
            cuvs_peak_meter_free(dev_, sz);
            throw;
        } catch (...) {
            cuvs_failed_allocation::clear();
            cuvs_peak_meter_free(dev_, sz);
            throw;
        }
    }
    void do_deallocate(void *ptr, std::size_t bytes, rmm::cuda_stream_view stream) noexcept override
    {
        upstream_.deallocate(stream, ptr, bytes);
        cuvs_peak_meter_free(dev_, rmm::align_up(bytes, rmm::CUDA_ALLOCATION_ALIGNMENT));
    }
    bool do_is_equal(rmm::mr::device_memory_resource const &other) const noexcept override
    {
        return this == std::addressof(other);
    }
    rmm::device_async_resource_ref upstream_;
    int dev_;
};
using RmmUsage = PeakUsage;
struct RmmPoolRegistry;
static RmmPoolRegistry *g_rmm_registry = nullptr;  /* null on the legacy path */

/* Created after RMM's default-resource registry on the first pooled startup
 * call, so teardown runs while that registry is still alive. Clear retained
 * RAFT resources before releasing the memory they can reference. */
struct RmmPoolRegistry {
    rmm::mr::device_memory_resource *previous[CUVS_MAX_GPUS] = {};
    std::unique_ptr<RmmUpstream> upstreams[CUVS_MAX_GPUS];
    std::unique_ptr<RmmPool> pools[CUVS_MAX_GPUS];
    std::unique_ptr<RmmUsage> usages[CUVS_MAX_GPUS];
    size_t limits[CUVS_MAX_GPUS] = {};
    ~RmmPoolRegistry() {
        g_rmm_registry = nullptr;
        for (int dev = 0; dev < CUVS_MAX_GPUS; dev++) {
            if (!pools[dev]) continue;
            if (cudaSetDevice(dev) != cudaSuccess) {
                /* Process exit with a lost CUDA context: let the driver reclaim
                 * the pool rather than freeing it on the wrong device. */
                g_device_pools[dev].abandon();
                (void)pools[dev].release();
                (void)usages[dev].release();
                (void)upstreams[dev].release();
                continue;
            }
            g_device_pools[dev].clear();
            rmm::mr::set_current_device_resource(previous[dev]);
            usages[dev].reset();
            pools[dev].reset();
            upstreams[dev].reset();
        }
    }
};

/* test-only: armed by cuvs_set_inject_extend_oom(); self-clears in cuvs_cagra_extend */
static std::atomic<int> g_inject_extend_oom{0};

/* Set when a cuvs_cagra_build[_multi] failure was an OOM (std::bad_alloc, incl.
 * RMM out_of_memory which derives from it). Read and cleared by the daemon via
 * cuvs_last_build_was_oom() on the thread that ran the build, to report an
 * after-admission OOM (ADR-086; no evict-and-retry). Thread-local (I3): with a
 * process-wide flag a concurrent build on another thread could clear or set
 * it between this build and its read. */
static thread_local int g_last_build_oom = 0;
static thread_local int64_t g_last_build_failed_bytes = -1;
/* I3 (C5 ATT): the built index did not hold all rows of its dataset. */
static thread_local int g_last_build_att = 0;
/* test-only: armed by cuvs_set_inject_build_oom(n); each build decrements and fails. */
static std::atomic<int> g_inject_build_oom{0};

std::unique_ptr<ResSlot> acquire_res(int device_id)
{
    /* A new slot's stream/handles are created on the selected device. */
    cudaError_t err = cudaSetDevice(device_id);
    if (err != cudaSuccess) {
        fprintf(stderr, "[acquire_res] cudaSetDevice(%d) failed: %s\n",
                device_id, cudaGetErrorString(err));
        throw std::runtime_error("cudaSetDevice failed in acquire_res");
    }
    return g_device_pools[device_id].acquire();
}

void release_res(int device_id, std::unique_ptr<ResSlot> r)
{
    g_device_pools[device_id].release(std::move(r));
}

/* Q6 (cuvs_host_free_fence.hpp): RAFT's default host resource becomes a
 * fenced new/delete resource, installed once before the first request
 * borrows a stream. Containers created before that keep their own resource
 * and free as before. libcuvs exports the default-resource holder as a
 * unique symbol, so the containers cuVS creates inside libcuvs see it too. */
static int fence_sync_stream(void *stream)
{
    return (int)cudaStreamSynchronize(static_cast<cudaStream_t>(stream));
}

/* Q6 B1: a failed wait quarantines the block (not freed). Record the CUDA
 * error, stream and device with the running totals; never throws. */
static void fence_log_failure(int err, void *stream, int device, std::size_t bytes)
{
    CuvsHostFreeFence::Stats &st = CuvsHostFreeFence::stats();
    fprintf(stderr, "[host_free_fence] stream wait failed: err=%d %s stream=%p device=%d; "
                    "quarantined %zu bytes (not freed); totals sync_failures=%llu "
                    "quarantined_blocks=%llu quarantined_bytes=%llu (before this block)\n",
            err, cudaGetErrorName(static_cast<cudaError_t>(err)), stream, device, bytes,
            (unsigned long long)st.sync_failures.load(),
            (unsigned long long)st.quarantined_blocks.load(),
            (unsigned long long)st.quarantined_bytes.load());
}

static void ensure_fenced_host_resource()
{
    static const bool installed = [] {
        static CuvsFencedHostResource fenced;
        static raft::pmr::resource_adaptor adaptor{&fenced};
        raft::mr::set_default_host_resource(raft::mr::host_resource_ref{adaptor});
        return true;
    }();
    (void)installed;
}

/* Largest squared row norm of a device row-major matrix (+inf if any row is
 * non-finite), for the fp16 kNN bound (cuvs_hnsw_ivfpq_needs_fp32). */
struct CuvsRowSqNorm {
    const float *data;
    int64_t dim;
    __device__ double operator()(int64_t row) const
    {
        const float *r = data + row * dim;
        double sum = 0.0;
        for (int64_t j = 0; j < dim; j++)
            sum += (double)r[j] * (double)r[j];
        return isfinite(sum) ? sum : INFINITY;
    }
};

static double device_max_sq_norm(raft::device_resources &res, const float *data,
                                 int64_t rows, int64_t dim)
{
    if (rows <= 0) return 0.0;
    return thrust::transform_reduce(thrust::cuda::par.on(res.get_stream()),
        thrust::counting_iterator<int64_t>(0), thrust::counting_iterator<int64_t>(rows),
        CuvsRowSqNorm{data, dim}, 0.0, thrust::maximum<double>());
}

/* RAII: borrow on construction, return to the pool on destruction. If a cuVS
 * call threw, the stream/handles may carry a sticky error; reusing such a
 * resource from the pool can abort a later, unrelated request (the whole
 * daemon, since one process serves every backend). poison() makes the
 * destructor DESTROY the resource instead of pooling it, so a failed request
 * cannot corrupt healthy ones. Declare PooledRes OUTSIDE the try block and
 * call poison() in the catch so it is still alive when the catch runs.
 *
 * cudaSetDevice is per-thread state. Since the daemon spawns one pthread per
 * connection and each handles exactly one request, setting the device in the
 * constructor is safe -- no other thread can race the device selection within
 * the same PooledRes lifetime. */
struct PooledRes {
    std::unique_ptr<ResSlot> r; /* poisoned: destroyed with its own stream */
    int dev;
    bool poisoned = false;
    /* Q6: host frees on this thread wait for the borrowed stream until the
     * resource is returned or destroyed (closed first in the destructor). */
    std::optional<CuvsHostFreeFence::Scope> fence;
    explicit PooledRes(int device_id) : r(acquire_res(device_id)), dev(device_id) {
        cudaError_t err = cudaSetDevice(device_id);
        if (err != cudaSuccess) {
            fprintf(stderr, "[PooledRes] cudaSetDevice(%d) failed: %s\n",
                    device_id, cudaGetErrorString(err));
            poisoned = true;
            throw std::runtime_error("cudaSetDevice failed");
        }
        ensure_fenced_host_resource();
        fence.emplace(static_cast<void *>(r->res->get_stream().value()), fence_sync_stream,
                      device_id, fence_log_failure);
    }
    ~PooledRes()
    {
        /* Q6 B1: a failed wait means this stream's work was not confirmed
         * complete; never pool such a resource. */
        const bool fence_failed = fence && fence->failures() > 0;
        fence.reset();
        if (fence_failed && !poisoned) {
            fprintf(stderr, "[PooledRes] host free fence wait failed on device %d; "
                            "destroying the resource instead of pooling it\n", dev);
            poisoned = true;
        }
        if (r && !poisoned) release_res(dev, std::move(r));
    }
    raft::device_resources &get() { return *r->res; }
    void poison() { poisoned = true; }
    /* Q6 B1: fail the request (inside its try block) if a host free of this
     * request could not wait for the stream; the catch then poisons. */
    void fence_check()
    {
        if (fence && fence->failures() > 0)
            throw std::runtime_error("host free fence: request stream wait failed (err=" +
                                     std::to_string(fence->last_error()) + ")");
    }
};

/* I4 / ADR-088: build-only large_workspace resource. cudaMallocManaged via
 * RMM's managed resource (outside the pool, the quota and the usage adaptor),
 * wrapped by a lock-free live/peak counter (lw_peak). At each new peak the
 * physical free is sampled for the [build_peak] spill flag (C8). */
class LwManagedResource final : public rmm::mr::device_memory_resource {
  public:
    CuvsLwCounter counter;

  private:
    rmm::mr::managed_memory_resource managed_;

    void *do_allocate(std::size_t bytes, rmm::cuda_stream_view stream) override
    {
        void *p = managed_.allocate(stream, bytes);
        if (counter.on_alloc(bytes)) {
            size_t free_bytes = 0, total_bytes = 0;
            if (cudaMemGetInfo(&free_bytes, &total_bytes) == cudaSuccess)
                counter.note_free_at_peak(free_bytes);
        }
        return p;
    }
    void do_deallocate(void *p, std::size_t bytes, rmm::cuda_stream_view stream) noexcept override
    {
        managed_.deallocate(stream, p, bytes);
        counter.on_free(bytes);
    }
    bool do_is_equal(const rmm::mr::device_memory_resource &other) const noexcept override
    {
        return this == &other;
    }
};

/* CuvsLwWorkspaceGuard operations on one borrowed device_resources. restore()
 * reinstalls a default factory (current device resource, i.e. the RMM pool
 * adaptor), so the resource returns to the free list as it was borrowed. If
 * restore fails, the resource is poisoned instead of pooled; if allocations
 * are still live, the counted resource is kept alive rather than destroyed
 * under them (reported as lw_live_end). */
struct LwWorkspaceOps {
    PooledRes &pr;
    CuvsLwBuildStats *stats;
    std::shared_ptr<LwManagedResource> mr;

    void install()
    {
        mr = std::make_shared<LwManagedResource>();
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        raft::resource::set_large_workspace_resource(pr.get(), mr);
#pragma GCC diagnostic pop
        stats->applied = 1;
    }
    void restore() noexcept
    {
        try {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
            raft::resource::set_large_workspace_resource(
                pr.get(), std::shared_ptr<rmm::mr::device_memory_resource>{});
#pragma GCC diagnostic pop
            stats->restored = 1;
        } catch (...) {
            fprintf(stderr, "[cuvs_cagra_build_multi] large_workspace restore failed; "
                            "poisoning resources\n");
            pr.poison();
        }
        stats->lw_peak = mr->counter.peak.load();
        stats->lw_live_end = mr->counter.live.load();
        stats->free_at_peak = mr->counter.free_at_peak.load();
        if (stats->lw_live_end != 0) {
            fprintf(stderr, "[cuvs_cagra_build_multi] large_workspace live %llu bytes at "
                            "restore; keeping the managed resource alive\n",
                    (unsigned long long)stats->lw_live_end);
            (void)new std::shared_ptr<LwManagedResource>(mr);  /* deliberate leak */
        }
    }
};

} // namespace

extern "C" int
cuvs_rmm_set_dedicated_stream(int on)
{
    for (int dev = 0; dev < CUVS_MAX_GPUS; dev++)
        if (g_device_pools[dev].set_dedicated(on != 0) != 0)
            return -1;
    return 0;
}

extern "C" int
cuvs_rmm_pool_init(int device_id, size_t initial_pool_bytes, uint32_t maximum_percent)
{
    if (initial_pool_bytes == 0)
        return 0;   /* disabled path: no resource lookup or CUDA allocation */
    if (device_id < 0 || device_id >= CUVS_MAX_GPUS ||
        cudaSetDevice(device_id) != cudaSuccess)
        return 1;
    try {
        size_t free_bytes = 0, total_bytes = 0;
        if (cudaMemGetInfo(&free_bytes, &total_bytes) != cudaSuccess)
            return 1;
        size_t maximum = cuvs_rmm_pool_max_bytes(total_bytes, maximum_percent);
        if (maximum == 0 || initial_pool_bytes > maximum) {
            fprintf(stderr, "[cuvs_rmm_pool_init] GPU %d: initial %zu exceeds quota %zu bytes\n",
                    device_id, initial_pool_bytes, maximum);
            return 1;
        }
        auto *upstream = rmm::mr::get_current_device_resource();
        static RmmPoolRegistry registry;
        if (registry.pools[device_id])
            return 1;   /* startup-only: never replace a live allocator */
        auto counted_upstream = std::make_unique<RmmUpstream>(
            upstream, maximum);
        /* A finite pool constructor maximum changes growth to remaining/2.
         * Enforce the quota upstream to retain the existing doubling policy. */
        auto pool = std::make_unique<RmmPool>(counted_upstream.get(), initial_pool_bytes);
        auto usage = std::make_unique<RmmUsage>(pool.get(), device_id);
        registry.previous[device_id] = upstream;
        rmm::mr::set_current_device_resource(usage.get());
        registry.upstreams[device_id] = std::move(counted_upstream);
        registry.pools[device_id] = std::move(pool);
        registry.usages[device_id] = std::move(usage);
        registry.limits[device_id] = maximum;
        g_rmm_registry = &registry;
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_rmm_pool_init] GPU %d: %s\n", device_id, e.what());
        return 1;
    } catch (...) {
        fprintf(stderr, "[cuvs_rmm_pool_init] GPU %d: unknown exception\n", device_id);
        return 1;
    }
}

extern "C" int
cuvs_rmm_pool_stats(int device_id, CuvsRmmPoolStats *out)
{
    if (!g_rmm_registry || device_id < 0 || device_id >= CUVS_MAX_GPUS ||
        !g_rmm_registry->pools[device_id])
        return 1;
    std::memset(out, 0, sizeof(*out));
    out->device_id = (uint32_t)device_id;
    out->maximum_bytes = g_rmm_registry->limits[device_id];
    /* Pools retain upstream blocks until shutdown. Reading usage first avoids
     * pairing a new usage value with the reservation from before its growth. */
    out->used_bytes = g_rmm_registry->usages[device_id]->get_allocated_bytes();
    out->reserved_bytes = g_rmm_registry->upstreams[device_id]->get_allocated_bytes();
    out->peak_bytes = g_rmm_registry->usages[device_id]->get_peak_bytes();   /* I3-B */
    return 0;
}

/* I3-B: lock-free peak of live pool allocations since the last reset. */
extern "C" size_t
cuvs_rmm_pool_peak_bytes(int device_id)
{
    if (!g_rmm_registry || device_id < 0 || device_id >= CUVS_MAX_GPUS ||
        !g_rmm_registry->usages[device_id])
        return 0;
    return g_rmm_registry->usages[device_id]->get_peak_bytes();
}

extern "C" void
cuvs_rmm_pool_peak_reset(int device_id)
{
    if (g_rmm_registry && device_id >= 0 && device_id < CUVS_MAX_GPUS &&
        g_rmm_registry->usages[device_id])
        g_rmm_registry->usages[device_id]->reset_peak();
}

/* Opaque CAGRA index wrapper.
 *
 * The dataset (d_corpus) MUST outlive the index because cagra::index holds an
 * mdspan view into the dataset's device memory rather than owning it. If the
 * dataset is freed (e.g., function-scope local), the index has dangling
 * pointers. Symptom: SIGSEGV on serialize(include_dataset=true) and on search.
 *
 * raft::device_resources has a deleted move constructor in cuVS 25.x+; the
 * pool above holds them via unique_ptr and hands them out with PooledRes.
 */
struct CuvsCagraIndexImpl {
    /* dataset MUST be declared before idx so destruction is reverse order:
     * idx destroyed first, then dataset. */
    raft::device_matrix<float, int64_t> dataset;
    cuvs::neighbors::cagra::index<float, uint32_t> idx;
    uint32_t metric;   /* CUVS_METRIC_*; selects the tail padding sentinel */

    CuvsCagraIndexImpl(raft::device_matrix<float, int64_t> &&d,
                       cuvs::neighbors::cagra::index<float, uint32_t> &&i,
                       uint32_t m)
        : dataset(std::move(d)), idx(std::move(i)), metric(m)
    {}
};

/* Forward decl: defined below, but cuvs_brute_force_search / cuvs_bf_build (above
 * the definition) need the metric -> cuVS DistanceType mapping. */
static cuvs::distance::DistanceType cuvs_distance_type(uint32_t metric);

/* Inverse of cuvs_distance_type. A deserialized index carries the cuVS
 * DistanceType but not the CUVS_METRIC_* code it was built from, and the tail
 * contract's padding sentinel depends on that code. */
static uint32_t
cuvs_metric_code(cuvs::distance::DistanceType dt)
{
    switch (dt) {
        case cuvs::distance::DistanceType::CosineExpanded: return CUVS_METRIC_COSINE;
        case cuvs::distance::DistanceType::InnerProduct:   return CUVS_METRIC_IP;
        default:                                           return CUVS_METRIC_L2;
    }
}

/* TAIL CONTRACT (cuvs_wrapper.h, #103): fill results[from, to) with the padding
 * sentinel. Every search entry point that clamps top_k to the corpus size calls
 * this for the slots the corpus cannot fill, so a successful search always
 * writes all top_k slots. The CPU shim pads identically in shim_topk. */
static inline void
pad_results(CuvsSearchResult *results, int from, int to, uint32_t metric)
{
    for (int i = from; i < to; i++) {
        results[i].item_id  = CUVS_PAD_ITEM_ID;
        results[i].distance = cuvs_pad_distance(metric);
    }
}

/* TAIL CONTRACT (cuvs_wrapper.h, #103), prefilter clause. pad_results above
 * covers only the first stated reason a slot goes unfilled — the corpus holding
 * fewer than top_k vectors. The second, "a prefilter excluded them", lands
 * INSIDE [0, bk): cuVS writes its own no-neighbour marker there, and that marker
 * is not CUVS_PAD_ITEM_ID (CAGRA indexes with uint32, so it arrives as a large
 * positive value after the widening cast, never as -1). Callers were told they
 * may detect unfilled slots by item_id alone, so translate anything outside
 * [0, n) to the documented sentinel. The CPU shim already behaves this way —
 * shim_topk does not distinguish the two reasons — which is why Tier-1 cannot
 * express this divergence. Valid ids pass through untouched. */
static inline void
store_result(CuvsSearchResult *slot, int64_t id, float distance,
             int64_t n, uint32_t metric)
{
    if (id < 0 || id >= n) {
        slot->item_id  = CUVS_PAD_ITEM_ID;
        slot->distance = cuvs_pad_distance(metric);
    } else {
        slot->item_id  = id;
        slot->distance = distance;
    }
}

/* Phase 3I-1: CPU HNSW index (hnswlib-backed via cuVS).
 * from_cagra() returns unique_ptr; deserialize() returns raw ptr via out-param.
 * We store a raw ptr and delete in cuvs_hnsw_free. */
struct CuvsHnswIndexImpl {
    cuvs::neighbors::hnsw::index<float> *idx; /* owned; delete in free */
    int     dim;
    uint32_t metric;
};

/* Opaque resident brute-force index (Phase 3B delta cache + Phase 3L main BF).
 * Like CAGRA, the dataset must outlive the index, so we retain it alongside;
 * n bounds a defensive top_k clamp (brute_force cannot return more than the
 * corpus). Phase 3L: the resident copy is either float32 or float16 — exactly
 * one variant pair is active (the other is null). Heap-owned so the index's
 * dataset view stays valid for the handle's lifetime. */
struct CuvsBfIndexImpl {
    int      dim;
    int64_t  n;
    uint32_t precision;   /* 0 = float32, 1 = float16 */
    uint32_t metric;      /* CUVS_METRIC_*; selects the tail padding sentinel */

    /* cuVS brute_force::index is index<DataT, DistanceT>; distances always
     * accumulate in float, so the half variant is index<half, float> (NOT
     * index<half, half>) to match build()'s return type and search()'s
     * overloads. float's DistanceT already defaults to float. */
    raft::device_matrix<float, int64_t>              *ds_f32  = nullptr;
    cuvs::neighbors::brute_force::index<float>        *idx_f32 = nullptr;
    raft::device_matrix<half, int64_t>               *ds_f16  = nullptr;
    cuvs::neighbors::brute_force::index<half, float>  *idx_f16 = nullptr;

    CuvsBfIndexImpl(int dm, int64_t nn, uint32_t prec, uint32_t met)
        : dim(dm), n(nn), precision(prec), metric(met) {}
    ~CuvsBfIndexImpl() {
        delete idx_f32; delete ds_f32;
        delete idx_f16; delete ds_f16;
    }
};

/* ----------------------------------------------------------------
 * GPU detection (Phase 3E)
 * ---------------------------------------------------------------- */
extern "C" int
cuvs_detect_gpus(CuvsGpuDeviceInfo *out, int max_devices)
{
    int device_count = 0;
    cudaError_t err = cudaGetDeviceCount(&device_count);
    if (err != cudaSuccess || device_count <= 0)
        return 0;

    int n = (device_count < max_devices) ? device_count : max_devices;
    for (int i = 0; i < n; i++) {
        out[i].device_id = i;
        out[i].total_vram_bytes = 0;
        out[i].name[0] = '\0';

        cudaDeviceProp prop;
        if (cudaGetDeviceProperties(&prop, i) != cudaSuccess) {
            fprintf(stderr, "[cuvs_detect_gpus] GPU %d: cudaGetDeviceProperties failed, treating as unavailable\n", i);
            continue;
        }
        if (cudaSetDevice(i) != cudaSuccess) {
            fprintf(stderr, "[cuvs_detect_gpus] GPU %d: cudaSetDevice failed, treating as unavailable\n", i);
            continue;
        }
        void *probe = NULL;
        cudaError_t probe_err = cudaMalloc(&probe, 1);
        if (probe_err != cudaSuccess) {
            fprintf(stderr, "[cuvs_detect_gpus] GPU %d: health probe cudaMalloc failed (%s), treating as unavailable\n",
                    i, cudaGetErrorString(probe_err));
            continue;
        }
        cudaFree(probe);

        out[i].total_vram_bytes = prop.totalGlobalMem;
        strncpy(out[i].name, prop.name, sizeof(out[i].name) - 1);
        out[i].name[sizeof(out[i].name) - 1] = '\0';
    }
    return n;
}

/* ----------------------------------------------------------------
 * GPU availability check
 * ---------------------------------------------------------------- */
extern "C" int
cuvs_gpu_available(void)
{
    int device_count = 0;
    cudaError_t err = cudaGetDeviceCount(&device_count);
    return (err == cudaSuccess && device_count > 0) ? 1 : 0;
}

/* ----------------------------------------------------------------
 * ADR-075 Phase 1: physical hardware-constant probe (cost-model v2).
 * Best-effort; each field written + bit set only on success.
 * ---------------------------------------------------------------- */
extern "C" int
cuvs_probe_hw(int device_id, double *link_bw_bpus, double *hbm_bw_bpus,
              double *gpu_bf_tput, double *gpu_cagra_lat_us,
              unsigned int *probe_status)
{
  try {
    if (cudaSetDevice(device_id) != cudaSuccess)
        return 0;

    const size_t BYTES = 64u * 1024u * 1024u;   /* 64 MB transfer probe */
    const int    ITERS = 5;

    cudaEvent_t t0, t1;
    if (cudaEventCreate(&t0) != cudaSuccess)
        return 0;
    if (cudaEventCreate(&t1) != cudaSuccess) { cudaEventDestroy(t0); return 0; }

    /* --- link_bw: pinned-host -> device H2D (min of ITERS) --- */
    {
        void *h = nullptr, *d = nullptr;
        if (cudaMallocHost(&h, BYTES) == cudaSuccess &&
            cudaMalloc(&d, BYTES) == cudaSuccess)
        {
            memset(h, 0, BYTES);
            cudaMemcpy(d, h, BYTES, cudaMemcpyHostToDevice);   /* warm up */
            float best_ms = 1e30f;
            for (int i = 0; i < ITERS; i++) {
                cudaEventRecord(t0);
                cudaMemcpy(d, h, BYTES, cudaMemcpyHostToDevice);
                cudaEventRecord(t1);
                if (cudaEventSynchronize(t1) != cudaSuccess) { best_ms = 1e30f; break; }
                float ms = 0.0f;
                if (cudaEventElapsedTime(&ms, t0, t1) == cudaSuccess && ms > 0.0f && ms < best_ms)
                    best_ms = ms;
            }
            if (best_ms < 1e29f && best_ms > 0.0f) {
                *link_bw_bpus = (double)BYTES / ((double)best_ms * 1000.0);  /* bytes/us */
                *probe_status |= CUVS_HWPROBE_LINK_BW;
            }
        }
        if (d) cudaFree(d);
        if (h) cudaFreeHost(h);
    }

    /* --- hbm_bw: device -> device (2*BYTES traffic = read+write) --- */
    {
        void *a = nullptr, *b = nullptr;
        if (cudaMalloc(&a, BYTES) == cudaSuccess &&
            cudaMalloc(&b, BYTES) == cudaSuccess)
        {
            cudaMemset(a, 0, BYTES);
            cudaMemcpy(b, a, BYTES, cudaMemcpyDeviceToDevice);   /* warm up */
            float best_ms = 1e30f;
            for (int i = 0; i < ITERS; i++) {
                cudaEventRecord(t0);
                cudaMemcpy(b, a, BYTES, cudaMemcpyDeviceToDevice);
                cudaEventRecord(t1);
                if (cudaEventSynchronize(t1) != cudaSuccess) { best_ms = 1e30f; break; }
                float ms = 0.0f;
                if (cudaEventElapsedTime(&ms, t0, t1) == cudaSuccess && ms > 0.0f && ms < best_ms)
                    best_ms = ms;
            }
            if (best_ms < 1e29f && best_ms > 0.0f) {
                double bw = (2.0 * (double)BYTES) / ((double)best_ms * 1000.0);  /* bytes/us */
                /* Plausibility band (~10 GB/s .. 100 TB/s). A contended/garbage
                 * measurement outside this leaves the DEFAULT + bit clear so the
                 * planner uses the legacy cost (else a tiny hbm_bw would blow up
                 * the flat cost and silently drop the path). */
                if (bw >= 1.0e4 && bw <= 1.0e8) {
                    *hbm_bw_bpus = bw;
                    *probe_status |= CUVS_HWPROBE_HBM_BW;
                }
            }
        }
        if (a) cudaFree(a);
        if (b) cudaFree(b);
    }

    cudaEventDestroy(t0);
    cudaEventDestroy(t1);

    /* --- gpu_bf_tput: time a synthetic resident BF (reuses the real entry) --- */
    {
        const int64_t n = 20000; const int dim = 128; const int k = 10;
        std::vector<float> corpus((size_t)n * dim, 0.5f);
        std::vector<float> query(dim, 0.5f);
        std::vector<CuvsSearchResult> res(k);
        auto s0 = std::chrono::steady_clock::now();
        int rc = cuvs_brute_force_search(corpus.data(), query.data(), n, dim, k,
                                         CUVS_METRIC_L2, res.data(), device_id);
        auto s1 = std::chrono::steady_clock::now();
        if (rc == 0) {
            double us = std::chrono::duration<double, std::micro>(s1 - s0).count();
            if (us > 0.0) {
                *gpu_bf_tput = ((double)n * (double)dim) / us;   /* (vec*dim)/us */
                *probe_status |= CUVS_HWPROBE_BF_TPUT;
            }
        }
    }

    /* --- gpu_cagra_lat_us: build a small CAGRA index, time a single-query search
     * (the per-query graph-search latency floor; ~N-independent). Warm once so the
     * timed runs do not pay first-query kernel load. Best-effort. --- */
    {
        const int64_t n = 10000; const int dim = 128; const int k = 10;
        std::vector<float> corpus((size_t)n * dim), query(dim, 0.1f);
        for (size_t i = 0; i < corpus.size(); i++)
            corpus[i] = (float)((i * 2654435761u) % 1000) / 1000.0f;
        std::vector<CuvsSearchResult> res(k);
        CuvsCagraIndex idx = cuvs_cagra_build(corpus.data(), n, dim, CUVS_METRIC_L2,
                                              0, 0, CUVS_CAGRA_BUILD_AUTO, device_id,
                                              CUVS_LW_OVERFLOW_ERROR, NULL); /* I4: probe never managed */
        if (idx) {
            (void) cuvs_cagra_search(idx, query.data(), dim, k, res.data(), device_id, 0, 0); /* warm */
            double best_us = 1e30;
            for (int i = 0; i < 5; i++) {
                auto c0 = std::chrono::steady_clock::now();
                int rc = cuvs_cagra_search(idx, query.data(), dim, k, res.data(), device_id, 0, 0);
                auto c1 = std::chrono::steady_clock::now();
                if (rc == 0) {
                    double us = std::chrono::duration<double, std::micro>(c1 - c0).count();
                    if (us > 0.0 && us < best_us) best_us = us;
                }
            }
            if (best_us < 1e29) {
                *gpu_cagra_lat_us = best_us;
                *probe_status |= CUVS_HWPROBE_CAGRA_LAT;
            }
            cuvs_cagra_free(idx, device_id);
        }
    }
    return 0;
  } catch (...) {
    /* never let a C++ exception (cuVS/RMM bad_alloc, raft::exception, ...) cross
     * the extern "C" boundary and abort the daemon — the probe is best-effort,
     * any failure leaves DEFAULTs + bits clear. */
    return 0;
  }
}

/* ----------------------------------------------------------------
 * VRAM query
 * ---------------------------------------------------------------- */
/* cudaMemGetInfo free and the default async mempool's reclaimable bytes.
 * Returns false on any CUDA failure before free_bytes is known. */
static bool
vram_physical_parts(int device_id, size_t *free_out, size_t *async_out)
{
    size_t free_bytes = 0, total_bytes = 0;
    if (cudaSetDevice(device_id) != cudaSuccess)
        return false;
    if (cudaMemGetInfo(&free_bytes, &total_bytes) != cudaSuccess)
        return false;
    size_t async_reclaimable = 0;

    /* ADR-065: cuVS allocates through a CUDA async mempool that caches freed
     * blocks; cudaMemGetInfo counts that cache as "used", understating the
     * memory a new allocation can actually reuse. Add back the pool's
     * reclaimable bytes (reserved but not currently in use) so the headroom
     * estimate reflects real availability. Best-effort: any error leaves the
     * raw free figure untouched. */
    cudaMemPool_t pool;
    if (cudaDeviceGetDefaultMemPool(&pool, device_id) == cudaSuccess) {
        cuuint64_t reserved = 0, used = 0;
        if (cudaMemPoolGetAttribute(pool, cudaMemPoolAttrReservedMemCurrent, &reserved) == cudaSuccess &&
            cudaMemPoolGetAttribute(pool, cudaMemPoolAttrUsedMemCurrent,     &used)     == cudaSuccess &&
            reserved > used)
            async_reclaimable = (size_t)(reserved - used);
    }
    *free_out = free_bytes;
    *async_out = async_reclaimable;
    return true;
}

extern "C" size_t
cuvs_vram_physical_free_bytes_on(int device_id)
{
    size_t free_bytes = 0, async_reclaimable = 0;
    if (!vram_physical_parts(device_id, &free_bytes, &async_reclaimable))
        return 0;
    return free_bytes > SIZE_MAX - async_reclaimable ? SIZE_MAX : free_bytes + async_reclaimable;
}

extern "C" size_t
cuvs_vram_free_bytes_on(int device_id)
{
    size_t free_bytes = 0, async_reclaimable = 0;
    if (!vram_physical_parts(device_id, &free_bytes, &async_reclaimable))
        return 0;
    CuvsRmmPoolStats stats;
    if (cuvs_rmm_pool_stats(device_id, &stats) == 0)
        return cuvs_rmm_pool_headroom(free_bytes, async_reclaimable,
                                      (size_t)stats.maximum_bytes,
                                      (size_t)stats.reserved_bytes,
                                      (size_t)stats.used_bytes);
    return cuvs_rmm_pool_headroom(free_bytes, async_reclaimable, 0, 0, 0);
}

extern "C" size_t
cuvs_vram_free_bytes(void)
{
    return cuvs_vram_free_bytes_on(0);
}

/* ----------------------------------------------------------------
 * Brute-force search (exact, no index)
 * Used when no CAGRA index is available.
 * ---------------------------------------------------------------- */
extern "C" int
cuvs_brute_force_search(
    const float      *corpus_vecs,
    const float      *query_vec,
    int64_t           n_corpus,
    int               dim,
    int               top_k,
    uint32_t          metric,
    CuvsSearchResult *results,
    int               device_id)
{
    if (top_k <= 0)
        return 0;
    /* Clamp to the corpus and pad the tail (TAIL CONTRACT), as the indexed
     * entry points do. */
    int bk = top_k;
    if ((int64_t)bk > n_corpus)
        bk = (int)n_corpus;
    if (bk <= 0) {
        pad_results(results, 0, top_k, metric);
        return 0;
    }

    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();

        auto d_corpus = raft::make_device_matrix<float, int64_t>(res, n_corpus, (int64_t)dim);
        raft::copy(d_corpus.data_handle(), corpus_vecs, n_corpus * dim, res.get_stream());

        auto d_queries = raft::make_device_matrix<float, int64_t>(res, (int64_t)1, (int64_t)dim);
        raft::copy(d_queries.data_handle(), query_vec, dim, res.get_stream());

        auto d_indices   = raft::make_device_matrix<int64_t, int64_t>(res, 1, bk);
        auto d_distances = raft::make_device_matrix<float,   int64_t>(res, 1, bk);

        auto index = cuvs::neighbors::brute_force::build(
            res,
            raft::make_const_mdspan(d_corpus.view()),
            cuvs_distance_type(metric));

        cuvs::neighbors::brute_force::search(
            res, index,
            raft::make_const_mdspan(d_queries.view()),
            d_indices.view(),
            d_distances.view());

        res.sync_stream();

        std::vector<int64_t> h_indices(bk);
        std::vector<float>   h_distances(bk);
        raft::copy(h_indices.data(),   d_indices.data_handle(),   bk, res.get_stream());
        raft::copy(h_distances.data(), d_distances.data_handle(), bk, res.get_stream());
        res.sync_stream();

        for (int i = 0; i < bk; i++) {
            results[i].item_id  = h_indices[i];
            results[i].distance = h_distances[i];
        }
        pad_results(results, bk, top_k, metric);
        return 0;
    } catch (...) {
        _pr.poison();
        return 1;
    }
}

/* ----------------------------------------------------------------
 * Resident brute-force index (Phase 3B delta cache): build once, search many.
 * ---------------------------------------------------------------- */
extern "C" CuvsBfIndex
cuvs_bf_build(const float *vecs, int64_t n, int dim, uint32_t metric,
              uint32_t precision, int device_id)
{
    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();
        const size_t total = (size_t)n * (size_t)dim;

        /* Upload corpus once; retained in the impl so the index's view stays
         * valid for the handle's lifetime (mirror of the CAGRA dataset rule).
         * unique_ptr makes a mid-build throw clean up the partial impl. */
        std::unique_ptr<CuvsBfIndexImpl> impl(new CuvsBfIndexImpl(dim, n, precision, metric));

        if (precision == 1 /* float16 */) {
            /* Convert host float32 -> host half once, then upload. half search
             * accumulates in float32 inside cuVS, so recall is preserved while
             * the resident dataset uses half the VRAM bandwidth. */
            impl->ds_f16 = new raft::device_matrix<half, int64_t>(
                raft::make_device_matrix<half, int64_t>(res, n, (int64_t)dim));
            {
                std::vector<half> h_half(total);
                for (size_t i = 0; i < total; i++)
                    h_half[i] = __float2half(vecs[i]);
                raft::copy(impl->ds_f16->data_handle(), h_half.data(), total, res.get_stream());
                res.sync_stream();
            }
            auto idx = cuvs::neighbors::brute_force::build(
                res,
                raft::make_const_mdspan(impl->ds_f16->view()),
                cuvs_distance_type(metric));
            res.sync_stream();
            impl->idx_f16 = new cuvs::neighbors::brute_force::index<half, float>(std::move(idx));
        } else {
            impl->ds_f32 = new raft::device_matrix<float, int64_t>(
                raft::make_device_matrix<float, int64_t>(res, n, (int64_t)dim));
            raft::copy(impl->ds_f32->data_handle(), vecs, total, res.get_stream());
            res.sync_stream();
            auto idx = cuvs::neighbors::brute_force::build(
                res,
                raft::make_const_mdspan(impl->ds_f32->view()),
                cuvs_distance_type(metric));
            res.sync_stream();
            impl->idx_f32 = new cuvs::neighbors::brute_force::index<float>(std::move(idx));
        }
        return impl.release();
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_bf_build] exception: %s\n", e.what());
        _pr.poison();
        return nullptr;
    } catch (...) {
        fprintf(stderr, "[cuvs_bf_build] unknown exception\n");
        _pr.poison();
        return nullptr;
    }
}

extern "C" int
cuvs_bf_search(
    CuvsBfIndex       index,
    const float      *query_vec,
    int               dim,
    int               top_k,
    CuvsSearchResult *results,
    int               device_id)
{
    if (!index)
        return 1;

    CuvsBfIndexImpl *impl = static_cast<CuvsBfIndexImpl *>(index);
    if (dim != impl->dim)
        return 2;
    if (top_k <= 0)
        return 0;
    /* brute_force cannot return more neighbors than the corpus holds; the tail
     * slots [bk, top_k) carry the padding sentinel (TAIL CONTRACT). */
    int bk = top_k;
    if ((int64_t)bk > impl->n)
        bk = (int)impl->n;
    if (bk <= 0) {
        pad_results(results, 0, top_k, impl->metric);
        return 0;
    }

    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();

        auto d_indices   = raft::make_device_matrix<int64_t, int64_t>(res, 1, bk);
        auto d_distances = raft::make_device_matrix<float,   int64_t>(res, 1, bk);

        if (impl->precision == 1 /* float16 */) {
            auto d_queries = raft::make_device_matrix<half, int64_t>(res, (int64_t)1, (int64_t)dim);
            {
                std::vector<half> h_q(dim);
                for (int i = 0; i < dim; i++)
                    h_q[i] = __float2half(query_vec[i]);
                raft::copy(d_queries.data_handle(), h_q.data(), dim, res.get_stream());
            }
            cuvs::neighbors::brute_force::search(
                res, *impl->idx_f16,
                raft::make_const_mdspan(d_queries.view()),
                d_indices.view(),
                d_distances.view());
        } else {
            auto d_queries = raft::make_device_matrix<float, int64_t>(res, (int64_t)1, (int64_t)dim);
            raft::copy(d_queries.data_handle(), query_vec, dim, res.get_stream());
            cuvs::neighbors::brute_force::search(
                res, *impl->idx_f32,
                raft::make_const_mdspan(d_queries.view()),
                d_indices.view(),
                d_distances.view());
        }

        res.sync_stream();

        std::vector<int64_t> h_indices(bk);
        std::vector<float>   h_distances(bk);
        raft::copy(h_indices.data(),   d_indices.data_handle(),   bk, res.get_stream());
        raft::copy(h_distances.data(), d_distances.data_handle(), bk, res.get_stream());
        res.sync_stream();

        for (int i = 0; i < bk; i++) {
            results[i].item_id  = h_indices[i];
            results[i].distance = h_distances[i];
        }
        pad_results(results, bk, top_k, impl->metric);
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_bf_search] exception: %s\n", e.what());
        _pr.poison();
        return 1;
    } catch (...) {
        fprintf(stderr, "[cuvs_bf_search] unknown exception\n");
        _pr.poison();
        return 1;
    }
}

extern "C" void
cuvs_bf_free(CuvsBfIndex index, int device_id)
{
    if (index) {
        if (cudaSetDevice(device_id) != cudaSuccess)
            fprintf(stderr, "[cuvs_bf_free] cudaSetDevice(%d) failed\n", device_id);
        delete static_cast<CuvsBfIndexImpl *>(index);
    }
}

/* ----------------------------------------------------------------
 * 3O: GPU BF search with BITSET prefilter
 * ---------------------------------------------------------------- */
extern "C" int
cuvs_bf_search_filtered(
    CuvsBfIndex       index,
    const float      *query_vec,
    int               dim,
    int               top_k,
    const uint32_t   *bitset_words,
    int64_t           bitset_bits,
    CuvsSearchResult *results,
    int               device_id)
{
    if (!index)
        return 1;

    CuvsBfIndexImpl *impl = static_cast<CuvsBfIndexImpl *>(index);
    if (dim != impl->dim)
        return 2;
    /* Both precisions are searchable below (the search call branches on
     * impl->precision exactly as cuvs_bf_search does). Only a build that left
     * the active variant null gets refused — that would be corruption, and a
     * null deref here runs in the daemon, so it takes down every backend's
     * session rather than just this query.
     *
     * 1, not a new code: cuvs_wrapper.h documents only 0/2/1 for this entry
     * point, and the CPU shim — which has no precision concept at all — can
     * never produce anything outside that set. */
    if (impl->precision == 1 ? (impl->idx_f16 == nullptr)
                             : (impl->idx_f32 == nullptr))
        return 1;
    /* The bitset is dereferenced unconditionally below; the CPU shim refuses
     * NULL the same way rather than reading it as "no filter" (#110). */
    if (!bitset_words)
        return 1;
    if (top_k <= 0)
        return 0;
    int bk = top_k;
    if ((int64_t)bk > impl->n)
        bk = (int)impl->n;
    if (bk <= 0) {
        pad_results(results, 0, top_k, impl->metric);
        return 0;
    }

    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();

        auto d_indices   = raft::make_device_matrix<int64_t, int64_t>(res, 1, bk);
        auto d_distances = raft::make_device_matrix<float,   int64_t>(res, 1, bk);
        /* store_result() below trusts an in-range id to mean "cuVS wrote a
         * real neighbour here". That is false when the filter excludes more
         * than bk-n_pass candidates: unlike CAGRA, brute_force::search does
         * not write anything to a slot it has nothing to put there, leaving
         * whatever raft::make_device_matrix's allocator handed back — device
         * memory the pool may have reused from an unrelated prior call.
         * Measured on an A100: reinterpreted distance pairs from a previous
         * search (e.g. 0x4110000041100000, decoding as float(9.0),float(9.0))
         * land in-range and are indistinguishable from a genuine hit by value
         * alone. Seed every slot to CUVS_PAD_ITEM_ID first so an untouched
         * slot is deterministically caught by store_result()'s id<0 branch
         * regardless of pool history; a slot cuVS does write overwrites this.
         * CUVS_PAD_ITEM_ID is int64_t(-1), whose bit pattern is all-0xFF, so a
         * raw byte memset produces the exact value. Applies to both precision
         * branches below — same d_indices buffer either way. */
        cudaMemsetAsync(d_indices.data_handle(), 0xFF,
                         (size_t)bk * sizeof(int64_t), res.get_stream());

        /* Upload bitset. pg_cuvs builds bit=1 = EXCLUDE (daemon memsets 0xFF then
         * clears kept items); cuVS bitset_filter keeps SET bits (bit=1 = INCLUDE),
         * so invert at this cuVS boundary. Padding bits past bitset_bits are ignored
         * by bitset_view (bounded to bitset_bits). */
        int64_t n_words = (bitset_bits + 31) / 32;
        std::vector<uint32_t> inv_bits((size_t)n_words);
        for (int64_t w = 0; w < n_words; w++)
            inv_bits[w] = ~bitset_words[w];
        auto d_bs_data = raft::make_device_vector<uint32_t, int64_t>(res, n_words);
        raft::copy(d_bs_data.data_handle(), inv_bits.data(), (size_t)n_words, res.get_stream());

        auto bv        = cuvs::core::bitset_view<uint32_t, int64_t>(
                             d_bs_data.data_handle(), bitset_bits);
        auto prefilter = cuvs::neighbors::filtering::bitset_filter<uint32_t, int64_t>(bv);

        /* Filtered search. The query is uploaded inside each branch because the
         * device matrix element type follows the index variant — half for a
         * float16 index, float otherwise. Same shape as cuvs_bf_search; the
         * `.vectors` sidecar on disk is always float32, so precision only ever
         * affects the device-resident copy and the query conversion. */
        cuvs::neighbors::brute_force::search_params params;
        if (impl->precision == 1 /* float16 */) {
            auto d_queries = raft::make_device_matrix<half, int64_t>(res, (int64_t)1, (int64_t)dim);
            {
                std::vector<half> h_q(dim);
                for (int i = 0; i < dim; i++)
                    h_q[i] = __float2half(query_vec[i]);
                raft::copy(d_queries.data_handle(), h_q.data(), dim, res.get_stream());
            }
            cuvs::neighbors::brute_force::search(
                res, params, *impl->idx_f16,
                raft::make_const_mdspan(d_queries.view()),
                d_indices.view(),
                d_distances.view(),
                prefilter);
        } else {
            auto d_queries = raft::make_device_matrix<float, int64_t>(res, (int64_t)1, (int64_t)dim);
            raft::copy(d_queries.data_handle(), query_vec, dim, res.get_stream());
            cuvs::neighbors::brute_force::search(
                res, params, *impl->idx_f32,
                raft::make_const_mdspan(d_queries.view()),
                d_indices.view(),
                d_distances.view(),
                prefilter);
        }

        res.sync_stream();

        std::vector<int64_t> h_indices(bk);
        std::vector<float>   h_distances(bk);
        raft::copy(h_indices.data(),   d_indices.data_handle(),   bk, res.get_stream());
        raft::copy(h_distances.data(), d_distances.data_handle(), bk, res.get_stream());
        res.sync_stream();

        for (int i = 0; i < bk; i++)
            store_result(&results[i], h_indices[i], h_distances[i],
                         impl->n, impl->metric);
        pad_results(results, bk, top_k, impl->metric);
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_bf_search_filtered] exception: %s\n", e.what());
        _pr.poison();
        return 1;
    } catch (...) {
        fprintf(stderr, "[cuvs_bf_search_filtered] unknown exception\n");
        _pr.poison();
        return 1;
    }
}

/* ----------------------------------------------------------------
 * 3O: GPU CAGRA search with BITSET prefilter
 * ---------------------------------------------------------------- */
extern "C" int
cuvs_cagra_search_filtered(
    CuvsCagraIndex    index,
    const float      *query_vec,
    int               dim,
    int               top_k,
    const uint32_t   *bitset_words,
    int64_t           bitset_bits,
    CuvsSearchResult *results,
    int               device_id,
    uint32_t          itopk_size,
    uint32_t          search_width)
{
    if (!index)
        return 1;

    CuvsCagraIndexImpl *impl = static_cast<CuvsCagraIndexImpl *>(index);
    if ((int64_t)dim != impl->idx.dim())
        return 2;
    /* The bitset is dereferenced unconditionally below; the CPU shim refuses
     * NULL the same way rather than reading it as "no filter" (#110). */
    if (!bitset_words)
        return 1;
    if (top_k <= 0)
        return 0;

    int64_t n = (int64_t)impl->idx.dataset().extent(0);
    int bk = top_k;
    if ((int64_t)bk > n)
        bk = (int)n;
    if (bk <= 0) {
        pad_results(results, 0, top_k, impl->metric);
        return 0;
    }

    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();

        auto d_queries   = raft::make_device_matrix<float,    int64_t>(res, (int64_t)1, (int64_t)dim);
        auto d_indices   = raft::make_device_matrix<uint32_t, int64_t>(res, 1, bk);
        auto d_distances = raft::make_device_matrix<float,    int64_t>(res, 1, bk);
        /* cagra::search has, on an A100, been observed to write a deterministic
         * out-of-range marker (0xFFFFFFFF) to every excluded slot rather than
         * leaving pool memory untouched (see cuvs_bf_search_filtered above for
         * the sibling entry point where that assumption is measurably false).
         * That is this build's empirical behaviour, not a documented cuVS
         * contract — seed the same way so store_result()'s correctness does
         * not depend on it. A slot cuVS does write overwrites this. */
        cudaMemsetAsync(d_indices.data_handle(), 0xFF,
                         (size_t)bk * sizeof(uint32_t), res.get_stream());
        raft::copy(d_queries.data_handle(), query_vec, dim, res.get_stream());

        /* Upload bitset. pg_cuvs builds bit=1 = EXCLUDE; cuVS bitset_filter keeps
         * SET bits (bit=1 = INCLUDE), so invert at this cuVS boundary. Padding bits
         * past bitset_bits are ignored by bitset_view (bounded to bitset_bits). */
        int64_t n_words = (bitset_bits + 31) / 32;
        std::vector<uint32_t> inv_bits((size_t)n_words);
        for (int64_t w = 0; w < n_words; w++)
            inv_bits[w] = ~bitset_words[w];
        auto d_bs_data = raft::make_device_vector<uint32_t, int64_t>(res, n_words);
        raft::copy(d_bs_data.data_handle(), inv_bits.data(), (size_t)n_words, res.get_stream());

        auto bv        = cuvs::core::bitset_view<uint32_t, int64_t>(
                             d_bs_data.data_handle(), bitset_bits);
        auto prefilter = cuvs::neighbors::filtering::bitset_filter<uint32_t, int64_t>(bv);

        cuvs::neighbors::cagra::search_params sparams;
        sparams.itopk_size = cuvs_cagra_itopk_size(itopk_size, (uint32_t) bk);
        if (search_width) sparams.search_width = search_width;

        cuvs::neighbors::cagra::search(
            res, sparams, impl->idx,
            raft::make_const_mdspan(d_queries.view()),
            d_indices.view(),
            d_distances.view(),
            prefilter);

        res.sync_stream();

        std::vector<uint32_t> h_indices(bk);
        std::vector<float>    h_distances(bk);
        raft::copy(h_indices.data(),   d_indices.data_handle(),   bk, res.get_stream());
        raft::copy(h_distances.data(), d_distances.data_handle(), bk, res.get_stream());
        res.sync_stream();

        for (int i = 0; i < bk; i++)
            store_result(&results[i], (int64_t)h_indices[i], h_distances[i],
                         n, impl->metric);
        pad_results(results, bk, top_k, impl->metric);
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_cagra_search_filtered] exception: %s\n", e.what());
        _pr.poison();
        return 1;
    } catch (...) {
        fprintf(stderr, "[cuvs_cagra_search_filtered] unknown exception\n");
        _pr.poison();
        return 1;
    }
}

/* ----------------------------------------------------------------
 * CAGRA index build
 * ---------------------------------------------------------------- */
/* Map a CUVS_METRIC_* code to the cuVS DistanceType baked into the graph. */
static cuvs::distance::DistanceType
cuvs_distance_type(uint32_t metric)
{
    switch (metric) {
        case CUVS_METRIC_COSINE: return cuvs::distance::DistanceType::CosineExpanded;
        case CUVS_METRIC_IP:     return cuvs::distance::DistanceType::InnerProduct;
        case CUVS_METRIC_L2:
        default:                 return cuvs::distance::DistanceType::L2Expanded;
    }
}

/* ADR-059: build a CAGRA index from N host partitions WITHOUT host-side
 * concatenation. One device matrix [total][dim] is allocated and each partition
 * is copied to its row offset (N H2D copies vs one). This lets the daemon stream
 * parallel-build worker partials (separate named-shm segments) straight to the
 * GPU, eliminating the leader's merge copy (ADR-058 bottleneck).
 *
 * Correctness: CAGRA is order-independent and the daemon pairs (vector[i],
 * tid[i]) positionally, so concatenating partitions (each partition's pairs kept
 * in order) is equivalent to a single contiguous corpus. n_parts==1 reduces to a
 * single offset-0 copy — byte-identical to the legacy single-corpus build. */
extern "C" CuvsCagraIndex
cuvs_cagra_build_multi(const float **vecs, const int64_t *n_each, int n_parts,
                       int64_t total, int dim, uint32_t metric,
                       int graph_degree, int intermediate_graph_degree,
                       uint32_t build_algo, int device_id,
                       uint32_t lw_overflow, CuvsLwBuildStats *lw_out)
{
    CuvsLwBuildStats lw_local;
    CuvsLwBuildStats *lw = lw_out ? lw_out : &lw_local;
    std::memset(lw, 0, sizeof(*lw));
    lw->free_at_peak = UINT64_MAX;
    g_last_build_oom = 0;   /* fresh per build attempt */
    g_last_build_failed_bytes = -1;
    cuvs_failed_allocation::clear();
    g_last_build_att = 0;
    PooledRes _pr(device_id);
    LwWorkspaceOps lw_ops{_pr, lw, nullptr};
    try {
        /* I4: install right after acquiring the resources; the guard unwinds
         * before the catch blocks below and before PooledRes returns them. */
        CuvsLwWorkspaceGuard<LwWorkspaceOps> lw_guard(lw_ops, lw_overflow);

        /* test-only: simulate a VRAM OOM to exercise the daemon evict+retry path. */
        if (g_inject_build_oom.load() > 0) {
            g_inject_build_oom.fetch_sub(1);
            throw std::bad_alloc();
        }

        raft::device_resources &res = _pr.get();

        /* One device matrix for the whole corpus; moved into the impl below so
         * its memory stays alive for the index's lifetime. */
        auto d_corpus = raft::make_device_matrix<float, int64_t>(res, total, (int64_t)dim);
        int64_t off = 0;   /* running row offset into d_corpus */
        for (int i = 0; i < n_parts; i++) {
            if (n_each[i] <= 0)
                continue;   /* empty worker share: nothing to copy */
            raft::copy(d_corpus.data_handle() + off * (int64_t)dim,
                       vecs[i], n_each[i] * (int64_t)dim, res.get_stream());
            off += n_each[i];
        }
        res.sync_stream();

        /* CAGRA build parameters. metric is baked into the graph; search inherits
         * it. graph_degree / intermediate_graph_degree / build_algo come from the
         * index's reloptions (3R) — <=0 / AUTO keep cuVS defaults. */
        cuvs::neighbors::cagra::index_params params;
        params.metric                = cuvs_distance_type(metric);
        params.graph_degree              = (graph_degree > 0)
                                         ? (size_t)graph_degree : 64;
        params.intermediate_graph_degree = (intermediate_graph_degree > 0)
                                         ? (size_t)intermediate_graph_degree : 128;

        /* build_algo: graph-construction algorithm (cuVS 26.04 graph_build_params
         * std::variant). AUTO leaves std::monostate so cuVS picks heuristically
         * (ivf_pq for large, nn_descent for small). */
        namespace gbp = cuvs::neighbors::cagra::graph_build_params;
        /* IVF-PQ kNN precision (cuvs_hnsw_ivfpq_needs_fp32): the corpus is
         * already on the GPU, so the bound costs one reduction, not a host
         * pass. fp16 data keeps every parameter as before. */
        const bool may_use_ivf_pq = build_algo == CUVS_CAGRA_BUILD_IVF_PQ
            || build_algo == CUVS_CAGRA_BUILD_AUTO;
        const double max_sq = may_use_ivf_pq
            ? device_max_sq_norm(res, d_corpus.data_handle(), total, (int64_t)dim) : 0.0;
        const bool fp32 = may_use_ivf_pq && cuvs_hnsw_ivfpq_needs_fp32(max_sq, metric);
        auto fp32_ivf_pq = [&](gbp::ivf_pq_params pq) {
            pq.search_params.lut_dtype = CUDA_R_32F;
            pq.search_params.internal_distance_dtype = CUDA_R_32F;
            pq.search_params.coarse_search_dtype = CUDA_R_32F;
            return pq;
        };
        const char *knn = "auto";
        if (build_algo == CUVS_CAGRA_BUILD_IVF_PQ)
        {
            auto pq = gbp::ivf_pq_params(d_corpus.extents());
            params.graph_build_params = fp32 ? fp32_ivf_pq(pq) : pq;
            knn = "ivf_pq";
        }
        else if (build_algo == CUVS_CAGRA_BUILD_NN_DESCENT)
            params.graph_build_params =
                gbp::nn_descent_params(params.intermediate_graph_degree);
        else if (fp32)
        {
            /* AUTO with fp16-unsafe data: make cuVS's own choice once
             * (cagra_build.cuh: NN-descent when it fits the device, else
             * IVF-PQ from the dataset shape) and pin it, with fp32 IVF-PQ. */
            if (cuvs::neighbors::nn_descent::has_enough_device_memory(res, d_corpus.extents(),
                    sizeof(uint32_t)))
            {
                params.graph_build_params = gbp::nn_descent_params(
                    params.intermediate_graph_degree, params.metric);
                knn = "nn_descent";
            }
            else
            {
                params.graph_build_params = fp32_ivf_pq(
                    gbp::ivf_pq_params(d_corpus.extents(), params.metric));
                knn = "ivf_pq";
            }
        }
        /* else CUVS_CAGRA_BUILD_AUTO with fp16-safe data: leave monostate. */
        if (may_use_ivf_pq)
            fprintf(stderr, "[cuvs_cagra_build_multi] knn precision max_sq_norm=%.9g fp16_max=%.0f "
                            "knn_builder=%s ivfpq_distance_dtype=%s\n",
                    max_sq, CUVS_HNSW_FP16_MAX, knn, fp32 ? "fp32" : "fp16");

        auto idx = cuvs::neighbors::cagra::build(
            res,
            params,
            raft::make_const_mdspan(d_corpus.view()));
        _pr.fence_check();

        /* I3 (v6 C3, CK-30): cuVS keeps the dataset in rows of
         * ld = round_up(dim, 4) floats (16-byte alignment, make_aligned_dataset).
         * A contiguous dim % 4 != 0 corpus made update_dataset copy it into an
         * owning padded matrix while d_corpus stayed alive: two copies for the
         * index lifetime. Move the rows into a zero-padded [total x ld] matrix
         * and attach it as a strided view, which cuVS takes without copying
         * (stride == required stride); the contiguous copy is freed on return. */
        const int64_t ld = ((int64_t)dim + 3) / 4 * 4;
        if (ld != (int64_t)dim) {
            auto d_padded = raft::make_device_matrix<float, int64_t>(res, total, ld);
            RAFT_CUDA_TRY(cudaMemsetAsync(d_padded.data_handle(), 0,
                                          (size_t)total * (size_t)ld * sizeof(float),
                                          res.get_stream()));
            RAFT_CUDA_TRY(cudaMemcpy2DAsync(d_padded.data_handle(), (size_t)ld * sizeof(float),
                                            d_corpus.data_handle(),
                                            (size_t)dim * sizeof(float),
                                            (size_t)dim * sizeof(float), (size_t)total,
                                            cudaMemcpyDeviceToDevice, res.get_stream()));
            idx.update_dataset(res, raft::make_device_strided_matrix_view<const float, int64_t>(
                                        d_padded.data_handle(), total, (int64_t)dim, ld));
            res.sync_stream();
            if (idx.dataset().extent(0) != total) {   /* C5 ATT */
                fprintf(stderr, "[cuvs_cagra_build_multi] dataset not attached (%lld of %lld rows)\n",
                        (long long)idx.dataset().extent(0), (long long)total);
                g_last_build_att = 1;
                return nullptr;
            }
            return new CuvsCagraIndexImpl(std::move(d_padded), std::move(idx), metric);
        }

        /* Re-attach dataset explicitly so the index owns a valid view that
         * matches our retained d_corpus memory. */
        idx.update_dataset(res, raft::make_const_mdspan(d_corpus.view()));
        res.sync_stream();
        /* C5 ATT: cuVS only warns when it cannot attach the dataset and returns
         * a graph-only index; a CAGRA index without its rows cannot be searched. */
        if (idx.dataset().extent(0) != total) {
            fprintf(stderr, "[cuvs_cagra_build_multi] dataset not attached (%lld of %lld rows)\n",
                    (long long)idx.dataset().extent(0), (long long)total);
            g_last_build_att = 1;
            return nullptr;
        }

        return new CuvsCagraIndexImpl(std::move(d_corpus), std::move(idx), metric);
    } catch (const std::bad_alloc &) {
        g_last_build_failed_bytes = cuvs_failed_allocation::take_for_current_exception();
        /* OOM (incl. RMM out_of_memory). Flag it so the daemon reports an
         * after-admission OOM without evicting (ADR-086). */
        fprintf(stderr, "[cuvs_cagra_build_multi] out of memory\n");
        g_last_build_oom = 1;
        _pr.poison();
        return nullptr;
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_cagra_build_multi] exception: %s\n", e.what());
        _pr.poison();
        return nullptr;
    } catch (...) {
        fprintf(stderr, "[cuvs_cagra_build_multi] unknown exception\n");
        _pr.poison();
        return nullptr;
    }
}

extern "C" CuvsCagraIndex
cuvs_cagra_build(const float *vecs, int64_t n_vecs, int dim, uint32_t metric,
                 int graph_degree, int intermediate_graph_degree,
                 uint32_t build_algo, int device_id,
                 uint32_t lw_overflow, CuvsLwBuildStats *lw_out)
{
    /* Single-partition special case of the multi-partition build. */
    const float  *parts[1]  = { vecs };
    const int64_t n_each[1] = { n_vecs };
    return cuvs_cagra_build_multi(parts, n_each, 1, n_vecs, dim, metric,
                                  graph_degree, intermediate_graph_degree,
                                  build_algo, device_id, lw_overflow, lw_out);
}

/* ----------------------------------------------------------------
 * CAGRA index search
 * ---------------------------------------------------------------- */
extern "C" int
cuvs_cagra_search(
    CuvsCagraIndex   index,
    const float     *query_vec,
    int              dim,
    int              top_k,
    CuvsSearchResult *results,
    int              device_id,
    uint32_t         itopk_size,
    uint32_t         search_width)
{
    if (!index)
        return 1;

    CuvsCagraIndexImpl *impl = static_cast<CuvsCagraIndexImpl *>(index);

    /* Guard: a query whose dimension differs from the index dimension makes
     * cuVS throw a RAFT failure mid-search, and the resulting sticky CUDA
     * error has aborted the entire daemon (SIGABRT). Refuse it here, before
     * touching the GPU. Return 2 = dimension mismatch (distinct from 1). */
    if ((int64_t)dim != impl->idx.dim()) {
        fprintf(stderr,
                "[cuvs_cagra_search] dim mismatch: query=%d index=%lld; refusing\n",
                dim, (long long)impl->idx.dim());
        return 2;
    }

    if (top_k <= 0)
        return 0;
    /* Clamp to the corpus and pad the tail, as every sibling entry point does —
     * this one had neither guard, so an oversized k reached cagra::search
     * unclamped (#103). */
    int bk = top_k;
    if ((int64_t)bk > (int64_t)impl->idx.size())
        bk = (int)impl->idx.size();
    if (bk <= 0) {
        pad_results(results, 0, top_k, impl->metric);
        return 0;
    }

    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();

        auto d_queries = raft::make_device_matrix<float, int64_t>(res, (int64_t)1, (int64_t)dim);
        raft::copy(d_queries.data_handle(), query_vec, dim, res.get_stream());

        auto d_indices   = raft::make_device_matrix<uint32_t, int64_t>(res, 1, bk);
        auto d_distances = raft::make_device_matrix<float,    int64_t>(res, 1, bk);

        cuvs::neighbors::cagra::search_params sparams;
        /* Keep the legacy automatic buffer when unset, and always cover the
         * actual dispatch k (including shard/tombstone overfetch). */
        sparams.itopk_size = cuvs_cagra_itopk_size(itopk_size, (uint32_t) bk);
        if (search_width) sparams.search_width = search_width;

        cuvs::neighbors::cagra::search(
            res, sparams, impl->idx,
            raft::make_const_mdspan(d_queries.view()),
            d_indices.view(),
            d_distances.view());

        res.sync_stream();

        std::vector<uint32_t> h_indices(bk);
        std::vector<float>    h_distances(bk);
        raft::copy(h_indices.data(),   d_indices.data_handle(),   bk, res.get_stream());
        raft::copy(h_distances.data(), d_distances.data_handle(), bk, res.get_stream());
        res.sync_stream();

        for (int i = 0; i < bk; i++) {
            results[i].item_id  = (int64_t)h_indices[i];
            results[i].distance = h_distances[i];
        }
        pad_results(results, bk, top_k, impl->metric);
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_cagra_search] exception: %s\n", e.what());
        _pr.poison();
        return 1;
    } catch (...) {
        fprintf(stderr, "[cuvs_cagra_search] unknown exception\n");
        _pr.poison();
        return 1;
    }
}

/* ----------------------------------------------------------------
 * Phase 3M: batched search — Q queries in one GPU dispatch.
 * queries is Q*dim row-major; results is Q*top_k row-major
 * (results[q*top_k + j] = j-th neighbor of query q). One kernel launch for the
 * whole batch (cuVS cagra/brute_force search take a Q×dim query matrix), so the
 * fixed per-call overhead is amortized across Q. The caller must pass
 * top_k <= corpus size.
 * ---------------------------------------------------------------- */
extern "C" int
cuvs_cagra_search_batch(
    CuvsCagraIndex   index,
    const float     *queries,
    int              n_queries,
    int              dim,
    int              top_k,
    CuvsSearchResult *results,
    int              device_id,
    uint32_t         itopk_size,
    uint32_t         search_width)
{
    if (!index)
        return 1;
    if (n_queries <= 0 || top_k <= 0)
        return 0;

    CuvsCagraIndexImpl *impl = static_cast<CuvsCagraIndexImpl *>(index);
    if ((int64_t)dim != impl->idx.dim()) {
        fprintf(stderr,
                "[cuvs_cagra_search_batch] dim mismatch: query=%d index=%lld; refusing\n",
                dim, (long long)impl->idx.dim());
        return 2;
    }

    /* Clamp to the corpus; the per-query tail slots [bk, top_k) get the padding
     * sentinel (TAIL CONTRACT), as in cuvs_bf_search_batch. */
    int bk = top_k;
    if ((int64_t)bk > (int64_t)impl->idx.size())
        bk = (int)impl->idx.size();
    if (bk <= 0) {
        for (int q = 0; q < n_queries; q++)
            pad_results(results + (size_t)q * top_k, 0, top_k, impl->metric);
        return 0;
    }

    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();
        int64_t Q = n_queries;
        size_t  n = (size_t)Q * (size_t)bk;

        auto d_queries = raft::make_device_matrix<float, int64_t>(res, Q, (int64_t)dim);
        raft::copy(d_queries.data_handle(), queries, (size_t)Q * dim, res.get_stream());

        auto d_indices   = raft::make_device_matrix<uint32_t, int64_t>(res, Q, bk);
        auto d_distances = raft::make_device_matrix<float,    int64_t>(res, Q, bk);

        cuvs::neighbors::cagra::search_params sparams;
        sparams.itopk_size = cuvs_cagra_itopk_size(itopk_size, (uint32_t) bk);
        if (search_width) sparams.search_width = search_width;

        cuvs::neighbors::cagra::search(
            res, sparams, impl->idx,
            raft::make_const_mdspan(d_queries.view()),
            d_indices.view(),
            d_distances.view());
        res.sync_stream();

        std::vector<uint32_t> h_indices(n);
        std::vector<float>    h_distances(n);
        raft::copy(h_indices.data(),   d_indices.data_handle(),   n, res.get_stream());
        raft::copy(h_distances.data(), d_distances.data_handle(), n, res.get_stream());
        res.sync_stream();

        for (int64_t q = 0; q < Q; q++) {
            for (int j = 0; j < bk; j++) {
                CuvsSearchResult *r = &results[(size_t)q * top_k + j];
                r->item_id  = (int64_t)h_indices[(size_t)q * bk + j];
                r->distance = h_distances[(size_t)q * bk + j];
            }
            pad_results(results + (size_t)q * top_k, bk, top_k, impl->metric);
        }
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_cagra_search_batch] exception: %s\n", e.what());
        _pr.poison();
        return 1;
    } catch (...) {
        fprintf(stderr, "[cuvs_cagra_search_batch] unknown exception\n");
        _pr.poison();
        return 1;
    }
}

extern "C" int
cuvs_bf_search_batch(
    CuvsBfIndex      index,
    const float     *queries,
    int              n_queries,
    int              dim,
    int              top_k,
    CuvsSearchResult *results,
    int              device_id)
{
    if (!index)
        return 1;
    if (n_queries <= 0 || top_k <= 0)
        return 0;

    CuvsBfIndexImpl *impl = static_cast<CuvsBfIndexImpl *>(index);
    if (dim != impl->dim)
        return 2;
    /* brute_force cannot return more neighbors than the corpus holds; the tail
     * slots [bk, top_k) are padded with a sentinel so the Q*top_k layout holds. */
    int bk = top_k;
    if ((int64_t)bk > impl->n)
        bk = (int)impl->n;
    if (bk <= 0) {
        for (int q = 0; q < n_queries; q++)
            pad_results(results + (size_t)q * top_k, 0, top_k, impl->metric);
        return 0;
    }

    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();
        int64_t Q = n_queries;
        size_t  nb = (size_t)Q * (size_t)bk;

        auto d_indices   = raft::make_device_matrix<int64_t, int64_t>(res, Q, bk);
        auto d_distances = raft::make_device_matrix<float,   int64_t>(res, Q, bk);

        if (impl->precision == 1 /* float16 */) {
            auto d_q = raft::make_device_matrix<half, int64_t>(res, Q, (int64_t)dim);
            {
                std::vector<half> h_q((size_t)Q * dim);
                for (size_t i = 0; i < (size_t)Q * dim; i++)
                    h_q[i] = __float2half(queries[i]);
                raft::copy(d_q.data_handle(), h_q.data(), (size_t)Q * dim, res.get_stream());
            }
            cuvs::neighbors::brute_force::search(
                res, *impl->idx_f16,
                raft::make_const_mdspan(d_q.view()),
                d_indices.view(),
                d_distances.view());
        } else {
            auto d_q = raft::make_device_matrix<float, int64_t>(res, Q, (int64_t)dim);
            raft::copy(d_q.data_handle(), queries, (size_t)Q * dim, res.get_stream());
            cuvs::neighbors::brute_force::search(
                res, *impl->idx_f32,
                raft::make_const_mdspan(d_q.view()),
                d_indices.view(),
                d_distances.view());
        }
        res.sync_stream();

        std::vector<int64_t> h_indices(nb);
        std::vector<float>   h_distances(nb);
        raft::copy(h_indices.data(),   d_indices.data_handle(),   nb, res.get_stream());
        raft::copy(h_distances.data(), d_distances.data_handle(), nb, res.get_stream());
        res.sync_stream();

        for (int64_t q = 0; q < Q; q++) {
            for (int j = 0; j < bk; j++) {
                CuvsSearchResult *r = &results[(size_t)q * top_k + j];
                r->item_id  = h_indices[(size_t)q * bk + j];
                r->distance = h_distances[(size_t)q * bk + j];
            }
            pad_results(results + (size_t)q * top_k, bk, top_k, impl->metric);
        }
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_bf_search_batch] exception: %s\n", e.what());
        _pr.poison();
        return 1;
    } catch (...) {
        fprintf(stderr, "[cuvs_bf_search_batch] unknown exception\n");
        _pr.poison();
        return 1;
    }
}

/* ----------------------------------------------------------------
 * CAGRA index serialize / deserialize
 * ---------------------------------------------------------------- */
extern "C" int
cuvs_cagra_serialize(CuvsCagraIndex index, const char *path, int device_id)
{
    if (!index)
        return 1;

    try {
        CuvsCagraIndexImpl *impl = static_cast<CuvsCagraIndexImpl *>(index);
        PooledRes _pr(device_id); raft::device_resources &res = _pr.get();
        /* include_dataset=true: works now because impl->dataset keeps the
         * device memory alive that idx's view points to. */
        cuvs::neighbors::cagra::serialize(res, std::string(path), impl->idx, true);
        res.sync_stream();
        _pr.fence_check();
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_cagra_serialize] exception: %s\n", e.what());
        return 1;
    } catch (...) {
        fprintf(stderr, "[cuvs_cagra_serialize] unknown exception\n");
        return 1;
    }
}

extern "C" CuvsCagraIndex
cuvs_cagra_deserialize(const char *path, int dim, int device_id)
{
    try {
        PooledRes _pr(device_id); raft::device_resources &res = _pr.get();
        cuvs::neighbors::cagra::index<float, uint32_t> idx(res);
        cuvs::neighbors::cagra::deserialize(res, path, &idx);
        res.sync_stream();
        _pr.fence_check();
        (void)dim;  /* dim is encoded in the serialized index */

        /* After deserialize with include_dataset=true at save time, the index
         * owns its own dataset (allocated internally during deserialize).
         * We pass an empty placeholder device_matrix so the impl struct's
         * field stays valid; idx's internal dataset is what matters. */
        auto empty = raft::make_device_matrix<float, int64_t>(res, (int64_t)0, (int64_t)0);
        res.sync_stream();
        /* The metric is baked into the serialized graph; recover our code for
         * the tail contract's padding sentinel. */
        uint32_t met = cuvs_metric_code(idx.metric());
        return new CuvsCagraIndexImpl(std::move(empty), std::move(idx), met);
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_cagra_deserialize] exception: %s\n", e.what());
        return nullptr;
    } catch (...) {
        fprintf(stderr, "[cuvs_cagra_deserialize] unknown exception\n");
        return nullptr;
    }
}

/* ----------------------------------------------------------------
 * CAGRA index free
 * ---------------------------------------------------------------- */
extern "C" void
cuvs_cagra_free(CuvsCagraIndex index, int device_id)
{
    if (index) {
        if (cudaSetDevice(device_id) != cudaSuccess)
            fprintf(stderr, "[cuvs_cagra_free] cudaSetDevice(%d) failed\n", device_id);
        delete static_cast<CuvsCagraIndexImpl *>(index);
    }
}

/* ----------------------------------------------------------------
 * 3Q: cuvs_cagra_extend — in-place extend a CAGRA index with n_new vectors.
 *
 * After cagra::extend (with nullopt buffer views), cuVS internally reallocates
 * the dataset to hold old + new vectors. impl->dataset is the OLD allocation and
 * is no longer referenced by idx after extend; we replace it with an empty matrix
 * to release the VRAM rather than holding both old and new in parallel.
 * ---------------------------------------------------------------- */
extern "C" int
cuvs_cagra_extend(CuvsCagraIndex index,
                  const float   *new_vecs,
                  int64_t        n_new,
                  int            dim,
                  uint32_t       max_chunk_size,
                  int            device_id)
{
    if (!index || !new_vecs || n_new <= 0)
        return 1;

    CuvsCagraIndexImpl *impl = static_cast<CuvsCagraIndexImpl *>(index);
    if ((int64_t)dim != impl->idx.dim())
        return 2;

    PooledRes _pr(device_id);
    try {
        /* test-only: inject bad_alloc to exercise the _pr.poison() / BUILD_FAILED path */
        if (g_inject_extend_oom.exchange(0) != 0)
            throw std::bad_alloc();

        raft::device_resources &res = _pr.get();

        /* Upload new vectors to device */
        auto d_new = raft::make_device_matrix<float, int64_t>(res, n_new, (int64_t)dim);
        raft::copy(d_new.data_handle(), new_vecs, (size_t)(n_new * (int64_t)dim),
                   res.get_stream());
        res.sync_stream();

        cuvs::neighbors::cagra::extend_params params;
        params.max_chunk_size = max_chunk_size;   /* 0 = auto */

        /* Extend in-place; cuVS reallocates the dataset buffer internally. */
        cuvs::neighbors::cagra::extend(
            res, params,
            raft::make_const_mdspan(d_new.view()),
            impl->idx);
        res.sync_stream();
        _pr.fence_check();

        /* Release the old impl->dataset (no longer referenced by idx). */
        impl->dataset = raft::make_device_matrix<float, int64_t>(res, (int64_t)0, (int64_t)0);
        res.sync_stream();

        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_cagra_extend] exception: %s\n", e.what());
        _pr.poison();
        return 1;
    } catch (...) {
        fprintf(stderr, "[cuvs_cagra_extend] unknown exception\n");
        _pr.poison();
        return 1;
    }
}

/* ----------------------------------------------------------------
 * 3Q: cuvs_cagra_compact — merge-compact a CAGRA index removing dead vectors.
 *
 * keep_bits_words: host uint32_t bitset where bit[i]=1 means "keep vector i"
 * (greenlight semantics — opposite to search prefilter's "bit=1 = exclude").
 * cagra::merge creates a NEW index; the old handle must be freed by the caller.
 * The new index manages its own dataset internally (same as deserialize path).
 * ---------------------------------------------------------------- */
extern "C" CuvsCagraIndex
cuvs_cagra_compact(CuvsCagraIndex  index,
                   const uint32_t *keep_bits_words,
                   int64_t         n_total,
                   uint32_t        metric,
                   int             device_id)
{
    if (!index || !keep_bits_words || n_total <= 0)
        return nullptr;

    CuvsCagraIndexImpl *impl = static_cast<CuvsCagraIndexImpl *>(index);

    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();

        /* Upload keep-bitset to device (bit[i]=1 → include vector i). */
        int64_t n_words = (n_total + 31) / 32;
        auto d_bs_data = raft::make_device_vector<uint32_t, int64_t>(res, n_words);
        raft::copy(d_bs_data.data_handle(), keep_bits_words, (size_t)n_words,
                   res.get_stream());

        auto bv = cuvs::core::bitset_view<uint32_t, int64_t>(
                      d_bs_data.data_handle(), n_total);
        auto filter = cuvs::neighbors::filtering::bitset_filter<uint32_t, int64_t>(bv);

        cuvs::neighbors::cagra::index_params params;
        params.metric = cuvs_distance_type(metric);

        std::vector<cuvs::neighbors::cagra::index<float, uint32_t>*> indices = {&impl->idx};
        auto new_idx = cuvs::neighbors::cagra::merge(res, params, indices, filter);
        res.sync_stream();

        /* Wrap in new impl with empty placeholder dataset (same as deserialize). */
        auto empty = raft::make_device_matrix<float, int64_t>(res, (int64_t)0, (int64_t)0);
        res.sync_stream();
        return new CuvsCagraIndexImpl(std::move(empty), std::move(new_idx), metric);
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_cagra_compact] exception: %s\n", e.what());
        _pr.poison();
        return nullptr;
    } catch (...) {
        fprintf(stderr, "[cuvs_cagra_compact] unknown exception\n");
        _pr.poison();
        return nullptr;
    }
}

/* ----------------------------------------------------------------
 * 3P: IVF-PQ index
 *
 * Stores PQ-compressed codes internally; no retained dataset member needed
 * (unlike CAGRA which holds an mdspan view into d_corpus). IdxT = int64_t
 * maps directly to CuvsSearchResult.item_id.
 * ---------------------------------------------------------------- */
struct CuvsIvfPqIndexImpl {
    cuvs::neighbors::ivf_pq::index<int64_t> idx;
    int64_t  n;    /* corpus size; used for top_k clamping */
    int      dim;
    uint32_t metric;   /* CUVS_METRIC_*; selects the tail padding sentinel */

    CuvsIvfPqIndexImpl(cuvs::neighbors::ivf_pq::index<int64_t> &&i,
                       int64_t nn, int dm, uint32_t met)
        : idx(std::move(i)), n(nn), dim(dm), metric(met) {}
};

extern "C" int
cuvs_ivfpq_build(
    const float *vecs, int64_t n_vecs, int dim,
    uint32_t metric, uint32_t n_lists, uint32_t pq_bits, uint32_t pq_dim,
    int device_id, CuvsIvfPqIndex *out)
{
    if (!vecs || !out || n_vecs <= 0 || dim <= 0)
        return 1;

    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();

        auto d_corpus = raft::make_device_matrix<float, int64_t>(res, n_vecs, (int64_t)dim);
        raft::copy(d_corpus.data_handle(), vecs, (size_t)n_vecs * dim, res.get_stream());
        res.sync_stream();

        cuvs::neighbors::ivf_pq::index_params params;
        params.metric  = cuvs_distance_type(metric);
        params.n_lists = n_lists ? n_lists : 1024;
        params.pq_bits = pq_bits ? pq_bits : 8;
        params.pq_dim  = pq_dim  ? pq_dim  : (uint32_t)((dim + 1) / 2);

        auto idx = cuvs::neighbors::ivf_pq::build(
            res, params,
            raft::make_const_mdspan(d_corpus.view()));
        res.sync_stream();
        _pr.fence_check();

        *out = new CuvsIvfPqIndexImpl(std::move(idx), n_vecs, dim, metric);
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_ivfpq_build] exception: %s\n", e.what());
        _pr.poison();
        return 1;
    } catch (...) {
        fprintf(stderr, "[cuvs_ivfpq_build] unknown exception\n");
        _pr.poison();
        return 1;
    }
}

extern "C" int
cuvs_ivfpq_search(
    CuvsIvfPqIndex    index,
    const float      *query_vec,
    int               dim,
    int               top_k,
    uint32_t          n_probes,
    CuvsSearchResult *results,
    int               device_id)
{
    if (!index)
        return 1;

    CuvsIvfPqIndexImpl *impl = static_cast<CuvsIvfPqIndexImpl *>(index);
    if (dim != impl->dim)
        return 2;
    if (top_k <= 0)
        return 0;
    int bk = top_k;
    if ((int64_t)bk > impl->n)
        bk = (int)impl->n;
    if (bk <= 0) {
        pad_results(results, 0, top_k, impl->metric);
        return 0;
    }

    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();

        auto d_queries   = raft::make_device_matrix<float,   int64_t>(res, (int64_t)1, (int64_t)dim);
        auto d_indices   = raft::make_device_matrix<int64_t, int64_t>(res, 1, bk);
        auto d_distances = raft::make_device_matrix<float,   int64_t>(res, 1, bk);
        raft::copy(d_queries.data_handle(), query_vec, dim, res.get_stream());

        cuvs::neighbors::ivf_pq::search_params sparams;
        sparams.n_probes = n_probes ? n_probes : 64;

        cuvs::neighbors::ivf_pq::search(
            res, sparams, impl->idx,
            raft::make_const_mdspan(d_queries.view()),
            d_indices.view(),
            d_distances.view());

        res.sync_stream();

        std::vector<int64_t> h_indices(bk);
        std::vector<float>   h_distances(bk);
        raft::copy(h_indices.data(),   d_indices.data_handle(),   bk, res.get_stream());
        raft::copy(h_distances.data(), d_distances.data_handle(), bk, res.get_stream());
        res.sync_stream();

        for (int i = 0; i < bk; i++) {
            results[i].item_id  = h_indices[i];
            results[i].distance = h_distances[i];
        }
        pad_results(results, bk, top_k, impl->metric);
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_ivfpq_search] exception: %s\n", e.what());
        _pr.poison();
        return 1;
    } catch (...) {
        fprintf(stderr, "[cuvs_ivfpq_search] unknown exception\n");
        _pr.poison();
        return 1;
    }
}

extern "C" int
cuvs_ivfpq_serialize(CuvsIvfPqIndex index, const char *path, int device_id)
{
    if (!index || !path)
        return 1;
    try {
        CuvsIvfPqIndexImpl *impl = static_cast<CuvsIvfPqIndexImpl *>(index);
        PooledRes _pr(device_id); raft::device_resources &res = _pr.get();
        cuvs::neighbors::ivf_pq::serialize(res, std::string(path), impl->idx);
        res.sync_stream();
        _pr.fence_check();
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_ivfpq_serialize] exception: %s\n", e.what());
        return 1;
    } catch (...) {
        fprintf(stderr, "[cuvs_ivfpq_serialize] unknown exception\n");
        return 1;
    }
}

extern "C" CuvsIvfPqIndex
cuvs_ivfpq_deserialize(const char *path, int device_id)
{
    if (!path)
        return nullptr;
    try {
        PooledRes _pr(device_id); raft::device_resources &res = _pr.get();
        cuvs::neighbors::ivf_pq::index<int64_t> idx(res);
        cuvs::neighbors::ivf_pq::deserialize(res, std::string(path), &idx);
        res.sync_stream();
        _pr.fence_check();
        int64_t  n   = (int64_t)idx.size();
        int      dim = (int)idx.dim();
        uint32_t met = cuvs_metric_code(idx.metric());
        return new CuvsIvfPqIndexImpl(std::move(idx), n, dim, met);
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_ivfpq_deserialize] exception: %s\n", e.what());
        return nullptr;
    } catch (...) {
        fprintf(stderr, "[cuvs_ivfpq_deserialize] unknown exception\n");
        return nullptr;
    }
}

extern "C" void
cuvs_ivfpq_free(CuvsIvfPqIndex index, int device_id)
{
    if (index) {
        if (cudaSetDevice(device_id) != cudaSuccess)
            fprintf(stderr, "[cuvs_ivfpq_free] cudaSetDevice(%d) failed\n", device_id);
        delete static_cast<CuvsIvfPqIndexImpl *>(index);
    }
}

/* ----------------------------------------------------------------
 * Phase 3I-1 — CPU HNSW fallback index
 *
 * cuvs_hnsw_serialize: convert a resident CAGRA GPU index to CPU HNSW
 * (hnswlib format) and write to disk.  Called once after a successful CAGRA
 * build; non-fatal if it fails (daemon continues with GPU-only serving).
 *
 * cuvs_hnsw_deserialize: load a .hnsw sidecar from disk.  Called lazily in
 * handle_search when use_cpu_hnsw=1 is requested and the index is not yet in
 * RAM.  hnswlib itself is CPU-threaded; the raft resource is only needed for
 * the cuVS API wrapper, not for GPU work.
 * ---------------------------------------------------------------- */
extern "C" int
cuvs_hnsw_serialize(CuvsCagraIndex cagra_idx, const char *path, int device_id)
{
    if (!cagra_idx || !path) return -1;
    CuvsCagraIndexImpl *cimpl = static_cast<CuvsCagraIndexImpl *>(cagra_idx);
    /* hnswlib aborts on very small graphs; skip serialization rather than crash.
     * The CAGRA index remains functional for GPU search. SKIPPED, not 0: nothing
     * was written, and a caller reading 0 as "the sidecar exists" hands out a
     * path to a file nobody created (#106). */
    if (cimpl->idx.size() < (size_t)CUVS_HNSW_MIN_ELEMENTS) {
        fprintf(stderr, "[cuvs_hnsw_serialize] skip: n_vecs=%zu < %d (too small for hnswlib)\n",
                cimpl->idx.size(), CUVS_HNSW_MIN_ELEMENTS);
        return CUVS_HNSW_SERIALIZE_SKIPPED;
    }
    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();
        cuvs::neighbors::hnsw::index_params hparams;
        /* CPU hierarchy produces an hnswlib-compatible file on disk,
         * required for both Phase 3I-1 (cuVS CPU search) and 3I-2
         * (pgvector bulk import via our own hnswlib parser). */
        hparams.hierarchy = cuvs::neighbors::hnsw::HnswHierarchy::CPU;
        auto hnsw = cuvs::neighbors::hnsw::from_cagra(
            res, hparams, cimpl->idx);
        res.sync_stream();
        cuvs::neighbors::hnsw::serialize(res, std::string(path), *hnsw);
        _pr.fence_check();
        return 0;
    } catch (const std::exception &e) {
        _pr.poison();
        fprintf(stderr, "[cuvs_hnsw_serialize] %s\n", e.what());
        return -1;
    } catch (...) {
        _pr.poison();
        fprintf(stderr, "[cuvs_hnsw_serialize] unknown exception\n");
        return -1;
    }
}

extern "C" CuvsHnswIndex
cuvs_hnsw_deserialize(const char *path, int dim, uint32_t metric, int device_id)
{
    if (!path) return nullptr;
    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();
        cuvs::neighbors::hnsw::index_params hparams;
        hparams.hierarchy = cuvs::neighbors::hnsw::HnswHierarchy::CPU;
        cuvs::neighbors::hnsw::index<float> *loaded = nullptr;
        cuvs::neighbors::hnsw::deserialize(res, hparams, std::string(path),
                                           dim, cuvs_distance_type(metric),
                                           &loaded);
        _pr.fence_check();
        if (!loaded) return nullptr;
        CuvsHnswIndexImpl *impl = new CuvsHnswIndexImpl;
        impl->idx    = loaded;
        impl->dim    = dim;
        impl->metric = metric;
        return impl;
    } catch (const std::exception &e) {
        _pr.poison();
        fprintf(stderr, "[cuvs_hnsw_deserialize] %s\n", e.what());
        return nullptr;
    } catch (...) {
        _pr.poison();
        fprintf(stderr, "[cuvs_hnsw_deserialize] unknown exception\n");
        return nullptr;
    }
}

/* Single-query CPU HNSW search.  ef <= 0 uses max(200, k). */
extern "C" int
cuvs_hnsw_search(CuvsHnswIndex hidx, const float *query, int dim,
                 int k, int ef, CuvsSearchResult *out)
{
    if (!hidx || !query || !out || k <= 0) return -1;
    CuvsHnswIndexImpl *impl = static_cast<CuvsHnswIndexImpl *>(hidx);
    PooledRes _pr(0);   /* raft API requires a resource; hnswlib ignores CUDA */
    try {
        raft::device_resources &res = _pr.get();
        cuvs::neighbors::hnsw::search_params sparams;
        sparams.ef = (ef > 0) ? ef : std::max(200, k);

        /* search() returns uint64_t neighbors (hnswlib labels). */
        std::vector<uint64_t> h_nb((size_t)k, UINT64_MAX);
        std::vector<float>    h_dist((size_t)k, 0.0f);

        auto q_view = raft::make_host_matrix_view<const float, int64_t>(
            query, (int64_t)1, (int64_t)impl->dim);
        auto n_view = raft::make_host_matrix_view<uint64_t, int64_t>(
            h_nb.data(), (int64_t)1, (int64_t)k);
        auto d_view = raft::make_host_matrix_view<float, int64_t>(
            h_dist.data(), (int64_t)1, (int64_t)k);

        cuvs::neighbors::hnsw::search(res, sparams, *impl->idx,
                                       q_view, n_view, d_view);
        for (int i = 0; i < k; i++) {
            out[i].item_id  = (int64_t)h_nb[i];   /* item_id is int64 in CuvsSearchResult */
            out[i].distance = h_dist[i];
        }
        return 0;
    } catch (const std::exception &e) {
        _pr.poison();
        fprintf(stderr, "[cuvs_hnsw_search] %s\n", e.what());
        return -1;
    } catch (...) {
        _pr.poison();
        fprintf(stderr, "[cuvs_hnsw_search] unknown exception\n");
        return -1;
    }
}

extern "C" void
cuvs_hnsw_free(CuvsHnswIndex hidx)
{
    if (!hidx) return;
    CuvsHnswIndexImpl *impl = static_cast<CuvsHnswIndexImpl *>(hidx);
    delete impl->idx;   /* raw ptr returned by deserialize / released from unique_ptr */
    delete impl;
}

/* ----------------------------------------------------------------
 * Phase 3J: extract CAGRA adjacency list + corpus vectors from VRAM to CPU.
 * Used by handle_export_adjacency in the daemon to serve pg_cuvs_import_cagra.
 * ---------------------------------------------------------------- */
extern "C" int
cuvs_cagra_extract_adjacency(
    CuvsCagraIndex  handle,
    uint32_t      **adj_out,
    float         **vecs_out,
    size_t         *n_vecs_out,
    int            *graph_degree_out,
    int             device_id)
{
    if (!handle || !adj_out || !vecs_out || !n_vecs_out || !graph_degree_out)
        return -1;
    CuvsCagraIndexImpl *cimpl = static_cast<CuvsCagraIndexImpl *>(handle);
    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();

        auto graph_view = cimpl->idx.graph();
        size_t N   = (size_t)graph_view.extent(0);
        size_t D   = (size_t)graph_view.extent(1);

        /* Read the corpus from the index, NOT from cimpl->dataset. After
         * cuvs_cagra_deserialize (and cuvs_cagra_extend) that member is a
         * deliberate 0x0 placeholder — the index owns the real dataset. Taking
         * dim from the placeholder yielded 0, so `vecs` was a 0-byte malloc and
         * the copy below moved nothing, yet the function still returned 0; the
         * daemon then copied n_vecs * e->dim * sizeof(float) bytes out of that
         * empty buffer (a 12.8 KB over-read at 400x8, ~3 GB at 1M x 768). See
         * issue #101 — measured, not inferred. */
        auto   ds  = cimpl->idx.dataset();
        size_t dim = (size_t)ds.extent(1);

        uint32_t *adj  = (uint32_t *)malloc(N * D   * sizeof(uint32_t));
        float    *vecs = (float    *)malloc(N * dim  * sizeof(float));
        if (!adj || !vecs) {
            free(adj); free(vecs);
            fprintf(stderr, "[cuvs_cagra_extract_adjacency] malloc failed N=%zu D=%zu dim=%zu\n",
                    N, D, dim);
            return -1;
        }

        auto stream = raft::resource::get_cuda_stream(res);
        raft::copy(adj, graph_view.data_handle(), N * D, stream);
        /* cagra::index::dataset() is a strided view — cuVS pads rows for
         * alignment, so a flat N*dim copy is only valid when the row stride
         * equals dim. Fall back to a row-wise copy otherwise, or every vector
         * after the first would be read at the wrong offset. */
        {
            size_t row_stride = (size_t)ds.stride(0);
            if (row_stride == dim) {
                raft::copy(vecs, ds.data_handle(), N * dim, stream);
            } else {
                for (size_t i = 0; i < N; i++)
                    raft::copy(vecs + i * dim,
                               ds.data_handle() + i * row_stride,
                               dim, stream);
            }
        }
        res.sync_stream();

        *adj_out          = adj;
        *vecs_out         = vecs;
        *n_vecs_out       = N;
        *graph_degree_out = (int)D;
        return 0;
    } catch (const std::exception &e) {
        _pr.poison();
        fprintf(stderr, "[cuvs_cagra_extract_adjacency] %s\n", e.what());
        return -1;
    } catch (...) {
        _pr.poison();
        fprintf(stderr, "[cuvs_cagra_extract_adjacency] unknown exception\n");
        return -1;
    }
}

/* ----------------------------------------------------------------
 * Debug/test helpers: eat and free raw VRAM to simulate physical CUDA OOM.
 *
 * cuvs_eat_vram(leave_bytes, device_id): allocate enough VRAM (in 512 MB
 * chunks) to leave only ~leave_bytes free on the device.  Chunked allocation
 * is required because CUDA driver typically refuses a single cudaMalloc of
 * nearly-full VRAM on large GPUs (A100 40 GB etc.) — individual 512 MB
 * allocations stack up to 39+ GB without triggering the driver limit.
 *
 * cuvs_free_vram(device_id): release all held chunks.
 *
 * Max 256 chunks per device (256 × 512 MB = 128 GB — enough for any GPU).
 * ---------------------------------------------------------------- */

static void *g_vram_eaten_ptrs[16][256];
static int   g_vram_eaten_cnt[16];

extern "C" void
cuvs_set_inject_extend_oom(int enable)
{
    g_inject_extend_oom.store(enable);
}

extern "C" int
cuvs_last_build_was_oom(void)
{
    int v = g_last_build_oom;   /* read and clear */
    g_last_build_oom = 0;
    return v;
}

extern "C" int64_t
cuvs_last_failed_allocation_bytes(void)
{
    int64_t bytes = g_last_build_failed_bytes;
    g_last_build_failed_bytes = -1;
    return bytes;
}

extern "C" int
cuvs_last_build_attach_failed(void)
{
    int v = g_last_build_att;   /* read and clear */
    g_last_build_att = 0;
    return v;
}

extern "C" void
cuvs_set_inject_build_oom(int n_fail)
{
    g_inject_build_oom.store(n_fail);
}

extern "C" int
cuvs_eat_vram(int64_t leave_bytes, int device_id)
{
    if (device_id < 0 || device_id >= 16)
        return 1;

    cudaSetDevice(device_id);

    /* Release any previous allocation */
    for (int i = 0; i < g_vram_eaten_cnt[device_id]; i++) {
        cudaFree(g_vram_eaten_ptrs[device_id][i]);
        g_vram_eaten_ptrs[device_id][i] = NULL;
    }
    g_vram_eaten_cnt[device_id] = 0;

    if (leave_bytes < 0) leave_bytes = 0;

    size_t free_mem = 0, total_mem = 0;
    if (cudaMemGetInfo(&free_mem, &total_mem) != cudaSuccess)
        return 1;

    fprintf(stderr, "[cuvs_eat_vram] device=%d free=%zu MB leave=%ld MB\n",
            device_id, free_mem >> 20, (long)(leave_bytes >> 20));

    if ((size_t)leave_bytes >= free_mem)
        return 0;

    /* Two-phase eating so we leave very little free:
     *   Phase 1: 1 GB chunks — fast bulk consumption
     *   Phase 2: 4 MB chunks — fine-grained mop-up to approach driver headroom
     * CUDA driver reserves ~100-500 MB depending on GPU; we stop when it
     * refuses the next chunk, leaving only that headroom free. */
    const size_t LARGE = 1024UL * 1024 * 1024;   /* 1 GB */
    const size_t SMALL = 4UL * 1024 * 1024;       /* 4 MB */
    size_t remaining = free_mem - (size_t)leave_bytes;
    int cnt = 0;

    while (remaining >= LARGE && cnt < 256) {
        void *ptr = NULL;
        if (cudaMalloc(&ptr, LARGE) != cudaSuccess) {
            cudaGetLastError();
            break;
        }
        g_vram_eaten_ptrs[device_id][cnt++] = ptr;
        remaining -= LARGE;
    }

    while (remaining >= SMALL && cnt < 256) {
        void *ptr = NULL;
        if (cudaMalloc(&ptr, SMALL) != cudaSuccess) {
            cudaGetLastError();
            break;
        }
        g_vram_eaten_ptrs[device_id][cnt++] = ptr;
        remaining -= SMALL;
    }
    g_vram_eaten_cnt[device_id] = cnt;

    size_t free_after = 0, dummy = 0;
    cudaMemGetInfo(&free_after, &dummy);
    fprintf(stderr, "[cuvs_eat_vram] ate %d chunks; free_after=%zu MB\n",
            cnt, free_after >> 20);

    return 0;  /* always success — we ate what we could */
}

extern "C" int
cuvs_free_vram(int device_id)
{
    if (device_id < 0 || device_id >= 16)
        return 1;
    if (g_vram_eaten_cnt[device_id] > 0) {
        cudaSetDevice(device_id);
        for (int i = 0; i < g_vram_eaten_cnt[device_id]; i++) {
            cudaFree(g_vram_eaten_ptrs[device_id][i]);
            g_vram_eaten_ptrs[device_id][i] = NULL;
        }
        g_vram_eaten_cnt[device_id] = 0;
    }
    return 0;
}

/* ----------------------------------------------------------------
 * Warm-up: pay the one-time GPU init cost (CUDA primary context, RMM pool,
 * cuBLAS/cuSOLVER handles, kernel JIT) at daemon startup instead of on the
 * first client query. Runs a tiny brute-force search; also primes one entry
 * in the device_resources pool. Safe to call once per device at boot.
 * ---------------------------------------------------------------- */
extern "C" void
cuvs_warmup_device(int device_id)
{
    try {
        const int n = 512, dim = 16, k = 8;
        std::vector<float> corpus((size_t)n * dim), query(dim, 0.1f);
        for (size_t i = 0; i < corpus.size(); i++)
            corpus[i] = (float)((i * 2654435761u) % 1000) / 1000.0f;
        CuvsSearchResult out[8];
        /* brute force warms CUDA context / RMM / cuBLAS (gemm) */
        (void)cuvs_brute_force_search(corpus.data(), query.data(), n, dim, k,
                                      CUVS_METRIC_L2, out, device_id);
        /* cagra build+search warms the CAGRA-specific kernels so the first
         * real cagra query on a freshly booted daemon is not the one that
         * pays kernel load (~100 ms). */
        CuvsCagraIndex idx = cuvs_cagra_build(corpus.data(), n, dim, CUVS_METRIC_L2,
                                               0, 0, CUVS_CAGRA_BUILD_AUTO, device_id,
                                               CUVS_LW_OVERFLOW_ERROR, NULL); /* I4: warmup never managed */
        if (idx) {
            (void)cuvs_cagra_search(idx, query.data(), dim, k, out, device_id, 0, 0);
            cuvs_cagra_free(idx, device_id);
        }
    } catch (...) {
        /* warm-up is best-effort; a failure here must not stop the daemon */
    }
}

extern "C" void
cuvs_warmup(void)
{
    cuvs_warmup_device(0);
}

namespace {
/* Resolve the marker from the very DSO implementing from_cagra, rather than
 * trusting an environment flag or an unrelated globally-preloaded symbol. */
bool hnsw_sdk_marker_verified(const char *symbol, const char *expected)
{
    using Dataset = std::optional<raft::host_matrix_view<const float, int64_t, raft::row_major>>;
    using From = std::unique_ptr<cuvs::neighbors::hnsw::index<float>> (*)(
        raft::resources const&, cuvs::neighbors::hnsw::index_params const&,
        cuvs::neighbors::cagra::index<float, uint32_t> const&, Dataset);
    From fn = &cuvs::neighbors::hnsw::from_cagra;
    Dl_info impl = {}, marker = {};
    if (!dladdr(reinterpret_cast<void *>(fn), &impl) || !impl.dli_fname) return false;
    void *dso = dlopen(impl.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
    if (!dso) return false;
    auto patch = reinterpret_cast<const char *(*)()>(dlsym(dso, symbol));
    bool ok = patch && dladdr(reinterpret_cast<void *>(patch), &marker)
        && marker.dli_fbase == impl.dli_fbase;
    if (ok)
    {
        const char *id = patch();
        ok = id && std::strcmp(id, expected) == 0;
    }
    dlclose(dso);
    return ok;
}

struct HnswBuildFiles {
    const char *dir;
    void *mapping = MAP_FAILED;
    size_t bytes = 0;
    ~HnswBuildFiles() {
        if (mapping != MAP_FAILED) munmap(mapping, bytes);
        std::error_code ec;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().filename() != ".lease") std::filesystem::remove_all(it->path(), ec);
        }
        if (ec) fprintf(stderr, "[hnsw_build] cleanup failed: %s\n", ec.message().c_str());
        fprintf(stderr, "[hnsw_build] job=%s phase=cleanup-complete mono_us=%llu files_removed=%d\n",
            dir, (unsigned long long)cuvs_hnsw_monotonic_us(), !ec);
    }
};
}

namespace {
using HnswInputView = decltype(raft::make_host_matrix_view<const float, int64_t>(
    static_cast<const float *>(nullptr), int64_t{0}, int64_t{0}));
using HnswPqParams = decltype(cuvs::neighbors::cagra::graph_build_params::ivf_pq_params(
    std::declval<HnswInputView &>().extents(), cuvs_distance_type(CUVS_METRIC_L2)));
struct HnswPreparedSettings {
    CuvsHnswCapacity capacity = {};
    uint32_t degree = 0, intermediate = 0;
    bool tiny = false;
    uint64_t sample_rows = 0, actual_training = 0;
    std::optional<HnswPqParams> pq_params;
    CuvsHnswRouteDecision route = {};
    CuvsHnswPolicy receipt = {};
};

/* Preserve the B factory/sampling/reserve order BEFORE route selection. */
static int prepare_hnsw_settings(const CuvsHnswFileBuild *params, int vectors_fd,
    HnswPreparedSettings &prepared, char *error, size_t error_len)
{
    using namespace cuvs::neighbors;
    auto &cap = prepared.capacity;
    auto &degree = prepared.degree;
    degree = params->graph_degree;
    auto &intermediate = prepared.intermediate;
    intermediate = cuvs_hnsw_scaled_quality(params->intermediate_degree,
        params->ef_construction, degree, 4096);
    if (params->policy) memset(params->policy, 0, sizeof(*params->policy));
    if (!cuvs_hnsw_algo_supported(params->build_algo)
        || params->ef_construction < 4 || params->ef_construction > 1000)
    {
        snprintf(error, error_len, "unsupported HNSW builder/ef_construction; supported: auto, ivf_pq, ace (NN-descent memory model unsupported)");
        return CUVS_STATUS_BUILD_FAILED;
    }
    prepared.tiny = params->n_vecs > 0 && params->n_vecs < CUVS_HNSW_MIN_ELEMENTS;
    const bool tiny = prepared.tiny;
    if (tiny)
    {
        // A bounded <=15-row GPU exact graph avoids IVF-PQ's minimum training
        // population and keeps M>=2 even for one/two input rows.
        degree = std::max<uint32_t>(4, (uint32_t)(params->n_vecs & ~UINT64_C(1)));
        intermediate = degree;
    }
    else
    {
        if (degree >= params->n_vecs && params->n_vecs)
            degree = (uint32_t)((params->n_vecs - 1) & ~UINT64_C(1));
        if (intermediate >= params->n_vecs && params->n_vecs)
            intermediate = (uint32_t)(params->n_vecs - 1);
    }
    if (cuvs_hnsw_capacity(params->n_vecs, params->dim, degree, intermediate,
                          params->hierarchy != 0, &cap)
        || params->metric > CUVS_METRIC_IP || params->hierarchy > 1
        || !cuvs_hnsw_regular_disk(vectors_fd))
    {
        snprintf(error, error_len, "invalid HNSW capacity/dimension/degree/file descriptor");
        return CUVS_STATUS_BUILD_FAILED;
    }
    if (tiny && params->build_algo == CUVS_HNSW_ALGO_IVF_PQ)
    {
        snprintf(error, error_len, "explicit ivf_pq requires at least 16 rows; use build_algo=auto for the tiny exact route");
        return CUVS_STATUS_BUILD_FAILED;
    }
    if (tiny && params->build_algo == CUVS_HNSW_ALGO_ACE)
    {
        snprintf(error, error_len, "explicit ace conflicts with tiny input; use build_algo=auto for tiny exact");
        return CUVS_STATUS_BUILD_FAILED;
    }
    auto &sample_rows = prepared.sample_rows;
    sample_rows = cuvs_hnsw_sample_rows(params->n_vecs, params->dim,
                                         params->host_budget, params->gpu_budget);
    auto shape = raft::make_host_matrix_view<const float, int64_t>(
        static_cast<const float *>(nullptr), (int64_t)params->n_vecs, (int64_t)params->dim);
    prepared.pq_params.emplace(cagra::graph_build_params::ivf_pq_params(shape.extents(),
        params->metric == CUVS_METRIC_COSINE ? cuvs::distance::DistanceType::InnerProduct
                                          : cuvs_distance_type(params->metric)));
    auto &pq_params = *prepared.pq_params;
    pq_params.build_params.n_lists = std::min<uint32_t>(pq_params.build_params.n_lists,
        (uint32_t)std::max<uint64_t>(1, sample_rows / 32));
    if (sample_rows < 256) pq_params.build_params.pq_bits = 4;
    pq_params.build_params.kmeans_trainset_fraction = (double)sample_rows / params->n_vecs;
    pq_params.search_params.max_internal_batch_size = 4096;
    auto &actual_training = prepared.actual_training;
    actual_training = tiny ? 0 : cuvs_hnsw_training_rows(params->n_vecs,
        sample_rows, pq_params.build_params.n_lists);
    if (cuvs_hnsw_training_reserve(&cap, actual_training, params->dim))
    {
        snprintf(error, error_len, "HNSW training estimate overflow");
        return CUVS_STATUS_BUILD_FAILED;
    }
    return CUVS_STATUS_OK;
}

/* Own no outer mapping or result descriptor: the public caller's existing
 * HnswBuildFiles lifetime still spans preparation through returned FD handoff. */
static int execute_hnsw_file_build(const CuvsHnswFileBuild *params, int owner_fd,
    const char *workdir, int device_id, HnswBuildFiles &files,
    HnswPreparedSettings &prepared, int *out_fd)
{
    using namespace cuvs::neighbors;
    const auto &cap = prepared.capacity;
    const auto degree = prepared.degree, intermediate = prepared.intermediate;
    const auto tiny = prepared.tiny;
    const bool disk = prepared.route.route == CUVS_HNSW_ROUTE_ACE;
    const auto partitions = prepared.route.partitions;
    const auto sdk_host_budget = prepared.route.sdk_host_budget;
    const auto sdk_gpu_budget = prepared.route.sdk_gpu_budget;
    const auto &pq_params = *prepared.pq_params;
    auto &receipt = prepared.receipt;
    PooledRes resources(device_id);
    try
    {
        auto &res = resources.get();
        auto data = raft::make_host_matrix_view<const float, int64_t>(
            static_cast<const float *>(files.mapping), (int64_t)params->n_vecs, (int64_t)params->dim);
        cagra::index_params cp;
        // Cosine was normalized by the PG scan. hnswlib supports L2/IP,
        // not CosineExpanded; normalized inner product has the same order.
        cp.metric = params->metric == CUVS_METRIC_COSINE
            ? cuvs::distance::DistanceType::InnerProduct : cuvs_distance_type(params->metric);
        cp.graph_degree = degree;
        cp.intermediate_graph_degree = intermediate;
        cp.attach_dataset_on_build = false;
        hnsw::index_params hp;
        hp.hierarchy = params->hierarchy ? hnsw::HnswHierarchy::GPU : hnsw::HnswHierarchy::NONE;
        hp.num_threads = 1;
        hp.ef_construction = receipt.hierarchy_ef;
        if (disk)
        {
            cagra::graph_build_params::ace_params ace;
            ace.use_disk = true;
            ace.ef_construction = receipt.ace_ef;
            ace.build_dir = workdir;
            ace.npartitions = partitions;
            ace.max_host_memory_gb = (double)sdk_host_budget / (UINT64_C(1) << 30);
            ace.max_gpu_memory_gb = (double)sdk_gpu_budget / (UINT64_C(1) << 30);
#if CUVS_HNSW_HAS_SDK_PLAN
            size_t effective = cuvs_hnsw_sdk_ace_plan(cp, params->n_vecs, params->dim,
                partitions, workdir, ace.max_host_memory_gb, ace.max_gpu_memory_gb);
            size_t rechecked = cuvs_hnsw_sdk_ace_plan(cp, params->n_vecs, params->dim,
                effective, workdir, ace.max_host_memory_gb, ace.max_gpu_memory_gb);
            if (effective != rechecked)
                throw std::runtime_error("ACE SDK partition plan is not stable; no build admitted");
            ace.npartitions = effective;
            receipt.partitions = (uint32_t)effective;
#else
            throw std::runtime_error("ACE metadata requires matching pinned SDK source planner; set CUVS_SDK_SOURCE_INCLUDE");
#endif
            cp.graph_build_params = ace;
            // Guarded disk serializer reads THESE SAME positive budgets.
            hp.graph_build_params = ace;
        }
        else
        {
            cp.graph_build_params = pq_params;
        }
        fprintf(stderr, "[hnsw_build] job=%s phase=cuvs-enter call=graph mono_us=%llu\n",
            workdir, (unsigned long long)cuvs_hnsw_monotonic_us());
        auto graph = [&]() {
            if (!tiny) return cagra::build(res, cp, data);
            const int n = (int)params->n_vecs;
            auto bf = cuvs_bf_build(data.data_handle(), n, (int)params->dim,
                params->metric, 0, device_id);
            if (!bf) throw std::runtime_error("tiny HNSW GPU graph build failed");
            auto release = [device_id](void *p) { cuvs_bf_free(p, device_id); };
            std::unique_ptr<void, decltype(release)> bf_owner(bf, release);
            std::vector<CuvsSearchResult> nearest((size_t)n * n);
            if (cuvs_bf_search_batch(bf, data.data_handle(), n, (int)params->dim,
                n, nearest.data(), device_id))
                throw std::runtime_error("tiny HNSW GPU graph search failed");
            auto adjacency = raft::make_host_matrix<uint32_t, int64_t>(n, degree);
            for (int row = 0; row < n; row++)
            {
                uint32_t count = 0;
                for (int k = 0; k < n && count < degree; k++)
                {
                    int64_t id = nearest[(size_t)row * n + k].item_id;
                    if (id >= 0 && id < n && id != row)
                        adjacency(row, count++) = (uint32_t)id;
                }
                while (count < degree) { adjacency(row, count) = (uint32_t)row; count++; }
            }
            cagra::index<float, uint32_t> result(res, cp.metric);
            result.update_graph(res, raft::make_const_mdspan(adjacency.view()));
            res.sync_stream();
            return result;
        }();
        res.sync_stream();
        fprintf(stderr, "[hnsw_build] job=%s phase=cuvs-return call=graph mono_us=%llu cancelled=%d\n",
            workdir, (unsigned long long)cuvs_hnsw_monotonic_us(), cuvs_hnsw_owner_cancelled(owner_fd));
        if (params->phase_hook && params->phase_hook(params->phase_arg, "after-graph"))
            return CUVS_STATUS_CANCELED;
        if (cuvs_hnsw_owner_cancelled(owner_fd)) return CUVS_STATUS_CANCELED;
        std::string output = std::string(workdir) + "/result.hnsw";
        fprintf(stderr, "[hnsw_build] job=%s phase=cuvs-enter call=serialize mono_us=%llu\n",
            workdir, (unsigned long long)cuvs_hnsw_monotonic_us());
        if (!params->hierarchy && !disk)
        {
            // from_cagra<NONE> uses an uncontrolled /tmp file and loads
            // it into a full hnswlib heap index. Write the same base-layer
            // representation directly into our private disk directory.
            cagra::serialize_to_hnswlib(res, output, graph, data);
        }
        else
        {
            auto index = hnsw::from_cagra(res, hp, graph, data);
            res.sync_stream();
            fprintf(stderr, "[hnsw_build] job=%s phase=cuvs-return call=hierarchy mono_us=%llu cancelled=%d\n",
                workdir, (unsigned long long)cuvs_hnsw_monotonic_us(), cuvs_hnsw_owner_cancelled(owner_fd));
            if (cuvs_hnsw_owner_cancelled(owner_fd)) return CUVS_STATUS_CANCELED;
            if (disk)
                output = index->file_path(); // already serialized; avoid a second disk copy
            else
                hnsw::serialize(res, output, *index);
        }
        fprintf(stderr, "[hnsw_build] job=%s phase=cuvs-return call=serialize mono_us=%llu cancelled=%d\n",
            workdir, (unsigned long long)cuvs_hnsw_monotonic_us(), cuvs_hnsw_owner_cancelled(owner_fd));
        if (cuvs_hnsw_owner_cancelled(owner_fd)) return CUVS_STATUS_CANCELED;
        resources.fence_check();
        if (tiny)
        {
            int patch_fd = open(output.c_str(), O_RDWR | O_CLOEXEC);
            int rc = patch_fd < 0 ? -1 : cuvs_hnsw_fix_tiny_counts(patch_fd, params->n_vecs);
            if (patch_fd >= 0) close(patch_fd);
            if (rc) throw std::runtime_error("invalid tiny HNSW base-row neighbor counts");
        }
        int fd = open(output.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0 || !cuvs_hnsw_regular_disk(fd))
        {
            if (fd >= 0) close(fd);
            throw std::runtime_error("cannot open regular disk HNSW output");
        }
        // hnswlib clamps efConstruction to M. Record the serialized value,
        // not merely the requested value (notably at low ef_construction).
        uint64_t native_ef;
        if (pread(fd, &native_ef, sizeof(native_ef), 88) != sizeof(native_ef)
            || native_ef == 0 || native_ef > UINT16_MAX)
        {
            close(fd);
            throw std::runtime_error("invalid serialized HNSW construction effort");
        }
        receipt.hierarchy_ef = (uint32_t)native_ef;
        receipt.guard_passed = 1; /* route + selected SDK guard returned; not a hard cap */
        if (params->policy) *params->policy = receipt;
        *out_fd = fd; // files destructor unlinks the private directory
        fprintf(stderr, "[hnsw_build] %s partitions=%u rows=%llu dim=%u host_est=%llu gpu_est=%llu\n",
            disk ? "ACE-disk" : tiny ? "tiny-GPU" : "host-IVFPQ", partitions, (unsigned long long)params->n_vecs, params->dim,
            (unsigned long long)cap.host_peak, (unsigned long long)cap.gpu_peak);
        return CUVS_STATUS_OK;
    }
    catch (...) { resources.poison(); throw; }
}
} // namespace

extern "C" int
cuvs_hnsw_build_file(int vectors_fd, int owner_fd,
    const CuvsHnswFileBuild *params, const char *workdir, int device_id,
    int *out_fd, char *error, size_t error_len)
{
    using namespace cuvs::neighbors;
    *out_fd = -1;
    if (!params || !workdir || device_id < 0 || device_id >= CUVS_MAX_GPUS)
        return CUVS_STATUS_ERROR;
    HnswBuildFiles files{workdir};
    try
    {
        HnswPreparedSettings prepared;
        int rc = prepare_hnsw_settings(params, vectors_fd, prepared, error, error_len);
        if (rc != CUVS_STATUS_OK) return rc;
        const auto &cap = prepared.capacity;
        const auto degree = prepared.degree, intermediate = prepared.intermediate;
        const auto tiny = prepared.tiny;
        const auto sample_rows = prepared.sample_rows, actual_training = prepared.actual_training;
        const auto &pq_params = *prepared.pq_params;
        auto &receipt = prepared.receipt;
        if (params->hierarchy && !hnsw_sdk_marker_verified("pgcuvs_hnsw_disk_copy_patch_id",
                "pgcuvs-hnsw-disk-copy-v1@60b19a0f62ba0bd716de349382828e43be6e2daa"))
        {
            snprintf(error, error_len, "GPU hierarchy requires verified patched cuVS 26.04 DSO identity");
            return CUVS_STATUS_BUILD_FAILED;
        }
        if (cuvs_hnsw_owner_cancelled(owner_fd)) return CUVS_STATUS_CANCELED;
        rc = cuvs_hnsw_resolve_route(params->n_vecs, params->dim, degree, intermediate,
            params->build_algo, params->auto_overflow, &cap,
            params->host_budget, params->gpu_budget,
            &prepared.route, error, error_len);
        if (rc != CUVS_STATUS_OK) return rc;
        const bool disk = prepared.route.route == CUVS_HNSW_ROUTE_ACE;
        const auto partitions = prepared.route.partitions;
        const auto sdk_host_budget = prepared.route.sdk_host_budget;
        const auto sdk_gpu_budget = prepared.route.sdk_gpu_budget;
        if (disk)
        {
            if (std::strlen(workdir) > 255 || std::strstr(workdir, "//"))
            {
                snprintf(error, error_len, "ACE private workdir violates SDK path contract; refusing /tmp fallback");
                return CUVS_STATUS_BUILD_FAILED;
            }
            if (!hnsw_sdk_marker_verified("pgcuvs_hnsw_disk_copy_patch_id",
                    "pgcuvs-hnsw-disk-copy-v1@60b19a0f62ba0bd716de349382828e43be6e2daa")
                || !hnsw_sdk_marker_verified("pgcuvs_hnsw_memory_guard_patch_id",
                    "pgcuvs-hnsw-memory-guard-v2@60b19a0f62ba0bd716de349382828e43be6e2daa"))
            {
                snprintf(error, error_len, "ACE requires verified SDK partition/reorder/serializer capacity guard");
                return CUVS_STATUS_BUILD_FAILED;
            }
        }
        receipt.magic = CUVS_HNSW_POLICY_MAGIC;
        receipt.version = CUVS_HNSW_POLICY_VERSION;
        receipt.bytes = sizeof(receipt);
        receipt.route = prepared.route.route;
        receipt.requested_algo = params->build_algo;
        receipt.route_flags = prepared.route.overflowed ? CUVS_HNSW_ROUTE_FLAG_AUTO_OVERFLOW : 0;
        receipt.requested_ef = params->ef_construction;
        receipt.degree = degree;
        receipt.intermediate = intermediate;
        receipt.hierarchy = params->hierarchy;
        receipt.n_rows = params->n_vecs;
        receipt.dim = params->dim;
        receipt.metric = params->metric;
        receipt.hierarchy_ef = params->hierarchy
            ? cuvs_hnsw_scaled_quality(200, params->ef_construction, 4, 65535) : 200;
        receipt.ace_ef = cuvs_hnsw_scaled_quality(120, params->ef_construction, 4, 65535);
        receipt.partitions = partitions;
        receipt.internal_builder = tiny ? 1 : disk ? 3 : 2; /* exact / IVF-PQ / NN-descent */
        receipt.training_requested = tiny ? 0 : sample_rows;
        receipt.training_actual = actual_training; /* resident candidate estimate */
        receipt.host_estimate = cap.host_peak;
        receipt.gpu_estimate = cap.gpu_peak;
        receipt.host_budget = params->host_budget;
        receipt.gpu_budget = params->gpu_budget;
        receipt.sdk_host_budget = sdk_host_budget;
        receipt.sdk_gpu_budget = sdk_gpu_budget;
        if (!tiny && !disk) {
            receipt.n_lists = pq_params.build_params.n_lists;
            receipt.pq_dim = pq_params.build_params.pq_dim;
            receipt.pq_bits = pq_params.build_params.pq_bits;
            receipt.n_probes = pq_params.search_params.n_probes;
            receipt.query_batch = 4096;
        }
        if (cap.raw_bytes > SIZE_MAX)
        {
            snprintf(error, error_len, "HNSW input exceeds addressable file mapping");
            return CUVS_STATUS_BUILD_FAILED;
        }
        files.bytes = (size_t)cap.raw_bytes;
        files.mapping = mmap(nullptr, files.bytes, PROT_READ, MAP_SHARED, vectors_fd, 0);
        if (files.mapping == MAP_FAILED)
        {
            snprintf(error, error_len, "cannot map HNSW disk vector spool: %s", strerror(errno));
            return CUVS_STATUS_BUILD_FAILED;
        }
        fprintf(stderr, "[hnsw_build] job=%s phase=route mono_us=%llu route=%s partitions=%u host_budget=%llu gpu_budget=%llu sdk_host_budget=%llu sdk_gpu_budget=%llu host_est=%llu gpu_est=%llu\n",
            workdir, (unsigned long long)cuvs_hnsw_monotonic_us(),
            disk ? "ACE-disk" : tiny ? "tiny-GPU" : "host-IVFPQ", partitions,
            (unsigned long long)params->host_budget, (unsigned long long)params->gpu_budget,
            (unsigned long long)sdk_host_budget, (unsigned long long)sdk_gpu_budget,
            (unsigned long long)cap.host_peak, (unsigned long long)cap.gpu_peak);
        /* Resident IVF-PQ kNN precision from the rows themselves: fp16 distances
         * overflow for large-magnitude data (cuvs_hnsw_ivfpq_needs_fp32). ACE-disk
         * needs no rule: with the positive budgets passed below, the pinned
         * SDK's memory-guard patch builds every partition kNN with NN-descent
         * (cagra_build.cuh bounded_partition_builder), never fp16 IVF-PQ. */
        if (!tiny && !disk)
        {
            const double max_sq = cuvs_hnsw_max_sq_norm(static_cast<const float *>(files.mapping),
                params->n_vecs, params->dim);
            const bool fp32 = cuvs_hnsw_ivfpq_needs_fp32(max_sq, params->metric);
            if (fp32)
            {
                auto &sp = prepared.pq_params->search_params;
                sp.lut_dtype = CUDA_R_32F;
                sp.internal_distance_dtype = CUDA_R_32F;
                sp.coarse_search_dtype = CUDA_R_32F;
                receipt.route_flags |= CUVS_HNSW_ROUTE_FLAG_IVFPQ_FP32;
            }
            fprintf(stderr, "[hnsw_build] job=%s phase=precision max_sq_norm=%.9g fp16_max=%.0f ivfpq_distance_dtype=%s\n",
                workdir, max_sq, CUVS_HNSW_FP16_MAX, fp32 ? "fp32" : "fp16");
        }
        try
        {
            return execute_hnsw_file_build(params, owner_fd, workdir, device_id, files, prepared, out_fd);
        }
        catch (const std::exception &e)
        {
            /* R1 on the default path: auto's resident IVF-PQ kNN graph rejected
             * by cuVS -> ACE-disk with a NOTICE, when overflow is ace. */
            if (!cuvs_hnsw_graph_failure_to_ace(params->build_algo, prepared.route.route,
                    params->auto_overflow, params->n_vecs, e.what()))
                throw;
            CuvsHnswRouteDecision ace{};
            char why[256] = "";
            const bool sdk_ok = hnsw_sdk_marker_verified("pgcuvs_hnsw_disk_copy_patch_id",
                    "pgcuvs-hnsw-disk-copy-v1@60b19a0f62ba0bd716de349382828e43be6e2daa")
                && hnsw_sdk_marker_verified("pgcuvs_hnsw_memory_guard_patch_id",
                    "pgcuvs-hnsw-memory-guard-v2@60b19a0f62ba0bd716de349382828e43be6e2daa");
            if (!sdk_ok || std::strlen(workdir) > 255 || std::strstr(workdir, "//")
                || cuvs_hnsw_resolve_route(params->n_vecs, params->dim, degree, intermediate,
                       CUVS_HNSW_ALGO_ACE, params->auto_overflow, &cap, params->host_budget,
                       params->gpu_budget, &ace, why, sizeof(why)) != CUVS_STATUS_OK)
            {
                fprintf(stderr, "[hnsw_build] job=%s phase=graph-fallback refused sdk_ok=%d reason=%s\n",
                    workdir, sdk_ok ? 1 : 0, why[0] ? why : "workdir or SDK guard");
                throw;
            }
            fprintf(stderr, "[hnsw_build] job=%s phase=graph-fallback from=host-IVFPQ to=ACE-disk partitions=%u mono_us=%llu error=%s\n",
                workdir, ace.partitions, (unsigned long long)cuvs_hnsw_monotonic_us(), e.what());
            prepared.route = ace;
            receipt.route = CUVS_HNSW_ROUTE_ACE;
            receipt.internal_builder = 3; /* NN-descent partitions */
            receipt.partitions = ace.partitions;
            receipt.sdk_host_budget = ace.sdk_host_budget;
            receipt.sdk_gpu_budget = ace.sdk_gpu_budget;
            receipt.n_lists = receipt.pq_dim = receipt.pq_bits = receipt.n_probes = 0;
            receipt.query_batch = 0;
            receipt.route_flags = (receipt.route_flags & ~CUVS_HNSW_ROUTE_FLAG_IVFPQ_FP32)
                | CUVS_HNSW_ROUTE_FLAG_GRAPH_FALLBACK;
            return execute_hnsw_file_build(params, owner_fd, workdir, device_id, files, prepared, out_fd);
        }
    }
    catch (const std::bad_alloc &e)
    {
        /* v6 C2: an allocation failure after the route was admitted (host
         * std::bad_alloc or RMM out_of_memory, which derives from it) is
         * status 2, not a generic build failure. Nothing is evicted. */
        snprintf(error, error_len, "allocation failed during HNSW build: %s", e.what());
        fprintf(stderr, "[hnsw_build] job=%s phase=cuvs-oom mono_us=%llu cancelled=%d error=%s\n",
            workdir, (unsigned long long)cuvs_hnsw_monotonic_us(), cuvs_hnsw_owner_cancelled(owner_fd), e.what());
        return CUVS_STATUS_OOM_FALLBACK;
    }
    catch (const std::exception &e)
    {
        snprintf(error, error_len, "%s", e.what());
        fprintf(stderr, "[hnsw_build] job=%s phase=cuvs-exception mono_us=%llu cancelled=%d error=%s\n",
            workdir, (unsigned long long)cuvs_hnsw_monotonic_us(), cuvs_hnsw_owner_cancelled(owner_fd), e.what());
        return CUVS_STATUS_BUILD_FAILED;
    }
    catch (...)
    {
        fprintf(stderr, "[hnsw_build] job=%s phase=cuvs-exception mono_us=%llu cancelled=%d\n",
            workdir, (unsigned long long)cuvs_hnsw_monotonic_us(), cuvs_hnsw_owner_cancelled(owner_fd));
        snprintf(error, error_len, "unknown exception building temporary HNSW");
        return CUVS_STATUS_BUILD_FAILED;
    }
}

extern "C" int
cuvs_hnsw_reap_workdirs(const char *root)
{
    int removed = 0;
    try {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) return 0;
        for (const auto &entry : std::filesystem::directory_iterator(root)) {
            std::string name = entry.path().filename().string();
            if (name.rfind(".hnsw-job-", 0) != 0) continue;
            struct stat dir_st, lease_st;
            std::string path = entry.path().string();
            if (lstat(path.c_str(), &dir_st) || !S_ISDIR(dir_st.st_mode)
                || dir_st.st_uid != geteuid() || (dir_st.st_mode & 077) != 0
                || std::time(nullptr) - dir_st.st_mtime < 60) continue;
            std::string lease = path + "/.lease";
            int fd = open(lease.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0) continue;
            if (fstat(fd, &lease_st) == 0 && S_ISREG(lease_st.st_mode)
                && lease_st.st_uid == geteuid() && flock(fd, LOCK_EX | LOCK_NB) == 0) {
                std::filesystem::remove_all(path, ec);
                if (!ec) removed++;
                else fprintf(stderr, "[hnsw_reap] %s: %s\n", path.c_str(), ec.message().c_str());
            }
            close(fd);
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "[hnsw_reap] %s\n", e.what());
        return -1;
    }
    return removed;
}
