/* Variable access across FSMs and objects (fsm-actions.md §6): object lookups, tags, GetFsm and SetFsm,
 * PlayerData, static variables, FsmVar. */
#include "act.h"
#include "core/alloc.h"
#include "../lifecycle.h"

/* ---- object lookups */

/* GetOwner — ACT/GetOwner.cs:16-20 */
typedef struct { const fsm_pv *store; } st_getowner;
static void getowner_bind(act_inst *a) { ST(st_getowner); s->store = FIELD(storeGameObject); }
static void getowner_enter(act_inst *a) { ST(st_getowner); pgo_set(f, s->store, f->go); act_finish(a); }
static const act_vtable AV_GetOwner = { "GetOwner", sizeof(st_getowner), getowner_bind, getowner_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* GetHero — HK/GetHero.cs:12-18: HeroController.instance.gameObject */
typedef struct { const fsm_pv *store; } st_gethero;
static void gethero_bind(act_inst *a) { ST(st_gethero); s->store = FIELD(storeResult); }
static void gethero_enter(act_inst *a) { ST(st_gethero); pgo_set(f, s->store, w->knight_go); act_finish(a); }
static const act_vtable AV_GetHero = { "GetHero", sizeof(st_gethero), gethero_bind, gethero_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* GetParent — ACT/GetParent.cs:21-33 */
typedef struct { const fsm_pv *go, *store; } st_getparent;
static void getparent_bind(act_inst *a) { ST(st_getparent); s->go = FIELD(gameObject); s->store = FIELD(storeResult); }
static void getparent_enter(act_inst *a)
{
    ST(st_getparent);
    int32_t t = p_owner_default(a, s->go);
    pgo_set(f, s->store, t >= 0 ? go_parent(w, t) : -1);
    act_finish(a);
}
static const act_vtable AV_GetParent = { "GetParent", sizeof(st_getparent), getparent_bind, getparent_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* FindChild — ACT/FindChild.cs:29-43 (Transform.Find) */
typedef struct { const fsm_pv *go, *childName, *store; } st_findchild;
static void findchild_bind(act_inst *a) { ST(st_findchild); s->go = FIELD(gameObject); s->childName = FIELD(childName); s->store = FIELD(storeResult); }
static void findchild_enter(act_inst *a)
{
    ST(st_findchild);
    int32_t t = p_owner_default(a, s->go);
    if (t >= 0) pgo_set(f, s->store, go_find_child(w, t, w_str(w, ps(f, s->childName))));
    act_finish(a);
}
static const act_vtable AV_FindChild = { "FindChild", sizeof(st_findchild), findchild_bind, findchild_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* GetChild - ACT/GetChild.cs:24-31 plus DoGetChildByName :33-60: depth-first over descendants -- each child is
 * tested, and if it does not match the search recurses into it before the next sibling.  With a name the tag
 * must also match when non-empty; with no name the tag alone selects. */
static int32_t get_child(fsm_world *w, int32_t root, const char *name, const char *tag)
{
    if (root < 0) return -1;                                          /* :35-38 root null -> null */
    int has_name = (name && name[0]) ? 1 : 0, has_tag = (tag && tag[0]) ? 1 : 0;
    for (int32_t c = w->gos[root].first_child; c >= 0; c = w->gos[c].next_sibling) {
        const char *cn = go_name(w, c);
        const char *ct = w_str(w, go_tag(w, c));
        if (has_name) {
            if (cn && strcmp(cn, name) == 0) {                        /* :42-53 */
                if (!has_tag) return c;
                if (ct && strcmp(ct, tag) == 0) return c;
            }
        } else if (has_tag && ct && strcmp(ct, tag) == 0) {           /* :54-57 tag only */
            return c;
        }
        int32_t r = get_child(w, c, name, tag);                       /* :59 depth-first recurse */
        if (r >= 0) return r;
    }
    return -1;
}
typedef struct { const fsm_pv *gameObject, *childName, *withTag, *storeResult; } st_gc;
static void gc_bind(act_inst *a) { ST(st_gc); s->gameObject = FIELD(gameObject); s->childName = FIELD(childName); s->withTag = FIELD(withTag); s->storeResult = FIELD(storeResult); }
static void gc_enter(act_inst *a)
{
    ST(st_gc);
    int32_t root = p_owner_default(a, s->gameObject);
    pgo_set(f, s->storeResult, get_child(w, root, w_str(w, ps(f, s->childName)), w_str(w, ps(f, s->withTag))));   /* :27 assigned even when null */
    act_finish(a);
}
static const act_vtable AV_GetChild = { "GetChild", sizeof(st_gc), gc_bind, gc_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* FindGameObject — ACT/FindGameObject.cs:28-60 */
typedef struct { const fsm_pv *objectName, *withTag, *store; } st_findgo;
static void findgo_bind(act_inst *a) { ST(st_findgo); s->objectName = FIELD(objectName); s->withTag = FIELD(withTag); s->store = FIELD(store); }
static void findgo_enter(act_inst *a)
{
    ST(st_findgo);
    const char *tag = w_str(w, ps(f, s->withTag));
    const char *name = w_str(w, ps(f, s->objectName));
    int32_t found = -1;
    if (strcmp(tag, "Untagged") != 0) {                            /* :35-52 FindGameObjectsWithTag (+ name filter) / FindGameObjectWithTag */
        int32_t tid = w_find_string(w, tag);
        for (int32_t g = 0; g < w->n_gos && found < 0; g++)       /* registry order stands in for Unity's (Q-fsmact-1) */
            if (tid >= 0 && !w->gos[g].def->asset && go_tag(w, g) == tid && (!*name || strcmp(go_name(w, g), name) == 0)) found = g;
        /* tags of the DontDestroyOnLoad objects are not dumped: an object NAMED like the tag (CameraTarget) is accepted */
        if (found < 0 && !*name) found = world_go_find_name(w, tag);
        if (found < 0) world_log(w, LOG_NOTE, f->id, w_intern(w, "FindGameObject: no object with the tag"), w_intern(w, tag));
    } else {
        found = world_go_find_name(w, name);                       /* :55 GameObject.Find */
    }
    pgo_set(f, s->store, found);
    act_finish(a);
}
static const act_vtable AV_FindGameObject = { "FindGameObject", sizeof(st_findgo), findgo_bind, findgo_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* FindGameObjectsWithTag's set, for GetTagCount and FindClosest: live, active, in-scene objects carrying the
 * tag.  The native filter is not in the C# reference (GameObject.bindings.cs:302-303 is an extern); active-only
 * follows from the pool model -- every tagged attack object is a pooled clone, inactive until
 * world_pool_spawn activates it (e.g. dumps/GG_Hornet_2/hierarchy.json.gz: eight inactive Hornet Barb(Clone)
 * at SceneReady) -- and assets are not scene objects. */
static bool tagged_live(fsm_world *w, int32_t g, int32_t tid)
{
    return !w->gos[g].destroyed && w->gos[g].def->in_scene && go_tag(w, g) == tid && go_active_in_hierarchy(w, g);
}

/* GetTagCount — ACT/GetTagCount.cs:22-30: FindGameObjectsWithTag(tag).Length.  Hornet Boss 2 'Barb?' throws no
 * barb once five are in flight.  The FsmString may hold a runtime value, so the tag id is looked up by text. */
typedef struct { const fsm_pv *tag, *storeResult; } st_gtc;
static void gtc_bind(act_inst *a) { ST(st_gtc); s->tag = FIELD(tag); s->storeResult = FIELD(storeResult); }
static void gtc_enter(act_inst *a)
{
    ST(st_gtc);
    int32_t tid = w_find_string(w, w_str(w, ps(f, s->tag)));
    int32_t n = 0;
    if (tid >= 0) for (int32_t g = 0; g < w->n_gos; g++) if (tagged_live(w, g, tid)) n++;
    pi_set(f, s->storeResult, n);
    act_finish(a);
}
static const act_vtable AV_GetTagCount = { "GetTagCount", sizeof(st_gtc), gtc_bind, gtc_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* FindClosest — ACT/FindClosest.cs:46-84: over FindGameObjectsWithTag(withTag), skipping the owner when
 * ignoreOwner, the first object at the strictly smallest Vector3 sqrMagnitude wins (registry order for Unity's,
 * Q-fsmact-1); storeDistance = sqrt.  The untagged path (FindObjectsOfType) and mustBeVisible (a camera
 * frustum) are not ported and trap. */
typedef struct { const fsm_pv *go, *withTag, *ignoreOwner, *mustBeVisible, *storeObject, *storeDistance, *everyFrame; } st_fcl;
static void fcl_bind(act_inst *a)
{
    ST(st_fcl);
    s->go = FIELD(gameObject); s->withTag = FIELD(withTag); s->ignoreOwner = FIELD_OPT(ignoreOwner);
    s->mustBeVisible = FIELD_OPT(mustBeVisible); s->storeObject = FIELD(storeObject);
    s->storeDistance = FIELD_OPT(storeDistance); s->everyFrame = FIELD_OPT(everyFrame);
}
static void fcl_do(act_inst *a)
{
    ST(st_fcl);
    int32_t from = p_owner_default(a, s->go);
    if (from < 0) return;
    const char *tag = w_str(w, ps(f, s->withTag));
    if (!*tag || strcmp(tag, "Untagged") == 0)
        HKSIM_UNIMPLEMENTED("FindClosest with no tag scans every GameObject (FindObjectsOfType) in %s", fsm_label(f));
    if (s->mustBeVisible && pb(f, s->mustBeVisible))
        HKSIM_UNIMPLEMENTED("FindClosest mustBeVisible needs ActionHelpers.IsVisible (camera frustum) in %s", fsm_label(f));
    int32_t tid = w_find_string(w, tag);
    bool ignore_owner = !s->ignoreOwner || pb(f, s->ignoreOwner);
    int32_t owner = fsm_owner_go(a->fsm);
    float pf3[3]; go_world_pos(w, from, pf3);
    int32_t best = -1; float num = INFINITY;
    if (tid >= 0) {
        for (int32_t g = 0; g < w->n_gos; g++) {
            if (!tagged_live(w, g, tid) || (ignore_owner && g == owner)) continue;
            float po[3]; go_world_pos(w, g, po);
            float dx = pf3[0] - po[0], dy = pf3[1] - po[1], dz = pf3[2] - po[2];
            float d = (float)((double)dx * dx + (double)dy * dy + (double)dz * dz);   /* Vector3.sqrMagnitude: double stack, float return */
            if (d < num) { num = d; best = g; }
        }
    }
    pgo_set(f, s->storeObject, best);
    if (s->storeDistance && !p_isnone(s->storeDistance)) pf_set(f, s->storeDistance, sqrtf(num));
}
static void fcl_enter(act_inst *a) { ST(st_fcl); fcl_do(a); if (!s->everyFrame || !pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_FindClosest = { "FindClosest", sizeof(st_fcl), fcl_bind, fcl_enter, fcl_do, NULL, NULL, NULL, NULL, NULL };

/* GetTag — ACT/GetTag.cs:23-43 */
typedef struct { const fsm_pv *go, *store, *everyFrame; } st_gettag;
static void gettag_bind(act_inst *a) { ST(st_gettag); s->go = FIELD(gameObject); s->store = FIELD(storeResult); s->everyFrame = FIELD(everyFrame); }
static void gettag_do(act_inst *a)
{
    ST(st_gettag);
    int32_t g = pgo(f, s->go);
    if (g >= 0) {
        int32_t tag = go_tag(w, g);
        if (tag < 0) HKSIM_UNKNOWN("tag of '%s' not dumped", go_path(w, g));
        ps_set(f, s->store, tag);
    }
}
static void gettag_enter(act_inst *a) { ST(st_gettag); gettag_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GetTag = { "GetTag", sizeof(st_gettag), gettag_bind, gettag_enter, gettag_do, NULL, NULL, NULL, NULL, NULL };

/* SetTag — ACT/SetTag.cs:20-28: GameObject.tag, read back by go_tag through tag_override (e.g. Nightmare Grimm
 * flips 'Boss' <-> 'Spell Vulnerable' in Inflate/Deflate). */
typedef struct { const fsm_pv *gameObject, *tag; } st_settag;
static void settag_bind(act_inst *a) { ST(st_settag); s->gameObject = FIELD(gameObject); s->tag = FIELD(tag); }
static void settag_enter(act_inst *a)
{
    ST(st_settag);
    int32_t go = p_owner_default(a, s->gameObject);
    if (go >= 0) w->gos[go].tag_override = ps(f, s->tag);               /* :23-26 */
    act_finish(a);
}
static const act_vtable AV_SetTag = { "SetTag", sizeof(st_settag), settag_bind, settag_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* GetLayer — ACT/GetLayer.cs:20-40 */
typedef struct { const fsm_pv *go, *store, *everyFrame; } st_glayer;
static void glayer_bind(act_inst *a) { ST(st_glayer); s->go = FIELD(gameObject); s->store = FIELD(storeResult); s->everyFrame = FIELD(everyFrame); }
static void glayer_do(act_inst *a) { ST(st_glayer); int32_t g = pgo(f, s->go); if (g >= 0) pi_set(f, s->store, go_layer(w, g)); }
static void glayer_enter(act_inst *a) { ST(st_glayer); glayer_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GetLayer = { "GetLayer", sizeof(st_glayer), glayer_bind, glayer_enter, glayer_do, NULL, NULL, NULL, NULL, NULL };

/* GetName — ACT/GetName.cs:24-44 */
typedef struct { const fsm_pv *go, *store, *everyFrame; } st_gname;
static void gname_bind(act_inst *a) { ST(st_gname); s->go = FIELD(gameObject); s->store = FIELD(storeName); s->everyFrame = FIELD(everyFrame); }
static void gname_do(act_inst *a) { ST(st_gname); int32_t g = pgo(f, s->go); ps_set(f, s->store, w_intern(w, g >= 0 ? go_name(w, g) : "")); }
static void gname_enter(act_inst *a) { ST(st_gname); gname_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GetName = { "GetName", sizeof(st_gname), gname_bind, gname_enter, gname_do, NULL, NULL, NULL, NULL, NULL };

/* ---- another FSM's variables.  FsmVariables.FindFsm<T> is locals-only; GetFsm<T> walks locals, then globals,
 * then mints a throw-away, so a write to a missing variable is lost and a read of one reads 0. */

/* ActionHelpers.GetGameObjectFsm — HK/HutongGames.PlayMaker/ActionHelpers.cs:56-71: name scan, else first FSM */
int32_t get_game_object_fsm(fsm_world *w, int32_t go, const char *fsm_name)
{
    if (fsm_name[0]) { int32_t k = world_fsm_find(w, go, fsm_name); if (k >= 0) return k; }
    return world_fsm_first(w, go);
}

/* SetFsmBool / SetFsmFloat / SetFsmInt / SetFsmString / SetFsmGameObject — ACT/SetFsm<T>.cs:43-88, caching the
 * FSM by (GameObject, fsmName) */
typedef struct { const fsm_pv *go, *fsmName, *variableName, *setValue, *everyFrame; int32_t go_last, name_last, fsm; } st_setfsm;
static void setfsm_bind(act_inst *a) { ST(st_setfsm); s->go = FIELD(gameObject); s->fsmName = FIELD(fsmName); s->variableName = FIELD(variableName); s->setValue = FIELD(setValue); s->everyFrame = FIELD(everyFrame); s->go_last = -1; s->name_last = -2; s->fsm = -1; }
static bool setfsm_resolve(act_inst *a)
{
    ST(st_setfsm);
    int32_t t = p_owner_default(a, s->go);
    if (t < 0) return false;
    int32_t name = ps(f, s->fsmName);
    if (t != s->go_last || name != s->name_last) { s->go_last = t; s->name_last = name; s->fsm = get_game_object_fsm(w, t, w_str(w, name)); }
    return s->fsm >= 0;
}
/* SetFsmBool: FindFsmBool (SetFsmBool.cs:74) */
static void setfsmbool_do(act_inst *a)
{
    ST(st_setfsm);
    if (!setfsm_resolve(a)) return;
    fsm_inst *tf = &w->fsms[s->fsm];
    int32_t vi = fsm_find_var(tf, VB_BOOL, ps(f, s->variableName));
    if (vi >= 0) tf->vals[vi].i = pb(f, s->setValue) ? 1 : 0;
}
static void setfsmbool_enter(act_inst *a) { ST(st_setfsm); setfsmbool_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_SetFsmBool = { "SetFsmBool", sizeof(st_setfsm), setfsm_bind, setfsmbool_enter, setfsmbool_do, NULL, NULL, NULL, NULL, NULL };
/* SetFsmFloat: GetFsmFloat (boss-hornet.md §2.6 Stun Start) */
static void setfsmfloat_do(act_inst *a)
{
    ST(st_setfsm);
    if (!setfsm_resolve(a)) return;
    bool fresh; fsm_val *v = fsm_get_var(&w->fsms[s->fsm], VB_FLOAT, ps(f, s->variableName), &fresh);
    if (!fresh) v->f = pf(f, s->setValue);
}
static void setfsmfloat_enter(act_inst *a) { ST(st_setfsm); setfsmfloat_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_SetFsmFloat = { "SetFsmFloat", sizeof(st_setfsm), setfsm_bind, setfsmfloat_enter, setfsmfloat_do, NULL, NULL, NULL, NULL, NULL };
/* SetFsmInt: GetFsmInt (FsmVariables.cs:1283-1309) */
static void setfsmint_do(act_inst *a)
{
    ST(st_setfsm);
    if (!setfsm_resolve(a)) return;
    bool fresh; fsm_val *v = fsm_get_var(&w->fsms[s->fsm], VB_INT, ps(f, s->variableName), &fresh);
    if (!fresh) v->i = pi(f, s->setValue);
}
static void setfsmint_enter(act_inst *a) { ST(st_setfsm); setfsmint_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_SetFsmInt = { "SetFsmInt", sizeof(st_setfsm), setfsm_bind, setfsmint_enter, setfsmint_do, NULL, NULL, NULL, NULL, NULL };
/* SetFsmString: GetFsmString (SetFsmString.cs:42-87) */
static void setfsmstring_do(act_inst *a)
{
    ST(st_setfsm);
    if (!setfsm_resolve(a)) return;
    bool fresh; fsm_val *v = fsm_get_var(&w->fsms[s->fsm], VB_STRING, ps(f, s->variableName), &fresh);
    if (!fresh) v->i = ps(f, s->setValue);
}
static void setfsmstring_enter(act_inst *a) { ST(st_setfsm); setfsmstring_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_SetFsmString = { "SetFsmString", sizeof(st_setfsm), setfsm_bind, setfsmstring_enter, setfsmstring_do, NULL, NULL, NULL, NULL, NULL };
/* SetFsmGameObject: FindFsmGameObject (SetFsmGameObject.cs:43-83) */
static void setfsmgo_do(act_inst *a)
{
    ST(st_setfsm);
    if (!setfsm_resolve(a)) return;
    fsm_inst *tf = &w->fsms[s->fsm];
    int32_t vi = fsm_find_var(tf, VB_GO, ps(f, s->variableName));
    if (vi >= 0) tf->vals[vi].i = pgo(f, s->setValue);
}
static void setfsmgo_enter(act_inst *a) { ST(st_setfsm); setfsmgo_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_SetFsmGameObject = { "SetFsmGameObject", sizeof(st_setfsm), setfsm_bind, setfsmgo_enter, setfsmgo_do, NULL, NULL, NULL, NULL, NULL };

/* SetFsmVector3 - ACT/SetFsmVector3.cs:77-80 (the named FSM only) */
typedef struct { const fsm_pv *gameObject, *fsmName, *variableName, *setValue, *everyFrame; } st_sfv3;
static void sfv3_bind(act_inst *a) { ST(st_sfv3); s->gameObject = FIELD(gameObject); s->fsmName = FIELD(fsmName); s->variableName = FIELD(variableName); s->setValue = FIELD(setValue); s->everyFrame = FIELD(everyFrame); }
static void sfv3_do(act_inst *a)
{
    ST(st_sfv3);
    int32_t go = p_owner_default(a, s->gameObject);
    if (go < 0) return;
    int32_t other_fsm = world_fsm_find(w, go, w_str(w, ps(f, s->fsmName)));
    if (other_fsm < 0) return;
    bool fresh;
    fsm_val *v = fsm_get_var(&w->fsms[other_fsm], VB_V3, ps(f, s->variableName), &fresh);
    if (v && !p_isnone(s->setValue)) { const float *val = pv3(f, s->setValue); v->v[0] = val[0]; v->v[1] = val[1]; v->v[2] = val[2]; }
}
static void sfv3_enter(act_inst *a) { ST(st_sfv3); sfv3_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_SetFsmVector3 = { "SetFsmVector3", sizeof(st_sfv3), sfv3_bind, sfv3_enter, sfv3_do, NULL, NULL, NULL, NULL, NULL };

/* GetFsmBool - ACT/GetFsmBool.cs:30-56: FindFsmBool, so a miss leaves storeValue untouched (:52-55) */
typedef struct { const fsm_pv *gameObject, *fsmName, *variableName, *storeValue, *everyFrame; int32_t go_last, name_last, fsm; } st_gfb;
static void gfb_bind(act_inst *a)
{
    ST(st_gfb);
    s->gameObject = FIELD(gameObject); s->fsmName = FIELD(fsmName); s->variableName = FIELD(variableName);
    s->storeValue = FIELD(storeValue); s->everyFrame = FIELD(everyFrame);
    s->go_last = -1; s->name_last = -2; s->fsm = -1;
}
static void gfb_do(act_inst *a)
{
    ST(st_gfb);
    int32_t t = p_owner_default(a, s->gameObject);
    if (t < 0) return;                                                /* :39-42 null target -> return */
    int32_t nm = ps(f, s->fsmName);
    if (t != s->go_last || nm != s->name_last) { s->go_last = t; s->name_last = nm; s->fsm = get_game_object_fsm(w, t, w_str(w, nm)); }   /* :44-48 */
    if (s->fsm < 0) return;
    fsm_inst *tf = &w->fsms[s->fsm];
    int32_t vi = fsm_find_var(tf, VB_BOOL, ps(f, s->variableName));
    if (vi >= 0) pb_set(f, s->storeValue, tf->vals[vi].i != 0);
}
static void gfb_enter(act_inst *a) { ST(st_gfb); gfb_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GetFsmBool = { "GetFsmBool", sizeof(st_gfb), gfb_bind, gfb_enter, gfb_do, NULL, NULL, NULL, NULL, NULL };

/* GetFsmFloat — ACT/GetFsmFloat.cs:30-77: GetFsmFloat, so a miss reads 0 (:72-74) */
typedef struct { const fsm_pv *go, *fsmName, *variableName, *storeValue, *everyFrame; int32_t go_last, name_last, fsm; } st_getfsmf;
static void getfsmf_bind(act_inst *a) { ST(st_getfsmf); s->go = FIELD(gameObject); s->fsmName = FIELD(fsmName); s->variableName = FIELD(variableName); s->storeValue = FIELD(storeValue); s->everyFrame = a_field(a, "everyFrame"); s->go_last = -1; s->name_last = -2; s->fsm = -1; }
static void getfsmf_do(act_inst *a)
{
    ST(st_getfsmf);
    if (p_isnone(s->storeValue)) return;                            /* :54-57 */
    int32_t t = p_owner_default(a, s->go);
    if (t < 0) return;                                              /* :58-62 */
    int32_t name = ps(f, s->fsmName);
    if (t != s->go_last || name != s->name_last) { s->go_last = t; s->name_last = name; s->fsm = get_game_object_fsm(w, t, w_str(w, name)); }
    if (s->fsm < 0) return;                                         /* :70 fsm == null */
    bool fresh; fsm_val *v = fsm_get_var(&w->fsms[s->fsm], VB_FLOAT, ps(f, s->variableName), &fresh);
    pf_set(f, s->storeValue, fresh ? 0.0f : v->f);
}
static void getfsmf_enter(act_inst *a) { ST(st_getfsmf); getfsmf_do(a); if (!s->everyFrame || !pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GetFsmFloat = { "GetFsmFloat", sizeof(st_getfsmf), getfsmf_bind, getfsmf_enter, getfsmf_do, NULL, NULL, NULL, NULL, NULL };

/* GetFsmInt — ACT/GetFsmInt.cs:38-76, GetFsmGameObject — ACT/GetFsmGameObject.cs:38-76: cache the FSM by
 * GameObject only; a fresh FsmInt reads 0 (boss-hornet.md §2.3) */
typedef struct { const fsm_pv *go, *fsmName, *variableName, *storeValue, *everyFrame; int32_t go_last, fsm; } st_getfsm;
static void getfsm_bind(act_inst *a) { ST(st_getfsm); s->go = FIELD(gameObject); s->fsmName = FIELD(fsmName); s->variableName = FIELD(variableName); s->storeValue = FIELD(storeValue); s->everyFrame = FIELD(everyFrame); s->go_last = -1; s->fsm = -1; }
static bool getfsm_resolve(act_inst *a)
{
    ST(st_getfsm);
    int32_t t = p_owner_default(a, s->go);
    if (t < 0) return false;
    if (t != s->go_last) { s->go_last = t; s->fsm = get_game_object_fsm(w, t, w_str(w, ps(f, s->fsmName))); }
    return s->fsm >= 0;
}
static void getfsmint_do(act_inst *a)
{
    ST(st_getfsm);
    if (p_isnone(s->storeValue) && s->storeValue->kind == PV_NULL) return;
    if (!getfsm_resolve(a)) return;
    bool fresh; fsm_val *v = fsm_get_var(&w->fsms[s->fsm], VB_INT, ps(f, s->variableName), &fresh);
    pi_set(f, s->storeValue, v->i);
}
static void getfsmint_enter(act_inst *a) { ST(st_getfsm); getfsmint_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GetFsmInt = { "GetFsmInt", sizeof(st_getfsm), getfsm_bind, getfsmint_enter, getfsmint_do, NULL, NULL, NULL, NULL, NULL };
static void getfsmgo_do(act_inst *a)
{
    ST(st_getfsm);
    if (!getfsm_resolve(a)) return;
    bool fresh; fsm_val *v = fsm_get_var(&w->fsms[s->fsm], VB_GO, ps(f, s->variableName), &fresh);
    pgo_set(f, s->storeValue, fresh ? -1 : v->i);
}
static void getfsmgo_enter(act_inst *a) { ST(st_getfsm); getfsmgo_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GetFsmGameObject = { "GetFsmGameObject", sizeof(st_getfsm), getfsm_bind, getfsmgo_enter, getfsmgo_do, NULL, NULL, NULL, NULL, NULL };

/* ---- PlayerData (GameManager.playerData, stored by world_pd_* in knight.c).  The actions with a gameObject
 * field act only when it carries the GameManager, and otherwise never Finish(). */
static bool has_game_manager(fsm_world *w, int32_t go) { return go >= 0 && (go == w->game_manager_go || go_has_component(w, go, "GameManager")); }

typedef struct { const fsm_pv *go, *name, *value; } st_pd;
static void pdgi_bind(act_inst *a) { ST(st_pd); s->go = FIELD(gameObject); s->name = FIELD(intName); s->value = FIELD(storeValue); }
static void pdgf_bind(act_inst *a) { ST(st_pd); s->go = FIELD(gameObject); s->name = FIELD(floatName); s->value = FIELD(storeValue); }
static void pdgs_bind(act_inst *a) { ST(st_pd); s->go = FIELD(gameObject); s->name = FIELD(stringName); s->value = FIELD(storeValue); }
static void pdgv_bind(act_inst *a) { ST(st_pd); s->go = FIELD(gameObject); s->name = FIELD(vector3Name); s->value = FIELD(storeValue); }
static void pdsf_bind(act_inst *a) { ST(st_pd); s->go = FIELD(gameObject); s->name = FIELD(floatName); s->value = FIELD(value); }
static void pdss_bind(act_inst *a) { ST(st_pd); s->go = FIELD(gameObject); s->name = FIELD(stringName); s->value = FIELD(value); }
static void pdia_bind(act_inst *a) { ST(st_pd); s->go = FIELD(gameObject); s->name = FIELD(intName); s->value = FIELD(amount); }
static void pdinc_bind(act_inst *a) { ST(st_pd); s->go = FIELD(gameObject); s->name = FIELD(intName); s->value = NULL; }
static void pdgb_bind(act_inst *a) { ST(st_pd); s->go = NULL; s->name = FIELD(boolName); s->value = FIELD(storeValue); }
static void pdsb_bind(act_inst *a) { ST(st_pd); s->go = NULL; s->name = FIELD(boolName); s->value = FIELD(value); }
static void pdsi_bind(act_inst *a) { ST(st_pd); s->go = NULL; s->name = FIELD(intName); s->value = FIELD(value); }
/* the gameObject gate; the name of the field */
static const char *pd_gate(act_inst *a)
{
    ST(st_pd);
    int32_t t = p_owner_default(a, s->go);
    return (t >= 0 && has_game_manager(w, t)) ? w_str(w, ps(f, s->name)) : NULL;
}
/* GetPlayerDataInt — ACT/GetPlayerDataInt.cs:27-41 */
static void pdgi_enter(act_inst *a) { ST(st_pd); const char *n = pd_gate(a); if (!n) return; pi_set(f, s->value, world_pd_int(w, n)); act_finish(a); }
/* GetPlayerDataFloat — HK/GetPlayerDataFloat.cs:19-33 */
static void pdgf_enter(act_inst *a) { ST(st_pd); const char *n = pd_gate(a); if (!n) return; pf_set(f, s->value, world_pd_float(w, n)); act_finish(a); }
/* GetPlayerDataString — HK/GetPlayerDataString.cs:19-33 */
static void pdgs_enter(act_inst *a) { ST(st_pd); const char *n = pd_gate(a); if (!n) return; ps_set(f, s->value, world_pd_string(w, n)); act_finish(a); }
/* GetPlayerDataVector3 — ACT/GetPlayerDataVector3.cs:27-41; a field PlayerData lacks reads Vector3.zero
 * (PlayerData.cs:8560-8563), which pd_dump refuses instead */
static void pdgv_enter(act_inst *a)
{
    ST(st_pd);
    const char *n = pd_gate(a);
    if (!n) return;
    float v[3]; world_pd_vector3(w, n, v);
    pv3_set(f, s->value, v);
    act_finish(a);
}
/* SetPlayerDataFloat — HK/SetPlayerDataFloat.cs:19-33 */
static void pdsf_enter(act_inst *a) { ST(st_pd); const char *n = pd_gate(a); if (!n) return; world_pd_set_float(w, n, pf(f, s->value)); act_finish(a); }
/* SetPlayerDataString — HK/SetPlayerDataString.cs:19-33 */
static void pdss_enter(act_inst *a) { ST(st_pd); const char *n = pd_gate(a); if (!n) return; world_pd_set_string(w, n, ps(f, s->value)); act_finish(a); }
/* PlayerDataIntAdd — HK/PlayerDataIntAdd.cs:19-33: GameManager.IntAdd(name, amount) */
static void pdia_enter(act_inst *a) { ST(st_pd); const char *n = pd_gate(a); if (!n) return; world_pd_set_int(w, n, world_pd_int(w, n) + pi(f, s->value)); act_finish(a); }
/* IncrementPlayerDataInt — ACT/IncrementPlayerDataInt.cs:19-33 */
static void pdinc_enter(act_inst *a) { const char *n = pd_gate(a); if (!n) return; world_pd_set_int(a->fsm->w, n, world_pd_int(a->fsm->w, n) + 1); act_finish(a); }
/* GetPlayerDataBool — HK/GetPlayerDataBool.cs:17-28 (GameManager.instance) */
static void pdgb_enter(act_inst *a) { ST(st_pd); pb_set(f, s->value, world_pd_bool(w, w_str(w, ps(f, s->name)))); act_finish(a); }
/* SetPlayerDataBool — ACT/SetPlayerDataBool.cs:21-31 */
static void pdsb_enter(act_inst *a) { ST(st_pd); world_pd_set_bool(w, w_str(w, ps(f, s->name)), pb(f, s->value)); act_finish(a); }
/* SetPlayerDataInt - ACT/SetPlayerDataInt.cs:19-31 -> GameManager.SetPlayerDataInt */
static void pdsi_enter(act_inst *a) { ST(st_pd); world_pd_set_int(w, w_str(w, ps(f, s->name)), pi(f, s->value)); act_finish(a); }
static const act_vtable AV_GetPlayerDataInt = { "GetPlayerDataInt", sizeof(st_pd), pdgi_bind, pdgi_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_GetPlayerDataFloat = { "GetPlayerDataFloat", sizeof(st_pd), pdgf_bind, pdgf_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_GetPlayerDataString = { "GetPlayerDataString", sizeof(st_pd), pdgs_bind, pdgs_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_GetPlayerDataVector3 = { "GetPlayerDataVector3", sizeof(st_pd), pdgv_bind, pdgv_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_SetPlayerDataFloat = { "SetPlayerDataFloat", sizeof(st_pd), pdsf_bind, pdsf_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_SetPlayerDataString = { "SetPlayerDataString", sizeof(st_pd), pdss_bind, pdss_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_PlayerDataIntAdd = { "PlayerDataIntAdd", sizeof(st_pd), pdia_bind, pdia_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_IncrementPlayerDataInt = { "IncrementPlayerDataInt", sizeof(st_pd), pdinc_bind, pdinc_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_GetPlayerDataBool = { "GetPlayerDataBool", sizeof(st_pd), pdgb_bind, pdgb_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_SetPlayerDataBool = { "SetPlayerDataBool", sizeof(st_pd), pdsb_bind, pdsb_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_SetPlayerDataInt = { "SetPlayerDataInt", sizeof(st_pd), pdsi_bind, pdsi_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* PlayerDataBoolTest — ACT/PlayerDataBoolTest.cs:32-53 */
typedef struct { const fsm_pv *go, *name, *isTrue, *isFalse; } st_pdbt;
static void pdbt_bind(act_inst *a) { ST(st_pdbt); s->go = FIELD(gameObject); s->name = FIELD(boolName); s->isTrue = FIELD(isTrue); s->isFalse = FIELD(isFalse); }
static void pdbt_enter(act_inst *a)
{
    ST(st_pdbt);
    int32_t t = p_owner_default(a, s->go);
    if (t < 0 || !has_game_manager(w, t)) return;
    fsm_event(f, world_pd_bool(w, w_str(w, ps(f, s->name))) ? EV(s->isTrue) : EV(s->isFalse));
    act_finish(a);
}
static const act_vtable AV_PlayerDataBoolTest = { "PlayerDataBoolTest", sizeof(st_pdbt), pdbt_bind, pdbt_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* PlayerDataBoolTrueAndFalse — ACT/PlayerDataBoolTrueAndFalse.cs:24-46 */
typedef struct { const fsm_pv *go, *trueBool, *falseBool, *isTrue, *isFalse; } st_pdtf;
static void pdtf_bind(act_inst *a) { ST(st_pdtf); s->go = FIELD(gameObject); s->trueBool = FIELD(trueBool); s->falseBool = FIELD(falseBool); s->isTrue = FIELD(isTrue); s->isFalse = FIELD(isFalse); }
static void pdtf_enter(act_inst *a)
{
    ST(st_pdtf);
    int32_t t = p_owner_default(a, s->go);
    if (t < 0 || !has_game_manager(w, t)) return;
    bool v = world_pd_bool(w, w_str(w, ps(f, s->trueBool))) && !world_pd_bool(w, w_str(w, ps(f, s->falseBool)));
    fsm_event(f, v ? EV(s->isTrue) : EV(s->isFalse));
    act_finish(a);
}
static const act_vtable AV_PlayerDataBoolTrueAndFalse = { "PlayerDataBoolTrueAndFalse", sizeof(st_pdtf), pdtf_bind, pdtf_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* PlayerDataBoolAllTrue — HK/PlayerDataBoolAllTrue.cs:25-75 */
typedef struct { const fsm_pv *go, *names, *trueEvent, *falseEvent, *storeResult, *everyFrame; bool ok; } st_pdat;
static void pdat_bind(act_inst *a) { ST(st_pdat); s->go = FIELD(gameObject); s->names = FIELD(stringVariables); s->trueEvent = FIELD_OPT(trueEvent); s->falseEvent = FIELD_OPT(falseEvent); s->storeResult = FIELD(storeResult); s->everyFrame = FIELD(everyFrame); s->ok = false; }
static void pdat_do(act_inst *a)
{
    ST(st_pdat);
    int32_t n = a_array_len(a, s->names);
    if (n == 0) return;
    bool flag = true;
    for (int32_t i = 0; i < n; i++) if (!world_pd_bool(w, w_str(w, ps(f, a_array_elem(a, s->names, i))))) { flag = false; break; }
    fsm_event(f, flag ? EV(s->trueEvent) : EV(s->falseEvent));
    pb_set(f, s->storeResult, flag);
}
static void pdat_enter(act_inst *a)
{
    ST(st_pdat);
    int32_t t = p_owner_default(a, s->go);
    if (t < 0 || !has_game_manager(w, t)) return;
    s->ok = true;
    pdat_do(a);
    if (!pb(f, s->everyFrame)) act_finish(a);
}
static void pdat_update(act_inst *a) { ST(st_pdat); if (s->ok) pdat_do(a); }
static const act_vtable AV_PlayerDataBoolAllTrue = { "PlayerDataBoolAllTrue", sizeof(st_pdat), pdat_bind, pdat_enter, pdat_update, NULL, NULL, NULL, NULL, NULL };

/* ---- FsmVar (PV_FVAR pool: name, type, float, int, bool, string, v4, useVariable) */
int32_t fvar_type(act_inst *a, const fsm_pv *fv) { return a_pv(a, fv->i + 1)->i; }   /* VariableType (PM/VariableType.cs: Float 0 Int 1 Bool 2 GameObject 3 String 4 Vector2 5 Vector3 6) */
static int fvar_bucket(int32_t type)
{
    switch (type) { case 0: return VB_FLOAT; case 1: return VB_INT; case 2: return VB_BOOL; case 3: return VB_GO; case 4: return VB_STRING;
                    case 5: return VB_V2; case 6: return VB_V3; default: return -1; }
}
static fsm_val *fvar_slot(act_inst *a, const fsm_pv *fv)
{
    if (!fv || fv->kind != PV_FVAR) return NULL;
    if (!a_pv(a, fv->i + 7)->i) return NULL;                       /* useVariable false: literal */
    int32_t name = a_pv(a, fv->i)->i;
    if (name < 0 || !*w_str(a->fsm->w, name)) return NULL;
    int b = fvar_bucket(fvar_type(a, fv));
    if (b < 0) return NULL;
    bool fresh;
    return fsm_get_var(a->fsm, b, name, &fresh);                   /* NamedVariable resolved by name (FsmVar.UpdateValue) */
}
float fvar_float(act_inst *a, const fsm_pv *fv) { fsm_val *v = fvar_slot(a, fv); return v ? v->f : a_pv(a, fv->i + 2)->f[0]; }
int32_t fvar_int(act_inst *a, const fsm_pv *fv) { fsm_val *v = fvar_slot(a, fv); return v ? v->i : a_pv(a, fv->i + 3)->i; }
bool fvar_bool(act_inst *a, const fsm_pv *fv) { fsm_val *v = fvar_slot(a, fv); return (v ? v->i : a_pv(a, fv->i + 4)->i) != 0; }
int32_t fvar_string(act_inst *a, const fsm_pv *fv) { fsm_val *v = fvar_slot(a, fv); return v ? v->i : a_pv(a, fv->i + 5)->i; }
void fvar_store_bool(act_inst *a, const fsm_pv *fv, bool x) { fsm_val *v = fvar_slot(a, fv); if (v && fvar_type(a, fv) == 2) v->i = x ? 1 : 0; }
void fvar_store_int(act_inst *a, const fsm_pv *fv, int32_t x) { fsm_val *v = fvar_slot(a, fv); if (v && fvar_type(a, fv) == 1) v->i = x; }
void fvar_store_float(act_inst *a, const fsm_pv *fv, float x) { fsm_val *v = fvar_slot(a, fv); if (v && fvar_type(a, fv) == 0) v->f = x; }
void fvar_store_string(act_inst *a, const fsm_pv *fv, int32_t sid) { fsm_val *v = fvar_slot(a, fv); if (v && fvar_type(a, fv) == 4) v->i = sid; }

/* ---- StaticVariableList (HK/StaticVariableList.cs: a process-wide dictionary, per world here) */
static fsm_val *svar_find(fsm_world *w, int32_t name, bool create)
{
    for (int32_t i = 0; i < w->n_svars; i++) if (w->svars[i].name == name) return &w->svars[i].v;
    if (!create) return NULL;
    w->svars = realloc(w->svars, sizeof *w->svars * (size_t)(w->n_svars + 1));
    w->svars[w->n_svars].name = name; memset(&w->svars[w->n_svars].v, 0, sizeof(fsm_val));
    return &w->svars[w->n_svars++].v;
}
/* GetStaticVariable — HK/GetStaticVariable.cs:17-47 */
typedef struct { const fsm_pv *name, *value; } st_svar;
static void gsv_bind(act_inst *a) { ST(st_svar); s->name = FIELD(variableName); s->value = FIELD(storeValue); }
static void gsv_enter(act_inst *a)
{
    ST(st_svar);
    fsm_val *sv = p_isnone(s->name) ? NULL : svar_find(w, ps(f, s->name), false);
    if (sv && s->value->kind == PV_FVAR) {
        switch (fvar_type(a, s->value)) {
        case 2: fvar_store_bool(a, s->value, sv->i != 0); break;
        case 1: fvar_store_int(a, s->value, sv->i); break;
        case 0: fvar_store_float(a, s->value, sv->f); break;
        case 4: fvar_store_string(a, s->value, sv->i); break;
        default: break;                                            /* Debug.LogWarning: type not implemented */
        }
    }
    act_finish(a);
}
static const act_vtable AV_GetStaticVariable = { "GetStaticVariable", sizeof(st_svar), gsv_bind, gsv_enter, NULL, NULL, NULL, NULL, NULL, NULL };
/* SetStaticVariable — HK/SetStaticVariable.cs:17-48 */
static void ssv_bind(act_inst *a) { ST(st_svar); s->name = FIELD(variableName); s->value = FIELD(setValue); }
static void ssv_enter(act_inst *a)
{
    ST(st_svar);
    if (!p_isnone(s->name) && s->value->kind == PV_FVAR) {
        fsm_val *sv = svar_find(w, ps(f, s->name), true);
        switch (fvar_type(a, s->value)) {
        case 2: sv->i = fvar_bool(a, s->value) ? 1 : 0; break;
        case 1: sv->i = fvar_int(a, s->value); break;
        case 0: sv->f = fvar_float(a, s->value); break;
        case 4: sv->i = fvar_string(a, s->value); break;
        default: break;
        }
    }
    act_finish(a);
}
static const act_vtable AV_SetStaticVariable = { "SetStaticVariable", sizeof(st_svar), ssv_bind, ssv_enter, NULL, NULL, NULL, NULL, NULL, NULL };
/* CheckStaticBool — HK/CheckStaticBool.cs:17-25: falseEvent is sent unconditionally after the true check (sic) */
typedef struct { const fsm_pv *name, *trueEvent, *falseEvent; } st_csb;
static void csb_bind(act_inst *a) { ST(st_csb); s->name = FIELD(variableName); s->trueEvent = FIELD_OPT(trueEvent); s->falseEvent = FIELD_OPT(falseEvent); }
static void csb_enter(act_inst *a)
{
    ST(st_csb);
    fsm_val *sv = p_isnone(s->name) ? NULL : svar_find(w, ps(f, s->name), false);
    if (sv && sv->i) fsm_event(f, EV(s->trueEvent));
    fsm_event(f, EV(s->falseEvent));
    act_finish(a);
}
static const act_vtable AV_CheckStaticBool = { "CheckStaticBool", sizeof(st_csb), csb_bind, csb_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* ConvertStringToInt -- ACT/ConvertStringToInt.cs:13-28: int.Parse, which throws on anything but an optionally
 * signed decimal integer with surrounding white space (trapped) */
typedef struct { const fsm_pv *str, *out, *everyFrame; } st_csi;
static void csi_bind(act_inst *a) { ST(st_csi); s->str = FIELD(stringVariable); s->out = FIELD(intVariable); s->everyFrame = FIELD(everyFrame); }
static void csi_do(act_inst *a)
{
    ST(st_csi);
    const char *p = w_str(w, ps(f, s->str));
    while (*p == ' ' || *p == '\t') p++;
    int neg = 0;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    if (*p < '0' || *p > '9') HKSIM_UNIMPLEMENTED("ConvertStringToInt: int.Parse(\"%s\") throws in %s", w_str(w, ps(f, s->str)), fsm_label(f));
    int64_t v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); if (v > 2147483648LL) HKSIM_UNIMPLEMENTED("ConvertStringToInt: overflow in %s", fsm_label(f)); }
    while (*p == ' ' || *p == '\t') p++;
    if (*p) HKSIM_UNIMPLEMENTED("ConvertStringToInt: int.Parse(\"%s\") throws in %s", w_str(w, ps(f, s->str)), fsm_label(f));
    pi_set(f, s->out, (int32_t)(neg ? -v : v));
}
static void csi_enter(act_inst *a) { ST(st_csi); csi_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_ConvertStringToInt = { "ConvertStringToInt", sizeof(st_csi), csi_bind, csi_enter, csi_do, NULL, NULL, NULL, NULL, NULL };

/* GetFsmString -- ACT/GetFsmString.cs:18-54: the FSM cached by GameObject; GetFsmString mints on a miss (its "")  */
static void getfsmstr_do(act_inst *a)
{
    ST(st_getfsm);
    if (!getfsm_resolve(a)) return;
    bool fresh; fsm_val *v = fsm_get_var(&w->fsms[s->fsm], VB_STRING, ps(f, s->variableName), &fresh);
    ps_set(f, s->storeValue, fresh ? w_intern(w, "") : v->i);
}
static void getfsmstr_enter(act_inst *a) { ST(st_getfsm); getfsmstr_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GetFsmString = { "GetFsmString", sizeof(st_getfsm), getfsm_bind, getfsmstr_enter, getfsmstr_do, NULL, NULL, NULL, NULL, NULL };

/* SetFsmColor -- ACT/SetFsmColor.cs:19-62, SetFsmVector2 -- ACT/SetFsmVector2.cs:19-62: GetFsmColor / GetFsmVector2
 * (a miss mints a throw-away, so the write is lost) */
static void setfsmvec_do(act_inst *a, int bucket)
{
    ST(st_setfsm);
    if (!setfsm_resolve(a)) return;
    bool fresh; fsm_val *v = fsm_get_var(&w->fsms[s->fsm], bucket, ps(f, s->variableName), &fresh);
    if (!fresh) memcpy(v->v, pv3(f, s->setValue), sizeof v->v);
}
static void setfsmcolor_do(act_inst *a) { setfsmvec_do(a, VB_COLOR); }
static void setfsmcolor_enter(act_inst *a) { ST(st_setfsm); setfsmcolor_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_SetFsmColor = { "SetFsmColor", sizeof(st_setfsm), setfsm_bind, setfsmcolor_enter, setfsmcolor_do, NULL, NULL, NULL, NULL, NULL };
static void setfsmv2_do(act_inst *a) { setfsmvec_do(a, VB_V2); }
static void setfsmv2_enter(act_inst *a) { ST(st_setfsm); setfsmv2_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_SetFsmVector2 = { "SetFsmVector2", sizeof(st_setfsm), setfsm_bind, setfsmv2_enter, setfsmv2_do, NULL, NULL, NULL, NULL, NULL };

/* SetFsmStringReturn -- ACT/SetFsmStringReturn.cs:19-60: set another FSM's string, restore the previous value on
 * exit; a null target or a missing FSM returns before Finish() (:23-32) */
typedef struct { const fsm_pv *go, *fsmName, *variableName, *setValue; int32_t fsm, var, prev; } st_sfsr;
static void sfsr_bind(act_inst *a) { ST(st_sfsr); s->go = FIELD(gameObject); s->fsmName = FIELD(fsmName); s->variableName = FIELD(variableName); s->setValue = FIELD(setValue); s->fsm = -1; }
static void sfsr_enter(act_inst *a)
{
    ST(st_sfsr);
    s->fsm = -1;
    int32_t t = p_owner_default(a, s->go);
    if (t < 0) return;
    int32_t fi = get_game_object_fsm(w, t, w_str(w, ps(f, s->fsmName)));
    if (fi < 0) return;
    bool fresh; fsm_val *v = fsm_get_var(&w->fsms[fi], VB_STRING, ps(f, s->variableName), &fresh);
    if (!fresh) { s->fsm = fi; s->var = ps(f, s->variableName); s->prev = v->i; v->i = ps(f, s->setValue); }
    act_finish(a);
}
static void sfsr_exit(act_inst *a)
{
    ST(st_sfsr);
    if (s->fsm < 0) return;
    bool fresh; fsm_val *v = fsm_get_var(&w->fsms[s->fsm], VB_STRING, s->var, &fresh);
    if (!fresh) v->i = s->prev;
}
static const act_vtable AV_SetFsmStringReturn = { "SetFsmStringReturn", sizeof(st_sfsr), sfsr_bind, sfsr_enter, NULL, NULL, NULL, sfsr_exit, NULL, NULL };

/* SetGameObjectSelf -- ACT/SetGameObjectSelf.cs:19-43: FsmOwnerDefault.GetSafe, kept when null */
typedef struct { const fsm_pv *variable, *go, *everyFrame; } st_sgos;
static void sgos_bind(act_inst *a) { ST(st_sgos); s->variable = FIELD(variable); s->go = FIELD(gameObject); s->everyFrame = FIELD(everyFrame); }
static void sgos_do(act_inst *a) { ST(st_sgos); int32_t t = p_get_safe(a, s->go); if (t >= 0) pgo_set(f, s->variable, t); }
static void sgos_enter(act_inst *a) { ST(st_sgos); sgos_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_SetGameObjectSelf = { "SetGameObjectSelf", sizeof(st_sgos), sgos_bind, sgos_enter, sgos_do, NULL, NULL, NULL, NULL, NULL };

/* EnableFSM — ACT/EnableFSM.cs:24-75: Behaviour.enabled on a PlayMakerFSM (by name, or the first one).  A
 * missing FSM is a LogError with no state change (no trap).  OnExit's "reset" is the game's own logic, not a
 * restore of the state the FSM was in before this action ran: it sets enabled to !enable.Value. */
typedef struct { const fsm_pv *go, *fsmName, *enable, *resetOnExit; int32_t fsm; } st_efsm;
static void efsm_bind(act_inst *a) { ST(st_efsm); s->go = FIELD(gameObject); s->fsmName = FIELD(fsmName); s->enable = FIELD(enable); s->resetOnExit = FIELD(resetOnExit); s->fsm = -1; }
static void efsm_enter(act_inst *a)
{
    ST(st_efsm);
    s->fsm = -1;
    int32_t t = p_owner_default(a, s->go);
    if (t >= 0) {
        int32_t fi = get_game_object_fsm(w, t, w_str(w, ps(f, s->fsmName)));
        if (fi >= 0) { s->fsm = fi; lc_fsm_set_enabled(w, fi, pb(f, s->enable)); }
    }
    act_finish(a);
}
static void efsm_exit(act_inst *a) { ST(st_efsm); if (s->fsm >= 0 && pb(f, s->resetOnExit)) lc_fsm_set_enabled(w, s->fsm, !pb(f, s->enable)); }
static const act_vtable AV_EnableFSM = { "EnableFSM", sizeof(st_efsm), efsm_bind, efsm_enter, NULL, NULL, NULL, efsm_exit, NULL, NULL };

/* FinishFSM — ACT/FinishFSM.cs:5-11: base.Fsm.Stop() (fsm_stop, fsm_rt.c).  No Finish() call in the C#, so the
 * action instance itself is left running (moot once the FSM is finished). */
static void finishfsm_enter(act_inst *a) { fsm_stop(a->fsm); }
static const act_vtable AV_FinishFSM = { "FinishFSM", 0, NULL, finishfsm_enter, NULL, NULL, NULL, NULL, NULL, NULL };

const act_vtable *const act_registry_variables[] = {
    &AV_GetOwner, &AV_GetHero, &AV_GetParent, &AV_FindChild, &AV_GetChild, &AV_FindGameObject, &AV_GetTagCount,
    &AV_FindClosest, &AV_GetTag, &AV_SetTag, &AV_GetLayer, &AV_GetName,
    &AV_SetFsmBool, &AV_SetFsmFloat, &AV_SetFsmInt, &AV_SetFsmString, &AV_SetFsmGameObject, &AV_SetFsmVector3,
    &AV_GetFsmBool, &AV_GetFsmFloat, &AV_GetFsmInt, &AV_GetFsmGameObject,
    &AV_GetPlayerDataInt, &AV_GetPlayerDataFloat, &AV_GetPlayerDataString, &AV_GetPlayerDataVector3, &AV_SetPlayerDataFloat,
    &AV_SetPlayerDataString, &AV_PlayerDataIntAdd, &AV_IncrementPlayerDataInt, &AV_GetPlayerDataBool,
    &AV_SetPlayerDataBool, &AV_SetPlayerDataInt, &AV_PlayerDataBoolTest, &AV_PlayerDataBoolTrueAndFalse,
    &AV_PlayerDataBoolAllTrue, &AV_GetStaticVariable, &AV_SetStaticVariable, &AV_CheckStaticBool,
    &AV_ConvertStringToInt, &AV_GetFsmString, &AV_SetFsmColor, &AV_SetFsmVector2, &AV_SetFsmStringReturn, &AV_SetGameObjectSelf,
    &AV_EnableFSM, &AV_FinishFSM,
};
const int act_registry_variables_n = (int)(sizeof act_registry_variables / sizeof act_registry_variables[0]);
