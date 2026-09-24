/* State flow and tests: waits, bool/float/int/string comparisons and switches. */
#include "act.h"

static float mabs(float x) { return x < 0.0f ? -x : x; }   /* Mathf.Abs on float32 */

/* Wait — ACT/Wait.cs */
typedef struct { const fsm_pv *time, *finishEvent, *realTime; float timer; } st_wait;
static void wait_bind(act_inst *a) { ST(st_wait); s->time = FIELD(time); s->finishEvent = FIELD(finishEvent); s->realTime = FIELD(realTime); }
static void wait_enter(act_inst *a)
{
    ST(st_wait);
    if (pf(f, s->time) <= 0.0f) { fsm_event(f, EV(s->finishEvent)); act_finish(a); }       /* :27-32 */
    /* :36 timer = 0 (startTime is realtime-only) -- except when a snapshot re-enters the state the dump
     * caught mid-countdown, where the dumped `timer` is the right starting value. */
    else s->timer = act_live_float(a, "timer", 0.0f);
}
static void wait_update(act_inst *a)
{
    ST(st_wait);
    if (pb(f, s->realTime)) HKSIM_UNKNOWN("Wait.realTime (FsmTime.RealtimeSinceStartup) in %s — wall clock", fsm_label(f));
    s->timer += w->dt;                                                                      /* :47 */
    if (s->timer >= pf(f, s->time)) {                                                       /* :49-56 finish THEN event */
        act_finish(a);
        int32_t ev = EV(s->finishEvent);
        if (ev >= 0) fsm_event(f, ev);
    }
}
static const act_vtable AV_Wait = { "Wait", sizeof(st_wait), wait_bind, wait_enter, wait_update, NULL, NULL, NULL, NULL, NULL };

/* WaitRandom — ACT/WaitRandom.cs: ONE Range(timeMin,timeMax) draw at OnEnter (:34) */
typedef struct { const fsm_pv *timeMin, *timeMax, *finishEvent, *realTime; float time, timer; } st_waitrandom;
static void waitrandom_bind(act_inst *a) { ST(st_waitrandom); s->timeMin = FIELD(timeMin); s->timeMax = FIELD(timeMax); s->finishEvent = FIELD(finishEvent); s->realTime = FIELD(realTime); }
static void waitrandom_enter(act_inst *a)
{
    ST(st_waitrandom);
    /* :34 ONE Range(timeMin,timeMax) draw at OnEnter -- except when a snapshot re-enters the state the
     * dump caught mid-countdown: the game drew its duration before SceneReady, and the dump carries both
     * the drawn `time` and the elapsed `timer` (oracle/Oracle/FsmDumper.cs:280-292).  Outside snapshot_mode
     * act_live_float returns the -1 sentinel. */
    float live_time = act_live_float(a, "time", -1.0f);
    if (live_time >= 0.0f) {
        s->time = live_time;
        s->timer = act_live_float(a, "timer", 0.0f);
        if (s->time <= 0.0f) { fsm_event(f, EV(s->finishEvent)); act_finish(a); }
        return;
    }
    s->time = hk_rng_range_f_site(w->rng, a->rng_site, pf(f, s->timeMin), pf(f, s->timeMax));
    if (s->time <= 0.0f) { fsm_event(f, EV(s->finishEvent)); act_finish(a); }
    else s->timer = 0.0f;
}
static void waitrandom_update(act_inst *a)
{
    ST(st_waitrandom);
    if (pb(f, s->realTime)) HKSIM_UNKNOWN("WaitRandom.realTime in %s — wall clock", fsm_label(f));
    s->timer += w->dt;
    if (s->timer >= s->time) {
        act_finish(a);
        int32_t ev = EV(s->finishEvent);
        if (ev >= 0) fsm_event(f, ev);
    }
}
static const act_vtable AV_WaitRandom = { "WaitRandom", sizeof(st_waitrandom), waitrandom_bind, waitrandom_enter, waitrandom_update, NULL, NULL, NULL, NULL, NULL };

/* NextFrameEvent — ACT/NextFrameEvent.cs:15-23 */
typedef struct { const fsm_pv *sendEvent; } st_nfe;
static void nfe_bind(act_inst *a) { ST(st_nfe); s->sendEvent = FIELD(sendEvent); }
static void nfe_update(act_inst *a) { ST(st_nfe); act_finish(a); fsm_event(f, EV(s->sendEvent)); }
static const act_vtable AV_NextFrameEvent = { "NextFrameEvent", sizeof(st_nfe), nfe_bind, NULL, nfe_update, NULL, NULL, NULL, NULL, NULL };

