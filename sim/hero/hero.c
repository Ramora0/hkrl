/* HeroController motion / input / collision.  HC:<n> = analysis/decomp/Assembly-CSharp/HeroController.cs:<n>
 * (1.5.78.11833, analysis/specs/hero-motion.md).  Paths without trace coverage are marked UNVERIFIED or trap. */
#include <math.h>
#include <string.h>
#include "hero/hero.h"
#include "hero/hero_internal.h"
#include "core/trap.h"

#define MATHF_EPSILON 1.401298E-45f   /* UnityEngine.Mathf.Epsilon == float.Epsilon (single.Epsilon) */
#define LAYER_TERRAIN_MASK 256u       /* 1 << 8 (PhysLayers.TERRAIN), HC:4317 et al. */

/* ---- rb2d / transform access through the vtable ------------------------------------------------------- */
phys_v2 hero_vel(hero *h) { phys_v2 v; h->ops.get_vel(h->ops.ctx, &v); return v; }
void hero_set_vel(hero *h, float x, float y) { phys_v2 v = { x, y }; h->ops.set_vel(h->ops.ctx, &v); }
phys_v2 hero_pos(hero *h) { phys_v2 p; h->ops.get_pos(h->ops.ctx, &p); return p; }
float hero_scale_x(hero *h) { return h->ops.get_scale_x(h->ops.ctx); }
void hero_set_scale_x(hero *h, float sx) { h->ops.set_scale_x(h->ops.ctx, sx); }
float hero_gravity(hero *h) { return h->ops.get_gravity(h->ops.ctx); }
void hero_set_gravity(hero *h, float g) { h->ops.set_gravity(h->ops.ctx, g); }

void hero_fsm_event(hero *h, int fsm, const char *ev)
{
    if (h->hooks.fsm_send_event) h->hooks.fsm_send_event(h->hooks.ctx, fsm, ev);
    else h->dropped_fsm_events++;
}
void hero_effect(hero *h, const char *name)   /* prefab spawn / SetActive / audio / vibration: cosmetic */
{
    if (h->hooks.effect) h->hooks.effect(h->hooks.ctx, name);
    else h->dropped_effects++;
}
static void anim_update_state(hero *h, int s) { hero_anim_update_state(h, s); }   /* animCtrl.UpdateState (hero_anim.c) */

/* HeroAudioController.PlaySound (analysis/decomp/Assembly-CSharp/HeroAudioController.cs:43-101): nothing agent-visible.
 * RandomizePitch's Random.Range (:180-184) only aligned the shared stream and is not drawn.  isPaused guard :45-48. */
void hero_audio_play_sound(hero *h, int sound)
{
    (void)sound;
    if (h->cs.isPaused) return;
    hero_effect(h, "audio");
}

/* col2d.bounds of the body BoxCollider2D: center = position + offset, size = size (edgeRadius NOT included:
 * dumps/GG_Hornet_1/physics.json#heroColliders[0].bounds.size == .size).  offset.x scales with localScale.x. */
hero_bounds hero_col_bounds(hero *h)
{
    /* native Unity Bounds (Collider2D.bounds, no C# decomp): float32 per operation (docs/float-parity.md, native code) */
    hero_bounds b;
    phys_v2 p = hero_pos(h);
    float sx = hero_scale_x(h);
    b.center.x = p.x + h->col_offset.x * sx;
    b.center.y = p.y + h->col_offset.y;
    b.extents.x = h->col_size.x * 0.5f;
    b.extents.y = h->col_size.y * 0.5f;
    b.size = h->col_size;
    b.min.x = b.center.x - b.extents.x; b.min.y = b.center.y - b.extents.y;
    b.max.x = b.center.x + b.extents.x; b.max.y = b.center.y + b.extents.y;
    return b;
}
static int raycast(hero *h, float ox, float oy, float dx, float dy, float len, hero_hit *hit)
{
    phys_v2 o = { ox, oy }, d = { dx, dy };
    hero_hit tmp;
    if (!hit) hit = &tmp;
    memset(hit, 0, sizeof *hit);
    return h->ops.raycast(h->ops.ctx, &o, &d, len, LAYER_TERRAIN_MASK, hit);
}

void hero_bind(hero *h, const hero_phys_ops *ops, const hero_hooks *hooks)
{
    if (ops) h->ops = *ops;
    if (hooks) h->hooks = *hooks;
    hero_anim_play_idle(h); /* cite: analysis/dumps/GG_Hornet_1/physics.json#heroAnimator (currentClip: "Idle") */
}
void hero_bind_tk2d(hero *h, const hero_tk2d_ops *tk)
{
    if (tk) h->tk = *tk;
}
void hero_set_clock(hero *h, uint32_t frameCount, float deltaTime, float timeSinceLevelLoad)
{
    if (frameCount != h->frameCount) { h->co.phase_ran = 0; h->co.after_env_ran = 0; }
    h->frameCount = frameCount;
    h->deltaTime = deltaTime;
    h->timeSinceLevelLoad = timeSinceLevelLoad;
    if (!h->anim.initial_clip_set) {
        h->anim.initial_clip_set = 1;
        hero_anim_prime_scene_ready(h);
    }
}

/* ================================================================================================= */
/* Update (HC:904-908) -> orig_Update (HC:5106-5426)                                                  */
/* ================================================================================================= */
static void update10(hero *h)   /* HC:1212-1231 */
{
    /* OutOfBoundsCheck (HC:3912-3922): the only effect is `_ = boundsChecking` -> no-op */
    float scaleX = hero_scale_x(h);                  /* HC:1218 */
    if (scaleX < -1.0f) hero_set_scale_x(h, -1.0f);  /* HC:1219-1222 */
    if (scaleX > 1.0f) hero_set_scale_x(h, 1.0f);    /* HC:1223-1226 */
    /* HC:1227-1230 transform.SetPositionZ(0.004f): z is not modelled (2D physics) */
}

static void fall_check(hero *h)   /* HC:3853-3910 */
{
    if (hero_vel(h).y <= -1E-06f) {                                       /* HC:3855 */
        if (hero_check_touching_ground(h)) return;                        /* HC:3857-3860 */
        h->cs.falling = 1;                                                /* HC:3861 */
        h->cs.onGround = 0;                                               /* HC:3862 */
        h->cs.wallJumping = 0;                                            /* HC:3863 */
        hero_fsm_event(h, FSM_PROXY, "HeroCtrl-LeftGround");              /* HC:3864 */
        if (h->f.hero_state != AS_no_input) hero_set_state(h, AS_airborne);   /* HC:3865-3868 */
        if (h->cs.wallSliding) h->f.fallTimer = 0.0f;                     /* HC:3869-3872 */
        else h->f.fallTimer += h->deltaTime;                              /* HC:3875 */
        if (h->f.fallTimer > h->f.BIG_FALL_TIME) {                        /* HC:3877 */
            if (!h->cs.willHardLand) h->cs.willHardLand = 1;              /* HC:3879-3882 */
            if (!h->f.fallRumble) hero_start_fall_rumble(h);              /* HC:3883-3886 */
        }
        if (h->f.fallCheckFlagged) h->f.fallCheckFlagged = 0;             /* HC:3888-3891 */
    } else {
        h->cs.falling = 0;                                                /* HC:3895 */
        h->f.fallTimer = 0.0f;                                            /* HC:3896 */
        if (h->f.transitionState != HTS_ENTERING_SCENE) h->cs.willHardLand = 0;   /* HC:3897-3900 */
        if (h->f.fallCheckFlagged) h->f.fallCheckFlagged = 0;             /* HC:3901-3904 */
        if (h->f.fallRumble) hero_cancel_fall_effects(h);                 /* HC:3905-3908 */
    }
}

static void fail_safe_checks(hero *h)   /* HC:3946-3998 */
{
    if (h->f.hero_state == AS_hard_landing) {                              /* HC:3948 UNVERIFIED (no hard landing in corpus) */
        h->f.hardLandFailSafeTimer += h->deltaTime;                        /* HC:3950 */
        if ((double)h->f.hardLandFailSafeTimer > (double)h->f.HARD_LANDING_TIME + (double)0.3f) {  /* HC:3951 (double stack) */
            hero_set_state(h, AS_grounded);                                /* HC:3953 */
            hero_back_on_ground(h);                                        /* HC:3954 */
            h->f.hardLandFailSafeTimer = 0.0f;                             /* HC:3955 */
        }
    } else {
        h->f.hardLandFailSafeTimer = 0.0f;                                 /* HC:3960 */
    }
    if (h->cs.hazardDeath) {                                               /* HC:3962 UNVERIFIED */
        h->f.hazardDeathTimer += h->deltaTime;                             /* HC:3964 */
        if (h->f.hazardDeathTimer > h->f.HAZARD_DEATH_CHECK_TIME && h->f.hero_state != AS_no_input) {   /* HC:3965 */
            hero_reset_motion(h);                                          /* HC:3967 */
            hero_affected_by_gravity(h, 0);                                /* HC:3968 */
            hero_set_state(h, AS_no_input);                                /* HC:3969 */
            h->f.hazardDeathTimer = 0.0f;                                  /* HC:3970 */
        }
    } else {
        h->f.hazardDeathTimer = 0.0f;                                      /* HC:3975 */
    }
    if (hero_vel(h).y != 0.0f || h->cs.onGround || h->cs.falling || h->cs.jumping || h->cs.dashing
        || h->f.hero_state == AS_hard_landing || h->f.hero_state == AS_no_input) {   /* HC:3977 */
        return;
    }
    if (hero_check_touching_ground(h)) {                                   /* HC:3981 */
        h->f.floatingBufferTimer += h->deltaTime;                          /* HC:3983 */
        if (h->f.floatingBufferTimer > h->f.FLOATING_CHECK_TIME) {         /* HC:3984 */
            if (h->cs.recoiling) hero_cancel_damage_recoil(h);             /* HC:3986-3989 */
            hero_back_on_ground(h);                                        /* HC:3990 */
            h->f.floatingBufferTimer = 0.0f;                               /* HC:3991 */
        }
    } else {
        h->f.floatingBufferTimer = 0.0f;                                   /* HC:3996 */
    }
}

static void look_for_input(hero *h)   /* HC:3256-3328 */
{
    if (!h->f.acceptingInput || h->gm_isPaused || !h->f.isGameplayScene) return;   /* HC:3258-3261 */
    h->f.move_input = h->in.mv_x;                                     /* HC:3262 moveVector.Vector.x */
    h->f.vertical_input = h->in.mv_y;                                 /* HC:3263 */
    hero_filter_input(h);                                             /* HC:3264 */
    if (h->pd.hasWalljump && hero_can_wall_slide(h) && !h->cs.attacking) {   /* HC:3265 UNVERIFIED (Q-hero-2) */
        if (h->f.touchingWallL && hero_pa_is_pressed(h, PA_LEFT) && !h->cs.wallSliding) {   /* HC:3267 */
            h->f.airDashed = 0;                                       /* HC:3269 */
            h->f.doubleJumped = 0;                                    /* HC:3270 */
            hero_effect(h, "wallSlideVibrationPlayer.Play");          /* HC:3271 */
            h->cs.wallSliding = 1;                                    /* HC:3272 */
            h->cs.willHardLand = 0;                                   /* HC:3273 */
            hero_effect(h, "wallslideDust.enableEmission=true");      /* HC:3274 */
            h->f.wallSlidingL = 1;                                    /* HC:3275 */
            h->f.wallSlidingR = 0;                                    /* HC:3276 */
            hero_face_left(h);                                        /* HC:3277 */
            hero_cancel_fall_effects(h);                              /* HC:3278 */
        }
        if (h->f.touchingWallR && hero_pa_is_pressed(h, PA_RIGHT) && !h->cs.wallSliding) {   /* HC:3280 */
            h->f.airDashed = 0;
            h->f.doubleJumped = 0;
            hero_effect(h, "wallSlideVibrationPlayer.Play");
            h->cs.wallSliding = 1;                                    /* HC:3285 */
            h->cs.willHardLand = 0;
            hero_effect(h, "wallslideDust.enableEmission=true");
            h->f.wallSlidingL = 0;                                    /* HC:3288 */
            h->f.wallSlidingR = 1;                                    /* HC:3289 */
            hero_face_right(h);                                       /* HC:3290 */
            hero_cancel_fall_effects(h);                              /* HC:3291 */
        }
    }
    if (h->cs.wallSliding && hero_pa_was_pressed(h, PA_DOWN)) {       /* HC:3294 */
        hero_cancel_wallsliding(h);                                   /* HC:3296 */
        hero_flip_sprite(h);                                          /* HC:3297 */
    }
    if (h->f.wallLocked && h->f.wallJumpedL && hero_pa_is_pressed(h, PA_RIGHT) && h->f.wallLockSteps >= h->f.WJLOCK_STEPS_SHORT) {   /* HC:3299 */
        h->f.wallLocked = 0;                                          /* HC:3301 */
    }
    if (h->f.wallLocked && h->f.wallJumpedR && hero_pa_is_pressed(h, PA_LEFT) && h->f.wallLockSteps >= h->f.WJLOCK_STEPS_SHORT) {   /* HC:3303 */
        h->f.wallLocked = 0;                                          /* HC:3305 */
    }
    if (hero_pa_was_released(h, PA_JUMP) && h->f.jumpReleaseQueueingEnabled) {   /* HC:3307 */
        h->f.jumpReleaseQueueSteps = h->f.JUMP_RELEASE_QUEUE_STEPS;   /* HC:3309 */
        h->f.jumpReleaseQueuing = 1;                                  /* HC:3310 */
    }
    if (!hero_pa_is_pressed(h, PA_JUMP)) hero_jump_released(h);       /* HC:3312-3315 */
    if (!hero_pa_is_pressed(h, PA_DASH)) {                            /* HC:3316 */
        if (h->cs.preventDash && !h->cs.dashCooldown) h->cs.preventDash = 0;   /* HC:3318-3321 */
        h->f.dashQueuing = 0;                                         /* HC:3322 */
    }
    if (!hero_pa_is_pressed(h, PA_ATTACK)) h->f.attackQueuing = 0;    /* HC:3324-3327 */
}

