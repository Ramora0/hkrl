/* hksim ABI + frame scheduler.  Ties sim/hero, sim/phys and sim/fsm together and emits the .hktrace stream.
 *
 * cite: analysis/specs/frame-order.md §3.4 — per agent step: 1 frozen frame (HeroController.Update with
 *       dt = 0, then ActionDecoder.ApplyAction in the coroutine phase, no FixedUpdate) + frames_per_wait
 *       live frames, each FixedUpdate → physics → Update → LateUpdate; capture points per docs/trace-format.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <setjmp.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#include "hksim.h"
#include "trap.h"
#include "rng.h"
#include "phys.h"
#include "tracew.h"
#include "sim_modules.h"
#include "obs_batch.h"
#include "scene_registry.inc"   /* generated: every sim/core/scene_*.h */
#include "hero/hero.h"
#include "obs/obs.h"
#include "lockstep.h"

#define R2_DT 0.02f    /* cite: docs/frame-order.md: captureDeltaTime = fixedDeltaTime = 0.02 */

/* Profiling: per-module wall time accumulated per instance (always on), read with
 * hksim_get_value("prof.<name>") in seconds; names in PROF_NAMES. */
#include <time.h>
#include "core/alloc.h"
static double now_s(void)
{
#if defined(_WIN32)
    static double inv = 0.0; LARGE_INTEGER c;
    if (inv == 0.0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); inv = 1.0 / (double)f.QuadPart; }
    QueryPerformanceCounter(&c); return (double)c.QuadPart * inv;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
#endif
}
enum { PROF_HERO_FIXED, PROF_FSM_FIXED, PROF_PHYS, PROF_EVENTS, PROF_HERO_UPDATE, PROF_FSM_UPDATE, PROF_HERO_CORO,
       PROF_LATE, PROF_TRACE, PROF_OBS, PROF_FROZEN, PROF_N };
static const char *const PROF_NAMES[PROF_N] = { "hero_fixed", "fsm_fixed", "phys", "events", "hero_update", "fsm_update",
                                                "hero_coro", "late", "trace", "obs", "frozen" };
#define PROF(s, slot, stmt) do { double _t0 = now_s(); stmt; (s)->prof[slot] += now_s() - _t0; } while (0)

static char g_create_err[512];

/* Initial capacities of the observation scratch; build_obs grows them on demand.  BinaryProtocol writes
 * both the combat row count and the FSM snapshot count as a u16 (oracle/Net/BinaryProtocol.cs:48,
 * :147-150; the 255 cap there applies only to string lengths, :75-93), so neither list is truncated
 * below 65535. */
enum { OBS_MAXC = 255, OBS_MAXF = 256, OBS_WIRE_U16_MAX = 65535 };
struct obs_combat_row;

struct hksim {
    /* The instance's arena; the struct itself and everything it reaches is allocated from it. */
    hks_arena *arena;
    hksim_config cfg;
    char level[64];
    hksim_trap_ctx trap;
    char err[512];
    const hk_scene_def *scene;
    hk_rng rng;
    /* physics */
    phys_world *pw;
    phys_body_id hero_body;
    phys_shape_id hero_shape;
    phys_body_id *static_bodies;
    phys_shape_id *static_shapes;   /* parallel to static_bodies; handed to the FSM module by bind_statics */
    int32_t *static_iids;           /* Collider2D.instanceID per static, the key the FSM module matches on */
    /* modules */
    hero H;
    void *fsm_ctx;
    /* clocks (Unity) */
    uint32_t frame, fixed_count, step, reset_index;
    float time, fixed_time, tsll;
    int32_t last_action[4];
    uint32_t last_input;
    int episode_done;
    int episode_end_kind;                   /* EPEND_*: 0 none, 1 loss, 2 win, 3 left_arena (see finish_step) */
    int32_t hp_at_step_start;

    /* observation scratch (per instance, so instances on different threads do not share it) */
    struct obs_combat_src *obs_srcs;
    struct obs_combat_row *obs_rows;
    const char **obs_kinds, **obs_clips, **obs_snaps;
    uint32_t obs_cap_c, obs_cap_f;   /* capacities of the combat arrays and of obs_snaps */
    uint8_t *obs_buf; size_t obs_len, obs_cap;
    int32_t obs_mode;            /* HKSIM_OBS_* bit mask; HKSIM_OBS_WIRE unless the caller opts in */
    obs_numeric nb;              /* hksim.h fast path: the numeric observation, filled under HKSIM_OBS_BATCH */
    struct obs_terrain_row *tr_scratch;   /* terrain expansion scratch, grown once per scene */
    /* live Terrain bucket (fsmi_terrain_live): per scene->statics / scene->terrain_dyn entry */
    int32_t *ter_static_iid, *ter_dyn_iid; uint8_t *ter_static_active, *ter_dyn_active;
    phys_v2 *ter_dyn_pts; uint32_t *ter_dyn_off, *ter_dyn_n, ter_dyn_pts_cap;
    float step_game_time;
    int last_committed;
    /* trace */
    int trace_on;
    tw_buf trace;
    tw_buf header;
    double prof[PROF_N];
};

/* ---------------------------------------------------------------------------------------------- errors */
static void set_err(hksim *s, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vsnprintf(s ? s->err : g_create_err, 512, fmt, ap);
    va_end(ap);
}
const char *hksim_last_error(hksim *s) { return s ? s->err : g_create_err; }
/* hksim_abi_version / hksim_fp_probe live in version.c */

/* ABI boundary: bind the instance's arena and arm the trap; a trap longjmps back here and the call returns
 * its code.  _prev_arena is volatile so its value is guaranteed across the longjmp. */
#define ABI_ENTER(s) \
    hks_arena *volatile _prev_arena = hks_arena_bind((s)->arena); \
    if (setjmp((s)->trap.jb) != 0) { memcpy((s)->err, (s)->trap.msg, sizeof (s)->err); hks_arena_bind(_prev_arena); return (s)->trap.code; } \
    hksim_trap_arm(&(s)->trap)
#define ABI_LEAVE() do { hksim_trap_disarm(); hks_arena_bind(_prev_arena); } while (0)

static void build_obs(hksim *s, int is_reset, const hksim_step_result *r);

/* ----------------------------------------------------------- resolve a phys owner handle to scene data */
static const hk_static_collider *static_of_user(const hksim *s, uint32_t user)
{
    if (user >= HKSIM_USER_STATIC_BASE && user < HKSIM_USER_STATIC_BASE + s->scene->n_statics)
        return &s->scene->statics[user - HKSIM_USER_STATIC_BASE];
    return NULL;
}
static uint32_t hit_flags_of(const hksim *s, phys_shape_id sh)
{
    uint32_t f = phys_shape_is_trigger(s->pw, sh) ? HIT_TRIGGER : 0;
    const hk_static_collider *c = static_of_user(s, phys_shape_user(s->pw, sh));
    if (c) {
        if (c->marker_flags & 2) f |= HIT_STEEP_SLOPE;
        if (c->marker_flags & 4) f |= HIT_NON_SLIDER;
        if (c->marker_flags & 8) f |= HIT_NON_THUNKER_ACTIVE;
    }
    return f;
}

static uint32_t hit_layer_of(const hksim *s, phys_shape_id sh)
{
    const hk_static_collider *c = static_of_user(s, phys_shape_user(s->pw, sh));
    return c ? c->layer : 0;   /* dynamic owners (fsm bodies) report their layer through phys_body_layer */
}

/* ------------------------------------------------------------------------------- hero physics vtable */
static void    op_get_pos(void *c, phys_v2 *out) { hksim *s = c; *out = phys_body_position(s->pw, s->hero_body); }
static void    op_set_pos(void *c, const phys_v2 *p) { hksim *s = c; phys_body_set_position(s->pw, s->hero_body, *p); }
static void    op_get_vel(void *c, phys_v2 *out) { hksim *s = c; *out = phys_body_velocity(s->pw, s->hero_body); }
static void    op_set_vel(void *c, const phys_v2 *v) { hksim *s = c; phys_body_set_velocity(s->pw, s->hero_body, *v); }
static float   op_get_gravity(void *c) { hksim *s = c; return phys_body_gravity_scale(s->pw, s->hero_body); }
static void    op_set_gravity(void *c, float g) { hksim *s = c; phys_body_set_gravity_scale(s->pw, s->hero_body, g); }
static float   op_get_scale_x(void *c) { hksim *s = c; return phys_body_scale_x(s->pw, s->hero_body); }
static void    op_set_scale_x(void *c, float sx) { hksim *s = c; phys_body_set_scale_x(s->pw, s->hero_body, sx); }
static void    op_set_kinematic(void *c, int on) { hksim *s = c; phys_body_set_type(s->pw, s->hero_body, on ? PHYS_BODY_KINEMATIC : PHYS_BODY_DYNAMIC); }
static void    op_set_layer(void *c, int layer) { hksim *s = c; phys_body_set_layer(s->pw, s->hero_body, (uint32_t)layer); }
static int op_raycast(void *c, const phys_v2 *origin, const phys_v2 *dir, float len, uint32_t mask, hero_hit *out)
{
    hksim *s = c; phys_v2 p, n; phys_shape_id sh;
    if (!phys_raycast(s->pw, *origin, *dir, len, mask, &p, &n, &sh)) return 0;
    if (out) { out->point = p; out->normal = n; out->flags = hit_flags_of(s, sh); out->layer = hit_layer_of(s, sh); }
    return 1;
}
static int op_boxcast(void *c, const phys_v2 *origin, const phys_v2 *size, const phys_v2 *dir, float len, uint32_t mask, hero_hit *out)
{
    hksim *s = c; phys_v2 p, n; phys_shape_id sh;
    if (!phys_boxcast(s->pw, *origin, *size, *dir, len, mask, &p, &n, &sh)) return 0;
    if (out) { out->point = p; out->normal = n; out->flags = hit_flags_of(s, sh); out->layer = hit_layer_of(s, sh); }
    return 1;
}
static void op_slash_set_enabled(void *c, int slash, int poly_on, int clash_tink_on)
{
    hksim *s = c;
    fsmi_slash_set_enabled(s->fsm_ctx, slash, poly_on, clash_tink_on);
}
static const hero_phys_ops OPS_TEMPLATE = {
    NULL, op_get_pos, op_set_pos, op_get_vel, op_set_vel, op_get_gravity, op_set_gravity,
    op_get_scale_x, op_set_scale_x, op_set_kinematic, op_set_layer, op_raycast, op_boxcast, op_slash_set_enabled,
};

