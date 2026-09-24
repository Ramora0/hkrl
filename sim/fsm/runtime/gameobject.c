/* GameObjects: lookup, hierarchy, tag / layer, Transform (2D TRS; z carried through), SetActive. */
#include "fsm/fsm.h"
#include "fsm/lifecycle.h"
#include "world_internal.h"
#include "core/phys.h"
#include "rcpps_zen3.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "core/alloc.h"

/* ---- GO lookup ---- */
/* A prefab asset (go_def.asset) is not in the scene: no lookup finds it. */
int32_t world_go_find_path(const fsm_world *w, const char *path)
{
    for (int32_t i = 0; i < w->n_gos; i++)
        if (!w->gos[i].destroyed && !w->gos[i].def->asset && strcmp(w_str(w, w->gos[i].def->path), path) == 0) return i;
    return -1;
}
/* GameObject.Find(name): Unity searches active objects by name or path; iteration order is engine
 * internal (Q-fsmact-1).  Ported as: exact path match first, else first active GO with that name in
 * dump order (ASSUMPTION, Q-pfsm-3). */
int32_t world_go_find_name(const fsm_world *w, const char *name)
{
    int32_t id = world_go_find_path(w, name);
    if (id >= 0 && go_active_in_hierarchy(w, id)) return id;
    for (int32_t i = 0; i < w->n_gos; i++)
        if (!w->gos[i].destroyed && go_active_in_hierarchy(w, i) && strcmp(w_str(w, w->gos[i].def->name), name) == 0) return i;
    return -1;
}
const char *go_name(const fsm_world *w, int32_t go) { return go < 0 ? "(null)" : w_str(w, w->gos[go].def->name); }
const char *go_path(const fsm_world *w, int32_t go) { return go < 0 ? "(null)" : w_str(w, w->gos[go].def->path); }
int32_t go_tag(const fsm_world *w, int32_t go)
{
    if (go < 0) return -1;
    /* ACT/SetTag.cs:24 writes gameObject.tag at runtime; tag_override carries it (-1 = never written).
     * STR is interned (one id per distinct literal), so an override id compares equal to a def->tag id. */
    return w->gos[go].tag_override >= 0 ? w->gos[go].tag_override : w->gos[go].def->tag;
}
int32_t go_layer(const fsm_world *w, int32_t go)
{
    if (go < 0) return 0;
    if (go == w->knight_go && w->phys && w->hero_body) return (int32_t)phys_body_layer((const phys_world *)w->phys, w->hero_body);   /* HeroController writes gameObject.layer */
    return w->gos[go].layer_override >= 0 ? w->gos[go].layer_override : w->gos[go].def->layer;
}
/* GameObject.layer = L (ACT/SetLayer.cs:29-35 is the one FSM writer).  Physics2D filters every pair by
 * the layers of the two colliders' OWN GameObjects (physics.json#layerCollisionMatrix), so each collider
 * of this object moves to row L; the colliders of OTHER objects on this object's Rigidbody2D keep theirs
 * (they inherited the body's layer only because it was equal at bind -- pin them first).  Existing
 * contacts are re-filtered at the next Collide (b2Fixture::Refilter via phys_shape_set_layer /
 * phys_body_set_layer). */
void go_set_layer(fsm_world *w, int32_t go, int32_t layer)
{
    if (go < 0) return;
    HKSIM_ASSERT(layer >= 0 && layer < 32, "GameObject.layer = %d on '%s'", layer, go_path(w, go));
    if (go == w->knight_go) HKSIM_UNIMPLEMENTED("GameObject.layer write on the Knight from an FSM: sim/hero owns the Knight's layer");
    go_inst *g = &w->gos[go];
    if (go_layer(w, go) == layer) return;
    phys_world *pw = (phys_world *)w->phys;
    if (pw && g->body) {
        for (int32_t x = 0; x < w->n_gos; x++) {           /* pin other objects' shapes on this body */
            if (x == go) continue;
            for (int32_t k = 0; k < w->gos[x].n_cols; k++) {
                col_inst *c = &w->gos[x].cols[k];
                if (c->shape && c->def->rb_go == go) phys_shape_set_layer(pw, c->shape, (uint32_t)go_layer(w, x));
            }
        }
        phys_body_set_layer(pw, g->body, (uint32_t)layer);
    }
    g->layer_override = (int16_t)layer;
    if (pw)
        for (int32_t k = 0; k < g->n_cols; k++) {
            col_inst *c = &g->cols[k];
            if (!c->shape) continue;
            if (c->def->rb_go < 0) { if (c->body) phys_body_set_layer(pw, c->body, (uint32_t)layer); }   /* core static: shape inherits its own body */
            else phys_shape_set_layer(pw, c->shape, (uint32_t)layer);
        }
}
int32_t go_parent(const fsm_world *w, int32_t go) { return go < 0 ? -1 : w->gos[go].parent; }

bool is_under(const fsm_world *w, int32_t go, int32_t root)
{
    while (go >= 0) { if (go == root) return true; go = w->gos[go].parent; }
    return false;
}

void update_active_in_hierarchy_dfs(fsm_world *w, int32_t go, bool parent_active)
{
    if (go < 0) return;
    go_inst *g = &w->gos[go];
    g->active_in_hierarchy = (parent_active && g->active_self && !g->destroyed) ? 1 : 0;
    for (int32_t c = g->first_child; c >= 0; c = w->gos[c].next_sibling) {
        update_active_in_hierarchy_dfs(w, c, g->active_in_hierarchy != 0);
    }
}

