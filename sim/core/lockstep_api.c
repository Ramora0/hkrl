/* The per-frame lockstep harness's view of the sim (docs/lockstep.md): the sim's state as a table of named fields that
 * hkpy/lockstep.py reads (export) and overwrites (import) with the game's recorded values (docs/state-record.md).
 *
 * An entry is one field of one object, named the way the state record names it: the owning GameObject (its index; the
 * harness pairs it with the recorded GameObject by path and clone index), a component label and the recorded field
 * name.  Values cross as int64: a float as its bit pattern, an int or bool as itself, a name (FSM state, tk2d clip) as
 * its index, a string as the world's string id, a GameObject reference as its index.  Entries are enumerated per
 * GameObject in index order, so a world that grew (runtime clones) keeps every earlier entry's index.
 *
 * Export reads only; hkpy/lockstep.py wraps it in a checkpoint save/restore because some reads resolve the sim's lazy
 * caches (ensure_transform_clean pushes a body's rotation).  Import writes through the setters the ported code uses
 * (SetActive, transform and Rigidbody2D writes, collider enable), so the state they derive follows; fields the
 * setters do not reach are written in place.  What neither direction covers is listed in docs/lockstep.md. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include "lockstep.h"
#include "sim_modules.h"
#include "trap.h"
#include "hero/hero.h"
#include "fsm/fsm.h"
#include "fsm/runtime/world_internal.h"
#include "phys/phys_internal.h"
#include "alloc.h"

enum {   /* entry groups */
    G_TIME, G_RNG, G_HERO_F, G_HERO_CS, G_HERO_PD, G_GVAR,
    G_GO, G_RB, G_COL, G_FSM, G_STATE, G_ACT, G_VAR, G_HM, G_DH, G_RECOIL, G_ANIM, G_PHYS, G_SLASH
};
/* value types (the int64 encoding) */
enum { T_F = 'f', T_I = 'i', T_B = 'b', T_U = 'u', T_E = 'e', T_S = 's', T_G = 'g', T_L = 'l', T_X = 'x' };
enum { F_RO = 1 };   /* export only: derived state, set through the fields it derives from */
#define HKLS_NONE    INT64_MIN   /* the sim has no such value now (compared with nothing) */
#define HKLS_UNKNOWN (-2)        /* a name, string or object the record cannot hold */

typedef struct { uint8_t grp, type, flags, fid; int32_t go, obj, a, b; } ls_entry;

typedef struct hkls {
    hksim *s;
    ls_entry *e; int32_t n, cap;
    int32_t n_gos_done;   /* the enumeration has covered GameObjects [0, n_gos_done) */
    char buf[1024];
    struct { int32_t fsm, state, act; char name[48]; float v; } *live; int32_t n_live, cap_live;
    hksim_trap_ctx trap;
    char err[600];
} hkls;

static fsm_world *W(hkls *L) { hksim_parts p; hksim_get_parts(L->s, &p); return fsmi_world(p.fsm_ctx); }

#define LS_ENTER(L, fail) \
    hksim_parts P; hksim_get_parts((L)->s, &P); \
    hks_arena *volatile _prev = hks_arena_bind(P.arena); \
    if (setjmp((L)->trap.jb) != 0) { snprintf((L)->err, sizeof (L)->err, "%s", hksim_trap_message()); hksim_trap_disarm(); \
                                      hks_arena_bind(_prev); return (fail); } \
    hksim_trap_arm(&(L)->trap)
#define LS_LEAVE() do { hksim_trap_disarm(); hks_arena_bind(_prev); } while (0)

static void add(hkls *L, int grp, int type, int flags, int fid, int32_t go, int32_t obj, int32_t a, int32_t b)
{
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 4096;
        L->e = hks_sys_realloc(L->e, sizeof(ls_entry) * (size_t)L->cap);
    }
    ls_entry *e = &L->e[L->n++];
    e->grp = (uint8_t)grp; e->type = (uint8_t)type; e->flags = (uint8_t)flags; e->fid = (uint8_t)fid;
    e->go = go; e->obj = obj; e->a = a; e->b = b;
}

/* ---- field names per group ---- */
static const char *const GO_F[] = { "exists", "activeSelf", "activeInHierarchy", "layer", "tag",
    "lp.x", "lp.y", "lp.z", "ls.x", "ls.y", "ls.z", "leuler.z", "wp.x", "wp.y", "wp.z", "lossy.x", "lossy.y", "lossy.z", "euler.z" };
enum { GO_EXISTS, GO_ACTIVE, GO_AIH, GO_LAYER, GO_TAG, GO_LP, GO_LS = GO_LP + 3, GO_LEULER = GO_LS + 3, GO_WP, GO_LOSSY = GO_WP + 3,
       GO_EULER = GO_LOSSY + 3, GO_N };
static const char *const RB_F[] = { "hasBody", "position.x", "position.y", "velocity.x", "velocity.y", "rotation", "gravityScale", "bodyType" };
enum { RB_HAS, RB_PX, RB_PY, RB_VX, RB_VY, RB_ROT, RB_GRAV, RB_TYPE, RB_N };
static const char *const COL_F[] = { "enabled", "isTrigger", "offset.x", "offset.y", "size.x", "size.y", "radius", "n.proxies" };
enum { COL_EN, COL_TRIG, COL_OX, COL_OY, COL_SX, COL_SY, COL_RAD, COL_PROX, COL_N };
static const char *const FSM_F[] = { "enabled", "fsm.active", "fsm.previous", "fsm.switchTo", "fsm.started", "fsm.finished",
    "fsm.activeStateEntered", "fsm.switchedState" };
enum { FL_EN, FL_ACTIVE, FL_PREV, FL_SWITCH, FL_STARTED, FL_FINISHED, FL_ENTERED, FL_SWITCHED, FL_N };
static const char *const ST_F[] = { "active", "finished", "activeActionIndex", "stateTime", "loopCount", "maxLoopCount",
    "activeAction", "activeActions", "finishedActions" };