/* ------------------------------------------------------------------------------------ world building */
static const hk_hero_collider *hero_body_collider(const hk_scene_def *sc)
{
    for (uint32_t i = 0; i < sc->n_hero_cols; i++) {
        const hk_hero_collider *c = &sc->hero_cols[i];
        if (strcmp(c->path, "Knight") == 0 && strcmp(c->type, "BoxCollider2D") == 0 && !c->is_trigger) return c;
    }
    return NULL;
}

static void build_world(hksim *s)
{
    const hk_scene_def *sc = s->scene;
    if (s->pw) { phys_destroy(s->pw); s->pw = NULL; }
    free(s->static_bodies); s->static_bodies = NULL;
    free(s->static_shapes); s->static_shapes = NULL;
    free(s->static_iids); s->static_iids = NULL;
    s->pw = phys_create(sc->gravity, sc->velocity_iters, sc->position_iters, sc->layer_mask);
    HKSIM_ASSERT(s->pw != NULL, "phys_create failed");
    /* hero body: cite dumps/GG_Hornet_1/physics.json#rb2d (gravityScale, mass, freezeRotation, Continuous)
     * and #heroColliders[path=="Knight"] (offset, size, edgeRadius, layer 9 Player).  It is created before the scene's
     * colliders: the Knight is DontDestroyOnLoad and exists before the boss scene loads, so its proxy ids are the
     * lower ones and its box is fixture A against the terrain (UP!0x180baa590 b2BroadPhase::UpdatePairs; the r2
     * traces' wall-contact velocity residuals need the hero's face as the reference: tests/test_phys.py E2). */
    const hk_hero_collider *hc = hero_body_collider(sc);
    HKSIM_ASSERT(hc != NULL, "hero body collider not in scene tables");
    phys_body_desc hb = { PHYS_BODY_DYNAMIC, strcmp(sc->hero_cd_mode, "Continuous") == 0 ? PHYS_CD_CONTINUOUS : PHYS_CD_DISCRETE,
                          { 0.0f, 0.0f }, 0.0f, { 1.0f, 1.0f }, { 0.0f, 0.0f }, sc->hero_gravity_scale, sc->hero_mass,
                          true, hc->layer, HKSIM_USER_HERO, sc->hero_freeze_rotation == 0 };
    phys_shape_desc hs = { PHYS_SHAPE_BOX, false, true, { hc->ox, hc->oy }, { hc->w, hc->h }, 0.0f, hc->edge_radius,
                           NULL, 0, HKSIM_USER_HERO, PHYS_LAYER_INHERIT, hc->instance_id };
    hb.position.x = sc->hero_pos_x; hb.position.y = sc->hero_pos_y;      /* SceneReady pose: dumps/physics.json#rb2d */
    hb.velocity.x = sc->hero_vel_x; hb.velocity.y = sc->hero_vel_y;
    hb.scale.x = sc->hero_scale_x;                                        /* hero.json#transform.localScale.x (facing) */
    s->hero_body = phys_body_add(s->pw, &hb);   /* drag = angularDrag = 0 (physics.json#rb2d): the damping phys_body_add starts with */
    s->hero_shape = phys_shape_add(s->pw, s->hero_body, &hs);
    s->static_bodies = calloc(sc->n_statics, sizeof *s->static_bodies);
    s->static_shapes = calloc(sc->n_statics, sizeof *s->static_shapes);
    s->static_iids = calloc(sc->n_statics, sizeof *s->static_iids);
    for (uint32_t i = 0; i < sc->n_statics; i++) {
        const hk_static_collider *c = &sc->statics[i];
        s->static_iids[i] = c->instance_id;
        /* `active` is `activeInHierarchy && Collider2D.enabled` (gen_scene.py).  Every collider gets a shape and
         * `active` is its initial enabled state, so a hazard switched on mid-fight (through the id handed over
         * by bind_statics) has a shape to enable. */
        if (c->unsupported) {
            if (c->active) HKSIM_UNIMPLEMENTED("static collider %s: unsupported shape (code %d)", c->path, c->unsupported);
            continue;   /* no shape for an unsupported one; the trap still fires the moment it is the active one */
        }
        phys_body_desc bd = { PHYS_BODY_STATIC, PHYS_CD_DISCRETE, { c->px, c->py }, c->rot_deg, { c->sx, c->sy },
                              { 0.0f, 0.0f }, 0.0f, 0.0f, true, c->layer, HKSIM_USER_STATIC_BASE + i, false };
        phys_shape_desc sd = { c->shape, c->is_trigger != 0, c->active != 0, { c->ox, c->oy }, { c->w, c->h }, c->radius,
                               c->edge_radius, c->points, c->n_points, HKSIM_USER_STATIC_BASE + i, PHYS_LAYER_INHERIT,
                               c->instance_id };
        s->static_bodies[i] = phys_body_add(s->pw, &bd);
        s->static_shapes[i] = phys_shape_add(s->pw, s->static_bodies[i], &sd);
    }
    s->H.col_offset.x = hc->ox; s->H.col_offset.y = hc->oy;
    s->H.col_size.x = hc->w; s->H.col_size.y = hc->h;
    s->H.col_edgeRadius = hc->edge_radius;
}

/* --------------------------------------------------------------------------------------- trace bits */
static void emit_pose(hksim *s, uint8_t kind)
{
    if (!s->trace_on) return;
    phys_v2 p = phys_body_position(s->pw, s->hero_body), v = phys_body_velocity(s->pw, s->hero_body);
    tw_pose(&s->trace, kind, s->frame, s->fixed_count, s->fixed_time, p.x, p.y, p.x, p.y, v.x, v.y);
}

static void emit_frame(hksim *s)
{
    if (!s->trace_on) return;
    uint32_t rng[4] = { s->rng.x, s->rng.y, s->rng.z, s->rng.w };
    phys_v2 p = phys_body_position(s->pw, s->hero_body), v = phys_body_velocity(s->pw, s->hero_body);
    tw_frame_begin(&s->trace, s->frame, s->fixed_count, s->time, R2_DT, R2_DT, s->fixed_time, 1.0f, rng,
                   hero_shim_key_bits(&s->H), s->step);
    tw_hero_begin(&s->trace, p.x, p.y, phys_body_scale_x(s->pw, s->hero_body), p.x, p.y, v.x, v.y,
                  phys_body_gravity_scale(s->pw, s->hero_body), false, hero_cstate_bits(&s->H));
    uint32_t n; const hero_field_desc *t = hero_field_table(&n);
    for (uint32_t i = 0; i < n; i++) {
        const char *base = (const char *)&s->H.f + t[i].offset;
        if (t[i].code == 'f') tw_f32(&s->trace, *(const float *)base);
        else if (t[i].code == 'b') tw_i32(&s->trace, *(const uint8_t *)base ? 1 : 0);
        else tw_i32(&s->trace, *(const int32_t *)base);
    }
    t = hero_pd_field_table(&n);
    for (uint32_t i = 0; i < n; i++) {
        const char *base = (const char *)&s->H.pd + t[i].offset;
        if (t[i].code == 'f') tw_f32(&s->trace, *(const float *)base);
        else if (t[i].code == 'b') tw_i32(&s->trace, *(const uint8_t *)base ? 1 : 0);
        else tw_i32(&s->trace, *(const int32_t *)base);
    }
    {
        const char *clip = ""; int32_t fr = 0; float ct = 0.0f; int playing = 0; float fps = 0.0f;
        fsmi_hero_anim(s->fsm_ctx, &clip, &fr, &ct, &playing, &fps);
        tw_anim(&s->trace, clip, fr, ct, playing != 0, fps);
    }
    tw_cols_begin(&s->trace, 1);
    tw_col(&s->trace, "BoxCollider2D", true, s->H.col_offset.x, s->H.col_offset.y, s->H.col_size.x, s->H.col_size.y);
    fsmi_emit_entities(s->fsm_ctx, &s->trace);
}

static void build_header(hksim *s)
{
    char cap[256];
    snprintf(cap, sizeof cap,
             "{\"scene\":\"%s\",\"level_requested\":\"%s\",\"mode\":\"sim\",\"capture_dt\":0.02,\"fixed_dt\":0.02,"
             "\"frames_per_wait\":%u,\"seed\":%d,\"sim_abi\":%u}",
             s->level, s->level, s->cfg.frames_per_wait, s->cfg.seed, HKSIM_ABI_VERSION);
    uint32_t nh, nc, np;
    const hero_field_desc *hf = hero_field_table(&nh);
    const char *const *cs = hero_cstate_names(&nc);
    const hero_field_desc *pf = hero_pd_field_table(&np);
    const char **hn = malloc(nh * sizeof *hn), **ht = malloc(nh * sizeof *ht);
    const char **pn = malloc(np * sizeof *pn), **pt = malloc(np * sizeof *pt);
    /* cite: real trace headers use "f32" / "i32" (bools and enums are i32 on the wire; oracle/Oracle/TraceWriter.cs) */
    for (uint32_t i = 0; i < nh; i++) { hn[i] = hf[i].name; ht[i] = hf[i].code == 'f' ? "f32" : "i32"; }
    for (uint32_t i = 0; i < np; i++) { pn[i] = pf[i].name; pt[i] = pf[i].code == 'f' ? "f32" : "i32"; }
    static const char *const inputs[] = { "left", "right", "up", "down", "jump", "attack", "dash", "cast", "dream_nail", "super_dash", "focus" };  /* cite: oracle/Game/ProxyController.cs KeyNames */
    tw_clear(&s->header);
    tw_header_json(&s->header, cap, hn, ht, nh, cs, nc, pn, pt, np, inputs, (uint32_t)(sizeof inputs / sizeof *inputs), "Knight");
    tw_u8(&s->header, 0);   /* NUL terminator for hksim_trace_header */
    free(hn); free(ht); free(pn); free(pt);
}