bool go_active_in_hierarchy(const fsm_world *w, int32_t go)
{
    return go >= 0 && w->gos[go].active_in_hierarchy;
}

/* Transform.Find(name) — supports "A/B" paths (Unity semantics; children searched in sibling order) */
int32_t go_find_child(const fsm_world *w, int32_t go, const char *name)
{
    if (go < 0 || !name) return -1;
    char buf[256];
    strncpy(buf, name, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    char *seg = buf;
    while (seg && *seg) {
        char *slash = strchr(seg, '/');
        if (slash) *slash = 0;
        int32_t found = -1;
        for (int32_t c = w->gos[go].first_child; c >= 0; c = w->gos[c].next_sibling) {
            if (!w->gos[c].destroyed && strcmp(w_str(w, w->gos[c].def->name), seg) == 0) { found = c; break; }
        }
        if (found < 0) return -1;
        go = found;
        seg = slash ? slash + 1 : NULL;
    }
    return go;
}

/* FSM lookup on a GO: component order = fsm_idx order (scene.json component order where dumped) */
int32_t world_fsm_find(const fsm_world *w, int32_t go, const char *fsm_name)
{
    if (go < 0) return -1;
    const go_def *g = w->gos[go].def;
    for (int32_t k = 0; k < g->n_fsms; k++) {
        int32_t fi = w->sc->fsm_idx[g->fsm_start + k];
        if (strcmp(w_str(w, w->fsms[fi].def->fsm_name), fsm_name) == 0) return fi;
    }
    return -1;
}
/* FSMUtility.LocateFSM / ContainsFSM (FSMUtility.cs): the first PlayMakerFSM on the GameObject with that name, in
 * component order, whether or not the sim ticks it.  A caller that reads a variable of one outside the live set traps. */
int32_t world_fsm_locate(const fsm_world *w, int32_t go, const char *fsm_name)
{
    if (go < 0) return -1;
    const go_def *g = w->gos[go].def;
    for (int32_t k = 0; k < g->n_fsms; k++) {
        int32_t fi = w->sc->fsm_idx[g->fsm_start + k];
        if (strcmp(w_str(w, w->fsms[fi].def->fsm_name), fsm_name) == 0) return fi;
    }
    return -1;
}
int32_t world_fsm_first(const fsm_world *w, int32_t go)
{
    if (go < 0) return -1;
    const go_def *g = w->gos[go].def;
    return g->n_fsms > 0 ? w->sc->fsm_idx[g->fsm_start] : -1;
}

/* ---- transforms (2D TRS; z carried through untouched) ---- */
static void go_require_pose(const fsm_world *w, int32_t go)
{
    HKSIM_ASSERT(go >= 0, "transform read on a null GameObject");
    if (!w->gos[go].has_transform)
        HKSIM_UNIMPLEMENTED("transform of '%s' is not in any dump (no collider row in scene.json)", go_path(w, go));
}

void invalidate_transform_dfs(fsm_world *w, int32_t go)
{
    if (go < 0) return;
    go_inst *g = &w->gos[go];
    g->transform_dirty = 1;
    if (!g->shapes_dirty) {   /* cleared only by world_flush_dirty_shapes, unlike transform_dirty
                               * which any intervening read clears before the flush can see it. */
        g->shapes_dirty = 1;
        if (!w->dirty_bits) {
            w->n_dirty_words = (w->n_gos + 63) >> 6;
            w->dirty_bits = calloc((size_t)w->n_dirty_words, sizeof(uint64_t));
            w->drain_bits = calloc((size_t)w->n_dirty_words, sizeof(uint64_t));
            HKSIM_ASSERT(w->dirty_bits && w->drain_bits, "out of memory marking a dirty shape");
        }
        w->dirty_bits[go >> 6] |= (uint64_t)1 << (go & 63);
    }
    for (int32_t c = g->first_child; c >= 0; c = w->gos[c].next_sibling) {
        invalidate_transform_dfs(w, c);
    }
}


/* Remember a GameObject that has acquired a body or a rigidbody, so the invalidate walk below has a
 * list to iterate instead of all n_gos to filter. */
void world_xf_track(fsm_world *w, int32_t go)
{
    if (go < 0 || w->gos[go].xf_tracked) return;
    if (w->n_xf_gos == w->cap_xf_gos) {
        w->cap_xf_gos = w->cap_xf_gos ? w->cap_xf_gos * 2 : 128;
        w->xf_gos = realloc(w->xf_gos, sizeof(int32_t) * (size_t)w->cap_xf_gos);
        HKSIM_ASSERT(w->xf_gos != NULL, "out of memory tracking body transforms");
    }
    w->gos[go].xf_tracked = 1;
    w->xf_gos[w->n_xf_gos++] = go;
}

/* Signed float sign, 0 treated as +1 (degenerate zero-scale objects report no rotation flip either way). */
static float fsign1(float v) { return v < 0.0f ? -1.0f : 1.0f; }

static void body_writeback(fsm_world *w, int32_t go);
/* A kinematic body under an awake body is not written back: Simulate syncs it from its Transform instead
 * (native-physics2d.md §3.2; every sim body stays awake, Q-pphys-8). */
static bool kinematic_under_body(const fsm_world *w, int32_t go)
{
    if (!w->gos[go].kinematic) return false;
    for (int32_t a = w->gos[go].parent; a >= 0; a = w->gos[a].parent) if (w->gos[a].body) return true;
    return false;
}
/* PhysicsManager2D::Simulate's write-back (body_writeback) for the bodies the last step moved or turned: root body
 * positions are read from the bodies on demand; nested body positions and all rotations are handed to the
 * Transforms here. */
void world_invalidate_body_transforms(fsm_world *w)
{
    bool due = false;
    if (w->phys) {
        phys_world *pw = (phys_world *)w->phys;
        for (int32_t k = 0; k < w->n_xf_gos; k++) {
            go_inst *g = &w->gos[w->xf_gos[k]];
            if (!g->body) continue;
            if (phys_body_take_rotation(pw, g->body, &g->rot_pending_deg)) { g->rot_pending = 1; due = true; }
            if (phys_body_take_writeback(pw, g->body) && g->parent >= 0 && g->body != w->hero_body && !kinematic_under_body(w, w->xf_gos[k])) {
                g->pos_pending = 1; due = true;
            }
        }
    }
    for (int32_t k = 0; k < w->n_xf_gos; k++) invalidate_transform_dfs(w, w->xf_gos[k]);
    if (due)
        for (int32_t k = 0; k < w->n_xf_gos; k++) {
            const go_inst *g = &w->gos[w->xf_gos[k]];
            if (g->rot_pending || g->pos_pending) body_writeback(w, w->xf_gos[k]);
        }
}

/* ---- the native Transform maths (UnityPlayer.dll 2020.2.2f1, analysis/native_specs/native-transform_time.md).
 * A quaternion is (x, y, z, w).  Each expression keeps the native operand order and association; the build does
 * not contract or re-associate (CMakeLists.txt HKSIM_FP_FLAGS). ---- */
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, sizeof u); return u; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, sizeof f); return f; }
#define DEG2RAD_F 0.017453292f   /* Mathf.Deg2Rad 0x3C8EFA35 (Mathf.cs:171) */
#define RAD2DEG_F 57.29578f      /* Mathf.Rad2Deg 0x42652EE1 (Mathf.cs:174) */

