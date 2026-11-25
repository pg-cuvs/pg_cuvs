/* Link with the native overlay and wrapper objects, pthread, and libm. */
#include "cuvs_page_overlay_gpu.h"
#include "cuvs_wrapper.h"

#include <stdio.h>
#include <stdlib.h>

#if !defined(__x86_64__)
int
main(void)
{
    puts("SKIP: page overlay GPU collect requires x86-64 host FP checks");
    return 77;
}
#else

#include <fenv.h>
#include <immintrin.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

enum { DEVICE = 0, DIM = 768, MIN_ROWS = 3, OUTPUT_CAPACITY = MIN_ROWS + 2 };
static const size_t initial_pool_bytes = (size_t) 64 << 20;

typedef struct AdmissionLedger
{
    size_t pending;
    size_t resident;
    size_t reserves;
    size_t finishes;
    size_t releases;
} AdmissionLedger;

static struct
{
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    AdmissionLedger ledger;
    size_t stop_on_reserve;
    bool entered;
    bool resume;
    bool worker_done;
} admission_state = {.mutex = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER};

static bool
fake_reserve(int device, size_t bytes)
{
    CHECK(device == DEVICE && bytes > 0);
    CHECK(pthread_mutex_lock(&admission_state.mutex) == 0);
    AdmissionLedger *ledger = &admission_state.ledger;

    ledger->pending += bytes;
    ++ledger->reserves;
    if (ledger->reserves == admission_state.stop_on_reserve)
    {
        admission_state.entered = true;
        CHECK(pthread_cond_broadcast(&admission_state.changed) == 0);
        while (!admission_state.resume)
            CHECK(pthread_cond_wait(&admission_state.changed, &admission_state.mutex) == 0);
    }
    CHECK(pthread_mutex_unlock(&admission_state.mutex) == 0);
    return true;
}

static void
fake_finish(int device, size_t bytes, bool resident)
{
    CHECK(device == DEVICE && bytes > 0 && resident);
    CHECK(pthread_mutex_lock(&admission_state.mutex) == 0);
    AdmissionLedger *ledger = &admission_state.ledger;

    CHECK(ledger->pending >= bytes && ledger->finishes < ledger->reserves);
    ledger->pending -= bytes;
    ledger->resident += bytes;
    ++ledger->finishes;
    CHECK(pthread_mutex_unlock(&admission_state.mutex) == 0);
}

static void
fake_release(int device, size_t bytes)
{
    CHECK(device == DEVICE && bytes > 0);
    CHECK(pthread_mutex_lock(&admission_state.mutex) == 0);
    AdmissionLedger *ledger = &admission_state.ledger;

    CHECK(ledger->resident >= bytes && ledger->releases < ledger->finishes);
    ledger->resident -= bytes;
    ++ledger->releases;
    CHECK(pthread_mutex_unlock(&admission_state.mutex) == 0);
}

static const CuvsPageOverlayGpuAdmission admission = {
    .reserve = fake_reserve,
    .finish = fake_finish,
    .release = fake_release,
};

typedef struct Accounting
{
    AdmissionLedger ledger;
    CuvsRmmPoolStats pool;
    size_t host_used;
} Accounting;

/* Call only between collect calls or while the worker is at the reserve gate. */
static Accounting
read_accounting(const CuvsPageOverlayBudget *budget)
{
    Accounting result;

    CHECK(pthread_mutex_lock(&admission_state.mutex) == 0);
    result.ledger = admission_state.ledger;
    CHECK(pthread_mutex_unlock(&admission_state.mutex) == 0);
    CHECK(cuvs_rmm_pool_stats(DEVICE, &result.pool) == 0);
    CHECK(result.pool.device_id == DEVICE);
    result.host_used = __atomic_load_n(&budget->used, __ATOMIC_RELAXED);
    CHECK(result.pool.used_bytes == result.ledger.resident);
    return result;
}

static void
expect_same_accounting(Accounting before, Accounting after)
{
    CHECK(after.ledger.pending == before.ledger.pending);
    CHECK(after.ledger.resident == before.ledger.resident);
    CHECK(after.ledger.reserves == before.ledger.reserves);
    CHECK(after.ledger.finishes == before.ledger.finishes);
    CHECK(after.ledger.releases == before.ledger.releases);
    CHECK(after.pool.used_bytes == before.pool.used_bytes);
    CHECK(after.pool.reserved_bytes == before.pool.reserved_bytes);
    CHECK(after.host_used == before.host_used);
}