/* ------------------------------------------------------------------------------------ frame engine */
/* One physics event, routed to every receiver.  The single routing point: any caller delivering an
 * event goes through here rather than re-implementing the routing. */
static void dispatch_one_phys_event(hksim *s, const phys_event *ev)
{
    uint32_t ua = phys_body_user(s->pw, ev->body_a);
    uint32_t sa = phys_shape_user(s->pw, ev->shape_a);
    if (sa >= HKSIM_USER_FSM_BASE) {
        /* shapes the fsm module owns on the hero body (NailSlash polygons, Clash Tink, HeroBox) */
        fsmi_on_phys_event(s->fsm_ctx, ev);
        return;
    }
    if (ua == HKSIM_USER_HERO) {
        uint32_t ub = phys_body_user(s->pw, ev->body_b);
        const hk_static_collider *sc = static_of_user(s, ub);
        hero_contact c = { ev->normal, phys_body_layer(s->pw, ev->body_b), 0, 0 };
        if (sc) {
            c.tag_hero_walkable = strcmp(sc->tag, "HeroWalkable") == 0;
            c.flags = ((sc->marker_flags & 1) ? HERO_CONTACT_NO_HARD_LANDING : 0) | ((sc->marker_flags & 2) ? HERO_CONTACT_STEEP_SLOPE : 0)
                    | ((sc->marker_flags & 4) ? HERO_CONTACT_NON_SLIDER : 0);
        }
        switch (ev->kind) {
        case PHYS_EV_COLLISION_ENTER:
            hero_on_collision_enter(&s->H, &c);
            /* Roof.OnCollisionEnter2D (Roof.cs:11-19) after HeroController's: the knight's collider has the lower
             * instance id in every ported scene (DDOL 84234 < any scene object's; gen_scene.py checks), and a report
             * reaches collider A's object first (native-physics2d.md §6.3 SendCallbackReports) */
            if (sc && (sc->marker_flags & 16)) {
                hero_cancel_super_dash(&s->H);
                hero_cancel_hero_jump(&s->H);                             /* HC:1669-1680 */
            }
            if (sc && (sc->marker_flags & 32))
                HKSIM_UNIMPLEMENTED("KillOnContact on '%s' touched the knight: HeroController.HazardRespawn started directly "
                                    "(KillOnContact.cs:20-23) is not ported", sc->path);
            break;
        case PHYS_EV_COLLISION_STAY:  hero_on_collision_stay(&s->H, &c); break;
        case PHYS_EV_COLLISION_EXIT:  hero_on_collision_exit(&s->H, &c); break;
        default: break;   /* trigger events on the hero body: HeroBox/DamageHero path */
        }
    } else if (ua >= HKSIM_USER_FSM_BASE) {
        fsmi_on_phys_event(s->fsm_ctx, ev);
    } else if (sa >= HKSIM_USER_STATIC_BASE) {
        /* A STATIC collider (core-owned, no Rigidbody2D) receiving the callback: Unity sends
         * OnTrigger/OnCollision*2D to the GameObject of EACH collider of the pair whether or not it has a
         * Rigidbody2D.  The fsm module maps the static handle back to its GameObject. */
        fsmi_on_phys_event(s->fsm_ctx, ev);
    }
}

/* PhysicsManager2D::Simulate (UP!0x180beb390, native-physics2d.md §4): the step, the body -> Transform write-back,
 * then ProcessContacts delivers the step's callbacks */
static void dispatch_phys_events(hksim *s)
{
    fsmi_after_physics_step(s->fsm_ctx);
    const phys_event *ev; uint32_t n = phys_events(s->pw, &ev);
    for (uint32_t i = 0; i < n; i++) dispatch_one_phys_event(s, &ev[i]);
}

/* ------------------------------------------------------------------------ the component lifecycle
 * A frame is the player loop's stages in order; every component -- the FSM world's and the core's own -- is
 * dispatched by sim/fsm/runtime/lifecycle.c in Unity's order (docs/engine-lifecycle.md R0-R7):
 * HeroController.FixedUpdate (208) and NailSlash.FixedUpdate (500) run AFTER every order-0 PlayMakerFixedUpdate /
 * Recoil (R3); InControlManager (-100) before and HeroController.Update (208) after every order-0 Update;
 * HeroBox.LateUpdate (750) after the animators (-30095) and PlayMakerLateUpdate (0).  The core keeps its trace
 * bookkeeping (emit_events, the HC pre/post pose records of the recorder's MonoMod hooks) around its own
 * components' calls. */
static void ev_flush(hksim *s)
{
    if (s->trace_on) PROF(s, PROF_TRACE, fsmi_emit_events(s->fsm_ctx, &s->trace, s->frame, s->fixed_count));
}
static void lc_core_tick(void *ctx, int type, int cb)
{
    hksim *s = ctx;
    switch (type) {
    case LCT_EXT_INPUT:                                   /* InControlManager.Update (-100) */
        if (cb == LCB_UPDATE) PROF(s, PROF_HERO_UPDATE, hero_input_tick(&s->H));
        break;
    case LCT_EXT_HERO:                                    /* HeroController (208); hero_update includes HeroAnimationController (550, A-18) */
        if (cb == LCB_FIXED) {
            ev_flush(s);
            PROF(s, PROF_TRACE, emit_pose(s, TW_HC_FIXED_PRE));
            PROF(s, PROF_HERO_FIXED, hero_fixed_update(&s->H));
            ev_flush(s);                                  /* events raised inside HeroController.FixedUpdate */
            PROF(s, PROF_TRACE, emit_pose(s, TW_HC_FIXED_POST));
        } else if (cb == LCB_UPDATE) {
            /* FSM records raised by physics callbacks / the order-0 Updates precede the HC_UPDATE_PRE pre-hook
             * record in the real stream (frame-order.md §1.4 phase census) */
            ev_flush(s);
            PROF(s, PROF_TRACE, emit_pose(s, TW_HC_UPDATE_PRE));
            PROF(s, PROF_HERO_UPDATE, hero_update(&s->H));
            ev_flush(s);                                  /* events raised inside HeroController.Update (ProxyFSM etc.) */
            PROF(s, PROF_TRACE, emit_pose(s, TW_HC_UPDATE_POST));
        }
        break;
    case LCT_EXT_NAILSLASH:                               /* NailSlash.FixedUpdate (500) */
        if (cb == LCB_FIXED) PROF(s, PROF_HERO_FIXED, hero_slash_fixed_update(&s->H));
        break;
    case LCT_EXT_HEROBOX:                                 /* HeroBox.LateUpdate (750) */
        if (cb == LCB_LATE) PROF(s, PROF_LATE, hero_box_late_update(&s->H));
        break;
    default: break;
    }
}
/* the core's coroutines: HeroController's (hero_coroutine_phase), resumed in update_delayed after the pending
 * Starts (R1) and before the env coroutine that writes FRAME / OBS */
static void lc_core_coroutines(void *ctx, int stage)
{
    hksim *s = ctx;
    if (stage == LCS_UPDATE_DELAYED) PROF(s, PROF_HERO_CORO, hero_coroutine_phase(&s->H));
    else if (stage == LCS_END_OF_FRAME) PROF(s, PROF_HERO_CORO, hero_end_of_frame(&s->H));   /* WaitForEndOfFrame (R6) */
}
/* the coroutines queued behind the env coroutine in update_delayed (the WaitForSeconds resumes: HeroController's
 * Invulnerable, GameManager.PlayerDeadFromHazard and the HazardRespawn it starts): after FRAME / OBS, before
 * LateUpdate */
static void lc_after_env(hksim *s)
{
    PROF(s, PROF_HERO_CORO, hero_coroutine_after_env(&s->H));
    ev_flush(s);
}
static void lc_core_phys_dispatch(void *ctx, const phys_event *e) { dispatch_one_phys_event((hksim *)ctx, e); }
static void lc_bind_core(hksim *s)
{
    hksim_lc_core core = { s, lc_core_tick, lc_core_coroutines, lc_core_phys_dispatch };
    fsmi_lc_bind(s->fsm_ctx, &core);
    s->H.co.lc = 1;                                   /* the hero's WaitForSeconds resumes run at Unity's points */
}
#define LC_STAGE(s, st, dt, fl) do { fsmi_lc_stage(s->fsm_ctx, (st), (dt), (fl)); ev_flush(s); } while (0)

/* frozen frame (timeScale 0, R0: no fixed step).  Split at the env coroutine like the live frame: the STEP's ActionDecoder.ApplyAction runs in update_delayed, BEFORE this frame's
 * LateUpdate (TrainingEnv.Step resumes there and sets timeScale = 1, TrainingEnv.cs:604) */
static void lc_frozen_pre(hksim *s)
{
    s->frame++;
    hero_set_clock(&s->H, s->frame, 0.0f, s->tsll);
    fsmi_lc_frame(s->fsm_ctx, 1, s->frame, 0, s->time);
    LC_STAGE(s, LCS_STARTUP, 0.0f, LCF_FROZEN);
    LC_STAGE(s, LCS_UPDATE, 0.0f, LCF_FROZEN);            /* PlayMakerFSM.Update gated (FsmPauseGate), the rest dt 0 */
    LC_STAGE(s, LCS_UPDATE_DELAYED, 0.0f, LCF_FROZEN);
}
/* `timescale_restored`: the STEP coroutine reached `Time.timeScale = 1f` (TrainingEnv.cs:603) before this
 * frame's LateUpdate, so FsmPauseGate is open for it -- its Time.deltaTime is still the frame's 0.  On a step
 * taken after the episode ended the coroutine `yield break`s at TrainingEnv.cs:580-595, BEFORE that
 * assignment, so timeScale is still 0 and the gate stays CLOSED for the LateUpdate of that frame. */