/* The Transform's local scale.  The hero module owns the Knight's x flip (HC:1535-1549 write transform.localScale). */
static void local_s(const fsm_world *w, const go_inst *g, float s[3])
{
    s[0] = g->local_scale[0]; s[1] = g->local_scale[1]; s[2] = g->local_scale[2];
    if (g->body && g->body == w->hero_body && w->phys) s[0] = phys_body_scale_x((const phys_world *)w->phys, g->body);
}
/* The Transform's local translation.  phys owns a root body's position: the write-back makes it the root's
 * translation after every step, and a translation write moves the body. */
static void local_t(const fsm_world *w, const go_inst *g, float t[3])
{
    t[0] = g->local_pos[0]; t[1] = g->local_pos[1]; t[2] = g->local_pos[2];
    if (g->parent < 0 && g->body && w->phys) {
        phys_v2 p = phys_body_position((const phys_world *)w->phys, g->body);
        t[0] = p.x; t[1] = p.y;
    }
}
/* localEulerAngles = (0, 0, z): Quaternion.Euler (Quaternion.cs:193, z * Deg2Rad in float), then EulerToQuaternion
 * UP!0x180732910, whose ZXY product with ex = ey = 0 is (+0, +0, sinf(h), cosf(h)), h = fl(e.z * 0.5f) (§3.5), then
 * SetLocalR UP!0x1800c5a30's normalize (§3.3): lanes x, z divide by sqrt((w²+z²)+(y²+x²)), lanes y, w by
 * sqrt((x²+w²)+(z²+y²)), identity unless 1e-30 < sum.  m_sin/m_cos stand in for the UCRT sinf/cosf (§7). */
static const float *local_q(const go_inst *gc)
{
    go_inst *g = (go_inst *)gc;
    if (g->local_q_valid && f2u(g->local_q_src) == f2u(g->local_euler_z)) return g->local_q;
    float h = (g->local_euler_z * DEG2RAD_F) * 0.5f;
    float x = 0.0f, y = 0.0f, z = m_sin(h), q_w = m_cos(h);
    float xx = x * x, yy = y * y, zz = z * z, ww = q_w * q_w;
    float s0 = (ww + zz) + (yy + xx), s1 = (xx + ww) + (zz + yy);
    bool m0 = 1e-30f < s0, m1 = 1e-30f < s1;
    float n0 = sqrtf(s0), n1 = sqrtf(s1);
    g->local_q[0] = m0 ? x / n0 : 0.0f;
    g->local_q[1] = m1 ? y / n1 : 0.0f;
    g->local_q[2] = m0 ? z / n0 : 0.0f;
    g->local_q[3] = m1 ? q_w / n1 : 1.0f;
    g->local_q_src = g->local_euler_z; g->local_q_valid = 1;
    return g->local_q;
}
/* math::quatMulVec in the lane shape of Transform::GetPosition's loop (UP!0x1807e6720, 0x1807e67e0-0x1807e6888,
 * §2.1), shared by InverseTransformPosition (UP!0x18016cfa0) with the conjugate. */
