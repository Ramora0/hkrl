/* FSM variables and action parameter binding: PM/FsmVariables.cs, PM/NamedVariable.cs. */
#include "fsm/fsm.h"
#include <stdlib.h>
#include <string.h>
#include "core/alloc.h"

/* ---- variables & params ---- */
static int pv_bucket(uint8_t kind)
{
    switch (kind) {
    case PV_FFLOAT: return VB_FLOAT;   case PV_FINT: return VB_INT;     case PV_FBOOL: return VB_BOOL;
    case PV_FSTRING: return VB_STRING; case PV_FV2: return VB_V2;       case PV_FV3: return VB_V3;
    case PV_FRECT: return VB_RECT;     case PV_FQUAT: return VB_QUAT;   case PV_FCOLOR: return VB_COLOR;
    case PV_FGO: return VB_GO;         case PV_FARRAY: return VB_ARRAY; case PV_FENUM: return VB_ENUM;
    case PV_FOBJ: return VB_OBJ;       case PV_FMAT: return VB_MAT;     case PV_FTEX: return VB_TEX;
    default: return -1;
    }
}

int32_t fsm_find_var(fsm_inst *f, int bucket, int32_t name_id)
{
    const fsm_def *d = f->def;
    for (int32_t i = d->var_bucket_start[bucket]; i < d->var_bucket_start[bucket + 1]; i++)
        if (f->w->sc->vars[d->var_start + i].name == name_id) return i;
    return -1;
}

static fsm_val *dangling_slot(fsm_inst *f, int32_t name_id)
{
    for (int32_t i = 0; i < f->n_dangling; i++) if (f->dangling_names[i] == name_id) return &f->dangling[i];
    if (f->n_dangling == f->cap_dangling) {
        f->cap_dangling = f->cap_dangling ? f->cap_dangling * 2 : 4;
        f->dangling = realloc(f->dangling, sizeof(fsm_val) * (size_t)f->cap_dangling);
        f->dangling_names = realloc(f->dangling_names, sizeof(int32_t) * (size_t)f->cap_dangling);
    }
    memset(&f->dangling[f->n_dangling], 0, sizeof(fsm_val));
    f->dangling_names[f->n_dangling] = name_id;
    return &f->dangling[f->n_dangling++];
}

/* FsmVariables.GetFsmX(name) — FsmVariables.cs:1171-1197: local, global, else a fresh instance whose
 * CastVariable = GetVariable(name) (any bucket).  A same-named variable of another type would make
 * reads convert through it (NamedVariable.ToFloat etc.) — trapped until observed. */
/* FsmVariables.GlobalVariables.GetFsmGameObject(name).Value (FsmVariables.cs:1171-1197 on the globals): -1 when
 * no global of that name exists or it holds null */
int32_t world_global_go(fsm_world *w, const char *name)
{
    int32_t nid = w_find_string(w, name);
    const hkfsm_scene_def *sc = w->sc;
    for (int32_t i = sc->global_bucket_start[VB_GO]; nid >= 0 && i < sc->global_bucket_start[VB_GO + 1]; i++)
        if (sc->globals[i].name == nid) return w->gvals[i].i;
    return -1;
}

fsm_val *fsm_get_var(fsm_inst *f, int bucket, int32_t name_id, bool *fresh)
{
    if (fresh) *fresh = false;
    int32_t li = fsm_find_var(f, bucket, name_id);
    if (li >= 0) return &f->vals[li];
    const hkfsm_scene_def *sc = f->w->sc;
    for (int32_t i = sc->global_bucket_start[bucket]; i < sc->global_bucket_start[bucket + 1]; i++)
        if (sc->globals[i].name == name_id) return &f->w->gvals[i];
    for (int b = 0; b < VB_COUNT; b++) {
        if (b == bucket) continue;
        if (fsm_find_var(f, b, name_id) >= 0)
            HKSIM_UNIMPLEMENTED("variable '%s' missing in bucket %d of %s but present in bucket %d (CastVariable read-through, FsmVariables.cs:1195)",
                          w_str(f->w, name_id), bucket, fsm_label(f), b);
    }
    if (fresh) *fresh = true;
    return dangling_slot(f, name_id);
}

fsm_val *var_slot(fsm_inst *f, const fsm_pv *pv)
{
    switch (pv->vmode) {
    case VM_LOCAL: return &f->vals[pv->i];
    case VM_GLOBAL: return &f->w->gvals[pv->i];
    case VM_DANGLING: return fsm_get_var(f, pv_bucket(pv->kind), pv->i, NULL);
    default: return NULL;
    }
}
bool p_isnone(const fsm_pv *pv) { return pv->vmode == VM_NONE; }   /* NamedVariable.IsNone :132-142 */

static fsm_pv *mut(const fsm_pv *pv) { return (fsm_pv *)pv; }

