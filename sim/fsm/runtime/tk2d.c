/* tk2dSpriteAnimator port — analysis/decomp/Assembly-CSharp/tk2dSpriteAnimator.cs (tk2d-animator.md §1).
 * float32 everywhere: clipTime is in frames; ClipTimeSeconds = clipTime / clipFps. */
#include "fsm/fsm.h"
#include "fsm/lifecycle.h"
#include <string.h>
#include "hero/hero.h"

enum { AS_PLAYING = 1, AS_PAUSED = 2 };   /* State flags :7-12 */

/* The only writer of completed_owner (kind 0, an FSM action's own delegate) outside this file --
 * act_hk.c's Tk2dPlayAnimationWithEvents/Tk2dWatchAnimationEvents -- routes through here instead of
 * assigning the field directly, so becoming the writer also reclaims the delegate from a HeroAnimation-
 * Controller that held it (HAC's own h->anim.delegateInstalled guard is cleared via
 * hero_anim_handler_overridden, matching HAC.cs re-installing its own delegate on the way back). */
void anim_set_completed_owner(fsm_world *w, anim_inst *a, act_inst *owner, int32_t event)
{
    if (a->completed_kind == 1) {
        if (w->hero) hero_anim_handler_overridden(w->hero);
    }
    a->completed_kind = 0;
    a->completed_owner = owner;
    a->completed_event = event;
}

anim_inst *anim_of_go(fsm_world *w, int32_t go)
{
    if (go < 0 || w->gos[go].anim < 0) return NULL;
    return &w->anims[w->gos[go].anim];
}
static const clip_def *clip_at(const fsm_world *w, int32_t idx) { return idx < 0 ? NULL : &w->sc->clips[idx]; }
bool anim_playing(const anim_inst *a) { return (a->state & AS_PLAYING) != 0; }   /* :103 */
/* Paused :52-68 / Pause :401 / Resume :406.  anim_update returns while the flag is set (:435). */
void anim_set_paused(anim_inst *a, bool on) { if (on) a->state |= AS_PAUSED; else a->state &= (uint8_t)~AS_PAUSED; }
const char *anim_clip_name(const fsm_world *w, const anim_inst *a) { const clip_def *c = clip_at(w, a->cur_clip); return c ? w_str(w, c->name) : ""; }

/* GetClipByName over the animator's library (tk2dSpriteAnimation.GetClipByName: first name match) */
static int32_t clip_by_name(fsm_world *w, anim_inst *a, const char *name)
{
    if (a->lib < 0) HKSIM_UNIMPLEMENTED("tk2dSpriteAnimator on '%s': library not in any dump", go_path(w, a->go));
    const anim_lib_def *L = &w->sc->libs[a->lib];
    for (int32_t i = 0; i < L->n_clips; i++) if (strcmp(w_str(w, w->sc->clips[L->clip_start + i].name), name) == 0) return L->clip_start + i;
    return -1;
}

/* ClipTimeSeconds :117-127 */
float anim_clip_time_seconds(const fsm_world *w, const anim_inst *a)
{
    const clip_def *c = clip_at(w, a->cur_clip);
    if (!(a->clip_fps > 0.0f)) return c ? a->clip_time / c->fps : 0.0f;
    return a->clip_time / a->clip_fps;
}

/* CurrentFrame :146-183 */
int32_t anim_current_frame(const fsm_world *w, const anim_inst *a)
{
    const clip_def *c = clip_at(w, a->cur_clip);
    if (!c) return 0;
    int32_t n = c->n_frames;
    int32_t k = (int32_t)a->clip_time;                             /* C# (int) truncates toward zero */
    switch (c->wrap) {
    case 2: return k < n ? k : n;                                  /* Once: Mathf.Min(k, n) — n, not n-1 */
    case 0: case 5: return k % n;                                  /* Loop / RandomLoop */
    case 1: { int32_t r = c->loop_start + (k - c->loop_start) % (n - c->loop_start); return k >= c->loop_start ? r : k; }
    case 3: { int32_t j = n > 1 ? k % (n + n - 2) : 0; if (j >= n) j = 2 * n - 2 - j; return j; }
    case 6: return 0;
    default: return k % n;                                         /* RandomFrame: "Unhandled clip wrap mode" -> Loop */
    }
}

