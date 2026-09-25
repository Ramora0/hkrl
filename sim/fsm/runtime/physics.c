/* The FSM world's side of sim/phys: Rigidbody2D and Collider2D state, raycasts, the bodies and shapes
 * the world owns, the pre-step shape flush, and physics callbacks into PlayMakerUnity2DProxy / Fsm.OnTrigger*2D /
 * OnCollision*2D, AlertRange and TinkEffect (HK/PlayMakerUnity2DProxy.cs, HK/AlertRange.cs, HK/TinkEffect.cs). */
#include "fsm/fsm.h"
#include "fsm/lifecycle.h"
#include "world_internal.h"
#include "core/phys.h"
#include "core/sim_modules.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "core/alloc.h"

static void chain_to_body_rot(fsm_world *w, int32_t cg, int32_t bg, float off[2], float scale[2], float *angle_deg);
/* Re-bake one rigidbody-backed collider into its body's frame (chain from the collider up to its
 * Rigidbody2D GameObject, as offset + axis scale + angle), every collider type.  Unity rebuilds a fixture
 * whenever its transform changes (physics.json#Physics2D.autoSyncTransforms = true).  Same formulas as
 * phys_add_shapes_of_body, so an unchanged chain re-bakes to identical geometry. */
static void rebake_col_to_body(fsm_world *w, int32_t go, col_inst *c)
{
    phys_world *pw = (phys_world *)w->phys;
    {
        float off[2], sc[2], ang; chain_to_body_rot(w, go, c->def->rb_go, off, sc, &ang);
        float ca = 1.0f, sa = 0.0f;
        if (ang != 0.0f) { float ar = ang * ((float)M_PI / 180.0f); ca = m_cos(ar); sa = m_sin(ar); }
        if (c->def->type == COL_CIRCLE) {
            float rs = fabsf(sc[0]) > fabsf(sc[1]) ? fabsf(sc[0]) : fabsf(sc[1]);   /* CircleCollider2D: radius x max(|sx|,|sy|) */
            float lx = c->offset[0] * sc[0], ly = c->offset[1] * sc[1];
            phys_shape_set_circle(pw, c->shape, (phys_v2){ off[0] + ca * lx - sa * ly, off[1] + sa * lx + ca * ly }, c->radius * rs);
        } else if (c->def->type == COL_BOX && ang == 0.0f) {
            phys_shape_set_box(pw, c->shape, (phys_v2){ off[0] + c->offset[0] * sc[0], off[1] + c->offset[1] * sc[1] }, (phys_v2){ c->size[0] * sc[0], c->size[1] * sc[1] });
        } else if (c->def->type == COL_BOX) {   /* rotated relative to its body: its four corners as a polygon (as at bind) */
            float hx = c->size[0] * sc[0] * 0.5f, hy = c->size[1] * sc[1] * 0.5f, cx = c->offset[0] * sc[0], cy = c->offset[1] * sc[1];
            float corner[4][2] = { { cx - hx, cy - hy }, { cx + hx, cy - hy }, { cx + hx, cy + hy }, { cx - hx, cy + hy } };
            phys_v2 pts[4];
            for (int i = 0; i < 4; i++) pts[i] = (phys_v2){ off[0] + ca * corner[i][0] - sa * corner[i][1], off[1] + sa * corner[i][0] + ca * corner[i][1] };
            phys_shape_set_points(pw, c->shape, pts, 4);
        } else if ((c->def->type == COL_POLYGON || c->def->type == COL_EDGE) && c->def->n_pts > 0) {
            phys_v2 *pts = (phys_v2 *)malloc(sizeof(phys_v2) * (size_t)c->def->n_pts);
            HKSIM_ASSERT(pts != NULL, "out of memory re-baking a polygon collider");
            for (int32_t i = 0; i < c->def->n_pts; i++) {
                const float *pp = &w->sc->col_pts[(c->def->pts_start + i) * 2];
                float lx = (pp[0] + c->offset[0]) * sc[0], ly = (pp[1] + c->offset[1]) * sc[1];   /* points are offset-relative */
                pts[i] = (phys_v2){ off[0] + ca * lx - sa * ly, off[1] + sa * lx + ca * ly };
            }
            phys_shape_set_points(pw, c->shape, pts, (uint32_t)c->def->n_pts);
            free(pts);
        }
    }
}
void phys_rescale_shapes(fsm_world *w, int32_t go)
{
    if (!w->phys) return;
    go_inst *g = &w->gos[go];
    for (int32_t k = 0; k < g->n_cols; k++) {
        col_inst *c = &g->cols[k];
        if (!c->shape || c->def->rb_go < 0) continue;
        rebake_col_to_body(w, go, c);
    }
}

/* ---- bodies ---- */
bool go_has_rb(const fsm_world *w, int32_t go) { return go >= 0 && w->gos[go].has_rb; }
static void require_rb(const fsm_world *w, int32_t go)
{
    HKSIM_ASSERT(go >= 0, "Rigidbody2D access on a null GameObject");
    if (!w->gos[go].has_rb) HKSIM_UNIMPLEMENTED("no Rigidbody2D dumped for '%s' (scene.json)", go_path(w, go));
}
void go_velocity(fsm_world *w, int32_t go, float out[2])
{
    require_rb(w, go);
    if (w->phys && w->gos[go].body) {
        phys_v2 v = phys_body_velocity((const phys_world *)w->phys, w->gos[go].body);
        out[0] = v.x; out[1] = v.y;
        return;
    }
    out[0] = w->gos[go].vel[0]; out[1] = w->gos[go].vel[1];
}
void go_set_velocity(fsm_world *w, int32_t go, const float v[2])
{
    require_rb(w, go);
    w->gos[go].vel[0] = v[0]; w->gos[go].vel[1] = v[1];
    if (w->phys && w->gos[go].body) phys_body_set_velocity((phys_world *)w->phys, w->gos[go].body, (phys_v2){ v[0], v[1] });
}
/* Rigidbody2D.AddForce(force, ForceMode2D.Force) -> b2Body::ApplyForceToCenter.  No FSM-side mirror:
 * the accumulator lives in the body for one step (sim/phys island_solve consumes it). */
void go_add_force(fsm_world *w, int32_t go, const float f[2])
{
    require_rb(w, go);
    if (w->phys && w->gos[go].body) phys_body_add_force((phys_world *)w->phys, w->gos[go].body, (phys_v2){ f[0], f[1] });
}
/* Rigidbody2D.angularVelocity / AddTorque: the angular state lives only in the body (sim/phys), so an object
 * without one has none to read or write. */
static phys_body_id require_body(const fsm_world *w, int32_t go, const char *what)
{
    require_rb(w, go);
    if (!w->phys || !w->gos[go].body) HKSIM_UNIMPLEMENTED("%s on '%s', which has no physics body", what, go_path(w, go));
    return w->gos[go].body;
}
float go_angular_velocity(fsm_world *w, int32_t go)
{
    return phys_body_angular_velocity((const phys_world *)w->phys, require_body(w, go, "Rigidbody2D.angularVelocity"));
}
void go_set_angular_velocity(fsm_world *w, int32_t go, float deg_per_s)
{
    phys_body_set_angular_velocity((phys_world *)w->phys, require_body(w, go, "Rigidbody2D.angularVelocity"), deg_per_s);
}
void go_add_torque(fsm_world *w, int32_t go, float torque, bool impulse)
{
    phys_body_add_torque((phys_world *)w->phys, require_body(w, go, "Rigidbody2D.AddTorque"), torque, impulse);
}
void go_set_gravity_scale(fsm_world *w, int32_t go, float g)
{
    require_rb(w, go);
    w->gos[go].gravity_scale = g;
    if (w->phys && w->gos[go].body) phys_body_set_gravity_scale((phys_world *)w->phys, w->gos[go].body, g);
}
/* SetRigidbodySimulated2D.cs:25-36 (Rigidbody2D.simulated), a direct write independent of the automatic
 * GameObject-activity toggle (world_go_phys_enable/disable below).  No go_inst mirror: nothing else reads it
 * back, matching this file's other direct passthroughs; a body created later for a deferred pooled object
 * (world_phys_ensure_body) starts from the dumped Rigidbody2D.simulated, same limitation as
 * go_set_gravity_scale/go_set_kinematic above for a write before that body exists. */