enum { ST_ACTIVE, ST_FINISHED, ST_AAI, ST_TIME, ST_LOOP, ST_MAXLOOP, ST_AA, ST_AACTS, ST_FACTS, ST_N };
static const char *const ACT_F[] = { "base.enabled", "base.active", "base.finished", "base.entered" };
static const char *const HM_F[] = { "hp", "isDead", "invincible", "invincibleFromDirection", "evasionByHitRemaining", "directionOfLastAttack" };
static const char *const DH_F[] = { "damageDealt", "enabled" };
static const char *const RC_F[] = { "recoilSpeedBase", "state", "recoilTimeRemaining", "recoilSpeed", "isRecoilSweeping" };
static const char *const AN_F[] = { "enabled", "clipTime", "clipFps", "previousFrame", "state", "tk2d.clip", "tk2d.currentFrame", "tk2d.playing" };
enum { AN_EN, AN_TIME, AN_FPS, AN_PREV, AN_STATE, AN_CLIP, AN_CUR, AN_PLAYING };
/* NailSlash (sim/hero/hero.h hero_nailslash; NS:20-38) */
static const char *const NS_F[] = { "slashAngle", "slashing", "stepCounter", "polyCounter", "animCompleted", "longnail", "mantis", "fury" };
enum { NS_ANGLE, NS_SLASHING, NS_STEP, NS_POLY, NS_ANIMDONE, NS_LONG, NS_MANTIS, NS_FURY, NS_N };
static const char *const TIME_F[] = { "frameCount", "time", "fixedTime", "timeSinceLevelLoad", "fixedCount", "step" };
static const char *const PHYS_F[] = { "contactList", "collisions" };
/* FsmVariables buckets as the record names them (docs/state-record.md FsmVariables: `<kind>:<name>`) */
static const char *const VB_KIND[VB_COUNT] = { "float", "int", "bool", "string", "vector2", "vector3", NULL, "quaternion",
                                               "color", "gameObject", NULL, "enum", NULL, NULL, NULL };
static int vb_width(int b) { return b == VB_V2 ? 2 : b == VB_V3 ? 3 : (b == VB_QUAT || b == VB_COLOR) ? 4 : 1; }
static int vb_type(int b) { return b == VB_FLOAT || b == VB_V2 || b == VB_V3 || b == VB_QUAT || b == VB_COLOR ? T_F
                                 : b == VB_BOOL ? T_B : b == VB_STRING ? T_S : b == VB_GO ? T_G : T_I; }
static const char *const COL_TYPE[] = { "BoxCollider2D", "CircleCollider2D", "PolygonCollider2D", "EdgeCollider2D", "CapsuleCollider2D" };

static int bucket_of(const int32_t *start, int32_t i)
{
    int b = 0;
    while (b < VB_COUNT && !(i >= start[b] && i < start[b + 1])) b++;
    return b;
}

/* ---- enumeration ---- */
static void enum_vars(hkls *L, int grp, int32_t go, int32_t obj, const int32_t *bstart, int32_t n)
{
    for (int32_t i = 0; i < n; i++) {
        int b = bucket_of(bstart, i);
        if (b >= VB_COUNT || !VB_KIND[b]) continue;
        for (int k = 0; k < vb_width(b); k++) add(L, grp, vb_type(b), 0, 0, go, obj, i, k);
    }
}

static void enum_go(hkls *L, fsm_world *w, int32_t g)
{
    const go_inst *gi = &w->gos[g];
    if (gi->def->asset) return;
    for (int f = 0; f < GO_N; f++) {
        if (f >= GO_LP && !gi->has_transform) break;
        int ty = f == GO_EXISTS || f == GO_ACTIVE || f == GO_AIH ? T_B : f == GO_LAYER ? T_I : f == GO_TAG ? T_S : T_F;
        int ro = f == GO_EXISTS || f == GO_AIH || f >= GO_WP;
        add(L, G_GO, ty, ro ? F_RO : 0, f, g, g, 0, 0);
    }
    if ((gi->def->rb >= 0 || g == w->knight_go) && gi->has_transform)
        for (int f = 0; f < RB_N; f++) add(L, G_RB, f == RB_HAS ? T_B : f == RB_TYPE ? T_I : T_F, f == RB_HAS || f == RB_ROT ? F_RO : 0, f, g, g, 0, 0);
    for (int32_t k = 0; k < gi->n_cols; k++) {
        int ct = gi->cols[k].def->type;
        for (int f = 0; f < COL_N; f++) {
            if ((f == COL_SX || f == COL_SY) && ct != COL_BOX) continue;
            if (f == COL_RAD && ct != COL_CIRCLE) continue;
            add(L, G_COL, f <= COL_TRIG ? T_B : f == COL_PROX ? T_L : T_F, f == COL_PROX ? F_RO : 0, f, g, g, k, 0);
        }
    }
    const go_def *d = gi->def;
    for (int32_t k = 0; k < d->n_fsms; k++) {
        int32_t id = w->sc->fsm_idx[d->fsm_start + k];
        const fsm_inst *fi = &w->fsms[id];
        for (int f = 0; f < FL_N; f++)
            add(L, G_FSM, f == FL_ACTIVE || f == FL_PREV || f == FL_SWITCH ? T_E : T_B, 0, f, g, id, 0, 0);
        for (int32_t s = 0; s < fi->n_states; s++) {
            for (int f = 0; f < ST_N; f++)
                add(L, G_STATE, f <= ST_FINISHED ? T_B : f == ST_TIME ? T_F : f >= ST_AACTS ? T_L : T_I, 0, f, g, id, s, 0);
            for (int32_t a = 0; a < fi->states[s].n_acts; a++)
                for (int f = 0; f < 4; f++) add(L, G_ACT, T_B, 0, f, g, id, s, a);
        }
        enum_vars(L, G_VAR, g, id, fi->def->var_bucket_start, fi->n_vals);
    }
    if (gi->hm >= 0) for (int f = 0; f < 6; f++) add(L, G_HM, f == 4 ? T_F : (f == 1 || f == 2) ? T_B : T_I, 0, f, g, gi->hm, 0, 0);
    if (gi->dh >= 0) for (int f = 0; f < 2; f++) add(L, G_DH, f ? T_B : T_I, 0, f, g, gi->dh, 0, 0);
    if (gi->recoil >= 0) for (int f = 0; f < 5; f++) add(L, G_RECOIL, f == 1 ? T_I : f == 4 ? T_B : T_F, 0, f, g, gi->recoil, 0, 0);
    int32_t si = world_slash_index_of(w, g);
    if (si >= 0)
        for (int f = 0; f < NS_N; f++) add(L, G_SLASH, f == NS_ANGLE ? T_F : f == NS_STEP || f == NS_POLY ? T_I : T_B, 0, f, g, si, 0, 0);
    if (gi->anim >= 0)
        for (int f = 0; f <= AN_PLAYING; f++)
            add(L, G_ANIM, f == AN_EN || f == AN_PLAYING ? T_B : f == AN_TIME || f == AN_FPS ? T_F : f == AN_CLIP ? T_E : T_I,
                f >= AN_CUR ? F_RO : 0, f, g, gi->anim, 0, 0);
}

