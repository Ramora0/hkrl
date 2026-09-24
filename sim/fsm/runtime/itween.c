/* iTween runtime — port of analysis/decomp/Assembly-CSharp/iTween.cs for the tweens the iTween* FSM actions
 * launch (ScaleTo/ScaleBy, MoveTo/MoveBy, RotateTo/RotateBy, ShakePosition, FadeTo).
 *
 * Lifecycle (cite): ScaleTo :703-716 -> Launch :3747-3759 (tweens.Insert(0, args); AddComponent<iTween> ->
 * Awake :3579-3583 -> RetrieveArgs :3812-3955); Start :3585-3592 (delay > 0 -> TweenDelay coroutine :2320-2329,
 * else TweenStart :2331-2348); Update :3594-3619 (percentage < 1 ? TweenUpdate :2361-2366 : TweenComplete
 * :2368-2393); ApplyScaleToTargets :2065-2075; UpdatePercentage :4060-4078; Dispose :4096-4107; CallBack
 * :4081-4094 (SendMessage -> iTweenFSMEvents.cs:11-33); Stop(GameObject, string) :3496-3507; ConflictCheck
 * :4109-4147; EnableKinematic :4149-4151 (empty); physics = GetComponent<Rigidbody>() != null (:3834-3837,
 * a 3D Rigidbody: none in the scene dump -> Update-driven).
 *
 * Timing: Start (TweenStart) and ticks are dispatched by lifecycle.c (R1-R3).  A tween ticks in frozen frames
 * too (Time.deltaTime = 0, not gated by FsmPauseGate); the FSM sees completion through action.Finish() at its
 * next ungated Update. */
#include <stdio.h>
#include "fsm/fsm.h"
#include "fsm/lifecycle.h"
#include <stdlib.h>
#include <string.h>
#include "core/alloc.h"   /* per-instance arena */

/* Mathf.Clamp01 */
static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
/* clerp :4167-4184 (2D model: z axis only) -- picks the shortest angular path from `start` to `end`,
 * used by RotateTo (:1656) to precompute the actual target angle before easing samples along it, so a
 * 350-degree turn goes -10 degrees rather than the long way around. */
static float clerp(float start, float end, float value)
{
    float n2 = 360.0f;
    float n3 = fabsf((n2 - 0.0f) / 2.0f);
    if (end - start < 0.0f - n3) return start + (n2 - start + end) * value;
    if (end - start > n3) return start + (0.0f - (n2 - end + start)) * value;
    return start + (end - start) * value;
}

/* easing functions — the C# bodies are compound expressions: Mono evaluates them on a double stack and
 * rounds once at the float return / float call argument (docs/float-parity.md) */