static void lc_frozen_late(hksim *s, bool timescale_restored)
{
    lc_after_env(s);
    fsmi_lc_delayed_end(s->fsm_ctx); ev_flush(s);
    LC_STAGE(s, LCS_ANIM, 0.0f, 0);                       /* the Animators' dt was sampled at frame start: 0 */
    fsmi_lc_late_gate(s->fsm_ctx, timescale_restored ? 0 : 1);
    LC_STAGE(s, LCS_LATE, 0.0f, 0);
    LC_STAGE(s, LCS_POSTLATE_DELAYED, 0.0f, 0);
    LC_STAGE(s, LCS_END_OF_FRAME, 0.0f, 0);
    fsmi_lc_late_gate(s->fsm_ctx, 0);
    fsmi_lc_frame(s->fsm_ctx, 0, s->frame, 0, s->time);
}
static void lc_live_pre(hksim *s)
{
    s->frame++; s->fixed_count++;
    s->fixed_time += R2_DT; s->time += R2_DT; s->tsll += R2_DT;
    hero_set_clock(&s->H, s->frame, R2_DT, s->tsll);
    fsmi_lc_frame(s->fsm_ctx, 1, s->frame, 1, s->time);
    LC_STAGE(s, LCS_STARTUP, R2_DT, 0);
    PROF(s, PROF_TRACE, emit_pose(s, TW_FIXED));
    PROF(s, PROF_FSM_FIXED, LC_STAGE(s, LCS_FIXED, R2_DT, 0));
    fsmi_lc_stage(s->fsm_ctx, LCS_PHYSICS, R2_DT, 0);
    PROF(s, PROF_PHYS, phys_step(s->pw, R2_DT));
    PROF(s, PROF_EVENTS, dispatch_phys_events(s));
    ev_flush(s);
    LC_STAGE(s, LCS_FIXED_DELAYED, R2_DT, 0);
    PROF(s, PROF_FSM_UPDATE, LC_STAGE(s, LCS_UPDATE, R2_DT, 0));
    LC_STAGE(s, LCS_UPDATE_DELAYED, R2_DT, 0);
    /* FRAME: the env coroutine (TrainingEnv.cs:614-617), after the pending Starts and the coroutines that
     * were waiting before it, before LateUpdate */
    PROF(s, PROF_TRACE, emit_frame(s));
}
static void lc_live_late(hksim *s, bool last_of_step)
{
    lc_after_env(s);
    fsmi_lc_delayed_end(s->fsm_ctx); ev_flush(s);
    LC_STAGE(s, LCS_ANIM, R2_DT, 0);                      /* sampled at frame start: 0.02 even on the step's last frame */
    /* the step's last live frame: TrainingEnv set timeScale = 0 in the coroutine (TrainingEnv.cs:635), so
     * FsmPauseGate swallows PlayMakerLateUpdate */
    fsmi_lc_late_gate(s->fsm_ctx, last_of_step ? 1 : 0);
    PROF(s, PROF_LATE, LC_STAGE(s, LCS_LATE, R2_DT, 0));
    LC_STAGE(s, LCS_POSTLATE_DELAYED, R2_DT, 0);
    LC_STAGE(s, LCS_END_OF_FRAME, R2_DT, 0);
    fsmi_lc_late_gate(s->fsm_ctx, 0);
    fsmi_lc_frame(s->fsm_ctx, 0, s->frame, 1, s->time);
}

/* Compiled scenes.  Each needs BOTH halves generated: sim/core/scene_<name>.c (gen_scene.py) and
 * sim/fsm/tables_<name>.c (gen_tables.py); scene_registry.inc is rebuilt by sim/gen_registries.py. */
static const hk_scene_def *hk_scene_lookup(const char *name)
{
    for (size_t i = 0; i < sizeof HK_REGISTRY_SCENES / sizeof HK_REGISTRY_SCENES[0]; i++) {
        const hk_scene_def *sc = HK_REGISTRY_SCENES[i]();
        if (sc && strcmp(sc->name, name) == 0) return sc;
    }
    return NULL;
}

int32_t hksim_scene_count(void) { return (int32_t)(sizeof HK_REGISTRY_SCENES / sizeof HK_REGISTRY_SCENES[0]); }

const char *hksim_scene_name(int32_t i)
{
    if (i < 0 || i >= hksim_scene_count()) return NULL;
    const hk_scene_def *sc = HK_REGISTRY_SCENES[i]();
    return sc ? sc->name : NULL;
}

/* ------------------------------------------------------------------------------------------- ABI */
hksim *hksim_create(const hksim_config *cfg)
{
    if (!cfg || !cfg->level) { set_err(NULL, "hksim_create: null config/level"); return NULL; }
    const hk_scene_def *scene = hk_scene_lookup(cfg->level);
    if (!scene) { set_err(NULL, "UNKNOWN level %s (no compiled tables; run sim/core/gen_scene.py and sim/fsm/gen/gen_tables.py for it)", cfg->level); return NULL; }
    /* frames_per_wait: any N >= 1.  R2_DT is the per-FRAME dt, and locked_steps_for ports
     * ProxyController.cs:289-300, which derives the hold length from framesPerWait.  The trainer sets
     * this from train/config.py's frames_per_wait. */
    if (cfg->frames_per_wait < 1) { set_err(NULL, "frames_per_wait must be >= 1 (got %u)", cfg->frames_per_wait); return NULL; }
    hks_arena *arena = hks_arena_create(0);
    if (!arena) { set_err(NULL, "hksim_create: cannot reserve an address-space region for the instance"); return NULL; }
    hks_arena *prev = hks_arena_bind(arena);
    hksim *s = calloc(1, sizeof *s);
    s->arena = arena;
    s->cfg = *cfg;
    snprintf(s->level, sizeof s->level, "%s", cfg->level);
    s->cfg.level = s->level;
    s->scene = scene;
    s->obs_mode = HKSIM_OBS_WIRE;   /* default */
    s->trace_on = cfg->trace != 0;
    tw_init(&s->trace); tw_init(&s->header);
    s->obs_srcs  = calloc(OBS_MAXC, sizeof *s->obs_srcs);
    s->obs_rows  = calloc(OBS_MAXC, sizeof *s->obs_rows);
    s->obs_kinds = calloc(OBS_MAXC, sizeof *s->obs_kinds);
    s->obs_clips = calloc(OBS_MAXC, sizeof *s->obs_clips);
    s->obs_snaps = calloc(OBS_MAXF, sizeof *s->obs_snaps);
    s->obs_cap_c = OBS_MAXC; s->obs_cap_f = OBS_MAXF;
    {   /* live-terrain scratch, sized by the scene tables once */
        uint32_t ns = scene->n_statics, nd = scene->terrain_dyn ? scene->n_terrain_dyn : 0, np = 0;
        s->ter_static_iid = calloc(ns ? ns : 1, sizeof *s->ter_static_iid);
        s->ter_static_active = calloc(ns ? ns : 1, 1);
        s->ter_dyn_iid = calloc(nd ? nd : 1, sizeof *s->ter_dyn_iid);
        s->ter_dyn_active = calloc(nd ? nd : 1, 1);
        s->ter_dyn_off = calloc(nd ? nd : 1, sizeof *s->ter_dyn_off);
        s->ter_dyn_n = calloc(nd ? nd : 1, sizeof *s->ter_dyn_n);
        for (uint32_t i = 0; i < ns; i++) s->ter_static_iid[i] = scene->statics[i].instance_id;
        for (uint32_t j = 0; j < nd; j++) {
            const hk_static_collider *c = &scene->terrain_dyn[j];
            s->ter_dyn_iid[j] = c->instance_id;
            s->ter_dyn_off[j] = np;
            np += c->shape == PHYS_SHAPE_BOX ? 4u : c->n_points;
        }
        s->ter_dyn_pts = calloc(np ? np : 1, sizeof *s->ter_dyn_pts);
        s->ter_dyn_pts_cap = np;
    }
    hks_arena_bind(prev);
    return s;
}

void hksim_destroy(hksim *s)
{
    if (!s) return;
    hks_arena *arena = s->arena;
    hks_arena *prev = hks_arena_bind(arena);
    hk_rng_free(&s->rng);
    if (s->fsm_ctx) fsmi_destroy(s->fsm_ctx);
    if (s->pw) phys_destroy(s->pw);
    free(s->static_bodies);
    free(s->static_shapes);
    free(s->static_iids);
    tw_free(&s->trace); tw_free(&s->header);
    free(s->obs_buf);
    free(s->tr_scratch);
    nb_free(&s->nb);
    free(s);
    hks_arena_bind(prev == arena ? NULL : prev);
    hks_arena_destroy(arena);          /* releases the whole instance in one piece */
}

/* ------------------------------------------------------------------------------- checkpoints
 * The instance and everything it reaches live in one arena whose base never moves (alloc.h), so a
 * checkpoint is a copy of the arena's used bytes and a restore copies them back.  Runtime pool growth
 * (world_instantiate) allocates from the same arena, so a restore also drops clones made since. */
struct hksim_checkpoint {
    const hks_arena *arena;   /* the producing instance: its bytes are pointers into that arena */
    void  *buf;
    size_t cap, len;
};

/* Not simulation state, though inside the arena: the trap context (a jmp_buf naming the running ABI
 * call's frame), the last error, and the wall-clock profile.  A save zeroes them, so two saves of one
 * position are equal byte for byte; a restore keeps the live ones. */
static void scaffold_blank(const hksim *s, void *buf, size_t len)
{
    const unsigned char *base = hks_arena_base(s->arena);
    const struct { const void *p; size_t n; } parts[] = {
        { &s->trap, sizeof s->trap }, { s->err, sizeof s->err }, { s->prof, sizeof s->prof },
    };
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        size_t off = (size_t)((const unsigned char *)parts[i].p - base);
        if (off + parts[i].n <= len) memset((unsigned char *)buf + off, 0, parts[i].n);
    }
}