/* Extend the table over GameObjects created since the last call; the table's count. */
HKSIM_API int32_t hkls_refresh(hkls *L)
{
    LS_ENTER(L, -1);
    fsm_world *w = fsmi_world(P.fsm_ctx);
    if (L->n == 0) {
        for (int f = 0; f < 6; f++) add(L, G_TIME, f == 1 || f == 2 || f == 3 ? T_F : f == 5 ? T_I : T_U, 0, f, -1, 0, 0, 0);
        for (int k = 0; k < 4; k++) add(L, G_RNG, T_I, 0, k, -1, 0, k, 0);
        uint32_t n;
        const hero_field_desc *t = hero_field_table(&n);
        for (uint32_t i = 0; i < n; i++) add(L, G_HERO_F, t[i].code == 'f' ? T_F : t[i].code == 'b' ? T_B : T_I, 0, 0, w->knight_go, 0, (int32_t)i, 0);
        (void)hero_cstate_names(&n);
        for (uint32_t i = 0; i < n; i++) add(L, G_HERO_CS, T_B, 0, 0, w->knight_go, 0, (int32_t)i, 0);
        t = hero_pd_field_table(&n);
        for (uint32_t i = 0; i < n; i++) add(L, G_HERO_PD, t[i].code == 'f' ? T_F : t[i].code == 'b' ? T_B : T_I, 0, 0, -1, 0, (int32_t)i, 0);
        enum_vars(L, G_GVAR, -1, 0, w->sc->global_bucket_start, w->n_gvals);
        for (int f = 0; f < 2; f++) add(L, G_PHYS, T_X, F_RO, f, -1, 0, 0, 0);
    }
    for (; L->n_gos_done < w->n_gos; L->n_gos_done++) enum_go(L, w, L->n_gos_done);
    LS_LEAVE();
    return L->n;
}

HKSIM_API hkls *hkls_open(hksim *s)
{
    hkls *L = hks_sys_calloc(1, sizeof *L);
    L->s = s;
    if (hkls_refresh(L) < 0) { hks_sys_free(L->e); hks_sys_free(L); return NULL; }
    return L;
}
HKSIM_API void hkls_close(hkls *L) { if (L) { hks_sys_free(L->e); hks_sys_free(L->live); hks_sys_free(L); } }
HKSIM_API const char *hkls_error(hkls *L) { return L ? L->err : ""; }

/* ---- naming ---- */
static int ord_same_fsm(const fsm_world *w, int32_t id)
{
    const fsm_inst *f = &w->fsms[id];
    const go_def *d = w->gos[f->go].def;
    int o = 0;
    for (int32_t k = 0; k < d->n_fsms; k++) {
        int32_t j = w->sc->fsm_idx[d->fsm_start + k];
        if (j == id) break;
        if (w->fsms[j].def->fsm_name == f->def->fsm_name) o++;
    }
    return o;
}
static int ord_same_col(const fsm_world *w, int32_t go, int32_t k)
{
    int o = 0;
    for (int32_t j = 0; j < k; j++) if (w->gos[go].cols[j].def->type == w->gos[go].cols[k].def->type) o++;
    return o;
}
static const char *col_type(const fsm_world *w, int32_t go, int32_t k)
{
    int t = w->gos[go].cols[k].def->type;
    return t >= 0 && t <= COL_CAPSULE ? COL_TYPE[t] : "Collider2D";
}

/* out: {owner GameObject (-1 global), value type, flags}; *comp the component label, *field the recorded field name.
 * The strings live until the next call. */
HKSIM_API int hkls_entry(hkls *L, int32_t i, int32_t out[3], const char **comp, const char **field)
{
    if (i < 0 || i >= L->n) return HKSIM_ERR_BAD_ARG;
    fsm_world *w = W(L);
    const ls_entry *e = &L->e[i];
    out[0] = e->go; out[1] = e->type; out[2] = e->flags;
    char *b = L->buf; size_t cap = sizeof L->buf / 2; char *fb = L->buf + cap;
    *comp = b; *field = fb; b[0] = fb[0] = 0;
    uint32_t n;
    switch (e->grp) {
    case G_TIME: snprintf(b, cap, "%s", e->fid >= 4 ? "Env" : "Time"); snprintf(fb, cap, "%s", TIME_F[e->fid]); break;
    case G_RNG: snprintf(b, cap, "Random"); snprintf(fb, cap, "state#%d", e->a); break;
    case G_HERO_F: snprintf(b, cap, "HeroController"); snprintf(fb, cap, "%s", hero_field_table(&n)[e->a].name); break;
    case G_HERO_CS: snprintf(b, cap, "cState"); snprintf(fb, cap, "%s", hero_cstate_names(&n)[e->a]); break;
    case G_HERO_PD: snprintf(b, cap, "PlayerData"); snprintf(fb, cap, "%s", hero_pd_field_table(&n)[e->a].name); break;
    case G_GVAR: case G_VAR: {
        const int32_t *bs = e->grp == G_GVAR ? w->sc->global_bucket_start : w->fsms[e->obj].def->var_bucket_start;
        int32_t name = e->grp == G_GVAR ? w->sc->globals[e->a].name : w->sc->vars[w->fsms[e->obj].def->var_start + e->a].name;
        int bk = bucket_of(bs, e->a), wd = vb_width(bk);
        static const char *const XYZW = "xyzw", *const RGBA = "rgba";
        if (e->grp == G_GVAR) snprintf(b, cap, "PlayMakerGlobals/V");
        else snprintf(b, cap, "FSM:%s#%d/V", w_str(w, w->fsms[e->obj].def->fsm_name), ord_same_fsm(w, e->obj));
        if (wd == 1) snprintf(fb, cap, "%s:%s", VB_KIND[bk], w_str(w, name));
        else snprintf(fb, cap, "%s:%s.%c", VB_KIND[bk], w_str(w, name), (bk == VB_COLOR ? RGBA : XYZW)[e->b]);
        break;
    }
    case G_GO: snprintf(b, cap, "GameObject"); snprintf(fb, cap, "%s", GO_F[e->fid]); break;
    case G_RB: snprintf(b, cap, "Rigidbody2D"); snprintf(fb, cap, "%s", RB_F[e->fid]); break;
    case G_COL: snprintf(b, cap, "%s#%d", col_type(w, e->go, e->a), ord_same_col(w, e->go, e->a)); snprintf(fb, cap, "%s", COL_F[e->fid]); break;
    case G_FSM: case G_STATE: case G_ACT: {
        int o = snprintf(b, cap, "FSM:%s#%d", w_str(w, w->fsms[e->obj].def->fsm_name), ord_same_fsm(w, e->obj));
        if (e->grp != G_FSM) o += snprintf(b + o, cap - (size_t)o, "/S%d:%s", e->a, state_name(&w->fsms[e->obj], e->a));
        if (e->grp == G_ACT) snprintf(b + o, cap - (size_t)o, "/A%d", e->b);
        snprintf(fb, cap, "%s", e->grp == G_FSM ? FSM_F[e->fid] : e->grp == G_STATE ? ST_F[e->fid] : ACT_F[e->fid]);
        break;
    }
    case G_HM: snprintf(b, cap, "HealthManager"); snprintf(fb, cap, "%s", HM_F[e->fid]); break;
    case G_DH: snprintf(b, cap, "DamageHero"); snprintf(fb, cap, "%s", DH_F[e->fid]); break;
    case G_RECOIL: snprintf(b, cap, "Recoil"); snprintf(fb, cap, "%s", RC_F[e->fid]); break;
    case G_ANIM: snprintf(b, cap, "tk2dSpriteAnimator"); snprintf(fb, cap, "%s", AN_F[e->fid]); break;
    case G_SLASH: snprintf(b, cap, "NailSlash"); snprintf(fb, cap, "%s", NS_F[e->fid]); break;
    case G_PHYS: snprintf(b, cap, "%s", e->fid ? "PhysicsContacts2D" : "b2World"); snprintf(fb, cap, "%s", PHYS_F[e->fid]); break;
    default: return HKSIM_ERR_BAD_ARG;
    }
    return HKSIM_OK;
}

