/* The fsmi_* entry points the core calls (sim/core/sim_modules.h).  The core owns the clocks, the RNG and the
 * physics world; this side owns the GameObjects/FSMs/animators/HK components and, through the component
 * lifecycle (lifecycle.c), WHEN every component -- the core's included -- runs. */
#include "fsm/fsm.h"
#include "fsm/lifecycle.h"
#include "world_internal.h"
#include "core/sim_modules.h"
#include "hero/hero.h"
#include "obs/obs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "core/alloc.h"   /* per-instance arena */

extern void event_register_reset(fsm_world *w);
extern void event_register_seed(fsm_world *w);

#define FSM_ENT_MAX 8
typedef struct {
    fsm_world *w;
    struct hero *hero;
    bool slash_poly_on[5], slash_tink_on[5];
    int32_t n_log_emitted;
    int32_t fsm_by_hook[FSM_N];
    int32_t run_effect;             /* HeroController.runEffect: the Run Effects clone the last run spawned, -1 */
} fsm_ctx;

/* ---- hero hooks (sim/hero/hero.h hero_hooks) ---- */
static void hook_fsm_send_event(void *c, int fsm, const char *event)
{
    fsm_ctx *x = c;
    if (fsm < 0 || fsm >= FSM_N || x->fsm_by_hook[fsm] < 0) return;
    fsm_event_name(&x->w->fsms[x->fsm_by_hook[fsm]], event);       /* PlayMakerFSM.SendEvent (PMF:444-447) */
}
static void hook_fsm_set_bool(void *c, int fsm, const char *var, int value)
{
    fsm_ctx *x = c;
    if (fsm < 0 || fsm >= FSM_N || x->fsm_by_hook[fsm] < 0) return;
    fsm_inst *f = &x->w->fsms[x->fsm_by_hook[fsm]];
    int32_t vi = fsm_find_var(f, VB_BOOL, w_find_string(x->w, var));
    if (vi >= 0) f->vals[vi].i = value ? 1 : 0;
}
static void hook_noop(void *c) { (void)c; }
static void hook_noop_i(void *c, int v) { (void)c; (void)v; }
static void hook_hero_box_inactive(void *c, int v) { fsm_ctx *x = c; x->w->hero_box_inactive = v ? 1 : 0; }
static void hook_effect(void *c, const char *name)
{
    fsm_ctx *x = c;
    if (!x || !x->w || !name) return;
    fsm_world *w = x->w;
    if (strcmp(name, "runEffectPrefab.Spawn") == 0) {
        /* HC:5135-5136 runEffect = runEffectPrefab.Spawn(); SetParent(hero, worldPositionStays: false) */
        int32_t prefab = world_pool_prefab(w, "Run Effects(Clone)");
        if (prefab < 0) HKSIM_UNIMPLEMENTED("runEffectPrefab.Spawn: no Run Effects(Clone) pool in the tables");
        const float zero[3] = { 0.0f, 0.0f, 0.0f };
        int32_t g = world_pool_spawn(w, prefab, zero, 0.0f);
        if (g >= 0) {
            float ls[3]; go_local_scale(w, g, ls);
            go_set_parent(w, g, w->knight_go);
            go_set_local_pos(w, g, zero); go_set_local_euler_z(w, g, 0.0f); go_set_local_scale(w, g, ls);
            x->fsm_by_hook[FSM_RUN_EFFECT] = world_fsm_find(w, g, "Run Effects");
            x->run_effect = g;
        }
    } else if (strcmp(name, "runEffect.SetParent(null)") == 0) {
        if (x->run_effect >= 0) go_set_parent(w, x->run_effect, -1);   /* HC:5130, HC:5147 worldPositionStays: true */
    } else if (strcmp(name, "shadowRechargePrefab.SetActive(true)") == 0) {
        int32_t g = world_go_find_path(x->w, "Knight/Effects/Shadow Recharge");
        if (g >= 0) go_set_active(x->w, g, true);
    } else if (strcmp(name, "shadowdashParticles/shadowRing") == 0) {
        /* HC:3594 shadowRingPrefab.Spawn(transform.position) */
        int32_t prefab = world_pool_prefab(w, "Shadow Ring(Clone)");
        if (prefab < 0) HKSIM_UNIMPLEMENTED("shadowRingPrefab.Spawn: no Shadow Ring(Clone) pool in the tables");
        float p[3]; go_world_pos(w, w->knight_go, p);
        world_pool_spawn(w, prefab, p, 0.0f);
    } else if (strstr(name, "dJumpFlash") != NULL || strstr(name, "doubleJumpClip") != NULL) {
        int32_t g = world_go_find_path(x->w, "Knight/Effects/White Flash DJ");
        if (g >= 0) go_set_active(x->w, g, true);
    }
}
/* animCtrl.PlayClip / tk2dSpriteAnimator.Play(name) on the Knight's animator (HeroAnimationController port in sim/hero calls it) */
static void hook_anim_play_clip(void *c, const char *clip)
{
    fsm_ctx *x = c; fsm_world *w = x->w;
    anim_inst *a = w->knight_go >= 0 ? anim_of_go(w, w->knight_go) : NULL;
    if (a && clip && *clip) anim_play_name(w, a, clip);
}
static void hook_noop_f(void *c, float v) { (void)c; (void)v; }
/* HeroController.Attack (HC:1465-1494): slashFsm.FsmVariables.GetFsmFloat("direction").Value = 0 / 180 /
 * 90 / 270 by facing and attack direction.  TakeDamage reads it as HitInstance.Direction, which sets the
 * enemy's recoil direction, the HIT LEFT/RIGHT/UP/DOWN events and the corpse fling. */