int hksim_checkpoint_save(hksim *s, hksim_checkpoint *c)
{
    if (!s || !c) return HKSIM_ERR_BAD_ARG;
    ABI_ENTER(s);
    hks_arena_save(s->arena, &c->buf, &c->cap, &c->len);
    scaffold_blank(s, c->buf, c->len);
    c->arena = s->arena;
    ABI_LEAVE();
    return HKSIM_OK;
}

hksim_checkpoint *hksim_checkpoint_new(hksim *s)
{
    if (!s) return NULL;
    hksim_checkpoint *c = hks_sys_calloc(1, sizeof *c);
    if (!c) { set_err(s, "checkpoint: out of memory"); return NULL; }
    if (hksim_checkpoint_save(s, c) != HKSIM_OK) { hks_sys_free(c->buf); hks_sys_free(c); return NULL; }
    return c;
}

int hksim_checkpoint_restore(hksim *s, const hksim_checkpoint *c)
{
    if (!s || !c || !c->buf) return HKSIM_ERR_BAD_ARG;
    if (c->arena != s->arena) {
        set_err(s, "checkpoint: taken from a different instance (its bytes point into that instance's arena)");
        return HKSIM_ERR_BAD_ARG;
    }
    ABI_ENTER(s);
    hksim_trap_ctx trap = s->trap;   /* `s` is inside the arena: the copy would overwrite these */
    double prof[PROF_N];
    char err[sizeof s->err];
    memcpy(prof, s->prof, sizeof prof);
    memcpy(err, s->err, sizeof err);
    hks_arena_load(s->arena, c->buf, c->len);
    s->trap = trap;
    memcpy(s->prof, prof, sizeof prof);
    memcpy(s->err, err, sizeof err);
    ABI_LEAVE();
    return HKSIM_OK;
}

void hksim_checkpoint_free(hksim_checkpoint *c)
{
    if (!c) return;
    hks_sys_free(c->buf);
    hks_sys_free(c);
}

size_t hksim_checkpoint_bytes(const hksim_checkpoint *c) { return c ? c->len : 0; }

size_t hksim_checkpoint_read(const hksim_checkpoint *c, uint8_t *buf, size_t cap)
{
    if (!c || !c->buf) return 0;
    if (buf && cap >= c->len) memcpy(buf, c->buf, c->len);
    return c->len;
}

int hksim_reset(hksim *s, int32_t seed)
{
    if (!s) return HKSIM_ERR_BAD_ARG;
    ABI_ENTER(s);
    s->cfg.seed = seed;
    s->reset_index++;
    s->step = 0; s->episode_done = 0;
    uint32_t seed_frame = s->cfg.scene_ready_frame >= 101 ? s->cfg.scene_ready_frame - 101 : 0;
    uint32_t seed_fc = s->cfg.fixed_count0 >= 102 ? s->cfg.fixed_count0 - 102 : 0;
    s->frame = seed_frame; s->fixed_count = seed_fc;
    s->time = s->cfg.time0 - 101.0f * R2_DT; s->fixed_time = s->time; s->tsll = 0.0f;
    memset(s->last_action, 0, sizeof s->last_action);
    if (s->fsm_ctx) { fsmi_destroy(s->fsm_ctx); s->fsm_ctx = NULL; }
    if (s->trace_on) {
        tw_ev_str(&s->trace, TW_EV_RESET_BEGIN, seed_frame, seed_fc, 3, s->level);
        tw_ev_str(&s->trace, TW_EV_SCENE_LOADED, seed_frame, seed_fc, 3, s->level);
    }
    hk_rng_init(&s->rng, seed);   /* Random.InitState(seed) at sceneLoaded */
    if (s->trace_on) tw_ev_rng_seed(&s->trace, seed_frame, seed_fc, 3, seed);
    memset(&s->H, 0, sizeof s->H);
    build_world(s);
    /* contact priming: at SceneReady the hero has been standing on the floor for ~100 frames (frame-order.md,
     * port-fsm.md pre-SceneReady window), so Box2D's contact already exists and the first step reports Stay, not
     * Enter.  A fresh world would report Enter (-> landing logic).  One physics step at rest is bit-stable
     * (port-phys.md E3) and its events are discarded (hero not bound yet). */
    { const phys_event *ev; phys_step(s->pw, R2_DT); (void)phys_events(s->pw, &ev); }
    hero_init_from_dump(&s->H);
    /* ...then the fields this scene's own dump disagrees with Hornet's on (e.g. Gruz Mother and False
     * Knight start with acceptingInput=false / controlReqlinquished=true while the boss lands). */
    hero_init_scene(&s->H, s->level);
    s->H.rng = &s->rng;   /* HeroController Random.Range draws (HC:1862-1898, HC:4402) come from the shared stream */
    hero_phys_ops ops = OPS_TEMPLATE; ops.ctx = s;
    hero_hooks hooks;
    char err[256] = "";
    if (fsmi_create(&s->fsm_ctx, s->level, &s->rng, s->pw, &s->H, &hooks, err, sizeof err) != 0)
        HKSIM_UNIMPLEMENTED("fsm create: %s", err);
    /* The FSM event log feeds only hksim_drain, so it is recorded only when tracing. */
    fsmi_set_record(s->fsm_ctx, s->trace_on);
    fsmi_bind_hero_body(s->fsm_ctx, s->hero_body);
    /* Static scene colliders (no Rigidbody2D) are core-owned (user HKSIM_USER_STATIC_BASE + i); hand the module
     * the (instance_id -> body, shape) table so it attaches them to the GameObjects that own them. */
    fsmi_bind_statics(s->fsm_ctx, s->static_iids, s->static_bodies, s->static_shapes, s->scene->n_statics);
    lc_bind_core(s);                   /* the lifecycle dispatches the core's components too */
    hero_bind(&s->H, &ops, &hooks);
    fsmi_scene_start(s->fsm_ctx, s->cfg.time0);
    s->frame = s->cfg.scene_ready_frame;
    s->fixed_count = s->cfg.fixed_count0;
    s->time = s->cfg.time0;
    s->fixed_time = s->cfg.time0;
    s->tsll = s->cfg.time_since_level_load0;
    build_header(s);
    if (s->trace_on) tw_ev_str(&s->trace, TW_EV_SCENE_READY, s->frame, s->fixed_count, 3, s->level);
    s->step_game_time = 0.0f;
    build_obs(s, 1, NULL);
    s->hp_at_step_start = s->H.pd.health;
    (void)fsmi_take_damage_landed(s->fsm_ctx);   /* the episode's accumulator starts at zero */
    s->H.after_take_damage_sum = 0;   /* TrainingEnv.cs:524-529 re-zeroes _hitsTakenInStep after every frame the reset ticked */
    ABI_LEAVE();
    return HKSIM_OK;
}

/* Episode results, as the wire's info trailer spells them (oracle TrainingEnv.cs:655-690; BinaryProtocol.cs:160-165).
 * EPEND_LEFT_ARENA has no game counterpart: the game's episode would run on into a non-arena scene, which this
 * simulator does not contain (act_knight.c BeginSceneTransition), so it is a sim-only, explicitly named end. */
enum { EPEND_NONE = 0, EPEND_LOSS = 1, EPEND_WIN = 2, EPEND_LEFT_ARENA = 3 };
static const char *epend_info(int kind)
{
    return kind == EPEND_WIN ? "win" : kind == EPEND_LOSS ? "loss" : kind == EPEND_LEFT_ARENA ? "left_arena" : "";
}

/* TrainingEnv.Step's frame loop breaks after any frame in which `_bossDied || PlayerData.instance.health <= 0`
 * (oracle TrainingEnv.cs:622-633).  _bossDied is set only by OnBossesDead / a tracked OnDeath (TrainingEnv.cs:1277-1297),
 * i.e. boss_dead() == 1; leaving the arena (2) does not stop the loop. */
static int step_should_break(hksim *s)
{
    if (s->H.pd.health <= 0) return 1;
    return fsmi_boss_dead(s->fsm_ctx) == 1;
}

static void finish_step(hksim *s, hksim_step_result *out)
{
    int32_t hp = s->H.pd.health;
    int32_t dhp = hp - s->hp_at_step_start;
    /* hits_taken is TrainingEnv.OnKnightDamaged's sum over ModHooks.AfterTakeDamage (oracle TrainingEnv.cs:1169-1177,
     * HeroController.cs:1958): damage after the BossLevel switch, before the PlayerData clamp, landed hits only.  It
     * is not the HP delta: an overkill counts in full, and a hit and a heal in one step do not net out. */
    float hits = (float)s->H.after_take_damage_sum;
    s->H.after_take_damage_sum = 0;
    float landed = 0.0f;
    int boss_dead = 0;
    /* TrainingEnv.cs:1163 accumulates, per hit, DamageDealt / (n * maxHP) * 100 with n = _bossHMs.Count
     * (overkill counts in full).  The simulator accumulates the same sum at the hit (hk_comp.c
     * HealthManager.TakeDamage) and reads it here. */
    landed = fsmi_take_damage_landed(s->fsm_ctx);
    boss_dead = fsmi_boss_dead(s->fsm_ctx);
    s->hp_at_step_start = hp;
    s->episode_done = (hp <= 0) || boss_dead;
    /* boss_dead: 1 = the fight is over (OnBossesDead or all tracked HMs dead), 2 = the arena was left.
     * Precedence is TrainingEnv.cs:655-690's if/else-if order: _bossDied ("win") before health <= 0 ("loss"), so a
     * step on which both die is a win.  (The fake-reset branch before them needs _fakeResetProb > 0; not modelled.) */
    s->episode_end_kind = !s->episode_done ? EPEND_NONE : boss_dead == 1 ? EPEND_WIN : hp <= 0 ? EPEND_LOSS : EPEND_LEFT_ARENA;
    /* hp_healed = max(0, hpNow - hpAtStepStart), forced to 0 on the done step (TrainingEnv.cs:704-709) */
    float healed = (dhp > 0 && !s->episode_done) ? (float)dhp : 0.0f;
    if (s->episode_done && s->trace_on) tw_ev_str(&s->trace, TW_EV_EPISODE_END, s->frame, s->fixed_count, 0, epend_info(s->episode_end_kind));
    if (out) { out->done = s->episode_done != 0; out->damage_landed = landed; out->hits_taken = hits; out->hp_healed = healed; out->frame = s->frame; }
}