void go_set_simulated(fsm_world *w, int32_t go, bool on)
{
    require_rb(w, go);
    if (w->phys && w->gos[go].body) phys_body_set_simulated((phys_world *)w->phys, w->gos[go].body, on);
}
void go_set_kinematic(fsm_world *w, int32_t go, bool k)
{
    require_rb(w, go);
    w->gos[go].kinematic = k ? 1 : 0;
    if (w->phys && w->gos[go].body) phys_body_set_type((phys_world *)w->phys, w->gos[go].body, k ? PHYS_BODY_KINEMATIC : PHYS_BODY_DYNAMIC);
}

/* ---- colliders ---- */
col_inst *go_box_collider(fsm_world *w, int32_t go)
{
    if (go < 0) return NULL;
    for (int32_t i = 0; i < w->gos[go].n_cols; i++) if (w->gos[go].cols[i].def->type == COL_BOX) return &w->gos[go].cols[i];
    return NULL;
}
col_inst *go_poly_collider(fsm_world *w, int32_t go)
{
    if (go < 0) return NULL;
    for (int32_t i = 0; i < w->gos[go].n_cols; i++) if (w->gos[go].cols[i].def->type == COL_POLYGON) return &w->gos[go].cols[i];
    return NULL;
}
col_inst *go_first_collider(fsm_world *w, int32_t go)
{
    if (go < 0 || w->gos[go].n_cols == 0) return NULL;
    return &w->gos[go].cols[0];
}
/* Collider2D.bounds: world-space AABB of the shape under the owner's world TRS. */
void col_bounds(fsm_world *w, int32_t go, const col_inst *c, float min[2], float max[2])
{
    /* A disabled collider, or one on an inactive object, reports ZERO-size bounds centred on the
     * transform position: dumps_all/GG_Hornet_1/scene.json (every such collider has bounds.size 0, e.g.
     * `Boss Holder/Hornet Boss 1/Corpse Hornet GG(Clone)`), physics.json#heroColliders `Knight/Attacks/Slash`.
     * CheckCollisionSide, BoundsBoxCollider and Recoil read bounds unfiltered (the observation filters, HO:742). */
    if (!(c->enabled && go_active_in_hierarchy(w, go))) {
        ensure_transform_clean(w, go);
        const float *p = w->gos[go].world_pos;
        min[0] = max[0] = p[0]; min[1] = max[1] = p[1];
        return;
    }
    col_shape_bounds(w, go, c, min, max);
}
/* The same AABB computed from the collider's shape regardless of `enabled`: the box an armed hazard will
 * occupy once its collider switches on (the observation's armed rows, HitboxObserver.ShapeBounds). */
void col_shape_bounds(fsm_world *w, int32_t go, const col_inst *c, float min[2], float max[2])
{
    ensure_transform_clean(w, go);
    const go_inst *g = &w->gos[go];
    const float *p = g->world_pos;
    const float *s = g->lossy_scale;
    float ang = g->world_euler_z * ((float)M_PI / 180.0f);
    float cs = m_cos(ang), sn = m_sin(ang);
    float pts[8][2]; int n = 0;
    if (c->def->type == COL_BOX) {
        float hx = c->size[0] * 0.5f, hy = c->size[1] * 0.5f;
        float corners[4][2] = { { c->offset[0] - hx, c->offset[1] - hy }, { c->offset[0] + hx, c->offset[1] - hy },
                                { c->offset[0] + hx, c->offset[1] + hy }, { c->offset[0] - hx, c->offset[1] + hy } };
        for (int i = 0; i < 4; i++) { pts[n][0] = corners[i][0]; pts[n][1] = corners[i][1]; n++; }
    } else if (c->def->type == COL_CIRCLE) {
        float r = c->radius * fmaxf(fabsf(s[0]), fabsf(s[1]));
        float cx = p[0] + (c->offset[0] * s[0]) * cs - (c->offset[1] * s[1]) * sn;
        float cy = p[1] + (c->offset[0] * s[0]) * sn + (c->offset[1] * s[1]) * cs;
        min[0] = cx - r; min[1] = cy - r; max[0] = cx + r; max[1] = cy + r;
        return;
    } else if ((c->def->type == COL_POLYGON || c->def->type == COL_EDGE) && c->def->n_pts > 0) {
        /* PolygonCollider2D / EdgeCollider2D bounds: AABB of the transformed points (offset-relative) [ENGINE, obs-wire.md Q-obs-4] */
        min[0] = min[1] = INFINITY; max[0] = max[1] = -INFINITY;
        for (int32_t i = 0; i < c->def->n_pts; i++) {
            const float *q = &w->sc->col_pts[(c->def->pts_start + i) * 2];
            float lx = (q[0] + c->offset[0]) * s[0], ly = (q[1] + c->offset[1]) * s[1];
            float wx = p[0] + lx * cs - ly * sn, wy = p[1] + lx * sn + ly * cs;
            if (wx < min[0]) min[0] = wx;
            if (wx > max[0]) max[0] = wx;
            if (wy < min[1]) min[1] = wy;
            if (wy > max[1]) max[1] = wy;
        }
        return;
    } else {
        HKSIM_UNIMPLEMENTED("Collider2D.bounds for collider type %d on '%s'", c->def->type, go_path(w, go));
    }
    min[0] = min[1] = INFINITY; max[0] = max[1] = -INFINITY;
    for (int i = 0; i < n; i++) {
        float lx = pts[i][0] * s[0], ly = pts[i][1] * s[1];
        float wx = p[0] + lx * cs - ly * sn, wy = p[1] + lx * sn + ly * cs;
        if (wx < min[0]) min[0] = wx;
        if (wx > max[0]) max[0] = wx;
        if (wy < min[1]) min[1] = wy;
        if (wy > max[1]) max[1] = wy;
    }
}
/* R5: disabling a Collider2D ends its contacts inside the call, outside the physics step; most such Exits
 * come from FSM-driven hitboxes toggled here (delivery of the ended contacts: A-12). */
/* Behaviour.enabled of a Collider2D.  On an inactive object only the flag changes: Collider2D::AwakeFromLoad creates or
 * destroys the fixtures only when GameObject::IsActive (UP!0x180bfec70, native-animator.md N-AN-10); activation then
 * builds them from the flag (world_go_phys_enable). */
void col_set_enabled(fsm_world *w, col_inst *c, bool on)
{
    c->enabled = on ? 1 : 0;
    if (w->phys && c->shape) {
        phys_shape_set_enabled((phys_world *)w->phys, c->shape, on && go_active_in_hierarchy(w, c->def->go));
        lc_physics_exit_on_disable(w);
    }
}
void col_set_trigger(fsm_world *w, col_inst *c, bool on)
{
    c->is_trigger = on ? 1 : 0;
    if (w->phys && c->shape) phys_shape_set_trigger((phys_world *)w->phys, c->shape, on);
}
void col_set_box(fsm_world *w, int32_t go, col_inst *c, const float *size, const float *offset)
{
    if (size) { c->size[0] = size[0]; c->size[1] = size[1]; }
    if (offset) { c->offset[0] = offset[0]; c->offset[1] = offset[1]; }
    if (!w->phys || !c->shape) return;
    if (c->def->rb_go >= 0 && c->def->rb_go != go) {
        /* Shape geometry lives in the BODY's frame, so a box on a CHILD of the Rigidbody2D object needs the
         * child-to-body offset/scale/angle chain, not the raw collider-local values pushed below. */
        rebake_col_to_body(w, go, c);
        return;
    }
    phys_shape_set_box((phys_world *)w->phys, c->shape, (phys_v2){ c->offset[0], c->offset[1] }, (phys_v2){ c->size[0], c->size[1] });
}

/* Physics2D.Raycast for CheckCollisionSide (Q-hornet-6): only through sim/phys. */
/* owner handle -> GameObject (sim_modules.h HKSIM_USER_*: hero body 1, slashes 2..6, static colliders 0x1000+k, FSM objects
 * HKSIM_USER_FSM_BASE + go).  Static colliders and the slash shapes have no FSM-world object (-1). */
