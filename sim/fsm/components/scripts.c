/* Small MonoBehaviours whose serialized fields ride in comp_def (fsm_tables.h, gen_tables.py SCRIPTS).  lifecycle.c
 * registers one component per instance (LCT_SCR_*) and calls these bodies from its callbacks; each keeps its private
 * fields in the component's scr_state.  Ports of analysis/decomp/Assembly-CSharp/<Class>.cs. */
#include "components.h"
#include "../lifecycle.h"
#include <math.h>

/* RandomScale.ApplyScale -- RandomScale.cs:30-34: localScale = Vector3.one * Random.Range(minScale, maxScale) */
static void random_scale_apply(fsm_world *w, int32_t go, const comp_def *d, scr_state *st)
{
    float s = hk_rng_range_f_site(w->rng, hk_rng_site(go_path(w, go), "RandomScale", "ApplyScale", 0), d->f[0], d->f[1]);
    const float v[3] = { s, s, s };
    go_set_local_scale(w, go, v);
    st->flag = 1;                                                     /* didScale */
}

/* EnemyDreamnailReaction.States -- EnemyDreamnailReaction.cs:5-10 */
enum { DREAM_SUPPRESSED = 0, DREAM_READY = 1, DREAM_COOLING = 2 };

/* Corpse.States -- Corpse.cs:6-13.  DeathAnimation (spriteAnimator-driven) never has a dumped instance reach it
 * without also reaching InAir or PendingLandEffects first, so it needs no scr_update case of its own: the game
 * ends Corpse.Update's DeathAnimation branch by Completing once its animator stops playing, which this port never
 * starts (playAnimationOnBounce/animation calls are cosmetic; see scr_start). */
enum { CORPSE_NOT_STARTED = 0, CORPSE_IN_AIR = 1, CORPSE_DEATH_ANIM = 2, CORPSE_COMPLETE = 3, CORPSE_PENDING_LAND = 4 };

/* EnemyBullet's own state (no C# enum: `active` plus the Collision coroutine's two waits) */
enum { EBULLET_FLYING = 0, EBULLET_IMPACT_WAIT = 1, EBULLET_IMPACT_ANIM = 2 };

/* Walker.States -- Walker.cs:6-13.  NotReady/WaitingForConditions are not encoded: Start() (:129-142, the only
 * writer that reaches them) never reruns for an object already Started at the dump (docs/porting.md #1), and
 * gen_tables.py's _walker traps a dumped instance in either state. */
enum { WALKER_STOPPED = 0, WALKER_WALKING = 1, WALKER_TURNING = 2 };
enum { WALKER_BORED = 0, WALKER_CONTROLLED = 1 };   /* Walker.StopReasons -- :15-19 */
static void walker_tick(fsm_world *w, int32_t go, const comp_def *d, scr_state *st);   /* defined below; scr_update and
                                                                                          * the public entry points both call it */
static float v2_mag(float x, float y);   /* Vector2.magnitude, defined below with ObjectBounce; scr_update also uses it */

void scr_start(fsm_world *w, int type, int32_t go, const comp_def *d, scr_state *st)
{
    switch (type) {
    case LCT_SCR_RANDOM_SCALE:                                        /* RandomScale.Start :14-20 */
        if (!st->flag) random_scale_apply(w, go, d, st);
        break;
    case LCT_SCR_ENEMY_MESSAGE: {                                     /* SendEnemyMessageTrigger.Start :9-20 */
        int32_t fi = world_fsm_find(w, go, "enemy_message");
        if (fi >= 0) {
            int32_t vi = fsm_find_var(&w->fsms[fi], VB_STRING, w_find_string(w, "Event"));
            if (vi >= 0) st->e[0] = w->fsms[fi].vals[vi].i;            /* FindFsmString: locals only */
            lc_fsm_set_enabled(w, fi, false);
        }
        break;
    }
    case LCT_SCR_DREAM_REACTION:                                      /* EnemyDreamnailReaction.Start :38-41 */
        st->state = (d->i[2] & 1) ? DREAM_SUPPRESSED : DREAM_READY;
        break;
    case LCT_SCR_OBJECT_BOUNCE:                                       /* ObjectBounce.Start :53-62 */
        if (d->i[0]) HKSIM_UNIMPLEMENTED("ObjectBounce on '%s': playAnimationOnBounce / sendFSMEvent set (no dumped "
                                         "instance sets one)", go_path(w, go));
        st->flag = 1;                                                 /* rb = GetComponent<Rigidbody2D>() */
        break;
    case LCT_SCR_BREAKABLE:                                           /* Breakable.Start :188-211 */
        HKSIM_UNIMPLEMENTED("Breakable.Start on '%s': its inert-depth Destroy(this) and thresholds are not carried; "
                            "every ported instance Started before the dump", go_path(w, go));
    case LCT_SCR_CORPSE:                                              /* Corpse.Start :120-167.  massless=true (state
         * would start DeathAnimation instead), instantChunker && !breaker (an immediate Land()), and the COLOSSEUM
         * GetCurrentMapZone DropThroughFloor coroutine (:162-165, unreachable: no GG_ scene's zone is "COLOSSEUM")
         * all trap in gen_tables.py's _corpse (no dumped instance sets massless/instantChunker); resetRotation,
         * noSteam, spellBurn trap there too.  startAudio, corpseSteam/corpseFlame and the "Death Air" animator play
         * are cosmetic (audio/rendering, out of scope). */
        st->state = CORPSE_IN_AIR;                                   /* :148 (massless excluded above) */
        break;
    case LCT_SCR_PARTICLE_AUTO_DISABLE:                               /* ParticleSystemAutoDisable.Update :19-28: not
         * ported -- the sim does not simulate ParticleSystem.IsAlive() (see gen_tables.py's SCRIPTS entry) */
        HKSIM_UNIMPLEMENTED("ParticleSystemAutoDisable on '%s': particle playback is not simulated", go_path(w, go));
    default: break;
    }
}

