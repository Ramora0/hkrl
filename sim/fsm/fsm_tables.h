#pragma once
/* Static scene tables compiled by sim/fsm/gen/gen_tables.py from analysis/fsm/<scene>.json and
 * analysis/dumps/<scene>/{scene,bosses,globals,physics,playerdata}.json.  Read-only; the runtime
 * (sim/fsm/runtime) never mutates these.  Layout mirrors the dump 1:1 so the generator
 * round-trips every FSM (tests/test_fsm.py checks counts). */
#include <stdint.h>

/* pv.kind: what the field holds.  cite: analysis/fsm/GG_Hornet_1.json field "type" strings +
 * analysis/decomp/PlayMaker/HutongGames.PlayMaker/{NamedVariable,FsmOwnerDefault,FsmEventTarget,FunctionCall,FsmVar}.cs */
enum {
    PV_NULL = 0,      /* C# null (null FsmEvent, null array element, null Fsm* object) */
    PV_BOOL, PV_INT, PV_FLOAT, PV_STRING,   /* raw C# fields: i / i / f[0] / i=string id */
    PV_ENUM,          /* i = value, j = enum type string id */
    PV_FFLOAT, PV_FINT, PV_FBOOL, PV_FSTRING, PV_FV2, PV_FV3, PV_FQUAT, PV_FCOLOR, PV_FRECT,
    PV_FGO, PV_FOBJ, PV_FMAT, PV_FTEX, PV_FENUM, PV_FARRAY,   /* Fsm* typed params (NamedVariable) */
    PV_FVAR,          /* FsmVar: i = pool start (variableName PV_STRING, type PV_ENUM, float, int, bool, string, v4) */
    PV_FEVENT,        /* FsmEvent: i = event string id, j = isGlobal */
    PV_OWNERDEF,      /* FsmOwnerDefault: sub = 0 UseOwner / 1 SpecifyGameObject; i = pool index of the FGO */
    PV_EVTARGET,      /* FsmEventTarget: i = pool start of 5 pvs: target(ENUM) excludeSelf(FBOOL) gameObject(OWNERDEF) fsmName(FSTRING) sendToChildren(FBOOL) */
    PV_FUNCCALL,      /* FunctionCall: i = pool start: FunctionName(STRING) parameterType(STRING) Bool Float Int GameObject Object String Vector2 Vector3 Rect Color Material Texture Quaternion Enum Array */
    PV_SETPROP,       /* FsmProperty (SetProperty's targetProperty): i = pool start of: targetIsSelf(BOOL, i=1 when
                       * TargetObject == the owning FSM's own GameObject; else not ported) TargetTypeName(STRING)
                       * PropertyName(STRING) setProperty(BOOL) Bool Float Int GameObject Object String Vector2
                       * Vector3 Rect Color Material Texture Quaternion Enum Array (same 15-param order as
                       * PV_FUNCCALL, minus FunctionName/parameterType) */
    PV_ARRAY,         /* C# array: i = pool start, j = count, sub = element kind */
    PV_OBJREF,        /* UnityEngine.Object reference: i = name string id (-1 null), j = type string id, sub = 0 */
    PV_UNSUPPORTED    /* dumper could not serialise: i = type string id */
};

/* pv.vmode for Fsm* typed params.  cite: NamedVariable.cs:132-154 (IsNone / UsesVariable),
 * ActionData.cs:1181-1211 (a named param IS the FSM's variable object, resolved by name; a miss
 * falls through to the global store then a fresh dangling instance, FsmVariables.cs:1171-1197). */
enum {
    VM_LITERAL = 0,   /* useVariable false: the value fields are the parameter */
    VM_LOCAL = 1,     /* i = index into the owning FSM's flat variable list */
    VM_GLOBAL = 2,    /* i = index into the global variable list (globals.json) */
    VM_NONE = 3,      /* useVariable true + empty name: PlayMaker "None" */
    VM_DANGLING = 4   /* useVariable true + name not found anywhere: i = name string id */
};

typedef struct {
    uint8_t  kind;
    uint8_t  vmode;
    uint16_t sub;
    int32_t  i, j;
    float    f[4];
} fsm_pv;

