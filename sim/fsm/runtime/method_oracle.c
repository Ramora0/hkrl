/* Test-only entry points of the method-level oracle (tools/method_oracle.py, docs/method-oracle.md).
 *
 * The oracle mod records single calls of PlayMaker actions and ported component methods in the game
 * (oracle/Record/MethodRecorder.cs): identity, the inputs the call reads and the outputs it writes.  These
 * functions let the replay put a reset world into a recorded call's input state, run exactly that one
 * callback through the action's own vtable (or the ported component function), and read the outputs back.
 * Nothing here is reached by hksim_step: the functions poke instance state directly, the way the recorded
 * game state dictates, and are only as faithful as the inputs the record carries.
 *
 * Every call that can run sim code arms a trap context: a trap returns its HKSIM_ERR_* code and
 * hkmo_last_error() carries the message. */
#include "fsm/fsm.h"
#include "fsm/lifecycle.h"
#include "world_internal.h"
#include "core/sim_modules.h"
#include "fsm/components/components.h"
#include "hero/hero.h"
#include "core/phys.h"
#include "core/alloc.h"   /* per-instance arena: realloc of instance state goes through it */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "../actions/act_registry_list.inc"


static char g_err[600];
static hksim_trap_ctx g_trap;

#define ARM(w) if (setjmp(g_trap.jb) != 0) { snprintf(g_err, sizeof g_err, "%s", hksim_trap_message()); hksim_trap_disarm(); if (w) (w)->stack_depth = 0; return g_trap.code; } hksim_trap_arm(&g_trap); g_trap.armed = 1
#define DISARM() hksim_trap_disarm(); g_trap.armed = 0

HKSIM_API const char *hkmo_last_error(void) { return g_err; }

HKSIM_API fsm_world *hkmo_world(hksim *s)
{
    void *ctx = hksim_fsm_ctx(s);
    return ctx ? fsmi_world(ctx) : NULL;
}

/* ---- registry (coverage) ---- */
HKSIM_API int32_t hkmo_registry_count(void)
{
    int32_t n = 0;
    for (int r = 0; r < ACT_REGISTRIES_N; r++) n += *ACT_REGISTRIES[r].n;
    return n;
}
HKSIM_API const char *hkmo_registry_name(int32_t i)
{
    for (int r = 0; r < ACT_REGISTRIES_N; r++) {
        if (i < *ACT_REGISTRIES[r].n) return ACT_REGISTRIES[r].v[i]->type_short;
        i -= *ACT_REGISTRIES[r].n;
    }
    return NULL;
}

/* ---- identity ---- */
/* The canonical path of a sim GameObject: its generated path with the `$k` / `#k` pool-copy suffixes the game
 * does not have dropped from every segment (the rule of sim/core/rng.c fnv1a_owner). */