int32_t world_go_of_user(fsm_world *w, uint32_t user)
{
    if (user >= HKSIM_USER_FSM_BASE) { int32_t g = (int32_t)(user - HKSIM_USER_FSM_BASE); return g < w->n_gos ? g : -1; }
    if (user == HKSIM_USER_HERO) return w->knight_go;
    if (user >= HKSIM_USER_STATIC_BASE && user - HKSIM_USER_STATIC_BASE < w->n_static_go)
        return w->static_go[user - HKSIM_USER_STATIC_BASE];   /* core-owned static collider, bound by world_bind_statics */
    return -1;
}
bool world_raycast_ex(fsm_world *w, const float origin[2], const float dir[2], float len, uint32_t layer_mask,
                      float hit_point[2], float hit_normal[2], int32_t *hit_go, bool *hit_is_trigger)
{
    if (!(w->phys))
        HKSIM_UNIMPLEMENTED("Physics2D.Raycast needs sim/phys (not linked yet)");
    phys_v2 hp = { 0, 0 }, hn = { 0, 0 }; phys_shape_id hs = 0;
    if (isinf(len)) len = 1.0e4f;                                  /* Physics2D.Raycast(+inf): the scene is < 100 units */
    bool hit = phys_raycast((const phys_world *)w->phys, (phys_v2){ origin[0], origin[1] }, (phys_v2){ dir[0], dir[1] }, len, layer_mask, &hp, &hn, &hs);
    if (hit_point) { hit_point[0] = hp.x; hit_point[1] = hp.y; }
    if (hit_normal) { hit_normal[0] = hn.x; hit_normal[1] = hn.y; }
    if (hit_go) *hit_go = (hit) ? world_go_of_user(w, phys_shape_user((const phys_world *)w->phys, hs)) : -1;
    if (hit_is_trigger) *hit_is_trigger = (hit) ? phys_shape_is_trigger((const phys_world *)w->phys, hs) : false;
    return hit;
}
bool world_raycast(fsm_world *w, const float origin[2], const float dir[2], float len, uint32_t layer_mask, bool *hit_is_trigger)
{
    return world_raycast_ex(w, origin, dir, len, layer_mask, NULL, NULL, NULL, hit_is_trigger);
}

/* AlertRange.Find -- HK/AlertRange.cs:46-66: walk the root's direct children (in sibling order) and
 * return the first that carries an AlertRange, optionally requiring the child's name.  The component
 * list is compiled into the tables, so the search needs no extra generator output. */
int32_t world_alert_range_find(fsm_world *w, int32_t root, const char *child_name)
{
    if (root < 0) return -1;
    bool named = child_name && child_name[0];
    for (int32_t c = w->gos[root].first_child; c >= 0; c = w->gos[c].next_sibling) {
        if (named && strcmp(go_name(w, c), child_name) != 0) continue;
        if (go_has_component(w, c, "AlertRange")) return c;
    }
    return -1;
}

/* AlertRange.isHeroInRange -- HK/AlertRange.cs:7-27: a LATCH, set by OnTriggerEnter2D from ANY collider
 * the layer matrix lets reach it and cleared by OnTriggerExit2D unless the object has more than one
 * Collider2D and StillInColliders still finds a layer-9 collider.  OnTrigger*2D reaches disabled
 * MonoBehaviours too, so the latch ignores the component's enabled flag. */
bool world_alert_in_range(fsm_world *w, int32_t go)
{
    return go >= 0 && w->gos[go].alert_in_range != 0;
}
/* AlertRange.StillInColliders (:29-45): every Collider2D on the object (GetComponents, Awake), in order;
 * a circle queries OverlapCircle(TransformPoint(offset), radius * max(localScale.x, localScale.y), 512),
 * a box OverlapBox(TransformPoint(offset), size * localScale, eulerAngles.z, 512); other types leave the
 * flag as it was.  Note LOCAL scale, as the source has it. */
static bool alert_still_in_colliders(fsm_world *w, int32_t go)
{
    go_inst *g = &w->gos[go];
    float p[3], ls[3], lossy[3]; go_world_pos(w, go, p); go_local_scale(w, go, ls); go_lossy_scale(w, go, lossy);
    float ez = go_euler_z(w, go), a = ez * ((float)M_PI / 180.0f), ca = m_cos(a), sa = m_sin(a);
    bool flag = false;
    for (int32_t k = 0; k < g->n_cols && !flag; k++) {
        const col_inst *c = &g->cols[k];
        float ox = c->offset[0] * lossy[0], oy = c->offset[1] * lossy[1];                 /* Transform.TransformPoint */
        phys_v2 ctr = { p[0] + (ox * ca - oy * sa), p[1] + (ox * sa + oy * ca) };
        if (c->def->type == COL_CIRCLE) {
            float r = c->radius * fmaxf(ls[0], ls[1]);
            if (r <= 0.0f) HKSIM_UNIMPLEMENTED("AlertRange.StillInColliders on '%s': OverlapCircle radius %g (negative localScale)", go_path(w, go), (double)r);
            flag = phys_overlap_circle((const phys_world *)w->phys, ctr, r, 512u) != 0;
        } else if (c->def->type == COL_BOX) {
            phys_v2 sz = { c->size[0] * ls[0], c->size[1] * ls[1] };
            if (sz.x <= 0.0f || sz.y <= 0.0f)
                HKSIM_UNIMPLEMENTED("AlertRange.StillInColliders on '%s': OverlapBox size (%g,%g) from a negative localScale", go_path(w, go), (double)sz.x, (double)sz.y);
            flag = phys_overlap_box((const phys_world *)w->phys, ctr, sz, ez, 512u) != 0;
        }
    }
    return flag;
}
static void alert_range_callback(fsm_world *w, int32_t go, int kind)
{
    /* Enter sets the latch (:17-20).  STAY sets it too: Unity never delivers a Stay without its Enter, so
     * the game's latch is already true then, and an overlap that exists at scene start may reach the
     * simulator only as Stays. */
    if (kind == PHYS_EV_TRIGGER_ENTER || kind == PHYS_EV_TRIGGER_STAY) w->gos[go].alert_in_range = 1;
    else if (kind == PHYS_EV_TRIGGER_EXIT) {                                            /* :22-27 */
        if (w->gos[go].n_cols <= 1 || !alert_still_in_colliders(w, go)) w->gos[go].alert_in_range = 0;
    }
}

/* ---- PlayMakerUnity2DProxy delegates (HK/PlayMakerUnity2DProxy.cs:56-118) ---- */
void proxy_add(fsm_world *w, int32_t go, int kind, act_inst *a)
{
    if (!w->gos[go].dlg) {
        w->gos[go].dlg = calloc(1, sizeof *w->gos[go].dlg);
        HKSIM_ASSERT(w->gos[go].dlg != NULL, "out of memory registering a PlayMakerUnity2DProxy delegate");
    }
    delegate_list *l = &w->gos[go].dlg->l[kind];
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 4; l->a = realloc(l->a, sizeof(act_inst *) * (size_t)l->cap); }
    l->a[l->n++] = a;                                              /* Delegate.Combine: append */
}
void proxy_remove(fsm_world *w, int32_t go, int kind, act_inst *a)
{
    if (!w->gos[go].dlg) return;
    delegate_list *l = &w->gos[go].dlg->l[kind];
    for (int32_t i = l->n - 1; i >= 0; i--) {                     /* Delegate.Remove: last occurrence */
        if (l->a[i] == a) { memmove(&l->a[i], &l->a[i + 1], sizeof(act_inst *) * (size_t)(l->n - i - 1)); l->n--; return; }
    }
}
/* dispatch to a snapshot of the list (a delegate may unregister itself while running).  Collision2dEvent
 * registers on this list too, but in the game it is an Fsm-level DoCollision*2D override
 * (ACT/Collision2dEvent.cs:40-89) delivered through world_fsm_2d below, so it is skipped here. */
static void proxy_dispatch(fsm_world *w, int32_t go, int kind, int32_t other_go, int32_t other_layer)
{
    if (!w->gos[go].dlg) return;
    delegate_list *l = &w->gos[go].dlg->l[kind];
    if (l->n == 0) return;
    int32_t n = l->n;
    act_inst **snap = malloc(sizeof(act_inst *) * (size_t)n);
    memcpy(snap, l->a, sizeof(act_inst *) * (size_t)n);
    for (int32_t i = 0; i < n; i++) if (!(kind < 3 && snap[i]->vt && strcmp(snap[i]->vt->type_short, "Collision2dEvent") == 0)) act_proxy_callback(snap[i], kind, other_go, other_layer);
    free(snap);
}
/* PlayMakerTrigger{Enter,Stay,Exit}2D / PlayMakerCollision{Enter,Stay,Exit}2D (PM PlayMakerTriggerEnter2D.cs:6-16,
 * PlayMakerCollisionEnter2D.cs:6-13 and twins): PlayMakerFSM.AddEventHandlerComponents (PlayMakerFSM.cs:212-270)
 * adds one of these to the FSM's GameObject when the FSM's Handle*2D flag is set, and it forwards the
 * callback to `Fsm.OnTrigger*2D / OnCollision*2D` (Fsm.cs:2759-2865) of every TargetFSM that is Active
 * and still has the flag -- the path that raises the TRIGGER/COLLISION * 2D system events and runs the
 * actions' DoCollision*2D overrides.  TargetFSMs order is the GameObject's PlayMakerFSM order (go_def fsm
 * list); the Fsm path runs before PlayMakerUnity2DProxy on the same object. */
