/*
 * CK-34 (I3-B, UC-6 A): the lock-free pool peak meter.
 * (1) 8 threads allocate concurrently with no frees: peak == sum of all
 *     allocations exactly (every increase reaches the CAS maximum);
 * (2) interleaved alloc/free bursts: peak == the highest live level any
 *     thread observed, never less; live returns to 0;
 * (3) reset sets the peak to the current live bytes;
 * (4) off (--rmm-peak-meter off): live is kept, the peak is not updated.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "cuvs_peak_meter.h"

static int g_pass, g_fail;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define THREADS 8
#define OPS     200000

/* Portable start barrier (macOS has no pthread_barrier_t). */
static atomic_int g_ready;
static void start_together(void)
{
    atomic_fetch_add(&g_ready, 1);
    while (atomic_load(&g_ready) < THREADS)
        ;
}

static atomic_long g_lost;   /* peak seen below a level this thread produced */

static void *
alloc_only(void *arg)
{
    (void) arg;
    start_together();
    for (int i = 0; i < OPS; i++)
    {
        size_t now = cuvs_peak_meter_alloc(1, 256);
        /* The peak only grows and already covers this thread's level once
         * its update returns: a lower reading is a lost update. */
        if (cuvs_peak_meter_peak(1) < now)
            atomic_fetch_add(&g_lost, 1);
    }
    return NULL;
}

static void *
burst(void *arg)
{
    size_t *seen = arg;
    start_together();
    for (int i = 0; i < OPS / 10; i++)
    {
        for (int k = 0; k < 10; k++)
            cuvs_peak_meter_alloc(2, 512);
        size_t live = cuvs_peak_meter_live(2);
        if (live > *seen)
            *seen = live;
        for (int k = 0; k < 10; k++)
            cuvs_peak_meter_free(2, 512);
    }
    return NULL;
}

int
main(void)
{
    pthread_t t[THREADS];
    size_t seen[THREADS];

    /* (1) concurrent allocations only */
    cuvs_peak_meter_set_enabled(1);
    atomic_store(&g_ready, 0);
    for (int i = 0; i < THREADS; i++)
        pthread_create(&t[i], NULL, alloc_only, NULL);
    for (int i = 0; i < THREADS; i++)
        pthread_join(t[i], NULL);
    size_t total = (size_t) THREADS * OPS * 256;
    CHECK(cuvs_peak_meter_live(1) == total, "live %zu == %zu", cuvs_peak_meter_live(1), total);
    CHECK(cuvs_peak_meter_peak(1) == total, "peak %zu == %zu (no lost CAS update)",
          cuvs_peak_meter_peak(1), total);
    CHECK(atomic_load(&g_lost) == 0, "no thread saw the peak below its own level (%ld)",
          (long) atomic_load(&g_lost));

    /* (2) interleaved bursts */
    memset(seen, 0, sizeof seen);
    atomic_store(&g_ready, 0);
    for (int i = 0; i < THREADS; i++)
        pthread_create(&t[i], NULL, burst, &seen[i]);
    for (int i = 0; i < THREADS; i++)
        pthread_join(t[i], NULL);
    size_t max_seen = 0;
    for (int i = 0; i < THREADS; i++)
        if (seen[i] > max_seen)
            max_seen = seen[i];
    CHECK(cuvs_peak_meter_live(2) == 0, "live back to 0 (%zu)", cuvs_peak_meter_live(2));
    CHECK(cuvs_peak_meter_peak(2) >= max_seen, "peak %zu >= observed %zu",
          cuvs_peak_meter_peak(2), max_seen);
    CHECK(cuvs_peak_meter_peak(2) <= (size_t) THREADS * 10 * 512, "peak bounded by all bursts");

    /* (3) reset */
    cuvs_peak_meter_alloc(3, 1000);
    cuvs_peak_meter_alloc(3, 3000);
    cuvs_peak_meter_free(3, 3000);
    CHECK(cuvs_peak_meter_peak(3) == 4000, "peak before reset %zu", cuvs_peak_meter_peak(3));
    cuvs_peak_meter_reset(3);
    CHECK(cuvs_peak_meter_peak(3) == 1000, "reset -> live 1000 (%zu)", cuvs_peak_meter_peak(3));
    cuvs_peak_meter_alloc(3, 500);
    CHECK(cuvs_peak_meter_peak(3) == 1500, "window restarts (%zu)", cuvs_peak_meter_peak(3));

    /* (3b) reserve without commit (the allocation failed) never reaches the
     * peak once its bytes are given back. */
    size_t before = cuvs_peak_meter_peak(5);
    size_t lvl = cuvs_peak_meter_reserve(5, 1u << 30);
    cuvs_peak_meter_free(5, 1u << 30);
    CHECK(lvl == (1u << 30) && cuvs_peak_meter_peak(5) == before && cuvs_peak_meter_live(5) == 0,
          "failed allocation leaves the peak (%zu) and live (%zu)", cuvs_peak_meter_peak(5),
          cuvs_peak_meter_live(5));
    cuvs_peak_meter_commit(5, cuvs_peak_meter_reserve(5, 64));
    CHECK(cuvs_peak_meter_peak(5) == 64, "committed allocation reaches the peak");

    /* (4) off: live kept, peak not updated */
    cuvs_peak_meter_set_enabled(0);
    CHECK(!cuvs_peak_meter_enabled(), "off");
    cuvs_peak_meter_alloc(4, 7000);
    CHECK(cuvs_peak_meter_live(4) == 7000, "off keeps live (%zu)", cuvs_peak_meter_live(4));
    CHECK(cuvs_peak_meter_peak(4) == 0, "off: 0 peak updates (%zu)", cuvs_peak_meter_peak(4));
    cuvs_peak_meter_set_enabled(1);
    cuvs_peak_meter_alloc(4, 1);
    CHECK(cuvs_peak_meter_peak(4) == 7001, "back on resumes (%zu)", cuvs_peak_meter_peak(4));

    CHECK(cuvs_peak_meter_peak(-1) == 0 && cuvs_peak_meter_peak(CUVS_PEAK_METER_GPUS) == 0,
          "out-of-range device");
    printf("test_peak_meter: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
