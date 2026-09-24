/* World create / destroy from the compiled scene tables, scene start (restore from the SceneReady dump),
 * the per-stage world chores, and TrainingEnv's boss binding. */
#include "fsm/fsm.h"
#include "fsm/lifecycle.h"
#include "world_internal.h"
#include "core/phys.h"
#include "core/sim_modules.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "core/alloc.h"
#include "core/tls.h"
#include "fsm/fsm_scene_registry.inc"   /* generated: every sim/fsm/tables_*.h */

/* TrainingEnv.cs:535-548 -- the rescan the mod falls back on when BossSceneController.bosses is
 * empty.  It runs at the top of Step (TrainingEnv.cs:563-578) at `_stepCount >= 240 && _stepCount % 240
 * == 0` and binds every HealthManager alive with hp >= 100, recording _bossMaxHPs[hm] = hm.hp AT THAT
 * MOMENT (a boss damaged earlier gets a smaller denominator).  Hits before the bind credit zero
 * (TrainingEnv.cs:1152), and is_target follows the same flag. */
/* TrainingEnv.ScanBindBossHMs (oracle TrainingEnv.cs:1433-1449): every HealthManager that
 * Object.FindObjectsOfType<HealthManager>() returns -- ACTIVE GameObjects only, as the method's own
 * comment says ("FindObjectsOfType sees only ACTIVE objects") -- that is not isDead and has hp >= 100,
 * binding _bossMaxHPs[hm] = hm.hp as it is now.  Returns how many were bound. */
int32_t world_scan_bind_boss_hms(fsm_world *w)
{
    int32_t n = 0;
    for (int32_t i = 0; i < w->n_hms; i++) {
        hm_inst *h = &w->hms[i];
        if (h->bound || !go_active_in_hierarchy(w, h->go)) continue;
        if (h->is_dead || h->hp < 100) continue;               /* TrainingEnv.cs:1437 */
        h->bound = 1; h->bound_max_hp = h->hp; w->n_bound_bosses++; n++;
    }
    return n;
}

void world_boss_bind_tick(fsm_world *w)
{
    w->agent_step++;
    if (w->sc->boss_bind_route != 2 || w->n_bound_bosses > 0) return;
    /* The mod scans at the TOP of Step (TrainingEnv.cs:563-578), before that step's frames.  This tick
     * runs at the end of reset and of every step, so agent_step is the NUMBER OF THE NEXT STEP; nothing
     * runs in between, so scanning here is the game's instant and step 240's own hits are credited. */
    int32_t next = w->agent_step;
    if (next < 240 || (next % 240) != 0) return;               /* TrainingEnv.cs:290-294: every 240 steps */
    if (world_scan_bind_boss_hms(w) > 0) return;
    /* Nothing active to bind yet is the game's own case too: a boss still in its entrance (GG_Radiance's
     * `Absolute Radiance` switches on 262 live frames after SceneReady, in game and sim alike, so at one frame per
     * step the mod binds her at step 480).  A fight where NO HealthManager can ever pass the scan is one the
     * reward cannot describe -- no damage credit, no is_target, no win detection -- so that refuses. */
    for (int32_t i = 0; i < w->n_hms; i++) {
        const hm_inst *h = &w->hms[i];
        if (!w->gos[h->go].destroyed && !h->is_dead && h->hp >= 100) return;
    }
    HKSIM_UNIMPLEMENTED("scan-route boss bind at step %d: none of %d HealthManagers can ever bind (none alive with "
                        "hp >= 100, TrainingEnv.cs:696-712)", next, w->n_hms);
}

float world_take_damage_landed(fsm_world *w)
{
    world_boss_bind_tick(w);   /* once per agent step: the mod's every-240-steps rescan (see above) */
    float v = w->damage_landed_step;
    w->damage_landed_step = 0.0f;
    return v;
}

/* ---- per-scene action cache -------------------------------------------------------------------
 * An action's bound arena (field values plus referenced pool ranges, resolved to arena-local indices),
 * RNG site hash and vtable are functions of the SCENE TABLE alone, so they are computed once per scene.
 * Sites and vtables are shared outright.  The arena is NOT: pf_set/pi_set write literal fields (PlayMaker
 * instance state), so each world memcpys its own copy of the shared TEMPLATE; act_inst.arena is
 * `const fsm_pv *` so those writes must go through mut().
 * Entries are keyed by INSTANTIATION ORDINAL (the walk order of fsm_instantiate), which stays correct if
 * two fsm_defs share a state range: the arena would be common, rng_site (GO path + FSM name) would not.
 * The cache lives for the process, one entry per distinct scene, built under g_once. */
static const char *sc_str(const hkfsm_scene_def *sc, int32_t id)
{
    return (id >= 0 && id < sc->n_strings) ? sc->strings[id] : "";   /* every id is static at build time */
}

/* pvbuf and everything scene_cache_get builds with it belong to the SCENE, not to an instance:
 * one copy is shared by every world of that scene and outlives all of them.  Putting it in whichever
 * instance happened to build it first would free it under the others, so these use the system heap
 * explicitly -- the only allocations in this file that are not per-instance state. */
typedef struct { fsm_pv *v; int32_t n, cap; } pvbuf;
static int32_t pv_push(pvbuf *b, const fsm_pv *src)
{
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 8;
        b->v = hks_sys_realloc(b->v, sizeof *b->v * (size_t)b->cap);
        HKSIM_ASSERT(b->v != NULL, "out of memory building an action arena");
    }
    b->v[b->n] = *src;
    return b->n++;
}
/* Resolve one arena entry's pool reference into arena-local indices. */
static void pv_fix(pvbuf *b, const hkfsm_scene_def *sc, int32_t idx)
{
    fsm_pv v = b->v[idx];
    int32_t count = 0;
    switch (v.kind) {
    case PV_OWNERDEF: {
        int32_t n = pv_push(b, &sc->pool[v.i]);
        pv_fix(b, sc, n);
        b->v[idx].i = n;
        return;
    }
    case PV_EVTARGET: case PV_FUNCCALL: case PV_FVAR: case PV_ARRAY: case PV_SETPROP: count = v.j; break;
    case PV_FARRAY: if (v.vmode == VM_LITERAL) count = v.j; else return; break;
    default: return;
    }
    if (count <= 0 && v.kind != PV_EVTARGET) { b->v[idx].i = b->n; b->v[idx].j = 0; return; }
    int32_t start = b->n;
    for (int32_t k = 0; k < count; k++) pv_push(b, &sc->pool[v.i + k]);
    for (int32_t k = 0; k < count; k++) pv_fix(b, sc, start + k);
    b->v[idx].i = start;
}

struct scene_cache {
    struct scene_cache *next;
    const hkfsm_scene_def *sc;
    const fsm_pv *tmpl;               /* every instantiated action's arena, back to back */
    int32_t n_tmpl;                   /* fsm_pv entries in `tmpl` */
    int32_t *off, *len;               /* [n_acts] slice of `arena` for instantiation ordinal i */
    uint64_t *site;                   /* [n_acts] hk_rng_site */
    const act_vtable **vt;            /* [n_acts] */
    int32_t *st_off;                  /* [n_acts] byte offset of the action's private state, -1 if it has none */
    int32_t n_st;                     /* bytes in the world's action-state block */
    int32_t *fsm_base;                /* [sc->n_fsms] first ordinal of that FSM */
    int32_t n_acts;
    size_t blk_size;                  /* bytes the world's single structure block needs */
};
static scene_cache *g_scene_caches;
static hks_mutex g_once;              /* the cache is built once and read from every thread */
HKS_CTOR hks_world_ctor(void) { HKS_MUTEX_INIT(&g_once); }

