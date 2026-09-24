/* Shared by every PlayMaker action file: the binding macros and the helpers used by more than one
 * category.  Each action is a line-by-line port of the cited ACT/<name>.cs (HutongGames.PlayMaker.Actions)
 * or HK/<name>.cs file; ACT_REGISTRIES (sim/gen_registries.py) collects every act_registry_<name>[]. */
#pragma once
#include "../fsm.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ST(T) T *s = (T *)a->st; fsm_inst *f = a->fsm; fsm_world *w = f->w; (void)w; (void)f; (void)s
#define FIELD(name) a_field_req(a, #name)
#define FIELD_OPT(name) a_field(a, #name)
#define EV(pv) ((pv) ? p_event(pv) : -1)

/* OnEnter bodies for actions whose only observable effect is their lifetime (act.c): Finish() at once, or
 * Finish() unless the action's everyFrame (or everyframe) flag is set. */
void act_finish_enter(act_inst *a);
void act_finish_unless_every_frame(act_inst *a);

/* FsmEventTarget field -> the arena index fsm_event_to() takes, or -1 for the default target */
static inline int32_t act_event_target(act_inst *a, const fsm_pv *t) { return t->kind == PV_EVTARGET ? (int32_t)(t - a->arena) : -1; }

/* ComponentAction<Rigidbody2D>.UpdateCache (ACT/ComponentAction.cs:25-41): re-resolve only when the target changed */
static inline bool act_cache_rb(act_inst *a, int32_t t, int32_t *cached, bool *has)
{
    if (t < 0) return false;
    if (*cached != t || !*has) { *cached = t; *has = go_has_rb(a->fsm->w, t); }
    return *has;
}

/* One velocity axis multiplied by `d` without crossing zero (DecelerateV2.cs, DecelerateXY.cs, DistanceFlySmooth.cs) */
static inline float act_damp_axis(float v, float d)
{
    if (v < 0.0f) { v *= d; if (v > 0.0f) v = 0.0f; }
    else if (v > 0.0f) { v *= d; if (v < 0.0f) v = 0.0f; }
    return v;
}

/* ActionHelpers.GetGameObjectFsm (variables.c) */
int32_t get_game_object_fsm(fsm_world *w, int32_t go, const char *fsm_name);

/* FsmVar (PV_FVAR pool: name, type, float, int, bool, string, v4, useVariable) accessors (variables.c) */
int32_t fvar_type(act_inst *a, const fsm_pv *fv);
float   fvar_float(act_inst *a, const fsm_pv *fv);
int32_t fvar_int(act_inst *a, const fsm_pv *fv);
bool    fvar_bool(act_inst *a, const fsm_pv *fv);
int32_t fvar_string(act_inst *a, const fsm_pv *fv);
void    fvar_store_bool(act_inst *a, const fsm_pv *fv, bool x);
void    fvar_store_int(act_inst *a, const fsm_pv *fv, int32_t x);
void    fvar_store_float(act_inst *a, const fsm_pv *fv, float x);
void    fvar_store_string(act_inst *a, const fsm_pv *fv, int32_t sid);

/* EaseFsmAction (ACT/EaseFsmAction.cs): the base of EaseFloat and EaseColor (math.c) */
typedef struct {
    const fsm_pv *time, *speed, *delay, *easeType, *reverse, *realTime;
    float runningTime, delayTime, percentage, from[4], to[4], result[4];
    int n;
    bool start, isRunning, finished, finishAction;
} ease_core;
void ease_bind(act_inst *a, ease_core *e);
void ease_enter(act_inst *a, ease_core *e);
void ease_update(act_inst *a, ease_core *e);
bool ease_final_is_from(act_inst *a, const ease_core *e);
