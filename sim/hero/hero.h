#pragma once
/* HeroController (HC:), HeroControllerStates, HeroActions / InControl, oracle/Game/ProxyController.cs (InputDeviceShim
 * + ActionDecoder), NailSlash (NS:), HeroBox (HB:).  Design notes: analysis/specs/port-hero.md.
 * Physics goes through hero_phys_ops (mirrors sim/core/phys.h); PlayMaker / tk2d / GameManager effects through hero_hooks. */
#include <stdint.h>
#include "core/hksim.h"
#include "core/phys.h"
#include "core/rng.h"
#include "hero_fields.h"

/* cite: analysis/decomp/Assembly-CSharp/GlobalEnums/ActorStates.cs */
enum { AS_grounded = 0, AS_idle = 1, AS_running = 2, AS_airborne = 3, AS_wall_sliding = 4,
       AS_hard_landing = 5, AS_dash_landing = 6, AS_no_input = 7, AS_previous = 8 };
/* cite: analysis/decomp/Assembly-CSharp/GlobalEnums/HeroTransitionState.cs */
enum { HTS_WAITING_TO_TRANSITION = 0, HTS_EXITING_SCENE = 1, HTS_WAITING_TO_ENTER_LEVEL = 2,
       HTS_ENTERING_SCENE = 3, HTS_DROPPING_DOWN = 4 };
/* cite: analysis/decomp/Assembly-CSharp/GlobalEnums/DamageMode.cs */
enum { DM_FULL_DAMAGE = 0, DM_HAZARD_ONLY = 1, DM_NO_DAMAGE = 2 };
/* cite: analysis/decomp/Assembly-CSharp/GlobalEnums/CollisionSide.cs */
enum { CS_top = 0, CS_left = 1, CS_right = 2, CS_bottom = 3, CS_other = 4 };
/* cite: analysis/decomp/Assembly-CSharp/GlobalEnums/GatePosition.cs */
enum { GP_top = 0, GP_right = 1, GP_left = 2, GP_bottom = 3, GP_door = 4, GP_unknown = 5 };
/* cite: analysis/decomp/Assembly-CSharp/GlobalEnums/HazardType.cs */
enum { HZ_NON_HAZARD = 0, HZ_SPIKES = 1, HZ_ACID = 2, HZ_LAVA = 3, HZ_PIT = 4 };
/* cite: analysis/decomp/Assembly-CSharp/GlobalEnums/AttackDirection.cs */
enum { AD_normal = 0, AD_upward = 1, AD_downward = 2 };
/* cite: analysis/decomp/Assembly-CSharp/GlobalEnums/PhysLayers.cs */
enum { PL_TERRAIN = 8, PL_PLAYER = 9, PL_ENEMIES = 11, PL_HERO_ATTACK = 17, PL_INTERACTIVE_OBJECT = 19,
       PL_HERO_BOX = 20, PL_SOFT_TERRAIN = 25 };
/* NailSlash instances: HeroController.normalSlash/alternateSlash/upSlash/downSlash/wallSlash (HC:323-331) */
enum { SLASH_NORMAL = 0, SLASH_ALT = 1, SLASH_UP = 2, SLASH_DOWN = 3, SLASH_WALL = 4, SLASH_N = 5, SLASH_NONE = -1 };

/* ---- traced state ------------------------------------------------------------------------------------ */
typedef struct hero_state {
#define X(ct, cn, tn, code) ct cn;
    HERO_FIELDS(X)
#undef X
    /* non-primitive HeroController fields (Vector2 / arrays; not on the trace wire) */
    phys_v2 current_velocity;                 /* HC:235 */
    phys_v2 slashOffset, upSlashOffset, downwardSlashOffset, spell1Offset;   /* HC:241-247 */
    phys_v2 transition_vel;                   /* HC:265 */
    phys_v2 recoilVector;                     /* HC:472 */
    phys_v2 lastInputState;                   /* HC:474 */
    phys_v2 positionHistory[2];               /* HC:613 */
    phys_v2 oldPos;                           /* HC:723 */
} hero_state;

typedef struct hero_cstate {
#define X(n) uint8_t n;
    HERO_CSTATE(X)
#undef X
} hero_cstate;

/* `hero->pd` is the world's only PlayerData store; other modules access it by name via hero_pd_field_table().
 * Health/soul changes that go through HeroController in the game must use the hero_* calls below so the
 * ProxyFSM / soul-orb events fire. */
typedef struct hero_pd {
#define X(ct, cn, tn, code) ct cn;
    HERO_PD_FIELDS(X)
#undef X
} hero_pd;

/* {name, code, offset} tables so the core trace writer emits fields by their real names (docs/trace-format.md). */
typedef struct { const char *name; char code; uint32_t offset; } hero_field_desc;
HKSIM_API const hero_field_desc *hero_field_table(uint32_t *count);      /* offsets into hero_state */
HKSIM_API const char *const   *hero_cstate_names(uint32_t *count);        /* bit i = field i */
HKSIM_API const hero_field_desc *hero_pd_field_table(uint32_t *count);   /* offsets into hero_pd */
HKSIM_API uint32_t hero_sizeof(void);                                     /* sizeof(hero) for ctypes callers */
HKSIM_API int32_t  hero_offsetof(const char *member);                     /* offsetof(hero, member) by name, -1 unknown */

