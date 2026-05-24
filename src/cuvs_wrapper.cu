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

#include <cuvs/neighbors/brute_force.hpp>
#include <cuvs/neighbors/cagra.hpp>  /* serialize/deserialize merged here in cuVS 25.x+ */
#include <raft/core/device_resources.hpp>
#include <raft/core/device_mdarray.hpp>
#include <raft/core/host_mdarray.hpp>

#include <cuda_runtime.h>
#include <cstring>
#include <memory>
#include <vector>
#include <fstream>
#include <stdexcept>

/* Opaque CAGRA index wrapper.
 *
 * The dataset (d_corpus) MUST outlive the index because cagra::index holds an
 * mdspan view into the dataset's device memory rather than owning it. If the
 * dataset is freed (e.g., function-scope local), the index has dangling
 * pointers. Symptom: SIGSEGV on serialize(include_dataset=true) and on search.
 *
 * raft::device_resources has a deleted move constructor in cuVS 25.x+, so we
 * create a fresh one per operation rather than storing it.
 */
struct CuvsCagraIndexImpl {
    /* dataset MUST be declared before idx so destruction is reverse order:
     * idx destroyed first, then dataset. */
    raft::device_matrix<float, int64_t> dataset;
    cuvs::neighbors::cagra::index<float, uint32_t> idx;

    CuvsCagraIndexImpl(raft::device_matrix<float, int64_t> &&d,
                       cuvs::neighbors::cagra::index<float, uint32_t> &&i)
        : dataset(std::move(d)), idx(std::move(i))
    {}
};

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
 * VRAM query
 * ---------------------------------------------------------------- */
extern "C" size_t
cuvs_vram_free_bytes(void)
{
    size_t free_bytes = 0, total_bytes = 0;
    cudaMemGetInfo(&free_bytes, &total_bytes);
    return free_bytes;
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
    CuvsSearchResult *results)
{
    try {
        raft::device_resources res;

        auto d_corpus = raft::make_device_matrix<float, int64_t>(res, n_corpus, (int64_t)dim);
        raft::copy(d_corpus.data_handle(), corpus_vecs, n_corpus * dim, res.get_stream());

        auto d_queries = raft::make_device_matrix<float, int64_t>(res, (int64_t)1, (int64_t)dim);
        raft::copy(d_queries.data_handle(), query_vec, dim, res.get_stream());

        auto d_indices   = raft::make_device_matrix<int64_t, int64_t>(res, 1, top_k);
        auto d_distances = raft::make_device_matrix<float,   int64_t>(res, 1, top_k);

        auto index = cuvs::neighbors::brute_force::build(
            res,
            raft::make_const_mdspan(d_corpus.view()),
            cuvs::distance::DistanceType::L2Expanded);

        cuvs::neighbors::brute_force::search(
            res, index,
            raft::make_const_mdspan(d_queries.view()),
            d_indices.view(),
            d_distances.view());

        res.sync_stream();

        std::vector<int64_t> h_indices(top_k);
        std::vector<float>   h_distances(top_k);
        raft::copy(h_indices.data(),   d_indices.data_handle(),   top_k, res.get_stream());
        raft::copy(h_distances.data(), d_distances.data_handle(), top_k, res.get_stream());
        res.sync_stream();

        for (int i = 0; i < top_k; i++) {
            results[i].item_id  = h_indices[i];
            results[i].distance = h_distances[i];
        }
        return 0;
    } catch (...) {
        return 1;
    }
}

/* ----------------------------------------------------------------
 * CAGRA index build
 * ---------------------------------------------------------------- */
