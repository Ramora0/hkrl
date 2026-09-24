/* Rigidbody chase and flight behaviours. */
#include "act.h"

/* Vector2.ClampMagnitude -- analysis/upstream/UnityCsReference Runtime/Export/Math/Vector2.cs:223-238 (2020.2.2f1) on the
 * Mono double stack (docs/float-parity.md): sqrMagnitude (:191) returns x*x + y*y rounded to float, the test widens
 * maxLength * maxLength, the magnitude is (float)Math.Sqrt of that FLOAT, and each normalized component is rounded at
 * its float local before the final multiply.  No normalized-epsilon branch in this Unity version. */
static void clamp_magnitude2(float v[2], float max_len)
{
    float sq = (float)((double)v[0] * (double)v[0] + (double)v[1] * (double)v[1]);
    if (!((double)sq > (double)max_len * (double)max_len)) return;   /* :226 under the cap: unchanged */
    float mag = (float)sqrt((double)sq);                              /* :228 */
    float nx = (float)((double)v[0] / (double)mag), ny = (float)((double)v[1] / (double)mag);   /* :233-234 */
    v[0] = (float)((double)nx * (double)max_len);
    v[1] = (float)((double)ny * (double)max_len);
}

/* ChaseObjectGround — ACT/ChaseObjectGround.cs:44-97.  Ground chase: accelerate x toward the target,
 * clamp to +/-speedMax, and (optionally) swap between a run and a turn clip as x changes sign.
 * `Awake`/`OnPreprocess` set HandleFixedUpdate, and both OnEnter and OnFixedUpdate run DoChase. */
typedef struct { const fsm_pv *gameObject, *target, *speedMax, *acceleration, *animateTurnAndRun,
                        *runAnimation, *turnAnimation, *turnRange; int32_t cached_go; bool has, turning; } st_cog;
static void cog_bind(act_inst *a)
{
    ST(st_cog);
    s->gameObject = FIELD(gameObject); s->target = FIELD(target); s->speedMax = FIELD(speedMax);
    s->acceleration = FIELD(acceleration); s->animateTurnAndRun = FIELD(animateTurnAndRun);
    s->runAnimation = FIELD(runAnimation); s->turnAnimation = FIELD(turnAnimation);
    s->turnRange = FIELD(turnRange); s->cached_go = -1;
}
static void cog_do(act_inst *a)
{
    ST(st_cog);
    int32_t t = p_owner_default(a, s->gameObject);
    if (!act_cache_rb(a, t, &s->cached_go, &s->has)) return;          /* :47-50 rb2d == null -> return */
    int32_t tgt = pgo(f, s->target);
    if (tgt < 0) return;
    float vel[2], sp[3], tp[3];
    go_velocity(w, t, vel);
    go_world_pos(w, t, sp); go_world_pos(w, tgt, tp);
    float range = pf(f, s->turnRange);
    /* :53 return unless self.x is outside [target.x - turnRange, target.x + turnRange] */
    if (!(sp[0] < tp[0] - range) && !(sp[0] > tp[0] + range)) return;
    float acc = pf(f, s->acceleration);
    bool anim = pb(f, s->animateTurnAndRun);
    anim_inst *an = anim ? anim_of_go(w, t) : NULL;
    if (sp[0] < tp[0]) {                                              /* :57 chase right */
        vel[0] += acc;
        if (anim) {
            if (vel[0] < 0.0f && !s->turning) { if (an) anim_play_name(w, an, w_str(w, ps(f, s->turnAnimation))); s->turning = true; }
            if (vel[0] > 0.0f && s->turning)  { if (an) anim_play_name(w, an, w_str(w, ps(f, s->runAnimation)));  s->turning = false; }
        }
    } else {                                                          /* :73 chase left */
        vel[0] -= acc;
        if (anim) {
            if (vel[0] > 0.0f && !s->turning) { if (an) anim_play_name(w, an, w_str(w, ps(f, s->turnAnimation))); s->turning = true; }
            if (vel[0] < 0.0f && s->turning)  { if (an) anim_play_name(w, an, w_str(w, ps(f, s->runAnimation)));  s->turning = false; }
        }
    }
    float smax = pf(f, s->speedMax);
    if (vel[0] > smax) vel[0] = smax;                                 /* :89-95 */
    if (vel[0] < 0.0f - smax) vel[0] = 0.0f - smax;
    go_set_velocity(w, t, vel);
}
static void cog_enter(act_inst *a) { cog_do(a); }                     /* :30-36 cache + DoChase, never Finish()es */
static const act_vtable AV_ChaseObjectGround = { "ChaseObjectGround", sizeof(st_cog), cog_bind, cog_enter, NULL, cog_do, NULL, NULL, NULL, NULL };