/* ---- InControl model (device controls -> PlayerActions -> moveVector) ---------------------------------- */
typedef struct { uint8_t State; float Value; float RawValue; } ic_state;             /* InputControlState.cs */
typedef struct {                                                                       /* OneAxisInputControl.cs */
    ic_state lastState, thisState, nextState;
    uint64_t pendingTick;
    float    stateThreshold, lowerDeadZone, upperDeadZone;
    uint8_t  Raw;
} ic_control;

/* device controls the shim drives (oracle/Game/ProxyController.cs:67-77, :104-113) */
enum { DEV_DPAD_UP = 0, DEV_DPAD_DOWN, DEV_DPAD_LEFT, DEV_DPAD_RIGHT, DEV_ACTION1, DEV_RIGHT_BUMPER, DEV_ACTION3,
       DEV_RIGHT_TRIGGER, DEV_ACTION4, DEV_LEFT_TRIGGER,
       DEV_ACTION2,   /* PC:72 AddControl(Action2, "Cast"), driven by the focus key (PC:103) */
       DEV_N };
/* HeroActions (analysis/decomp/Assembly-CSharp/HeroActions.cs:61-103), creation order */
enum { PA_LEFT = 0, PA_RIGHT, PA_UP, PA_DOWN, PA_RS_UP, PA_RS_DOWN, PA_RS_LEFT, PA_RS_RIGHT, PA_JUMP, PA_ATTACK,
       PA_EVADE, PA_DASH, PA_SUPERDASH, PA_DREAMNAIL, PA_CAST, PA_FOCUS, PA_QUICKMAP, PA_QUICKCAST,
       PA_OPEN_INVENTORY,   /* HeroActions.cs:100 openInventory (textSpeedup and skipCutscene, created before it, unused) */
       PA_N };
/* InputDeviceShim.KeyNames bit order (oracle/Game/ProxyController.cs:22-27) */
enum { KEY_LEFT = 0, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_JUMP, KEY_ATTACK, KEY_DASH, KEY_CAST, KEY_DREAM_NAIL,
       KEY_SUPER_DASH,
       KEY_FOCUS,     /* PC:31 bit 10: held by action 3, drives Action2 = HeroActions.cast (ControllerMapping.cs:16), the Focus path */
       KEY_N };
/* InputDeviceShim.CommitState (oracle/Game/ProxyController.cs:41) */
enum { COMMIT_IDLE = 0, COMMIT_LOCKED = 1, COMMIT_RELEASING = 2 };

typedef struct hero_input {
    uint64_t   tick;                 /* InputManager.currentTick */
    ic_control dev[DEV_N];
    ic_control dpad[4];              /* InputDevice.DPad (TwoAxisInputControl) Left,Right,Up,Down */
    float      dpad_x, dpad_y;
    ic_control pa[PA_N];
    ic_control mv[4];                /* HeroActions.moveVector Left,Right,Up,Down */
    float      mv_x, mv_y;           /* moveVector.X / .Y == .Vector */
    /* InputDeviceShim (oracle/Game/ProxyController.cs:9-57) */
    uint8_t    key[KEY_N];
    uint8_t    retapAttack, retapCast;
    int32_t    CState, LockedAction, LockedStepsLeft, LockedStepsTotal;
} hero_input;

/* ---- physics vtable (mirrors sim/core/phys.h; bound by the core) --------------------------------------- */
enum { HIT_TRIGGER = 1u << 0, HIT_STEEP_SLOPE = 1u << 1, HIT_NON_SLIDER = 1u << 2, HIT_NON_THUNKER_ACTIVE = 1u << 3 };
typedef struct { phys_v2 point; phys_v2 normal; uint32_t flags; uint32_t layer; } hero_hit;

/* (vectors are passed by pointer so a ctypes harness can implement the table; the core wraps phys_*) */
typedef struct hero_phys_ops {
    void   *ctx;
    void    (*get_pos)(void *ctx, phys_v2 *out);      /* rb2d.position == transform.position (R2: interpolation None) */
    void    (*set_pos)(void *ctx, const phys_v2 *p);
    void    (*get_vel)(void *ctx, phys_v2 *out);      /* rb2d.velocity */
    void    (*set_vel)(void *ctx, const phys_v2 *v);
    float   (*get_gravity)(void *ctx);                /* rb2d.gravityScale */
    void    (*set_gravity)(void *ctx, float g);
    float   (*get_scale_x)(void *ctx);                /* transform.localScale.x */
    void    (*set_scale_x)(void *ctx, float sx);
    void    (*set_kinematic)(void *ctx, int on);      /* rb2d.isKinematic */
    void    (*set_layer)(void *ctx, int layer);       /* gameObject.layer */
    int     (*raycast)(void *ctx, const phys_v2 *origin, const phys_v2 *dir, float len, uint32_t mask, hero_hit *out);
    int     (*boxcast)(void *ctx, const phys_v2 *origin, const phys_v2 *size, const phys_v2 *dir, float len, uint32_t mask, hero_hit *out);
    void    (*slash_set_enabled)(void *ctx, int slash, int poly_on, int clash_tink_on);   /* NailSlash colliders */
} hero_phys_ops;