static float ease_linear(float start, float end, float value)            /* :4162-4165 -> Mathf.Lerp: a + (b - a) * Clamp01(t) */
{
    return (float)((double)start + ((double)end - (double)start) * (double)clamp01(value));
}
static float ease_out_sine(float start, float end, float value)          /* :4299-4303 */
{
    end = end - start;                                                    /* end -= start (single op) */
    float arg = (float)((double)value / 1.0 * (double)((float)M_PI / 2.0f));   /* float argument of Mathf.Sin */
    return (float)((double)end * (double)m_sin(arg) + (double)start);
}
static float ease_out_quad(float start, float end, float value)          /* :4200-4204 */
{
    end = end - start;
    return (float)((0.0 - (double)end) * (double)value * ((double)value - 2.0) + (double)start);
}
static float ease_out_cubic(float start, float end, float value)          /* :4224-4229 */
{
    value = value - 1.0f;                                                 /* value -= 1f (single op) */
    end = end - start;
    return (float)((double)end * ((double)value * (double)value * (double)value + 1.0) + (double)start);
}
static float ease_in_sine(float start, float end, float value)           /* :4293-4297 */
{
    end = end - start;
    float arg = (float)((double)value / 1.0 * (double)((float)M_PI / 2.0f));
    return (float)((0.0 - (double)end) * (double)m_cos(arg) + (double)end + (double)start);
}
static float ease_in_out_sine(float start, float end, float value)       /* :4305-4309 */
{
    end = end - start;                                                    /* end -= start (single op) */
    float arg = (float)((double)((float)M_PI) * (double)value / 1.0);     /* float argument of Mathf.Cos */
    return (float)((0.0 - (double)end) / 2.0 * ((double)m_cos(arg) - 1.0) + (double)start);
}
/* the rest of GetEasingFunction (:3957-4058), restricted to the ease values the dumped arenas use */
static float ease_in_out_quad(float start, float end, float value)       /* :4206-4215 */
{
    value = value / 0.5f;                                                 /* value /= 0.5f (single op) */
    end = end - start;                                                    /* end -= start (single op) */
    if (value < 1.0f) return (float)((double)end / 2.0 * (double)value * (double)value + (double)start);
    value = value - 1.0f;                                                 /* value -= 1f (single op) */
    return (float)((0.0 - (double)end) / 2.0 * ((double)value * ((double)value - 2.0) - 1.0) + (double)start);
}
static float ease_in_out_cubic(float start, float end, float value)      /* :4231-4241 */
{
    value = value / 0.5f;
    end = end - start;
    if (value < 1.0f) return (float)((double)end / 2.0 * (double)value * (double)value * (double)value + (double)start);
    value = value - 2.0f;                                                 /* value -= 2f (single op) */
    return (float)((double)end / 2.0 * ((double)value * (double)value * (double)value + 2.0) + (double)start);
}
static float ease_out_circ(float start, float end, float value)          /* :4341-4346 */
{
    value = value - 1.0f;                                                 /* value -= 1f (single op) */
    end = end - start;
    float arg = (float)(1.0 - (double)value * (double)value);            /* float argument of Mathf.Sqrt */
    return (float)((double)end * (double)m_sqrt(arg) + (double)start);
}
static float ease_spring(float start, float end, float value)            /* :4187-4192 */
{
    value = clamp01(value);
    float sin_arg = (float)((double)value * (double)M_PI * (0.2 + 2.5 * (double)value * (double)value * (double)value));  /* float arg of Mathf.Sin */
    float pow_arg = 1.0f - value;                                         /* float arg of Mathf.Pow (single op) */
    value = (float)(((double)m_sin(sin_arg) * (double)m_pow(pow_arg, 2.2f) + (double)value) * (1.0 + 1.2 * (1.0 - (double)value)));
    return (float)((double)start + ((double)end - (double)start) * (double)value);
}
/* Shared ease table, iTween.EaseType (iTween.cs:11-46), GetEasingFunction :3957-4058 -- restricted to the
 * ease values any action uses in analysis/dumps_all/STAR/fsm.json.  EaseFsmAction (act_core.c EaseFloat) uses
 * it too: its ease bodies (EaseFsmAction.cs) are identical to iTween.cs's. */
float itween_ease_value(int32_t ease_type, float start, float end, float value, const char *label)
{
    switch (ease_type) {
    case 1:  return ease_out_quad(start, end, value);
    case 2:  return ease_in_out_quad(start, end, value);                  /* easeInOutQuad (enum :2) */
    case 4:  return ease_out_cubic(start, end, value);                    /* easeOutCubic (enum :15) */
    case 5:  return ease_in_out_cubic(start, end, value);                 /* easeInOutCubic (enum :6) */
    case 12: return ease_in_sine(start, end, value);
    case 13: return ease_out_sine(start, end, value);
    case 14: return ease_in_out_sine(start, end, value);                  /* easeInOutSine (enum :25) */
    case 19: return ease_out_circ(start, end, value);                     /* easeOutCirc (enum :20) */
    case 21: return ease_linear(start, end, value);
    case 22: return ease_spring(start, end, value);                       /* spring (enum :23) */
    default:
        HKSIM_UNIMPLEMENTED("ease type %d on '%s' not ported (census over analysis/dumps_all/STAR/fsm.json "
                            "found only easeOutQuad/easeInOutQuad/easeOutCubic/easeInOutCubic/easeInSine/"
                            "easeOutSine/easeInOutSine/easeOutCirc/linear/spring used by any action, any arena)", ease_type, label);
        return start;
    }
}
static float ease_apply(fsm_world *w, const itween_inst *t, float start, float end, float value)
{
    return itween_ease_value(t->ease, start, end, value, go_name(w, t->go));
}

static itween_inst *itween_new(fsm_world *w)
{
    if (w->n_itweens == w->cap_itweens) {
        w->cap_itweens = w->cap_itweens ? w->cap_itweens * 2 : 8;
        w->itweens = realloc(w->itweens, sizeof(itween_inst) * (size_t)w->cap_itweens);
    }
    itween_inst *t = &w->itweens[w->n_itweens++];
    memset(t, 0, sizeof *t);
    return t;
}