static void fire_completed(fsm_world *w, anim_inst *a)          /* OnAnimationCompleted :577-584 */
{
    /* animator.AnimationCompleted is a single delegate field: route to whichever of the three C# writers
     * currently owns it (completed_kind) */
    const char *clip_name = anim_clip_name(w, a);   /* sampled before previous_frame resets, while cur_clip is still the completing clip */
    a->previous_frame = -1;
    if (a->completed_kind == 1) {                                     /* HeroAnimationController.AnimationCompleteDelegate (HAC:432-454) */
        if (w->hero) hero_anim_on_completed(w->hero, clip_name);
        return;
    }
    if (a->completed_kind == 2) {                                     /* NailSlash.Disable (NS:155-158) */
        if (w->hero) hero_slash_anim_completed(w->hero, a->completed_slash_slot);
        return;
    }
    if (a->completed_owner) {
        act_inst *o = a->completed_owner;
        /* Tk2dPlayAnimationWithEvents.AnimationCompleteDelegate :76-93: EventData.IntData = clip index in library */
        int32_t idx = -1;
        if (a->lib >= 0 && a->cur_clip >= 0) idx = a->cur_clip - w->sc->libs[a->lib].clip_start;
        w->ev_int = idx;
        fsm_event(o->fsm, a->completed_event);
    }
}
static void fire_triggered(fsm_world *w, anim_inst *a, int32_t frame)
{
    if (!a->triggered_owner) return;
    const clip_def *c = clip_at(w, a->cur_clip);
    const clip_frame_def *fr = &w->sc->frames[c->frame_start + frame];
    w->ev_int = fr->event_int; w->ev_string = fr->event_info; w->ev_float = fr->event_float;
    fsm_event(a->triggered_owner->fsm, a->triggered_event);
}

/* ProcessEvents :560-575 */
static void process_events(fsm_world *w, anim_inst *a, int32_t start, int32_t last, int32_t direction)
{
    if (!a->triggered_owner || start == last) return;
    int sgn_ls = (last - start) > 0 ? 1 : ((last - start) < 0 ? -1 : 0);   /* Mathf.Sign(0) = 1 in Unity */
    if (sgn_ls == 0) sgn_ls = 1;
    int sgn_d = direction >= 0 ? 1 : -1;
    if (sgn_ls != sgn_d) return;
    const clip_def *c = clip_at(w, a->cur_clip);
    int32_t num = last + direction;
    for (int32_t i = start + direction; i != num; i += direction)
        if (w->sc->frames[c->frame_start + i].trigger_event && a->triggered_owner) fire_triggered(w, a, i);
}

/* tk2dBaseSprite.SetSprite -> the spriteId setter (:159-186) -> UpdateCollider (:434-621).  For a
 * physicsEngine == Physics2D sprite, every sprite CHANGE rewrites the Collider2D: Box writes offset and size
 * and force-enables it (:505-510), None disables it (:597-600), Unset and Custom leave it (:587-590).  So an
 * enemy's hurtbox follows its animation. */
static void sprite_apply(fsm_world *w, anim_inst *a, int32_t frame_index)
{
    const clip_def *c = clip_at(w, a->cur_clip);
    if (!c || frame_index < 0 || frame_index >= c->n_frames) return;
    const clip_frame_def *fr = &w->sc->frames[c->frame_start + frame_index];
    if (fr->sprite_id < 0) return;
    /* SetSprite(collection, id) (:159-186): UpdateCollider fires when the (collection,id) PAIR changes,
     * not the raw spriteId alone -- two sprites from different collections can share a numeric id, and
     * C# resets _spriteId to -1 first whenever the collection changes.  fr->sprite is the SPRITEDEFS
     * index gen_tables.py interns per (collection,spriteId) pair, so comparing IT is exactly that
     * combined test. */
    if (fr->sprite == a->sprite_def) return;           /* :167 `if (value != _spriteId)`, collection-aware */
    a->sprite_id = fr->sprite_id;
    a->sprite_def = fr->sprite;
    if (fr->sprite < 0) return;                       /* collection not in sprites.json: nothing to apply */
    const spritedef_def *sd = &w->sc->spritedefs[fr->sprite];
    if (sd->physics_engine != 1) return;               /* Physics3D: the 2D colliders are never touched */
    col_inst *col = go_box_collider(w, a->go);
    float sx = a->def->sprite_scale[0], sy = a->def->sprite_scale[1];
    switch (sd->collider_type) {
    case 2: {                                          /* Box :467-511 */
        if (!col) col = world_add_box_collider(w, a->go);   /* :473-479 GetComponent, else AddComponent<BoxCollider2D>() */
        float off[2] = { sd->off[0] * sx, sd->off[1] * sy };
        float size[2] = { fabsf(2.0f * sd->half[0] * sx), fabsf(2.0f * sd->half[1] * sy) };
        col_set_enabled(w, col, true);                 /* :505-508 */
        col_set_box(w, a->go, col, size, off);         /* :509-510 */
        break;
    }
    case 1:                                            /* None :585-621 */
        if (col) col_set_enabled(w, col, false);
        break;
    case 3:
        HKSIM_UNIMPLEMENTED("tk2dSpriteDefinition.ColliderType.Mesh on '%s' sprite %d: UpdateCollider "
                            "drives PolygonCollider2D/EdgeCollider2D point arrays (:512-584) and the "
                            "port has no per-sprite polygon storage", go_path(w, a->go), fr->sprite_id);
        break;
    default: break;                                    /* Unset / Custom :587-590 */
    }
}

