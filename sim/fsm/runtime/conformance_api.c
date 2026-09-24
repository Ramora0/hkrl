/* Test entry points for the engine conformance suite (tests/test_conformance.py, hkpy/conformance.py): the
 * scenarios the game runs in probe mode (oracle/Probe) run here through the simulator's own lifecycle
 * (lifecycle.c) and physics (sim/phys), with synthetic probe components (LCT_PROBE) standing in for the game's
 * MonoBehaviours.  Every probe callback goes to one caller-supplied function, which logs it and runs the
 * scenario's ops through these same entry points.
 *
 * Objects are runtime clones of a plain GameObject of the SYNTH_fsm scene.  Their Rigidbody2D / Collider2Ds
 * live in this module's own phys world, joined and left where the lifecycle calls world_go_phys_enable /
 * world_go_phys_disable (lc_probe_hook's phys callback).  One frame is sim/core/sim.c's stage sequence
 * (lc_live_pre / lc_live_late, lc_frozen_pre / lc_frozen_late), with the physics step and its callback
 * dispatch in the physics stage (sim.c dispatch_phys_events). */
#include "fsm/fsm.h"
#include "fsm/lifecycle.h"
#include "world_internal.h"
#include "core/phys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CONF_CB_ENV = 20 };   /* the env coroutine's point in update_delayed (the recorder's ENVFRAME code) */
enum { CONF_CB_PHYS0 = 10 }; /* + phys_event_kind order below: the recorder's OnTrigger / OnCollision codes */
#define CONF_DT 0.02f        /* regime R2 (docs/frame-order.md) */

typedef struct {
    uint32_t body;                 /* 0: none (its colliders sit on static bodies of their own) */
    uint32_t *shapes, *statics; uint8_t *on; int32_t n_shapes;
    int32_t *probes; int32_t n_probes;
} conf_go;

typedef struct hkconf {
    fsm_world *w;
    phys_world *pw;
    int32_t tmpl;                  /* the plain GameObject every object is cloned from, and their container */
    lc_probe_fn fn; void *ctx;
    conf_go *g; int32_t n_g;
    uint32_t frame;
    int in_frame;
    int stage;                     /* the stage of the callback running now: where a disable's Exits are delivered */
} hkconf;

static hksim_trap_ctx g_trap;
static char g_err[600];
static int g_depth;
/* Only the outermost call arms the trap: calls made from inside the callback nest in an armed one. */
#define ENTER(ret) do { if (g_depth++ == 0) { if (setjmp(g_trap.jb) != 0) { snprintf(g_err, sizeof g_err, "%s", \
        hksim_trap_message()); hksim_trap_disarm(); g_depth = 0; return ret; } hksim_trap_arm(&g_trap); g_trap.armed = 1; } } while (0)
#define LEAVE() do { if (--g_depth == 0) { hksim_trap_disarm(); g_trap.armed = 0; } } while (0)

HKSIM_API const char *hkconf_last_error(void) { return g_err; }

static conf_go *cg(hkconf *c, int32_t go)
{
    if (go >= c->n_g) {
        int32_t n = go + 64;
        c->g = realloc(c->g, sizeof(conf_go) * (size_t)n);
        HKSIM_ASSERT(c->g != NULL, "conformance: out of memory");
        memset(c->g + c->n_g, 0, sizeof(conf_go) * (size_t)(n - c->n_g));
        c->n_g = n;
    }
    return &c->g[go];
}

static void dispatch_list(hkconf *c, const phys_event *ev, uint32_t n, int stage);
/* lifecycle.c lc_physics_exit_on_disable: a collider disable's Exits, inside the call (docs/engine-lifecycle.md R5) */
static void deliver_exits(hkconf *c)
{
    phys_event ev[64]; uint32_t n;
    while ((n = phys_take_exit_events(c->pw, ev, 64)) > 0) dispatch_list(c, ev, n, c->stage);
}

/* lifecycle.c active_walk / deactivate_object: the object's Rigidbody2D and Collider2Ds join / leave physics;
 * disable_behaviours: Collider2D.enabled = false on each (the body stays) */
