/* UnityEngine.Animator (Mecanim) for the controller shape the ported scenes run: one layer, one state holding one
 * clip, no parameters, no transitions, Normal update mode, AlwaysAnimate or CullCompletely culling, no root motion,
 * no animation events, controller state not kept on disable (sim/fsm/gen/mecanim.py stops the generator on anything
 * else).  A CullCompletely instance traps instead of running (A-26 below): the sim has no camera-visibility model.
 * Ported from the native engine: analysis/native_specs/native-animator.md (N-AN-*), citing
 * analysis/decomp_native (UP!<address>).
 *
 * Stage (N-AN-1).  A Normal-mode Animator's graph prepares (advances the state clock and samples the clip) in
 * PreLateUpdate/DirectorUpdateAnimationBegin and writes its bound properties in
 * PreLateUpdate/DirectorUpdateAnimationEnd (UP!0x180113120 AnimationPlayableOutput::GetStages: FK pass stage 3, IK
 * pass stage 4; UP!0x1800e41b0 Animator::SetPrepareStage).  Both run in the lifecycle's `anim` stage (LCS_ANIM),
 * after update_delayed's env coroutine and before LateUpdate: mecanim_stage runs the Begin half for every Animator,
 * then the End half.
 *
 * dt (N-AN-2).  Sampled at frame start from Time.deltaTime (UP!0x180897e80 DirectorSampleTime): 0.02 in a live
 * frame, 0 in a frozen one, whatever timeScale does later in the frame.  The core passes exactly that to LCS_ANIM.
 *
 * Activation (N-AN-3 and its Check).  Animator::AddToManager (UP!0x1800d9460) builds the graph and GenerateGraph
 * (UP!0x18010fff0) puts the default state in at normalized time 0 with dt 0; the queued Play starts the graph at the
 * next ExecuteStage, again with dt 0 (UP!0x180899210 ProcessPlayStateChanges).  So the first advance is the next
 * DirectorUpdateAnimationBegin: the same frame for an object activated before it, the next frame for one activated
 * in or after it.  Deactivation (UP!0x1800dc320 Animator::Deactivate -> ClearObject) clears the controller state
 * when m_KeepAnimatorControllerStateOnDisable is false: every re-activation (a pool re-spawn) restarts at 0.
 * Nothing advances between activation and the next Begin, so building the state lazily at that Begin
 * (`playing` 0 -> 1, nt 0) is the same clock; an object activated during this stage's End half is not in the Begin
 * set and starts next frame.
 *
 * Clock (N-AN-4).  UP!0x18014e900 EvaluateState: nt = (s*dt)/D + nt in float32, s = |state speed| * speed
 * parameter (1: none) * Animator.speed (1) (UP!0x18014d940 ComputeStateSpeed), D = |(stop-start) * node duration|
 * (UP!0x18014ebc0 EvaluateStateDuration -> UP!0x18012d340 EvaluateBlendTree), 1 when 0.
 *
 * Clip time (N-AN-6).  The clip playable's time is f32(len*nt) as a double (UP!0x18010dab0
 * PropagateStateMachineInfoToChildClips), normalized back by ToFloatRoundUp(time/len) (UP!0x18010bb20,
 * UP!0x18010e3a0), then UP!0x180d5c0f0 ComputeClipTime: frac for a looping clip, clamp to [0,1] otherwise, times
 * (stop-start) plus start.
 *
 * Curves (N-AN-7a/b).  Streamed segments, the last one whose key time is <= t (UP!0x18012fb60 SeekClipForward:
 * inclusive), evaluated in float32 Horner form at u = t - key (UP!0x18012d7c0 EvaluateCaches); a constant curve
 * is one segment from -FLT_MAX.
 *
 * Writes (N-AN-9/10).  Transform properties are written by WriteStep for every Animator, then the generic ones on
 * the main thread per Animator whose object is active (UP!0x1800e6b80 UpdateAvatars).  A float bound to a bool
 * (Behaviour / Collider2D m_Enabled) is false iff -0.001 <= v <= 0.001, and only a change has a side effect
 * (UP!0x1800bbf30 SetBoundCurveFloatValue; UP!0x1800c44b0 SetGenericFloatPropertyValues -> AwakeFromLoad(0x10) ->
 * UP!0x180bfec70 Collider2D::AwakeFromLoad: fixtures created or destroyed at once).  GameObject.m_IsActive calls
 * GameObject::SetSelfActive with the same threshold every frame (UP!0x180580c90, the path GameObject.SetActive
 * takes).  Culling mode 0 (AlwaysAnimate) makes all of this independent of the camera (N-AN-8); mode 2
 * (CullCompletely) pauses PreLateUpdate for an off-screen instance (SyncPlayStateToCulling, UP!0x1800e5460) --
 * mecanim_stage traps instead of guessing camera visibility (A-26). */
