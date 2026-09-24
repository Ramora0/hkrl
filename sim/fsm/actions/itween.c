/* iTween actions: ACT/iTween<Name>.cs on iTweenFsmAction.cs:41-95.  The tween runtime is sim/fsm/itween.c.
 * OnEnter launches a tween bridged back to the action by events_id; OnExit severs the bridge and, unless
 * stopOnExit is false, stops that tween type on the object.  A null owner launches nothing and the action
 * never finishes.  Unported modes (speed, axis, orientToPath, lookAt, x/y rotation) trap. */
#include "act.h"

/* the fields of iTweenFsmAction and the launch inputs every action here reads the same way */
typedef struct { const fsm_pv *gameObject, *time, *delay, *easeType, *loopType, *startEvent, *finishEvent, *realTime, *stopOnExit, *loopDontFinish; int32_t events_id; } it_base;
typedef struct { int32_t go, loop, ease, ev_start, ev_finish; float time, delay; bool is_looping, donotfinish, ignore_ts; } it_launch;

static void it_bind(act_inst *a, it_base *b)
{
    b->gameObject = FIELD(gameObject); b->time = FIELD(time); b->delay = FIELD(delay); b->loopType = FIELD(loopType);
    b->easeType = FIELD_OPT(easeType); b->startEvent = FIELD_OPT(startEvent); b->finishEvent = FIELD_OPT(finishEvent);
    b->realTime = FIELD_OPT(realTime); b->stopOnExit = FIELD_OPT(stopOnExit); b->loopDontFinish = FIELD_OPT(loopDontFinish);
    b->events_id = -1;
}
/* OnEnteriTween (:41-52: AddComponent<iTweenFSMEvents>, itweenIDCount++) and the common DoiTween inputs */
static bool it_begin(act_inst *a, it_base *b, it_launch *L)
{
    fsm_inst *f = a->fsm; fsm_world *w = f->w;
    L->go = p_owner_default(a, b->gameObject);
    if (L->go < 0) return false;
    b->events_id = ++w->itween_events_id_count;
    L->donotfinish = b->loopDontFinish && !p_isnone(b->loopDontFinish) && pb(f, b->loopDontFinish);
    L->loop = pi(f, b->loopType);                                   /* iTween.LoopType: none=0, loop=1, pingPong=2 */
    L->is_looping = L->loop != 0;                                   /* :63-66 IsLoop(true) */
    L->ease = b->easeType ? pi(f, b->easeType) : 0;
    L->time = p_isnone(b->time) ? 1.0f : pf(f, b->time);            /* Reset: time = 1f */
    L->delay = p_isnone(b->delay) ? 0.0f : pf(f, b->delay);
    L->ignore_ts = b->realTime && !p_isnone(b->realTime) && pb(f, b->realTime);
    L->ev_start = EV(b->startEvent); L->ev_finish = EV(b->finishEvent);
    return true;
}
static void it_end(act_inst *a, it_base *b, const char *type)
{
    fsm_inst *f = a->fsm; fsm_world *w = f->w;
    int32_t go = p_owner_default(a, b->gameObject);
    if (go < 0) return;
    if (b->events_id >= 0) { itween_sever_events(w, b->events_id); b->events_id = -1; }   /* Object.Destroy(itweenEvents) :83-86 */
    if (!b->stopOnExit || p_isnone(b->stopOnExit) || pb(f, b->stopOnExit)) itween_stop_type(w, go, type);   /* :87-94 iTween.Stop(go, itweenType) */
}

