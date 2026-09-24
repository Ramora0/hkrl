/* GameObject activation, destruction, instantiation and pooled spawns.  A pooled spawn goes through
 * world_pool_spawn (ObjectPool.Spawn, HK/ObjectPool.cs:472-526); the prefab's own components decide whether
 * the clone is observable, so a cosmetic prefab contributes nothing. */
#include "act.h"
#include "../lifecycle.h"

/* Spawn pose shared by the pooled spawns: spawnPoint's world position plus `position` as an offset, else
 * `position`, else zero; euler z from `rotation`, else the spawn point's, else 0 (Vector3.up ->
 * Quaternion.Euler(0,1,0)) -- SpawnObjectFromGlobalPool.cs:74-95, FlingObjectsFromGlobalPool.cs:85-97. */
static float spawn_pose(act_inst *a, const fsm_pv *spawnPoint, const fsm_pv *position, const fsm_pv *rotation, float v[3])
{
    fsm_inst *f = a->fsm; fsm_world *w = f->w;
    float eulz = 0.0f;
    v[0] = v[1] = v[2] = 0.0f;
    int32_t sp = spawnPoint ? pgo(f, spawnPoint) : -1;
    bool has_pos = position && !p_isnone(position), has_rot = rotation && !p_isnone(rotation);
    if (sp >= 0) {
        go_world_pos(w, sp, v);
        if (has_pos) { const float *o = pv3(f, position); v[0] += o[0]; v[1] += o[1]; v[2] += o[2]; }
        eulz = has_rot ? pv3(f, rotation)[2] : go_euler_z(w, sp);
    } else {
        if (has_pos) { const float *o = pv3(f, position); v[0] = o[0]; v[1] = o[1]; v[2] = o[2]; }
        if (has_rot) eulz = pv3(f, rotation)[2];
    }
    return eulz;
}

/* SpawnObjectFromGlobalPool — ACT/SpawnObjectFromGlobalPool.cs:70-99 */
typedef struct { const fsm_pv *go, *spawnPoint, *position, *rotation, *store; } st_spawn;
static void spawn_bind(act_inst *a) { ST(st_spawn); s->go = FIELD(gameObject); s->spawnPoint = FIELD(spawnPoint); s->position = FIELD(position); s->rotation = FIELD(rotation); s->store = FIELD(storeObject); }
static void spawn_enter(act_inst *a)
{
    ST(st_spawn);
    int32_t prefab = pgo(f, s->go);
    if (prefab >= 0) {
        float v[3];
        float eulz = spawn_pose(a, s->spawnPoint, s->position, s->rotation, v);
        pgo_set(f, s->store, world_pool_spawn(w, prefab, v, eulz));   /* :97 storeObject.Value = value */
    }
    act_finish(a);
}
static const act_vtable AV_SpawnObjectFromGlobalPool = { "SpawnObjectFromGlobalPool", sizeof(st_spawn), spawn_bind, spawn_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* SpawnObjectFromGlobalPoolOverTime — ACT/SpawnObjectFromGlobalPoolOverTime.cs:41-77: one spawn every `frequency`
 * seconds from OnUpdate, never finishing.  There is no OnEnter, so `timer` persists across re-entries. */
typedef struct { const fsm_pv *go, *spawnPoint, *position, *rotation, *frequency; float timer; } st_sgpo;
static void sgpo_bind(act_inst *a)
{
    ST(st_sgpo);
    s->go = FIELD(gameObject); s->spawnPoint = FIELD(spawnPoint); s->position = FIELD(position);
    s->rotation = FIELD(rotation); s->frequency = FIELD(frequency);
    s->timer = 0.0f;
}
static void sgpo_update(act_inst *a)
{
    ST(st_sgpo);
    s->timer += w->dt;                                                 /* :45 Time.deltaTime */
    if (s->timer < pf(f, s->frequency)) return;                        /* :46-49 */
    s->timer = 0.0f;
    int32_t prefab = pgo(f, s->go);
    if (prefab < 0) return;                                            /* :51-54 */
    float v[3];
    float eulz = spawn_pose(a, s->spawnPoint, s->position, s->rotation, v);   /* :56-76 */
    world_pool_spawn(w, prefab, v, eulz);
}
static const act_vtable AV_SpawnObjectFromGlobalPoolOverTime = { "SpawnObjectFromGlobalPoolOverTime", sizeof(st_sgpo), sgpo_bind, NULL, sgpo_update, NULL, NULL, NULL, NULL, NULL };

/* SpawnObjectFromGlobalPoolOverTimeV2 - ACT/SpawnObjectFromGlobalPoolOverTimeV2.cs:26-72: the same, plus a
 * Range(scaleMin, scaleMax) draw on every spawn whose value scales the clone unless it is 1 (:64-70); OnEnter
 * resets the timer. */
typedef struct { const fsm_pv *go, *spawnPoint, *position, *rotation, *frequency, *scaleMin, *scaleMax; float timer; } st_sgpot;
static void sgpot_bind(act_inst *a)
{
    ST(st_sgpot);
    s->go = FIELD(gameObject); s->spawnPoint = FIELD(spawnPoint); s->position = FIELD(position);
    s->rotation = FIELD(rotation); s->frequency = FIELD(frequency);
    s->scaleMin = a_field(a, "scaleMin"); s->scaleMax = a_field(a, "scaleMax");
    s->timer = 0.0f;
}
static void sgpot_enter(act_inst *a) { ST(st_sgpot); s->timer = 0.0f; }
static void sgpot_update(act_inst *a)
{
    ST(st_sgpot);
    s->timer += w->dt;                                                /* :28 Time.deltaTime */
    if (s->timer < pf(f, s->frequency)) return;                       /* :29-32 */
    s->timer = 0.0f;
    int32_t prefab = pgo(f, s->go);
    if (prefab < 0) return;                                           /* :34-37 */
    float v[3];
    float eulz = spawn_pose(a, s->spawnPoint, s->position, s->rotation, v);   /* :39-58 */
    int32_t inst = world_pool_spawn(w, prefab, v, eulz);
    float num = hk_rng_range_f_site(w->rng, a->rng_site, pf(f, s->scaleMin), pf(f, s->scaleMax));
    if (inst >= 0 && num != 1.0f) { float sc[3] = { num, num, num }; go_set_local_scale(w, inst, sc); }
}
static const act_vtable AV_SpawnObjectFromGlobalPoolOverTimeV2 = { "SpawnObjectFromGlobalPoolOverTimeV2", sizeof(st_sgpot), sgpot_bind, sgpot_enter, sgpot_update, NULL, NULL, NULL, NULL, NULL };

/* The FlingObjectsFromGlobalPool family: Range(spawnMin, spawnMax+1) clones, and per clone: spawn at the base
 * position, originVariationX then originVariationY jitter, then two velocity draws -- speed and angle (polar)
 * or speedX and speedY (Vel).  `originAdjusted` is an action field in the C#, never reset per clone, so once a
 * clone has varied its origin every later clone also gets its position written back.  A null ovx/ovy skips
 * that draw. */
static void fling_one(act_inst *a, int32_t prefab, const float base_pos[3], const fsm_pv *ovx, const fsm_pv *ovy,
                      const fsm_pv *s1, const fsm_pv *s2, const fsm_pv *s3, const fsm_pv *s4, int polar, int *origin_adjusted)
{
    fsm_inst *f = a->fsm; fsm_world *w = f->w;
    int32_t c = world_pool_spawn(w, prefab, base_pos, 0.0f);
    float p[3] = { base_pos[0], base_pos[1], base_pos[2] };
    if (c >= 0) go_world_pos(w, c, p);
    float x = p[0], y = p[1], z = p[2];
    if (ovx) { x = p[0] + hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - pf(f, ovx), pf(f, ovx)); *origin_adjusted = 1; }
    if (ovy) { y = p[1] + hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - pf(f, ovy), pf(f, ovy)); *origin_adjusted = 1; }
    if (*origin_adjusted && c >= 0) { float np[3]; np[0] = x; np[1] = y; np[2] = z; go_set_world_pos(w, c, np); }
    float vx, vy;
    if (polar) {
        float spd = hk_rng_range_f_site(w->rng, a->rng_site, pf(f, s1), pf(f, s2));
        float ang = hk_rng_range_f_site(w->rng, a->rng_site, pf(f, s3), pf(f, s4));
        vx = spd * cosf(ang * (3.14159265358979f / 180.0f));
        vy = spd * sinf(ang * (3.14159265358979f / 180.0f));
    } else {
        vx = hk_rng_range_f_site(w->rng, a->rng_site, pf(f, s1), pf(f, s2));
        vy = hk_rng_range_f_site(w->rng, a->rng_site, pf(f, s3), pf(f, s4));
    }
    if (c >= 0 && go_has_rb(w, c)) { float v[2]; v[0] = vx; v[1] = vy; go_set_velocity(w, c, v); }
}