static void quat_mul_vec(const float q[4], const float v[3], float out[3])
{
    float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    float A0 = (qy * -2.0f) * qy - (qz * 2.0f) * qz,  B0 = (qz * -2.0f) * qw - (qx * -2.0f) * qy,  C0 = (qx * 2.0f) * qz - (qy * -2.0f) * qw;
    float A1 = (qy * 2.0f) * qx - (qz * -2.0f) * qw,  B1 = (qz * -2.0f) * qz - (qx * 2.0f) * qx,   C1 = (qx * -2.0f) * qw - (qy * -2.0f) * qz;
    float A2 = (qy * -2.0f) * qw - (qz * -2.0f) * qx, B2 = (qz * 2.0f) * qy - (qx * -2.0f) * qw,   C2 = (qx * -2.0f) * qx - (qy * 2.0f) * qy;
    out[0] = (A0 * v[0] + v[0]) + (B0 * v[1] + C0 * v[2]);
    out[1] = (A1 * v[0] + v[1]) + (B1 * v[1] + C1 * v[2]);
    out[2] = (A2 * v[0] + v[2]) + (B2 * v[1] + C2 * v[2]);
}
/* Transform::TransformPoint (UP!0x1807ebbb0, §2.2) of `a`, a < 0 = world space: at `a` and then at each ancestor,
 * p = t + quatMulVec(q, s * p) per lane.  Transform::GetPosition (UP!0x1807e6720, §2.1) is this fold of the
 * object's own translation from its parent up: the world position is folded leaf to root. */
static void transform_point(const fsm_world *w, int32_t a, const float p[3], float out[3])
{
    float x[3] = { p[0], p[1], p[2] };
    for (; a >= 0; a = w->gos[a].parent) {
        const go_inst *g = &w->gos[a];
        float t[3], s[3], u[3], r[3];
        local_t(w, g, t); local_s(w, g, s);
        u[0] = s[0] * x[0]; u[1] = s[1] * x[1]; u[2] = s[2] * x[2];
        quat_mul_vec(local_q(g), u, r);
        x[0] = r[0] + t[0]; x[1] = r[1] + t[1]; x[2] = r[2] + t[2];
    }
    out[0] = x[0]; out[1] = x[1]; out[2] = x[2];
}
void go_transform_point(fsm_world *w, int32_t a, const float p[3], float out[3]) { transform_point(w, a, p, out); }
/* rcpps on the oracle machine (Zen3, rcpps_zen3.h), for the normal range; a zero or denormal gives +-inf, an
 * infinity or a result below the normal range +-0. */
static float rcpps_f(float s)
{
    uint32_t u = f2u(s), sign = u & 0x80000000u, e = (u >> 23) & 0xffu;
    if (e == 0xffu) return (u & 0x7fffffu) ? s : u2f(sign);
    if (e == 0u) return u2f(sign | 0x7f800000u);
    if (e >= 253u) return u2f(sign);
    return u2f(sign | (rcpps_zen3[(u >> 11) & 0xfffu] - ((e - 127u) << 23)));
}
/* The reciprocal of InverseTransformPosition (UP!0x18016cfa0, §3.2): rcpps, two Newton steps (the first with
 * 2.0000005f = 0x40000002), 0 where |s| < 1e-9. */
static float inv_scale(float s)
{
    float r0 = rcpps_f(s);
    float r1 = (2.0000005f - r0 * s) * r0;
    float r2 = r1 * (2.0f - r1 * s);
    return fabsf(s) < 1e-9f ? 0.0f : r2;
}
/* InverseTransformPosition (UP!0x18016cfa0 [TransformHierarchy.h:590-600], §3.2): world -> the local space of `a`
 * (a < 0 = world), root first: at each level p = quatMulVec(conj q, p - t) * inv(s). */
static void inverse_transform_position(const fsm_world *w, int32_t a, float p[3])
{
    if (a < 0) return;
    inverse_transform_position(w, w->gos[a].parent, p);
    const go_inst *g = &w->gos[a];
    float t[3], s[3], d[3], v[3];
    local_t(w, g, t); local_s(w, g, s);
    const float *q = local_q(g);
    float cq[4] = { -q[0], -q[1], -q[2], q[3] };
    d[0] = p[0] - t[0]; d[1] = p[1] - t[1]; d[2] = p[2] - t[2];
    quat_mul_vec(cq, d, v);
    p[0] = v[0] * inv_scale(s[0]); p[1] = v[1] * inv_scale(s[1]); p[2] = v[2] * inv_scale(s[2]);
}
/* CalculateGlobalRotation (UP!0x1800d1ab0 [TransformHierarchy.h:368-389], §2.4): r = own q; at each ancestor the
 * lanes flip with the sign bits of its scale (x by sy*sz, y by sx*sz, z by sx*sy), then r = P (x) r. */