/* The step's frozen frame, through its LateUpdate.  Returns 1 when the episode had already ended: `*out` holds the
 * step's result and no live frame follows; else 0, with the action applied. */
static int step_frozen_frame(hksim *s, const int32_t action[4], hksim_step_result *out)
{
    s->step++;   /* cite: r2 traces — TrainingEnv._stepCount is incremented before the STEP event; the first step is 1 */
    PROF(s, PROF_FROZEN, lc_frozen_pre(s));
    if (s->episode_done) {
        /* TrainingEnv.cs:576-592: a step after the episode ended applies no action and runs no frame; it returns
         * done with the stored result, zero scalars and the empty observation.
         * The check is INSIDE the Step coroutine (TrainingEnv.cs:580), which resumes mid-frame: `yield break`
         * ends the coroutine, not the frame, so the frozen frame still gets its LateUpdate (lc_frozen_late).
         * timeScale is never restored on this path (the yield break precedes TrainingEnv.cs:603), hence `false`. */
        PROF(s, PROF_FROZEN, lc_frozen_late(s, false));
        hksim_step_result tmp; memset(&tmp, 0, sizeof tmp);
        tmp.done = 1; tmp.frame = s->frame;
        s->last_committed = 0;
        s->step_game_time = 0.0f;
        PROF(s, PROF_OBS, build_obs(s, 0, &tmp));
        if (out) *out = tmp;
        return 1;
    }
    memcpy(s->last_action, action, sizeof s->last_action);
    int32_t act[4] = { action[0], action[1], action[2], action[3] };
    int committed = hero_apply_action(&s->H, act, (int)s->cfg.frames_per_wait);   /* ActionDecoder.ApplyAction in the coroutine phase */
    s->last_committed = committed;
    /* the STEP event records the action AFTER ActionDecoder.ApplyAction rewrote it (hard-commit override of
     * action[2]; oracle/Environment/TrainingEnv.cs RaiseStepBegin after data.action_committed) — r2 traces */
    if (s->trace_on) tw_ev_step(&s->trace, s->frame, s->fixed_count, 0, s->step, act, committed != 0);
    PROF(s, PROF_FROZEN, lc_frozen_late(s, true));   /* the frozen frame's LateUpdate follows the STEP coroutine */
    s->step_game_time = 0.0f;
    return 0;
}

/* One live frame of a step.  `last`: the frame-skip loop ends with this frame; with `may_break` it also ends on the
 * episode-end test of TE:630-632.  Returns whether the frame was the last; the last one fills `*res`. */
static int step_live_frame(hksim *s, int last, int may_break, hksim_step_result *res)
{
    lc_live_pre(s);
    s->step_game_time += R2_DT;   /* TE:619,628 float32 Σ dt */
    /* TE:630-632: `if (_bossDied || health <= 0) break;` right after the frame, so the remaining frames of the
     * step never run and the terminal reward/observation come from this one. */
    if (!last && may_break && step_should_break(s)) last = 1;
    if (last) {   /* still in the coroutine: reward + OBS precede this frame's LateUpdate */
        finish_step(s, res);
        PROF(s, PROF_OBS, build_obs(s, 0, res));
    }
    /* TE:635 `Time.timeScale = 0` sits right AFTER the frame-skip loop, and the loop leaves it either by
     * exhausting _frameSkipCount or through TE:630-632's break -- both inside the coroutine resume in this
     * frame's update_delayed, before this frame's LateUpdate.  So the FsmPauseGate closes on whichever frame
     * is `last`. */
    lc_live_late(s, last != 0);
    return last;
}

int hksim_step(hksim *s, const int32_t action[4], hksim_step_result *out)
{
    if (!s || !action) return HKSIM_ERR_BAD_ARG;
    if (!s->pw) { set_err(s, "hksim_step before hksim_reset"); return HKSIM_ERR_BAD_ARG; }
    ABI_ENTER(s);
    if (step_frozen_frame(s, action, out)) { ABI_LEAVE(); return HKSIM_OK; }
    hksim_step_result tmp; memset(&tmp, 0, sizeof tmp);
    for (uint32_t i = 0; i < s->cfg.frames_per_wait; i++)
        if (step_live_frame(s, i + 1 == s->cfg.frames_per_wait, 1, &tmp)) break;
    if (out) *out = tmp;
    ABI_LEAVE();
    return HKSIM_OK;
}

/* INTERNAL (sim/core/lockstep.h): one frame of the kind the recording shows.  The recording decides where a step's
 * frames end, so a live frame never ends the step on the sim's own episode-end test. */
int hksim_frame(hksim *s, int kind, const int32_t action[4])
{
    if (!s || !s->pw || kind < HKSIM_FRAME_STEP || kind > HKSIM_FRAME_FROZEN) return HKSIM_ERR_BAD_ARG;
    if (kind == HKSIM_FRAME_STEP && !action) return HKSIM_ERR_BAD_ARG;
    ABI_ENTER(s);
    hksim_step_result tmp; memset(&tmp, 0, sizeof tmp);
    if (kind == HKSIM_FRAME_STEP) (void)step_frozen_frame(s, action, &tmp);
    else if (kind == HKSIM_FRAME_FROZEN) { lc_frozen_pre(s); lc_frozen_late(s, false); }
    else (void)step_live_frame(s, kind == HKSIM_FRAME_LIVE_LAST, 0, &tmp);
    ABI_LEAVE();
    return HKSIM_OK;
}

void hksim_get_parts(hksim *s, hksim_parts *p)
{
    memset(p, 0, sizeof *p);
    if (!s) return;
    p->arena = s->arena; p->hero = &s->H; p->rng = &s->rng; p->pw = s->pw; p->hero_body = s->hero_body;
    p->fsm_ctx = s->fsm_ctx; p->frame = &s->frame; p->fixed_count = &s->fixed_count; p->step = &s->step;
    p->time = &s->time; p->fixed_time = &s->fixed_time; p->tsll = &s->tsll; p->err = s->err; p->err_len = sizeof s->err;
}

/* ------------------------------------------------------------------------------- value injection */
static int find_field(const hero_field_desc *t, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++) if (strcmp(t[i].name, name) == 0) return (int)i;
    return -1;
}

int hksim_set_value(hksim *s, const char *key, double value)
{
    if (!s || !key || !s->pw) return HKSIM_ERR_BAD_ARG;
    uint32_t n;
    if (strncmp(key, "hero.f.", 7) == 0) {
        const hero_field_desc *t = hero_field_table(&n); int i = find_field(t, n, key + 7);
        if (i < 0) return HKSIM_ERR_BAD_ARG;
        char *base = (char *)&s->H.f + t[i].offset;
        if (t[i].code == 'f') *(float *)base = (float)value; else if (t[i].code == 'b') *(uint8_t *)base = value != 0; else *(int32_t *)base = (int32_t)value;
        return HKSIM_OK;
    }
    if (strncmp(key, "hero.pd.", 8) == 0) {
        const hero_field_desc *t = hero_pd_field_table(&n); int i = find_field(t, n, key + 8);
        if (i < 0) return HKSIM_ERR_BAD_ARG;
        char *base = (char *)&s->H.pd + t[i].offset;
        if (t[i].code == 'f') *(float *)base = (float)value; else if (t[i].code == 'b') *(uint8_t *)base = value != 0; else *(int32_t *)base = (int32_t)value;
        return HKSIM_OK;
    }
    if (strncmp(key, "hero.cstate.", 12) == 0) return hero_set_cstate(&s->H, key + 12, value != 0) ? HKSIM_OK : HKSIM_ERR_BAD_ARG;
    /* Re-take the hit/heal baseline from the knight's current health, after a caller set hero.pd.health at an
     * episode start: without it the next step reports the set as a hit or a heal (finish_step's dhp). */
    if (strcmp(key, "hp.resync") == 0) { s->hp_at_step_start = s->H.pd.health; return HKSIM_OK; }
    phys_v2 p = phys_body_position(s->pw, s->hero_body), v = phys_body_velocity(s->pw, s->hero_body);
    if (strcmp(key, "hero.rb_pos_x") == 0 || strcmp(key, "hero.pos_x") == 0) { p.x = (float)value; phys_body_set_position(s->pw, s->hero_body, p); return HKSIM_OK; }
    if (strcmp(key, "hero.rb_pos_y") == 0 || strcmp(key, "hero.pos_y") == 0) { p.y = (float)value; phys_body_set_position(s->pw, s->hero_body, p); return HKSIM_OK; }
    if (strcmp(key, "hero.rb_vel_x") == 0) { v.x = (float)value; phys_body_set_velocity(s->pw, s->hero_body, v); return HKSIM_OK; }
    if (strcmp(key, "hero.rb_vel_y") == 0) { v.y = (float)value; phys_body_set_velocity(s->pw, s->hero_body, v); return HKSIM_OK; }
    if (strcmp(key, "hero.rb_gravity") == 0) { phys_body_set_gravity_scale(s->pw, s->hero_body, (float)value); return HKSIM_OK; }
    if (strcmp(key, "hero.scale_x") == 0) { phys_body_set_scale_x(s->pw, s->hero_body, (float)value); return HKSIM_OK; }
    if (strcmp(key, "rng.s0") == 0) { s->rng.x = (uint32_t)value; return HKSIM_OK; }
    if (strcmp(key, "rng.s1") == 0) { s->rng.y = (uint32_t)value; return HKSIM_OK; }
    if (strcmp(key, "rng.s2") == 0) { s->rng.z = (uint32_t)value; return HKSIM_OK; }
    if (strcmp(key, "rng.s3") == 0) { s->rng.w = (uint32_t)value; return HKSIM_OK; }
    if (strcmp(key, "input") == 0) { hero_shim_set_keys(&s->H, (uint32_t)value); return HKSIM_OK; }
    if (strcmp(key, "frame") == 0) { s->frame = (uint32_t)value; return HKSIM_OK; }
    if (strcmp(key, "fixed_count") == 0) { s->fixed_count = (uint32_t)value; return HKSIM_OK; }
    if (strcmp(key, "time") == 0) { s->time = (float)value; return HKSIM_OK; }
    if (strcmp(key, "fixed_time") == 0) { s->fixed_time = (float)value; return HKSIM_OK; }
    if (strcmp(key, "tsll") == 0) { s->tsll = (float)value; return HKSIM_OK; }
    set_err(s, "hksim_set_value: unknown key %s", key);
    return HKSIM_ERR_BAD_ARG;
}

