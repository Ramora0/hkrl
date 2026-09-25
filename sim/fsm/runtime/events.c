/* Strings, FsmEvent interning, the trace event log and event fan-out: PM/FsmEvent.cs, PM/Fsm.cs
 * (Event / BroadcastEvent), HK/FSMUtility.cs. */
#include "fsm/fsm.h"
#include "world_internal.h"
#include "core/str_hash.h"
#include <stdlib.h>
#include <string.h>
#include "core/alloc.h"

/* ---- strings & events ---- */
const char *w_str(const fsm_world *w, int32_t id)
{
    if (id < 0) return "";
    if (id < w->sc->n_strings) return w->sc->strings[id];
    id -= w->sc->n_strings;
    if (id < w->n_dyn) return w->dyn_strings[id];
    return "";
}

/* The lowest id holding `s`: the scene's strings are distinct, and w_intern appends only unknown ones. */
int32_t w_find_string(const fsm_world *w, const char *s)
{
    uint32_t h = hks_str_hash(s), mask;
    const int32_t *ix = world_string_index(w, &mask);
    for (uint32_t k = h & mask; ix[k] >= 0; k = (k + 1) & mask)
        if (strcmp(w->sc->strings[ix[k]], s) == 0) return ix[k];
    if (w->dyn_ix)
        for (uint32_t k = h & w->dyn_mask; w->dyn_ix[k] >= 0; k = (k + 1) & w->dyn_mask)
            if (strcmp(w->dyn_strings[w->dyn_ix[k]], s) == 0) return w->sc->n_strings + w->dyn_ix[k];
    return -1;
}

/* Open addressing over dyn_strings, at most half full. */
static void dyn_index_add(fsm_world *w, int32_t i)
{
    if (2u * (uint32_t)(w->n_dyn + 1) > w->dyn_mask + 1u || !w->dyn_ix) {
        uint32_t size = 64;
        while (size < 2u * (uint32_t)(w->n_dyn + 1)) size *= 2;
        free(w->dyn_ix);
        w->dyn_ix = malloc(sizeof(int32_t) * size);
        HKSIM_ASSERT(w->dyn_ix != NULL, "out of memory indexing the runtime strings");
        memset(w->dyn_ix, 0xff, sizeof(int32_t) * size);
        w->dyn_mask = size - 1;
        for (int32_t j = 0; j < i; j++) {
            uint32_t k = hks_str_hash(w->dyn_strings[j]) & w->dyn_mask;
            while (w->dyn_ix[k] >= 0) k = (k + 1) & w->dyn_mask;
            w->dyn_ix[k] = j;
        }
    }
    uint32_t k = hks_str_hash(w->dyn_strings[i]) & w->dyn_mask;
    while (w->dyn_ix[k] >= 0) k = (k + 1) & w->dyn_mask;
    w->dyn_ix[k] = i;
}

int32_t w_intern(fsm_world *w, const char *s)
{
    int32_t id = w_find_string(w, s);
    if (id >= 0) return id;
    if (w->n_dyn == w->cap_dyn) {
        w->cap_dyn = w->cap_dyn ? w->cap_dyn * 2 : 64;
        w->dyn_strings = realloc(w->dyn_strings, sizeof(char *) * (size_t)w->cap_dyn);
    }
    w->dyn_strings[w->n_dyn] = strdup(s);
    dyn_index_add(w, w->n_dyn);
    return w->sc->n_strings + w->n_dyn++;
}

static void ensure_event_bits(fsm_world *w, int32_t id)
{
    if (id >= w->cap_event_registered) {
        int32_t nc = w->cap_event_registered ? w->cap_event_registered : 1024;
        while (nc <= id) nc *= 2;
        w->event_registered = realloc(w->event_registered, (size_t)nc);
        memset(w->event_registered + w->cap_event_registered, 0, (size_t)(nc - w->cap_event_registered));
        w->cap_event_registered = nc;
    }
}
bool w_event_registered(const fsm_world *w, int32_t name_id)
{
    return name_id >= 0 && name_id < w->cap_event_registered && w->event_registered[name_id];
}
/* FsmEvent.GetFsmEvent(string) — FsmEvent.cs:402-413: returns the interned instance, creating it. */
int32_t w_get_fsm_event_id(fsm_world *w, int32_t name_id)
{
    if (name_id < 0) return -1;
    ensure_event_bits(w, name_id);
    w->event_registered[name_id] = 1;
    return name_id;
}
int32_t w_get_fsm_event(fsm_world *w, const char *name)
{
    if (!name || !name[0]) return -1;                              /* IsNullOrEmpty -> nothing */
    return w_get_fsm_event_id(w, w_intern(w, name));
}
/* FsmEvent.FindEvent — FsmEvent.cs:383-390: null when never registered (HK/FSMUtility.cs:158 uses it). */
int32_t w_find_event(fsm_world *w, const char *name)
{
    int32_t id = w_find_string(w, name);
    return w_event_registered(w, id) ? id : -1;
}

/* ---- log ---- */
void world_log(fsm_world *w, uint8_t kind, int32_t fsm, int32_t a, int32_t b)
{
    if (!w->log_enabled) return;
    if (w->n_log == w->cap_log) {
        w->cap_log = w->cap_log ? w->cap_log * 2 : 1024;
        w->log = realloc(w->log, sizeof(log_rec) * (size_t)w->cap_log);
    }
    log_rec *r = &w->log[w->n_log++];
    r->kind = kind; r->fsm = fsm; r->a = a; r->b = b; r->c = 0; r->frame = w->frame;
    /* Which player-loop stage raised this event (trace phase: 0 Update, 1 FixedUpdate, 2 LateUpdate). */
    r->phase = w->phase == 0 ? 1 : (w->phase == 2 ? 2 : 0);
}
void world_log4(fsm_world *w, uint8_t kind, int32_t fsm, int32_t a, int32_t b, int32_t c)
{
    world_log(w, kind, fsm, a, b);
    if (w->n_log > 0) w->log[w->n_log - 1].c = c;
}

