#pragma once
/* Unity's component lifecycle, as ONE model: every simulated MonoBehaviour instance is registered here with
 * its type, script execution order, the callbacks its type declares and its awake / enabled / started state,
 * and every FixedUpdate / Update / LateUpdate / Start / OnEnable / OnDisable / coroutine resume / Destroy the
 * simulator performs is dispatched from here.
 *
 * Spec: docs/engine-lifecycle.md (R0-R7), ported from Unity's player loop (hksim/analysis/native_specs/
 * native-playerloop.md, cited UP!<addr>).  Where neither reaches, the code names an assumption id (A-n) from
 * docs/engine-lifecycle.md.
 *
 * The player-loop stages (engine-lifecycle §1 "Stage names"), in frame order; the delayed stages are
 * DelayedCallManager passes over ONE queue of Starts, coroutine resumes, Destroys and the env coroutine:
 *   startup           EarlyUpdate/ScriptRunDelayedStartupFrame        pass, mask startup (Starts)
 *   fixed             FixedUpdate/ScriptRunBehaviourFixedUpdate       FixedUpdate, by execution order
 *   physics           FixedUpdate/Physics2DFixedUpdate                step + Enter/Stay/Exit callbacks (R5)
 *   fixed_delayed     FixedUpdate/ScriptRunDelayedFixedFrameRate      pass, mask fixed, clock fixed time
 *   update            Update/ScriptRunBehaviourUpdate                 Update, by execution order
 *   update_delayed    Update/ScriptRunDelayedDynamicFrameRate         pass, mask dynamic; stops at the env
 *   anim              PreLateUpdate/DirectorUpdateAnimationBegin+End  Animators advance, then write (mecanim.c)
 *   late              PreLateUpdate/ScriptRunBehaviourLateUpdate      LateUpdate, by execution order
 *   postlate_delayed  PostLateUpdate/ScriptRunDelayedDynamicFrameRate pass, mask dynamic
 *   end_of_frame      PostLateUpdate/TriggerEndOfFrameCallbacks       pass, mask end-of-frame (R6)
 * A frozen frame (timeScale 0) has no fixed / physics / fixed_delayed (R0). */
#include <stdint.h>
#include <stdbool.h>

#include "core/sim_modules.h"   /* LCS_* stages, LCB_* callbacks, LCF_* flags, hksim_lc_core */

struct fsm_world;
struct comp_def;

/* Simulated component types.  lifecycle.c holds the per-type table (recorder type name, execution order,
 * declared callbacks from the recorder's type_decl header). */
enum {
    LCT_FSM = 0,        /* PlayMakerFSM                         0 */
    LCT_PM_FIXED,       /* PlayMakerFixedUpdate (one per GameObject, AddComponent in PlayMakerFSM.Awake) 0 */
    LCT_PM_LATE,        /* PlayMakerLateUpdate  (idem)            0 */
    LCT_TK2D,           /* tk2dSpriteAnimator              -30095 */
    LCT_ITWEEN,         /* iTween (one component per tween)       0 */
    LCT_HM,             /* HealthManager                          0 */
    LCT_RECOIL,         /* Recoil                                 0 */
    LCT_CONSTRAIN,      /* ConstrainPosition                      0 */
    LCT_LSE,            /* LimitSendEvents                        0 */
    LCT_ARCY,           /* AutoRecycleSelf                        0 */
    LCT_GRIMMBALL,      /* GrimmballControl                       0 */
    LCT_INPUT,          /* InControl.InControlManager          -100  (core: hero_input_tick) */
    LCT_HERO,           /* HeroController                       208  (core: hero_fixed_update / hero_update) */
    LCT_NAILSLASH,      /* NailSlash                            500  (core: hero_slash_fixed_update) */
    LCT_HEROBOX,        /* HeroBox                              750  (core: hero_box_late_update) */
    LCT_DEACT2DTK,      /* DeactivateAfter2dtkAnimation           0 */
    LCT_RECYCLE2DTK,    /* RecycleAfter2dtkAnimation              0 */
    /* the MonoBehaviours sim/fsm/components/scripts.c ports (comp_def payload, gen_tables.py SCRIPTS), all order 0 */
    LCT_SCR_RANDOM_SCALE,       /* RandomScale */
    LCT_SCR_KEEP_SCALE_POSITIVE,/* KeepWorldScalePositive */
    LCT_SCR_DEACT_PD_TRUE,      /* DeactivateIfPlayerdataTrue */
    LCT_SCR_DEACT_PD_FALSE,     /* DeactivateIfPlayerdataFalse */
    LCT_SCR_DEACT_DELAY,        /* DeactivateAfterDelay */
    LCT_SCR_DISABLE_TIME,       /* DisableAfterTime */
    LCT_SCR_ENEMY_MESSAGE,      /* SendEnemyMessageTrigger */
    LCT_SCR_DREAM_REACTION,     /* EnemyDreamnailReaction */
    LCT_SCR_ENVIRO_REGION,      /* EnviroRegion */
    LCT_SCR_OBJECT_BOUNCE,      /* ObjectBounce */
    LCT_SCR_BREAKABLE,          /* Breakable */
    LCT_SCR_KEEP_WORLD_POS,     /* KeepWorldPosition */
    LCT_SCR_KEEP_ROTATION,      /* KeepRotation */
    LCT_SCR_HIVE_STINGER,       /* HiveKnightStinger */
    LCT_SCR_CORPSE,             /* Corpse */
    LCT_SCR_ENEMY_BULLET,       /* EnemyBullet */
    LCT_SCR_WALKER,             /* Walker */
    LCT_SCR_PARTICLE_AUTO_DISABLE,  /* ParticleSystemAutoDisable */
    LCT_SCR_CORPSE_BIT_END,     /* CorpseBitEnd */
    LCT_PROBE,          /* a conformance probe (tests only)       0 */
    LCT_N
};