/* ChaseObjectV2 — ACT/ChaseObjectV2.cs:38-52.  Steer toward (target + offset) with a force whose
 * direction is the offset vector clamped to unit length, then cap the resulting speed.
 * Uses ForceMode2D.Force, which is what go_add_force implements (fsm.h:385). */
typedef struct { const fsm_pv *gameObject, *target, *speedMax, *accelerationForce, *offsetX, *offsetY;
                 int32_t cached_go; bool has; } st_cov2;
static void cov2_bind(act_inst *a)
{
    ST(st_cov2);
    s->gameObject = FIELD(gameObject); s->target = FIELD(target); s->speedMax = FIELD(speedMax);
    s->accelerationForce = FIELD(accelerationForce); s->offsetX = FIELD(offsetX); s->offsetY = FIELD(offsetY);
    s->cached_go = -1;
}
static void cov2_do(act_inst *a)
{
    ST(st_cov2);
    int32_t t = p_owner_default(a, s->gameObject);
    if (!act_cache_rb(a, t, &s->cached_go, &s->has)) return;          /* :40 rb2d == null -> nothing */
    int32_t tgt = pgo(f, s->target);
    if (tgt < 0) return;
    float sp[3], tp[3];
    go_world_pos(w, t, sp); go_world_pos(w, tgt, tp);
    float v[2] = { tp[0] + pf(f, s->offsetX) - sp[0], tp[1] + pf(f, s->offsetY) - sp[1] };   /* :42 */
    clamp_magnitude2(v, 1.0f);                                        /* :43 ClampMagnitude(v, 1f) */
    float force = pf(f, s->accelerationForce);
    v[0] *= force; v[1] *= force;                                     /* :44 */
    go_add_force(w, t, v);                                            /* :45 AddForce(v) */
    float vel[2];
    go_velocity(w, t, vel);
    clamp_magnitude2(vel, pf(f, s->speedMax));                        /* :47 ClampMagnitude(velocity, speedMax) */
    go_set_velocity(w, t, vel);
}
static void cov2_enter(act_inst *a) { cov2_do(a); }                   /* :31-36 cache + DoChase, never Finish()es */
static const act_vtable AV_ChaseObjectV2 = { "ChaseObjectV2", sizeof(st_cov2), cov2_bind, cov2_enter, NULL, cov2_do, NULL, NULL, NULL, NULL };

/* DistanceFlySmooth - ACT/DistanceFlySmooth.cs:40-101.  Holds a standoff distance from the target:
 * outside the [distance-targetRadius, distance+targetRadius] band it accelerates toward (or away from)
 * the target and caps speed; inside the band it damps each velocity axis by `deceleration`, clamping
 * to zero if the multiply flips the sign.  Same ClampMagnitude/AddForce shape as ChaseObjectV2. */
typedef struct { const fsm_pv *gameObject, *target, *distance, *speedMax, *accelerationForce,
                        *targetRadius, *deceleration, *offset; int32_t cached_go; bool has; } st_dfs;