typedef struct { int32_t name; fsm_pv v; } fsm_field;     /* action field: name string id + value */
typedef struct { int32_t name; fsm_pv v; } fsm_vardef;    /* variable: name + initial value (bucket = v.kind) */

/* One private, non-serialized field of an action that was MID-EXECUTION at SceneReady: PlayMaker's Wait
 * keeps its countdown in `timer`, WaitRandom its drawn duration, iTween actions their elapsed time.  The
 * dumper collects these only for each FSM's active state (FsmDumper.LiveFields), because only there do
 * they mean anything.  An action re-entered by the scene restore reads them through act_live_float so its
 * timer resumes where the game's was instead of restarting at zero. */
typedef struct { int32_t name; float f; int32_t i; } act_livefield_def;

typedef struct {
    int32_t type;          /* action type string id (full "HutongGames.PlayMaker.Actions.X") */
    int32_t type_short;    /* short name string id ("X") */
    uint8_t enabled;
    int32_t field_start, n_fields;
    int32_t live_start, n_live;   /* act_livefield_def range; 0 unless this action's state was active at the dump */
} fsm_action_def;

typedef struct { int32_t event; int32_t to_state; } fsm_trans_def;   /* to_state -1 = unresolved (Fsm.cs:2329) */

typedef struct {
    int32_t name;
    uint8_t is_sequence;
    int32_t action_start, n_actions;
    int32_t trans_start, n_trans;
} fsm_state_def;

/* variable buckets in FsmVariables order (FsmVariables.cs:11-54) */
enum { VB_FLOAT = 0, VB_INT, VB_BOOL, VB_STRING, VB_V2, VB_V3, VB_RECT, VB_QUAT, VB_COLOR, VB_GO, VB_ARRAY, VB_ENUM, VB_OBJ, VB_MAT, VB_TEX, VB_COUNT };

typedef struct {
    int32_t path, go_name, fsm_name;   /* string ids */
    int32_t go;                        /* GO id */
    int32_t instance_id;
    int32_t scene;                     /* string id ("GG_Hornet_1" / "DontDestroyOnLoad") */
    int32_t template_name;             /* string id or -1 */
    uint8_t active_in_hierarchy, active_self, enabled, initialized_before_dump;
    uint8_t handle_fixed, handle_late, restart_on_enable, manual_update, keep_delayed_on_exit;
    uint8_t has_host, used_in_template;
    int32_t max_loop_count_override, max_loop_count, sub_fsm_count, exposed_events;
    int32_t start_state, active_state_at_dump;   /* state indices, -1 none */
    uint8_t started_at_dump, finished_at_dump, fsm_active_at_dump;
    int32_t state_start, n_states;
    int32_t gtrans_start, n_gtrans;
    int32_t var_start, n_vars;
    int32_t var_bucket_start[VB_COUNT + 1];  /* relative to var_start; bucket b = [start[b], start[b+1]) */
    int32_t event_start, n_events;           /* declared events: string ids in fsm_event_decl */
    uint8_t snapshot_start;                  /* emitted by gen_tables.py (SNAPSHOT_RULES) but not read by the sim: every started FSM is restored */
} fsm_def;

typedef struct { int32_t name; uint8_t is_global; } fsm_event_decl;

/* ---- scene objects ---- */
enum { COL_BOX = 0, COL_CIRCLE = 1, COL_POLYGON = 2, COL_EDGE = 3, COL_CAPSULE = 4 };

typedef struct {
    int32_t go;
    uint8_t type, enabled, is_trigger, active_in_hierarchy;
    uint8_t layer; int32_t tag;
    float offset[2], size[2], radius, edge_radius;
    int32_t pts_start, n_pts;    /* polygon path 0 / edge points, local space */
    int32_t rb_go;               /* GO owning the Rigidbody2D this collider is attached to, -1 none */
    int32_t instance_id;
} col_def;

typedef struct {
    int32_t go;
    uint8_t body_type;           /* 0 Dynamic 1 Kinematic 2 Static (UnityEngine.RigidbodyType2D) */
    uint8_t is_kinematic, simulated, freeze_rotation, interpolation, cd_mode;
    float gravity_scale, mass;
    float drag, angular_drag;    /* Rigidbody2D.drag / angularDrag (m_LinearDrag / m_AngularDrag) */
    float pos[2], vel[2];
} rb_def;