static const scene_cache *scene_cache_get(const hkfsm_scene_def *sc)
{
    for (scene_cache *c = g_scene_caches; c; c = c->next) if (c->sc == sc) return c;
    HKS_LOCK(&g_once);
    for (scene_cache *c = g_scene_caches; c; c = c->next)
        if (c->sc == sc) { HKS_UNLOCK(&g_once); return c; }   /* another thread built it while we waited */

    scene_cache *c = hks_sys_calloc(1, sizeof *c);
    HKSIM_ASSERT(c != NULL, "out of memory building the scene action cache");
    c->sc = sc;
    c->fsm_base = hks_sys_malloc(sizeof(int32_t) * (size_t)(sc->n_fsms > 0 ? sc->n_fsms : 1));
    HKSIM_ASSERT(c->fsm_base != NULL, "out of memory building the scene action cache");
    int32_t n = 0;
    for (int32_t i = 0; i < sc->n_fsms; i++) {
        const fsm_def *d = &sc->fsms[i];
        c->fsm_base[i] = n;
        for (int32_t st = 0; st < d->n_states; st++) n += sc->states[d->state_start + st].n_actions;
    }
    c->n_acts = n;
    size_t m = (size_t)(n > 0 ? n : 1);
    c->off = hks_sys_malloc(sizeof(int32_t) * m);
    c->len = hks_sys_malloc(sizeof(int32_t) * m);
    c->site = hks_sys_malloc(sizeof(uint64_t) * m);
    c->vt = hks_sys_malloc(sizeof *c->vt * m);
    c->st_off = hks_sys_malloc(sizeof(int32_t) * m);
    HKSIM_ASSERT(c->off && c->len && c->site && c->vt && c->st_off, "out of memory building the scene action cache");

    pvbuf all = { NULL, 0, 0 }, one = { NULL, 0, 0 };
    int32_t ord = 0;
    for (int32_t i = 0; i < sc->n_fsms; i++) {
        const fsm_def *d = &sc->fsms[i];
        const char *path = sc_str(sc, sc->gos[d->go].path);
        const char *fname = sc_str(sc, d->fsm_name);
        for (int32_t st = 0; st < d->n_states; st++) {
            const fsm_state_def *sd = &sc->states[d->state_start + st];
            const char *sname = sc_str(sc, sd->name);
            for (int32_t k = 0; k < sd->n_actions; k++, ord++) {
                const fsm_action_def *ad = &sc->actions[sd->action_start + k];
                one.n = 0;
                for (int32_t j = 0; j < ad->n_fields; j++) pv_push(&one, &sc->fields[ad->field_start + j].v);
                for (int32_t j = 0; j < ad->n_fields; j++) pv_fix(&one, sc, j);
                c->off[ord] = all.n;
                c->len[ord] = one.n;
                for (int32_t j = 0; j < one.n; j++) pv_push(&all, &one.v[j]);
                c->site[ord] = hk_rng_site(path, fname, sname, k);
                c->vt[ord] = act_lookup(sc_str(sc, ad->type_short));
                /* Lay out the action's private state in the world's single block.  8-byte alignment
                 * covers every member these structs hold (pointers, floats, int32, int64). */
                if (c->vt[ord] && c->vt[ord]->state_size) {
                    c->st_off[ord] = c->n_st;
                    c->n_st += (int32_t)((c->vt[ord]->state_size + 7u) & ~(size_t)7u);
                } else {
                    c->st_off[ord] = -1;
                }
            }
        }
    }
    HKSIM_ASSERT(ord == n, "scene cache walked %d actions but was sized for %d", ord, n);
    hks_sys_free(one.v);
    c->tmpl = all.v;
    c->n_tmpl = all.n;

    /* Total the world's single structure block.  Mirrors world_create / fsm_instantiate exactly,
     * including their `? x : 1` minimum sizes, plus 8 bytes of alignment slack per slice. */
    size_t blk = (size_t)all.n * sizeof(fsm_pv) + 8 + (size_t)c->n_st + 8;
    for (int32_t i = 0; i < sc->n_fsms; i++) {
        const fsm_def *d = &sc->fsms[i];
        blk += (size_t)(d->n_vars > 0 ? d->n_vars : 1) * sizeof(fsm_val) + 8;
        blk += (size_t)(d->n_states > 0 ? d->n_states : 1) * sizeof(state_inst) + 8;
        for (int32_t st = 0; st < d->n_states; st++) {
            int32_t na = sc->states[d->state_start + st].n_actions;
            blk += (size_t)(na > 0 ? na : 1) * sizeof(act_inst) + 8;
        }
    }
    for (int32_t i = 0; i < sc->n_gos; i++)
        blk += (size_t)(sc->gos[i].n_cols > 0 ? sc->gos[i].n_cols : 1) * sizeof(col_inst) + 8;
    c->blk_size = blk;
    c->next = g_scene_caches;
    g_scene_caches = c;
    HKS_UNLOCK(&g_once);
    return c;
}

/* A zeroed block handed out in 8-byte-aligned slices, each once: the world's structure block at creation, or
 * one runtime clone's own block (world_instantiate). */
typedef struct { unsigned char *p; size_t used, size; } slab;
static void *slab_alloc(slab *b, size_t n)
{
    size_t off = (b->used + 7u) & ~(size_t)7u;
    HKSIM_ASSERT(off + n <= b->size, "structure block overflow: %lu + %lu > %lu",
                 (unsigned long)off, (unsigned long)n, (unsigned long)b->size);
    b->used = off + n;
    return b->p + off;
}
static void *blk_alloc(fsm_world *w, size_t n)
{
    slab b = { w->blk, w->blk_used, w->blk_size };
    void *p = slab_alloc(&b, n);
    w->blk_used = b.used;
    return p;
}

/* ---- world creation ---- */
/* An FsmObject variable holding an AlertRange: the simulator names the component by the GameObject that carries
 * it (act_knight.c FindAlertRange / CheckAlertRange), while the table carries the dumped reference as the
 * object's NAME (gen_tables.py FsmObject: pv.i = name, pv.j = type).  A restored FSM never re-runs the Init
 * that FindAlertRange'd it, so the dumped reference is converted here (GG_Mega_Moss_Charger
 * `Mossy Control | Attack Range Obj`). */
static int32_t fobj_alert_range_go(fsm_world *w, const fsm_pv *pv)
{
    if (pv->vmode != 0 || !pv->sub || pv->i < 0 || pv->j < 0 || strcmp(w_str(w, pv->j), "AlertRange") != 0) return pv->i;
    const char *nm = w_str(w, pv->i);
    for (int32_t g = 0; g < w->n_gos; g++)
        if (!w->gos[g].def->asset && strcmp(go_name(w, g), nm) == 0 && go_has_component(w, g, "AlertRange")) return g;
    return -1;
}
static void val_from_pv(fsm_world *w, fsm_val *v, const fsm_pv *pv)
{
    memset(v, 0, sizeof *v);
    switch (pv->kind) {
    case PV_FFLOAT: v->f = pv->f[0]; break;
    case PV_FOBJ: v->i = fobj_alert_range_go(w, pv); break;
    case PV_FINT: case PV_FBOOL: case PV_FSTRING: case PV_FGO: case PV_FENUM: case PV_FMAT: case PV_FTEX:
        v->i = pv->i; break;
    case PV_FV2: case PV_FV3: case PV_FQUAT: case PV_FCOLOR: case PV_FRECT: memcpy(v->v, pv->f, sizeof(float) * 4); break;
    case PV_FARRAY: v->i = pv->i; v->f = (float)pv->j; break;    /* static element range (read-only until an array action is ported) */
    default: break;
    }
    (void)w;
}

/* One FSM instance from its def.  `tmpl` is the scene-table FSM whose action cache entries it uses (itself,
 * or for a runtime clone the FSM it copies); its structures come from `mem`, the action arenas from
 * `arena` / `act_st`, which hold that FSM's slices starting at cache offsets `arena_off0` / `st_off0`.
 * `remap` rewrites the literal GameObject references of a clone's arena before the actions bind. */
typedef struct { const int32_t *from, *to; int32_t n; } go_map;
static void go_map_pv(fsm_pv *v, int32_t n, const go_map *m);
static void fsm_instantiate(fsm_world *w, int32_t id, int32_t tmpl, slab *mem, fsm_pv *arena, int32_t arena_off0,
                            unsigned char *act_st, int32_t st_off0, const go_map *remap)
{
    fsm_inst *f = &w->fsms[id];
    const fsm_def *d = &w->sc->fsms[id];
    f->def = d; f->id = id; f->w = w; f->go = d->go;
    f->component_enabled = d->enabled;
    f->handle_fixed = d->handle_fixed; f->handle_late = d->handle_late;
    f->active_state = -1; f->previous_state = -1; f->switch_to = -1; f->last_transition = -1;
    f->event_target = -1;
    /* not started until fsm_enter_dumped_state or its own Start (started_at_dump is read by the restore) */
    f->started = 0;
    f->finished = d->finished_at_dump;
    f->n_vals = d->n_vars;
    f->vals = slab_alloc(mem, (size_t)(d->n_vars > 0 ? d->n_vars : 1) * sizeof(fsm_val));
    for (int32_t i = 0; i < d->n_vars; i++) val_from_pv(w, &f->vals[i], &w->sc->vars[d->var_start + i].v);
    const scene_cache *sk = w->cache;
    int32_t ord = sk->fsm_base[tmpl];
    f->n_states = d->n_states;
    f->states = slab_alloc(mem, (size_t)(d->n_states > 0 ? d->n_states : 1) * sizeof(state_inst));
    for (int32_t s = 0; s < d->n_states; s++) {
        state_inst *st = &f->states[s];
        st->def = &w->sc->states[d->state_start + s];
        st->fsm = f;
        st->n_acts = st->def->n_actions;
        st->acts = slab_alloc(mem, (size_t)(st->n_acts > 0 ? st->n_acts : 1) * sizeof(act_inst));
        st->active_action = -1;
        for (int32_t k = 0; k < st->n_acts; k++) {
            act_inst *a = &st->acts[k];
            a->def = &w->sc->actions[st->def->action_start + k];
            a->fsm = f; a->state = st; a->index = k;
            a->enabled = a->def->enabled;
            a->n_fields = a->def->n_fields;
            a->rng_site = sk->site[ord];      /* arena, site and vtable are scene-derived: scene_cache_get */
            a->vt = sk->vt[ord];
            fsm_pv *ar = arena + (sk->off[ord] - arena_off0);   /* world-owned slice; the template is per scene */
            if (remap) go_map_pv(ar, sk->len[ord], remap);
            a->arena = ar;
            a->n_arena = sk->len[ord];
            if (a->vt) {
                if (sk->st_off[ord] >= 0) a->st = act_st + (sk->st_off[ord] - st_off0);
                if (a->vt->bind) a->vt->bind(a);
            }
            ord++;
        }
    }
    /* Handle*2D flags (see fsm_inst.handle_2d).  Fsm.CheckFsmEventsForEventHandlers (Fsm.cs:1685-1730):
     * a declared TRIGGER/COLLISION ENTER|STAY|EXIT 2D system event sets the matching flag; and
     * Collision2dEvent.OnPreprocess (ACT/Collision2dEvent.cs:40-55) sets HandleCollision<collision>2D for
     * every such action in the FSM, active or not (Collision2DType: 0 Enter 1 Stay 2 Exit). */
    {
        static const char *const n2d[6] = { "COLLISION ENTER 2D", "COLLISION STAY 2D", "COLLISION EXIT 2D",
                                            "TRIGGER ENTER 2D", "TRIGGER STAY 2D", "TRIGGER EXIT 2D" };
        for (int32_t e = 0; e < d->n_events; e++) {
            const char *en = w_str(w, w->sc->events[d->event_start + e].name);
            for (int b = 0; b < 6; b++) if (strcmp(en, n2d[b]) == 0) f->handle_2d |= (uint8_t)(1u << b);
        }
        for (int32_t si = 0; si < f->n_states; si++)
            for (int32_t k = 0; k < f->states[si].n_acts; k++) {
                act_inst *a = &f->states[si].acts[k];
                if (!a->vt || strcmp(a->vt->type_short, "Collision2dEvent") != 0) continue;
                int32_t c = pi(f, a_field_req(a, "collision"));
                if (c >= 0 && c <= 2) f->handle_2d |= (uint8_t)(1u << c);
            }
    }
}

