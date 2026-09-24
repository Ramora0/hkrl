#pragma once
/* Observation packer — byte-identical to oracle/Net/BinaryProtocol.cs Pack() (verified obs-wire.md
 * preamble).  Design notes: analysis/specs/port-obs.md.
 *
 * This header is a plain-data VIEW that sim/core fills from the other modules; it never includes sim/hero
 * or sim/fsm headers.  Field names follow the C# source they transcribe.  Citation keys as in
 * analysis/specs/obs-wire.md: BP: = oracle/Net/BinaryProtocol.cs, PR: = oracle/Net/Protocol.cs,
 * SE: = oracle/Game/StateExtractor.cs, HO: = oracle/Game/HitboxObserver.cs, TE: = oracle/Environment/TrainingEnv.cs,
 * PC: = oracle/Game/ProxyController.cs.  Engine facts (no decomp) are tagged [ENGINE] with the obs-wire.md § that
 * evidences them.
 *
 * Terrain rows are NOT in the view: the packer derives them itself from the compiled scene tables
 * (sim/core/scene.h) + knightPos, per obs-wire.md §4 (HO:416-518, 803-822).
 */
#include <stdint.h>
#include <stddef.h>
#include "core/hksim.h"
#include "core/scene.h"   /* hk_scene_def / hk_static_collider: terrain geometry source */

enum {
    OBS_GLOBAL_DIM     = 33,   /* cite: SE:11 GlobalStateDim; obs-wire.md §2 */
    OBS_COMBAT_FEAT    = 14,   /* cite: HO:793-797; obs-wire.md §3.3 */
    OBS_TERRAIN_FEAT   = 8,    /* cite: HO:516; obs-wire.md §4.2 */
    OBS_RESET_PHASES   = 7,    /* cite: PR:100 ResetPhase.Count; keys PR:95-99 */
    OBS_COMMIT_ACTIONS = 8,    /* cite: SE:66 commitAction = new float[8] */
};

/* Message type ids.  cite: BP:9-15 (MSG_*), BP:17-29 (IdToType/TypeToId). */
enum { OBS_MSG_INIT = 0, OBS_MSG_RESET = 1, OBS_MSG_STEP = 2, OBS_MSG_ACTION = 3,
       OBS_MSG_PAUSE = 4, OBS_MSG_RESUME = 5, OBS_MSG_CLOSE = 6 };

/* InputDeviceShim.CommitState.  cite: PC:41 (Idle = 0, Locked = 1, Releasing = 2). */
enum { OBS_COMMIT_IDLE = 0, OBS_COMMIT_LOCKED = 1, OBS_COMMIT_RELEASING = 2 };

/* ResetPhase branch codes.  cite: PR:102-104. */
enum { OBS_RESET_BRANCH_WORKSHOP = 0, OBS_RESET_BRANCH_NATURAL_END = 1, OBS_RESET_BRANCH_UNKNOWN = 2 };

/* Everything StateExtractor.GetGlobalState (SE:30-96) and the Knight bucket of HitboxObserver.GetSplitFeatures
 * (HO:711, 746-750) read, sampled at the capture point of obs-wire.md §1.6 (coroutine resume of the step's
 * last live frame, after Update, before LateUpdate). */
typedef struct obs_hero_view {
    float   knightPos_x, knightPos_y;       /* HeroController.instance.transform.position (HO:711) — transform, not rb2d.position (obs-wire.md §3.2) */
    float   knightW, knightH;               /* bounds.size.x/.y of the last active non-trigger Knight-bucket collider (HO:746-750); 0.5 × 1.28125, edgeRadius excluded (obs-wire.md §2 idx 4-5) */
    float   rb2d_velocity_x, rb2d_velocity_y; /* HeroController.rb2d.velocity (SE:33-36; 0 if rb null) */
    int32_t pd_health;                      /* PlayerData.instance.health (SE:37) */
    int32_t pd_MPCharge;                    /* PlayerData.instance.MPCharge (SE:38) */
    uint8_t pd_hasDash, pd_canWallJump, pd_hasDoubleJump, pd_hasSuperDash,
            pd_hasDreamNail, pd_hasAcidArmour, pd_hasNailArt;   /* SE:41-47 (hasNailArt via pd.GetBool, obs-wire.md Q-obs-7) */
    uint8_t CanJump, CanDoubleJump, CanWallJump, CanDash, CanAttack, CanCast,
            CanNailCharge, CanDreamNail, CanSuperDash;          /* SE:50-58; exact conjunctions hero-motion.md §4; CanJump has a side effect (obs-wire.md §2) — evaluate as the call */
    uint8_t shim_CState;                    /* InputDeviceShim.CState (PC:42), OBS_COMMIT_* */
    int32_t shim_LockedAction;              /* PC:43 (-1 when idle) */
    int32_t shim_LockedStepsLeft;           /* PC:44 */
    int32_t shim_LockedStepsTotal;          /* PC:49 */
    /* The knight's hazard immunities HitboxObserver.GivesDamage reads (HeroBox.cs:59, HeroController.cs:1847) */
    uint8_t cState_shadowDashing;           /* HeroController.cState.shadowDashing */
    uint8_t damageMode_hazardOnly;          /* HeroController.damageMode == DamageMode.HAZARD_ONLY */
    float   parryInvulnTimer;               /* HeroController.parryInvulnTimer */
} obs_hero_view;

