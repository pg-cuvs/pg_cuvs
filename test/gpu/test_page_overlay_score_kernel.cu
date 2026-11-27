#include "cuvs_ipc.h"
#include "cuvs_page_overlay_gpu_kernel.cuh"

#include <cuda_runtime.h>

#include <algorithm>
#include <cfenv>
#include <cfloat>
#include <cinttypes>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

static_assert(sizeof(float) == sizeof(uint32_t) && std::numeric_limits<float>::is_iec559 &&
              std::numeric_limits<float>::digits == 24);
static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits == 53);

uint32_t
float_bits(float value)
{
    uint32_t bits;

    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

double
ordered_dot(const float *query, const float *row, uint32_t dim)
{
    double sum = 0.0;

    for (uint32_t d = 0; d < dim; ++d)
    {
        /* Materialize the rounded product before adding it. */
        const volatile double product = static_cast<double>(query[d]) * row[d];

        sum += product;
    }
    return sum;
}

/* Host oracle for the scoring loop at 8673c9c3. It uses neither kernel
 * helpers nor the kernel's reduction order or error estimates. */
float
ordered_score(const float *query, const float *row, uint32_t dim, uint32_t metric)
{
    if (metric == CUVS_METRIC_L2)
    {
        double sum = 0.0;

        for (uint32_t d = 0; d < dim; ++d)
        {
            const volatile double difference = static_cast<double>(query[d]) - row[d];
            const volatile double square = difference * difference;

            sum += square;
        }
        return static_cast<float>(sum);
    }
    if (metric == CUVS_METRIC_IP)
        return static_cast<float>(ordered_dot(query, row, dim));

    const double query_norm = ordered_dot(query, query, dim);
    const double row_norm = ordered_dot(row, row, dim);

    if (!(query_norm > 0.0 && row_norm > 0.0))
        return 1.0f;
    const double dot = ordered_dot(query, row, dim);
    const double denominator = std::sqrt(query_norm * row_norm);

    return static_cast<float>(1.0 - dot / denominator);
}

bool
finite_vector(const float *values, uint32_t dim)
{
    for (uint32_t d = 0; d < dim; ++d)
        if (!std::isfinite(values[d]))
            return false;
    return true;
}

constexpr uint32_t normal_dim = 768;
constexpr size_t output_pad = 8;
constexpr int cuda_failure = 2;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        std::exit(EXIT_FAILURE); \
    } \
} while (0)

#define CUDA_CHECK(call) do { \
    const cudaError_t status = (call); \
    if (status != cudaSuccess) { \
        std::fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, #call, cudaGetErrorString(status)); \
        std::exit(cuda_failure); \
    } \
} while (0)

struct Metric {
    uint32_t id;
    const char *name;
};

constexpr Metric l2{CUVS_METRIC_L2, "l2"};
constexpr Metric ip{CUVS_METRIC_IP, "ip"};
constexpr Metric cosine{CUVS_METRIC_COSINE, "cosine"};
constexpr Metric metrics[] = {l2, ip, cosine};

struct Fixture {
    uint32_t dim, count;
    std::vector<float> query;
    std::vector<float> rows;

    Fixture(uint32_t dim, uint32_t count)
        : dim(dim), count(count), query(dim), rows(static_cast<size_t>(dim) * count)
    {
    }

    float *row(uint32_t r) { return rows.data() + static_cast<size_t>(r) * dim; }
    const float *row(uint32_t r) const { return rows.data() + static_cast<size_t>(r) * dim; }
};

void
expect_score(const Fixture &fixture, Metric metric, uint32_t row, float expected)
{
    const float score = ordered_score(fixture.query.data(), fixture.row(row), fixture.dim, metric.id);

    CHECK(float_bits(score) == float_bits(expected));
}