/* One sprite's baked collider, from analysis/dumps/<scene>/sprites.json (tk2dSpriteDefinition).
 * tk2dBaseSprite.UpdateCollider (:434-621) reads exactly these fields when the sprite changes, and for
 * Physics2D+Box writes the GameObject's BoxCollider2D outright, so an enemy's hurtbox follows its
 * ANIMATION.  Only the sprites some dumped clip frame actually references are emitted. */
typedef struct {
    uint8_t physics_engine;    /* tk2dSpriteDefinition.PhysicsEngine: 0 Physics3D, 1 Physics2D */
    uint8_t collider_type;     /* ColliderType: 0 Unset, 1 None, 2 Box, 3 Mesh, 4 Custom */
    float off[2];              /* colliderVertices[0] -> BoxCollider2D.offset (x _scale) */
    float half[2];             /* colliderVertices[1] -> BoxCollider2D.size = |2 x this x _scale| */
} spritedef_def;

typedef struct {
    uint8_t trigger_event; int32_t event_info; int32_t event_int; float event_float;
    int32_t sprite;            /* SPRITEDEFS index for this frame's spriteId, -1 if its collection is not dumped */
    int32_t sprite_id;         /* tk2dSpriteAnimationFrame.spriteId: UpdateCollider only runs when it CHANGES (:167) */
} clip_frame_def;

typedef struct {
    int32_t name; float fps; uint8_t wrap; int32_t loop_start; int32_t frame_start, n_frames;
} clip_def;

typedef struct { int32_t name; int32_t clip_start, n_clips; } anim_lib_def;

typedef struct {
    int32_t go, lib;
    uint8_t enabled, play_automatically, paused, playing;
    int32_t default_clip, cur_clip, cur_frame;
    float clip_time_s, clip_fps;
    float sprite_scale[2];     /* tk2dSprite._scale on the same GameObject: UpdateCollider scales by it (:509-510) */
    /* 1 if the GameObject is in the DontDestroyOnLoad scene.  Unity does not re-run Start on a DDOL
     * component when a new scene loads, so tk2dSpriteAnimator.Start's Play(DefaultClip) never happens
     * at boss-scene load: the animator is still on whatever clip it was playing, which is what the
     * dump's cur_clip / cur_frame / clip_time_s record. */
    uint8_t ddol;
} animator_def;

typedef struct {
    /* In BossSceneController.bosses, i.e. the set TrainingEnv.InitBossRefs binds -- which is the
     * reward denominator n and the is_target flag, and is NOT "every HealthManager in the scene".
     * Derived in gen_tables.py from the dump's inBossesArray, or from the mod's own hp >= 100
     * fallback scan for dumps predating that field. */
    uint8_t is_boss;
    int32_t go;
    int32_t hp, enemy_type;
    uint8_t invincible, prevent_invincible_effect, has_alternate_hit_animation, ignore_acid;
    uint8_t damage_override, has_special_death, is_dead, mega_fling_geo;
    int32_t invincible_from_direction;
    int32_t hp_level1, hp_level2, hp_level3;
    int32_t small_geo, medium_geo, large_geo;
    float effect_origin[3];
    float evasion_by_hit_remaining;
    int32_t stun_control_fsm;    /* fsm def index, -1 */
    int32_t send_hit_to;         /* GO id, -1 */
    uint8_t has_recoil, has_hit_effects;
    float ehe_pitch_min, ehe_pitch_max;      /* EnemyHitEffectsUninfected.enemyDamage AudioEvent (hierarchy.json.gz) */
    uint8_t ehe_present, ehe_ghost1_rb, ehe_ghost2_rb;   /* slashEffectGhost1/2 prefabs carry a Rigidbody2D (asset components) */
    uint8_t ehe_armoured;        /* the component is EnemyHitEffectsArmoured, not ...Uninfected (EnemyHitEffectsArmoured.cs) */
    int32_t ehe_armour_hit;      /* its `armourHit` GameObject, receiver of ARMOUR HIT R|L|U|D; -1 */
} hm_def;