/* A ported script's private fields (scripts.c says which it uses). */
typedef struct { float t; float v[6]; int32_t e[4]; int32_t n_e; uint8_t flag, state; } scr_state;

typedef hksim_lc_core lc_core;

typedef struct lc_state lc_state;

/* ---- world lifetime ---- */
void lc_create(struct fsm_world *w);                    /* registry for every component the tables carry */
void lc_destroy(struct fsm_world *w);
void lc_bind_core(struct fsm_world *w, const lc_core *core);
/* R5 (A-12): deliver, inside the current call, the callbacks of a collider disable or an object deactivation
 * (Collider2D::Cleanup(kColliderDisable) -> ProcessContacts(collider): phys_take_exit_events). */
void lc_physics_exit_on_disable(struct fsm_world *w);

/* ---- scene start (restore from the dump, R4 scene-load order) ---- */
void lc_scene_load(struct fsm_world *w);                /* OnEnable of everything active at the dump, in the
                                                         * dump-reconstructed enable order; per-component
                                                         * started state from the dump */

/* ---- the frame ---- */
void lc_stage(struct fsm_world *w, int stage, float dt, int flags);   /* run one player-loop stage */
void lc_stage_enter(struct fsm_world *w, int stage);    /* mark a stage whose body the core runs (physics) */
void lc_frame_begin(struct fsm_world *w, uint32_t frame, bool live);
void lc_frame_end(struct fsm_world *w);
void lc_set_late_gate(struct fsm_world *w, bool closed); /* FsmPauseGate for PlayMakerLateUpdate on the
                                                          * step's last live frame */
/* Time.time now and the per-run gap r = Time.time - Time.fixedTime (native-playerloop.md PL-2); lc_scene_load
 * sets (Time.time, 0) unless this was called first */
void lc_set_clock(struct fsm_world *w, double time, double residual);

/* ---- activation (R4) ---- */
/* go's activeSelf or parent just changed: recompute activeInHierarchy down the subtree and run the OnDisable /
 * Awake / OnEnable calls it implies (GameObject::ActivateAwakeRecursively) */
void lc_go_active_changed(struct fsm_world *w, int32_t go);
void lc_fsm_set_enabled(struct fsm_world *w, int32_t fsm, bool enabled);         /* PlayMakerFSM.enabled */

/* ---- ported scripts (sim/fsm/components/scripts.c) ---- */
/* The first component of script type `type` on `go`: its comp_def (NULL if none) and its private fields. */
const struct comp_def *lc_script_def(struct fsm_world *w, int32_t go, int type, scr_state **st);
/* scripts.c: the callbacks lifecycle.c dispatches to them, and the calls other code makes */
void scr_start(struct fsm_world *w, int type, int32_t go, const struct comp_def *d, scr_state *st);
void scr_on_enable(struct fsm_world *w, int type, int32_t go, const struct comp_def *d, scr_state *st);
void scr_update(struct fsm_world *w, int type, int32_t go, const struct comp_def *d, scr_state *st);
void scr_fixed_update(struct fsm_world *w, int type, int32_t go, const struct comp_def *d, scr_state *st);
/* the private fields the component held at the dump (lc_scene_load, once per component, before anything runs) */
void scr_restore(struct fsm_world *w, int type, int32_t go, const struct comp_def *d, scr_state *st);
/* Breakable.Hit (HitTaker.Hit): `attack_type` AttackTypes, `direction` HitInstance.Direction */
void scr_breakable_hit(struct fsm_world *w, int32_t go, int attack_type, float direction, float magnitude);
void scr_enemy_message_stay(struct fsm_world *w, int32_t go, int32_t other);   /* SendEnemyMessageTrigger.OnTriggerStay2D */
void scr_dream_impact(struct fsm_world *w, int32_t go);                         /* EnemyDreamnailReaction.RecieveDreamImpact */
void scr_enviro_region(struct fsm_world *w, int32_t go, bool enter);           /* EnviroRegion.OnTriggerEnter2D / Exit2D */
/* ObjectBounce.OnCollisionEnter2D; `normal` points from the other collider toward `go` (native-physics2d.md §6.5) */
void scr_object_bounce_enter(struct fsm_world *w, int32_t go, float nx, float ny, uint32_t contact_count);
void scr_object_bounce_set(struct fsm_world *w, int32_t go, bool bouncing);    /* ObjectBounce.StartBounce / StopBounce */
void scr_corpse_land(struct fsm_world *w, int32_t go);                        /* Corpse.OnCollisionEnter2D / OnCollisionStay2D */
/* EnemyBullet.Collision coroutine's synchronous half (:104-125; `do_rotation` false is OnTriggerEnter2D's
 * always-zero call) -- the wait/disable/recycle tail is scr_update (state machine in scr_state). */
