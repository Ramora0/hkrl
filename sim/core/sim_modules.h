#pragma once
/* The calls between sim/core/sim.c and the FSM world (sim/fsm/runtime/core_iface.c).  The core owns the clocks,
 * the RNG and the physics world; sim/fsm owns the GameObjects / FSMs / animators / HK components and, through the
 * component lifecycle (sim/fsm/runtime/lifecycle.c), WHEN every component -- the core's own included -- runs: a
 * frame is the player loop's stages in Unity's measured order (docs/engine-lifecycle.md). */
#include <stdint.h>
#include <stddef.h>
#include "hksim.h"
#include "rng.h"
#include "phys.h"
#include "tracew.h"

struct hero;                    /* sim/hero/hero.h */
struct hero_hooks;
struct obs_combat_src;          /* sim/obs/obs.h: per-collider observation source (bounds, HM state, anim) */
struct obs_hero_view;

/* ---- the component lifecycle (sim/fsm/lifecycle.h) ----
 * Player-loop stages, in frame order, and the core-owned component types the lifecycle dispatches back into
 * the core. */
enum {
    LCS_NONE = 0, LCS_LOAD, LCS_STARTUP, LCS_FIXED, LCS_PHYSICS, LCS_FIXED_DELAYED, LCS_UPDATE,
    LCS_UPDATE_DELAYED, LCS_ANIM, LCS_LATE, LCS_POSTLATE_DELAYED, LCS_END_OF_FRAME, LCS_OTHER, LCS_N
};
enum { LCT_EXT_INPUT = 11, LCT_EXT_HERO = 12, LCT_EXT_NAILSLASH = 13, LCT_EXT_HEROBOX = 14 };   /* == lifecycle.h LCT_* */
enum { LCB_AWAKE = 2, LCB_ONENABLE = 3, LCB_START = 4, LCB_FIXED = 5, LCB_UPDATE = 6, LCB_LATE = 7,
       LCB_ONDISABLE = 8, LCB_ONDESTROY = 9 };
enum { LCF_FROZEN = 1,          /* timeScale == 0 during this stage: FsmPauseGate closes the PlayMaker ticks */
       LCF_SKIP_RESTORED = 2 }; /* the dump frame's tail at scene start: restored components are not ticked */
typedef struct hksim_lc_core {
    void *ctx;
    void (*tick)(void *ctx, int type, int callback);             /* LCT_EXT_* x LCB_* */
    void (*coroutines)(void *ctx, int stage);                     /* the core's coroutine resumes in `stage` */
    void (*phys_dispatch)(void *ctx, const phys_event *ev);
} hksim_lc_core;

/* Build the scene's GameObjects/FSMs from the compiled tables, create the bodies/shapes they own in `pw`, fill
 * `hooks_out` (every hero hook).  Returns 0, or HKSIM_ERR_UNKNOWN_LEVEL with `err` filled. */
int  fsmi_create(void **ctx, const char *level, hk_rng *rng, phys_world *pw, struct hero *hero,
                 struct hero_hooks *hooks_out, char *err, size_t errlen);
void fsmi_destroy(void *ctx);
/* The FSM world behind a context (struct fsm_world, sim/fsm/fsm.h): hksim_fsm_world and the method oracle's
 * test entry points. */
struct fsm_world *fsmi_world(void *ctx);
/* Restore every component from the SceneReady dump (the episode's first instant); `time` is Time.time then. */
void fsmi_scene_start(void *ctx, float time);
/* Trace emission: the ENTITY block (docs/trace-format.md FRAME) and the FSM_TRANSITION / FSM_EVENT /
 * ENEMY_DAMAGE / HERO_DAMAGE events accumulated since the last call, in occurrence order. */
void fsmi_emit_entities(void *ctx, tw_buf *b);
void fsmi_emit_events(void *ctx, tw_buf *b, uint32_t frame, uint32_t fixed_count);
void fsmi_set_record(void *ctx, int on);   /* record the event log fsmi_emit_events drains (tracing only) */
/* 1 when the fight is over the way TrainingEnv decides it, 2 when the arena was left, else 0. */
int  fsmi_boss_dead(void *ctx);
/* GameObjects in the FSM world: the scene's, plus every clone a pool Instantiated at runtime. */
int32_t fsmi_n_gos(void *ctx);
/* TrainingEnv._damageLandedInStep for the step just run, read-and-cleared: the sum over hits of
 * DamageDealt / (n * maxHP) * 100 with n = the number of boss HealthManagers (TrainingEnv.cs:1163). */