const hkfsm_scene_def *hkfsm_scene_lookup(const char *name)
{
    for (size_t i = 0; i < sizeof HKFSM_REGISTRY_SCENES / sizeof HKFSM_REGISTRY_SCENES[0]; i++)
        if (strcmp(HKFSM_REGISTRY_SCENES[i]->level_key ? HKFSM_REGISTRY_SCENES[i]->level_key : HKFSM_REGISTRY_SCENES[i]->scene_name, name) == 0)
            return HKFSM_REGISTRY_SCENES[i];   /* level key: "<scene>" or "<scene>@T1|@T2" (fsm_tables.h) */
    return NULL;
}

/* Spare slots every instance array keeps for runtime clones (see fsm_world.gos): enough for every clone one
 * stage can Instantiate; world_instantiate traps rather than move an array mid-stage. */
#define GROW_RESERVE 128

/* The scene-load state of each instance kind, from its def (world_create, and the fresh instances of a
 * runtime clone in world_instantiate). */
static void go_init(fsm_world *w, int32_t i, col_inst *cols)
{
    const hkfsm_scene_def *sc = w->sc;
    go_inst *g = &w->gos[i];
    const go_def *d = &sc->gos[i];
    g->def = d; g->id = i;
    g->active_self = d->asset ? 0 : d->active_self;   /* a prefab asset is never in the world; its def keeps the authored flag */
    g->pool_of = (d->prefab >= 0 && !d->asset) ? d->prefab : -1;   /* a dumped pooled clone: in its prefab's pool */
    g->parent = d->parent; g->first_child = d->first_child; g->next_sibling = d->next_sibling;
    g->has_transform = d->has_transform;
    memcpy(g->local_pos, d->parent < 0 ? d->pos : d->local_pos, sizeof(float) * 3);
    memcpy(g->local_scale, d->local_scale, sizeof(float) * 3);
    g->local_euler_z = d->local_euler_z;
    g->anim = g->hm = g->dh = g->recoil = g->constrain = g->arcy = g->mec = -1;
    g->mesh_renderer_enabled = 1;
    g->tag_override = -1;                   /* nothing has written gameObject.tag yet; go_tag falls back to def->tag */
    g->layer_override = -1;                 /* nothing has written gameObject.layer yet */
    g->nonbouncer_active = 1;               /* NonBouncer.cs:5 `public bool active = true` */
    g->transform_dirty = 1;
    g->shapes_dirty = 0;                    /* shapes are baked at bind time, so they start clean */
    if (go_has_component(w, i, "LimitSendEvents")) {                /* owner list for the LimitSendEvents components (lifecycle.c LCT_LSE) */
        w->lse_gos = realloc(w->lse_gos, sizeof(int32_t) * (size_t)(w->n_lse_gos + 1));
        g->lse = calloc(1, sizeof *g->lse);
        HKSIM_ASSERT(w->lse_gos != NULL && g->lse != NULL, "out of memory building the LimitSendEvents owner list");
        w->lse_gos[w->n_lse_gos++] = i;
    }
    if (d->rb >= 0) {
        const rb_def *r = &sc->rbs[d->rb];
        g->has_rb = 1; g->vel[0] = r->vel[0]; g->vel[1] = r->vel[1];
        world_xf_track(w, i);
        g->gravity_scale = r->gravity_scale; g->kinematic = r->is_kinematic;
    }
    g->n_cols = d->n_cols;
    g->cols = cols;
    for (int32_t k = 0; k < d->n_cols; k++) {
        const col_def *c = &sc->cols[sc->col_idx[d->col_start + k]];
        g->cols[k].def = c; g->cols[k].enabled = c->enabled; g->cols[k].is_trigger = c->is_trigger;
        memcpy(g->cols[k].offset, c->offset, sizeof(float) * 2); memcpy(g->cols[k].size, c->size, sizeof(float) * 2);
        g->cols[k].radius = c->radius;
    }
}
static void anim_init(fsm_world *w, int32_t i)
{
    anim_inst *a = &w->anims[i];
    a->def = &w->sc->animators[i]; a->go = a->def->go; a->lib = a->def->lib;
    a->cur_clip = -1; a->clip_fps = -1.0f; a->previous_frame = -1;   /* tk2dSpriteAnimator.cs:34-37 */
    a->sprite_id = -1;                                          /* tk2dBaseSprite._spriteId serialized default (:33) */
    a->sprite_def = -1;
    a->enabled = a->def->enabled; a->completed_event = a->triggered_event = -1;
    w->gos[a->go].anim = i;
}
static void dh_init(fsm_world *w, int32_t i)
{
    w->dhs[i].def = &w->sc->damageheros[i]; w->dhs[i].go = w->dhs[i].def->go;
    w->dhs[i].damage_dealt = w->dhs[i].def->damage_dealt; w->dhs[i].enabled = w->dhs[i].def->enabled;
    w->gos[w->dhs[i].go].dh = i;
}
static void recoil_init(fsm_world *w, int32_t i)
{
    w->recoils[i].def = &w->sc->recoils[i]; w->recoils[i].go = w->recoils[i].def->go;
    w->recoils[i].speed_base = w->recoils[i].def->speed_base;
    w->gos[w->recoils[i].go].recoil = i;
}
static void arcy_init(fsm_world *w, int32_t i)
{
    w->autorecycles[i].def = &w->sc->autorecycles[i];
    w->autorecycles[i].go = w->sc->autorecycles[i].go;
    w->gos[w->autorecycles[i].go].arcy = i;
}
static void constrain_init(fsm_world *w, int32_t i)
{
    w->constrains[i].def = &w->sc->constrains[i]; w->constrains[i].go = w->constrains[i].def->go;
    w->gos[w->constrains[i].go].constrain = i;
}