static void world_fsm_2d(fsm_world *w, int32_t go, bool trigger, int kind, int32_t other_go, int32_t other_layer)
{
    const go_def *d = w->gos[go].def;
    for (int32_t k = 0; k < d->n_fsms; k++) {
        fsm_inst *f = &w->fsms[w->sc->fsm_idx[d->fsm_start + k]];
        /* PlayMakerFSM.Active (PM PlayMakerFSM.cs:88 `public bool Active => fsm.Active;`) forwards to
         * Fsm.Active (PM HutongGames.PlayMaker/Fsm.cs:546-556): `owner != null && owner.gameObject != null
         * && !Finished && ActiveState != null`.  It does NOT read Behaviour.enabled, and PlayMakerFSM
         * declares no OnCollision*2D/OnTrigger*2D message of its own -- the only delivery path is these
         * proxy components, which PlayMakerFSM.OnDisable (:392-403) never removes.  So a disabled
         * PlayMakerFSM still receives these events while its state machine is active. */
        if (!fsm_is_active(f)) continue;                  /* Fsm.Active, Fsm.cs:546-556 */
        if (!((f->handle_2d >> ((trigger ? 3 : 0) + kind)) & 1u)) continue;
        if (trigger) fsm_on_trigger2d(f, kind, other_go);
        else fsm_on_collision2d(f, kind, other_go, other_layer);
    }
}
void world_dispatch_trigger(fsm_world *w, int32_t go, int kind, int32_t other_go, int32_t other_layer)
{
    world_fsm_2d(w, go, true, kind, other_go, other_layer);
    proxy_dispatch(w, go, 3 + kind, other_go, other_layer);   /* PlayMakerUnity2DProxy.OnTrigger*2D delegates */
}
void world_dispatch_collision(fsm_world *w, int32_t go, int kind, int32_t other_go, int32_t other_layer)
{
    world_fsm_2d(w, go, false, kind, other_go, other_layer);
    proxy_dispatch(w, go, kind, other_go, other_layer);       /* PlayMakerUnity2DProxy.OnCollision*2D delegates */
}
/* A Collider2D with no Rigidbody2D (a Unity static collider) that the core did not create: a runtime Instantiate's
 * copy has no dumped instanceID for world_bind_statics to match, so without this it is observed but never touches
 * anything.  Built as the core builds its statics (sim.c build_world): one static body per collider at the object's
 * pose and lossy scale, the shape in collider-local terms. */
static bool phys_owned_body(fsm_world *w, int32_t go);   /* below */
static void phys_create_statics(fsm_world *w, int32_t go)
{
    phys_world *pw = (phys_world *)w->phys;
    go_inst *g = &w->gos[go];
    for (int32_t k = 0; k < g->n_cols; k++) {
        col_inst *c = &g->cols[k];
        /* a dumped collider (instanceID set) is the core's, even when it built no shape for an unsupported one */
        if (c->shape || c->def->rb_go >= 0 || c->def->instance_id != -1) continue;
        float p[3], ls[3]; go_world_pos(w, go, p); go_lossy_scale(w, go, ls);
        phys_body_desc bd; memset(&bd, 0, sizeof bd);
        bd.type = PHYS_BODY_STATIC;
        bd.cd = PHYS_CD_DISCRETE;
        bd.position = (phys_v2){ p[0], p[1] };
        bd.rotation_deg = go_euler_z(w, go);
        bd.scale = (phys_v2){ ls[0], ls[1] };
        bd.simulated = true;
        bd.layer = (uint32_t)g->def->layer;
        bd.user = HKSIM_USER_FSM_BASE + (uint32_t)go;
        phys_shape_desc sd; memset(&sd, 0, sizeof sd);
        switch (c->def->type) {
        case COL_BOX: sd.type = PHYS_SHAPE_BOX; break;
        case COL_CIRCLE: sd.type = PHYS_SHAPE_CIRCLE; break;
        case COL_POLYGON: sd.type = PHYS_SHAPE_POLYGON; break;
        case COL_EDGE: sd.type = PHYS_SHAPE_EDGE; break;
        default: HKSIM_UNIMPLEMENTED("collider type %d on '%s' (capsule) has no phys shape", c->def->type, go_path(w, go));
        }
        if ((sd.type == PHYS_SHAPE_POLYGON || sd.type == PHYS_SHAPE_EDGE) && c->def->n_pts <= 0)
            HKSIM_UNIMPLEMENTED("polygon/edge collider on '%s' has no points in scene.json", go_path(w, go));
        sd.is_trigger = c->is_trigger != 0;
        sd.enabled = c->enabled && go_active_in_hierarchy(w, go);
        sd.offset = (phys_v2){ c->offset[0], c->offset[1] };
        sd.size = (phys_v2){ c->size[0], c->size[1] };
        sd.radius = c->radius;
        sd.edge_radius = c->def->edge_radius;
        sd.points = c->def->n_pts > 0 ? (const phys_v2 *)&w->sc->col_pts[c->def->pts_start * 2] : NULL;   /* local, offset not applied (phys.h) */
        sd.n_points = c->def->n_pts > 0 ? (uint32_t)c->def->n_pts : 0u;
        sd.user = HKSIM_USER_FSM_BASE + (uint32_t)go;
        sd.layer = PHYS_LAYER_INHERIT;
        c->body = phys_body_add(pw, &bd);
        c->shape = phys_shape_add(pw, c->body, &sd);
        g->has_static_body = 1;
    }
}
/* The native half of an object's activation: its Rigidbody2D and Collider2Ds join the physics world.  Called by
 * the lifecycle per object of the activated subtree, before the scripts' OnEnable (A-6). */
void world_go_phys_enable(fsm_world *w, int32_t go)
{
    go_inst *g = &w->gos[go];
    if (!g->body && g->def->rb >= 0) world_phys_ensure_body(w, go);   /* deferred pooled body: created on first spawn */
    if (w->phys && phys_owned_body(w, go)) phys_create_statics(w, go);
    if (w->phys && g->body) phys_body_set_simulated((phys_world *)w->phys, g->body, true);
    if (w->phys)
        for (int32_t k = 0; k < g->n_cols; k++) if (g->cols[k].shape) {
            phys_shape_set_enabled((phys_world *)w->phys, g->cols[k].shape, g->cols[k].enabled != 0);
        }
}
/* A Collider2D added at runtime (world_add_box_collider) joins the physics world as a serialized one does: on its
 * attached Rigidbody2D's body when that body exists (phys_add_shapes_of_body skips the shapes already made), else
 * as its own static body. */
static void phys_add_shapes_of_body(fsm_world *w, int32_t bg, bool skip_own);
void world_phys_add_collider(fsm_world *w, int32_t go, const col_inst *c)
{
    if (!w->phys) return;
    int32_t bg = c->def->rb_go;
    if (bg >= 0) { if (w->gos[bg].body) phys_add_shapes_of_body(w, bg, false); }   /* else built with the deferred body */
    else if (phys_owned_body(w, go)) phys_create_statics(w, go);
}
void world_go_phys_disable(fsm_world *w, int32_t go)
{
    go_inst *g = &w->gos[go];
    if (w->phys && g->body) phys_body_set_simulated((phys_world *)w->phys, g->body, false);
    if (w->phys)
        for (int32_t k = 0; k < g->n_cols; k++) if (g->cols[k].shape) {
            phys_shape_set_enabled((phys_world *)w->phys, g->cols[k].shape, false);
        }
    lc_physics_exit_on_disable(w);
}