float pf(fsm_inst *f, const fsm_pv *pv)
{
    HKSIM_ASSERT(pv->kind == PV_FFLOAT || pv->kind == PV_FLOAT, "pf: kind %d", pv->kind);
    fsm_val *v = var_slot(f, pv);
    return v ? v->f : pv->f[0];
}
void pf_set(fsm_inst *f, const fsm_pv *pv, float x)
{
    fsm_val *v = var_slot(f, pv);
    if (v) v->f = x; else mut(pv)->f[0] = x;                       /* literal/None: the action's own value field */
}
int32_t pi(fsm_inst *f, const fsm_pv *pv)
{
    fsm_val *v = var_slot(f, pv);
    if (v) return v->i;
    return pv->vmode == VM_LITERAL ? pv->i : pv->j;                /* None: serialized residue lives in j */
}
void pi_set(fsm_inst *f, const fsm_pv *pv, int32_t x)
{
    fsm_val *v = var_slot(f, pv);
    if (v) v->i = x; else if (pv->vmode == VM_LITERAL) mut(pv)->i = x; else mut(pv)->j = x;
}
bool pb(fsm_inst *f, const fsm_pv *pv)
{
    HKSIM_ASSERT(pv->kind == PV_FBOOL || pv->kind == PV_BOOL, "pb: kind %d", pv->kind);
    return pi(f, pv) != 0;
}
void pb_set(fsm_inst *f, const fsm_pv *pv, bool x) { pi_set(f, pv, x ? 1 : 0); }
int32_t ps(fsm_inst *f, const fsm_pv *pv)
{
    HKSIM_ASSERT(pv->kind == PV_FSTRING || pv->kind == PV_STRING, "ps: kind %d", pv->kind);
    return pi(f, pv);
}
void ps_set(fsm_inst *f, const fsm_pv *pv, int32_t sid) { pi_set(f, pv, sid); }
const float *pv3(fsm_inst *f, const fsm_pv *pv)
{
    fsm_val *v = var_slot(f, pv);
    return v ? v->v : pv->f;
}
void pv3_set(fsm_inst *f, const fsm_pv *pv, const float *x)
{
    fsm_val *v = var_slot(f, pv);
    float *dst = v ? v->v : mut(pv)->f;
    /* only the type's own components: callers pass a float[2] for an FsmVector2 and a float[3] for an FsmVector3 */
    size_t n = pv->kind == PV_FV2 ? 2 : (pv->kind == PV_FV3 ? 3 : 4);
    memcpy(dst, x, sizeof(float) * n);
}
int32_t pgo(fsm_inst *f, const fsm_pv *pv)
{
    HKSIM_ASSERT(pv->kind == PV_FGO, "pgo: kind %d", pv->kind);
    int32_t g = pi(f, pv);
    if (g >= 0 && f->w->gos[g].destroyed) return -1;              /* destroyed UnityEngine.Object compares == null */
    return g;
}
void pgo_set(fsm_inst *f, const fsm_pv *pv, int32_t go) { pi_set(f, pv, go); }
int32_t p_event(const fsm_pv *pv) { return pv->kind == PV_FEVENT ? pv->i : -1; }

const fsm_pv *a_pv(act_inst *a, int32_t idx)
{
    HKSIM_ASSERT(idx >= 0 && idx < a->n_arena, "arena index %d out of %d", idx, a->n_arena);
    return &a->arena[idx];
}
const fsm_pv *a_field(act_inst *a, const char *name)
{
    const hkfsm_scene_def *sc = a->fsm->w->sc;
    for (int32_t k = 0; k < a->n_fields; k++)
        if (strcmp(sc->strings[sc->fields[a->def->field_start + k].name], name) == 0) return &a->arena[k];
    return NULL;
}
const fsm_pv *a_field_req(act_inst *a, const char *name)
{
    const fsm_pv *p = a_field(a, name);
    if (!p) HKSIM_UNIMPLEMENTED("action %s has no field '%s' in the dump", w_str(a->fsm->w, a->def->type_short), name);
    return p;
}
int32_t a_array_len(act_inst *a, const fsm_pv *pv)
{
    (void)a;
    if (pv->kind == PV_ARRAY) return pv->j;
    if (pv->kind == PV_FARRAY && pv->vmode == VM_LITERAL) return pv->j;
    if (pv->kind == PV_NULL) return 0;
    HKSIM_UNIMPLEMENTED("array access on pv kind %d vmode %d", pv->kind, pv->vmode);
}
int32_t world_array_elems(act_inst *a, const fsm_pv *pv, const fsm_pv **elems)
{
    if (pv->kind == PV_ARRAY || (pv->kind == PV_FARRAY && pv->vmode == VM_LITERAL)) { *elems = &a->arena[pv->i]; return pv->j; }
    if (pv->kind == PV_FARRAY) {                                   /* array variable: its static element range (world.c val_from_pv) */
        fsm_val *v = var_slot(a->fsm, pv);
        if (!v) { *elems = NULL; return 0; }
        *elems = &a->fsm->w->sc->pool[v->i];
        return (int32_t)v->f;
    }
    *elems = NULL;
    return 0;
}
const fsm_pv *a_array_elem(act_inst *a, const fsm_pv *pv, int32_t k)
{
    HKSIM_ASSERT(k >= 0 && k < a_array_len(a, pv), "array index %d out of range", k);
    return a_pv(a, pv->i + k);
}

/* Fsm.GetOwnerDefaultTarget — Fsm.cs:2458-2469 */
int32_t p_owner_default(act_inst *a, const fsm_pv *pv)
{
    if (!pv || pv->kind == PV_NULL) return -1;
    HKSIM_ASSERT(pv->kind == PV_OWNERDEF, "owner default: kind %d", pv->kind);
    if (pv->sub != 0) return pgo(a->fsm, a_pv(a, pv->i));
    return a->fsm->go;
}
/* FsmOwnerDefault.GetSafe — HK/FSMUtility.cs:186-193 (same rule, stateAction.Owner for UseOwner) */
int32_t p_get_safe(act_inst *a, const fsm_pv *pv) { return p_owner_default(a, pv); }