static void look_for_queue_input(hero *h)   /* HC:3330-3423 */
{
    if (!h->f.acceptingInput || h->gm_isPaused || !h->f.isGameplayScene) return;   /* HC:3332-3335 */
    if (hero_pa_was_pressed(h, PA_JUMP)) {                            /* HC:3336 */
        if (hero_can_wall_jump(h)) hero_do_wall_jump(h);              /* HC:3338-3341 */
        else if (hero_can_jump(h)) hero_hero_jump(h);                 /* HC:3342-3345 */
        else if (hero_can_double_jump(h)) hero_do_double_jump(h);     /* HC:3346-3349 */
        else if (hero_can_infinite_air_jump(h)) {                     /* HC:3350 UNVERIFIED (infiniteAirJump false) */
            hero_cancel_jump(h);                                      /* HC:3352 */
            hero_audio_play_sound(h, HSND_JUMP);                      /* HC:3353 (one RNG draw) */
            hero_reset_look(h);                                       /* HC:3354 */
            h->cs.jumping = 1;                                        /* HC:3355 */
        } else {
            h->f.jumpQueueSteps = 0;                                  /* HC:3359 */
            h->f.jumpQueuing = 1;                                     /* HC:3360 */
            h->f.doubleJumpQueueSteps = 0;                            /* HC:3361 */
            h->f.doubleJumpQueuing = 1;                               /* HC:3362 */
        }
    }
    if (hero_pa_was_pressed(h, PA_DASH) /* && !ModHooks.OnDashPressed(): no subscriber in the oracle */) {   /* HC:3365 */
        if (hero_can_dash(h)) hero_hero_dash(h);                      /* HC:3367-3370 */
        else {
            h->f.dashQueueSteps = 0;                                  /* HC:3373 */
            h->f.dashQueuing = 1;                                     /* HC:3374 */
        }
    }
    if (hero_pa_was_pressed(h, PA_ATTACK)) {                          /* HC:3377 */
        if (hero_can_attack(h)) hero_do_attack(h);                    /* HC:3379-3382 */
        else {
            h->f.attackQueueSteps = 0;                                /* HC:3385 */
            h->f.attackQueuing = 1;                                   /* HC:3386 */
        }
    }
    if (hero_pa_is_pressed(h, PA_JUMP)) {                             /* HC:3389 */
        if (h->f.jumpQueueSteps <= h->f.JUMP_QUEUE_STEPS && hero_can_jump(h) && h->f.jumpQueuing) {   /* HC:3391 */
            hero_hero_jump(h);                                        /* HC:3393 */
        } else if (h->f.doubleJumpQueueSteps <= h->f.DOUBLE_JUMP_QUEUE_STEPS && hero_can_double_jump(h) && h->f.doubleJumpQueuing) {   /* HC:3395 */
            if (h->cs.onGround) hero_hero_jump(h);                    /* HC:3397-3400 */
            else hero_do_double_jump(h);                              /* HC:3403 */
        }
        if (hero_can_swim(h)) {                                       /* HC:3406 UNVERIFIED (inAcid never true) */
            if (h->f.hero_state != AS_airborne) hero_set_state(h, AS_airborne);   /* HC:3408-3411 */
            h->cs.swimming = 1;                                       /* HC:3412 */
        }
    }
    if (hero_pa_is_pressed(h, PA_DASH) && h->f.dashQueueSteps <= h->f.DASH_QUEUE_STEPS && hero_can_dash(h)
        && h->f.dashQueuing && hero_can_dash(h)) {                    /* HC:3415 (ModHooks.OnDashPressed: no subscriber) */
        hero_hero_dash(h);                                            /* HC:3417 */
    }
    if (hero_pa_is_pressed(h, PA_ATTACK) && h->f.attackQueueSteps <= h->f.ATTACK_QUEUE_STEPS && hero_can_attack(h) && h->f.attackQueuing) {   /* HC:3419 */
        hero_do_attack(h);                                            /* HC:3421 */
    }
}

static void orig_update(hero *h)   /* HC:5106-5426 */
{
    if (h->frameCount % 10u == 0u) update10(h);                       /* HC:5108-5111 */
    h->f.current_velocity = hero_vel(h);                              /* HC:5112 */
    fall_check(h);                                                    /* HC:5113 */
    fail_safe_checks(h);                                              /* HC:5114 */
    if (h->f.hero_state == AS_running && !h->cs.dashing && !h->cs.backDashing && !h->f.controlReqlinquished) {   /* HC:5115 */
        /* HC:5117-5126 footstep audio: cosmetic */
        phys_v2 v = hero_vel(h);
        if (h->f.runMsgSent && v.x > -0.1f && v.x < 0.1f) {          /* HC:5127 */
            hero_fsm_event(h, FSM_RUN_EFFECT, "RUN STOP");            /* HC:5129 */
            hero_effect(h, "runEffect.SetParent(null)");              /* HC:5130 */
            h->f.runMsgSent = 0;                                      /* HC:5131 */
        }
        v = hero_vel(h);
        if (!h->f.runMsgSent && (v.x < -0.1f || v.x > 0.1f)) {        /* HC:5133 */
            hero_effect(h, "runEffectPrefab.Spawn");                  /* HC:5135-5136 */
            h->f.runMsgSent = 1;                                      /* HC:5137 */
        }
    } else {
        /* HC:5142-5143 audio: cosmetic */
        if (h->f.runMsgSent) {                                        /* HC:5144 */
            hero_fsm_event(h, FSM_RUN_EFFECT, "RUN STOP");            /* HC:5146 */
            hero_effect(h, "runEffect.SetParent(null)");              /* HC:5147 */
            h->f.runMsgSent = 0;                                      /* HC:5148 */
        }
    }
    if (h->f.hero_state == AS_dash_landing) {                         /* HC:5151 UNVERIFIED (charm 31 not equipped) */
        h->f.dashLandingTimer += h->deltaTime;                        /* HC:5153 */
        if (h->f.dashLandingTimer > h->f.DOWN_DASH_TIME) hero_back_on_ground(h);   /* HC:5154-5157 */
    }
    if (h->f.hero_state == AS_hard_landing) {                         /* HC:5159 UNVERIFIED */
        h->f.hardLandingTimer += h->deltaTime;                        /* HC:5161 */
        if (h->f.hardLandingTimer > h->f.HARD_LANDING_TIME) {         /* HC:5162 */
            hero_set_state(h, AS_grounded);                           /* HC:5164 */
            hero_back_on_ground(h);                                   /* HC:5165 */
        }
    } else if (h->f.hero_state == AS_no_input) {                      /* HC:5168 */
        if (h->cs.recoiling) {                                        /* HC:5170 */
            if ((!h->pd.equippedCharm_4 && h->f.recoilTimer < h->f.RECOIL_DURATION)
                || (h->pd.equippedCharm_4 && h->f.recoilTimer < h->f.RECOIL_DURATION_STAL)) {   /* HC:5172 */
                h->f.recoilTimer += h->deltaTime;                     /* HC:5174 */
            } else {
                hero_cancel_damage_recoil(h);                         /* HC:5178 */
                if ((h->f.prev_hero_state == AS_idle || h->f.prev_hero_state == AS_running) && !hero_check_touching_ground(h)) {   /* HC:5179 */
                    h->cs.onGround = 0;                               /* HC:5181 */
                    hero_set_state(h, AS_airborne);                   /* HC:5182 */
                } else {
                    hero_set_state(h, AS_previous);                   /* HC:5186 */
                }
                hero_fsm_event(h, FSM_THORN_COUNTER, "THORN COUNTER");   /* HC:5188 */
            }
        }
    } else if (h->f.hero_state != AS_no_input) {                      /* HC:5192 */
        look_for_input(h);                                            /* HC:5194 */
        if (h->cs.recoiling) {                                        /* HC:5195 */
            h->cs.recoiling = 0;                                      /* HC:5197 */
            hero_affected_by_gravity(h, 1);                           /* HC:5198 */
        }
        if (h->cs.attacking && !h->cs.dashing) {                      /* HC:5200 */
            h->f.attack_time += h->deltaTime;                         /* HC:5202 */
            if (h->f.attack_time >= h->f.attackDuration) {            /* HC:5203 */
                hero_reset_attacks(h);                                /* HC:5205 */
                hero_anim_stop_attack(h);                             /* HC:5206 animCtrl.StopAttack() */
            }
        }
        if (h->cs.bouncing) {                                         /* HC:5209 UNVERIFIED */
            if (h->f.bounceTimer < h->f.BOUNCE_TIME) h->f.bounceTimer += h->deltaTime;   /* HC:5211-5214 */
            else {
                hero_cancel_bounce(h);                                /* HC:5217 */
                hero_set_vel(h, hero_vel(h).x, 0.0f);                 /* HC:5218 */
            }
        }
        if (h->cs.shroomBouncing && h->f.current_velocity.y <= 0.0f) h->cs.shroomBouncing = 0;   /* HC:5221-5224 */
        if (h->f.hero_state == AS_idle) {                             /* HC:5225 */
            if (!h->f.controlReqlinquished && !h->gm_isPaused) {      /* HC:5227 */
                if (hero_pa_is_pressed(h, PA_UP) || hero_pa_is_pressed(h, PA_RS_UP)) {   /* HC:5229 */
                    h->cs.lookingDown = 0;                            /* HC:5231 */
                    h->cs.lookingDownAnim = 0;                        /* HC:5232 */
                    if (h->f.lookDelayTimer >= h->f.LOOK_DELAY || (hero_pa_is_pressed(h, PA_RS_UP) && !h->cs.jumping && !h->cs.dashing)) {   /* HC:5233 */
                        h->cs.lookingUp = 1;                          /* HC:5235 */
                    } else {
                        h->f.lookDelayTimer += h->deltaTime;          /* HC:5239 */
                    }
                    if (h->f.lookDelayTimer >= h->f.LOOK_ANIM_DELAY || hero_pa_is_pressed(h, PA_RS_UP)) h->cs.lookingUpAnim = 1;   /* HC:5241-5244 */
                    else h->cs.lookingUpAnim = 0;                     /* HC:5247 */
                } else if (hero_pa_is_pressed(h, PA_DOWN) || hero_pa_is_pressed(h, PA_RS_DOWN)) {   /* HC:5250 */
                    h->cs.lookingUp = 0;                              /* HC:5252 */
                    h->cs.lookingUpAnim = 0;                          /* HC:5253 */
                    if (h->f.lookDelayTimer >= h->f.LOOK_DELAY || (hero_pa_is_pressed(h, PA_RS_DOWN) && !h->cs.jumping && !h->cs.dashing)) {   /* HC:5254 */
                        h->cs.lookingDown = 1;                        /* HC:5256 */
                    } else {
                        h->f.lookDelayTimer += h->deltaTime;          /* HC:5260 */
                    }
                    if (h->f.lookDelayTimer >= h->f.LOOK_ANIM_DELAY || hero_pa_is_pressed(h, PA_RS_DOWN)) h->cs.lookingDownAnim = 1;   /* HC:5262-5265 */
                    else h->cs.lookingDownAnim = 0;                   /* HC:5268 */
                } else {
                    hero_reset_look(h);                               /* HC:5273 */
                }
            }
            h->f.runPuffTimer = 0.0f;                                 /* HC:5276 */
        }
    }
    look_for_queue_input(h);                                          /* HC:5279 */
    if (h->f.drainMP) {                                               /* HC:5280 UNVERIFIED (focus never completes in corpus) */
        h->f.drainMP_timer += h->deltaTime;                           /* HC:5282 */
        h->f.drainMP_seconds += h->deltaTime;                         /* HC:5283 */
        while (h->f.drainMP_timer >= h->f.drainMP_time) {             /* HC:5284 */
            h->f.MP_drained += 1.0f;                                  /* HC:5286 */
            h->f.drainMP_timer -= h->f.drainMP_time;                  /* HC:5287 */
            hero_take_mp(h, 1);                                       /* HC:5288 */
            hero_fsm_event(h, FSM_SOUL_ORB, "MP DRAIN");              /* HC:5289 */
            if (h->f.MP_drained == h->f.focusMP_amount) {             /* HC:5290 */
                h->f.MP_drained -= h->f.drainMP_time;                 /* HC:5292 (as written: damage-path.md 8.6) */
                hero_fsm_event(h, FSM_PROXY, "HeroCtrl-FocusCompleted");   /* HC:5293 */
            }
        }
    }
    if (h->cs.wallSliding) {                                          /* HC:5297 UNVERIFIED */
        if (h->f.airDashed) h->f.airDashed = 0;                       /* HC:5299-5302 */
        if (h->f.doubleJumped) h->f.doubleJumped = 0;                 /* HC:5303-5306 */
        if (h->cs.onGround) {                                         /* HC:5307 */
            hero_flip_sprite(h);                                      /* HC:5309 */
            hero_cancel_wallsliding(h);                               /* HC:5310 */
        }
        if (!h->cs.touchingWall) {                                    /* HC:5312 */
            hero_flip_sprite(h);                                      /* HC:5314 */
            hero_cancel_wallsliding(h);                               /* HC:5315 */
        }
        if (!hero_can_wall_slide(h)) hero_cancel_wallsliding(h);      /* HC:5317-5320 */
        if (!h->f.playedMantisClawClip) {                             /* HC:5321 */
            hero_effect(h, "audio mantisClawClip");                   /* HC:5323 */
            h->f.playedMantisClawClip = 1;                            /* HC:5324 */
        }
        if (!h->f.playingWallslideClip) {                             /* HC:5326 */
            if (h->f.wallslideClipTimer <= h->f.WALLSLIDE_CLIP_DELAY) {   /* HC:5328 */
                h->f.wallslideClipTimer += h->deltaTime;              /* HC:5330 */
            } else {
                h->f.wallslideClipTimer = 0.0f;                       /* HC:5334 */
                hero_effect(h, "audio WALLSLIDE");                    /* HC:5335 */
                h->f.playingWallslideClip = 1;                        /* HC:5336 */
            }
        }
    } else if (h->f.playedMantisClawClip) {                           /* HC:5340 */
        h->f.playedMantisClawClip = 0;                                /* HC:5342 */
    }
    if (!h->cs.wallSliding && h->f.playingWallslideClip) {            /* HC:5344 */
        hero_effect(h, "audio stop WALLSLIDE");                       /* HC:5346 */
        h->f.playingWallslideClip = 0;                                /* HC:5347 */
    }
    if (!h->cs.wallSliding && h->f.wallslideClipTimer > 0.0f) h->f.wallslideClipTimer = 0.0f;   /* HC:5349-5352 */
    if (h->f.wallSlashing && !h->cs.wallSliding) hero_cancel_attack(h);   /* HC:5353-5356 */
    if (h->f.attack_cooldown > 0.0f) h->f.attack_cooldown -= h->deltaTime;   /* HC:5357-5360 */
    if (h->f.dashCooldownTimer > 0.0f) h->f.dashCooldownTimer -= h->deltaTime;   /* HC:5361-5364 */
    if (h->f.shadowDashTimer > 0.0f) {                                /* HC:5365 */
        h->f.shadowDashTimer -= h->deltaTime;                         /* HC:5367 */
        if (h->f.shadowDashTimer <= 0.0f) hero_effect(h, "spriteFlash.FlashShadowRecharge");   /* HC:5368-5371 */
    }
    h->f.preventCastByDialogueEndTimer -= h->deltaTime;               /* HC:5373 */
    if (!h->gm_isPaused) {                                            /* HC:5374 */
        if (hero_pa_is_pressed(h, PA_ATTACK) && hero_can_nail_charge(h)) {   /* HC:5376 */
            h->cs.nailCharging = 1;                                   /* HC:5378 */
            h->f.nailChargeTimer += h->deltaTime;                     /* HC:5379 */
        } else if (h->cs.nailCharging || h->f.nailChargeTimer != 0.0f) {   /* HC:5381 */
            hero_effect(h, "artChargeEffect.SetActive(false)");       /* HC:5383 */
            h->cs.nailCharging = 0;                                   /* HC:5384 */
            /* HC:5385 audio stop: cosmetic */
        }
        /* HC:5387-5411: effect activation is cosmetic except HC:5405, which reads artChargedEffect.activeSelf
           (h->artChargedActive) */
        if (h->cs.nailCharging && h->f.nailChargeTimer > 0.5f && !h->artChargeActive && h->f.nailChargeTimer < h->f.nailChargeTime) {   /* HC:5387 */
            h->artChargeActive = 1;                                   /* HC:5389 */
        }
        if (h->artChargeActive && (!h->cs.nailCharging || h->f.nailChargeTimer > h->f.nailChargeTime)) {   /* HC:5392 */
            h->artChargeActive = 0;                                   /* HC:5394 */
        }
        if (!h->artChargedActive && h->f.nailChargeTimer >= h->f.nailChargeTime) {   /* HC:5397 */
            h->artChargedActive = 1;                                  /* HC:5399 */
            hero_effect(h, "artChargedFlash");                        /* HC:5400-5401 */
            hero_fsm_event(h, FSM_CAMERA_SHAKE, "EnemyKillShake");    /* HC:5402 */
            /* HC:5403-5404 audio: cosmetic */
            h->cs.nailCharging = 1;                                   /* HC:5405 */
        }
        if (h->artChargedActive && (h->f.nailChargeTimer < h->f.nailChargeTime || !h->cs.nailCharging)) {   /* HC:5407 */
            h->artChargedActive = 0;                                  /* HC:5409 */
        }
    }
    if (h->gm_isPaused && !hero_pa_is_pressed(h, PA_ATTACK)) {        /* HC:5413 */
        h->cs.nailCharging = 0;                                       /* HC:5415 */
        h->f.nailChargeTimer = 0.0f;                                  /* HC:5416 */
    }
    if (h->cs.swimming && !hero_can_swim(h)) h->cs.swimming = 0;      /* HC:5418-5421 */
    if (h->f.parryInvulnTimer > 0.0f) h->f.parryInvulnTimer -= h->deltaTime;   /* HC:5422-5425 */
}

