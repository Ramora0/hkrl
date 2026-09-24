/* HeroAnimationController (HAC:<n> = analysis/decomp/Assembly-CSharp/HeroAnimationController.cs:<n>): clip
 * selection only.  The Knight tk2dSpriteAnimator is reached through hero_tk2d_ops or the FSM world's animator
 * (analysis/specs/tk2d-animator.md 1-2). */
#include <string.h>
#include "hero/hero.h"
#include "hero/hero_internal.h"
#include "core/trap.h"

#include "fsm/fsm.h"
static anim_inst *fsm_knight_anim(hero *h, fsm_world **w_out)
{
    if (!h->hooks.ctx) return NULL;
    fsm_world *w = *(fsm_world **)h->hooks.ctx;
    if (!w || w->knight_go < 0) return NULL;
    if (w_out) *w_out = w;
    return anim_of_go(w, w->knight_go);
}

static int tk_play(hero *h, const char *clip)                    /* animator.Play(string) tk2dSpriteAnimator.cs:235-238 */
{
    if (h->tk.play) return h->tk.play(h->tk.ctx, clip);
    fsm_world *w = NULL;
    anim_inst *a = fsm_knight_anim(h, &w);
    if (a && clip && *clip) { anim_play_name(w, a, clip); return 1; }
    if (h->hooks.anim_play_clip) { h->hooks.anim_play_clip(h->hooks.ctx, clip); return 1; }
    h->dropped_anim_calls++; return 0;
}
static void tk_play_from_frame(hero *h, const char *clip, int frame)   /* tk2dSpriteAnimator.cs:254-262 */
{
    if (h->tk.play_from_frame) { h->tk.play_from_frame(h->tk.ctx, clip, frame); return; }
    fsm_world *w = NULL;
    anim_inst *a = fsm_knight_anim(h, &w);
    if (a && clip && *clip) {
        /* PlayFromFrame(name, frame) is one Play()/Warp call (tk2dSpriteAnimator.cs:253-262), not Play(name)
         * followed by PlayFromFrame(int) */
        anim_play_from_frame_named(w, a, clip, frame);
        return;
    }
    if (h->hooks.anim_play_clip) { h->hooks.anim_play_clip(h->hooks.ctx, clip); return; }
    h->dropped_anim_calls++;
}
static void tk_stop(hero *h)                                     /* tk2dSpriteAnimator.cs:342-345 */
{
    if (h->tk.stop) { h->tk.stop(h->tk.ctx); return; }
    fsm_world *w = NULL;
    anim_inst *a = fsm_knight_anim(h, &w);
    if (a) { anim_stop(a); return; }
    h->dropped_anim_calls++;
}
static int tk_is_playing(hero *h, const char *clip)              /* tk2dSpriteAnimator.cs:356-363 */
{
    if (h->tk.is_playing) return h->tk.is_playing(h->tk.ctx, clip);
    fsm_world *w = NULL;
    anim_inst *a = fsm_knight_anim(h, &w);
    if (a && anim_playing(a)) {
        return strcmp(anim_clip_name(w, a), clip) == 0;
    }
    h->dropped_anim_calls++; return 0;
}
static const char *tk_current_clip(hero *h)                      /* animator.CurrentClip.name */
{
    if (h->tk.current_clip) {
        const char *c = h->tk.current_clip(h->tk.ctx);
        return c ? c : "";
    }
    fsm_world *w = NULL;
    anim_inst *a = fsm_knight_anim(h, &w);
    if (a) {
        const char *c = anim_clip_name(w, a);
        return c ? c : "";
    }
    h->dropped_anim_calls++; return "";
}
static void tk_set_completed_handler(hero *h)                    /* animator.AnimationCompleted = AnimationCompleteDelegate */
{
    h->anim.delegateInstalled = 1;
    if (h->tk.set_completed_handler) { h->tk.set_completed_handler(h->tk.ctx, HERO_ANIM_HANDLER_HERO); return; }
    /* completed_kind 1: the animator's completion goes to hero_anim_on_completed, replacing any FSM owner */
    anim_inst *a = fsm_knight_anim(h, NULL);
    if (a) { a->completed_kind = 1; return; }
    h->dropped_anim_calls++;
}