/* ---- values ---- */
static int64_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return (int64_t)u; }
static float bitsf(int64_t v) { uint32_t u = (uint32_t)v; float f; memcpy(&f, &u, 4); return f; }
/* a list of ints as one value: FNV-1a 64 over its count and elements (little-endian 32-bit words), top two bits
 * clear; hkls_list reads the list itself and hkls_import_list writes one */
static int64_t hash_list(const int32_t *v, int32_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (int32_t k = -1; k < n; k++) {
        uint32_t x = (uint32_t)(k < 0 ? n : v[k]);
        for (int j = 0; j < 4; j++) { h ^= (x >> (8 * j)) & 0xff; h *= 0x100000001b3ull; }
    }
    return (int64_t)(h >> 2);
}
static int32_t col_of_shape(const fsm_world *w, int32_t go, uint32_t shape)
{
    if (go < 0) return -1;
    for (int32_t k = 0; k < w->gos[go].n_cols; k++) if (w->gos[go].cols[k].shape == shape) return k;
    return -1;
}
static phys_body_id body_of(const fsm_world *w, int32_t go) { return w->gos[go].body; }

static int64_t get_value(hkls *L, const hksim_parts *P, fsm_world *w, const ls_entry *e)
{
    const hero *H = P->hero;
    uint32_t n;
    switch (e->grp) {
    case G_TIME:
        switch (e->fid) {
        case 0: return *P->frame;
        case 1: return fbits(*P->time);
        case 2: return fbits(*P->fixed_time);
        case 3: return fbits(*P->tsll);
        case 4: return *P->fixed_count;
        default: return (int32_t)*P->step;
        }
    case G_RNG: return (int32_t)(e->a == 0 ? P->rng->x : e->a == 1 ? P->rng->y : e->a == 2 ? P->rng->z : P->rng->w);
    case G_HERO_F: case G_HERO_PD: {
        const hero_field_desc *t = e->grp == G_HERO_F ? hero_field_table(&n) : hero_pd_field_table(&n);
        const char *base = (e->grp == G_HERO_F ? (const char *)&H->f : (const char *)&H->pd) + t[e->a].offset;
        return t[e->a].code == 'f' ? fbits(*(const float *)base) : t[e->a].code == 'b' ? *(const uint8_t *)base : *(const int32_t *)base;
    }
    case G_HERO_CS: return hero_get_state(H, hero_cstate_names(&n)[e->a]) != 0;
    case G_GVAR: case G_VAR: {
        const fsm_val *v = e->grp == G_GVAR ? &w->gvals[e->a] : &w->fsms[e->obj].vals[e->a];
        int bk = bucket_of(e->grp == G_GVAR ? w->sc->global_bucket_start : w->fsms[e->obj].def->var_bucket_start, e->a);
        if (e->type == T_F) return fbits(vb_width(bk) == 1 ? v->f : v->v[e->b]);
        if (e->type == T_B) return v->i != 0;
        if (e->type == T_G && v->i >= 0 && v->i < w->n_gos && w->gos[v->i].def->asset) return HKLS_UNKNOWN;   /* a prefab: not in the record */
        return v->i;
    }
    case G_GO: {
        go_inst *g = &w->gos[e->go];
        float v3[3];
        switch (e->fid) {
        case GO_EXISTS: return !g->destroyed;
        case GO_ACTIVE: return g->active_self;
        case GO_AIH: return g->active_in_hierarchy;
        case GO_LAYER: return go_layer(w, e->go);
        case GO_TAG: return go_tag(w, e->go);
        case GO_LEULER: return fbits(go_local_euler_z(w, e->go));
        case GO_EULER: return fbits(go_euler_z(w, e->go));
        default: break;
        }
        if (e->fid < GO_LS) { go_local_pos(w, e->go, v3); return fbits(v3[e->fid - GO_LP]); }
        if (e->fid < GO_LEULER) { go_local_scale(w, e->go, v3); return fbits(v3[e->fid - GO_LS]); }
        if (e->fid < GO_LOSSY) { go_world_pos(w, e->go, v3); return fbits(v3[e->fid - GO_WP]); }
        go_lossy_scale(w, e->go, v3); return fbits(v3[e->fid - GO_LOSSY]);
    }
    case G_RB: {
        phys_body_id b = body_of(w, e->go);
        /* Unity has a b2Body only while the Rigidbody2D is simulated in an active GameObject; the sim keeps an
         * inactive one unsimulated.  Without one the record reads the Transform (docs/state-record.md Rigidbody2D) and
         * the body's own fields have no value to compare (HKLS_NONE). */
        int has = b && P->pw->bodies[b].simulated;
        if (e->fid == RB_HAS) return has;
        if (!has) {
            float v3[3];
            if (e->fid == RB_PX || e->fid == RB_PY) { go_world_pos(w, e->go, v3); return fbits(v3[e->fid - RB_PX]); }
            if (e->fid == RB_ROT) return fbits(go_euler_z(w, e->go));
            return HKLS_NONE;
        }
        const body_t *bd = &P->pw->bodies[b];
        switch (e->fid) {
        case RB_PX: return fbits(bd->p.x);
        case RB_PY: return fbits(bd->p.y);
        case RB_VX: return fbits(bd->v.x);
        case RB_VY: return fbits(bd->v.y);
        case RB_ROT: return fbits(bd->rot_deg);
        case RB_GRAV: return fbits(bd->gravity_scale);
        default: return bd->type == PHYS_BODY_DYNAMIC ? 0 : bd->type == PHYS_BODY_KINEMATIC ? 1 : 2;   /* RigidbodyType2D */
        }
    }
    case G_COL: {
        const col_inst *c = &w->gos[e->go].cols[e->a];
        switch (e->fid) {
        case COL_EN: return c->enabled;
        case COL_TRIG: return c->is_trigger;
        case COL_OX: return fbits(c->offset[0]);
        case COL_OY: return fbits(c->offset[1]);
        case COL_SX: return fbits(c->size[0]);
        case COL_SY: return fbits(c->size[1]);
        case COL_RAD: return fbits(c->radius);
        default: {
            if (!c->shape) return hash_list(NULL, 0);
            const shape_t *sh = &P->pw->shapes[c->shape];
            return hash_list(sh->proxies, sh->proxies ? sh->n_proxies : 0);
        }
        }
    }
    case G_FSM: {
        const fsm_inst *f = &w->fsms[e->obj];
        switch (e->fid) {
        case FL_EN: return f->component_enabled;
        case FL_ACTIVE: return f->active_state;
        case FL_PREV: return f->previous_state;
        case FL_SWITCH: return f->switch_to;
        case FL_STARTED: return f->started;
        case FL_FINISHED: return f->finished;
        case FL_ENTERED: return f->active_state_entered;
        default: return f->switched_state;
        }
    }
    case G_STATE: {
        const state_inst *s = &w->fsms[e->obj].states[e->a];
        switch (e->fid) {
        case ST_ACTIVE: return s->active;
        case ST_FINISHED: return s->finished;
        case ST_AAI: return s->active_action_index;
        case ST_TIME: return fbits(s->state_time);
        case ST_LOOP: return s->loop_count;
        case ST_MAXLOOP: return s->max_loop_count_seen;
        case ST_AA: return s->active_action;
        case ST_AACTS: return hash_list(s->active_actions, s->n_active);
        default: return hash_list(s->finished_actions, s->n_finished);
        }
    }
    case G_ACT: {
        const act_inst *a = &w->fsms[e->obj].states[e->a].acts[e->b];
        return e->fid == 0 ? a->enabled : e->fid == 1 ? a->active : e->fid == 2 ? a->finished : a->entered;
    }
    case G_HM: {
        const hm_inst *h = &w->hms[e->obj];
        switch (e->fid) {
        case 0: return h->hp;
        case 1: return h->is_dead;
        case 2: return h->invincible;
        case 3: return h->invincible_from_direction;
        case 4: return fbits(h->evasion_by_hit_remaining);
        default: return h->direction_of_last_attack;
        }
    }
    case G_DH: return e->fid ? w->dhs[e->obj].enabled : w->dhs[e->obj].damage_dealt;
    case G_RECOIL: {
        const recoil_inst *r = &w->recoils[e->obj];
        switch (e->fid) {
        case 0: return fbits(r->speed_base);
        case 1: return r->state;
        case 2: return fbits(r->time_remaining);
        case 3: return fbits(r->speed);
        default: return r->is_sweeping;
        }
    }
    case G_ANIM: {
        const anim_inst *a = &w->anims[e->obj];
        switch (e->fid) {
        case AN_EN: return a->enabled;
        case AN_TIME: return fbits(a->clip_time);
        case AN_FPS: return fbits(a->clip_fps);
        case AN_PREV: return a->previous_frame;
        case AN_STATE: return a->state;
        case AN_CLIP: return a->cur_clip;
        case AN_CUR: return a->cur_clip >= 0 ? anim_current_frame(w, a) : -1;   /* the getter throws without a clip; the record writes -1 */
        default: return anim_playing(a);
        }
    }
    case G_SLASH: {
        const hero_nailslash *n = &H->slash[e->obj];
        switch (e->fid) {
        case NS_ANGLE: return fbits(n->slashAngle);
        case NS_SLASHING: return n->slashing;
        case NS_STEP: return n->stepCounter;
        case NS_POLY: return n->polyCounter;
        case NS_ANIMDONE: return n->animCompleted;
        case NS_LONG: return n->longnail;
        case NS_MANTIS: return n->mantis;
        default: return n->fury;
        }
    }
    default: return 0;
    }
    (void)L;
}