/* Dispose :4096-4107: remove the args from `tweens`, Destroy(this) (end of frame; no callback can reach a
 * disposed tween through Update any more, Stop/ConflictCheck use GetComponents which still sees it until the
 * end of the frame — irrelevant for the single-tween owners here) */
static void itween_dispose(itween_inst *t) { t->alive = 0; t->running = 0; }

/* CallBack(name) :4081-4094 -> gameObject.SendMessage("iTweenOnStart"/"iTweenOnComplete", itweenID) ->
 * iTweenFSMEvents.cs:11-33 on every events component of the GO whose itweenID matches */
static void itween_on_start(fsm_world *w, itween_inst *t)
{
    (void)w;
    if (!t->action) return;
    if (t->start_event >= 0) fsm_event(t->action->fsm, t->start_event);   /* Fsm.Event(startEvent); null -> no-op (Fsm.cs:2192-2198) */
}
static void itween_on_complete(fsm_world *w, itween_inst *t)
{
    (void)w;
    if (!t->action) return;
    if (t->is_looping && t->donotfinish) return;                          /* iTweenFSMEvents.cs:23-27 */
    act_inst *a = t->action;
    if (t->finish_event >= 0) fsm_event(a->fsm, t->finish_event); /* :25,:31 Fsm.Event(finishEvent) */
    if (t->action) act_finish(t->action);                                 /* :26,:32 itweenFSMAction.Finish() */
}

/* type identity for ConflictCheck (mirrors iTween.cs's `type` string).  The sim's `kind` fuses type AND
 * method for move/rotate (kind 1 move/by, 2 move/to, 3 rotate/by, 4 rotate/to -- each already unique), but
 * NOT for scale: ScaleTo and ScaleBy share kind 0 because their runtime (generate/apply) is identical
 * (act_hk.c) -- `method_by` (set only by the ScaleBy launcher) disambiguates them for THIS check only. */
static int itween_type_id(uint8_t kind)
{
    switch (kind) {
    case 0: return 0;              /* scale */
    case 1: case 2: return 1;      /* move */
    case 3: case 4: return 2;      /* rotate */
    case 5: return 3;              /* shake */
    case 6: return 4;              /* color (FadeTo) */
    default: return -1;
    }
}
/* ConflictCheck :4109-4147, over components in creation order (self is skipped: not running yet).  A TYPE
 * mismatch or a not-running tween -> continue (:4119); a METHOD mismatch -> break (:4130).  Every PlayMaker
 * launch carries a fresh itweenID (iTweenFsmAction.cs:63-65), so the argument sets never match and the game
 * always disposes the OTHER tween (:4143); the self-dispose branch (:4145) is unreachable. */
static void itween_conflict_check(fsm_world *w, itween_inst *self)
{
    int self_type = itween_type_id(self->kind);
    for (int32_t i = 0; i < w->n_itweens; i++) {
        itween_inst *o = &w->itweens[i];
        if (o == self || !o->alive || o->go != self->go) continue;
        if (!o->running || itween_type_id(o->kind) != self_type) continue;         /* :4119 !isRunning || type mismatch -> continue */
        bool method_mismatch = (o->kind != self->kind) || (self->kind == 0 && o->method_by != self->method_by);
        if (method_mismatch) break;                                                /* :4130 method mismatch -> break (stop scanning) */
        itween_dispose(o);                                                         /* args always differ (fresh events_id) -> :4143 other.Dispose() */
        return;                                                                    /* :4144 return */
    }
}

