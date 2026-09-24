/* Variable arithmetic (fsm-actions.md §2): set/add/multiply/convert, vectors, random values and choices,
 * easing. */
#include "act.h"

static float mclamp(float v, float lo, float hi) { if (v < lo) v = lo; else if (v > hi) v = hi; return v; }   /* Mathf.Clamp */

#define SETVAL(NAME, TYPE, VAR, VAL, GET, SET) \
typedef struct { const fsm_pv *v, *x, *everyFrame; } st_##NAME; \
static void NAME##_bind(act_inst *a) { ST(st_##NAME); s->v = FIELD(VAR); s->x = FIELD(VAL); s->everyFrame = FIELD(everyFrame); } \
static void NAME##_do(act_inst *a) { ST(st_##NAME); SET(f, s->v, GET(f, s->x)); } \
static void NAME##_enter(act_inst *a) { ST(st_##NAME); NAME##_do(a); if (!pb(f, s->everyFrame)) act_finish(a); } \
static const act_vtable AV_##TYPE = { #TYPE, sizeof(st_##NAME), NAME##_bind, NAME##_enter, NAME##_do, NULL, NULL, NULL, NULL, NULL };
SETVAL(setfloat, SetFloatValue, floatVariable, floatValue, pf, pf_set)      /* ACT/SetFloatValue.cs:23-35 */
SETVAL(setbool, SetBoolValue, boolVariable, boolValue, pb, pb_set)          /* ACT/SetBoolValue.cs */
SETVAL(setint, SetIntValue, intVariable, intValue, pi, pi_set)              /* ACT/SetIntValue.cs */
SETVAL(setgo, SetGameObject, variable, gameObject, pgo, pgo_set)            /* ACT/SetGameObject.cs:22-34 */
SETVAL(setstring, SetStringValue, stringVariable, stringValue, ps, ps_set)  /* ACT/SetStringValue.cs (same shape as SetFloatValue) */