/* BoolTest — ACT/BoolTest.cs:29-41 */
typedef struct { const fsm_pv *boolVariable, *isTrue, *isFalse, *everyFrame; } st_booltest;
static void booltest_bind(act_inst *a) { ST(st_booltest); s->boolVariable = FIELD(boolVariable); s->isTrue = FIELD(isTrue); s->isFalse = FIELD(isFalse); s->everyFrame = FIELD(everyFrame); }
static void booltest_enter(act_inst *a)
{
    ST(st_booltest);
    fsm_event(f, pb(f, s->boolVariable) ? EV(s->isTrue) : EV(s->isFalse));
    if (!pb(f, s->everyFrame)) act_finish(a);
}
static void booltest_update(act_inst *a) { ST(st_booltest); fsm_event(f, pb(f, s->boolVariable) ? EV(s->isTrue) : EV(s->isFalse)); }
static const act_vtable AV_BoolTest = { "BoolTest", sizeof(st_booltest), booltest_bind, booltest_enter, booltest_update, NULL, NULL, NULL, NULL, NULL };

/* BoolTestMulti — ACT/BoolTestMulti.cs:35-73 */
typedef struct { const fsm_pv *boolVariables, *boolStates, *trueEvent, *falseEvent, *storeResult, *everyFrame; } st_btm;
static void btm_bind(act_inst *a) { ST(st_btm); s->boolVariables = FIELD(boolVariables); s->boolStates = FIELD(boolStates); s->trueEvent = FIELD(trueEvent); s->falseEvent = FIELD(falseEvent); s->storeResult = FIELD(storeResult); s->everyFrame = FIELD(everyFrame); }
static void btm_do(act_inst *a)
{
    ST(st_btm);
    int32_t n1 = a_array_len(a, s->boolVariables), n2 = a_array_len(a, s->boolStates);
    if (n1 == 0 || n2 == 0 || n1 != n2) return;
    bool flag = true;
    for (int32_t i = 0; i < n1; i++) {
        if (pb(f, a_array_elem(a, s->boolVariables, i)) != pb(f, a_array_elem(a, s->boolStates, i))) { flag = false; break; }
    }
    pb_set(f, s->storeResult, flag);
    fsm_event(f, flag ? EV(s->trueEvent) : EV(s->falseEvent));
}
static void btm_enter(act_inst *a) { ST(st_btm); btm_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_BoolTestMulti = { "BoolTestMulti", sizeof(st_btm), btm_bind, btm_enter, btm_do, NULL, NULL, NULL, NULL, NULL };

/* FloatCompare — ACT/FloatCompare.cs:44-72 */
typedef struct { const fsm_pv *float1, *float2, *tolerance, *equal, *lessThan, *greaterThan, *everyFrame; } st_fcmp;
static void fcmp_bind(act_inst *a) { ST(st_fcmp); s->float1 = FIELD(float1); s->float2 = FIELD(float2); s->tolerance = FIELD(tolerance); s->equal = FIELD(equal); s->lessThan = FIELD(lessThan); s->greaterThan = FIELD(greaterThan); s->everyFrame = FIELD(everyFrame); }
static void fcmp_do(act_inst *a)
{
    ST(st_fcmp);
    float v1 = pf(f, s->float1), v2 = pf(f, s->float2);
    if (mabs(v1 - v2) <= pf(f, s->tolerance)) fsm_event(f, EV(s->equal));
    else if (v1 < v2) fsm_event(f, EV(s->lessThan));
    else if (v1 > v2) fsm_event(f, EV(s->greaterThan));
}
static void fcmp_enter(act_inst *a) { ST(st_fcmp); fcmp_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_FloatCompare = { "FloatCompare", sizeof(st_fcmp), fcmp_bind, fcmp_enter, fcmp_do, NULL, NULL, NULL, NULL, NULL };

/* FloatTestToBool — ACT/FloatTestToBool.cs:44-84 */
typedef struct { const fsm_pv *float1, *float2, *tolerance, *equalBool, *lessThanBool, *greaterThanBool, *everyFrame; } st_ftb;
static void ftb_bind(act_inst *a) { ST(st_ftb); s->float1 = FIELD(float1); s->float2 = FIELD(float2); s->tolerance = FIELD(tolerance); s->equalBool = FIELD(equalBool); s->lessThanBool = FIELD(lessThanBool); s->greaterThanBool = FIELD(greaterThanBool); s->everyFrame = FIELD(everyFrame); }
static void ftb_do(act_inst *a)
{
    ST(st_ftb);
    float v1 = pf(f, s->float1), v2 = pf(f, s->float2);
    pb_set(f, s->equalBool, mabs(v1 - v2) <= pf(f, s->tolerance));
    pb_set(f, s->lessThanBool, v1 < v2);
    pb_set(f, s->greaterThanBool, v1 > v2);
}
static void ftb_enter(act_inst *a) { ST(st_ftb); ftb_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_FloatTestToBool = { "FloatTestToBool", sizeof(st_ftb), ftb_bind, ftb_enter, ftb_do, NULL, NULL, NULL, NULL, NULL };

/* FloatInRange — ACT/FloatInRange.cs:38-67 (inclusive both ends) */
typedef struct { const fsm_pv *floatVariable, *lowerValue, *upperValue, *boolVariable, *trueEvent, *falseEvent, *everyFrame; } st_fir;
static void fir_bind(act_inst *a) { ST(st_fir); s->floatVariable = FIELD(floatVariable); s->lowerValue = FIELD(lowerValue); s->upperValue = FIELD(upperValue); s->boolVariable = FIELD(boolVariable); s->trueEvent = FIELD(trueEvent); s->falseEvent = FIELD(falseEvent); s->everyFrame = FIELD(everyFrame); }
static void fir_do(act_inst *a)
{
    ST(st_fir);
    if (p_isnone(s->floatVariable)) return;
    float v = pf(f, s->floatVariable);
    if (v <= pf(f, s->upperValue) && v >= pf(f, s->lowerValue)) { pb_set(f, s->boolVariable, true); fsm_event(f, EV(s->trueEvent)); }
    else { pb_set(f, s->boolVariable, false); fsm_event(f, EV(s->falseEvent)); }
}
static void fir_enter(act_inst *a) { ST(st_fir); fir_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_FloatInRange = { "FloatInRange", sizeof(st_fir), fir_bind, fir_enter, fir_do, NULL, NULL, NULL, NULL, NULL };

/* IntCompare — ACT/IntCompare.cs:34-62 */
typedef struct { const fsm_pv *integer1, *integer2, *equal, *lessThan, *greaterThan, *everyFrame; } st_icmp;
static void icmp_bind(act_inst *a) { ST(st_icmp); s->integer1 = FIELD(integer1); s->integer2 = FIELD(integer2); s->equal = FIELD(equal); s->lessThan = FIELD(lessThan); s->greaterThan = FIELD(greaterThan); s->everyFrame = FIELD(everyFrame); }
static void icmp_do(act_inst *a)
{
    ST(st_icmp);
    int32_t v1 = pi(f, s->integer1), v2 = pi(f, s->integer2);
    if (v1 == v2) fsm_event(f, EV(s->equal));
    else if (v1 < v2) fsm_event(f, EV(s->lessThan));
    else if (v1 > v2) fsm_event(f, EV(s->greaterThan));
}
static void icmp_enter(act_inst *a) { ST(st_icmp); icmp_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_IntCompare = { "IntCompare", sizeof(st_icmp), icmp_bind, icmp_enter, icmp_do, NULL, NULL, NULL, NULL, NULL };

/* IntSwitch — analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/IntSwitch.cs:26-54 */
typedef struct { const fsm_pv *intVariable, *compareTo, *sendEvent, *everyFrame; } st_isw;
static void isw_bind(act_inst *a) { ST(st_isw); s->intVariable = FIELD(intVariable); s->compareTo = FIELD(compareTo); s->sendEvent = FIELD(sendEvent); s->everyFrame = FIELD(everyFrame); }
static void isw_do(act_inst *a)
{
    ST(st_isw);
    if (p_isnone(s->intVariable)) return;
    int32_t val = pi(f, s->intVariable);
    int32_t n = a_array_len(a, s->compareTo);
    int32_t ne = a_array_len(a, s->sendEvent);
    int32_t count = n < ne ? n : ne;
    for (int32_t i = 0; i < count; i++) {
        if (val == pi(f, a_array_elem(a, s->compareTo, i))) {
            fsm_event(f, EV(a_array_elem(a, s->sendEvent, i)));
            break;
        }
    }
}
static void isw_enter(act_inst *a) { ST(st_isw); isw_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static void isw_update(act_inst *a) { ST(st_isw); isw_do(a); }
static const act_vtable AV_IntSwitch = { "IntSwitch", sizeof(st_isw), isw_bind, isw_enter, isw_update, NULL, NULL, NULL, NULL, NULL };

/* GotoPreviousState — ACT/GotoPreviousState.cs:12-20: SwitchState synchronously, then Finish */
static void gps_enter(act_inst *a) { fsm_inst *f = a->fsm; if (f->previous_state >= 0) fsm_goto_previous_state(f); act_finish(a); }
static const act_vtable AV_GotoPreviousState = { "GotoPreviousState", 0, NULL, gps_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* GameObjectIsNull — ACT/GameObjectIsNull.cs:29-46 */
typedef struct { const fsm_pv *gameObject, *isNull, *isNotNull, *storeResult, *everyFrame; } st_gon;
static void gon_bind(act_inst *a) { ST(st_gon); s->gameObject = FIELD(gameObject); s->isNull = FIELD(isNull); s->isNotNull = FIELD(isNotNull); s->storeResult = FIELD(storeResult); s->everyFrame = FIELD(everyFrame); }
static void gon_do(act_inst *a)
{
    ST(st_gon);
    bool flag = pgo(f, s->gameObject) < 0;
    pb_set(f, s->storeResult, flag);
    fsm_event(f, flag ? EV(s->isNull) : EV(s->isNotNull));
}
static void gon_enter(act_inst *a) { ST(st_gon); gon_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GameObjectIsNull = { "GameObjectIsNull", sizeof(st_gon), gon_bind, gon_enter, gon_do, NULL, NULL, NULL, NULL, NULL };

/* FloatSwitch — ACT/FloatSwitch.cs:28-56 */
typedef struct { const fsm_pv *floatVariable, *lessThan, *sendEvent, *everyFrame; } st_fswitch;
static void fswitch_bind(act_inst *a) { ST(st_fswitch); s->floatVariable = FIELD(floatVariable); s->lessThan = FIELD(lessThan); s->sendEvent = FIELD(sendEvent); s->everyFrame = FIELD(everyFrame); }
static void fswitch_do(act_inst *a) {
    ST(st_fswitch);
    if (p_isnone(s->floatVariable)) return;
    float val = pf(f, s->floatVariable);
    int32_t n = a_array_len(a, s->lessThan);
    for (int32_t i = 0; i < n; i++) {
        if (val < pf(f, a_array_elem(a, s->lessThan, i))) {
            fsm_event(f, EV(a_array_elem(a, s->sendEvent, i)));
            break;
        }
    }
}
static void fswitch_enter(act_inst *a) { ST(st_fswitch); fswitch_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static void fswitch_update(act_inst *a) { ST(st_fswitch); fswitch_do(a); }
static const act_vtable AV_FloatSwitch = { "FloatSwitch", sizeof(st_fswitch), fswitch_bind, fswitch_enter, fswitch_update, NULL, NULL, NULL, NULL, NULL };

/* BoolAnyTrue — ACT/BoolAnyTrue.cs:27-43: first true variable raises sendEvent and stops the scan */
typedef struct { const fsm_pv *boolVariables, *sendEvent, *storeResult, *everyFrame; } st_bat;
static void bat_bind(act_inst *a) { ST(st_bat); s->boolVariables = FIELD(boolVariables); s->sendEvent = a_field(a, "sendEvent"); s->storeResult = FIELD(storeResult); s->everyFrame = a_field(a, "everyFrame"); }
static void bat_do(act_inst *a)
{
    ST(st_bat);
    int32_t n = a_array_len(a, s->boolVariables);
    if (n == 0) return;                                            /* :29-32 */
    pb_set(f, s->storeResult, false);                              /* :33 */
    for (int32_t i = 0; i < n; i++) {
        const fsm_pv *e = a_array_elem(a, s->boolVariables, i);
        if (e && pb(f, e)) { fsm_event(f, EV(s->sendEvent)); pb_set(f, s->storeResult, true); break; }
    }
}
static void bat_enter(act_inst *a) { ST(st_bat); bat_do(a); if (!s->everyFrame || !pb(f, s->everyFrame)) act_finish(a); }
static void bat_update(act_inst *a) { bat_do(a); }
static const act_vtable AV_BoolAnyTrue = { "BoolAnyTrue", sizeof(st_bat), bat_bind, bat_enter, bat_update, NULL, NULL, NULL, NULL, NULL };

/* IntTestToBool — ACT/IntTestToBool.cs:38-70.  Writes all three bools every pass, including the
 * false branches; the decomp has no null guard because an unbound FsmBool still exists inline. */
typedef struct { const fsm_pv *int1, *int2, *equalBool, *lessThanBool, *greaterThanBool, *everyFrame; } st_ittb;
static void ittb_bind(act_inst *a)
{
    ST(st_ittb);
    s->int1 = FIELD(int1); s->int2 = FIELD(int2);
    s->equalBool = a_field(a, "equalBool"); s->lessThanBool = a_field(a, "lessThanBool");
    s->greaterThanBool = a_field(a, "greaterThanBool"); s->everyFrame = FIELD(everyFrame);
}
static void ittb_do(act_inst *a)
{
    ST(st_ittb);
    int32_t i1 = pi(f, s->int1), i2 = pi(f, s->int2);
    if (s->equalBool && !p_isnone(s->equalBool)) pb_set(f, s->equalBool, i1 == i2);
    if (s->lessThanBool && !p_isnone(s->lessThanBool)) pb_set(f, s->lessThanBool, i1 < i2);
    if (s->greaterThanBool && !p_isnone(s->greaterThanBool)) pb_set(f, s->greaterThanBool, i1 > i2);
}
static void ittb_enter(act_inst *a) { ST(st_ittb); ittb_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_IntTestToBool = { "IntTestToBool", sizeof(st_ittb), ittb_bind, ittb_enter, ittb_do, NULL, NULL, NULL, NULL, NULL };

/* BoolAllTrue — ACT/BoolAllTrue.cs:24-56, BoolNoneTrue — ACT/BoolNoneTrue.cs:24-56 */
typedef struct { const fsm_pv *vars, *sendEvent, *storeResult, *everyFrame; bool none; } st_ball;
static void ball_bind_common(act_inst *a, bool none) { ST(st_ball); s->vars = FIELD(boolVariables); s->sendEvent = FIELD_OPT(sendEvent); s->storeResult = FIELD(storeResult); s->everyFrame = FIELD(everyFrame); s->none = none; }
static void ball_bind(act_inst *a) { ball_bind_common(a, false); }
static void bnone_bind(act_inst *a) { ball_bind_common(a, true); }
static void ball_do(act_inst *a)
{
    ST(st_ball);
    int32_t n = a_array_len(a, s->vars);
    if (n == 0) return;
    bool flag = true;
    for (int32_t i = 0; i < n; i++) { bool v = pb(f, a_array_elem(a, s->vars, i)); if (s->none ? v : !v) { flag = false; break; } }
    if (flag) fsm_event(f, EV(s->sendEvent));
    pb_set(f, s->storeResult, flag);
}
static void ball_enter(act_inst *a) { ST(st_ball); ball_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_BoolAllTrue = { "BoolAllTrue", sizeof(st_ball), ball_bind, ball_enter, ball_do, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_BoolNoneTrue = { "BoolNoneTrue", sizeof(st_ball), bnone_bind, ball_enter, ball_do, NULL, NULL, NULL, NULL, NULL };

/* StringCompare — ACT/StringCompare.cs:29-59 */
typedef struct { const fsm_pv *v, *compareTo, *equalEvent, *notEqualEvent, *storeResult, *everyFrame; } st_scmp;
static void scmp_bind(act_inst *a) { ST(st_scmp); s->v = FIELD(stringVariable); s->compareTo = FIELD(compareTo); s->equalEvent = FIELD_OPT(equalEvent); s->notEqualEvent = FIELD_OPT(notEqualEvent); s->storeResult = FIELD_OPT(storeResult); s->everyFrame = FIELD(everyFrame); }
static void scmp_do(act_inst *a)
{
    ST(st_scmp);
    bool flag = strcmp(w_str(w, ps(f, s->v)), w_str(w, ps(f, s->compareTo))) == 0;
    if (s->storeResult) pb_set(f, s->storeResult, flag);
    if (flag && EV(s->equalEvent) >= 0) fsm_event(f, EV(s->equalEvent));
    else if (!flag && EV(s->notEqualEvent) >= 0) fsm_event(f, EV(s->notEqualEvent));
}
static void scmp_enter(act_inst *a) { ST(st_scmp); scmp_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_StringCompare = { "StringCompare", sizeof(st_scmp), scmp_bind, scmp_enter, scmp_do, NULL, NULL, NULL, NULL, NULL };

/* CompareNames — ACT/CompareNames.cs:20-41: name.Contains(any of strings) -> trueEvent else falseEvent */
typedef struct { const fsm_pv *name, *strings, *target, *trueEvent, *falseEvent; } st_cnames;
static void cnames_bind(act_inst *a) { ST(st_cnames); s->name = FIELD(name); s->strings = FIELD(strings); s->target = FIELD(target); s->trueEvent = FIELD_OPT(trueEvent); s->falseEvent = FIELD_OPT(falseEvent); }
static void cnames_enter(act_inst *a)
{
    ST(st_cnames);
    if (!p_isnone(s->name) && *w_str(w, ps(f, s->name))) {
        const char *name = w_str(w, ps(f, s->name));
        int32_t tgt = act_event_target(a, s->target);
        const fsm_pv *elems; int32_t n = world_array_elems(a, s->strings, &elems);
        for (int32_t i = 0; i < n; i++) {
            if (strstr(name, w_str(w, ps(f, &elems[i]))) != NULL) {
                if (EV(s->trueEvent) >= 0) fsm_event_to(f, a, tgt, EV(s->trueEvent));
                act_finish(a); return;
            }
        }
        if (EV(s->falseEvent) >= 0) fsm_event_to(f, a, tgt, EV(s->falseEvent));
    }
    act_finish(a);
}
static const act_vtable AV_CompareNames = { "CompareNames", sizeof(st_cnames), cnames_bind, cnames_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* HasComponent - ACT/HasComponent.cs:41-70: go.GetComponent(component.Value) != null, against the dump's
 * component list (go_has_component).  removeOnExit (:56-61) would Object.Destroy the component; runtime
 * component removal is not modelled, so it traps. */
typedef struct { const fsm_pv *gameObject, *trueEvent, *falseEvent, *store, *everyFrame, *removeOnExit; const char *component; bool had; } st_hc;
static void hc_bind(act_inst *a) {
    ST(st_hc);
    s->gameObject = FIELD(gameObject); s->trueEvent = FIELD(trueEvent); s->falseEvent = FIELD(falseEvent);
    s->store = FIELD(store); s->everyFrame = FIELD(everyFrame); s->removeOnExit = FIELD_OPT(removeOnExit);
    s->component = w_str(w, ps(f, FIELD(component)));
}
static void hc_do(act_inst *a) {
    ST(st_hc);
    int32_t go = p_owner_default(a, s->gameObject);
    bool has = go >= 0 && go_has_component(w, go, s->component);
    s->had = has;
    if (!p_isnone(s->store)) pb_set(f, s->store, has);
    fsm_event(f, has ? EV(s->trueEvent) : EV(s->falseEvent));   /* base.Fsm.Event(null) is a safe no-op (fsm_event drops ev<0) */
}
static void hc_enter(act_inst *a) { ST(st_hc); hc_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static void hc_exit(act_inst *a) {
    ST(st_hc);
    if (s->removeOnExit && s->had && pb(f, s->removeOnExit))
        HKSIM_UNIMPLEMENTED("HasComponent removeOnExit destroys %s generically (no dumped user sets it true) in %s", s->component, fsm_label(f));
}
static const act_vtable AV_HasComponent = { "HasComponent", sizeof(st_hc), hc_bind, hc_enter, hc_do, NULL, NULL, hc_exit, NULL, NULL };

/* Vector3Compare -- ACT/Vector3Compare.cs:30-58: per-axis |a - b| <= tolerance */
typedef struct { const fsm_pv *v1, *v2, *tol, *equal, *notEqual, *everyFrame; } st_v3c;
static void v3c_bind(act_inst *a) { ST(st_v3c); s->v1 = FIELD(vector3Variable1); s->v2 = FIELD(vector3Variable2); s->tol = FIELD(tolerance); s->equal = FIELD(equal); s->notEqual = FIELD(notEqual); s->everyFrame = FIELD(everyFrame); }
static void v3c_do(act_inst *a)
{
    ST(st_v3c);
    const float *x = pv3(f, s->v1), *y = pv3(f, s->v2);
    float t = pf(f, s->tol);
    if (fabsf(x[0] - y[0]) <= t && fabsf(x[1] - y[1]) <= t && fabsf(x[2] - y[2]) <= t) fsm_event(f, EV(s->equal));
    else fsm_event(f, EV(s->notEqual));
}
static void v3c_enter(act_inst *a) { ST(st_v3c); v3c_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_Vector3Compare = { "Vector3Compare", sizeof(st_v3c), v3c_bind, v3c_enter, v3c_do, NULL, NULL, NULL, NULL, NULL };

/* StringSwitch -- ACT/StringSwitch.cs:26-54: the first equal entry sends its event (sendEvent[i] shorter than
 * compareTo throws: trapped) */
typedef struct { const fsm_pv *str, *compareTo, *sendEvent, *everyFrame; } st_ssw;
static void ssw_bind(act_inst *a) { ST(st_ssw); s->str = FIELD(stringVariable); s->compareTo = FIELD(compareTo); s->sendEvent = FIELD(sendEvent); s->everyFrame = FIELD(everyFrame); }
static void ssw_do(act_inst *a)
{
    ST(st_ssw);
    if (p_isnone(s->str)) return;
    int32_t v = ps(f, s->str), n = a_array_len(a, s->compareTo), ne = a_array_len(a, s->sendEvent);
    for (int32_t i = 0; i < n; i++) {
        if (v != ps(f, a_array_elem(a, s->compareTo, i))) continue;
        if (i >= ne) HKSIM_UNIMPLEMENTED("StringSwitch: sendEvent[%d] out of range in %s", i, fsm_label(f));
        fsm_event(f, EV(a_array_elem(a, s->sendEvent, i)));
        break;
    }
}
static void ssw_enter(act_inst *a) { ST(st_ssw); ssw_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_StringSwitch = { "StringSwitch", sizeof(st_ssw), ssw_bind, ssw_enter, ssw_do, NULL, NULL, NULL, NULL, NULL };

/* FloatSignTest -- ACT/FloatSignTest.cs:30-46.  floatValue is RequiredField, dereferenced directly like the
 * other required-field math inputs elsewhere in this file. */
typedef struct { const fsm_pv *floatValue, *isPositive, *isNegative, *everyFrame; } st_fst;
static void fst_bind(act_inst *a) { ST(st_fst); s->floatValue = FIELD(floatValue); s->isPositive = FIELD_OPT(isPositive); s->isNegative = FIELD_OPT(isNegative); s->everyFrame = FIELD(everyFrame); }
static void fst_do(act_inst *a) { ST(st_fst); fsm_event(f, pf(f, s->floatValue) < 0.0f ? EV(s->isNegative) : EV(s->isPositive)); }
static void fst_enter(act_inst *a) { ST(st_fst); fst_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_FloatSignTest = { "FloatSignTest", sizeof(st_fst), fst_bind, fst_enter, fst_do, NULL, NULL, NULL, NULL, NULL };

/* GameObjectCompare -- ACT/GameObjectCompare.cs:35-59.  storeResult/equalEvent/notEqualEvent are not
 * RequiredField and are dereferenced unconditionally in the C# (a null one throws); guarded here instead of
 * reproducing that crash, since every dumped use sets storeResult. */
typedef struct { const fsm_pv *gameObjectVariable, *compareTo, *equalEvent, *notEqualEvent, *storeResult, *everyFrame; } st_goc;
static void goc_bind(act_inst *a) { ST(st_goc); s->gameObjectVariable = FIELD(gameObjectVariable); s->compareTo = FIELD(compareTo); s->equalEvent = FIELD_OPT(equalEvent); s->notEqualEvent = FIELD_OPT(notEqualEvent); s->storeResult = FIELD_OPT(storeResult); s->everyFrame = FIELD(everyFrame); }
static void goc_do(act_inst *a)
{
    ST(st_goc);
    bool eq = p_owner_default(a, s->gameObjectVariable) == pgo(f, s->compareTo);
    if (s->storeResult) pb_set(f, s->storeResult, eq);
    fsm_event(f, eq ? EV(s->equalEvent) : EV(s->notEqualEvent));
}
static void goc_enter(act_inst *a) { ST(st_goc); goc_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GameObjectCompare = { "GameObjectCompare", sizeof(st_goc), goc_bind, goc_enter, goc_do, NULL, NULL, NULL, NULL, NULL };

/* RandomWait -- ACT/RandomWait.cs:26-58: WaitRandom's shape (control.c above), minus WaitRandom's
 * SNAPSHOT_RULES live-value support (no dumped FSM catches this action mid-wait yet). */
typedef struct { const fsm_pv *min, *max, *finishEvent, *realTime; float time, timer; } st_rw;
static void rw_bind(act_inst *a) { ST(st_rw); s->min = FIELD(min); s->max = FIELD(max); s->finishEvent = FIELD_OPT(finishEvent); s->realTime = FIELD(realTime); }
static void rw_enter(act_inst *a)
{
    ST(st_rw);
    s->time = hk_rng_range_f_site(w->rng, a->rng_site, pf(f, s->min), pf(f, s->max));
    if (s->time <= 0.0f) { fsm_event(f, EV(s->finishEvent)); act_finish(a); }
    else s->timer = 0.0f;
}
static void rw_update(act_inst *a)
{
    ST(st_rw);
    if (pb(f, s->realTime)) HKSIM_UNKNOWN("RandomWait.realTime in %s -- wall clock", fsm_label(f));
    s->timer += w->dt;
    if (s->timer >= s->time) {
        act_finish(a);
        int32_t ev = EV(s->finishEvent);
        if (ev >= 0) fsm_event(f, ev);
    }
}
static const act_vtable AV_RandomWait = { "RandomWait", sizeof(st_rw), rw_bind, rw_enter, rw_update, NULL, NULL, NULL, NULL, NULL };

/* ColorInterpolate — ACT/ColorInterpolate.cs:41-97.  The interpolated colour (storeColor, an FsmColor
 * variable) only ever feeds a sprite/material colour write downstream (Tk2dSpriteSetColor `Colour` in every
 * dumped use, analysis/fsm), rendering and out of scope; this ports the state-machine-visible part alone: a
 * timer of `time` seconds that fires `finishEvent` once (colors.Length>=2), or Finish()es at once with no
 * event (colors.Length<2, :45-52).  colors.Length is the only thing read from `colors`; the array's own
 * elements (the actual colour stops) are never read here. */
typedef struct { const fsm_pv *colors, *time, *finishEvent, *realTime; int32_t n; float timer; } st_ci;
static void ci_bind(act_inst *a) { ST(st_ci); s->colors = FIELD(colors); s->time = FIELD(time); s->finishEvent = FIELD_OPT(finishEvent); s->realTime = FIELD(realTime); }
static void ci_enter(act_inst *a)
{
    ST(st_ci);
    s->n = a_array_len(a, s->colors);
    if (s->n < 2) { act_finish(a); return; }                        /* :45-52 Finish(), no event either way */
    s->timer = 0.0f;
}
static void ci_update(act_inst *a)
{
    ST(st_ci);
    if (pb(f, s->realTime)) HKSIM_UNKNOWN("ColorInterpolate.realTime in %s -- wall clock", fsm_label(f));
    s->timer += w->dt;
    if (s->timer > pf(f, s->time)) { act_finish(a); fsm_event(f, EV(s->finishEvent)); }   /* :69-77 Finish() before the event */
}
static const act_vtable AV_ColorInterpolate = { "ColorInterpolate", sizeof(st_ci), ci_bind, ci_enter, ci_update, NULL, NULL, NULL, NULL, NULL };

const act_vtable *const act_registry_control[] = {
    &AV_Wait, &AV_WaitRandom, &AV_NextFrameEvent, &AV_BoolTest, &AV_BoolTestMulti, &AV_FloatCompare,
    &AV_FloatTestToBool, &AV_FloatInRange, &AV_IntCompare, &AV_IntSwitch, &AV_GotoPreviousState,
    &AV_GameObjectIsNull, &AV_FloatSwitch, &AV_BoolAnyTrue, &AV_IntTestToBool, &AV_BoolAllTrue,
    &AV_BoolNoneTrue, &AV_StringCompare, &AV_CompareNames, &AV_HasComponent,
    &AV_Vector3Compare, &AV_StringSwitch,
    &AV_FloatSignTest, &AV_GameObjectCompare, &AV_RandomWait, &AV_ColorInterpolate,
};
const int act_registry_control_n = (int)(sizeof act_registry_control / sizeof act_registry_control[0]);