static void global_rotation(const fsm_world *w, int32_t go, float r[4])
{
    const float *q0 = local_q(&w->gos[go]);
    r[0] = q0[0]; r[1] = q0[1]; r[2] = q0[2]; r[3] = q0[3];
    for (int32_t a = w->gos[go].parent; a >= 0; a = w->gos[a].parent) {
        const go_inst *g = &w->gos[a];
        float s[3]; local_s(w, g, s);
        const float *P = local_q(g);
        float P0 = P[0], P1 = P[1], P2 = P[2], P3 = P[3];
        float gx = u2f((f2u(s[0]) & 0x80000000u) ^ 0x3f800000u), gy = u2f((f2u(s[1]) & 0x80000000u) ^ 0x3f800000u),
              gz = u2f((f2u(s[2]) & 0x80000000u) ^ 0x3f800000u);
        float R3 = r[3];
        float R0 = u2f((f2u(gy * gz) & 0x80000000u) ^ f2u(r[0]));
        float R1 = u2f((f2u(gx * gz) & 0x80000000u) ^ f2u(r[1]));
        float R2 = u2f((f2u(gx * gy) & 0x80000000u) ^ f2u(r[2]));
        r[0] = -(((P2 * R1 - P1 * R2) - R0 * P3) - P0 * R3);
        r[1] = -(((P0 * R2 - P2 * R0) - R1 * P3) - P1 * R3);
        r[2] = -(((P1 * R0 - P3 * R2) - R3 * P2) - P0 * R1);
        r[3] = ((P3 * R3 - P0 * R0) - R2 * P2) - P1 * R1;
    }
}
/* Transform.eulerAngles.z of a rotation (§2.5): NormalizeSafe (UP!0x1800ba910: identity below 1e-05, else divide),
 * QuaternionToEuler kOrderUnityDefault (UP!0x180735e90: z = atan2f(2(wz+xy), ((y²-z²)-x²)+w²)), * Rad2Deg, then
 * Quaternion.Internal_MakePositive (Quaternion.cs:164-183).  m_atan2 stands in for the UCRT atan2f (§7). */
static float quat_euler_z(const float q[4])
{
    float x = q[0], y = q[1], z = q[2], qw = q[3];
    float n = sqrtf(((x * x + y * y) + z * z) + qw * qw);
    if (n < 1e-05f) { x = 0.0f; y = 0.0f; z = 0.0f; qw = 1.0f; }
    else { x = x / n; y = y / n; z = z / n; qw = qw / n; }
    float t = qw * z + x * y;
    float zd = m_atan2(t + t, ((y * y - z * z) - x * x) + qw * qw) * RAD2DEG_F;
    const float neg_flip = -0.0001f * RAD2DEG_F, pos_flip = 360.0f + neg_flip;
    if (zd < neg_flip) zd = zd + 360.0f;
    else if (zd > pos_flip) zd = zd - 360.0f;
    return zd;
}
/* The rotation matrix columns of q (CalculateGlobalRS UP!0x1800d17b0's first block; c[col][row]). */
static void quat_cols(const float q[4], float c[3][3])
{
    float x = q[0], y = q[1], z = q[2], qw = q[3];
    c[0][0] = ((y * -2.0f) * y + (z * -2.0f) * z) + 1.0f;
    c[0][1] = ((y * 2.0f) * x + (z * 2.0f) * qw) + 0.0f;
    c[0][2] = ((y * -2.0f) * qw + (z * 2.0f) * x) + 0.0f;
    c[1][0] = ((z * -2.0f) * qw + (x * 2.0f) * y) + 0.0f;
    c[1][1] = ((z * -2.0f) * z + (x * -2.0f) * x) + 1.0f;
    c[1][2] = ((z * 2.0f) * y + (x * 2.0f) * qw) + 0.0f;
    c[2][0] = ((x * 2.0f) * z + (y * 2.0f) * qw) + 0.0f;
    c[2][1] = ((x * -2.0f) * qw + (y * 2.0f) * z) + 0.0f;
    c[2][2] = ((x * -2.0f) * x + (y * -2.0f) * y) + 1.0f;
}
/* Transform::GetWorldScaleLossy (UP!0x1807e6ce0, §2.6): the diagonal of SM = R(conj q_world) * RS_world, where
 * CalculateGlobalRS (UP!0x1800d17b0) folds RS (R(q) scaled per column by s) leaf to root as M = P * M, and
 * CalculateGlobalSM (UP!0x1800d1c00) multiplies; both products associate a + (b + c) (the tracer, §2.6). */
static void lossy_scale(const fsm_world *w, int32_t go, const float q_world[4], float out[3])
{
    float m[3][3], c[3][3], s[3];
    const go_inst *g0 = &w->gos[go];
    quat_cols(local_q(g0), c); local_s(w, g0, s);
    for (int k = 0; k < 3; k++) for (int r = 0; r < 3; r++) m[k][r] = s[k] * c[k][r];
    for (int32_t a = g0->parent; a >= 0; a = w->gos[a].parent) {
        const go_inst *g = &w->gos[a];
        float P[3][3], n[3][3];
        quat_cols(local_q(g), c); local_s(w, g, s);
        for (int k = 0; k < 3; k++) for (int r = 0; r < 3; r++) P[k][r] = s[k] * c[k][r];
        for (int k = 0; k < 3; k++) for (int r = 0; r < 3; r++) n[k][r] = P[0][r] * m[k][0] + (P[1][r] * m[k][1] + P[2][r] * m[k][2]);
        memcpy(m, n, sizeof m);
    }
    float cq[4] = { -q_world[0], -q_world[1], -q_world[2], q_world[3] };
    quat_cols(cq, c);
    for (int k = 0; k < 3; k++) out[k] = c[0][k] * m[k][0] + (c[1][k] * m[k][1] + c[2][k] * m[k][2]);
}

/* The parent pose a nested body was last synchronised against (body_follow_parent). */
static void fol_anchor(fsm_world *w, int32_t go)
{
    go_inst *g = &w->gos[go];
    float pp[3], ps[3];
    go_world_pos(w, g->parent, pp); go_lossy_scale(w, g->parent, ps);
    g->fol_ppos[0] = pp[0]; g->fol_ppos[1] = pp[1]; g->fol_prot = go_euler_z(w, g->parent);
    g->fol_pscale[0] = ps[0]; g->fol_pscale[1] = ps[1]; g->fol_valid = 1;
}

