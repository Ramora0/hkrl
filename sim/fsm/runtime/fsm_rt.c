/* PlayMaker Fsm / FsmState / FsmStateAction runtime — port of
 *   analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs, FsmState.cs, FsmStateAction.cs, DelayedEvent.cs,
 *   FsmExecutionStack.cs and analysis/decomp/PlayMaker/PlayMakerFSM.cs.
 * Editor-only branches (breakpoints, logging, IsEditor) are hard-coded off: fsm-runtime.md §2.7. */
#include "fsm/fsm.h"
#include "fsm/lifecycle.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "core/alloc.h"   /* per-instance arena */
#include "core/tls.h"

/* Pure formatting scratch, returned to the caller, so it is per thread rather than per world:
 * a world is only ever stepped by one thread, but two worlds can format a label at the same time. */
static hks_tls_key label_key;
HKS_CTOR hks_label_ctor(void) { label_key = HKS_TLS_NEW(); }
static char *label_buf(void)
{
    char *b = (char *)HKS_TLS_GET(label_key);
    /* the process heap, not the arena: the buffer outlives the instance that first formats a label */
    if (!b) { b = (char *)hks_sys_calloc(1, 256); HKS_TLS_SET(label_key, b); }
    return b;
}
const char *fsm_label(const fsm_inst *f)
{
    char *b = label_buf();
    snprintf(b, 256, "%s/%s", w_str(f->w, f->def->go_name), w_str(f->w, f->def->fsm_name));
    return b;
}

const char *state_name(const fsm_inst *f, int32_t s)
{
    if (s < 0 || s >= f->n_states) return "";
    return w_str(f->w, f->states[s].def->name);
}

int32_t fsm_owner_go(const fsm_inst *f) { return f->go; }

/* ---- FsmExecutionStack (PM/FsmExecutionStack.cs:9-73) ---- */
int32_t exec_fsm(const fsm_world *w) { return w->stack_depth > 0 ? w->stack[w->stack_depth - 1] : -1; }
void push_fsm(fsm_world *w, int32_t id)
{
    HKSIM_ASSERT(w->stack_depth < 256, "FsmExecutionStack overflow (256, FsmExecutionStack.cs:7)");
    w->stack[w->stack_depth++] = id;
}
void pop_fsm(fsm_world *w) { HKSIM_ASSERT(w->stack_depth > 0, "FsmExecutionStack underflow"); w->stack_depth--; }

/* Fsm.SetEventDataSentByInfo() — Fsm.cs:2082-2087: EventData.SentByFsm = ExecutingFsm */
static void set_event_data_sent_by(fsm_world *w) { w->ev_sent_by_fsm = exec_fsm(w); }

/* ---- properties ---- */
/* Fsm.Active — Fsm.cs:546-556: owner != null && owner.gameObject != null && !Finished && ActiveState != null */
bool fsm_is_active(const fsm_inst *f) { return !f->finished && f->active_state >= 0; }
bool fsm_is_switching(const fsm_inst *f) { return f->switch_to >= 0; }   /* Fsm.cs:560 */
/* Fsm.MaxLoopCount — Fsm.cs:595-605 (maxLoopCount <= 0 -> 1000) */
static int32_t max_loop_count(const fsm_inst *f) { return f->def->max_loop_count <= 0 ? 1000 : f->def->max_loop_count; }

/* ---- FsmStateAction.Finish — FsmStateAction.cs:181-189 ---- */
void act_finish(act_inst *a)
{
    if (!a->finished) {
        a->active = 0;
        a->finished = 1;
        state_inst *s = a->state;                                  /* State.FinishAction(this) FsmState.cs:596-598 */
        s->finished_actions = realloc(s->finished_actions, sizeof(int32_t) * (size_t)(s->n_finished + 1));
        s->finished_actions[s->n_finished++] = a->index;
    }
}