/* One Enemy/Attack-bucket row exactly as HO:793-797 builds it, in the order the caller supplies
 * (bucket order Enemy then Attack, within a bucket by scene order — obs-wire.md §3.8 / Q-obs-1). */
typedef struct obs_combat_row {
    float relX, relY, w, h, velX, velY,
          isTrigger, givesDamage, takesDamage, isTarget, isInvincible,
          hpRaw, hpMaxRaw, animPhase;       /* HO:793-797 column order (obs-wire.md §3.3) */
    const char *kind;                       /* reader.GetKind(col) (HO:798, 134-142); NULL → "unknown" (BP:77) */
    const char *clipKey;                    /* reader.GetClipKey(col) (HO:799, 152-175) = "<entity>|<clip>" or "none"; NULL → "" (BP:88) */
} obs_combat_row;

/* Raw inputs of one combat row; obs_combat_fill() applies HO:753-797 to them. */
typedef struct obs_combat_src {
    float   bounds_center_x, bounds_center_y, bounds_size_x, bounds_size_y; /* Collider2D.bounds (HO:744) world AABB [ENGINE, obs-wire.md Q-obs-4] */
    uint8_t isTrigger;                      /* col.isTrigger (HO:757) */
    uint8_t bucket_is_enemy;                /* kvp.Key == HitboxType.Enemy (HO:763) */
    uint8_t armed;                          /* !col.enabled on an Enemy row (HitboxObserver.CombatRow) */
    int32_t damage_dealt;                   /* Enemy rows: the damage HeroBox.CheckForDamage would pass on (HitboxObserver.GivesDamage) */
    int32_t hazard_type;                    /* Enemy rows: the hazardType HeroBox.CheckForDamage would pass on (HeroBox.cs:47 / :62) */
    uint8_t shadow_dash_hazard;             /* Enemy rows: DamageHero.shadowDashHazard (0 on the damages_hero FSM route) */
    uint8_t has_hm;                         /* reader.GetParentHm(col) != null (HO:761, 197-204, 221-239: self or ≤ 7 ancestors) */
    uint8_t is_boss_hm;                     /* bossHms.Contains(hm) (HO:765) */
    uint8_t hm_IsInvincible;                /* hm.IsInvincible (HO:776) */
    int32_t hm_hp;                          /* hm.hp (HO:766) */
    int32_t hm_max_hp;                      /* reader.ObserveMaxHp(hm) (HO:767, 208-219): caller keeps the per-HM cache */
    uint8_t has_prev_rel;                   /* prevRelCache[col].tick == MotionTick − 1 (HO:783-784) */
    float   prev_rel_x, prev_rel_y;         /* prevRelCache[col].rel (HO:785-786) */
    uint8_t has_clip;                       /* anim != null && anim.CurrentClip != null (HO:160-163) */
    int32_t anim_CurrentFrame;              /* anim.CurrentFrame (HO:167) */
    int32_t clip_frames_Length;             /* clip.frames.Length, 0 if frames == null (HO:165) */
} obs_combat_src;