static void
expect_empty_device(Accounting state, uint64_t initial_reserved)
{
    CHECK(state.ledger.pending == 0 && state.ledger.resident == 0);
    CHECK(state.ledger.reserves == state.ledger.finishes);
    CHECK(state.ledger.finishes == state.ledger.releases);
    CHECK(state.pool.used_bytes == 0);
    CHECK(state.pool.reserved_bytes == initial_reserved);
}

typedef struct CollectRun
{
    CuvsPageOverlayBudget *budget;
    const CuvsPageOverlayChunk *chunk;
    uint32_t gpu_rows;
    int count;
    CuvsPageOverlayResult results[OUTPUT_CAPACITY];
} CollectRun;

static const CuvsPageSearchRequest search = {.n_slots = 16, .folded = 41, .tail = 47};
static const CuvsCmdFrame cmd = {.dim = DIM, .metric = CUVS_METRIC_L2, .k = 1};
static const float query[DIM] = {1.0f, -2.0f};
static const CuvsPageOverlayResult guard = {
    .slot = CUVS_PAGE_SLOT_INVALID, .distance = -123.0f, .block = UINT32_MAX,
};
static const CuvsPageOverlayResult graph_seed = {.slot = 2, .distance = 8.0f, .block = 0};

static CuvsPageOverlayChunk *
make_chunk(CuvsPageOverlayBudget *budget)
{
    const CuvsPagePosition marks[] = {
        {70, CUVS_PAGE_POSITION_VECTOR},
        {0, CUVS_PAGE_POSITION_DEAD},
        {72, CUVS_PAGE_POSITION_VECTOR},
        {73, CUVS_PAGE_POSITION_VECTOR},
        {0, CUVS_PAGE_POSITION_DEAD},
        {75, CUVS_PAGE_POSITION_VECTOR},
        {0, CUVS_PAGE_POSITION_DEAD},
        {77, CUVS_PAGE_POSITION_VECTOR},
    };
    const float rows[5][DIM] = {
        {111.0f},
        {4.0f, -2.0f, [DIM - 1] = -1.0f},
        {1.0f, -4.0f, 3.0f},
        {0.0f, -2.0f, [DIM - 1] = 0x1p-12f},
        {-111.0f},
    };
    const CuvsPageOverlayRequest request = {
        .from = 40, .positions = 8, .vector_rows = 5,
    };
    CuvsPageOverlayChunk *chunk = NULL;

    CHECK(cuvs_page_overlay_alloc(budget, &request, sizeof(marks) + sizeof(rows), &chunk) ==
          CUVS_STATUS_OK);
    CHECK(chunk != NULL);
    memcpy(chunk->marks, marks, sizeof(marks));
    memcpy(chunk->marks + chunk->positions, rows, sizeof(rows));
    CHECK(cuvs_page_overlay_valid(chunk, request.vector_rows, true));
    CHECK(cuvs_page_overlay_gpu_mirror(chunk) == NULL);
    return chunk;
}

static void
reset_results(CollectRun *run)
{
    run->count = 1;
    for (size_t i = 0; i < OUTPUT_CAPACITY; ++i)
        run->results[i] = guard;
    run->results[0] = graph_seed;
}

static void
collect(CollectRun *run)
{
    run->gpu_rows = cuvs_page_overlay_gpu_collect(DEVICE, MIN_ROWS, &admission,
                                                  run->budget, run->chunk, search.tail,
                                                  &search, query, &cmd, run->results, &run->count);
}

static void
expect_result(const CuvsPageOverlayResult *actual, const CuvsPageOverlayResult *expected)
{
    if (actual->slot != expected->slot || actual->block != expected->block ||
        memcmp(&actual->distance, &expected->distance, sizeof(actual->distance)) != 0)
    {
        fprintf(stderr, "FAIL result: slot=%" PRIu64 " block=%" PRIu32 " score=%a, "
                        "expected slot=%" PRIu64 " block=%" PRIu32 " score=%a\n",
                actual->slot, actual->block, (double) actual->distance,
                expected->slot, expected->block, (double) expected->distance);
        exit(EXIT_FAILURE);
    }
}