static void act_set_finished(act_inst *a, bool v)              /* FsmStateAction.Finished setter :124-138 */
{
    if (v) a->active = 0;
    a->finished = v ? 1 : 0;
}

static void act_trap_unported(act_inst *a, const char *hook)
{
    fsm_inst *f = a->fsm;
    /* the FULL path, not the bare go_name: 9 objects in GG_Hornet_1 share the name "Hit L", and pooled
     * clones repeat their children's names, so a bare name cannot be resolved back to an object. */
    HKSIM_UNIMPLEMENTED("UNIMPLEMENTED FsmStateAction %s (%s) in %s | %s state '%s' action %d (go active=%d)",
                        w_str(f->w, a->def->type), hook, go_path(f->w, f->go), w_str(f->w, f->def->fsm_name),
                        w_str(f->w, a->state->def->name), a->index, (int)go_active_in_hierarchy(f->w, f->go));
}

static void act_on_enter(act_inst *a)
{
    if (!a->vt) act_trap_unported(a, "OnEnter");
    if (a->vt->on_enter) a->vt->on_enter(a);
}
static void act_on_update(act_inst *a)        { if (!a->vt) act_trap_unported(a, "OnUpdate");      if (a->vt->on_update) a->vt->on_update(a); }
static void act_on_fixed_update(act_inst *a)  { if (!a->vt) act_trap_unported(a, "OnFixedUpdate"); if (a->vt->on_fixed_update) a->vt->on_fixed_update(a); }
static void act_on_late_update(act_inst *a)   { if (!a->vt) act_trap_unported(a, "OnLateUpdate");  if (a->vt->on_late_update) a->vt->on_late_update(a); }
static void act_on_exit(act_inst *a)          { if (!a->vt) act_trap_unported(a, "OnExit");        if (a->vt->on_exit) a->vt->on_exit(a); }
static bool act_on_event(act_inst *a, int32_t ev)
{
    if (!a->vt) act_trap_unported(a, "Event");
    return a->vt->on_event ? a->vt->on_event(a, ev) : false;        /* base FsmStateAction.Event returns false :176-179 */
}

/* ---- FsmState ---- */
static void state_remove_finished_actions(state_inst *s)      /* FsmState.cs:600-607 */
{
    for (int32_t i = 0; i < s->n_finished; i++) {
        int32_t k = s->finished_actions[i];
        for (int32_t j = 0; j < s->n_active; j++) {              /* List.Remove: first occurrence */
            if (s->active_actions[j] == k) {
                memmove(&s->active_actions[j], &s->active_actions[j + 1], sizeof(int32_t) * (size_t)(s->n_active - j - 1));
                s->n_active--;
                break;
            }
        }
    }
    s->n_finished = 0;
}

static bool state_activate_actions(state_inst *s, int32_t start_index);
static void state_check_all_actions_finished(state_inst *s);

/* FsmState.ActivateActions — FsmState.cs:286-317 */
static bool state_activate_actions(state_inst *s, int32_t start_index)
{
    for (int32_t i = start_index; i < s->n_acts; i++) {
        s->active_action_index = i;
        act_inst *a = &s->acts[i];
        if (!a->enabled) {                                         /* :292-296 disabled: Finished=true, no OnEnter */
            act_set_finished(a, true);
            continue;
        }
        s->active_action = i;
        a->active = 1;
        act_set_finished(a, false);
        a->entered = 1;                                            /* :301 sticky (never reset anywhere) */
        act_on_enter(a);
        if (!a->finished) {                                        /* :303-306 */
            s->active_actions = realloc(s->active_actions, sizeof(int32_t) * (size_t)(s->n_active + 1));
            s->active_actions[s->n_active++] = i;
        }
        if (fsm_is_switching(s->fsm)) return false;                /* :307-310 abort on pending transition */
        if (!a->finished && s->def->is_sequence) return false;     /* :311-314 */
    }
    return true;
}

