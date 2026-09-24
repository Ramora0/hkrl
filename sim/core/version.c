#include <stdint.h>
#include <math.h>
#include "core/hksim.h"
HKSIM_API uint32_t hksim_abi_version(void) { return 1; }
// FP-contract probe: with -ffp-contract=off this is two roundings, never an FMA.
HKSIM_API float hksim_fp_probe(float a, float b, float c) { return a * b + c; }