/* The private fields each script held at the dump, from the payload (gen_tables.py SCRIPTS: the dumped values, or the
 * field initialisers for an object the dump does not carry). */
void scr_restore(fsm_world *w, int type, int32_t go, const comp_def *d, scr_state *st)
{
    (void)go;
    switch (type) {
    case LCT_SCR_RANDOM_SCALE: st->flag = (uint8_t)d->i[1]; break;   /* didScale */
    case LCT_SCR_DEACT_DELAY: st->t = d->f[1]; break;                 /* timer */
    case LCT_SCR_DISABLE_TIME: st->t = w->time + d->f[1]; break;      /* disableTime, carried relative to the dump's Time.time */
    case LCT_SCR_ENEMY_MESSAGE: st->e[0] = d->i[1]; break;            /* eventName as Start left it (-1: not dumped) */
    case LCT_SCR_DREAM_REACTION:                                      /* state, cooldownTimeRemaining */
        if (d->i[2] >> 3) st->state = (uint8_t)((d->i[2] >> 3) - 1);
        st->t = d->f[0];
        break;
    case LCT_SCR_OBJECT_BOUNCE: {                                     /* bouncing, rb, stepCounter, velocity, lastPos, speed */
        st->state = (d->i[1] & 1) ? 0 : 1;
        st->flag = (uint8_t)((d->i[1] >> 1) & 1);
        st->n_e = d->i[2];
        const float *sf = &w->sc->script_floats[d->i[3]];            /* {velocity.x,y, lastPos.x,y, speed} */
        st->v[0] = sf[0]; st->v[1] = sf[1]; st->v[2] = sf[2]; st->v[3] = sf[3]; st->v[4] = sf[4];
        break;
    }
    case LCT_SCR_BREAKABLE:                                           /* angleOffset, isBroken */
        st->v[0] = d->f[0];
        st->flag = (uint8_t)((d->i[2] >> 1) & 1);
        break;
    case LCT_SCR_CORPSE_BIT_END: st->t = d->f[1]; st->flag = (uint8_t)d->i[0]; break;   /* timer, stopped */
    case LCT_SCR_WALKER: {                                            /* state, stopReason, currentFacing, turningFacing
         * (gen_tables.py _walker's packed i[3]) and turnCooldownRemaining/walkTimeRemaining/pauseTimeRemaining
         * (the i[2] float list): every dumped instance is Started (a generator trap otherwise), so this is the
         * only place Walker's per-instance state is ever set (no scr_start case: see the enum note) */
        int32_t bits = d->i[3];
        st->state = (uint8_t)(bits & 3);
        st->flag = (uint8_t)((bits >> 2) & 1);
        st->e[0] = ((bits >> 3) & 3) - 1;
        st->e[1] = ((bits >> 5) & 3) - 1;
        const float *dyn = &w->sc->script_floats[d->i[2]];
        st->t = dyn[0]; st->v[0] = dyn[1]; st->v[1] = dyn[2];
        break;
    }
    default: break;
    }
}

void scr_on_enable(fsm_world *w, int type, int32_t go, const comp_def *d, scr_state *st)
{
    switch (type) {
    case LCT_SCR_RANDOM_SCALE:                                        /* RandomScale.OnEnable :22-28 */
        if (d->i[0]) random_scale_apply(w, go, d, st);
        break;
    case LCT_SCR_DEACT_PD_TRUE:                                       /* DeactivateIfPlayerdataTrue.OnEnable :17-31 */
    case LCT_SCR_DEACT_PD_FALSE: {                                    /* DeactivateIfPlayerdataFalse.OnEnable :17-31 */
        bool v = world_pd_bool(w, w_str(w, d->i[0]));
        if (v == (type == LCT_SCR_DEACT_PD_TRUE)) go_set_active(w, go, false);
        break;
    }
    case LCT_SCR_DEACT_DELAY:                                         /* DeactivateAfterDelay.OnEnable :19-27 */
        st->t = d->f[0];                                              /* timer = time */
        if (d->i[0]) {                                                /* stayInPlace (startPos from Awake, lifecycle.c) */
            go_set_local_pos(w, go, &st->v[3]);
            go_world_pos(w, go, st->v);                               /* worldPos */
        }
        break;
    case LCT_SCR_DISABLE_TIME:                                        /* DisableAfterTime.OnEnable :9-12 */
        st->t = w->time + d->f[0];                                    /* disableTime = Time.time + waitTime */
        break;
    case LCT_SCR_HIVE_STINGER:                                        /* HiveKnightStinger.OnEnable :19-30. startPos
         * (localPosition) captured once, in v[0..2]; rb caching is not ported, the sim always resolves it */
        if (!st->flag) { go_local_pos(w, go, st->v); st->flag = 1; }
        else go_set_local_pos(w, go, st->v);
        st->t = 2.0f;                                                 /* time (HiveKnightStinger.cs:9) */
        break;
    case LCT_SCR_ENEMY_BULLET: {                                      /* EnemyBullet.OnEnable :41-48 */
        st->flag = 1;                                                 /* active = true */
        st->state = EBULLET_FLYING;
        const float *cfg = &w->sc->script_floats[d->i[0]];            /* {scaleMin,scaleMax,stretchFactor,stretchMinX,stretchMaxY} */
        st->v[0] = hk_rng_range_f_site(w->rng, hk_rng_site(go_path(w, go), "EnemyBullet", "OnEnable", 0), cfg[0], cfg[1]);
        col_inst *c = go_first_collider(w, go);
        if (c) col_set_enabled(w, c, true);
        go_set_kinematic(w, go, false);
        const float zero[2] = { 0.0f, 0.0f };
        go_set_velocity(w, go, zero);
        anim_inst *a = anim_of_go(w, go);
        if (a) anim_play_name(w, a, "Idle");                          /* also resets Playing for scr_update's EBULLET_IMPACT_ANIM read */
        break;
    }
    default: break;
    }
}