static void canon_path(const char *p, char *out, size_t cap)
{
    size_t o = 0;
    while (*p && o + 1 < cap) {
        if (*p == '$' || *p == '#') {
            const char *e = p + 1;
            while (*e >= '0' && *e <= '9') e++;
            if (e > p + 1 && (*e == '/' || *e == '\0')) { p = e; continue; }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
}

/* The nth GameObject (0-based, in id order) whose canonical path is `path`, or -1. */
HKSIM_API int32_t hkmo_find_go(fsm_world *w, const char *path, int32_t nth)
{
    char buf[512];
    for (int32_t g = 0; g < w->n_gos; g++) {
        const char *gp = go_path(w, g);
        if (!gp) continue;
        canon_path(gp, buf, sizeof buf);
        if (strcmp(buf, path) == 0 && nth-- == 0) return g;
    }
    return -1;
}
HKSIM_API const char *hkmo_go_path(fsm_world *w, int32_t go) { return go >= 0 && go < w->n_gos ? go_path(w, go) : ""; }

/* The first FSM named `fsm_name` on GameObject `go`; -1. */
HKSIM_API int32_t hkmo_find_fsm(fsm_world *w, int32_t go, const char *fsm_name)
{
    for (int32_t i = 0; i < w->n_fsms; i++)
        if (w->fsms[i].go == go && strcmp(w_str(w, w->fsms[i].def->fsm_name), fsm_name) == 0) return i;
    return -1;
}
HKSIM_API int32_t hkmo_state_index(fsm_world *w, int32_t fsm, const char *state)
{
    fsm_inst *f = &w->fsms[fsm];
    for (int32_t s = 0; s < f->n_states; s++) if (strcmp(state_name(f, s), state) == 0) return s;
    return -1;
}
/* The action's type (short name) from the scene table, "" when out of range; *ported = the sim has a vtable. */
HKSIM_API const char *hkmo_action_type(fsm_world *w, int32_t fsm, int32_t state, int32_t idx, int32_t *ported)
{
    fsm_inst *f = &w->fsms[fsm];
    *ported = 0;
    if (state < 0 || state >= f->n_states || idx < 0 || idx >= f->states[state].n_acts) return "";
    act_inst *a = &f->states[state].acts[idx];
    *ported = a->vt != NULL;
    return w_str(w, a->def->type_short);
}

/* ---- variables ---- */
/* The variable slots named `name` in one bucket (fsm -1: PlayMakerGlobals), in declaration order: an FSM can declare
 * two variables with one name (GG_Hornet_1 Hornet Boss 1/Control "Area Title", Knight/Nail Arts "Has Cyclone"). */
static int32_t vars_by_name(fsm_world *w, int32_t fsm, int bucket, const char *name, fsm_val **out, int32_t cap)
{
    int32_t sid = w_find_string(w, name), n = 0;
    if (sid < 0) return 0;
    const hkfsm_scene_def *sc = w->sc;
    if (fsm >= 0) {
        fsm_inst *f = &w->fsms[fsm];
        const fsm_def *d = f->def;
        for (int32_t i = d->var_bucket_start[bucket]; i < d->var_bucket_start[bucket + 1] && n < cap; i++)
            if (sc->vars[d->var_start + i].name == sid) out[n++] = &f->vals[i];
        return n;
    }
    for (int32_t i = sc->global_bucket_start[bucket]; i < sc->global_bucket_start[bucket + 1] && n < cap; i++)
        if (sc->globals[i].name == sid) out[n++] = &w->gvals[i];
    return n;
}
static fsm_val *var_by_name(fsm_world *w, int32_t fsm, int bucket, const char *name)
{
    fsm_val *x = NULL;
    return vars_by_name(w, fsm, bucket, name, &x, 1) ? x : NULL;
}

/* Set a variable (fsm -1 = PlayMakerGlobals), every declaration of the name: the game's record has one value per
 * name.  float/vector buckets read v[0..3], int/bool/enum read i, string reads s (interned), GameObject reads i
 * (a GameObject id, -1 null).  Returns 0, or -1 when the variable does not exist in the sim. */
HKSIM_API int hkmo_var_set(fsm_world *w, int32_t fsm, int32_t bucket, const char *name, const float *v, int32_t i, const char *s)
{
    fsm_val *xs[8];
    int32_t n = vars_by_name(w, fsm, bucket, name, xs, 8);
    if (!n) return -1;
    for (int32_t k = 0; k < n; k++) {
        fsm_val *x = xs[k];
        switch (bucket) {
        case VB_FLOAT: x->f = v[0]; break;
        case VB_INT: case VB_BOOL: case VB_ENUM: case VB_GO: x->i = i; break;
        case VB_OBJ: x->i = i; break;   /* the only object the sim stores: an AlertRange as its GameObject id (actions/hk.c) */
        case VB_STRING: x->i = s ? w_intern(w, s) : -1; break;
        case VB_V2: case VB_V3: case VB_RECT: case VB_QUAT: case VB_COLOR: memcpy(x->v, v, sizeof(float) * 4); break;
        default: return -2;
        }
    }
    return 0;
}
/* AlertRange.isHeroInRange (HK/AlertRange.cs:6-27), the latch its trigger callbacks keep */
HKSIM_API void hkmo_alert_set(fsm_world *w, int32_t go, int32_t v) { w->gos[go].alert_in_range = v ? 1 : 0; }
HKSIM_API int hkmo_var_get(fsm_world *w, int32_t fsm, int32_t bucket, const char *name, float *v, int32_t *i, const char **s)
{
    fsm_val *x = var_by_name(w, fsm, bucket, name);
    if (!x) return -1;
    v[0] = x->f; v[1] = v[2] = v[3] = 0.0f;
    *i = x->i; *s = NULL;
    switch (bucket) {
    case VB_STRING: *s = x->i >= 0 ? w_str(w, x->i) : NULL; break;
    case VB_V2: case VB_V3: case VB_RECT: case VB_QUAT: case VB_COLOR: memcpy(v, x->v, sizeof(float) * 4); break;
    case VB_GO: if (x->i >= 0 && w->gos[x->i].destroyed) *i = -1; break;
    default: break;
    }
    return 0;
}

/* ---- GameObjects ---- */
/* out: [0..2] world pos, [3..5] local pos, [6..8] local scale, [9] local euler z, [10] world euler z,
 * [11..12] velocity, [13] has_rb, [14] active_self, [15] active_in_hierarchy, [16] has_transform,
 * [17] gravity scale, [18] kinematic */
HKSIM_API int hkmo_go_get(fsm_world *w, int32_t go, float out[19])
{
    ARM(w);
    go_inst *g = &w->gos[go];
    memset(out, 0, sizeof(float) * 19);
    out[14] = g->active_self; out[15] = g->active_in_hierarchy; out[16] = g->has_transform;
    if (g->has_transform) {
        go_world_pos(w, go, out);
        go_local_pos(w, go, out + 3);
        go_local_scale(w, go, out + 6);
        out[9] = go_local_euler_z(w, go);
        out[10] = go_euler_z(w, go);
    }
    if (go_has_rb(w, go)) {
        go_velocity(w, go, out + 11); out[13] = 1; out[18] = g->kinematic;
        out[17] = (w->phys && g->body) ? phys_body_gravity_scale((const phys_world *)w->phys, g->body) : g->gravity_scale;   /* the hero module writes the Knight's body directly */
    }
    DISARM();
    return 0;
}

/* Put a GameObject into a recorded pose: local scale and local euler z first (when lscale is given), then the
 * world position (Transform.position semantics through the parent chain), then the Rigidbody2D velocity.  `active` 0/1 pokes
 * activeSelf/activeInHierarchy without running OnEnable/OnDisable (-1 leaves them). */
HKSIM_API int hkmo_go_set(fsm_world *w, int32_t go, const float *pos, const float *lscale, float lz, const float *vel, int32_t active_self, int32_t active_h)
{
    ARM(w);
    go_inst *g = &w->gos[go];
    if (active_self >= 0) g->active_self = (uint8_t)active_self;
    if (active_h >= 0) g->active_in_hierarchy = (uint8_t)active_h;
    if (g->has_transform && lscale) {
        go_set_local_scale(w, go, lscale);
        go_set_local_euler_z(w, go, lz);
    }
    if (g->has_transform && pos) go_set_world_pos(w, go, pos);
    if (vel && go_has_rb(w, go)) go_set_velocity(w, go, vel);
    DISARM();
    return 0;
}

/* Rigidbody2D.gravityScale: an input of AffectedByGravity (HC:3241-3254) and the gravity actions. */
HKSIM_API int hkmo_go_set_gravity(fsm_world *w, int32_t go, float gravity_scale)
{
    ARM(w);
    if (go_has_rb(w, go)) go_set_gravity_scale(w, go, gravity_scale);
    DISARM();
    return 0;
}

/* ---- animator / HealthManager / Recoil state ---- */
static int32_t clip_named(fsm_world *w, anim_inst *a, const char *name)
{
    if (!name || a->lib < 0) return -1;
    const anim_lib_def *L = &w->sc->libs[a->lib];
    for (int32_t i = 0; i < L->n_clips; i++) if (strcmp(w_str(w, w->sc->clips[L->clip_start + i].name), name) == 0) return L->clip_start + i;
    return -2;
}
/* Returns -1 when the GameObject has no animator, -2 when the clip is not in its library. */
HKSIM_API int hkmo_anim_set(fsm_world *w, int32_t go, const char *clip, float clip_time, float clip_fps, int32_t prev_frame, int32_t state, int32_t sprite_id)
{
    anim_inst *a = anim_of_go(w, go);
    if (!a) return -1;
    int32_t c = clip ? clip_named(w, a, clip) : -1;
    if (c == -2) return -2;
    a->cur_clip = c; a->clip_time = clip_time; a->clip_fps = clip_fps; a->previous_frame = prev_frame; a->state = (uint8_t)state;
    a->sprite_id = sprite_id; a->sprite_def = -1;
    if (c >= 0) {
        const clip_def *cd = &w->sc->clips[c];
        for (int32_t k = 0; k < cd->n_frames; k++)
            if (w->sc->frames[cd->frame_start + k].sprite_id == sprite_id) { a->sprite_def = w->sc->frames[cd->frame_start + k].sprite; break; }
    }
    return 0;
}
/* out: clip_time, clip_fps, previous_frame, state, sprite_id, current frame; returns the clip name ("" none, NULL no animator) */
HKSIM_API const char *hkmo_anim_get(fsm_world *w, int32_t go, float out[6])
{
    anim_inst *a = anim_of_go(w, go);
    if (!a) return NULL;
    out[0] = a->clip_time; out[1] = a->clip_fps; out[2] = (float)a->previous_frame; out[3] = a->state; out[4] = (float)a->sprite_id;
    out[5] = a->cur_clip >= 0 ? (float)anim_current_frame(w, a) : -1.0f;
    return anim_clip_name(w, a);
}
/* v: hp, isDead, invincible, invincibleFromDirection, evasionByHitRemaining, directionOfLastAttack */
HKSIM_API int hkmo_hm_set(fsm_world *w, int32_t go, const float v[6])
{
    hm_inst *h = hm_of_go(w, go);
    if (!h) return -1;
    h->hp = (int32_t)v[0]; h->is_dead = v[1] != 0.0f; h->invincible = v[2] != 0.0f; h->invincible_from_direction = (int32_t)v[3];
    h->evasion_by_hit_remaining = v[4]; h->direction_of_last_attack = (int32_t)v[5];
    return 0;
}
HKSIM_API int hkmo_hm_get(fsm_world *w, int32_t go, float v[6])
{
    hm_inst *h = hm_of_go(w, go);
    if (!h) return -1;
    v[0] = (float)h->hp; v[1] = h->is_dead; v[2] = h->invincible; v[3] = (float)h->invincible_from_direction;
    v[4] = h->evasion_by_hit_remaining; v[5] = (float)h->direction_of_last_attack;
    return 0;
}
static recoil_inst *recoil_of_go(fsm_world *w, int32_t go) { return (go >= 0 && w->gos[go].recoil >= 0) ? &w->recoils[w->gos[go].recoil] : NULL; }
/* v: state, recoilTimeRemaining, recoilSpeed, isRecoilSweeping, recoilSpeedBase */
HKSIM_API int hkmo_recoil_set(fsm_world *w, int32_t go, const float v[5])
{
    recoil_inst *r = recoil_of_go(w, go);
    if (!r) return -1;
    r->state = (uint8_t)v[0]; r->time_remaining = v[1]; r->speed = v[2]; r->is_sweeping = v[3] != 0.0f; r->speed_base = v[4];
    return 0;
}
HKSIM_API int hkmo_recoil_get(fsm_world *w, int32_t go, float v[5])
{
    recoil_inst *r = recoil_of_go(w, go);
    if (!r) return -1;
    v[0] = r->state; v[1] = r->time_remaining; v[2] = r->speed; v[3] = r->is_sweeping; v[4] = r->speed_base;
    return 0;
}

/* ---- clocks, RNG, log ---- */
HKSIM_API void hkmo_set_clock(fsm_world *w, float dt, float fixed_dt, float time, uint32_t frame, int32_t phase)
{
    w->dt = dt; w->fixed_dt = fixed_dt; w->time = time; w->frame = frame; w->phase = (uint8_t)phase;
}
HKSIM_API void hkmo_set_rng(fsm_world *w, const uint32_t s[4]) { w->rng->x = s[0]; w->rng->y = s[1]; w->rng->z = s[2]; w->rng->w = s[3]; }
HKSIM_API void hkmo_get_rng(fsm_world *w, uint32_t s[4]) { s[0] = w->rng->x; s[1] = w->rng->y; s[2] = w->rng->z; s[3] = w->rng->w; }
/* Enable the event log and return its length: the mark a call's events are read from. */
HKSIM_API int32_t hkmo_log_mark(fsm_world *w) { w->log_enabled = true; return w->n_log; }
/* Log entry i: 1 when it is an FSM_EVENT, with the receiving FSM, the event name and the FsmExecutionStack depth it
 * was sent at (fsm_rt.c fsm_event). */
HKSIM_API int hkmo_log_event(fsm_world *w, int32_t i, int32_t *fsm, const char **name, int32_t *depth)
{
    if (i < 0 || i >= w->n_log || w->log[i].kind != LOG_FSM_EVENT) return 0;
    *fsm = w->log[i].fsm; *name = w_str(w, w->log[i].a); *depth = w->log[i].b;
    return 1;
}
HKSIM_API int32_t hkmo_log_count(fsm_world *w) { return w->n_log; }
HKSIM_API int32_t hkmo_delayed_count(fsm_world *w, int32_t fsm) { return w->fsms[fsm].n_delayed; }
HKSIM_API const char *hkmo_delayed_get(fsm_world *w, int32_t fsm, int32_t i, float *delay)
{
    delayed_ev *d = &w->fsms[fsm].delayed[i];
    *delay = d->delay;
    return d->event >= 0 ? w_str(w, d->event) : "";
}

/* ---- the call ---- */
HKSIM_API const char *hkmo_switch_to(fsm_world *w, int32_t fsm) { fsm_inst *f = &w->fsms[fsm]; return f->switch_to >= 0 ? state_name(f, f->switch_to) : ""; }
HKSIM_API const char *hkmo_active_state(fsm_world *w, int32_t fsm) { fsm_inst *f = &w->fsms[fsm]; return f->active_state >= 0 ? state_name(f, f->active_state) : ""; }

/* Put the FSM in `state` as the game had it when the call ran, without running any action: FsmState.OnEnter's
 * bookkeeping (PM/FsmState.cs:267-284) is replaced by direct writes, because the other actions' entries are
 * separate records.  switch_to: the pending transition target (Fsm.switchToState), -1 none; previous: the
 * previous active state (Fsm.PreviousActiveState, what GotoPreviousState returns to), -1 none. */
HKSIM_API int hkmo_prepare(fsm_world *w, int32_t fsm, int32_t state, float state_time, int32_t switch_to, int32_t previous)
{
    fsm_inst *f = &w->fsms[fsm];
    f->previous_state = previous;
    if (!f->started) f->started = 1;
    f->finished = 0;
    f->active_state = state;
    f->active_state_entered = 1;
    f->switch_to = switch_to;
    state_inst *st = &f->states[state];
    st->active = 1;
    st->finished = 0;
    st->state_time = state_time;
    return 0;
}

enum { CB_ENTER = 0, CB_UPDATE, CB_FIXED, CB_LATE, CB_EXIT, CB_EVENT };

/* Run one action callback with the FSM on the execution stack, as FsmState's dispatch does (PM/FsmState.cs
 * :294-302 for OnEnter: Active, Finished=false, Entered; the others run on an active action).  finished0 is
 * the action's Finished before the call (from the record).  Returns 0, -1 when the action type has no vtable
 * or no such callback (nothing ran), or the trap code; out[0] = Finished after, out[1] = Event's return, out[2] = the
 * FsmExecutionStack depth the callback ran at (its own events are logged at it). */
HKSIM_API int hkmo_call(fsm_world *w, int32_t fsm, int32_t state, int32_t idx, int32_t cb, const char *event, int32_t finished0, int32_t out[3])
{
    fsm_inst *f = &w->fsms[fsm];
    state_inst *st = &f->states[state];
    act_inst *a = &st->acts[idx];
    out[0] = out[1] = -1;
    out[2] = w->stack_depth + 1;
    if (!a->vt) { snprintf(g_err, sizeof g_err, "unported action type %s", w_str(w, a->def->type_short)); return -1; }
    ARM(w);
    push_fsm(w, f->id);
    if (cb == CB_ENTER) {
        st->active_action_index = idx;
        st->active_action = idx;
        a->active = 1; a->finished = 0; a->entered = 1;
        if (a->vt->on_enter) a->vt->on_enter(a);
    } else {
        a->finished = (uint8_t)(finished0 != 0);
        a->active = (uint8_t)!a->finished;
        switch (cb) {
        case CB_UPDATE: if (a->vt->on_update) a->vt->on_update(a); break;
        case CB_FIXED: if (a->vt->on_fixed_update) a->vt->on_fixed_update(a); break;
        case CB_LATE: if (a->vt->on_late_update) a->vt->on_late_update(a); break;
        case CB_EXIT: st->active_action = idx; if (a->vt->on_exit) a->vt->on_exit(a); break;
        case CB_EVENT: out[1] = a->vt->on_event ? a->vt->on_event(a, w_get_fsm_event(w, event ? event : "")) : 0; break;
        default: break;
        }
    }
    pop_fsm(w);
    DISARM();
    out[0] = a->finished;
    return 0;
}

/* ---- component calls: 0, -1 when the object has no such component (nothing ran), or the trap code ---- */
/* HealthManager.Hit(HitInstance) -- HK/HealthManager.cs:332-347 through hm_hit. */
HKSIM_API int hkmo_hm_hit(fsm_world *w, int32_t go, int32_t source_go, int32_t attack_type, int32_t damage, float direction,
                          int32_t circle, int32_t ignore_inv, float magnitude_mult, float mult)
{
    hm_inst *h = hm_of_go(w, go);
    if (!h) { snprintf(g_err, sizeof g_err, "no HealthManager on %s", go_path(w, go)); return -1; }
    ARM(w);
    hm_hit(w, h, source_go, attack_type, damage, direction, circle != 0, ignore_inv != 0, magnitude_mult, mult);
    DISARM();
    return 0;
}
/* Recoil.RecoilByDirection -- HK/Recoil.cs:112-152; Recoil.FixedUpdate -- :191-194. */
HKSIM_API int hkmo_recoil_by_direction(fsm_world *w, int32_t go, int32_t dir, float mag)
{
    recoil_inst *r = recoil_of_go(w, go);
    if (!r) { snprintf(g_err, sizeof g_err, "no Recoil on %s", go_path(w, go)); return -1; }
    ARM(w);
    recoil_by_direction(w, r, dir, mag);
    DISARM();
    return 0;
}
HKSIM_API int hkmo_recoil_fixed_update(fsm_world *w, int32_t go)
{
    recoil_inst *r = recoil_of_go(w, go);
    if (!r) { snprintf(g_err, sizeof g_err, "no Recoil on %s", go_path(w, go)); return -1; }
    ARM(w);
    recoil_fixed_update(w, r);
    DISARM();
    return 0;
}
/* tk2dSpriteAnimator.UpdateAnimation(dt) -- HK/tk2dSpriteAnimator.cs:433-526; Play(clip, t, fps) -- :291-340. */
HKSIM_API int hkmo_anim_update(fsm_world *w, int32_t go, float dt)
{
    anim_inst *a = anim_of_go(w, go);
    if (!a) { snprintf(g_err, sizeof g_err, "no tk2dSpriteAnimator on %s", go_path(w, go)); return -1; }
    ARM(w);
    anim_update(w, a, dt);
    DISARM();
    return 0;
}
HKSIM_API int hkmo_anim_play(fsm_world *w, int32_t go, const char *clip, float t, float fps)
{
    anim_inst *a = anim_of_go(w, go);
    if (!a) { snprintf(g_err, sizeof g_err, "no tk2dSpriteAnimator on %s", go_path(w, go)); return -1; }
    int32_t c = clip_named(w, a, clip);
    if (c < 0) { snprintf(g_err, sizeof g_err, "clip '%s' not in the library of %s", clip ? clip : "(null)", go_path(w, go)); return -1; }
    ARM(w);
    anim_play(w, a, c, t, fps);
    DISARM();
    return 0;
}

/* Every variable of an FSM (fsm -1: PlayMakerGlobals) as text lines "<bucket>\t<name>\t<value>\n", float values
 * as the hex of their bits so the caller compares them exactly: float "%08x"; int/bool/enum/GameObject id "%d";
 * string the text (-1 id: "\x01"); vector/rect/quaternion/color four "%08x" separated by ','.  Array, Object,
 * Material and Texture variables are skipped.  Returns the bytes needed (write again with a bigger buffer when
 * it exceeds cap). */
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
HKSIM_API int32_t hkmo_vars_dump(fsm_world *w, int32_t fsm, char *buf, int32_t cap)
{
    int32_t off = 0;
    const hkfsm_scene_def *sc = w->sc;
    for (int b = 0; b < VB_COUNT; b++) {
        if (b == VB_ARRAY || b == VB_OBJ || b == VB_MAT || b == VB_TEX) continue;
        int32_t lo, hi;
        const fsm_vardef *defs; fsm_val *vals;
        if (fsm >= 0) {
            fsm_inst *f = &w->fsms[fsm];
            lo = f->def->var_bucket_start[b]; hi = f->def->var_bucket_start[b + 1];
            defs = &sc->vars[f->def->var_start]; vals = f->vals;
        } else {
            lo = sc->global_bucket_start[b]; hi = sc->global_bucket_start[b + 1];
            defs = sc->globals; vals = w->gvals;
        }
        for (int32_t i = lo; i < hi; i++) {
            char line[1024];
            const fsm_val *x = &vals[i];
            const char *name = w_str(w, defs[i].name);
            int n;
            switch (b) {
            case VB_FLOAT: n = snprintf(line, sizeof line, "%d\t%s\t%08x\n", b, name, (unsigned)fbits(x->f)); break;
            case VB_STRING: n = snprintf(line, sizeof line, "%d\t%s\t%s\n", b, name, x->i >= 0 ? w_str(w, x->i) : "\x01"); break;
            case VB_V2: case VB_V3: case VB_RECT: case VB_QUAT: case VB_COLOR:
                n = snprintf(line, sizeof line, "%d\t%s\t%08x,%08x,%08x,%08x\n", b, name, (unsigned)fbits(x->v[0]), (unsigned)fbits(x->v[1]),
                             (unsigned)fbits(x->v[2]), (unsigned)fbits(x->v[3]));
                break;
            case VB_GO: n = snprintf(line, sizeof line, "%d\t%s\t%d\n", b, name, (x->i >= 0 && !w->gos[x->i].destroyed) ? x->i : -1); break;
            default: n = snprintf(line, sizeof line, "%d\t%s\t%d\n", b, name, x->i); break;
            }
            if (n < 0) continue;
            if (n >= (int)sizeof line) n = (int)sizeof line - 1;
            if (off + n < cap) memcpy(buf + off, line, (size_t)n);
            off += n;
        }
    }
    if (off < cap) buf[off] = 0; else if (cap > 0) buf[cap - 1] = 0;
    return off + 1;
}

/* Put a GameObject into a recorded LOCAL pose (Transform.localScale, localEulerAngles.z, localPosition), the way
 * Unity stores a transform: the world pose then composes through the parent chain, which the caller sets first. */
HKSIM_API int hkmo_go_set_local(fsm_world *w, int32_t go, const float *lpos, const float *lscale, float lz)
{
    ARM(w);
    if (w->gos[go].has_transform) {
        go_set_local_scale(w, go, lscale);
        go_set_local_euler_z(w, go, lz);
        go_set_local_pos(w, go, lpos);
    }
    DISARM();
    return 0;
}

/* HeroActions state (sim/hero/hero.h PA_* order): bit 0 IsPressed, 1 WasPressed, 2 WasReleased, written as the
 * InControl two-state pair (OneAxisInputControl thisState / lastState) the sim reads, plus the direction values and
 * moveVector the held direction keys give (HeroController reads moveVector.X/Y, e.g. SetStartingMotionState HC:4264). */
HKSIM_API int hkmo_input_set(fsm_world *w, const int32_t *bits, int32_t n)
{
    struct hero *h = (struct hero *)w->hero;
    if (!h) return -1;
    for (int32_t i = 0; i < n && i < PA_N; i++) {
        int is = bits[i] & 1, was_p = (bits[i] >> 1) & 1, was_r = (bits[i] >> 2) & 1;
        h->in.pa[i].thisState.State = is;
        h->in.pa[i].lastState.State = is ? !was_p : was_r;
    }
    if (n >= PA_N)
        hero_input_set_direction_values(h, bits[PA_LEFT] & 1, bits[PA_RIGHT] & 1, bits[PA_UP] & 1, bits[PA_DOWN] & 1);
    return 0;
}

/* PlayerData by name: kind 0 float, 1 int, 2 bool.  Returns -1 when the sim has no such field. */
HKSIM_API int hkmo_pd_set(fsm_world *w, const char *name, int32_t kind, float f, int32_t i)
{
    ARM(w);
    if (kind == 0) world_pd_set_float(w, name, f);
    else if (kind == 1) world_pd_set_int(w, name, i);
    else world_pd_set_bool(w, name, i != 0);
    DISARM();
    return 0;
}
HKSIM_API int hkmo_pd_get(fsm_world *w, const char *name, int32_t kind, float *f, int32_t *i)
{
    ARM(w);
    *f = 0.0f; *i = 0;
    if (kind == 0) *f = world_pd_float(w, name);
    else if (kind == 1) *i = world_pd_int(w, name);
    else *i = world_pd_bool(w, name);
    DISARM();
    return 0;
}

/* The game reparents objects at runtime (ObjectPool.Spawn, SetParent actions): the replay gives a recorded object
 * the parent it had in the game before setting its local pose.  parent -1 = a root. */
HKSIM_API int32_t hkmo_go_parent(fsm_world *w, int32_t go) { return w->gos[go].parent; }
HKSIM_API int hkmo_go_set_parent(fsm_world *w, int32_t go, int32_t parent)
{
    ARM(w);
    if (w->gos[go].has_transform) go_set_parent(w, go, parent);
    DISARM();
    return 0;
}

/* LimitSendEvents.sentList (HK/LimitSendEvents.cs:7) of a GameObject that has the component: ids[0..n). */
HKSIM_API int hkmo_lse_set(fsm_world *w, int32_t go, const int32_t *ids, int32_t n)
{
    go_lse *l = w->gos[go].lse;
    if (!l) return -1;
    if (l->cap < n) { l->cap = n; l->sent = realloc(l->sent, sizeof(int32_t) * (size_t)n); }
    for (int32_t i = 0; i < n; i++) l->sent[i] = ids[i];
    l->n = n;
    return 0;
}

/* GameObject.SetActive through the lifecycle (OnEnable / OnDisable of its components), for the recorded call's owner
 * when the reset world has it inactive: a component the lifecycle never enabled would not get the OnDisable that,
 * e.g., RecycleSelf's SetActive(false) raises in the game. */
HKSIM_API int hkmo_go_activate(fsm_world *w, int32_t go, int32_t active)
{
    ARM(w);
    go_set_active(w, go, active != 0);
    DISARM();
    return 0;
}

/* Fsm.EventData (PM/Fsm.cs:31) as the sim tracks it (fsm.h fsm_world ev_*): sender FSM id (-1 none), int, float,
 * string (NULL = none). */
HKSIM_API void hkmo_set_event_data(fsm_world *w, int32_t sender, int32_t i, float f, const char *s)
{
    w->ev_sent_by_fsm = sender; w->ev_int = i; w->ev_float = f; w->ev_string = s ? w_intern(w, s) : -1;
}

/* An action's private state (act_inst.st, vt->state_size bytes): the game keeps an action object's private fields
 * from one activation to the next, so the replay carries the sim's copy across the activations it replays of one
 * action.  get returns the size (copying at most cap bytes); set copies n bytes back. */
HKSIM_API int32_t hkmo_act_state_get(fsm_world *w, int32_t fsm, int32_t state, int32_t idx, void *buf, int32_t cap)
{
    act_inst *a = &w->fsms[fsm].states[state].acts[idx];
    int32_t n = a->vt ? (int32_t)a->vt->state_size : 0;
    if (buf && n > 0) memcpy(buf, a->st, (size_t)(n < cap ? n : cap));
    return n;
}
HKSIM_API void hkmo_act_state_set(fsm_world *w, int32_t fsm, int32_t state, int32_t idx, const void *buf, int32_t n)
{
    act_inst *a = &w->fsms[fsm].states[state].acts[idx];
    if (a->vt && n == (int32_t)a->vt->state_size && n > 0) memcpy(a->st, buf, (size_t)n);
}

/* world_reserve between two replayed callbacks: the engine refills the instance arrays' spare slots between stages,
 * and a replayed activation runs its callbacks back to back. */
HKSIM_API void hkmo_reserve(fsm_world *w) { world_reserve(w); }
