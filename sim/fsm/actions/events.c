/* Event sending (fsm-actions.md §3): SendEvent, SendEventByName, SendRandomEvent, EventRegister, and the
 * event-routing checks. */
#include "act.h"
#include "core/alloc.h"

/* SendEventByName — ACT/SendEventByName.cs:31-60; SendEventByNameV2 — ACT/SendEventByNameV2.cs:23-52 has the
 * same fields and body */
typedef struct { const fsm_pv *eventTarget, *sendEvent, *delay, *everyFrame; int32_t de; } st_sebn;
static void sebn_bind(act_inst *a) { ST(st_sebn); s->eventTarget = FIELD(eventTarget); s->sendEvent = FIELD(sendEvent); s->delay = FIELD(delay); s->everyFrame = FIELD(everyFrame); }
static void sebn_enter(act_inst *a)
{
    ST(st_sebn);
    int32_t ev = w_get_fsm_event(w, w_str(w, ps(f, s->sendEvent)));   /* Fsm.Event(target, string): empty -> no-op (Fsm.cs:2106-2112) */
    if (pf(f, s->delay) < 0.001f) {
        if (ev >= 0) fsm_event_to(f, a, act_event_target(a, s->eventTarget), ev);
        if (!pb(f, s->everyFrame)) act_finish(a);
    } else {
        fsm_add_delayed(f, a, act_event_target(a, s->eventTarget), ev, pf(f, s->delay), &s->de);
    }
}
static void sebn_update(act_inst *a)
{
    ST(st_sebn);
    if (!pb(f, s->everyFrame)) { if (delayed_was_sent(f, s->de)) act_finish(a); }
    else { int32_t ev = w_get_fsm_event(w, w_str(w, ps(f, s->sendEvent))); if (ev >= 0) fsm_event_to(f, a, act_event_target(a, s->eventTarget), ev); }
}
static const act_vtable AV_SendEventByName = { "SendEventByName", sizeof(st_sebn), sebn_bind, sebn_enter, sebn_update, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_SendEventByNameV2 = { "SendEventByNameV2", sizeof(st_sebn), sebn_bind, sebn_enter, sebn_update, NULL, NULL, NULL, NULL, NULL };

/* SendEvent — ACT/SendEvent.cs:35-64 */
static void se_enter(act_inst *a)
{
    ST(st_sebn);
    int32_t ev = EV(s->sendEvent);
    if (pf(f, s->delay) < 0.001f) {
        fsm_event_to(f, a, act_event_target(a, s->eventTarget), ev);   /* Event(target, null) -> ProcessEvent drops null */
        if (!pb(f, s->everyFrame)) act_finish(a);
    } else {
        fsm_add_delayed(f, a, act_event_target(a, s->eventTarget), ev, pf(f, s->delay), &s->de);
    }
}
static void se_update(act_inst *a)
{
    ST(st_sebn);
    if (!pb(f, s->everyFrame)) { if (delayed_was_sent(f, s->de)) act_finish(a); }
    else fsm_event_to(f, a, act_event_target(a, s->eventTarget), EV(s->sendEvent));
}
static const act_vtable AV_SendEvent = { "SendEvent", sizeof(st_sebn), sebn_bind, se_enter, se_update, NULL, NULL, NULL, NULL, NULL };

/* SendRandomEvent — ACT/SendRandomEvent.cs:24-52 */
typedef struct { const fsm_pv *events, *weights, *delay; int32_t de; } st_sre;
static void sre_bind(act_inst *a) { ST(st_sre); s->events = FIELD(events); s->weights = FIELD(weights); s->delay = FIELD(delay); }
static void sre_enter(act_inst *a)
{
    ST(st_sre);
    if (a_array_len(a, s->events) != 0) {
        int32_t idx = random_weighted_index(a, s->weights);
        if (idx != -1) {
            int32_t ev = EV(a_array_elem(a, s->events, idx));
            if (pf(f, s->delay) < 0.001f) { fsm_event(f, ev); act_finish(a); }
            else fsm_add_delayed(f, NULL, -1, ev, pf(f, s->delay), &s->de);
            return;
        }
    }
    act_finish(a);
}
static void sre_update(act_inst *a) { ST(st_sre); if (delayed_was_sent(f, s->de)) act_finish(a); }
static const act_vtable AV_SendRandomEvent = { "SendRandomEvent", sizeof(st_sre), sre_bind, sre_enter, sre_update, NULL, NULL, NULL, NULL, NULL };

/* SendRandomEventV2 — ACT/SendRandomEventV2.cs:26-45: rejection loop, ONE draw per iteration */
typedef struct { const fsm_pv *events, *weights, *trackingInts, *eventMax; } st_srev2;
static void srev2_bind(act_inst *a) { ST(st_srev2); s->events = FIELD(events); s->weights = FIELD(weights); s->trackingInts = FIELD(trackingInts); s->eventMax = FIELD(eventMax); }
static void srev2_enter(act_inst *a)
{
    ST(st_srev2);
    bool flag = false;
    int32_t guard = 0;
    while (!flag) {
        int32_t idx = random_weighted_index(a, s->weights);
        if (idx != -1 && pi(f, a_array_elem(a, s->trackingInts, idx)) < pi(f, a_array_elem(a, s->eventMax, idx))) {
            int32_t value = pi(f, a_array_elem(a, s->trackingInts, idx)) + 1;
            pi_set(f, a_array_elem(a, s->trackingInts, idx), value);
            int32_t n = a_array_len(a, s->trackingInts);
            for (int32_t i = 0; i < n; i++) pi_set(f, a_array_elem(a, s->trackingInts, i), 0);
            pi_set(f, a_array_elem(a, s->trackingInts, idx), value);
            flag = true;
            fsm_event(f, EV(a_array_elem(a, s->events, idx)));
        }
        if (++guard > 1000000) HKSIM_UNKNOWN("SendRandomEventV2 spins forever in %s (Q-fsmact-9)", fsm_label(f));
    }
    act_finish(a);
}
static const act_vtable AV_SendRandomEventV2 = { "SendRandomEventV2", sizeof(st_srev2), srev2_bind, srev2_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* SendRandomEventV3 — ACT/SendRandomEventV3.cs:33-86: `loops` is an instance field never reset */
typedef struct { const fsm_pv *events, *weights, *trackingInts, *eventMax, *trackingIntsMissed, *missedMax; int32_t loops; } st_srev3;
static void srev3_bind(act_inst *a) { ST(st_srev3); s->events = FIELD(events); s->weights = FIELD(weights); s->trackingInts = FIELD(trackingInts); s->eventMax = FIELD(eventMax); s->trackingIntsMissed = FIELD(trackingIntsMissed); s->missedMax = FIELD(missedMax); }
static void srev3_enter(act_inst *a)
{
    ST(st_srev3);
    bool flag = false, flag2 = false;
    int32_t num = 0;
    int32_t nT = a_array_len(a, s->trackingInts), nM = a_array_len(a, s->trackingIntsMissed);
    while (!flag) {
        int32_t idx = random_weighted_index(a, s->weights);
        if (idx != -1) {
            for (int32_t i = 0; i < nM; i++) {                        /* :43-50 no break: highest over-limit index wins */
                if (pi(f, a_array_elem(a, s->trackingIntsMissed, i)) >= pi(f, a_array_elem(a, s->missedMax, i))) { flag2 = true; num = i; }
            }
            if (flag2) {
                flag = true;
                for (int32_t j = 0; j < nT; j++) {
                    pi_set(f, a_array_elem(a, s->trackingInts, j), 0);
                    const fsm_pv *m = a_array_elem(a, s->trackingIntsMissed, j);
                    pi_set(f, m, pi(f, m) + 1);
                }
                pi_set(f, a_array_elem(a, s->trackingIntsMissed, num), 0);
                pi_set(f, a_array_elem(a, s->trackingInts, num), 1);
                fsm_event(f, EV(a_array_elem(a, s->events, num)));
            } else if (pi(f, a_array_elem(a, s->trackingInts, idx)) < pi(f, a_array_elem(a, s->eventMax, idx))) {
                int32_t value = pi(f, a_array_elem(a, s->trackingInts, idx)) + 1;
                pi_set(f, a_array_elem(a, s->trackingInts, idx), value);
                for (int32_t k = 0; k < nT; k++) {
                    pi_set(f, a_array_elem(a, s->trackingInts, k), 0);
                    const fsm_pv *m = a_array_elem(a, s->trackingIntsMissed, k);
                    pi_set(f, m, pi(f, m) + 1);
                }
                pi_set(f, a_array_elem(a, s->trackingInts, idx), value);
                pi_set(f, a_array_elem(a, s->trackingIntsMissed, idx), 0);
                flag = true;
                fsm_event(f, EV(a_array_elem(a, s->events, idx)));
            }
        }
        s->loops++;
        if (s->loops > 100) {                                        /* :78-84 cumulative budget */
            fsm_event(f, EV(a_array_elem(a, s->events, 0)));
            flag = true;
            act_finish(a);
        }
    }
    act_finish(a);
}
static const act_vtable AV_SendRandomEventV3 = { "SendRandomEventV3", sizeof(st_srev3), srev3_bind, srev3_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* SendEventToGameObjectOptimized — ACT/SendEventToGameObjectOptimized.cs:15-38 -> FSMUtility.SendEventToGameObject */
typedef struct { const fsm_pv *target, *sendEvent, *everyFrame; } st_segoo;
static void segoo_bind(act_inst *a) { ST(st_segoo); s->target = FIELD(target); s->sendEvent = FIELD(sendEvent); s->everyFrame = FIELD(everyFrame); }
static void segoo_do(act_inst *a) { ST(st_segoo); int32_t g = p_get_safe(a, s->target); if (g >= 0) world_send_event_to_go(w, g, w_str(w, ps(f, s->sendEvent)), false); }
static void segoo_enter(act_inst *a) { ST(st_segoo); segoo_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_SendEventToGameObjectOptimized = { "SendEventToGameObjectOptimized", sizeof(st_segoo), segoo_bind, segoo_enter, segoo_do, NULL, NULL, NULL, NULL, NULL };

/* EventRegister (HK/EventRegister.cs): static event name -> subscriber list.  Subscribers come from the
 * EventRegister components authored on prefabs (hierarchy.json.gz `subscribedEvent`, compiled to
 * sc->evregs and installed by event_register_seed) and from the AddEventRegister action at run time. */
static void event_register_send(fsm_world *w, const char *name)
{
    if (!name[0]) return;
    for (int32_t i = 0; i < w->n_ev_reg; i++)
        if (strcmp(w_str(w, w->ev_reg[i].name), name) == 0) {
            int32_t go = w->ev_reg[i].go;
            world_send_event_to_go(w, go, name, false);                /* ReceiveEvent :26-31 */
            /* EnemyKillEventListener.OnEnable subscribes Die to the SAME EventRegister.OnReceivedEvent
             * delegate (EnemyKillEventListener.cs:17-27); Die (:29-40) hits its own HealthManager for
             * lethal, unblockable damage (AttackTypes.Generic=1, 9999, IgnoreInvulnerable=true). */
            if (go_has_component(w, go, "EnemyKillEventListener")) {
                hm_inst *h = hm_of_go(w, go);
                if (h) hm_hit(w, h, -1, 1, 9999, 0.0f, false, true, 0.0f, 1.0f);
            }
        }
}
/* SendEventToRegister — HK/SendEventToRegister.cs:13-20 */
typedef struct { const fsm_pv *eventName; } st_setr;
static void setr_bind(act_inst *a) { ST(st_setr); s->eventName = FIELD(eventName); }
static void setr_enter(act_inst *a) { ST(st_setr); event_register_send(w, w_str(w, ps(f, s->eventName))); act_finish(a); }
static const act_vtable AV_SendEventToRegister = { "SendEventToRegister", sizeof(st_setr), setr_bind, setr_enter, NULL, NULL, NULL, NULL, NULL, NULL };
/* AddEventRegister — HK/AddEventRegister.cs:13-23 */
typedef struct { const fsm_pv *target, *eventName; } st_aer;
static void aer_bind(act_inst *a) { ST(st_aer); s->target = FIELD(target); s->eventName = FIELD(eventName); }
static void aer_enter(act_inst *a)
{
    ST(st_aer);
    int32_t name = ps(f, s->eventName);
    if (w_str(w, name)[0]) {
        int32_t g = p_get_safe(a, s->target);
        if (g >= 0) { HKSIM_ASSERT(w->n_ev_reg < FSM_EV_REG_CAP, "EventRegister table full"); w->ev_reg[w->n_ev_reg].go = g; w->ev_reg[w->n_ev_reg].name = name; w->n_ev_reg++; }
    }
    act_finish(a);
}
static const act_vtable AV_AddEventRegister = { "AddEventRegister", sizeof(st_aer), aer_bind, aer_enter, NULL, NULL, NULL, NULL, NULL, NULL };
void event_register_reset(fsm_world *w) { w->n_ev_reg = 0; }
/* The prefab-authored subscribers (EventRegister.Awake -> SubscribeEvent).  HK subscribes on the
 * object's FIRST ACTIVATION rather than at scene load, and unsubscribes only in OnDestroy, so a
 * pooled object like Scr Heads 2 is unsubscribed until its first cast and subscribed forever after.
 * Installing all of them up front is observationally the same: delivery is world_send_event_to_go,
 * which skips FSMs that are not live, and an object that has never been activated has none. */
void event_register_seed(fsm_world *w)
{
    for (int32_t i = 0; i < w->sc->n_evregs; i++) {
        const evreg_def *e = &w->sc->evregs[i];
        if (e->go < 0 || e->name < 0) continue;
        HKSIM_ASSERT(w->n_ev_reg < FSM_EV_REG_CAP, "EventRegister table full");
        w->ev_reg[w->n_ev_reg].go = e->go;
        w->ev_reg[w->n_ev_reg].name = e->name;
        w->n_ev_reg++;
    }
}

/* HKSimTestConsumeEvent — test-only action (not in HK): FsmStateAction.Event override returning `consume`
 * for `eventName`, used by tests/test_fsm.py to verify FsmState.OnEvent's last-action rule. */
typedef struct { const fsm_pv *eventName, *consume, *hits; } st_tce;
static void tce_bind(act_inst *a) { ST(st_tce); s->eventName = FIELD(eventName); s->consume = FIELD(consume); s->hits = FIELD(hits); }
static bool tce_event(act_inst *a, int32_t ev)
{
    ST(st_tce);
    if (strcmp(w_str(w, ev), w_str(w, ps(f, s->eventName))) != 0) return false;
    pi_set(f, s->hits, pi(f, s->hits) + 1);
    return pb(f, s->consume);
}
static const act_vtable AV_HKSimTestConsumeEvent = { "HKSimTestConsumeEvent", sizeof(st_tce), tce_bind, NULL, NULL, NULL, NULL, NULL, tce_event, NULL };

/* SendEventByScale — ACT/SendEventByScale.cs:19-38: sign of the owner's (lossy | local) x|y scale -> positive/negative event */
typedef struct { const fsm_pv *go, *eventTarget, *xScale, *positiveEvent, *negativeEvent, *space; } st_sebs;
static void sebs_bind(act_inst *a) { ST(st_sebs); s->go = FIELD(gameObject); s->eventTarget = FIELD(eventTarget); s->xScale = FIELD(xScale); s->positiveEvent = FIELD_OPT(positiveEvent); s->negativeEvent = FIELD_OPT(negativeEvent); s->space = FIELD(space); }
static void sebs_enter(act_inst *a)
{
    ST(st_sebs);
    int32_t t = p_owner_default(a, s->go);
    if (t < 0) return;                                             /* null owner: no Finish */
    float v[3];
    if (pi(f, s->space) == 0) go_lossy_scale(w, t, v); else go_local_scale(w, t, v);   /* Space.World 0 / Self 1 */
    float num = pb(f, s->xScale) ? v[0] : v[1];
    int32_t ev = num > 0.0f ? EV(s->positiveEvent) : EV(s->negativeEvent);
    if (ev >= 0) fsm_event_to(f, a, act_event_target(a, s->eventTarget), ev);
    act_finish(a);
}
static const act_vtable AV_SendEventByScale = { "SendEventByScale", sizeof(st_sebs), sebs_bind, sebs_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* CheckSendEventLimit — HK/CheckSendEventLimit.cs:19-37 with HK/LimitSendEvents.cs (sentList per owner, cleared on enable and when
 * the monitored collider's enabled flag flips — lifecycle.c LCT_LSE) */
typedef struct { const fsm_pv *go, *target, *trueEvent, *falseEvent; } st_csel;
static void csel_bind(act_inst *a) { ST(st_csel); s->go = FIELD(gameObject); s->target = FIELD(target); s->trueEvent = FIELD_OPT(trueEvent); s->falseEvent = FIELD_OPT(falseEvent); }
static bool lse_add(fsm_world *w, int32_t owner, int32_t obj)     /* LimitSendEvents.Add :30-38 */
{
    go_lse *l = w->gos[owner].lse;
    if (!l) return true;                                           /* no LimitSendEvents component: caller checked, but be explicit */
    for (int32_t i = 0; i < l->n; i++) if (l->sent[i] == obj) return false;
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 8; l->sent = realloc(l->sent, sizeof(int32_t) * (size_t)l->cap); }
    HKSIM_ASSERT(l->sent != NULL, "out of memory in LimitSendEvents.Add");
    l->sent[l->n++] = obj;
    return true;
}
static void csel_enter(act_inst *a)
{
    ST(st_csel);
    int32_t obj = pgo(f, s->go);
    if (obj >= 0) {
        int32_t tgt = act_event_target(a, s->target);
        bool has = go_has_component(w, f->go, "LimitSendEvents");
        int32_t ev = (has && !lse_add(w, f->go, obj)) ? EV(s->falseEvent) : EV(s->trueEvent);
        if (ev >= 0) fsm_event_to(f, a, tgt, ev);
    }
    act_finish(a);
}
static const act_vtable AV_CheckSendEventLimit = { "CheckSendEventLimit", sizeof(st_csel), csel_bind, csel_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* GetEventSender - ACT/GetEventSender.cs:19-27: Fsm.EventData.SentByFsm.GameObject (w->ev_sent_by_fsm), or
 * null when the current EventData has no SentByFsm. */
typedef struct { const fsm_pv *sentByGameObject; } st_ges;
static void ges_bind(act_inst *a) { ST(st_ges); s->sentByGameObject = FIELD(sentByGameObject); }
static void ges_enter(act_inst *a) {
    ST(st_ges);
    int32_t sender_fsm = w->ev_sent_by_fsm;
    int32_t go = (sender_fsm >= 0 && sender_fsm < w->n_fsms) ? fsm_owner_go(&w->fsms[sender_fsm]) : -1;
    pgo_set(f, s->sentByGameObject, go);
    act_finish(a);
}
static const act_vtable AV_GetEventSender = { "GetEventSender", sizeof(st_ges), ges_bind, ges_enter, NULL, NULL, NULL, NULL, NULL, NULL };

const act_vtable *const act_registry_events[] = {
    &AV_SendEventByName, &AV_SendEvent, &AV_SendRandomEvent, &AV_SendRandomEventV2, &AV_SendRandomEventV3,
    &AV_SendEventToGameObjectOptimized, &AV_SendEventToRegister, &AV_AddEventRegister,
    &AV_HKSimTestConsumeEvent, &AV_SendEventByNameV2, &AV_SendEventByScale, &AV_CheckSendEventLimit,
    &AV_GetEventSender,
};
const int act_registry_events_n = (int)(sizeof act_registry_events / sizeof act_registry_events[0]);
