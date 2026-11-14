#ifndef PGC_UVS_RESIDENT_NN_UPPER_CONTRACT_H
#define PGC_UVS_RESIDENT_NN_UPPER_CONTRACT_H
#include <stdint.h>
#include <stddef.h>
/* Private product/SDK protocol, used only for non-disk resident NN hierarchy.
 * These impossible ACE partition counts carry the base computation dtype. */
#define PGC_UVS_NN_UPPER_FP16 SIZE_MAX
#define PGC_UVS_NN_UPPER_FP32 (SIZE_MAX - 1)
#endif