/* Step-reply scalars (TE:596-763) and the diag block (TE:716-721). */
typedef struct obs_step_view {
    float   damage_landed;                  /* _damageLandedInStep (TE:704; accumulated TE:1172-1187, obs-wire.md §6.1) */
    int32_t hits_taken;                     /* _hitsTakenInStep (TE:705; int, written as (float) at BP:65) */
    float   step_game_time;                 /* Σ Time.deltaTime over the step's live frames (TE:619, 628) */
    float   step_real_time;                 /* Σ Time.unscaledDeltaTime (TE:620, 629) — wall clock, masked by the parity gate (obs-wire.md §5) */
    float   hp_healed;                      /* _hpHealedInStep (TE:699-701, 706) */
    uint8_t done;                           /* _episodeDone (TE:737 / TE:760) */
    uint8_t action_committed;               /* ActionDecoder.ApplyAction return (TE:598-600) */
    const char *info;                       /* _episodeResult when done (TE:738), else "" (obs-wire.md §6.3); NULL → "" (BP:167) */
    uint16_t diag_enemy_count, diag_attack_count, diag_terrain_count; /* HitboxObserver.GetCacheSizes: the observation's bucket sizes (masked, obs-wire.md §5) */
    int32_t  diag_kind_cache_size;          /* TE:720 */
    float    diag_gc_heap_mb;               /* TE:721 GC.GetTotalMemory / 1 MiB (masked, obs-wire.md §5) */
} obs_step_view;

/* Reset-reply trailer (TE:352-354 fake path, TE:508-510 full path; BP:128-138). */
typedef struct obs_reset_view {
    uint8_t  reset_branch;                  /* _resetBranch: OBS_RESET_BRANCH_* (obs-wire.md §6.4) */
    float    reset_phase_ms[OBS_RESET_PHASES];      /* _resetPhaseMs in PR:95-99 key order (wall clock, masked) */
    uint16_t reset_phase_frames[OBS_RESET_PHASES];  /* _resetPhaseFrames (masked) */
} obs_reset_view;

/* Live state of the scene's Terrain-bucket colliders on Dynamic/Kinematic bodies (hk_scene_def.terrain_dyn), filled by
 * sim/fsm: whether each is in the reader's bucket and isActiveAndEnabled now, and its current world outline in the
 * collider's own point order (box bl, br, tr, tl; edge/polygon path 0).  NULL in obs_view -> the SceneReady state. */
typedef struct obs_terrain_dyn_live {
    const uint8_t  *active;                 /* [n_terrain_dyn] */
    const phys_v2  *wpts;                   /* concatenated outlines */
    const uint32_t *off, *n;                /* [n_terrain_dyn]: this entry's slice of wpts */
} obs_terrain_dyn_live;

/* The whole reply.  Pointers are borrowed for the duration of the pack call. */
typedef struct obs_view {
    obs_hero_view          hero;
    const obs_combat_row  *combat;          /* Enemy rows then Attack rows (HO:19 bucket order; obs-wire.md §3.1, §3.8) */
    uint32_t               n_combat;
    const hk_scene_def    *scene;           /* terrain source (the level's hk_scene_<scene>()); NULL → no terrain rows */
    const uint8_t         *terrain_active;  /* optional per-scene->statics[i] isActiveAndEnabled (HO:742); NULL → statics[i].active (SceneReady state) */
    uint8_t                eval_mode;       /* _evalMode → emitTerrainDebug (TE:291, 750); must be 0: BuildTerrainDebug (HO:522-664) is engine queries — traps otherwise */
    const char *const     *fsm_snapshots;   /* FsmObserver.Snapshot strings "<src>|<owner>|<fsm>|<state>" (TE:759, obs-wire.md §3.7) */
    uint32_t               n_fsm;
    obs_step_view          step;            /* step replies only */
    obs_reset_view         reset;           /* reset replies only */
    const obs_terrain_dyn_live *terrain_dyn; /* optional live state of scene->terrain_dyn; NULL -> SceneReady state */
} obs_view;

/* One terrain segment row (HO:516) plus its provenance. */
typedef struct obs_terrain_row {
    float    mx, my, hdx, hdy, npx, npy, dist, isTrigger;  /* HO:516 column order (obs-wire.md §4.2) */
    uint32_t collider;                      /* index into scene->statics */
    uint32_t seg_idx;                       /* HO:484, 517-518: restarts at 0 per collider */
} obs_terrain_row;