typedef struct { int32_t go; int32_t damage_dealt, hazard_type; uint8_t shadow_dash_hazard, reset_on_enable, enabled; } damagehero_def;
typedef struct { int32_t go; uint8_t freeze_in_place, stop_vx_when_up, prevent_recoil_up, skip_freezing; float speed_base, duration; } recoil_def;
typedef struct { int32_t go; uint8_t constrain_x, constrain_y; float xmin, xmax, ymin, ymax; } constrain_def;
/* every other component: its type name, serialized flags a port reads (COMP_* below), and for a MonoBehaviour
 * sim/fsm/components/scripts.c ports (gen_tables.py SCRIPTS) its serialized fields and, for a dumped object, the
 * private fields it held at the dump (field initialisers otherwise; scripts.c scr_restore applies them):
 *   RandomScale                   f = {minScale, maxScale}, i = {scaleOnEnable, didScale}
 *   DeactivateIfPlayerdataTrue|False  i[0] = boolName (string id)
 *   DeactivateAfterDelay          f = {time, timer}, i[0] = stayInPlace
 *   DisableAfterTime              f = {waitTime, disableTime - Time.time at the dump}, i[0] = sendEvent (string id)
 *   SendEnemyMessageTrigger       i = {eventName (string id), eventName at the dump (string id, -1 none)}
 *   EnemyDreamnailReaction        f[0] = cooldownTimeRemaining, i = {convoAmount, convoTitle (string id),
 *                                      startSuppressed | noSoul << 1 | allowUseChildColliders << 2 | (state + 1) << 3
 *                                      (0: not dumped), dreamImpactPrefab (template root, -1 none)}
 *   EnviroRegion                  i[0] = environmentType
 *   ObjectBounce                  f = {bounceFactor, speedThreshold}, i = {playAnimationOnBounce | sendFSMEvent << 1,
 *                                      bouncing | (rb assigned) << 1, stepCounter, script_floats index of
 *                                      {velocity.x, velocity.y, lastPos.x, lastPos.y, speed} at the dump}
 *   Breakable                     f = {angleOffset, flingSpeedMin, flingSpeedMax}, i = {script_gos index of
 *                                      wholeParts, remnantParts, debrisParts (each a count, then the GameObject ids),
 *                                      hitEventReciever (GO id, -1), forwardBreakEvent | isBroken << 1}
 *   TinkEffect                    i = {sendFSMEvent | sendDirectionalFSMEvents << 1, FSMEvent (string id), the FsmName
 *                                      of `fsm`, a PlayMakerFSM on the same object (string id, -1 when no flag is set)}
 *   CorpseBitEnd                  f = {timer (serialized), timer at the dump}, i[0] = stopped at the dump
 *   KeepWorldScalePositive        (none) */
typedef struct comp_def { int32_t go; int32_t type; uint8_t enabled; uint8_t flags; float f[3]; int32_t i[4]; } comp_def;
enum { COMP_RANDOMISE_ROTATION = 1 };   /* RecycleAfter2dtkAnimation.randomiseRotation */
/* EventRegister.cs:16-31,52-66: an EventRegister component subscribes its GameObject to one named event
 * in Awake, and EventRegister.SendEvent(name) calls ReceiveEvent on every subscriber, which is
 * FSMUtility.SendEventToGameObject(go, name).  These are authored on the prefab, not added by an
 * AddEventRegister action (e.g. the SendEventToRegister("HERO DAMAGED") subscribers). */
typedef struct { int32_t go; int32_t name; } evreg_def;

typedef struct {
    int32_t path, name;
    int32_t parent;              /* GO id, -1 root */
    int32_t first_child, next_sibling;   /* sibling order = dump order (gen_tables.py, ASSUMPTION Q-pfsm) */
    int32_t instance_id;
    uint8_t active_self, has_transform, layer, in_scene;
    int32_t tag;                 /* string id, -1 unknown */
    float pos[3], local_pos[3], local_scale[3], lossy_scale[3], euler_z, local_euler_z;
    int32_t rb;                  /* rb_def index, -1 */
    int32_t col_start, n_cols;   /* col_idx[] range */
    int32_t comp_start, n_comps; /* comp_def range */
    int32_t animator, hm, damage_hero, recoil, constrain;   /* def indices, -1 */
    int32_t fsm_start, n_fsms;   /* fsm_idx[] range: fsm def indices on this GO, dump order */
    /* 1: an object of a prefab asset (sim/fsm/gen/prefabs.py): the template Object.Instantiate copies, or a bare
     * stub for a prefab the ported scenes never spawn (completeness.py UNREACHABLE_PREFABS).  Never active, found,
     * observed or ticked. */
    uint8_t asset;
    int32_t prefab;              /* a pooled clone's root: its prefab's template root (the ObjectPool key), -1 */
} go_def;

