#include "cuvs_page_overlay_gpu.h"
#include "cuvs_page_overlay_gpu_kernel.cuh"
#include "cuvs_page_overlay_gpu_rmm.hpp"
#include "cuvs_util.h"

#include <algorithm>
#include <array>
#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#include <rmm/aligned.hpp>
#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace {

class DeviceScope
{
  public:
    explicit DeviceScope(int device)
    {
        if (cudaGetDevice(&previous_) == cudaSuccess)
        {
            changed_ = previous_ != device;
            ready_ = !changed_ || cudaSetDevice(device) == cudaSuccess;
        }
    }

    ~DeviceScope()
    {
        if (ready_ && changed_)
            (void)cudaSetDevice(previous_);
    }

    bool ready() const { return ready_; }

  private:
    int previous_ = 0;
    bool changed_ = false;
    bool ready_ = false;
};

class HostCharge
{
  public:
    HostCharge() = default;
    HostCharge(const HostCharge&) = delete;
    HostCharge& operator=(const HostCharge&) = delete;

    bool reserve(CuvsPageOverlayBudget *budget, std::size_t bytes)
    {
        if (!cuvs_page_overlay_reserve(budget, bytes))
            return false;
        budget_ = budget;
        bytes_ = bytes;
        return true;
    }

    void reset()
    {
        if (budget_)
            cuvs_page_overlay_unreserve(budget_, bytes_);
        budget_ = nullptr;
    }

    ~HostCharge() { reset(); }

  private:
    CuvsPageOverlayBudget *budget_ = nullptr;
    std::size_t bytes_ = 0;
};

struct Stream
{
    int device;
    cudaStream_t handle = nullptr;

    explicit Stream(int device_id) : device(device_id) {}
    /* The pool can keep free blocks associated with this stream. CUDA
     * reclaims the one stream per device when its process context exits. */
};

/* Do not unpin host chunks or recycle scratch if CUDA cannot confirm that
 * it has finished accessing them. The native fold uses the same policy. */
void
finish_cuda(cudaStream_t stream)
{
    cudaError_t status = cudaStreamSynchronize(stream);

    if (status != cudaSuccess)
    {
        std::fprintf(stderr, "[page_overlay_gpu] CUDA completion unconfirmed: %s\n",
                     cudaGetErrorString(status));
        std::_Exit(EXIT_FAILURE);
    }
}

struct DeviceBuffer
{
    std::shared_ptr<Stream> stream;
    const CuvsPageOverlayGpuAdmission *admission = nullptr;
    float *data = nullptr;
    std::size_t charge = 0;

    DeviceBuffer() = default;
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;

    bool allocate(const std::shared_ptr<Stream>& owner, std::size_t bytes,
                  const CuvsPageOverlayGpuAdmission *hooks)
    {
        charge = rmm::align_up(bytes, rmm::CUDA_ALLOCATION_ALIGNMENT);
        if (!hooks->reserve(owner->device, charge))
            return false;
        data = static_cast<float *>(cuvs_page_overlay_rmm_allocate(owner->device, charge, owner->handle));
        hooks->finish(owner->device, charge, data != nullptr);
        if (!data)
            return false;
        stream = owner;
        admission = hooks;
        return true;
    }

    void reset()
    {
        if (!data)
            return;
        DeviceScope scope(stream->device);

        if (!scope.ready())
        {
            std::fprintf(stderr, "[page_overlay_gpu] cannot select GPU %d for release\n", stream->device);
            std::_Exit(EXIT_FAILURE);
        }
        cuvs_page_overlay_rmm_free(stream->device, data, charge, stream->handle);
        admission->release(stream->device, charge);
        data = nullptr;
        stream.reset();
    }

    ~DeviceBuffer() { reset(); }
};

struct Mirror;

/* One nonblocking workspace per device. A concurrent query uses the CPU
 * instead of queuing behind another query with the pinned result buffer. */
struct Workspace
{
    std::mutex mutex;
    std::shared_ptr<Stream> stream;
    DeviceBuffer device;
    HostCharge host_charge;
    float *host = nullptr;
    Mirror *mirrors = nullptr;

    bool initialize(int device_id, const CuvsPageOverlayGpuAdmission *admission,
                    CuvsPageOverlayBudget *budget)
    {
        constexpr std::size_t bytes = (CUVS_VECTORS_MAX_DIM + CUVS_PAGE_OVERLAY_MAX) * sizeof(float);

        if (host)
            return true;
        if (!stream)
        {
            auto candidate = std::make_shared<Stream>(device_id);

            if (cudaStreamCreateWithFlags(&candidate->handle, cudaStreamNonBlocking) != cudaSuccess)
                return false;
            stream = std::move(candidate);
        }
        if (!host_charge.reserve(budget, bytes))
            return false;
        void *pinned = nullptr;

        if (!device.allocate(stream, bytes, admission) ||
            cudaHostAlloc(&pinned, bytes, cudaHostAllocDefault) != cudaSuccess)
        {
            device.reset();
            host_charge.reset();
            return false;
        }
        host = static_cast<float *>(pinned);
        return true;
    }