/* Python → C# request (BP:179-205; obs-wire.md §1.2). */
typedef struct obs_request {
    uint8_t  type;                          /* OBS_MSG_* */
    int32_t  frames_per_wait;               /* reset: BP:190 */
    int32_t  time_scale;                    /* reset: BP:191 (never read by TrainingEnv, obs-wire.md §1.2) */
    uint8_t  eval;                          /* reset: BP:192 */
    uint8_t  force_full;                    /* reset: BP:193 */
    char     level[256];                    /* reset: BP:194-195 (u16 len + UTF-8; > 255 bytes → HKSIM_ERR_BAD_ARG) */
    int32_t  action_vec[4];                 /* action: BP:198-201 [move, dir, action, jump] (PC:302-309) */
} obs_request;

/* Fill the 14 features of one combat row from its raw inputs (HO:753-797).  kind/clipKey are left untouched. */
HKSIM_API void     obs_combat_fill(obs_combat_row *row, const obs_combat_src *src, const obs_hero_view *hero);

/* The 33 globals (SE:86-96). */
HKSIM_API void     obs_global_state(const obs_hero_view *hero, float out[OBS_GLOBAL_DIM]);

/* Collider2D.bounds.size of an unrotated BoxCollider2D at world position (px, py) with local offset (ox, oy) and size
 * (w, h) — the Knight body (SCENE#1215: offset (0, −0.75), size 0.5 × 1.28125, edgeRadius excluded).  [ENGINE, measured]:
 * the AABB of the float32-transformed vertices, size = fl(p + (o + h/2)) − fl(p + (o − h/2)); reproduces gs[4:6] in
 * 540/540 payloads where the constant 0.5 does not — port-obs.md Q-pobs-5.
 * Sign of the scale does not change the extent. */
HKSIM_API void     obs_box_bounds_size(float px, float py, float ox, float oy, float w, float h, float *size_x, float *size_y);

/* TE:1183 `_damageLandedInStep += hitInstance.DamageDealt / (float)(n * maxHP) * 100f` under the Mono float model
 * (port-obs.md §4): returns the new accumulator (obs-wire.md Q-dmg-9). */
HKSIM_API float    obs_damage_landed_accumulate(float acc, int32_t damageDealt, int32_t n_bosses, int32_t maxHP);

/* Terrain rows for the scene at the given knight position (HO:803-822 + 416-518).  Returns the row count;
 * rows beyond `cap` are counted but not written (cap 0 = size query).  `active_override` as in obs_view. */
HKSIM_API uint32_t obs_terrain_rows(const hk_scene_def *scene, const uint8_t *active_override,
                                    float knightPos_x, float knightPos_y, obs_terrain_row *out, uint32_t cap);
/* The same with the live state of the dynamic-body terrain (obs_view.terrain_dyn).  Row `collider` indexes
 * scene->statics for i < n_statics and scene->terrain_dyn[i - n_statics] above. */
HKSIM_API uint32_t obs_terrain_rows_live(const hk_scene_def *scene, const uint8_t *active_override, const obs_terrain_dyn_live *dyn,
                                         float knightPos_x, float knightPos_y, obs_terrain_row *out, uint32_t cap);

/* Pack a `step` (type 2) / `reset` (type 1) reply exactly as BP:32-176.  Returns the full byte length; bytes past
 * `cap` are not written (buf may be NULL with cap 0 to size the reply).  A `done` step packs the TE:735-746 shape. */
HKSIM_API size_t   obs_pack_step(const obs_view *v, uint8_t *buf, size_t cap);
HKSIM_API size_t   obs_pack_reset(const obs_view *v, uint8_t *buf, size_t cap);

/* One-byte replies for init / pause / resume (BP:37-40 body gate; echoed at TE:1118, TE:162, TE:171).
 * `close` gets no reply (TE:141-143, obs-wire.md §1.3) — never call this for it. */
HKSIM_API size_t   obs_pack_ack(uint8_t msg_id, uint8_t *buf, size_t cap);

/* Parse a request (BP:179-205).  HKSIM_OK, or HKSIM_ERR_BAD_ARG on a short buffer / unknown id. */
HKSIM_API int      obs_unpack_request(const uint8_t *data, size_t len, obs_request *out);

/* sizeof() of the view structs, for the ctypes layout check in tests/test_obs.py:
 * [hero_view, combat_row, combat_src, step_view, reset_view, view, terrain_row, request]. */
HKSIM_API void     obs_layout(uint32_t out[8]);