static void hook_slash_dir(void *c, int slash, float d)
{
    fsm_ctx *x = c; fsm_world *w = x->w;
    if (slash < 0 || slash >= 5) return;
    int32_t g = world_slash_go(w, slash);
    if (g < 0) return;
    int32_t fi = world_fsm_find(w, g, "damages_enemy");
    if (fi < 0) return;
    int32_t name = w_find_string(w, "direction");
    if (name < 0) return;
    int32_t vi = fsm_find_var(&w->fsms[fi], VB_FLOAT, name);
    if (vi >= 0) w->fsms[fi].vals[vi].f = d;
}
/* NailSlash.StartSlash (NS:64-92): localScale = scale * {1, 1.15, 1.25, 1.4} and anim.Play(animName [+ " M"/" F"]).
 * `scale` and `animName` are serialized NailSlash fields (scene.json -> NAILSLASH[]); `scale` is NOT the
 * GameObject's localScale (Knight/Attacks/Slash: scale.x 1.601078 vs localScale.x 1.62).  sim/hero supplies only
 * the charm multiplier and the clip suffix. */
static void hook_slash_anim(void *c, int slash, const char *suffix, float m)
{
    fsm_ctx *x = c; fsm_world *w = x->w;
    if (slash < 0 || slash >= 5) return;
    int32_t g = world_slash_go(w, slash);
    if (g < 0) return;
    const nailslash_def *ns = NULL;
    for (int32_t i = 0; i < w->sc->n_nailslash; i++)
        if (w->sc->nailslash[i].go == g) { ns = &w->sc->nailslash[i]; break; }
    if (!ns) return;
    float s[3] = { ns->scale[0] * m, ns->scale[1] * m, ns->scale[2] };   /* NS:70 leaves scale.z alone */
    go_set_local_scale(w, g, s);
    /* the physics shape is baked from the transform chain at bind time: re-bake after the rescale */
    world_rebake_polygon_shapes(w, g);
    anim_inst *a = anim_of_go(w, g);
    if (a) {
        char clip[96];
        snprintf(clip, sizeof clip, "%s%s", w_str(w, ns->anim_name), suffix ? suffix : "");
        anim_play_name(w, a, clip);                                      /* NS:71/76/81/86/90 */
        /* NS:92 anim.PlayFromFrame(0): unlike Play(name) above, restarts a clip already mid-playback */
        anim_play_from_frame(w, a, 0);
        /* NS:98 animator.AnimationCompleted = Disable: claim the delegate the same "last writer wins"
         * way HeroAnimationController does (tk2d.c fire_completed, completed_kind==2 ->
         * hero_slash_anim_completed), so CancelAttack's animCompleted gate (NS:117-120) fires. */
        a->completed_kind = 2;
        a->completed_slash_slot = slash;
    }
}