/* ---- hooks into the FSM / animator / GameManager -------------------------------------------------------- */
enum { FSM_PROXY = 0, FSM_SUPERDASH, FSM_THORN_COUNTER, FSM_SPELL_CONTROL, FSM_DASH_BURST, FSM_DAMAGE_EFFECT,
       FSM_FALL_TRAIL, FSM_ORBIT_SHIELD, FSM_VIGNETTE, FSM_CAMERA_SHAKE, FSM_SOUL_ORB, FSM_SOUL_VESSEL,
       FSM_CAMERA_FADE, FSM_RUN_EFFECT, FSM_SHADOW_RECHARGE, FSM_N };

typedef struct hero_hooks {
    void *ctx;
    void (*fsm_send_event)(void *ctx, int fsm, const char *event);               /* PlayMakerFSM.SendEvent */
    void (*fsm_set_bool)(void *ctx, int fsm, const char *var, int value);        /* Fsm.Variables.FindFsmBool(..).Value = */
    void (*slash_fsm_set_direction)(void *ctx, int slash, float direction);      /* slashFsm "direction" (HC:1469-1494) */
    void (*slash_anim_play)(void *ctx, int slash, const char *anim, float scale_mul);   /* NailSlash.StartSlash tk2d */
    void (*anim_update_state)(void *ctx, int actor_state);                       /* HAC:103-118 */
    void (*anim_finished_dash)(void *ctx);                                       /* HAC:550-553 */
    void (*anim_stop_attack)(void *ctx);                                         /* HAC:555-561 */
    void (*anim_play_clip)(void *ctx, const char *clip);                         /* HAC:120-130 */
    void (*anim_set_play_landing)(void *ctx, int on);                            /* HC:4274 animCtrl.playLanding = */
    void (*anim_control)(void *ctx, int enable);                                 /* HAC:525-539 Stop/StartControl */
    void (*effect)(void *ctx, const char *name);                                 /* prefab spawn / SetActive (cosmetic) */
    void (*on_taken_damage)(void *ctx);                                          /* OnTakenDamage event (HC:1990-1993) */
    /* post-hook of HeroController.TakeDamage, where the oracle records EVENT HERO_DAMAGE (docs/trace-format.md
     * ev 2).  Fires on every call, including ones i-frames swallow. */
    void (*take_damage_post)(void *ctx, int32_t amount, int32_t hazard_type, int32_t hp_after);
    void (*on_death)(void *ctx);                                                 /* OnDeath event (HC:3701-3704) */
    void (*gm_player_dead)(void *ctx, float wait);                               /* gm.PlayerDead(DEATH_WAIT) */
    void (*gm_player_dead_from_hazard)(void *ctx);                               /* gm.PlayerDeadFromHazard(0) */
    void (*gm_hazard_reload)(void *ctx);                                         /* GM:710 PlayMakerFSM.BroadcastEvent("HAZARD RELOAD") */
    void (*hero_box_set_inactive)(void *ctx, int on);                            /* HeroBox.inactive = */
} hero_hooks;

/* ---- Knight tk2dSpriteAnimator as seen by HeroAnimationController --------------------------------------- */
typedef struct hero_anim_state {         /* FRAME hero ANIM sub-block (docs/trace-format.md): clip, CurrentFrame, ClipTimeSeconds, Playing, ClipFps */
    char    clip[64];
    int32_t frame;
    float   clip_time;
    uint8_t playing;
    float   clip_fps;
} hero_anim_state;
enum { HERO_ANIM_HANDLER_HERO = 1 };      /* owner id for animator.AnimationCompleted = HeroAnimationController.AnimationCompleteDelegate */
typedef struct hero_tk2d_ops {
    void *ctx;
    int         (*play)(void *ctx, const char *clip);                    /* animator.Play(name); 0 if the clip is missing (Play(null) path) */
    void        (*play_from_frame)(void *ctx, const char *clip, int frame);   /* animator.PlayFromFrame(name, frame) */
    void        (*stop)(void *ctx);                                      /* animator.Stop() */
    int         (*is_playing)(void *ctx, const char *clip);              /* animator.IsPlaying(name) */
    const char *(*current_clip)(void *ctx);                              /* animator.CurrentClip.name ("" if none) */
    float       (*clip_duration)(void *ctx, const char *clip);           /* frames.Length / fps, -1 if missing (HAC:568-581) */
    void        (*set_completed_handler)(void *ctx, int owner);          /* animator.AnimationCompleted = <owner's delegate> */
    void        (*current)(void *ctx, hero_anim_state *out);             /* for the FRAME anim block */
} hero_tk2d_ops;
/* HeroAnimationController fields (HAC:6-40) */
typedef struct hero_anim {
    uint8_t playLanding, playRunToIdle, playDashToIdle, playBackDashToIdleEnd;   /* HAC:15-21 */
    uint8_t wasAttacking, wasFacingRight, setEntryAnim, changedClipFromLastFrame, attackComplete;   /* HAC:23-32 */
    int32_t actorState, prevActorState, stateBeforeControl;   /* HAC:34-38 */
    uint8_t controlEnabled;                                    /* HAC:40 */
    uint8_t delegateInstalled;   /* animator.AnimationCompleted currently == AnimationCompleteDelegate (last writer wins) */
    uint8_t initial_clip_set;    /* primed Idle at SceneReady (dumps/GG_Hornet_1/physics.json#heroAnimator) */
} hero_anim;