int hksim_get_value(hksim *s, const char *key, double *out)
{
    if (!s || !key || !out || !s->pw) return HKSIM_ERR_BAD_ARG;
    uint32_t n;
    if (strncmp(key, "hero.f.", 7) == 0) {
        const hero_field_desc *t = hero_field_table(&n); int i = find_field(t, n, key + 7);
        if (i < 0) return HKSIM_ERR_BAD_ARG;
        const char *base = (const char *)&s->H.f + t[i].offset;
        *out = t[i].code == 'f' ? *(const float *)base : t[i].code == 'b' ? (double)*(const uint8_t *)base : (double)*(const int32_t *)base;
        return HKSIM_OK;
    }
    if (strncmp(key, "hero.pd.", 8) == 0) {
        const hero_field_desc *t = hero_pd_field_table(&n); int i = find_field(t, n, key + 8);
        if (i < 0) return HKSIM_ERR_BAD_ARG;
        const char *base = (const char *)&s->H.pd + t[i].offset;
        *out = t[i].code == 'f' ? *(const float *)base : t[i].code == 'b' ? (double)*(const uint8_t *)base : (double)*(const int32_t *)base;
        return HKSIM_OK;
    }
    if (strncmp(key, "hero.cstate.", 12) == 0) { int v = hero_get_state(&s->H, key + 12); *out = v; return HKSIM_OK; }
    phys_v2 p = phys_body_position(s->pw, s->hero_body), v = phys_body_velocity(s->pw, s->hero_body);
    if (strcmp(key, "hero.rb_pos_x") == 0) { *out = p.x; return HKSIM_OK; }
    if (strcmp(key, "hero.rb_pos_y") == 0) { *out = p.y; return HKSIM_OK; }
    if (strcmp(key, "hero.rb_vel_x") == 0) { *out = v.x; return HKSIM_OK; }
    if (strcmp(key, "hero.rb_vel_y") == 0) { *out = v.y; return HKSIM_OK; }
    if (strcmp(key, "rng.s0") == 0) { *out = s->rng.x; return HKSIM_OK; }
    if (strcmp(key, "rng.s3") == 0) { *out = s->rng.w; return HKSIM_OK; }
    if (strcmp(key, "frame") == 0) { *out = s->frame; return HKSIM_OK; }
    if (strcmp(key, "fsm.n_gos") == 0) { *out = fsmi_n_gos(s->fsm_ctx); return HKSIM_OK; }
    if (strncmp(key, "prof.", 5) == 0) {
        for (int i = 0; i < PROF_N; i++) if (strcmp(key + 5, PROF_NAMES[i]) == 0) { *out = s->prof[i]; return HKSIM_OK; }
        if (strcmp(key + 5, "steps") == 0) { *out = s->step; return HKSIM_OK; }
    }
    return HKSIM_ERR_BAD_ARG;
}

/* ------------------------------------------------------------------------------- observation */
static double field_get(const hero_field_desc *t, uint32_t n, const void *base_obj, const char *name, int *found)
{
    for (uint32_t i = 0; i < n; i++) if (strcmp(t[i].name, name) == 0) {
        const char *base = (const char *)base_obj + t[i].offset;
        *found = 1;
        return t[i].code == 'f' ? *(const float *)base : t[i].code == 'b' ? (double)*(const uint8_t *)base : (double)*(const int32_t *)base;
    }
    *found = 0; return 0.0;
}
static int32_t pd_i32(hksim *s, const char *name)
{
    uint32_t n; const hero_field_desc *t = hero_pd_field_table(&n); int f;
    double v = field_get(t, n, &s->H.pd, name, &f);
    if (!f) HKSIM_UNIMPLEMENTED("PlayerData.%s is not modelled by sim/hero (needed by StateExtractor)", name);
    return (int32_t)v;
}

static void fill_hero_view(hksim *s, obs_hero_view *v)
{
    /* cite: oracle/Game/StateExtractor.cs (SE) and HitboxObserver.cs (HO) via analysis/specs/obs-wire.md §2-3 */
    phys_v2 p = phys_body_position(s->pw, s->hero_body), vel = phys_body_velocity(s->pw, s->hero_body);
    v->knightPos_x = p.x; v->knightPos_y = p.y;
    obs_box_bounds_size(p.x, p.y, s->H.col_offset.x, s->H.col_offset.y, s->H.col_size.x, s->H.col_size.y, &v->knightW, &v->knightH);
    v->rb2d_velocity_x = vel.x; v->rb2d_velocity_y = vel.y;
    v->pd_health = pd_i32(s, "health");
    v->pd_MPCharge = pd_i32(s, "MPCharge");
    v->pd_hasDash = pd_i32(s, "hasDash") != 0; v->pd_canWallJump = pd_i32(s, "canWallJump") != 0;
    v->pd_hasDoubleJump = pd_i32(s, "hasDoubleJump") != 0; v->pd_hasSuperDash = pd_i32(s, "hasSuperDash") != 0;
    v->pd_hasDreamNail = pd_i32(s, "hasDreamNail") != 0; v->pd_hasAcidArmour = pd_i32(s, "hasAcidArmour") != 0;
    v->pd_hasNailArt = pd_i32(s, "hasNailArt") != 0;
    /* SE:50-58 evaluation order; CanJump has a side effect (ledgeBufferSteps = 0) */
    v->CanJump = hero_can_jump(&s->H) != 0; v->CanDoubleJump = hero_can_double_jump(&s->H) != 0;
    v->CanWallJump = hero_can_wall_jump(&s->H) != 0; v->CanDash = hero_can_dash(&s->H) != 0;
    v->CanAttack = hero_can_attack(&s->H) != 0; v->CanCast = hero_can_cast(&s->H) != 0;
    v->CanNailCharge = hero_can_nail_charge(&s->H) != 0; v->CanDreamNail = hero_can_dream_nail(&s->H) != 0;
    v->CanSuperDash = hero_can_super_dash(&s->H) != 0;
    v->shim_CState = (uint8_t)s->H.in.CState; v->shim_LockedAction = s->H.in.LockedAction;
    v->shim_LockedStepsLeft = s->H.in.LockedStepsLeft; v->shim_LockedStepsTotal = s->H.in.LockedStepsTotal;
    v->cState_shadowDashing = s->H.cs.shadowDashing;
    v->damageMode_hazardOnly = s->H.f.damageMode == DM_HAZARD_ONLY;
    v->parryInvulnTimer = s->H.f.parryInvulnTimer;
}

/* The same obs_view the packer would serialise, kept as numbers (hksim.h fast path).  Reusing the
 * view rather than re-deriving anything is what makes the two paths agree by construction; the only
 * work not shared with obs_pack_step is the terrain expansion, which the packer does internally. */
static void fill_numeric(hksim *s, const obs_view *v, int is_reset, const hksim_step_result *r)
{
    obs_numeric *nb = &s->nb;
    /* Done step: TrainingEnv.cs:735-746 sends empty combat and terrain lists and 33 zero globals, exactly as
     * pack_body does for the wire (obs.c); the fast path must agree. */
    const int done = !is_reset && r && r->done;
    uint32_t n = done ? 0u : v->n_combat > NB_MAX_COMBAT ? NB_MAX_COMBAT : v->n_combat;
    nb->n_combat = (int32_t)n;
    for (uint32_t i = 0; i < n; i++) {
        memcpy(nb->combat[i], &v->combat[i].relX, NB_COMBAT_FEAT * sizeof(float));   /* obs_combat_row is 14 floats then the two strings */
        nb->kind[i] = v->combat[i].kind;
        nb->parent[i] = v->combat[i].clipKey;
    }
    if (done) memset(nb->global_state, 0, sizeof nb->global_state);
    else obs_global_state(&v->hero, nb->global_state);
    /* obs_terrain_row carries two provenance uint32s after its 8 floats, so the rows are expanded
     * into a scratch array (kept across steps -- terrain row count is constant per scene) and their
     * numeric prefix copied out.  Both buffers grow only on the first step of a scene. */
    nb->n_terrain = 0;
    if (v->scene && !done) {
        uint32_t want = obs_terrain_rows_live(v->scene, v->terrain_active, v->terrain_dyn, v->hero.knightPos_x, v->hero.knightPos_y, NULL, 0);
        if ((int32_t)want > nb->terrain_cap) {
            float *p = realloc(nb->terrain, (size_t)want * NB_TERRAIN_FEAT * sizeof(float));
            obs_terrain_row *q = realloc(s->tr_scratch, (size_t)want * sizeof *q);
            if (!p || !q) { if (p) nb->terrain = p; if (q) s->tr_scratch = q; return; }   /* OOM: leave n_terrain 0 */
            nb->terrain = p; s->tr_scratch = q; nb->terrain_cap = (int32_t)want;
        }
        if (nb->terrain_cap > 0) {
            obs_terrain_rows_live(v->scene, v->terrain_active, v->terrain_dyn, v->hero.knightPos_x, v->hero.knightPos_y, s->tr_scratch, want);
            for (uint32_t i = 0; i < want; i++)
                memcpy(nb->terrain + (size_t)i * NB_TERRAIN_FEAT, &s->tr_scratch[i].mx, NB_TERRAIN_FEAT * sizeof(float));
            nb->n_terrain = (int32_t)want;
        }
    }
    if (!is_reset && r) {
        nb->step[0] = r->damage_landed; nb->step[1] = r->hits_taken; nb->step[2] = r->hp_healed;
        nb->done = r->done != 0;
    } else {
        nb->step[0] = nb->step[1] = nb->step[2] = 0.0f;
        nb->done = 0;
    }
    nb->valid = 1;
}