/* GenerateScaleToTargets :1520-1556 ("scale" is a Vector3, no "speed"); GenerateMoveByTargets :1440-1478 ("amount"); GenerateMoveToTargets :1433-1480 */
static void itween_generate_targets(fsm_world *w, itween_inst *t)
{
    if (t->kind == 1) {
        float p[3]; go_world_pos(w, t->go, p);
        for (int i = 0; i < 3; i++) { t->from[i] = p[i]; t->last[i] = p[i]; t->to[i] = p[i] + t->amount[i]; }   /* vector3s[0]=[3]=position, [1] = [0] + amount */
        /* :1513-1517 num = Abs(Vector3.Distance(vector3s[0], vector3s[1])); time = num / speed.  Distance is
         * (b - a).magnitude, so this is the rounded float difference, not |amount| -- computed the same way
         * as the move/to branch below. */
        if (t->has_speed) {
            float dx = t->to[0] - t->from[0], dy = t->to[1] - t->from[1], dz = t->to[2] - t->from[2];
            t->time = m_sqrt(dx * dx + dy * dy + dz * dz) / t->speed;
        }
        return;
    }
    if (t->kind == 3) {
        /* GenerateRotateByTargets :2178-2200: vector3s[0] = [1] = [3] = transform.eulerAngles, then
         * [1] += Vector3.Scale(amount, (360,360,360)) -- "amount" is in REVOLUTIONS, not degrees.
         * The pose model is 2D, so only z is carried; the action traps on a non-zero x/y amount. */
        float ez = go_euler_z(w, t->go);
        t->from[0] = 0.0f; t->from[1] = 0.0f; t->from[2] = ez;
        t->last[0] = 0.0f; t->last[1] = 0.0f; t->last[2] = ez;
        t->to[0] = t->amount[0] * 360.0f; t->to[1] = t->amount[1] * 360.0f;
        t->to[2] = ez + t->amount[2] * 360.0f;
        return;
    }
    if (t->kind == 2) {
        if (t->space == 1) go_local_pos(w, t->go, t->from); else go_world_pos(w, t->go, t->from);
        /* move/to tests `speed > 0`, not has_speed; they differ only for an explicit speed == 0 */
        if (t->speed > 0.0f) {
            float dx = t->to[0] - t->from[0], dy = t->to[1] - t->from[1], dz = t->to[2] - t->from[2];
            float dist = m_sqrt(dx * dx + dy * dy + dz * dz);
            t->time = dist / t->speed;
        }
        return;
    }
    if (t->kind == 4) {
        /* GenerateRotateToTargets :1618-1660 (2D model: z axis only, matching the same restriction the
         * kind==3 rotate/by branch above already carries).  from=to=current z, then `to` is overwritten
         * by the absolute target (t->amount[2], resolved at launch from the vector/transform arg) and
         * finally clerp'd toward the shortest angular path exactly as :1656 does (value=1 at generate
         * time; ease() during Apply then re-samples along that already-shortest-path target). */
        float ez = t->space == 1 ? go_local_euler_z(w, t->go) : go_euler_z(w, t->go);
        t->from[0] = 0.0f; t->from[1] = 0.0f; t->from[2] = ez;
        t->last[0] = 0.0f; t->last[1] = 0.0f; t->last[2] = ez;
        t->to[0] = 0.0f; t->to[1] = 0.0f; t->to[2] = clerp(ez, t->amount[2], 1.0f);
        if (t->has_speed) t->time = fabsf(t->to[2] - t->from[2]) / t->speed;
        return;
    }
    if (t->kind == 5) {
        /* GenerateShakePositionTargets :1724-1745: vector3s[0] = transform.position, always WORLD
         * (iTween.cs:1728); `space` only governs the Translate in ApplyShakePositionTargets.
         * vector3s[1] (the "amount" arg) is captured at launch. */
        go_world_pos(w, t->go, t->from);
        return;
    }
    if (t->kind == 6) return;   /* GenerateColorToTargets :1180-1210: no transform state -- see itween_apply kind 6 */
    go_local_scale(w, t->go, t->from);
    if (t->method_by) {
        /* GenerateScaleByTargets :1558-1583: target = Vector3.Scale(localScale, amount), read HERE
         * (TweenStart), not at launch. */
        t->to[0] = t->from[0] * t->amount[0];
        t->to[1] = t->from[1] * t->amount[1];
        t->to[2] = t->from[2] * t->amount[2];
    }
    /* else (ScaleTo): to[] was captured at launch (the Vector3 arg) */
}

/* TweenStart :2331-2348 */
static void itween_tween_start(fsm_world *w, itween_inst *t)
{
    itween_on_start(w, t);                                                /* CallBack("onstart") */
    if (!t->restarted) {                                                  /* :2334 if (!loop) */
        itween_conflict_check(w, t);                                      /* !loop -> ConflictCheck(); GenerateTargets() */
        itween_generate_targets(w, t);
    }
    /* EnableKinematic() :4149 empty */
    t->running = 1;
}