/* NailSlash (analysis/decomp/Assembly-CSharp/NailSlash.cs) */
typedef struct hero_nailslash {
    float   slashAngle;              /* NS:20 (mirror of the slash FSM "direction" var) */
    uint8_t slashing;                /* NS:30 */
    int32_t stepCounter, polyCounter;/* NS:32,36 */
    uint8_t polyEnabled, clashTinkEnabled;   /* NS:34, NS:42 .enabled */
    uint8_t animCompleted;           /* NS:38 (set via hero_slash_anim_completed) */
    uint8_t longnail, mantis, fury;  /* NS:24-28 (set from the charm FSMs via SetLongnail/SetMantis/SetFury) */
    float   fsmDirection;            /* slashFsm.FsmVariables "direction" */
} hero_nailslash;

/* HeroBox (analysis/decomp/Assembly-CSharp/HeroBox.cs) */
typedef struct hero_box {
    uint8_t isHitBuffered;           /* HB:12 */
    int32_t damageDealt, hazardType, collisionSide;   /* HB:14-18 */
} hero_box;

/* coroutine equivalents.  Resume rule (Q-dmg-8 / port-hero.md 4): a coroutine that yields during frame F before F's
 * coroutine phase (for the resumes after the env: F's hero_coroutine_after_env) first resumes at F+1's; `skip` marks
 * "F's pass still pending". */
typedef struct hero_coroutines {
    uint8_t phase_ran;               /* this frame's hero_coroutine_phase already ran (cleared by hero_set_clock on a new frame) */
    uint8_t after_env_ran;           /* this frame's hero_coroutine_after_env already ran (cleared likewise) */
    uint8_t recoil_pending, recoil_skip;   /* StartRecoil parked at HC:3829 (`yield return StartCoroutine(FreezeMoment)`) */
    uint8_t invul_phase, invul_skip; /* 0 idle, 1 = WaitForSeconds(DAMAGE_FREEZE_DOWN), 2 = WaitForSeconds(duration) */
    double  invul_acc;               /* dynamic time since the wait's yield: Unity's Time is a double (hero_coroutine_after_env) */
    float   invul_duration;
    struct { uint8_t active, terrainHit, skip; float thunkTimer; int32_t attackDir; } thunk[4];  /* CheckForTerrainThunk */
    uint8_t die_pending, die_skip;   /* Die() parked at HC:3742 */
    uint8_t hazard_die_pending, hazard_die_skip;   /* DieFromHazard parked at HC:3777 */
    /* hazard respawn chain: gm.PlayerDeadFromHazard(0) (GM:702-712) + HC.HazardRespawn (HC:2794-2830).  Phases:
     * 1 = GM:707 WaitForSeconds(0), 2 = GM:709 WaitForSeconds(0.8f), 3 = HC:2812 WaitForEndOfFrame,
     * 4 = HC:2826 WaitForSeconds(clipDuration). */
    uint8_t hzr_phase, hzr_skip;
    /* lc: the lifecycle (sim/fsm/runtime/lifecycle.c) drives the WaitForSeconds resumes (the hazard-respawn chain,
     * Invulnerable) from Unity's resume points (hero_coroutine_after_env; hero_end_of_frame, phase 5 = parked at
     * WaitForEndOfFrame) instead of hero_coroutine_phase (docs/engine-lifecycle.md R6) */
    uint8_t lc;
    float   hzr_acc, hzr_clip_duration;
} hero_coroutines;

