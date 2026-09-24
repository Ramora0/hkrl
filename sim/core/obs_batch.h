#pragma once
/* Internal support for the hksim.h fast path (hksim_obs_batch): the string->id vocab and the
 * per-instance numeric observation that build_obs fills instead of, or alongside, the wire pack.
 * Column counts must equal obs.h's OBS_COMBAT_FEAT / OBS_TERRAIN_FEAT / OBS_GLOBAL_DIM. */
#include <stdint.h>
#include "hksim.h"

enum {
    NB_COMBAT_FEAT  = 14,    /* HO:793-797; obs-wire.md 3.3 */
    NB_TERRAIN_FEAT = 8,     /* HO:516;     obs-wire.md 4.2 */
    NB_GLOBAL_DIM   = 33,    /* SE:11;      obs-wire.md 2 */
    NB_MAX_COMBAT   = 255,   /* BinaryProtocol writes nc as u8 (BP:48); build_obs clamps to this */
};

/* One instance's observation as numbers only.  Strings are borrowed pointers into the FSM module's
 * tables, interned into the caller's vocab only at hksim_obs_batch time. */
typedef struct {
    int32_t     n_combat, n_terrain;
    float       combat[NB_MAX_COMBAT][NB_COMBAT_FEAT];
    const char *kind[NB_MAX_COMBAT];
    const char *parent[NB_MAX_COMBAT];
    float      *terrain;                 /* [terrain_cap][8], grown on demand */
    int32_t     terrain_cap;
    float       global_state[NB_GLOBAL_DIM];
    float       step[3];                 /* damage_landed, hits_taken, hp_healed */
    uint8_t     done;
    uint8_t     valid;                   /* 0 until a step/reset has filled it under HKSIM_OBS_BATCH */
} obs_numeric;

void nb_free(obs_numeric *nb);

/* Write one instance's cached numbers into row `slot` of the caller's arrays.  Interns kind/parent
 * through `vocab`.  Rows past the caps are counted, not written. */
void nb_emit(const obs_numeric *nb, hksim_batch *out, int32_t slot, hksim_vocab *vocab);