void ensure_transform_clean(fsm_world *w, int32_t go)
{
    go_require_pose(w, go);
    go_inst *g = &w->gos[go];
    if (!g->transform_dirty) return;
    if (g->parent >= 0) ensure_transform_clean(w, g->parent);
    /* A nested body whose parent moved is carried along BEFORE its pose is used (body_follow_parent), on the read
     * and not only at the pre-step flush: in the game a child never lags its parent by a step
     * (Knight/Spells/Scr Heads 2 in analysis/polbat_hornet/ph_ep02.a.hktrace frames 22541-22547). */
    if (g->body && g->body != w->hero_body && g->parent >= 0 && w->phys) body_follow_parent(w, go, false);
    float t[3], r[4];
    local_t(w, g, t);
    transform_point(w, g->parent, t, g->world_pos);
    global_rotation(w, go, r);
    g->world_euler_z = quat_euler_z(r);
    lossy_scale(w, go, r, g->lossy_scale);
    /* phys stores the body's WORLD rotation, and this is the only place world_euler_z is derived, so
     * pushing from here covers every writer and every parent-driven rotation. */
    if (g->body && w->phys) {
        if (g->rot_from_body) phys_body_note_rotation((phys_world *)w->phys, g->body, g->world_euler_z);
        else phys_body_set_rotation((phys_world *)w->phys, g->body, g->world_euler_z);
        g->rot_from_body = 0;
    }
    /* Same push for a collider with NO Rigidbody2D (a core-owned static body): Unity moves a static
     * collider with its transform (e.g. GG_Grimm_Nightmare's pooled hazards).  Guarded by a per-GO flag
     * from world_bind_statics: this is on every transform resolve. */
    if (g->has_static_body && w->phys) {
        phys_world *pw = (phys_world *)w->phys;
        for (int32_t k = 0; k < g->n_cols; k++) {
            uint32_t b = g->cols[k].body;
            if (!b) continue;
            phys_body_set_position(pw, b, (phys_v2){ g->world_pos[0], g->world_pos[1] });
            phys_body_set_rotation(pw, b, g->world_euler_z);
            phys_body_set_scale_x(pw, b, g->lossy_scale[0]);
        }
    }
    g->transform_dirty = 0;
}

/* PhysicsManager2D::Simulate (UP!0x180beb390, native-physics2d.md §3.2) walks the bodies root to leaf and writes each
 * simulated awake body's pose to its Transform with SetPositionAndRotation (UP!0x1807eac80, §3.6): the position
 * (body.p.x, body.p.y, the Transform's own z) through InverseTransformPositionAndRotation (UP!0x1800d35e0, §3.2's
 * position lanes), and a turned body's angle as the world z rotation.  CheckAndClearChangedForMultipleSystems
 * follows, so the write does not re-sync the body, which keeps its own position and unwrapped angle.  A pending
 * ancestor goes first, so a child's local pose is taken against its parent's new pose. */
static void body_writeback(fsm_world *w, int32_t go)
{
    go_inst *g = &w->gos[go];
    bool rot = g->rot_pending, pos = g->pos_pending;
    g->rot_pending = 0; g->pos_pending = 0;
    for (int32_t a = g->parent; a >= 0; a = w->gos[a].parent)
        if (w->gos[a].rot_pending || w->gos[a].pos_pending) { body_writeback(w, a); break; }
    if (pos) {
        float p[3];
        transform_point(w, g->parent, g->local_pos, p);
        phys_v2 b = phys_body_position((const phys_world *)w->phys, g->body);
        p[0] = b.x; p[1] = b.y;
        inverse_transform_position(w, g->parent, p);
        memcpy(g->local_pos, p, sizeof p);
        fol_anchor(w, go);
    }
    if (rot) {
        float pz = 0.0f, hs = 1.0f;
        if (g->parent >= 0) {
            pz = go_euler_z(w, g->parent);
            float ps[3]; go_lossy_scale(w, g->parent, ps);
            hs = fsign1(ps[0] * ps[1]);   /* invert world = pz + hs*local, as go_set_euler_z */
        }
        g->local_euler_z = (g->rot_pending_deg - pz) * hs;
        g->rot_from_body = 1;
    }
    invalidate_transform_dfs(w, go);
    ensure_transform_clean(w, go);
}

void go_world_pos(fsm_world *w, int32_t go, float out[3])
{
    ensure_transform_clean(w, go);
    out[0] = w->gos[go].world_pos[0];
    out[1] = w->gos[go].world_pos[1];
    out[2] = w->gos[go].world_pos[2];
}

void go_world_to_local_point(fsm_world *w, int32_t parent, const float p[3], float out[3])
{
    out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
    inverse_transform_position(w, parent, out);
}

/* The translation write both setters end in (SetLocalT UP!0x1800c5c60, §3.1).  With autoSyncTransforms the next
 * SyncTransforms moves the body to the Transform's world position, GetPosition's fold of t (§4). */
static void set_local_t(fsm_world *w, int32_t go, const float t[3])
{
    go_inst *g = &w->gos[go];
    memcpy(g->local_pos, t, sizeof g->local_pos);
    if (w->phys && g->body) {
        float wp[3];
        transform_point(w, g->parent, t, wp);
        phys_body_set_position((phys_world *)w->phys, g->body, (phys_v2){ wp[0], wp[1] });
        if (g->parent >= 0) fol_anchor(w, go);
    }
    invalidate_transform_dfs(w, go);
}