fsm_world *world_create(const hkfsm_scene_def *sc)
{
    fsm_world *w = calloc(1, sizeof *w);
    w->sc = sc;
    w->cache = scene_cache_get(sc);
    w->blk_size = w->cache->blk_size;
    w->blk = calloc(w->blk_size, 1);
    HKSIM_ASSERT(w->blk != NULL, "out of memory allocating the world structure block");
    w->arena = blk_alloc(w, sizeof(fsm_pv) * (size_t)(w->cache->n_tmpl > 0 ? w->cache->n_tmpl : 1));
    w->act_st = blk_alloc(w, (size_t)(w->cache->n_st > 0 ? w->cache->n_st : 1));
    memcpy(w->arena, w->cache->tmpl, sizeof(fsm_pv) * (size_t)w->cache->n_tmpl);
    w->rng = &w->rng_own;
    w->log_enabled = true;
    w->fixed_dt = 0.02f;                                           /* R2: dumps/GG_Hornet_1/physics.json#Time.fixedDeltaTime */
    w->bosses_dead_signal = 0; w->scene_left = 0;
    w->boss_level = sc->boss_level;   /* BossSceneController.BossLevel of the dumped load: meta.json bossLevel (gen_tables.py) */
    w->is_boss_scene = true;                                       /* GG_* scenes: BossSceneController.IsBossScene */
    /* events: every declared/transition event of every FSM, global events, system events (FsmEvent.cs:440-497) */
    static const char *sys_events[] = { "FINISHED", "DISABLE", "BECAME INVISIBLE", "BECAME VISIBLE", "LEVEL LOADED", "MOUSE DOWN",
        "MOUSE DRAG", "MOUSE ENTER", "MOUSE EXIT", "MOUSE OVER", "MOUSE UP", "MOUSE UP AS BUTTON", "COLLISION ENTER", "COLLISION EXIT",
        "COLLISION STAY", "CONTROLLER COLLIDER HIT", "TRIGGER ENTER", "TRIGGER EXIT", "TRIGGER STAY", "COLLISION ENTER 2D",
        "COLLISION EXIT 2D", "COLLISION STAY 2D", "TRIGGER ENTER 2D", "TRIGGER EXIT 2D", "TRIGGER STAY 2D", "PLAYER CONNECTED",
        "SERVER INITIALIZED", "CONNECTED TO SERVER", "PLAYER DISCONNECTED", "DISCONNECTED FROM SERVER", "FAILED TO CONNECT",
        "FAILED TO CONNECT TO MASTER SERVER", "MASTER SERVER EVENT", "NETWORK INSTANTIATE", "APPLICATION FOCUS", "APPLICATION PAUSE",
        "APPLICATION QUIT", "PARTICLE COLLISION", "JOINT BREAK", "JOINT BREAK 2D", "UI BEGIN DRAG", "UI DRAG", "UI END DRAG", "UI CLICK",
        "UI DROP", "UI POINTER CLICK", "UI POINTER DOWN", "UI POINTER ENTER", "UI POINTER EXIT", "UI POINTER UP", "UI BOOL VALUE CHANGED",
        "UI FLOAT VALUE CHANGED", "UI INT VALUE CHANGED", "UI VECTOR2 VALUE CHANGED", "UI END EDIT" };
    for (size_t i = 0; i < sizeof sys_events / sizeof sys_events[0]; i++) w_get_fsm_event(w, sys_events[i]);
    for (int32_t i = 0; i < sc->n_events; i++) w_get_fsm_event_id(w, sc->events[i].name);
    for (int32_t i = 0; i < sc->n_trans; i++) w_get_fsm_event_id(w, sc->trans[i].event);
    for (int32_t i = 0; i < sc->n_global_events; i++) w_get_fsm_event_id(w, sc->global_events[i]);
    /* GameObjects */
    w->n_gos = sc->n_gos;
    w->cap_gos = sc->n_gos + GROW_RESERVE;
    w->gos = calloc((size_t)w->cap_gos, sizeof(go_inst));
    for (int32_t i = 0; i < sc->n_gos; i++)
        go_init(w, i, blk_alloc(w, (size_t)(sc->gos[i].n_cols > 0 ? sc->gos[i].n_cols : 1) * sizeof(col_inst)));
    /* camera parent: its transform is not in scene.json (no collider); ShakePositionV2 only moves the camera,
     * which the observation never reads (boss-hornet.md §6.4.1b "not the position writes") -> origin pose */
    for (int32_t g = sc->camera_parent_go; g >= 0; g = w->gos[g].parent)
        if (!w->gos[g].has_transform) w->gos[g].has_transform = 1;   /* whole _GameCameras chain: camera-only, unobserved */
    for (int32_t i = 0; i < w->n_gos; i++) {
        if (w->gos[i].parent < 0) update_active_in_hierarchy_dfs(w, i, true);
    }
    w->knight_go = sc->knight_go; w->camera_parent_go = sc->camera_parent_go; w->game_manager_go = sc->game_manager_go;
    /* globals */
    w->n_gvals = sc->n_globals;
    w->gvals = calloc((size_t)(sc->n_globals > 0 ? sc->n_globals : 1), sizeof(fsm_val));
    for (int32_t i = 0; i < sc->n_globals; i++) val_from_pv(w, &w->gvals[i], &sc->globals[i].v);
    /* FSMs */
    w->n_fsms = sc->n_fsms;
    w->cap_fsms = sc->n_fsms + GROW_RESERVE;
    w->fsms = calloc((size_t)w->cap_fsms, sizeof(fsm_inst));
    {
        slab mem = { w->blk, w->blk_used, w->blk_size };
        for (int32_t i = 0; i < sc->n_fsms; i++) fsm_instantiate(w, i, i, &mem, w->arena, 0, w->act_st, 0, NULL);
        w->blk_used = mem.used;
    }
    /* components */
    w->n_anims = sc->n_animators;
    w->cap_anims = sc->n_animators + GROW_RESERVE;
    w->anims = calloc((size_t)w->cap_anims, sizeof(anim_inst));
    for (int32_t i = 0; i < sc->n_animators; i++) anim_init(w, i);
    w->n_hms = sc->n_hms;
    w->hms = calloc((size_t)(sc->n_hms > 0 ? sc->n_hms : 1), sizeof(hm_inst));
    for (int32_t i = 0; i < sc->n_hms; i++) {
        hm_inst *h = &w->hms[i];
        h->def = &sc->hms[i]; h->go = h->def->go;
        h->hp = h->def->hp; h->is_dead = h->def->is_dead; h->invincible = h->def->invincible;
        h->invincible_from_direction = h->def->invincible_from_direction;
        h->evasion_by_hit_remaining = -1.0f;                       /* HealthManager.Start :299 */
        h->stun_control_fsm = h->def->stun_control_fsm;
        w->gos[h->go].hm = i;
        if (h->def->is_boss) w->n_boss_hms++;
        /* NATIVE route: BossSceneController.bosses was populated, so InitBossRefs binds at tick 1 with
         * the serialized hp.  SCAN route bosses are bound by world_boss_bind_tick's every-240-steps rescan. */
        if (sc->boss_bind_route != 2 && h->def->is_boss) {
            h->bound = 1; h->bound_max_hp = h->def->hp; w->n_bound_bosses++;
        }
    }
    /* An empty derived set means the rule did not understand this scene.  Native route only: on the scan
     * route an empty set at t=0 is expected (world_boss_bind_tick traps instead). */
    if (sc->n_hms > 0 && sc->boss_bind_route != 2 && w->n_boss_hms == 0)
        HKSIM_UNIMPLEMENTED("no boss HealthManager derived for this scene: %d HealthManagers, none in "
                      "BossSceneController.bosses (gen_tables.py is_boss, bind-at-start route)", sc->n_hms);
    w->n_dhs = sc->n_damageheros;
    w->cap_dhs = sc->n_damageheros + GROW_RESERVE;
    w->dhs = calloc((size_t)w->cap_dhs, sizeof(dh_inst));
    for (int32_t i = 0; i < sc->n_damageheros; i++) dh_init(w, i);
    w->n_recoils = sc->n_recoils;
    w->cap_recoils = sc->n_recoils + GROW_RESERVE;
    w->recoils = calloc((size_t)w->cap_recoils, sizeof(recoil_inst));
    for (int32_t i = 0; i < sc->n_recoils; i++) recoil_init(w, i);
    w->n_autorecycles = sc->n_autorecycles;
    w->cap_autorecycles = sc->n_autorecycles + GROW_RESERVE;
    w->autorecycles = calloc((size_t)w->cap_autorecycles, sizeof(autorecycle_inst));
    for (int32_t i = 0; i < sc->n_autorecycles; i++) arcy_init(w, i);
    w->n_constrains = sc->n_constrains;
    w->cap_constrains = sc->n_constrains + GROW_RESERVE;
    w->constrains = calloc((size_t)w->cap_constrains, sizeof(constrain_inst));
    for (int32_t i = 0; i < sc->n_constrains; i++) constrain_init(w, i);
    w->n_mecs = sc->n_mec_anims;
    w->cap_mecs = sc->n_mec_anims + GROW_RESERVE;
    w->mecs = calloc((size_t)w->cap_mecs, sizeof(mec_inst));
    for (int32_t i = 0; i < sc->n_mec_anims; i++) mecanim_init(w, i);
    w->n_pd = sc->n_playerdata;
    w->pd = calloc((size_t)(sc->n_playerdata > 0 ? sc->n_playerdata : 1), sizeof(pd_val));
    for (int32_t i = 0; i < sc->n_playerdata; i++) {
        const pd_field_def *p = &sc->playerdata[i];
        w->pd[i].name = p->name; w->pd[i].kind = p->kind; w->pd[i].f = p->f; w->pd[i].i = p->i; w->pd[i].b = p->b; w->pd[i].s = p->s;
        memcpy(w->pd[i].v, p->v, sizeof p->v);
    }
    w->phys = NULL;
    lc_create(w);                                                  /* the component registry (lifecycle.c) */
    return w;
}

/* ---- runtime Instantiate ---------------------------------------------------------------------------
 * Object.Instantiate(prefab) copies the prefab's whole subtree (ObjectPool.cs:505 and :552 for the pool,
 * CreateObject.cs:79, SpawnRandomObjects.cs:88, SpawnRandomObjectsV2.cs:90).  The prefab is a template the scene
 * tables carry (sim/fsm/gen/prefabs.py: the extracted asset, flagged `asset`), or a scene object an action names
 * directly (GG_Ghost_No_Eyes `No Eyes Head`).  A copy takes the same defs, re-pointed at its own GameObjects, and
 * fresh instances built from them the way world_create builds every other object.
 *
 * The defs go into per-world copies of the scene tables (`struct world_grow`), which `w->sc` then points
 * at.  Def arrays are read-only, so a superseded one is kept, not freed, until world_destroy: a caller up
 * the stack may still read through it.  Instance arrays are mutable and do not move here (fsm_world.gos). */
enum { GD_GOS, GD_COL_IDX, GD_COLS, GD_RBS, GD_COMPS, GD_EVREGS, GD_FSM_IDX, GD_FSMS, GD_ANIMS, GD_DHS, GD_RECOILS,
       GD_CONSTRAINS, GD_ARCY, GD_MECS, GD_MEC_BINDS, GD_N };
typedef struct world_grow world_grow;
struct world_grow {
    hkfsm_scene_def sc;             /* the tables `w->sc` points at: the static ones with grown arrays */
    const hkfsm_scene_def *sc0;     /* the static tables */
    int32_t cap[GD_N];              /* capacity of the per-world copy of each array; 0 = still the static one */
    int32_t n_col_idx;              /* col_idx entries in use (the table has no count of its own) */
    void **keep; int32_t n_keep, cap_keep;   /* freed by world_destroy: superseded def arrays, clone blocks */
};

static void grow_keep(world_grow *g, void *p)
{
    if (g->n_keep == g->cap_keep) {
        g->cap_keep = g->cap_keep ? g->cap_keep * 2 : 64;
        g->keep = realloc(g->keep, sizeof(void *) * (size_t)g->cap_keep);
        HKSIM_ASSERT(g->keep != NULL, "out of memory growing the scene tables");
    }
    g->keep[g->n_keep++] = p;
}
/* Room for `add` more entries after `n` in def array `k`, returned writable (the caller stores it back). */
static void *def_room(world_grow *g, int k, const void *cur, int32_t n, int32_t add, size_t esz)
{
    if (n + add <= g->cap[k]) return (void *)cur;
    int32_t nc = n + add > 2 * n ? n + add : 2 * n;
    if (nc < 16) nc = 16;
    void *p = malloc(esz * (size_t)nc);
    HKSIM_ASSERT(p != NULL, "out of memory growing the scene tables");
    if (n > 0) memcpy(p, cur, esz * (size_t)n);
    if (g->cap[k] > 0) grow_keep(g, (void *)cur);
    g->cap[k] = nc;
    return p;
}

/* The per-world copies of the scene tables, made on first use. */
static world_grow *grow_tables(fsm_world *w)
{
    world_grow *g = w->grow;
    if (g) return g;
    g = calloc(1, sizeof *g);
    HKSIM_ASSERT(g != NULL, "out of memory growing the scene tables");
    memcpy(&g->sc, w->sc, sizeof g->sc);
    g->sc0 = w->sc;
    for (int32_t i = 0; i < w->sc->n_gos; i++) {
        int32_t e = w->sc->gos[i].col_start + w->sc->gos[i].n_cols;
        if (w->sc->gos[i].n_cols > 0 && e > g->n_col_idx) g->n_col_idx = e;
    }
    w->grow = g;
    w->sc = &g->sc;
    return g;
}