void hero_update(hero *h)   /* HC:904-908: ModHooks.OnHeroUpdate() has no subscriber in the oracle */
{
    orig_update(h);
    hero_anim_update(h);
}

/* ================================================================================================= */
/* FixedUpdate (HC:910-1210)                                                                          */
/* ================================================================================================= */
void hero_fixed_update(hero *h)
{
    if (h->cs.recoilingLeft || h->cs.recoilingRight) {                /* HC:912 UNVERIFIED (Q-hero-2) */
        if ((float)h->f.recoilSteps <= h->f.RECOIL_HOR_STEPS) h->f.recoilSteps++;   /* HC:914-916 */
        else hero_cancel_recoil_horizontal(h);                        /* HC:920 */
    }
    if (h->cs.dead) hero_set_vel(h, 0.0f, 0.0f);                      /* HC:923-926 */
    if ((h->f.hero_state == AS_hard_landing && !h->cs.onConveyor) || h->f.hero_state == AS_dash_landing) {   /* HC:927 */
        hero_reset_motion(h);                                         /* HC:929 */
    } else if (h->f.hero_state == AS_no_input) {                      /* HC:931 */
        if (h->cs.transitioning) {                                    /* HC:933 UNVERIFIED (scene transitions never happen in-episode) */
            if (h->f.transitionState == HTS_EXITING_SCENE) {          /* HC:935 */
                hero_affected_by_gravity(h, 0);                       /* HC:937 */
                if (!h->f.stopWalkingOut) {                           /* HC:938 */
                    phys_v2 v = hero_vel(h);
                    hero_set_vel(h, h->f.transition_vel.x, h->f.transition_vel.y + v.y);   /* HC:940 */
                }
            } else if (h->f.transitionState == HTS_ENTERING_SCENE) {  /* HC:943 */
                hero_set_vel(h, h->f.transition_vel.x, h->f.transition_vel.y);   /* HC:945 */
            } else if (h->f.transitionState == HTS_DROPPING_DOWN) {   /* HC:947 */
                hero_set_vel(h, h->f.transition_vel.x, hero_vel(h).y);   /* HC:949 */
            }
        } else if (h->cs.recoiling) {                                 /* HC:952 */
            hero_affected_by_gravity(h, 0);                           /* HC:954 */
            hero_set_vel(h, h->f.recoilVector.x, h->f.recoilVector.y);   /* HC:955 */
        }
    } else if (h->f.hero_state != AS_no_input) {                      /* HC:958 */
        if (h->f.hero_state == AS_running) {                          /* HC:960 */
            if (h->f.move_input > 0.0f) {                             /* HC:962 */
                if (hero_check_for_bump(h, CS_right)) hero_set_vel(h, hero_vel(h).x, h->f.BUMP_VELOCITY);   /* HC:964-967 */
            } else if (h->f.move_input < 0.0f && hero_check_for_bump(h, CS_left)) {   /* HC:969 */
                hero_set_vel(h, hero_vel(h).x, h->f.BUMP_VELOCITY);   /* HC:971 */
            }
        }
        if (!h->cs.backDashing && !h->cs.dashing) {                   /* HC:974 */
            hero_move(h, h->f.move_input);                            /* HC:976 */
            if ((!h->cs.attacking || !(h->f.attack_time < h->f.ATTACK_RECOVERY_TIME)) && !h->cs.wallSliding && !h->f.wallLocked) {   /* HC:977 */
                if (h->f.move_input > 0.0f && !h->cs.facingRight) {   /* HC:979 */
                    hero_flip_sprite(h);                              /* HC:981 */
                    hero_cancel_attack(h);                            /* HC:982 */
                } else if (h->f.move_input < 0.0f && h->cs.facingRight) {   /* HC:984 */
                    hero_flip_sprite(h);                              /* HC:986 */
                    hero_cancel_attack(h);                            /* HC:987 */
                }
            }
            if (h->cs.recoilingLeft) {                                /* HC:990 UNVERIFIED */
                float num = (!h->f.recoilLarge) ? h->f.RECOIL_HOR_VELOCITY : h->f.RECOIL_HOR_VELOCITY_LONG;   /* HC:992 */
                phys_v2 v = hero_vel(h);
                if (v.x > 0.0f - num) hero_set_vel(h, 0.0f - num, v.y);   /* HC:993-996 */
                else hero_set_vel(h, v.x - num, v.y);                 /* HC:999 */
            }
            if (h->cs.recoilingRight) {                               /* HC:1002 UNVERIFIED */
                float num2 = (!h->f.recoilLarge) ? h->f.RECOIL_HOR_VELOCITY : h->f.RECOIL_HOR_VELOCITY_LONG;   /* HC:1004 */
                phys_v2 v = hero_vel(h);
                if (v.x < num2) hero_set_vel(h, num2, v.y);           /* HC:1005-1008 */
                else hero_set_vel(h, v.x + num2, v.y);                /* HC:1011 */
            }
        }
        if ((h->cs.lookingUp || h->cs.lookingDown) && fabsf(h->f.move_input) > 0.6f) hero_reset_look(h);   /* HC:1015-1018 */
        if (h->cs.jumping) hero_jump(h);                              /* HC:1019-1022 */
        if (h->cs.doubleJumping) hero_double_jump(h);                 /* HC:1023-1026 */
        if (h->cs.dashing) hero_dash(h);                              /* HC:1027-1030 */
        if (h->cs.casting) {                                          /* HC:1031 UNVERIFIED (Spell Control FSM sets casting) */
            if (h->cs.castRecoiling) {                                /* HC:1033 */
                if (h->cs.facingRight) hero_set_vel(h, 0.0f - h->f.CAST_RECOIL_VELOCITY, 0.0f);   /* HC:1035-1038 */
                else hero_set_vel(h, h->f.CAST_RECOIL_VELOCITY, 0.0f);   /* HC:1041 */
            } else {
                hero_set_vel(h, 0.0f, 0.0f);                          /* HC:1046 */
            }
        }
        if (h->cs.bouncing) hero_set_vel(h, hero_vel(h).x, h->f.BOUNCE_VELOCITY);   /* HC:1049-1052 UNVERIFIED */
        /* HC:1053 `_ = cState.shroomBouncing;` no-op */
        if (h->f.wallLocked) {                                        /* HC:1054 UNVERIFIED */
            if (h->f.wallJumpedR) hero_set_vel(h, h->f.currentWalljumpSpeed, hero_vel(h).y);   /* HC:1056-1059 */
            else if (h->f.wallJumpedL) hero_set_vel(h, 0.0f - h->f.currentWalljumpSpeed, hero_vel(h).y);   /* HC:1060-1063 */
            h->f.wallLockSteps++;                                     /* HC:1064 */
            if (h->f.wallLockSteps > h->f.WJLOCK_STEPS_LONG) h->f.wallLocked = 0;   /* HC:1065-1068 */
            h->f.currentWalljumpSpeed -= h->f.walljumpSpeedDecel;     /* HC:1069 */
        }
        if (h->cs.wallSliding) {                                      /* HC:1071 UNVERIFIED */
            if (h->f.wallSlidingL && hero_pa_is_pressed(h, PA_RIGHT)) h->f.wallUnstickSteps++;   /* HC:1073-1076 */
            else if (h->f.wallSlidingR && hero_pa_is_pressed(h, PA_LEFT)) h->f.wallUnstickSteps++;   /* HC:1077-1080 */
            else h->f.wallUnstickSteps = 0;                           /* HC:1083 */
            if (h->f.wallUnstickSteps >= h->f.WALL_STICKY_STEPS) hero_cancel_wallsliding(h);   /* HC:1085-1088 */
            if (h->f.wallSlidingL) {                                  /* HC:1089 */
                if (!hero_check_still_touching_wall(h, CS_left, 0)) { /* HC:1091 */
                    hero_flip_sprite(h);                              /* HC:1093 */
                    hero_cancel_wallsliding(h);                       /* HC:1094 */
                }
            } else if (h->f.wallSlidingR && !hero_check_still_touching_wall(h, CS_right, 0)) {   /* HC:1097 */
                hero_flip_sprite(h);                                  /* HC:1099 */
                hero_cancel_wallsliding(h);                           /* HC:1100 */
            }
        }
    }
    if (hero_vel(h).y < 0.0f - h->f.MAX_FALL_VELOCITY && !h->f.inAcid && !h->f.controlReqlinquished
        && !h->cs.shadowDashing && !h->cs.spellQuake) {               /* HC:1104 */
        hero_set_vel(h, hero_vel(h).x, 0.0f - h->f.MAX_FALL_VELOCITY);   /* HC:1106 */
    }
    if (h->f.jumpQueuing) h->f.jumpQueueSteps++;                      /* HC:1108-1111 */
    if (h->f.doubleJumpQueuing) h->f.doubleJumpQueueSteps++;          /* HC:1112-1115 */
    if (h->f.dashQueuing) h->f.dashQueueSteps++;                      /* HC:1116-1119 */
    if (h->f.attackQueuing) h->f.attackQueueSteps++;                  /* HC:1120-1123 */
    if (h->cs.wallSliding && !h->cs.onConveyorV) {                    /* HC:1124 UNVERIFIED (Q-hero-1) */
        phys_v2 v = hero_vel(h);
        if (v.y > h->f.WALLSLIDE_SPEED) {                             /* HC:1126 */
            hero_set_vel(h, v.x, v.y - h->f.WALLSLIDE_DECEL);         /* HC:1128 */
            v = hero_vel(h);
            if (v.y < h->f.WALLSLIDE_SPEED) hero_set_vel(h, v.x, h->f.WALLSLIDE_SPEED);   /* HC:1129-1132 */
        }
        v = hero_vel(h);
        if (v.y < h->f.WALLSLIDE_SPEED) {                             /* HC:1134 */
            hero_set_vel(h, v.x, v.y + h->f.WALLSLIDE_DECEL);         /* HC:1136 */
            v = hero_vel(h);
            if (v.y < h->f.WALLSLIDE_SPEED) hero_set_vel(h, v.x, h->f.WALLSLIDE_SPEED);   /* HC:1137-1140 */
        }
    }
    if (h->f.nailArt_cyclone) {                                       /* HC:1143 (Nail Arts FSM StartCyclone/EndCyclone) */
        if (hero_pa_is_pressed(h, PA_RIGHT) && !hero_pa_is_pressed(h, PA_LEFT)) {   /* HC:1145 */
            hero_set_vel(h, h->f.CYCLONE_HORIZONTAL_SPEED, hero_vel(h).y);   /* HC:1147 */
        } else if (hero_pa_is_pressed(h, PA_LEFT) && !hero_pa_is_pressed(h, PA_RIGHT)) {   /* HC:1149 */
            hero_set_vel(h, 0.0f - h->f.CYCLONE_HORIZONTAL_SPEED, hero_vel(h).y);   /* HC:1151 */
        } else {
            hero_set_vel(h, 0.0f, hero_vel(h).y);                     /* HC:1155 */
        }
    }
    if (h->cs.swimming) {                                             /* HC:1158 UNVERIFIED */
        phys_v2 v = hero_vel(h);
        hero_set_vel(h, v.x, v.y + h->f.SWIM_ACCEL);                  /* HC:1160 */
        v = hero_vel(h);
        if (v.y > h->f.SWIM_MAX_SPEED) hero_set_vel(h, v.x, h->f.SWIM_MAX_SPEED);   /* HC:1161-1164 */
    }
    if (h->cs.superDashOnWall && !h->cs.onConveyorV) hero_set_vel(h, 0.0f, 0.0f);   /* HC:1166-1169 UNVERIFIED */
    if (h->cs.onConveyor && ((h->cs.onGround && !h->cs.superDashing) || h->f.hero_state == AS_hard_landing)) {   /* HC:1170 UNVERIFIED */
        if (h->cs.freezeCharge || h->f.hero_state == AS_hard_landing || h->f.controlReqlinquished) hero_set_vel(h, 0.0f, 0.0f);   /* HC:1172-1175 */
        phys_v2 v = hero_vel(h);
        hero_set_vel(h, v.x + h->f.conveyorSpeed, v.y);               /* HC:1176 */
    }
    if (h->cs.inConveyorZone) {                                       /* HC:1178 UNVERIFIED */
        if (h->cs.freezeCharge || h->f.hero_state == AS_hard_landing) hero_set_vel(h, 0.0f, 0.0f);   /* HC:1180-1183 */
        phys_v2 v = hero_vel(h);
        hero_set_vel(h, v.x + h->f.conveyorSpeed, v.y);               /* HC:1184 */
        hero_fsm_event(h, FSM_SUPERDASH, "SLOPE CANCEL");             /* HC:1185 */
    }
    if (h->cs.slidingLeft && hero_vel(h).x > -5.0f) hero_set_vel(h, -5.0f, hero_vel(h).y);   /* HC:1187-1190 UNVERIFIED */
    if (h->f.landingBufferSteps > 0) h->f.landingBufferSteps--;       /* HC:1191-1194 */
    if (h->f.ledgeBufferSteps > 0) h->f.ledgeBufferSteps--;           /* HC:1195-1198 */
    if (h->f.headBumpSteps > 0) h->f.headBumpSteps--;                 /* HC:1199-1202 */
    if (h->f.jumpReleaseQueueSteps > 0) h->f.jumpReleaseQueueSteps--; /* HC:1203-1206 */
    h->f.positionHistory[1] = h->f.positionHistory[0];                /* HC:1207 */
    h->f.positionHistory[0] = hero_pos(h);                            /* HC:1208 transform.position */
    h->cs.wasOnGround = h->cs.onGround;                               /* HC:1209 */
}

/* ================================================================================================= */
/* motion primitives                                                                                  */
/* ================================================================================================= */
void hero_move(hero *h, float move_direction)   /* HC:1249-1278 */
{
    if (h->cs.onGround) hero_set_state(h, AS_grounded);              /* HC:1251-1254 */
    if (h->f.acceptingInput && !h->cs.wallSliding) {                 /* HC:1255 */
        float vy = hero_vel(h).y;
        if (h->cs.inWalkZone) hero_set_vel(h, move_direction * h->f.WALK_SPEED, vy);   /* HC:1257-1260 */
        else if (h->f.inAcid) hero_set_vel(h, move_direction * h->f.UNDERWATER_SPEED, vy);   /* HC:1261-1264 */
        else if (h->pd.equippedCharm_37 && h->cs.onGround && h->pd.equippedCharm_31) hero_set_vel(h, move_direction * h->f.RUN_SPEED_CH_COMBO, vy);   /* HC:1265-1268 */
        else if (h->pd.equippedCharm_37 && h->cs.onGround) hero_set_vel(h, move_direction * h->f.RUN_SPEED_CH, vy);   /* HC:1269-1272 */
        else hero_set_vel(h, move_direction * h->f.RUN_SPEED, vy);   /* HC:1275 */
    }
}