float fsmi_take_damage_landed(void *ctx);
/* Every phys_event whose body/shape `user` handle belongs to the FSM world (>= HKSIM_USER_FSM_BASE) or to a
 * core static collider (>= HKSIM_USER_STATIC_BASE, mapped back to its GameObject). */
void fsmi_on_phys_event(void *ctx, const phys_event *e);
/* PhysicsManager2D::Simulate's body -> Transform write-back, after phys_step and before its events. */
void fsmi_after_physics_step(void *ctx);
/* The combat rows HitboxObserver would emit this step (analysis/specs/obs-wire.md §3): every
 * isActiveAndEnabled Collider2D in the Enemy bucket then the Attack bucket, with its `kind` string and clip
 * key.  Strings are interned.  Returns the row count, which may exceed cap (the caller re-asks). */
uint32_t fsmi_combat_sources(void *ctx, struct obs_combat_src *out, const char **kinds, const char **clip_keys,
                             uint32_t cap, const struct obs_hero_view *hero);
/* FsmObserver.Snapshot strings "<src>|<owner>|<fsm>|<state>" in FsmObserver order (obs-wire.md §3.7). */
uint32_t fsmi_fsm_snapshots(void *ctx, const char **out, uint32_t cap);
/* The Knight's child attack colliders (NailSlash polygons, Clash Tink, HeroBox) ride the hero's Rigidbody2D:
 * the core passes the hero body so the module creates those shapes on it. */
void fsmi_bind_hero_body(void *ctx, phys_body_id hero_body);
/* Static scene colliders (no Rigidbody2D) are created by the CORE (user HKSIM_USER_STATIC_BASE + i); this hands
 * the module the (Collider2D.instanceID -> body, shape) table so each binds to the GameObject that owns it
 * (a DamageHero on a static collider still hurts the knight).  Entries the core skipped are 0. */
void fsmi_bind_statics(void *ctx, const int32_t *instance_ids, const phys_body_id *bodies,
                       const phys_shape_id *shapes, uint32_t n);
void fsmi_slash_set_enabled(void *ctx, int slash, int poly_on, int clash_tink_on);   /* NailSlash.FixedUpdate */
/* The Knight's tk2dSpriteAnimator for the FRAME hero ANIM sub-block: clip ("" if none), CurrentFrame,
 * ClipTimeSeconds, Playing, clip fps. */
void fsmi_hero_anim(void *ctx, const char **clip, int32_t *frame, float *clip_time, int *playing, float *fps);
/* The lifecycle:
 *   lc_bind       hand the module the core callbacks (after create)
 *   lc_frame      begin (1) / end (0) of a frame; live = a fixed step runs this frame; time = the frame's Time.time
 *   lc_stage      run one stage; LCS_PHYSICS only marks the stage (the core steps physics itself)
 *   lc_late_gate  FsmPauseGate on PlayMakerLateUpdate for this frame's LateUpdate (step's last live frame)
 *   lc_delayed_end the rest of the update_delayed pass after the env coroutine (FRAME/OBS) */
void fsmi_lc_bind(void *ctx, const hksim_lc_core *core);
void fsmi_lc_frame(void *ctx, int begin, uint32_t frame, int live, float time);
void fsmi_lc_stage(void *ctx, int stage, float dt, int flags);
void fsmi_lc_late_gate(void *ctx, int closed);
void fsmi_lc_delayed_end(void *ctx);
/* The observer's live Terrain bucket (oracle/Game/HitboxObserver.cs:740-742, 803-822): per core static collider
 * (by Collider2D.instanceID) and per dynamic-body terrain collider, whether it is in the HitboxReader's bucket
 * and isActiveAndEnabled now, and each active dynamic one's world outline at dyn_pts + dyn_off[j] (count to
 * dyn_n[j]).  Entries the module does not know keep the value the caller put in (the SceneReady state). */
void fsmi_terrain_live(void *ctx, const int32_t *static_iids, uint32_t n_static, uint8_t *static_active,
                       const int32_t *dyn_iids, uint32_t n_dyn, uint8_t *dyn_active,
                       phys_v2 *dyn_pts, const uint32_t *dyn_off, uint32_t *dyn_n, uint32_t pts_cap);

enum { HKSIM_USER_NONE = 0, HKSIM_USER_HERO = 1, HKSIM_USER_HERO_SLASH0 = 2 /* +slash index, 5 slashes */,
       HKSIM_USER_STATIC_BASE = 0x1000 /* + static collider index */, HKSIM_USER_FSM_BASE = 0x10000 };