static int32_t go_map_find(const go_map *m, int32_t go)
{
    for (int32_t k = 0; k < m->n; k++) if (m->from[k] == go) return m->to[k];
    return -1;
}
/* A literal GameObject reference into the copied subtree names the clone's own object (gen_tables.py
 * re-encodes a pool copy's FSMs with the copy's paths); references outside it are shared. */
static void go_map_pv(fsm_pv *v, int32_t n, const go_map *m)
{
    for (int32_t i = 0; i < n; i++) {
        if (v[i].kind != PV_FGO || v[i].vmode != VM_LITERAL) continue;
        int32_t t = go_map_find(m, v[i].i);
        if (t >= 0) v[i].i = t;
    }
}

static void subtree_defs(const hkfsm_scene_def *sc, int32_t go, int32_t *out, int32_t *n, int32_t cap)
{
    HKSIM_ASSERT(*n < cap, "Instantiate: pooled prefab subtree deeper than %d objects", cap);
    out[(*n)++] = go;
    for (int32_t c = sc->gos[go].first_child; c >= 0; c = sc->gos[c].next_sibling) subtree_defs(sc, c, out, n, cap);
}

/* Every instance's def pointer, after the def arrays may have moved: a def is found by address
 * (observer.c col_inst_of), so none may keep pointing at a superseded copy. */
static void repoint_defs(fsm_world *w)
{
    const hkfsm_scene_def *sc = w->sc;
    for (int32_t i = 0; i < w->n_gos; i++) {
        go_inst *g = &w->gos[i];
        g->def = &sc->gos[i];
        for (int32_t k = 0; k < g->n_cols; k++) g->cols[k].def = &sc->cols[sc->col_idx[g->def->col_start + k]];
    }
    for (int32_t i = 0; i < w->n_fsms; i++) w->fsms[i].def = &sc->fsms[i];
    for (int32_t i = 0; i < w->n_anims; i++) w->anims[i].def = &sc->animators[i];
    for (int32_t i = 0; i < w->n_dhs; i++) w->dhs[i].def = &sc->damageheros[i];
    for (int32_t i = 0; i < w->n_recoils; i++) w->recoils[i].def = &sc->recoils[i];
    for (int32_t i = 0; i < w->n_autorecycles; i++) w->autorecycles[i].def = &sc->autorecycles[i];
    for (int32_t i = 0; i < w->n_constrains; i++) w->constrains[i].def = &sc->constrains[i];
    for (int32_t i = 0; i < w->n_mecs; i++) w->mecs[i].def = &sc->mec_anims[i];
}

/* A clone's GameObject name and path.  Instantiate names the copy "<prefab>(Clone)" (Object.Instantiate); the
 * oracle names an object by its path, and a pooled clone by its pool home wherever it is parented
 * (oracle/Record/RngDrawRecorder.cs Canon: `_GameManager/GlobalPool/<name>`), so a pooled copy takes that path and
 * any other copy the path of a scene root. */
static void clone_names(fsm_world *w, const go_def *gd, const int32_t *sub, int32_t k, bool pooled,
                        int32_t *name, int32_t *path)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "%s(Clone)", w_str(w, gd[sub[0]].name));
    name[0] = w_intern(w, buf);
    if (pooled) { char p2[1100]; snprintf(p2, sizeof p2, "_GameManager/GlobalPool/%s", buf); path[0] = w_intern(w, p2); }
    else path[0] = name[0];
    for (int32_t i = 1; i < k; i++) {
        int32_t pi_ = -1;
        for (int32_t j = 0; j < i; j++) if (sub[j] == gd[sub[i]].parent) { pi_ = j; break; }
        HKSIM_ASSERT(pi_ >= 0, "Instantiate: %s's parent is outside the copied subtree", w_str(w, gd[sub[i]].path));
        name[i] = gd[sub[i]].name;
        snprintf(buf, sizeof buf, "%s/%s", w_str(w, path[pi_]), w_str(w, gd[sub[i]].name));
        path[i] = w_intern(w, buf);
    }
}

/* Object.Instantiate(prefab): a new, INACTIVE copy of the prefab's subtree, a root (Transform.parent null) or the
 * last child of `parent`, carrying the prefab's serialized state; nothing on it has Awoken or Started (a fresh
 * object; lc_go_added).  `pooled`: an ObjectPool instance (named by its pool home).  The caller poses it and
 * activates it.  Returns the new root. */
/* GameObject.AddComponent<BoxCollider2D>() (tk2dBaseSprite.UpdateCollider, tk2dBaseSprite.cs:473-479): a new
 * BoxCollider2D as Collider2D::Collider2D (UP!0x180bfd9b0: enabled) and BoxCollider2D::Reset (UP!0x180c15190, with
 * Collider2D::Reset UP!0x180c07490) leave it -- not a trigger, offset (0,0), size (1,1) -- on the object's layer,
 * attached to the Rigidbody2D on the object or its nearest ancestor.  It joins physics now if the object is active
 * and the observer through the scene collider count (observer.c obs_sync).  Its def goes into the per-world tables
 * as a clone's do; an object that already has colliders would need its collider block moved, which the observer's
 * cached col_inst pointers do not survive, so that traps. */
col_inst *world_add_box_collider(fsm_world *w, int32_t go)
{
    go_inst *gi = &w->gos[go];
    if (gi->n_cols > 0)
        HKSIM_UNIMPLEMENTED("AddComponent<BoxCollider2D> on '%s', which already has %d Collider2D(s)", go_path(w, go), gi->n_cols);
    int32_t rb_go = go;
    while (rb_go >= 0 && w->gos[rb_go].def->rb < 0) rb_go = w->gos[rb_go].parent;
    if (rb_go >= 0 && rb_go == w->knight_go)
        HKSIM_UNIMPLEMENTED("AddComponent<BoxCollider2D> on '%s' attaches to the Knight's body (hero module)", go_path(w, go));
    world_grow *g = grow_tables(w);
    hkfsm_scene_def *gs = &g->sc;
    go_def *gd = def_room(g, GD_GOS, gs->gos, gs->n_gos, 0, sizeof *gd); gs->gos = gd;
    int32_t *cix = def_room(g, GD_COL_IDX, gs->col_idx, g->n_col_idx, 1, sizeof *cix); gs->col_idx = cix;
    col_def *cols = def_room(g, GD_COLS, gs->cols, gs->n_cols, 1, sizeof *cols); gs->cols = cols;
    col_def cd; memset(&cd, 0, sizeof cd);
    cd.go = go; cd.type = COL_BOX; cd.enabled = 1; cd.is_trigger = 0;
    cd.active_in_hierarchy = gi->active_in_hierarchy;
    cd.layer = (uint8_t)go_layer(w, go); cd.tag = go_tag(w, go);
    cd.size[0] = cd.size[1] = 1.0f;
    cd.pts_start = -1; cd.rb_go = rb_go; cd.instance_id = -1;       /* not a scene.json collider */
    cols[gs->n_cols] = cd;
    gd[go].col_start = g->n_col_idx;
    gd[go].n_cols = 1;
    cix[g->n_col_idx++] = gs->n_cols++;
    col_inst *ci = calloc(1, sizeof *ci);
    HKSIM_ASSERT(ci != NULL, "out of memory adding a collider");
    grow_keep(g, ci);
    ci->enabled = 1; ci->size[0] = ci->size[1] = 1.0f;
    gi->cols = ci; gi->n_cols = 1;
    repoint_defs(w);
    world_phys_add_collider(w, go, ci);
    return ci;
}

