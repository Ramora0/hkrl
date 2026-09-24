/* Rigidbody2D velocity and forces, Collider2D state, raycasts, collision and trigger events. */
#include "act.h"

/* RayCast2d — ACT/RayCast2d.cs:74-153, RayCast2dV2 — RayCast2dV2.cs:74-152
 * (V2 differs only in the stores: storeHitDistance <- fraction, storeDistance <- distance, and `distance != 0`). */
typedef struct {
    const fsm_pv *fromGameObject, *fromPosition, *direction, *space, *distance, *minDepth, *maxDepth, *hitEvent, *storeDidHit, *storeHitObject,
           *storeHitPoint, *storeHitNormal, *storeHitDistance, *storeHitFraction, *storeDistance, *repeatInterval, *layerMask, *invertMask;
    int32_t trans, repeat; bool v2;
} st_ray;
static void ray_bind_common(act_inst *a, bool v2)
{
    ST(st_ray);
    s->fromGameObject = FIELD(fromGameObject); s->fromPosition = FIELD(fromPosition); s->direction = FIELD(direction); s->space = FIELD(space);
    s->distance = FIELD(distance); s->minDepth = FIELD(minDepth); s->maxDepth = FIELD(maxDepth); s->hitEvent = FIELD_OPT(hitEvent);
    s->storeDidHit = FIELD(storeDidHit); s->storeHitObject = FIELD(storeHitObject); s->storeHitPoint = FIELD(storeHitPoint);
    s->storeHitNormal = FIELD(storeHitNormal); s->storeHitDistance = FIELD(storeHitDistance); s->storeHitFraction = FIELD_OPT(storeHitFraction);
    s->storeDistance = FIELD_OPT(storeDistance); s->repeatInterval = FIELD(repeatInterval); s->layerMask = FIELD(layerMask); s->invertMask = FIELD(invertMask);
    s->trans = -1; s->repeat = 0; s->v2 = v2;
}
static void ray_bind(act_inst *a) { ray_bind_common(a, false); }
static void rayv2_bind(act_inst *a) { ray_bind_common(a, true); }
/* ActionHelpers.LayerArrayToLayerMask — PM/ActionHelpers.cs:345-361 */
static uint32_t layer_array_to_mask(act_inst *a, const fsm_pv *layers, bool invert)
{
    int32_t num = 0;
    int32_t n = a_array_len(a, layers);
    for (int32_t i = 0; i < n; i++) num |= 1 << pi(a->fsm, a_array_elem(a, layers, i));
    if (invert) num = ~num;
    return (uint32_t)(num != 0 ? num : -5);                        /* -5 == Physics2D.DefaultRaycastLayers */
}
static void ray_do(act_inst *a)
{
    ST(st_ray);
    s->repeat = pi(f, s->repeatInterval);
    float dist = pf(f, s->distance);
    if (s->v2 ? (dist != 0.0f) : !(fabsf(dist) < 1.4013e-45f)) {   /* Mathf.Epsilon */
        const float *fp = pv3(f, s->fromPosition);
        float origin[2] = { fp[0], fp[1] };
        if (s->trans >= 0) { float p[3]; go_world_pos(w, s->trans, p); origin[0] += p[0]; origin[1] += p[1]; }
        float len = dist > 0.0f ? dist : INFINITY;
        /* Vector2.normalized: magnitude = (float)Math.Sqrt(x*x + y*y) (double stack), > 1e-5 ? this / mag : zero */
        const float *dv = pv3(f, s->direction);
        float dir[2] = { 0.0f, 0.0f };
        float mag = (float)sqrt((double)dv[0] * dv[0] + (double)dv[1] * dv[1]);
        if (mag > 1E-05f) { dir[0] = dv[0] / mag; dir[1] = dv[1] / mag; }
        if (s->trans >= 0 && pi(f, s->space) == 1)                  /* Space.Self: Transform.TransformDirection (mirrors under a flipped ancestor) */
            go_transform_direction(w, s->trans, dv, dir);
        if (!(p_isnone(s->minDepth) && p_isnone(s->maxDepth))) HKSIM_UNKNOWN("RayCast2d with min/maxDepth in %s", fsm_label(f));
        uint32_t mask = layer_array_to_mask(a, s->layerMask, pb(f, s->invertMask));
        float hp[2], hn[2]; int32_t hgo; bool htrig;
        bool hit = world_raycast_ex(w, origin, dir, len, mask, hp, hn, &hgo, &htrig);
        pb_set(f, s->storeDidHit, hit);
        if (hit) {
            pgo_set(f, s->storeHitObject, hgo);
            float v3[3] = { hp[0], hp[1], 0.0f }; pv3_set(f, s->storeHitPoint, v3);
            float n3[3] = { hn[0], hn[1], 0.0f }; pv3_set(f, s->storeHitNormal, n3);
            /* RaycastHit2D.distance = |point - origin| (Vector2.Distance: per-component float subtraction, double-stack sqrt) */
            float dx = hp[0] - origin[0], dy = hp[1] - origin[1];
            float d = (float)sqrt((double)dx * dx + (double)dy * dy);
            float frac = isinf(len) ? 0.0f : d / len;
            if (s->v2) { pf_set(f, s->storeHitDistance, frac); if (s->storeDistance) pf_set(f, s->storeDistance, d); }
            else       { pf_set(f, s->storeHitDistance, d); if (s->storeHitFraction) pf_set(f, s->storeHitFraction, frac); }
            fsm_event(f, EV(s->hitEvent));
        }
    }
}
static void ray_enter(act_inst *a)
{
    ST(st_ray);
    s->trans = p_owner_default(a, s->fromGameObject);
    ray_do(a);
    if (pi(f, s->repeatInterval) == 0) act_finish(a);
}
static void ray_update(act_inst *a)
{
    ST(st_ray);
    s->repeat--;
    if (s->repeat == 0) ray_do(a);
}
static const act_vtable AV_RayCast2d = { "RayCast2d", sizeof(st_ray), ray_bind, ray_enter, ray_update, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_RayCast2dV2 = { "RayCast2dV2", sizeof(st_ray), rayv2_bind, ray_enter, ray_update, NULL, NULL, NULL, NULL, NULL };

/* SetBoxCollider2DSize — ACT/SetBoxCollider2DSize.cs:30-55 */
typedef struct { const fsm_pv *go, *width, *height, *offsetX, *offsetY; } st_sbcs;
static void sbcs_bind(act_inst *a) { ST(st_sbcs); s->go = FIELD(gameObject1); s->width = FIELD(width); s->height = FIELD(height); s->offsetX = FIELD(offsetX); s->offsetY = FIELD(offsetY); }
static void sbcs_enter(act_inst *a)
{
    ST(st_sbcs);
    int32_t t = p_owner_default(a, s->go);
    col_inst *c = t >= 0 ? go_box_collider(w, t) : NULL;
    if (!c) HKSIM_UNKNOWN("SetBoxCollider2DSize: no BoxCollider2D on '%s' (NullReferenceException in C#)", t >= 0 ? go_path(w, t) : "null");
    float size[2] = { c->size[0], c->size[1] }, off[2] = { c->offset[0], c->offset[1] };
    if (!p_isnone(s->width)) size[0] = pf(f, s->width);
    if (!p_isnone(s->height)) size[1] = pf(f, s->height);
    if (!p_isnone(s->offsetX)) off[0] = pf(f, s->offsetX);
    if (!p_isnone(s->offsetY)) off[1] = pf(f, s->offsetY);
    col_set_box(w, t, c, size, off);
    act_finish(a);
}
static const act_vtable AV_SetBoxCollider2DSize = { "SetBoxCollider2DSize", sizeof(st_sbcs), sbcs_bind, sbcs_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* SetInterpolate / SetExtrapolate — ACT/SetInterpolate.cs:15-30, SetExtrapolate.cs:15-30.  Interpolation is not
 * render-only: it makes transform.position lag rb2d.position by a wall-clock residual that FSMs read (B9).  A no-op
 * is exact because the game runs regime R2: oracle/Env/RegimeTweaks.cs IL-rewrites every set_interpolation call to
 * pass None (docs/frame-order.md), so these actions set None in the game too. */
static const act_vtable AV_SetInterpolate = { "SetInterpolate", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_SetExtrapolate = { "SetExtrapolate", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* collideTag test of the collision / trigger events: None, "" and "Untagged" match anything */
static bool collide_tag_matches(act_inst *a, const fsm_pv *collideTag, int32_t other_go)
{
    fsm_inst *f = a->fsm; fsm_world *w = f->w;
    const char *tag = w_str(w, ps(f, collideTag));
    if (p_isnone(collideTag) || tag[0] == 0 || strcmp(tag, "Untagged") == 0) return true;
    int32_t ot = go_tag(w, other_go);
    return ot >= 0 && strcmp(w_str(w, ot), tag) == 0;
}

/* Collision2dEvent — ACT/Collision2dEvent.cs:22-83: PlayMakerUnity2DProxy collision delegates (kinds 0 enter, 1 stay, 2 exit) */
typedef struct { const fsm_pv *collision, *collideTag, *sendEvent, *storeCollider, *storeForce; int32_t proxy_go; } st_c2e;
static void c2e_bind(act_inst *a) { ST(st_c2e); s->collision = FIELD(collision); s->collideTag = FIELD(collideTag); s->sendEvent = FIELD_OPT(sendEvent); s->storeCollider = FIELD(storeCollider); s->storeForce = FIELD(storeForce); s->proxy_go = -1; }
static void c2e_enter(act_inst *a)
{
    ST(st_c2e);
    if (pi(f, s->collision) > 2) HKSIM_UNIMPLEMENTED("Collision2dEvent OnParticleCollision in %s", fsm_label(f));
    s->proxy_go = f->go; proxy_add(w, f->go, pi(f, s->collision), a);
}
static void c2e_exit(act_inst *a) { ST(st_c2e); if (s->proxy_go >= 0) proxy_remove(w, s->proxy_go, pi(f, s->collision), a); }
/* cite: analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/Collision2dEvent.cs:64-89 */
static void c2e_proxy(act_inst *a, int kind, int32_t other_go, int32_t other_layer)
{
    (void)other_layer;
    ST(st_c2e);
    if (kind != pi(f, s->collision)) return;
    if (!collide_tag_matches(a, s->collideTag, other_go)) return;
    pgo_set(f, s->storeCollider, other_go);                        /* StoreCollisionInfo: collisionInfo.gameObject */
    if (!p_isnone(s->storeForce)) HKSIM_UNKNOWN("Collision2dEvent.storeForce (relativeVelocity.magnitude) needs the contact velocity in %s", fsm_label(f));
    fsm_event(f, EV(s->sendEvent));
}
static const act_vtable AV_Collision2dEvent = { "Collision2dEvent", sizeof(st_c2e), c2e_bind, c2e_enter, NULL, NULL, NULL, c2e_exit, NULL, c2e_proxy };

/* AddForce2d — ACT/AddForce2d.cs:80-107: force = vector (else x,y), then vector3, x, y override; atPosition
 * selects AddForceAtPosition.  OnPreprocess sets HandleFixedUpdate and DoAddForce repeats from OnFixedUpdate
 * (:70-75); b2World clears the accumulator each step.  Only ForceMode2D.Force is ported (Impulse differs by a
 * factor of dt), and AddForceAtPosition adds torque the fixed-rotation bodies here cannot take: both trap. */
typedef struct { const fsm_pv *go, *forceMode, *atPosition, *vector, *x, *y, *vector3, *everyFrame; } st_af2;
static void af2_bind(act_inst *a)
{
    ST(st_af2);
    s->go = FIELD(gameObject); s->forceMode = FIELD_OPT(forceMode); s->atPosition = FIELD_OPT(atPosition);
    s->vector = FIELD_OPT(vector); s->x = FIELD_OPT(x); s->y = FIELD_OPT(y);
    s->vector3 = FIELD_OPT(vector3); s->everyFrame = FIELD_OPT(everyFrame);
}
static void af2_do(act_inst *a)
{
    ST(st_af2);
    int32_t t = p_owner_default(a, s->go);
    if (t < 0) return;
    if (s->forceMode && pi(f, s->forceMode) != 0)
        HKSIM_UNIMPLEMENTED("AddForce2d forceMode=%d (only ForceMode2D.Force=0 is ported) in %s", pi(f, s->forceMode), fsm_label(f));
    if (s->atPosition && !p_isnone(s->atPosition))
        HKSIM_UNIMPLEMENTED("AddForce2d atPosition set (AddForceAtPosition adds torque) in %s", fsm_label(f));
    float fx = 0.0f, fy = 0.0f;
    if (s->vector && !p_isnone(s->vector)) { const float *vv = pv3(f, s->vector); fx = vv[0]; fy = vv[1]; }
    else { fx = s->x ? pf(f, s->x) : 0.0f; fy = s->y ? pf(f, s->y) : 0.0f; }
    if (s->vector3 && !p_isnone(s->vector3)) { const float *v3 = pv3(f, s->vector3); fx = v3[0]; fy = v3[1]; }
    if (s->x && !p_isnone(s->x)) fx = pf(f, s->x);
    if (s->y && !p_isnone(s->y)) fy = pf(f, s->y);
    float fv[2] = { fx, fy };
    go_add_force(w, t, fv);
}
static void af2_enter(act_inst *a) { ST(st_af2); af2_do(a); if (!s->everyFrame || !pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_AddForce2d = { "AddForce2d", sizeof(st_af2), af2_bind, af2_enter, NULL, af2_do, NULL, NULL, NULL, NULL };

/* SetVelocity2d — ACT/SetVelocity2d.cs:41-80 (FixedUpdate; OnEnter + next OnFixedUpdate both write) */
typedef struct { const fsm_pv *go, *vector, *x, *y, *everyFrame; int32_t cached_go; bool has; } st_setvel;
static void setvel_bind(act_inst *a) { ST(st_setvel); s->go = FIELD(gameObject); s->vector = FIELD(vector); s->x = FIELD(x); s->y = FIELD(y); s->everyFrame = FIELD(everyFrame); s->cached_go = -1; }
static void setvel_do(act_inst *a)
{
    ST(st_setvel);
    int32_t t = p_owner_default(a, s->go);
    if (!act_cache_rb(a, t, &s->cached_go, &s->has)) return;
    float v[2];
    if (!p_isnone(s->vector)) { const float *vv = pv3(f, s->vector); v[0] = vv[0]; v[1] = vv[1]; } else go_velocity(w, t, v);
    if (!p_isnone(s->x)) v[0] = pf(f, s->x);
    if (!p_isnone(s->y)) v[1] = pf(f, s->y);
    go_set_velocity(w, t, v);
}
static void setvel_enter(act_inst *a) { ST(st_setvel); setvel_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static void setvel_fixed(act_inst *a) { ST(st_setvel); setvel_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_SetVelocity2d = { "SetVelocity2d", sizeof(st_setvel), setvel_bind, setvel_enter, NULL, setvel_fixed, NULL, NULL, NULL, NULL };

/* GetVelocity2d — ACT/GetVelocity2d.cs:42-70 (Update) */
typedef struct { const fsm_pv *go, *vector, *x, *y, *space, *everyFrame; int32_t cached_go; bool has; } st_getvel;
static void getvel_bind(act_inst *a) { ST(st_getvel); s->go = FIELD(gameObject); s->vector = FIELD(vector); s->x = FIELD(x); s->y = FIELD(y); s->space = FIELD(space); s->everyFrame = FIELD(everyFrame); s->cached_go = -1; }
static void getvel_do(act_inst *a)
{
    ST(st_getvel);
    int32_t t = p_owner_default(a, s->go);
    if (!act_cache_rb(a, t, &s->cached_go, &s->has)) return;
    float v[4] = { 0, 0, 0, 0 }; go_velocity(w, t, v);
    if (s->space->i == 1) HKSIM_UNKNOWN("GetVelocity2d Space.Self (InverseTransformDirection) in %s", fsm_label(f));
    pv3_set(f, s->vector, v);
    pf_set(f, s->x, v[0]); pf_set(f, s->y, v[1]);
}
static void getvel_enter(act_inst *a) { ST(st_getvel); getvel_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GetVelocity2d = { "GetVelocity2d", sizeof(st_getvel), getvel_bind, getvel_enter, getvel_do, NULL, NULL, NULL, NULL, NULL };

/* GetSpeed2d — ACT/GetSpeed2d.cs:26-46 */
typedef struct { const fsm_pv *go, *store, *everyFrame; int32_t cached_go; bool has; } st_getspeed;
static void getspeed_bind(act_inst *a) { ST(st_getspeed); s->go = FIELD(gameObject); s->store = FIELD(storeResult); s->everyFrame = FIELD(everyFrame); s->cached_go = -1; }
static void getspeed_do(act_inst *a)
{
    ST(st_getspeed);
    if (p_isnone(s->store)) return;
    int32_t t = p_owner_default(a, s->go);
    if (!act_cache_rb(a, t, &s->cached_go, &s->has)) return;
    float v[2]; go_velocity(w, t, v);
    pf_set(f, s->store, (float)sqrt((double)v[0] * v[0] + (double)v[1] * v[1]));   /* Vector2.magnitude: (float)Math.Sqrt(x*x + y*y), double stack */
}
static void getspeed_enter(act_inst *a) { ST(st_getspeed); getspeed_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GetSpeed2d = { "GetSpeed2d", sizeof(st_getspeed), getspeed_bind, getspeed_enter, getspeed_do, NULL, NULL, NULL, NULL, NULL };

/* SetVelocityAsAngle — ACT/SetVelocityAsAngle.cs:40-80 (FixedUpdate) */
typedef struct { const fsm_pv *go, *angle, *speed, *everyFrame; int32_t rb; } st_setvelang;
static void setvelang_bind(act_inst *a) { ST(st_setvelang); s->go = FIELD(gameObject); s->angle = FIELD(angle); s->speed = FIELD(speed); s->everyFrame = FIELD(everyFrame); s->rb = -1; }
static void setvelang_do(act_inst *a)
{
    ST(st_setvelang);
    if (s->rb < 0) return;
    float ang = pf(f, s->angle) * ((float)M_PI / 180.0f);
    float v[2] = { pf(f, s->speed) * m_cos(ang), pf(f, s->speed) * m_sin(ang) };
    go_set_velocity(w, s->rb, v);
}
static void setvelang_enter(act_inst *a)
{
    ST(st_setvelang);
    int32_t t = p_owner_default(a, s->go);
    if (t >= 0 && go_has_rb(w, t)) s->rb = t;
    setvelang_do(a);
    if (!pb(f, s->everyFrame)) act_finish(a);
}
static void setvelang_fixed(act_inst *a) { ST(st_setvelang); setvelang_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_SetVelocityAsAngle = { "SetVelocityAsAngle", sizeof(st_setvelang), setvelang_bind, setvelang_enter, NULL, setvelang_fixed, NULL, NULL, NULL, NULL };

/* SetGravity2dScale — ACT/SetGravity2dScale.cs:24-37; SetIsKinematic2d — ACT/SetIsKinematic2d.cs:24-37 */
typedef struct { const fsm_pv *go, *v; int32_t cached_go; bool has; } st_rbset;
static void setgrav_bind(act_inst *a) { ST(st_rbset); s->go = FIELD(gameObject); s->v = FIELD(gravityScale); s->cached_go = -1; }
static void setgrav_enter(act_inst *a) { ST(st_rbset); int32_t t = p_owner_default(a, s->go); if (act_cache_rb(a, t, &s->cached_go, &s->has)) go_set_gravity_scale(w, t, pf(f, s->v)); act_finish(a); }
static const act_vtable AV_SetGravity2dScale = { "SetGravity2dScale", sizeof(st_rbset), setgrav_bind, setgrav_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static void setkin_bind(act_inst *a) { ST(st_rbset); s->go = FIELD(gameObject); s->v = FIELD(isKinematic); s->cached_go = -1; }
static void setkin_enter(act_inst *a) { ST(st_rbset); int32_t t = p_owner_default(a, s->go); if (act_cache_rb(a, t, &s->cached_go, &s->has)) go_set_kinematic(w, t, pb(f, s->v)); act_finish(a); }
static const act_vtable AV_SetIsKinematic2d = { "SetIsKinematic2d", sizeof(st_rbset), setkin_bind, setkin_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* DecelerateV2 — ACT/DecelerateV2.cs:21-82 (FixedUpdate, never finishes) */
typedef struct { const fsm_pv *go, *decel; int32_t rb; } st_decel;
static void decel_bind(act_inst *a) { ST(st_decel); s->go = FIELD(gameObject); s->decel = FIELD(deceleration); s->rb = -1; }
static void decel_self(act_inst *a)
{
    ST(st_decel);
    if (s->rb < 0) return;
    float v[2]; go_velocity(w, s->rb, v);
    float d = pf(f, s->decel);
    v[0] = act_damp_axis(v[0], d);
    v[1] = act_damp_axis(v[1], d);
    go_set_velocity(w, s->rb, v);
}
static void decel_enter(act_inst *a) { ST(st_decel); int32_t t = p_owner_default(a, s->go); if (t >= 0 && go_has_rb(w, t)) s->rb = t; decel_self(a); }
static const act_vtable AV_DecelerateV2 = { "DecelerateV2", sizeof(st_decel), decel_bind, decel_enter, NULL, decel_self, NULL, NULL, NULL, NULL };

/* DecelerateXY — ACT/DecelerateXY.cs:24-99 */
typedef struct { const fsm_pv *go, *dx, *dy; int32_t rb; } st_decelxy;
static void decelxy_bind(act_inst *a) { ST(st_decelxy); s->go = FIELD(gameObject); s->dx = FIELD(decelerationX); s->dy = FIELD(decelerationY); s->rb = -1; }
static void decelxy_self(act_inst *a)
{
    ST(st_decelxy);
    if (s->rb < 0) return;
    float v[2]; go_velocity(w, s->rb, v);
    if (!p_isnone(s->dx)) {
        float d = pf(f, s->dx);
        v[0] = act_damp_axis(v[0], d);
        if (v[0] < 0.001f && v[0] > -0.001f) v[0] = 0.0f;
    }
    if (!p_isnone(s->dy)) {
        float d = pf(f, s->dy);
        v[1] = act_damp_axis(v[1], d);
        if (v[1] < 0.001f && v[1] > -0.001f) v[1] = 0.0f;
    }
    go_set_velocity(w, s->rb, v);
}
static void decelxy_enter(act_inst *a) { ST(st_decelxy); int32_t t = p_owner_default(a, s->go); if (t >= 0 && go_has_rb(w, t)) s->rb = t; decelxy_self(a); }
static const act_vtable AV_DecelerateXY = { "DecelerateXY", sizeof(st_decelxy), decelxy_bind, decelxy_enter, NULL, decelxy_self, NULL, NULL, NULL, NULL };

/* CheckCollisionSide — ACT/CheckCollisionSide.cs:75-240 */
typedef struct {
    const fsm_pv *topHit, *rightHit, *bottomHit, *leftHit, *topHitEvent, *rightHitEvent, *bottomHitEvent, *leftHitEvent, *otherLayer, *otherLayerNumber, *ignoreTriggers;
    bool checkUp, checkDown, checkLeft, checkRight; int32_t proxy_go;
} st_ccs;
static void ccs_bind(act_inst *a)
{
    ST(st_ccs);
    s->topHit = FIELD(topHit); s->rightHit = FIELD(rightHit); s->bottomHit = FIELD(bottomHit); s->leftHit = FIELD(leftHit);
    s->topHitEvent = FIELD(topHitEvent); s->rightHitEvent = FIELD(rightHitEvent); s->bottomHitEvent = FIELD(bottomHitEvent); s->leftHitEvent = FIELD(leftHitEvent);
    s->otherLayer = FIELD(otherLayer); s->otherLayerNumber = FIELD(otherLayerNumber); s->ignoreTriggers = FIELD(ignoreTriggers);
    s->proxy_go = -1;
}
/* three rays per side from the collider bounds, length 0.08 (RAYCAST_LENGTH :49), mask 1<<layer */
static bool ccs_side(act_inst *a, int layer, const float o[3][2], const float dir[2], const fsm_pv *hitBool, const fsm_pv *hitEvent)
{
    ST(st_ccs);
    pb_set(f, hitBool, false);
    for (int i = 0; i < 3; i++) {
        bool trig;
        if (world_raycast(w, o[i], dir, 0.08f, 1u << layer, &trig) && (!pb(f, s->ignoreTriggers) || !trig)) {
            pb_set(f, hitBool, true);
            fsm_event(f, EV(hitEvent));
            return true;
        }
    }
    return false;
}
static void ccs_check_touching(act_inst *a, int layer, bool all_sides)
{
    ST(st_ccs);
    col_inst *c = go_first_collider(w, f->go);
    HKSIM_ASSERT(c != NULL, "CheckCollisionSide: no Collider2D on %s", fsm_label(f));
    float mn[2], mx[2]; col_bounds(w, f->go, c, mn, mx);
    float cx = (mn[0] + mx[0]) * 0.5f, cy = (mn[1] + mx[1]) * 0.5f;
    if (all_sides || s->checkUp) { float o[3][2] = { { mn[0], mx[1] }, { cx, mx[1] }, { mx[0], mx[1] } }; float d[2] = { 0, 1 }; ccs_side(a, layer, o, d, s->topHit, s->topHitEvent); }
    if (all_sides || s->checkRight) { float o[3][2] = { { mx[0], mx[1] }, { mx[0], cy }, { mx[0], mn[1] } }; float d[2] = { 1, 0 }; ccs_side(a, layer, o, d, s->rightHit, s->rightHitEvent); }
    if (all_sides || s->checkDown) { float o[3][2] = { { mx[0], mn[1] }, { cx, mn[1] }, { mn[0], mn[1] } }; float d[2] = { 0, -1 }; ccs_side(a, layer, o, d, s->bottomHit, s->bottomHitEvent); }
    if (all_sides || s->checkLeft) { float o[3][2] = { { mn[0], mn[1] }, { mn[0], cy }, { mn[0], mx[1] } }; float d[2] = { -1, 0 }; ccs_side(a, layer, o, d, s->leftHit, s->leftHitEvent); }
}
static void ccs_enter(act_inst *a)
{
    ST(st_ccs);
    s->proxy_go = f->go;
    proxy_add(w, f->go, 1, a);                                     /* AddOnCollisionStay2dDelegate :82 */
    s->checkUp = !p_isnone(s->topHit) || EV(s->topHitEvent) >= 0;
    s->checkRight = !p_isnone(s->rightHit) || EV(s->rightHitEvent) >= 0;
    s->checkDown = !p_isnone(s->bottomHit) || EV(s->bottomHitEvent) >= 0;
    s->checkLeft = !p_isnone(s->leftHit) || EV(s->leftHitEvent) >= 0;
}
static void ccs_exit(act_inst *a) { ST(st_ccs); if (s->proxy_go >= 0) proxy_remove(w, s->proxy_go, 1, a); }
static void ccs_update(act_inst *a)
{
    ST(st_ccs);
    if (pb(f, s->topHit) || pb(f, s->bottomHit) || pb(f, s->rightHit) || pb(f, s->leftHit))
        ccs_check_touching(a, pb(f, s->otherLayer) ? s->otherLayerNumber->i : 8, false);   /* :131-138 hardcoded layer 8 */
}
/* cite: analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/CheckCollisionSide.cs:142-155 */
static void ccs_proxy(act_inst *a, int kind, int32_t other_go, int32_t other_layer)
{
    ST(st_ccs);
    (void)other_go;
    if (kind != 1) return;
    if (!pb(f, s->otherLayer)) { if (other_layer == 8) ccs_check_touching(a, 8, false); }
    else ccs_check_touching(a, s->otherLayerNumber->i, false);
}
static const act_vtable AV_CheckCollisionSide = { "CheckCollisionSide", sizeof(st_ccs), ccs_bind, ccs_enter, ccs_update, NULL, NULL, ccs_exit, NULL, ccs_proxy };

/* CheckCollisionSideEnter — ACT/CheckCollisionSideEnter.cs:63-160 */
static void ccse_enter(act_inst *a) { ST(st_ccs); s->proxy_go = f->go; proxy_add(w, f->go, 0, a); }
static void ccse_exit(act_inst *a) { ST(st_ccs); if (s->proxy_go >= 0) proxy_remove(w, s->proxy_go, 0, a); }
/* cite: analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/CheckCollisionSideEnter.cs:83-96 */
static void ccse_proxy(act_inst *a, int kind, int32_t other_go, int32_t other_layer)
{
    ST(st_ccs);
    (void)other_go;
    if (kind != 0) return;
    if (!pb(f, s->otherLayer)) {
        if ((other_layer >= 0 && other_layer < 32 && strcmp(w->sc->layer_names[other_layer], "Terrain") == 0) || other_layer == 8) ccs_check_touching(a, 8, true);   /* :87-94 */
    } else ccs_check_touching(a, s->otherLayerNumber->i, true);
}
static const act_vtable AV_CheckCollisionSideEnter = { "CheckCollisionSideEnter", sizeof(st_ccs), ccs_bind, ccse_enter, NULL, NULL, NULL, ccse_exit, NULL, ccse_proxy };

/* Trigger2dEvent — ACT/Trigger2dEvent.cs:41-111 */
typedef struct { const fsm_pv *trigger, *collideTag, *collideLayer, *sendEvent, *storeCollider; int32_t proxy_go; } st_t2e;
static void t2e_bind(act_inst *a) { ST(st_t2e); s->trigger = FIELD(trigger); s->collideTag = FIELD(collideTag); s->collideLayer = FIELD(collideLayer); s->sendEvent = FIELD(sendEvent); s->storeCollider = FIELD(storeCollider); s->proxy_go = -1; }
static void t2e_enter(act_inst *a) { ST(st_t2e); s->proxy_go = f->go; proxy_add(w, f->go, 3 + s->trigger->i, a); }
static void t2e_exit(act_inst *a) { ST(st_t2e); if (s->proxy_go >= 0) proxy_remove(w, s->proxy_go, 3 + s->trigger->i, a); }
/* cite: analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/Trigger2dEvent.cs:86-111 */
static void t2e_proxy(act_inst *a, int kind, int32_t other_go, int32_t other_layer)
{
    (void)other_layer;
    ST(st_t2e);
    if (kind != 3 + s->trigger->i) return;
    const char *tag = w_str(w, ps(f, s->collideTag));
    int32_t other_tag = go_tag(w, other_go);
    bool match = p_isnone(s->collideTag) || tag[0] == 0 || strcmp(tag, "Untagged") == 0;
    if (!match) {
        if (other_tag < 0) HKSIM_UNKNOWN("tag of '%s' not dumped (Trigger2dEvent collideTag='%s')", go_path(w, other_go), tag);
        match = strcmp(w_str(w, other_tag), tag) == 0;
    }
    if (match) { pgo_set(f, s->storeCollider, other_go); fsm_event(f, EV(s->sendEvent)); }
}
static const act_vtable AV_Trigger2dEvent = { "Trigger2dEvent", sizeof(st_t2e), t2e_bind, t2e_enter, NULL, NULL, NULL, t2e_exit, NULL, t2e_proxy };

/* Trigger2dEventLayer - ACT/Trigger2dEventLayer.cs:64-88.  Identical to Trigger2dEvent except that the
 * handler also requires `collisionInfo.gameObject.layer == collideLayer.Value` unless collideLayer
 * IsNone, so it reuses that action's state, binder, enter and exit and only re-implements the test. */
static void t2el_proxy(act_inst *a, int kind, int32_t other_go, int32_t other_layer)
{
    ST(st_t2e);
    if (kind != 3 + s->trigger->i) return;
    if (!p_isnone(s->collideLayer) && other_layer != pi(f, s->collideLayer)) return;   /* :67 the layer conjunct */
    const char *tag = w_str(w, ps(f, s->collideTag));
    int32_t other_tag = go_tag(w, other_go);
    bool match = p_isnone(s->collideTag) || tag[0] == 0 || strcmp(tag, "Untagged") == 0;
    if (!match) {
        if (other_tag < 0) HKSIM_UNKNOWN("tag of '%s' not dumped (Trigger2dEventLayer collideTag='%s')", go_path(w, other_go), tag);
        match = strcmp(w_str(w, other_tag), tag) == 0;
    }
    if (match) { pgo_set(f, s->storeCollider, other_go); fsm_event(f, EV(s->sendEvent)); }
}
static const act_vtable AV_Trigger2dEventLayer = { "Trigger2dEventLayer", sizeof(st_t2e), t2e_bind, t2e_enter, NULL, NULL, NULL, t2e_exit, NULL, t2el_proxy };

/* SetCollider — ACT/SetCollider.cs:21-32 (first BoxCollider2D; target dereferenced unchecked) */
typedef struct { const fsm_pv *go, *v; } st_setcol;
static void setcol_bind(act_inst *a) { ST(st_setcol); s->go = FIELD(gameObject); s->v = FIELD(active); }
static void setcol_enter(act_inst *a)
{
    ST(st_setcol);
    int32_t t = p_owner_default(a, s->go);
    HKSIM_ASSERT(t >= 0, "SetCollider: null target (NullReferenceException in C#) in %s", fsm_label(f));
    col_inst *c = go_box_collider(w, t);
    if (c) col_set_enabled(w, c, pb(f, s->v));
    act_finish(a);
}
static const act_vtable AV_SetCollider = { "SetCollider", sizeof(st_setcol), setcol_bind, setcol_enter, NULL, NULL, NULL, NULL, NULL, NULL };
/* SetPolygonCollider - ACT/SetPolygonCollider.cs:21-32 (first PolygonCollider2D; :31 Finish() runs whether or
 * not the object has one).  Drives the Great Slash / Dash Slash hit colliders. */
static void spc_bind(act_inst *a) { ST(st_setcol); s->go = FIELD(gameObject); s->v = FIELD(active); }
static void spc_enter(act_inst *a)
{
    ST(st_setcol);
    int32_t t = p_owner_default(a, s->go);
    HKSIM_ASSERT(t >= 0, "SetPolygonCollider: null target (NullReferenceException in C#) in %s", fsm_label(f));
    col_inst *c = go_poly_collider(w, t);          /* :25 GetComponent<PolygonCollider2D>() */
    if (c) col_set_enabled(w, c, pb(f, s->v));     /* :26-29 */
    act_finish(a);
}
static const act_vtable AV_SetPolygonCollider = { "SetPolygonCollider", sizeof(st_setcol), spc_bind, spc_enter, NULL, NULL, NULL, NULL, NULL, NULL };
/* SetBoxColliderTrigger — ACT/SetBoxColliderTrigger.cs:21-32 */
static void sbct_bind(act_inst *a) { ST(st_setcol); s->go = FIELD(gameObject); s->v = FIELD(trigger); }
static void sbct_enter(act_inst *a)
{
    ST(st_setcol);
    int32_t t = p_owner_default(a, s->go);
    HKSIM_ASSERT(t >= 0, "SetBoxColliderTrigger: null target in %s", fsm_label(f));
    col_inst *c = go_box_collider(w, t);
    if (c) col_set_trigger(w, c, pb(f, s->v));
    act_finish(a);
}
static const act_vtable AV_SetBoxColliderTrigger = { "SetBoxColliderTrigger", sizeof(st_setcol), sbct_bind, sbct_enter, NULL, NULL, NULL, NULL, NULL, NULL };
/* SetBoxCollider2DSizeVector — ACT/SetBoxCollider2DSizeVector.cs:28-45 */
typedef struct { const fsm_pv *go, *size, *offset; } st_sbcsv;
static void sbcsv_bind(act_inst *a) { ST(st_sbcsv); s->go = FIELD(gameObject1); s->size = FIELD(size); s->offset = FIELD(offset); }
static void sbcsv_enter(act_inst *a)
{
    ST(st_sbcsv);
    int32_t t = p_owner_default(a, s->go);
    HKSIM_ASSERT(t >= 0, "SetBoxCollider2DSizeVector: null target in %s", fsm_label(f));
    col_inst *c = go_box_collider(w, t);
    HKSIM_ASSERT(c != NULL, "SetBoxCollider2DSizeVector: no BoxCollider2D on %s", go_path(w, t));
    col_set_box(w, t, c, p_isnone(s->size) ? NULL : pv3(f, s->size), p_isnone(s->offset) ? NULL : pv3(f, s->offset));
    act_finish(a);
}
static const act_vtable AV_SetBoxCollider2DSizeVector = { "SetBoxCollider2DSizeVector", sizeof(st_sbcsv), sbcsv_bind, sbcsv_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* SetAngularVelocity2d — ACT/SetAngularVelocity2d.cs:33-58: OnEnter caches the owner's Rigidbody2D
 * (RigidBody2dActionBase.CacheRigidBody2d) and writes rb2d.angularVelocity unless it is None; OnFixedUpdate writes
 * through the same cache; both Finish unless everyFrame. */
typedef struct { const fsm_pv *go, *angularVelocity, *everyFrame; int32_t rb_go; } st_sav;
static void sav_bind(act_inst *a) { ST(st_sav); s->go = FIELD(gameObject); s->angularVelocity = FIELD(angularVelocity); s->everyFrame = FIELD(everyFrame); s->rb_go = -1; }
static void sav_do(act_inst *a)
{
    ST(st_sav);
    if (s->rb_go < 0 || p_isnone(s->angularVelocity)) return;   /* :54 rb2d == null || IsNone */
    go_set_angular_velocity(w, s->rb_go, pf(f, s->angularVelocity));
}
static void sav_enter(act_inst *a)
{
    ST(st_sav);
    int32_t t = p_owner_default(a, s->go);
    if (t >= 0) s->rb_go = go_has_rb(w, t) ? t : -1;   /* RigidBody2dActionBase.cs:9-18: a null target keeps the cache */
    sav_do(a);
    if (!pb(f, s->everyFrame)) act_finish(a);
}
static void sav_fixed(act_inst *a) { ST(st_sav); sav_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_SetAngularVelocity2d = { "SetAngularVelocity2d", sizeof(st_sav), sav_bind, sav_enter, NULL, sav_fixed, NULL, NULL, NULL, NULL };

/* AddTorque2d — ACT/AddTorque2d.cs:35-56: rigidbody2d.AddTorque(torque, forceMode) on the owner's Rigidbody2D
 * (ComponentAction.UpdateCache) from OnEnter, and from OnFixedUpdate while everyFrame keeps the action running.
 * ForceMode2D: Force 0, Impulse 1. */
typedef struct { const fsm_pv *go, *forceMode, *torque, *everyFrame; int32_t cached_go; bool has; } st_at2;
static void at2_bind(act_inst *a) { ST(st_at2); s->go = FIELD(gameObject); s->forceMode = FIELD_OPT(forceMode); s->torque = FIELD(torque); s->everyFrame = FIELD(everyFrame); s->cached_go = -1; }
static void at2_do(act_inst *a)
{
    ST(st_at2);
    int32_t t = p_owner_default(a, s->go);
    if (!act_cache_rb(a, t, &s->cached_go, &s->has)) return;
    int mode = s->forceMode ? pi(f, s->forceMode) : 0;
    if (mode != 0 && mode != 1) HKSIM_UNIMPLEMENTED("AddTorque2d forceMode=%d in %s", mode, fsm_label(f));
    go_add_torque(w, t, pf(f, s->torque), mode == 1);
}
static void at2_enter(act_inst *a) { ST(st_at2); at2_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_AddTorque2d = { "AddTorque2d", sizeof(st_at2), at2_bind, at2_enter, NULL, at2_do, NULL, NULL, NULL, NULL };

/* SetCircleCollider — analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/SetCircleCollider.cs:22-35 */
typedef struct { const fsm_pv *gameObject, *active; } st_scc;
static void scc_bind(act_inst *a) { ST(st_scc); s->gameObject = FIELD(gameObject); s->active = FIELD(active); }
static void scc_enter(act_inst *a) {
    ST(st_scc);
    int32_t t = p_owner_default(a, s->gameObject);
    col_inst *c = t >= 0 ? go_first_collider(w, t) : NULL;
    if (c != NULL) col_set_enabled(w, c, pb(f, s->active));
    act_finish(a);
}
static const act_vtable AV_SetCircleCollider = { "SetCircleCollider", sizeof(st_scc), scc_bind, scc_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* Collision2dEventLayer — analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/Collision2dEventLayer.cs:58-154 */
typedef struct { const fsm_pv *collision, *collideTag, *collideLayer, *sendEvent, *storeCollider, *storeForce; int32_t proxy_go; } st_c2el;
static void c2el_bind(act_inst *a) {
    ST(st_c2el);
    s->collision = FIELD(collision);
    s->collideTag = FIELD(collideTag);
    s->collideLayer = FIELD(collideLayer);
    s->sendEvent = FIELD_OPT(sendEvent);
    s->storeCollider = FIELD_OPT(storeCollider);
    s->storeForce = FIELD_OPT(storeForce);
    s->proxy_go = -1;
}
static void c2el_enter(act_inst *a) {
    ST(st_c2el);
    if (pi(f, s->collision) > 2) HKSIM_UNIMPLEMENTED("Collision2dEventLayer non-collision in %s", fsm_label(f));
    s->proxy_go = f->go;
    proxy_add(w, f->go, pi(f, s->collision), a);
}
static void c2el_exit(act_inst *a) { ST(st_c2el); if (s->proxy_go >= 0) proxy_remove(w, s->proxy_go, pi(f, s->collision), a); }
static void c2el_proxy(act_inst *a, int kind, int32_t other_go, int32_t other_layer) {
    ST(st_c2el);
    if (kind != pi(f, s->collision)) return;
    bool tag_match = collide_tag_matches(a, s->collideTag, other_go);
    bool layer_match = p_isnone(s->collideLayer) || other_layer == pi(f, s->collideLayer);
    if (tag_match && layer_match) {
        if (s->storeCollider) pgo_set(f, s->storeCollider, other_go);
        if (s->storeForce && !p_isnone(s->storeForce)) HKSIM_UNKNOWN("Collision2dEventLayer.storeForce (relativeVelocity.magnitude) needs the contact velocity in %s", fsm_label(f));
        if (s->sendEvent) fsm_event(f, EV(s->sendEvent));
    }
}
static const act_vtable AV_Collision2dEventLayer = { "Collision2dEventLayer", sizeof(st_c2el), c2el_bind, c2el_enter, NULL, NULL, NULL, c2el_exit, NULL, c2el_proxy };

/* SendTrigger2DEventByName — analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/SendTrigger2DEventByName.cs:42-126 */
typedef struct { const fsm_pv *eventTarget, *trigger, *collideTag, *collideLayer, *sendEvent, *storeCollider; int32_t proxy_go; } st_st2ebn;
static void st2ebn_bind(act_inst *a) {
    ST(st_st2ebn);
    s->eventTarget = FIELD(eventTarget);
    s->trigger = FIELD(trigger);
    s->collideTag = FIELD(collideTag);
    s->collideLayer = FIELD(collideLayer);
    s->sendEvent = FIELD(sendEvent);
    s->storeCollider = FIELD_OPT(storeCollider);
    s->proxy_go = -1;
}
static void st2ebn_enter(act_inst *a) {
    ST(st_st2ebn);
    int32_t trig = pi(f, s->trigger);
    if (trig < 0 || trig > 2) HKSIM_UNIMPLEMENTED("SendTrigger2DEventByName invalid trigger %d in %s", trig, fsm_label(f));
    s->proxy_go = f->go;
    proxy_add(w, f->go, trig + 3, a);
}
static void st2ebn_exit(act_inst *a) {
    ST(st_st2ebn);
    if (s->proxy_go >= 0) proxy_remove(w, s->proxy_go, pi(f, s->trigger) + 3, a);
}
static void st2ebn_proxy(act_inst *a, int kind, int32_t other_go, int32_t other_layer) {
    ST(st_st2ebn);
    if (kind != pi(f, s->trigger) + 3) return;
    bool tag_match = collide_tag_matches(a, s->collideTag, other_go);
    bool layer_match = p_isnone(s->collideLayer) || other_layer == pi(f, s->collideLayer);
    if (tag_match && layer_match) {
        if (s->storeCollider) pgo_set(f, s->storeCollider, other_go);
        int32_t target_idx = act_event_target(a, s->eventTarget);
        int32_t ev = w_get_fsm_event(w, w_str(w, ps(f, s->sendEvent)));
        if (ev >= 0) fsm_event_to(f, a, target_idx, ev);
    }
}
static const act_vtable AV_SendTrigger2DEventByName = { "SendTrigger2DEventByName", sizeof(st_st2ebn), st2ebn_bind, st2ebn_enter, NULL, NULL, NULL, st2ebn_exit, NULL, st2ebn_proxy };

/* FlingObject — analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/FlingObject.cs:34-51 */
typedef struct { const fsm_pv *flungObject, *speedMin, *speedMax, *angleMin, *angleMax; } st_fling;
static void fling_bind(act_inst *a) { ST(st_fling); s->flungObject = FIELD(flungObject); s->speedMin = FIELD(speedMin); s->speedMax = FIELD(speedMax); s->angleMin = FIELD(angleMin); s->angleMax = FIELD(angleMax); }
static void fling_enter(act_inst *a) {
    ST(st_fling);
    int32_t t = p_owner_default(a, s->flungObject);
    if (t >= 0 && go_has_rb(w, t)) {                                /* :38-48; the velocity needs a Rigidbody2D */
        float num = hk_rng_range_f_site(w->rng, a->rng_site, pf(f, s->speedMin), pf(f, s->speedMax));
        float num2 = hk_rng_range_f_site(w->rng, a->rng_site, pf(f, s->angleMin), pf(f, s->angleMax));
        float vectorX = num * m_cos(num2 * ((float)M_PI / 180.0f));
        float vectorY = num * m_sin(num2 * ((float)M_PI / 180.0f));
        float v[2] = { vectorX, vectorY };
        go_set_velocity(w, t, v);                                  /* :47 rb2d.velocity */
    }
    act_finish(a);
}
static const act_vtable AV_FlingObject = { "FlingObject", sizeof(st_fling), fling_bind, fling_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* FireAtTarget — analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/FireAtTarget.cs:51-87 */
typedef struct { const fsm_pv *gameObject, *target, *speed, *position, *spread, *everyFrame; int32_t self; } st_fat;
static void fat_bind(act_inst *a) {
    ST(st_fat);
    s->gameObject = FIELD(gameObject);
    s->target = FIELD(target);
    s->speed = FIELD(speed);
    s->position = FIELD_OPT(position);
    s->spread = FIELD_OPT(spread);
    s->everyFrame = FIELD_OPT(everyFrame);
}
static void fat_do(act_inst *a) {
    ST(st_fat);
    int32_t t = pgo(f, s->target);
    if (s->self >= 0 && go_has_rb(w, s->self) && t >= 0) {
        float t_pos[3], s_pos[3], p_off[3] = { 0, 0, 0 };
        go_world_pos(w, t, t_pos);
        go_world_pos(w, s->self, s_pos);
        if (s->position && !p_isnone(s->position)) {
            const float *pv = pv3(f, s->position);
            p_off[0] = pv[0]; p_off[1] = pv[1]; p_off[2] = pv[2];
        }
        /* :77-78 on the Mono double stack: one rounding per float local (docs/float-parity.md) */
        float num = (float)((double)t_pos[1] + (double)p_off[1] - (double)s_pos[1]);
        float num2 = (float)((double)t_pos[0] + (double)p_off[0] - (double)s_pos[0]);
        float num3 = m_atan2(num, num2) * (180.0f / (float)M_PI);
        if (s->spread && !p_isnone(s->spread)) {
            float sp = pf(f, s->spread);
            num3 += hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - sp, sp);
        }
        float vx = pf(f, s->speed) * m_cos(num3 * ((float)M_PI / 180.0f));
        float vy = pf(f, s->speed) * m_sin(num3 * ((float)M_PI / 180.0f));
        float v[2] = { vx, vy };
        go_set_velocity(w, s->self, v);
    }
}
static void fat_enter(act_inst *a) {
    ST(st_fat);
    s->self = p_owner_default(a, s->gameObject);
    fat_do(a);
    if (!s->everyFrame || !pb(f, s->everyFrame)) act_finish(a);
}
static void fat_fixed(act_inst *a) {
    ST(st_fat);
    fat_do(a);
    if (!s->everyFrame || !pb(f, s->everyFrame)) act_finish(a);
}
static const act_vtable AV_FireAtTarget = { "FireAtTarget", sizeof(st_fat), fat_bind, fat_enter, NULL, fat_fixed, NULL, NULL, NULL, NULL };

/* Decelerate — ACT/Decelerate.cs:31-82 on RigidBody2dActionBase.  OnEnter caches the body and runs once,
 * then OnFixedUpdate every fixed step (Fsm.HandleFixedUpdate is forced true in Awake/OnPreprocess).
 * Both components move toward zero by `deceleration`; each branch clamps at zero rather than past it. */
typedef struct { const fsm_pv *gameObject, *deceleration; } st_dec;
static void dec_bind(act_inst *a) { ST(st_dec); s->gameObject = FIELD(gameObject); s->deceleration = FIELD(deceleration); }
static void dec_do(act_inst *a)
{
    ST(st_dec);
    int32_t t = p_owner_default(a, s->gameObject);
    if (t < 0 || !go_has_rb(w, t)) return;                          /* :32-35 rb2d == null */
    float v[2]; go_velocity(w, t, v);
    float d = pf(f, s->deceleration);
    for (int k = 0; k < 2; k++) {                                     /* :49-64 x, :65-80 y: the same two branches */
        if (v[k] < 0.0f) { v[k] += d; if (v[k] > 0.0f) v[k] = 0.0f; }
        else if (v[k] > 0.0f) { v[k] -= d; if (v[k] < 0.0f) v[k] = 0.0f; }
    }
    go_set_velocity(w, t, v);
}
static void dec_enter(act_inst *a) { dec_do(a); }                   /* :20-24 no Finish: runs until the state exits */
static const act_vtable AV_Decelerate = { "Decelerate", sizeof(st_dec), dec_bind, dec_enter, NULL, dec_do, NULL, NULL, NULL, NULL };

/* AccelerateVelocity — ACT/AccelerateVelocity.cs:40-65.  OnEnter only caches the body; the work is in
 * OnFixedUpdate.  Each axis is optional (IsNone) and clamped to +-max for that axis. */
typedef struct { const fsm_pv *gameObject, *xAccel, *yAccel, *xMaxSpeed, *yMaxSpeed; } st_accv;
static void accv_bind(act_inst *a) { ST(st_accv); s->gameObject = FIELD(gameObject); s->xAccel = FIELD(xAccel); s->yAccel = FIELD(yAccel); s->xMaxSpeed = FIELD(xMaxSpeed); s->yMaxSpeed = FIELD(yMaxSpeed); }
static void accv_fixed(act_inst *a)
{
    ST(st_accv);
    int32_t t = p_owner_default(a, s->gameObject);
    if (t < 0 || !go_has_rb(w, t)) return;                          /* :48 rb2d == null */
    float v[2]; go_velocity(w, t, v);
    if (!p_isnone(s->xAccel)) {                                     /* :51-56 */
        float m = pf(f, s->xMaxSpeed), x = v[0] + pf(f, s->xAccel);
        v[0] = x < -m ? -m : (x > m ? m : x);
    }
    if (!p_isnone(s->yAccel)) {                                     /* :57-62 */
        float m = pf(f, s->yMaxSpeed), y = v[1] + pf(f, s->yAccel);
        v[1] = y < -m ? -m : (y > m ? m : y);
    }
    go_set_velocity(w, t, v);
}
static const act_vtable AV_AccelerateVelocity = { "AccelerateVelocity", sizeof(st_accv), accv_bind, NULL, NULL, accv_fixed, NULL, NULL, NULL, NULL };

/* BoundsBoxCollider — ACT/BoundsBoxCollider.cs:18-46: BoxCollider2D.bounds.size of the target.
 * col_bounds gives the same world AABB the obs packer uses. */
typedef struct { const fsm_pv *gameObject1, *scaleVector2, *scaleX, *scaleY, *everyFrame; } st_bbc;
static void bbc_bind(act_inst *a) { ST(st_bbc); s->gameObject1 = FIELD(gameObject1); s->scaleVector2 = a_field(a, "scaleVector2"); s->scaleX = a_field(a, "scaleX"); s->scaleY = a_field(a, "scaleY"); s->everyFrame = a_field(a, "everyFrame"); }
static void bbc_do(act_inst *a)
{
    ST(st_bbc);
    int32_t t = p_owner_default(a, s->gameObject1);
    if (t < 0) HKSIM_ASSERT(0, "BoundsBoxCollider: null target (NullReferenceException in C#) in %s", fsm_label(f));
    col_inst *c = go_box_collider(w, t);
    if (!c) HKSIM_ASSERT(0, "BoundsBoxCollider: '%s' has no BoxCollider2D (NRE in C#) in %s", go_path(w, t), fsm_label(f));
    float lo[2], hi[2]; col_bounds(w, t, c, lo, hi);
    float v[3] = { hi[0] - lo[0], hi[1] - lo[1], 0.0f };
    if (s->scaleVector2 && !p_isnone(s->scaleVector2)) pv3_set(f, s->scaleVector2, v);
    if (s->scaleX && !p_isnone(s->scaleX)) pf_set(f, s->scaleX, v[0]);
    if (s->scaleY && !p_isnone(s->scaleY)) pf_set(f, s->scaleY, v[1]);
}
static void bbc_enter(act_inst *a) { ST(st_bbc); bbc_do(a); if (!s->everyFrame || !pb(f, s->everyFrame)) act_finish(a); }
static void bbc_update(act_inst *a) { bbc_do(a); }
static const act_vtable AV_BoundsBoxCollider = { "BoundsBoxCollider", sizeof(st_bbc), bbc_bind, bbc_enter, bbc_update, NULL, NULL, NULL, NULL, NULL };

/* BoxColliderOffset - ACT/BoxColliderOffset.cs:32-47. */
typedef struct { const fsm_pv *gameObject1, *offsetVector2, *offsetX, *offsetY, *everyFrame; } st_bco;
static void bco_bind(act_inst *a) { ST(st_bco); s->gameObject1 = FIELD(gameObject1); s->offsetVector2 = FIELD(offsetVector2); s->offsetX = FIELD(offsetX); s->offsetY = FIELD(offsetY); s->everyFrame = FIELD(everyFrame); }
static void bco_do(act_inst *a) {
    ST(st_bco);
    int32_t go = p_owner_default(a, s->gameObject1);
    if (go >= 0) {
        col_inst *c = go_box_collider(w, go);
        if (c != NULL) {
            float off[3] = { c->offset[0], c->offset[1], 0.0f };
            if (!p_isnone(s->offsetVector2)) pv3_set(f, s->offsetVector2, off);
            if (!p_isnone(s->offsetX)) pf_set(f, s->offsetX, off[0]);
            if (!p_isnone(s->offsetY)) pf_set(f, s->offsetY, off[1]);
        }
    }
}
static void bco_enter(act_inst *a) { ST(st_bco); bco_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_BoxColliderOffset = { "BoxColliderOffset", sizeof(st_bco), bco_bind, bco_enter, bco_do, NULL, NULL, NULL, NULL, NULL };

/* ReceivedDamage - ACT/ReceivedDamage.cs: a collision or trigger with an object whose FSM has damageDealt > 0 */
typedef struct { const fsm_pv *collideTag, *sendEvent, *fsmName, *storeGameObject, *ignoreAcid, *ignoreWater; int32_t proxy_go; } st_rdam;
static void rdam_bind(act_inst *a) { ST(st_rdam); s->collideTag = FIELD(collideTag); s->sendEvent = FIELD(sendEvent); s->fsmName = FIELD(fsmName); s->storeGameObject = FIELD(storeGameObject); s->ignoreAcid = FIELD(ignoreAcid); s->ignoreWater = FIELD(ignoreWater); s->proxy_go = -1; }
static void rdam_enter(act_inst *a) {
    ST(st_rdam);
    s->proxy_go = f->go;
    proxy_add(w, f->go, 0, a); proxy_add(w, f->go, 3, a); proxy_add(w, f->go, 4, a);
}
static void rdam_exit(act_inst *a) {
    ST(st_rdam);
    if (s->proxy_go >= 0) { proxy_remove(w, s->proxy_go, 0, a); proxy_remove(w, s->proxy_go, 3, a); proxy_remove(w, s->proxy_go, 4, a); }
}
static void rdam_proxy(act_inst *a, int kind, int32_t other_go, int32_t other_layer) {
    (void)other_layer;
    ST(st_rdam);
    if (kind != 0 && kind != 3 && kind != 4) return;
    const char *tag = w_str(w, ps(f, s->collideTag));
    bool match = p_isnone(s->collideTag) || tag[0] == 0 || strcmp(tag, "Untagged") == 0;
    int32_t ot = go_tag(w, other_go);
    if (!match) { match = ot >= 0 && strcmp(w_str(w, ot), tag) == 0; }
    if (!match) return;
    if (ot >= 0) {
        const char *ot_str = w_str(w, ot);
        if (pb(f, s->ignoreAcid) && strcmp(ot_str, "Acid") == 0) return;
        if (pb(f, s->ignoreWater) && strcmp(ot_str, "Water Surface") == 0) return;
    }
    int32_t other_fsm = get_game_object_fsm(w, other_go, w_str(w, ps(f, s->fsmName)));
    if (other_fsm >= 0) {
        bool fresh;
        fsm_val *v = fsm_get_var(&w->fsms[other_fsm], VB_INT, w_intern(w, "damageDealt"), &fresh);
        if (!fresh && v->i > 0) {
            pgo_set(f, s->storeGameObject, other_go);
            if (EV(s->sendEvent) >= 0) fsm_event(f, EV(s->sendEvent));
        }
    }
}
static const act_vtable AV_ReceivedDamage = { "ReceivedDamage", sizeof(st_rdam), rdam_bind, rdam_enter, NULL, NULL, NULL, rdam_exit, NULL, rdam_proxy };

/* SendTrigger2DEvent -- ACT/SendTrigger2DEvent.cs:29-120: a PlayMakerUnity2DProxy trigger delegate like
 * Trigger2dEvent's, filtered by tag (unless IsNone or empty) and layer (unless IsNone), sent to eventTarget */
typedef struct { const fsm_pv *eventTarget, *trigger, *collideTag, *collideLayer, *sendEvent, *storeCollider; int32_t proxy_go; } st_st2e;
static void st2e_bind(act_inst *a)
{
    ST(st_st2e);
    s->eventTarget = FIELD(eventTarget); s->trigger = FIELD(trigger); s->collideTag = FIELD(collideTag);
    s->collideLayer = FIELD(collideLayer); s->sendEvent = FIELD(sendEvent); s->storeCollider = FIELD(storeCollider);
    s->proxy_go = -1;
}
static void st2e_enter(act_inst *a) { ST(st_st2e); s->proxy_go = f->go; proxy_add(w, f->go, 3 + s->trigger->i, a); }
static void st2e_exit(act_inst *a) { ST(st_st2e); if (s->proxy_go >= 0) proxy_remove(w, s->proxy_go, 3 + s->trigger->i, a); }
static void st2e_proxy(act_inst *a, int kind, int32_t other_go, int32_t other_layer)
{
    ST(st_st2e);
    if (kind != 3 + s->trigger->i) return;
    const char *tag = p_isnone(s->collideTag) ? "" : w_str(w, ps(f, s->collideTag));
    if (tag[0]) {
        int32_t other_tag = go_tag(w, other_go);
        if (other_tag < 0) HKSIM_UNKNOWN("tag of '%s' not dumped (SendTrigger2DEvent collideTag='%s')", go_path(w, other_go), tag);
        if (strcmp(w_str(w, other_tag), tag) != 0) return;
    }
    if (!p_isnone(s->collideLayer) && other_layer != pi(f, s->collideLayer)) return;
    pgo_set(f, s->storeCollider, other_go);                         /* :91 StoreCollisionInfo */
    fsm_event_to(f, a, act_event_target(a, s->eventTarget), EV(s->sendEvent));
}
static const act_vtable AV_SendTrigger2DEvent = { "SendTrigger2DEvent", sizeof(st_st2e), st2e_bind, st2e_enter, NULL, NULL, NULL, st2e_exit, NULL, st2e_proxy };

/* AddForce2dV2 — ACT/AddForce2dV2.cs:66-147: AddForce2d (above) plus atPosition (torque, unported: no dumped
 * use sets it), vector3 (z ignored) and a post-force per-axis / magnitude speed clamp.  maxSpeed(X/Y) being
 * "not None" in the C# is a null FsmFloat check (`!= null`), not FsmFloat.IsNone (compare FIELD_OPT's NULL
 * for an absent field, like every other RigidBody2dActionBase clamp in this file). */
typedef struct { const fsm_pv *go, *atPosition, *vector, *x, *y, *vector3, *maxSpeed, *maxSpeedX, *maxSpeedY, *everyFrame; int32_t cached_go; bool has; } st_af2v2;
static void af2v2_bind(act_inst *a)
{
    ST(st_af2v2);
    s->go = FIELD(gameObject); s->atPosition = FIELD_OPT(atPosition); s->vector = FIELD_OPT(vector);
    s->x = FIELD_OPT(x); s->y = FIELD_OPT(y); s->vector3 = FIELD_OPT(vector3);
    s->maxSpeed = FIELD_OPT(maxSpeed); s->maxSpeedX = FIELD_OPT(maxSpeedX); s->maxSpeedY = FIELD_OPT(maxSpeedY);
    s->everyFrame = FIELD(everyFrame); s->cached_go = -1;
}
static void af2v2_do(act_inst *a)
{
    ST(st_af2v2);
    int32_t t = p_owner_default(a, s->go);
    if (!act_cache_rb(a, t, &s->cached_go, &s->has)) return;
    if (s->atPosition && !p_isnone(s->atPosition))
        HKSIM_UNIMPLEMENTED("AddForce2dV2 atPosition set (AddForceAtPosition adds torque) in %s", fsm_label(f));
    float fx = 0.0f, fy = 0.0f;
    if (s->vector && !p_isnone(s->vector)) { const float *vv = pv3(f, s->vector); fx = vv[0]; fy = vv[1]; }
    else { fx = s->x ? pf(f, s->x) : 0.0f; fy = s->y ? pf(f, s->y) : 0.0f; }
    if (s->vector3 && !p_isnone(s->vector3)) { const float *v3 = pv3(f, s->vector3); fx = v3[0]; fy = v3[1]; }
    if (s->x && !p_isnone(s->x)) fx = pf(f, s->x);
    if (s->y && !p_isnone(s->y)) fy = pf(f, s->y);
    float fv[2] = { fx, fy };
    go_add_force(w, t, fv);
    if (s->maxSpeedX) {
        float v[2]; go_velocity(w, t, v);
        float m = pf(f, s->maxSpeedX);
        if (v[0] > m) v[0] = m;
        if (v[0] < -m) v[0] = -m;
        go_set_velocity(w, t, v);
    }
    if (s->maxSpeedY) {
        float v[2]; go_velocity(w, t, v);
        float m = pf(f, s->maxSpeedY);
        if (v[1] > m) v[1] = m;
        if (v[1] < -m) v[1] = -m;
        go_set_velocity(w, t, v);
    }
    if (s->maxSpeed) {
        float v[2]; go_velocity(w, t, v);
        float max_len = pf(f, s->maxSpeed);
        float sq = (float)((double)v[0] * (double)v[0] + (double)v[1] * (double)v[1]);   /* Vector2.ClampMagnitude,
         * movement.c clamp_magnitude2's citation: sqrMagnitude rounded to float, test widens max_len*max_len,
         * magnitude/normalize on the double stack */
        if ((double)sq > (double)max_len * (double)max_len) {
            float mag = (float)sqrt((double)sq);
            float nx = (float)((double)v[0] / (double)mag), ny = (float)((double)v[1] / (double)mag);
            v[0] = (float)((double)nx * (double)max_len);
            v[1] = (float)((double)ny * (double)max_len);
        }
        go_set_velocity(w, t, v);
    }
}
static void af2v2_enter(act_inst *a) { ST(st_af2v2); af2v2_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_AddForce2dV2 = { "AddForce2dV2", sizeof(st_af2v2), af2v2_bind, af2v2_enter, NULL, af2v2_do, NULL, NULL, NULL, NULL };

/* GetSpeed — ACT/GetSpeed.cs:29-54: ComponentAction<Rigidbody> (the 3D component; Rigidbody2D is a distinct
 * type UpdateCache never finds).  Its one dumped use, GG_Hollow_Knight `Battle Scene/HK Prime/Control`, is on
 * `HK Prime`, whose components are all 2D (UnityEngine.Rigidbody2D, no UnityEngine.Rigidbody:
 * analysis/dumps_all/GG_Hollow_Knight/hierarchy.json.gz), so UpdateCache always returns false and storeResult
 * is never written -- a permanent no-op this port keeps as one, since no 3D Rigidbody is modelled at all. */
static const act_vtable AV_GetSpeed = { "GetSpeed", 0, NULL, act_finish_unless_every_frame, NULL, NULL, NULL, NULL, NULL, NULL };

/* GetVelocityAsAngle — ACT/GetVelocityAsAngle.cs:25-52: Mathf.Atan2(vx, -vy) in degrees minus 90, wrapped into
 * [0, 360).  storeAngle is dereferenced unconditionally when rb2d != null (no IsNone guard in the C# either),
 * matching CrashTrap style FIELD elsewhere in this file (not FIELD_OPT). */
typedef struct { const fsm_pv *go, *storeAngle, *everyFrame; int32_t cached_go; bool has; } st_gvaa;
static void gvaa_bind(act_inst *a) { ST(st_gvaa); s->go = FIELD(gameObject); s->storeAngle = FIELD(storeAngle); s->everyFrame = FIELD(everyFrame); s->cached_go = -1; }
static void gvaa_do(act_inst *a)
{
    ST(st_gvaa);
    int32_t t = p_owner_default(a, s->go);
    if (!act_cache_rb(a, t, &s->cached_go, &s->has)) return;
    float v[2]; go_velocity(w, t, v);
    float num = (float)(atan2((double)v[0], (double)(0.0f - v[1])) * (180.0 / M_PI)) - 90.0f;
    if (num < 0.0f) num += 360.0f;
    pf_set(f, s->storeAngle, num);
}
static void gvaa_enter(act_inst *a) { ST(st_gvaa); gvaa_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GetVelocityAsAngle = { "GetVelocityAsAngle", sizeof(st_gvaa), gvaa_bind, gvaa_enter, gvaa_do, NULL, NULL, NULL, NULL, NULL };

/* SetCollider2dIsTrigger — ACT/SetCollider2dIsTrigger.cs:21-53: null-owner guarded (returns, no Finish() skip:
 * Finish() itself always runs at :31 regardless), then either every Collider2D in component order
 * (setAllColliders, GetComponents<Collider2D>()) or just the first (go_first_collider). */
typedef struct { const fsm_pv *go, *isTrigger, *setAllColliders; } st_sc2it;
static void sc2it_bind(act_inst *a) { ST(st_sc2it); s->go = FIELD(gameObject); s->isTrigger = FIELD(isTrigger); s->setAllColliders = FIELD(setAllColliders); }
static void sc2it_enter(act_inst *a)
{
    ST(st_sc2it);
    int32_t t = p_owner_default(a, s->go);
    if (t >= 0) {
        bool v = pb(f, s->isTrigger);
        if (pb(f, s->setAllColliders)) {
            for (int32_t k = 0; k < w->gos[t].n_cols; k++) col_set_trigger(w, &w->gos[t].cols[k], v);
        } else {
            col_inst *c = go_first_collider(w, t);
            if (c) col_set_trigger(w, c, v);
        }
    }
    act_finish(a);
}
static const act_vtable AV_SetCollider2dIsTrigger = { "SetCollider2dIsTrigger", sizeof(st_sc2it), sc2it_bind, sc2it_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* SetRigidbodySimulated2D — HK/SetRigidbodySimulated2D.cs:19-36: null-owner and null-component guarded. */
typedef struct { const fsm_pv *go, *isSimulated; } st_srs2d;
static void srs2d_bind(act_inst *a) { ST(st_srs2d); s->go = FIELD(gameObject); s->isSimulated = FIELD(isSimulated); }
static void srs2d_enter(act_inst *a)
{
    ST(st_srs2d);
    int32_t t = p_owner_default(a, s->go);
    if (t >= 0 && go_has_rb(w, t)) go_set_simulated(w, t, pb(f, s->isSimulated));
    act_finish(a);
}
static const act_vtable AV_SetRigidbodySimulated2D = { "SetRigidbodySimulated2D", sizeof(st_srs2d), srs2d_bind, srs2d_enter, NULL, NULL, NULL, NULL, NULL, NULL };

const act_vtable *const act_registry_physics2d[] = {
    &AV_RayCast2d, &AV_RayCast2dV2, &AV_SetBoxCollider2DSize, &AV_SetInterpolate, &AV_SetExtrapolate,
    &AV_Collision2dEvent, &AV_AddForce2d, &AV_SetVelocity2d, &AV_GetVelocity2d, &AV_GetSpeed2d,
    &AV_SetVelocityAsAngle, &AV_SetGravity2dScale, &AV_SetIsKinematic2d, &AV_DecelerateV2, &AV_DecelerateXY,
    &AV_CheckCollisionSide, &AV_CheckCollisionSideEnter, &AV_Trigger2dEvent, &AV_Trigger2dEventLayer,
    &AV_SetCollider, &AV_SetPolygonCollider, &AV_SetBoxColliderTrigger, &AV_SetBoxCollider2DSizeVector,
    &AV_SetAngularVelocity2d, &AV_SetCircleCollider, &AV_Collision2dEventLayer, &AV_SendTrigger2DEventByName,
    &AV_FlingObject, &AV_AddTorque2d, &AV_FireAtTarget, &AV_Decelerate, &AV_AccelerateVelocity,
    &AV_BoundsBoxCollider, &AV_BoxColliderOffset, &AV_ReceivedDamage,
    &AV_SendTrigger2DEvent,
    &AV_AddForce2dV2, &AV_GetSpeed, &AV_GetVelocityAsAngle, &AV_SetCollider2dIsTrigger, &AV_SetRigidbodySimulated2D,
};
const int act_registry_physics2d_n = (int)(sizeof act_registry_physics2d / sizeof act_registry_physics2d[0]);