void scr_update(fsm_world *w, int type, int32_t go, const comp_def *d, scr_state *st)
{
    switch (type) {
    case LCT_SCR_KEEP_SCALE_POSITIVE: {                               /* KeepWorldScalePositive.Update :7-13 */
        float lossy[3]; go_lossy_scale(w, go, lossy);
        if (lossy[0] < 0.0f) {
            float ls[3]; go_local_scale(w, go, ls);
            ls[0] = 0.0f - ls[0];
            go_set_local_scale(w, go, ls);
        }
        break;
    }
    case LCT_SCR_DEACT_DELAY:                                         /* DeactivateAfterDelay.Update :29-44 */
        if (st->t > 0.0f) {
            st->t -= w->dt;
            if (d->i[0]) go_set_world_pos(w, go, st->v);
        } else {
            go_set_active(w, go, false);
        }
        break;
    case LCT_SCR_DISABLE_TIME:                                        /* DisableAfterTime.Update :14-27 */
        if (w->time >= st->t) {
            if (d->i[0] >= 0 && w_str(w, d->i[0])[0]) world_send_event_to_go(w, go, w_str(w, d->i[0]), false);
            else go_set_active(w, go, false);
        }
        break;
    case LCT_SCR_OBJECT_BOUNCE:                                       /* ObjectBounce.Update :83-89 */
        if (st->t > 0.0f) st->t -= w->dt;                             /* animTimer */
        break;
    case LCT_SCR_DREAM_REACTION:                                      /* EnemyDreamnailReaction.Update :90-100 */
        if (st->state == DREAM_COOLING) {
            st->t -= w->dt;                                           /* cooldownTimeRemaining -= Time.deltaTime */
            if (st->t <= 0.0f) st->state = DREAM_READY;
        }
        break;
    case LCT_SCR_KEEP_WORLD_POS: {                                    /* KeepWorldPosition.Update :10-20. f[0]/f[1]
         * are the fixed xPosition/yPosition fields (never restored: no dumped instance sets keepX or keepY, so
         * neither write is reachable in a ported scene; carried anyway, as a config constant). */
        if (!d->i[0] && !d->i[1]) break;
        float p[3]; go_world_pos(w, go, p);
        if (d->i[0]) p[0] = d->f[0];
        if (d->i[1]) p[1] = d->f[1];
        go_set_world_pos(w, go, p);
        break;
    }
    case LCT_SCR_CORPSE_BIT_END:                                      /* CorpseBitEnd.Update :8-27 */
        if (st->t <= 0.0f && !st->flag) {
            if (!go_has_rb(w, go))                                    /* :13-17: `component.velocity` on a null body */
                HKSIM_UNIMPLEMENTED("CorpseBitEnd on '%s': no Rigidbody2D (a NullReferenceException in the game)", go_path(w, go));
            go_set_kinematic(w, go, true);
            const float zero[2] = { 0.0f, 0.0f };
            go_set_velocity(w, go, zero);
            go_set_angular_velocity(w, go, 0.0f);
            scr_object_bounce_set(w, go, false);                      /* :20 GetComponent<ObjectBounce>().StopBounce() */
            go_inst *g = &w->gos[go];
            int32_t k = 0;
            while (k < g->n_cols && g->cols[k].def->type != COL_POLYGON) k++;
            if (k == g->n_cols)                                       /* :21 GetComponent<PolygonCollider2D>().enabled */
                HKSIM_UNIMPLEMENTED("CorpseBitEnd on '%s': no PolygonCollider2D (a NullReferenceException in the game)", go_path(w, go));
            col_set_enabled(w, &g->cols[k], false);
            st->flag = 1;                                             /* stopped */
        } else {
            st->t -= w->dt;
        }
        break;
    case LCT_SCR_KEEP_ROTATION:                                       /* KeepRotation.Update :17-22: localEulerAngles
         * = (0,0,angle) every frame; x/y are already 0 in the 2D transform (B21) */
        go_set_local_euler_z(w, go, d->f[0]);
        break;
    case LCT_SCR_HIVE_STINGER: {                                      /* HiveKnightStinger.Update :33-47. speed = 20
         * (HiveKnightStinger.cs:7); direction is the prefab/dumped field in f[0] */
        float rad = d->f[0] * ((float)M_PI / 180.0f);
        const float v[2] = { 20.0f * m_cos(rad), 20.0f * m_sin(rad) };
        go_set_velocity(w, go, v);
        if (st->t > 0.0f) st->t -= w->dt;
        else go_set_active(w, go, false);
        break;
    }
    case LCT_SCR_CORPSE:
        if (st->state == CORPSE_IN_AIR) {                            /* Corpse.Update :178-185 */
            float p[3]; go_world_pos(w, go, p);
            if (p[1] < -10.0f) {                                     /* :181 transform.position.y < -10f */
                st->state = CORPSE_COMPLETE;                          /* Complete(detachChildren:true,destroyMe:true) */
                lc_destroy_go(w, go, 0.0f, true);
            }
        } else if (st->state == CORPSE_PENDING_LAND) {                /* Corpse.Update :186-193 */
            st->t -= w->dt;                                          /* landEffectsDelayRemaining -= Time.deltaTime */
            if (st->t <= 0.0f) st->state = CORPSE_COMPLETE;           /* Complete(detachChildren:false,destroyMe:false) */
        }
        break;
    case LCT_SCR_ENEMY_BULLET:
        if (st->state == EBULLET_FLYING) {                            /* EnemyBullet.Update :53-68 (active only) */
            float v[2]; go_velocity(w, go, v);
            go_set_local_euler_z(w, go, m_atan2(v[1], v[0]) * (180.0f / (float)M_PI));
            const float *cfg = &w->sc->script_floats[d->i[0]];
            float speed = v2_mag(v[0], v[1]);
            float num = 1.0f - speed * cfg[2] * 0.01f, num2 = 1.0f + speed * cfg[2] * 0.01f;
            if (num2 < cfg[3]) num2 = cfg[3];                          /* stretchMinX */
            if (num > cfg[4]) num = cfg[4];                            /* stretchMaxY */
            num *= st->v[0]; num2 *= st->v[0];                         /* * scale */
            float ls[3]; go_local_scale(w, go, ls);
            const float ns[3] = { num2, num, ls[2] };
            go_set_local_scale(w, go, ns);
        } else if (st->state == EBULLET_IMPACT_WAIT) {                 /* Collision :137 `yield return null`, :138 col.enabled=false */
            col_inst *c = go_first_collider(w, go);
            if (c) col_set_enabled(w, c, false);
            st->state = EBULLET_IMPACT_ANIM;
        } else if (st->state == EBULLET_IMPACT_ANIM) {                 /* :139 WaitForSeconds((frames-1)/fps), approximated
             * by the animator's own Playing flag (the Impact clip is non-looping) rather than a hand-derived duration */
            anim_inst *a = anim_of_go(w, go);
            if (!a || !anim_playing(a)) world_pool_recycle(w, go);     /* :140 gameObject.Recycle() */
        }
        break;
    case LCT_SCR_WALKER:
        walker_tick(w, go, d, st);
        break;
    default: break;
    }
}