static void dfs_bind(act_inst *a)
{
    ST(st_dfs);
    s->gameObject = FIELD(gameObject); s->target = FIELD(target); s->distance = FIELD(distance);
    s->speedMax = FIELD(speedMax); s->accelerationForce = FIELD(accelerationForce);
    s->targetRadius = FIELD(targetRadius); s->deceleration = FIELD(deceleration); s->offset = FIELD(offset);
    s->cached_go = -1; s->has = false;
}
static void dfs_do(act_inst *a)
{
    ST(st_dfs);
    int32_t t = p_owner_default(a, s->gameObject);
    if (!act_cache_rb(a, t, &s->cached_go, &s->has)) return;          /* :42-45 rb2d null -> return */
    int32_t tgt = pgo(f, s->target);
    if (tgt < 0) return;
    float sp[3], tp[3], off[3];
    off[0] = 0.0f; off[1] = 0.0f; off[2] = 0.0f;
    go_world_pos(w, t, sp); go_world_pos(w, tgt, tp);
    if (!p_isnone(s->offset)) { const float *o = pv3(f, s->offset); off[0] = o[0]; off[1] = o[1]; off[2] = o[2]; }
    float dx = sp[0] - (tp[0] + off[0]), dy = sp[1] - (tp[1] + off[1]);
    float away = (float)sqrt((double)dx * (double)dx + (double)dy * (double)dy);   /* :47 */
    float dist = pf(f, s->distance), rad = pf(f, s->targetRadius);
    float vel[2]; go_velocity(w, t, vel);
    if (!(away > dist - rad) || !(away < dist + rad)) {                /* :49 outside the band */
        float v[2];
        v[0] = tp[0] + off[0] - sp[0]; v[1] = tp[1] + off[1] - sp[1];
        clamp_magnitude2(v, 1.0f);
        float force = pf(f, s->accelerationForce);
        v[0] *= force; v[1] *= force;
        if (away < dist) { v[0] = 0.0f - v[0]; v[1] = 0.0f - v[1]; }   /* :55-58 too close: push away */
        go_add_force(w, t, v);
        clamp_magnitude2(vel, pf(f, s->speedMax));
        go_set_velocity(w, t, vel);
        return;
    }
    float dec = pf(f, s->deceleration);                                /* :66-99 inside the band: damp */
    vel[0] = act_damp_axis(vel[0], dec);
    vel[1] = act_damp_axis(vel[1], dec);
    go_set_velocity(w, t, vel);
}
static void dfs_enter(act_inst *a) { dfs_do(a); }                      /* :31-36 cache + DoChase, never Finish()es */
static const act_vtable AV_DistanceFlySmooth = { "DistanceFlySmooth", sizeof(st_dfs), dfs_bind, dfs_enter, NULL, dfs_do, NULL, NULL, NULL, NULL };

/* ChaseObject - ACT/ChaseObject.cs DoBuzz: nudge each velocity axis by +/-acceleration toward (target +
 * spread), then clamp each axis to +/-speedMax (no force, no ClampMagnitude).  With targetSpread > 0, once the
 * timer passes spreadResetTime it draws spreadX, spreadY, then the next spreadResetTime; otherwise the timer
 * advances by deltaTime.  HandleFixedUpdate is set in Awake and OnPreprocess. */
typedef struct {
    const fsm_pv *go, *target, *speedMax, *acceleration, *targetSpread, *spreadMin, *spreadMax;
    float timer, spread_reset_time, spread_x, spread_y;
} st_chase;
static void chase_bind(act_inst *a)
{
    ST(st_chase);
    s->go = FIELD(gameObject); s->target = FIELD(target); s->speedMax = FIELD(speedMax);
    s->acceleration = FIELD(acceleration); s->targetSpread = FIELD_OPT(targetSpread);
    s->spreadMin = FIELD_OPT(spreadResetTimeMin); s->spreadMax = FIELD_OPT(spreadResetTimeMax);
    s->timer = 0.0f; s->spread_reset_time = 0.0f; s->spread_x = 0.0f; s->spread_y = 0.0f;
}
static void chase_do(act_inst *a)
{
    ST(st_chase);
    int32_t self = p_owner_default(a, s->go);
    int32_t tgt = pgo(f, s->target);
    if (self < 0 || tgt < 0 || !go_has_rb(w, self)) return;         /* rb2d == null -> return */
    if (s->targetSpread && pf(f, s->targetSpread) > 0.0f) {
        if (s->timer >= s->spread_reset_time) {
            float sp = pf(f, s->targetSpread);
            s->spread_x = hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - sp, sp);
            s->spread_y = hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - sp, sp);
            s->timer = 0.0f;
            s->spread_reset_time = hk_rng_range_f_site(w->rng, a->rng_site,
                                                       s->spreadMin ? pf(f, s->spreadMin) : 0.0f,
                                                       s->spreadMax ? pf(f, s->spreadMax) : 0.0f);
        } else {
            s->timer += w->dt;
        }
    }
    float v[2]; go_velocity(w, self, v);
    float ps_[3], pt[3]; go_world_pos(w, self, ps_); go_world_pos(w, tgt, pt);
    float acc = pf(f, s->acceleration), vmax = pf(f, s->speedMax);
    v[0] += (ps_[0] < pt[0] + s->spread_x) ? acc : -acc;
    v[1] += (ps_[1] < pt[1] + s->spread_y) ? acc : -acc;
    if (v[0] > vmax) v[0] = vmax;
    if (v[0] < -vmax) v[0] = -vmax;
    if (v[1] > vmax) v[1] = vmax;
    if (v[1] < -vmax) v[1] = -vmax;
    go_set_velocity(w, self, v);
}
static void chase_enter(act_inst *a) { chase_do(a); }
static const act_vtable AV_ChaseObject = { "ChaseObject", sizeof(st_chase), chase_bind, chase_enter, NULL, chase_do, NULL, NULL, NULL, NULL };