/* iTweenMoveTo — ACT/iTweenMoveTo.cs:58-254 */
typedef struct { it_base b; const fsm_pv *transformPosition, *vectorPosition, *speed, *space; } st_itmoveto;
static void itmoveto_bind(act_inst *a) { ST(st_itmoveto); it_bind(a, &s->b); s->transformPosition = FIELD(transformPosition); s->vectorPosition = FIELD(vectorPosition); s->speed = FIELD(speed); s->space = FIELD(space); }
static void itmoveto_enter(act_inst *a)
{
    ST(st_itmoveto);
    it_launch L;
    if (!it_begin(a, &s->b, &L)) return;
    float vec[3] = { 0.0f, 0.0f, 0.0f };
    if (!p_isnone(s->vectorPosition)) { const float *v = pv3(f, s->vectorPosition); vec[0] = v[0]; vec[1] = v[1]; vec[2] = v[2]; }
    int32_t sp = pi(f, s->space);
    if (!p_isnone(s->transformPosition) && pgo(f, s->transformPosition) >= 0) {
        /* :175-177: World (or a parentless owner) adds the target's world position; Self adds
         * owner.transform.parent.InverseTransformPoint(target.position). */
        float tp_world[3]; go_world_pos(w, pgo(f, s->transformPosition), tp_world);
        float tp_pos[3];
        if (sp == 0 || w->gos[L.go].parent < 0) { tp_pos[0] = tp_world[0]; tp_pos[1] = tp_world[1]; tp_pos[2] = tp_world[2]; }
        else go_world_to_local_point(w, w->gos[L.go].parent, tp_world, tp_pos);
        vec[0] += tp_pos[0]; vec[1] += tp_pos[1]; vec[2] += tp_pos[2];
    }
    float speed = p_isnone(s->speed) ? 0.0f : pf(f, s->speed);
    itween_move_to(w, L.go, vec, sp, L.time, L.delay, speed, L.ease, L.loop, L.ignore_ts, s->b.events_id, a,
                   L.donotfinish, L.is_looping, L.ev_start, L.ev_finish);
}
static void itmoveto_exit(act_inst *a) { ST(st_itmoveto); it_end(a, &s->b, "move"); }
static const act_vtable AV_iTweenMoveTo = { "iTweenMoveTo", sizeof(st_itmoveto), itmoveto_bind, itmoveto_enter, NULL, NULL, NULL, itmoveto_exit, NULL, NULL };

/* iTweenMoveBy — ACT/iTweenMoveBy.cs:58-110.  :108 adds EITHER "time" or "speed"; with speed,
 * GenerateMoveByTargets (:1513-1517) overwrites time with distance / speed. */
typedef struct { it_base b; const fsm_pv *vector, *speed, *space, *orientToPath, *lookAtObject, *lookAtVector, *axis; } st_itmove;
static void itmove_bind(act_inst *a)
{
    ST(st_itmove);
    it_bind(a, &s->b);
    s->vector = FIELD(vector); s->speed = FIELD(speed); s->space = FIELD(space); s->orientToPath = FIELD(orientToPath);
    s->lookAtObject = FIELD(lookAtObject); s->lookAtVector = FIELD(lookAtVector); s->axis = FIELD(axis);
}
static void itmove_enter(act_inst *a)
{
    ST(st_itmove);
    it_launch L;
    if (!it_begin(a, &s->b, &L)) return;
    if (!p_isnone(s->orientToPath) && pb(f, s->orientToPath)) HKSIM_UNIMPLEMENTED("iTweenMoveBy orientToPath in %s", fsm_label(f));
    if ((!p_isnone(s->lookAtObject) && pgo(f, s->lookAtObject) >= 0) || !p_isnone(s->lookAtVector)) HKSIM_UNIMPLEMENTED("iTweenMoveBy lookAt in %s", fsm_label(f));
    if (pi(f, s->axis) != 0) HKSIM_UNIMPLEMENTED("iTweenMoveBy axis restriction %d in %s", pi(f, s->axis), fsm_label(f));
    float amount[3] = { 0, 0, 0 };
    if (!p_isnone(s->vector)) { const float *v = pv3(f, s->vector); amount[0] = v[0]; amount[1] = v[1]; amount[2] = v[2]; }
    bool has_speed = !p_isnone(s->speed);
    float speed = has_speed ? pf(f, s->speed) : 0.0f;
    itween_move_by(w, L.go, amount, pi(f, s->space), L.time, L.delay, speed, has_speed, L.ease, L.loop, L.ignore_ts,
                   s->b.events_id, a, L.donotfinish, L.is_looping, L.ev_start, L.ev_finish);
}
static void itmove_exit(act_inst *a) { ST(st_itmove); it_end(a, &s->b, "move"); }
static const act_vtable AV_iTweenMoveBy = { "iTweenMoveBy", sizeof(st_itmove), itmove_bind, itmove_enter, NULL, NULL, NULL, itmove_exit, NULL, NULL };