/* Vector2.magnitude: (float)Math.Sqrt(x * x + y * y) */
static float v2_mag(float x, float y) { return (float)sqrt((double)(x * x + y * y)); }

/* ObjectBounce keeps velocity in v[0..1], lastPos in v[2..3], speed in v[4], animTimer in t, stepCounter in n_e,
 * !bouncing in state (the field starts true, :47) and "Start ran" (rb assigned) in flag. */
void scr_fixed_update(fsm_world *w, int type, int32_t go, const comp_def *d, scr_state *st)
{
    (void)d;
    if (type == LCT_SCR_ENEMY_MESSAGE) st->n_e = 0;                  /* SendEnemyMessageTrigger.FixedUpdate :21-24 */
    if (type == LCT_SCR_OBJECT_BOUNCE && !st->state) {                /* ObjectBounce.FixedUpdate :64-81 */
        if (st->n_e >= 3) {
            float p[3]; go_world_pos(w, go, p);
            st->v[0] = p[0] - st->v[2]; st->v[1] = p[1] - st->v[3];   /* velocity = position - lastPos */
            st->v[2] = p[0]; st->v[3] = p[1];
            float rv[2] = { 0.0f, 0.0f };
            bool rb = st->flag && go_has_rb(w, go);
            if (rb) go_velocity(w, go, rv);
            st->v[4] = rb ? v2_mag(rv[0], rv[1]) : 0.0f;              /* speed */
            st->n_e = 0;
        } else {
            st->n_e++;
        }
    }
}

/* ObjectBounce.OnCollisionEnter2D -- ObjectBounce.cs:91-126 */
void scr_object_bounce_enter(fsm_world *w, int32_t go, float nx, float ny, uint32_t contact_count)
{
    scr_state *st;
    const comp_def *d = lc_script_def(w, go, LCT_SCR_OBJECT_BOUNCE, &st);
    if (!d) return;
    if (!st->flag || !go_has_rb(w, go) || w->gos[go].kinematic || st->state || !(st->v[4] > d->f[1])) return;   /* :93-96 */
    if (contact_count == 0)
        HKSIM_UNIMPLEMENTED("ObjectBounce on '%s': a collision with no contact point (the Collision2DUtils.cs:26-35 "
                            "fallback normal)", go_path(w, go));
    /* :97-99 Vector3.Reflect(velocity.normalized, normal).normalized; Vector2.Normalize / Vector3.Normalize zero a
     * vector whose magnitude is not above 1e-5 */
    float m = v2_mag(st->v[0], st->v[1]);
    float dx = 0.0f, dy = 0.0f;
    if (m > 1e-5f) { dx = st->v[0] / m; dy = st->v[1] / m; }
    float k = -2.0f * (nx * dx + ny * dy);
    float rx = k * nx + dx, ry = k * ny + dy;
    float rm = v2_mag(rx, ry);
    float ox = 0.0f, oy = 0.0f;
    if (rm > 1e-5f) { ox = rx / rm; oy = ry / rm; }
    float r = hk_rng_range_f_site(w->rng, hk_rng_site(go_path(w, go), "ObjectBounce", "OnCollisionEnter2D", 0), 0.8f, 1.2f);
    float s = st->v[4] * (d->f[0] * r);
    const float v[2] = { ox * s, oy * s };
    go_set_velocity(w, go, v);
    /* :100-111 playSound: its Random.Range draws pick an AudioSource one-shot (audio); :112-121 trap at Start;
     * :122-125 OnBounce: SpellFluke.cs:56 is its only subscriber (analysis/decomp), an UNREACHABLE_PREFABS spell fluke */
}

/* ObjectBounce.StartBounce / StopBounce -- :128-136 */
void scr_object_bounce_set(fsm_world *w, int32_t go, bool bouncing)
{
    scr_state *st;
    if (!lc_script_def(w, go, LCT_SCR_OBJECT_BOUNCE, &st))
        HKSIM_UNIMPLEMENTED("CallMethodProper ObjectBounce on '%s': no ObjectBounce there", go_path(w, go));
    st->state = bouncing ? 0 : 1;
}

/* Sweep.Check -- Sweep.cs:21-56 (game code, not Box2D): `card` a DirectionUtils cardinal (Right=0,Up=1,Left=2,
 * Down=3), `ox,oy` the `offset` parameter (Corpse/Walker both pass transform.position, Walker's hole check adds
 * an extra x offset first).  `ray_count` rays spread across the collider's world half-extent perpendicular to
 * `card`, from (ColliderOffset + ColliderExtents*Direction + spread*sideAxis + Direction*-skinThickness),
 * skinThickness 0.1f (Sweep.cs default), each a Physics2D.Raycast(..., dist+skin, layerMask). */
