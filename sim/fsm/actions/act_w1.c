/* TrackTriggerObjects.cs: a trigger-overlap counter added to a GameObject at runtime by AddTrackTrigger and
 * read back by CheckTrackTriggerCount.  State lives on go_inst (fsm.h: tt_present/tt_inside), because it is
 * per-object like a Collider2D, not a PlayMaker action's own private state. */
#include "act.h"

/* TrackTriggerObjects.OnTriggerEnter2D/Exit2D -- TrackTriggerObjects.cs:164-176 (insideGameObjects add/remove,
 * de-duplicated by Contains).  Dispatched from sim/fsm/runtime/physics.c's world_phys_event for every trigger
 * pair touching a `tt_present` object.  Not modelled: the ignoreLayers mask (:177-181) and the OnEnable
 * GetOverlappedColliders() scan of objects already inside when the component is added (:47-52, :96-141) --
 * there is no physics overlap-query primitive in this port, so only objects that cross the trigger boundary
 * after AddTrackTrigger ran are counted. */
void tt_trigger_event(fsm_world *w, int32_t go, int32_t other, bool enter)
{
    if (other < 0) return;
    go_inst *g = &w->gos[go];
    if (enter) {
        for (int32_t i = 0; i < g->n_tt_inside; i++) if (g->tt_inside[i] == other) return;    /* :168 Contains */
        if (g->n_tt_inside >= g->cap_tt_inside) {
            g->cap_tt_inside = g->cap_tt_inside ? g->cap_tt_inside * 2 : 4;
            g->tt_inside = realloc(g->tt_inside, sizeof(int32_t) * (size_t)g->cap_tt_inside);
        }
        g->tt_inside[g->n_tt_inside++] = other;                                               /* :171 Add */
    } else {
        for (int32_t i = 0; i < g->n_tt_inside; i++) {
            if (g->tt_inside[i] == other) { g->tt_inside[i] = g->tt_inside[--g->n_tt_inside]; return; }  /* :175 Remove */
        }
    }
}

/* AddTrackTrigger -- AddTrackTrigger.cs:9-22: `!safe.GetComponent<TrackTriggerObjects>()` gates the add, so a
 * second AddTrackTrigger on the same object is a no-op (matches tt_present's idempotent set).  `skipIfPresent`
 * is a Reset()-only default the OnEnter body never reads (:11,:19-21 -- the presence check runs unconditionally). */
typedef struct { const fsm_pv *target; } st_att;
static void att_bind(act_inst *a) { ST(st_att); s->target = FIELD(target); }
static void att_enter(act_inst *a)
{
    ST(st_att);
    int32_t t = p_get_safe(a, s->target);
    if (t >= 0) w->gos[t].tt_present = 1;
    act_finish(a);
}
static const act_vtable AV_AddTrackTrigger = { "AddTrackTrigger", sizeof(st_att), att_bind, att_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* CheckTrackTriggerCount -- CheckTrackTriggerCount.cs:44-90.  OnEnter always Finish()es except when a
 * TrackTriggerObjects is present, the count test is currently false, and everyFrame is set (:58-64): then the
 * action stays alive and CheckCount() runs from OnFixedUpdate (:70-75, HandleFixedUpdate via OnPreprocess :36-39). */
typedef struct { const fsm_pv *target, *count, *test, *everyFrame, *successEvent; int32_t track_go; } st_cttc;
static void cttc_bind(act_inst *a)
{
    ST(st_cttc);
    s->target = FIELD(target); s->count = FIELD(count); s->test = FIELD(test);
    s->everyFrame = a_field(a, "everyFrame"); s->successEvent = FIELD_OPT(successEvent);
}
static bool cttc_check(act_inst *a, int32_t track_go)
{
    ST(st_cttc);
    if (track_go < 0) return false;
    int32_t n = w->gos[track_go].n_tt_inside, c = pi(f, s->count);
    switch (pi(f, s->test)) {                                       /* IntTest (:80-89) */
    case 0: return n == c;
    case 1: return n < c;
    case 2: return n > c;
    case 3: return n <= c;
    case 4: return n >= c;
    }
    return false;
}
static void cttc_enter(act_inst *a)
{
    ST(st_cttc);
    s->track_go = -1;
    int32_t t = p_get_safe(a, s->target);
    if (t >= 0 && w->gos[t].tt_present) {                            /* :57 track = safe.GetComponent<...>() */
        if (cttc_check(a, t)) { fsm_event(f, EV(s->successEvent)); }  /* :63 */
        else if (s->everyFrame && pb(f, s->everyFrame)) { s->track_go = t; return; }   /* :60-61 stay alive */
    }                                                                 /* t>=0 && !tt_present: Debug.LogError, :67 -- no state change */
    act_finish(a);
}
static void cttc_fixed_update(act_inst *a)
{
    ST(st_cttc);
    if (s->track_go >= 0 && cttc_check(a, s->track_go)) fsm_event(f, EV(s->successEvent));   /* :72-74 */
}
static const act_vtable AV_CheckTrackTriggerCount = { "CheckTrackTriggerCount", sizeof(st_cttc), cttc_bind, cttc_enter, NULL, cttc_fixed_update, NULL, NULL, NULL, NULL };

const act_vtable *const act_registry_w1[] = {
    &AV_AddTrackTrigger, &AV_CheckTrackTriggerCount,
};
const int act_registry_w1_n = (int)(sizeof act_registry_w1 / sizeof act_registry_w1[0]);
