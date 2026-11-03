#ifndef CUVS_PEAK_METER_H
#define CUVS_PEAK_METER_H

/*
 * I3-B (UC-6 A, KB-17, CK-34): always-on lock-free peak of live pool bytes per
 * GPU. The RMM usage adaptor (cuvs_wrapper.cu) calls alloc/free; `live` is a
 * plain atomic counter and `peak` a CAS maximum (no lock, unlike
 * statistics_resource_adaptor's shared_mutex). With the meter off
 * (`--rmm-peak-meter off`, diagnostics) only `live` is kept and the peak is not
 * updated. C11 atomics; callable from C++ through the extern "C" block.
 */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CUVS_PEAK_METER_GPUS 16

void   cuvs_peak_meter_set_enabled(int on);
int    cuvs_peak_meter_enabled(void);
/* Returns the live bytes right after this allocation (0 for a bad dev). */
size_t cuvs_peak_meter_alloc(int dev, size_t bytes);
void   cuvs_peak_meter_free(int dev, size_t bytes);
size_t cuvs_peak_meter_live(int dev);
size_t cuvs_peak_meter_peak(int dev);
/* Restart the window: peak = current live bytes. */
void   cuvs_peak_meter_reset(int dev);

#ifdef __cplusplus
}
#endif

#endif /* CUVS_PEAK_METER_H */