static bool sweep_check(fsm_world *w, int32_t go, int card, int ray_count, float ox, float oy, float dist, uint32_t layer_mask)
{
    static const float DX[4] = { 1.0f, 0.0f, -1.0f, 0.0f }, DY[4] = { 0.0f, 1.0f, 0.0f, -1.0f };
    float dirx = DX[card & 3], diry = DY[card & 3];
    col_inst *c = go_first_collider(w, go);
    if (!c) return false;
    float mn[2], mx[2]; col_bounds(w, go, c, mn, mx);
    float ex = (mx[0] - mn[0]) * 0.5f, ey = (mx[1] - mn[1]) * 0.5f;    /* collider.bounds.extents */
    float scale[3]; go_local_scale(w, go, scale);
    float coffx = c->offset[0] * scale[0], coffy = c->offset[1] * scale[1];   /* collider.offset * localScale */
    const float skin = 0.1f;
    float vx = coffx + ex * dirx, vy = coffy + ey * diry;
    float spreadx = ex * fabsf(diry), spready = ey * fabsf(dirx);      /* extents * (|dir.y|, |dir.x|) */
    for (int i = 0; i < ray_count; i++) {
        float t = ray_count > 1 ? 2.0f * ((float)i / (float)(ray_count - 1)) - 1.0f : -1.0f;
        float rx = ox + vx + spreadx * t - dirx * skin, ry = oy + vy + spready * t - diry * skin;
        const float origin[2] = { rx, ry }, dir[2] = { dirx, diry };
        if (world_raycast(w, origin, dir, dist + skin, layer_mask, NULL)) return true;   /* triggers included */
    }
    return false;
}

/* Corpse.OnCollision -- Corpse.cs:228-234, called from both OnCollisionEnter2D and OnCollisionStay2D: grounds the
 * corpse once new Sweep(bodyCollider, Down, 3).Check(transform.position, 0.08f, 256) hits (layer 8 = Terrain,
 * physics.json layerNames). */
void scr_corpse_land(fsm_world *w, int32_t go)
{
    scr_state *st;
    const comp_def *d = lc_script_def(w, go, LCT_SCR_CORPSE, &st);
    if (!d || st->state != CORPSE_IN_AIR) return;
    float p[3]; go_world_pos(w, go, p);
    if (!sweep_check(w, go, 3 /* Down */, 3, p[0], p[1], 0.08f, 1u << 8)) return;
    st->t = 1.0f;                                                     /* Corpse.Land :259 landEffectsDelayRemaining = 1f */
    st->state = CORPSE_PENDING_LAND;                                  /* breaker/landEffects/hitAcid trap in gen_tables.py */
}

/* Walker.BeginWalking -- :293-305.  preventScaleChange (:296 SetScaleX) traps clear in gen_tables.py's _walker, so
 * this instance never reaches it; animator.Play(walkClip) and audioSource.Play() are cosmetic (nothing in Walker
 * reads Walking-state animator/audio state, unlike Turning's animator.Playing read below). */
static void walker_begin_walking(fsm_world *w, int32_t go, const comp_def *d, scr_state *st, int32_t facing)
{
    st->state = WALKER_WALKING;
    const float *cfg = &w->sc->script_floats[d->i[0]];
    st->v[0] = hk_rng_range_f_site(w->rng, hk_rng_site(go_path(w, go), "Walker", "BeginWalking", 0), cfg[2], cfg[3]);  /* pauseWaitMin/Max */
    float cur[2]; go_velocity(w, go, cur);
    const float v[2] = { facing > 0 ? cfg[5] : cfg[4], cur[1] };       /* walkSpeedR : walkSpeedL */
    go_set_velocity(w, go, v);
}

/* Walker.BeginTurning -- :338-350. */
static void walker_begin_turning(fsm_world *w, int32_t go, const comp_def *d, scr_state *st, int32_t facing)
{
    st->e[1] = facing;
    int32_t bits = d->i[3];
    if ((bits >> 9) & 1) { st->e[0] = facing; walker_begin_walking(w, go, d, st, facing); return; }   /* preventTurn -> EndTurning */
    st->state = WALKER_TURNING;
    const float *cfg = &w->sc->script_floats[d->i[0]];
    st->t = cfg[7];                                                    /* turnPause */
    float cur[2]; go_velocity(w, go, cur);
    const float v[2] = { 0.0f, cur[1] };
    go_set_velocity(w, go, v);
    anim_inst *a = anim_of_go(w, go);
    if (a) anim_play_name(w, a, w_str(w, d->i[1]));                    /* turnClip */
    /* :348 FSMUtility.SendEventToGameObject(gameObject, "TURN LEFT"/"TURN RIGHT"): no dumped FSM on a Walker's own
     * object has a transition on either name (analysis/fsm), so it is a verified no-op */
}

/* Walker.EndStopping -- :270-281 (currentFacing != 0: Start()'s only writer of 0 never reruns, see the enum note). */
static void walker_end_stopping(fsm_world *w, int32_t go, const comp_def *d, scr_state *st)
{
    int turn_pct = (int32_t)((uint32_t)d->i[3] >> 10) & 0x7f;
    if (hk_rng_range_i_site(w->rng, hk_rng_site(go_path(w, go), "Walker", "EndStopping", 0), 0, 100) < turn_pct)
        walker_begin_turning(w, go, d, st, -st->e[0]);
    else
        walker_begin_walking(w, go, d, st, st->e[0]);
}

/* Walker.BeginStopped -- :249-268 (Bored only: Controlled skips :255-267 entirely, matching UpdateStopping's own
 * `if (stopReason == Bored)` guard, so a Controlled stop just freezes until the next Go/StartMoving/RecieveGoMessage
 * call from hk.c's StartWalker/SendEnemyMessage). */