/* ApplyScaleToTargets :2065-2075; ApplyMoveByTargets :1960-1988 (transform.Translate(v2 - v3, space); v3 = v2); ApplyMoveToTargets :2006-2037 */
static void itween_apply(fsm_world *w, itween_inst *t)
{
    float v[3];
    for (int i = 0; i < 3; i++) v[i] = ease_apply(w, t, t->from[i], t->to[i], t->percentage);
    if (t->kind == 1) {
        float p[3]; go_world_pos(w, t->go, p);
        float d[3] = { v[0] - t->last[0], v[1] - t->last[1], v[2] - t->last[2] };
        if (t->space == 1 && go_euler_z(w, t->go) != 0.0f) {
            /* Transform.Translate(v, Space.Self): position += rotation * v (TransformDirection: rotation only, no scale;
             * analysis/upstream/UnityCsReference/Runtime/Transform/ScriptBindings/Transform.bindings.cs:117-123).  The 2D rotation is
             * the world z euler; a y half turn held folded (transform.c SetRotation) is not that rotation. */
            if (w->gos[t->go].fold_y180)
                HKSIM_UNIMPLEMENTED("iTween MoveBy Space.Self on '%s', held folded after a y half turn", go_path(w, t->go));
            float a = go_euler_z(w, t->go) * ((float)M_PI / 180.0f), c = m_cos(a), sn = m_sin(a);
            float dx = c * d[0] - sn * d[1], dy = sn * d[0] + c * d[1];
            d[0] = dx; d[1] = dy;
        }
        float np[3] = { p[0] + d[0], p[1] + d[1], p[2] + d[2] };
        go_set_world_pos(w, t->go, np);
        memcpy(t->last, v, sizeof v);
        return;
    }
    if (t->kind == 3) {
        /* ApplyRotateAddTargets :2100-2114 (method "by" reuses it): transform.Rotate(v - vector3s[3],
         * space), then vector3s[3] = v -- incremental, exactly like ApplyMoveByTargets above. */
        float d = v[2] - t->last[2];
        if (t->space == 1) go_set_local_euler_z(w, t->go, go_local_euler_z(w, t->go) + d);
        else               go_set_euler_z(w, t->go, go_euler_z(w, t->go) + d);
        memcpy(t->last, v, sizeof v);
        return;
    }
    if (t->kind == 2) {
        if (t->space == 1) {
            go_set_local_pos(w, t->go, v);
            if (t->percentage == 1.0f) go_set_local_pos(w, t->go, t->to);
        } else {
            go_set_world_pos(w, t->go, v);
            if (t->percentage == 1.0f) go_set_world_pos(w, t->go, t->to);
        }
        return;
    }
    if (t->kind == 4) {
        /* ApplyRotateToTargets :2092-2120: absolute set every frame (not incremental like rotate/by). */
        if (t->space == 1) go_set_local_euler_z(w, t->go, v[2]); else go_set_euler_z(w, t->go, v[2]);
        if (t->percentage == 1.0f) { if (t->space == 1) go_set_local_euler_z(w, t->go, t->to[2]); else go_set_euler_z(w, t->go, t->to[2]); }
        return;
    }
    if (t->kind == 5) {
        /* ApplyShakePositionTargets :2141-2168: position = vector3s[0] (world, :2154), then
         * Translate(vector3s[2], space) (:2159); Space.Self rotates the offset by the object's OWN
         * rotation only (traps if nonzero).  The percentage==0 Translate at :2150-2152 is a dead store. */
        if (t->space == 1 && go_euler_z(w, t->go) != 0.0f) HKSIM_UNIMPLEMENTED("iTween ShakePosition Space.Self on a rotated object");
        float num = 1.0f - t->percentage;
        float rx = hk_rng_range_f_site(w->rng, t->shake_site, 0.0f - t->amount[0] * num, t->amount[0] * num);
        float ry = hk_rng_range_f_site(w->rng, t->shake_site, 0.0f - t->amount[1] * num, t->amount[1] * num);
        /* amount.z: no z position in the 2D model, so the third Random.Range is not drawn (draw count
         * is not a fidelity target) */
        float np[3] = { t->from[0] + rx, t->from[1] + ry, t->from[2] };
        go_set_world_pos(w, t->go, np);
        return;
    }
    if (t->kind == 6) return;   /* ApplyColorToTargets :1880-1922: renderer color is not modelled */
    go_set_local_scale(w, t->go, v);
    if (t->percentage == 1.0f) go_set_local_scale(w, t->go, t->to);
}

/* TweenUpdate :2361-2366 + UpdatePercentage :4060-4078 (useRealTime never set by the live launchers) */
static void itween_tween_update(fsm_world *w, itween_inst *t, float dt)
{
    itween_apply(w, t);
    /* CallBack("onupdate"): not in the args */
    t->running_time = t->running_time + dt;                               /* runningTime += Time.deltaTime */
    t->percentage = t->reverse ? (float)(1.0 - (double)t->running_time / (double)t->time)   /* :4071 compound */
                               : t->running_time / t->time;               /* :4075 single op */
}