static void build_obs(hksim *s, int is_reset, const hksim_step_result *r)
{
    obs_view v; memset(&v, 0, sizeof v);
    fill_hero_view(s, &v.hero);
    /* combat rows from the FSM module (HitboxObserver Enemy + Attack buckets), filled through obs_combat_fill */
    v.combat = s->obs_rows; v.n_combat = 0;
    uint32_t n = fsmi_combat_sources(s->fsm_ctx, s->obs_srcs, s->obs_kinds, s->obs_clips, s->obs_cap_c, &v.hero);
    if (n > OBS_WIRE_U16_MAX)
        HKSIM_UNIMPLEMENTED("%u combat rows: BinaryProtocol writes the count as a u16 (BinaryProtocol.cs:48)", n);
    if (n > s->obs_cap_c) {
        /* The pass counted every row but filled only obs_cap_c of them: grow the scratch and fill it again. */
        s->obs_srcs = realloc(s->obs_srcs, (size_t)n * sizeof *s->obs_srcs);
        s->obs_rows = realloc(s->obs_rows, (size_t)n * sizeof *s->obs_rows);
        s->obs_kinds = realloc(s->obs_kinds, (size_t)n * sizeof *s->obs_kinds);
        s->obs_clips = realloc(s->obs_clips, (size_t)n * sizeof *s->obs_clips);
        HKSIM_ASSERT(s->obs_srcs && s->obs_rows && s->obs_kinds && s->obs_clips, "observation scratch: out of memory");
        s->obs_cap_c = n;
        v.combat = s->obs_rows;
        /* world_combat_sources returns an over-cap count BEFORE touching any state (observer.c), so this
         * second pass is the only one that advances the motion tick. */
        n = fsmi_combat_sources(s->fsm_ctx, s->obs_srcs, s->obs_kinds, s->obs_clips, s->obs_cap_c, &v.hero);
        HKSIM_ASSERT(n <= s->obs_cap_c, "combat row count changed between two passes of one frame");
    }
    for (uint32_t i = 0; i < n; i++) {
        obs_combat_fill(&s->obs_rows[i], &s->obs_srcs[i], &v.hero);
        s->obs_rows[i].kind = s->obs_kinds[i]; s->obs_rows[i].clipKey = s->obs_clips[i];
    }
    v.n_combat = n;
    v.scene = s->scene; v.terrain_active = NULL; v.eval_mode = 0;
    /* The Terrain bucket as the game reads it at this instant (HitboxObserver.cs:740-742, 803-822): membership and
     * isActiveAndEnabled from the FSM world, and dynamic-body terrain at its live pose. */
    obs_terrain_dyn_live dl;
    const hk_scene_def *sc = s->scene;
    uint32_t nd = sc->terrain_dyn ? sc->n_terrain_dyn : 0;
    for (uint32_t i = 0; i < sc->n_statics; i++) s->ter_static_active[i] = sc->statics[i].active;
    for (uint32_t j = 0; j < nd; j++) s->ter_dyn_active[j] = sc->terrain_dyn[j].active;
    fsmi_terrain_live(s->fsm_ctx, s->ter_static_iid, sc->n_statics, s->ter_static_active,
                         s->ter_dyn_iid, nd, s->ter_dyn_active, s->ter_dyn_pts, s->ter_dyn_off, s->ter_dyn_n, s->ter_dyn_pts_cap);
    dl.active = s->ter_dyn_active; dl.wpts = s->ter_dyn_pts; dl.off = s->ter_dyn_off; dl.n = s->ter_dyn_n;
    v.terrain_active = s->ter_static_active; v.terrain_dyn = &dl;
    v.fsm_snapshots = s->obs_snaps; v.n_fsm = 0;
    /* FSM snapshots are a WIRE-only block (obs-wire.md 5.3) and expensive to build, so skipped under
     * HKSIM_OBS_BATCH alone. */
    if (s->obs_mode & HKSIM_OBS_WIRE) {
        uint32_t n = fsmi_fsm_snapshots(s->fsm_ctx, s->obs_snaps, s->obs_cap_f);
        if (n > s->obs_cap_f) {   /* world_fsm_snapshots is a pure read: grow and take it again */
            s->obs_snaps = realloc(s->obs_snaps, (size_t)n * sizeof *s->obs_snaps);
            HKSIM_ASSERT(s->obs_snaps != NULL, "observation scratch: out of memory");
            s->obs_cap_f = n;
            n = fsmi_fsm_snapshots(s->fsm_ctx, s->obs_snaps, s->obs_cap_f);
        }
        /* BinaryProtocol.cs:147-150 writes min(count, 65535) entries */
        v.fsm_snapshots = s->obs_snaps; v.n_fsm = n > OBS_WIRE_U16_MAX ? OBS_WIRE_U16_MAX : n;
    }
    if (!is_reset && r) {
        v.step.damage_landed = r->damage_landed; v.step.hits_taken = (int32_t)r->hits_taken;
        v.step.step_game_time = s->step_game_time; v.step.step_real_time = 0.0f;
        v.step.hp_healed = r->hp_healed; v.step.done = r->done != 0; v.step.action_committed = s->last_committed != 0;
        v.step.info = !s->episode_done ? "" : epend_info(s->episode_end_kind);   /* TrainingEnv.cs:655-690, :743-744 */
    } else {
        /* TrainingEnv.cs:370-415: the branch names where the reset found the game.  The first reset of a process
         * starts in GG_Workshop (WORKSHOP); every later one arrives with time frozen in the arena the previous
         * episode ran in, so preScene != GG_Workshop and the NATURAL_END branch is taken.  Fake resets
         * (_resetBranch = 2) need _fakeResetProb > 0 and are not modelled. */
        v.reset.reset_branch = s->reset_index <= 1 ? OBS_RESET_BRANCH_WORKSHOP : OBS_RESET_BRANCH_NATURAL_END;
    }
    if (s->obs_mode & HKSIM_OBS_BATCH) fill_numeric(s, &v, is_reset, r);
    if (!(s->obs_mode & HKSIM_OBS_WIRE)) { s->obs_len = 0; return; }
    size_t need = is_reset ? obs_pack_reset(&v, NULL, 0) : obs_pack_step(&v, NULL, 0);
    if (need > s->obs_cap) { s->obs_buf = realloc(s->obs_buf, need); s->obs_cap = need; }
    s->obs_len = is_reset ? obs_pack_reset(&v, s->obs_buf, s->obs_cap) : obs_pack_step(&v, s->obs_buf, s->obs_cap);
    if (s->trace_on) tw_obs(&s->trace, is_reset ? 0 : 1, s->reset_index, s->step, s->frame, s->obs_buf, (uint32_t)s->obs_len);   /* cite: r2 traces — reset_index is TrainingEnv.ResetCount after increment (first reset = 1) */
}

/* ----------------------------------------------------------------------------------------- trace */
size_t hksim_drain(hksim *s, uint8_t *buf, size_t cap)
{
    if (!s) return 0;
    size_t n = s->trace.len;
    if (!buf || cap < n) return n;
    memcpy(buf, s->trace.data, n);
    tw_clear(&s->trace);
    return n;
}

const char *hksim_trace_header(hksim *s)
{
    if (!s || s->header.len == 0) return "{}";
    return (const char *)s->header.data;
}

void *hksim_fsm_world(hksim *s) { return s && s->fsm_ctx ? fsmi_world(s->fsm_ctx) : NULL; }

size_t hksim_obs(hksim *s, uint8_t *buf, size_t cap)
{
    if (!s) return 0;
    if (!buf || cap < s->obs_len) return s->obs_len;
    memcpy(buf, s->obs_buf, s->obs_len);
    return s->obs_len;
}

void hksim_set_obs_mode(hksim *s, int32_t mode) { if (s) s->obs_mode = mode; }

int hksim_obs_batch(hksim *const *sims, int32_t n, hksim_vocab *vocab, hksim_batch *out)
{
    if (!out || (n > 0 && !sims)) { set_err(NULL, "hksim_obs_batch: null sims/out"); return HKSIM_ERR_BAD_ARG; }
    if (n < 0 || out->cap_combat < 0 || out->cap_terrain < 0) { set_err(NULL, "hksim_obs_batch: negative n/cap"); return HKSIM_ERR_BAD_ARG; }
    if (!vocab && (out->combat_kind || out->combat_parent)) {
        set_err(NULL, "hksim_obs_batch: combat_kind/combat_parent requested without a vocab");
        return HKSIM_ERR_BAD_ARG;
    }
    out->n_sims = n;
    for (int32_t i = 0; i < n; i++) {
        hksim *s = sims[i];
        if (!s) { set_err(NULL, "hksim_obs_batch: sims[%d] is NULL", i); return HKSIM_ERR_BAD_ARG; }
        if (!(s->obs_mode & HKSIM_OBS_BATCH)) {
            set_err(NULL, "hksim_obs_batch: sims[%d] was not built with HKSIM_OBS_BATCH (call hksim_set_obs_mode before reset)", i);
            return HKSIM_ERR_BAD_ARG;
        }
        if (!s->nb.valid) { set_err(NULL, "hksim_obs_batch: sims[%d] has no observation yet (reset it first)", i); return HKSIM_ERR_BAD_ARG; }
        nb_emit(&s->nb, out, i, vocab);
    }
    return HKSIM_OK;
}

hk_rng *hksim_rng(hksim *s) { return s ? &s->rng : NULL; }
void *hksim_fsm_ctx(hksim *s) { return s ? s->fsm_ctx : NULL; }
void hksim_bind_arena(hksim *s) { hks_arena_bind(s ? s->arena : NULL); }