/* iTweenScaleTo — ACT/iTweenScaleTo.cs:58-88: target = transformScale.localScale + vectorScale */
typedef struct { it_base b; const fsm_pv *transformScale, *vectorScale, *speed; } st_itscale;
static void itscale_bind(act_inst *a) { ST(st_itscale); it_bind(a, &s->b); s->transformScale = FIELD(transformScale); s->vectorScale = FIELD(vectorScale); s->speed = FIELD(speed); }
static void itscale_enter(act_inst *a)
{
    ST(st_itscale);
    it_launch L;
    if (!it_begin(a, &s->b, &L)) return;
    float vec[3] = { 0.0f, 0.0f, 0.0f };
    if (!p_isnone(s->vectorScale)) { const float *v = pv3(f, s->vectorScale); vec[0] = v[0]; vec[1] = v[1]; vec[2] = v[2]; }
    if (!p_isnone(s->transformScale) && pgo(f, s->transformScale) >= 0) {
        float ls[3]; go_local_scale(w, pgo(f, s->transformScale), ls);   /* Vector3 operator+: per-component single ops */
        vec[0] = ls[0] + vec[0]; vec[1] = ls[1] + vec[1]; vec[2] = ls[2] + vec[2];
    }
    if (!p_isnone(s->speed)) HKSIM_UNIMPLEMENTED("iTweenScaleTo speed mode (time = distance / speed :1551-1555) in %s", fsm_label(f));
    itween_scale_to(w, L.go, vec, L.time, L.delay, L.ease, L.loop, L.ignore_ts, s->b.events_id, a, L.donotfinish, L.is_looping,
                    L.ev_start, L.ev_finish);
}
static void itscale_exit(act_inst *a) { ST(st_itscale); it_end(a, &s->b, "scale"); }
static const act_vtable AV_iTweenScaleTo = { "iTweenScaleTo", sizeof(st_itscale), itscale_bind, itscale_enter, NULL, NULL, NULL, itscale_exit, NULL, NULL };

/* iTweenScaleBy — ACT/iTweenScaleBy.cs:47-70: ScaleTo's runtime with method "by" (iTween.cs:779-785).
 * GenerateScaleByTargets (:1558-1583) multiplies localScale by `amount` at TweenStart, not at OnEnter, so only
 * the multiplier is captured here and itween_generate_targets resolves the target then. */
typedef struct { it_base b; const fsm_pv *vector, *speed; } st_itscaleby;
static void itscaleby_bind(act_inst *a) { ST(st_itscaleby); it_bind(a, &s->b); s->vector = FIELD(vector); s->speed = FIELD(speed); }
static void itscaleby_enter(act_inst *a)
{
    ST(st_itscaleby);
    it_launch L;
    if (!it_begin(a, &s->b, &L)) return;
    float amount[3] = { 1.0f, 1.0f, 1.0f };
    if (!p_isnone(s->vector)) { const float *v = pv3(f, s->vector); amount[0] = v[0]; amount[1] = v[1]; amount[2] = v[2]; }
    if (!p_isnone(s->speed)) HKSIM_UNIMPLEMENTED("iTweenScaleBy speed mode (time = distance / speed :1580-1584) in %s", fsm_label(f));
    int32_t k = itween_scale_to(w, L.go, amount, L.time, L.delay, L.ease, L.loop, L.ignore_ts, s->b.events_id, a, L.donotfinish, L.is_looping,
                                L.ev_start, L.ev_finish);
    itween_inst *t = &w->itweens[k];
    t->method_by = 1;
    memcpy(t->amount, amount, sizeof amount);
}
static void itscaleby_exit(act_inst *a) { ST(st_itscaleby); it_end(a, &s->b, "scale"); }
static const act_vtable AV_iTweenScaleBy = { "iTweenScaleBy", sizeof(st_itscaleby), itscaleby_bind, itscaleby_enter, NULL, NULL, NULL, itscaleby_exit, NULL, NULL };