void hero_jump(hero *h)   /* HC:1280-1300 */
{
    if (h->f.jump_steps <= h->f.JUMP_STEPS) {                         /* HC:1282 */
        if (h->f.inAcid) hero_set_vel(h, hero_vel(h).x, h->f.JUMP_SPEED_UNDERWATER);   /* HC:1284-1287 */
        else hero_set_vel(h, hero_vel(h).x, h->f.JUMP_SPEED);         /* HC:1290 */
        h->f.jump_steps++;                                            /* HC:1292 */
        h->f.jumped_steps++;                                          /* HC:1293 */
        h->f.ledgeBufferSteps = 0;                                    /* HC:1294 */
    } else {
        hero_cancel_jump(h);                                          /* HC:1298 */
    }
}

void hero_double_jump(hero *h)   /* HC:1302-1320 */
{
    if (h->f.doubleJump_steps <= h->f.DOUBLE_JUMP_STEPS) {            /* HC:1304 */
        if (h->f.doubleJump_steps > 3) hero_set_vel(h, hero_vel(h).x, (float)((double)h->f.JUMP_SPEED * (double)1.1f));   /* HC:1306-1309 (double stack, float ctor arg) */
        h->f.doubleJump_steps++;                                      /* HC:1310 */
    } else {
        hero_cancel_double_jump(h);                                   /* HC:1314 */
    }
    if (h->cs.onGround) hero_cancel_double_jump(h);                   /* HC:1316-1319 */
}

static phys_v2 orig_dash_vector(hero *h)   /* HC:5428-5433 */
{
    float num = (!h->pd.equippedCharm_16 || !h->cs.shadowDashing) ? h->f.DASH_SPEED : h->f.DASH_SPEED_SHARP;   /* HC:5430 */
    phys_v2 result;
    if (h->f.dashingDown) { result.x = 0.0f; result.y = 0.0f - num; }   /* HC:5431 */
    else if (h->cs.facingRight) {
        if (!hero_check_for_bump(h, CS_right)) { result.x = num; result.y = 0.0f; }
        else { result.x = num; result.y = (!h->cs.onGround) ? 5.0f : 4.0f; }   /* HC:5431 literals (== BUMP_VELOCITY_DASH / BUMP_VELOCITY, HC:5453) */
    } else {
        if (!hero_check_for_bump(h, CS_left)) { result.x = 0.0f - num; result.y = 0.0f; }
        else { result.x = 0.0f - num; result.y = (!h->cs.onGround) ? 5.0f : 4.0f; }   /* HC:5431 */
    }
    return result;
}

void hero_dash(hero *h)   /* HC:1508-1521 (called from FixedUpdate HC:1029) */
{
    hero_affected_by_gravity(h, 0);                                   /* HC:1510 */
    hero_reset_hard_landing_timer(h);                                 /* HC:1511 */
    if (h->f.dash_timer > h->f.DASH_TIME) {                           /* HC:1512 */
        hero_finished_dashing(h);                                     /* HC:1514 */
        return;
    }
    phys_v2 change = orig_dash_vector(h);                             /* HC:1517; ModHooks.DashVelocityChange: no subscriber */
    h->ops.set_vel(h->ops.ctx, &change);                              /* HC:1519 */
    /* HC:1520: Time.deltaTime inside FixedUpdate == fixedDeltaTime == 0.02 under R2 (hero-motion.md 2.3, Q-hero-9) */
    h->f.dash_timer += h->deltaTime;
}

void hero_face_right(hero *h)   /* HC:1535-1541 */
{
    h->cs.facingRight = 1;
    hero_set_scale_x(h, -1.0f);
}
void hero_face_left(hero *h)    /* HC:1543-1549 */
{
    h->cs.facingRight = 0;
    hero_set_scale_x(h, 1.0f);
}
void hero_flip_sprite(hero *h)  /* HC:1785-1791 */
{
    h->cs.facingRight = !h->cs.facingRight;
    hero_set_scale_x(h, hero_scale_x(h) * -1.0f);
}

void hero_set_back_on_ground(hero *h) { h->cs.onGround = 1; }                  /* HC:1562-1565 */
/* HeroController.SetDarkness (HC:1657-1666).  Not cosmetic: wieldingLantern selects the `Lantern Idle` /
 * `Lantern Run` clips (HAC:477-480, :489-491), and the clip is in the observation. */
void hero_set_darkness(hero *h, int darkness)
{
    h->f.wieldingLantern = (darkness > 0 && h->pd.hasLantern) ? 1 : 0;   /* HC:1659-1666 */
}

void hero_set_start_with_wallslide(hero *h) { h->f.startWithWallslide = 1; }   /* HC:1567-1570 */
void hero_set_start_with_jump(hero *h) { h->f.startWithJump = 1; }             /* HC:1572-1575 */
void hero_set_start_with_full_jump(hero *h) { h->f.startWithFullJump = 1; }    /* HC:1577-1580 */
void hero_set_start_with_dash(hero *h) { h->f.startWithDash = 1; }             /* HC:1582-1585 */
void hero_set_start_with_attack(hero *h) { h->f.startWithAttack = 1; }         /* HC:1587-1590 */
void hero_set_super_dash_exit(hero *h) { h->f.exitedSuperDashing = 1; }        /* HC:1592-1595 */
void hero_set_quake_exit(hero *h) { h->f.exitedQuake = 1; }                    /* HC:1597-1600 */
void hero_set_take_no_damage(hero *h) { h->f.takeNoDamage = 1; }               /* HC:1602-1605 */
void hero_end_take_no_damage(hero *h) { h->f.takeNoDamage = 0; }               /* HC:1607-1610 */
void hero_is_swimming(hero *h) { h->cs.swimming = 1; }                         /* HC:1621-1624 */
void hero_not_swimming(hero *h) { h->cs.swimming = 0; }                        /* HC:1626-1629 */
void hero_reset_air_moves(hero *h) { h->f.doubleJumped = 0; h->f.airDashed = 0; }   /* HC:1636-1640 */
void hero_set_conveyor_speed(hero *h, float speed) { h->f.conveyorSpeed = speed; }      /* HC:1642-1645 */
void hero_set_conveyor_speed_v(hero *h, float speed) { h->f.conveyorSpeedV = speed; }   /* HC:1647-1650 */
void hero_enter_without_input(hero *h, int flag) { h->f.enterWithoutInput = (uint8_t)(flag != 0); }   /* HC:1652-1655 */

void hero_cancel_super_dash(hero *h) { hero_fsm_event(h, FSM_SUPERDASH, "SLOPE CANCEL"); }   /* HC:2866-2869 */

void hero_cancel_hero_jump(hero *h)   /* HC:1669-1680 */
{
    if (h->cs.jumping) {
        hero_cancel_jump(h);
        hero_cancel_double_jump(h);
        if (hero_vel(h).y > 0.0f) hero_set_vel(h, hero_vel(h).x, 0.0f);
    }
}

int hero_can_input(const hero *h) { return h->f.acceptingInput; }   /* HC:1770-1773 */
int hero_can_talk(const hero *h)   /* HC:1775-1783 */
{
    return hero_can_input(h) && h->f.hero_state != AS_no_input && !h->f.controlReqlinquished && h->cs.onGround
           && !h->cs.attacking && !h->cs.dashing;
}
void hero_prevent_cast_by_dialogue_end(hero *h) { h->f.preventCastByDialogueEndTimer = 0.3f; }   /* HC:2968-2970 */

void hero_nail_parry(hero *h) { h->f.parryInvulnTimer = h->f.INVUL_TIME_PARRY; }   /* HC:1793-1796 */
void hero_nail_parry_recover(hero *h)   /* HC:1798-1803 */
{
    h->f.attackDuration = 0.0f;
    h->f.attack_cooldown = 0.0f;
    hero_cancel_attack(h);
}
void hero_quake_invuln(hero *h) { h->f.parryInvulnTimer = h->f.INVUL_TIME_QUAKE; }     /* HC:1805-1808 */
void hero_cancel_parry_invuln(hero *h) { h->f.parryInvulnTimer = 0.0f; }               /* HC:1810-1813 */
void hero_cyclone_invuln(hero *h) { h->f.parryInvulnTimer = h->f.INVUL_TIME_CYCLONE; } /* HC:1815-1818 */

/* ---- public velocity pokes (HC:2229-2329) ---------------------------------------------------------- */
void hero_bounce(hero *h)   /* HC:2229-2237 UNVERIFIED */
{
    if (!h->cs.bouncing && !h->cs.shroomBouncing && !h->f.controlReqlinquished) {
        h->f.doubleJumped = 0;
        h->f.airDashed = 0;
        h->cs.bouncing = 1;
    }
}
void hero_bounce_high(hero *h)   /* HC:2239-2249 UNVERIFIED */
{
    if (!h->cs.bouncing && !h->f.controlReqlinquished) {
        h->f.doubleJumped = 0;
        h->f.airDashed = 0;
        h->cs.bouncing = 1;
        h->f.bounceTimer = -0.03f;                                   /* HC:2246 */
        hero_set_vel(h, hero_vel(h).x, h->f.BOUNCE_VELOCITY);
    }
}
void hero_shroom_bounce(hero *h)   /* HC:2251-2258 UNVERIFIED */
{
    h->f.doubleJumped = 0;
    h->f.airDashed = 0;
    h->cs.bouncing = 0;
    h->cs.shroomBouncing = 1;
    hero_set_vel(h, hero_vel(h).x, h->f.SHROOM_BOUNCE_VELOCITY);
}
void hero_recoil_left(hero *h)   /* HC:2260-2271 UNVERIFIED */
{
    if (!h->cs.recoilingLeft && !h->cs.recoilingRight && !h->pd.equippedCharm_14 && !h->f.controlReqlinquished) {
        hero_cancel_dash(h);
        h->f.recoilSteps = 0;
        h->cs.recoilingLeft = 1;
        h->cs.recoilingRight = 0;
        h->f.recoilLarge = 0;
        hero_set_vel(h, 0.0f - h->f.RECOIL_HOR_VELOCITY, hero_vel(h).y);
    }
}
void hero_recoil_right(hero *h)   /* HC:2273-2284 UNVERIFIED */
{
    if (!h->cs.recoilingLeft && !h->cs.recoilingRight && !h->pd.equippedCharm_14 && !h->f.controlReqlinquished) {
        hero_cancel_dash(h);
        h->f.recoilSteps = 0;
        h->cs.recoilingRight = 1;
        h->cs.recoilingLeft = 0;
        h->f.recoilLarge = 0;
        hero_set_vel(h, h->f.RECOIL_HOR_VELOCITY, hero_vel(h).y);
    }
}
void hero_recoil_right_long(hero *h)   /* HC:2286-2298 UNVERIFIED */
{
    if (!h->cs.recoilingLeft && !h->cs.recoilingRight && !h->f.controlReqlinquished) {
        hero_cancel_dash(h);
        hero_reset_attacks(h);
        h->f.recoilSteps = 0;
        h->cs.recoilingRight = 1;
        h->cs.recoilingLeft = 0;
        h->f.recoilLarge = 1;
        hero_set_vel(h, h->f.RECOIL_HOR_VELOCITY_LONG, hero_vel(h).y);
    }
}
void hero_recoil_left_long(hero *h)   /* HC:2300-2312 UNVERIFIED */
{
    if (!h->cs.recoilingLeft && !h->cs.recoilingRight && !h->f.controlReqlinquished) {
        hero_cancel_dash(h);
        hero_reset_attacks(h);
        h->f.recoilSteps = 0;
        h->cs.recoilingRight = 0;
        h->cs.recoilingLeft = 1;
        h->f.recoilLarge = 1;
        hero_set_vel(h, 0.0f - h->f.RECOIL_HOR_VELOCITY_LONG, hero_vel(h).y);
    }
}
void hero_recoil_down(hero *h)   /* HC:2314-2321 UNVERIFIED */
{
    hero_cancel_jump(h);
    if (hero_vel(h).y > h->f.RECOIL_DOWN_VELOCITY && !h->f.controlReqlinquished) {
        hero_set_vel(h, hero_vel(h).x, h->f.RECOIL_DOWN_VELOCITY);
    }
}
void hero_force_hard_landing(hero *h)   /* HC:2323-2329 UNVERIFIED */
{
    if (!h->cs.onGround) h->cs.willHardLand = 1;
}

void hero_start_cyclone(hero *h) { h->f.nailArt_cyclone = 1; }   /* HC:2833-2836 */
void hero_end_cyclone(hero *h) { h->f.nailArt_cyclone = 0; }     /* HC:2838-2841 */

void hero_reset_hard_landing_timer(hero *h)   /* HC:2858-2864 */
{
    h->cs.willHardLand = 0;
    h->f.hardLandingTimer = 0.0f;
    h->f.fallTimer = 0.0f;
    h->f.hardLanded = 0;
}

void hero_relinquish_control_not_velocity(hero *h)   /* HC:2871-2886 */
{
    if (!h->f.controlReqlinquished) {
        h->f.prev_hero_state = AS_idle;
        hero_reset_input(h);
        hero_reset_motion_not_velocity(h);
        hero_set_state(h, AS_no_input);
        hero_ignore_input(h);
        h->f.controlReqlinquished = 1;
        hero_reset_look(h);
        hero_reset_attacks(h);
        h->f.touchingWallL = 0;
        h->f.touchingWallR = 0;
    }
}
void hero_relinquish_control(hero *h)   /* HC:2888-2901 */
{
    if (!h->f.controlReqlinquished && !h->cs.dead) {
        hero_reset_input(h);
        hero_reset_motion(h);
        hero_ignore_input(h);
        h->f.controlReqlinquished = 1;
        hero_reset_look(h);
        hero_reset_attacks(h);
        h->f.touchingWallL = 0;
        h->f.touchingWallR = 0;
    }
}
void hero_regain_control(hero *h)   /* HC:2903-2966 */
{
    h->f.enteringVertically = 0;                                     /* HC:2905 */
    h->f.doubleJumpQueuing = 0;                                      /* HC:2906 */
    hero_accept_input(h);                                            /* HC:2907 */
    h->f.hero_state = AS_idle;                                       /* HC:2908 (direct write, not SetState) */
    if (!h->f.controlReqlinquished || h->cs.dead) return;            /* HC:2909-2912 */
    hero_affected_by_gravity(h, 1);                                  /* HC:2913 */
    hero_set_starting_motion_state(h);                               /* HC:2914 */
    h->f.controlReqlinquished = 0;                                   /* HC:2915 */
    if (h->f.startWithWallslide) {                                   /* HC:2916 UNVERIFIED */
        hero_effect(h, "wallSlideVibrationPlayer.Play");
        h->cs.wallSliding = 1;                                       /* HC:2919 */
        h->cs.willHardLand = 0;                                      /* HC:2920 */
        h->cs.touchingWall = 1;                                      /* HC:2921 */
        h->f.airDashed = 0;                                          /* HC:2922 */
        h->f.startWithWallslide = 0;                                 /* HC:2924 */
        if (hero_scale_x(h) < 0.0f) { h->f.wallSlidingR = 1; h->f.touchingWallR = 1; }   /* HC:2925-2929 */
        else { h->f.wallSlidingL = 1; h->f.touchingWallL = 1; }      /* HC:2932-2933 */
    } else if (h->f.startWithJump) {                                 /* HC:2936 UNVERIFIED */
        hero_hero_jump_no_effect(h);                                 /* HC:2938 */
        h->f.doubleJumpQueuing = 0;
        h->f.startWithJump = 0;
    } else if (h->f.startWithFullJump) {                             /* HC:2942 UNVERIFIED */
        hero_hero_jump(h);                                           /* HC:2944 */
        h->f.doubleJumpQueuing = 0;
        h->f.startWithFullJump = 0;
    } else if (h->f.startWithDash) {                                 /* HC:2948 UNVERIFIED */
        hero_hero_dash(h);                                           /* HC:2950 */
        h->f.doubleJumpQueuing = 0;
        h->f.startWithDash = 0;
    } else if (h->f.startWithAttack) {                               /* HC:2954 UNVERIFIED */
        hero_do_attack(h);                                           /* HC:2956 */
        h->f.doubleJumpQueuing = 0;
        h->f.startWithAttack = 0;
    } else {
        h->cs.touchingWall = 0;                                      /* HC:2962 */
        h->f.touchingWallL = 0;                                      /* HC:2963 */
        h->f.touchingWallR = 0;                                      /* HC:2964 */
    }
}

