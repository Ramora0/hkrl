/* The ctypes surface tests/test_fsm.py drives on the synthetic scene.  Every
 * call that can run FSM code arms the trap context: a trap inside returns HKSIM_ERR_*
 * and hkfsm_last_error() carries the message (core/trap.h). */
#include "fsm/fsm.h"
#include "fsm/lifecycle.h"
#include "world_internal.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "core/alloc.h"


static char g_err[600];
static hksim_trap_ctx g_trap;

#define ARM(w) if (setjmp(g_trap.jb) != 0) { snprintf(g_err, sizeof g_err, "%s", hksim_trap_message()); hksim_trap_disarm(); if (w) (w)->stack_depth = 0; return g_trap.code; } hksim_trap_arm(&g_trap); g_trap.armed = 1
#define DISARM() hksim_trap_disarm(); g_trap.armed = 0

HKSIM_API const char *hkfsm_last_error(void) { return g_err; }

HKSIM_API fsm_world *hkfsm_world_create(const char *scene)
{
    const hkfsm_scene_def *sc = hkfsm_scene_lookup(scene);
    if (!sc) { snprintf(g_err, sizeof g_err, "no compiled tables for scene '%s'", scene); return NULL; }
    fsm_world *w = NULL;
    if (setjmp(g_trap.jb) != 0) { snprintf(g_err, sizeof g_err, "%s", hksim_trap_message()); hksim_trap_disarm(); return NULL; }
    hksim_trap_arm(&g_trap); g_trap.armed = 1;
    w = world_create(sc);
    w->rng = &w->rng_own;
    DISARM();
    return w;
}
HKSIM_API void hkfsm_world_destroy(fsm_world *w) { world_destroy(w); }

HKSIM_API int hkfsm_cold_start(fsm_world *w) { ARM(w); world_restore_scene(w); DISARM(); return 0; }
/* One live frame up to the env coroutine: startup, Update and the whole update_delayed pass. */
HKSIM_API int hkfsm_update(fsm_world *w, float dt)
{
    ARM(w);
    lc_frame_end(w);
    lc_frame_begin(w, w->frame + 1, true);
    w->time += dt;                               /* Time.time */
    lc_stage(w, LCS_STARTUP, dt, 0);
    lc_stage(w, LCS_UPDATE, dt, 0);
    lc_stage(w, LCS_UPDATE_DELAYED, dt, 0);
    lc_delayed_end(w);
    DISARM();
    return 0;
}

/* HitTaker.Hit on `go` from `source` (a TakeDamage with these HitInstance fields, multiplier 1) */
HKSIM_API int hkfsm_hit(fsm_world *w, int32_t go, int32_t source, int32_t attack_type, int32_t damage, float direction,
                        float magnitude)
{
    ARM(w);
    world_hit_taker(w, go, source, attack_type, damage, direction, false, false, magnitude, 1.0f);
    DISARM();
    return 0;
}
/* ObjectPool.Spawn of the pool whose clones are named `clone_name`, at `pos`: the spawned clone, or -1 */
HKSIM_API int32_t hkfsm_pool_spawn(fsm_world *w, const char *clone_name, const float pos[3])
{
    int32_t prefab = world_pool_prefab(w, clone_name), go = -1;
    if (prefab < 0) return -1;
    ARM(w);
    go = world_pool_spawn(w, prefab, pos, 0.0f);
    DISARM();
    return go;
}
/* Collider2D k of `go`: enabled flag, and whether its GameObject is active in the hierarchy */
HKSIM_API int hkfsm_col_state(fsm_world *w, int32_t go, int32_t k, int32_t out[2])
{
    out[0] = w->gos[go].cols[k].enabled; out[1] = go_active_in_hierarchy(w, go);
    return 0;
}

/* lookups */
HKSIM_API int32_t hkfsm_go_find(fsm_world *w, const char *path) { return world_go_find_path(w, path); }
HKSIM_API int32_t hkfsm_fsm_find(fsm_world *w, const char *go_path, const char *fsm_name)
{
    int32_t go = world_go_find_path(w, go_path);
    if (go < 0) return -1;
    for (int32_t i = 0; i < w->n_fsms; i++) if (w->fsms[i].go == go && strcmp(w_str(w, w->fsms[i].def->fsm_name), fsm_name) == 0) return i;
    return -1;
}
HKSIM_API const char *hkfsm_fsm_state(fsm_world *w, int32_t id)
{
    if (id < 0) return "";
    fsm_inst *f = &w->fsms[id];
    return f->active_state >= 0 ? state_name(f, f->active_state) : "";
}
HKSIM_API int hkfsm_fsm_flags(fsm_world *w, int32_t id, int32_t out[8])
{
    fsm_inst *f = &w->fsms[id];
    out[0] = f->started; out[1] = f->finished; out[2] = f->active_state_entered; out[3] = f->in_fsm_list;
    out[4] = f->component_enabled; out[5] = f->active_state; out[6] = f->previous_state; out[7] = f->switch_to;
    return 0;
}
HKSIM_API int32_t hkfsm_fsm_var_count(fsm_world *w, int32_t id) { return w->fsms[id].n_vals; }
HKSIM_API int hkfsm_fsm_var_get(fsm_world *w, int32_t id, int32_t i, const char **name, int32_t *bucket, float *value, int32_t *ivalue)
{
    fsm_inst *f = &w->fsms[id];
    if (i < 0 || i >= f->n_vals) return -1;
    const fsm_def *d = f->def;
    int b = 0;
    while (b < VB_COUNT && !(i >= d->var_bucket_start[b] && i < d->var_bucket_start[b + 1])) b++;
    *name = w_str(w, w->sc->vars[d->var_start + i].name);
    *bucket = b;
    *value = b == VB_FLOAT ? f->vals[i].f : (float)f->vals[i].i;
    *ivalue = f->vals[i].i;
    return 0;
}