void scr_enemy_bullet_impact(struct fsm_world *w, int32_t go, float nx, float ny, bool do_rotation);
/* Walker's public methods (hk.c StartWalker/StopWalker/SendEnemyMessage's "GO LEFT"/"GO RIGHT" branch) */
void scr_walker_start_moving(struct fsm_world *w, int32_t go);
void scr_walker_go(struct fsm_world *w, int32_t go, int32_t facing);
void scr_walker_receive_go(struct fsm_world *w, int32_t go, int32_t facing);
void scr_walker_stop(struct fsm_world *w, int32_t go);
void scr_walker_clear_turn_cooldown(struct fsm_world *w, int32_t go);

/* ---- runtime components ---- */
void lc_go_added(struct fsm_world *w, int32_t go);      /* register a runtime-Instantiated GameObject's components */
void lc_itween_added(struct fsm_world *w, int32_t itween_uid, int32_t go);      /* AddComponent<iTween> */
void lc_itween_removed(struct fsm_world *w, int32_t itween_uid);                /* Dispose -> Destroy(this) */

/* ---- Object.Destroy (R7): Behaviours of go and its direct children disabled now (delay <= 0), the destroy at
 * the first delayed pass whose clock reaches min(fixedTime, time) + delay ---- */
void lc_destroy_go(struct fsm_world *w, int32_t go, float delay, bool detach_children);

/* ---- coroutines (R6) ---- */
void lc_grimmball_fire(struct fsm_world *w, int32_t go);    /* DoFire started (first segment already run) */
void lc_grimmball_hit(struct fsm_world *w, int32_t go);     /* Shrink started (first segment already run) */

/* ---- queries ---- */
bool lc_started(struct fsm_world *w, int type, int32_t ref);

/* ---- scene start helpers (world_restore_scene) ---- */
bool lc_is_restored(struct fsm_world *w, int type, int32_t ref);
void lc_unrestore(struct fsm_world *w, int type, int32_t ref);   /* clear started / restored; Start again by R1 */
void lc_delayed_end(struct fsm_world *w);               /* the rest of update_delayed after the env coroutine */

/* ---- conformance probes (sim/fsm/runtime/conformance_api.c, tests/test_conformance.py) ---- */
enum { LCB_PROBE_CO = 18 };   /* a probe coroutine's resume (the recorder's COROUTINE code) */
enum { LC_YIELD_DONE = 0, LC_YIELD_NULL, LC_YIELD_FIXED, LC_YIELD_EOF, LC_YIELD_SECONDS };
/* cb: LCB_* or LCB_PROBE_CO (arg = the coroutine's tag); returns the coroutine's next yield, *wait its seconds */
typedef int (*lc_probe_fn)(void *ctx, int32_t comp, int cb, int stage, int32_t arg, float *wait);
enum { LC_PHYS_OFF = 0, LC_PHYS_ON, LC_PHYS_COLLIDERS_OFF };
/* the object's body and colliders join (LC_PHYS_ON) / leave (LC_PHYS_OFF), or its colliders are disabled
 * (LC_PHYS_COLLIDERS_OFF: Destroy's DisableBehaviours) */
typedef void (*lc_probe_phys_fn)(void *ctx, int32_t go, int mode);
void    lc_probe_hook(struct fsm_world *w, lc_probe_fn fn, lc_probe_phys_fn phys, void *ctx);
int32_t lc_probe_add(struct fsm_world *w, int32_t go, uint32_t lcb_mask, int32_t iid, bool enabled);
void    lc_probe_set_enabled(struct fsm_world *w, int32_t comp, bool enabled);
void    lc_probe_state(struct fsm_world *w, int32_t comp, int32_t out[5]);   /* enabled, in_lists, awake, started, dead */
void    lc_probe_coroutine(struct fsm_world *w, int32_t comp, int yield, float wait, int32_t tag);
void    lc_probe_stop_coroutines(struct fsm_world *w, int32_t comp);
void    lc_probe_destroy(struct fsm_world *w, int32_t comp, float delay);   /* Object.Destroy(component, delay) */