/* ==================================================================================================================
 * sim/phys binding: the bodies and shapes the FSM world owns (the Boss Holder objects and the Needle: everything with a
 * Rigidbody2D in scene.json except the Knight, the pools and the HUD), and the physics callbacks back into the
 * FSMs.  Users: body HKSIM_USER_FSM_BASE + owner GO, shape HKSIM_USER_FSM_BASE + collider GO (child triggers keep
 * their own object so Trigger2dEvent / Fsm.OnTriggerEnter2D reach the right FSM). */

/* collider GO -> body GO frame, T_body^-1 · T_child as offset + axis scale + rotation (degrees): each level
 * applies its own local scale, then rotation, then translation (Unity TRS).
 * A level's ANISOTROPIC scale (|sx| != |sy|) over a rotation accumulated below it shears (S*R is no R'*S'), which
 * this form cannot hold, so it traps; a level's rotation over an anisotropic scale below it is a rotated rectangle
 * (GG_Radiance `Burst k` turning its `Radiant Beam` children).  A MIRROR is representable since R(t)*M == M*R(-t): keep it in `scale` and negate that level's angle
 * (GG_Ghost_Markoth's shield spins while he flips). */
static void chain_to_body_rot(fsm_world *w, int32_t cg, int32_t bg, float off[2], float scale[2], float *angle_deg)
{
    off[0] = off[1] = 0.0f; scale[0] = scale[1] = 1.0f; *angle_deg = 0.0f;
    for (int32_t g = cg; g != bg && g >= 0; g = w->gos[g].parent) {
        float lp[3], ls[3];
        go_local_pos(w, g, lp); go_local_scale(w, g, ls);
        float ez = go_local_euler_z(w, g);
        float sx = ls[0] * off[0], sy = ls[1] * off[1];
        if (fabsf(ls[0]) != fabsf(ls[1]) && fmodf(*angle_deg, 180.0f) != 0.0f)   /* a sign difference is a MIRROR */
            HKSIM_UNIMPLEMENTED("collider child '%s': non-uniform scale %g,%g over the %g deg rotation of the levels "
                                "below it (a shear)", go_path(w, g), ls[0], ls[1], *angle_deg);
        if (ez != 0.0f) {
            float a = ez * ((float)M_PI / 180.0f), c = m_cos(a), sn = m_sin(a);
            float rx = c * sx - sn * sy, ry = sn * sx + c * sy;
            sx = rx; sy = ry;
        }
        off[0] = lp[0] + sx; off[1] = lp[1] + sy;
        float mirror_below = scale[0] * scale[1];
        scale[0] *= ls[0]; scale[1] *= ls[1];
        *angle_deg += (mirror_below < 0.0f) ? -ez : ez;   /* through the mirrors accumulated BELOW this level */
    }
}
static bool phys_owned_body(fsm_world *w, int32_t go)
{
    const char *p = go_path(w, go);
    /* The hero module owns the Knight's OWN body.  Its descendants are ordinary Rigidbody2Ds
     * (Knight/Spells/Scr Heads and Scr Heads 2, dumps_all/GG_Gruz_Mother/scene.json). */
    if (go == w->knight_go) return false;
    if (strncmp(p, "_GameCameras/", 13) == 0) return false;                          /* HUD: no physics */
    /* `_GameManager/GlobalPool/...` DOES need bodies: projectiles spawn from it and get a velocity.
     * Their bodies are deferred until first activation (phys_body_is_deferred). */
    if (strncmp(p, "_GameManager/", 13) == 0 && strncmp(p, "_GameManager/GlobalPool/", 24) != 0) return false;
    return true;
}
/* Re-bake one GameObject's POLYGON shapes at its current transform chain.  Bind-time points use the
 * scale chain as it was at bind; `NailSlash.StartSlash` rescales the slashes by the charm factor. */
void world_rebake_polygon_shapes(fsm_world *w, int32_t go)
{
    if (!w->phys || go < 0) return;
    go_inst *g = &w->gos[go];
    int32_t bg = -1;
    for (int32_t k = 0; k < g->n_cols; k++) if (g->cols[k].def->rb_go >= 0) { bg = g->cols[k].def->rb_go; break; }
    if (bg < 0) return;
    for (int32_t k = 0; k < g->n_cols; k++) {
        col_inst *c = &g->cols[k];
        if (!c->shape || c->def->n_pts <= 0) continue;               /* boxes/circles carry no point list */
        float off[2], sc[2], ang; chain_to_body_rot(w, go, bg, off, sc, &ang);
        float ca = 1.0f, sa = 0.0f;
        if (ang != 0.0f) { float ar = ang * ((float)M_PI / 180.0f); ca = m_cos(ar); sa = m_sin(ar); }
        phys_v2 *pts = (phys_v2 *)malloc(sizeof(phys_v2) * (size_t)c->def->n_pts);
        if (!pts) return;
        for (int32_t i = 0; i < c->def->n_pts; i++) {
            const float *pp = &w->sc->col_pts[(c->def->pts_start + i) * 2];
            float lx = (pp[0] + c->offset[0]) * sc[0], ly = (pp[1] + c->offset[1]) * sc[1];
            pts[i] = (phys_v2){ off[0] + ca * lx - sa * ly, off[1] + sa * lx + ca * ly };
        }
        phys_shape_set_points((phys_world *)w->phys, c->shape, pts, (uint32_t)c->def->n_pts);
        free(pts);
    }
}

static void phys_add_shapes_of_body(fsm_world *w, int32_t bg, bool skip_own)
{
    phys_world *pw = (phys_world *)w->phys;
    go_inst *b = &w->gos[bg];
    for (int32_t x = 0; x < w->n_gos; x++) {
        go_inst *g = &w->gos[x];
        if (skip_own && x == bg) continue;
        for (int32_t k = 0; k < g->n_cols; k++) {
            col_inst *c = &g->cols[k];
            if (c->def->rb_go != bg || c->shape) continue;
            float off[2], sc[2], ang; chain_to_body_rot(w, x, bg, off, sc, &ang);
            float ca = 1.0f, sa = 0.0f;
            if (ang != 0.0f) { float ar = ang * ((float)M_PI / 180.0f); ca = m_cos(ar); sa = m_sin(ar); }
            phys_shape_desc d; memset(&d, 0, sizeof d);
            switch (c->def->type) {
            case COL_BOX: d.type = PHYS_SHAPE_BOX; break;
            case COL_CIRCLE: d.type = PHYS_SHAPE_CIRCLE; break;
            case COL_POLYGON: d.type = PHYS_SHAPE_POLYGON; break;
            case COL_EDGE: d.type = PHYS_SHAPE_EDGE; break;
            default: HKSIM_UNIMPLEMENTED("collider type %d on '%s' (capsule) has no phys shape", c->def->type, go_path(w, x));
            }
            d.is_trigger = c->is_trigger != 0;
            d.enabled = c->enabled && go_active_in_hierarchy(w, x);
            d.offset = (phys_v2){ off[0] + c->offset[0] * sc[0], off[1] + c->offset[1] * sc[1] };
            if (d.type == PHYS_SHAPE_CIRCLE && ang != 0.0f) {   /* the offset turns with the chain (False Knight Staff/Staff Head) */
                float lx = c->offset[0] * sc[0], ly = c->offset[1] * sc[1];
                d.offset = (phys_v2){ off[0] + ca * lx - sa * ly, off[1] + sa * lx + ca * ly };
            }
            d.size = (phys_v2){ c->size[0] * sc[0], c->size[1] * sc[1] };
            float rs = fabsf(sc[0]) > fabsf(sc[1]) ? fabsf(sc[0]) : fabsf(sc[1]);   /* CircleCollider2D: radius x max(|sx|,|sy|) */
            d.radius = c->radius * rs;
            d.edge_radius = c->def->edge_radius;
            phys_v2 *pts = NULL;
            if (c->def->n_pts > 0) {
                pts = malloc(sizeof(phys_v2) * (size_t)c->def->n_pts);
                for (int32_t i = 0; i < c->def->n_pts; i++) {
                    const float *p = &w->sc->col_pts[(c->def->pts_start + i) * 2];
                    float lx = (p[0] + c->offset[0]) * sc[0], ly = (p[1] + c->offset[1]) * sc[1];   /* PolygonCollider2D points are offset-relative */
                    pts[i] = (phys_v2){ off[0] + ca * lx - sa * ly, off[1] + sa * lx + ca * ly };
                }
                d.points = pts; d.n_points = (uint32_t)c->def->n_pts;
                d.offset = (phys_v2){ 0.0f, 0.0f };
            } else if (d.type == PHYS_SHAPE_POLYGON || d.type == PHYS_SHAPE_EDGE) {
                HKSIM_UNIMPLEMENTED("polygon/edge collider on '%s' has no points in scene.json", go_path(w, x));
            } else if (d.type == PHYS_SHAPE_BOX && ang != 0.0f) {   /* rotated child box -> its four corners as a polygon */
                pts = malloc(sizeof(phys_v2) * 4);
                float hx = c->size[0] * sc[0] * 0.5f, hy = c->size[1] * sc[1] * 0.5f, cx = c->offset[0] * sc[0], cy = c->offset[1] * sc[1];
                float corner[4][2] = { { cx - hx, cy - hy }, { cx + hx, cy - hy }, { cx + hx, cy + hy }, { cx - hx, cy + hy } };
                for (int i = 0; i < 4; i++) pts[i] = (phys_v2){ off[0] + ca * corner[i][0] - sa * corner[i][1], off[1] + sa * corner[i][0] + ca * corner[i][1] };
                d.type = PHYS_SHAPE_POLYGON; d.points = pts; d.n_points = 4; d.offset = (phys_v2){ 0.0f, 0.0f };
            }
            d.user = HKSIM_USER_FSM_BASE + (uint32_t)x;
            d.instance_id = c->def->instance_id;
            d.layer = (g->def->layer == b->def->layer) ? PHYS_LAYER_INHERIT : (uint32_t)g->def->layer;
            c->shape = phys_shape_add(pw, b->body, &d);
            free(pts);
        }
    }
}
/* Create the physics body for one GameObject that has a serialized Rigidbody2D (at bind, or at a
 * pooled object's first activation). */