/* EVENT HERO_DAMAGE (docs/trace-format.md ev 2): source is the DamageHero / damages_hero object whose
 * trigger reached HeroBox, stashed by world_phys_event just before it dispatches the hit. */
static void hook_take_damage_post(void *c, int32_t amount, int32_t hazard_type, int32_t hp_after)
{
    fsm_ctx *x = c; fsm_world *w = x->w;
    int32_t src = w->dmg_src_go >= 0 ? w_intern(w, go_name(w, w->dmg_src_go)) : w_intern(w, "");
    world_log4(w, LOG_HERO_DAMAGE, src, amount, hazard_type, hp_after);
}

/* GM:710 PlayMakerFSM.BroadcastEvent("HAZARD RELOAD"), sent from the hazard respawn chain in sim/hero.
 * The GG boss FSMs listen for it to tear the fight down and re-arm after the knight is put back. */
static void hook_gm_hazard_reload(void *ctx)
{
    fsm_ctx *x = ctx;
    world_broadcast_event(x->w, w_get_fsm_event(x->w, "HAZARD RELOAD"), -1, false, false, 0);
}

int fsmi_create(void **ctx, const char *level, hk_rng *rng, phys_world *pw, struct hero *hero,
                        struct hero_hooks *hooks_out, char *err, size_t errlen)
{
    const hkfsm_scene_def *sc = hkfsm_scene_lookup(level);
    if (!sc) { snprintf(err, errlen, "no compiled FSM tables for '%s'", level); return HKSIM_ERR_UNKNOWN_LEVEL; }
    fsm_ctx *x = calloc(1, sizeof *x);
    x->w = world_create(sc);
    x->w->dmg_src_go = -1;
    x->w->rng = rng;
    x->w->phys = pw;
    x->hero = hero;
    x->w->hero = hero;
    world_phys_bind(x->w);                                         /* Hornet, Needle, corpse: bodies + shapes in `pw` */
    /* HealthManager.Start sets hp = hpScale.GetScaledHP(hp) (analysis/decomp/Assembly-CSharp/HealthManager.cs:297-315,
     * GetScaledHP :20-46: BossLevel 0/1/2 -> level1/level2/level3 when > 0).  A HealthManager dumped ACTIVE already
     * carries its scaled hp (Start already ran in the game); world.c:494 loads the raw dumped hp for every
     * HealthManager, active or not.  One dumped INACTIVE (never yet Awoken this session, e.g. a boss whose arena
     * intro hasn't triggered it) is deferred, not lost: lc_scene_load's inactive_started_at_dump leaves it
     * un-started, and its first lc_go_activate (R1) runs hm_start (lifecycle.c) through the normal Start queue,
     * scaling hp then -- exactly Unity's timing. No refusal needed here. */
    event_register_reset(x->w);
    event_register_seed(x->w);   /* prefab EventRegister components (EventRegister.Awake -> SubscribeEvent) */
    for (int i = 0; i < FSM_N; i++) x->fsm_by_hook[i] = -1;
    int32_t knight = sc->knight_go;
    x->fsm_by_hook[FSM_PROXY] = world_fsm_find(x->w, knight, "ProxyFSM");
    x->fsm_by_hook[FSM_SUPERDASH] = world_fsm_find(x->w, knight, "Superdash");
    x->fsm_by_hook[FSM_SPELL_CONTROL] = world_fsm_find(x->w, knight, "Spell Control");
    x->fsm_by_hook[FSM_CAMERA_SHAKE] = world_fsm_find(x->w, sc->camera_parent_go, "CameraShake");
    x->fsm_by_hook[FSM_DAMAGE_EFFECT] = world_fsm_find(x->w, world_go_find_path(x->w, "Knight/Effects/Damage Effect"), "Knight Damage");
    x->run_effect = -1;             /* FSM_RUN_EFFECT: the spawned runEffect's FSM, bound at its spawn */
    x->fsm_by_hook[FSM_SHADOW_RECHARGE] = world_fsm_find(x->w, world_go_find_path(x->w, "Knight/Effects/Shadow Recharge"), "Recharge Effect");
    if (hooks_out) {
        memset(hooks_out, 0, sizeof *hooks_out);
        hooks_out->ctx = x;
        hooks_out->fsm_send_event = hook_fsm_send_event;
        hooks_out->fsm_set_bool = hook_fsm_set_bool;
        hooks_out->slash_fsm_set_direction = hook_slash_dir;
        hooks_out->slash_anim_play = hook_slash_anim;
        hooks_out->anim_update_state = hook_noop_i;
        hooks_out->anim_finished_dash = hook_noop;
        hooks_out->anim_stop_attack = hook_noop;
        hooks_out->anim_play_clip = hook_anim_play_clip;
        hooks_out->anim_set_play_landing = hook_noop_i;
        hooks_out->anim_control = hook_noop_i;
        hooks_out->effect = hook_effect;
        hooks_out->take_damage_post = hook_take_damage_post;
        hooks_out->on_taken_damage = hook_noop;
        hooks_out->on_death = hook_noop;
        hooks_out->gm_player_dead = hook_noop_f;
        hooks_out->gm_player_dead_from_hazard = hook_noop;
        hooks_out->gm_hazard_reload = hook_gm_hazard_reload;
        hooks_out->hero_box_set_inactive = hook_hero_box_inactive;
    }
    *ctx = x;
    return 0;
}
void fsmi_destroy(void *ctx) { fsm_ctx *x = ctx; if (!x) return; world_destroy(x->w); free(x); }
fsm_world *fsmi_world(void *ctx) { return ((fsm_ctx *)ctx)->w; }