/* Play / PlayFromFrame (HAC:507-523): changedClipFromLastFrame is set when the name differs from CurrentClip.name */
static void hac_play(hero *h, const char *clipName)              /* HAC:507-514 */
{
    if (strcmp(clipName, tk_current_clip(h)) != 0) h->anim.changedClipFromLastFrame = 1;   /* HAC:509-512 */
    tk_play(h, clipName);                                        /* HAC:513 */
}
static void hac_play_from_frame(hero *h, const char *clipName, int frame)   /* HAC:516-523 */
{
    if (strcmp(clipName, tk_current_clip(h)) != 0) h->anim.changedClipFromLastFrame = 1;   /* HAC:518-521 */
    tk_play_from_frame(h, clipName, frame);                      /* HAC:522 */
}

void hero_anim_play_idle(hero *h)                                /* HAC:456-485 PlayIdle */
{
    if (h->pd.health == 1 && h->pd.healthBlue < 1) {             /* HAC:458 */
        if (h->pd.equippedCharm_6) tk_play(h, "Idle");           /* HAC:460-462 */
        else tk_play(h, "Idle Hurt");                            /* HAC:466 */
    } else if (tk_is_playing(h, "LookUp")) {                     /* HAC:469 */
        tk_play(h, "LookUpEnd");                                 /* HAC:471 */
    } else if (tk_is_playing(h, "LookDown")) {                   /* HAC:473 */
        tk_play(h, "LookDownEnd");                               /* HAC:475 */
    } else if (h->f.wieldingLantern) {                           /* HAC:477 */
        tk_play(h, "Lantern Idle");                              /* HAC:479 */
    } else {
        tk_play(h, "Idle");                                      /* HAC:483 */
    }
}

static void play_run(hero *h)                                    /* HAC:487-505 PlayRun */
{
    if (h->f.wieldingLantern) tk_play(h, "Lantern Run");         /* HAC:489-491 */
    else if (h->pd.equippedCharm_37) hac_play(h, "Sprint");      /* HAC:493-495 */
    else if (h->anim.wasAttacking) tk_play_from_frame(h, "Run", 3);   /* HAC:497-499 (animator.PlayFromFrame directly) */
    else tk_play(h, "Run");                                      /* HAC:503 */
}

static int can_play_idle(hero *h)                                /* HAC:405-412 */
{
    return !tk_is_playing(h, "Land") && !tk_is_playing(h, "Run To Idle") && !tk_is_playing(h, "Dash To Idle")
        && !tk_is_playing(h, "Backdash Land") && !tk_is_playing(h, "Backdash Land 2") && !tk_is_playing(h, "LookUpEnd")
        && !tk_is_playing(h, "LookDownEnd") && !tk_is_playing(h, "Exit Door To Idle") && !tk_is_playing(h, "Wake Up Ground")
        && !tk_is_playing(h, "Hazard Respawn");
}
static int can_play_look_down(hero *h)                           /* HAC:414-421 ("Lookup" never resolves: tk2d-animator.md 2.5) */
{
    return h->cs.lookingDownAnim && !tk_is_playing(h, "Lookup");
}
static int can_play_turn(hero *h)                                /* HAC:423-430 */
{
    return !tk_is_playing(h, "Wake Up Ground") && !tk_is_playing(h, "Hazard Respawn");
}

static void reset_all(hero *h)                                   /* HAC:87-94 */
{
    h->anim.playLanding = 0;
    h->anim.playRunToIdle = 0;
    h->anim.playDashToIdle = 0;
    h->anim.wasFacingRight = 0;
    h->anim.controlEnabled = 1;
}
static void reset_plays(hero *h)                                 /* HAC:96-101 */
{
    h->anim.playLanding = 0;
    h->anim.playRunToIdle = 0;
    h->anim.playDashToIdle = 0;
}