/* ---- event fan-out ---- */
/* PlayMakerFSM.FsmList membership: a component ADDS itself in OnEnable (PlayMakerFSM.cs:363-366) and
 * REMOVES itself in OnDisable (:392-398), so the list holds exactly those FSMs whose component is
 * enabled and whose GameObject is active in the hierarchy.  w->fsm_list is append-only (the FSMs live
 * at init), so it is filtered here.  Every PlayMaker consumer copies the list before iterating
 * (`new List<PlayMakerFSM>(FsmList)`, Fsm.cs:2225,2287), so membership is frozen at the start of the
 * broadcast: the filter belongs in the snapshot, not in the delivery loop. */
static int32_t fsmlist_snapshot(fsm_world *w, int32_t *out)
{
    int32_t m = 0;
    for (int32_t i = 0; i < w->n_fsm_list; i++) {
        fsm_inst *f = &w->fsms[w->fsm_list[i]];
        if (f->component_enabled && go_active_in_hierarchy(w, f->go)) out[m++] = f->id;
    }
    return m;
}
/* Fsm.BroadcastEvent — Fsm.cs:2222-2232: snapshot of PlayMakerFSM.FsmList */
void world_broadcast_event(fsm_world *w, int32_t ev, int32_t from_fsm, bool exclude_self, bool with_data, int32_t sent_by)
{
    int32_t *snap = malloc(sizeof(int32_t) * (size_t)(w->n_fsm_list > 0 ? w->n_fsm_list : 1));
    int32_t n = fsmlist_snapshot(w, snap);
    for (int32_t i = 0; i < n; i++) {
        fsm_inst *f = &w->fsms[snap[i]];
        if (!exclude_self || f->id != from_fsm) fsm_process_event(f, ev, with_data, sent_by);
    }
    free(snap);
}
/* Fsm.BroadcastEventToGameObject — Fsm.cs:2242-2270: FSMs on go in FsmList order, then children depth-first */
void world_broadcast_to_go(fsm_world *w, int32_t go, int32_t ev, bool send_to_children, bool exclude_self, int32_t from_fsm, int32_t sent_by)
{
    if (go < 0) return;
    int32_t *list = malloc(sizeof(int32_t) * (size_t)(w->n_fsm_list > 0 ? w->n_fsm_list : 1));
    int32_t n = fsmlist_snapshot(w, list), m = 0;
    for (int32_t i = 0; i < n; i++) if (w->fsms[list[i]].go == go) list[m++] = list[i];
    for (int32_t i = 0; i < m; i++) {
        if (!exclude_self || list[i] != from_fsm) fsm_process_event(&w->fsms[list[i]], ev, true, sent_by);
    }
    free(list);
    if (send_to_children) {
        for (int32_t c = w->gos[go].first_child; c >= 0; c = w->gos[c].next_sibling)
            world_broadcast_to_go(w, c, ev, true, exclude_self, from_fsm, sent_by);
    }
}
/* Fsm.SendEventToFsmOnGameObject — Fsm.cs:2280-2307 */
void world_send_to_fsm_on_go(fsm_world *w, int32_t go, int32_t fsm_name_sid, int32_t ev, int32_t from_fsm)
{
    (void)from_fsm;
    if (go < 0) return;
    w->ev_sent_by_fsm = exec_fsm(w);                               /* SetEventDataSentByInfo :2286 */
    int32_t *snap = malloc(sizeof(int32_t) * (size_t)(w->n_fsm_list > 0 ? w->n_fsm_list : 1));
    int32_t n = fsmlist_snapshot(w, snap);
    const char *name = w_str(w, fsm_name_sid);
    if (!name[0]) {
        for (int32_t i = 0; i < n; i++) if (w->fsms[snap[i]].go == go) fsm_process_event(&w->fsms[snap[i]], ev, false, 0);
    } else {
        for (int32_t i = 0; i < n; i++) {
            fsm_inst *f = &w->fsms[snap[i]];
            if (f->go == go && strcmp(w_str(w, f->def->fsm_name), name) == 0) { fsm_process_event(f, ev, false, 0); break; }
        }
    }
    free(snap);
}
/* FSMUtility.SendEventToGameObject(go, string, recursive) — HK/FSMUtility.cs:155-183:
 * FindEvent (null when unregistered), go.GetComponents<PlayMakerFSM>() in component order, Fsm.Event(ev) each */
void world_send_event_to_go(fsm_world *w, int32_t go, const char *ev_name, bool recursive)
{
    if (go < 0) return;
    int32_t ev = w_find_event(w, ev_name);
    const go_def *g = w->gos[go].def;
    int32_t n = g->n_fsms;
    int32_t *snap = malloc(sizeof(int32_t) * (size_t)(n > 0 ? n : 1));
    for (int32_t k = 0; k < n; k++) snap[k] = w->sc->fsm_idx[g->fsm_start + k];
    for (int32_t k = 0; k < n; k++) {
        fsm_inst *f = &w->fsms[snap[k]];
        fsm_event(f, ev);                                          /* Fsm.Event(FsmEvent): null -> no-op */
    }
    free(snap);
    if (recursive) {
        for (int32_t c = w->gos[go].first_child; c >= 0; c = w->gos[c].next_sibling)
            world_send_event_to_go(w, c, ev_name, true);
    }
}
