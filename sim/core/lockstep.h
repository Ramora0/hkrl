#pragma once
/* INTERNAL: the per-frame lockstep harness (docs/lockstep.md).  sim/core/sim.c steps one frame of a recorded kind and
 * hands out the instance's parts; sim/core/lockstep_api.c reads and overwrites the sim's state field by field. */
#include <stdint.h>
#include <stddef.h>
#include "hksim.h"
#include "rng.h"
#include "phys.h"
typedef struct hks_arena hks_arena;

struct hero;

/* Frame kinds, as the recording shows them (FRAME flags, Env.step, Time.timeScale at the frame's end):
 *   STEP      the frozen frame that runs a step's ActionDecoder.ApplyAction (Env.step advanced)
 *   LIVE      a live frame whose LateUpdate runs (timeScale 1 at its end)
 *   LIVE_LAST the step's last live frame: finish_step, then FsmPauseGate closes PlayMakerLateUpdate (timeScale 0)
 *   FROZEN    a frozen frame without a step (timeScale stays 0) */
enum { HKSIM_FRAME_STEP = 0, HKSIM_FRAME_LIVE = 1, HKSIM_FRAME_LIVE_LAST = 2, HKSIM_FRAME_FROZEN = 3 };
HKSIM_API int hksim_frame(hksim *s, int kind, const int32_t action[4]);

typedef struct {
    hks_arena *arena;
    struct hero *hero;
    hk_rng *rng;
    phys_world *pw;
    phys_body_id hero_body;
    void *fsm_ctx;
    uint32_t *frame, *fixed_count, *step;
    float *time, *fixed_time, *tsll;
    char *err; size_t err_len;
} hksim_parts;
void hksim_get_parts(hksim *s, hksim_parts *p);