static void set_frame_internal(fsm_world *w, anim_inst *a, int32_t frame)      /* :551-558 */
{
    if (a->previous_frame != frame) { sprite_apply(w, a, frame); a->previous_frame = frame; }
}

/* WarpClipToLocalTime :538-549 */
static void warp(fsm_world *w, anim_inst *a, int32_t clip, float time)
{
    const clip_def *c = clip_at(w, clip);
    a->clip_time = time;
    int32_t num = (int32_t)a->clip_time % c->n_frames;
    sprite_apply(w, a, num);                                       /* :545 SetSprite before the trigger */
    if (w->sc->frames[c->frame_start + num].trigger_event && a->triggered_owner) fire_triggered(w, a, num);
    a->previous_frame = num;
}

/* Play(clip, clipStartTime, overrideFps) :291-340 */
void anim_play(fsm_world *w, anim_inst *a, int32_t clip, float clip_start_time, float override_fps)
{
    if (clip < 0) {                                                /* :334-339 */
        fire_completed(w, a);
        a->state &= (uint8_t)~AS_PLAYING;
        return;
    }
    const clip_def *c = clip_at(w, clip);
    float num = override_fps > 0.0f ? override_fps : c->fps;
    if (clip_start_time == 0.0f && anim_playing(a) && a->cur_clip == clip) { a->clip_fps = num; return; }   /* :296-300 no restart */
    a->state |= AS_PLAYING;
    a->cur_clip = clip;
    a->clip_fps = num;
    if (c->wrap == 6 || c->n_frames == 0) {                         /* Single */
        warp(w, a, clip, 0.0f);
        a->state &= (uint8_t)~AS_PLAYING;
    } else if (c->wrap == 4 || c->wrap == 5) {                      /* RandomFrame / RandomLoop: Random.Range(0, n) */
        /* site: the animator's GameObject | the method (an overload, so its parameter types) | "" | k (RngDrawRecorder key) */
        int32_t num2 = hk_rng_range_i_site(w->rng, hk_rng_site(go_path(w, a->go), "tk2dSpriteAnimator.Play(tk2dSpriteAnimationClip,Single,Single)", "", 0),
                                           0, c->n_frames);
        warp(w, a, clip, (float)num2);
        if (c->wrap == 4) { a->previous_frame = -1; a->state &= (uint8_t)~AS_PLAYING; }
    } else {
        float num3 = clip_start_time * a->clip_fps;
        if (c->wrap == 2 && num3 >= a->clip_fps * (float)c->n_frames) {
            warp(w, a, clip, (float)(c->n_frames - 1));
            a->state &= (uint8_t)~AS_PLAYING;
        } else {
            warp(w, a, clip, num3);
            a->clip_time = num3;
        }
    }
}