/* Every entry's value into out[0..n). */
HKSIM_API int hkls_export(hkls *L, int64_t *out, int32_t n)
{
    if (n > L->n) return HKSIM_ERR_BAD_ARG;
    LS_ENTER(L, HKSIM_ERR_INTERNAL);
    fsm_world *w = fsmi_world(P.fsm_ctx);
    for (int32_t i = 0; i < n; i++) out[i] = get_value(L, &P, w, &L->e[i]);
    LS_LEAVE();
    return HKSIM_OK;
}

/* ---- structure entries (T_X): a sequence of identities as text, GameObjects by index ---- */
static void shape_ident(const hksim_parts *P, fsm_world *w, uint32_t shape, char *out, size_t cap)
{
    uint32_t user = phys_shape_user(P->pw, shape);
    int32_t go = user == HKSIM_USER_HERO ? w->knight_go : world_go_of_user(w, user);
    int32_t k = col_of_shape(w, go, shape);
    if (go >= 0 && k >= 0) snprintf(out, cap, "%d/%s#%d", go, col_type(w, go, k), ord_same_col(w, go, k));
    else if (user == HKSIM_USER_HERO) snprintf(out, cap, "%d/BoxCollider2D#0", go);
    else snprintf(out, cap, "?user%u", user);
}
static const char *struct_value(hkls *L, const hksim_parts *P, fsm_world *w, const ls_entry *e)
{
    static char big[65536];
    size_t o = 0; big[0] = 0;
    char a[128], b[128];
    const phys_world *pw = P->pw;
    if (e->fid == 0) {   /* b2ContactManager::m_contactList, head first */
        for (int32_t c = pw->contact_list; c >= 0 && o < sizeof big - 300; c = pw->contacts[c].next) {
            const contact_t *ct = &pw->contacts[c];
            shape_ident(P, w, ct->sa, a, sizeof a); shape_ident(P, w, ct->sb, b, sizeof b);
            o += (size_t)snprintf(big + o, sizeof big - o, "%s%s|%s", o ? ";" : "", a, b);
        }
    } else {             /* PhysicsContacts2D::m_Collisions, with each record's state */
        for (uint32_t k = 0; k < pw->n_colls && o < sizeof big - 300; k++) {
            const coll_t *c = &pw->coll_pool[pw->colls[k]];
            shape_ident(P, w, c->sa, a, sizeof a); shape_ident(P, w, c->sb, b, sizeof b);
            o += (size_t)snprintf(big + o, sizeof big - o, "%s%s|%s:%d", o ? ";" : "", a, b, c->state);
        }
    }
    (void)L;
    return big;
}
HKSIM_API const char *hkls_struct(hkls *L, int32_t i)
{
    if (i < 0 || i >= L->n || L->e[i].type != T_X) return NULL;
    LS_ENTER(L, NULL);
    const char *r = struct_value(L, &P, fsmi_world(P.fsm_ctx), &L->e[i]);
    LS_LEAVE();
    return r;
}

/* ---- names of enum and string values ---- */
static int32_t state_by_name(const fsm_inst *f, const char *name)
{
    for (int32_t s = 0; s < f->n_states; s++) if (strcmp(state_name(f, s), name) == 0) return s;
    return -2;
}
static int32_t clip_by_name(const fsm_world *w, const anim_inst *a, const char *name)
{
    if (a->lib < 0) return -2;
    const anim_lib_def *lb = &w->sc->libs[a->lib];
    for (int32_t i = 0; i < lb->n_clips; i++) if (strcmp(w_str(w, w->sc->clips[lb->clip_start + i].name), name) == 0) return lb->clip_start + i;
    return -2;
}
/* The value a name stands for in entry i (T_E: FSM state / tk2d clip; T_S: a string, interned when `intern`); "" is
 * -1 (none); a name the sim does not have is -2. */