/* ---- Can* (HC:2973-3072, HC:4717-4830) --------------------------------------------------------------- */
int hero_can_cast(const hero *h)   /* HC:2973-2980 */
{
    return !h->gm_isPaused && !h->cs.dashing && h->f.hero_state != AS_no_input && !h->cs.backDashing
           && (!h->cs.attacking || !(h->f.attack_time < h->f.ATTACK_RECOVERY_TIME)) && !h->cs.recoiling
           && !h->cs.recoilFrozen && !h->cs.transitioning && !h->cs.hazardDeath && !h->cs.hazardRespawning
           && hero_can_input(h) && h->f.preventCastByDialogueEndTimer <= 0.0f;
}
int hero_can_focus(const hero *h)   /* HC:2982-2989 */
{
    return !h->gm_isPaused && h->f.hero_state != AS_no_input && !h->cs.dashing && !h->cs.backDashing
           && (!h->cs.attacking || !(h->f.attack_time < h->f.ATTACK_RECOVERY_TIME)) && !h->cs.recoiling
           && h->cs.onGround && !h->cs.transitioning && !h->cs.recoilFrozen && !h->cs.hazardDeath
           && !h->cs.hazardRespawning && hero_can_input(h);
}
int hero_can_nail_art(hero *h)   /* HC:2991-3000 */
{
    if (!h->cs.transitioning && h->f.hero_state != AS_no_input && !h->cs.attacking && !h->cs.hazardDeath
        && !h->cs.hazardRespawning && h->f.nailChargeTimer >= h->f.nailChargeTime) {
        h->f.nailChargeTimer = 0.0f;
        return 1;
    }
    h->f.nailChargeTimer = 0.0f;
    return 0;
}
int hero_can_quick_map(const hero *h)   /* HC:3002-3009 */
{
    return !h->gm_isPaused && !h->f.controlReqlinquished && h->f.hero_state != AS_no_input && !h->cs.onConveyor
           && !h->cs.dashing && !h->cs.backDashing && (!h->cs.attacking || !(h->f.attack_time < h->f.ATTACK_RECOVERY_TIME))
           && !h->cs.recoiling && !h->cs.transitioning && !h->cs.hazardDeath && !h->cs.hazardRespawning
           && !h->cs.recoilFrozen && h->cs.onGround && hero_can_input(h);
}
int hero_can_inspect(const hero *h)   /* HC:3011-3018 */
{
    return !h->gm_isPaused && !h->cs.dashing && h->f.hero_state != AS_no_input && !h->cs.backDashing
           && (!h->cs.attacking || !(h->f.attack_time < h->f.ATTACK_RECOVERY_TIME)) && !h->cs.recoiling
           && !h->cs.transitioning && !h->cs.hazardDeath && !h->cs.hazardRespawning && !h->cs.recoilFrozen
           && h->cs.onGround && hero_can_input(h);
}
int hero_can_back_dash(const hero *h)   /* HC:3020-3027 */
{
    return !h->gm_isPaused && !h->cs.dashing && h->f.hero_state != AS_no_input && !h->cs.backDashing
           && (!h->cs.attacking || !(h->f.attack_time < h->f.ATTACK_RECOVERY_TIME)) && !h->cs.preventBackDash
           && !h->cs.backDashCooldown && !h->f.controlReqlinquished && !h->cs.recoilFrozen && !h->cs.recoiling
           && !h->cs.transitioning && h->cs.onGround && h->pd.canBackDash;
}
int hero_can_super_dash(const hero *h)   /* HC:3029-3036 */
{
    return !h->gm_isPaused && h->f.hero_state != AS_no_input && !h->cs.dashing && !h->cs.hazardDeath
           && !h->cs.hazardRespawning && !h->cs.backDashing && (!h->cs.attacking || !(h->f.attack_time < h->f.ATTACK_RECOVERY_TIME))
           && !h->cs.slidingLeft && !h->cs.slidingRight && !h->f.controlReqlinquished && !h->cs.recoilFrozen
           && !h->cs.recoiling && !h->cs.transitioning && h->pd.hasSuperDash && (h->cs.onGround || h->cs.wallSliding);
}
int hero_can_dream_nail(const hero *h)   /* HC:3038-3045 */
{
    return !h->gm_isPaused && h->f.hero_state != AS_no_input && !h->cs.dashing && !h->cs.backDashing
           && (!h->cs.attacking || !(h->f.attack_time < h->f.ATTACK_RECOVERY_TIME)) && !h->f.controlReqlinquished
           && !h->cs.hazardDeath && hero_vel((hero *)h).y > -0.1f /* HC:3040 */ && !h->cs.hazardRespawning
           && !h->cs.recoilFrozen && !h->cs.recoiling && !h->cs.transitioning && h->pd.hasDreamNail && h->cs.onGround;
}
int hero_can_dream_gate(const hero *h)   /* HC:3047-3054 */
{
    return !h->gm_isPaused && h->f.hero_state != AS_no_input && !h->cs.dashing && !h->cs.backDashing
           && (!h->cs.attacking || !(h->f.attack_time < h->f.ATTACK_RECOVERY_TIME)) && !h->f.controlReqlinquished
           && !h->cs.hazardDeath && !h->cs.hazardRespawning && !h->cs.recoilFrozen && !h->cs.recoiling
           && !h->cs.transitioning && h->pd.hasDreamGate && h->cs.onGround;
}
int hero_can_interact(const hero *h)   /* HC:3056-3063 */
{
    return hero_can_input(h) && h->f.hero_state != AS_no_input && !h->gm_isPaused && !h->cs.dashing && !h->cs.backDashing
           && !h->cs.attacking && !h->f.controlReqlinquished && !h->cs.hazardDeath && !h->cs.hazardRespawning
           && !h->cs.recoilFrozen && !h->cs.recoiling && !h->cs.transitioning && h->cs.onGround;
}
int hero_can_open_inventory(const hero *h)   /* HC:3065-3072 */
{
    return (!h->gm_isPaused && h->f.hero_state != AS_airborne && !h->f.controlReqlinquished && !h->cs.recoiling
            && !h->cs.transitioning && !h->cs.hazardDeath && !h->cs.hazardRespawning && h->cs.onGround
            && !h->pd.disablePause && !h->cs.dashing && hero_can_input(h)) || h->pd.atBench;
}

void hero_set_damage_mode_int(hero *h, int invincibilityType)   /* HC:3074-3088 == SetDamageModeFSM HC:3090-3104 */
{
    switch (invincibilityType) {
    case 0: h->f.damageMode = DM_FULL_DAMAGE; break;
    case 1: h->f.damageMode = DM_HAZARD_ONLY; break;
    case 2: h->f.damageMode = DM_NO_DAMAGE; break;
    default: break;
    }
}
void hero_reset_quake_damage(hero *h)   /* HC:3106-3112 */
{
    if (h->f.damageMode == DM_HAZARD_ONLY) h->f.damageMode = DM_FULL_DAMAGE;
}
void hero_set_damage_mode(hero *h, int newDamageMode)   /* HC:3114-3125 */
{
    h->f.damageMode = newDamageMode;
    if (newDamageMode == DM_NO_DAMAGE) h->pd.isInvincible = 1;
    else h->pd.isInvincible = 0;
}

void hero_ignore_input(hero *h)   /* HC:3137-3144 */
{
    if (h->f.acceptingInput) {
        h->f.acceptingInput = 0;
        hero_reset_input(h);
    }
}
void hero_ignore_input_without_reset(hero *h)   /* HC:3146-3152 */
{
    if (h->f.acceptingInput) h->f.acceptingInput = 0;
}
void hero_accept_input(hero *h) { h->f.acceptingInput = 1; }   /* HC:3154-3157 */

void hero_pause(hero *h)   /* HC:3159-3165 UNVERIFIED (the env never pauses) */
{
    if (h->f.acceptingInput) h->f.acceptingInput = 0;              /* PauseInput HC:3204-3211 */
    h->f.lastInputState.x = h->f.move_input;
    h->f.lastInputState.y = h->f.vertical_input;
    hero_jump_released(h);                                          /* HC:3163 */
    h->cs.isPaused = 1;                                             /* HC:3164 */
}
void hero_unpause(hero *h)   /* HC:3167-3172 UNVERIFIED */
{
    h->cs.isPaused = 0;
    if (!h->f.controlReqlinquished) {                               /* UnPauseInput HC:3213-3234 */
        if (hero_pa_is_pressed(h, PA_RIGHT)) h->f.move_input = h->f.lastInputState.x;
        else if (hero_pa_is_pressed(h, PA_LEFT)) h->f.move_input = h->f.lastInputState.x;
        else {
            hero_set_vel(h, 0.0f, hero_vel(h).y);
            h->f.move_input = 0.0f;
        }
        h->f.vertical_input = h->f.lastInputState.y;
        h->f.acceptingInput = 1;
    }
}
void hero_near_bench(hero *h, int isNearBench) { h->cs.nearBench = (uint8_t)(isNearBench != 0); }   /* HC:3174-3177 */
void hero_set_walk_zone(hero *h, int inWalkZone) { h->cs.inWalkZone = (uint8_t)(inWalkZone != 0); }  /* HC:3179-3182 */

void hero_reset_state(hero *h)   /* HC:3184-3187 -> HeroControllerStates.Reset (HCS:133-162) */
{
    h->cs.onGround = 0; h->cs.jumping = 0; h->cs.falling = 0; h->cs.dashing = 0; h->cs.backDashing = 0;
    h->cs.touchingWall = 0; h->cs.wallSliding = 0; h->cs.transitioning = 0; h->cs.attacking = 0;
    h->cs.lookingUp = 0; h->cs.lookingDown = 0; h->cs.altAttack = 0; h->cs.upAttacking = 0; h->cs.downAttacking = 0;
    h->cs.bouncing = 0; h->cs.dead = 0; h->cs.hazardDeath = 0; h->cs.willHardLand = 0; h->cs.recoiling = 0;
    h->cs.recoilFrozen = 0; h->cs.invulnerable = 0; h->cs.casting = 0; h->cs.castRecoiling = 0;
    h->cs.preventDash = 0; h->cs.preventBackDash = 0; h->cs.dashCooldown = 0; h->cs.backDashCooldown = 0;
}

void hero_affected_by_gravity(hero *h, int gravityApplies)   /* HC:3241-3254 */
{
    float g = hero_gravity(h);
    if (g > MATHF_EPSILON && !gravityApplies) {                     /* HC:3244 */
        h->f.prevGravityScale = g;                                  /* HC:3246 */
        hero_set_gravity(h, 0.0f);                                  /* HC:3247 */
    } else if (g <= MATHF_EPSILON && gravityApplies) {              /* HC:3249 */
        hero_set_gravity(h, h->f.prevGravityScale);                 /* HC:3251 */
        h->f.prevGravityScale = 0.0f;                               /* HC:3252 */
    }
}

/* ---- jump / dash / attack entry points ------------------------------------------------------------ */
void hero_hero_jump(hero *h)   /* HC:3425-3435 */
{
    hero_effect(h, "jumpEffectPrefab.Spawn");                       /* HC:3427 */
    hero_audio_play_sound(h, HSND_JUMP);                            /* HC:3428 audioCtrl.PlaySound(JUMP): one RNG draw */
    hero_reset_look(h);                                             /* HC:3429 */
    h->cs.recoiling = 0;                                            /* HC:3430 */
    h->cs.jumping = 1;                                              /* HC:3431 */
    h->f.jumpQueueSteps = 0;                                        /* HC:3432 */
    h->f.jumped_steps = 0;                                          /* HC:3433 */
    h->f.doubleJumpQueuing = 0;                                     /* HC:3434 */
}
void hero_hero_jump_no_effect(hero *h)   /* HC:3437-3445 UNVERIFIED */
{
    hero_reset_look(h);
    h->f.jump_steps = 5;
    h->cs.jumping = 1;
    h->f.jumpQueueSteps = 0;
    h->f.jumped_steps = 0;
    h->f.jump_steps = 5;
}
void hero_do_wall_jump(hero *h)   /* HC:3447-3478 UNVERIFIED (Q-hero-2) */
{
    hero_effect(h, "wallPuffPrefab");                               /* HC:3449 */
    hero_audio_play_sound(h, HSND_WALLJUMP);                        /* HC:3450 audioCtrl.PlaySound(WALLJUMP): one RNG draw */
    if (h->f.touchingWallL) {                                       /* HC:3452 */
        hero_face_right(h);                                         /* HC:3454 */
        h->f.wallJumpedR = 1;                                       /* HC:3455 */
        h->f.wallJumpedL = 0;                                       /* HC:3456 */
    } else if (h->f.touchingWallR) {                                /* HC:3458 */
        hero_face_left(h);                                          /* HC:3460 */
        h->f.wallJumpedR = 0;                                       /* HC:3461 */
        h->f.wallJumpedL = 1;                                       /* HC:3462 */
    }
    hero_cancel_wallsliding(h);                                     /* HC:3464 */
    h->cs.touchingWall = 0;                                         /* HC:3465 */
    h->f.touchingWallL = 0;                                         /* HC:3466 */
    h->f.touchingWallR = 0;                                         /* HC:3467 */
    h->f.airDashed = 0;                                             /* HC:3468 */
    h->f.doubleJumped = 0;                                          /* HC:3469 */
    h->f.currentWalljumpSpeed = h->f.WJ_KICKOFF_SPEED;              /* HC:3470 */
    /* HC:3471 `(WJ_KICKOFF_SPEED - RUN_SPEED) / (float)WJLOCK_STEPS_LONG`: double stack, one rounding at the store */
    h->f.walljumpSpeedDecel = (float)(((double)h->f.WJ_KICKOFF_SPEED - (double)h->f.RUN_SPEED) / (double)(float)h->f.WJLOCK_STEPS_LONG);
    hero_fsm_event(h, FSM_DASH_BURST, "CANCEL");                    /* HC:3472 */
    h->cs.jumping = 1;                                              /* HC:3473 */
    h->f.wallLockSteps = 0;                                         /* HC:3474 */
    h->f.wallLocked = 1;                                            /* HC:3475 */
    h->f.jumpQueueSteps = 0;                                        /* HC:3476 */
    h->f.jumped_steps = 0;                                          /* HC:3477 */
}
void hero_do_double_jump(hero *h)   /* HC:3480-3492 */
{
    hero_effect(h, "dJumpWings/dJumpFlash/dJumpFeathers/doubleJumpClip");   /* HC:3482-3487 */
    hero_reset_look(h);                                             /* HC:3487 */
    h->cs.jumping = 0;                                              /* HC:3488 */
    h->cs.doubleJumping = 1;                                        /* HC:3489 */
    h->f.doubleJump_steps = 0;                                      /* HC:3490 */
    h->f.doubleJumped = 1;                                          /* HC:3491 */
}
void hero_do_hard_landing(hero *h)   /* HC:3494-3503 UNVERIFIED */
{
    hero_affected_by_gravity(h, 1);                                 /* HC:3496 */
    hero_reset_input(h);                                            /* HC:3497 */
    hero_set_state(h, AS_hard_landing);                             /* HC:3498 */
    hero_cancel_attack(h);                                          /* HC:3499 */
    h->f.hardLanded = 1;                                            /* HC:3500 */
    hero_audio_play_sound(h, HSND_HARD_LANDING);                    /* HC:3501 (no draw) */
    hero_effect(h, "hardLandingEffectPrefab.Spawn");                /* HC:3502 */
}
void hero_do_attack(hero *h)   /* HC:3505-3509 -> orig_DoAttack HC:5513-5548 (ModHooks.OnDoAttack: no subscriber) */
{
    hero_reset_look(h);                                             /* HC:5515 */
    h->cs.recoiling = 0;                                            /* HC:5516 */
    if (h->pd.equippedCharm_32) h->f.attack_cooldown = h->f.ATTACK_COOLDOWN_TIME_CH;   /* HC:5517-5520 */
    else h->f.attack_cooldown = h->f.ATTACK_COOLDOWN_TIME;          /* HC:5523 */
    if (h->f.vertical_input > MATHF_EPSILON) {                      /* HC:5525 */
        hero_attack(h, AD_upward);                                  /* HC:5527 */
        hero_start_terrain_thunk(h, AD_upward);                     /* HC:5528 */
    } else if (h->f.vertical_input < 0.0f - MATHF_EPSILON) {        /* HC:5530 */
        if (h->f.hero_state != AS_idle && h->f.hero_state != AS_running) {   /* HC:5532 */
            hero_attack(h, AD_downward);                            /* HC:5534 */
            hero_start_terrain_thunk(h, AD_downward);               /* HC:5535 */
        } else {
            hero_attack(h, AD_normal);                              /* HC:5539 */
            hero_start_terrain_thunk(h, AD_normal);                 /* HC:5540 */
        }
    } else {
        hero_attack(h, AD_normal);                                  /* HC:5545 */
        hero_start_terrain_thunk(h, AD_normal);                     /* HC:5546 */
    }
}