/* FlingObjectsFromGlobalPool — ACT/FlingObjectsFromGlobalPool.cs:59-123: polar; the origin draws are guarded
 * by IsNone (:109, :114), and originAdjusted starts false on each OnEnter. */
typedef struct { const fsm_pv *go, *spawnPoint, *position, *spawnMin, *spawnMax, *speedMin, *speedMax, *angleMin, *angleMax, *ovx, *ovy, *FSM; } st_fling;
static void fling_bind(act_inst *a)
{
    ST(st_fling);
    s->go = FIELD(gameObject); s->spawnPoint = FIELD(spawnPoint); s->position = FIELD(position); s->spawnMin = FIELD(spawnMin); s->spawnMax = FIELD(spawnMax);
    s->speedMin = FIELD(speedMin); s->speedMax = FIELD(speedMax); s->angleMin = FIELD(angleMin); s->angleMax = FIELD(angleMax);
    s->ovx = FIELD(originVariationX); s->ovy = FIELD(originVariationY); s->FSM = FIELD(FSM);
}
static void fling_enter(act_inst *a)
{
    ST(st_fling);
    if (!p_isnone(s->go)) {                                        /* :83 if (gameObject.Value != null) */
        int32_t prefab = pgo(f, s->go);
        if (!p_isnone(s->FSM)) HKSIM_UNIMPLEMENTED("FlingObjectsFromGlobalPool with FSM event to '%s' in %s", go_path(w, prefab), fsm_label(f));
        float base_pos[3];
        spawn_pose(a, s->spawnPoint, s->position, NULL, base_pos);
        int32_t num = hk_rng_range_i_site(w->rng, a->rng_site, pi(f, s->spawnMin), pi(f, s->spawnMax) + 1);   /* :99 */
        int origin_adjusted = 0;
        for (int32_t i = 1; i <= num; i++)
            fling_one(a, prefab, base_pos, p_isnone(s->ovx) ? NULL : s->ovx, p_isnone(s->ovy) ? NULL : s->ovy,
                      s->speedMin, s->speedMax, s->angleMin, s->angleMax, 1, &origin_adjusted);
    }
    act_finish(a);
}
static const act_vtable AV_FlingObjectsFromGlobalPool = { "FlingObjectsFromGlobalPool", sizeof(st_fling), fling_bind, fling_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* FlingObjectsFromGlobalPoolVel - ACT/FlingObjectsFromGlobalPoolVel.cs:62-109: cartesian speeds; the origin
 * draws are guarded by `originVariationX != null` (an FsmFloat object, present even when 0). */
typedef struct { const fsm_pv *go, *spawnPoint, *position, *spawnMin, *spawnMax, *sminx, *smaxx, *sminy, *smaxy, *ovx, *ovy; int origin_adjusted; } st_flingvel;
static void flingvel_bind(act_inst *a)
{
    ST(st_flingvel);
    s->go = FIELD(gameObject); s->spawnPoint = FIELD_OPT(spawnPoint); s->position = FIELD_OPT(position);
    s->spawnMin = FIELD(spawnMin); s->spawnMax = FIELD(spawnMax);
    s->sminx = FIELD(speedMinX); s->smaxx = FIELD(speedMaxX); s->sminy = FIELD(speedMinY); s->smaxy = FIELD(speedMaxY);
    s->ovx = FIELD_OPT(originVariationX); s->ovy = FIELD_OPT(originVariationY); s->origin_adjusted = 0;
}
static void flingvel_enter(act_inst *a)
{
    ST(st_flingvel);
    int32_t prefab = pgo(f, s->go);
    if (prefab >= 0) {
        float base_pos[3];
        spawn_pose(a, s->spawnPoint, s->position, NULL, base_pos);
        int32_t num = hk_rng_range_i_site(w->rng, a->rng_site, pi(f, s->spawnMin), pi(f, s->spawnMax) + 1);
        for (int32_t i = 1; i <= num; i++)
            fling_one(a, prefab, base_pos, s->ovx, s->ovy, s->sminx, s->smaxx, s->sminy, s->smaxy, 0, &s->origin_adjusted);
    }
    act_finish(a);
}
static const act_vtable AV_FlingObjectsFromGlobalPoolVel = { "FlingObjectsFromGlobalPoolVel", sizeof(st_flingvel), flingvel_bind, flingvel_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* FlingObjectsFromGlobalPoolTime - ACT/FlingObjectsFromGlobalPoolTime.cs:73-135: polar, fired from OnUpdate on
 * a `frequency` timer. */
typedef struct { const fsm_pv *go, *spawnPoint, *position, *spawnMin, *spawnMax, *speedMin, *speedMax, *angleMin, *angleMax, *ovx, *ovy, *frequency; float timer; int origin_adjusted; } st_flingtime;
static void flingtime_bind(act_inst *a)
{
    ST(st_flingtime);
    s->go = FIELD(gameObject); s->spawnPoint = FIELD_OPT(spawnPoint); s->position = FIELD_OPT(position);
    s->spawnMin = FIELD(spawnMin); s->spawnMax = FIELD(spawnMax);
    s->speedMin = FIELD(speedMin); s->speedMax = FIELD(speedMax);
    s->angleMin = FIELD(angleMin); s->angleMax = FIELD(angleMax);
    s->ovx = FIELD_OPT(originVariationX); s->ovy = FIELD_OPT(originVariationY);
    s->frequency = FIELD(frequency); s->timer = 0.0f; s->origin_adjusted = 0;
}
static void flingtime_update(act_inst *a)
{
    ST(st_flingtime);
    s->timer += w->dt;
    if (s->timer < pf(f, s->frequency)) return;
    s->timer = 0.0f;
    int32_t prefab = pgo(f, s->go);
    if (prefab < 0) return;
    float base_pos[3];
    spawn_pose(a, s->spawnPoint, s->position, NULL, base_pos);
    int32_t num = hk_rng_range_i_site(w->rng, a->rng_site, pi(f, s->spawnMin), pi(f, s->spawnMax) + 1);
    for (int32_t i = 1; i <= num; i++)
        fling_one(a, prefab, base_pos, s->ovx, s->ovy, s->speedMin, s->speedMax, s->angleMin, s->angleMax, 1, &s->origin_adjusted);
}
static const act_vtable AV_FlingObjectsFromGlobalPoolTime = { "FlingObjectsFromGlobalPoolTime", sizeof(st_flingtime), flingtime_bind, NULL, flingtime_update, NULL, NULL, NULL, NULL, NULL };

/* SpawnRandomObjects — ACT/SpawnRandomObjects.cs:62-104, SpawnRandomObjectsV2 — SpawnRandomObjectsV2.cs:67-119:
 * Range(spawnMin, spawnMax+1) copies Instantiated at the spawn position (spawnPoint's world position plus
 * `position`, else `position`, else zero; rotation Euler(0,0,0)), each moved by its origin variation and given a
 * polar velocity.  V1 draws both origin offsets whenever `originVariation` exists (an FsmFloat is never null);
 * V2 draws x and y separately, and `originAdjusted`, an action field never reset, keeps writing the position back
 * once set.  A copy without a Rigidbody2D throws at `rb2d.velocity` (CacheRigidBody2d leaves rb2d null, :96-103),
 * which ends the action without Finish: that traps. */
typedef struct { const fsm_pv *go, *spawnPoint, *position, *spawnMin, *spawnMax, *speedMin, *speedMax, *angleMin, *angleMax, *ov, *ovx, *ovy; int origin_adjusted; } st_sro;
static void sro_bind(act_inst *a)
{
    ST(st_sro);
    s->go = FIELD(gameObject); s->spawnPoint = FIELD(spawnPoint); s->position = FIELD(position);
    s->spawnMin = FIELD(spawnMin); s->spawnMax = FIELD(spawnMax); s->speedMin = FIELD(speedMin); s->speedMax = FIELD(speedMax);
    s->angleMin = FIELD(angleMin); s->angleMax = FIELD(angleMax);
    s->ov = a_field(a, "originVariation"); s->ovx = a_field(a, "originVariationX"); s->ovy = a_field(a, "originVariationY");
    s->origin_adjusted = 0;
}
static void sro_enter(act_inst *a)
{
    ST(st_sro);
    int32_t prefab = pgo(f, s->go);
    if (prefab >= 0) {
        float base[3];
        spawn_pose(a, s->spawnPoint, s->position, NULL, base);
        int32_t num = hk_rng_range_i_site(w->rng, a->rng_site, pi(f, s->spawnMin), pi(f, s->spawnMax) + 1);
        if (num >= 1 && w->sc->gos[prefab].rb < 0)
            HKSIM_UNIMPLEMENTED("%s: '%s' has no Rigidbody2D, so rb2d.velocity throws in %s", a->vt->type_short,
                                go_name(w, prefab), fsm_label(f));
        for (int32_t i = 1; i <= num; i++) {
            int32_t c = world_instantiate_at(w, prefab, base, 0.0f);
            float p[3]; go_world_pos(w, c, p);
            if (s->ov) {                                               /* V1 :90-96 */
                p[0] += hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - pf(f, s->ov), pf(f, s->ov));
                p[1] += hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - pf(f, s->ov), pf(f, s->ov));
                go_set_world_pos(w, c, p);
            } else {                                                   /* V2 :91-106 */
                if (s->ovx) { p[0] += hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - pf(f, s->ovx), pf(f, s->ovx)); s->origin_adjusted = 1; }
                if (s->ovy) { p[1] += hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - pf(f, s->ovy), pf(f, s->ovy)); s->origin_adjusted = 1; }
                if (s->origin_adjusted) go_set_world_pos(w, c, p);
            }
            float spd = hk_rng_range_f_site(w->rng, a->rng_site, pf(f, s->speedMin), pf(f, s->speedMax));
            float ang = hk_rng_range_f_site(w->rng, a->rng_site, pf(f, s->angleMin), pf(f, s->angleMax));
            float v[2] = { spd * m_cos(ang * ((float)M_PI / 180.0f)), spd * m_sin(ang * ((float)M_PI / 180.0f)) };
            go_set_velocity(w, c, v);
        }
    }
    act_finish(a);
}
static const act_vtable AV_SpawnRandomObjects = { "SpawnRandomObjects", sizeof(st_sro), sro_bind, sro_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_SpawnRandomObjectsV2 = { "SpawnRandomObjectsV2", sizeof(st_sro), sro_bind, sro_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* SpawnFromPool — ACT/SpawnFromPool.cs:52-88 on RigidBody2dActionBase.  Draws Range(spawnMin,spawnMax+1) once,
 * then per clone: the child index, and the speed and angle of a child with a Rigidbody2D.  Each clone is
 * activated, given the velocity, offset by adjustPosition and deparented -- which shrinks the pool, so childCount
 * is re-read every iteration. */
typedef struct { const fsm_pv *pool, *adjustPosition, *spawnMin, *spawnMax, *speedMin, *speedMax, *angleMin, *angleMax; } st_sfp;
static void sfp_bind(act_inst *a)
{
    ST(st_sfp);
    s->pool = FIELD(pool); s->adjustPosition = FIELD(adjustPosition);
    s->spawnMin = FIELD(spawnMin); s->spawnMax = FIELD(spawnMax);
    s->speedMin = FIELD(speedMin); s->speedMax = FIELD(speedMax);
    s->angleMin = FIELD(angleMin); s->angleMax = FIELD(angleMax);
}
static void sfp_enter(act_inst *a)
{
    ST(st_sfp);
    int32_t pool = pgo(f, s->pool);
    if (pool >= 0) {                                                  /* :54 if (value != null) */
        int32_t num = hk_rng_range_i_site(w->rng, a->rng_site, pi(f, s->spawnMin), pi(f, s->spawnMax) + 1);
        for (int32_t i = 1; i <= num; i++) {
            int32_t n_child = 0;
            for (int32_t c = w->gos[pool].first_child; c >= 0; c = w->gos[c].next_sibling) n_child++;
            if (n_child <= 0) break;                                  /* :62-69 Finish() then return */
            int32_t k = hk_rng_range_i_site(w->rng, a->rng_site, 0, n_child);
            int32_t child = w->gos[pool].first_child;
            for (int32_t j = 0; j < k && child >= 0; j++) child = w->gos[child].next_sibling;
            if (child < 0) break;
            go_set_active(w, child, true);                            /* :71 */
            if (go_has_rb(w, child)) {                                /* :73-78 rb2d.velocity */
                float speed = hk_rng_range_f_site(w->rng, a->rng_site, pf(f, s->speedMin), pf(f, s->speedMax));
                float ang = hk_rng_range_f_site(w->rng, a->rng_site, pf(f, s->angleMin), pf(f, s->angleMax));
                float v[2] = { speed * m_cos(ang * ((float)M_PI / 180.0f)), speed * m_sin(ang * ((float)M_PI / 180.0f)) };
                go_set_velocity(w, child, v);
            }
            if (!p_isnone(s->adjustPosition)) {                       /* :80-83 transform.position += */
                const float *o = pv3(f, s->adjustPosition);
                float wp[3]; go_world_pos(w, child, wp);
                wp[0] += o[0]; wp[1] += o[1]; wp[2] += o[2];
                go_set_world_pos(w, child, wp);
            }
            go_set_parent(w, child, -1);                              /* :84 transform.parent = null */
        }
    }
    act_finish(a);
}
static const act_vtable AV_SpawnFromPool = { "SpawnFromPool", sizeof(st_sfp), sfp_bind, sfp_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* CreateObject — ACT/CreateObject.cs:50-82: Object.Instantiate(gameObject, position, Quaternion.Euler(rotation)),
 * stored.  The pose is spawnPoint's world position plus `position` (else `position`, else zero) and `rotation`
 * (else the spawn point's eulerAngles, else zero).  A rotation off the z axis has no 2D pose here: it traps. */
typedef struct { const fsm_pv *go, *spawnPoint, *position, *rotation, *store; } st_create;
static void create_bind(act_inst *a) { ST(st_create); s->go = FIELD(gameObject); s->spawnPoint = FIELD(spawnPoint); s->position = FIELD(position); s->rotation = FIELD(rotation); s->store = FIELD(storeObject); }
static void create_enter(act_inst *a)
{
    ST(st_create);
    int32_t prefab = pgo(f, s->go);
    if (prefab >= 0 && !world_prefab_matters(w, prefab)) {
        pgo_set(f, s->store, -1);                                     /* no copy of a prefab nothing observes */
    } else if (prefab >= 0) {
        if (!p_isnone(s->rotation)) {
            const float *r = pv3(f, s->rotation);
            if (r[0] != 0.0f || r[1] != 0.0f)
                HKSIM_UNIMPLEMENTED("CreateObject '%s' with rotation (%g,%g,%g) in %s", go_name(w, prefab), r[0], r[1], r[2], fsm_label(f));
        }
        float v[3];
        float eulz = spawn_pose(a, s->spawnPoint, s->position, s->rotation, v);
        pgo_set(f, s->store, world_instantiate_at(w, prefab, v, eulz));   /* :79-80 */
    }
    act_finish(a);
}
static const act_vtable AV_CreateObject = { "CreateObject", sizeof(st_create), create_bind, create_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* SpawnBlood — HK/SpawnBlood.cs:43-47 `Spawn(); Finish();`: GlobalPrefabDefaults.SpawnBlood (GlobalPrefabDefaults.cs:32-63)
 * pool-spawns a ParticleSystem (no Collider2D, no HealthManager) and draws no Random.  SpawnBloodTime
 * (HK/SpawnBloodTime.cs:14-25) overrides OnEnter to do nothing and re-spawns from OnUpdate, never finishing. */
static const act_vtable AV_SpawnBlood = { "SpawnBlood", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };
static const act_vtable AV_SpawnBloodTime = { "SpawnBloodTime", 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

/* PreSpawnCorpse - ACT/PreSpawnCorpse.cs:16-29 -> EnemyDeathEffects.PreInstantiate (EnemyDeathEffects.cs:95-117),
 * which Start() already ran (:92) before the SceneReady dump.  Its three branches are each guarded against a
 * second run: the corpse is already instantiated (:97; e.g. "Ghost Death Markoth(Clone)" in the GG_Ghost_Markoth
 * dump), the journal popup (:107-110) is a HUD object instantiated inactive, and CreateStartupPools (:112) is
 * guarded by createdStartupPools = True (PersonalObjectPool.cs:24-28).  A dump with corpse = None would need the
 * corpse instantiated here. */
static const act_vtable AV_PreSpawnCorpse = { "PreSpawnCorpse", 0, NULL, act_finish_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* RecycleSelf — ACT/RecycleSelf.cs:7-14: gameObject.Recycle() (ObjectPool.Recycle) */
static void recs_enter(act_inst *a) { world_pool_recycle(a->fsm->w, a->fsm->go); act_finish(a); }
static const act_vtable AV_RecycleSelf = { "RecycleSelf", 0, NULL, recs_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* ActivateGameObject — ACT/ActivateGameObject.cs:37-90 */
typedef struct { const fsm_pv *go, *activate, *recursive, *resetOnExit, *everyFrame; int32_t activated; } st_ago;
static void ago_bind(act_inst *a) { ST(st_ago); s->go = FIELD(gameObject); s->activate = FIELD(activate); s->recursive = FIELD(recursive); s->resetOnExit = FIELD(resetOnExit); s->everyFrame = FIELD(everyFrame); s->activated = -1; }
static void set_active_recursively(fsm_world *w, int32_t go, bool state)     /* :83-90 parent first, children in sibling order */
{
    go_set_active(w, go, state);
    for (int32_t c = w->gos[go].first_child; c >= 0; c = w->gos[c].next_sibling) set_active_recursively(w, c, state);
}
static void ago_do(act_inst *a)
{
    ST(st_ago);
    int32_t t = p_owner_default(a, s->go);
    if (t < 0) return;
    if (pb(f, s->recursive)) set_active_recursively(w, t, pb(f, s->activate)); else go_set_active(w, t, pb(f, s->activate));
    s->activated = t;
}
static void ago_enter(act_inst *a) { ST(st_ago); ago_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static void ago_exit(act_inst *a)
{
    ST(st_ago);
    if (s->activated >= 0 && pb(f, s->resetOnExit)) {
        if (pb(f, s->recursive)) set_active_recursively(w, s->activated, !pb(f, s->activate)); else go_set_active(w, s->activated, !pb(f, s->activate));
    }
}
static const act_vtable AV_ActivateGameObject = { "ActivateGameObject", sizeof(st_ago), ago_bind, ago_enter, ago_do, NULL, NULL, ago_exit, NULL, NULL };

/* ActivateAllChildren — ACT/ActivateAllChildren.cs:21-32 */
typedef struct { const fsm_pv *go, *activate; } st_aac;
static void aac_bind(act_inst *a) { ST(st_aac); s->go = FIELD(gameObject); s->activate = FIELD(activate); }
static void aac_enter(act_inst *a)
{
    ST(st_aac);
    int32_t t = pgo(f, s->go);
    if (t >= 0) for (int32_t c = w->gos[t].first_child; c >= 0; c = w->gos[c].next_sibling) go_set_active(w, c, pb(f, s->activate));
    act_finish(a);
}
static const act_vtable AV_ActivateAllChildren = { "ActivateAllChildren", sizeof(st_aac), aac_bind, aac_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* DestroySelf / DestroyObject / DestroyAllChildren — ACT/DestroySelf.cs:17-29, DestroyObject.cs:25-50,
 * DestroyAllChildren.cs:20-38.  Object.Destroy is deferred: the object runs to the end of the frame and is gone
 * after the next delayed-call pass (lc_destroy_go; docs/engine-lifecycle.md R7).  DetachChildren
 * is synchronous. */
typedef struct { const fsm_pv *detach; } st_destroyself;
static void destroyself_bind(act_inst *a) { ST(st_destroyself); s->detach = a_field(a, "detachChildren"); }
static void destroyself_enter(act_inst *a)
{
    ST(st_destroyself);
    bool detach = s->detach && !p_isnone(s->detach) && pb(f, s->detach);
    lc_destroy_go(w, f->go, 0.0f, detach);                         /* DestroySelf.cs:21-25 */
    act_finish(a);
}
static const act_vtable AV_DestroySelf = { "DestroySelf", sizeof(st_destroyself), destroyself_bind, destroyself_enter, NULL, NULL, NULL, NULL, NULL, NULL };

typedef struct { const fsm_pv *go, *delay, *detach; } st_destroy;
static void destroy_bind(act_inst *a) { ST(st_destroy); s->go = FIELD(gameObject); s->delay = FIELD(delay); s->detach = FIELD(detachChildren); }
static void destroy_enter(act_inst *a)
{
    ST(st_destroy);
    int32_t t = pgo(f, s->go);
    if (t >= 0) {
        float delay = pf(f, s->delay);
        bool detach = !p_isnone(s->detach) && pb(f, s->detach);
        lc_destroy_go(w, t, delay > 0.0f ? delay : 0.0f, detach);  /* DestroyObject.cs:31-45 */
    }
    act_finish(a);
}
static const act_vtable AV_DestroyObject = { "DestroyObject", sizeof(st_destroy), destroy_bind, destroy_enter, NULL, NULL, NULL, NULL, NULL, NULL };

typedef struct { const fsm_pv *gameObject, *disable; } st_dac;
static void dac_bind(act_inst *a) { ST(st_dac); s->gameObject = FIELD(gameObject); s->disable = a_field(a, "disable"); }
static void dac_enter(act_inst *a)
{
    ST(st_dac);
    int32_t t = pgo(f, s->gameObject);
    if (t >= 0) {
        bool dis = s->disable && !p_isnone(s->disable) && pb(f, s->disable);
        for (int32_t c = w->gos[t].first_child; c >= 0; c = w->gos[c].next_sibling) {
            if (dis) go_set_active(w, c, false);
            else lc_destroy_go(w, c, 0.0f, false);
        }
    }
    act_finish(a);
}
static const act_vtable AV_DestroyAllChildren = { "DestroyAllChildren", sizeof(st_dac), dac_bind, dac_enter, NULL, NULL, NULL, NULL, NULL, NULL };

static int32_t go_child_count(const fsm_world *w, int32_t go)
{
    int32_t n = 0;
    for (int32_t c = w->gos[go].first_child; c >= 0; c = w->gos[c].next_sibling) n++;
    return n;
}

/* GetRandomChild -- ACT/GetRandomChild.cs:23-38 */
typedef struct { const fsm_pv *gameObject, *storeResult; } st_grc;
static void grc_bind(act_inst *a) { ST(st_grc); s->gameObject = FIELD(gameObject); s->storeResult = FIELD(storeResult); }
static void grc_enter(act_inst *a)
{
    ST(st_grc);
    int32_t t = p_owner_default(a, s->gameObject);
    if (t >= 0) {
        int32_t n = go_child_count(w, t);
        if (n != 0) {
            int32_t idx = hk_rng_range_i_site(w->rng, a->rng_site, 0, n), c = w->gos[t].first_child;
            for (int32_t j = 0; j < idx && c >= 0; j++) c = w->gos[c].next_sibling;
            pgo_set(f, s->storeResult, c);
        }
    }
    act_finish(a);
}
static const act_vtable AV_GetRandomChild = { "GetRandomChild", sizeof(st_grc), grc_bind, grc_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* GetNextChild -- ACT/GetNextChild.cs:31-77: loops through a parent's children across re-Enters; `go`/
 * `nextChildIndex` are the action's own private fields, reset when `parent` changes. */
typedef struct { const fsm_pv *gameObject, *storeNextChild, *loopEvent, *finishedEvent; int32_t go, idx; } st_gnc;
static void gnc_bind(act_inst *a)
{
    ST(st_gnc);
    s->gameObject = FIELD(gameObject); s->storeNextChild = FIELD(storeNextChild);
    s->loopEvent = FIELD_OPT(loopEvent); s->finishedEvent = FIELD_OPT(finishedEvent);
    s->go = -1; s->idx = 0;
}
static void gnc_enter(act_inst *a)
{
    ST(st_gnc);
    int32_t parent = p_owner_default(a, s->gameObject);
    if (parent < 0) { act_finish(a); return; }                              /* :43-46 */
    if (s->go != parent) { s->go = parent; s->idx = 0; }                    /* :48-52 */
    int32_t n = go_child_count(w, parent);
    if (s->idx >= n) { s->idx = 0; fsm_event(f, EV(s->finishedEvent)); act_finish(a); return; }   /* :54-58 */
    int32_t c = w->gos[parent].first_child;
    for (int32_t j = 0; j < s->idx && c >= 0; j++) c = w->gos[c].next_sibling;
    pgo_set(f, s->storeNextChild, c);                                       /* :62 */
    s->idx++;                                                               /* :68 (the :64-67 re-check can't differ within one call) */
    if (EV(s->loopEvent) >= 0) fsm_event(f, EV(s->loopEvent));              /* :70-73 */
    act_finish(a);
}
static const act_vtable AV_GetNextChild = { "GetNextChild", sizeof(st_gnc), gnc_bind, gnc_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* GetChildCount -- ACT/GetChildCount.cs:22-37 */
typedef struct { const fsm_pv *gameObject, *storeResult; } st_gcc;
static void gcc_bind(act_inst *a) { ST(st_gcc); s->gameObject = FIELD(gameObject); s->storeResult = FIELD(storeResult); }
static void gcc_enter(act_inst *a)
{
    ST(st_gcc);
    int32_t t = p_owner_default(a, s->gameObject);
    if (t >= 0) pi_set(f, s->storeResult, go_child_count(w, t));
    act_finish(a);
}
static const act_vtable AV_GetChildCount = { "GetChildCount", sizeof(st_gcc), gcc_bind, gcc_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* SetLayer -- ACT/SetLayer.cs:22-35: go_set_layer already ports GameObject.layer's write-side effects
 * (physics.c refiltering), so this action is the one FSM writer the port's own comment expects. */
typedef struct { const fsm_pv *gameObject, *layer; } st_slay;
static void slay_bind(act_inst *a) { ST(st_slay); s->gameObject = FIELD(gameObject); s->layer = a_field_req(a, "layer"); }
static void slay_enter(act_inst *a)
{
    ST(st_slay);
    int32_t t = p_owner_default(a, s->gameObject);
    if (t >= 0) go_set_layer(w, t, pi(f, s->layer));
    act_finish(a);
}
static const act_vtable AV_SetLayer = { "SetLayer", sizeof(st_slay), slay_bind, slay_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* GetYDistance -- ACT/GetYDistance.cs:27-56: |gameObject.y - target.y|, every frame while everyFrame. */
typedef struct { const fsm_pv *gameObject, *target, *storeResult, *everyFrame; } st_gyd;
static void gyd_bind(act_inst *a) { ST(st_gyd); s->gameObject = FIELD(gameObject); s->target = FIELD(target); s->storeResult = FIELD(storeResult); s->everyFrame = FIELD(everyFrame); }
static void gyd_do(act_inst *a)
{
    ST(st_gyd);
    int32_t t = p_owner_default(a, s->gameObject), tgt = pgo(f, s->target);
    if (t >= 0 && tgt >= 0 && !p_isnone(s->storeResult)) {
        float p1[3], p2[3]; go_world_pos(w, t, p1); go_world_pos(w, tgt, p2);
        pf_set(f, s->storeResult, fabsf(p1[1] - p2[1]));
    }
}
static void gyd_enter(act_inst *a) { ST(st_gyd); gyd_do(a); if (!pb(f, s->everyFrame)) act_finish(a); }
static const act_vtable AV_GetYDistance = { "GetYDistance", sizeof(st_gyd), gyd_bind, gyd_enter, gyd_do, NULL, NULL, NULL, NULL, NULL };

/* AddComponent -- ACT/AddComponent.cs:29-63: a reflection AddComponent(type), dispatched by name.  The only
 * type any dumped FSM in this backlog requests is BounceShroom (GG_Uumuu(_V) 'Mega Jellyfish' Recover,
 * dumps_all/GG_Uumuu/fsm.json), a components-category class (GEN backlog: GG_Uumuu@T0) not ported here. */
typedef struct { const fsm_pv *gameObject, *component; } st_addc;
static void addc_bind(act_inst *a) { ST(st_addc); s->gameObject = FIELD(gameObject); s->component = FIELD(component); }
static void addc_enter(act_inst *a)
{
    ST(st_addc);
    int32_t t = p_owner_default(a, s->gameObject);
    if (t >= 0) HKSIM_UNIMPLEMENTED("AddComponent('%s') on '%s': no dispatch entry", w_str(w, ps(f, s->component)), go_path(w, t));
    act_finish(a);
}
static const act_vtable AV_AddComponent = { "AddComponent", sizeof(st_addc), addc_bind, addc_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* DestroyComponent -- ACT/DestroyComponent.cs:24-49: Object.Destroy(go.GetComponent(type)); a present
 * component traps (runtime component removal is not modelled, matching HasComponent's removeOnExit,
 * control.c), an absent one only LogErrors in the C# (a true no-op). */
typedef struct { const fsm_pv *gameObject, *component; } st_dcomp;
static void dcomp_bind(act_inst *a) { ST(st_dcomp); s->gameObject = FIELD(gameObject); s->component = FIELD(component); }
static void dcomp_enter(act_inst *a)
{
    ST(st_dcomp);
    int32_t t = p_owner_default(a, s->gameObject);
    const char *cn = w_str(w, ps(f, s->component));
    if (t >= 0 && go_has_component(w, t, cn)) HKSIM_UNIMPLEMENTED("DestroyComponent('%s') on '%s': runtime component removal is not modelled", cn, go_path(w, t));
    act_finish(a);
}
static const act_vtable AV_DestroyComponent = { "DestroyComponent", sizeof(st_dcomp), dcomp_bind, dcomp_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* CreatePoolObjects -- ACT/CreatePoolObjects.cs:40-84 (RigidBody2dActionBase, but never touches rb2d/velocity
 * here): `amount` fresh Object.Instantiate copies at pool's position (+ `position` offset), each nudged by
 * originVariationX/Y (a null field in this class's Reset(), so IsNone means "not wired", the FlingObjects*
 * convention already used above), reparented under `pool`, optionally deactivated.  originAdjusted is one
 * action-level flag that, once set by any clone, also re-writes every later clone's already-current position
 * (:60-73), the same quirk ported for SpawnRandomObjects's V1/V2 branches below. */
typedef struct { const fsm_pv *gameObject, *pool, *position, *amount, *ovx, *ovy, *deactivate; } st_cpo;
static void cpo_bind(act_inst *a)
{
    ST(st_cpo);
    s->gameObject = FIELD(gameObject); s->pool = FIELD(pool); s->position = FIELD(position); s->amount = FIELD(amount);
    s->ovx = a_field(a, "originVariationX"); s->ovy = a_field(a, "originVariationY"); s->deactivate = a_field(a, "deactivate");
}
static void cpo_enter(act_inst *a)
{
    ST(st_cpo);
    int32_t prefab = pgo(f, s->gameObject);
    if (prefab >= 0) {
        int32_t pool = pgo(f, s->pool);
        float base_pos[3] = { 0, 0, 0 };
        if (pool >= 0) go_world_pos(w, pool, base_pos);
        if (!p_isnone(s->position)) { const float *o = pv3(f, s->position); base_pos[0] += o[0]; base_pos[1] += o[1]; base_pos[2] += o[2]; }
        int32_t num = pi(f, s->amount);
        bool ovx = s->ovx && !p_isnone(s->ovx), ovy = s->ovy && !p_isnone(s->ovy);
        bool deact = s->deactivate && !p_isnone(s->deactivate) && pb(f, s->deactivate);
        int origin_adjusted = 0;
        for (int32_t i = 1; i <= num; i++) {
            int32_t c = world_instantiate_at(w, prefab, base_pos, 0.0f);
            float p[3]; go_world_pos(w, c, p);
            if (ovx) { p[0] += hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - pf(f, s->ovx), pf(f, s->ovx)); origin_adjusted = 1; }
            if (ovy) { p[1] += hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - pf(f, s->ovy), pf(f, s->ovy)); origin_adjusted = 1; }
            if (origin_adjusted) go_set_world_pos(w, c, p);
            go_set_parent(w, c, pool);
            if (deact) go_set_active(w, c, false);
        }
    }
    act_finish(a);
}
static const act_vtable AV_CreatePoolObjects = { "CreatePoolObjects", sizeof(st_cpo), cpo_bind, cpo_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* FlingObjects -- ACT/FlingObjects.cs:35-82: sets the velocity (and optionally moves) every EXISTING child of
 * `containerObject`, not a spawn -- unlike the FlingObjectsFromGlobalPool family above.  childCount is read
 * once before the loop (:52); this port doesn't reparent or destroy inside the loop, so a live walk is
 * equivalent. */
typedef struct { const fsm_pv *containerObject, *adjustPosition, *randomisePosition, *speedMin, *speedMax, *angleMin, *angleMax; } st_flo;
static void flo_bind(act_inst *a)
{
    ST(st_flo);
    s->containerObject = FIELD(containerObject); s->adjustPosition = FIELD(adjustPosition); s->randomisePosition = FIELD(randomisePosition);
    s->speedMin = FIELD(speedMin); s->speedMax = FIELD(speedMax); s->angleMin = FIELD(angleMin); s->angleMax = FIELD(angleMax);
}
static void flo_enter(act_inst *a)
{
    ST(st_flo);
    int32_t cont = pgo(f, s->containerObject);
    if (cont >= 0) {
        bool has_adj = !p_isnone(s->adjustPosition), randomise = has_adj && pb(f, s->randomisePosition);
        const float *adj = has_adj ? pv3(f, s->adjustPosition) : NULL;
        for (int32_t c = w->gos[cont].first_child; c >= 0; c = w->gos[c].next_sibling) {
            if (!go_has_rb(w, c)) continue;                                 /* CacheRigidBody2d: skip if rb2d stays null */
            float spd = hk_rng_range_f_site(w->rng, a->rng_site, pf(f, s->speedMin), pf(f, s->speedMax));
            float ang = hk_rng_range_f_site(w->rng, a->rng_site, pf(f, s->angleMin), pf(f, s->angleMax));
            float v[2] = { spd * m_cos(ang * ((float)M_PI / 180.0f)), spd * m_sin(ang * ((float)M_PI / 180.0f)) };
            go_set_velocity(w, c, v);
            if (has_adj) {
                float p[3]; go_world_pos(w, c, p);
                if (randomise) {
                    p[0] += hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - adj[0], adj[0]);
                    p[1] += hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - adj[1], adj[1]);
                } else { p[0] += adj[0]; p[1] += adj[1]; p[2] += adj[2]; }
                go_set_world_pos(w, c, p);
            }
        }
    }
    act_finish(a);
}
static const act_vtable AV_FlingObjects = { "FlingObjects", sizeof(st_flo), flo_bind, flo_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* SpawnRandomObjectsOverTime -- ACT/SpawnRandomObjectsOverTime.cs:56-119: like SpawnRandomObjects (above) but
 * fired from OnUpdate on a `frequency` timer, one shared `originVariation` draw pair (x then y, V1's
 * convention), and rb2d.velocity written with NO null guard (:110-116) -- a prefab without a Rigidbody2D NREs
 * in the game, the same trap SpawnRandomObjects already gives it. */
typedef struct { const fsm_pv *gameObject, *spawnPoint, *position, *frequency, *spawnMin, *spawnMax, *speedMin, *speedMax, *angleMin, *angleMax, *ov; float timer; } st_sroot;
static void sroot_bind(act_inst *a)
{
    ST(st_sroot);
    s->gameObject = FIELD(gameObject); s->spawnPoint = FIELD(spawnPoint); s->position = FIELD(position);
    s->frequency = FIELD(frequency); s->spawnMin = FIELD(spawnMin); s->spawnMax = FIELD(spawnMax);
    s->speedMin = FIELD(speedMin); s->speedMax = FIELD(speedMax); s->angleMin = FIELD(angleMin); s->angleMax = FIELD(angleMax);
    s->ov = a_field(a, "originVariation"); s->timer = 0.0f;
}
static void sroot_update(act_inst *a)
{
    ST(st_sroot);
    s->timer += w->dt;
    if (s->timer < pf(f, s->frequency)) return;
    s->timer = 0.0f;
    int32_t prefab = pgo(f, s->gameObject);
    if (prefab < 0) return;
    int32_t num = hk_rng_range_i_site(w->rng, a->rng_site, pi(f, s->spawnMin), pi(f, s->spawnMax) + 1);
    if (num >= 1 && w->sc->gos[prefab].rb < 0)
        HKSIM_UNIMPLEMENTED("SpawnRandomObjectsOverTime: '%s' has no Rigidbody2D, so rb2d.velocity throws in %s", go_name(w, prefab), fsm_label(f));
    float base_pos[3]; spawn_pose(a, s->spawnPoint, s->position, NULL, base_pos);
    for (int32_t i = 1; i <= num; i++) {
        int32_t c = world_instantiate_at(w, prefab, base_pos, 0.0f);
        if (s->ov) {                                                        /* :99-105 */
            float p[3]; go_world_pos(w, c, p);
            p[0] += hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - pf(f, s->ov), pf(f, s->ov));
            p[1] += hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - pf(f, s->ov), pf(f, s->ov));
            go_set_world_pos(w, c, p);
        }
        float spd = hk_rng_range_f_site(w->rng, a->rng_site, pf(f, s->speedMin), pf(f, s->speedMax));
        float ang = hk_rng_range_f_site(w->rng, a->rng_site, pf(f, s->angleMin), pf(f, s->angleMax));
        float v[2] = { spd * m_cos(ang * ((float)M_PI / 180.0f)), spd * m_sin(ang * ((float)M_PI / 180.0f)) };
        go_set_velocity(w, c, v);
    }
}
static const act_vtable AV_SpawnRandomObjectsOverTime = { "SpawnRandomObjectsOverTime", sizeof(st_sroot), sroot_bind, NULL, sroot_update, NULL, NULL, NULL, NULL, NULL };

/* SpawnRandomObjectsOverTimeV2 -- ACT/SpawnRandomObjectsOverTimeV2.cs:63-149: separate originVariationX/Y
 * draws with the same persistent-`originAdjusted` quirk as CreatePoolObjects/SpawnRandomObjects V2, an
 * explicit `GetComponent<Rigidbody2D>() != null` guard (no trap), and a scaleMin/scaleMax clone scale. */
typedef struct {
    const fsm_pv *gameObject, *spawnPoint, *position, *frequency, *spawnMin, *spawnMax, *speedMin, *speedMax,
                 *angleMin, *angleMax, *ovx, *ovy, *scaleMin, *scaleMax;
    float timer; int origin_adjusted;
} st_sroot2;
static void sroot2_bind(act_inst *a)
{
    ST(st_sroot2);
    s->gameObject = FIELD(gameObject); s->spawnPoint = FIELD(spawnPoint); s->position = FIELD(position);
    s->frequency = FIELD(frequency); s->spawnMin = FIELD(spawnMin); s->spawnMax = FIELD(spawnMax);
    s->speedMin = FIELD(speedMin); s->speedMax = FIELD(speedMax); s->angleMin = FIELD(angleMin); s->angleMax = FIELD(angleMax);
    s->ovx = a_field(a, "originVariationX"); s->ovy = a_field(a, "originVariationY");
    s->scaleMin = FIELD(scaleMin); s->scaleMax = FIELD(scaleMax); s->timer = 0.0f; s->origin_adjusted = 0;
}
static void sroot2_update(act_inst *a)
{
    ST(st_sroot2);
    s->timer += w->dt;
    if (s->timer < pf(f, s->frequency)) return;
    s->timer = 0.0f;
    int32_t prefab = pgo(f, s->gameObject);
    if (prefab < 0) return;
    float base_pos[3]; spawn_pose(a, s->spawnPoint, s->position, NULL, base_pos);
    int32_t num = hk_rng_range_i_site(w->rng, a->rng_site, pi(f, s->spawnMin), pi(f, s->spawnMax) + 1);
    for (int32_t i = 1; i <= num; i++) {
        int32_t c = world_instantiate_at(w, prefab, base_pos, 0.0f);
        float p[3]; go_world_pos(w, c, p);
        if (s->ovx && !p_isnone(s->ovx)) { p[0] += hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - pf(f, s->ovx), pf(f, s->ovx)); s->origin_adjusted = 1; }
        if (s->ovy && !p_isnone(s->ovy)) { p[1] += hk_rng_range_f_site(w->rng, a->rng_site, 0.0f - pf(f, s->ovy), pf(f, s->ovy)); s->origin_adjusted = 1; }
        if (s->origin_adjusted) go_set_world_pos(w, c, p);
        if (go_has_rb(w, c)) {
            float spd = hk_rng_range_f_site(w->rng, a->rng_site, pf(f, s->speedMin), pf(f, s->speedMax));
            float ang = hk_rng_range_f_site(w->rng, a->rng_site, pf(f, s->angleMin), pf(f, s->angleMax));
            float v[2] = { spd * m_cos(ang * ((float)M_PI / 180.0f)), spd * m_sin(ang * ((float)M_PI / 180.0f)) };
            go_set_velocity(w, c, v);
        }
        float sc = hk_rng_range_f_site(w->rng, a->rng_site, pf(f, s->scaleMin), pf(f, s->scaleMax));
        if (sc != 1.0f) { float scv[3] = { sc, sc, sc }; go_set_local_scale(w, c, scv); }
    }
}
static const act_vtable AV_SpawnRandomObjectsOverTimeV2 = { "SpawnRandomObjectsOverTimeV2", sizeof(st_sroot2), sroot2_bind, NULL, sroot2_update, NULL, NULL, NULL, NULL, NULL };

const act_vtable *const act_registry_objects[] = {
    &AV_SpawnObjectFromGlobalPool, &AV_SpawnObjectFromGlobalPoolOverTime, &AV_SpawnObjectFromGlobalPoolOverTimeV2,
    &AV_FlingObjectsFromGlobalPool, &AV_FlingObjectsFromGlobalPoolVel, &AV_FlingObjectsFromGlobalPoolTime,
    &AV_SpawnRandomObjectsV2, &AV_SpawnRandomObjects, &AV_SpawnFromPool, &AV_CreateObject, &AV_SpawnBlood,
    &AV_SpawnBloodTime, &AV_PreSpawnCorpse, &AV_RecycleSelf, &AV_ActivateGameObject, &AV_ActivateAllChildren,
    &AV_DestroySelf, &AV_DestroyObject, &AV_DestroyAllChildren,
    &AV_GetRandomChild, &AV_GetNextChild, &AV_GetChildCount, &AV_SetLayer, &AV_GetYDistance, &AV_AddComponent,
    &AV_DestroyComponent, &AV_CreatePoolObjects, &AV_FlingObjects, &AV_SpawnRandomObjectsOverTime, &AV_SpawnRandomObjectsOverTimeV2,
};
const int act_registry_objects_n = (int)(sizeof act_registry_objects / sizeof act_registry_objects[0]);