void hero_anim_init(hero *h)                                     /* Awake + Start without the initial Play (HAC:42-69): the arena
                                                                    dump has the animator already on "Idle" (physics.json#heroAnimator) */
{
    memset(&h->anim, 0, sizeof h->anim);
    reset_all(h);                                                /* HAC:52 */
    h->anim.actorState = h->f.hero_state;                        /* HAC:53 */
    /* the dump is long after Start: the latches (HAC:384/392, :394-401) already hold facing / attacking, else the
       first Update plays a spurious "Turn" (HAC:380-391) */
    h->anim.wasFacingRight = h->cs.facingRight;
    h->anim.wasAttacking = h->cs.attacking;
}
/* Knight animator at SceneReady from this scene's dump (physics.json#heroAnimator).  +0.02 s: the dump is taken
 * at SceneReady and the first recorded FRAME is one step later; CurrentFrame derives from clipTime (tk2d :146-183). */
void hero_anim_prime_scene_ready(hero *h)
{
    fsm_world *w = NULL;
    anim_inst *a = fsm_knight_anim(h, &w);
    if (a && a->def->cur_clip >= 0) {
        anim_play(w, a, a->def->cur_clip, a->def->clip_time_s + 0.02f, a->def->clip_fps);
        /* anim_play always sets playing; apply the dump's playing / paused bits */
        if (!a->def->playing) anim_stop(a);
        anim_set_paused(a, a->def->paused != 0);
        return;
    }
    if (!h->tk.play) {
        tk_play(h, "Idle");                                      /* cite: analysis/dumps/GG_Hornet_1/physics.json#heroAnimator */
    }
}

void hero_anim_start(hero *h)                                    /* HAC:49-69 Start UNVERIFIED */
{
    reset_all(h);
    h->anim.actorState = h->f.hero_state;
    if (h->anim.controlEnabled) {
        if (h->f.hero_state == AS_airborne) hac_play_from_frame(h, "Airborne", 7);   /* HAC:56-58 */
        else hero_anim_play_idle(h);                             /* HAC:62 */
    } else {
        tk_stop(h);                                              /* HAC:67 */
    }
}

void hero_anim_update_state(hero *h, int newState)               /* HAC:103-118 */
{
    if (h->anim.controlEnabled && newState != h->anim.actorState) {
        if (h->anim.actorState == AS_airborne && newState == AS_idle && !h->anim.playLanding) h->anim.playLanding = 1;   /* HAC:107-110 */
        if (h->anim.actorState == AS_running && newState == AS_idle && !h->anim.playRunToIdle && !h->cs.inWalkZone && !h->cs.attacking) {
            h->anim.playRunToIdle = 1;                           /* HAC:111-114 */
        }
        h->anim.prevActorState = h->anim.actorState;             /* HAC:115 */
        h->anim.actorState = newState;                           /* HAC:116 */
    }
}

void hero_anim_play_clip(hero *h, const char *clipName)          /* HAC:120-130 */
{
    if (h->anim.controlEnabled) {
        if (strcmp(clipName, "Exit Door To Idle") == 0) tk_set_completed_handler(h);   /* HAC:124-127 */
        hac_play(h, clipName);                                   /* HAC:128 */
    }
}