static void phys_create_body(fsm_world *w, int32_t go)
{
    phys_world *pw = (phys_world *)w->phys;
    go_inst *g = &w->gos[go];
    const rb_def *rb = &w->sc->rbs[g->def->rb];
    float p[3], ls[3]; go_world_pos(w, go, p); go_lossy_scale(w, go, ls);
    phys_body_desc d; memset(&d, 0, sizeof d);
    d.type = rb->body_type == 0 ? PHYS_BODY_DYNAMIC : rb->body_type == 1 ? PHYS_BODY_KINEMATIC : PHYS_BODY_STATIC;   /* RigidbodyType2D Dynamic 0 Kinematic 1 Static 2 */
    d.cd = rb->cd_mode ? PHYS_CD_CONTINUOUS : PHYS_CD_DISCRETE;
    d.position = (phys_v2){ p[0], p[1] };
    d.rotation_deg = go_euler_z(w, go);
    d.scale = (phys_v2){ ls[0], ls[1] };
    d.velocity = (phys_v2){ rb->vel[0], rb->vel[1] };
    d.gravity_scale = rb->gravity_scale;
    d.mass = rb->mass;
    d.free_rotation = !rb->freeze_rotation;
    d.simulated = rb->simulated && go_active_in_hierarchy(w, go);
    d.layer = (uint32_t)g->def->layer;
    d.user = HKSIM_USER_FSM_BASE + (uint32_t)go;
    g->body = phys_body_add(pw, &d);
    phys_body_set_damping(pw, g->body, rb->drag, rb->angular_drag);
    world_xf_track(w, go);
    if (g->parent >= 0) {   /* body_follow_parent: the pose above was derived from this parent pose */
        float pp[3], ps[3]; go_world_pos(w, g->parent, pp); go_lossy_scale(w, g->parent, ps);
        g->fol_ppos[0] = pp[0]; g->fol_ppos[1] = pp[1]; g->fol_prot = go_euler_z(w, g->parent);
        g->fol_pscale[0] = ps[0]; g->fol_pscale[1] = ps[1]; g->fol_valid = 1;
    }
    phys_add_shapes_of_body(w, go, false);
}
/* A pooled prefab's body is created the first time the object is activated, so Q-pphys-13 only sees
 * objects that really enter the world (not e.g. an unused charm's prefab idle in the pool). */
static bool phys_body_is_deferred(fsm_world *w, int32_t go)
{
    return strncmp(go_path(w, go), "_GameManager/GlobalPool/", 24) == 0 && !go_active_in_hierarchy(w, go);
}
void world_phys_ensure_body(fsm_world *w, int32_t go)
{
    if (!w->phys) return;
    go_inst *g = &w->gos[go];
    if (g->def->rb < 0 || g->body || !phys_owned_body(w, go)) return;
    phys_create_body(w, go);
}
void world_phys_bind(fsm_world *w)
{
    if (!w->phys) return;
    for (int32_t go = 0; go < w->n_gos; go++) {
        go_inst *g = &w->gos[go];
        if (g->def->rb < 0 || g->body || !phys_owned_body(w, go)) continue;
        if (phys_body_is_deferred(w, go)) continue;
        phys_create_body(w, go);
    }
}
/* Attach the CORE's static scene colliders to the GameObjects that own them.
 * A Collider2D with no Rigidbody2D up its chain is a Unity STATIC collider (`col_def.rb_go` -1); the core
 * creates its shape with user handle HKSIM_USER_STATIC_BASE + i.  This records handle -> GameObject for
 * `world_go_of_user` (a static collider still has a real `collision.gameObject`, e.g. a DamageHero spike)
 * and mirrors the shape id onto the `col_inst` so `col_set_enabled` can reach it.  The shapes stay
 * core-owned; `shapes[i]` is 0 for a static the core skipped (inactive or disabled at SceneReady). */
void world_bind_statics(fsm_world *w, const int32_t *instance_ids, const uint32_t *bodies, const uint32_t *shapes, uint32_t n)
{
    free(w->static_go); w->static_go = NULL; w->n_static_go = 0;
    if (!instance_ids || n == 0) return;
    w->static_go = (int32_t *)malloc(sizeof(int32_t) * (size_t)n);
    if (!w->static_go) return;
    for (uint32_t i = 0; i < n; i++) w->static_go[i] = -1;
    w->n_static_go = n;
    for (int32_t go = 0; go < w->n_gos; go++) {
        go_inst *g = &w->gos[go];
        for (int32_t k = 0; k < g->n_cols; k++) {
            col_inst *c = &g->cols[k];
            if (c->def->rb_go >= 0 || c->shape) continue;   /* rigidbody-backed colliders are this module's own */
            for (uint32_t i = 0; i < n; i++) {
                if (instance_ids[i] != c->def->instance_id) continue;
                w->static_go[i] = go;
                c->shape = shapes ? shapes[i] : 0u;
                c->body  = bodies ? bodies[i] : 0u;
                if (c->body) g->has_static_body = 1;
                break;
            }
        }
    }
}
/* the hero's Rigidbody2D (created by the core): the Knight GO reads its pose from it, and the Knight's child colliders
 * (Attacks/Slash..., Clash Tink, HeroBox, ...) become shapes on it; the Knight's own box is the core's */
void world_bind_hero_body(fsm_world *w, uint32_t body)
{
    w->hero_body = body;
    if (w->knight_go < 0) return;
    w->gos[w->knight_go].body = body;
    world_xf_track(w, w->knight_go);
    if (w->phys && body) phys_add_shapes_of_body(w, w->knight_go, true);
    world_invalidate_body_transforms(w);
}
/* NailSlash.StartSlash / CancelAttack / FixedUpdate collider toggles forwarded by sim/hero (hero_phys_ops.slash_set_enabled) */
static const char *const SLASH_PATHS[5] = { "Knight/Attacks/Slash", "Knight/Attacks/AltSlash", "Knight/Attacks/UpSlash", "Knight/Attacks/DownSlash", "Knight/Attacks/WallSlash" };
void world_slash_set_enabled(fsm_world *w, int slash, bool poly_on, bool tink_on)
{
    if (slash < 0 || slash >= 5) return;
    int32_t g = world_go_find_path(w, SLASH_PATHS[slash]);
    if (g < 0) return;
    if (w->gos[g].n_cols > 0) col_set_enabled(w, &w->gos[g].cols[0], poly_on);
    int32_t tink = go_find_child(w, g, "Clash Tink");
    if (tink >= 0 && w->gos[tink].n_cols > 0) col_set_enabled(w, &w->gos[tink].cols[0], tink_on);
}
int32_t world_slash_go(fsm_world *w, int slash)
{
    if (slash < 0 || slash >= 5) return -1;
    return world_go_find_path(w, SLASH_PATHS[slash]);
}
int world_slash_index_of(fsm_world *w, int32_t go)
{
    const char *n = go_name(w, go);
    for (int i = 0; i < 5; i++) if (strcmp(strrchr(SLASH_PATHS[i], '/') + 1, n) == 0) return i;
    return -1;
}
static bool go_under(fsm_world *w, int32_t go, int32_t anc) { for (; go >= 0; go = w->gos[go].parent) if (go == anc) return true; return false; }
/* Nested Rigidbody2D follow.  A Rigidbody2D under a parent moves with the parent's world pose: the parent's change
 * marks this Transform's global position changed, and with physics.json#Physics2D.autoSyncTransforms = true the
 * next SyncTransforms sets the body to the Transform's world position, GetPosition's fold of its unchanged local
 * translation (native-transform_time.md §4).  Rotation needs no work: ensure_transform_clean already pushes the
 * world eulerZ. */