static void conf_phys(void *ctx, int32_t go, int mode)
{
    hkconf *c = ctx;
    if (go >= c->n_g) return;
    conf_go *g = &c->g[go];
    if (mode == LC_PHYS_COLLIDERS_OFF) {
        for (int32_t k = 0; k < g->n_shapes; k++) { g->on[k] = 0; phys_shape_set_enabled(c->pw, g->shapes[k], false); }
        deliver_exits(c);
        return;
    }
    bool on = mode == LC_PHYS_ON;
    if (g->body) phys_body_set_simulated(c->pw, g->body, on);
    for (int32_t k = 0; k < g->n_shapes; k++) {
        if (g->statics[k]) phys_body_set_simulated(c->pw, g->statics[k], on);
        phys_shape_set_enabled(c->pw, g->shapes[k], on && g->on[k]);
    }
    deliver_exits(c);
}

static int conf_cb(void *ctx, int32_t comp, int cb, int stage, int32_t arg, float *wait)
{
    hkconf *c = ctx;
    int outer = c->stage;
    c->stage = stage;
    int r = c->fn ? c->fn(c->ctx, comp, cb, stage, arg, wait) : LC_YIELD_DONE;
    c->stage = outer;
    return r;
}

HKSIM_API hkconf *hkconf_create(phys_v2 gravity, uint32_t velocity_iters, uint32_t position_iters)
{
    hkconf *volatile c = calloc(1, sizeof *c);   /* volatile: live across the setjmp in ENTER */
    if (!c) return NULL;
    ENTER(NULL);
    const hkfsm_scene_def *sc = hkfsm_scene_lookup("SYNTH_fsm");
    HKSIM_ASSERT(sc != NULL, "conformance: no SYNTH_fsm tables");
    c->w = world_create(sc);
    c->w->rng = &c->w->rng_own;
    c->tmpl = -1;
    for (int32_t i = 0; i < c->w->n_gos && c->tmpl < 0; i++) {
        const go_def *d = c->w->gos[i].def;
        if (d->n_fsms == 0 && d->n_cols == 0 && d->n_comps == 0 && d->rb < 0 && d->first_child < 0 && d->animator < 0 &&
            d->hm < 0 && d->damage_hero < 0 && d->recoil < 0 && d->constrain < 0 && c->w->gos[i].active_in_hierarchy)
            c->tmpl = i;
    }
    HKSIM_ASSERT(c->tmpl >= 0, "conformance: SYNTH_fsm has no plain active GameObject to clone");
    uint32_t matrix[32];
    for (int i = 0; i < 32; i++) matrix[i] = 0xFFFFFFFFu;
    c->pw = phys_create(gravity, velocity_iters, position_iters, matrix);
    lc_probe_hook(c->w, conf_cb, conf_phys, c);
    LEAVE();
    return c;
}

HKSIM_API void hkconf_destroy(hkconf *c)
{
    if (!c) return;
    for (int32_t i = 0; i < c->n_g; i++) {
        free(c->g[i].shapes); free(c->g[i].statics); free(c->g[i].on); free(c->g[i].probes);
    }
    free(c->g);
    if (c->pw) phys_destroy(c->pw);
    if (c->w) world_destroy(c->w);
    free(c);
}

/* cb(ctx, comp, cb, stage, arg, wait): comp = probe id (-1 for CONF_CB_ENV); cb = LCB_* / LCB_PROBE_CO (arg = its
 * tag) / CONF_CB_PHYS0 + kind (arg = the other GameObject) / CONF_CB_ENV (arg = the frame); returns the
 * coroutine's next yield. */
HKSIM_API void hkconf_set_hook(hkconf *c, lc_probe_fn fn, void *ctx) { c->fn = fn; c->ctx = ctx; }
HKSIM_API phys_world *hkconf_phys(hkconf *c) { return c->pw; }

/* A new GameObject: Object.Instantiate of the plain template under `parent` (-1: the container), INACTIVE. */
HKSIM_API int32_t hkconf_go_new(hkconf *c, int32_t parent)
{
    ENTER(-1);
    if (!c->in_frame) world_reserve(c->w);
    /* The observer does not see these objects: world_obs_clone_added would grow its per-collider table to the
     * scene's 0 colliders, and realloc(p, 0) returns NULL (observer.c:256-262 traps "out of memory"). */
    void *oc = c->w->obs_cache;
    c->w->obs_cache = NULL;
    int32_t go = world_instantiate(c->w, c->tmpl, parent >= 0 ? parent : c->tmpl, false);
    c->w->obs_cache = oc;
    cg(c, go);
    LEAVE();
    return go;
}
HKSIM_API int hkconf_go_set_active(hkconf *c, int32_t go, int32_t v) { ENTER(-1); go_set_active(c->w, go, v != 0); LEAVE(); return 0; }
HKSIM_API int hkconf_go_set_parent(hkconf *c, int32_t go, int32_t parent) { ENTER(-1); go_set_parent(c->w, go, parent >= 0 ? parent : c->tmpl); LEAVE(); return 0; }
HKSIM_API void hkconf_go_state(hkconf *c, int32_t go, int32_t out[3])
{
    const go_inst *g = &c->w->gos[go];
    out[0] = g->active_self; out[1] = g->active_in_hierarchy; out[2] = g->destroyed;
}
HKSIM_API int hkconf_destroy_go(hkconf *c, int32_t go, float delay) { ENTER(-1); lc_destroy_go(c->w, go, delay, false); LEAVE(); return 0; }
HKSIM_API int hkconf_destroy_comp(hkconf *c, int32_t comp, float delay) { ENTER(-1); lc_probe_destroy(c->w, comp, delay); LEAVE(); return 0; }
/* The game process's clock gap r = Time.time - Time.fixedTime (native-playerloop.md PL-2): a per-process constant
 * the probe logs do not record, so the caller supplies it.  Time starts at 0. */