static void update_animation(hero *h)                            /* HAC:132-403 */
{
    h->anim.changedClipFromLastFrame = 0;                        /* HAC:134 */
    if (h->anim.playLanding) {                                   /* HAC:135 */
        hac_play(h, "Land");                                     /* HAC:137 */
        tk_set_completed_handler(h);                             /* HAC:138 */
        h->anim.playLanding = 0;                                 /* HAC:139 */
    }
    if (h->anim.playRunToIdle) {                                 /* HAC:141 */
        hac_play(h, "Run To Idle");                              /* HAC:143 */
        tk_set_completed_handler(h);                             /* HAC:144 */
        h->anim.playRunToIdle = 0;                               /* HAC:145 */
    }
    if (h->anim.playBackDashToIdleEnd) {                         /* HAC:147 (never set: tk2d-animator.md 2.5) */
        hac_play(h, "Backdash Land 2");                          /* HAC:149 */
        tk_set_completed_handler(h);                             /* HAC:150 */
        h->anim.playBackDashToIdleEnd = 0;                       /* HAC:151 */
    }
    if (h->anim.playDashToIdle) {                                /* HAC:153 */
        hac_play(h, "Dash To Idle");                             /* HAC:155 */
        tk_set_completed_handler(h);                             /* HAC:156 */
        h->anim.playDashToIdle = 0;                              /* HAC:157 */
    }
    if (h->anim.actorState == AS_no_input) {                     /* HAC:159 */
        if (h->cs.recoilFrozen) hac_play(h, "Stun");             /* HAC:161-163 */
        else if (h->cs.recoiling) hac_play(h, "Recoil");         /* HAC:165-167 */
        else if (h->cs.transitioning) {                          /* HAC:169 UNVERIFIED (no scene transition in an episode) */
            if (h->cs.onGround) {                                /* HAC:171 */
                if (h->f.transitionState == HTS_EXITING_SCENE) {  /* HAC:173 */
                    if (!tk_is_playing(h, "Run")) {              /* HAC:175 */
                        if (!h->pd.equippedCharm_37) hac_play(h, "Run");   /* HAC:177-179 */
                        else hac_play(h, "Sprint");              /* HAC:183 */
                    }
                } else if (h->f.transitionState == HTS_ENTERING_SCENE) {   /* HAC:187 */
                    if (!h->pd.equippedCharm_37) {               /* HAC:189 */
                        if (!tk_is_playing(h, "Run")) hac_play_from_frame(h, "Run", 3);   /* HAC:191-193 */
                    } else {
                        hac_play(h, "Sprint");                   /* HAC:198 */
                    }
                }
            } else if (h->f.transitionState == HTS_EXITING_SCENE) {   /* HAC:202 */
                if (!tk_is_playing(h, "Airborne")) hac_play_from_frame(h, "Airborne", 7);   /* HAC:204-206 */
            } else if (h->f.transitionState == HTS_WAITING_TO_ENTER_LEVEL) {   /* HAC:209 */
                if (!tk_is_playing(h, "Airborne")) hac_play_from_frame(h, "Airborne", 7);   /* HAC:211-213 */
            } else if (h->f.transitionState == HTS_ENTERING_SCENE && !h->anim.setEntryAnim) {   /* HAC:216 */
                if (h->f.gatePosition == GP_top) hac_play_from_frame(h, "Airborne", 7);        /* HAC:218-220 */
                else if (h->f.gatePosition == GP_bottom) hac_play_from_frame(h, "Airborne", 3);   /* HAC:222-224 */
                h->anim.setEntryAnim = 1;                        /* HAC:226 */
            }
        }
    } else if (h->anim.setEntryAnim) {                           /* HAC:230 */
        h->anim.setEntryAnim = 0;                                /* HAC:232 */
    } else if (h->cs.dashing) {                                  /* HAC:234 */
        if (h->f.dashingDown) {                                  /* HAC:236 */
            if (h->cs.shadowDashing) {                           /* HAC:238 */
                if (h->pd.equippedCharm_16) hac_play(h, "Shadow Dash Down Sharp");   /* HAC:240-242 */
                else hac_play(h, "Shadow Dash Down");            /* HAC:246 */
            } else {
                hac_play(h, "Dash Down");                        /* HAC:251 */
            }
        } else if (h->cs.shadowDashing) {                        /* HAC:254 */
            if (h->pd.equippedCharm_16) hac_play(h, "Shadow Dash Sharp");   /* HAC:256-258 */
            else hac_play(h, "Shadow Dash");                     /* HAC:262 */
        } else {
            hac_play(h, "Dash");                                 /* HAC:267 */
        }
    } else if (h->cs.backDashing) {                              /* HAC:270 */
        hac_play(h, "Back Dash");                                /* HAC:272 (missing clip: Play(null) path, tk2d-animator.md 2.5) */
    } else if (h->cs.attacking) {                                /* HAC:274 */
        if (h->cs.upAttacking) hac_play(h, "UpSlash");           /* HAC:276-278 */
        else if (h->cs.downAttacking) hac_play(h, "DownSlash");  /* HAC:280-282 */
        else if (h->cs.wallSliding) hac_play(h, "Wall Slash");   /* HAC:284-286 */
        else if (!h->cs.altAttack) hac_play(h, "Slash");         /* HAC:288-290 */
        else hac_play(h, "SlashAlt");                            /* HAC:294 */
    } else if (h->cs.casting) {                                  /* HAC:297 */
        hac_play(h, "Fireball");                                 /* HAC:299 (missing clip) */
    } else if (h->cs.wallSliding) {                              /* HAC:301 */
        hac_play(h, "Wall Slide");                               /* HAC:303 */
    } else if (h->anim.actorState == AS_idle) {                  /* HAC:305 */
        if (h->cs.lookingUpAnim && !tk_is_playing(h, "LookUp")) hac_play(h, "LookUp");   /* HAC:307-309 */
        else if (can_play_look_down(h)) hac_play(h, "LookDown"); /* HAC:311-313 */
        else if (!h->cs.lookingUpAnim && !h->cs.lookingDownAnim && can_play_idle(h)) hero_anim_play_idle(h);   /* HAC:315-317 */
    } else if (h->anim.actorState == AS_running) {               /* HAC:320 */
        if (!tk_is_playing(h, "Turn")) {                         /* HAC:322 */
            if (h->cs.inWalkZone) {                              /* HAC:324 */
                if (!tk_is_playing(h, "Walk")) hac_play(h, "Walk");   /* HAC:326-328 */
            } else {
                play_run(h);                                     /* HAC:333 */
            }
        }
    } else if (h->anim.actorState == AS_airborne) {              /* HAC:337 */
        if (h->cs.swimming) hac_play(h, "Swim");                 /* HAC:339-341 (missing clip) */
        else if (h->f.wallLocked) hac_play(h, "Walljump");       /* HAC:343-345 */
        else if (h->cs.doubleJumping) hac_play(h, "Double Jump");/* HAC:347-349 */
        else if (h->cs.jumping) {                                /* HAC:351 */
            if (!tk_is_playing(h, "Airborne")) hac_play_from_frame(h, "Airborne", 0);   /* HAC:353-355 */
        } else if (h->cs.falling) {                              /* HAC:358 */
            if (!tk_is_playing(h, "Airborne")) hac_play_from_frame(h, "Airborne", 5);   /* HAC:360-362 */
        } else if (!tk_is_playing(h, "Airborne")) {              /* HAC:365 */
            hac_play_from_frame(h, "Airborne", 3);               /* HAC:367 */
        }
    } else if (h->anim.actorState == AS_dash_landing) {          /* HAC:370 */
        hac_play(h, "Dash Down Land");                           /* HAC:372 */
    } else if (h->anim.actorState == AS_hard_landing) {          /* HAC:374 */
        hac_play(h, "HardLand");                                 /* HAC:376 */
    }
    if (h->cs.facingRight) {                                     /* HAC:378 */
        if (!h->anim.wasFacingRight && h->cs.onGround && can_play_turn(h)) hac_play(h, "Turn");   /* HAC:380-383 */
        h->anim.wasFacingRight = 1;                              /* HAC:384 */
    } else {
        if (h->anim.wasFacingRight && h->cs.onGround && can_play_turn(h)) hac_play(h, "Turn");    /* HAC:388-391 */
        h->anim.wasFacingRight = 0;                              /* HAC:392 */
    }
    h->anim.wasAttacking = h->cs.attacking ? 1 : 0;              /* HAC:394-401 */
    reset_plays(h);                                              /* HAC:402 */
}