/* iTweenRotateBy - ACT/iTweenRotateBy.cs:35-60: spins by `vector` REVOLUTIONS over `time`.  The pose model is
 * 2D, so only z. */
typedef struct { it_base b; const fsm_pv *vector, *speed, *space; } st_itrot;
static void itrot_bind(act_inst *a) { ST(st_itrot); it_bind(a, &s->b); s->vector = FIELD(vector); s->speed = FIELD(speed); s->space = FIELD(space); }
static void itrot_enter(act_inst *a)
{
    ST(st_itrot);
    it_launch L;
    if (!it_begin(a, &s->b, &L)) return;
    if (!p_isnone(s->speed)) HKSIM_UNIMPLEMENTED("iTweenRotateBy speed mode in %s", fsm_label(f));
    float amount[3] = { 0, 0, 0 };
    if (!p_isnone(s->vector)) { const float *v = pv3(f, s->vector); amount[0] = v[0]; amount[1] = v[1]; amount[2] = v[2]; }
    if (amount[0] != 0.0f || amount[1] != 0.0f)
        HKSIM_UNKNOWN("iTweenRotateBy with a non-zero x/y amount (%g, %g) in %s: the pose model is 2D "
                      "(z euler only)", (double)amount[0], (double)amount[1], fsm_label(f));
    itween_rotate_by(w, L.go, amount, pi(f, s->space), L.time, L.delay, L.ease, L.loop, L.ignore_ts,
                     s->b.events_id, a, L.donotfinish, L.is_looping, L.ev_start, L.ev_finish);
}
static void itrot_exit(act_inst *a) { ST(st_itrot); it_end(a, &s->b, "rotate"); }
static const act_vtable AV_iTweenRotateBy = { "iTweenRotateBy", sizeof(st_itrot), itrot_bind, itrot_enter, NULL, NULL, NULL, itrot_exit, NULL, NULL };

/* iTweenShakePosition - ACT/iTweenShakePosition.cs:9-64 */
typedef struct { it_base b; const fsm_pv *vector, *space, *axis; } st_itshake;
static void itshake_bind(act_inst *a) { ST(st_itshake); it_bind(a, &s->b); s->vector = FIELD(vector); s->space = FIELD(space); s->axis = FIELD(axis); }
static void itshake_enter(act_inst *a)
{
    ST(st_itshake);
    it_launch L;
    if (!it_begin(a, &s->b, &L)) return;
    if (!p_isnone(s->axis) && pi(f, s->axis) != 0)
        HKSIM_UNIMPLEMENTED("iTweenShakePosition axis restriction %d in %s", pi(f, s->axis), fsm_label(f));
    float amount[3] = { 0, 0, 0 };
    if (!p_isnone(s->vector)) { const float *v = pv3(f, s->vector); amount[0] = v[0]; amount[1] = v[1]; amount[2] = v[2]; }
    itween_shake_position(w, L.go, amount, pi(f, s->space), L.time, L.delay, L.loop, L.ignore_ts,
                          s->b.events_id, a, L.donotfinish, L.is_looping, L.ev_start, L.ev_finish);
}
static void itshake_exit(act_inst *a) { ST(st_itshake); it_end(a, &s->b, "shake"); }
static const act_vtable AV_iTweenShakePosition = { "iTweenShakePosition", sizeof(st_itshake), itshake_bind, itshake_enter, NULL, NULL, NULL, itshake_exit, NULL, NULL };

/* iTweenFadeTo - ACT/iTweenFadeTo.cs:9-72.  No renderer colour is modelled (itween.c applies nothing for this
 * kind); the tween's timing, events and Stop semantics are.  DoiTween sets itweenType "fade" but launches
 * iTween.ColorTo, whose type+method is "colorto", so the game's Stop-on-exit never matches it: the "fade"
 * stop below is a faithful no-op. */
