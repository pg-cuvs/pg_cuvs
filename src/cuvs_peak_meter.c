/* I3-B lock-free pool peak meter. See cuvs_peak_meter.h. */
#include "cuvs_peak_meter.h"

#include <stdatomic.h>

static _Atomic size_t g_live[CUVS_PEAK_METER_GPUS];
static _Atomic size_t g_peak[CUVS_PEAK_METER_GPUS];
static _Atomic int    g_enabled = 1;

static int
ok(int dev)
{
    return dev >= 0 && dev < CUVS_PEAK_METER_GPUS;
}

void
cuvs_peak_meter_set_enabled(int on)
{
    atomic_store_explicit(&g_enabled, on ? 1 : 0, memory_order_relaxed);
}

int
cuvs_peak_meter_enabled(void)
{
    return atomic_load_explicit(&g_enabled, memory_order_relaxed);
}

size_t
cuvs_peak_meter_alloc(int dev, size_t bytes)
{
    size_t now, prev;

    if (!ok(dev))
        return 0;
    now = atomic_fetch_add_explicit(&g_live[dev], bytes, memory_order_relaxed) + bytes;
    if (!atomic_load_explicit(&g_enabled, memory_order_relaxed))
        return now;
    prev = atomic_load_explicit(&g_peak[dev], memory_order_relaxed);
    while (now > prev &&
           !atomic_compare_exchange_weak_explicit(&g_peak[dev], &prev, now,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed))
        ;
    return now;
}

void
cuvs_peak_meter_free(int dev, size_t bytes)
{
    if (ok(dev))
        atomic_fetch_sub_explicit(&g_live[dev], bytes, memory_order_relaxed);
}

size_t
cuvs_peak_meter_live(int dev)
{
    return ok(dev) ? atomic_load_explicit(&g_live[dev], memory_order_relaxed) : 0;
}

size_t
cuvs_peak_meter_peak(int dev)
{
    return ok(dev) ? atomic_load_explicit(&g_peak[dev], memory_order_relaxed) : 0;
}

void
cuvs_peak_meter_reset(int dev)
{
    if (ok(dev))
        atomic_store_explicit(&g_peak[dev],
                              atomic_load_explicit(&g_live[dev], memory_order_relaxed),
                              memory_order_relaxed);
}