void hero_anim_update(hero *h)                                   /* HAC:71-85 Update */
{
    if (h->anim.controlEnabled) update_animation(h);             /* HAC:73-76 */
    else if (h->cs.facingRight) h->anim.wasFacingRight = 1;      /* HAC:77-80 */
    else h->anim.wasFacingRight = 0;                             /* HAC:83 */
}

void hero_anim_on_completed(hero *h, const char *clipName)       /* HAC:432-454 AnimationCompleteDelegate */
{
    if (!h->anim.delegateInstalled) return;                      /* another writer owns animator.AnimationCompleted */
    if (strcmp(clipName, "Land") == 0) hero_anim_play_idle(h);             /* HAC:434-437 */
    if (strcmp(clipName, "Run To Idle") == 0) hero_anim_play_idle(h);      /* HAC:438-441 */
    if (strcmp(clipName, "Backdash To Idle") == 0) hero_anim_play_idle(h); /* HAC:442-445 */
    if (strcmp(clipName, "Dash To Idle") == 0) hero_anim_play_idle(h);     /* HAC:446-449 */
    if (strcmp(clipName, "Exit Door To Idle") == 0) hero_anim_play_idle(h);/* HAC:450-453 */
}
void hero_anim_handler_overridden(hero *h)                       /* another component assigned animator.AnimationCompleted */
{
    h->anim.delegateInstalled = 0;
}