void body_follow_parent(fsm_world *w, int32_t go, bool invalidate)
{
    go_inst *g = &w->gos[go];
    if (!g->body || g->body == w->hero_body || g->parent < 0) return;
    for (int32_t a = g->parent; a >= 0; a = w->gos[a].parent)   /* an ancestor body follows ITS parent first */
        if (w->gos[a].body) { body_follow_parent(w, a, invalidate); break; }
    float pp[3], ps[3]; go_world_pos(w, g->parent, pp); go_lossy_scale(w, g->parent, ps);
    float pr = go_euler_z(w, g->parent);
    if (g->fol_valid && pp[0] == g->fol_ppos[0] && pp[1] == g->fol_ppos[1] && pr == g->fol_prot && ps[0] == g->fol_pscale[0] && ps[1] == g->fol_pscale[1]) return;
    bool carry = g->fol_valid;
    g->fol_ppos[0] = pp[0]; g->fol_ppos[1] = pp[1]; g->fol_prot = pr; g->fol_pscale[0] = ps[0]; g->fol_pscale[1] = ps[1]; g->fol_valid = 1;
    if (!carry) return;
    float wp[3]; go_transform_point(w, g->parent, g->local_pos, wp);
    phys_body_set_position((phys_world *)w->phys, g->body, (phys_v2){ wp[0], wp[1] });
    if (invalidate) invalidate_transform_dfs(w, go);
}
/* The body's own lossy scale, both axes: an ancestor's scale write, or the Knight's facing flip (owned
 * by sim/hero), changes it without any write to the body's own object. */
static void body_sync_scale(fsm_world *w, int32_t go)
{
    go_inst *g = &w->gos[go];
    if (!g->body || g->body == w->hero_body) return;
    float ls[3]; go_lossy_scale(w, go, ls);
    phys_body_set_scale((phys_world *)w->phys, g->body, (phys_v2){ ls[0], ls[1] });
}
/* Deferred shape flush, once per physics step just before phys_step (physics.json#Physics2D
 * autoSyncTransforms=true): transform writers only mark objects dirty, geometry is pushed here. */
void world_flush_dirty_shapes(fsm_world *w)
{
    if (!w->phys || !w->dirty_bits) return;
    /* Take the set and clear it, so anything marked DURING the drain waits for the next frame.  Word
     * then bit order visits GameObjects in ascending id order: the order the colliders are re-created in, and so
     * the order their new proxies take ids (PhysicsManager2D::SyncTransforms' own order is not in any source). */
    world_shapes_dirty_set(w, w->drain_bits);
    memset(w->dirty_bits, 0, sizeof(uint64_t) * (size_t)w->n_dirty_words);
    for (int32_t word = 0; word < w->n_dirty_words; word++) {
        uint64_t bits = w->drain_bits[word];
        while (bits) {
            int32_t go = (word << 6) + __builtin_ctzll(bits);
            bits &= bits - 1;
            if (!go_shapes_dirty(w, &w->gos[go])) continue;
            go_shapes_clean(w, &w->gos[go]);
            if (w->gos[go].body) { body_follow_parent(w, go, true); body_sync_scale(w, go); }
            /* A static collider's core-owned body pose is pushed by ensure_transform_clean;
             * autoSyncTransforms syncs every changed transform before the step, so resolve it here. */
            if (w->gos[go].has_static_body) ensure_transform_clean(w, go);
            phys_rescale_shapes(w, go);
        }
    }
    /* The step runs next.  Everything resolved so far cached a PRE-step world_pos for its bodies;
     * invalidate them so the first read after the step re-reads the body. */
    world_invalidate_body_transforms(w);
}
/* TinkEffect.OnTriggerEnter2D -- HK/TinkEffect.cs:45-117: the nail hitting a "tink" surface knocks the
 * Knight back.  Its serialized fields are the component's comp_def payload (gen_tables.py _tink_effect).
 * useNailPosition, blockEffect and its Random.Range pitch only place a cosmetic spawn (no collider, body
 * or FSM), so they are not ported. */
static const comp_def *tink_def_of(const fsm_world *w, int32_t go)
{
    const go_def *g = w->gos[go].def;
    for (int32_t k = 0; k < g->n_comps; k++) {
        const comp_def *c = &w->sc->comps[g->comp_start + k];
        if (strcmp(w_str(w, c->type), "TinkEffect") == 0) return c;
    }
    return NULL;
}
static void tink_effect_enter(fsm_world *w, int32_t go, int32_t other)
{
    if (other < 0) return;
    int32_t tg = go_tag(w, other);
    if (tg < 0 || strcmp(w_str(w, tg), "Nail Attack") != 0) return;                  /* :47 */
    go_inst *g = &w->gos[go];
    if (g->tink_ok_frame && w->live_frame_seq < g->tink_ok_frame) return;              /* :47 Time.time < nextTinkTime */
    /* :51 nextTinkTime = Time.time + 0.25: Time.time advances 0.02 per live frame (R2 fixedDeltaTime), so
     * 12 frames (0.24) are still blocked and the 13th (0.26) is not. */
    g->tink_ok_frame = w->live_frame_seq + 13u;
    const char *path = go_path(w, go);
    const comp_def *d = tink_def_of(w, go);
    if (!d) HKSIM_UNIMPLEMENTED("TinkEffect on '%s': no comp_def in the scene tables", path);
    const bool send_fsm_event = d->i[0] & 1, send_directional = d->i[0] & 2;
    /* :52-53 direction of the nail from its damages_enemy FSM (0 when there is none) */
    float degrees = 0.0f;
    int32_t df = world_fsm_find(w, other, "damages_enemy");
    if (df >= 0) { bool fresh; degrees = fsm_get_var(&w->fsms[df], VB_FLOAT, w_intern(w, "direction"), &fresh)->f; }
    {   /* :54-57 gameCam.cameraShakeFSM.SendEvent("EnemyKillShake"), addressed as hk_comp.c hm_invincible does */
        int32_t cs = world_fsm_find(w, w->camera_parent_go, "CameraShake");
        if (cs >= 0) fsm_event_name(&w->fsms[cs], "EnemyKillShake");
    }
    int cd = cardinal_direction(degrees);                                             /* :74 */
    switch (cd) {
    case 0: world_hero_recoil_left(w); break;                                         /* :79 */
    case 1: world_hero_recoil_down(w); break;                                         /* :87 */
    case 2: world_hero_recoil_right(w); break;                                        /* :96 */
    default: break;                                                                   /* :103-110: no recoil */
    }
    /* :112 blockEffect.Spawn(...).GetComponent<AudioSource>().pitch = Random.Range(0.85f, 1.15f): cosmetic, see above */
    if (send_directional) {      /* :78-108 fsm.SendEvent("TINK " + the opposite cardinal direction) */
        static const char *const TINK_DIR_EVENTS[4] = { "TINK RIGHT", "TINK UP", "TINK LEFT", "TINK DOWN" };
        int32_t tf = world_fsm_find(w, go, w_str(w, d->i[2]));
        if (tf < 0) HKSIM_UNIMPLEMENTED("TinkEffect on '%s': its directional fsm '%s' is not in the world", path, w_str(w, d->i[2]));
        fsm_event_name(&w->fsms[tf], TINK_DIR_EVENTS[cd]);
    }
    if (send_fsm_event) {                                                             /* :113-116 fsm.SendEvent(FSMEvent) */
        int32_t tf = world_fsm_find(w, go, w_str(w, d->i[2]));
        if (tf < 0) HKSIM_UNIMPLEMENTED("TinkEffect on '%s': its fsm '%s' is not in the world", path, w_str(w, d->i[2]));
        fsm_event_name(&w->fsms[tf], w_str(w, d->i[1]));
    }
}