static void walker_begin_stopped(fsm_world *w, int32_t go, const comp_def *d, scr_state *st, int32_t reason)
{
    st->state = WALKER_STOPPED;
    st->flag = (uint8_t)reason;
    if (reason != WALKER_BORED) return;
    if ((d->i[3] >> 7) & 1) {                                          /* pauses */
        const float *cfg = &w->sc->script_floats[d->i[0]];
        st->v[1] = hk_rng_range_f_site(w->rng, hk_rng_site(go_path(w, go), "Walker", "BeginStopped", 0), cfg[0], cfg[1]);
    } else {
        walker_end_stopping(w, go, d, st);
    }
}

/* Walker.Update -- :129-142.  Called once a frame (scr_update) and once more, synchronously, from inside
 * StartMoving/Go (:176, :201) -- a real quirk of the C#: turnCooldownRemaining ticks down twice in one frame
 * whenever either is called. */
static void walker_tick(fsm_world *w, int32_t go, const comp_def *d, scr_state *st)
{
    st->t -= w->dt;                                                    /* turnCooldownRemaining -= Time.deltaTime */
    const float *cfg = &w->sc->script_floats[d->i[0]];
    int32_t bits = d->i[3];
    if (st->state == WALKER_STOPPED) {                                 /* UpdateStopping :260-268 */
        if (st->flag == WALKER_BORED) {
            st->v[1] -= w->dt;
            if (st->v[1] <= 0.0f) walker_end_stopping(w, go, d, st);
        }
    } else if (st->state == WALKER_WALKING) {                          /* UpdateWalking :312-336 */
        bool turned = false;
        if (st->t <= 0.0f) {
            col_inst *c = go_first_collider(w, go);
            float ex = 0.0f;
            if (c) { float mn[2], mx[2]; col_bounds(w, go, c, mn, mx); ex = (mx[0] - mn[0]) * 0.5f; }
            float p[3]; go_world_pos(w, go, p);
            int card = 1 - (int)st->e[0];                              /* DirectionUtils: facing 1/-1 -> Right(0)/Left(2) */
            if (sweep_check(w, go, card, 3, p[0], p[1], ex + 0.5f, 1u << 8)) {
                walker_begin_turning(w, go, d, st, -st->e[0]); turned = true;
            } else if (!((bits >> 8) & 1)) {                            /* !ignoreHoles */
                float hx = p[0] + (ex + 0.5f + cfg[6]) * (float)st->e[0];   /* edgeXAdjuster */
                if (!sweep_check(w, go, 3 /* Down */, 3, hx, p[1], 0.25f, 1u << 8)) {
                    walker_begin_turning(w, go, d, st, -st->e[0]); turned = true;
                }
            }
        }
        if (!turned && (bits >> 7) & 1) {                               /* pauses */
            st->v[0] -= w->dt;
            if (st->v[0] <= 0.0f) { walker_begin_stopped(w, go, d, st, WALKER_BORED); turned = true; }
        }
        if (!turned) {
            float cur[2]; go_velocity(w, go, cur);
            const float v[2] = { st->e[0] > 0 ? cfg[5] : cfg[4], cur[1] };
            go_set_velocity(w, go, v);
        }
    } else if (st->state == WALKER_TURNING) {                          /* UpdateTurning :344-350 */
        float cur[2]; go_velocity(w, go, cur);
        const float v[2] = { 0.0f, cur[1] };
        go_set_velocity(w, go, v);
        anim_inst *a = anim_of_go(w, go);
        if (!a || !anim_playing(a)) { st->e[0] = st->e[1]; walker_begin_walking(w, go, d, st, st->e[0]); }   /* EndTurning :352-356 */
    }
}

/* Walker's four public entry points hk.c's StartWalker/StopWalker/SendEnemyMessage("GO LEFT"/"GO RIGHT") call.
 * StartMoving/Go both end with a synchronous Update() call (:176, :201); walker_tick (the scr_update case's body,
 * factored out below) is that same logic, called an extra time here to match. */
void scr_walker_start_moving(fsm_world *w, int32_t go)                 /* Walker.StartMoving -- :168-176 */
{
    scr_state *st; const comp_def *d = lc_script_def(w, go, LCT_SCR_WALKER, &st);
    if (!d) return;
    /* BeginWalkingOrTurning(facing), facing = currentFacing (never 0: see the enum note) -> always BeginWalking,
     * since currentFacing == facing here always holds */
    if (st->state == WALKER_STOPPED) walker_begin_walking(w, go, d, st, st->e[0]);
    walker_tick(w, go, d, st);
}
void scr_walker_go(fsm_world *w, int32_t go, int32_t facing)           /* Walker.Go -- :192-202 */
{
    scr_state *st; const comp_def *d = lc_script_def(w, go, LCT_SCR_WALKER, &st);
    if (!d) return;
    st->t = 0.0f - 1e-6f;                                              /* turnCooldownRemaining = -Epsilon */
    if (st->state == WALKER_STOPPED || st->state == WALKER_WALKING) {
        if (st->e[0] == facing) walker_begin_walking(w, go, d, st, facing); else walker_begin_turning(w, go, d, st, facing);
    } else if (st->state == WALKER_TURNING && st->e[0] == facing) {
        walker_begin_walking(w, go, d, st, st->e[0]);                  /* CancelTurn :185-190 */
    }
    walker_tick(w, go, d, st);
}
void scr_walker_receive_go(fsm_world *w, int32_t go, int32_t facing)   /* Walker.RecieveGoMessage -- :204-210 */
{
    scr_state *st; const comp_def *d = lc_script_def(w, go, LCT_SCR_WALKER, &st);
    if (!d) return;
    if (st->state == WALKER_STOPPED && st->flag == WALKER_CONTROLLED) return;
    scr_walker_go(w, go, facing);
}
void scr_walker_stop(fsm_world *w, int32_t go)                         /* Walker.Stop(Controlled) -- :212-215, hk.c StopWalker */
{
    scr_state *st; const comp_def *d = lc_script_def(w, go, LCT_SCR_WALKER, &st);
    if (!d) return;
    walker_begin_stopped(w, go, d, st, WALKER_CONTROLLED);
}
void scr_walker_clear_turn_cooldown(fsm_world *w, int32_t go)          /* Walker.ClearTurnCooldown -- :358-361 */
{
    scr_state *st; const comp_def *d = lc_script_def(w, go, LCT_SCR_WALKER, &st);
    if (d) st->t = 0.0f - 1e-6f;
}