HKSIM_API void hkconf_set_residual(hkconf *c, double r) { lc_set_clock(c->w, 0.0, r); }

HKSIM_API int32_t hkconf_probe_add(hkconf *c, int32_t go, uint32_t lcb_mask, int32_t iid, int32_t enabled)
{
    ENTER(-1);
    int32_t id = lc_probe_add(c->w, go, lcb_mask, iid, enabled != 0);
    conf_go *g = cg(c, go);
    g->probes = realloc(g->probes, sizeof(int32_t) * (size_t)(g->n_probes + 1));
    HKSIM_ASSERT(g->probes != NULL, "conformance: out of memory");
    g->probes[g->n_probes++] = id;
    LEAVE();
    return id;
}
HKSIM_API int hkconf_probe_set_enabled(hkconf *c, int32_t comp, int32_t v) { ENTER(-1); lc_probe_set_enabled(c->w, comp, v != 0); LEAVE(); return 0; }
HKSIM_API void hkconf_probe_state(hkconf *c, int32_t comp, int32_t out[5]) { lc_probe_state(c->w, comp, out); }
HKSIM_API int hkconf_coroutine(hkconf *c, int32_t comp, int32_t yield, float wait, int32_t tag)
{
    ENTER(-1); lc_probe_coroutine(c->w, comp, yield, wait, tag); LEAVE(); return 0;
}
HKSIM_API int hkconf_stop_coroutines(hkconf *c, int32_t comp) { ENTER(-1); lc_probe_stop_coroutines(c->w, comp); LEAVE(); return 0; }

/* The object's Rigidbody2D (user = the object); simulated while the object is active in the hierarchy. */
HKSIM_API uint32_t hkconf_body_add(hkconf *c, int32_t go, const phys_body_desc *d)
{
    ENTER(0);
    phys_body_desc b = *d;
    b.user = (uint32_t)go;
    b.simulated = c->w->gos[go].active_in_hierarchy != 0;
    conf_go *g = cg(c, go);
    g->body = phys_body_add(c->pw, &b);
    LEAVE();
    return g->body;
}
/* A Collider2D on the object: on its Rigidbody2D, or (none) on a static body at `pose` -- Unity's static
 * colliders, as the core builds its statics (sim/core/sim.c build_world).  Enabled iff `enabled` and the object is
 * active in the hierarchy; its user is the object. */