/* FsmState.OnEnter — FsmState.cs:267-284 */
static void state_on_enter(state_inst *s)
{
    s->loop_count++;
    if (s->loop_count > s->max_loop_count_seen) s->max_loop_count_seen = s->loop_count;
    s->active = 1;
    s->finished = 0;
    s->n_finished = 0;                                             /* finishedActions.Clear() */
    s->state_time = 0.0f;                                          /* :277 (RealStartTime :276 is editor/realtime only) */
    s->n_active = 0;                                               /* ActiveActions.Clear() :279 */
    if (state_activate_actions(s, 0)) state_check_all_actions_finished(s);
}

/* FsmState.CheckAllActionsFinished — FsmState.cs:609-620 */
static void state_check_all_actions_finished(state_inst *s)
{
    fsm_inst *f = s->fsm;
    if (!s->finished && s->active && !fsm_is_switching(f)) {
        state_remove_finished_actions(s);
        if (s->n_active == 0 &&
            (!s->def->is_sequence || ++s->active_action_index >= s->n_acts || state_activate_actions(s, s->active_action_index))) {
            s->finished = 1;
            fsm_event(f, w_get_fsm_event(f->w, "FINISHED"));       /* FsmEvent.Finished (FsmEvent.cs:442) */
        }
    }
}

/* FsmState.OnEvent — FsmState.cs:319-329.  NOTE: `flag = action.Event(e)` is an assignment: only the
 * LAST active action's answer survives (fsm-runtime.md §2.2). */
static bool state_on_event(state_inst *s, int32_t ev)
{
    bool flag = false;
    for (int32_t i = 0; i < s->n_active; i++) {
        act_inst *a = &s->acts[s->active_actions[i]];
        flag = act_on_event(a, ev);
    }
    return fsm_is_switching(s->fsm) || flag;
}

/* FsmState.OnUpdate — FsmState.cs:342-355 */
static void state_on_update(state_inst *s)
{
    if (!s->finished) {
        s->state_time += s->fsm->w->dt;                            /* :346 Time.deltaTime */
        for (int32_t i = 0; i < s->n_active; i++)                  /* live count, forward index */
            act_on_update(&s->acts[s->active_actions[i]]);
        state_check_all_actions_finished(s);
    }
}
/* FsmState.OnFixedUpdate — :331-340 (no finished latch, no StateTime) */
static void state_on_fixed_update(state_inst *s)
{
    for (int32_t i = 0; i < s->n_active; i++) act_on_fixed_update(&s->acts[s->active_actions[i]]);
    state_check_all_actions_finished(s);
}
/* FsmState.OnLateUpdate — :357-366 */
static void state_on_late_update(state_inst *s)
{
    for (int32_t i = 0; i < s->n_active; i++) act_on_late_update(&s->acts[s->active_actions[i]]);
    state_check_all_actions_finished(s);
}
/* FsmState.OnExit — FsmState.cs:622-636: every action with Entered==true (sticky) */
static void state_on_exit(state_inst *s)
{
    s->active = 0;
    s->finished = 0;
    for (int32_t i = 0; i < s->n_acts; i++) {
        act_inst *a = &s->acts[i];
        if (a->entered) {
            s->active_action = i;
            act_on_exit(a);
        }
    }
}

/* ---- delayed events (Fsm.cs:2200-2212, :1924-1943, DelayedEvent.cs:68-88) ----
 * The live array is compacted and realloc'd, so the creating action holds a stable id (delayed_ev.id), not a
 * pointer or index: like the game's per-object DelayedEvent, WasSent(handle) never aliases another event. */