HKSIM_API int hkls_value_of(hkls *L, int32_t i, const char *name, int32_t intern, int64_t *out)
{
    if (i < 0 || i >= L->n) return HKSIM_ERR_BAD_ARG;
    LS_ENTER(L, HKSIM_ERR_INTERNAL);
    fsm_world *w = fsmi_world(P.fsm_ctx);
    const ls_entry *e = &L->e[i];
    if (e->type == T_S) *out = intern ? w_intern(w, name) : w_find_string(w, name) >= 0 ? w_find_string(w, name) : -2;
    else if (!name[0]) *out = -1;
    else if (e->grp == G_ANIM) *out = clip_by_name(w, &w->anims[e->obj], name);
    else *out = state_by_name(&w->fsms[e->obj], name);
    LS_LEAVE();
    return HKSIM_OK;
}
HKSIM_API const char *hkls_name_of(hkls *L, int32_t i, int64_t v)
{
    if (i < 0 || i >= L->n) return NULL;
    fsm_world *w = W(L);
    const ls_entry *e = &L->e[i];
    if (v < 0) return "";
    if (e->type == T_S) return w_str(w, (int32_t)v);
    if (e->grp == G_ANIM) return v < w->sc->n_clips ? w_str(w, w->sc->clips[v].name) : "";
    return state_name(&w->fsms[e->obj], (int32_t)v);
}

/* ---- import ---- */
static void set_value(const hksim_parts *P, fsm_world *w, const ls_entry *e, int64_t v)
{
    hero *H = P->hero;
    uint32_t n;
    float fv = bitsf(v);
    int32_t iv = (int32_t)v;
    switch (e->grp) {
    case G_TIME:
        switch (e->fid) {
        case 0: *P->frame = (uint32_t)v; break;
        case 1: *P->time = fv; break;
        case 2: *P->fixed_time = fv; break;
        case 3: *P->tsll = fv; break;
        case 4: *P->fixed_count = (uint32_t)v; break;
        default: *P->step = (uint32_t)iv; break;
        }
        return;
    case G_RNG: *(e->a == 0 ? &P->rng->x : e->a == 1 ? &P->rng->y : e->a == 2 ? &P->rng->z : &P->rng->w) = (uint32_t)iv; return;
    case G_HERO_F: case G_HERO_PD: {
        const hero_field_desc *t = e->grp == G_HERO_F ? hero_field_table(&n) : hero_pd_field_table(&n);
        char *base = (e->grp == G_HERO_F ? (char *)&H->f : (char *)&H->pd) + t[e->a].offset;
        if (t[e->a].code == 'f') *(float *)base = fv; else if (t[e->a].code == 'b') *(uint8_t *)base = v != 0; else *(int32_t *)base = iv;
        return;
    }
    case G_HERO_CS: hero_set_cstate(H, hero_cstate_names(&n)[e->a], v != 0); return;
    case G_GVAR: case G_VAR: {
        fsm_val *x = e->grp == G_GVAR ? &w->gvals[e->a] : &w->fsms[e->obj].vals[e->a];
        int bk = bucket_of(e->grp == G_GVAR ? w->sc->global_bucket_start : w->fsms[e->obj].def->var_bucket_start, e->a);
        if (e->type == T_F) { if (vb_width(bk) == 1) x->f = fv; else x->v[e->b] = fv; }
        else x->i = e->type == T_B ? v != 0 : iv;
        return;
    }
    case G_GO: {
        float v3[3];
        switch (e->fid) {
        case GO_ACTIVE: go_set_active(w, e->go, v != 0); return;
        case GO_LAYER:   /* the Knight's layer is its body's (go_layer), which sim/hero writes */
            if (e->go == w->knight_go && P->hero_body) phys_body_set_layer(P->pw, P->hero_body, (uint32_t)iv);
            else go_set_layer(w, e->go, iv);
            return;
        case GO_TAG: w->gos[e->go].tag_override = iv; return;
        case GO_LEULER: go_set_local_euler_z(w, e->go, fv); return;
        default: break;
        }
        if (e->fid >= GO_LP && e->fid < GO_LS) { go_local_pos(w, e->go, v3); v3[e->fid - GO_LP] = fv; go_set_local_pos(w, e->go, v3); }
        else if (e->fid >= GO_LS && e->fid < GO_LEULER) { go_local_scale(w, e->go, v3); v3[e->fid - GO_LS] = fv; go_set_local_scale(w, e->go, v3); }
        return;
    }
    case G_RB: {
        phys_body_id b = body_of(w, e->go);
        if (!b) return;
        phys_v2 p = phys_body_position(P->pw, b), vel = phys_body_velocity(P->pw, b);
        switch (e->fid) {
        case RB_PX: p.x = fv; phys_body_set_position(P->pw, b, p); invalidate_transform_dfs(w, e->go); break;
        case RB_PY: p.y = fv; phys_body_set_position(P->pw, b, p); invalidate_transform_dfs(w, e->go); break;
        case RB_VX: vel.x = fv; phys_body_set_velocity(P->pw, b, vel); break;
        case RB_VY: vel.y = fv; phys_body_set_velocity(P->pw, b, vel); break;
        case RB_GRAV: phys_body_set_gravity_scale(P->pw, b, fv); break;
        case RB_TYPE: phys_body_set_type(P->pw, b, iv == 0 ? PHYS_BODY_DYNAMIC : iv == 1 ? PHYS_BODY_KINEMATIC : PHYS_BODY_STATIC); break;
        default: break;
        }
        return;
    }
    case G_COL: {
        col_inst *c = &w->gos[e->go].cols[e->a];
        switch (e->fid) {
        case COL_EN: col_set_enabled(w, c, v != 0); break;
        case COL_TRIG: col_set_trigger(w, c, v != 0); break;
        case COL_OX: case COL_OY: case COL_SX: case COL_SY: {
            float off[2] = { c->offset[0], c->offset[1] }, sz[2] = { c->size[0], c->size[1] };
            if (e->fid == COL_OX) off[0] = fv; else if (e->fid == COL_OY) off[1] = fv; else if (e->fid == COL_SX) sz[0] = fv; else sz[1] = fv;
            if (c->def->type == COL_BOX) col_set_box(w, e->go, c, sz, off); else { c->offset[0] = off[0]; c->offset[1] = off[1]; }
            break;
        }
        case COL_RAD: c->radius = fv; break;
        default: break;
        }
        return;
    }
    case G_FSM: {
        fsm_inst *f = &w->fsms[e->obj];
        switch (e->fid) {
        case FL_EN: f->component_enabled = v != 0; break;
        case FL_ACTIVE: f->active_state = iv; break;
        case FL_PREV: f->previous_state = iv; break;
        case FL_SWITCH: f->switch_to = iv; break;
        case FL_STARTED: f->started = v != 0; break;
        case FL_FINISHED: f->finished = v != 0; break;
        case FL_ENTERED: f->active_state_entered = v != 0; break;
        default: f->switched_state = v != 0; break;
        }
        return;
    }
    case G_STATE: {
        state_inst *s = &w->fsms[e->obj].states[e->a];
        switch (e->fid) {
        case ST_ACTIVE: s->active = v != 0; break;
        case ST_FINISHED: s->finished = v != 0; break;
        case ST_AAI: s->active_action_index = iv; break;
        case ST_TIME: s->state_time = fv; break;
        case ST_LOOP: s->loop_count = iv; break;
        case ST_MAXLOOP: s->max_loop_count_seen = iv; break;
        case ST_AA: s->active_action = iv; break;
        default: break;   /* lists: hkls_import_list */
        }
        return;
    }
    case G_ACT: {
        act_inst *a = &w->fsms[e->obj].states[e->a].acts[e->b];
        uint8_t b = v != 0;
        if (e->fid == 0) a->enabled = b; else if (e->fid == 1) a->active = b; else if (e->fid == 2) a->finished = b; else a->entered = b;
        return;
    }
    case G_HM: {
        hm_inst *h = &w->hms[e->obj];
        switch (e->fid) {
        case 0: h->hp = iv; break;
        case 1: h->is_dead = v != 0; break;
        case 2: h->invincible = v != 0; break;
        case 3: h->invincible_from_direction = iv; break;
        case 4: h->evasion_by_hit_remaining = fv; break;
        default: h->direction_of_last_attack = iv; break;
        }
        return;
    }
    case G_DH: if (e->fid) w->dhs[e->obj].enabled = v != 0; else w->dhs[e->obj].damage_dealt = iv; return;
    case G_RECOIL: {
        recoil_inst *r = &w->recoils[e->obj];
        switch (e->fid) {
        case 0: r->speed_base = fv; break;
        case 1: r->state = (uint8_t)iv; break;
        case 2: r->time_remaining = fv; break;
        case 3: r->speed = fv; break;
        default: r->is_sweeping = v != 0; break;
        }
        return;
    }
    case G_SLASH: {
        hero_nailslash *ns = &H->slash[e->obj];
        uint8_t b = v != 0;
        switch (e->fid) {
        case NS_ANGLE: ns->slashAngle = fv; break;
        case NS_SLASHING: ns->slashing = b; break;
        case NS_STEP: ns->stepCounter = iv; break;
        case NS_POLY: ns->polyCounter = iv; break;
        case NS_ANIMDONE: ns->animCompleted = b; break;
        case NS_LONG: ns->longnail = b; break;
        case NS_MANTIS: ns->mantis = b; break;
        default: ns->fury = b; break;
        }
        return;
    }
    case G_ANIM: {
        anim_inst *a = &w->anims[e->obj];
        switch (e->fid) {
        case AN_EN: a->enabled = v != 0; break;
        case AN_TIME: a->clip_time = fv; break;
        case AN_FPS: a->clip_fps = fv; break;
        case AN_PREV: a->previous_frame = iv; break;
        case AN_STATE: a->state = (uint8_t)iv; break;
        case AN_CLIP: a->cur_clip = iv; break;
        default: break;
        }
        return;
    }
    default: return;
    }
}