void
run_case(cudaStream_t stream, const char *name, const Fixture &fixture, Metric metric,
         uint32_t min_hits = 0)
{
    const size_t output_count = fixture.count + 2 * output_pad;
    const size_t output_bytes = output_count * sizeof(float);
    const size_t query_bytes = fixture.query.size() * sizeof(float);
    const size_t row_bytes = fixture.rows.size() * sizeof(float);
    const float poison = std::numeric_limits<float>::infinity();
    constexpr float canary = -12345.0f;
    float *device_query = nullptr, *device_rows = nullptr, *device_scores = nullptr;
    float *pinned = nullptr;
    uint32_t hits = 0, required_fallbacks = 0;

    CUDA_CHECK(cudaMalloc(reinterpret_cast<void **>(&device_query), query_bytes));
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void **>(&device_rows), row_bytes));
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void **>(&device_scores), output_bytes));
    CUDA_CHECK(cudaMallocHost(reinterpret_cast<void **>(&pinned), output_bytes));
    std::fill_n(pinned, output_count, canary);
    std::fill_n(pinned + output_pad, fixture.count, poison);

    CUDA_CHECK(cudaMemcpyAsync(device_query, fixture.query.data(), query_bytes,
                               cudaMemcpyHostToDevice, stream));
    CUDA_CHECK(cudaMemcpyAsync(device_rows, fixture.rows.data(), row_bytes,
                               cudaMemcpyHostToDevice, stream));
    CUDA_CHECK(cudaMemcpyAsync(device_scores, pinned, output_bytes, cudaMemcpyHostToDevice, stream));
    CUDA_CHECK(cuvs_page_overlay_launch_scores(stream, device_query, device_rows, fixture.dim,
                                             fixture.count, metric.id, device_scores + output_pad));
    CUDA_CHECK(cudaMemcpyAsync(pinned, device_scores, output_bytes, cudaMemcpyDeviceToHost, stream));
    CUDA_CHECK(cudaStreamSynchronize(stream));

    for (size_t i = 0; i < output_pad; ++i)
    {
        if (float_bits(pinned[i]) != float_bits(canary) ||
            float_bits(pinned[output_pad + fixture.count + i]) != float_bits(canary))
        {
            std::fprintf(stderr, "output guard overwritten case=%s metric=%s pad=%zu\n",
                         name, metric.name, i);
            std::exit(EXIT_FAILURE);
        }
    }

    const bool finite_query = finite_vector(fixture.query.data(), fixture.dim);
    for (uint32_t r = 0; r < fixture.count; ++r)
    {
        const float *row = fixture.row(r);
        const float expected = ordered_score(fixture.query.data(), row, fixture.dim, metric.id);
        const bool need_nan = !finite_query || !finite_vector(row, fixture.dim) || !std::isfinite(expected);
        const float actual = pinned[output_pad + r];

        required_fallbacks += need_nan;
        if (std::isnan(actual))
        {
            if ((float_bits(actual) & UINT32_C(0x00400000)) == 0)
            {
                std::fprintf(stderr, "signaling NaN case=%s metric=%s row=%" PRIu32 "\n",
                             name, metric.name, r);
                std::exit(EXIT_FAILURE);
            }
            continue;
        }
        if (need_nan || !std::isfinite(actual) || float_bits(actual) != float_bits(expected))
        {
            std::fprintf(stderr,
                         "score mismatch case=%s metric=%s dim=%" PRIu32 " row=%" PRIu32
                         " got=0x%08" PRIx32 " oracle=0x%08" PRIx32 " required_nan=%d\n",
                         name, metric.name, fixture.dim, r, float_bits(actual), float_bits(expected),
                         static_cast<int>(need_nan));
            std::exit(EXIT_FAILURE);
        }
        ++hits;
    }

    if (hits < min_hits)
    {
        std::fprintf(stderr, "insufficient certified scores case=%s metric=%s hits=%" PRIu32
                             "/%" PRIu32 " min=%" PRIu32 "\n",
                     name, metric.name, hits, fixture.count, min_hits);
        std::exit(EXIT_FAILURE);
    }
    std::printf("PASS case=%s metric=%s dim=%" PRIu32 " hits=%" PRIu32 "/%" PRIu32
                " required_nan=%" PRIu32 "\n",
                name, metric.name, fixture.dim, hits, fixture.count, required_fallbacks);
    CUDA_CHECK(cudaFreeHost(pinned));
    CUDA_CHECK(cudaFree(device_scores));
    CUDA_CHECK(cudaFree(device_rows));
    CUDA_CHECK(cudaFree(device_query));
}

void
run_metrics(cudaStream_t stream, const char *name, const Fixture &fixture, uint32_t min_hits = 0)
{
    for (const Metric metric : metrics)
        run_case(stream, name, fixture, metric, min_hits);
}

void
check_launch_contract(cudaStream_t stream)
{
    CUDA_CHECK(cuvs_page_overlay_launch_scores(stream, nullptr, nullptr, 0, 0, UINT32_MAX, nullptr));
    float *device = nullptr;
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void **>(&device), 3 * sizeof(float)));
    CHECK(cuvs_page_overlay_launch_scores(stream, device, device + 1, 0, 1, l2.id, device + 2) ==
          cudaErrorInvalidValue);
    CHECK(cuvs_page_overlay_launch_scores(stream, device, device + 1, 1, 1, UINT32_MAX, device + 2) ==
          cudaErrorInvalidValue);
    CHECK(cuvs_page_overlay_launch_scores(stream, nullptr, device + 1, 1, 1, l2.id, device + 2) ==
          cudaErrorInvalidValue);
    CHECK(cuvs_page_overlay_launch_scores(stream, device, nullptr, 1, 1, l2.id, device + 2) ==
          cudaErrorInvalidValue);
    CHECK(cuvs_page_overlay_launch_scores(stream, device, device + 1, 1, 1, l2.id, nullptr) ==
          cudaErrorInvalidValue);
    CUDA_CHECK(cudaFree(device));
    std::puts("PASS case=launch-contract");
}