typedef struct { int32_t name, type; float f; int32_t i; uint8_t b; int32_t s; uint8_t kind; float v[3]; } pd_field_def; /* PlayerData field: kind 0 float 1 int 2 bool 3 string 4 Vector3 */

/* NailSlash serialized fields, verbatim from analysis/dumps/<scene>/scene.json
 * (colliders[].components[type=NailSlash].fields): the authored `scale` that StartSlash multiplies by the
 * charm factor (NailSlash.cs:68-86) and the tk2d clip base name it plays.  NOT the GameObject's localScale --
 * for Knight/Attacks/Slash those differ (scale.x 1.601078 vs localScale.x 1.62). */
typedef struct { int32_t go; float scale[3]; int32_t anim_name; } nailslash_def;

/* AutoRecycleSelf (HK/AutoRecycleSelf.cs): OnEnable starts a WaitForSeconds(timeToWait) that ends in
 * gameObject.Recycle().  `after_event` is GlobalEnums.AfterEvent; only TIME (0) is modelled -- see
 * world_autorecycle_update for why the others need nothing. */
typedef struct { int32_t go; int32_t after_event; float time_to_wait; } autorecycle_def;

/* ---- Mecanim (UnityEngine.Animator), sim/fsm/runtime/mecanim.c ----
 * From analysis/assets animation.json.gz (the AnimatorController's compiled state machine and its AnimationClips,
 * tools/extract_assets.py), emitted by sim/fsm/gen/mecanim.py for the controller shape mecanim.c ports.  Each
 * binding is one clip curve resolved against the Animator's own subtree, in the clip's curve order; a Transform
 * vector property is three consecutive bindings x, y, z (`index` = the axis). */
enum { MEC_COLLIDER_ENABLED = 0,   /* Collider2D.m_Enabled: `index` = the collider (go_inst.cols) of that class */
       MEC_GO_ACTIVE = 1,          /* GameObject.m_IsActive (a non-root path) */
       MEC_LOCAL_POS = 2,          /* Transform.m_LocalPosition */
       MEC_LOCAL_SCALE = 3,        /* Transform.m_LocalScale */
       MEC_LOCAL_EULER_Z = 4 };    /* Transform.m_LocalEulerAngles.z (x and y constant 0) */
typedef struct { float t; float c[4]; } mec_key_def;   /* streamed segment from t: ((c0*u + c1)*u + c2)*u + c3, u = t' - t */
typedef struct { float start, stop; uint8_t loop; } mec_clip_def;   /* AnimationClip m_MuscleClip start/stop, loopTime */
typedef struct { int32_t go; uint8_t prop; int32_t index; int32_t key_start, n_keys; } mec_bind_def;
typedef struct {
    int32_t go;
    uint8_t enabled;               /* Behaviour.m_Enabled as serialized */
    int32_t clip;                  /* the single state's clip (mec_clip_def index) */
    float speed;                   /* StateConstant.m_Speed */
    float node_duration;           /* the state's blend-tree node m_Duration */
    int32_t bind_start, n_binds;
    uint8_t cull_completely;       /* m_CullingMode == 2 (mecanim.c traps: native-animator.md A-26) */
} mec_anim_def;

/* one localisation entry: STR indices for sheet title, key, and text (Language.cs:21) */
typedef struct { int32_t sheet, key, value; } lang_entry;