extern "C" CuvsCagraIndex
cuvs_cagra_build(const float *vecs, int64_t n_vecs, int dim)
{
    try {
        raft::device_resources res;

        /* Upload corpus to device. d_corpus will be moved into the impl
         * struct below so its memory stays alive for the index's lifetime. */
        auto d_corpus = raft::make_device_matrix<float, int64_t>(res, n_vecs, (int64_t)dim);
        raft::copy(d_corpus.data_handle(), vecs, n_vecs * dim, res.get_stream());
        res.sync_stream();

        /* CAGRA build parameters (defaults are good for Phase 1) */
        cuvs::neighbors::cagra::index_params params;
        params.graph_degree          = 64;
        params.intermediate_graph_degree = 128;

        auto idx = cuvs::neighbors::cagra::build(
            res,
            params,
            raft::make_const_mdspan(d_corpus.view()));

        /* Re-attach dataset explicitly so the index owns a valid view that
         * matches our retained d_corpus memory. */
        idx.update_dataset(res, raft::make_const_mdspan(d_corpus.view()));
        res.sync_stream();

        return new CuvsCagraIndexImpl(std::move(d_corpus), std::move(idx));
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_cagra_build] exception: %s\n", e.what());
        return nullptr;
    } catch (...) {
        fprintf(stderr, "[cuvs_cagra_build] unknown exception\n");
        return nullptr;
    }
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
    CuvsSearchResult *results)
{
    if (!index)
        return 1;

    try {
        CuvsCagraIndexImpl *impl = static_cast<CuvsCagraIndexImpl *>(index);
        raft::device_resources res;

        auto d_queries = raft::make_device_matrix<float, int64_t>(res, (int64_t)1, (int64_t)dim);
        raft::copy(d_queries.data_handle(), query_vec, dim, res.get_stream());

        auto d_indices   = raft::make_device_matrix<uint32_t, int64_t>(res, 1, top_k);
        auto d_distances = raft::make_device_matrix<float,    int64_t>(res, 1, top_k);

        cuvs::neighbors::cagra::search_params sparams;
        /* CAGRA multi-cta search requires:
         *   num_cta_per_query * 32 >= top_k
         * where num_cta_per_query = max(search_width, ceil(itopk_size / 32))
         * Default itopk_size=64, search_width=1 → num_cta=2 → max top_k=64.
         * Round itopk_size up to a multiple of 32 that satisfies top_k. */
        int itopk = ((top_k + 31) / 32) * 32;
        if (itopk < 64) itopk = 64;
        sparams.itopk_size = itopk;

        cuvs::neighbors::cagra::search(
            res, sparams, impl->idx,
            raft::make_const_mdspan(d_queries.view()),
            d_indices.view(),
            d_distances.view());

        res.sync_stream();

        std::vector<uint32_t> h_indices(top_k);
        std::vector<float>    h_distances(top_k);
        raft::copy(h_indices.data(),   d_indices.data_handle(),   top_k, res.get_stream());
        raft::copy(h_distances.data(), d_distances.data_handle(), top_k, res.get_stream());
        res.sync_stream();

        for (int i = 0; i < top_k; i++) {
            results[i].item_id  = (int64_t)h_indices[i];
            results[i].distance = h_distances[i];
        }
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_cagra_search] exception: %s\n", e.what());
        return 1;
    } catch (...) {
        fprintf(stderr, "[cuvs_cagra_search] unknown exception\n");
        return 1;
    }
}

/* ----------------------------------------------------------------
 * CAGRA index serialize / deserialize
 * ---------------------------------------------------------------- */
extern "C" int
cuvs_cagra_serialize(CuvsCagraIndex index, const char *path)
{
    if (!index)
        return 1;

    try {
        CuvsCagraIndexImpl *impl = static_cast<CuvsCagraIndexImpl *>(index);
        raft::device_resources res;
        /* include_dataset=true: works now because impl->dataset keeps the
         * device memory alive that idx's view points to. */
        cuvs::neighbors::cagra::serialize(res, std::string(path), impl->idx, true);
        res.sync_stream();
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
cuvs_cagra_deserialize(const char *path, int dim)
{
    try {
        raft::device_resources res;
        cuvs::neighbors::cagra::index<float, uint32_t> idx(res);
        cuvs::neighbors::cagra::deserialize(res, path, &idx);
        res.sync_stream();
        (void)dim;  /* dim is encoded in the serialized index */

        /* After deserialize with include_dataset=true at save time, the index
         * owns its own dataset (allocated internally during deserialize).
         * We pass an empty placeholder device_matrix so the impl struct's
         * field stays valid; idx's internal dataset is what matters. */
        auto empty = raft::make_device_matrix<float, int64_t>(res, (int64_t)0, (int64_t)0);
        res.sync_stream();
        return new CuvsCagraIndexImpl(std::move(empty), std::move(idx));
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
cuvs_cagra_free(CuvsCagraIndex index)
{
    if (index)
        delete static_cast<CuvsCagraIndexImpl *>(index);
}