/* TweenLoop :2395-2412 -> TweenRestart :2350-2359.  The coroutine runs synchronously to its first yield,
 * which only exists for delay > 0 (traps), so `loop = true; TweenStart();` runs inside this call.
 * pingPong does NOT reset `percentage`: TweenComplete clamped it and the flipped `reverse` runs it back
 * (Update :3600-3618).  `loop` makes TweenStart skip GenerateTargets, so the endpoints stay fixed. */
static void itween_tween_loop(fsm_world *w, itween_inst *t)
{
    /* DisableKinematic() :4153-4155 empty */
    if (t->loop == 1) {                                                   /* LoopType.loop :2400-2405 */
        t->percentage = 0.0f;
        t->running_time = 0.0f;
        itween_apply(w, t);
    } else if (t->loop == 2) {                                            /* LoopType.pingPong :2406-2410 */
        t->reverse = !t->reverse;
        t->running_time = 0.0f;
    }
    if (t->delay > 0.0f) HKSIM_UNIMPLEMENTED("iTween loop restart with delay > 0 (TweenRestart :2352-2356 yield) not ported");
    t->restarted = 1;                                                     /* :2357 loop = true */
    itween_tween_start(w, t);                                             /* :2358 */
}

/* TweenComplete :2368-2393 */
static void itween_tween_complete(fsm_world *w, itween_inst *t)
{
    t->running = 0;
    t->percentage = t->percentage > 0.5f ? 1.0f : 0.0f;
    itween_apply(w, t);
    if (t->loop == 0) itween_dispose(t);                                  /* LoopType.none -> Dispose() */
    else itween_tween_loop(w, t);                                         /* :2390 TweenLoop() */
    itween_on_complete(w, t);                                             /* CallBack("oncomplete") */
}



/* iTween.ScaleTo(GameObject, Hashtable) :703-716 -> Launch :3747-3759 -> Awake/RetrieveArgs */
int32_t itween_scale_to(fsm_world *w, int32_t go, const float scale[3], float time, float delay, int32_t ease, int32_t loop,
                        bool ignore_timescale, int32_t events_id, act_inst *action, bool donotfinish, bool is_looping,
                        int32_t start_event, int32_t finish_event)
{
    /* iTween.Launch :3747-3752 calls GenerateID() (:3790-3809), whose Random.Range draws only build a
     * string ID that nothing reads.  The ID has no gameplay effect, so it is not modelled (draw count is
     * not a fidelity target; only the distribution of each random decision is). */
    itween_inst *t = itween_new(w);
    t->go = go; t->id = ++w->itween_id_count; t->events_id = events_id; t->action = action;
    t->tick_pending = 0;               /* first tick / Start: the lifecycle's R1 / R2 rules (lc_itween_added below) */
    t->alive = 1; t->started = 0; t->running = 0; t->physics = 0; t->reverse = 0;
    t->is_looping = is_looping; t->donotfinish = donotfinish; t->use_real_time = ignore_timescale;
    t->time = time; t->delay = delay; t->running_time = 0.0f; t->percentage = 0.0f;
    memcpy(t->to, scale, sizeof t->to); memset(t->from, 0, sizeof t->from);
    t->ease = ease; t->loop = loop; t->start_event = start_event; t->finish_event = finish_event;
    if (ignore_timescale) HKSIM_UNIMPLEMENTED("iTween ignoretimescale (realtimeSinceStartup clock) not ported");
    int32_t k = (int32_t)(t - w->itweens);
    /* Launch :3758 AddComponent<iTween>(): Awake (RetrieveArgs, done above) and OnEnable run inside the call;
     * Start (TweenStart) and the first Update / LateUpdate come by the lifecycle's R1 / R2. */
    lc_itween_added(w, t->id, go);
    return k;
}

/* iTween.Stop(GameObject target, string type) :3496-3507 — (type + method) prefix match, case-insensitive */
void itween_stop_type(fsm_world *w, int32_t go, const char *type)
{
    size_t n = strlen(type);
    for (int32_t i = 0; i < w->n_itweens; i++) {
        itween_inst *t = &w->itweens[i];
        if (!t->alive || t->go != go) continue;
        const char *tm = t->kind == 1 ? "moveby" : (t->kind == 2 ? "moveto" : (t->kind == 3 ? "rotateby" :
                         (t->kind == 4 ? "rotateto" : (t->kind == 5 ? "shakeposition" : (t->kind == 6 ? "colorto" : "scaleto")))));   /* type + method */
        if (n <= strlen(tm) && strncmp(tm, type, n) == 0) itween_dispose(t);
    }
}
/* iTween.RotateBy(GameObject, Hashtable) :876-882 -> Launch (type "rotate", method "by").
 * Launch dispatches "rotate"/"by" to GenerateRotateByTargets + ApplyRotateAddTargets (:1058-1072). */