/* the order a frame's writes are applied in: activation first (its callbacks may touch anything below), then
 * the transform, the bodies it carries, and the rest (which overwrites what the callbacks changed) */
static int pass_of(const ls_entry *e)
{
    if (e->grp == G_GO) return e->fid == GO_ACTIVE ? 0 : 1;
    if (e->grp == G_RB) return 2;
    return 3;
}
/* Write vals[k] into entry idx[k] for k < n.  Export-only entries and values the sim cannot hold (-2: a name it does
 * not have) are skipped; the count skipped goes to *skipped. */
HKSIM_API int hkls_import(hkls *L, const int32_t *idx, const int64_t *vals, int32_t n, int32_t *skipped)
{
    LS_ENTER(L, HKSIM_ERR_INTERNAL);
    fsm_world *w = fsmi_world(P.fsm_ctx);
    int32_t sk = 0;
    for (int pass = 0; pass < 4; pass++)
        for (int32_t k = 0; k < n; k++) {
            if (idx[k] < 0 || idx[k] >= L->n) continue;
            const ls_entry *e = &L->e[idx[k]];
            if (pass_of(e) != pass) continue;
            if ((e->flags & F_RO) || e->type == T_X || ((e->type == T_E || e->type == T_S || e->type == T_G) && vals[k] < -1)) { sk++; continue; }
            set_value(&P, w, e, vals[k]);
        }
    if (skipped) *skipped = sk;
    LS_LEAVE();
    return HKSIM_OK;
}

/* ---- list entries (T_L) ---- */
static int32_t *list_of(hkls *L, fsm_world *w, const hksim_parts *P, int32_t i, int32_t *n)
{
    const ls_entry *e = &L->e[i];
    if (e->grp == G_STATE) {
        state_inst *s = &w->fsms[e->obj].states[e->a];
        *n = e->fid == ST_AACTS ? s->n_active : s->n_finished;
        return e->fid == ST_AACTS ? s->active_actions : s->finished_actions;
    }
    const col_inst *c = &w->gos[e->go].cols[e->a];
    const shape_t *sh = c->shape ? &P->pw->shapes[c->shape] : NULL;
    *n = sh && sh->proxies ? sh->n_proxies : 0;
    return sh ? sh->proxies : NULL;
}
/* The list of entry i into out (up to cap); its length. */
HKSIM_API int32_t hkls_list(hkls *L, int32_t i, int32_t *out, int32_t cap)
{
    if (i < 0 || i >= L->n || L->e[i].type != T_L) return -1;
    hksim_parts P; hksim_get_parts(L->s, &P);
    int32_t n; const int32_t *v = list_of(L, fsmi_world(P.fsm_ctx), &P, i, &n);
    for (int32_t k = 0; k < n && k < cap; k++) out[k] = v[k];
    return n;
}
/* Replace an FSM state's active or finished action list (they grow by realloc, fsm_rt.c). */
HKSIM_API int hkls_import_list(hkls *L, int32_t i, const int32_t *v, int32_t n)
{
    if (i < 0 || i >= L->n || L->e[i].type != T_L || L->e[i].grp != G_STATE || n < 0) return HKSIM_ERR_BAD_ARG;
    LS_ENTER(L, HKSIM_ERR_INTERNAL);
    const ls_entry *e = &L->e[i];
    state_inst *s = &fsmi_world(P.fsm_ctx)->fsms[e->obj].states[e->a];
    int32_t **arr = e->fid == ST_AACTS ? &s->active_actions : &s->finished_actions;
    *arr = realloc(*arr, sizeof(int32_t) * (size_t)(n ? n : 1));
    memcpy(*arr, v, sizeof(int32_t) * (size_t)n);
    if (e->fid == ST_AACTS) s->n_active = n; else s->n_finished = n;
    LS_LEAVE();
    return HKSIM_OK;
}