#include "fsm/fsm.h"
#include "world_internal.h"
#include <float.h>
#include <math.h>
#include <string.h>

/* UP!0x18010e3a0 ToFloatRoundUp: the least float >= d (the bit-pattern step there moves toward +inf). */
static float to_float_round_up(double d)
{
    float f = (float)d;
    if (d <= (double)f || f == FLT_MAX) return f;
    return nextafterf(f, INFINITY);
}

static float mec_sample(const fsm_world *w, const mec_bind_def *b, float t)
{
    const mec_key_def *k = &w->sc->mec_keys[b->key_start];
    int32_t i = 0;
    while (i + 1 < b->n_keys && k[i + 1].t <= t) i++;
    float u = t - k[i].t;
    return ((u * k[i].c[0] + k[i].c[1]) * u + k[i].c[2]) * u + k[i].c[3];
}

static bool mec_bool(float v) { return !(v <= 0.001f && v >= -0.001f); }   /* kBindFloatToBool / GameObjectActive */

/* DirectorUpdateAnimationBegin: advance the state clock and fix this frame's clip time. */
static void mec_begin(fsm_world *w, mec_inst *m, float dt)
{
    const mec_anim_def *d = m->def;
    const mec_clip_def *c = &w->sc->mec_clips[d->clip];
    if (!m->playing) { m->playing = 1; m->nt = 0.0f; }            /* default state entered at normalized time 0 */
    float len = c->stop - c->start;
    float s = fabsf(d->speed) * 1.0f * 1.0f;                     /* speed >= 0 (mecanim.py check_shape): no reversal */
    float D = fabsf(len * d->node_duration) * 1.0f + 0.0f;
    if (D == 0.0f) D = 1.0f;
    float delta = (s * dt) / D;
    m->nt = delta + m->nt;
    float tn = to_float_round_up((double)(len * m->nt) / (double)len);
    float f;
    if (c->loop) { float ip; f = modff(tn, &ip); }
    else f = tn < 0.0f ? 0.0f : (tn > 1.0f ? 1.0f : tn);
    m->t = f * (c->stop - c->start) + c->start;
    m->evaluated = 1;
}

/* DirectorUpdateAnimationEnd, WriteStep: the Transform bindings (grouped x,y,z per property, mecanim.py). */
static void mec_write_transforms(fsm_world *w, const mec_inst *m)
{
    const mec_anim_def *d = m->def;
    for (int32_t k = 0; k < d->n_binds; k++) {
        const mec_bind_def *b = &w->sc->mec_binds[d->bind_start + k];
        if (b->prop != MEC_LOCAL_POS && b->prop != MEC_LOCAL_SCALE && b->prop != MEC_LOCAL_EULER_Z) continue;
        if (b->prop == MEC_LOCAL_EULER_Z) { go_set_local_euler_z(w, b->go, mec_sample(w, b, m->t)); continue; }
        HKSIM_ASSERT(k + 2 < d->n_binds, "Animator transform binding group on '%s' is not x,y,z", go_path(w, b->go));
        float v[3];
        for (int a = 0; a < 3; a++) v[a] = mec_sample(w, &b[a], m->t);
        if (b->prop == MEC_LOCAL_POS) go_set_local_pos(w, b->go, v);
        else go_set_local_scale(w, b->go, v);
        k += 2;
    }
}