void hero_hero_dash(hero *h)   /* HC:3511-3607 */
{
    if (!h->cs.onGround && !h->f.inAcid) h->f.airDashed = 1;        /* HC:3513-3516 */
    hero_reset_attacks_dash(h);                                     /* HC:3517 */
    hero_cancel_bounce(h);                                          /* HC:3518 */
    /* HC:3519-3521 audio: cosmetic */
    hero_reset_look(h);                                             /* HC:3522 */
    h->cs.recoiling = 0;                                            /* HC:3523 */
    if (h->cs.wallSliding) hero_flip_sprite(h);                     /* HC:3524-3527 */
    else if (hero_pa_is_pressed(h, PA_RIGHT)) hero_face_right(h);   /* HC:3528-3531 */
    else if (hero_pa_is_pressed(h, PA_LEFT)) hero_face_left(h);     /* HC:3532-3535 */
    h->cs.dashing = 1;                                              /* HC:3536 */
    h->f.dashQueueSteps = 0;                                        /* HC:3537 */
    if (hero_pa_is_pressed(h, PA_DOWN) && !h->cs.onGround && h->pd.equippedCharm_31
        && !hero_pa_is_pressed(h, PA_LEFT) && !hero_pa_is_pressed(h, PA_RIGHT)) {   /* HC:3539 */
        hero_effect(h, "dashBurst down pose");                      /* HC:3541-3542 */
        h->f.dashingDown = 1;                                       /* HC:3543 */
    } else {
        hero_effect(h, "dashBurst side pose");                      /* HC:3547-3548 */
        h->f.dashingDown = 0;                                       /* HC:3549 */
    }
    if (h->pd.equippedCharm_31) h->f.dashCooldownTimer = h->f.DASH_COOLDOWN_CH;   /* HC:3551-3554 */
    else h->f.dashCooldownTimer = h->f.DASH_COOLDOWN;               /* HC:3557 */
    if (h->pd.hasShadowDash && h->f.shadowDashTimer <= 0.0f) {      /* HC:3559 */
        h->f.shadowDashTimer = h->f.SHADOW_DASH_COOLDOWN;           /* HC:3561 */
        h->cs.shadowDashing = 1;                                    /* HC:3562 */
        /* HC:3563-3571 sharpShadowPrefab / audio: cosmetic */
        if (h->pd.equippedCharm_16) hero_effect(h, "sharpShadowPrefab.SetActive(true)");
    }
    if (h->cs.shadowDashing) {                                      /* HC:3573 */
        hero_effect(h, "shadowdashBurst");                          /* HC:3575-3589 */
        hero_effect(h, "shadowRechargePrefab.SetActive(true)");     /* HC:3590 */
        hero_fsm_event(h, FSM_SHADOW_RECHARGE, "RESET");            /* HC:3591 */
        hero_effect(h, "shadowdashParticles/shadowRing");           /* HC:3592-3594 */
    } else {
        hero_fsm_event(h, FSM_DASH_BURST, "PLAY");                  /* HC:3598 */
        hero_effect(h, "dashParticles");                            /* HC:3599-3600 */
    }
    if (h->cs.onGround && !h->cs.shadowDashing) hero_effect(h, "backDashPrefab.Spawn");   /* HC:3602-3606 */
}

void hero_start_fall_rumble(hero *h)   /* HC:3609-3614 UNVERIFIED */
{
    h->f.fallRumble = 1;
    if (h->hooks.fsm_set_bool) h->hooks.fsm_set_bool(h->hooks.ctx, FSM_CAMERA_SHAKE, "RumblingFall", 1);   /* HC:3613 */
    else h->dropped_fsm_events++;
}
void hero_cancel_fall_effects(hero *h)   /* HC:4104-4109 */
{
    h->f.fallRumble = 0;
    if (h->hooks.fsm_set_bool) h->hooks.fsm_set_bool(h->hooks.ctx, FSM_CAMERA_SHAKE, "RumblingFall", 0);   /* HC:4108 */
    else h->dropped_fsm_events++;
}

void hero_set_state(hero *h, int newState)   /* HC:3616-3633 */
{
    switch (newState) {
    case AS_grounded:
        newState = (!(fabsf(h->f.move_input) > MATHF_EPSILON)) ? AS_idle : AS_running;   /* HC:3621 */
        break;
    case AS_previous:
        newState = h->f.prev_hero_state;                            /* HC:3624 */
        break;
    default: break;
    }
    if (newState != h->f.hero_state) {                              /* HC:3627 */
        h->f.prev_hero_state = h->f.hero_state;                     /* HC:3629 */
        h->f.hero_state = newState;                                 /* HC:3630 */
        anim_update_state(h, newState);                             /* HC:3631 */
    }
}

/* ---- cancels / resets (HC:4013-4170) ---------------------------------------------------------------- */
void hero_cancel_jump(hero *h)   /* HC:4013-4018 */
{
    h->cs.jumping = 0;
    h->f.jumpReleaseQueuing = 0;
    h->f.jump_steps = 0;
}
void hero_cancel_double_jump(hero *h)   /* HC:4020-4024 */
{
    h->cs.doubleJumping = 0;
    h->f.doubleJump_steps = 0;
}
void hero_cancel_dash(hero *h)   /* HC:4026-4044 */
{
    if (h->cs.shadowDashing) h->cs.shadowDashing = 0;               /* HC:4028-4031 */
    h->cs.dashing = 0;                                              /* HC:4032 */
    h->f.dash_timer = 0.0f;                                         /* HC:4033 */
    hero_affected_by_gravity(h, 1);                                 /* HC:4034 */
    hero_effect(h, "sharpShadowPrefab.SetActive(false)/particles off");   /* HC:4035-4043 */
}
void hero_cancel_wallsliding(hero *h)   /* HC:4046-4055 */
{
    hero_effect(h, "wallslideDust off / vibration stop");           /* HC:4048-4049 */
    h->cs.wallSliding = 0;                                          /* HC:4050 */
    h->f.wallSlidingL = 0;                                          /* HC:4051 */
    h->f.wallSlidingR = 0;                                          /* HC:4052 */
    h->f.touchingWallL = 0;                                         /* HC:4053 */
    h->f.touchingWallR = 0;                                         /* HC:4054 */
}
void hero_cancel_back_dash(hero *h)   /* HC:4057-4061 */
{
    h->cs.backDashing = 0;
    h->f.back_dash_timer = 0.0f;
}
void hero_cancel_down_attack(hero *h)   /* HC:4063-4070 */
{
    if (h->cs.downAttacking) {
        hero_slash_cancel_attack(h, h->slashComponent);             /* HC:4067 slashComponent.CancelAttack() */
        hero_reset_attacks(h);                                      /* HC:4068 */
    }
}
void hero_cancel_attack(hero *h)   /* HC:4072-4079 */
{
    if (h->cs.attacking) {
        hero_slash_cancel_attack(h, h->slashComponent);             /* HC:4076 */
        hero_reset_attacks(h);                                      /* HC:4077 */
    }
}
void hero_cancel_attack_msg(hero *h) { hero_cancel_attack(h); }
void hero_cancel_bounce(hero *h)   /* HC:4081-4086 */
{
    h->cs.bouncing = 0;
    h->cs.shroomBouncing = 0;
    h->f.bounceTimer = 0.0f;
}
void hero_cancel_recoil_horizontal(hero *h)   /* HC:4088-4093 */
{
    h->cs.recoilingLeft = 0;
    h->cs.recoilingRight = 0;
    h->f.recoilSteps = 0;
}
void hero_cancel_damage_recoil(hero *h)   /* HC:4095-4102 */
{
    h->cs.recoiling = 0;                                            /* HC:4097 */
    h->f.recoilTimer = 0.0f;                                        /* HC:4098 */
    hero_reset_motion(h);                                           /* HC:4099 */
    hero_affected_by_gravity(h, 1);                                 /* HC:4100 */
    hero_set_damage_mode(h, DM_FULL_DAMAGE);                        /* HC:4101 */
}
void hero_reset_attacks(hero *h)   /* HC:4111-4119 */
{
    h->cs.nailCharging = 0;
    h->f.nailChargeTimer = 0.0f;
    h->cs.attacking = 0;
    h->cs.upAttacking = 0;
    h->cs.downAttacking = 0;
    h->f.attack_time = 0.0f;
}
void hero_reset_attacks_dash(hero *h)   /* HC:4121-4127 */
{
    h->cs.attacking = 0;
    h->cs.upAttacking = 0;
    h->cs.downAttacking = 0;
    h->f.attack_time = 0.0f;
}
void hero_reset_motion(hero *h)   /* HC:4129-4142 */
{
    hero_cancel_jump(h);
    hero_cancel_double_jump(h);
    hero_cancel_dash(h);
    hero_cancel_back_dash(h);
    hero_cancel_bounce(h);
    hero_cancel_recoil_horizontal(h);
    hero_cancel_wallsliding(h);
    hero_set_vel(h, 0.0f, 0.0f);                                    /* HC:4138 */
    h->f.transition_vel.x = 0.0f; h->f.transition_vel.y = 0.0f;     /* HC:4139 */
    h->f.wallLocked = 0;                                            /* HC:4140 */
    h->f.nailChargeTimer = 0.0f;                                    /* HC:4141 */
}
void hero_reset_motion_not_velocity(hero *h)   /* HC:4144-4155 */
{
    hero_cancel_jump(h);
    hero_cancel_double_jump(h);
    hero_cancel_dash(h);
    hero_cancel_back_dash(h);
    hero_cancel_bounce(h);
    hero_cancel_recoil_horizontal(h);
    hero_cancel_wallsliding(h);
    h->f.transition_vel.x = 0.0f; h->f.transition_vel.y = 0.0f;
    h->f.wallLocked = 0;
}
void hero_reset_look(hero *h)   /* HC:4157-4164 */
{
    h->cs.lookingUp = 0;
    h->cs.lookingDown = 0;
    h->cs.lookingUpAnim = 0;
    h->cs.lookingDownAnim = 0;
    h->f.lookDelayTimer = 0.0f;
}
void hero_reset_input(hero *h)   /* HC:4166-4170 */
{
    h->f.move_input = 0.0f;
    h->f.vertical_input = 0.0f;
}

void hero_back_on_ground(hero *h)   /* HC:4172-4202 */
{
    if (h->f.landingBufferSteps <= 0) {                             /* HC:4174 */
        h->f.landingBufferSteps = h->f.LANDING_BUFFER_STEPS;        /* HC:4176 */
        if (!h->cs.onGround && !h->f.hardLanded && !h->cs.superDashing) hero_effect(h, "softLandingEffectPrefab.Spawn");   /* HC:4177-4181 */
    }
    h->cs.falling = 0;                                              /* HC:4183 */
    h->f.fallTimer = 0.0f;                                          /* HC:4184 */
    h->f.dashLandingTimer = 0.0f;                                   /* HC:4185 */
    h->cs.willHardLand = 0;                                         /* HC:4186 */
    h->f.hardLandingTimer = 0.0f;                                   /* HC:4187 */
    h->f.hardLanded = 0;                                            /* HC:4188 */
    h->f.jump_steps = 0;                                            /* HC:4189 */
    if (h->cs.doubleJumping) hero_hero_jump(h);                     /* HC:4190-4193 */
    hero_set_state(h, AS_grounded);                                 /* HC:4194 */
    h->cs.onGround = 1;                                             /* HC:4195 */
    h->f.airDashed = 0;                                             /* HC:4196 */
    h->f.doubleJumped = 0;                                          /* HC:4197 */
    /* HC:4198-4201 dJumpWingsPrefab.SetActive(false): cosmetic */
}

void hero_jump_released(hero *h)   /* HC:4204-4228 */
{
    if (hero_vel(h).y > 0.0f && h->f.jumped_steps >= h->f.JUMP_STEPS_MIN && !h->f.inAcid && !h->cs.shroomBouncing) {   /* HC:4206 */
        if (h->f.jumpReleaseQueueingEnabled) {                      /* HC:4208 (false: hero.json#jumpReleaseQueueingEnabled) */
            if (h->f.jumpReleaseQueuing && h->f.jumpReleaseQueueSteps <= 0) {   /* HC:4210 */
                hero_set_vel(h, hero_vel(h).x, 0.0f);               /* HC:4212 */
                hero_cancel_jump(h);                                /* HC:4213 */
            }
        } else {
            hero_set_vel(h, hero_vel(h).x, 0.0f);                   /* HC:4218 */
            hero_cancel_jump(h);                                    /* HC:4219 */
        }
    }
    h->f.jumpQueuing = 0;                                           /* HC:4222 */
    h->f.doubleJumpQueuing = 0;                                     /* HC:4223 */
    if (h->cs.swimming) h->cs.swimming = 0;                         /* HC:4224-4227 */
}