/* EnemyBullet.Collision's synchronous half -- EnemyBullet.cs:104-125 (`normal`/`doRotation` from OnCollisionEnter2D's
 * contact normal, or (0,0)/false from OnTriggerEnter2D's HeroBox hit); the audio call (:121) is cosmetic. */
void scr_enemy_bullet_impact(fsm_world *w, int32_t go, float nx, float ny, bool do_rotation)
{
    scr_state *st;
    const comp_def *d = lc_script_def(w, go, LCT_SCR_ENEMY_BULLET, &st);
    if (!d || !st->flag) return;                                     /* :74/:82 `if (active)` */
    st->flag = 0;                                                    /* active = false */
    float ls[3]; go_local_scale(w, go, ls);
    const float uniform[3] = { st->v[0], st->v[0], ls[2] };           /* :106 localScale = (scale, scale, z) */
    go_set_local_scale(w, go, uniform);
    go_set_kinematic(w, go, true);                                    /* :107 */
    const float zero[2] = { 0.0f, 0.0f };
    go_set_velocity(w, go, zero);                                     /* :108 (angularVelocity=0: not modelled, rotation is a direct write) */
    anim_inst *a = anim_of_go(w, go);
    if (a) anim_play_name(w, a, "Impact");                            /* :111 */
    if (!do_rotation || (ny >= 0.75f && fabsf(nx) < 0.5f)) go_set_local_euler_z(w, go, 0.0f);        /* :113-116 */
    else if (ny <= 0.75f && fabsf(nx) < 0.5f) go_set_local_euler_z(w, go, 180.0f);                   /* :117-120 */
    else if (nx >= 0.75f && fabsf(ny) < 0.5f) go_set_local_euler_z(w, go, 270.0f);                   /* :121-124 (:121 here is the rotation snap, not the audio line above) */
    else if (nx <= 0.75f && fabsf(ny) < 0.5f) go_set_local_euler_z(w, go, 90.0f);                    /* :125-128 */
    /* else: no branch matches (a diagonal normal outside every band) -- rotation is left as Update last wrote it,
     * as in the game (no SetRotation2D call runs) */
    st->state = EBULLET_IMPACT_WAIT;                                  /* scr_update: :137-140 next frame */
}

/* SendEnemyMessageTrigger.OnTriggerStay2D -- :25-33: once per physics step per attached Rigidbody2D's object, send
 * the event to the collider's object (FSMUtility.SendEventToGameObject); SendWalkerGoInDirection needs a Walker,
 * which no ported scene carries (completeness.py: Walker is neither ported nor excluded, so the generator would
 * stop on one). */
void scr_enemy_message_stay(fsm_world *w, int32_t go, int32_t other)
{
    scr_state *st;
    const comp_def *d = lc_script_def(w, go, LCT_SCR_ENEMY_MESSAGE, &st);
    if (!d || other < 0) return;
    int32_t item = other;                                             /* collision.attachedRigidbody.gameObject */
    for (int32_t g = other; g >= 0; g = w->gos[g].parent)
        if (go_has_rb(w, g)) { item = g; break; }
    for (int32_t k = 0; k < st->n_e; k++) if (st->e[1 + k] == item) return;
    if (st->n_e >= 3) HKSIM_UNIMPLEMENTED("SendEnemyMessageTrigger on '%s': more than 3 bodies in one step", go_path(w, go));
    st->e[1 + st->n_e++] = item;
    int32_t ev = st->e[0] >= 0 ? st->e[0] : d->i[0];
    if (ev >= 0 && w_str(w, ev)[0]) world_send_event_to_go(w, other, w_str(w, ev), false);
}

/* EnviroRegion.OnTriggerEnter2D / OnTriggerExit2D -- EnviroRegion.cs:19-30: PlayerData environmentType set to the region's,
 * or back to environmentTypeDefault; then HeroController.checkEnvironment, which picks footstep clips only (HC:1689-1726) */
void scr_enviro_region(fsm_world *w, int32_t go, bool enter)
{
    scr_state *st;
    const comp_def *d = lc_script_def(w, go, LCT_SCR_ENVIRO_REGION, &st);
    if (!d) return;
    world_pd_set_int(w, "environmentType", enter ? d->i[0] : world_pd_int(w, "environmentTypeDefault"));
}

/* EnemyDreamnailReaction.RecieveDreamImpact -- :43-72 */
void scr_dream_impact(fsm_world *w, int32_t go)
{
    scr_state *st;
    const comp_def *d = lc_script_def(w, go, LCT_SCR_DREAM_REACTION, &st);
    if (!d || st->state != DREAM_READY) return;
    if (!(d->i[2] & 2)) world_hero_add_mp(w, world_pd_bool(w, "equippedCharm_30") ? 66 : 33);   /* :47-51 */
    {                                                                 /* ShowConvo :82-88 */
        int32_t msg = world_global_go(w, "Enemy Dream Msg");
        int32_t fi = msg >= 0 ? world_fsm_find(w, msg, "Display") : -1;
        if (fi < 0) HKSIM_UNIMPLEMENTED("EnemyDreamnailReaction.ShowConvo: no `Display` FSM on the Enemy Dream Msg object");
        fsm_inst *f = &w->fsms[fi]; bool fresh;
        fsm_get_var(f, VB_INT, w_intern(w, "Convo Amount"), &fresh)->i = d->i[0];
        fsm_get_var(f, VB_STRING, w_intern(w, "Convo Title"), &fresh)->i = d->i[1];
        fsm_event_name(f, "DISPLAY ENEMY DREAM");
    }
    if (d->i[3] >= 0) {                                               /* :53-56 dreamImpactPrefab.Spawn().position = ours */
        float p[3]; go_world_pos(w, go, p);
        world_pool_spawn(w, d->i[3], p, 0.0f);
    }
    int32_t rc = w->gos[go].recoil;                                   /* :57-62 */
    if (rc >= 0) {
        float ks[3]; go_lossy_scale(w, w->knight_go, ks);            /* the Knight is a root: lossy == localScale */
        recoil_by_direction(w, &w->recoils[rc], ks[0] <= 0.0f ? 0 : 2, 2.0f);
    }
    /* :63-67 SpriteFlash.flashDreamImpact: a sprite tint (completeness.py excludes SpriteFlash) */
    st->state = DREAM_COOLING;
    st->t = 0.2f;
}