void fsm_add_delayed(fsm_inst *f, act_inst *owner, int32_t target_arena, int32_t ev, float delay, int32_t *out_id)
{
    if (f->n_delayed == f->cap_delayed) {
        f->cap_delayed = f->cap_delayed ? f->cap_delayed * 2 : 4;
        f->delayed = realloc(f->delayed, sizeof(delayed_ev) * (size_t)f->cap_delayed);
    }
    delayed_ev *d = &f->delayed[f->n_delayed++];
    d->id = ++f->next_delayed_id;                                  /* fsm.h: pre-incremented, 0 never issued */
    d->event = ev; d->target = target_arena; d->owner = owner;
    d->timer = delay; d->delay = delay; d->fired = 0;              /* DelayedEvent ctor :24-36 */
    /* FsmEventData copy ctor: SentByFsm is overwritten with the executing FSM (:30-35); Int/Float/String
     * are copied as-is from the CURRENT global EventData. */
    d->sent_by_fsm = exec_fsm(f->w);
    d->ev_int = f->w->ev_int; d->ev_float = f->w->ev_float; d->ev_string = f->w->ev_string;
    if (out_id) *out_id = d->id;
}
bool delayed_was_sent(const fsm_inst *f, int32_t id)               /* :95-98 (WasSent(null) -> true) */
{
    if (id == 0 || f == NULL) return true;                         /* 0 = handle never set, same as a null DelayedEvent ref */
    for (int32_t i = 0; i < f->n_delayed; i++) if (f->delayed[i].id == id) return f->delayed[i].fired != 0;
    return true;   /* fired-and-compacted-out below: Finished latches true forever, so "gone" reads the same as "fired" */
}

static void fsm_kill_delayed_events(fsm_inst *f) { f->n_delayed = 0; }   /* Fsm.cs:1329-1332 */

/* Fsm.UpdateDelayedEvents — Fsm.cs:1924-1943 over a snapshot; DelayedEvent.Update :68-88 */
static void fsm_update_delayed_events(fsm_inst *f)
{
    int32_t n = f->n_delayed;
    if (n == 0) return;
    delayed_ev *snap = malloc(sizeof(delayed_ev) * (size_t)n);
    memcpy(snap, f->delayed, sizeof(delayed_ev) * (size_t)n);
    for (int32_t i = 0; i < n; i++) {
        delayed_ev *d = &snap[i];
        d->timer -= f->w->dt;
        if (d->timer < 0.0f) {
            int32_t saved_sender = f->w->ev_sent_by_fsm;           /* Fsm.EventData swap :72-73 (full captured subset) */
            int32_t saved_int = f->w->ev_int; float saved_float = f->w->ev_float; int32_t saved_string = f->w->ev_string;
            f->w->ev_sent_by_fsm = d->sent_by_fsm;
            f->w->ev_int = d->ev_int; f->w->ev_float = d->ev_float; f->w->ev_string = d->ev_string;
            if (d->target < 0) fsm_event(f, d->event);
            else fsm_event_to(f, d->owner, d->target, d->event);
            fsm_update_state_changes(f);
            d->fired = 1;
            f->w->ev_sent_by_fsm = saved_sender;
            f->w->ev_int = saved_int; f->w->ev_float = saved_float; f->w->ev_string = saved_string;
        }
        /* write back timer/fired into the live entry BY STABLE ID: firing may append (and realloc) or wipe the
         * array (KillDelayedEvents on a state exit); a vanished id has nothing to write back to, as in the game. */
        for (int32_t j = 0; j < f->n_delayed; j++) {
            if (f->delayed[j].id == d->id) { f->delayed[j].timer = d->timer; f->delayed[j].fired = d->fired; break; }
        }
    }
    /* removeEvents: every finished entry */
    int32_t k = 0;
    for (int32_t j = 0; j < f->n_delayed; j++) if (!f->delayed[j].fired) f->delayed[k++] = f->delayed[j];
    f->n_delayed = k;
    free(snap);
}

/* ---- state switching ---- */
static void fsm_enter_state(fsm_inst *f, int32_t s);
static void fsm_exit_state(fsm_inst *f, int32_t s);