void hero_finished_dashing(hero *h)   /* HC:4230-4255 */
{
    hero_cancel_dash(h);                                            /* HC:4232 */
    hero_affected_by_gravity(h, 1);                                 /* HC:4233 */
    hero_anim_finished_dash(h);                                     /* HC:4234 animCtrl.FinishedDash() */
    hero_fsm_event(h, FSM_PROXY, "HeroCtrl-DashEnd");               /* HC:4235 */
    if (h->cs.touchingWall && !h->cs.onGround && (h->pd.hasWalljump & (h->f.touchingWallL || h->f.touchingWallR))) {   /* HC:4236 UNVERIFIED */
        hero_effect(h, "wallslideDust on / vibration play");        /* HC:4238-4239 */
        h->cs.wallSliding = 1;                                      /* HC:4240 */
        h->cs.willHardLand = 0;                                     /* HC:4241 */
        if (h->f.touchingWallL) h->f.wallSlidingL = 1;              /* HC:4242-4245 */
        if (h->f.touchingWallR) h->f.wallSlidingR = 1;              /* HC:4246-4249 */
        if (h->f.dashingDown) hero_flip_sprite(h);                  /* HC:4250-4253 */
    }
}

void hero_set_starting_motion_state(hero *h)   /* HC:4257-4260 -> HC:4262-4284 with preventRunDip=false */
{
    h->f.move_input = (h->f.acceptingInput /* || preventRunDip */) ? h->in.mv_x : 0.0f;   /* HC:4264 moveVector.X, unfiltered */
    h->cs.touchingWall = 0;                                         /* HC:4265 */
    if (hero_check_touching_ground(h)) {                            /* HC:4266 */
        h->cs.onGround = 1;                                         /* HC:4268 */
        hero_set_state(h, AS_grounded);                             /* HC:4269 */
        hero_reset_air_moves(h);                                    /* HC:4270 */
        if (h->f.enteringVertically) {                              /* HC:4271 */
            hero_effect(h, "SpawnSoftLandingPrefab");               /* HC:4273 */
            hero_anim_set_play_landing(h, 1);                       /* HC:4274 animCtrl.playLanding = true */
            h->f.enteringVertically = 0;                            /* HC:4275 */
        }
    } else {
        h->cs.onGround = 0;                                         /* HC:4280 */
        hero_set_state(h, AS_airborne);                             /* HC:4281 */
    }
    anim_update_state(h, h->f.hero_state);                          /* HC:4283 */
}

void hero_filter_input(hero *h)   /* HC:5039-5065 */
{
    if (h->f.move_input > 0.3f) h->f.move_input = 1.0f;             /* HC:5041-5043 */
    else if (h->f.move_input < -0.3f) h->f.move_input = -1.0f;      /* HC:5045-5047 */
    else h->f.move_input = 0.0f;                                    /* HC:5051 */
    if (h->f.vertical_input > 0.5f) h->f.vertical_input = 1.0f;     /* HC:5053-5055 */
    else if (h->f.vertical_input < -0.5f) h->f.vertical_input = -1.0f;   /* HC:5057-5059 */
    else h->f.vertical_input = 0.0f;                                /* HC:5063 */
}

void hero_charm_update(hero *h)   /* HC:1682-1687 -> orig_CharmUpdate HC:5471-5511 (ModHooks.OnCharmUpdate: no subscriber) */
{
    if (h->pd.equippedCharm_26) h->f.nailChargeTime = h->f.NAIL_CHARGE_TIME_CHARM;   /* HC:5473-5476 */
    else h->f.nailChargeTime = h->f.NAIL_CHARGE_TIME_DEFAULT;       /* HC:5479 */
    if (h->pd.equippedCharm_23 && !h->pd.brokenCharm_23) {          /* HC:5481 */
        h->pd.maxHealth = h->pd.maxHealthBase + 2;                  /* HC:5483 */
        hero_max_health(h);                                         /* HC:5484 */
    } else {
        h->pd.maxHealth = h->pd.maxHealthBase;                      /* HC:5488 */
        hero_max_health(h);                                         /* HC:5489 */
    }
    if (h->pd.equippedCharm_27) {                                   /* HC:5491 */
        h->pd.joniHealthBlue = (int32_t)((double)(float)h->pd.maxHealth * (double)1.4f);   /* HC:5493 `(int)((float)maxHealth * 1.4f)`: double stack, conv.i4 */
        h->pd.maxHealth = 1;                                        /* HC:5494 */
        hero_max_health(h);                                         /* HC:5495 */
        h->f.joniBeam = 1;                                          /* HC:5496 */
    } else {
        h->pd.joniHealthBlue = 0;                                   /* HC:5500 */
    }
    if (h->pd.equippedCharm_40 && h->pd.grimmChildLevel == 5) h->f.carefreeShieldEquipped = 1;   /* HC:5502-5505 */
    else h->f.carefreeShieldEquipped = 0;                           /* HC:5508 */
    hero_pd_update_blue_health(h);                                  /* HC:5510 */
    hero_pd_update_blue_health(h);                                  /* HC:1686 CharmUpdate -> playerData.UpdateBlueHealth() again */
}

/* ---- Attack (HC:1322-1506) --------------------------------------------------------------------------- */
void hero_attack(hero *h, int attackDir)
{
    /* ModHooks.OnAttack (HC:1324): no subscriber */
    /* HC:1325 `Time.timeSinceLevelLoad - altAttackTime > ALT_ATTACK_RESET`: compared in double (docs/float-parity.md "Mono evaluation stack") */
    if ((double)h->timeSinceLevelLoad - (double)h->f.altAttackTime > (double)h->f.ALT_ATTACK_RESET) h->cs.altAttack = 0;   /* HC:1325-1328 */
    h->cs.attacking = 1;                                            /* HC:1329 */
    if (h->pd.equippedCharm_32) h->f.attackDuration = h->f.ATTACK_DURATION_CH;   /* HC:1330-1333 */
    else h->f.attackDuration = h->f.ATTACK_DURATION;                /* HC:1336 */
    if (h->cs.wallSliding) {                                        /* HC:1338 UNVERIFIED */
        h->f.wallSlashing = 1;                                      /* HC:1340 */
        h->slashComponent = SLASH_WALL;                             /* HC:1341-1342 */
    } else {
        h->f.wallSlashing = 0;                                      /* HC:1346 */
        switch (attackDir) {
        case AD_normal:                                             /* HC:1349 */
            if (!h->cs.altAttack) {                                 /* HC:1350 */
                h->slashComponent = SLASH_NORMAL;                   /* HC:1352-1353 */
                h->cs.altAttack = 1;                                /* HC:1354 */
            } else {
                h->slashComponent = SLASH_ALT;                      /* HC:1358-1359 */
                h->cs.altAttack = 0;                                /* HC:1360 */
            }
            if (!h->pd.equippedCharm_35) break;                     /* HC:1362-1365 */
            /* HC:1366-1403 Grubberfly beams (charm 35 not equipped in this save): prefab spawns only */
            hero_effect(h, "grubberFlyBeam");
            break;
        case AD_upward:                                             /* HC:1405 */
            h->slashComponent = SLASH_UP;                           /* HC:1406-1407 */
            h->cs.upAttacking = 1;                                  /* HC:1408 */
            if (!h->pd.equippedCharm_35) break;
            hero_effect(h, "grubberFlyBeamU");                      /* HC:1413-1432 */
            break;
        case AD_downward:                                           /* HC:1434 */
            h->slashComponent = SLASH_DOWN;                         /* HC:1435-1436 */
            h->cs.downAttacking = 1;                                /* HC:1437 */
            if (!h->pd.equippedCharm_35) break;
            hero_effect(h, "grubberFlyBeamD");                      /* HC:1442-1461 */
            break;
        default: break;
        }
    }
    {
        float direction;
        if (h->cs.wallSliding) direction = h->cs.facingRight ? 180.0f : 0.0f;                 /* HC:1465-1474 */
        else if (attackDir == AD_normal && h->cs.facingRight) direction = 0.0f;              /* HC:1476-1478 */
        else if (attackDir == AD_normal && !h->cs.facingRight) direction = 180.0f;           /* HC:1480-1482 */
        else if (attackDir == AD_upward) direction = 90.0f;                                   /* HC:1488-1489 */
        else if (attackDir == AD_downward) direction = 270.0f;                                /* HC:1491-1492 */
        else direction = h->slash[h->slashComponent].fsmDirection;
        h->slash[h->slashComponent].fsmDirection = direction;
        if (h->hooks.slash_fsm_set_direction) h->hooks.slash_fsm_set_direction(h->hooks.ctx, h->slashComponent, direction);
    }
    h->f.altAttackTime = h->timeSinceLevelLoad;                     /* HC:1496 */
    /* ModHooks.AfterAttack (HC:1497): no subscriber */
    if (h->cs.attacking) {                                          /* HC:1498 */
        hero_slash_start(h, h->slashComponent);                     /* HC:1500 slashComponent.StartSlash() */
        if (h->pd.equippedCharm_38) hero_fsm_event(h, FSM_ORBIT_SHIELD, "SLASH");   /* HC:1501-1504 (Q-hero-13) */
    }
}

/* ---- raycast probes (HC:4427-4619) ---------------------------------------------------------------- */
int hero_check_still_touching_wall(hero *h, int side, int checkTop)   /* HC:4427-4525 */
{
    hero_bounds b = hero_col_bounds(h);
    float distance = 0.1f;                                          /* HC:4435 */
    hero_hit r1 = {0}, r2 = {0}, r3 = {0};
    int hit1 = 0, hit2 = 0, hit3 = 0;
    switch (side) {
    case CS_left:                                                   /* HC:4444 */
        if (checkTop) hit1 = raycast(h, b.min.x, b.max.y, -1.0f, 0.0f, distance, &r1);   /* HC:4447 */
        hit2 = raycast(h, b.min.x, b.center.y, -1.0f, 0.0f, distance, &r2);              /* HC:4449 */
        hit3 = raycast(h, b.min.x, b.min.y, -1.0f, 0.0f, distance, &r3);                 /* HC:4450 */
        break;
    case CS_right:                                                  /* HC:4452 */
        if (checkTop) hit1 = raycast(h, b.max.x, b.max.y, 1.0f, 0.0f, distance, &r1);    /* HC:4455 */
        hit2 = raycast(h, b.max.x, b.center.y, 1.0f, 0.0f, distance, &r2);               /* HC:4457 */
        hit3 = raycast(h, b.max.x, b.min.y, 1.0f, 0.0f, distance, &r3);                  /* HC:4458 */
        break;
    default:
        return 0;                                                   /* HC:4461-4462 */
    }
    if (hit2) {                                                     /* HC:4464 */
        int flag2 = 1;
        if (r2.flags & HIT_TRIGGER) flag2 = 0;                      /* HC:4467-4470 */
        if (r2.flags & HIT_STEEP_SLOPE) flag2 = 0;                  /* HC:4471-4474 */
        if (r2.flags & HIT_NON_SLIDER) flag2 = 0;                   /* HC:4475-4478 */
        if (flag2) return 1;                                        /* HC:4479-4482 */
    }
    if (hit3) {                                                     /* HC:4484 */
        int flag3 = 1;
        if (r3.flags & HIT_TRIGGER) flag3 = 0;
        if (r3.flags & HIT_STEEP_SLOPE) flag3 = 0;
        if (r3.flags & HIT_NON_SLIDER) flag3 = 0;
        if (flag3) return 1;                                        /* HC:4499-4502 */
    }
    if (checkTop && hit1) {                                         /* HC:4504 */
        int flag = 1;
        if (r1.flags & HIT_TRIGGER) flag = 0;
        if (r1.flags & HIT_STEEP_SLOPE) flag = 0;
        if (r1.flags & HIT_NON_SLIDER) flag = 0;
        if (flag) return 1;                                         /* HC:4519-4522 */
    }
    return 0;                                                       /* HC:4524 */
}

int hero_check_for_bump(const hero *hc, int side)   /* HC:4527-4579 */
{
    hero *h = (hero *)hc;
    hero_bounds b = hero_col_bounds(h);
    float num = 0.025f;                                             /* HC:4529 */
    float num2 = 0.2f;                                              /* HC:4530 */
    phys_v2 vector = { b.min.x + num2, b.min.y + 0.2f };            /* HC:4531 */
    phys_v2 vector2 = { b.min.x + num2, b.min.y - num };            /* HC:4532 */
    phys_v2 vector3 = { b.max.x - num2, b.min.y + 0.2f };           /* HC:4533 */
    phys_v2 vector4 = { b.max.x - num2, b.min.y - num };            /* HC:4534 */
    float num3 = 0.32f + num2;                                      /* HC:4535 */
    hero_hit rh = {0}, rh2 = {0};
    int hit = 0, hit2 = 0;
    switch (side) {
    case CS_left:                                                   /* HC:4540 */
        hit2 = raycast(h, vector2.x, vector2.y, -1.0f, 0.0f, num3, &rh2);   /* HC:4543 */
        hit = raycast(h, vector.x, vector.y, -1.0f, 0.0f, num3, &rh);       /* HC:4544 */
        break;
    case CS_right:                                                  /* HC:4546 */
        hit2 = raycast(h, vector4.x, vector4.y, 1.0f, 0.0f, num3, &rh2);    /* HC:4549 */
        hit = raycast(h, vector3.x, vector3.y, 1.0f, 0.0f, num3, &rh);      /* HC:4550 */
        break;
    default:
        break;                                                      /* HC:4552-4554 */
    }
    if (hit2 && !hit) {                                             /* HC:4556 */
        phys_v2 vector5 = { rh2.point.x + ((side == CS_right) ? 0.1f : -0.1f), rh2.point.y + 1.0f };   /* HC:4558 */
        hero_hit rh3 = {0}, rh4 = {0};
        int hit3 = raycast(h, vector5.x, vector5.y, 0.0f, -1.0f, 1.5f, &rh3);   /* HC:4559 */
        phys_v2 vector6 = { rh2.point.x + ((side == CS_right) ? -0.1f : 0.1f), rh2.point.y + 1.0f };   /* HC:4560 */
        int hit4 = raycast(h, vector6.x, vector6.y, 0.0f, -1.0f, 1.5f, &rh4);   /* HC:4561 */
        if (hit3) {                                                 /* HC:4562 */
            if (!hit4) return 1;                                    /* HC:4565-4568 */
            float num4 = rh3.point.y - rh4.point.y;                 /* HC:4570 */
            if (num4 > 0.0f) return 1;                              /* HC:4571-4575 */
        }
    }
    return 0;                                                       /* HC:4578 */
}