void anim_play_name(fsm_world *w, anim_inst *a, const char *name)  /* Play(string) :235-238 -> GetClipByNameVerbose (null on miss) */
{
    anim_play(w, a, clip_by_name(w, a, name), 0.0f, 0.0f);
}
void anim_play_from_frame(fsm_world *w, anim_inst *a, int32_t frame)  /* PlayFromFrame(int) :245-262 */
{
    int32_t clip = a->cur_clip;
    if (clip < 0) clip = w->sc->libs[a->lib].clip_start + a->def->default_clip;   /* currentClip ?? DefaultClip */
    const clip_def *c = clip_at(w, clip);
    anim_play(w, a, clip, (float)(((double)frame + (double)0.001f) / (double)c->fps), 0.0f);   /* :261 compound -> double stack */
}
/* PlayFromFrame(string name, int frame) :253-256 -> PlayFromFrame(GetClipByNameVerbose(name), frame)
 * :258-261 -> PlayFrom :270-272 -> Play(clip, (frame+0.001)/fps, DefaultFps) :291 -- ONE Play()/Warp call,
 * so the landed frame's trigger/sprite fires once. */
void anim_play_from_frame_named(fsm_world *w, anim_inst *a, const char *name, int32_t frame)
{
    int32_t clip = clip_by_name(w, a, name);
    const clip_def *c = clip_at(w, clip);
    if (!c) { anim_play(w, a, clip, 0.0f, 0.0f); return; }   /* GetClipByNameVerbose miss -> Play(null) (anim_play's clip<0 branch) */
    anim_play(w, a, clip, (float)(((double)frame + (double)0.001f) / (double)c->fps), 0.0f);
}
void anim_stop(anim_inst *a) { a->state &= (uint8_t)~AS_PLAYING; }   /* :342-345 */
/* Start :193-199: Play(DefaultClip) — DefaultClip = library clip defaultClipId (:81-84) */
void anim_play_default(fsm_world *w, anim_inst *a)
{
    if (a->lib < 0) return;
    const anim_lib_def *L = &w->sc->libs[a->lib];
    if (a->def->default_clip >= 0 && a->def->default_clip < L->n_clips)
        anim_play(w, a, L->clip_start + a->def->default_clip, 0.0f, 0.0f);
    else
        /* tk2dSpriteAnimation.DefaultClip = GetClipById(defaultClipId) (tk2d-animator.md): an out-of-range
         * id gives null, and Start's Play(null) still fires AnimationCompleted (anim_play's clip<0 branch). */
        anim_play(w, a, -1, 0.0f, 0.0f);
}

/* UpdateAnimation :433-526 */
void anim_update(fsm_world *w, anim_inst *a, float dt)
{
    if (a->state != AS_PLAYING) return;                            /* (state | globalState) != Playing; globalState == Init */
    const clip_def *c = clip_at(w, a->cur_clip);
    if (!c) return;
    a->clip_time = (float)((double)a->clip_time + (double)dt * (double)a->clip_fps);   /* compound `clipTime += dt * fps` -> double stack, one rounding */
    int32_t num = a->previous_frame;
    int32_t n = c->n_frames;
    switch (c->wrap) {
    case 0: case 5: {
        int32_t num5 = (int32_t)a->clip_time % n;
        set_frame_internal(w, a, num5);
        if (num5 < num) { process_events(w, a, num, n - 1, 1); process_events(w, a, -1, num5, 1); }
        else process_events(w, a, num, num5, 1);
        break;
    }
    case 1: {
        int32_t num3 = (int32_t)a->clip_time;
        int32_t num4 = c->loop_start + (num3 - c->loop_start) % (n - c->loop_start);
        if (num3 >= c->loop_start) {
            set_frame_internal(w, a, num4);
            num3 = num4;
            if (num < c->loop_start) { process_events(w, a, num, c->loop_start - 1, 1); process_events(w, a, c->loop_start - 1, num3, 1); }
            else if (num3 < num) { process_events(w, a, num, n - 1, 1); process_events(w, a, c->loop_start - 1, num3, 1); }
            else process_events(w, a, num, num3, 1);
        } else {
            set_frame_internal(w, a, num3);
            process_events(w, a, num, num3, 1);
        }
        break;
    }
    case 3: {
        int32_t num6 = n > 1 ? (int32_t)a->clip_time % (n + n - 2) : 0;
        int32_t direction = 1;
        if (num6 >= n) { num6 = 2 * n - 2 - num6; direction = -1; }
        if (num6 < num) direction = -1;
        set_frame_internal(w, a, num6);
        process_events(w, a, num, num6, direction);
        break;
    }
    case 2: {
        int32_t num2 = (int32_t)a->clip_time;
        if (num2 >= n) {
            set_frame_internal(w, a, n - 1);
            a->state &= (uint8_t)~AS_PLAYING;
            process_events(w, a, num, n - 1, 1);
            fire_completed(w, a);
        } else {
            set_frame_internal(w, a, num2);
            process_events(w, a, num, num2, 1);
        }
        break;
    }
    default: break;                                                /* RandomFrame: no advance */
    }
}