    void reset_memory()
    {
        if (host && cudaFreeHost(host) != cudaSuccess)
        {
            std::fprintf(stderr, "[page_overlay_gpu] cannot release the pinned workspace\n");
            std::_Exit(EXIT_FAILURE);
        }
        host = nullptr;
        host_charge.reset();
        device.reset();
    }
};

struct Mirror : CuvsPageOverlayGpuMirror
{
    Workspace *const owner;
    DeviceBuffer vectors;
    const uint32_t dim;
    const uint32_t rows;
    Mirror *previous = nullptr;
    Mirror *next = nullptr;

    Mirror(Workspace *workspace, uint32_t dimensions, uint32_t packed_rows) noexcept
        : CuvsPageOverlayGpuMirror{release_mirror}, owner(workspace), dim(dimensions), rows(packed_rows) {}

    static void release_mirror(CuvsPageOverlayGpuMirror *handle) noexcept
    {
        auto *mirror = static_cast<Mirror *>(handle);
        Workspace& workspace = *mirror->owner;
        std::lock_guard<std::mutex> lock(workspace.mutex);

        if (mirror->previous)
            mirror->previous->next = mirror->next;
        else
            workspace.mirrors = mirror->next;
        if (mirror->next)
            mirror->next->previous = mirror->previous;
        mirror->~Mirror();
    }
};

struct Span
{
    const CuvsPageOverlayChunk *chunk;
    uint32_t first;
    uint32_t end;
    uint32_t skip = 0;
    uint32_t count = 0;
    uint32_t packed_rows = 0;
    Mirror *mirror = nullptr;
};

std::array<Workspace, CUVS_MAX_GPUS>&
workspaces()
{
    static std::array<Workspace, CUVS_MAX_GPUS> device_workspaces;
    return device_workspaces;
}

bool
upload(Mirror& mirror, const CuvsPageOverlayChunk *chunk, const CuvsPageOverlayGpuAdmission *admission)
{
    std::size_t bytes = (std::size_t)mirror.rows * mirror.dim * sizeof(float);
    const float *packed = reinterpret_cast<const float *>(chunk->marks + chunk->positions);

    if (mirror.vectors.data)
        return true;
    if (!mirror.vectors.allocate(mirror.owner->stream, bytes, admission))
        return false;
    cudaStream_t stream = mirror.owner->stream->handle;

    cudaError_t status = cudaMemcpyAsync(mirror.vectors.data, packed, bytes, cudaMemcpyHostToDevice, stream);
    finish_cuda(stream);
    if (status == cudaSuccess)
        return true;
    mirror.vectors.reset();
    return false;
}

Mirror *
find_or_upload(const Span& span, Workspace& workspace,
               const CuvsPageOverlayGpuAdmission *admission, uint32_t dim)
{
    auto *existing = static_cast<Mirror *>(cuvs_page_overlay_gpu_mirror(span.chunk));

    if (existing)
        return existing->owner == &workspace && existing->dim == dim &&
            existing->rows == span.packed_rows && upload(*existing, span.chunk, admission) ? existing : nullptr;

    void *storage = cuvs_page_overlay_claim_gpu_mirror(span.chunk);

    if (!storage)
        return nullptr;
    auto *mirror = new(storage) Mirror(&workspace, dim, span.packed_rows);

    if (!upload(*mirror, span.chunk, admission))
    {
        mirror->~Mirror();
        cuvs_page_overlay_publish_gpu_mirror(span.chunk, nullptr);
        return nullptr;
    }
    mirror->next = workspace.mirrors;
    if (workspace.mirrors)
        workspace.mirrors->previous = mirror;
    workspace.mirrors = mirror;
    cuvs_page_overlay_publish_gpu_mirror(span.chunk, mirror);
    return mirror;
}

bool
host_fp_mode_matches_cuda()
{
#if defined(__x86_64__)
    constexpr unsigned mask = _MM_ROUND_MASK | _MM_FLUSH_ZERO_MASK | _MM_DENORMALS_ZERO_MASK | _MM_MASK_MASK;

    return std::fegetround() == FE_TONEAREST &&
        (_mm_getcsr() & mask) == (_MM_ROUND_NEAREST | _MM_MASK_MASK);
#else
    return false;  // Host gradual underflow is part of the score proof.
#endif
}

} // namespace

extern "C" void
cuvs_page_overlay_gpu_init(void)
{
    static_assert(alignof(Mirror) <= alignof(std::max_align_t));
    cuvs_page_overlay_enable_gpu_cache(sizeof(Mirror));
}