/* ---- re-entering the recorded state ----
 * A state the game entered during a frame has action runtime state (a Wait's timer, a WaitRandom's drawn time) that
 * no import of the FSM's fields reaches.  The harness enters that state the way the scene restore enters a dumped one
 * (world.c fsm_enter_dumped_state: snapshot_mode, no events, the RNG put back), with act_live_float returning the
 * recorded values of the actions' private fields (hkls_live_add); the fields imported afterwards overwrite the rest. */
HKSIM_API int hkls_live_add(hkls *L, int32_t i, int32_t state, int32_t act, const char *name, float v)
{
    if (i < 0 || i >= L->n || L->e[i].grp != G_FSM || strlen(name) >= 48) return HKSIM_ERR_BAD_ARG;
    if (L->n_live == L->cap_live) {
        L->cap_live = L->cap_live ? L->cap_live * 2 : 64;
        L->live = hks_sys_realloc(L->live, sizeof *L->live * (size_t)L->cap_live);
    }
    L->live[L->n_live].fsm = L->e[i].obj; L->live[L->n_live].state = state; L->live[L->n_live].act = act;
    snprintf(L->live[L->n_live].name, sizeof L->live[L->n_live].name, "%s", name);
    L->live[L->n_live++].v = v;
    return HKSIM_OK;
}
static bool live_field(void *ctx, const act_inst *a, const char *name, float *out)
{
    const hkls *L = ctx;
    for (int32_t k = 0; k < L->n_live; k++)
        if (L->live[k].fsm == a->fsm->id && L->live[k].act == a->index && a->state == &a->fsm->states[L->live[k].state]
            && strcmp(L->live[k].name, name) == 0) { *out = L->live[k].v; return true; }
    return false;
}
/* Leave the FSM's active state and enter `state` (-1: none) silently; entry i is one of the FSM's.  An action that
 * traps on entry leaves the FSM in `state` with the trap's message in hkls_error. */
HKSIM_API int hkls_enter_state(hkls *L, int32_t i, int32_t state)
{
    if (i < 0 || i >= L->n || L->e[i].grp != G_FSM) return HKSIM_ERR_BAD_ARG;
    hksim_parts P; hksim_get_parts(L->s, &P);
    hks_arena *volatile prev = hks_arena_bind(P.arena);
    fsm_world *w = fsmi_world(P.fsm_ctx);
    fsm_inst *f = &w->fsms[L->e[i].obj];
    int was = w->snapshot_mode;
    bool log_was = w->log_enabled;
    hk_rng saved = *w->rng;
    int rc = HKSIM_OK;
    if (setjmp(L->trap.jb) != 0) {
        snprintf(L->err, sizeof L->err, "%s", hksim_trap_message());
        rc = L->trap.code ? L->trap.code : HKSIM_ERR_INTERNAL;
        w->stack_depth = 0;
    } else {
        hksim_trap_arm(&L->trap);
        w->snapshot_mode = 1; w->log_enabled = false;
        w->live_field = live_field; w->live_ctx = L;
        if (f->active_state >= 0 && f->active_state_entered) { push_fsm(w, f->id); state_silent_exit(f, f->active_state); pop_fsm(w); }
        f->active_state = state >= 0 && state < f->n_states ? state : -1;
        f->active_state_entered = 1;
        f->switch_to = -1;
        if (f->active_state >= 0) { push_fsm(w, f->id); state_silent_enter(f, f->active_state); pop_fsm(w); }
    }
    hksim_trap_disarm();
    w->live_field = NULL; w->live_ctx = NULL;
    *w->rng = saved;
    w->log_enabled = log_was; w->snapshot_mode = (uint8_t)was;
    L->n_live = 0;
    hks_arena_bind(prev);
    return rc;
}

/* ---- the Box2D contact state ----
 * At a frame's end the game's contact manager holds a b2Contact for every pair of overlapping fat AABBs (UpdatePairs
 * ends each Solve) and PhysicsContacts2D a record for every touching collider pair.  After an import has moved the
 * bodies, hkls_update_pairs flushes the moved shapes and runs the same FindNewContacts, and hkls_touch makes a pair the game has touching touch in
 * the sim with the record's state, so the next step reports Stay (not Enter) for it.  Proxy ids and the contact list's
 * order are not imported: they are the broadphase's allocation history (native-box2d.md §12, the N0 snapshot). */
HKSIM_API int hkls_update_pairs(hkls *L)
{
    LS_ENTER(L, HKSIM_ERR_INTERNAL);
    world_flush_dirty_shapes(fsmi_world(P.fsm_ctx));   /* the imported poses into the shapes, as before a step */
    ph_find_new_contacts(P.pw);
    LS_LEAVE();
    return HKSIM_OK;
}
static uint32_t shape_of_col(const hksim_parts *P, fsm_world *w, int32_t go, int type, int32_t ord)
{
    if (go < 0 || go >= w->n_gos) return 0;
    uint32_t sh = 0;
    for (int32_t k = 0, o = 0; k < w->gos[go].n_cols && !sh; k++)
        if (w->gos[go].cols[k].def->type == type && o++ == ord) sh = w->gos[go].cols[k].shape;
    if (sh) return sh;
    if (go == w->knight_go && type == COL_BOX && ord == 0 && P->hero_body)   /* the Knight's own box is the core's */
        for (phys_shape_id f = P->pw->bodies[P->hero_body].fixture_list; f; f = P->pw->shapes[f].next_fixture)
            if (P->pw->shapes[f].user == HKSIM_USER_HERO) return f;
    return 0;
}
/* Collider (go_a, type_a, ord_a) and (go_b, ...) touch with PhysicsContacts2D state `state` (ContactState).  Returns
 * 0 when the sim's pair now touches with that state, 1 when a collider has no shape, 2 when the sim has no contact for
 * the pair (its fat AABBs do not overlap), 3 when the pair's shapes do not overlap in the sim. */
HKSIM_API int hkls_touch(hkls *L, int32_t go_a, int32_t type_a, int32_t ord_a, int32_t go_b, int32_t type_b, int32_t ord_b,
                         int32_t state)
{
    LS_ENTER(L, -1);
    fsm_world *w = fsmi_world(P.fsm_ctx);
    phys_world *pw = P.pw;
    uint32_t sa = shape_of_col(&P, w, go_a, type_a, ord_a), sb = shape_of_col(&P, w, go_b, type_b, ord_b);
    int rc = 1;
    if (sa && sb) {
        rc = 2;
        for (int32_t c = pw->contact_list; c >= 0; c = pw->contacts[c].next) {
            contact_t *ct = &pw->contacts[c];
            if (!((ct->sa == sa && ct->sb == sb) || (ct->sa == sb && ct->sb == sa))) continue;
            if (!ct->touching) ph_contact_update(pw, ct);
            if (!ct->touching) { if (rc != 0) rc = 3; continue; }
            pw->coll_pool[ct->coll].state = (uint8_t)state;
            rc = 0;
        }
    }
    LS_LEAVE();
    return rc;
}