typedef struct hero {
    hero_state      f;
    hero_cstate     cs;
    hero_pd         pd;
    hero_input      in;
    hero_coroutines co;
    hero_nailslash  slash[SLASH_N];
    int32_t         slashComponent;      /* HC:665 (index into slash[] or SLASH_NONE) */
    hero_box        box;
    uint8_t         heroBox_inactive;    /* HeroBox.inactive (static) */
    int32_t         gameObject_layer;    /* Knight GameObject.layer */
    hero_phys_ops   ops;
    hero_hooks      hooks;
    hero_tk2d_ops   tk;                  /* Knight animator (bind with hero_bind_tk2d) */
    hero_anim       anim;                /* HeroAnimationController (sim/hero/hero_anim.c) */
    /* Unity clocks, owned by the core; hero_set_clock before each callback */
    uint32_t frameCount;                 /* Time.frameCount */
    float    deltaTime;                  /* Time.deltaTime (0 on the frozen frame, frame-order.md 3.2) */
    float    timeSinceLevelLoad;         /* Time.timeSinceLevelLoad */
    /* GameManager / BossSceneController reads */
    uint8_t  gm_isPaused;                /* GameManager.isPaused */
    uint8_t  gm_isGameplayScene;         /* GameManager.IsGameplayScene() */
    uint8_t  gm_startedOnThisScene;
    uint8_t  gm_mapZoneIsDreamOrGG;      /* gm.GetCurrentMapZone() in {DREAM_WORLD, GODS_GLORY} */
    uint8_t  bsc_isBossScene;            /* BossSceneController.IsBossScene */
    uint8_t  bsc_isTransitioning;        /* BossSceneController.IsTransitioning */
    int32_t  bsc_bossLevel;              /* BossSceneController.BossLevel */
    /* body collider geometry (dumps/GG_Hornet_1/physics.json#heroColliders[0]) */
    phys_v2  col_offset, col_size;
    float    col_edgeRadius;
    /* PlayerData.hazardRespawnLocation (HC:2807).  Off the PD X-macro, which is the trace/obs schema; per-scene
     * value set by gen_fields.py. */
    phys_v2  hazardRespawnLocation;
    /* GameObject.activeSelf mirrors read back by orig_Update (HC:5387-5411) */
    uint8_t  artChargeActive, artChargedActive;
    /* shared UnityEngine.Random stream (core-owned); draws: HC:1862-1898 (carefree shield), HC:4402 (thunk effect) */
    hk_rng  *rng;
    /* calls dropped because no hook is bound (diagnostics) */
    uint32_t dropped_fsm_events, dropped_effects, dropped_rng_draws, dropped_anim_calls;
    /* sum of damageAmount (> 0) reaching ModHooks.AfterTakeDamage (HC:1958), i.e. after HC:1833-1844 and
     * HC:1920-1925, before overcharm doubling; TrainingEnv.OnKnightDamaged (TrainingEnv.cs:1169-1177) sums it
     * into hits_taken.  The core reads and zeroes it once per agent step. */
    int32_t  after_take_damage_sum;
} hero;

/* ---- construction ----------------------------------------------------------------------------------- */
HKSIM_API void hero_init_from_dump(hero *h);                      /* GG_Hornet_1 hero.json / playerdata.json / physics.json */
HKSIM_API void hero_init_scene(hero *h, const char *scene);       /* the fields whose dumped value is NOT Hornet's (acceptingInput, ...) */
HKSIM_API void hero_bind(hero *h, const hero_phys_ops *ops, const hero_hooks *hooks);
HKSIM_API void hero_bind_tk2d(hero *h, const hero_tk2d_ops *tk);         /* the Knight tk2dSpriteAnimator */
HKSIM_API void hero_input_init(hero_input *in);                  /* InputManager/HeroActions defaults (called by init) */
HKSIM_API void hero_set_clock(hero *h, uint32_t frameCount, float deltaTime, float timeSinceLevelLoad);
HKSIM_API uint64_t hero_cstate_bits(const hero *h);               /* FRAME.cstate encoding */

/* ---- per-frame entry points (order: analysis/specs/frame-order.md 3.3-3.4, docs/frame-order.md "Regime R2")
 *   live frame (dt = 0.02):
 *     hero_set_clock(h, frameCount, 0.02f, tsll)           Time.deltaTime inside FixedUpdate == fixedDeltaTime (Q-hero-9)
 *     hero_fixed_update(h); hero_slash_fixed_update(h)      order Q-phero-4
 *     phys_step(...)  -> hero_on_collision_{enter,stay,exit}; HeroBox overlaps: hero_box_check_for_damage / hero_take_damage
 *     hero_input_tick(h)                                    once per rendered frame (Q-hero-12)
 *     [PlayMakerFSM.Update for most Knight FSMs]
 *     hero_update(h)
 *     hero_anim_update(h)                                   after HeroController.Update (frame-order.md 4; tk2d-animator.md 1.1)
 *     hero_coroutine_phase(h)                               before FRAME / obs emission (port-hero.md 4)
 *     [FRAME record / obs]   hero_coroutine_after_env(h) (lc)   then LateUpdate: hero_box_late_update(h)
 *   frozen frame (Time.timeScale = 0 -> dt = 0, no FixedUpdate, no phys_step):
 *     hero_set_clock(h, frameCount, 0.0f, tsll); hero_input_tick(h); hero_update(h); hero_anim_update(h); hero_coroutine_phase(h);
 *     then hero_apply_action(h, action, frames_per_wait)  (TrainingEnv.cs:596-601)
 *   Time.frameCount increments once per frame (live or frozen); Update10 keys off frameCount % 10 (HC:5108). */
