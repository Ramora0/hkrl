/* Action lookup and the helpers every category shares. */
#include "act.h"

void act_finish_enter(act_inst *a) { act_finish(a); }

void act_finish_unless_every_frame(act_inst *a)
{
    const fsm_pv *e = a_field(a, "everyFrame");
    if (!e) e = a_field(a, "everyframe");
    if (!e || !pb(a->fsm, e)) act_finish(a);
}

/* ActionHelpers.GetRandomWeightedIndex -- HK/HutongGames.PlayMaker/ActionHelpers.cs:73-90: float32 sum,
 * ONE Range(0,sum) draw, strict `<` walk, -1 when it falls off the end (fsm-actions.md §0.6). */
int32_t random_weighted_index(act_inst *a, const fsm_pv *weights)
{
    fsm_inst *f = a->fsm;
    float num = 0.0f;
    int32_t n = a_array_len(a, weights);
    for (int32_t i = 0; i < n; i++) num += pf(f, a_array_elem(a, weights, i));
    float num2 = hk_rng_range_f_site(f->w->rng, a->rng_site, 0.0f, num);
    for (int32_t j = 0; j < n; j++) {
        float wj = pf(f, a_array_elem(a, weights, j));
        if (num2 < wj) return j;
        num2 -= wj;
    }
    return -1;
}

/* The value a private action field held at SceneReady (fsm_tables.h act_livefield_def), or `dflt`; while the lockstep
 * harness re-enters a recorded state, the value the recording holds (fsm_world.live_field).
 * Only meaningful while an FSM listed in SNAPSHOT_RULES is being put into its dumped state (snapshot_mode):
 * outside that window the action is starting fresh and must not inherit the dump's timer. */
float act_live_float(act_inst *a, const char *name, float dflt)
{
    fsm_world *w = a->fsm->w;
    if (!w->snapshot_mode) return dflt;
    if (w->live_field) { float v; return w->live_field(w->live_ctx, a, name, &v) ? v : dflt; }
    for (int32_t i = 0; i < a->def->n_live; i++) {
        const act_livefield_def *L = &w->sc->livefields[a->def->live_start + i];
        if (L->name >= 0 && strcmp(w_str(w, L->name), name) == 0) return L->f;
    }
    return dflt;
}

/* Every `const act_vtable *const act_registry_<name>[]` under sim/fsm, found by sim/gen_registries.py.
 * gate/dup_actions.py keeps each type registered once, so the registry order does not matter. */
#include "act_registry_list.inc"

const act_vtable *act_lookup(const char *type_short)
{
    for (int r = 0; r < ACT_REGISTRIES_N; r++)
        for (int i = 0; i < *ACT_REGISTRIES[r].n; i++)
            if (strcmp(ACT_REGISTRIES[r].v[i]->type_short, type_short) == 0) return ACT_REGISTRIES[r].v[i];
    return NULL;
}