/* Fsm.UpdateStateChanges — Fsm.cs:2314-2325 */
void fsm_update_state_changes(fsm_inst *f)
{
    while (fsm_is_switching(f)) fsm_switch_state(f, f->switch_to);
    for (int32_t i = 0; i < f->n_states; i++) f->states[i].loop_count = 0;   /* ResetLoopCount FsmState.cs:638-641 */
    f->event_target = -1; f->event_target_owner = NULL;                     /* EventTarget = null :2324 */
}

/* Fsm.SwitchState — Fsm.cs:2347-2365; FSM_TRANSITION is logged post-orig (oracle/Oracle/TraceRecorder.cs:202-220) */
void fsm_switch_state(fsm_inst *f, int32_t to)
{
    if (to < 0) return;
    int32_t from = f->active_state;
    if (f->active_state >= 0 && f->active_state_entered) fsm_exit_state(f, f->active_state);
    f->active_state = to;
    fsm_enter_state(f, to);
    world_log(f->w, LOG_FSM_TRANSITION, f->id, from, to);
}

void fsm_goto_previous_state(fsm_inst *f)                      /* Fsm.cs:2367-2373 */
{
    if (f->previous_state >= 0) fsm_switch_state(f, f->previous_state);
}

/* Fsm.EnterState — Fsm.cs:2375-2397 */
static void fsm_enter_state(fsm_inst *f, int32_t s)
{
    f->event_target = -1; f->event_target_owner = NULL;
    f->switched_state = 1;
    f->active_state_entered = 1;
    f->switch_to = -1;
    state_inst *st = &f->states[s];
    if (st->loop_count >= max_loop_count(f)) {                     /* :2385-2390 */
        /* Owner.enabled = false: Behaviour.enabled -- OnDisable only if the object is active in the hierarchy */
        lc_fsm_set_enabled(f->w, f->id, false);
        return;
    }
    state_on_enter(st);
}

/* Fsm.ExitState — Fsm.cs:2420-2434 */
static void fsm_exit_state(fsm_inst *f, int32_t s)
{
    f->previous_state = s;
    f->active_state = -1;
    state_on_exit(&f->states[s]);
    if (!f->def->keep_delayed_on_exit) fsm_kill_delayed_events(f);   /* fsm-runtime.md §2.4 */
}

/* Fsm.DoTransition — Fsm.cs:2327-2345 */
static bool fsm_do_transition(fsm_inst *f, int32_t trans_index)
{
    const fsm_trans_def *t = &f->w->sc->trans[trans_index];
    if (t->to_state < 0) return false;                             /* :2329-2333 unresolved ToState */
    f->last_transition = trans_index;
    f->switch_to = t->to_state;
    if (f->w->ev_sent_by_fsm != f->id) fsm_update_state_changes(f);   /* :2340-2343 THE HINGE */
    return true;
}

/* ---- lifecycle ---- */
static void fsm_continue(fsm_inst *f)                          /* Fsm.Continue :2980-2986 */
{
    f->active_state_entered = 1;
    fsm_enter_state(f, f->active_state);
}

/* Fsm.Start — Fsm.cs:1863-1893 */
void fsm_start(fsm_inst *f)
{
    f->started = 1;
    f->finished = 0;
    push_fsm(f->w, f->id);
    if (f->active_state < 0) {
        f->active_state = f->def->start_state;
        f->active_state_entered = 0;
    }
    f->switch_to = f->active_state;                                /* :1885-1886 (breakpoints off) */
    fsm_update_state_changes(f);
    pop_fsm(f->w);
}

/* PlayMakerFSM.Update (PMF:369-375) + Fsm.Update (Fsm.cs:1895-1922).  Start (PMF:355-361) is run separately
 * by lifecycle.c before the first Update (R1, docs/engine-lifecycle.md). */