HKSIM_API void hero_input_tick(hero *h);          /* one InputManager.UpdateInternal tick (before HeroController.Update) */
HKSIM_API void hero_input_set_direction_values(hero *h, int left, int right, int up, int down);   /* one tick's committed direction values (method oracle inputs) */
HKSIM_API void hero_update(hero *h);              /* HeroController.Update (HC:904) with h->deltaTime */
HKSIM_API void hero_fixed_update(hero *h);        /* HeroController.FixedUpdate (HC:910); h->deltaTime must be 0.02 */
HKSIM_API void hero_coroutine_phase(hero *h);     /* resumes parked coroutines; after hero_update, before FRAME/obs */
HKSIM_API void hero_coroutine_after_env(hero *h); /* (lc) the WaitForSeconds resumes, after FRAME/obs */
HKSIM_API void hero_end_of_frame(hero *h);        /* (lc) WaitForEndOfFrame resumes, end of the same frame */
HKSIM_API void hero_slash_fixed_update(hero *h);  /* NailSlash.FixedUpdate for the 5 slashes (NS:103-127) */
HKSIM_API void hero_box_late_update(hero *h);     /* HeroBox.LateUpdate (HB:358-364) */
/* HeroAnimationController (sim/hero/hero_anim.c, HAC:<n> = HeroAnimationController.cs:<n>) */
HKSIM_API void hero_anim_init(hero *h);                       /* Awake/Start state without the initial Play (arena: animator already on Idle) */
HKSIM_API void hero_anim_prime_scene_ready(hero *h);         /* Seed Idle clip at SceneReady (physics.json#heroAnimator) */
HKSIM_API void hero_anim_start(hero *h);                      /* HAC:49-69 Start */
HKSIM_API void hero_anim_update(hero *h);                     /* HAC:71-85 Update, once per rendered frame after hero_update */
HKSIM_API void hero_anim_on_completed(hero *h, const char *clip);   /* tk2d AnimationCompleted while the hero delegate is installed */
HKSIM_API void hero_anim_handler_overridden(hero *h);         /* another component assigned animator.AnimationCompleted (PlayMaker) */
HKSIM_API void hero_anim_update_state(hero *h, int actor_state);   /* HAC:103-118 (called by SetState / SetStartingMotionState) */
HKSIM_API void hero_anim_play_clip(hero *h, const char *clip);     /* HAC:120-130 */
HKSIM_API void hero_anim_play_idle(hero *h);                  /* HAC:456-485 */
HKSIM_API void hero_anim_stop_control(hero *h);               /* HAC:525-532 = HeroController.StopAnimationControl (HC:3127) */
HKSIM_API void hero_anim_start_control(hero *h);              /* HAC:534-539 = HeroController.StartAnimationControl (HC:3132) */
HKSIM_API void hero_anim_start_control_without_setting_state(hero *h);   /* HAC:541-548 */
HKSIM_API void hero_anim_control(hero *h, int enable);        /* HAC:525-539 StartControl / StopControl */
HKSIM_API void hero_anim_finished_dash(hero *h);              /* HAC:550-553 */
HKSIM_API void hero_anim_stop_attack(hero *h);                /* HAC:555-561 */
HKSIM_API void hero_anim_set_play_landing(hero *h, int on);   /* animCtrl.playLanding = (HC:4274) */
HKSIM_API float hero_anim_get_clip_duration(hero *h, const char *clip);   /* HAC:568-581 */
HKSIM_API int  hero_anim_current(hero *h, hero_anim_state *out);          /* FRAME anim block; 0 if no animator is bound */
/* HeroAudioController.PlaySound (analysis/decomp/Assembly-CSharp/HeroAudioController.cs:43-101): JUMP / WALLJUMP /
 * SOFT_LANDING call RandomizePitch (:180-184) = one UnityEngine.Random.Range(0.9f, 1.1f) draw on the shared stream. */
enum { HSND_SOFT_LANDING = 0, HSND_HARD_LANDING, HSND_JUMP, HSND_WALLJUMP, HSND_TAKE_HIT, HSND_DASH, HSND_WALLSLIDE,
       HSND_BACKDASH, HSND_FOOTSTEPS_RUN, HSND_FOOTSTEPS_WALK, HSND_NAIL_ART_CHARGE, HSND_NAIL_ART_READY, HSND_FALLING };
HKSIM_API void hero_audio_play_sound(hero *h, int sound);

/* physics callbacks (from phys_events after phys_step); other = the collider hit.  flags: marker components on
 * the other GameObject (analysis/decomp/Assembly-CSharp/NoHardLanding.cs, SteepSlope.cs, NonSlider.cs). */
enum { HERO_CONTACT_NO_HARD_LANDING = 1u << 0, HERO_CONTACT_STEEP_SLOPE = 1u << 1, HERO_CONTACT_NON_SLIDER = 1u << 2 };
typedef struct { phys_v2 normal; uint32_t layer; uint8_t tag_hero_walkable; uint32_t flags; } hero_contact;
HKSIM_API void hero_on_collision_enter(hero *h, const hero_contact *c);   /* HC:4841 */
HKSIM_API void hero_on_collision_stay(hero *h, const hero_contact *c);    /* HC:4906 */
HKSIM_API void hero_on_collision_exit(hero *h, const hero_contact *c);    /* HC:4961 */
/* HeroBox.OnTriggerEnter2D/Stay2D against a DamageHero (HB:302-351); damages_hero FSM path: hero_take_damage */
HKSIM_API void hero_box_check_for_damage(hero *h, float other_pos_x, int damageDealt, int hazardType, int shadowDashHazard,
                                         int has_damages_hero_fsm);