int32_t world_instantiate(fsm_world *w, int32_t prefab, int32_t parent, bool pooled)
{
    enum { SUB_CAP = 256 };
    int32_t sub[SUB_CAP], to[SUB_CAP], k = 0;
    const go_def *pd = &w->sc->gos[prefab];
    if (pd->asset && pd->prefab != prefab)
        HKSIM_UNIMPLEMENTED("Instantiate '%s': the tables hold only a stub of this prefab, which the ported scenes never "
                            "spawn (sim/fsm/gen/completeness.py UNREACHABLE_PREFABS)", go_name(w, prefab));
    subtree_defs(w->sc, prefab, sub, &k, SUB_CAP);
    const go_map map = { sub, to, k };
    int32_t go0 = w->n_gos;
    for (int32_t i = 0; i < k; i++) to[i] = go0 + i;
    int32_t names[SUB_CAP], paths[SUB_CAP];
    clone_names(w, w->sc->gos, sub, k, pooled, names, paths);

    /* what the subtree carries */
    const hkfsm_scene_def *sc = w->sc;
    int32_t n_col = 0, n_comp = 0, n_fsm = 0, n_anim = 0, n_dh = 0, n_rec = 0, n_con = 0, n_arcy = 0, n_rb = 0, n_ev = 0;
    int32_t n_mec = 0, n_mbind = 0;
    for (int32_t i = 0; i < k; i++) {
        const go_def *d = &sc->gos[sub[i]];
        if (d->hm >= 0 || go_has_component(w, sub[i], "HealthManager"))
            HKSIM_UNIMPLEMENTED("Instantiate '%s': %s carries a HealthManager; the tables carry HealthManagers only from "
                                "bosses.json", go_name(w, prefab), go_path(w, sub[i]));
        n_col += d->n_cols; n_comp += d->n_comps; n_fsm += d->n_fsms;
        n_anim += d->animator >= 0; n_dh += d->damage_hero >= 0; n_rec += d->recoil >= 0; n_con += d->constrain >= 0;
        n_rb += d->rb >= 0; n_arcy += w->gos[sub[i]].arcy >= 0;
        if (w->gos[sub[i]].mec >= 0) { n_mec++; n_mbind += sc->mec_anims[w->gos[sub[i]].mec].n_binds; }
    }
    int32_t n_ev0 = sc->n_evregs;
    for (int32_t e = 0; e < n_ev0; e++) n_ev += go_map_find(&map, sc->evregs[e].go) >= 0;
    if (w->n_gos + k > w->cap_gos || w->n_fsms + n_fsm > w->cap_fsms || w->n_anims + n_anim > w->cap_anims ||
        w->n_dhs + n_dh > w->cap_dhs || w->n_recoils + n_rec > w->cap_recoils ||
        w->n_constrains + n_con > w->cap_constrains || w->n_autorecycles + n_arcy > w->cap_autorecycles ||
        w->n_mecs + n_mec > w->cap_mecs)
        HKSIM_UNIMPLEMENTED("Instantiate '%s': more clones in one stage than the %d spare instance slots "
                            "(world_reserve refills them between stages)", go_name(w, prefab), GROW_RESERVE);
    HKSIM_ASSERT(w->n_gos == sc->n_gos && w->n_fsms == sc->n_fsms && w->n_anims == sc->n_animators &&
                 w->n_dhs == sc->n_damageheros && w->n_recoils == sc->n_recoils &&
                 w->n_constrains == sc->n_constrains && w->n_autorecycles == sc->n_autorecycles &&
                 w->n_mecs == sc->n_mec_anims,
                 "Instantiate: instance arrays out of step with the scene tables");

    /* the per-world tables */
    world_grow *g = grow_tables(w);
    hkfsm_scene_def *gs = &g->sc;
    go_def *gd = def_room(g, GD_GOS, gs->gos, gs->n_gos, k, sizeof *gd); gs->gos = gd;
    int32_t *cix = def_room(g, GD_COL_IDX, gs->col_idx, g->n_col_idx, n_col, sizeof *cix); gs->col_idx = cix;
    col_def *cols = def_room(g, GD_COLS, gs->cols, gs->n_cols, n_col, sizeof *cols); gs->cols = cols;
    rb_def *rbs = def_room(g, GD_RBS, gs->rbs, gs->n_rbs, n_rb, sizeof *rbs); gs->rbs = rbs;
    comp_def *comps = def_room(g, GD_COMPS, gs->comps, gs->n_comps, n_comp, sizeof *comps); gs->comps = comps;
    evreg_def *evs = def_room(g, GD_EVREGS, gs->evregs, gs->n_evregs, n_ev, sizeof *evs); gs->evregs = evs;
    int32_t *fix = def_room(g, GD_FSM_IDX, gs->fsm_idx, gs->n_fsm_idx, n_fsm, sizeof *fix); gs->fsm_idx = fix;
    fsm_def *fds = def_room(g, GD_FSMS, gs->fsms, gs->n_fsms, n_fsm, sizeof *fds); gs->fsms = fds;
    animator_def *ads = def_room(g, GD_ANIMS, gs->animators, gs->n_animators, n_anim, sizeof *ads); gs->animators = ads;
    damagehero_def *dds = def_room(g, GD_DHS, gs->damageheros, gs->n_damageheros, n_dh, sizeof *dds); gs->damageheros = dds;
    recoil_def *rds = def_room(g, GD_RECOILS, gs->recoils, gs->n_recoils, n_rec, sizeof *rds); gs->recoils = rds;
    constrain_def *cds = def_room(g, GD_CONSTRAINS, gs->constrains, gs->n_constrains, n_con, sizeof *cds); gs->constrains = cds;
    autorecycle_def *ards = def_room(g, GD_ARCY, gs->autorecycles, gs->n_autorecycles, n_arcy, sizeof *ards); gs->autorecycles = ards;
    mec_anim_def *mds = def_room(g, GD_MECS, gs->mec_anims, gs->n_mec_anims, n_mec, sizeof *mds); gs->mec_anims = mds;
    mec_bind_def *mbs = def_room(g, GD_MEC_BINDS, gs->mec_binds, gs->n_mec_binds, n_mbind, sizeof *mbs); gs->mec_binds = mbs;
    repoint_defs(w);

    /* the clone's defs: the template's, re-pointed at the clone (gen_tables.py pool copies) */
    int32_t fsm0 = gs->n_fsms, anim0 = gs->n_animators, dh0 = gs->n_damageheros;
    int32_t rec0 = gs->n_recoils, con0 = gs->n_constrains, arcy0 = gs->n_autorecycles, mec0 = gs->n_mec_anims;
    int32_t fsm_tmpl[SUB_CAP * 4], nf = 0;
    for (int32_t i = 0; i < k; i++) {
        int32_t t = sub[i], nid = to[i];
        go_def d = gd[t];
        d.parent = i == 0 ? parent : go_map_find(&map, d.parent);
        d.first_child = d.first_child >= 0 ? go_map_find(&map, d.first_child) : -1;
        d.next_sibling = (i == 0 || d.next_sibling < 0) ? -1 : go_map_find(&map, d.next_sibling);
        d.name = names[i]; d.path = paths[i];
        d.asset = 0; d.in_scene = 1; d.instance_id = -1;
        d.prefab = (i == 0 && pooled) ? prefab : -1;          /* a pool clone's root names its pool (go_inst.pool_of) */
        if (i == 0) d.active_self = 0;                      /* the caller activates it (Instantiate / Spawn) */
        if (d.rb >= 0) { rb_def r = rbs[d.rb]; r.go = nid; rbs[gs->n_rbs] = r; d.rb = gs->n_rbs++; }
        int32_t cs = g->n_col_idx;
        for (int32_t c = 0; c < d.n_cols; c++) {
            col_def cd = cols[cix[d.col_start + c]];
            cd.go = nid;
            if (cd.rb_go >= 0) {
                int32_t m = go_map_find(&map, cd.rb_go);
                if (m < 0) HKSIM_UNIMPLEMENTED("Instantiate '%s': a collider of %s is attached to a Rigidbody2D outside "
                                               "the clone (%s)", go_name(w, prefab), go_path(w, t), go_path(w, cd.rb_go));
                cd.rb_go = m;
            }
            cd.instance_id = -1;                            /* not a scene.json collider */
            cols[gs->n_cols] = cd;
            cix[g->n_col_idx++] = gs->n_cols++;
        }
        d.col_start = cs;
        int32_t ps = gs->n_comps;
        for (int32_t c = 0; c < d.n_comps; c++) { comp_def cd = comps[d.comp_start + c]; cd.go = nid; comps[gs->n_comps++] = cd; }
        d.comp_start = ps;
        if (d.animator >= 0) { animator_def a = ads[d.animator]; a.go = nid; ads[gs->n_animators] = a; d.animator = gs->n_animators++; }
        if (d.damage_hero >= 0) { damagehero_def x = dds[d.damage_hero]; x.go = nid; dds[gs->n_damageheros] = x; d.damage_hero = gs->n_damageheros++; }
        if (d.recoil >= 0) { recoil_def x = rds[d.recoil]; x.go = nid; rds[gs->n_recoils] = x; d.recoil = gs->n_recoils++; }
        if (d.constrain >= 0) { constrain_def x = cds[d.constrain]; x.go = nid; cds[gs->n_constrains] = x; d.constrain = gs->n_constrains++; }
        if (w->gos[t].arcy >= 0) { autorecycle_def x = ards[w->gos[t].arcy]; x.go = nid; ards[gs->n_autorecycles++] = x; }
        if (w->gos[t].mec >= 0) {                              /* an Animator: its bindings name objects of the copy */
            mec_anim_def x = mds[w->gos[t].mec]; x.go = nid;
            int32_t b0 = gs->n_mec_binds;
            for (int32_t b = 0; b < x.n_binds; b++) {
                mec_bind_def y = mbs[x.bind_start + b];
                y.go = go_map_find(&map, y.go);
                if (y.go < 0) HKSIM_UNIMPLEMENTED("Instantiate '%s': an Animator of %s binds an object outside the clone",
                                                  go_name(w, prefab), go_path(w, t));
                mbs[gs->n_mec_binds++] = y;
            }
            x.bind_start = b0;
            mds[gs->n_mec_anims++] = x;
        }
        int32_t fs = gs->n_fsm_idx;
        for (int32_t c = 0; c < d.n_fsms; c++) {
            int32_t fi = fix[d.fsm_start + c];
            fsm_def fd = fds[fi]; fd.go = nid;
            HKSIM_ASSERT(nf < (int32_t)(sizeof fsm_tmpl / sizeof fsm_tmpl[0]), "Instantiate: too many FSMs on one clone");
            fsm_tmpl[nf++] = fi;
            fds[gs->n_fsms] = fd;
            fix[gs->n_fsm_idx++] = gs->n_fsms++;
        }
        d.fsm_start = fs;
        gd[nid] = d; gs->n_gos++;
    }
    for (int32_t e = 0; e < n_ev0; e++) {
        int32_t m = go_map_find(&map, evs[e].go);
        if (m < 0) continue;
        evreg_def x = evs[e]; x.go = m; evs[gs->n_evregs++] = x;
    }

    /* fresh instances */
    for (int32_t i = 0; i < k; i++) {
        int32_t nid = to[i];
        memset(&w->gos[nid], 0, sizeof w->gos[nid]);
        col_inst *ci = calloc((size_t)(gd[nid].n_cols > 0 ? gd[nid].n_cols : 1), sizeof(col_inst));
        HKSIM_ASSERT(ci != NULL, "out of memory instantiating a clone");
        grow_keep(g, ci);
        go_init(w, nid, ci);
    }
    w->n_gos = go0 + k;
    if (parent >= 0) {                                        /* the last child of `parent` */
        go_inst *pg = &w->gos[parent];
        if (pg->first_child < 0) pg->first_child = go0;
        else { int32_t c = pg->first_child; while (w->gos[c].next_sibling >= 0) c = w->gos[c].next_sibling; w->gos[c].next_sibling = go0; }
        update_active_in_hierarchy_dfs(w, go0, pg->active_in_hierarchy != 0);
    } else {
        update_active_in_hierarchy_dfs(w, go0, true);
    }
    const scene_cache *sk = w->cache;
    for (int32_t j = 0; j < nf; j++) {
        int32_t id = fsm0 + j, t = fsm_tmpl[j];
        const fsm_def *d = &fds[id];
        /* the template's cache slices: its actions' ordinals are consecutive, so are their arena and state */
        int32_t ord0 = sk->fsm_base[t], nact = 0;
        for (int32_t st = 0; st < d->n_states; st++) nact += gs->states[d->state_start + st].n_actions;
        int32_t off0 = 0, n_ar = 0, st0 = -1, st_end = 0;
        if (nact > 0) {
            off0 = sk->off[ord0];
            n_ar = sk->off[ord0 + nact - 1] + sk->len[ord0 + nact - 1] - off0;
            for (int32_t o = ord0; o < ord0 + nact; o++) {
                if (sk->st_off[o] < 0) continue;
                if (st0 < 0) st0 = sk->st_off[o];
                st_end = sk->st_off[o] + (int32_t)((sk->vt[o]->state_size + 7u) & ~(size_t)7u);
            }
        }
        if (st0 < 0) st0 = st_end = 0;
        size_t sz = (size_t)(d->n_vars > 0 ? d->n_vars : 1) * sizeof(fsm_val) + 8 +
                    (size_t)(n_ar > 0 ? n_ar : 1) * sizeof(fsm_pv) + 8 + (size_t)(st_end - st0 > 0 ? st_end - st0 : 1) + 8;
        sz += (size_t)(d->n_states > 0 ? d->n_states : 1) * sizeof(state_inst) + 8;
        for (int32_t st = 0; st < d->n_states; st++) {
            int32_t na = gs->states[d->state_start + st].n_actions;
            sz += (size_t)(na > 0 ? na : 1) * sizeof(act_inst) + 8;
        }
        slab mem = { calloc(sz, 1), 0, sz };
        HKSIM_ASSERT(mem.p != NULL, "out of memory instantiating a clone");
        grow_keep(g, mem.p);
        fsm_pv *arena = slab_alloc(&mem, (size_t)(n_ar > 0 ? n_ar : 1) * sizeof(fsm_pv));
        if (n_ar > 0) memcpy(arena, sk->tmpl + off0, sizeof(fsm_pv) * (size_t)n_ar);   /* scene-load field values */
        unsigned char *act_st = slab_alloc(&mem, (size_t)(st_end - st0 > 0 ? st_end - st0 : 1));
        memset(&w->fsms[id], 0, sizeof w->fsms[id]);
        fsm_instantiate(w, id, t, &mem, arena, off0, act_st, st0, &map);
        fsm_inst *f = &w->fsms[id];
        f->finished = 0;                                      /* finished_at_dump is the dumped clone's, not a new one's */
        for (int32_t v = 0; v < d->n_vars; v++) {
            const fsm_pv *pv = &gs->vars[d->var_start + v].v;
            if (pv->kind == PV_FGO) { int32_t m = go_map_find(&map, f->vals[v].i); if (m >= 0) f->vals[v].i = m; }
            else if (pv->kind == PV_FARRAY)                   /* its elements stay in the shared pool (val_from_pv) */
                for (int32_t e = 0; e < pv->j; e++)
                    if (gs->pool[pv->i + e].kind == PV_FGO && go_map_find(&map, gs->pool[pv->i + e].i) >= 0)
                        HKSIM_UNIMPLEMENTED("Instantiate '%s': FsmArray variable of %s holds the clone's own GameObjects",
                                            go_name(w, prefab), fsm_label(f));
        }
        /* RNG sites are named by the owner's path (hk_rng_site): the copy's, not the prefab's */
        const char *fname = w_str(w, d->fsm_name);
        for (int32_t st = 0; st < f->n_states; st++)
            for (int32_t a = 0; a < f->states[st].n_acts; a++)
                f->states[st].acts[a].rng_site = hk_rng_site(go_path(w, f->go), fname, w_str(w, f->states[st].def->name), a);
    }
    w->n_fsms = fsm0 + nf;
    for (int32_t i = anim0; i < gs->n_animators; i++) { memset(&w->anims[i], 0, sizeof w->anims[i]); anim_init(w, i); }
    w->n_anims = gs->n_animators;
    for (int32_t i = dh0; i < gs->n_damageheros; i++) { memset(&w->dhs[i], 0, sizeof w->dhs[i]); dh_init(w, i); }
    w->n_dhs = gs->n_damageheros;
    for (int32_t i = rec0; i < gs->n_recoils; i++) { memset(&w->recoils[i], 0, sizeof w->recoils[i]); recoil_init(w, i); }
    w->n_recoils = gs->n_recoils;
    for (int32_t i = con0; i < gs->n_constrains; i++) { memset(&w->constrains[i], 0, sizeof w->constrains[i]); constrain_init(w, i); }
    w->n_constrains = gs->n_constrains;
    for (int32_t i = arcy0; i < gs->n_autorecycles; i++) { memset(&w->autorecycles[i], 0, sizeof w->autorecycles[i]); arcy_init(w, i); }
    w->n_autorecycles = gs->n_autorecycles;
    for (int32_t i = mec0; i < gs->n_mec_anims; i++) { memset(&w->mecs[i], 0, sizeof w->mecs[i]); mecanim_init(w, i); }
    w->n_mecs = gs->n_mec_anims;
    /* EventRegister.Awake subscribes (event_register_seed installs the scene's up front, likewise) */
    for (int32_t e = n_ev0; e < gs->n_evregs; e++) {
        if (evs[e].name < 0) continue;
        HKSIM_ASSERT(w->n_ev_reg < FSM_EV_REG_CAP, "EventRegister table full");
        w->ev_reg[w->n_ev_reg].go = evs[e].go; w->ev_reg[w->n_ev_reg].name = evs[e].name; w->n_ev_reg++;
    }
    if (w->dirty_bits && ((w->n_gos + 63) >> 6) > w->n_dirty_words) {   /* the shape-flush bitsets cover every object */
        int32_t nw = (w->n_gos + 63) >> 6;
        w->dirty_bits = realloc(w->dirty_bits, sizeof(uint64_t) * (size_t)nw);
        w->drain_bits = realloc(w->drain_bits, sizeof(uint64_t) * (size_t)nw);
        HKSIM_ASSERT(w->dirty_bits && w->drain_bits, "out of memory growing the dirty-shape set");
        memset(w->dirty_bits + w->n_dirty_words, 0, sizeof(uint64_t) * (size_t)(nw - w->n_dirty_words));
        memset(w->drain_bits + w->n_dirty_words, 0, sizeof(uint64_t) * (size_t)(nw - w->n_dirty_words));
        w->n_dirty_words = nw;
    }
    for (int32_t i = 0; i < k; i++) lc_go_added(w, to[i]);
    return go0;
}