/* GameObjects */
HKSIM_API int hkfsm_go_set_active(fsm_world *w, int32_t go, int32_t active) { ARM(w); go_set_active(w, go, active != 0); DISARM(); return 0; }

/* events (external senders) */
HKSIM_API int hkfsm_send_event(fsm_world *w, int32_t fsm, const char *ev)             /* PlayMakerFSM.SendEvent */
{ ARM(w); fsm_event_name(&w->fsms[fsm], ev); DISARM(); return 0; }

/* log */
HKSIM_API int32_t hkfsm_log_count(fsm_world *w) { return w->n_log; }
HKSIM_API void hkfsm_log_clear(fsm_world *w) { w->n_log = 0; }
/* kind 5 = FSM_EVENT (a = event string id), 4 = FSM_TRANSITION (a = from state, b = to state) */
HKSIM_API int hkfsm_log_get(fsm_world *w, int32_t i, int32_t *kind, int32_t *fsm, const char **s1, const char **s2, uint32_t *frame)
{
    if (i < 0 || i >= w->n_log) return -1;
    log_rec *r = &w->log[i];
    *kind = r->kind; *fsm = r->fsm; *frame = r->frame;
    if (r->kind == LOG_FSM_EVENT) { *s1 = w_str(w, r->a); *s2 = ""; }
    else { fsm_inst *f = &w->fsms[r->fsm]; *s1 = state_name(f, r->a); *s2 = state_name(f, r->b); }
    return 0;
}


/* object identity (tests/test_prefabs.py) */
HKSIM_API int32_t hkfsm_go_count(fsm_world *w) { return w->n_gos; }
HKSIM_API const char *hkfsm_go_path(fsm_world *w, int32_t go) { return go_path(w, go); }
/* out: parent, asset, prefab, has Rigidbody2D, colliders, pool_of, animator (-1 none), FSMs */
HKSIM_API int hkfsm_go_info(fsm_world *w, int32_t go, int32_t out[8])
{
    const go_def *d = w->gos[go].def;
    out[0] = w->gos[go].parent; out[1] = d->asset; out[2] = d->prefab; out[3] = d->rb >= 0; out[4] = w->gos[go].n_cols;
    out[5] = w->gos[go].pool_of; out[6] = w->gos[go].anim; out[7] = d->n_fsms;
    return 0;
}
HKSIM_API int32_t hkfsm_col_rb_go(fsm_world *w, int32_t go, int32_t k) { return w->gos[go].cols[k].def->rb_go; }

/* Test-only fixtures for physics-touching FSM actions on the synthetic scene, which carries no scene.json
 * collider/rigidbody rows (tests/test_fsm.py, SYNTH_fsm).  Both flags are per-instance runtime fields
 * (fsm.h go_inst), set here directly rather than through a body scene.json would normally bind: go_velocity /
 * go_set_velocity already fall back to go_inst.vel when no phys body exists (physics.c), and a root object
 * (no parent) needs only local_pos/local_scale/local_euler_z for ensure_transform_clean to derive world_pos. */
HKSIM_API int hkfsm_test_set_transform(fsm_world *w, int32_t go, float x, float y)
{
    go_inst *g = &w->gos[go];
    g->has_transform = 1;
    g->local_pos[0] = x; g->local_pos[1] = y; g->local_pos[2] = 0.0f;
    g->local_scale[0] = 1.0f; g->local_scale[1] = 1.0f; g->local_scale[2] = 1.0f;
    g->local_euler_z = 0.0f;
    g->transform_dirty = 1;
    return 0;
}
HKSIM_API int hkfsm_test_set_rb(fsm_world *w, int32_t go, float vx, float vy)
{
    go_inst *g = &w->gos[go];
    g->has_rb = 1;
    g->vel[0] = vx; g->vel[1] = vy;
    return 0;
}