int32_t itween_rotate_by(fsm_world *w, int32_t go, const float amount[3], int32_t space, float time, float delay, int32_t ease, int32_t loop,
                         bool ignore_timescale, int32_t events_id, act_inst *action, bool donotfinish, bool is_looping,
                         int32_t start_event, int32_t finish_event)
{
    float zero[3] = { 0, 0, 0 };
    int32_t k = itween_scale_to(w, go, zero, time, delay, ease, loop, ignore_timescale, events_id, action, donotfinish, is_looping, start_event, finish_event);
    itween_inst *t = &w->itweens[k];
    t->kind = 3; t->space = space; memcpy(t->amount, amount, sizeof t->amount);
    return k;
}
/* iTween.MoveBy(GameObject, Hashtable) :690-696 -> Launch */
int32_t itween_move_by(fsm_world *w, int32_t go, const float amount[3], int32_t space, float time, float delay,
                       float speed, bool has_speed, int32_t ease, int32_t loop,
                       bool ignore_timescale, int32_t events_id, act_inst *action, bool donotfinish, bool is_looping,
                       int32_t start_event, int32_t finish_event)
{
    float zero[3] = { 0, 0, 0 };
    int32_t k = itween_scale_to(w, go, zero, time, delay, ease, loop, ignore_timescale, events_id, action, donotfinish, is_looping, start_event, finish_event);
    itween_inst *t = &w->itweens[k];
    t->kind = 1; t->space = space; memcpy(t->amount, amount, sizeof t->amount);
    t->speed = speed; t->has_speed = has_speed;
    return k;
}
/* iTween.MoveTo(GameObject, Hashtable) :684-690 -> Launch */
int32_t itween_move_to(fsm_world *w, int32_t go, const float pos[3], int32_t space, float time, float delay, float speed,
                       int32_t ease, int32_t loop, bool ignore_timescale, int32_t events_id, act_inst *action,
                       bool donotfinish, bool is_looping, int32_t start_event, int32_t finish_event)
{
    int32_t k = itween_scale_to(w, go, pos, time, delay, ease, loop, ignore_timescale, events_id, action, donotfinish, is_looping, start_event, finish_event);
    itween_inst *t = &w->itweens[k];
    t->kind = 2; t->space = space; t->speed = speed;
    return k;
}

/* iTween.RotateTo(GameObject, Hashtable) :792-882 -> Launch (type "rotate", method "to"). `rot` is the
 * ABSOLUTE target angle (already resolved from the vector or transformRotation arg at the call site,
 * act_hk.c), carried in `amount` the same way itween_rotate_by borrows it for its (different) purpose. */
int32_t itween_rotate_to(fsm_world *w, int32_t go, const float rot[3], int32_t space, float time, float delay, float speed,
                         int32_t ease, int32_t loop, bool ignore_timescale, int32_t events_id, act_inst *action,
                         bool donotfinish, bool is_looping, int32_t start_event, int32_t finish_event)
{
    float zero[3] = { 0, 0, 0 };
    int32_t k = itween_scale_to(w, go, zero, time, delay, ease, loop, ignore_timescale, events_id, action, donotfinish, is_looping, start_event, finish_event);
    itween_inst *t = &w->itweens[k];
    t->kind = 4; t->space = space; memcpy(t->amount, rot, sizeof t->amount);
    t->speed = speed; t->has_speed = speed > 0.0f;
    return k;
}
/* iTween.ShakePosition(GameObject, Hashtable) :884-895 -> Launch (type "shake", method "position").
 * `amount` is the shake amplitude vector.  `action->rng_site` (the launching FSM action's site, stable per
 * owner/fsm/state/action-index -- sim/core/rng.h) seeds the per-frame Random.Range draws in
 * ApplyShakePositionTargets; captured once here since `action` can go NULL later (itween_sever_events). */