/* Refill the spare instance slots (fsm_world.gos).  Only between stages: nothing up the stack holds a
 * pointer into these arrays there.  Stored pointers are fixed: the FSM back-pointers of every state and
 * action; animators are held by index (anim_ref). */
#define RESERVE(arr, n, cap) do {                                                                   \
        if ((cap) - (n) >= GROW_RESERVE) break;                                                     \
        int32_t nc_ = 2 * (cap);                                                                    \
        (arr) = realloc((arr), sizeof *(arr) * (size_t)nc_);                                        \
        HKSIM_ASSERT((arr) != NULL, "out of memory growing an instance array");                     \
        memset((arr) + (cap), 0, sizeof *(arr) * (size_t)(nc_ - (cap)));                            \
        (cap) = nc_;                                                                                \
    } while (0)
void world_reserve(fsm_world *w)
{
    RESERVE(w->gos, w->n_gos, w->cap_gos);
    fsm_inst *f0 = w->fsms;
    RESERVE(w->fsms, w->n_fsms, w->cap_fsms);
    if (w->fsms != f0)
        for (int32_t i = 0; i < w->n_fsms; i++) {
            fsm_inst *f = &w->fsms[i];
            for (int32_t s = 0; s < f->n_states; s++) {
                f->states[s].fsm = f;
                for (int32_t k = 0; k < f->states[s].n_acts; k++) f->states[s].acts[k].fsm = f;
            }
        }
    RESERVE(w->anims, w->n_anims, w->cap_anims);
    RESERVE(w->dhs, w->n_dhs, w->cap_dhs);
    RESERVE(w->recoils, w->n_recoils, w->cap_recoils);
    RESERVE(w->autorecycles, w->n_autorecycles, w->cap_autorecycles);
    RESERVE(w->constrains, w->n_constrains, w->cap_constrains);
    RESERVE(w->mecs, w->n_mecs, w->cap_mecs);
}
#undef RESERVE

static void world_grow_free(fsm_world *w)
{
    world_grow *g = w->grow;
    if (!g) return;
    for (int32_t i = 0; i < g->n_keep; i++) free(g->keep[i]);
    free(g->keep);
    const void *own[GD_N] = { g->sc.gos, g->sc.col_idx, g->sc.cols, g->sc.rbs, g->sc.comps, g->sc.evregs, g->sc.fsm_idx,
                              g->sc.fsms, g->sc.animators, g->sc.damageheros, g->sc.recoils, g->sc.constrains,
                              g->sc.autorecycles, g->sc.mec_anims, g->sc.mec_binds };
    for (int k = 0; k < GD_N; k++) if (g->cap[k] > 0) free((void *)own[k]);
    w->sc = g->sc0;
    free(g);
    w->grow = NULL;
}