static void
expect_collected(const CollectRun *run)
{
    /* Logical positions 42, 43, 45. Squared L2 values are 9+1, 4+9,
     * and 1+2^-24 (binary32 RN-even rounds the last one to 1). */
    const CuvsPageOverlayResult expected[MIN_ROWS] = {
        {.slot = 17, .distance = 10.0f, .block = 72},
        {.slot = 18, .distance = 13.0f, .block = 73},
        {.slot = 20, .distance = 1.0f, .block = 75},
    };

    CHECK(run->gpu_rows == MIN_ROWS && run->count == 1 + MIN_ROWS);
    expect_result(&run->results[0], &graph_seed);
    for (size_t i = 0; i < MIN_ROWS; ++i)
        expect_result(&run->results[i + 1], &expected[i]);
    expect_result(&run->results[OUTPUT_CAPACITY - 1], &guard);
}

static void
check_fp_refusal(CollectRun *run, const char *label, unsigned mxcsr_bits, int fenv_round)
{
    const Accounting before = read_accounting(run->budget);
    const unsigned original_mxcsr = _mm_getcsr();
    fenv_t original_env;

    CHECK(fegetenv(&original_env) == 0);
    reset_results(run);
    CHECK(fesetround(fenv_round) == 0);
    /* Restore the original SSE rounding bits even for FE_DOWNWARD. This
     * changes x87 alone and detects removal of the separate fegetround guard. */
    _mm_setcsr(original_mxcsr | mxcsr_bits);
    CHECK(fegetround() == fenv_round && _mm_getcsr() == (original_mxcsr | mxcsr_bits));
    collect(run);
    CHECK(fesetenv(&original_env) == 0);
    _mm_setcsr(original_mxcsr);

    CHECK(run->gpu_rows == 0 && run->count == 1);
    expect_result(&run->results[0], &graph_seed);
    for (size_t i = 1; i < OUTPUT_CAPACITY; ++i)
        expect_result(&run->results[i], &guard);
    expect_same_accounting(before, read_accounting(run->budget));
    printf("PASS %s refused GPU collect and preserved results and accounting\n", label);
}

static void *
collect_worker(void *arg)
{
    CollectRun *run = arg;

    reset_results(run);
    collect(run);
    CHECK(pthread_mutex_lock(&admission_state.mutex) == 0);
    admission_state.worker_done = true;
    CHECK(pthread_cond_broadcast(&admission_state.changed) == 0);
    CHECK(pthread_mutex_unlock(&admission_state.mutex) == 0);
    return NULL;
}

static void
deadlock_watchdog(int signal_number)
{
    static const char message[] = "FAIL: reclaim/collect rendezvous did not complete\n";

    (void) signal_number;
    ssize_t written = write(STDERR_FILENO, message, sizeof(message) - 1);

    (void) written;
    _Exit(EXIT_FAILURE);
}

static void
check_busy_reclaim_and_reupload(CollectRun *run, Accounting empty)
{
    pthread_t worker;

    /* The first reserve restores device scratch. The next reserve pauses
     * before allocating the row mirror, while collect still owns its lane. */
    CHECK(pthread_mutex_lock(&admission_state.mutex) == 0);
    admission_state.stop_on_reserve = empty.ledger.reserves + 2;
    CHECK(pthread_mutex_unlock(&admission_state.mutex) == 0);

    /* A broken blocking reclaim would deadlock the rendezvous. This watchdog
     * can only fail the test. A pass always requires the reserve gate. */
    CHECK(signal(SIGALRM, deadlock_watchdog) != SIG_ERR);
    alarm(120);
    CHECK(pthread_create(&worker, NULL, collect_worker, run) == 0);
    CHECK(pthread_mutex_lock(&admission_state.mutex) == 0);
    while (!admission_state.entered && !admission_state.worker_done)
        CHECK(pthread_cond_wait(&admission_state.changed, &admission_state.mutex) == 0);
    CHECK(admission_state.entered && !admission_state.worker_done);
    CHECK(pthread_mutex_unlock(&admission_state.mutex) == 0);

    Accounting busy = read_accounting(run->budget);
    CHECK(busy.ledger.pending > 0 && busy.pool.used_bytes > 0);
    CHECK(busy.ledger.finishes == empty.ledger.finishes + 1);
    CHECK(!cuvs_page_overlay_gpu_reclaim(DEVICE));
    expect_same_accounting(busy, read_accounting(run->budget));
    puts("PASS busy reclaim refused with scratch live and a reservation pending");

    CHECK(pthread_mutex_lock(&admission_state.mutex) == 0);
    admission_state.resume = true;
    CHECK(pthread_cond_broadcast(&admission_state.changed) == 0);
    CHECK(pthread_mutex_unlock(&admission_state.mutex) == 0);
    CHECK(pthread_join(worker, NULL) == 0);
    alarm(0);
    expect_collected(run);
}