/* TrainingEnv's reset loop binds a SCAN-route boss (BossSceneController.bosses empty, boss_bind_route 2) once
 * an ENEMY-bucket collider is live (TrainingEnv.cs:437-470, ScanBindBossHMs); the dump is taken after that
 * loop.  Otherwise Step's rescan (world_boss_bind_tick) binds it.
 *
 * The reset loop itself (oracle/Env/TrainingEnv.cs:196-221) never fails: it ticks up to kMaxWakeFrames=600
 * frames waiting for `_bossHMs.Count > 0 && HasActiveCombatHitboxes()`, then proceeds regardless
 * (`bool bossAwake = HasActiveCombatHitboxes();` logged, not thrown) -- a boss whose HealthManager is
 * still inactive at SceneReady (entrance cinematic; GG_Radiance's `Boss Control/Absolute Radiance` dumps
 * activeSelf false, hp 3000) is expected to still be unbound here; world_boss_bind_tick's every-240-steps
 * rescan binds her later, as Step's does (TrainingEnv.cs:290-294). */
static void wake_scan_bind(fsm_world *w)
{
    if (w->sc->boss_bind_route != 2 || w->n_bound_bosses > 0) return;
    if (!world_has_active_enemy_hitbox(w)) return;
    world_scan_bind_boss_hms(w);
}

/* Scene start restores every component from the SceneReady dump; no frame runs here. */
void fsmi_scene_start(void *ctx, float time)
{
    fsm_ctx *x = ctx;
    x->w->time = time;
    world_restore_scene(x->w);
    wake_scan_bind(x->w);
    x->w->n_log = 0; x->n_log_emitted = 0;                         /* the oracle records no FSM events before SCENE_READY */
}