extern "C" bool
cuvs_page_overlay_gpu_reclaim(int device)
{
    Workspace& workspace = workspaces()[device];
    std::unique_lock<std::mutex> lock(workspace.mutex, std::try_to_lock);

    if (!lock.owns_lock())
        return false;
    DeviceScope scope(device);

    if (!scope.ready())
        return false;
    for (Mirror *mirror = workspace.mirrors; mirror; mirror = mirror->next)
        mirror->vectors.reset();
    workspace.reset_memory();
    return true;
}

extern "C" uint32_t
cuvs_page_overlay_gpu_collect(int device, uint32_t min_rows,
                              const CuvsPageOverlayGpuAdmission *admission,
                              CuvsPageOverlayBudget *host_budget,
                              const CuvsPageOverlayChunk *snapshot, uint64_t upto,
                              const CuvsPageSearchRequest *request, const float *query,
                              const CuvsCmdFrame *cmd, CuvsPageOverlayResult *results,
                              int *count)
{
    if (device < 0 || device >= CUVS_MAX_GPUS || !snapshot || !min_rows ||
        !cmd->dim || cmd->dim > CUVS_VECTORS_MAX_DIM ||
        request->folded >= upto || upto - request->folded < min_rows || !host_fp_mode_matches_cuda())
        return 0;

    try {
        Workspace& workspace = workspaces()[device];
        std::unique_lock<std::mutex> lock(workspace.mutex, std::try_to_lock);

        if (!lock.owns_lock())
            return 0;

        std::size_t chunks = 0;
        for (auto *chunk = snapshot; chunk; chunk = chunk->next)
        {
            if (chunk->from < request->folded && request->folded - chunk->from >= chunk->positions)
                break;
            if (chunk->from < upto)
                ++chunks;
        }
        HostCharge plan_charge;

        if (!plan_charge.reserve(host_budget, chunks * sizeof(Span)))
            return 0;
        std::vector<Span> spans;
        spans.reserve(chunks);
        std::size_t requested = 0;

        for (auto *chunk = snapshot; chunk; chunk = chunk->next)
        {
            if (chunk->from >= upto)
                continue;
            uint64_t first = request->folded > chunk->from ? request->folded - chunk->from : 0;

            if (first >= chunk->positions)
                break;
            uint32_t end = static_cast<uint32_t>(std::min<uint64_t>(chunk->positions, upto - chunk->from));
            Span span{chunk, static_cast<uint32_t>(first), end};

            for (uint32_t i = 0; i < chunk->positions; ++i)
            {
                if (chunk->marks[i].state != CUVS_PAGE_POSITION_VECTOR)
                    continue;
                ++span.packed_rows;
                span.skip += i < first;
                span.count += i >= first && i < end;
            }
            requested += span.count;
            spans.push_back(span);
        }
        if (requested < min_rows || requested > CUVS_PAGE_OVERLAY_MAX)
            return 0;

        DeviceScope scope(device);
        if (!scope.ready() || !workspace.initialize(device, admission, host_budget))
            return 0;

        uint32_t gpu_rows = 0;
        for (auto& span : spans)
        {
            if (!span.count)
                continue;
            span.mirror = find_or_upload(span, workspace, admission, cmd->dim);
            if (span.mirror)
                gpu_rows += span.count;
        }
        if (gpu_rows < min_rows)
            return 0;

        cudaStream_t stream = workspace.stream->handle;
        float *device_query = workspace.device.data;
        float *device_scores = device_query + CUVS_VECTORS_MAX_DIM;
        float *scores = workspace.host + CUVS_VECTORS_MAX_DIM;
        std::size_t query_bytes = (std::size_t)cmd->dim * sizeof(float);

        std::memcpy(workspace.host, query, query_bytes);
        cudaError_t status = cudaMemcpyAsync(device_query, workspace.host, query_bytes,
                                              cudaMemcpyHostToDevice, stream);
        if (status != cudaSuccess)
        {
            finish_cuda(stream);
            return 0;
        }

        std::size_t offset = 0;
        for (const auto& span : spans)
        {
            if (!span.mirror)
                continue;
            auto *rows = span.mirror->vectors.data + (std::size_t)span.skip * cmd->dim;
            status = cuvs_page_overlay_launch_scores(stream, device_query, rows, cmd->dim,
                                                      span.count, cmd->metric, device_scores + offset);
            if (status != cudaSuccess)
                break;
            offset += span.count;
        }
        if (status == cudaSuccess)
            status = cudaMemcpyAsync(scores, device_scores, (std::size_t)gpu_rows * sizeof(float),
                                     cudaMemcpyDeviceToHost, stream);
        finish_cuda(stream);
        if (status != cudaSuccess)
            return 0;

        for (const auto& span : spans)
        {
            if (span.mirror)
            {
                cuvs_page_overlay_append_scores(span.chunk, span.first, span.end, span.skip, request,
                                                 query, cmd, scores, results, count);
                scores += span.count;
            }
            else
                cuvs_page_overlay_collect_range(span.chunk, request->folded, upto, request,
                                                 query, cmd, results, count);
        }
        return gpu_rows;
    } catch (...) {
        return 0;
    }
}