/* Restore an animator to the state the DUMP recorded, without a Play():
 *  1. Play() runs WarpClipToLocalTime (:538-549), which fires the landed frame's trigger and can fire
 *     completion; the game made no Play call at SceneReady.
 *  2. Warp picks its sprite with `(int)clipTime % frames.Length` for every wrap mode, but CurrentFrame
 *     (:146-181) folds PingPong over `2n-2`; a DORMANT animator keeps what it shows (GG_Grimm_Nightmare's
 *     Grimm, `Slash Antic`), so the frame comes from anim_current_frame.
 * The +0.02 s: the dumper samples the animator one LateUpdate behind the first FRAME record (A-21,
 * measured on GG_Nosk). */
void anim_restore_dumped(fsm_world *w, anim_inst *a)
{
    if (!a || a->def->cur_clip < 0) return;
    const clip_def *c = clip_at(w, a->def->cur_clip);
    /* a stopped animator does not advance (UpdateAnimation returns unless Playing): honour the dump's
     * `playing` bit */
    if (a->def->playing) a->state |= AS_PLAYING; else a->state &= (uint8_t)~AS_PLAYING;
    a->cur_clip = a->def->cur_clip;
    a->clip_fps = a->def->clip_fps > 0.0f ? a->def->clip_fps : (c ? c->fps : 0.0f);
    a->clip_time = (a->def->clip_time_s + 0.02f) * a->clip_fps;
    int32_t frame = anim_current_frame(w, a);
    /* seed sprite_id from the frame being restored BEFORE applying it: a restore must not replay a
     * SetSprite (and its collider rewrite) the game never made.  A pooled clone's first Play() still
     * starts from -1: its prior spriteId (tk2dSprite's serialized field) is not in animator_def. */
    if (c && frame >= 0 && frame < c->n_frames) {
        int32_t sid = w->sc->frames[c->frame_start + frame].sprite_id;
        if (sid >= 0) a->sprite_id = sid;
    }
    set_frame_internal(w, a, frame);
}

void world_animators_start(fsm_world *w)
{
    for (int32_t i = 0; i < w->n_anims; i++) {
        anim_inst *a = &w->anims[i];
        if (!a->enabled) continue;
        /* An animator enabled but not started at the dump gets its Start (and default clip) from the
         * lifecycle (R1).  The test is `started`, not activeInHierarchy: an animator on an INACTIVE object
         * that ran before has Started (A-3) and keeps its dumped clip (GG_Hornet_1's detached `Needle`). */
        if (!lc_started(w, LCT_TK2D, i)) continue;
        /* A DontDestroyOnLoad animator (the Knight's) does NOT Start again at scene load: its dumped clip
         * and time are mid-way (e.g. `Collect Normal 3`, whose end the arrival FSM waits on). */
        if (a->def->ddol && a->def->cur_clip >= 0) {
            anim_play(w, a, a->def->cur_clip, a->def->clip_time_s, a->def->clip_fps);
            /* anim_play always sets AS_PLAYING; a dumped stopped DDOL animator must not advance */
            if (!a->def->playing) a->state &= (uint8_t)~AS_PLAYING;
            anim_set_paused(a, a->def->paused != 0);
        } else if (a->def->cur_clip >= 0) {
            /* A SCENE animator Started at scene load, so the dumped clip and time are the truth at the first
             * observation.  RESTORED, not replayed (anim_restore_dumped).  An FSM's dumped state still wins:
             * world_restore_scene restores the FSMs after this pass. */
            anim_restore_dumped(w, a);
            anim_set_paused(a, a->def->paused != 0);
        } else if (a->def->play_automatically) {
            anim_play(w, a, w->sc->libs[a->lib].clip_start + a->def->default_clip, 0.0f, 0.0f);
        }
        /* Unity runs Start() once per component: mark it so the default Play is not run again */
        a->started = 1;
        a->start_pending = 0;
    }
}