void fsmi_hero_anim(void *ctx, const char **clip, int32_t *frame, float *clip_time, int *playing, float *fps)
{
    fsm_ctx *x = ctx; fsm_world *w = x->w; (void)x;
    anim_inst *a = w->knight_go >= 0 ? anim_of_go(w, w->knight_go) : NULL;
    if (!a) {
        if (clip) *clip = "";
        if (frame) *frame = 0;
        if (clip_time) *clip_time = 0.0f;
        if (playing) *playing = 0;
        if (fps) *fps = 0.0f;
        return;
    }
    if (clip) *clip = anim_clip_name(w, a);
    if (frame) *frame = anim_current_frame(w, a);
    if (clip_time) *clip_time = anim_clip_time_seconds(w, a);
    if (playing) *playing = anim_playing(a);
    if (fps) *fps = a->clip_fps;
}

/* ENTITY block: the HealthManagers (docs/trace-format.md FRAME) with their FSMs' float/int/bool variables */
void fsmi_emit_entities(void *ctx, tw_buf *b)
{
    fsm_ctx *x = ctx; fsm_world *w = x->w;
    world_invalidate_body_transforms(w);
    tw_entities_begin(b, (uint16_t)w->n_hms);
    for (int32_t i = 0; i < w->n_hms; i++) {
        hm_inst *h = &w->hms[i];
        float p[3] = { 0, 0, 0 }, s[3] = { 1, 1, 1 }, v[2] = { 0, 0 };
        if (w->gos[h->go].has_transform) { go_world_pos(w, h->go, p); go_local_scale(w, h->go, s); }
        if (go_has_rb(w, h->go)) go_velocity(w, h->go, v);
        tw_entity_begin(b, go_name(w, h->go), w->gos[h->go].def->instance_id, go_active_in_hierarchy(w, h->go), h->hp,
                        h->is_dead != 0, h->invincible != 0, p[0], p[1], s[0], v[0], v[1]);
        anim_inst *a = anim_of_go(w, h->go);
        if (a) tw_anim(b, anim_clip_name(w, a), anim_current_frame(w, a), anim_clip_time_seconds(w, a), anim_playing(a), a->clip_fps);
        else tw_anim(b, "", 0, 0.0f, false, 0.0f);
        /* GetComponentsInChildren<PlayMakerFSM>(true): hierarchy walk = generated GO order under the entity */
        int32_t n = 0;
        for (int32_t k = 0; k < w->n_fsms; k++) if (is_under(w, w->fsms[k].go, h->go)) n++;
        tw_fsms_begin(b, (uint16_t)n);
        for (int32_t k = 0; k < w->n_fsms; k++) {
            fsm_inst *f = &w->fsms[k];
            if (!is_under(w, f->go, h->go)) continue;
            const char *root = go_path(w, h->go);
            const char *fp = go_path(w, f->go);
            const char *rel = strlen(fp) > strlen(root) ? fp + strlen(root) + 1 : "";
            const fsm_def *d = f->def;
            int32_t nv = d->var_bucket_start[VB_BOOL + 1] - d->var_bucket_start[VB_FLOAT];
            tw_fsm_begin(b, rel, w_str(w, d->fsm_name), f->active_state >= 0 ? state_name(f, f->active_state) : "",
                         f->component_enabled && go_active_in_hierarchy(w, f->go), (uint16_t)nv);
            for (int32_t vi = d->var_bucket_start[VB_FLOAT]; vi < d->var_bucket_start[VB_BOOL + 1]; vi++) {
                int bkt = vi < d->var_bucket_start[VB_INT] ? VB_FLOAT : (vi < d->var_bucket_start[VB_BOOL] ? VB_INT : VB_BOOL);
                tw_fsm_var(b, w_str(w, w->sc->vars[d->var_start + vi].name), (uint8_t)bkt,
                           bkt == VB_FLOAT ? f->vals[vi].f : (float)f->vals[vi].i);
            }
        }
    }
}