/* IdleBuzz / IdleBuzzV3 - ACT/IdleBuzz.cs and IdleBuzzV3.cs: a flyer wandering around a remembered start point.
 * Per fixed step: bounce off the roaming box, then on a countdown pick fresh random accelerations, integrate
 * velocity and clamp to speedMax.  V3 differs in three ways: separate roamingRangeX / roamingRangeY, the in-box
 * accel draw floors at accelerationMin (0 in IdleBuzz), and manualStartPos can override the start.
 * Draws per call: up to one Range(waitMin, waitMax) from the Y boundary branch (past the edge AND still moving
 * out), the same for X, then on expiry accelY, accelX and one more Range(waitMin, waitMax) -- the three-way test
 * picks each accel's range, not whether to draw.  The /2000 and the 1.125 dampener are literal from the source. */
typedef struct {
    const fsm_pv *go, *waitMin, *waitMax, *speedMax, *accelMin, *accelMax, *rangeX, *rangeY, *manualStart;
    float start_x, start_y, accel_x, accel_y, wait;
    int use_accel_min;          /* V3 floors the in-box draw at accelerationMin; IdleBuzz floors at 0 */
} st_buzz;

static void buzz_bind_common(act_inst *a)
{
    ST(st_buzz);
    s->go = FIELD_OPT(gameObject); s->waitMin = FIELD(waitMin); s->waitMax = FIELD(waitMax);
    s->speedMax = FIELD(speedMax); s->accelMax = FIELD(accelerationMax);
    s->accel_x = s->accel_y = s->wait = 0.0f; s->start_x = s->start_y = 0.0f;
}
static void buzz_bind(act_inst *a)
{
    ST(st_buzz);
    buzz_bind_common(a);
    s->rangeX = s->rangeY = FIELD(roamingRange);      /* one range for both axes */
    s->accelMin = NULL; s->manualStart = NULL; s->use_accel_min = 0;
}
static void buzzv3_bind(act_inst *a)
{
    ST(st_buzz);
    buzz_bind_common(a);
    s->rangeX = FIELD(roamingRangeX); s->rangeY = FIELD(roamingRangeY);
    s->accelMin = FIELD_OPT(accelerationMin); s->manualStart = FIELD_OPT(manualStartPos);
    s->use_accel_min = 1;
}

#define BUZZ_EPS 1.401298464e-45f      /* Mathf.Epsilon */

