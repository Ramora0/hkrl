/* Transform reads and writes: position, rotation, scale, parenting, facing. */
#include "act.h"

/* Rotate — ACT/Rotate.cs:97-145.  The pose model is 2D (z euler only), so a non-zero x/y rotation traps.
 * :139-143 `transform.Rotate(vector * Time.deltaTime, space)` when perSecond, else the raw vector; for a z-only
 * rotation Space.Self is an add to the local euler z and Space.World to the world euler z. */
typedef struct { const fsm_pv *gameObject, *vector, *xAngle, *yAngle, *zAngle, *space, *perSecond,
                        *everyFrame, *lateUpdate, *fixedUpdate; } st_rot;
static void rot_bind(act_inst *a)
{
    ST(st_rot);
    s->gameObject = FIELD(gameObject); s->vector = FIELD(vector);
    s->xAngle = FIELD(xAngle); s->yAngle = FIELD(yAngle); s->zAngle = FIELD(zAngle);
    s->space = FIELD(space); s->perSecond = FIELD(perSecond); s->everyFrame = FIELD(everyFrame);
    s->lateUpdate = FIELD(lateUpdate); s->fixedUpdate = FIELD(fixedUpdate);
}
static void rot_do(act_inst *a)
{
    ST(st_rot);
    int32_t t = p_owner_default(a, s->gameObject);
    if (t < 0) return;                                                /* :129 null target: nothing */
    /* :131 vector = IsNone ? (x,y,z) : vector, then each angle overrides its component if not IsNone */
    float v[3] = { 0.0f, 0.0f, 0.0f };
    if (!p_isnone(s->vector)) { const float *pv = pv3(f, s->vector); v[0] = pv[0]; v[1] = pv[1]; v[2] = pv[2]; }
    else { v[0] = pf(f, s->xAngle); v[1] = pf(f, s->yAngle); v[2] = pf(f, s->zAngle); }
    if (!p_isnone(s->xAngle)) v[0] = pf(f, s->xAngle);
    if (!p_isnone(s->yAngle)) v[1] = pf(f, s->yAngle);
    if (!p_isnone(s->zAngle)) v[2] = pf(f, s->zAngle);

    if (v[0] != 0.0f || v[1] != 0.0f)
        HKSIM_UNKNOWN("Rotate with a non-zero x/y angle (%g, %g) in %s: the pose model is 2D (z euler "
                      "only), so an x/y rotation cannot be represented", (double)v[0], (double)v[1], fsm_label(f));

    float dz = pb(f, s->perSecond) ? v[2] * w->dt : v[2];             /* :139-143 */
    if (pi(f, s->space) == 1) go_set_local_euler_z(w, t, go_local_euler_z(w, t) + dz);
    else                          go_set_euler_z(w, t, go_euler_z(w, t) + dz);
}
static void rot_enter(act_inst *a)
{
    ST(st_rot);
    /* :112-118 only acts on enter when it is a one-shot (no everyFrame / lateUpdate / fixedUpdate) */
    if (!pb(f, s->everyFrame) && !pb(f, s->lateUpdate) && !pb(f, s->fixedUpdate)) { rot_do(a); act_finish(a); }
}
static void rot_update(act_inst *a)
{
    ST(st_rot);
    if (!pb(f, s->lateUpdate) && !pb(f, s->fixedUpdate)) rot_do(a);   /* :120-126 */
}
static void rot_fixed(act_inst *a)
{
    ST(st_rot);
    if (pb(f, s->fixedUpdate)) rot_do(a);                             /* :136-145 */
    if (!pb(f, s->everyFrame)) act_finish(a);
}
static void rot_late(act_inst *a)
{
    ST(st_rot);
    if (pb(f, s->lateUpdate)) rot_do(a);                              /* :128-135 */
    if (!pb(f, s->everyFrame)) act_finish(a);
}
static const act_vtable AV_Rotate = { "Rotate", sizeof(st_rot), rot_bind, rot_enter, rot_update, rot_fixed, rot_late, NULL, NULL, NULL };

/* RotateTo - ACT/RotateTo.cs:16-40.  Spins local z toward targetAngle at `speed` deg/sec, choosing the
 * direction by the shorter way round, and clamps once it passes the target.
 *
 * `localEulerAngles` reads back in [0,360), so the comparison is done on the wrapped angle.  The clamp
 * branches in the decomp assign `new Vector3(transform.rotation.x, rotation.y, targetAngle)` -
 * QUATERNION components into an euler vector, which is a bug in the original but harmless here,
 * because the pose model is 2D and only z is represented. */
typedef struct { const fsm_pv *gameObject, *targetAngle, *speed; } st_rto;
static void rto_bind(act_inst *a) { ST(st_rto); s->gameObject = FIELD(gameObject); s->targetAngle = FIELD(targetAngle); s->speed = FIELD(speed); }
static float wrap360(float z) { z = (float)fmod((double)z, 360.0); if (z < 0.0f) z += 360.0f; return z; }
static void rto_update(act_inst *a)
{
    ST(st_rto);
    int32_t t = p_owner_default(a, s->gameObject);
    if (t < 0) return;
    float target = pf(f, s->targetAngle), speed = pf(f, s->speed);
    float cur = wrap360(go_local_euler_z(w, t));
    float num = target - cur;                                          /* :20 */
    int positive = (num < 0.0f) ? (num < -180.0f) : (!(num > 180.0f)); /* :21 verbatim */
    if (positive) {
        go_set_local_euler_z(w, t, go_local_euler_z(w, t) + speed * w->dt);
        if (wrap360(go_local_euler_z(w, t)) > target) go_set_local_euler_z(w, t, target);
    } else {
        go_set_local_euler_z(w, t, go_local_euler_z(w, t) - speed * w->dt);
        if (wrap360(go_local_euler_z(w, t)) < target) go_set_local_euler_z(w, t, target);
    }
}
static const act_vtable AV_RotateTo = { "RotateTo", sizeof(st_rto), rto_bind, NULL, rto_update, NULL, NULL, NULL, NULL, NULL };

/* GetXDistance — ACT/GetXDistance.cs:45-58.  |owner.x - target.x|, x axis only.  The decomp's own
 * guard is `gameObject != null && target.Value != null && storeResult != null`. */