typedef struct { it_base b; } st_itfade;
static void itfade_bind(act_inst *a) { ST(st_itfade); it_bind(a, &s->b); }
static void itfade_enter(act_inst *a)
{
    ST(st_itfade);
    it_launch L;
    if (!it_begin(a, &s->b, &L)) return;
    itween_fade_to(w, L.go, L.time, L.delay, L.ease, L.loop, L.ignore_ts, s->b.events_id, a, L.donotfinish, L.is_looping,
                   L.ev_start, L.ev_finish);
}
static void itfade_exit(act_inst *a) { ST(st_itfade); it_end(a, &s->b, "fade"); }
static const act_vtable AV_iTweenFadeTo = { "iTweenFadeTo", sizeof(st_itfade), itfade_bind, itfade_enter, NULL, NULL, NULL, itfade_exit, NULL, NULL };

/* iTweenRotateTo — ACT/iTweenRotateTo.cs:58-121: resolves an ABSOLUTE target euler angle (vectorRotation, plus
 * transformRotation's eulerAngles/localEulerAngles under `space`) and hands it to itween_rotate_to
 * (runtime/itween.c kind 4), which already carries the 2D (z-only) restriction and the clerp shortest-path
 * pick iTween.cs:1656 applies at launch. */
typedef struct { it_base b; const fsm_pv *transformRotation, *vectorRotation, *speed, *space; } st_itrotto;
static void itrotto_bind(act_inst *a) { ST(st_itrotto); it_bind(a, &s->b); s->transformRotation = FIELD(transformRotation); s->vectorRotation = FIELD(vectorRotation); s->speed = FIELD(speed); s->space = FIELD(space); }
static void itrotto_enter(act_inst *a)
{
    ST(st_itrotto);
    it_launch L;
    if (!it_begin(a, &s->b, &L)) return;
    float vec[3] = { 0, 0, 0 };
    if (!p_isnone(s->vectorRotation)) { const float *v = pv3(f, s->vectorRotation); vec[0] = v[0]; vec[1] = v[1]; vec[2] = v[2]; }
    int32_t tr = pgo(f, s->transformRotation);
    int32_t sp = pi(f, s->space);
    if (!p_isnone(s->transformRotation) && tr >= 0)
        vec[2] += (sp == 1 ? go_local_euler_z(w, tr) : go_euler_z(w, tr));   /* :113-115 eulerAngles/localEulerAngles + vector */
    if (vec[0] != 0.0f || vec[1] != 0.0f)
        HKSIM_UNKNOWN("iTweenRotateTo with a non-zero x/y target (%g, %g) in %s: the pose model is 2D (z euler only)", (double)vec[0], (double)vec[1], fsm_label(f));
    float speed = p_isnone(s->speed) ? 0.0f : pf(f, s->speed);
    itween_rotate_to(w, L.go, vec, sp, L.time, L.delay, speed, L.ease, L.loop, L.ignore_ts, s->b.events_id, a,
                     L.donotfinish, L.is_looping, L.ev_start, L.ev_finish);
}
static void itrotto_exit(act_inst *a) { ST(st_itrotto); it_end(a, &s->b, "rotate"); }
static const act_vtable AV_iTweenRotateTo = { "iTweenRotateTo", sizeof(st_itrotto), itrotto_bind, itrotto_enter, NULL, NULL, NULL, itrotto_exit, NULL, NULL };

const act_vtable *const act_registry_itween[] = {
    &AV_iTweenMoveTo, &AV_iTweenMoveBy, &AV_iTweenScaleTo, &AV_iTweenScaleBy, &AV_iTweenRotateBy,
    &AV_iTweenShakePosition, &AV_iTweenFadeTo, &AV_iTweenRotateTo,
};
const int act_registry_itween_n = (int)(sizeof act_registry_itween / sizeof act_registry_itween[0]);