int hero_check_near_roof(const hero *hc)   /* HC:4581-4600 */
{
    hero *h = (hero *)hc;
    hero_bounds b = hero_col_bounds(h);
    phys_v2 origin = b.max;                                                         /* HC:4583 */
    phys_v2 origin2 = { b.min.x, b.max.y };                                         /* HC:4584 */
    /* HC:4586-4587 `bounds.center.x +/- bounds.size.x / 4f`: double stack, one rounding into the Vector2 */
    phys_v2 origin3 = { (float)((double)b.center.x + (double)b.size.x / 4.0), b.max.y };   /* HC:4586 */
    phys_v2 origin4 = { (float)((double)b.center.x - (double)b.size.x / 4.0), b.max.y };   /* HC:4587 */
    int r1 = raycast(h, origin2.x, origin2.y, -0.5f, 1.0f, 2.0f, NULL);            /* HC:4591 */
    int r2 = raycast(h, origin.x, origin.y, 0.5f, 1.0f, 2.0f, NULL);               /* HC:4592 */
    int r3 = raycast(h, origin3.x, origin3.y, 0.0f, 1.0f, 1.0f, NULL);             /* HC:4593 */
    int r4 = raycast(h, origin4.x, origin4.y, 0.0f, 1.0f, 1.0f, NULL);             /* HC:4594 */
    return r1 || r2 || r3 || r4;                                                    /* HC:4595-4599 */
}

int hero_check_touching_ground(const hero *hc)   /* HC:4602-4619 */
{
    hero *h = (hero *)hc;
    hero_bounds b = hero_col_bounds(h);
    float distance = b.extents.y + 0.16f;                                           /* HC:4607 */
    int r1 = raycast(h, b.min.x, b.center.y, 0.0f, -1.0f, distance, NULL);          /* HC:4611 */
    int r2 = raycast(h, b.center.x, b.center.y, 0.0f, -1.0f, distance, NULL);       /* HC:4612 */
    int r3 = raycast(h, b.max.x, b.center.y, 0.0f, -1.0f, distance, NULL);          /* HC:4613 */
    return r1 || r2 || r3;                                                          /* HC:4614-4618 */
}

static int find_collision_direction(const hero_contact *c)   /* HC:4692-4715 */
{
    float x = c->normal.x, y = c->normal.y;
    if (y >= 0.5f) return CS_bottom;                                /* HC:4697-4700 */
    if (y <= -0.5f) return CS_top;                                  /* HC:4701-4704 */
    if (x < 0.0f) return CS_right;                                  /* HC:4705-4708 */
    if (x > 0.0f) return CS_left;                                   /* HC:4709-4712 */
    return CS_bottom;                                               /* HC:4713-4714 (LogError, then bottom) */
}

/* ---- Can* predicates (HC:4717-4830) --------------------------------------------------------------- */
int hero_can_jump(hero *h)   /* HC:4717-4733 */
{
    if (h->f.hero_state != AS_no_input && h->f.hero_state != AS_hard_landing && h->f.hero_state != AS_dash_landing
        && !h->cs.wallSliding && !h->cs.dashing && !h->cs.backDashing && !h->cs.jumping && !h->cs.bouncing
        && !h->cs.shroomBouncing) {                                 /* HC:4719 */
        if (h->cs.onGround) return 1;                               /* HC:4721-4724 */
        if (h->f.ledgeBufferSteps > 0 && !h->cs.dead && !h->cs.hazardDeath && !h->f.controlReqlinquished
            && h->f.headBumpSteps <= 0 && !hero_check_near_roof(h)) {   /* HC:4725 */
            h->f.ledgeBufferSteps = 0;                              /* HC:4727 */
            return 1;
        }
        return 0;
    }
    return 0;
}
int hero_can_double_jump(const hero *h)   /* HC:4735-4742 */
{
    return h->pd.hasDoubleJump && !h->f.controlReqlinquished && !h->f.doubleJumped && !h->f.inAcid
           && h->f.hero_state != AS_no_input && h->f.hero_state != AS_hard_landing && h->f.hero_state != AS_dash_landing
           && !h->cs.dashing && !h->cs.wallSliding && !h->cs.backDashing && !h->cs.attacking && !h->cs.bouncing
           && !h->cs.shroomBouncing && !h->cs.onGround;
}
int hero_can_infinite_air_jump(const hero *h)   /* HC:4744-4751 */
{
    return h->pd.infiniteAirJump && h->f.hero_state != AS_hard_landing && !h->cs.onGround;
}
int hero_can_swim(const hero *h)   /* HC:4753-4760 */
{
    return h->f.hero_state != AS_no_input && h->f.hero_state != AS_hard_landing && h->f.hero_state != AS_dash_landing
           && !h->cs.attacking && !h->cs.dashing && !h->cs.jumping && !h->cs.bouncing && !h->cs.shroomBouncing
           && !h->cs.onGround && h->f.inAcid;
}
int hero_can_dash(const hero *h)   /* HC:4762-4769 */
{
    return h->f.hero_state != AS_no_input && h->f.hero_state != AS_hard_landing && h->f.hero_state != AS_dash_landing
           && h->f.dashCooldownTimer <= 0.0f && !h->cs.dashing && !h->cs.backDashing
           && (!h->cs.attacking || !(h->f.attack_time < h->f.ATTACK_RECOVERY_TIME)) && !h->cs.preventDash
           && (h->cs.onGround || !h->f.airDashed || h->cs.wallSliding) && !h->cs.hazardDeath && h->pd.canDash;
}
int hero_can_attack(const hero *h)   /* HC:4771-4778 */
{
    return h->f.attack_cooldown <= 0.0f && !h->cs.attacking && !h->cs.dashing && !h->cs.dead && !h->cs.hazardDeath
           && !h->cs.hazardRespawning && !h->f.controlReqlinquished && h->f.hero_state != AS_no_input
           && h->f.hero_state != AS_hard_landing && h->f.hero_state != AS_dash_landing;
}
int hero_can_nail_charge(const hero *h)   /* HC:4780-4787 */
{
    return !h->cs.attacking && !h->f.controlReqlinquished && !h->cs.recoiling && !h->cs.recoilingLeft
           && !h->cs.recoilingRight && h->pd.hasNailArt;
}
int hero_can_wall_slide(const hero *h)   /* HC:4789-4800 */
{
    if (h->cs.wallSliding && h->gm_isPaused) return 1;              /* HC:4791-4794 */
    return !h->cs.touchingNonSlider && !h->f.inAcid && !h->cs.dashing && h->pd.hasWalljump && !h->cs.onGround
           && !h->cs.recoiling && !h->gm_isPaused && !h->f.controlReqlinquished && !h->cs.transitioning
           && (h->cs.falling || h->cs.wallSliding) && !h->cs.doubleJumping && hero_can_input(h);   /* HC:4795 */
}
int hero_can_take_damage(const hero *h)   /* HC:4802-4809 */
{
    return h->f.damageMode != DM_NO_DAMAGE && h->f.transitionState == HTS_WAITING_TO_TRANSITION && !h->cs.invulnerable
           && !h->cs.recoiling && !h->pd.isInvincible && !h->cs.dead && !h->cs.hazardDeath && !h->bsc_isTransitioning;
}
int hero_can_wall_jump(const hero *h)   /* HC:4811-4830 */
{
    if (h->pd.hasWalljump) {
        if (h->cs.touchingNonSlider) return 0;                      /* HC:4815-4818 */
        if (h->cs.wallSliding) return 1;                            /* HC:4819-4822 */
        if (h->cs.touchingWall && !h->cs.onGround) return 1;        /* HC:4823-4826 */
        return 0;
    }
    return 0;
}
static int should_hard_land(const hero *h, const hero_contact *c)   /* HC:4832-4839 */
{
    return !(c->flags & HERO_CONTACT_NO_HARD_LANDING) && h->cs.willHardLand && !h->f.inAcid && h->f.hero_state != AS_hard_landing;
}

/* ---- Unity collision callbacks (HC:4841-4999) ------------------------------------------------------ */
void hero_on_collision_enter(hero *h, const hero_contact *c)   /* HC:4841-4904 */
{
    if (h->cs.superDashing && (hero_check_still_touching_wall(h, CS_left, 0) || hero_check_still_touching_wall(h, CS_right, 0))) {   /* HC:4843 UNVERIFIED */
        hero_fsm_event(h, FSM_SUPERDASH, "HIT WALL");               /* HC:4845 */
    }
    if ((c->layer == PL_TERRAIN || c->tag_hero_walkable) && hero_check_touching_ground(h)) {   /* HC:4847 */
        hero_fsm_event(h, FSM_PROXY, "HeroCtrl-Landed");            /* HC:4849 */
    }
    if (h->f.hero_state != AS_no_input) {                           /* HC:4851 */
        int collisionSide = find_collision_direction(c);            /* HC:4853 */
        if (c->layer != PL_TERRAIN && !c->tag_hero_walkable) return;   /* HC:4854-4857 */
        h->f.fallTrailGenerated = 0;                                /* HC:4858 */
        if (collisionSide == CS_top) {                              /* HC:4859 */
            h->f.headBumpSteps = h->f.HEAD_BUMP_STEPS;              /* HC:4861 */
            if (h->cs.jumping) {                                    /* HC:4862 */
                hero_cancel_jump(h);                                /* HC:4864 */
                hero_cancel_double_jump(h);                         /* HC:4865 */
            }
            if (h->cs.bouncing) {                                   /* HC:4867 */
                hero_cancel_bounce(h);                              /* HC:4869 */
                hero_set_vel(h, hero_vel(h).x, 0.0f);               /* HC:4870 */
            }
            if (h->cs.shroomBouncing) {                             /* HC:4872 */
                hero_cancel_bounce(h);                              /* HC:4874 */
                hero_set_vel(h, hero_vel(h).x, 0.0f);               /* HC:4875 */
            }
        }
        if (collisionSide == CS_bottom) {                           /* HC:4878 */
            if (h->cs.attacking) hero_cancel_down_attack(h);        /* HC:4880-4883 */
            if (should_hard_land(h, c)) hero_do_hard_landing(h);    /* HC:4884-4887 */
            else if (!(c->flags & HERO_CONTACT_STEEP_SLOPE) && h->f.hero_state != AS_hard_landing) hero_back_on_ground(h);   /* HC:4888-4891 */
            if (h->cs.dashing && h->f.dashingDown) {                /* HC:4892 UNVERIFIED */
                hero_affected_by_gravity(h, 1);                     /* HC:4894 */
                hero_set_state(h, AS_dash_landing);                 /* HC:4895 */
                h->f.hardLanded = 1;                                /* HC:4896 */
            }
        }
    } else if (h->f.hero_state == AS_no_input && h->f.transitionState == HTS_DROPPING_DOWN
               && (h->f.gatePosition == GP_bottom || h->f.gatePosition == GP_top)) {   /* HC:4900 */
        HKSIM_UNIMPLEMENTED("HeroController.FinishedEnteringScene from OnCollisionEnter2D (HC:4902): scene entry is not part of the arena episode");
    }
}

void hero_on_collision_stay(hero *h, const hero_contact *c)   /* HC:4906-4959 */
{
    if (h->cs.superDashing && (hero_check_still_touching_wall(h, CS_left, 0) || hero_check_still_touching_wall(h, CS_right, 0))) {   /* HC:4908 UNVERIFIED */
        hero_fsm_event(h, FSM_SUPERDASH, "HIT WALL");               /* HC:4910 */
    }
    if (h->f.hero_state == AS_no_input || c->layer != PL_TERRAIN) return;   /* HC:4912-4915 */
    if (!(c->flags & HERO_CONTACT_NON_SLIDER)) {                    /* HC:4916 */
        h->cs.touchingNonSlider = 0;                                /* HC:4918 */
        if (hero_check_still_touching_wall(h, CS_left, 0)) {        /* HC:4919 */
            h->cs.touchingWall = 1;                                 /* HC:4921 */
            h->f.touchingWallL = 1;                                 /* HC:4922 */
            h->f.touchingWallR = 0;                                 /* HC:4923 */
        } else if (hero_check_still_touching_wall(h, CS_right, 0)) {   /* HC:4925 */
            h->cs.touchingWall = 1;                                 /* HC:4927 */
            h->f.touchingWallL = 0;                                 /* HC:4928 */
            h->f.touchingWallR = 1;                                 /* HC:4929 */
        } else {
            h->cs.touchingWall = 0;                                 /* HC:4933 */
            h->f.touchingWallL = 0;                                 /* HC:4934 */
            h->f.touchingWallR = 0;                                 /* HC:4935 */
        }
        if (hero_check_touching_ground(h)) {                        /* HC:4937 */
            if (should_hard_land(h, c)) hero_do_hard_landing(h);    /* HC:4939-4942 */
            else if (h->f.hero_state != AS_hard_landing && h->f.hero_state != AS_dash_landing && h->cs.falling) hero_back_on_ground(h);   /* HC:4943-4946 */
        } else if (h->cs.jumping || h->cs.falling) {                /* HC:4948 */
            h->cs.onGround = 0;                                     /* HC:4950 */
            hero_fsm_event(h, FSM_PROXY, "HeroCtrl-LeftGround");    /* HC:4951 */
            hero_set_state(h, AS_airborne);                         /* HC:4952 */
        }
    } else {
        h->cs.touchingNonSlider = 1;                                /* HC:4957 */
    }
}

void hero_on_collision_exit(hero *h, const hero_contact *c)   /* HC:4961-4999 */
{
    if (h->cs.recoilingLeft || h->cs.recoilingRight) {              /* HC:4963 */
        h->cs.touchingWall = 0;                                     /* HC:4965 */
        h->f.touchingWallL = 0;                                     /* HC:4966 */
        h->f.touchingWallR = 0;                                     /* HC:4967 */
        h->cs.touchingNonSlider = 0;                                /* HC:4968 */
    }
    if (h->f.touchingWallL && !hero_check_still_touching_wall(h, CS_left, 0)) {   /* HC:4970 */
        h->cs.touchingWall = 0;                                     /* HC:4972 */
        h->f.touchingWallL = 0;                                     /* HC:4973 */
    }
    if (h->f.touchingWallR && !hero_check_still_touching_wall(h, CS_right, 0)) {   /* HC:4975 */
        h->cs.touchingWall = 0;                                     /* HC:4977 */
        h->f.touchingWallR = 0;                                     /* HC:4978 */
    }
    if (h->f.hero_state == AS_no_input || h->cs.recoiling || c->layer != PL_TERRAIN || hero_check_touching_ground(h)) return;   /* HC:4980-4983 */
    if (!h->cs.jumping && !h->f.fallTrailGenerated && h->cs.onGround) {   /* HC:4984 */
        if (h->pd.environmentType != 6) hero_fsm_event(h, FSM_FALL_TRAIL, "PLAY");   /* HC:4986-4989 */
        h->f.fallTrailGenerated = 1;                                /* HC:4990 */
    }
    h->cs.onGround = 0;                                             /* HC:4992 */
    hero_fsm_event(h, FSM_PROXY, "HeroCtrl-LeftGround");            /* HC:4993 */
    hero_set_state(h, AS_airborne);                                 /* HC:4994 */
    if (h->cs.wasOnGround) h->f.ledgeBufferSteps = h->f.LEDGE_BUFFER_STEPS;   /* HC:4995-4998 */
}

/* ---- GetState / SetCState by name (HC:2843-2856 -> HeroControllerStates.cs:123-131, reflection) ---- */
int hero_get_state(const hero *h, const char *name)
{
#define X(n) if (strcmp(name, #n) == 0) return h->cs.n;
    HERO_CSTATE(X)
#undef X
    return 0;   /* ReflectionHelper.GetField -> null -> false */
}
int hero_set_cstate(hero *h, const char *name, int value)
{
#define X(n) if (strcmp(name, #n) == 0) { h->cs.n = (uint8_t)(value != 0); return 1; }
    HERO_CSTATE(X)
#undef X
    return 0;   /* SetFieldSafe: unknown field is a no-op */
}