typedef struct { const fsm_pv *gameObject, *target, *storeResult, *everyFrame; } st_gxd;
static void gxd_bind(act_inst *a)
{
    ST(st_gxd);
    s->gameObject = FIELD(gameObject); s->target = FIELD(target);
    s->storeResult = FIELD(storeResult); s->everyFrame = FIELD(everyFrame);
}
static void gxd_do(act_inst *a)
{
    ST(st_gxd);
    int32_t go = p_owner_default(a, s->gameObject), tgt = pgo(f, s->target);
    if (go < 0 || tgt < 0 || !s->storeResult) return;
    float p[3], q[3];
    go_world_pos(w, go, p); go_world_pos(w, tgt, q);
    float d = p[0] - q[0];
    if (d < 0.0f) d *= -1.0f;                                          /* :53-56 */
    pf_set(f, s->storeResult, d);
}
static void gxd_enter(act_inst *a) { ST(st_gxd); gxd_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GetXDistance = { "GetXDistance", sizeof(st_gxd), gxd_bind, gxd_enter, gxd_do, NULL, NULL, NULL, NULL, NULL };

/* ObjectJitter — ACT/ObjectJitter.cs:53-83 */
typedef struct { const fsm_pv *gameObject, *x, *y, *z, *allowMovement; float start[3]; } st_jitter;
static void jitter_bind(act_inst *a) { ST(st_jitter); s->gameObject = FIELD(gameObject); s->x = FIELD(x); s->y = FIELD(y); s->z = FIELD(z); s->allowMovement = FIELD(allowMovement); }
static void jitter_enter(act_inst *a) {
    ST(st_jitter);
    int32_t t = p_owner_default(a, s->gameObject);
    if (t >= 0) go_world_pos(w, t, s->start);
}
static void jitter_fixed(act_inst *a) {
    ST(st_jitter);
    int32_t t = p_owner_default(a, s->gameObject);
    if (t >= 0) {
        float dx = hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - pf(f, s->x), pf(f, s->x));
        float dy = hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - pf(f, s->y), pf(f, s->y));
        float dz = hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - pf(f, s->z), pf(f, s->z));
        
        if (pb(f, s->allowMovement)) {
            float p[3]; go_world_pos(w, t, p);
            float ang = go_euler_z(w, t) * ((float)M_PI / 180.0f);
            float c = m_cos(ang), sn = m_sin(ang);
            float rx = dx * c - dy * sn, ry = dx * sn + dy * c;
            p[0] += rx; p[1] += ry; p[2] += dz;
            go_set_world_pos(w, t, p);
        } else {
            float p[3] = { s->start[0] + dx, s->start[1] + dy, s->start[2] + dz };
            go_set_world_pos(w, t, p);
        }
    }
}
static const act_vtable AV_ObjectJitter = { "ObjectJitter", sizeof(st_jitter), jitter_bind, jitter_enter, NULL, jitter_fixed, NULL, NULL, NULL, NULL };

/* FaceDirection — ACT/FaceDirection.cs:43-134 */
typedef struct { const fsm_pv *gameObject, *spriteFacesRight, *playNewAnimation, *newAnimationClip, *everyFrame, *pauseBetweenTurns, *pauseTime; int32_t rb; int32_t sprite; float xScale, pauseTimer; } st_facedir;
static void facedir_bind(act_inst *a) {
    ST(st_facedir);
    s->gameObject = FIELD(gameObject); s->spriteFacesRight = FIELD(spriteFacesRight);
    s->playNewAnimation = a_field(a, "playNewAnimation"); s->newAnimationClip = a_field(a, "newAnimationClip");
    s->everyFrame = FIELD(everyFrame); s->pauseBetweenTurns = a_field(a, "pauseBetweenTurns"); s->pauseTime = a_field(a, "pauseTime");
    s->rb = -1;
}
/* one turn: flip localScale.x to `target` unless it is already there, restart the pause and optionally the clip */
static void facedir_turn(act_inst *a, float ls[3], float target)
{
    ST(st_facedir);
    if (ls[0] == target) return;
    s->pauseTimer = s->pauseTime && !p_isnone(s->pauseTime) ? pf(f, s->pauseTime) : 0.0f;
    ls[0] = target;
    anim_inst *sp = anim_deref(w, s->sprite);
    if (s->playNewAnimation && pb(f, s->playNewAnimation) && sp) {
        anim_play_name(w, sp, w_str(w, ps(f, s->newAnimationClip)));
        anim_play_from_frame(w, sp, 0);
    }
}
static void facedir_do(act_inst *a) {
    ST(st_facedir);
    if (s->rb < 0) return;
    float vel[2]; go_velocity(w, s->rb, vel);
    int32_t t = p_owner_default(a, s->gameObject);
    if (t < 0) return;
    float ls[3]; go_local_scale(w, t, ls);
    float x = vel[0];
    bool right = pb(f, s->spriteFacesRight);
    bool pause_turns = s->pauseBetweenTurns && pb(f, s->pauseBetweenTurns);
    if (s->pauseTimer <= 0.0f || !pause_turns) {
        if (x > 0.0f) facedir_turn(a, ls, right ? s->xScale : 0.0f - s->xScale);
        else if (x <= 0.0f) facedir_turn(a, ls, right ? 0.0f - s->xScale : s->xScale);
    } else {
        s->pauseTimer -= w->dt;
    }
    go_set_local_scale(w, t, ls);
}
static void facedir_enter(act_inst *a) {
    ST(st_facedir);
    int32_t t = p_owner_default(a, s->gameObject);
    s->rb = (t >= 0 && go_has_rb(w, t)) ? t : -1;
    s->sprite = anim_ref(w, t >= 0 ? anim_of_go(w, t) : NULL);
    if (t >= 0) {
        float ls[3]; go_local_scale(w, t, ls);
        s->xScale = ls[0];
        if (s->xScale < 0.0f) s->xScale *= -1.0f;
    }
    facedir_do(a);
    if (!pb(f, s->everyFrame)) act_finish(a);
}
static void facedir_update(act_inst *a) { ST(st_facedir); facedir_do(a); }
static const act_vtable AV_FaceDirection = { "FaceDirection", sizeof(st_facedir), facedir_bind, facedir_enter, facedir_update, NULL, NULL, NULL, NULL, NULL };

#define SPACE_WORLD 0   /* UnityEngine.Space.World = 0, Self = 1 (dump enum values) */