void
fill_normal(std::vector<float> &values, uint32_t &seed)
{
    for (float &value : values)
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        value = static_cast<float>(seed >> 8) * 0x1p-23f - 1.0f;
    }
}

void
check_normal(cudaStream_t stream)
{
    constexpr uint32_t rows = 257;
    Fixture fixture(normal_dim, rows);
    uint32_t row_seed = UINT32_C(0x45bc2ef1), query_seed = UINT32_C(0x8a6de357);

    fill_normal(fixture.rows, row_seed);
    for (int q = 0; q < 4; ++q)
    {
        fill_normal(fixture.query, query_seed);
        const std::string name = "normal-" + std::to_string(q);

        run_metrics(stream, name.c_str(), fixture, (90 * rows + 99) / 100);
    }
}

void
check_short_and_odd(cudaStream_t stream)
{
    constexpr uint32_t shapes[][2] = {
        {1, 1}, {3, 4}, {31, 17}, {32, 17}, {33, 17}, {65, 17}, {769, 17},
    };
    uint32_t seed = UINT32_C(0x2f093ac5);

    for (const auto &shape : shapes)
    {
        Fixture fixture(shape[0], shape[1]);

        fill_normal(fixture.query, seed);
        fill_normal(fixture.rows, seed);
        run_metrics(stream, "short-odd", fixture);
    }
}

void
check_near_ties(cudaStream_t stream)
{
    const float inf = std::numeric_limits<float>::infinity();
    const float next = std::nextafter(1.0f, inf);
    Fixture fixture(normal_dim, 6);

    for (uint32_t r = 0; r < fixture.count; ++r)
        fixture.row(r)[0] = 1.0f;
    fixture.row(1)[1] = 0x1p-12f;
    fixture.row(2)[1] = std::nextafter(0x1p-12f, inf);
    fixture.row(3)[1] = std::nextafter(0x1p-12f, 0.0f);
    fixture.row(4)[0] = -1.0f;
    fixture.row(5)[normal_dim - 1] = 0x1.4p-12f;
    expect_score(fixture, l2, 1, 1.0f);
    expect_score(fixture, l2, 2, next);
    expect_score(fixture, l2, 5, next);
    run_case(stream, "near-ties", fixture, l2);

    std::fill(fixture.rows.begin(), fixture.rows.end(), 0.0f);
    fixture.query[0] = fixture.query[1] = 1.0f;
    for (uint32_t r = 0; r < fixture.count; ++r)
        fixture.row(r)[0] = 1.0f;
    fixture.row(1)[1] = 0x1p-24f;
    fixture.row(2)[1] = std::nextafter(0x1p-24f, inf);
    fixture.row(3)[1] = std::nextafter(0x1p-24f, 0.0f);
    fixture.row(4)[1] = -0x1p-24f;
    fixture.row(5)[0] = next;
    expect_score(fixture, ip, 1, 1.0f);
    expect_score(fixture, ip, 2, next);
    expect_score(fixture, ip, 4, std::nextafter(1.0f, 0.0f));
    run_case(stream, "near-ties", fixture, ip);

    std::fill(fixture.rows.begin(), fixture.rows.end(), 0.0f);
    fixture.query[1] = 0.0f;
    fixture.row(0)[1] = 1.0f;
    fixture.row(1)[0] = 0x1p-24f;
    fixture.row(1)[1] = 1.0f;
    fixture.row(2)[0] = -0x1p-23f;
    fixture.row(2)[1] = 1.0f;
    fixture.row(3)[0] = 1.0f;
    fixture.row(3)[normal_dim - 1] = 0x1p-20f;
    fixture.row(4)[0] = -1.0f;
    expect_score(fixture, cosine, 0, 1.0f);
    expect_score(fixture, cosine, 1, std::nextafter(1.0f, 0.0f));
    expect_score(fixture, cosine, 2, next);
    expect_score(fixture, cosine, 4, 2.0f);
    expect_score(fixture, cosine, 5, 1.0f);
    run_case(stream, "near-ties", fixture, cosine);
}