/* Breakable -- Breakable.cs.  angleOffset in v[0], isBroken in flag.  The payload's lists (script_gos at i[0]):
 * wholeParts, remnantParts, debrisParts, each a count then the GameObject ids (-1: an unassigned entry, which only
 * logs). */
static const int32_t *breakable_list(const fsm_world *w, const comp_def *d, int which)
{
    const int32_t *p = &w->sc->script_gos[d->i[0]];
    for (int k = 0; k < which; k++) p += 1 + p[0];
    return p;
}

/* SetStaticPartsActivation(true) -- :325-363; wholeRenderer.enabled is a renderer write */
static void breakable_static_parts(fsm_world *w, int32_t go, const comp_def *d)
{
    const int32_t *whole = breakable_list(w, d, 0), *remnant = breakable_list(w, d, 1);
    for (int32_t k = 1; k <= whole[0]; k++) if (whole[k] >= 0) go_set_active(w, whole[k], false);
    for (int32_t k = 1; k <= remnant[0]; k++) if (remnant[k] >= 0) go_set_active(w, remnant[k], true);
    if (d->i[1] >= 0) world_send_event_to_go(w, d->i[1], "HIT", false);
    col_inst *c = go_first_collider(w, go);                           /* bodyCollider = GetComponent<Collider2D>() (Awake) */
    if (c) col_set_enabled(w, c, false);
}

/* Breakable.Break -- :365-441 */
static void breakable_break(fsm_world *w, int32_t go, const comp_def *d, scr_state *st, float lo, float hi, float mult)
{
    if (st->flag) return;                                             /* :367-370 */
    breakable_static_parts(w, go, d);                                 /* :371 */
    const int32_t *debris = breakable_list(w, d, 2);
    for (int32_t k = 1; k <= debris[0]; k++) {                        /* :372-390 */
        int32_t g = debris[k];
        if (g < 0) continue;
        go_set_active(w, g, true);
        go_set_local_euler_z(w, g, go_local_euler_z(w, g) + st->v[0]);   /* SetRotationZ(localEulerAngles.z + angleOffset) */
        if (go_has_rb(w, g)) {
            float a = hk_rng_range_f_site(w->rng, hk_rng_site(go_path(w, go), "Breakable", "Break", 0), lo, hi);
            float r = a * ((float)M_PI / 180.0f);
            float s = hk_rng_range_f_site(w->rng, hk_rng_site(go_path(w, go), "Breakable", "Break", 1), d->f[1], d->f[2]) * mult;
            const float v[2] = { m_cos(r) * s, m_sin(r) * s };
            go_set_velocity(w, g, v);
        }
    }
    /* :391-415 containingParticles: empty in every ported instance (gen_tables.py _breakable stops otherwise);
     * :416-417 break audio */
    if (d->i[1] >= 0) world_send_event_to_go(w, d->i[1], "HIT", false);   /* :418-421 */
    if (d->i[2] & 1) world_send_event_to_go(w, go, "BREAK", false);       /* :422-425 forwardBreakEvent */
    {                                                                  /* :426-434 CameraParent's CameraShake */
        int32_t cs = world_fsm_find(w, w->camera_parent_go, "CameraShake");
        if (cs >= 0) fsm_event_name(&w->fsms[cs], "EnemyKillShake");
    }
    col_inst *c = go_first_collider(w, go);                           /* :439 bodyCollider.enabled = false */
    if (c) col_set_enabled(w, c, false);
    st->flag = 1;                                                     /* :440 isBroken */
}

/* Breakable.Hit -- :225-286.  The effect spawns (:233-247 strike / nail-hit / spell-hit, :281-284 dust) are
 * cosmetic, as in hit_effects.c; so is SpawnNailHitEffect's rotation draw. */
void scr_breakable_hit(fsm_world *w, int32_t go, int attack_type, float direction, float magnitude)
{
    scr_state *st;
    const comp_def *d = lc_script_def(w, go, LCT_SCR_BREAKABLE, &st);
    if (!d) HKSIM_UNIMPLEMENTED("Breakable.Hit on '%s': no Breakable there", go_path(w, go));
    if (st->flag) return;                                             /* :227-230 */
    float num = magnitude;                                            /* :232 */
    if (attack_type != 2 && attack_type != 0 && attack_type != 1) num = 1.0f;   /* :239-243 neither Spell, Nail nor Generic */
    float lo, hi;
    switch (cardinal_direction(direction)) {                          /* :248-280 */
    case 2: st->v[0] *= -1.0f; lo = 120.0f; hi = 160.0f; break;
    case 0: lo = 30.0f; hi = 70.0f; break;
    case 1: st->v[0] = 0.0f; lo = 70.0f; hi = 110.0f; num *= 1.5f; break;
    default: st->v[0] = 0.0f; lo = 160.0f; hi = 380.0f; break;
    }
    breakable_break(w, go, d, st, lo, hi, num);                       /* :285 */
}