int32_t itween_shake_position(fsm_world *w, int32_t go, const float amount[3], int32_t space, float time, float delay,
                              int32_t loop, bool ignore_timescale, int32_t events_id, act_inst *action, bool donotfinish,
                              bool is_looping, int32_t start_event, int32_t finish_event)
{
    float zero[3] = { 0, 0, 0 };
    int32_t k = itween_scale_to(w, go, zero, time, delay, 21 /* linear: ShakePosition has no easeType arg */, loop,
                                ignore_timescale, events_id, action, donotfinish, is_looping, start_event, finish_event);
    itween_inst *t = &w->itweens[k];
    t->kind = 5; t->space = space; memcpy(t->amount, amount, sizeof t->amount);
    t->shake_site = action ? action->rng_site : 0;
    return k;
}
/* iTween.FadeTo(GameObject, Hashtable) -> ColorTo (type "color", method "to", :392-431).  No renderer/
 * material/color state exists anywhere in the sim (see itween_apply kind==6), so this models only the
 * tween's timing, events and Stop/ConflictCheck semantics -- everything an FSM waiting on its
 * start/finishEvent can observe. */
int32_t itween_fade_to(fsm_world *w, int32_t go, float time, float delay, int32_t ease, int32_t loop,
                       bool ignore_timescale, int32_t events_id, act_inst *action, bool donotfinish, bool is_looping,
                       int32_t start_event, int32_t finish_event)
{
    float zero[3] = { 0, 0, 0 };
    int32_t k = itween_scale_to(w, go, zero, time, delay, ease, loop, ignore_timescale, events_id, action, donotfinish, is_looping, start_event, finish_event);
    w->itweens[k].kind = 6;
    return k;
}

/* Object.Destroy(itweenEvents) (iTweenFsmAction.cs:83-86): later callbacks do not reach the action */
void itween_sever_events(fsm_world *w, int32_t events_id)
{
    for (int32_t i = 0; i < w->n_itweens; i++)
        if (w->itweens[i].events_id == events_id) w->itweens[i].action = NULL;
}

/* ---- lifecycle entry points (sim/fsm/lifecycle.c dispatches every iTween component) ---- */
itween_inst *itween_by_uid(fsm_world *w, int32_t uid)
{
    for (int32_t i = 0; i < w->n_itweens; i++) if (w->itweens[i].id == uid) return w->itweens[i].alive ? &w->itweens[i] : NULL;
    return NULL;
}
/* iTween.Start :3585-3592 -> TweenDelay :2320-2329.  Start begins with `yield return new
 * WaitForSeconds(delay)`, so delay 0 reaches TweenStart synchronously in the Start stage (R1); delay > 0 is
 * counted down in itween_run_update.  isRunning stays false meanwhile, so Update early-returns as the game's. */
void itween_run_start(fsm_world *w, itween_inst *t)
{
    if (t->started) return;
    t->started = 1;
    if (t->delay > 0.0f) return;                 /* TweenStart waits; itween_run_update counts the delay down */
    itween_tween_start(w, t);
}
/* iTween.Update :3594-3619 */
void itween_run_update(fsm_world *w, itween_inst *t, float dt)
{
    if (!t->alive) return;
    if (t->started && !t->running && t->delay > 0.0f && t->delay_elapsed < t->delay) {
        t->delay_elapsed += dt;                  /* TweenDelay :2320-2329 WaitForSeconds(delay) */
        if (t->delay_elapsed < t->delay) return;
        itween_tween_start(w, t);                /* :3590, once the wait is over */
    }
    if (!t->running || t->physics) return;
    if (!t->reverse) {
        if (t->percentage < 1.0f) itween_tween_update(w, t, dt); else itween_tween_complete(w, t);
    } else {
        if (t->percentage > 0.0f) itween_tween_update(w, t, dt); else itween_tween_complete(w, t);
    }
}
/* iTween.FixedUpdate :3621-3646: only tweens on a 3D Rigidbody (physics), none in any dump */
void itween_run_fixed_update(fsm_world *w, itween_inst *t)
{
    (void)w;
    if (!t->alive || !t->running || !t->physics) return;
    HKSIM_UNIMPLEMENTED("iTween FixedUpdate on a physics tween (3D Rigidbody): none in the dumps");
}
/* Disposed tweens (Dispose -> Destroy(this)) leave the registry and the array at the end of the frame */
void itween_compact(fsm_world *w)
{
    for (int32_t i = 0; i < w->n_itweens; i++) if (!w->itweens[i].alive) lc_itween_removed(w, w->itweens[i].id);
    int32_t k = 0;
    for (int32_t i = 0; i < w->n_itweens; i++) if (w->itweens[i].alive) w->itweens[k++] = w->itweens[i];
    w->n_itweens = k;
}