static void buzz_do(act_inst *a)
{
    ST(st_buzz);
    int32_t self = p_owner_default(a, s->go);
    if (self < 0 || !go_has_rb(w, self)) return;              /* rb2d == null -> return */
    float v[2]; go_velocity(w, self, v);
    float pos[3]; go_world_pos(w, self, pos);
    float amax = pf(f, s->accelMax);
    float amin = (s->use_accel_min && s->accelMin) ? pf(f, s->accelMin) : 0.0f;
    float rx = pf(f, s->rangeX), ry = pf(f, s->rangeY);
    float wmin = pf(f, s->waitMin), wmax = pf(f, s->waitMax);

    if (pos[1] < s->start_y - ry) {
        if (v[1] < 0.0f) {
            s->accel_y = amax / 2000.0f; v[1] /= 1.125f;
            s->wait = hk_rng_range_f_site(w->rng, a->rng_site, wmin, wmax);
        }
    } else if (pos[1] > s->start_y + ry && v[1] > 0.0f) {
        s->accel_y = (0.0f - amax) / 2000.0f; v[1] /= 1.125f;
        s->wait = hk_rng_range_f_site(w->rng, a->rng_site, wmin, wmax);
    }
    if (pos[0] < s->start_x - rx) {
        if (v[0] < 0.0f) {
            s->accel_x = amax / 2000.0f; v[0] /= 1.125f;
            s->wait = hk_rng_range_f_site(w->rng, a->rng_site, wmin, wmax);
        }
    } else if (pos[0] > s->start_x + rx && v[0] > 0.0f) {
        s->accel_x = (0.0f - amax) / 2000.0f; v[0] /= 1.125f;
        s->wait = hk_rng_range_f_site(w->rng, a->rng_site, wmin, wmax);
    }

    if (s->wait <= BUZZ_EPS) {
        /* the three-way test picks the RANGE; the draw happens either way */
        if (pos[1] < s->start_y - ry)      s->accel_y = hk_rng_range_f_site(w->rng, a->rng_site, amin, amax);
        else if (pos[1] > s->start_y + ry) s->accel_y = hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - amax, amin);
        else                               s->accel_y = hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - amax, amax);
        if (pos[0] < s->start_x - rx)      s->accel_x = hk_rng_range_f_site(w->rng, a->rng_site, amin, amax);
        else if (pos[0] > s->start_x + rx) s->accel_x = hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - amax, amin);
        else                               s->accel_x = hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - amax, amax);
        s->accel_y /= 2000.0f; s->accel_x /= 2000.0f;
        s->wait = hk_rng_range_f_site(w->rng, a->rng_site, wmin, wmax);
    }
    if (s->wait > BUZZ_EPS) s->wait -= w->dt;

    v[0] += s->accel_x; v[1] += s->accel_y;
    float vmax = pf(f, s->speedMax);
    if (v[0] > vmax) v[0] = vmax;
    if (v[0] < -vmax) v[0] = -vmax;
    if (v[1] > vmax) v[1] = vmax;
    if (v[1] < -vmax) v[1] = -vmax;
    go_set_velocity(w, self, v);
}
static void buzz_enter(act_inst *a)
{
    ST(st_buzz);
    int32_t self = p_owner_default(a, s->go);
    if (self >= 0) { float p3[3]; go_world_pos(w, self, p3); s->start_x = p3[0]; s->start_y = p3[1]; }
    if (s->manualStart && !p_isnone(s->manualStart)) {        /* V3 :OnEnter override */
        const float *q = pv3(f, s->manualStart); s->start_x = q[0]; s->start_y = q[1];
    }
    buzz_do(a);
}
static const act_vtable AV_IdleBuzz   = { "IdleBuzz",   sizeof(st_buzz), buzz_bind,   buzz_enter, NULL, buzz_do, NULL, NULL, NULL, NULL };
static const act_vtable AV_IdleBuzzV3 = { "IdleBuzzV3", sizeof(st_buzz), buzzv3_bind, buzz_enter, NULL, buzz_do, NULL, NULL, NULL, NULL };

/* DistanceFly — ACT/DistanceFly.cs:52-144 (Awake/OnPreprocess set HandleFixedUpdate; OnEnter and OnFixedUpdate
 * both run DoBuzz).  `self`/rb2d resolve the same FsmOwnerDefault, cached together like the other
 * RigidBody2dActionBase actions in this file; target is dereferenced unconditionally in the C# (every dumped
 * use sets it, so a missing one traps rather than silently returning).  The two y branches (the plain
 * distance-chase pair and, separately, the targetsHeight pair) are mutually exclusive `if`/`else` blocks in
 * the source and stay that way here, not folded together. */