/* NailSlash.OnTriggerEnter2D (NS:129-133, :183-276): the slash collider touched something */
HKSIM_API void hero_slash_trigger(hero *h, int slash, int other_layer, int nonbouncer_active, int is_bounce_shroom, int is_big_bouncer);
HKSIM_API void hero_slash_anim_completed(hero *h, int slash);             /* NS:155-158 (tk2d AnimationCompleted) */

/* ---- input ------------------------------------------------------------------------------------------- */
HKSIM_API void     hero_shim_set_keys(hero *h, uint32_t bits);            /* raw KeyBits (replay) */
HKSIM_API uint32_t hero_shim_key_bits(const hero *h);                     /* InputDeviceShim.KeyBits() */
HKSIM_API void     hero_shim_reset(hero *h);                              /* InputDeviceShim.Reset + ResetCommit */
HKSIM_API int      hero_apply_action(hero *h, int32_t action[4], int framesPerWait);   /* ActionDecoder.ApplyAction */
HKSIM_API int      hero_pa_is_pressed(const hero *h, int pa);
HKSIM_API int      hero_pa_was_pressed(const hero *h, int pa);
HKSIM_API int      hero_pa_was_released(const hero *h, int pa);

/* ---- HeroController public surface used by PlayMaker / other components ----------------------------- */
HKSIM_API void hero_take_damage(hero *h, int damageSide, int damageAmount, int hazardType, float go_rot_z);
HKSIM_API int  hero_can_jump(hero *h);          /* NOTE: side effect ledgeBufferSteps = 0 (HC:4727) */
HKSIM_API int  hero_can_double_jump(const hero *h);
HKSIM_API int  hero_can_wall_jump(const hero *h);
HKSIM_API int  hero_can_dash(const hero *h);
HKSIM_API int  hero_can_attack(const hero *h);
HKSIM_API int  hero_can_cast(const hero *h);
HKSIM_API int  hero_can_nail_charge(const hero *h);
HKSIM_API int  hero_can_dream_nail(const hero *h);
HKSIM_API int  hero_can_super_dash(const hero *h);
HKSIM_API int  hero_can_focus(const hero *h);
HKSIM_API int  hero_can_nail_art(hero *h);      /* side effect nailChargeTimer = 0 (HC:2995,2998) */
HKSIM_API int  hero_can_back_dash(const hero *h);
HKSIM_API int  hero_can_quick_map(const hero *h);
HKSIM_API int  hero_can_inspect(const hero *h);
HKSIM_API int  hero_can_dream_gate(const hero *h);
HKSIM_API int  hero_can_interact(const hero *h);
HKSIM_API int  hero_can_open_inventory(const hero *h);
HKSIM_API int  hero_can_input(const hero *h);
HKSIM_API int  hero_can_talk(const hero *h);
HKSIM_API void hero_prevent_cast_by_dialogue_end(hero *h);   /* HC:2968-2970: gates hero_can_cast for 0.3s */
HKSIM_API int  hero_can_take_damage(const hero *h);
HKSIM_API int  hero_can_wall_slide(const hero *h);
HKSIM_API int  hero_check_touching_ground(const hero *h);
HKSIM_API int  hero_check_near_roof(const hero *h);
HKSIM_API int  hero_check_for_bump(const hero *h, int side);
HKSIM_API int  hero_get_state(const hero *h, const char *name);         /* GetState / GetCState */
HKSIM_API int  hero_set_cstate(hero *h, const char *name, int value);   /* SetCState; returns 0 if unknown name */