/* DirectorUpdateAnimationEnd, main thread: the generic bindings in binding order. */
static void mec_write_generic(fsm_world *w, const mec_inst *m)
{
    const mec_anim_def *d = m->def;
    for (int32_t k = 0; k < d->n_binds; k++) {
        const mec_bind_def *b = &w->sc->mec_binds[d->bind_start + k];
        switch (b->prop) {
        case MEC_COLLIDER_ENABLED: {
            go_inst *g = &w->gos[b->go];
            HKSIM_ASSERT(b->index < g->n_cols, "Animator binding: collider %d of '%s'", b->index, go_path(w, b->go));
            bool on = mec_bool(mec_sample(w, b, m->t));
            if ((g->cols[b->index].enabled != 0) != on) col_set_enabled(w, &g->cols[b->index], on);
            break;
        }
        case MEC_GO_ACTIVE:
            go_set_active(w, b->go, mec_bool(mec_sample(w, b, m->t)));
            break;
        case MEC_LOCAL_POS: case MEC_LOCAL_SCALE: case MEC_LOCAL_EULER_Z:
            break;
        default:
            HKSIM_UNIMPLEMENTED("Animator binding kind %d on '%s'", b->prop, go_path(w, b->go));
        }
    }
}

static bool mec_runs(const fsm_world *w, const mec_inst *m)
{
    return m->enabled && !w->gos[m->go].destroyed && go_active_in_hierarchy(w, m->go);
}

/* PreLateUpdate/DirectorUpdateAnimationBegin + End.  Writes can activate or deactivate objects (m_IsActive): the
 * instance array does not move within a stage (world_reserve runs between stages), so `m` stays valid, and an
 * Animator a write deactivates is not written (UpdateAvatars skips an inactive object; mecanim_go_disable cleared
 * its state). */
void mecanim_stage(fsm_world *w, float dt)
{
    int32_t n = w->n_mecs;
    for (int32_t i = 0; i < n; i++) {
        mec_inst *m = &w->mecs[i];
        m->evaluated = 0;
        if (mec_runs(w, m)) {
            /* CullCompletely (m_CullingMode 2): every Animator that reaches MEC_ANIMS binds a gameplay property
             * (mecanim.py plan/check_shape), so the sim would need to know whether it is on-screen to decide if
             * this write is paused; it has no camera-visibility model (native-animator.md A-26). */
            if (m->def->cull_completely)
                HKSIM_UNIMPLEMENTED("Animator on '%s' is CullCompletely and binds a gameplay property: no "
                                    "camera-visibility model (native-animator.md A-26)", go_path(w, m->go));
            mec_begin(w, m, dt);
        }
    }
    for (int32_t i = 0; i < n; i++)
        if (w->mecs[i].evaluated && mec_runs(w, &w->mecs[i])) mec_write_transforms(w, &w->mecs[i]);
    for (int32_t i = 0; i < n; i++)
        if (w->mecs[i].evaluated && mec_runs(w, &w->mecs[i]) && w->mecs[i].playing) mec_write_generic(w, &w->mecs[i]);
}

/* Animator::Deactivate -> ClearObject: the controller state is dropped (keepAnimatorControllerStateOnDisable is
 * false on every emitted Animator); lifecycle.c deactivate_rec. */
void mecanim_go_disable(fsm_world *w, int32_t go)
{
    int32_t i = w->gos[go].mec;
    if (i >= 0) { w->mecs[i].playing = 0; w->mecs[i].evaluated = 0; }
}

void mecanim_init(fsm_world *w, int32_t i)
{
    mec_inst *m = &w->mecs[i];
    m->def = &w->sc->mec_anims[i];
    m->go = m->def->go;
    m->enabled = m->def->enabled;
    m->playing = 0;
    m->evaluated = 0;
    m->nt = 0.0f;
    m->t = 0.0f;
    w->gos[m->go].mec = i;
}