typedef struct { const fsm_pv *go, *target, *distance, *speedMax, *acceleration, *targetsHeight, *height; int32_t cached_go; bool has; } st_dfly;
static void dfly_bind(act_inst *a)
{
    ST(st_dfly);
    s->go = FIELD(gameObject); s->target = FIELD_OPT(target); s->distance = FIELD(distance);
    s->speedMax = FIELD(speedMax); s->acceleration = FIELD(acceleration); s->targetsHeight = FIELD(targetsHeight);
    s->height = FIELD_OPT(height); s->cached_go = -1;
}
static void dfly_do(act_inst *a)
{
    ST(st_dfly);
    int32_t self = p_owner_default(a, s->go);
    if (!act_cache_rb(a, self, &s->cached_go, &s->has)) return;      /* :66-69 rb2d == null -> return */
    int32_t tgt = pgo(f, s->target);
    HKSIM_ASSERT(tgt >= 0, "DistanceFly: null target (NullReferenceException in C#) in %s", fsm_label(f));
    float sp[3], tp[3]; go_world_pos(w, self, sp); go_world_pos(w, tgt, tp);
    float dx = sp[0] - tp[0], dy = sp[1] - tp[1];
    float distanceAway = m_sqrt(m_pow(dx, 2.0f) + m_pow(dy, 2.0f));  /* :70 Mathf.Sqrt(Mathf.Pow(dx,2)+Mathf.Pow(dy,2)) */
    float v[2]; go_velocity(w, self, v);
    float accel = pf(f, s->acceleration);
    bool th = pb(f, s->targetsHeight);
    if (distanceAway > pf(f, s->distance)) {
        v[0] += (sp[0] < tp[0]) ? accel : -accel;
        if (!th) v[1] += (sp[1] < tp[1]) ? accel : -accel;
    } else {
        v[0] += (sp[0] < tp[0]) ? -accel : accel;
        if (!th) v[1] += (sp[1] < tp[1]) ? -accel : accel;
    }
    if (th) {
        float h = pf(f, s->height);
        if (sp[1] < tp[1] + h) v[1] += accel;
        if (sp[1] > tp[1] + h) v[1] -= accel;
    }
    float vmax = pf(f, s->speedMax);
    if (v[0] > vmax) v[0] = vmax;
    if (v[0] < -vmax) v[0] = -vmax;
    if (v[1] > vmax) v[1] = vmax;
    if (v[1] < -vmax) v[1] = -vmax;
    go_set_velocity(w, self, v);
}
static const act_vtable AV_DistanceFly = { "DistanceFly", sizeof(st_dfly), dfly_bind, dfly_do, NULL, dfly_do, NULL, NULL, NULL, NULL };

/* DistanceWalk — ACT/DistanceWalk.cs:62-175 (Awake/OnPreprocess set HandleFixedUpdate; DoWalk on OnEnter and
 * OnFixedUpdate, changeTimer counted down on OnUpdate).  changeAnimation is True on every dumped use
 * (GG_Mage_Knight(_V), analysis/fsm), so the Play(forward/backAnimation) branch is ported; `animator` is
 * dereferenced unconditionally when changeAnimation (a missing tk2dSpriteAnimator would NPE in the C# too).
 * `randomStart` is written but never read anywhere in the class -- not ported. */