HKSIM_API void hero_face_right(hero *h);
HKSIM_API void hero_face_left(hero *h);
HKSIM_API void hero_flip_sprite(hero *h);
HKSIM_API void hero_affected_by_gravity(hero *h, int gravityApplies);
HKSIM_API void hero_relinquish_control(hero *h);
HKSIM_API void hero_relinquish_control_not_velocity(hero *h);
HKSIM_API void hero_regain_control(hero *h);
HKSIM_API void hero_ignore_input(hero *h);
HKSIM_API void hero_ignore_input_without_reset(hero *h);
HKSIM_API void hero_accept_input(hero *h);
HKSIM_API void hero_start_cyclone(hero *h);
HKSIM_API void hero_end_cyclone(hero *h);
HKSIM_API void hero_bounce(hero *h);
HKSIM_API void hero_bounce_high(hero *h);
HKSIM_API void hero_shroom_bounce(hero *h);
HKSIM_API void hero_recoil_left(hero *h);
HKSIM_API void hero_recoil_right(hero *h);
HKSIM_API void hero_recoil_left_long(hero *h);
HKSIM_API void hero_recoil_right_long(hero *h);
HKSIM_API void hero_recoil_down(hero *h);
HKSIM_API void hero_force_hard_landing(hero *h);
HKSIM_API void hero_reset_hard_landing_timer(hero *h);
HKSIM_API void hero_cancel_hero_jump(hero *h);
HKSIM_API void hero_cancel_super_dash(hero *h);   /* HC:2866-2869 superDash.SendEvent("SLOPE CANCEL") */
HKSIM_API void hero_cancel_attack_msg(hero *h);         /* SendMessage("CancelAttack") from nail_cancel_check */
HKSIM_API void hero_reset_air_moves(hero *h);
HKSIM_API void hero_set_back_on_ground(hero *h);
HKSIM_API void hero_is_swimming(hero *h);
HKSIM_API void hero_not_swimming(hero *h);
HKSIM_API void hero_set_walk_zone(hero *h, int inWalkZone);
HKSIM_API void hero_near_bench(hero *h, int isNearBench);
HKSIM_API void hero_set_conveyor_speed(hero *h, float speed);
HKSIM_API void hero_set_conveyor_speed_v(hero *h, float speed);
HKSIM_API void hero_set_darkness(hero *h, int darkness);
HKSIM_API void hero_set_start_with_wallslide(hero *h);
HKSIM_API void hero_set_start_with_jump(hero *h);
HKSIM_API void hero_set_start_with_full_jump(hero *h);
HKSIM_API void hero_set_start_with_dash(hero *h);
HKSIM_API void hero_set_start_with_attack(hero *h);
HKSIM_API void hero_set_super_dash_exit(hero *h);
HKSIM_API void hero_set_quake_exit(hero *h);
HKSIM_API void hero_set_take_no_damage(hero *h);
HKSIM_API void hero_end_take_no_damage(hero *h);
HKSIM_API void hero_enter_without_input(hero *h, int flag);
HKSIM_API void hero_set_damage_mode_int(hero *h, int invincibilityType);   /* SetDamageMode(int) / SetDamageModeFSM */
HKSIM_API void hero_set_damage_mode(hero *h, int newDamageMode);           /* SetDamageMode(DamageMode) */
HKSIM_API void hero_reset_quake_damage(hero *h);
HKSIM_API void hero_nail_parry(hero *h);
HKSIM_API void hero_nail_parry_recover(hero *h);
HKSIM_API void hero_quake_invuln(hero *h);
HKSIM_API void hero_cancel_parry_invuln(hero *h);
HKSIM_API void hero_cyclone_invuln(hero *h);
HKSIM_API void hero_pause(hero *h);
HKSIM_API void hero_unpause(hero *h);
HKSIM_API void hero_reset_state(hero *h);                /* ResetState -> cState.Reset() */
HKSIM_API void hero_charm_update(hero *h);
HKSIM_API void hero_start_mp_drain(hero *h, float time);
HKSIM_API void hero_stop_mp_drain(hero *h);
/* soul / health surface (HC:2070-2217), all update hero->pd exactly as PlayerData does (PD:4618-4636, :4779-4844):
 *   hero_add_mp_charge        AddMPCharge(int)        HC:2070  (Grubsong HC:1974-1978; FSM SendMessageV2 "AddMPCharge")
 *   hero_soul_gain            SoulGain()              HC:2081  (HealthManager.cs:459 on every nail hit)
 *   hero_add_mp_charge_spa    AddMPChargeSpa(int)     HC:2118  (Blessing Ghost FSM; not reachable in a fight)
 *   hero_try_add_mp_charge_spa TryAddMPChargeSpa(int) HC:2123
 *   hero_set_mp_charge        SetMPCharge(int)        HC:2135  (Spell Control grace checks)
 *   hero_take_mp / _quick     TakeMP / TakeMPQuick    HC:2141 / :2153 (spell cost, focus drain)
 *   hero_take_reserve_mp      TakeReserveMP(int)      HC:2165
 *   hero_clear_mp[_send_events]                       HC:2207 / :2212
 *   hero_add_health / hero_take_health / hero_max_health[_keep_blue]   HC:2171 / :2177 / :2183 / :2189
 *   AddToMaxHealth / AddToMaxMPReserve (HC:2197, :2219) are collectible pickups, unreachable in an arena: not ported. */
HKSIM_API void hero_add_mp_charge(hero *h, int amount);
HKSIM_API void hero_soul_gain(hero *h);
HKSIM_API void hero_add_mp_charge_spa(hero *h, int amount);
HKSIM_API int  hero_try_add_mp_charge_spa(hero *h, int amount);
HKSIM_API void hero_set_mp_charge(hero *h, int amount);
HKSIM_API void hero_take_mp(hero *h, int amount);
HKSIM_API void hero_take_mp_quick(hero *h, int amount);
HKSIM_API void hero_take_reserve_mp(hero *h, int amount);
HKSIM_API void hero_add_health(hero *h, int amount);
HKSIM_API void hero_take_health(hero *h, int amount);
HKSIM_API void hero_max_health(hero *h);
HKSIM_API void hero_max_health_keep_blue(hero *h);
HKSIM_API void hero_clear_mp(hero *h);
HKSIM_API void hero_clear_mp_send_events(hero *h);
HKSIM_API void hero_set_slash_longnail(hero *h, int slash, int set);
HKSIM_API void hero_slash_cancel_attack(hero *h, int slash);   /* NailSlash.CancelAttack NS:175-181 */
HKSIM_API void hero_set_slash_mantis(hero *h, int slash, int set);
HKSIM_API void hero_set_slash_fury(hero *h, int slash, int set);
HKSIM_API void hero_set_starting_motion_state(hero *h);