/* Transform::SetPosition (UP!0x1807eac00) -> SetGlobalT (UP!0x1807e9440): the world point goes into the parent's
 * space through InverseTransformPosition (§3.2). */
void go_set_world_pos(fsm_world *w, int32_t go, const float p[3])
{
    go_require_pose(w, go);
    float t[3] = { p[0], p[1], p[2] };
    inverse_transform_position(w, w->gos[go].parent, t);
    set_local_t(w, go, t);
}

void go_local_pos(fsm_world *w, int32_t go, float out[3])
{
    go_require_pose(w, go);
    if (w->gos[go].parent < 0) { go_world_pos(w, go, out); return; }
    memcpy(out, w->gos[go].local_pos, sizeof(float) * 3);
}
/* Transform::SetLocalPosition (UP!0x1807e9a30, §3.1) stores the three floats. */
void go_set_local_pos(fsm_world *w, int32_t go, const float p[3])
{
    go_require_pose(w, go);
    set_local_t(w, go, p);
}
void go_local_scale(const fsm_world *w, int32_t go, float out[3])
{
    go_require_pose(w, go);
    memcpy(out, w->gos[go].local_scale, sizeof(float) * 3);
    const go_inst *g = &w->gos[go];
    if (w->phys && g->body && g->body == w->hero_body)
        out[0] = phys_body_scale_x((const phys_world *)w->phys, g->body);   /* the hero module owns the Knight's flip (HC:1535-1549 write transform.localScale) */
}
void go_set_local_scale(fsm_world *w, int32_t go, const float s[3])
{
    go_require_pose(w, go);
    go_inst *g = &w->gos[go];
    memcpy(g->local_scale, s, sizeof(float) * 3);
    if (w->phys && g->body && g->body == w->hero_body)
        phys_body_set_scale_x((phys_world *)w->phys, g->body, s[0]);   /* the Knight: a root, local == lossy (hero module owns the flip) */
    phys_rescale_shapes(w, go);
    invalidate_transform_dfs(w, go);
    /* Any other body takes its object's LOSSY scale on both axes.  Bodies further down the subtree are
     * refreshed by the pre-step flush (world_flush_dirty_shapes). */
    if (w->phys && g->body && g->body != w->hero_body) {
        float ls[3]; go_lossy_scale(w, go, ls);
        phys_body_set_scale((phys_world *)w->phys, g->body, (phys_v2){ ls[0], ls[1] });
    }
}
/* Transform.lossyScale: product of local scales up the chain (no rotation-induced skew for the
 * axis-aligned, z-rotation-only hierarchies in the dumps). */
void go_lossy_scale(const fsm_world *w, int32_t go, float out[3])
{
    ensure_transform_clean((fsm_world *)w, go);
    out[0] = w->gos[go].lossy_scale[0];
    out[1] = w->gos[go].lossy_scale[1];
    out[2] = w->gos[go].lossy_scale[2];
}
float go_local_euler_z(const fsm_world *w, int32_t go) { go_require_pose(w, go); return w->gos[go].local_euler_z; }
float go_euler_z(const fsm_world *w, int32_t go)
{
    ensure_transform_clean((fsm_world *)w, go);
    return w->gos[go].world_euler_z;
}
/* Transform.TransformDirection — UP!0x1807eb8e0 TransformDirection [TransformHierarchy.h:767-789]: rotate by the
 * object's own local rotation (its own scale is not applied), then at each ancestor flip every component whose
 * ancestor localScale component is < 0 (`(0<s)-(s<0)`, so -0 and NaN do not flip) and rotate by that ancestor's
 * local rotation.  The rotations come from the stored euler angles (class T0, native-transform_time.md §5). */
void go_transform_direction(fsm_world *w, int32_t go, const float d[2], float out[2])
{
    go_require_pose(w, go);
    float x = d[0], y = d[1];
    for (int32_t i = go; i >= 0; i = w->gos[i].parent) {
        const go_inst *g = &w->gos[i];
        if (i != go) {
            float sx = g->local_scale[0];
            if (g->body && g->body == w->hero_body && w->phys)
                sx = phys_body_scale_x((const phys_world *)w->phys, g->body);   /* the hero module owns the Knight's flip */
            if (sx < 0.0f) x = -x;
            if (g->local_scale[1] < 0.0f) y = -y;
        }
        if (g->local_euler_z != 0.0f) {
            float ang = g->local_euler_z * ((float)M_PI / 180.0f);
            float c = m_cos(ang), s = m_sin(ang);
            float rx = c * x - s * y, ry = s * x + c * y;
            x = rx; y = ry;
        }
    }
    out[0] = x; out[1] = y;
}
/* A rotation write moves the world rotation of the object AND of everything under it, and phys stores the
 * world rotation on the body -- so every body at or below the written object has to be re-synchronised, the
 * same way go_set_world_pos / go_set_local_scale push position and scale. */