typedef struct {
    const fsm_pv *go, *target, *distance, *speed, *range, *changeAnimation, *spriteFacesRight, *forwardAnimation, *backAnimation;
    int32_t cached_go, self, sprite; bool has, movingRight; float changeTimer;
} st_dwalk;
static void dwalk_bind(act_inst *a)
{
    ST(st_dwalk);
    s->go = FIELD(gameObject); s->target = FIELD_OPT(target); s->distance = FIELD(distance); s->speed = FIELD(speed);
    s->range = FIELD(range); s->changeAnimation = FIELD(changeAnimation); s->spriteFacesRight = FIELD(spriteFacesRight);
    s->forwardAnimation = FIELD(forwardAnimation); s->backAnimation = FIELD(backAnimation);
    s->cached_go = -1; s->self = -1; s->sprite = -1;
}
static void dwalk_walk(act_inst *a)
{
    ST(st_dwalk);
    if (!act_cache_rb(a, s->self, &s->cached_go, &s->has)) return;   /* :88-91 rb2d == null -> return */
    int32_t tgt = pgo(f, s->target);
    HKSIM_ASSERT(tgt >= 0, "DistanceWalk: null target (NullReferenceException in C#) in %s", fsm_label(f));
    float sp[3], tp[3]; go_world_pos(w, s->self, sp); go_world_pos(w, tgt, tp);
    float distanceAway = sp[0] - tp[0]; if (distanceAway < 0.0f) distanceAway = -distanceAway;
    float v[2]; go_velocity(w, s->self, v);
    float speed = pf(f, s->speed), dist = pf(f, s->distance), range = pf(f, s->range);
    if (distanceAway > dist + range) {
        if (sp[0] < tp[0]) { if (!s->movingRight && s->changeTimer <= 0.0f) { v[0] = speed; s->movingRight = true; s->changeTimer = 0.6f; } }
        else if (s->movingRight && s->changeTimer <= 0.0f) { v[0] = -speed; s->movingRight = false; s->changeTimer = 0.6f; }
    } else if (distanceAway < dist - range) {
        if (sp[0] < tp[0]) { if (s->movingRight && s->changeTimer <= 0.0f) { v[0] = -speed; s->movingRight = false; s->changeTimer = 0.6f; } }
        else if (!s->movingRight && s->changeTimer <= 0.0f) { v[0] = speed; s->movingRight = true; s->changeTimer = 0.6f; }
    }
    if (v[0] > -0.1f && v[0] < 0.1f) {                                /* :134 reads rb2d.velocity.x fresh, == v[0] here */
        if (hk_rng_value_site(w->rng, a->rng_site) > 0.5f) { v[0] = speed; s->movingRight = true; }
        else { v[0] = -speed; s->movingRight = false; }
    }
    go_set_velocity(w, s->self, v);
    if (!pb(f, s->changeAnimation)) return;
    anim_inst *sprite = anim_of_go(w, s->self);
    HKSIM_ASSERT(sprite != NULL, "DistanceWalk: no tk2dSpriteAnimator on %s (NullReferenceException in C#)", go_path(w, s->self));
    bool right = pb(f, s->spriteFacesRight);
    float lsc[3]; go_local_scale(w, s->self, lsc); float sx = lsc[0];
    bool forward = (right && s->movingRight) || (!right && !s->movingRight);
    if ((sx > 0.0f) == forward) anim_play_name(w, sprite, w_str(w, ps(f, s->forwardAnimation)));
    else anim_play_name(w, sprite, w_str(w, ps(f, s->backAnimation)));
}
static void dwalk_enter(act_inst *a)
{
    ST(st_dwalk);
    s->self = p_owner_default(a, s->go);
    dwalk_walk(a);
}
static void dwalk_update(act_inst *a) { ST(st_dwalk); if (s->changeTimer > 0.0f) s->changeTimer -= w->dt; }
static const act_vtable AV_DistanceWalk = { "DistanceWalk", sizeof(st_dwalk), dwalk_bind, dwalk_enter, dwalk_update, dwalk_walk, NULL, NULL, NULL, NULL };

/* ProjectileSquash — ACT/ProjectileSquash.cs:47-81 (Awake/OnPreprocess set HandleFixedUpdate; DoStretch on
 * OnEnter and OnFixedUpdate).  target's z is kept from its current localScale.z (never written here). */
typedef struct { const fsm_pv *go, *stretchFactor, *stretchMinX, *stretchMaxY, *scaleModifier, *everyFrame; int32_t cached_go, target; bool has; } st_psq;
static void psq_bind(act_inst *a)
{
    ST(st_psq);
    s->go = FIELD(gameObject); s->stretchFactor = FIELD(stretchFactor); s->stretchMinX = FIELD(stretchMinX);
    s->stretchMaxY = FIELD(stretchMaxY); s->scaleModifier = FIELD(scaleModifier); s->everyFrame = FIELD(everyFrame);
    s->cached_go = -1; s->target = -1;
}
static void psq_do(act_inst *a)
{
    ST(st_psq);
    if (!act_cache_rb(a, s->target, &s->cached_go, &s->has)) return;
    float v[2]; go_velocity(w, s->target, v);
    float speed = m_sqrt(v[0] * v[0] + v[1] * v[1]);                 /* Vector2.magnitude */
    float factor = pf(f, s->stretchFactor);
    float sy = 1.0f - speed * factor * 0.01f, sx = 1.0f + speed * factor * 0.01f;
    float minx = pf(f, s->stretchMinX), maxy = pf(f, s->stretchMaxY);
    if (sx < minx) sx = minx;
    if (sy > maxy) sy = maxy;
    float mod = pf(f, s->scaleModifier);
    sy *= mod; sx *= mod;
    float ls[3]; go_local_scale(w, s->target, ls);
    ls[0] = sx; ls[1] = sy;
    go_set_local_scale(w, s->target, ls);
}
static void psq_enter(act_inst *a)
{
    ST(st_psq);
    s->target = p_owner_default(a, s->go);
    psq_do(a);
    if (!pb(f, s->everyFrame)) act_finish(a);
}
static const act_vtable AV_ProjectileSquash = { "ProjectileSquash", sizeof(st_psq), psq_bind, psq_enter, NULL, psq_do, NULL, NULL, NULL, NULL };