void fsmi_emit_events(void *ctx, tw_buf *b, uint32_t frame, uint32_t fixed_count)
{
    fsm_ctx *x = ctx; fsm_world *w = x->w;
    for (int32_t i = x->n_log_emitted; i < w->n_log; i++) {
        log_rec *r = &w->log[i];
        fsm_inst *f = (r->kind == LOG_HERO_DAMAGE || r->kind == LOG_ENEMY_DAMAGE) ? NULL : &w->fsms[r->fsm];
        (void)f;
        if (r->kind == LOG_HERO_DAMAGE) { tw_ev_hero_damage(b, frame, fixed_count, r->phase, w_str(w, r->fsm), r->a, r->b, r->c); continue; }
        if (r->kind == LOG_ENEMY_DAMAGE) { tw_ev_enemy_damage(b, frame, fixed_count, r->phase, go_name(w, r->fsm), r->a, r->b, r->c); continue; }
        if (r->kind == LOG_FSM_EVENT) tw_ev_fsm_event(b, frame, fixed_count, r->phase, go_name(w, f->go), w_str(w, f->def->fsm_name), w_str(w, r->a));
        else if (r->kind == LOG_FSM_TRANSITION) tw_ev_fsm_transition(b, frame, fixed_count, r->phase, go_name(w, f->go), w_str(w, f->def->fsm_name), state_name(f, r->a), state_name(f, r->b));
    }
    x->n_log_emitted = 0; w->n_log = 0;
}
/* A boss counts as dead when its HealthManager died OR when an FSM fired OnDeath through
 * SendHealthManagerDeathEvent (HealthManager.cs:685-691 sets no isDead), because the trainer
 * detects boss death by subscribing to that delegate, not by polling HP. */
/* Returns 1 when the fight is over the way TrainingEnv decides it, 2 when the arena was left, else 0.
 *   1: BossSceneController.OnBossesDead fired (EndGGBossScene / CheckBossesDead), OR an OnDeath fired on a
 *      tracked boss HM and every other tracked boss HM is at hp <= 0 (TrainingEnv.OnBossActualDeath's
 *      all-dead test: a multi-HM fight -- Mantis Lords, Oro & Mato, Watcher Knights, False Knight's two --
 *      is not over on its first death).  Tracked = gen_tables is_boss; if the scene marks none (scan-route
 *      bosses), every HealthManager counts, which is what the env's fallback scan binds.
 *   2: scene_left (BeginSceneTransition ran) and neither of the above. */
int32_t fsmi_n_gos(void *ctx) { fsm_ctx *x = ctx; return x->w->n_gos; }
int fsmi_boss_dead(void *ctx)
{
    fsm_ctx *x = ctx; fsm_world *w = x->w;
    if (w->bosses_dead_signal) return 1;
    /* Tracked = the HMs the env has OnDeath handlers on: `bound` (native route: is_boss at tick 1;
     * scan route: world_boss_bind_tick every 240 steps with TrainingEnv.ScanBindBossHMs' hp >= 100 test).
     * Before anything is bound the env has no handler, so an OnDeath cannot end the episode. */
    int any_death = 0, all_down = 1, n_bound = 0;
    for (int32_t i = 0; i < w->n_hms; i++) {
        const hm_inst *h = &w->hms[i];
        if (!h->bound) continue;
        n_bound++;
        int died = h->is_dead || h->death_event_sent;
        if (died) any_death = 1;
        else if (h->hp > 0) all_down = 0;
    }
    if (n_bound && any_death && all_down) return 1;
    return w->scene_left ? 2 : 0;
}
float fsmi_take_damage_landed(void *ctx) { fsm_ctx *x = ctx; return world_take_damage_landed(x->w); }
void fsmi_on_phys_event(void *ctx, const phys_event *e) { fsm_ctx *x = ctx; world_phys_event(x->w, e); }
void fsmi_after_physics_step(void *ctx) { fsm_ctx *x = ctx; world_invalidate_body_transforms(x->w); }
/* observer.c (declared here: fsm.h does not see phys_v2) */
void world_terrain_live(fsm_world *w, const int32_t *static_iids, uint32_t n_static, uint8_t *static_active,
                              const int32_t *dyn_iids, uint32_t n_dyn, uint8_t *dyn_active,
                              phys_v2 *dyn_pts, const uint32_t *dyn_off, uint32_t *dyn_n, uint32_t pts_cap);
