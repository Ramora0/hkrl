/* tk2dSpriteAnimator playback (fsm-actions.md §5), sprite and material colour, renderer state.  Colours,
 * meshes and renderer bounds are not modelled and nothing observed reads them: those actions keep only their
 * lifetime, and the getters leave their stores untouched, as the C# does when the renderer is missing. */
#include "act.h"

static anim_inst *sprite_of(act_inst *a, const fsm_pv *go)
{
    int32_t t = p_owner_default(a, go);
    return t < 0 ? NULL : anim_of_go(a->fsm->w, t);
}

/* Tk2dPlayAnimation — ACT/Tk2dPlayAnimation.cs:39-55 */
typedef struct { const fsm_pv *go, *clipName; } st_tpa;
static void tpa_bind(act_inst *a) { ST(st_tpa); s->go = FIELD(gameObject); s->clipName = FIELD(clipName); }
static void tpa_enter(act_inst *a)
{
    ST(st_tpa);
    anim_inst *sp = sprite_of(a, s->go);
    if (sp) anim_play_name(w, sp, w_str(w, ps(f, s->clipName)));  /* missing animator: LogWarning only */
    act_finish(a);
}
static const act_vtable AV_Tk2dPlayAnimation = { "Tk2dPlayAnimation", sizeof(st_tpa), tpa_bind, tpa_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* Tk2dPlayAnimationWithEvents — ACT/Tk2dPlayAnimationWithEvents.cs:43-93 (delegates assigned, never finishes) */
typedef struct { const fsm_pv *go, *clipName, *triggerEvent, *completeEvent; int32_t sprite; } st_tpae;
static void tpae_bind(act_inst *a) { ST(st_tpae); s->go = FIELD(gameObject); s->clipName = FIELD(clipName); s->triggerEvent = FIELD(animationTriggerEvent); s->completeEvent = FIELD(animationCompleteEvent); }
static void tpae_enter(act_inst *a)
{
    ST(st_tpae);
    anim_inst *sp = sprite_of(a, s->go);
    s->sprite = anim_ref(w, sp);
    if (!sp) return;
    anim_play_name(w, sp, w_str(w, ps(f, s->clipName)));
    if (EV(s->triggerEvent) >= 0) { sp->triggered_owner = a; sp->triggered_event = EV(s->triggerEvent); }
    if (EV(s->completeEvent) >= 0) anim_set_completed_owner(w, sp, a, EV(s->completeEvent));
}
static const act_vtable AV_Tk2dPlayAnimationWithEvents = { "Tk2dPlayAnimationWithEvents", sizeof(st_tpae), tpae_bind, tpae_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* Tk2dPauseAnimation — ACT/Tk2dPauseAnimation.cs:41-70: sets/clears tk2dSpriteAnimator.Paused
 * (tk2dSpriteAnimator.cs:401-409) and Finish()es unless everyframe; UpdateAnimation returns at once while
 * paused (:435), so the animator holds its clip and frame (e.g. the Knight mid-kneel in
 * `Boss Scene Controller/Dream Entry | Control` 'Pause Kneeling'). */
typedef struct { const fsm_pv *go, *pause, *everyframe; int32_t sprite; } st_tpause;
static void tpause_bind(act_inst *a) { ST(st_tpause); s->go = FIELD(gameObject); s->pause = FIELD(pause); s->everyframe = FIELD(everyframe); }
static void tpause_do(act_inst *a)
{
    ST(st_tpause);
    anim_inst *sp = anim_deref(w, s->sprite);
    if (sp) anim_set_paused(sp, pb(f, s->pause));
}
static void tpause_enter(act_inst *a)
{
    ST(st_tpause);
    s->sprite = anim_ref(w, sprite_of(a, s->go));
    tpause_do(a);
    if (!pb(f, s->everyframe)) act_finish(a);
}
static const act_vtable AV_Tk2dPauseAnimation = { "Tk2dPauseAnimation", sizeof(st_tpause), tpause_bind, tpause_enter, tpause_do, NULL, NULL, NULL, NULL, NULL };

/* Tk2dWatchAnimationEvents — ACT/Tk2dWatchAnimationEvents.cs:38-51 */
typedef struct { const fsm_pv *go, *triggerEvent, *completeEvent; int32_t sprite; } st_twae;
static void twae_bind(act_inst *a) { ST(st_twae); s->go = FIELD(gameObject); s->triggerEvent = FIELD(animationTriggerEvent); s->completeEvent = FIELD(animationCompleteEvent); }
static void twae_enter(act_inst *a)
{
    ST(st_twae);
    anim_inst *sp = sprite_of(a, s->go);
    s->sprite = anim_ref(w, sp);
    if (!sp) return;
    if (EV(s->triggerEvent) >= 0) { sp->triggered_owner = a; sp->triggered_event = EV(s->triggerEvent); }
    if (EV(s->completeEvent) >= 0) anim_set_completed_owner(w, sp, a, EV(s->completeEvent));
}
static void twae_update(act_inst *a)
{
    ST(st_twae);
    anim_inst *sp = anim_deref(w, s->sprite);
    HKSIM_ASSERT(sp != NULL, "Tk2dWatchAnimationEvents: _sprite null in OnUpdate (NRE in C#) %s", fsm_label(f));
    if (!anim_playing(sp)) { fsm_event(f, EV(s->completeEvent)); act_finish(a); }
}
static const act_vtable AV_Tk2dWatchAnimationEvents = { "Tk2dWatchAnimationEvents", sizeof(st_twae), twae_bind, twae_enter, twae_update, NULL, NULL, NULL, NULL, NULL };

/* Tk2dPlayFrame — ACT/Tk2dPlayFrame.cs:34-46 */
typedef struct { const fsm_pv *go, *frame; } st_tpf;
static void tpf_bind(act_inst *a) { ST(st_tpf); s->go = FIELD(gameObject); s->frame = FIELD(frame); }
static void tpf_enter(act_inst *a) { ST(st_tpf); anim_inst *sp = sprite_of(a, s->go); if (sp) anim_play_from_frame(w, sp, pi(f, s->frame)); act_finish(a); }
static const act_vtable AV_Tk2dPlayFrame = { "Tk2dPlayFrame", sizeof(st_tpf), tpf_bind, tpf_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* Tk2dStopAnimation — HK/Tk2dStopAnimation.cs:25-42 (_sprite null -> NullReferenceException in the warning itself) */
typedef struct { const fsm_pv *go; } st_tstop;
static void tstop_bind(act_inst *a) { ST(st_tstop); s->go = FIELD(gameObject); }
static void tstop_enter(act_inst *a)
{
    ST(st_tstop);
    int32_t t = p_owner_default(a, s->go);
    anim_inst *an = t >= 0 ? anim_of_go(w, t) : NULL;
    if (!an) HKSIM_UNKNOWN("Tk2dStopAnimation without a tk2dSpriteAnimator on '%s' (C# throws)", t >= 0 ? go_path(w, t) : "null");
    anim_stop(an);
    act_finish(a);
}
static const act_vtable AV_Tk2dStopAnimation = { "Tk2dStopAnimation", sizeof(st_tstop), tstop_bind, tstop_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* LerpTk2dSpriteColor - ACT/LerpTk2dSpriteColor.cs:20-46: the colour is not modelled but the timing is --
 * DoAction finishes once `State.StateTime / LerpTime >= 1` (:39-44), and a target with no tk2dSprite finishes at
 * once (:27-31). */
typedef struct { const fsm_pv *Target, *LerpTime; } st_lt2c;
static void lt2c_bind(act_inst *a) { ST(st_lt2c); s->Target = FIELD(Target); s->LerpTime = FIELD(LerpTime); }
static void lt2c_do(act_inst *a)
{
    ST(st_lt2c);
    float lt = pf(f, s->LerpTime);
    if (lt == 0.0f) { act_finish(a); return; }                          /* division by zero -> +inf >= 1 */
    if (a->state->state_time / lt >= 1.0f) act_finish(a);
}
static void lt2c_enter(act_inst *a)
{
    ST(st_lt2c);
    int32_t t = p_owner_default(a, s->Target);
    if (t < 0 || !anim_of_go(w, t)) { act_finish(a); return; }
    lt2c_do(a);
}
static const act_vtable AV_LerpTk2dSpriteColor = { "LerpTk2dSpriteColor", sizeof(st_lt2c), lt2c_bind, lt2c_enter, lt2c_do, NULL, NULL, NULL, NULL, NULL };

/* EaseColor — ACT/EaseColor.cs:29-76 on an FsmColor variable (EaseFsmAction core in math.c) */
typedef struct { ease_core e; const fsm_pv *fromValue, *toValue, *colorVariable, *finishEvent; bool finishInNextStep; } st_ecol;
static void ecol_bind(act_inst *a)
{
    ST(st_ecol);
    ease_bind(a, &s->e);
    s->fromValue = FIELD(fromValue); s->toValue = FIELD(toValue); s->colorVariable = FIELD(colorVariable); s->finishEvent = FIELD_OPT(finishEvent);
}
static void ecol_enter(act_inst *a)                                   /* :29-45 */
{
    ST(st_ecol);
    ease_enter(a, &s->e);
    s->e.n = 4;
    const float *c0 = pv3(f, s->fromValue), *c1 = pv3(f, s->toValue);
    for (int i = 0; i < 4; i++) { s->e.from[i] = c0[i]; s->e.to[i] = c1[i]; }
    s->finishInNextStep = false;
    pv3_set(f, s->colorVariable, c0);
}
static void ecol_update(act_inst *a)                                  /* :52-75 */
{
    ST(st_ecol);
    ease_update(a, &s->e);
    if (!p_isnone(s->colorVariable) && s->e.isRunning) pv3_set(f, s->colorVariable, s->e.result);
    if (s->finishInNextStep) {
        act_finish(a);
        if (EV(s->finishEvent) >= 0) fsm_event(f, EV(s->finishEvent));
    }
    if (s->e.finishAction && !s->finishInNextStep) {
        if (!p_isnone(s->colorVariable)) pv3_set(f, s->colorVariable, pv3(f, ease_final_is_from(a, &s->e) ? s->fromValue : s->toValue));
        s->finishInNextStep = true;
    }
}
static const act_vtable AV_EaseColor = { "EaseColor", sizeof(st_ecol), ecol_bind, ecol_enter, ecol_update, NULL, NULL, NULL, NULL, NULL };

/* SetSpriteRenderer — ACT/SetSpriteRenderer.cs:20-35; SetMeshRenderer — ACT/SetMeshRenderer.cs:20-35 */
typedef struct { const fsm_pv *go, *active; } st_ssr;
static void ssr_bind(act_inst *a) { ST(st_ssr); s->go = FIELD(gameObject); s->active = FIELD(active); }
static void ssr_enter(act_inst *a)
{
    ST(st_ssr);
    int32_t g = p_get_safe(a, s->go);
    if (g >= 0) w->gos[g].mesh_renderer_enabled = pb(f, s->active) ? 1 : 0;
    act_finish(a);
}
static const act_vtable AV_SetSpriteRenderer = { "SetSpriteRenderer", sizeof(st_ssr), ssr_bind, ssr_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static void smr_enter(act_inst *a) { ST(st_ssr); int32_t t = p_owner_default(a, s->go); if (t >= 0) w->gos[t].mesh_renderer_enabled = pb(f, s->active); act_finish(a); }
static const act_vtable AV_SetMeshRenderer = { "SetMeshRenderer", sizeof(st_ssr), ssr_bind, smr_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* Finish at once: ACT/GetMaterialColor.cs:41-94 (Renderer.material colour), PreBuildTK2DSprites.cs
 * (tk2dSprite.ForceBuild rebuilds a mesh), SetSpriteRendererSprite.cs, GetMeshRendererBounds.cs:31-69
 * (renderer and TextMeshPro bounds: its only user, `Area Title Control`, feeds them to title-card layout
 * only). */
static const act_vtable AV_GetMaterialColor = { "GetMaterialColor", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_PreBuildTK2DSprites = { "PreBuildTK2DSprites", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_SetSpriteRendererSprite = { "SetSpriteRendererSprite", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_GetMeshRendererBounds = { "GetMeshRendererBounds", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* Finish unless everyFrame: ACT/SetMaterialColor.cs:42-96, Tk2dSpriteGetColor.cs, Tk2dSpriteSetColor.cs:24-46 */
static const act_vtable AV_SetMaterialColor = { "SetMaterialColor", 0, NULL, act_finish_unless_every_frame, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_Tk2dSpriteGetColor = { "Tk2dSpriteGetColor", 0, NULL, act_finish_unless_every_frame, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_Tk2dSpriteSetColor = { "Tk2dSpriteSetColor", 0, NULL, act_finish_unless_every_frame, NULL, NULL, NULL, NULL, NULL, NULL };

/* Tk2dPlayAnimationV2 -- ACT/Tk2dPlayAnimationV2.cs:32-58: Play(clipName) unless doNotResetCurrentClip and it is
 * already the current clip; Finish() */
typedef struct { const fsm_pv *go, *clipName, *doNotReset; } st_tpa2;
static void tpa2_bind(act_inst *a) { ST(st_tpa2); s->go = FIELD(gameObject); s->clipName = FIELD(clipName); s->doNotReset = FIELD(doNotResetCurrentClip); }
static void tpa2_enter(act_inst *a)
{
    ST(st_tpa2);
    anim_inst *sp = sprite_of(a, s->go);
    const char *clip = w_str(w, ps(f, s->clipName));
    if (sp && pb(f, s->doNotReset) && sp->cur_clip < 0)
        HKSIM_UNIMPLEMENTED("Tk2dPlayAnimationV2: doNotResetCurrentClip with no current clip (NullReferenceException) in %s", fsm_label(f));
    if (sp && !(pb(f, s->doNotReset) && strcmp(anim_clip_name(w, sp), clip) == 0)) anim_play_name(w, sp, clip);
    act_finish(a);
}
static const act_vtable AV_Tk2dPlayAnimationV2 = { "Tk2dPlayAnimationV2", sizeof(st_tpa2), tpa2_bind, tpa2_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* Lifetime only (renderer state, see the top of this file):
 *   SetMeshRendererChildren -- ACT/SetMeshRendererChildren.cs:20-37: Finish() in OnEnter.
 *   GetSpriteRendererSprite -- ACT/GetSpriteRendererSprite.cs:20-27: Finish() in OnEnter; the store (a Sprite in an
 *     FsmObject) feeds only SetSpriteRendererSprite.
 *   SetSpriteRendererOrder -- ACT/SetSpriteRendererOrder.cs:25-58: Finish() in OnEnter unless a nonzero delay is set,
 *     then Finish() once `timer` (+= Time.deltaTime each Update) reaches it. */
static const act_vtable AV_SetMeshRendererChildren = { "SetMeshRendererChildren", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_GetSpriteRendererSprite = { "GetSpriteRendererSprite", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
typedef struct { const fsm_pv *delay; float timer; } st_ssro;
static void ssro_bind(act_inst *a) { ST(st_ssro); s->delay = FIELD(delay); }
static void ssro_enter(act_inst *a) { ST(st_ssro); s->timer = 0.0f; if (p_isnone(s->delay) || pf(f, s->delay) == 0.0f) act_finish(a); }
static void ssro_update(act_inst *a)
{
    ST(st_ssro);
    if (s->timer < pf(f, s->delay)) { s->timer += w->dt; return; }
    act_finish(a);
}
static const act_vtable AV_SetSpriteRendererOrder = { "SetSpriteRendererOrder", sizeof(st_ssro), ssro_bind, ssro_enter, ssro_update, NULL, NULL, NULL, NULL, NULL };

/* SetVisibility — ACT/SetVisibility.cs:19-64: toggles the renderer-enabled flag SetSpriteRenderer/SetMeshRenderer
 * write (mesh_renderer_enabled); like those, "no Renderer component" is not distinguished (every dumped object
 * this touches has one).  resetOnExit restores the value captured on entry. */
typedef struct { const fsm_pv *go, *toggle, *visible, *resetOnExit; int32_t target; uint8_t initial, has_initial; } st_setvis;
static void setvis_bind(act_inst *a) { ST(st_setvis); s->go = FIELD(gameObject); s->toggle = FIELD(toggle); s->visible = FIELD(visible); s->resetOnExit = FIELD(resetOnExit); s->target = -1; s->has_initial = 0; }
static void setvis_enter(act_inst *a)
{
    ST(st_setvis);
    s->target = p_owner_default(a, s->go);
    s->has_initial = 0;
    if (s->target >= 0) {
        s->initial = w->gos[s->target].mesh_renderer_enabled;
        s->has_initial = 1;
        w->gos[s->target].mesh_renderer_enabled = pb(f, s->toggle) ? (uint8_t)!s->initial : (pb(f, s->visible) ? 1 : 0);
    }
    act_finish(a);
}
static void setvis_exit(act_inst *a) { ST(st_setvis); if (s->has_initial && pb(f, s->resetOnExit)) w->gos[s->target].mesh_renderer_enabled = s->initial; }
static const act_vtable AV_SetVisibility = { "SetVisibility", sizeof(st_setvis), setvis_bind, setvis_enter, NULL, NULL, NULL, setvis_exit, NULL, NULL };

const act_vtable *const act_registry_tk2d[] = {
    &AV_Tk2dPlayAnimation, &AV_Tk2dPlayAnimationWithEvents, &AV_Tk2dPauseAnimation, &AV_Tk2dWatchAnimationEvents,
    &AV_Tk2dPlayFrame, &AV_Tk2dStopAnimation, &AV_LerpTk2dSpriteColor, &AV_EaseColor, &AV_SetSpriteRenderer,
    &AV_SetMeshRenderer, &AV_GetMaterialColor, &AV_PreBuildTK2DSprites, &AV_SetSpriteRendererSprite,
    &AV_GetMeshRendererBounds, &AV_SetMaterialColor, &AV_Tk2dSpriteGetColor, &AV_Tk2dSpriteSetColor,
    &AV_Tk2dPlayAnimationV2, &AV_SetMeshRendererChildren, &AV_GetSpriteRendererSprite, &AV_SetSpriteRendererOrder,
    &AV_SetVisibility,
};
const int act_registry_tk2d_n = (int)(sizeof act_registry_tk2d / sizeof act_registry_tk2d[0]);