void fsm_update(fsm_inst *f)
{
    HKSIM_ASSERT(f->started, "fsm_update on %s before its Start: Unity runs Start before the first Update (R1)", fsm_label(f));
    if (f->finished || f->def->manual_update) return;             /* PMF:371 */
    push_fsm(f->w, f->id);
    if (!f->active_state_entered) fsm_continue(f);                 /* :1906-1909 */
    fsm_update_delayed_events(f);                                  /* :1910 */
    if (f->active_state >= 0) {                                    /* UpdateState :2406-2411 */
        state_on_update(&f->states[f->active_state]);
        fsm_update_state_changes(f);
    }
    pop_fsm(f->w);
}

/* PlayMakerFixedUpdate.FixedUpdate (PM/PlayMakerFixedUpdate.cs:6-16) + Fsm.FixedUpdate (Fsm.cs:1950-1958) */
void fsm_fixed_update(fsm_inst *f)
{
    if (!fsm_is_active(f) || !f->handle_fixed) return;
    push_fsm(f->w, f->id);
    if (f->active_state >= 0 && f->active_state_entered) {         /* FixedUpdateState :2399-2404 */
        state_on_fixed_update(&f->states[f->active_state]);
        fsm_update_state_changes(f);
    }
    pop_fsm(f->w);
}

void fsm_late_update(fsm_inst *f)                              /* PlayMakerLateUpdate + Fsm.LateUpdate :1960-1968 */
{
    if (!fsm_is_active(f) || !f->handle_late) return;
    push_fsm(f->w, f->id);
    if (f->active_state >= 0 && f->active_state_entered) {
        state_on_late_update(&f->states[f->active_state]);
        fsm_update_state_changes(f);
    }
    pop_fsm(f->w);
}

static void fsm_list_add(fsm_world *w, fsm_inst *f)
{
    if (f->in_fsm_list) return;
    if (w->n_fsm_list == w->cap_fsm_list) {
        w->cap_fsm_list = w->cap_fsm_list ? w->cap_fsm_list * 2 : 64;
        w->fsm_list = realloc(w->fsm_list, sizeof(int32_t) * (size_t)w->cap_fsm_list);
    }
    w->fsm_list[w->n_fsm_list++] = f->id;
    f->in_fsm_list = 1;
}
static void fsm_list_remove(fsm_world *w, fsm_inst *f)
{
    if (!f->in_fsm_list) return;
    for (int32_t i = 0; i < w->n_fsm_list; i++) {
        if (w->fsm_list[i] == f->id) {
            memmove(&w->fsm_list[i], &w->fsm_list[i + 1], sizeof(int32_t) * (size_t)(w->n_fsm_list - i - 1));
            w->n_fsm_list--;
            break;
        }
    }
    f->in_fsm_list = 0;
}

/* PlayMakerFSM.OnEnable (PMF:363-367) -> Fsm.OnEnable (Fsm.cs:1839-1856) */
void fsm_on_enable(fsm_inst *f)
{
    fsm_list_add(f->w, f);
    f->finished = 0;
    if (f->active_state < 0 || f->def->restart_on_enable) {
        f->active_state = f->def->start_state;
        f->active_state_entered = 0;
        if (f->started) fsm_start(f);
    }
}

/* Fsm.Stop / StopAndReset — Fsm.cs:1970-2004 */
void fsm_stop(fsm_inst *f)
{
    if (f->def->restart_on_enable) {
        push_fsm(f->w, f->id);
        if (f->active_state >= 0 && f->active_state_entered) fsm_exit_state(f, f->active_state);
        f->active_state = -1;
        f->last_transition = -1;
        f->switched_state = 0;
        pop_fsm(f->w);
    }
    f->finished = 1;
}

/* PlayMakerFSM.OnDisable — PMF:392-403 */
void fsm_on_disable(fsm_inst *f)
{
    if (f->started) fsm_event(f, w_get_fsm_event(f->w, "DISABLE"));   /* FsmEvent.Disable (FsmEvent.cs:443), FSM still live */
    fsm_list_remove(f->w, f);
    if (!f->finished) fsm_stop(f);
}