void fsmi_terrain_live(void *ctx, const int32_t *static_iids, uint32_t n_static, uint8_t *static_active,
                               const int32_t *dyn_iids, uint32_t n_dyn, uint8_t *dyn_active,
                               phys_v2 *dyn_pts, const uint32_t *dyn_off, uint32_t *dyn_n, uint32_t pts_cap)
{
    fsm_ctx *x = ctx;
    world_invalidate_body_transforms(x->w);
    world_terrain_live(x->w, static_iids, n_static, static_active, dyn_iids, n_dyn, dyn_active, dyn_pts, dyn_off, dyn_n, pts_cap);
}
uint32_t fsmi_combat_sources(void *ctx, struct obs_combat_src *out, const char **kinds, const char **clip_keys, uint32_t cap, const struct obs_hero_view *hero)
{
    fsm_ctx *x = ctx;
    return world_combat_sources(x->w, out, kinds, clip_keys, cap, hero ? hero->knightPos_x : 0.0f, hero ? hero->knightPos_y : 0.0f);
}
uint32_t fsmi_fsm_snapshots(void *ctx, const char **out, uint32_t cap) { fsm_ctx *x = ctx; return world_fsm_snapshots(x->w, out, cap); }
void fsmi_bind_hero_body(void *ctx, phys_body_id hero_body) { fsm_ctx *x = ctx; world_bind_hero_body(x->w, hero_body); }
void fsmi_bind_statics(void *ctx, const int32_t *iids, const phys_body_id *bodies, const phys_shape_id *shapes, uint32_t n)
{ fsm_ctx *x = ctx; world_bind_statics(x->w, iids, bodies, shapes, n); }
/* the slash / Clash Tink polygons are not in any dump (scene.json rows carry 0 points, hierarchy.json.gz has no
 * DontDestroyOnLoad objects) — the enable state is kept, no shape exists to toggle (port-fsm.md Q-pfsm-17) */
void fsmi_slash_set_enabled(void *ctx, int slash, int poly_on, int clash_tink_on)
{
    fsm_ctx *x = ctx;
    if (slash >= 0 && slash < 5) { x->slash_poly_on[slash] = poly_on != 0; x->slash_tink_on[slash] = clash_tink_on != 0; }
    world_slash_set_enabled(x->w, slash, poly_on != 0, clash_tink_on != 0);
}

void fsmi_set_record(void *ctx, int on) { fsm_ctx *x = ctx; x->w->log_enabled = on != 0; }

/* the component lifecycle (sim/core/sim_modules.h, sim/fsm/lifecycle.h) */
void fsmi_lc_bind(void *ctx, const hksim_lc_core *core) { fsm_ctx *x = ctx; lc_bind_core(x->w, core); }
void fsmi_lc_frame(void *ctx, int begin, uint32_t frame, int live, float time)
{
    fsm_ctx *x = ctx;
    x->w->time = time;                                             /* Time.time: the core's clock */
    if (begin) lc_frame_begin(x->w, frame, live != 0); else lc_frame_end(x->w);
}
void fsmi_lc_stage(void *ctx, int stage, float dt, int flags)
{
    fsm_ctx *x = ctx;
    if (stage == LCS_PHYSICS) lc_stage_enter(x->w, stage);           /* the core steps physics itself */
    else lc_stage(x->w, stage, dt, flags);
}
void fsmi_lc_late_gate(void *ctx, int closed) { fsm_ctx *x = ctx; lc_set_late_gate(x->w, closed != 0); }
void fsmi_lc_delayed_end(void *ctx) { fsm_ctx *x = ctx; lc_delayed_end(x->w); }