/* physics callbacks -> PlayMakerUnity2DProxy / Fsm.OnTrigger*2D / OnCollision*2D on the shape's own object */
void world_phys_event(fsm_world *w, const void *phys_event_ptr)
{
    const phys_event *e = (const phys_event *)phys_event_ptr;
    if (!w->phys) return;
    world_reserve(w);                            /* each event is dispatched from the core, between stages' work */
    phys_world *pw = (phys_world *)w->phys;
    int32_t ga = world_go_of_user(w, phys_shape_user(pw, e->shape_a));
    if (ga < 0) return;
    int32_t gb = world_go_of_user(w, phys_shape_user(pw, e->shape_b));
    if (gb < 0) gb = world_go_of_user(w, phys_body_user(pw, e->body_b));
    int32_t other_layer = gb >= 0 ? go_layer(w, gb) : (int32_t)phys_shape_layer(pw, e->shape_b);
    /* GrimmballControl.OnTriggerEnter2D :74-80 -- `collision.gameObject.layer == 8` (Terrain, per the
     * generated LAYER_NAMES) ends the ball's flight and starts Shrink.  Either side of the pair can be
     * the ball; grimmball_do_hit ignores a GO that is not a live one and carries the `!hit` guard. */
    if (e->kind == PHYS_EV_TRIGGER_ENTER) {
        if (other_layer == 8) grimmball_do_hit(w, ga);
        if (gb >= 0 && go_layer(w, ga) == 8) grimmball_do_hit(w, gb);
    }
    if (w->hero && w->knight_go >= 0 && go_under(w, ga, w->knight_go) && (e->kind == PHYS_EV_TRIGGER_ENTER || e->kind == PHYS_EV_TRIGGER_STAY)) {
        if (go_has_component(w, ga, "NailSlash")) {                /* NailSlash.OnTriggerEnter2D / Stay2D (NS:129-133, 150-153) */
            int slash = world_slash_index_of(w, ga);
            /* NailSlash.cs:192 bounces when `NonBouncer == null || !NonBouncer.active`, so the flag
             * passed here is "has a NonBouncer AND it is currently active". */
            if (slash >= 0) knight_slash_trigger(w, slash, other_layer,
                                                 gb >= 0 && go_has_component(w, gb, "NonBouncer") && w->gos[gb].nonbouncer_active, 0, 0);
        }
        if (go_has_component(w, ga, "HeroBox") && !w->hero_box_inactive && gb >= 0) {   /* HeroBox.OnTriggerEnter2D/Stay2D -> CheckForDamage (HB:25-71) */
            float op[3]; go_world_pos(w, gb, op);
            int32_t dfsm = world_fsm_locate(w, gb, "damages_hero");   /* HB:43-45 ContainsFSM / LocateFSM */
            if (dfsm >= 0) {
                fsm_inst *df = &w->fsms[dfsm]; bool fresh;
                int32_t dmg = fsm_get_var(df, VB_INT, w_intern(w, "damageDealt"), &fresh)->i;
                int32_t haz = fsm_get_var(df, VB_INT, w_intern(w, "hazardType"), &fresh)->i;
                w->dmg_src_go = gb;   /* HERO_DAMAGE source name (docs/trace-format.md ev 2) */
                knight_box_check_for_damage(w, op[0], dmg, haz, 0, 1);
            } else if (w->gos[gb].dh >= 0) {
                dh_inst *dh = &w->dhs[w->gos[gb].dh];
                w->dmg_src_go = gb;
                knight_box_check_for_damage(w, op[0], dh->damage_dealt, dh->def->hazard_type, dh->def->shadow_dash_hazard, 0);
            }
        }
    }
    if (go_has_component(w, ga, "AlertRange")) alert_range_callback(w, ga, (int)e->kind);   /* AlertRange.OnTrigger*2D */
    if (e->kind == PHYS_EV_TRIGGER_ENTER && go_has_component(w, ga, "TinkEffect")) tink_effect_enter(w, ga, gb);   /* TinkEffect.OnTriggerEnter2D */
    if ((e->kind == PHYS_EV_TRIGGER_ENTER || e->kind == PHYS_EV_TRIGGER_EXIT) && go_has_component(w, ga, "EnviroRegion"))
        scr_enviro_region(w, ga, e->kind == PHYS_EV_TRIGGER_ENTER);   /* EnviroRegion.OnTriggerEnter2D / Exit2D */
    if ((e->kind == PHYS_EV_TRIGGER_ENTER || e->kind == PHYS_EV_TRIGGER_EXIT) && w->gos[ga].tt_present)
        tt_trigger_event(w, ga, gb, e->kind == PHYS_EV_TRIGGER_ENTER);   /* TrackTriggerObjects.OnTriggerEnter2D/Exit2D (act_w1.c) */
    /* ObjectBounce.OnCollisionEnter2D.  Unity calls an object's receivers in component order; this runs ahead of the
     * FSM collision proxies, which differs only on the knight's corpse pieces (Corpse Head, Corpse Nail Hero: both,
     * reached only after the knight's death, completeness.py _DEATH) */
    if (e->kind == PHYS_EV_COLLISION_ENTER && go_has_component(w, ga, "ObjectBounce"))
        scr_object_bounce_enter(w, ga, e->normal.x, e->normal.y, e->contact_count);
    /* Corpse.OnCollisionEnter2D / OnCollisionStay2D both call OnCollision (Corpse.cs:218-226) */
    if ((e->kind == PHYS_EV_COLLISION_ENTER || e->kind == PHYS_EV_COLLISION_STAY) && go_has_component(w, ga, "Corpse"))
        scr_corpse_land(w, ga);
    if (e->kind == PHYS_EV_COLLISION_ENTER && go_has_component(w, ga, "EnemyBullet"))   /* EnemyBullet.OnCollisionEnter2D :74-78 */
        scr_enemy_bullet_impact(w, ga, e->normal.x, e->normal.y, true);
    if (e->kind == PHYS_EV_TRIGGER_ENTER && gb >= 0 && go_has_component(w, ga, "EnemyBullet")) {   /* :82-86 */
        int32_t tg = go_tag(w, gb);
        if (tg >= 0 && strcmp(w_str(w, tg), "HeroBox") == 0) scr_enemy_bullet_impact(w, ga, 0.0f, 0.0f, false);
    }
    /* KillOnContact.OnCollisionEnter2D (KillOnContact.cs:5-24): the knight's branch is sim/core/sim.c's; a HealthManager
     * (Die) or GeoControl (Disable) branch is not ported */
    if (e->kind == PHYS_EV_COLLISION_ENTER && gb >= 0 && go_has_component(w, ga, "KillOnContact")
        && (hm_of_go(w, gb) || go_has_component(w, gb, "GeoControl")))
        HKSIM_UNIMPLEMENTED("KillOnContact on '%s' touched '%s': HealthManager.Die / GeoControl.Disable (KillOnContact.cs:7-18) "
                            "are not ported", go_path(w, ga), go_path(w, gb));
    if (e->kind == PHYS_EV_TRIGGER_STAY && go_has_component(w, ga, "SendEnemyMessageTrigger"))
        scr_enemy_message_stay(w, ga, gb);   /* SendEnemyMessageTrigger.OnTriggerStay2D (no Stay in a pair's Enter step, native-physics2d.md §6.3) */
    switch (e->kind) {
    case PHYS_EV_TRIGGER_ENTER:
        world_dispatch_trigger(w, ga, 0, gb, other_layer); break;
    case PHYS_EV_TRIGGER_STAY: world_dispatch_trigger(w, ga, 1, gb, other_layer); break;
    case PHYS_EV_TRIGGER_EXIT: world_dispatch_trigger(w, ga, 2, gb, other_layer); break;
    case PHYS_EV_COLLISION_ENTER: world_dispatch_collision(w, ga, 0, gb, other_layer); break;
    case PHYS_EV_COLLISION_STAY: world_dispatch_collision(w, ga, 1, gb, other_layer); break;
    case PHYS_EV_COLLISION_EXIT: world_dispatch_collision(w, ga, 2, gb, other_layer); break;
    }
}