/* GetPosition — ACT/GetPosition.cs:39-64 */
typedef struct { const fsm_pv *go, *vector, *x, *y, *z, *space, *everyFrame; } st_getpos;
static void getpos_bind(act_inst *a) { ST(st_getpos); s->go = FIELD(gameObject); s->vector = FIELD(vector); s->x = FIELD(x); s->y = FIELD(y); s->z = FIELD(z); s->space = FIELD(space); s->everyFrame = FIELD(everyFrame); }
static void getpos_do(act_inst *a)
{
    ST(st_getpos);
    int32_t t = p_owner_default(a, s->go);
    if (t < 0) return;
    float v[4] = { 0, 0, 0, 0 };
    if (s->space->i == SPACE_WORLD) go_world_pos(w, t, v); else go_local_pos(w, t, v);
    pv3_set(f, s->vector, v);
    pf_set(f, s->x, v[0]); pf_set(f, s->y, v[1]); pf_set(f, s->z, v[2]);
}
static void getpos_enter(act_inst *a) { ST(st_getpos); getpos_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GetPosition = { "GetPosition", sizeof(st_getpos), getpos_bind, getpos_enter, getpos_do, NULL, NULL, NULL, NULL, NULL };

/* GetScale — ACT/GetScale.cs:39-64 */
typedef struct { const fsm_pv *go, *vector, *x, *y, *z, *space, *everyFrame; } st_getscale;
static void getscale_bind(act_inst *a) { ST(st_getscale); s->go = FIELD(gameObject); s->vector = FIELD(vector); s->x = FIELD(xScale); s->y = FIELD(yScale); s->z = FIELD(zScale); s->space = FIELD(space); s->everyFrame = FIELD(everyFrame); }
static void getscale_do(act_inst *a)
{
    ST(st_getscale);
    int32_t t = p_owner_default(a, s->go);
    if (t < 0) return;
    if (w->gos[t].fold_y180) HKSIM_UNIMPLEMENTED("GetScale of '%s', held folded after a y half turn (SetRotation)", go_path(w, t));
    float v[4] = { 0, 0, 0, 0 };
    if (s->space->i == SPACE_WORLD) go_lossy_scale(w, t, v); else go_local_scale(w, t, v);
    pv3_set(f, s->vector, v);
    pf_set(f, s->x, v[0]); pf_set(f, s->y, v[1]); pf_set(f, s->z, v[2]);
}
static void getscale_enter(act_inst *a) { ST(st_getscale); getscale_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GetScale = { "GetScale", sizeof(st_getscale), getscale_bind, getscale_enter, getscale_do, NULL, NULL, NULL, NULL, NULL };

/* GetRotation — ACT/GetRotation.cs:44-82 */
typedef struct { const fsm_pv *go, *quaternion, *vector, *x, *y, *z, *space, *everyFrame; } st_getrot;
static void getrot_bind(act_inst *a) { ST(st_getrot); s->go = FIELD(gameObject); s->quaternion = FIELD(quaternion); s->vector = FIELD(vector); s->x = FIELD(xAngle); s->y = FIELD(yAngle); s->z = FIELD(zAngle); s->space = FIELD(space); s->everyFrame = FIELD(everyFrame); }
static void getrot_do(act_inst *a)
{
    ST(st_getrot);
    int32_t t = p_owner_default(a, s->go);
    if (t < 0) return;
    if (w->gos[t].fold_y180) HKSIM_UNIMPLEMENTED("GetRotation of '%s', held folded after a y half turn (SetRotation)", go_path(w, t));
    float z = s->space->i == SPACE_WORLD ? go_euler_z(w, t) : go_local_euler_z(w, t);
    float e[4] = { 0.0f, 0.0f, z, 0.0f };
    /* Quaternion.Euler(0,0,z): (0,0,sin(z/2),cos(z/2)) */
    float h = z * ((float)M_PI / 180.0f) * 0.5f;
    float q[4] = { 0.0f, 0.0f, m_sin(h), m_cos(h) };
    pv3_set(f, s->quaternion, q);
    pv3_set(f, s->vector, e);
    pf_set(f, s->x, 0.0f); pf_set(f, s->y, 0.0f); pf_set(f, s->z, z);
}
static void getrot_enter(act_inst *a) { ST(st_getrot); getrot_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GetRotation = { "GetRotation", sizeof(st_getrot), getrot_bind, getrot_enter, getrot_do, NULL, NULL, NULL, NULL, NULL };

/* SetPosition — ACT/SetPosition.cs:53-117 */
typedef struct { const fsm_pv *go, *vector, *x, *y, *z, *space, *everyFrame, *lateUpdate; } st_setpos;
static void setpos_bind(act_inst *a) { ST(st_setpos); s->go = FIELD(gameObject); s->vector = FIELD(vector); s->x = FIELD(x); s->y = FIELD(y); s->z = FIELD(z); s->space = FIELD(space); s->everyFrame = FIELD(everyFrame); s->lateUpdate = FIELD(lateUpdate); }
static void setpos_do(act_inst *a)
{
    ST(st_setpos);
    int32_t t = p_owner_default(a, s->go);
    if (t < 0) return;
    float v[3];
    if (!p_isnone(s->vector)) memcpy(v, pv3(f, s->vector), sizeof v);
    else if (s->space->i == SPACE_WORLD) go_world_pos(w, t, v); else go_local_pos(w, t, v);
    if (!p_isnone(s->x)) v[0] = pf(f, s->x);
    if (!p_isnone(s->y)) v[1] = pf(f, s->y);
    if (!p_isnone(s->z)) v[2] = pf(f, s->z);
    if (s->space->i == SPACE_WORLD) go_set_world_pos(w, t, v); else go_set_local_pos(w, t, v);
}
static void setpos_enter(act_inst *a) { ST(st_setpos); if (!pb(f, s->everyFrame) && !pb(f, s->lateUpdate)) { setpos_do(a); act_finish(a); } }
static void setpos_update(act_inst *a) { ST(st_setpos); if (!pb(f, s->lateUpdate)) setpos_do(a); }
static void setpos_late(act_inst *a) { ST(st_setpos); if (pb(f, s->lateUpdate)) setpos_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_SetPosition = { "SetPosition", sizeof(st_setpos), setpos_bind, setpos_enter, setpos_update, NULL, setpos_late, NULL, NULL, NULL };

/* SetScale — ACT/SetScale.cs:49-106 (OnEnter always acts) */
typedef struct { const fsm_pv *go, *vector, *x, *y, *z, *everyFrame, *lateUpdate; } st_setscale;
static void setscale_bind(act_inst *a) { ST(st_setscale); s->go = FIELD(gameObject); s->vector = FIELD(vector); s->x = FIELD(x); s->y = FIELD(y); s->z = FIELD(z); s->everyFrame = FIELD(everyFrame); s->lateUpdate = FIELD(lateUpdate); }
static void setscale_do(act_inst *a)
{
    ST(st_setscale);
    int32_t t = p_owner_default(a, s->go);
    if (t < 0) return;
    float v[3];
    if (p_isnone(s->vector)) go_local_scale(w, t, v); else memcpy(v, pv3(f, s->vector), sizeof v);
    if (!p_isnone(s->x)) v[0] = pf(f, s->x);
    if (!p_isnone(s->y)) v[1] = pf(f, s->y);
    if (!p_isnone(s->z)) v[2] = pf(f, s->z);
    go_set_local_scale(w, t, v);
}
static void setscale_enter(act_inst *a) { ST(st_setscale); setscale_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static void setscale_update(act_inst *a) { ST(st_setscale); if (!pb(f, s->lateUpdate)) setscale_do(a); }
static void setscale_late(act_inst *a) { ST(st_setscale); if (pb(f, s->lateUpdate)) setscale_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_SetScale = { "SetScale", sizeof(st_setscale), setscale_bind, setscale_enter, setscale_update, NULL, setscale_late, NULL, NULL, NULL };

/* SetRotation — ACT/SetRotation.cs:59-123 */
typedef struct { const fsm_pv *go, *quaternion, *vector, *x, *y, *z, *space, *everyFrame, *lateUpdate; } st_setrot;
static void setrot_bind(act_inst *a) { ST(st_setrot); s->go = FIELD(gameObject); s->quaternion = FIELD(quaternion); s->vector = FIELD(vector); s->x = FIELD(xAngle); s->y = FIELD(yAngle); s->z = FIELD(zAngle); s->space = FIELD(space); s->everyFrame = FIELD(everyFrame); s->lateUpdate = FIELD(lateUpdate); }
static void setrot_do(act_inst *a)
{
    ST(st_setrot);
    int32_t t = p_owner_default(a, s->go);
    if (t < 0) return;
    float e[3];
    if (!p_isnone(s->quaternion)) HKSIM_UNKNOWN("SetRotation.quaternion.eulerAngles conversion in %s", fsm_label(f));
    else if (p_isnone(s->vector)) { e[0] = e[1] = 0.0f; e[2] = s->space->i == 1 ? go_local_euler_z(w, t) : go_euler_z(w, t); }
    else memcpy(e, pv3(f, s->vector), sizeof e);
    if (!p_isnone(s->x)) e[0] = pf(f, s->x);
    if (!p_isnone(s->y)) e[1] = pf(f, s->y);
    if (!p_isnone(s->z)) e[2] = pf(f, s->z);
    /* x/y euler on an object with neither collider nor body is a sprite tilt (Superdash effect sprites): only z matters
     * to the sim.  On a physical object only a half turn about y is representable: Euler(0,180,z) = Ry(180)*Rz(z), whose
     * action on the xy plane is diag(-1,1)*R(z) = R(-z)*diag(-1,1), i.e. a z rotation of -z over a mirrored x scale
     * (GG_Soul_Master's `Shockwave Spurt L | y rotate`, world space, at the scene root).  The object keeps that folded
     * form (go_inst.fold_y180), and reading its scale or rotation back traps. */
    bool physical = w->gos[t].n_cols > 0 || w->gos[t].body;
    bool half_y = e[0] == 0.0f && (e[1] == 180.0f || e[1] == -180.0f);
    if ((e[0] != 0.0f || e[1] != 0.0f) && physical && !half_y) HKSIM_UNKNOWN("SetRotation with non-z euler (%g,%g) on a physical object in %s", e[0], e[1], fsm_label(f));
    if (physical && (half_y != (w->gos[t].fold_y180 != 0))) {
        if (s->space->i != 1 && w->gos[t].parent >= 0)
            HKSIM_UNIMPLEMENTED("SetRotation y=%g in world space on '%s' under a parent", e[1], go_path(w, t));
        float ls[3]; go_local_scale(w, t, ls); ls[0] = -ls[0];
        go_set_local_scale(w, t, ls);
        w->gos[t].fold_y180 = half_y ? 1 : 0;
    }
    if (w->gos[t].fold_y180) e[2] = -e[2];
    if (s->space->i == 1) go_set_local_euler_z(w, t, e[2]); else go_set_euler_z(w, t, e[2]);
}
static void setrot_enter(act_inst *a) { ST(st_setrot); if (!pb(f, s->everyFrame) && !pb(f, s->lateUpdate)) { setrot_do(a); act_finish(a); } }
static void setrot_update(act_inst *a) { ST(st_setrot); if (!pb(f, s->lateUpdate)) setrot_do(a); }
static void setrot_late(act_inst *a) { ST(st_setrot); if (pb(f, s->lateUpdate)) setrot_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
/* SetRandomRotation — ACT/SetRandomRotation.cs:31-56: each enabled axis of localEulerAngles becomes
 * Random.Range(0, 360) (the int overload, [0,360)); the pose model is 2D, so only z. */
typedef struct { const fsm_pv *go, *z; } st_srr;
static void srr_bind(act_inst *a) { ST(st_srr); s->go = FIELD(gameObject); s->z = FIELD(z); }
static void srr_enter(act_inst *a)
{
    ST(st_srr);
    int32_t t = p_owner_default(a, s->go);
    if (t >= 0) {
        float ez = go_local_euler_z(w, t);
        if (pb(f, s->z)) ez = (float)hk_rng_range_i_site(w->rng, a->rng_site, 0, 360);
        go_set_local_euler_z(w, t, ez);
    }
    act_finish(a);
}
static const act_vtable AV_SetRandomRotation = { "SetRandomRotation", sizeof(st_srr), srr_bind, srr_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_SetRotation = { "SetRotation", sizeof(st_setrot), setrot_bind, setrot_enter, setrot_update, NULL, setrot_late, NULL, NULL, NULL };

/* FlipScale — ACT/FlipScale.cs:30-75 */
typedef struct { const fsm_pv *go, *h, *v, *everyFrame, *lateUpdate; } st_flip;
static void flip_bind(act_inst *a) { ST(st_flip); s->go = FIELD(gameObject); s->h = FIELD(flipHorizontally); s->v = FIELD(flipVertically); s->everyFrame = FIELD(everyFrame); s->lateUpdate = FIELD(lateUpdate); }
static void flip_do(act_inst *a)
{
    ST(st_flip);
    int32_t t = p_owner_default(a, s->go);
    if (t < 0) return;
    float sc[3]; go_local_scale(w, t, sc);
    if (pb(f, s->h)) sc[0] = 0.0f - sc[0];
    if (pb(f, s->v)) sc[1] = 0.0f - sc[1];
    go_set_local_scale(w, t, sc);
}
static void flip_enter(act_inst *a) { ST(st_flip); flip_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static void flip_update(act_inst *a) { ST(st_flip); if (!pb(f, s->lateUpdate)) flip_do(a); }
static void flip_late(act_inst *a) { ST(st_flip); if (pb(f, s->lateUpdate)) flip_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_FlipScale = { "FlipScale", sizeof(st_flip), flip_bind, flip_enter, flip_update, NULL, flip_late, NULL, NULL, NULL };

/* Translate — ACT/Translate.cs:64-144 */
typedef struct { const fsm_pv *go, *vector, *x, *y, *z, *space, *perSecond, *everyFrame, *lateUpdate, *fixedUpdate; } st_translate;
static void translate_bind(act_inst *a) { ST(st_translate); s->go = FIELD(gameObject); s->vector = FIELD(vector); s->x = FIELD(x); s->y = FIELD(y); s->z = FIELD(z); s->space = FIELD(space); s->perSecond = FIELD(perSecond); s->everyFrame = FIELD(everyFrame); s->lateUpdate = FIELD(lateUpdate); s->fixedUpdate = FIELD(fixedUpdate); }
static void translate_do(act_inst *a)
{
    ST(st_translate);
    int32_t t = p_owner_default(a, s->go);
    if (t < 0) return;
    float v[3];
    if (p_isnone(s->vector)) { v[0] = pf(f, s->x); v[1] = pf(f, s->y); v[2] = pf(f, s->z); } else memcpy(v, pv3(f, s->vector), sizeof v);
    if (!p_isnone(s->x)) v[0] = pf(f, s->x);
    if (!p_isnone(s->y)) v[1] = pf(f, s->y);
    if (!p_isnone(s->z)) v[2] = pf(f, s->z);
    if (pb(f, s->perSecond)) { v[0] *= w->dt; v[1] *= w->dt; v[2] *= w->dt; }
    /* Transform.Translate(v, Space.Self) rotates v by the object's rotation; the object's own rotation
     * is z-only so Self == World when localEulerZ == 0 */
    float p[3]; go_world_pos(w, t, p);
    if (s->space->i == 1) {
        float ang = go_euler_z(w, t) * ((float)M_PI / 180.0f);
        float c = m_cos(ang), sn = m_sin(ang);
        float rx = v[0] * c - v[1] * sn, ry = v[0] * sn + v[1] * c;
        v[0] = rx; v[1] = ry;
    }
    p[0] += v[0]; p[1] += v[1]; p[2] += v[2];
    go_set_world_pos(w, t, p);
}
static void translate_enter(act_inst *a) { ST(st_translate); if (!pb(f, s->everyFrame) && !pb(f, s->lateUpdate) && !pb(f, s->fixedUpdate)) { translate_do(a); act_finish(a); } }
static void translate_update(act_inst *a) { ST(st_translate); if (!pb(f, s->lateUpdate) && !pb(f, s->fixedUpdate)) translate_do(a); }
static void translate_late(act_inst *a) { ST(st_translate); if (pb(f, s->lateUpdate)) translate_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static void translate_fixed(act_inst *a) { ST(st_translate); if (pb(f, s->fixedUpdate)) translate_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_Translate = { "Translate", sizeof(st_translate), translate_bind, translate_enter, translate_update, translate_fixed, translate_late, NULL, NULL, NULL };

/* SetParent — ACT/SetParent.cs:30-46 (world-pose rule UNKNOWN: Q-fsmact-11) */
typedef struct { const fsm_pv *go, *parent, *resetPos, *resetRot; } st_setparent;
static void setparent_bind(act_inst *a) { ST(st_setparent); s->go = FIELD(gameObject); s->parent = FIELD(parent); s->resetPos = FIELD(resetLocalPosition); s->resetRot = FIELD(resetLocalRotation); }
static void setparent_enter(act_inst *a)
{
    ST(st_setparent);
    int32_t t = p_owner_default(a, s->go);
    if (t >= 0) {
        go_set_parent(w, t, pgo(f, s->parent));                   /* traps: Q-fsmact-11 */
        /* SetParent.cs:33-42: the two reset flags are applied AFTER reparenting, unconditionally on
         * their own (independent of each other and of whether go_set_parent traps below them). */
        if (pb(f, s->resetPos)) { float z[3] = { 0.0f, 0.0f, 0.0f }; go_set_local_pos(w, t, z); }
        if (pb(f, s->resetRot)) go_set_local_euler_z(w, t, 0.0f);   /* localRotation = identity -> eulerZ 0 (2D model) */
    }
    act_finish(a);
}
static const act_vtable AV_SetParent = { "SetParent", sizeof(st_setparent), setparent_bind, setparent_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* FaceObject — ACT/FaceObject.cs:45-134 */
typedef struct { const fsm_pv *objectA, *objectB, *spriteFacesRight, *playNewAnimation, *newAnimationClip, *resetFrame, *everyFrame; float xScale; int32_t sprite; } st_faceobj;
static void faceobj_bind(act_inst *a) { ST(st_faceobj); s->objectA = FIELD(objectA); s->objectB = FIELD(objectB); s->spriteFacesRight = FIELD(spriteFacesRight); s->playNewAnimation = FIELD(playNewAnimation); s->newAnimationClip = FIELD(newAnimationClip); s->resetFrame = FIELD(resetFrame); s->everyFrame = FIELD(everyFrame); }
static void faceobj_changed(act_inst *a)
{
    ST(st_faceobj);
    anim_inst *sp = anim_deref(w, s->sprite);
    if (pb(f, s->resetFrame)) { if (!sp) HKSIM_UNKNOWN("FaceObject.resetFrame with null animator"); anim_play_from_frame(w, sp, 0); }
    if (pb(f, s->playNewAnimation)) { if (!sp) HKSIM_UNKNOWN("FaceObject.playNewAnimation with null animator"); anim_play_name(w, sp, w_str(w, ps(f, s->newAnimationClip))); }
}
static void faceobj_do(act_inst *a)
{
    ST(st_faceobj);
    int32_t A = pgo(f, s->objectA), B = pgo(f, s->objectB);
    HKSIM_ASSERT(A >= 0, "FaceObject.objectA null");
    float ls[3]; go_local_scale(w, A, ls);
    if (B < 0 || p_isnone(s->objectB)) { act_finish(a); }
    HKSIM_ASSERT(B >= 0, "FaceObject.objectB null (NullReferenceException in C#)");
    float pa[3], pbp[3]; go_world_pos(w, A, pa); go_world_pos(w, B, pbp);
    bool right = pb(f, s->spriteFacesRight);
    if (pa[0] < pbp[0]) {
        if (right) { if (ls[0] != s->xScale) { ls[0] = s->xScale; faceobj_changed(a); } }
        else if (ls[0] != 0.0f - s->xScale) { ls[0] = 0.0f - s->xScale; faceobj_changed(a); }
    } else if (right) {
        if (ls[0] != 0.0f - s->xScale) { ls[0] = 0.0f - s->xScale; faceobj_changed(a); }
    } else if (ls[0] != s->xScale) { ls[0] = s->xScale; faceobj_changed(a); }
    float cur[3]; go_local_scale(w, A, cur);
    float nv[3] = { ls[0], cur[1], cur[2] };
    go_set_local_scale(w, A, nv);
}
static void faceobj_enter(act_inst *a)
{
    ST(st_faceobj);
    int32_t A = pgo(f, s->objectA);
    HKSIM_ASSERT(A >= 0, "FaceObject.objectA null");
    s->sprite = anim_ref(w, anim_of_go(w, A));
    if (!s->sprite) act_finish(a);                                 /* :47-50 Finish but execution continues */
    float ls[3]; go_local_scale(w, A, ls);
    s->xScale = ls[0];
    if (s->xScale < 0.0f) s->xScale *= -1.0f;
    faceobj_do(a);
    if (!pb(f, s->everyFrame)) act_finish(a);
}
static const act_vtable AV_FaceObject = { "FaceObject", sizeof(st_faceobj), faceobj_bind, faceobj_enter, faceobj_do, NULL, NULL, NULL, NULL, NULL };

/* FaceAngle / FaceAngleV2 — ACT/FaceAngle.cs:28-62, FaceAngleV2.cs:29-70 (FixedUpdate) */
typedef struct { const fsm_pv *go, *angleOffset, *worldSpace, *everyFrame; int32_t rb, target; bool v2; } st_faceangle;
static void faceangle_bind(act_inst *a) { ST(st_faceangle); s->go = FIELD(gameObject); s->angleOffset = FIELD(angleOffset); s->worldSpace = a_field(a, "worldSpace"); s->everyFrame = FIELD(everyFrame); s->v2 = s->worldSpace != NULL; }
static void faceangle_do(act_inst *a)
{
    ST(st_faceangle);
    if (s->rb < 0) return;
    float v[2]; go_velocity(w, s->rb, v);
    /* compound statement: double stack, one rounding at the float store (float-parity.md) */
    float z = (float)((double)m_atan2(v[1], v[0]) * (double)(180.0f / (float)M_PI) + (double)pf(f, s->angleOffset));
    if (s->v2 && pb(f, s->worldSpace)) go_set_euler_z(w, s->target, z); else go_set_local_euler_z(w, s->target, z);
}
static void faceangle_enter(act_inst *a)
{
    ST(st_faceangle);
    int32_t t = p_owner_default(a, s->go);
    s->rb = (t >= 0 && go_has_rb(w, t)) ? t : -1;                  /* CacheRigidBody2d: null go leaves rb2d (RigidBody2dActionBase.cs:9-19) */
    s->target = t;
    faceangle_do(a);
    if (!pb(f, s->everyFrame)) act_finish(a);
}
static const act_vtable AV_FaceAngle = { "FaceAngle", sizeof(st_faceangle), faceangle_bind, faceangle_enter, NULL, faceangle_do, NULL, NULL, NULL, NULL };
static const act_vtable AV_FaceAngleV2 = { "FaceAngleV2", sizeof(st_faceangle), faceangle_bind, faceangle_enter, NULL, faceangle_do, NULL, NULL, NULL, NULL };

/* GetAngleToTarget2D — ACT/GetAngleToTarget2D.cs:41-77 */
typedef struct { const fsm_pv *go, *target, *offsetX, *offsetY, *storeAngle, *everyFrame; int32_t self; } st_gatt;
static void gatt_bind(act_inst *a) { ST(st_gatt); s->go = FIELD(gameObject); s->target = FIELD(target); s->offsetX = FIELD(offsetX); s->offsetY = FIELD(offsetY); s->storeAngle = FIELD(storeAngle); s->everyFrame = FIELD(everyFrame); }
static void gatt_do(act_inst *a)
{
    ST(st_gatt);
    int32_t tg = pgo(f, s->target);
    HKSIM_ASSERT(tg >= 0 && s->self >= 0, "GetAngleToTarget2D null %s in %s state %s", tg < 0 ? "target" : "self", fsm_label(f), state_name(f, f->active_state));
    float pt[3], ps_[3]; go_world_pos(w, tg, pt); go_world_pos(w, s->self, ps_);
    float num = (float)((double)pt[1] + (double)pf(f, s->offsetY) - (double)ps_[1]);    /* :70 compound -> double stack */
    float num2 = (float)((double)pt[0] + (double)pf(f, s->offsetX) - (double)ps_[0]);   /* :71 */
    float num3 = (float)((double)m_atan2(num, num2) * (double)(180.0f / (float)M_PI));  /* :73 */
    while (num3 < 0.0f) num3 += 360.0f;
    pf_set(f, s->storeAngle, num3);
}
static void gatt_enter(act_inst *a)
{
    ST(st_gatt);
    s->self = p_owner_default(a, s->go);
    if (p_isnone(s->offsetX)) pf_set(f, s->offsetX, 0.0f);         /* :29-35 writes the None param's value */
    if (p_isnone(s->offsetY)) pf_set(f, s->offsetY, 0.0f);
    gatt_do(a);
    if (!pb(f, s->everyFrame)) act_finish(a);
}
static void gatt_update(act_inst *a) { ST(st_gatt); gatt_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GetAngleToTarget2D = { "GetAngleToTarget2D", sizeof(st_gatt), gatt_bind, gatt_enter, gatt_update, NULL, NULL, NULL, NULL, NULL };

/* CheckTargetDirection — ACT/CheckTargetDirection.cs:48-109 */
typedef struct { const fsm_pv *go, *target, *aboveEvent, *belowEvent, *rightEvent, *leftEvent, *aboveBool, *belowBool, *rightBool, *leftBool, *everyFrame; int32_t self; } st_ctd;
static void ctd_bind(act_inst *a)
{
    ST(st_ctd);
    s->go = FIELD(gameObject); s->target = FIELD(target); s->aboveEvent = FIELD(aboveEvent); s->belowEvent = FIELD(belowEvent); s->rightEvent = FIELD(rightEvent); s->leftEvent = FIELD(leftEvent);
    s->aboveBool = FIELD(aboveBool); s->belowBool = FIELD(belowBool); s->rightBool = FIELD(rightBool); s->leftBool = FIELD(leftBool); s->everyFrame = FIELD(everyFrame);
}
static void ctd_do(act_inst *a)
{
    ST(st_ctd);
    int32_t tg = pgo(f, s->target);
    HKSIM_ASSERT(tg >= 0 && s->self >= 0, "CheckTargetDirection null target/self in %s", fsm_label(f));
    float ps_[3], pt[3]; go_world_pos(w, s->self, ps_); go_world_pos(w, tg, pt);
    if (ps_[0] < pt[0]) { fsm_event(f, EV(s->rightEvent)); pb_set(f, s->rightBool, true); } else pb_set(f, s->rightBool, false);
    if (ps_[0] > pt[0]) { fsm_event(f, EV(s->leftEvent)); pb_set(f, s->leftBool, true); } else pb_set(f, s->leftBool, false);
    if (ps_[1] < pt[1]) { fsm_event(f, EV(s->aboveEvent)); pb_set(f, s->aboveBool, true); } else pb_set(f, s->aboveBool, false);
    if (ps_[1] > pt[1]) { fsm_event(f, EV(s->belowEvent)); pb_set(f, s->belowBool, true); } else pb_set(f, s->belowBool, false);
}
static void ctd_enter(act_inst *a) { ST(st_ctd); s->self = p_owner_default(a, s->go); ctd_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static void ctd_update(act_inst *a) { ST(st_ctd); ctd_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_CheckTargetDirection = { "CheckTargetDirection", sizeof(st_ctd), ctd_bind, ctd_enter, ctd_update, NULL, NULL, NULL, NULL, NULL };

/* ScaleTo — ACT/ScaleTo.cs:41-107.  OnEnter caches the target's transform and its
 * localScale, then runs UpdateScaling once; OnUpdate runs it again.  UpdateScaling advances the timer by
 * Time.deltaTime FIRST, so the scale moves on the entry frame, and finishes only once the timer has passed
 * duration + delay (`>`).  Statements are on the Mono double stack (docs/float-parity.md): Clamp01's argument, the
 * curve's return and each Vector3.Lerp component (UnityCsReference Vector3.cs:38-46, 2020.2.2f1) round once. */
typedef struct { const fsm_pv *gameObject, *target, *duration, *delay, *curve; int32_t xf; float timer; float start_scale[3]; } st_scaleto;
static void scaleto_bind(act_inst *a) { ST(st_scaleto); s->gameObject = FIELD(gameObject); s->target = FIELD(target); s->duration = FIELD(duration); s->delay = FIELD(delay); s->curve = FIELD(curve); }
static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }   /* Mathf.Clamp01 (Mathf.cs:205-213); NaN passes */
static float scaleto_curve(float val, int curve)                   /* GetCurved :83-91 */
{
    if (curve == 1) return (float)((double)val * (2.0 - (double)val));                       /* QuadraticOut :98-101 */
    if (curve == 2) return m_sin((float)((double)val * (double)(float)M_PI * 0.5));         /* SinusoidalOut :103-106 */
    return val;                                                                                /* Linear :93-96 */
}
static void scaleto_scaling(act_inst *a)                           /* UpdateScaling :64-81 */
{
    ST(st_scaleto);
    if (s->xf < 0) { act_finish(a); return; }
    s->timer = (float)((double)s->timer + (double)w->dt);
    float dur = pf(f, s->duration), del = pf(f, s->delay);
    float curved = scaleto_curve(clamp01((float)(((double)s->timer - (double)del) / (double)dur)), s->curve ? s->curve->i : 0);
    const float *tgt = pv3(f, s->target);
    double t = (double)clamp01(curved);                            /* Vector3.Lerp clamps t again */
    float cur[3] = { (float)((double)s->start_scale[0] + ((double)tgt[0] - (double)s->start_scale[0]) * t),
                     (float)((double)s->start_scale[1] + ((double)tgt[1] - (double)s->start_scale[1]) * t),
                     (float)((double)s->start_scale[2] + ((double)tgt[2] - (double)s->start_scale[2]) * t) };
    go_set_local_scale(w, s->xf, cur);
    if ((double)s->timer > (double)dur + (double)del) {
        go_set_local_scale(w, s->xf, tgt);
        act_finish(a);
    }
}
static void scaleto_enter(act_inst *a)                             /* OnEnter :41-56 */
{
    ST(st_scaleto);
    s->timer = 0.0f;
    s->xf = p_get_safe(a, s->gameObject);
    if (s->xf >= 0) go_local_scale(w, s->xf, s->start_scale);
    scaleto_scaling(a);
}
static void scaleto_update(act_inst *a) { scaleto_scaling(a); }   /* OnUpdate :58-62 */

static const act_vtable AV_ScaleTo = { "ScaleTo", sizeof(st_scaleto), scaleto_bind, scaleto_enter, scaleto_update, NULL, NULL, NULL, NULL, NULL };

/* GetDistance — ACT/GetDistance.cs:48-54 */
typedef struct { const fsm_pv *go, *target, *storeResult, *everyFrame; } st_getdist;
static void getdist_bind(act_inst *a) { ST(st_getdist); s->go = FIELD(gameObject); s->target = FIELD(target); s->storeResult = FIELD(storeResult); s->everyFrame = FIELD(everyFrame); }
static void getdist_do(act_inst *a) {
    ST(st_getdist);
    int32_t t = p_owner_default(a, s->go);
    int32_t tgt = pgo(f, s->target);
    if (t >= 0 && tgt >= 0 && !p_isnone(s->storeResult)) {
        float pt[3], ptgt[3];
        go_world_pos(w, t, pt);
        go_world_pos(w, tgt, ptgt);
        /* Vector3.Distance (UnityCsReference Vector3.cs:331-337, 2020.2.2f1): float differences, then the sum of
         * squares on the Mono double stack and one rounding after Math.Sqrt (docs/float-parity.md) */
        float dx = pt[0] - ptgt[0], dy = pt[1] - ptgt[1], dz = pt[2] - ptgt[2];
        pf_set(f, s->storeResult, (float)sqrt((double)dx * dx + (double)dy * dy + (double)dz * dz));
    }
}
static void getdist_enter(act_inst *a) { ST(st_getdist); getdist_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static void getdist_update(act_inst *a) { ST(st_getdist); getdist_do(a); }
static const act_vtable AV_GetDistance = { "GetDistance", sizeof(st_getdist), getdist_bind, getdist_enter, getdist_update, NULL, NULL, NULL, NULL, NULL };

/* GetPosition2D - ACT/GetPosition2D.cs */
typedef struct { const fsm_pv *gameObject, *vector, *x, *y, *everyFrame; } st_gp2d;
static void gp2d_bind(act_inst *a) { ST(st_gp2d); s->gameObject = FIELD(gameObject); s->vector = FIELD(vector); s->x = FIELD(x); s->y = FIELD(y); s->everyFrame = FIELD(everyFrame); }
static void gp2d_do(act_inst *a) {
    ST(st_gp2d);
    int32_t go = p_owner_default(a, s->gameObject);
    if (go >= 0) {
        float pos[3]; go_world_pos(w, go, pos);
        if (!p_isnone(s->vector)) pv3_set(f, s->vector, pos);
        if (!p_isnone(s->x)) pf_set(f, s->x, pos[0]);
        if (!p_isnone(s->y)) pf_set(f, s->y, pos[1]);
    }
}
static void gp2d_enter(act_inst *a) { ST(st_gp2d); gp2d_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GetPosition2D = { "GetPosition2D", sizeof(st_gp2d), gp2d_bind, gp2d_enter, gp2d_do, NULL, NULL, NULL, NULL, NULL };

/* GetNamedParent -- ACT/GetNamedParent.cs:19-45: the nearest ancestor with that name (and tag, when given) */
typedef struct { const fsm_pv *go, *parentName, *withTag, *storeResult; } st_gnp;
static void gnp_bind(act_inst *a) { ST(st_gnp); s->go = FIELD(gameObject); s->parentName = FIELD(parentName); s->withTag = FIELD(withTag); s->storeResult = FIELD(storeResult); }
static void gnp_enter(act_inst *a)
{
    ST(st_gnp);
    int32_t t = p_owner_default(a, s->go), hit = -1;
    const char *name = w_str(w, ps(f, s->parentName));
    const char *tag = p_isnone(s->withTag) ? "" : w_str(w, ps(f, s->withTag));
    for (int32_t p = t >= 0 ? w->gos[t].parent : -1; p >= 0; p = w->gos[p].parent) {
        if (strcmp(go_name(w, p), name) != 0) continue;
        if (tag[0]) {
            int32_t tg = go_tag(w, p);
            if (tg < 0) HKSIM_UNKNOWN("GetNamedParent: tag of '%s' not dumped in %s", go_path(w, p), fsm_label(f));
            if (strcmp(w_str(w, tg), tag) != 0) continue;
        }
        hit = p;
        break;
    }
    pgo_set(f, s->storeResult, hit);
    act_finish(a);
}
static const act_vtable AV_GetNamedParent = { "GetNamedParent", sizeof(st_gnp), gnp_bind, gnp_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* SetTransformParent -- ACT/SetTransformParent.cs:21-39: Transform.SetParent(parent, worldPositionStays) */
typedef struct { const fsm_pv *go, *parent, *worldPositionStays; } st_stp;
static void stp_bind(act_inst *a) { ST(st_stp); s->go = FIELD(gameObject); s->parent = FIELD(parent); s->worldPositionStays = FIELD(worldPositionStays); }
static void stp_enter(act_inst *a)
{
    ST(st_stp);
    int32_t t = p_owner_default(a, s->go), par = pgo(f, s->parent);
    if (t >= 0) {
        if (pb(f, s->worldPositionStays)) go_set_parent(w, t, par);   /* go_set_parent keeps the world pose */
        else {
            float lp[3], ls[3], lz = go_local_euler_z(w, t);
            go_local_pos(w, t, lp); go_local_scale(w, t, ls);
            go_set_parent(w, t, par);
            go_set_local_scale(w, t, ls); go_set_local_euler_z(w, t, lz); go_set_local_pos(w, t, lp);
        }
    }
    act_finish(a);
}
static const act_vtable AV_SetTransformParent = { "SetTransformParent", sizeof(st_stp), stp_bind, stp_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* DetachChildren -- ACT/DetachChildren.cs:12-29: Transform.DetachChildren, every child to the root keeping its world
 * pose */
typedef struct { const fsm_pv *go; } st_dch;
static void dch_bind(act_inst *a) { ST(st_dch); s->go = FIELD(gameObject); }
static void dch_enter(act_inst *a)
{
    ST(st_dch);
    int32_t t = p_owner_default(a, s->go);
    while (t >= 0 && w->gos[t].first_child >= 0) go_set_parent(w, w->gos[t].first_child, -1);
    act_finish(a);
}
static const act_vtable AV_DetachChildren = { "DetachChildren", sizeof(st_dch), dch_bind, dch_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* ObjectJitterLocal -- ACT/ObjectJitterLocal.cs:30-73: the start local position, then every FixedUpdate
 * localPosition = start + Random.Range(-x, x) per axis (OnPreprocess sets HandleFixedUpdate); never finishes */
typedef struct { const fsm_pv *go, *x, *y, *z; float start[3]; } st_ojl;
static void ojl_bind(act_inst *a) { ST(st_ojl); s->go = FIELD(gameObject); s->x = FIELD(x); s->y = FIELD(y); s->z = FIELD(z); }
static void ojl_enter(act_inst *a) { ST(st_ojl); int32_t t = p_owner_default(a, s->go); if (t >= 0) go_local_pos(w, t, s->start); }
static void ojl_fixed(act_inst *a)
{
    ST(st_ojl);
    int32_t t = p_owner_default(a, s->go);
    if (t < 0) return;
    float p[3];
    const fsm_pv *ax[3] = { s->x, s->y, s->z };
    for (int k = 0; k < 3; k++) { float r = pf(f, ax[k]); p[k] = s->start[k] + hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - r, r); }
    go_set_local_pos(w, t, p);
}
static const act_vtable AV_ObjectJitterLocal = { "ObjectJitterLocal", sizeof(st_ojl), ojl_bind, ojl_enter, NULL, ojl_fixed, NULL, NULL, NULL, NULL };

/* SetPositionToObject — ACT/SetPositionToObject.cs:24-49: null-guarded on both the owner and targetObject;
 * each offset is optional (IsNone). */
typedef struct { const fsm_pv *go, *targetObject, *xOffset, *yOffset, *zOffset; } st_sp2o;
static void sp2o_bind(act_inst *a) { ST(st_sp2o); s->go = FIELD(gameObject); s->targetObject = FIELD(targetObject); s->xOffset = FIELD(xOffset); s->yOffset = FIELD(yOffset); s->zOffset = FIELD(zOffset); }
static void sp2o_enter(act_inst *a)
{
    ST(st_sp2o);
    int32_t t = p_owner_default(a, s->go);
    int32_t tgt = pgo(f, s->targetObject);
    if (t >= 0 && tgt >= 0) {
        float p[3]; go_world_pos(w, tgt, p);
        if (!p_isnone(s->xOffset)) p[0] += pf(f, s->xOffset);
        if (!p_isnone(s->yOffset)) p[1] += pf(f, s->yOffset);
        if (!p_isnone(s->zOffset)) p[2] += pf(f, s->zOffset);
        go_set_world_pos(w, t, p);
    }
    act_finish(a);
}
static const act_vtable AV_SetPositionToObject = { "SetPositionToObject", sizeof(st_sp2o), sp2o_bind, sp2o_enter, NULL, NULL, NULL, NULL, NULL, NULL };

const act_vtable *const act_registry_transform[] = {
    &AV_Rotate, &AV_RotateTo, &AV_GetXDistance, &AV_ObjectJitter, &AV_FaceDirection, &AV_GetPosition,
    &AV_GetScale, &AV_GetRotation, &AV_SetPosition, &AV_SetScale, &AV_SetRandomRotation, &AV_SetRotation,
    &AV_FlipScale, &AV_Translate, &AV_SetParent, &AV_FaceObject, &AV_FaceAngle, &AV_FaceAngleV2,
    &AV_GetAngleToTarget2D, &AV_CheckTargetDirection, &AV_ScaleTo, &AV_GetDistance, &AV_GetPosition2D,
    &AV_GetNamedParent, &AV_SetTransformParent, &AV_DetachChildren, &AV_ObjectJitterLocal,
    &AV_SetPositionToObject,
};
const int act_registry_transform_n = (int)(sizeof act_registry_transform / sizeof act_registry_transform[0]);