void
check_subnormals(cudaStream_t stream)
{
    const float denorm = std::numeric_limits<float>::denorm_min();
    Fixture fixture(33, 6);

    fixture.row(0)[0] = 0x1p-74f;
    fixture.row(1)[0] = 0x1p-75f;
    fixture.row(2)[0] = -denorm;
    fixture.row(3)[0] = denorm;
    expect_score(fixture, l2, 0, 2 * denorm);
    expect_score(fixture, l2, 1, +0.0f);
    run_case(stream, "subnormal", fixture, l2);

    fixture.query[0] = FLT_MIN;
    fixture.row(0)[0] = 0.5f;
    fixture.row(1)[0] = -0.5f;
    fixture.row(2)[0] = -FLT_MIN;
    fixture.row(3)[0] = FLT_MIN;
    fixture.row(4)[0] = denorm;
    fixture.row(5)[0] = -denorm;
    expect_score(fixture, ip, 0, 0x1p-127f);
    expect_score(fixture, ip, 1, -0x1p-127f);
    expect_score(fixture, ip, 2, -0.0f);
    expect_score(fixture, ip, 3, +0.0f);
    run_case(stream, "subnormal", fixture, ip);

    fixture.query[0] = denorm;
    fixture.row(0)[0] = 1.0f;
    fixture.row(1)[0] = -1.0f;
    expect_score(fixture, ip, 0, denorm);
    expect_score(fixture, ip, 1, -denorm);
    expect_score(fixture, cosine, 0, +0.0f);
    expect_score(fixture, cosine, 1, 2.0f);
    run_case(stream, "subnormal-input", fixture, ip);
    run_case(stream, "subnormal-input", fixture, cosine);
}

void
check_invalid(cudaStream_t stream)
{
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Fixture fixture(normal_dim, 7);

    fixture.query[0] = fixture.query[1] = FLT_MAX;
    fixture.query[2] = 1.0f;
    fixture.row(0)[0] = fixture.row(0)[1] = FLT_MAX;
    fixture.row(1)[0] = fixture.row(1)[1] = -FLT_MAX;
    fixture.row(2)[0] = FLT_MAX;
    fixture.row(2)[1] = -FLT_MAX;
    fixture.row(2)[2] = 1.0f;
    fixture.row(3)[normal_dim - 1] = nan;
    fixture.row(4)[normal_dim - 1] = inf;
    fixture.row(5)[normal_dim - 1] = -inf;
    expect_score(fixture, l2, 1, inf);
    expect_score(fixture, ip, 0, inf);
    expect_score(fixture, ip, 1, -inf);
    expect_score(fixture, ip, 2, 1.0f);
    expect_score(fixture, cosine, 3, 1.0f);
    run_metrics(stream, "overflow-and-nonfinite-row", fixture);

    fixture.query[normal_dim - 1] = nan;
    run_metrics(stream, "nan-query", fixture);
    fixture.query[normal_dim - 1] = -inf;
    run_metrics(stream, "inf-query", fixture);

    std::fill(fixture.query.begin(), fixture.query.end(), 0.0f);
    expect_score(fixture, cosine, 0, 1.0f);
    run_case(stream, "zero-query", fixture, cosine);
}

void
check_guard(cudaStream_t stream)
{
    constexpr uint32_t strides[] = {8, 16, 32, 64, 128, 256, 512};
    constexpr uint32_t nstrides = sizeof(strides) / sizeof(*strides);
    Fixture fixture(normal_dim, 2 * nstrides);
    std::fill(fixture.query.begin(), fixture.query.end(), 1.0f);

    for (uint32_t i = 0; i < nstrides; ++i)
    {
        float *winner = fixture.row(2 * i);
        float *lost = fixture.row(2 * i + 1);

        winner[0] = 0x1p60f;
        winner[1] = -0x1p60f;
        winner[strides[i]] = 1.0f;
        lost[0] = 0x1p60f;
        lost[1] = 1.0f;
        lost[strides[i]] = -0x1p60f;
        expect_score(fixture, ip, 2 * i, 1.0f);
        expect_score(fixture, ip, 2 * i + 1, +0.0f);
    }
    run_case(stream, "guard", fixture, ip);
    run_case(stream, "guard-cosine", fixture, cosine);
}

} // namespace

int
main(int argc, char **argv)
{
    const std::string mode = argc > 1 ? argv[1] : "all";

    if (argc > 2 || (mode != "all" && mode != "normal" && mode != "edge" && mode != "guard"))
    {
        std::fprintf(stderr, "usage: %s [all|normal|edge|guard]\n", argv[0]);
        return 2;
    }

    CHECK(std::fegetround() == FE_TONEAREST);
    cudaStream_t stream = nullptr;

    CUDA_CHECK(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
    if (mode == "all" || mode == "normal")
        check_normal(stream);
    if (mode == "all" || mode == "edge")
    {
        check_launch_contract(stream);
        check_short_and_odd(stream);
        check_near_ties(stream);
        check_subnormals(stream);
        check_invalid(stream);
    }
    if (mode == "all" || mode == "guard")
        check_guard(stream);
    CUDA_CHECK(cudaStreamDestroy(stream));
    return EXIT_SUCCESS;
}