void hero_anim_stop_control(hero *h)                             /* HAC:525-532 StopControl (HeroController.StopAnimationControl HC:3127) */
{
    if (h->anim.controlEnabled) {
        h->anim.controlEnabled = 0;
        h->anim.stateBeforeControl = h->anim.actorState;
    }
}
void hero_anim_start_control(hero *h)                            /* HAC:534-539 StartControl (HC:3132) */
{
    h->anim.actorState = h->f.hero_state;
    h->anim.controlEnabled = 1;
    hero_anim_play_idle(h);
}
void hero_anim_start_control_without_setting_state(hero *h)      /* HAC:541-548 */
{
    h->anim.controlEnabled = 1;
    if (h->anim.stateBeforeControl == AS_running && h->anim.actorState == AS_running) h->anim.actorState = AS_idle;
}
void hero_anim_control(hero *h, int enable)                      /* HAC:525-539 StopControl / StartControl */
{
    if (enable) hero_anim_start_control(h);
    else hero_anim_stop_control(h);
}
void hero_anim_finished_dash(hero *h) { h->anim.playDashToIdle = 1; }   /* HAC:550-553 */
void hero_anim_stop_attack(hero *h)                              /* HAC:555-561 */
{
    if (tk_is_playing(h, "UpSlash") || tk_is_playing(h, "DownSlash")) tk_stop(h);
}
void hero_anim_set_play_landing(hero *h, int on) { h->anim.playLanding = (uint8_t)(on != 0); }   /* HC:4274 animCtrl.playLanding = true */

float hero_anim_get_clip_duration(hero *h, const char *clipName) /* HAC:568-581 frames.Length / fps (nominal) */
{
    if (!h->tk.clip_duration) { h->dropped_anim_calls++; return -1.0f; }
    return h->tk.clip_duration(h->tk.ctx, clipName);
}

int hero_anim_current(hero *h, hero_anim_state *out)             /* FRAME hero ANIM sub-block (docs/trace-format.md) */
{
    memset(out, 0, sizeof *out);
    if (!h->tk.current) return 0;
    h->tk.current(h->tk.ctx, out);
    return 1;
}