/* GhostMovement — ACT/GhostMovement.cs:63-145 (Awake/OnPreprocess set HandleFixedUpdate; DoMove on OnEnter
 * and OnFixedUpdate).  direction_x/direction_y are FsmInt VARIABLES (read and written back, unlike the other
 * actions' private cached fields); no dumped scene uses this action (GG_Mighty_Zote has no dump), ported from
 * the source alone. */
typedef struct { const fsm_pv *go, *xPosMin, *xPosMax, *accel_x, *speedMax_x, *yPosMin, *yPosMax, *accel_y, *speedMax_y, *direction_x, *direction_y; int32_t cached_go, self; bool has; } st_ghost;
static void ghost_bind(act_inst *a)
{
    ST(st_ghost);
    s->go = FIELD(gameObject); s->xPosMin = FIELD(xPosMin); s->xPosMax = FIELD(xPosMax); s->accel_x = FIELD(accel_x);
    s->speedMax_x = FIELD(speedMax_x); s->yPosMin = FIELD(yPosMin); s->yPosMax = FIELD(yPosMax);
    s->accel_y = FIELD(accel_y); s->speedMax_y = FIELD(speedMax_y); s->direction_x = FIELD(direction_x);
    s->direction_y = FIELD(direction_y); s->cached_go = -1; s->self = -1;
}
static void ghost_do(act_inst *a)
{
    ST(st_ghost);
    if (!act_cache_rb(a, s->self, &s->cached_go, &s->has)) return;
    float pos[3]; go_world_pos(w, s->self, pos);
    float v[2]; go_velocity(w, s->self, v);
    float axmax = pf(f, s->speedMax_x), ax = pf(f, s->accel_x);
    if (pi(f, s->direction_x) == 0) {
        if (v[0] > -axmax) { v[0] -= ax; if (v[0] < -axmax) v[0] = -axmax; }
        if (pos[0] < pf(f, s->xPosMin)) pi_set(f, s->direction_x, 1);
    } else {
        if (v[0] < axmax) { v[0] += ax; if (v[0] > axmax) v[0] = axmax; }
        if (pos[0] > pf(f, s->xPosMax)) pi_set(f, s->direction_x, 0);
    }
    float aymax = pf(f, s->speedMax_y), ay = pf(f, s->accel_y);
    if (pi(f, s->direction_y) == 0) {
        if (v[1] > -aymax) { v[1] -= ay; if (v[1] < -aymax) v[1] = -aymax; }
        if (pos[1] < pf(f, s->yPosMin)) pi_set(f, s->direction_y, 1);
    } else {
        if (v[1] < aymax) { v[1] += ay; if (v[1] > aymax) v[1] = aymax; }
        if (pos[1] > pf(f, s->yPosMax)) pi_set(f, s->direction_y, 0);
    }
    go_set_velocity(w, s->self, v);
}
static void ghost_enter(act_inst *a) { ST(st_ghost); s->self = p_owner_default(a, s->go); ghost_do(a); }
static const act_vtable AV_GhostMovement = { "GhostMovement", sizeof(st_ghost), ghost_bind, ghost_enter, NULL, ghost_do, NULL, NULL, NULL, NULL };

const act_vtable *const act_registry_movement[] = {
    &AV_ChaseObjectGround, &AV_ChaseObjectV2, &AV_DistanceFlySmooth, &AV_ChaseObject, &AV_IdleBuzz,
    &AV_IdleBuzzV3, &AV_DistanceFly, &AV_DistanceWalk, &AV_ProjectileSquash, &AV_GhostMovement,
};
const int act_registry_movement_n = (int)(sizeof act_registry_movement / sizeof act_registry_movement[0]);