/* FloatAdd / FloatSubtract — ACT/FloatAdd.cs:32-56, FloatSubtract.cs:32-56 */
typedef struct { const fsm_pv *v, *x, *everyFrame, *perSecond; } st_fadd;
static void fadd_bind(act_inst *a) { ST(st_fadd); s->v = FIELD(floatVariable); s->x = FIELD(add); s->everyFrame = FIELD(everyFrame); s->perSecond = FIELD(perSecond); }
static void fadd_do(act_inst *a) { ST(st_fadd); if (pb(f, s->perSecond)) pf_set(f, s->v, (float)((double)pf(f, s->v) + (double)pf(f, s->x) * (double)w->dt)); else pf_set(f, s->v, pf(f, s->v) + pf(f, s->x)); }   /* :54 compound (double stack) / :50 */
static void fadd_enter(act_inst *a) { ST(st_fadd); fadd_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_FloatAdd = { "FloatAdd", sizeof(st_fadd), fadd_bind, fadd_enter, fadd_do, NULL, NULL, NULL, NULL, NULL };
static void fsub_bind(act_inst *a) { ST(st_fadd); s->v = FIELD(floatVariable); s->x = FIELD(subtract); s->everyFrame = FIELD(everyFrame); s->perSecond = FIELD(perSecond); }
static void fsub_do(act_inst *a) { ST(st_fadd); if (pb(f, s->perSecond)) pf_set(f, s->v, (float)((double)pf(f, s->v) - (double)pf(f, s->x) * (double)w->dt)); else pf_set(f, s->v, pf(f, s->v) - pf(f, s->x)); }   /* :54 compound (double stack) / :50 */
static void fsub_enter(act_inst *a) { ST(st_fadd); fsub_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_FloatSubtract = { "FloatSubtract", sizeof(st_fadd), fsub_bind, fsub_enter, fsub_do, NULL, NULL, NULL, NULL, NULL };

/* FloatMultiply — ACT/FloatMultiply.cs:26-38 */
typedef struct { const fsm_pv *v, *x, *everyFrame; } st_fmul;
static void fmul_bind(act_inst *a) { ST(st_fmul); s->v = FIELD(floatVariable); s->x = FIELD(multiplyBy); s->everyFrame = FIELD(everyFrame); }
static void fmul_do(act_inst *a) { ST(st_fmul); pf_set(f, s->v, pf(f, s->v) * pf(f, s->x)); }
static void fmul_enter(act_inst *a) { ST(st_fmul); fmul_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_FloatMultiply = { "FloatMultiply", sizeof(st_fmul), fmul_bind, fmul_enter, fmul_do, NULL, NULL, NULL, NULL, NULL };

/* FloatClamp — ACT/FloatClamp.cs:33-50 */
typedef struct { const fsm_pv *v, *lo, *hi, *everyFrame; } st_fclamp;
static void fclamp_bind(act_inst *a) { ST(st_fclamp); s->v = FIELD(floatVariable); s->lo = FIELD(minValue); s->hi = FIELD(maxValue); s->everyFrame = FIELD(everyFrame); }
static void fclamp_do(act_inst *a) { ST(st_fclamp); pf_set(f, s->v, mclamp(pf(f, s->v), pf(f, s->lo), pf(f, s->hi))); }
static void fclamp_enter(act_inst *a) { ST(st_fclamp); fclamp_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_FloatClamp = { "FloatClamp", sizeof(st_fclamp), fclamp_bind, fclamp_enter, fclamp_do, NULL, NULL, NULL, NULL, NULL };

/* FloatOperator — ACT/FloatOperator.cs:47-86 (enum Add,Subtract,Multiply,Divide,Min,Max) */
typedef struct { const fsm_pv *f1, *f2, *op, *store, *everyFrame; } st_fop;
static void fop_bind(act_inst *a) { ST(st_fop); s->f1 = FIELD(float1); s->f2 = FIELD(float2); s->op = FIELD(operation); s->store = FIELD(storeResult); s->everyFrame = FIELD(everyFrame); }
static void fop_do(act_inst *a)
{
    ST(st_fop);
    float v1 = pf(f, s->f1), v2 = pf(f, s->f2), r = 0.0f;
    switch (s->op->i) {
    case 0: r = v1 + v2; break; case 1: r = v1 - v2; break; case 2: r = v1 * v2; break; case 3: r = v1 / v2; break;
    case 4: r = v1 < v2 ? v1 : v2; break; case 5: r = v1 > v2 ? v1 : v2; break;
    default: HKSIM_UNKNOWN("FloatOperator.operation %d", s->op->i);
    }
    pf_set(f, s->store, r);
}
static void fop_enter(act_inst *a) { ST(st_fop); fop_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_FloatOperator = { "FloatOperator", sizeof(st_fop), fop_bind, fop_enter, fop_do, NULL, NULL, NULL, NULL, NULL };

/* SetFloatToHighest — ACT/SetFloatToHighest.cs:27-53 */
typedef struct { const fsm_pv *var, *v1, *v2, *everyFrame; } st_sfth;
static void sfth_bind(act_inst *a) { ST(st_sfth); s->var = FIELD(floatVariable); s->v1 = FIELD(value1); s->v2 = FIELD(value2); s->everyFrame = FIELD(everyFrame); }
static void sfth_do(act_inst *a)
{
    ST(st_sfth);
    float a1 = pf(f, s->v1);
    float a2 = pf(f, s->v2);
    pf_set(f, s->var, a1 > a2 ? a1 : a2);
}
static void sfth_enter(act_inst *a) { ST(st_sfth); sfth_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_SetFloatToHighest = { "SetFloatToHighest", sizeof(st_sfth), sfth_bind, sfth_enter, sfth_do, NULL, NULL, NULL, NULL, NULL };

/* IntAdd — ACT/IntAdd.cs:23-35; IntAddV2 — ACT/IntAddV2.cs:28-40 (FixedUpdate) */
typedef struct { const fsm_pv *v, *x, *everyFrame; } st_iadd;
static void iadd_bind(act_inst *a) { ST(st_iadd); s->v = FIELD(intVariable); s->x = FIELD(add); s->everyFrame = FIELD(everyFrame); }
static void iadd_do(act_inst *a) { ST(st_iadd); pi_set(f, s->v, pi(f, s->v) + pi(f, s->x)); }
static void iadd_enter(act_inst *a) { ST(st_iadd); iadd_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_IntAdd = { "IntAdd", sizeof(st_iadd), iadd_bind, iadd_enter, iadd_do, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_IntAddV2 = { "IntAddV2", sizeof(st_iadd), iadd_bind, iadd_enter, NULL, iadd_do, NULL, NULL, NULL, NULL };

/* IntOperator — ACT/IntOperator.cs:42-81 (integer divide, unguarded) */
typedef struct { const fsm_pv *i1, *i2, *op, *store, *everyFrame; } st_iop;
static void iop_bind(act_inst *a) { ST(st_iop); s->i1 = FIELD(integer1); s->i2 = FIELD(integer2); s->op = FIELD(operation); s->store = FIELD(storeResult); s->everyFrame = FIELD(everyFrame); }
static void iop_do(act_inst *a)
{
    ST(st_iop);
    int32_t v1 = pi(f, s->i1), v2 = pi(f, s->i2), r = 0;
    switch (s->op->i) {
    case 0: r = v1 + v2; break; case 1: r = v1 - v2; break; case 2: r = v1 * v2; break;
    case 3: if (v2 == 0) HKSIM_UNKNOWN("IntOperator divide by zero (DivideByZeroException in C#)"); r = v1 / v2; break;
    case 4: r = v1 < v2 ? v1 : v2; break; case 5: r = v1 > v2 ? v1 : v2; break;
    default: HKSIM_UNKNOWN("IntOperator.operation %d", s->op->i);
    }
    pi_set(f, s->store, r);
}
static void iop_enter(act_inst *a) { ST(st_iop); iop_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_IntOperator = { "IntOperator", sizeof(st_iop), iop_bind, iop_enter, iop_do, NULL, NULL, NULL, NULL, NULL };

/* Vector3AddXYZ — ACT/Vector3AddXYZ.cs:33-58 */
typedef struct { const fsm_pv *v, *x, *y, *z, *everyFrame, *perSecond; } st_v3add;
static void v3add_bind(act_inst *a) { ST(st_v3add); s->v = FIELD(vector3Variable); s->x = FIELD(addX); s->y = FIELD(addY); s->z = FIELD(addZ); s->everyFrame = FIELD(everyFrame); s->perSecond = FIELD(perSecond); }
static void v3add_do(act_inst *a)
{
    ST(st_v3add);
    float add[3] = { pf(f, s->x), pf(f, s->y), pf(f, s->z) };
    float cur[4]; memcpy(cur, pv3(f, s->v), sizeof cur);
    if (pb(f, s->perSecond)) {                                     /* :52 `Value += vector * Time.deltaTime`: Vector3 operator* then operator+, per-component single ops */
        float sc[3] = { add[0] * w->dt, add[1] * w->dt, add[2] * w->dt };
        cur[0] += sc[0]; cur[1] += sc[1]; cur[2] += sc[2];
    }
    else { cur[0] += add[0]; cur[1] += add[1]; cur[2] += add[2]; }
    pv3_set(f, s->v, cur);
}
static void v3add_enter(act_inst *a) { ST(st_v3add); v3add_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_Vector3AddXYZ = { "Vector3AddXYZ", sizeof(st_v3add), v3add_bind, v3add_enter, v3add_do, NULL, NULL, NULL, NULL, NULL };

/* RandomFloat — ACT/RandomFloat.cs:24-30: ONE draw */
typedef struct { const fsm_pv *min, *max, *store; } st_rf;
static void rf_bind(act_inst *a) { ST(st_rf); s->min = FIELD(min); s->max = FIELD(max); s->store = FIELD(storeResult); }
static void rf_enter(act_inst *a) { ST(st_rf); pf_set(f, s->store, hk_rng_range_f_site(w->rng, a->rng_site, pf(f, s->min), pf(f, s->max))); act_finish(a); }
static const act_vtable AV_RandomFloat = { "RandomFloat", sizeof(st_rf), rf_bind, rf_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* RandomFloatEither — analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/RandomFloatEither.cs:22-33 */
typedef struct { const fsm_pv *value1, *value2, *storeResult; } st_rfe;
static void rfe_bind(act_inst *a) { ST(st_rfe); s->value1 = FIELD(value1); s->value2 = FIELD(value2); s->storeResult = FIELD(storeResult); }
static void rfe_enter(act_inst *a) {
    ST(st_rfe);
    int32_t roll = hk_rng_range_i_site(w->rng, a->rng_site, 0, 100);
    if (roll < 50) {
        pf_set(f, s->storeResult, pf(f, s->value1));
    } else {
        pf_set(f, s->storeResult, pf(f, s->value2));
    }
    act_finish(a);
}
static const act_vtable AV_RandomFloatEither = { "RandomFloatEither", sizeof(st_rfe), rfe_bind, rfe_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* SetColorValue — analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/SetColorValue.cs:22-44 */
typedef struct { const fsm_pv *colorVariable, *color, *everyFrame; } st_scv;
static void scv_bind(act_inst *a) { ST(st_scv); s->colorVariable = FIELD(colorVariable); s->color = FIELD(color); s->everyFrame = FIELD_OPT(everyFrame); }
static void scv_do(act_inst *a) {
    ST(st_scv);
    if (!p_isnone(s->colorVariable)) {
        pv3_set(f, s->colorVariable, pv3(f, s->color));
    }
}
static void scv_enter(act_inst *a) { ST(st_scv); scv_do(a); if (!s->everyFrame || !pb(f, s->everyFrame)) act_finish(a); }
static void scv_update(act_inst *a) { scv_do(a); }
static const act_vtable AV_SetColorValue = { "SetColorValue", sizeof(st_scv), scv_bind, scv_enter, scv_update, NULL, NULL, NULL, NULL, NULL };

/* GetColorRGBA — analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/GetColorRGBA.cs:39-65 */
typedef struct { const fsm_pv *color, *storeRed, *storeGreen, *storeBlue, *storeAlpha, *everyFrame; } st_gcrgba;
static void gcrgba_bind(act_inst *a) {
    ST(st_gcrgba);
    s->color = FIELD(color);
    s->storeRed = FIELD_OPT(storeRed);
    s->storeGreen = FIELD_OPT(storeGreen);
    s->storeBlue = FIELD_OPT(storeBlue);
    s->storeAlpha = FIELD_OPT(storeAlpha);
    s->everyFrame = FIELD_OPT(everyFrame);
}
static void gcrgba_do(act_inst *a) {
    ST(st_gcrgba);
    if (!p_isnone(s->color)) {
        const float *c = pv3(f, s->color);
        if (s->storeRed && !p_isnone(s->storeRed)) pf_set(f, s->storeRed, c[0]);
        if (s->storeGreen && !p_isnone(s->storeGreen)) pf_set(f, s->storeGreen, c[1]);
        if (s->storeBlue && !p_isnone(s->storeBlue)) pf_set(f, s->storeBlue, c[2]);
        if (s->storeAlpha && !p_isnone(s->storeAlpha)) pf_set(f, s->storeAlpha, c[3]);
    }
}
static void gcrgba_enter(act_inst *a) { ST(st_gcrgba); gcrgba_do(a); if (!s->everyFrame || !pb(f, s->everyFrame)) act_finish(a); }
static void gcrgba_update(act_inst *a) { gcrgba_do(a); }
static const act_vtable AV_GetColorRGBA = { "GetColorRGBA", sizeof(st_gcrgba), gcrgba_bind, gcrgba_enter, gcrgba_update, NULL, NULL, NULL, NULL, NULL };

/* FloatDivide — ACT/FloatDivide.cs:26-38 */
typedef struct { const fsm_pv *v, *x, *everyFrame; } st_fdiv;
static void fdiv_bind(act_inst *a) { ST(st_fdiv); s->v = FIELD(floatVariable); s->x = FIELD(divideBy); s->everyFrame = FIELD(everyFrame); }
static void fdiv_do(act_inst *a) { ST(st_fdiv); pf_set(f, s->v, pf(f, s->v) / pf(f, s->x)); }
static void fdiv_enter(act_inst *a) { ST(st_fdiv); fdiv_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_FloatDivide = { "FloatDivide", sizeof(st_fdiv), fdiv_bind, fdiv_enter, fdiv_do, NULL, NULL, NULL, NULL, NULL };

/* RandomBool — ACT/RandomBool.cs:17-22: Random.Range(0, 100) < 50 */
typedef struct { const fsm_pv *storeResult; } st_rbool;
static void rbool_bind(act_inst *a) { ST(st_rbool); s->storeResult = FIELD(storeResult); }
static void rbool_enter(act_inst *a) {
    ST(st_rbool);
    int roll = hk_rng_range_i_site(w->rng, a->rng_site, 0, 100);
    pb_set(f, s->storeResult, roll < 50);
    act_finish(a);
}
static const act_vtable AV_RandomBool = { "RandomBool", sizeof(st_rbool), rbool_bind, rbool_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* EaseFsmAction (ACT/EaseFsmAction.cs), the base of EaseFloat and EaseColor.  The subclass sets from[] / to[] / n
 * after ease_enter and reads result[] after ease_update. */
void ease_bind(act_inst *a, ease_core *e)
{
    e->time = FIELD(time); e->speed = FIELD(speed); e->delay = FIELD(delay); e->easeType = FIELD(easeType);
    e->reverse = FIELD(reverse); e->realTime = FIELD(realTime);
}
void ease_enter(act_inst *a, ease_core *e)                             /* :123-135 */
{
    fsm_inst *f = a->fsm;
    if (!p_isnone(e->realTime) && pb(f, e->realTime))   /* EaseFsmAction.cs:147-150, :206-209 */
        HKSIM_UNKNOWN("%s.realTime (FsmTime.RealtimeSinceStartup) in %s: wall clock", a->vt->type_short, fsm_label(f));
    e->finished = false;
    e->isRunning = false;
    e->runningTime = 0.0f;
    e->percentage = p_isnone(e->reverse) ? 0.0f : (pb(f, e->reverse) ? 1.0f : 0.0f);
    e->finishAction = false;
    e->delayTime = p_isnone(e->delay) ? 0.0f : pf(f, e->delay);
    e->start = true;
}
static void ease_update_percentage(act_inst *a, ease_core *e)          /* :204-235, realTime false */
{
    fsm_inst *f = a->fsm; fsm_world *w = f->w;
    if (!p_isnone(e->speed)) e->runningTime = (float)((double)e->runningTime + (double)w->dt * (double)pf(f, e->speed));
    else                     e->runningTime += w->dt;
    if (!p_isnone(e->reverse) && pb(f, e->reverse))
        e->percentage = (float)(1.0 - (double)e->runningTime / (double)pf(f, e->time));
    else
        e->percentage = e->runningTime / pf(f, e->time);
}
void ease_update(act_inst *a, ease_core *e)                            /* :141-202 */
{
    fsm_inst *f = a->fsm; fsm_world *w = f->w;
    if (e->start && !e->isRunning) {
        if (e->delayTime >= 0.0f) e->delayTime -= w->dt;
        else { e->isRunning = true; e->start = false; }
    }
    if (!e->isRunning || e->finished) return;
    bool rev = !(p_isnone(e->reverse) || !pb(f, e->reverse));
    ease_update_percentage(a, e);
    if (rev ? e->percentage > 0.0f : e->percentage < 1.0f) {
        /* SetEasingFunction (:236-324) has iTween's bodies and enum order (iTween.cs GetEasingFunction) */
        for (int i = 0; i < e->n; i++)
            e->result[i] = itween_ease_value(e->easeType->i, e->from[i], e->to[i], e->percentage, fsm_label(f));
    } else {
        e->finishAction = true;
        e->finished = true;
        e->isRunning = false;
    }
}
/* the value finishAction stores (EaseFloat.cs:67-73, EaseColor.cs:67-74): toValue unless reverse is set and true */
bool ease_final_is_from(act_inst *a, const ease_core *e) { return !p_isnone(e->reverse) && pb(a->fsm, e->reverse); }

/* EaseFloat — ACT/EaseFloat.cs:28-76 */
typedef struct { ease_core e; const fsm_pv *fromValue, *toValue, *floatVariable, *finishEvent; bool finishInNextStep; } st_easef;
static void easef_bind(act_inst *a)
{
    ST(st_easef);
    ease_bind(a, &s->e);
    s->fromValue = FIELD(fromValue); s->toValue = FIELD(toValue); s->floatVariable = FIELD(floatVariable); s->finishEvent = FIELD_OPT(finishEvent);
}
static void easef_enter(act_inst *a)                                  /* :28-39 */
{
    ST(st_easef);
    ease_enter(a, &s->e);
    s->e.n = 1;
    s->e.from[0] = pf(f, s->fromValue);
    s->e.to[0] = pf(f, s->toValue);
    s->finishInNextStep = false;
    pf_set(f, s->floatVariable, pf(f, s->fromValue));
}
static void easef_update(act_inst *a)                                 /* :46-76 */
{
    ST(st_easef);
    ease_update(a, &s->e);
    if (!p_isnone(s->floatVariable) && s->e.isRunning) pf_set(f, s->floatVariable, s->e.result[0]);
    if (s->finishInNextStep) {
        act_finish(a);
        if (EV(s->finishEvent) >= 0) fsm_event(f, EV(s->finishEvent));
    }
    if (s->e.finishAction && !s->finishInNextStep) {
        if (!p_isnone(s->floatVariable)) pf_set(f, s->floatVariable, pf(f, ease_final_is_from(a, &s->e) ? s->fromValue : s->toValue));
        s->finishInNextStep = true;
    }
}
static const act_vtable AV_EaseFloat = { "EaseFloat", sizeof(st_easef), easef_bind, easef_enter, easef_update, NULL, NULL, NULL, NULL, NULL };

/* SetVector2XY — ACT/SetVector2XY.cs:26-55 */
typedef struct { const fsm_pv *var, *val, *x, *y, *everyFrame; } st_sv2;
static void sv2_bind(act_inst *a) { ST(st_sv2); s->var = FIELD(vector2Variable); s->val = FIELD(vector2Value); s->x = FIELD(x); s->y = FIELD(y); s->everyFrame = a_field(a, "everyFrame"); }
static void sv2_do(act_inst *a)
{
    ST(st_sv2);
    if (p_isnone(s->var) && s->var->kind != PV_FV2) return;
    float v[3] = { 0.0f, 0.0f, 0.0f };
    const float *cur = pv3(f, s->var); v[0] = cur[0]; v[1] = cur[1];
    if (!p_isnone(s->val)) { const float *o = pv3(f, s->val); v[0] = o[0]; v[1] = o[1]; }
    if (!p_isnone(s->x)) v[0] = pf(f, s->x);
    if (!p_isnone(s->y)) v[1] = pf(f, s->y);
    pv3_set(f, s->var, v);
}
static void sv2_enter(act_inst *a) { ST(st_sv2); sv2_do(a); if (!s->everyFrame || !pb(f, s->everyFrame)) act_finish(a); }
static void sv2_update(act_inst *a) { sv2_do(a); }
static const act_vtable AV_SetVector2XY = { "SetVector2XY", sizeof(st_sv2), sv2_bind, sv2_enter, sv2_update, NULL, NULL, NULL, NULL, NULL };

/* FloatMultiplyV2 - ACT/FloatMultiplyV2.cs:24-56.  FloatMultiply plus a `fixedUpdate` flag that moves
 * the repeat from OnUpdate to OnFixedUpdate; OnEnter multiplies once either way. */
typedef struct { const fsm_pv *floatVariable, *multiplyBy, *everyFrame, *fixedUpdate; } st_fmul2;
static void fmul2_bind(act_inst *a) { ST(st_fmul2); s->floatVariable = FIELD(floatVariable); s->multiplyBy = FIELD(multiplyBy); s->everyFrame = a_field(a, "everyFrame"); s->fixedUpdate = a_field(a, "fixedUpdate"); }
static void fmul2_do(act_inst *a) { ST(st_fmul2); pf_set(f, s->floatVariable, pf(f, s->floatVariable) * pf(f, s->multiplyBy)); }
static void fmul2_enter(act_inst *a) { ST(st_fmul2); fmul2_do(a); if (!s->everyFrame || !pb(f, s->everyFrame)) act_finish(a); }
static void fmul2_update(act_inst *a) { ST(st_fmul2); if (!s->fixedUpdate || !pb(f, s->fixedUpdate)) fmul2_do(a); }
static void fmul2_fixed(act_inst *a) { ST(st_fmul2); if (s->fixedUpdate && pb(f, s->fixedUpdate)) fmul2_do(a); }
static const act_vtable AV_FloatMultiplyV2 = { "FloatMultiplyV2", sizeof(st_fmul2), fmul2_bind, fmul2_enter, fmul2_update, fmul2_fixed, NULL, NULL, NULL, NULL };

/* RandomInt - ACT/RandomInt.cs:24-30.  ONE draw; inclusiveMax picks Range(min, max+1) over Range(min, max). */
typedef struct { const fsm_pv *min, *max, *storeResult, *inclusiveMax; } st_rndi;
static void rndi_bind(act_inst *a) { ST(st_rndi); s->min = FIELD(min); s->max = FIELD(max); s->storeResult = FIELD(storeResult); s->inclusiveMax = a_field(a, "inclusiveMax"); }
static void rndi_enter(act_inst *a)
{
    ST(st_rndi);
    int32_t hi = pi(f, s->max) + ((s->inclusiveMax && pb(f, s->inclusiveMax)) ? 1 : 0);
    pi_set(f, s->storeResult, hk_rng_range_i_site(w->rng, a->rng_site, pi(f, s->min), hi));
    act_finish(a);
}
static const act_vtable AV_RandomInt = { "RandomInt", sizeof(st_rndi), rndi_bind, rndi_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* DistanceBetweenPoints — ACT/DistanceBetweenPoints.cs:30-52.
 * `ignoreZ` takes the Vector2 overload, i.e. the z components are dropped rather than zeroed. */
typedef struct { const fsm_pv *distanceResult, *point1, *point2, *ignoreZ, *everyFrame; } st_dbp;
static void dbp_bind(act_inst *a)
{
    ST(st_dbp);
    s->distanceResult = FIELD(distanceResult); s->point1 = FIELD(point1); s->point2 = FIELD(point2);
    s->ignoreZ = FIELD(ignoreZ); s->everyFrame = FIELD(everyFrame);
}
static void dbp_do(act_inst *a)
{
    ST(st_dbp);
    if (!s->distanceResult) return;                                   /* :37 `if (distanceResult != null)` */
    const float *p1 = pv3(f, s->point1), *p2 = pv3(f, s->point2);
    /* Vector2.Distance / Vector3.Distance (UnityCsReference Vector2.cs:215-220, Vector3.cs:331-337): float
     * differences, the sum of squares on the Mono double stack, one rounding after Math.Sqrt */
    float dx = p1[0] - p2[0], dy = p1[1] - p2[1];
    double d2 = (double)dx * dx + (double)dy * dy;
    if (!pb(f, s->ignoreZ)) { float dz = p1[2] - p2[2]; d2 += (double)dz * dz; }   /* :46-48 Vector3.Distance */
    pf_set(f, s->distanceResult, (float)sqrt(d2));
}
static void dbp_enter(act_inst *a) { ST(st_dbp); dbp_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }  /* :20-26 */
static const act_vtable AV_DistanceBetweenPoints = { "DistanceBetweenPoints", sizeof(st_dbp), dbp_bind, dbp_enter, dbp_do, NULL, NULL, NULL, NULL, NULL };

/* GetAngleBetweenPoints — ACT/GetAngleBetweenPoints.cs:35-49.
 * atan2(p1.y - p2.y, p1.x - p2.x) in degrees, then wrapped up into [0, 360) by a `while (< 0) += 360`
 * loop (:44-46).  NOTE the decomp's OnUpdate also calls Finish() when !everyFrame (:31-34), unlike
 * every sibling action -- kept, because the state's action set is what decides FINISHED. */
typedef struct { const fsm_pv *point1, *point2, *storeAngle, *everyFrame; } st_gabp;
static void gabp_bind(act_inst *a)
{
    ST(st_gabp);
    s->point1 = FIELD(point1); s->point2 = FIELD(point2); s->storeAngle = FIELD(storeAngle);
    s->everyFrame = FIELD(everyFrame);
}
static void gabp_do(act_inst *a)
{
    ST(st_gabp);
    const float *p1 = pv3(f, s->point1), *p2 = pv3(f, s->point2);
    float num = p1[1] - p2[1], num2 = p1[0] - p2[0];
    /* :41 Mathf.Atan2 returns a float; `180f / (float)Math.PI` is a float constant (57.29578f); the product is
     * on the Mono double stack and rounds once at the float local (docs/float-parity.md) */
    float deg = (float)((double)m_atan2(num, num2) * (double)(180.0f / (float)M_PI));
    while (deg < 0.0f) deg += 360.0f;                                          /* :41-43 */
    pf_set(f, s->storeAngle, deg);
}
static void gabp_enter(act_inst *a) { ST(st_gabp); gabp_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static void gabp_update(act_inst *a) { ST(st_gabp); gabp_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GetAngleBetweenPoints = { "GetAngleBetweenPoints", sizeof(st_gabp), gabp_bind, gabp_enter, gabp_update, NULL, NULL, NULL, NULL, NULL };

/* Vector3Lerp — ACT/Vector3Lerp.cs:30-47.  Vector3.Lerp CLAMPS t to [0,1] (unlike LerpUnclamped). */
typedef struct { const fsm_pv *fromVector, *toVector, *amount, *storeResult, *everyFrame; } st_v3l;
static void v3l_bind(act_inst *a)
{
    ST(st_v3l);
    s->fromVector = FIELD(fromVector); s->toVector = FIELD(toVector); s->amount = FIELD(amount);
    s->storeResult = FIELD(storeResult); s->everyFrame = FIELD(everyFrame);
}
static void v3l_do(act_inst *a)
{
    ST(st_v3l);
    const float *fv = pv3(f, s->fromVector), *tv = pv3(f, s->toVector);
    float t = pf(f, s->amount);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);                      /* Vector3.Lerp clamps t */
    float out[3];
    /* Vector3.Lerp (UnityCsReference Vector3.cs:38-46): each component on the Mono double stack, one rounding */
    for (int i = 0; i < 3; i++) out[i] = (float)((double)fv[i] + ((double)tv[i] - (double)fv[i]) * (double)t);
    pv3_set(f, s->storeResult, out);
}
static void v3l_enter(act_inst *a) { ST(st_v3l); v3l_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_Vector3Lerp = { "Vector3Lerp", sizeof(st_v3l), v3l_bind, v3l_enter, v3l_do, NULL, NULL, NULL, NULL, NULL };

/* IntClamp — ACT/IntClamp.cs:29-42.  Mathf.Clamp(value, min, max). */
typedef struct { const fsm_pv *intVariable, *minValue, *maxValue, *everyFrame; } st_iclamp;
static void iclamp_bind(act_inst *a)
{
    ST(st_iclamp);
    s->intVariable = FIELD(intVariable); s->minValue = FIELD(minValue); s->maxValue = FIELD(maxValue);
    s->everyFrame = FIELD(everyFrame);
}
static void iclamp_do(act_inst *a)
{
    ST(st_iclamp);
    if (!s->intVariable) return;
    int32_t v = pi(f, s->intVariable), lo = pi(f, s->minValue), hi = pi(f, s->maxValue);
    if (v < lo) v = lo;                                                /* Mathf.Clamp(int,int,int) */
    if (v > hi) v = hi;
    pi_set(f, s->intVariable, v);
}
static void iclamp_enter(act_inst *a) { ST(st_iclamp); iclamp_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_IntClamp = { "IntClamp", sizeof(st_iclamp), iclamp_bind, iclamp_enter, iclamp_do, NULL, NULL, NULL, NULL, NULL };

/* BoolFlip — ACT/BoolFlip.cs:19-23 */
typedef struct { const fsm_pv *v; } st_bflip;
static void bflip_bind(act_inst *a) { ST(st_bflip); s->v = FIELD(boolVariable); }
static void bflip_enter(act_inst *a) { ST(st_bflip); pb_set(f, s->v, !pb(f, s->v)); act_finish(a); }
static const act_vtable AV_BoolFlip = { "BoolFlip", sizeof(st_bflip), bflip_bind, bflip_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* FloatAddV2 — ACT/FloatAddV2.cs:44-85 (fixedUpdate variant ticks in OnFixedUpdate; activeBool gate) */
typedef struct { const fsm_pv *v, *add, *everyFrame, *perSecond, *fixedUpdate, *activeBool; } st_faddv2;
static void faddv2_bind(act_inst *a) { ST(st_faddv2); s->v = FIELD(floatVariable); s->add = FIELD(add); s->everyFrame = FIELD(everyFrame); s->perSecond = FIELD(perSecond); s->fixedUpdate = FIELD(fixedUpdate); s->activeBool = FIELD(activeBool); }
static void faddv2_do(act_inst *a)
{
    ST(st_faddv2);
    if (p_isnone(s->activeBool) || pb(f, s->activeBool)) {
        if (!pb(f, s->perSecond)) pf_set(f, s->v, pf(f, s->v) + pf(f, s->add));
        else pf_set(f, s->v, (float)((double)pf(f, s->v) + (double)pf(f, s->add) * (double)w->dt));   /* compound -> double stack */
    }
}
static void faddv2_enter(act_inst *a) { ST(st_faddv2); faddv2_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static void faddv2_update(act_inst *a) { ST(st_faddv2); if (!pb(f, s->fixedUpdate)) faddv2_do(a); }
static void faddv2_fixed(act_inst *a) { ST(st_faddv2); if (pb(f, s->fixedUpdate)) faddv2_do(a); }
static const act_vtable AV_FloatAddV2 = { "FloatAddV2", sizeof(st_faddv2), faddv2_bind, faddv2_enter, faddv2_update, faddv2_fixed, NULL, NULL, NULL, NULL };

/* SetVector3XYZ — ACT/SetVector3XYZ.cs:33-62; SetVector3Value — ACT/SetVector3Value.cs:20-32 */
typedef struct { const fsm_pv *v, *value, *x, *y, *z, *everyFrame; } st_sv3;
static void sv3xyz_bind(act_inst *a) { ST(st_sv3); s->v = FIELD(vector3Variable); s->value = FIELD(vector3Value); s->x = FIELD(x); s->y = FIELD(y); s->z = FIELD(z); s->everyFrame = FIELD(everyFrame); }
static void sv3xyz_do(act_inst *a)
{
    ST(st_sv3);
    const float *cur = pv3(f, s->v); float v[3] = { cur[0], cur[1], cur[2] };
    if (!p_isnone(s->value)) { const float *q = pv3(f, s->value); v[0] = q[0]; v[1] = q[1]; v[2] = q[2]; }
    if (!p_isnone(s->x)) v[0] = pf(f, s->x);
    if (!p_isnone(s->y)) v[1] = pf(f, s->y);
    if (!p_isnone(s->z)) v[2] = pf(f, s->z);
    pv3_set(f, s->v, v);
}
static void sv3xyz_enter(act_inst *a) { ST(st_sv3); sv3xyz_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_SetVector3XYZ = { "SetVector3XYZ", sizeof(st_sv3), sv3xyz_bind, sv3xyz_enter, sv3xyz_do, NULL, NULL, NULL, NULL, NULL };

/* GetVector3XYZ - ACT/GetVector3XYZ.cs:31-62.  Each store is written only when that field holds a variable
 * (:49, :53, :57), and the whole body is skipped when vector3Variable is unset (:47). */
typedef struct { const fsm_pv *v, *x, *y, *z, *everyFrame; } st_gv3;
static void gv3xyz_bind(act_inst *a) { ST(st_gv3); s->v = FIELD(vector3Variable); s->x = FIELD(storeX); s->y = FIELD(storeY); s->z = FIELD(storeZ); s->everyFrame = FIELD(everyFrame); }
static void gv3xyz_do(act_inst *a)
{
    ST(st_gv3);
    if (p_isnone(s->v)) return;
    const float *q = pv3(f, s->v);
    if (!p_isnone(s->x)) pf_set(f, s->x, q[0]);
    if (!p_isnone(s->y)) pf_set(f, s->y, q[1]);
    if (!p_isnone(s->z)) pf_set(f, s->z, q[2]);
}
static void gv3xyz_enter(act_inst *a) { ST(st_gv3); gv3xyz_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GetVector3XYZ = { "GetVector3XYZ", sizeof(st_gv3), gv3xyz_bind, gv3xyz_enter, gv3xyz_do, NULL, NULL, NULL, NULL, NULL };
static void sv3v_bind(act_inst *a) { ST(st_sv3); s->v = FIELD(vector3Variable); s->value = FIELD(vector3Value); s->everyFrame = FIELD(everyFrame); }
static void sv3v_do(act_inst *a) { ST(st_sv3); const float *q = pv3(f, s->value); float v[3] = { q[0], q[1], q[2] }; pv3_set(f, s->v, v); }
static void sv3v_enter(act_inst *a) { ST(st_sv3); sv3v_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_SetVector3Value = { "SetVector3Value", sizeof(st_sv3), sv3v_bind, sv3v_enter, sv3v_do, NULL, NULL, NULL, NULL, NULL };

/* BuildString — ACT/BuildString.cs:30-59 */
typedef struct { const fsm_pv *parts, *separator, *addToEnd, *storeResult, *everyFrame; } st_bstr;
static void bstr_bind(act_inst *a) { ST(st_bstr); s->parts = FIELD(stringParts); s->separator = FIELD(separator); s->addToEnd = FIELD(addToEnd); s->storeResult = FIELD_OPT(storeResult); s->everyFrame = FIELD(everyFrame); }
static void bstr_do(act_inst *a)
{
    ST(st_bstr);
    if (!s->storeResult) return;
    char buf[2048]; size_t n = 0; buf[0] = 0;
    int32_t np = a_array_len(a, s->parts);
    const char *sep = w_str(w, ps(f, s->separator));
    for (int32_t i = 0; i < np - 1; i++) {
        const char *p = w_str(w, ps(f, a_array_elem(a, s->parts, i)));
        for (; *p && n < sizeof buf - 2; p++) {                      /* .Replace("\\n", "\n") */
            if (p[0] == '\\' && p[1] == 'n') { buf[n++] = '\n'; p++; } else buf[n++] = *p;
        }
        n += (size_t)snprintf(buf + n, sizeof buf - n, "%s", sep);
    }
    if (np > 0) n += (size_t)snprintf(buf + n, sizeof buf - n, "%s", w_str(w, ps(f, a_array_elem(a, s->parts, np - 1))));
    if (pb(f, s->addToEnd)) n += (size_t)snprintf(buf + n, sizeof buf - n, "%s", sep);
    if (n >= sizeof buf - 1) HKSIM_UNKNOWN("BuildString result exceeds %u bytes", (unsigned)sizeof buf);
    ps_set(f, s->storeResult, w_intern(w, buf));
}
static void bstr_enter(act_inst *a) { ST(st_bstr); bstr_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_BuildString = { "BuildString", sizeof(st_bstr), bstr_bind, bstr_enter, bstr_do, NULL, NULL, NULL, NULL, NULL };

/* ConvertIntToFloat — ACT/ConvertIntToFloat.cs:26-45 */
typedef struct { const fsm_pv *iv, *fv, *everyFrame; } st_i2f;
static void i2f_bind(act_inst *a) { ST(st_i2f); s->iv = FIELD(intVariable); s->fv = FIELD(floatVariable); s->everyFrame = a_field(a, "everyFrame"); }
static void i2f_do(act_inst *a) { ST(st_i2f); pf_set(f, s->fv, (float)pi(f, s->iv)); }
static void i2f_enter(act_inst *a) { ST(st_i2f); i2f_do(a); if (!s->everyFrame || !pb(f, s->everyFrame)) act_finish(a); }
static void i2f_update(act_inst *a) { i2f_do(a); }
static const act_vtable AV_ConvertIntToFloat = { "ConvertIntToFloat", sizeof(st_i2f), i2f_bind, i2f_enter, i2f_update, NULL, NULL, NULL, NULL, NULL };

/* ConvertFloatToInt — ACT/ConvertFloatToInt.cs:33-66.  rounding: 0 RoundDown, 1 RoundUp, 2 Nearest
 * (enum declaration order).  Mathf.RoundToInt is Math.Round(f) -> banker's rounding, half to EVEN. */
typedef struct { const fsm_pv *fv, *iv, *rounding, *everyFrame; } st_f2i;
static void f2i_bind(act_inst *a) { ST(st_f2i); s->fv = FIELD(floatVariable); s->iv = FIELD(intVariable); s->rounding = a_field(a, "rounding"); s->everyFrame = a_field(a, "everyFrame"); }
static void f2i_do(act_inst *a)
{
    ST(st_f2i);
    float v = pf(f, s->fv);
    int32_t mode = s->rounding ? pi(f, s->rounding) : 2;
    int32_t out;
    if (mode == 0) out = (int32_t)floorf(v);
    else if (mode == 1) out = (int32_t)ceilf(v);
    else { double r = nearbyint((double)v); out = (int32_t)r; }   /* nearbyint honours the default FE_TONEAREST = half to even */
    pi_set(f, s->iv, out);
}
static void f2i_enter(act_inst *a) { ST(st_f2i); f2i_do(a); if (!s->everyFrame || !pb(f, s->everyFrame)) act_finish(a); }
static void f2i_update(act_inst *a) { f2i_do(a); }
static const act_vtable AV_ConvertFloatToInt = { "ConvertFloatToInt", sizeof(st_f2i), f2i_bind, f2i_enter, f2i_update, NULL, NULL, NULL, NULL, NULL };

/* RandomlyFlipFloat — ACT/RandomlyFlipFloat.cs:16-23: `if ((double)Random.value >= 0.5) storeResult.Value *= -1f;
 * Finish();`.  storeResult is dereferenced without a null check, hence FIELD. */
typedef struct { const fsm_pv *storeResult; } st_rff;
static void rff_bind(act_inst *a) { ST(st_rff); s->storeResult = FIELD(storeResult); }
static void rff_enter(act_inst *a)
{
    ST(st_rff);
    if (hk_rng_value_site(w->rng, a->rng_site) >= 0.5f) pf_set(f, s->storeResult, pf(f, s->storeResult) * -1.0f);
    act_finish(a);
}
static const act_vtable AV_RandomlyFlipFloat = { "RandomlyFlipFloat", sizeof(st_rff), rff_bind, rff_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* FloatDivideV2 - ACT/FloatDivideV2.cs:30-52: floatVariable.Value /= divideBy.Value on OnEnter, then (while
 * everyFrame) from exactly one of OnUpdate / OnFixedUpdate, chosen by `fixedUpdate`. */
typedef struct { const fsm_pv *v, *by, *everyFrame, *fixedUpdate; } st_fdv2;
static void fdv2_bind(act_inst *a) { ST(st_fdv2); s->v = FIELD(floatVariable); s->by = FIELD(divideBy); s->everyFrame = FIELD_OPT(everyFrame); s->fixedUpdate = FIELD_OPT(fixedUpdate); }
static void fdv2_do(act_inst *a) { ST(st_fdv2); pf_set(f, s->v, pf(f, s->v) / pf(f, s->by)); }
static void fdv2_enter(act_inst *a) { ST(st_fdv2); fdv2_do(a); if (!s->everyFrame || !pb(f, s->everyFrame)) act_finish(a); }
static void fdv2_update(act_inst *a) { ST(st_fdv2); if (!s->fixedUpdate || !pb(f, s->fixedUpdate)) fdv2_do(a); }
static void fdv2_fixed_update(act_inst *a) { ST(st_fdv2); if (s->fixedUpdate && pb(f, s->fixedUpdate)) fdv2_do(a); }
static const act_vtable AV_FloatDivideV2 = { "FloatDivideV2", sizeof(st_fdv2), fdv2_bind, fdv2_enter, fdv2_update, fdv2_fixed_update, NULL, NULL, NULL, NULL };

/* SelectRandomVector3 / SelectRandomString - ACT/SelectRandomVector3.cs:30-40 and SelectRandomString.cs (same
 * shape): idx = ActionHelpers.GetRandomWeightedIndex(weights) (random_weighted_index), and on idx != -1 copy
 * array[idx] into the store. */
typedef struct { const fsm_pv *arr, *weights, *store; } st_srv3;
static void srv3_bind(act_inst *a) { ST(st_srv3); s->arr = FIELD(vector3Array); s->weights = FIELD(weights); s->store = FIELD(storeVector3); }
static void srv3_enter(act_inst *a)
{
    ST(st_srv3);
    if (a_array_len(a, s->arr) > 0 && s->store) {
        int32_t i = random_weighted_index(a, s->weights);
        if (i >= 0) pv3_set(f, s->store, pv3(f, a_array_elem(a, s->arr, i)));
    }
    act_finish(a);
}
static const act_vtable AV_SelectRandomVector3 = { "SelectRandomVector3", sizeof(st_srv3), srv3_bind, srv3_enter, NULL, NULL, NULL, NULL, NULL, NULL };

typedef struct { const fsm_pv *arr, *weights, *store; } st_srs2;
static void srs2_bind(act_inst *a) { ST(st_srs2); s->arr = FIELD(strings); s->weights = FIELD(weights); s->store = FIELD(storeString); }
static void srs2_enter(act_inst *a)
{
    ST(st_srs2);
    if (a_array_len(a, s->arr) > 0 && s->store) {
        int32_t i = random_weighted_index(a, s->weights);
        if (i >= 0) ps_set(f, s->store, ps(f, a_array_elem(a, s->arr, i)));
    }
    act_finish(a);
}
static const act_vtable AV_SelectRandomString = { "SelectRandomString", sizeof(st_srs2), srs2_bind, srs2_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* BoolFlipEveryFrame - ACT/BoolFlipEveryFrame.cs:34-37. */
typedef struct { const fsm_pv *boolVariable, *everyFrame; } st_bfef;
static void bfef_bind(act_inst *a) { ST(st_bfef); s->boolVariable = FIELD(boolVariable); s->everyFrame = FIELD(everyFrame); }
static void bfef_do(act_inst *a) { ST(st_bfef); if (!p_isnone(s->boolVariable)) pb_set(f, s->boolVariable, !pb(f, s->boolVariable)); }
static void bfef_enter(act_inst *a) { ST(st_bfef); bfef_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_BoolFlipEveryFrame = { "BoolFlipEveryFrame", sizeof(st_bfef), bfef_bind, bfef_enter, bfef_do, NULL, NULL, NULL, NULL, NULL };

/* GetVector2XY - ACT/GetVector2XY.cs:41-54. */
typedef struct { const fsm_pv *vector2Variable, *storeX, *storeY, *everyFrame; } st_gv2xy;
static void gv2xy_bind(act_inst *a) { ST(st_gv2xy); s->vector2Variable = FIELD(vector2Variable); s->storeX = FIELD(storeX); s->storeY = FIELD(storeY); s->everyFrame = FIELD(everyFrame); }
static void gv2xy_do(act_inst *a) {
    ST(st_gv2xy);
    if (!p_isnone(s->vector2Variable)) {
        const float *v = pv3(f, s->vector2Variable);
        if (s->storeX && !p_isnone(s->storeX)) pf_set(f, s->storeX, v[0]);
        if (s->storeY && !p_isnone(s->storeY)) pf_set(f, s->storeY, v[1]);
    }
}
static void gv2xy_enter(act_inst *a) { ST(st_gv2xy); gv2xy_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GetVector2XY = { "GetVector2XY", sizeof(st_gv2xy), gv2xy_bind, gv2xy_enter, gv2xy_do, NULL, NULL, NULL, NULL, NULL };

/* SetFloatValueV2 - ACT/SetFloatValueV2.cs:26-44. */
typedef struct { const fsm_pv *floatVariable, *floatValue, *everyFrame, *activeBool; } st_sfvv2;
static void sfvv2_bind(act_inst *a) { ST(st_sfvv2); s->floatVariable = FIELD(floatVariable); s->floatValue = FIELD(floatValue); s->everyFrame = FIELD(everyFrame); s->activeBool = FIELD(activeBool); }
static void sfvv2_do(act_inst *a) { ST(st_sfvv2); if (p_isnone(s->activeBool) || pb(f, s->activeBool)) pf_set(f, s->floatVariable, pf(f, s->floatValue)); }
static void sfvv2_enter(act_inst *a) { ST(st_sfvv2); sfvv2_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_SetFloatValueV2 = { "SetFloatValueV2", sizeof(st_sfvv2), sfvv2_bind, sfvv2_enter, sfvv2_do, NULL, NULL, NULL, NULL, NULL };

/* ConvertIntToString - ACT/ConvertIntToString.cs:45-55. Format string parses basic 0000 style. */
typedef struct { const fsm_pv *intVariable, *stringVariable, *format, *everyFrame; } st_cits;
static void cits_bind(act_inst *a) { ST(st_cits); s->intVariable = FIELD(intVariable); s->stringVariable = FIELD(stringVariable); s->format = FIELD(format); s->everyFrame = FIELD(everyFrame); }
static void cits_do(act_inst *a) {
    ST(st_cits);
    char buf[32];
    const char *fmt = p_isnone(s->format) ? "" : w_str(w, ps(f, s->format));
    if (fmt[0] == '0') {
        int count = strlen(fmt);
        snprintf(buf, sizeof(buf), "%0*d", count, pi(f, s->intVariable));
    } else {
        snprintf(buf, sizeof(buf), "%d", pi(f, s->intVariable));
    }
    ps_set(f, s->stringVariable, w_intern(w, buf));
}
static void cits_enter(act_inst *a) { ST(st_cits); cits_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_ConvertIntToString = { "ConvertIntToString", sizeof(st_cits), cits_bind, cits_enter, cits_do, NULL, NULL, NULL, NULL, NULL };

/* SetFloatToSmallest - ACT/SetFloatToSmallest.cs:27-53: floatVariable = value1 < value2 ? value1 : value2 */
typedef struct { const fsm_pv *value1, *value2, *floatVariable, *everyFrame; } st_sfts;
static void sfts_bind(act_inst *a) { ST(st_sfts); s->value1 = FIELD(value1); s->value2 = FIELD(value2); s->floatVariable = FIELD(floatVariable); s->everyFrame = FIELD(everyFrame); }
static void sfts_do(act_inst *a) {
    ST(st_sfts);
    float f1 = pf(f, s->value1), f2 = pf(f, s->value2);
    pf_set(f, s->floatVariable, f1 < f2 ? f1 : f2);
}
static void sfts_enter(act_inst *a) { ST(st_sfts); sfts_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_SetFloatToSmallest = { "SetFloatToSmallest", sizeof(st_sfts), sfts_bind, sfts_enter, sfts_do, NULL, NULL, NULL, NULL, NULL };

/* DistanceBetweenPoints2D — ACT/DistanceBetweenPoints2D.cs:26-51: Vector2.Distance (UnityCsReference
 * Vector2.cs:215-220), the z components dropped rather than zeroed (unlike DistanceBetweenPoints' ignoreZ
 * option above, this action never reads z at all). */
typedef struct { const fsm_pv *distanceResult, *point1, *point2, *everyFrame; } st_dbp2;
static void dbp2_bind(act_inst *a) { ST(st_dbp2); s->distanceResult = FIELD_OPT(distanceResult); s->point1 = FIELD(point1); s->point2 = FIELD(point2); s->everyFrame = FIELD(everyFrame); }
static void dbp2_do(act_inst *a)
{
    ST(st_dbp2);
    if (!s->distanceResult) return;
    const float *p1 = pv3(f, s->point1), *p2 = pv3(f, s->point2);
    float dx = p1[0] - p2[0], dy = p1[1] - p2[1];
    double d2 = (double)dx * dx + (double)dy * dy;
    pf_set(f, s->distanceResult, (float)sqrt(d2));
}
static void dbp2_enter(act_inst *a) { ST(st_dbp2); dbp2_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_DistanceBetweenPoints2D = { "DistanceBetweenPoints2D", sizeof(st_dbp2), dbp2_bind, dbp2_enter, dbp2_do, NULL, NULL, NULL, NULL, NULL };

/* ReflectAngle — ACT/ReflectAngle.cs:24-49.  storeResult is dereferenced unconditionally (no RequiredField, no
 * null check in the C# either -- a crash trap this port keeps as a hard requirement instead: FIELD, not
 * FIELD_OPT). */
typedef struct { const fsm_pv *angle, *reflectHorizontally, *reflectVertically, *storeResult; } st_refa;
static void refa_bind(act_inst *a) { ST(st_refa); s->angle = FIELD(angle); s->reflectHorizontally = FIELD(reflectHorizontally); s->reflectVertically = FIELD(reflectVertically); s->storeResult = FIELD(storeResult); }
static void refa_enter(act_inst *a)
{
    ST(st_refa);
    float n = pf(f, s->angle);
    if (pb(f, s->reflectHorizontally)) n = 180.0f - n;
    if (pb(f, s->reflectVertically)) n = 0.0f - n;
    while (n > 360.0f) n -= 360.0f;
    while (n < -360.0f) n += 360.0f;
    pf_set(f, s->storeResult, n);
    act_finish(a);
}
static const act_vtable AV_ReflectAngle = { "ReflectAngle", sizeof(st_refa), refa_bind, refa_enter, NULL, NULL, NULL, NULL, NULL, NULL };

const act_vtable *const act_registry_math[] = {
    &AV_SetFloatValue, &AV_SetBoolValue, &AV_SetIntValue, &AV_SetGameObject, &AV_SetStringValue, &AV_FloatAdd,
    &AV_FloatSubtract, &AV_FloatMultiply, &AV_FloatClamp, &AV_FloatOperator, &AV_SetFloatToHighest,
    &AV_IntAdd, &AV_IntAddV2, &AV_IntOperator, &AV_Vector3AddXYZ, &AV_RandomFloat, &AV_RandomFloatEither,
    &AV_SetColorValue, &AV_GetColorRGBA, &AV_FloatDivide, &AV_RandomBool, &AV_EaseFloat, &AV_SetVector2XY,
    &AV_FloatMultiplyV2, &AV_RandomInt, &AV_DistanceBetweenPoints, &AV_GetAngleBetweenPoints, &AV_Vector3Lerp,
    &AV_IntClamp, &AV_BoolFlip, &AV_FloatAddV2, &AV_SetVector3XYZ, &AV_GetVector3XYZ, &AV_SetVector3Value,
    &AV_BuildString, &AV_ConvertIntToFloat, &AV_ConvertFloatToInt, &AV_RandomlyFlipFloat, &AV_FloatDivideV2,
    &AV_SelectRandomVector3, &AV_SelectRandomString, &AV_BoolFlipEveryFrame, &AV_GetVector2XY,
    &AV_SetFloatValueV2, &AV_ConvertIntToString, &AV_SetFloatToSmallest,
    &AV_DistanceBetweenPoints2D, &AV_ReflectAngle,
};
const int act_registry_math_n = (int)(sizeof act_registry_math / sizeof act_registry_math[0]);