/* ---- events ---- */
/* Fsm.ProcessEvent — Fsm.cs:2023-2080 */
void fsm_process_event(fsm_inst *f, int32_t ev, bool with_data, int32_t sent_by)
{
    fsm_world *w = f->w;
    if (!fsm_is_active(f) || ev < 0) return;                       /* :2025-2028 (IsNullOrEmpty) */
    if (!f->started) fsm_start(f);                                 /* :2029-2032 */
    if (!fsm_is_active(f)) return;                                 /* :2033-2036 */
    if (with_data) w->ev_sent_by_fsm = sent_by;                    /* :2037-2040 SetEventDataSentByInfo(eventData) */
    push_fsm(w, f->id);
    if (state_on_event(&f->states[f->active_state], ev)) { pop_fsm(w); return; }   /* :2042-2046 */
    const fsm_def *d = f->def;
    for (int32_t i = 0; i < d->n_gtrans; i++) {                    /* :2047-2062 global transitions */
        int32_t ti = d->gtrans_start + i;
        if (w->sc->trans[ti].event == ev && fsm_do_transition(f, ti)) { pop_fsm(w); return; }
    }
    if (f->active_state >= 0) {                                    /* :2063-2078 state transitions */
        const fsm_state_def *sd = f->states[f->active_state].def;
        for (int32_t i = 0; i < sd->n_trans; i++) {
            int32_t ti = sd->trans_start + i;
            if (w->sc->trans[ti].event == ev && fsm_do_transition(f, ti)) { pop_fsm(w); return; }
        }
    }
    pop_fsm(w);
}

/* Fsm.Event(FsmEventTarget, FsmEvent) — Fsm.cs:2126-2182 */
void fsm_event_to(fsm_inst *f, act_inst *owner, int32_t target_arena, int32_t ev)
{
    fsm_world *w = f->w;
    if (w->snapshot_mode) return;
    set_event_data_sent_by(w);
    int target = 0;                                                /* null target -> Self (:2129-2132, targetSelf :244) */
    const fsm_pv *tp = NULL;
    if (target_arena >= 0 && owner) {
        tp = a_pv(owner, target_arena);
        HKSIM_ASSERT(tp->kind == PV_EVTARGET, "fsm_event_to: arena %d is not an FsmEventTarget", target_arena);
        target = a_pv(owner, tp->i)->i;                            /* FsmEventTarget.target enum */
    }
    switch (target) {
    case 0:                                                        /* Self */
        fsm_process_event(f, ev, false, 0);
        break;
    case 1: {                                                      /* GameObject :2142-2147 */
        int32_t go = p_owner_default(owner, a_pv(owner, tp->i + 2));
        bool send_to_children = pb(f, a_pv(owner, tp->i + 4));
        bool exclude_self = pb(f, a_pv(owner, tp->i + 1));
        world_broadcast_to_go(w, go, ev, send_to_children, exclude_self, f->id, exec_fsm(w));
        break;
    }
    case 2: {                                                      /* GameObjectFSM :2148-2153 */
        int32_t go = p_owner_default(owner, a_pv(owner, tp->i + 2));
        world_send_to_fsm_on_go(w, go, ps(f, a_pv(owner, tp->i + 3)), ev, f->id);
        break;
    }
    case 4:                                                        /* BroadcastAll :2160-2162 */
        world_broadcast_event(w, ev, f->id, pb(f, a_pv(owner, tp->i + 1)), true, exec_fsm(w));
        break;
    default:
        HKSIM_UNIMPLEMENTED("FsmEventTarget %d (FSMComponent/HostFSM/SubFSMs) in %s: not present in any dump (fsm-runtime.md §3.6)", target, fsm_label(f));
    }
    if (exec_fsm(w) != f->id) {                                    /* :2176-2181 */
        push_fsm(w, f->id);
        fsm_update_state_changes(f);
        pop_fsm(w);
    }
}

