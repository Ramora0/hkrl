/* Audio, particle, vibration, camera-shake, text and debug actions.  None of their targets is modelled;
 * what is kept is control flow (when each action finishes) and ShakePositionV2's transform writes. */
#include "act.h"

/* Finish at once: ACT/AudioPlaySimple.cs, AudioStop.cs, TransitionToAudioSnapshot.cs, ApplyMusicCue.cs,
 * PlayParticleEmitter.cs, StopParticleEmitter.cs, SetTextMeshProText.cs, SetTextMeshProAlignment.cs,
 * SetAudioClip.cs:24-32, SetParticleEmission.cs:21-32, HK/VibrationPlayerStop.cs, VibrationPlayerPlay.cs:15-27,
 * FadeGroupUp.cs:16-27, FadeGroupDown.cs:18-36.  FormatString / DebugLogConsole: the formatted FsmString is
 * read only by DebugLogConsole and nothing branches on it, so it is left untouched. */
static const act_vtable AV_AudioPlaySimple = { "AudioPlaySimple", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_AudioStop = { "AudioStop", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_TransitionToAudioSnapshot = { "TransitionToAudioSnapshot", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_ApplyMusicCue = { "ApplyMusicCue", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_SetAudioClip = { "SetAudioClip", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_PlayParticleEmitter = { "PlayParticleEmitter", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_StopParticleEmitter = { "StopParticleEmitter", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_SetParticleEmission = { "SetParticleEmission", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_VibrationPlayerPlay = { "VibrationPlayerPlay", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_VibrationPlayerStop = { "VibrationPlayerStop", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_FadeGroupUp = { "FadeGroupUp", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_FadeGroupDown = { "FadeGroupDown", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_SetTextMeshProText = { "SetTextMeshProText", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_SetTextMeshProAlignment = { "SetTextMeshProAlignment", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_FormatString = { "FormatString", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_DebugLogConsole = { "DebugLogConsole", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
/* Comment -- ACT/Comment.cs:9-16: an editor note field, no runtime body beyond Finish().
 * DebugFloat -- ACT/DebugFloat.cs:22-29: PlayMaker log window text (see DebugLogConsole above).
 * AudioPlayRandomSingle -- ACT/AudioPlayRandomSingle.cs:26-40: Random.Range(pitchMin,pitchMax) picks a pitch for
 * a one-shot clip; audio is out of scope and nothing reads the pitch back, so the RNG draw is not replayed
 * (only lifetime is kept, like the no-ops above). */
static const act_vtable AV_Comment = { "Comment", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_DebugFloat = { "DebugFloat", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_AudioPlayRandomSingle = { "AudioPlayRandomSingle", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* Finish unless everyFrame: ACT/SetTextMeshProColor.cs:30-45, SetParticleEmissionSpeed.cs:26-48,
 * SetParticleEmissionRate.cs:22-46, SetAudioVolume.cs, SetAudioPitch.cs:38-45. */
static const act_vtable AV_SetTextMeshProColor = { "SetTextMeshProColor", 0, NULL, act_finish_unless_every_frame, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_SetParticleEmissionSpeed = { "SetParticleEmissionSpeed", 0, NULL, act_finish_unless_every_frame, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_SetParticleEmissionRate = { "SetParticleEmissionRate", 0, NULL, act_finish_unless_every_frame, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_SetAudioVolume = { "SetAudioVolume", 0, NULL, act_finish_unless_every_frame, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_SetAudioPitch = { "SetAudioPitch", 0, NULL, act_finish_unless_every_frame, NULL, NULL, NULL, NULL, NULL, NULL };

/* Never finish: ACT/AudioPlayInState.cs:26-53, PlayParticleEmitterInState.cs:18-49 (Play on enter, Stop on
 * exit).  FINISHED waits for every action in the state (FsmState.cs:609-620), so these leave the state to its
 * Wait / transitions, as in the game. */
static const act_vtable AV_AudioPlayInState = { "AudioPlayInState", 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_PlayParticleEmitterInState = { "PlayParticleEmitterInState", 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

/* AudioPlay -- ACT/AudioPlay.cs:37-83.  OnEnter plays the target's AudioSource and Finishes at once when there is
 * none or it is disabled (:42-43, :65); OnUpdate Finishes (and sends finishedEvent) once !audio.isPlaying (:73-77).  Audio
 * plays in real time whatever Time.timeScale is, so that moment is a wall-clock read (docs/frame-order.md
 * "Wall-clock reads"), and clip lengths are not dumped.  It changes the fight only through finishedEvent, or through
 * FINISHED when this action is the last unfinished one of a state that has a FINISHED transition (FsmState.cs:609-620
 * CheckAllActionsFinished): both trap (Q-pfsm-16).  Otherwise it never finishes, which changes nothing. */
typedef struct { const fsm_pv *go, *finishedEvent; } st_aplay;
static const act_vtable AV_AudioPlay;
static void aplay_bind(act_inst *a) { ST(st_aplay); s->go = FIELD(gameObject); s->finishedEvent = FIELD_OPT(finishedEvent); }
/* GetComponent<AudioSource>() (:42) is the first AudioSource; `audio.enabled` (:43) is its serialized flag, as no
 * ported action enables or disables an AudioSource.  -1 none, else enabled. */
static int aplay_source_enabled(const fsm_world *w, int32_t go)
{
    const go_def *g = w->gos[go].def;
    for (int32_t k = 0; k < g->n_comps; k++) {
        const comp_def *c = &w->sc->comps[g->comp_start + k];
        const char *t = w_str(w, c->type), *dot = strrchr(t, '.');
        if (strcmp(dot ? dot + 1 : t, "AudioSource") == 0) return c->enabled;
    }
    return -1;
}
static void aplay_enter(act_inst *a)
{
    ST(st_aplay);
    int32_t t = p_owner_default(a, s->go);
    if (t < 0 || aplay_source_enabled(f->w, t) <= 0) { act_finish(a); return; }   /* :40-43, :65 */
    if (EV(s->finishedEvent) >= 0) HKSIM_UNKNOWN("AudioPlay.finishedEvent needs the clip length (not dumped) in %s", fsm_label(f));
}
static void aplay_update(act_inst *a)
{
    fsm_inst *f = a->fsm; state_inst *st = a->state;
    for (int32_t i = 0; i < st->n_active; i++) {
        const act_inst *o = &st->acts[st->active_actions[i]];
        if (!o->finished && o->vt != &AV_AudioPlay) return;          /* another action still holds the state */
    }
    int32_t fin = w_get_fsm_event(f->w, "FINISHED");
    for (int32_t i = 0; i < st->def->n_trans; i++)
        if (f->w->sc->trans[st->def->trans_start + i].event == fin)
            HKSIM_UNKNOWN("AudioPlay in %s is the last unfinished action of a state with a FINISHED transition: the state "
                          "ends when the clip stops playing, in real time (clip length not dumped)", fsm_label(f));
}
static const act_vtable AV_AudioPlay = { "AudioPlay", sizeof(st_aplay), aplay_bind, aplay_enter, aplay_update, NULL, NULL, NULL, NULL, NULL };

/* FadeAudio -- ACT/FadeAudio.cs:33-85: the volume ramp decides when the action finishes.  Each update adds
 * (end - start) * dt / time to the volume: timeElapsed is zeroed at the end of every update (:83).  The volume lives on
 * the AudioSource and each write goes through AudioSource::SetVolume (UP!0x180aa5ff0: `0 > v ? 0 : minss(1.0f, v)`,
 * constant @0x1815d5970), so a ramp whose endVolume lies outside [0, 1] never reaches it.  No AudioSource on the
 * target: ComponentAction.UpdateCache (ComponentAction.cs:25-41) fails and the action does nothing, never finishing. */
typedef struct { const fsm_pv *go, *startVolume, *endVolume, *time; float timeElapsed, timePercentage, volume; bool fadingDown; } st_fade;
static void fade_bind(act_inst *a) { ST(st_fade); s->go = FIELD(gameObject); s->startVolume = FIELD(startVolume); s->endVolume = FIELD(endVolume); s->time = FIELD(time); }
static float audio_set_volume(float v) { return 0.0f > v ? 0.0f : (1.0f < v ? 1.0f : v); }
static bool fade_cache(act_inst *a)
{
    ST(st_fade);
    int32_t t = p_owner_default(a, s->go);
    return t >= 0 && go_has_component(w, t, "AudioSource");
}
static void fade_enter(act_inst *a)
{
    ST(st_fade);
    s->fadingDown = pf(f, s->startVolume) > pf(f, s->endVolume);                                           /* :35-42 */
    if (fade_cache(a)) s->volume = audio_set_volume(pf(f, s->startVolume));                               /* :44-47 */
}
static void fade_exit(act_inst *a)
{
    ST(st_fade);
    if (fade_cache(a)) s->volume = audio_set_volume(pf(f, s->endVolume));                                 /* :53-56 */
}
static void fade_update(act_inst *a)
{
    ST(st_fade);
    if (!fade_cache(a)) return;                                                                            /* :67 */
    s->timeElapsed += w->dt;
    s->timePercentage = (float)((double)s->timeElapsed / (double)pf(f, s->time) * 100.0);                 /* :70 compound */
    float num = (float)(((double)pf(f, s->endVolume) - (double)pf(f, s->startVolume)) * ((double)s->timePercentage / 100.0));   /* :71 */
    s->volume = audio_set_volume(s->volume + num);                                                         /* :72 */
    if (s->fadingDown && s->volume <= pf(f, s->endVolume)) { s->volume = audio_set_volume(pf(f, s->endVolume)); act_finish(a); }
    else if (!s->fadingDown && s->volume >= pf(f, s->endVolume)) { s->volume = audio_set_volume(pf(f, s->endVolume)); act_finish(a); }
    s->timeElapsed = 0.0f;                                                                                 /* :83 */
}
static const act_vtable AV_FadeAudio = { "FadeAudio", sizeof(st_fade), fade_bind, fade_enter, fade_update, NULL, NULL, fade_exit, NULL, NULL };

/* AudioPlayerOneShot -- ACT/AudioPlayerOneShot.cs:49-97, AudioPlayerOneShotSingle -- AudioPlayerOneShotSingle.cs:42-93:
 * play after `delay`, then finish.  The clip and pitch picks only choose a sound. */
typedef struct { const fsm_pv *delay; float timer; } st_apos;
static void apos_bind(act_inst *a) { ST(st_apos); s->delay = FIELD(delay); }
static float apos_delay(act_inst *a) { ST(st_apos); return (!s->delay || p_isnone(s->delay)) ? 0.0f : pf(f, s->delay); }
static void apos_enter(act_inst *a) { ST(st_apos); s->timer = 0.0f; if (apos_delay(a) == 0.0f) act_finish(a); }
static void apos_update(act_inst *a)
{
    ST(st_apos);
    float del = apos_delay(a);
    if (del > 0.0f) {
        if (s->timer < del) { s->timer += w->dt; return; }
        act_finish(a);
    }
}
static const act_vtable AV_AudioPlayerOneShot = { "AudioPlayerOneShot", sizeof(st_apos), apos_bind, apos_enter, apos_update, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_AudioPlayerOneShotSingle = { "AudioPlayerOneShotSingle", sizeof(st_apos), apos_bind, apos_enter, apos_update, NULL, NULL, NULL, NULL, NULL };

/* AudioPlayRandom -- ACT/AudioPlayRandom.cs:34-58: picks and plays a clip, then finishes */
static const act_vtable AV_AudioPlayRandom = { "AudioPlayRandom", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* PlayVibration -- HK/PlayVibration.cs:36-76: the VibrationManager calls are no-ops; the loop timing is kept. */
typedef struct { const fsm_pv *loopTime; float cooldown; } st_pvib;
static void pvib_bind(act_inst *a) { ST(st_pvib); s->loopTime = FIELD(loopTime); }
static void pvib_enqueue(act_inst *a)
{
    ST(st_pvib);
    float num = p_isnone(s->loopTime) ? 0.0f : pf(f, s->loopTime);
    if (num < 1.401298E-45f) act_finish(a); else s->cooldown = num;   /* Mathf.Epsilon */
}
static void pvib_update(act_inst *a) { ST(st_pvib); s->cooldown -= w->dt; if (s->cooldown <= 0.0f) pvib_enqueue(a); }
static const act_vtable AV_PlayVibration = { "PlayVibration", sizeof(st_pvib), pvib_bind, pvib_enqueue, pvib_update, NULL, NULL, NULL, NULL, NULL };

/* ShakePositionV2 -- ACT/ShakePositionV2.cs:48-120: 3 Range(-1,1) draws per UpdateShaking (:96), once in OnEnter
 * and once per OnUpdate.  FpsLimit > 0 would throttle UpdateShaking by Time.unscaledTime (:82-89), which stays
 * wall clock even in capture mode (native-transform_time.md T3) -- but every regime-R2 oracle process installs
 * RegimeTweaks.PinShakeFpsLimit (oracle/Env/RegimeTweaks.cs:148-172, fixing B27), which IL-hooks both of
 * UpdateShaking's FpsLimit.Value reads (:82, :88) to the constant 0 for every instance, pooled and Instantiated
 * included; FpsLimit is read nowhere else in the class.  So the `> 0f` branch at :82 never executes in any
 * regime-conformant recording or eval and the field's dumped value (0 in the pre-2026-09-24 corpora, 60 in
 * analysis/dumps_v2 -- whichever real session or oracle build set that FSM variable last, not a per-scene
 * constant) is not observable game state: the port always takes the untimed path.  ConfigManager
 * .CameraShakeMultiplier is 1. */
typedef struct { const fsm_pv *Target, *Extents, *Duration, *IsLooping, *StopEvent; float timer; int32_t target; float start[3]; } st_shake;
static void shake_bind(act_inst *a) { ST(st_shake); s->Target = FIELD(Target); s->Extents = FIELD(Extents); s->Duration = FIELD(Duration); s->IsLooping = FIELD(IsLooping); s->StopEvent = FIELD(StopEvent); s->target = -1; }
static void shake_stop_reset(act_inst *a) { ST(st_shake); if (s->target >= 0) { go_set_world_pos(w, s->target, s->start); s->target = -1; } }
static void shake_update(act_inst *a)
{
    ST(st_shake);
    if (s->target >= 0) {
        s->timer += w->dt;
        bool looping = pb(f, s->IsLooping);
        float num = 1.0f;
        if (!looping) {                                             /* :91 compound -> double stack, then Mathf.Clamp01 */
            num = (float)(1.0 - (double)s->timer / (double)pf(f, s->Duration));
            num = num < 0.0f ? 0.0f : (num > 1.0f ? 1.0f : num);
        }
        const float *ext = pv3(f, s->Extents);
        float rx = hk_rng_range_f_site(w->rng, a->rng_site, -1.0f, 1.0f), ry = hk_rng_range_f_site(w->rng, a->rng_site, -1.0f, 1.0f), rz = hk_rng_range_f_site(w->rng, a->rng_site, -1.0f, 1.0f);
        float p[3] = { s->start[0] + ext[0] * rx * num, s->start[1] + ext[1] * ry * num, s->start[2] + ext[2] * rz * num };
        go_set_world_pos(w, s->target, p);
        if (!looping && s->timer > pf(f, s->Duration)) { shake_stop_reset(a); fsm_event(f, EV(s->StopEvent)); act_finish(a); }
    } else {
        shake_stop_reset(a); fsm_event(f, EV(s->StopEvent)); act_finish(a);
    }
}
static void shake_enter(act_inst *a)
{
    ST(st_shake);
    s->timer = 0.0f;
    int32_t t = p_get_safe(a, s->Target);
    if (t >= 0) { s->target = t; go_world_pos(w, t, s->start); } else s->target = -1;
    shake_update(a);
}
static const act_vtable AV_ShakePositionV2 = { "ShakePositionV2", sizeof(st_shake), shake_bind, shake_enter, shake_update, NULL, NULL, shake_stop_reset, NULL, NULL };

/* Lifetime only:
 *   SetAudioSource -- ACT/SetAudioSource.cs:20-35, SetTextMeshText -- ACT/SetTextMeshText.cs:22-33: Finish() in OnEnter.
 *   SetCameraCullingMask -- ACT/SetCameraCullingMask.cs:30-44: Finish() unless everyFrame.
 *   PlayAudioEvent -- HK/PlayAudioEvent.cs:31-50: OnEnter spawns a one-shot audio player (AudioEvent.SpawnAndPlayOneShot,
 *     an `Audio Player Actor` whose PlayAudioAndRecycle recycles it when the clip ends) and never Finish()es. */
static const act_vtable AV_SetAudioSource = { "SetAudioSource", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_SetTextMeshText = { "SetTextMeshText", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_SetCameraCullingMask = { "SetCameraCullingMask", 0, NULL, act_finish_unless_every_frame, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_PlayAudioEvent = { "PlayAudioEvent", 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

/* CameraFadeIn -- ACT/CameraFadeIn.cs:37-62, CameraFadeInWithDelay -- :46-82, CameraFadeOut -- ACT/CameraFadeOut.cs:
 * 37-59: a full-screen GUI tint; only the clock is kept.  currentTime += Time.deltaTime each Update (realTime reads
 * the wall clock: trapped); past `time` the fade sends finishEvent and (in and in-with-delay) Finish()es; the delayed
 * one first waits `delay`, then restarts its clock.  The fade-out sends finishEvent every Update past `time` and never
 * Finish()es. */
typedef struct { const fsm_pv *time, *delay, *finishEvent, *realTime; float t; uint8_t delay_passed; } st_cfade;
static void cfade_bind(act_inst *a) { ST(st_cfade); s->time = FIELD(time); s->delay = FIELD_OPT(delay); s->finishEvent = FIELD(finishEvent); s->realTime = FIELD(realTime); }
static void cfade_enter(act_inst *a)
{
    ST(st_cfade);
    if (pb(f, s->realTime)) HKSIM_UNIMPLEMENTED("%s with realTime reads the wall clock (%s)", a->vt->type_short, fsm_label(f));
    s->t = 0.0f;
}
static void cfade_in_update(act_inst *a)
{
    ST(st_cfade);
    s->t += w->dt;
    if (s->t > pf(f, s->time)) { if (EV(s->finishEvent) >= 0) fsm_event(f, EV(s->finishEvent)); act_finish(a); }
}
static void cfade_delay_update(act_inst *a)
{
    ST(st_cfade);
    s->t += w->dt;
    if (!s->delay_passed && s->t > pf(f, s->delay)) { s->t = 0.0f; s->delay_passed = 1; }
    if (!s->delay_passed) return;
    if (s->t > pf(f, s->time)) {
        if (EV(s->finishEvent) >= 0) fsm_event(f, EV(s->finishEvent));
        s->delay_passed = 0;
        act_finish(a);
    }
}
static void cfade_out_update(act_inst *a)
{
    ST(st_cfade);
    s->t += w->dt;
    if (s->t > pf(f, s->time) && EV(s->finishEvent) >= 0) fsm_event(f, EV(s->finishEvent));
}
static const act_vtable AV_CameraFadeIn = { "CameraFadeIn", sizeof(st_cfade), cfade_bind, cfade_enter, cfade_in_update, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_CameraFadeInWithDelay = { "CameraFadeInWithDelay", sizeof(st_cfade), cfade_bind, cfade_enter, cfade_delay_update, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_CameraFadeOut = { "CameraFadeOut", sizeof(st_cfade), cfade_bind, cfade_enter, cfade_out_update, NULL, NULL, NULL, NULL, NULL };

const act_vtable *const act_registry_audio_fx[] = {
    &AV_AudioPlaySimple, &AV_AudioStop, &AV_TransitionToAudioSnapshot, &AV_ApplyMusicCue, &AV_SetAudioClip,
    &AV_PlayParticleEmitter, &AV_StopParticleEmitter, &AV_SetParticleEmission, &AV_VibrationPlayerPlay,
    &AV_VibrationPlayerStop, &AV_FadeGroupUp, &AV_FadeGroupDown, &AV_SetTextMeshProText, &AV_SetTextMeshProAlignment,
    &AV_FormatString, &AV_DebugLogConsole, &AV_SetTextMeshProColor, &AV_SetParticleEmissionSpeed,
    &AV_SetParticleEmissionRate, &AV_SetAudioVolume, &AV_SetAudioPitch, &AV_AudioPlayInState,
    &AV_PlayParticleEmitterInState, &AV_AudioPlay, &AV_FadeAudio, &AV_AudioPlayerOneShot,
    &AV_AudioPlayerOneShotSingle, &AV_AudioPlayRandom, &AV_PlayVibration, &AV_ShakePositionV2,
    &AV_SetAudioSource, &AV_SetTextMeshText, &AV_SetCameraCullingMask, &AV_PlayAudioEvent, &AV_CameraFadeIn, &AV_CameraFadeInWithDelay, &AV_CameraFadeOut,
    &AV_Comment, &AV_DebugFloat, &AV_AudioPlayRandomSingle,
};
const int act_registry_audio_fx_n = (int)(sizeof act_registry_audio_fx / sizeof act_registry_audio_fx[0]);