typedef struct {
    const char *scene_name;
    const char *const *strings; int32_t n_strings;
    const fsm_pv *pool; int32_t n_pool;
    const fsm_field *fields; int32_t n_fields;
    const act_livefield_def *livefields; int32_t n_livefields;
    const fsm_action_def *actions; int32_t n_actions;
    const fsm_trans_def *trans; int32_t n_trans;
    const fsm_state_def *states; int32_t n_states;
    const fsm_vardef *vars; int32_t n_vars;
    const fsm_event_decl *events; int32_t n_events;
    const fsm_def *fsms; int32_t n_fsms;
    const fsm_vardef *globals; int32_t n_globals; const int32_t global_bucket_start[VB_COUNT + 1];
    const int32_t *global_events; int32_t n_global_events;
    const go_def *gos; int32_t n_gos;
    const int32_t *col_idx; const col_def *cols; int32_t n_cols;
    const float *col_pts; int32_t n_col_pts;
    const rb_def *rbs; int32_t n_rbs;
    const comp_def *comps; int32_t n_comps;
    const evreg_def *evregs; int32_t n_evregs;
    const int32_t *fsm_idx; int32_t n_fsm_idx;
    const anim_lib_def *libs; int32_t n_libs;
    const clip_def *clips; int32_t n_clips;
    const clip_frame_def *frames; int32_t n_frames;
    const spritedef_def *spritedefs; int32_t n_spritedefs;
    const animator_def *animators; int32_t n_animators;
    const hm_def *hms; int32_t n_hms;
    const damagehero_def *damageheros; int32_t n_damageheros;
    const recoil_def *recoils; int32_t n_recoils;
    const constrain_def *constrains; int32_t n_constrains;
    const pd_field_def *playerdata; int32_t n_playerdata;
    /* Localisation sheets, verbatim from analysis/dumps/<scene>/language.json (Language.currentEntrySheets,
     * analysis/decomp/Assembly-CSharp/Language/Language.cs:21).  Sorted by (sheet, key) for bsearch. */
    const nailslash_def *nailslash; int32_t n_nailslash;
    const lang_entry *lang; int32_t n_lang; int32_t lang_code;
    const char *const *layer_names;
    const uint32_t *layer_matrix;      /* bit j of [i]: layers i,j collide (phys.h contract) */
    float gravity[2];
    int32_t knight_go, hornet_go, camera_parent_go, game_manager_go;
    int32_t frame_count_at_dump;
    /* Which route TrainingEnv.InitBossRefs takes ("the dump does not say" != "the array was empty"):
     *   0 UNKNOWN  the dump predates the inBossesArray field: bind at t=0.
     *   1 NATIVE   field present and BossSceneController.bosses non-empty -> bound from tick 1.
     *   2 SCAN     field present and the array EMPTY -> the mod's own hp>=100 rescan every 240 steps
     *              (TrainingEnv.cs:535-548): is_target 0 and no damage credited before then.
     * Only 2 delays binding: the hp>=100 fallback says WHICH HealthManagers are in the set, not WHEN. */
    uint8_t boss_bind_route;
    /* appended LAST: the generated tables are positional initializers */
    const autorecycle_def *autorecycles; int32_t n_autorecycles;
    /* BossSceneController.BossLevel of the dump (meta.json bossLevel; gen_tables.py dump_boss_level): 0 Attuned,
     * 1 Ascended, 2 Radiant.  fsm_world.boss_level starts here. */
    int32_t boss_level;
    /* The level string this table is looked up by: the scene name, or "<scene>@T1|@T2" for a tier dump
     * (gen_tables.py split_level_key).  scene_name stays the Unity scene name. */
    const char *level_key;
    const mec_anim_def *mec_anims; int32_t n_mec_anims;
    const mec_bind_def *mec_binds; int32_t n_mec_binds;
    const mec_clip_def *mec_clips; int32_t n_mec_clips;
    const mec_key_def *mec_keys; int32_t n_mec_keys;
    /* GameObject lists of ported scripts' serialized fields (comp_def payloads index it: a count, then the ids) */
    const int32_t *script_gos; int32_t n_script_gos;
    /* Extra per-instance floats a script's fixed comp_def.f/i cannot hold (comp_def.i indexes the start; the
     * count is the payload's own, e.g. ObjectBounce's fixed 5) */
    const float *script_floats; int32_t n_script_floats;
} hkfsm_scene_def;