void world_destroy(fsm_world *w)
{
    if (!w) return;
    /* Order matters: w->blk holds fsm_inst.states / .vals, state_inst.acts, go_inst.cols, the action
     * arena and the action-state block, so everything reached THROUGH those is released before it. */
    lc_destroy(w);                                                 /* flushes HKSIM_LIFECYCLE_LOG */
    hk_rng_free(&w->rng_own);
    world_obs_cache_free(w);

    for (int32_t i = 0; i < w->n_fsms; i++) {                          /* reaches f->states, which is in w->blk */
        fsm_inst *f = &w->fsms[i];
        for (int32_t s = 0; s < f->n_states; s++) {
            free(f->states[s].active_actions);
            free(f->states[s].finished_actions);
        }
        free(f->dangling); free(f->dangling_names); free(f->delayed);
    }
    for (int32_t i = 0; i < w->n_gos; i++) {
        if (w->gos[i].dlg) { for (int k = 0; k < 6; k++) free(w->gos[i].dlg->l[k].a); free(w->gos[i].dlg); }
        if (w->gos[i].lse) { free(w->gos[i].lse->sent); free(w->gos[i].lse); }
        free(w->gos[i].tt_inside);   /* TrackTriggerObjects proxy state (AddTrackTrigger) */
    }
    for (int32_t i = 0; i < w->n_dyn; i++) free(w->dyn_strings[i]);

    free(w->dyn_strings); free(w->event_registered); free(w->gos); free(w->fsms); free(w->fsm_list);
    free(w->gvals); free(w->static_go); free(w->anims); free(w->hms); free(w->dhs); free(w->recoils);
    free(w->constrains); free(w->mecs); free(w->grimmballs); free(w->pd); free(w->log);
    free(w->itweens); free(w->svars); free(w->lse_gos); free(w->xf_gos);
    free(w->dirty_bits); free(w->drain_bits);
    world_grow_free(w);                                                /* the clones' blocks: their states were reached above */
    free(w->blk);                                                      /* last: everything above pointed into it */
    free(w);
}

pd_val *world_pd(fsm_world *w, const char *name)
{
    for (int32_t i = 0; i < w->n_pd; i++) if (strcmp(w_str(w, w->pd[i].name), name) == 0) return &w->pd[i];
    return NULL;
}

/* ---- scene start: RESTORE the SceneReady dump ----
 * An FSM "in state S" is, in PlayMaker, four things the dump records for every FSM: `started`
 * (Fsm.cs:1003, PlayMakerFSM.cs:355-367), `activeStateName` with activeStateEntered, the active state's
 * action LiveFields (oracle/Oracle/FsmDumper.cs:284-308), and the FsmVariables.  Nothing distinguishes an
 * FSM that arrived in S from one put there, so every started FSM is entered directly into its dumped state
 * (docs/roadmap.md).
 *
 * (a) The dump is taken at TrainingEnv.cs:513 (RaiseSceneReady) and the first FRAME one 0.02 step later
 *     (:616-617), both at coroutine resume; only the animator advances in the gap (frame N's LateUpdate),
 *     which tk2d.c anim_restore_dumped applies once.  FSM timers are restored raw.
 * (b) The dumped state's OnEnter must run (it arms timers and delegates) but its side effects are already in
 *     the dump: `snapshot_mode` suppresses fsm_event / fsm_event_to, go_set_active and world_pool_spawn, and
 *     the RNG is saved and restored around it.
 *
 * FSMs the dump shows not started, and the SNAP_COLD_OVERRIDE ones, start from their startState. */

/* FSMs that start from startState even though the dump shows them started: the states passed THROUGH write a plain
 * MonoBehaviour field the dump does not record.  `Knight/Charm Effects | Slash Size Modifiers` (startState
 * `Init`, dumped in terminal `Equipped`) reaches it via SendMessage(SetLongnail/SetMantis), which write
 * NailSlash's private `longnail` / `mantis` (analysis/decomp/Assembly-CSharp/NailSlash.cs:160-168); those
 * pick the slash scale and clip at NailSlash.cs:68-86.  sim/hero/hero_dump_init.c zeroes both and relies on
 * this FSM to set them. */
static const struct { const char *path, *fsm; } SNAP_COLD_OVERRIDE[] = {
    { "Knight/Charm Effects", "Slash Size Modifiers" },
};

static bool fsm_should_restore(fsm_world *w, const fsm_inst *f)
{
    if (!(f->def->started_at_dump && f->def->active_state_at_dump >= 0)) return false;
    for (size_t k = 0; k < sizeof SNAP_COLD_OVERRIDE / sizeof SNAP_COLD_OVERRIDE[0]; k++)
        if (strcmp(go_path(w, f->go), SNAP_COLD_OVERRIDE[k].path) == 0
            && strcmp(w_str(w, f->def->fsm_name), SNAP_COLD_OVERRIDE[k].fsm) == 0) return false;
    return true;
}

/* Every component is put in the dispatch lists as the dump shows it (lifecycle.c lc_scene_load); a started
 * FSM enters its dumped state, one enabled but not started (GG_Ghost_Markoth `Markoth Shield(Clone)`,
 * GG_Ghost_Hu / Marmu `Warp | Screen Shake`) was switched on in the dump frame's update_delayed and gets its
 * Start in that frame's tail stages below (R1, docs/engine-lifecycle.md). */
void world_restore_scene(fsm_world *w)
{
    lc_scene_load(w);
    world_animators_start(w);
    /* (c) The FsmVariables are the dump's, which already hold every store the dumped state's OnEnter made; the
     * re-entry below re-evaluates those stores against the DUMPED world, which is not the world they ran in (a
     * GetParent re-run after the object was re-parented: GG_Crystal_Guardian `Beam | destroy_if_gameobject_null`
     * dumps Parent = the Beam Miner, which deparents Beam later).  So every FSM's variables are put back to the
     * dump's after the re-entries. */
    size_t nv = 0;
    for (int32_t i = 0; i < w->n_fsms; i++) nv += (size_t)w->fsms[i].n_vals;
    fsm_val *saved = malloc((nv ? nv : 1) * sizeof *saved);
    if (!saved) HKSIM_UNKNOWN("world_restore_scene: out of memory saving %d FSM variables", (int)nv);
    nv = 0;
    for (int32_t i = 0; i < w->n_fsms; i++) {
        memcpy(saved + nv, w->fsms[i].vals, (size_t)w->fsms[i].n_vals * sizeof *saved);
        nv += (size_t)w->fsms[i].n_vals;
    }
    for (int32_t i = 0; i < w->n_fsms; i++) {
        fsm_inst *f = &w->fsms[i];
        if (!f->in_fsm_list || !lc_is_restored(w, LCT_FSM, i)) continue;
        if (!fsm_should_restore(w, f)) { lc_unrestore(w, LCT_FSM, f->id); continue; }
        fsm_enter_dumped_state(w, f);
    }
    nv = 0;
    for (int32_t i = 0; i < w->n_fsms; i++) {
        memcpy(w->fsms[i].vals, saved + nv, (size_t)w->fsms[i].n_vals * sizeof *saved);
        nv += (size_t)w->fsms[i].n_vals;
    }
    free(saved);
    /* the rest of the dump frame: the Animators, LateUpdate and postlate_delayed for what was not restored (the
     * tk2d animators' LateUpdate of this frame is the +0.02 anim_restore_dumped applies, A-21; no emitted Animator
     * is active at the dump, mecanim.py) */
    lc_stage(w, LCS_ANIM, 0.02f, LCF_SKIP_RESTORED);
    lc_stage(w, LCS_LATE, 0.02f, LCF_SKIP_RESTORED);
    lc_stage(w, LCS_POSTLATE_DELAYED, 0.02f, LCF_SKIP_RESTORED);
}

/* One FSM into its DUMPED state: its OnEnter runs under snapshot_mode with the RNG saved (see above). */
void fsm_enter_dumped_state(fsm_world *w, fsm_inst *f)
{
    int was = w->snapshot_mode;
    bool log_was = w->log_enabled;
    hk_rng saved = *w->rng;
    w->snapshot_mode = 1;
    w->log_enabled = false;
    if (f->active_state >= 0 && f->active_state_entered) {   /* leave a state the way a transition would */
        push_fsm(w, f->id); state_silent_exit(f, f->active_state); pop_fsm(w);
    }
    f->started = 1;
    f->finished = f->def->finished_at_dump;
    f->active_state = f->def->active_state_at_dump;
    f->active_state_entered = 1;
    push_fsm(w, f->id);
    f->switch_to = -1;
    state_silent_enter(f, f->active_state);
    pop_fsm(w);
    /* ...and the animator it drives: the clip is half of every combat row's "entity|clip" key.  The dumped
     * clipTimeSeconds is one animator update behind the first FRAME (bosses.json GG_Nosk Mimic Spider
     * `Roar Init` 0.08 vs 0.10 at the first FRAME); anim_restore_dumped adds that step. */
    if (f->go >= 0) {
        anim_inst *a = anim_of_go(w, f->go);
        if (a && a->def->cur_clip >= 0) {
            anim_restore_dumped(w, a);
            anim_set_paused(a, a->def->paused != 0);
        }
    }
    *w->rng = saved;
    w->log_enabled = log_was;
    w->snapshot_mode = was;
}

/* Per-stage chores of the world itself, called by lifecycle.c at the start / end of every stage it runs:
 * transforms read back from the bodies, the per-frame counters, and the shape flush at the end of the FixedUpdate stage -- the last point before the physics step (Unity: FixedUpdate -> transform
 * sync -> Physics2D.Simulate, physics.json#Physics2D autoSyncTransforms). */
void world_stage_begin(fsm_world *w, int stage)
{
    world_invalidate_body_transforms(w);
    if (stage == LCS_FIXED) {
        w->live_frame_seq++;   /* exactly one per frame in live play: the only reliable frame index for probes */
    } else if (stage == LCS_UPDATE) {
        w->update_seq++;
    }
}
void world_stage_end(fsm_world *w, int stage)
{
    if (stage == LCS_FIXED) world_flush_dirty_shapes(w);
}