HKSIM_API uint32_t hkconf_shape_add(hkconf *c, int32_t go, const phys_shape_desc *d, const phys_body_desc *pose)
{
    ENTER(0);
    conf_go *g = cg(c, go);
    bool active = c->w->gos[go].active_in_hierarchy != 0;
    uint32_t sb = 0, body = g->body;
    if (!body) {
        phys_body_desc b = *pose;
        b.type = PHYS_BODY_STATIC; b.user = (uint32_t)go; b.simulated = active;
        body = sb = phys_body_add(c->pw, &b);
    }
    phys_shape_desc s = *d;
    s.user = (uint32_t)go;
    bool on = s.enabled;
    s.enabled = on && active;
    uint32_t id = phys_shape_add(c->pw, body, &s);
    g->shapes = realloc(g->shapes, sizeof(uint32_t) * (size_t)(g->n_shapes + 1));
    g->statics = realloc(g->statics, sizeof(uint32_t) * (size_t)(g->n_shapes + 1));
    g->on = realloc(g->on, (size_t)(g->n_shapes + 1));
    HKSIM_ASSERT(g->shapes && g->statics && g->on, "conformance: out of memory");
    g->shapes[g->n_shapes] = id; g->statics[g->n_shapes] = sb; g->on[g->n_shapes] = on ? 1 : 0;
    g->n_shapes++;
    LEAVE();
    return id;
}
HKSIM_API uint32_t hkconf_go_body(hkconf *c, int32_t go) { return go < c->n_g ? c->g[go].body : 0; }
HKSIM_API uint32_t hkconf_go_shape(hkconf *c, int32_t go, int32_t k) { return go < c->n_g && k < c->g[go].n_shapes ? c->g[go].shapes[k] : 0; }
/* Collider2D.enabled: the fixture follows while the object is active in the hierarchy */
HKSIM_API int hkconf_col_set_enabled(hkconf *c, int32_t go, int32_t k, int32_t v)
{
    ENTER(-1);
    conf_go *g = &c->g[go];
    g->on[k] = v ? 1 : 0;
    if (c->w->gos[go].active_in_hierarchy) { phys_shape_set_enabled(c->pw, g->shapes[k], v != 0); deliver_exits(c); }
    LEAVE();
    return 0;
}

/* sim.c dispatch_phys_events: every event in phys order, to each probe on the receiving collider's object
 * (disabled ones too: physics.c world_fsm_2d delivers to a disabled PlayMakerFSM). */
static void dispatch_phys(hkconf *c)
{
    const phys_event *ev;
    uint32_t n = phys_events(c->pw, &ev);
    dispatch_list(c, ev, n, LCS_PHYSICS);
}
static void dispatch_list(hkconf *c, const phys_event *ev, uint32_t n, int stage)
{
    for (uint32_t i = 0; i < n; i++) {
        int32_t go = (int32_t)phys_shape_user(c->pw, ev[i].shape_a);
        int32_t other = (int32_t)phys_shape_user(c->pw, ev[i].shape_b);
        int code;
        switch (ev[i].kind) {
        case PHYS_EV_TRIGGER_ENTER: code = CONF_CB_PHYS0 + 0; break;
        case PHYS_EV_TRIGGER_STAY: code = CONF_CB_PHYS0 + 1; break;
        case PHYS_EV_TRIGGER_EXIT: code = CONF_CB_PHYS0 + 2; break;
        case PHYS_EV_COLLISION_ENTER: code = CONF_CB_PHYS0 + 3; break;
        case PHYS_EV_COLLISION_STAY: code = CONF_CB_PHYS0 + 4; break;
        default: code = CONF_CB_PHYS0 + 5; break;
        }
        if (go >= c->n_g || c->w->gos[go].destroyed) continue;
        for (int32_t k = 0; k < c->g[go].n_probes; k++) {
            int32_t st[5]; lc_probe_state(c->w, c->g[go].probes[k], st);
            if (st[4]) continue;
            conf_cb(c, c->g[go].probes[k], code, stage, other, NULL);
        }
    }
}

/* One frame.  live: timeScale 1 and one fixed step; else timeScale 0 (R0). */
HKSIM_API int hkconf_frame(hkconf *c, int32_t live)
{
    ENTER(-1);
    fsm_world *w = c->w;
    float dt = live ? CONF_DT : 0.0f;
    int fl = live ? 0 : LCF_FROZEN;
    c->in_frame = 1;
    lc_frame_begin(w, ++c->frame, live != 0);
    lc_stage(w, LCS_STARTUP, dt, fl);
    if (live) {
        lc_stage(w, LCS_FIXED, dt, 0);
        lc_stage_enter(w, LCS_PHYSICS);
        phys_step(c->pw, CONF_DT);
        dispatch_phys(c);
        lc_stage(w, LCS_FIXED_DELAYED, dt, 0);
    }
    lc_stage(w, LCS_UPDATE, dt, fl);
    lc_stage(w, LCS_UPDATE_DELAYED, dt, fl);
    conf_cb(c, -1, CONF_CB_ENV, LCS_UPDATE_DELAYED, (int32_t)c->frame, NULL);
    lc_delayed_end(w);
    lc_stage(w, LCS_ANIM, dt, 0);            /* sim.c lc_live_late / lc_frozen_late */
    lc_stage(w, LCS_LATE, dt, 0);
    lc_stage(w, LCS_POSTLATE_DELAYED, dt, 0);
    lc_stage(w, LCS_END_OF_FRAME, dt, 0);
    lc_frame_end(w);
    c->in_frame = 0;
    LEAVE();
    return 0;
}