int
main(void)
{
    if (!cuvs_gpu_available())
    {
        puts("SKIP: no CUDA device");
        return 77;
    }
    CHECK(fesetenv(FE_DFL_ENV) == 0);
    CHECK(fegetround() == FE_TONEAREST);
    const unsigned fp_mask = _MM_ROUND_MASK | _MM_FLUSH_ZERO_MASK |
        _MM_DENORMALS_ZERO_MASK | _MM_MASK_MASK;
    CHECK((_mm_getcsr() & fp_mask) == (_MM_ROUND_NEAREST | _MM_MASK_MASK));
    CHECK(cuvs_rmm_pool_init(DEVICE, initial_pool_bytes, 50) == 0);

    CuvsPageOverlayBudget budget = {.limit = (size_t) 8 << 20};
    Accounting initial = read_accounting(&budget);

    CHECK(initial.pool.reserved_bytes >= initial_pool_bytes);
    expect_empty_device(initial, initial.pool.reserved_bytes);
    cuvs_page_overlay_gpu_init();
    CuvsPageOverlayChunk *chunk = make_chunk(&budget);
    const size_t chunk_bytes = chunk->charged_bytes;
    CollectRun run = {.budget = &budget, .chunk = chunk};

    CHECK(budget.used == chunk_bytes);
    cuvs_page_overlay_retain(chunk);
    reset_results(&run);
    collect(&run);
    expect_collected(&run);
    Accounting cached = read_accounting(&budget);
    CHECK(cached.pool.used_bytes > 0 && cached.ledger.pending == 0);
    CHECK(cached.ledger.reserves == cached.ledger.finishes);
    CHECK(cached.ledger.releases == 0 && cached.host_used > chunk_bytes);
    CHECK(cached.pool.reserved_bytes == initial.pool.reserved_bytes);
    CHECK(cuvs_page_overlay_gpu_mirror(chunk) != NULL);
    printf("PASS GPU collect with RMM used_bytes=%" PRIu64 "\n", cached.pool.used_bytes);

    check_fp_refusal(&run, "DAZ", _MM_DENORMALS_ZERO_MASK, FE_TONEAREST);
    check_fp_refusal(&run, "FTZ", _MM_FLUSH_ZERO_MASK, FE_TONEAREST);
    check_fp_refusal(&run, "x87 FE_DOWNWARD", 0, FE_DOWNWARD);

    CHECK(cuvs_page_overlay_gpu_reclaim(DEVICE));
    Accounting reclaimed = read_accounting(&budget);
    expect_empty_device(reclaimed, initial.pool.reserved_bytes);
    CHECK(reclaimed.host_used >= chunk_bytes && reclaimed.host_used < cached.host_used);
    cuvs_page_overlay_release(chunk); /* Drop the registry reference, keep the reader pin. */
    CHECK(__atomic_load_n(&chunk->refs, __ATOMIC_RELAXED) == 1);
    CHECK(cuvs_page_overlay_valid(chunk, 5, true));
    expect_same_accounting(reclaimed, read_accounting(&budget));
    puts("PASS reclaim reduced live device usage to zero and kept the pinned chunk");

    check_busy_reclaim_and_reupload(&run, reclaimed);
    Accounting reuploaded = read_accounting(&budget);
    CHECK(reuploaded.ledger.pending == 0);
    CHECK(reuploaded.ledger.reserves == reclaimed.ledger.reserves + 2);
    CHECK(reuploaded.ledger.finishes == reuploaded.ledger.reserves);
    CHECK(reuploaded.ledger.releases == reclaimed.ledger.releases);
    CHECK(reuploaded.pool.used_bytes == cached.pool.used_bytes);
    CHECK(reuploaded.pool.reserved_bytes == initial.pool.reserved_bytes);
    printf("PASS GPU re-upload restored RMM used_bytes=%" PRIu64 "\n", reuploaded.pool.used_bytes);

    cuvs_page_overlay_release(chunk);
    Accounting chunk_released = read_accounting(&budget);
    CHECK(chunk_released.pool.used_bytes < reuploaded.pool.used_bytes);
    CHECK(chunk_released.ledger.releases > reuploaded.ledger.releases);
    CHECK(chunk_released.host_used < reuploaded.host_used);
    CHECK(cuvs_page_overlay_gpu_reclaim(DEVICE));
    Accounting drained = read_accounting(&budget);
    CHECK(drained.host_used == 0);
    expect_empty_device(drained, initial.pool.reserved_bytes);
    puts("PASS final chunk release and reclaim balanced host and device accounting");
    return 0;
}
#endif