/* Fsm.Event(FsmEvent) — Fsm.cs:2192-2198; the recorder's FSM_EVENT hook sits here (pre-orig). */
void fsm_event(fsm_inst *f, int32_t ev)
{
    if (ev < 0 || f->w->snapshot_mode) return;
    world_log(f->w, LOG_FSM_EVENT, f->id, ev, f->w->stack_depth);   /* b: the FsmExecutionStack depth it is sent at (method oracle) */
    fsm_event_to(f, f->event_target_owner, f->event_target, ev);
}

void fsm_event_name(fsm_inst *f, const char *name)             /* Fsm.Event(string) :2184-2190 */
{
    if (name && name[0]) fsm_event(f, w_get_fsm_event(f->w, name));
}

/* Fsm.OnTriggerEnter2D etc. — Fsm.cs:2813-2829 (and the Stay/Exit twins); FsmState.OnTriggerEnter2D
 * calls DoTriggerEnter2D on active actions — no ported action overrides it (all use the HK proxy),
 * so the state hook reduces to RemoveFinishedActions + IsSwitchingState (FsmState.cs:539-548). */
void fsm_on_trigger2d(fsm_inst *f, int kind, int32_t other_go)
{
    (void)other_go;
    static const char *names[3] = { "TRIGGER ENTER 2D", "TRIGGER STAY 2D", "TRIGGER EXIT 2D" };
    if (!fsm_is_active(f)) return;
    push_fsm(f->w, f->id);
    state_remove_finished_actions(&f->states[f->active_state]);
    if (!fsm_is_switching(f)) fsm_event(f, w_get_fsm_event(f->w, names[kind]));
    fsm_update_state_changes(f);
    pop_fsm(f->w);
}
void fsm_on_collision2d(fsm_inst *f, int kind, int32_t other_go, int32_t other_layer)
{
    static const char *names[3] = { "COLLISION ENTER 2D", "COLLISION STAY 2D", "COLLISION EXIT 2D" };
    if (!fsm_is_active(f)) return;
    push_fsm(f->w, f->id);
    /* FsmState.OnCollisionEnter2D/Stay2D/Exit2D (PM FsmState.cs:476-506): DoCollision*2D on every active
     * action, with this FSM on the execution stack.  Of all decompiled actions only Collision2dEvent
     * overrides DoCollision*2D (ACT/Collision2dEvent.cs:64-89; Collision2dEventLayer uses the
     * PlayMakerUnity2DProxy delegates instead), so its callback is the whole loop.  Its event, sent from
     * inside, leaves the FSM switching, which is what suppresses the system event below. */
    state_inst *st = &f->states[f->active_state];
    for (int32_t i = 0; i < st->n_active; i++) {
        act_inst *a = &st->acts[st->active_actions[i]];
        if (a->vt && strcmp(a->vt->type_short, "Collision2dEvent") == 0) act_proxy_callback(a, kind, other_go, other_layer);
    }
    state_remove_finished_actions(&f->states[f->active_state]);
    if (!fsm_is_switching(f)) fsm_event(f, w_get_fsm_event(f->w, names[kind]));
    fsm_update_state_changes(f);
    pop_fsm(f->w);
}

/* dump restore: FsmState.OnEnter bookkeeping for the dumped active state without observable effects */
void state_silent_exit(fsm_inst *f, int32_t s)   /* FsmState.OnExit bookkeeping (delegates unregister) without events */
{
    state_on_exit(&f->states[s]);
    f->n_delayed = 0;
}

void state_silent_enter(fsm_inst *f, int32_t s)
{
    state_on_enter(&f->states[s]);
    f->switch_to = -1;                 /* a suppressed FINISHED can not have queued anything, but be safe */
}

void act_proxy_callback(act_inst *a, int kind, int32_t other_go, int32_t other_layer)
{
    if (a->vt && a->vt->proxy_cb) a->vt->proxy_cb(a, kind, other_go, other_layer);
}