void world_push_body_rotations(fsm_world *w, int32_t go)
{
    if (go < 0) return;
    go_inst *g = &w->gos[go];
    if (g->body) (void)go_euler_z(w, go);   /* resolving the transform is what pushes it (ensure_transform_clean) */
    for (int32_t c = g->first_child; c >= 0; c = w->gos[c].next_sibling) world_push_body_rotations(w, c);
}
void go_set_local_euler_z(fsm_world *w, int32_t go, float z)
{
    go_require_pose(w, go);
    w->gos[go].local_euler_z = z;
    invalidate_transform_dfs(w, go);
    world_push_body_rotations(w, go);
}
void go_set_euler_z(fsm_world *w, int32_t go, float z)
{
    go_require_pose(w, go);
    int32_t parent = w->gos[go].parent;
    float pz = 0.0f, hs = 1.0f;
    if (parent >= 0) {
        pz = go_euler_z(w, parent);
        float ps[3]; go_lossy_scale(w, parent, ps);
        hs = fsign1(ps[0] * ps[1]);   /* invert world = pz + hs*local */
    }
    w->gos[go].local_euler_z = (z - pz) * hs;   /* hs is +-1, so *hs == /hs */
    invalidate_transform_dfs(w, go);
    world_push_body_rotations(w, go);
}

/* Transform.parent setter.  Whether Unity preserves the world pose is not stated by any decomp (Q-fsmact-11,
 * Q-hornet-3).  The only live callers are Needle Tink/Setup (re-parents itself) and Needle Tink/Deparent
 * (re-parents the Needle); both objects' world pose is re-written in world space before anything reads it
 * (Follow: SetPosition/SetRotation World every frame; Throw: SetPosition World before activation; scale: both
 * carry localScale (1,1,1) and Hornet is at scale (1,1,1) until her first FaceObject in Idle — dumps
 * scene.json#Needle, #Needle Tink, FSM#Control states Pause..GG Fall have no scale write), so either
 * semantic is observationally identical for them (port-fsm.md §SetParent).  Any other object traps. */
void go_set_parent(fsm_world *w, int32_t go, int32_t parent)
{
    float p[3], s[3]; float ez;
    go_world_pos(w, go, p); go_lossy_scale(w, go, s); ez = go_euler_z(w, go);
    go_inst *g = &w->gos[go];
    /* unlink */
    if (g->parent >= 0) {
        go_inst *pg = &w->gos[g->parent];
        if (pg->first_child == go) pg->first_child = g->next_sibling;
        else for (int32_t c = pg->first_child; c >= 0; c = w->gos[c].next_sibling) if (w->gos[c].next_sibling == go) { w->gos[c].next_sibling = g->next_sibling; break; }
    }
    g->parent = parent; g->next_sibling = -1;
    if (parent >= 0) {                                             /* appended as the last child */
        go_inst *pg = &w->gos[parent];
        if (pg->first_child < 0) pg->first_child = go;
        else { int32_t c = pg->first_child; while (w->gos[c].next_sibling >= 0) c = w->gos[c].next_sibling; w->gos[c].next_sibling = go; }
    }
    /* keep the world pose */
    float ps[3] = { 1, 1, 1 }; float pez = 0.0f;
    if (parent >= 0) { go_lossy_scale(w, parent, ps); pez = go_euler_z(w, parent); }
    g->local_scale[0] = ps[0] != 0.0f ? s[0] / ps[0] : s[0];
    g->local_scale[1] = ps[1] != 0.0f ? s[1] / ps[1] : s[1];
    g->local_scale[2] = ps[2] != 0.0f ? s[2] / ps[2] : s[2];
    g->local_euler_z = (ez - pez) * fsign1(ps[0] * ps[1]);   /* invert world = pez + hs*local */
    go_set_world_pos(w, go, p);
    invalidate_transform_dfs(w, go);
    world_push_body_rotations(w, go);   /* the new parent's rotation is now part of this subtree's world pose */
    /* Transform::SetParent (UP!0x1807e9f80) ends in GameObject::TransformParentHasChanged (UP!0x180580e70): a
     * move under an inactive (active) parent deactivates (activates) the subtree, with its callbacks */
    lc_go_active_changed(w, go);
}

bool go_has_component(const fsm_world *w, int32_t go, const char *type_short)
{
    if (go < 0) return false;
    const go_def *g = w->gos[go].def;
    for (int32_t k = 0; k < g->n_comps; k++) {
        const char *t = w_str(w, w->sc->comps[g->comp_start + k].type);
        const char *dot = strrchr(t, '.');
        if (strcmp(dot ? dot + 1 : t, type_short) == 0) return true;
    }
    return false;
}

/* ---- SetActive ---- */

/* GameObject.SetActive = GameObject::SetSelfActive (UP!0x180580c90): activeSelf is written first, then the
 * lifecycle's walk recomputes activeInHierarchy and runs the OnEnable / OnDisable calls -- synchronous, the
 * activated subtree in ascending execution order then DESCENDING instance id, a deactivation depth-first
 * post-order (lc_go_active_changed; R4, docs/engine-lifecycle.md).  So inside OnDisable the object reads
 * activeSelf and activeInHierarchy false. */
void go_set_active(fsm_world *w, int32_t go, bool active)
{
    if (go < 0 || w->snapshot_mode) return;
    go_inst *g = &w->gos[go];
    if (g->destroyed) return;                                      /* a destroyed object compares == null */
    if (g->active_self == (active ? 1 : 0)) return;
    g->active_self = active ? 1 : 0;
    lc_go_active_changed(w, go);
}
/* Object.Destroy took effect (lifecycle.c destroy_go_now): the subtree compares == null from now on */
void world_mark_destroyed(fsm_world *w, int32_t go)
{
    w->gos[go].destroyed = 1;
    w->gos[go].active_in_hierarchy = 0;
    for (int32_t c = w->gos[go].first_child; c >= 0; c = w->gos[c].next_sibling) world_mark_destroyed(w, c);
}
