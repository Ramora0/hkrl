/* GG_Grimm_Nightmare (Nightmare King Grimm): GrimmballControl and FireGrimmBall. */
#include "../actions/act.h"
#include "../lifecycle.h"
#include "core/alloc.h"

/* FireGrimmBall -- analysis/decomp/Assembly-CSharp/FireGrimmBall.cs:24-37.  Writes TweenY and Force onto
 * storedObject's GrimmballControl and calls Fire().  The decomp's guards are `if ((bool)storedObject.Value)`
 * then `if ((bool)component)`, so a null stored object or a target without the component is a silent
 * no-op, as in the game. */
typedef struct { const fsm_pv *storedObject, *tweenY, *force; } st_fgb;
static void fgb_bind(act_inst *a)
{
    ST(st_fgb);
    s->storedObject = FIELD(storedObject); s->tweenY = FIELD(tweenY); s->force = FIELD(force);
}
static void fgb_enter(act_inst *a)
{
    ST(st_fgb);
    int32_t go = pgo(f, s->storedObject);
    if (go >= 0 && go_has_component(w, go, "GrimmballControl"))
        grimmball_fire(w, go, pf(f, s->tweenY), pf(f, s->force));
    act_finish(a);
}
static const act_vtable AV_FireGrimmBall = { "FireGrimmBall", sizeof(st_fgb), fgb_bind, fgb_enter, NULL, NULL, NULL, NULL, NULL, NULL };

/* GrimmballControl — HK/GrimmballControl.cs: Nightmare Grimm's fire balls (ASSET `Flameball Grimmballoon`: layer 12,
 * CircleCollider2D + Rigidbody2D + DamageHero, dumps/GG_Grimm_Nightmare/hierarchy.json.gz), fired by FireGrimmBall
 * from `Nightmare Grimm Boss | Control` Fire Low/Mid/High L+R and Alt.  Coroutine segments follow
 * docs/engine-lifecycle.md R6: the first MoveNext runs in the caller's stage, then
 * WaitForFixedUpdate resumes in fixed_delayed and `yield return null` in update_delayed (lifecycle.c). */
static grimmball_inst *grimmball_slot(fsm_world *w, int32_t go)
{
    for (int32_t i = 0; i < w->n_grimmballs; i++)
        if (w->grimmballs[i].phase && w->grimmballs[i].go == go) return &w->grimmballs[i];
    return NULL;
}

/* GrimmballControl.Fire :96-99 -- `if (fireRoutine == null) fireRoutine = StartCoroutine(DoFire())`: a second
 * Fire() on a ball already firing is ignored.  DoFire :105-107:
 *     iTween.MoveBy(new Vector3(0f, tweenY + Random.Range(-0.2f, 0.2f), 0f), "time", 0.7f, easeOutSine, World) */
void grimmball_fire(fsm_world *w, int32_t go, float tween_y, float force)
{
    if (go < 0) return;
    if (grimmball_slot(w, go)) return;                             /* :96 fireRoutine already running */

    grimmball_inst *g = NULL;
    for (int32_t i = 0; i < w->n_grimmballs; i++) if (!w->grimmballs[i].phase) { g = &w->grimmballs[i]; break; }
    if (!g) {
        if (w->n_grimmballs >= w->cap_grimmballs) {
            int32_t cap = w->cap_grimmballs ? w->cap_grimmballs * 2 : 8;
            w->grimmballs = (grimmball_inst *)realloc(w->grimmballs, (size_t)cap * sizeof(grimmball_inst));
            w->cap_grimmballs = cap;
        }
        g = &w->grimmballs[w->n_grimmballs++];
    }
    memset(g, 0, sizeof(*g));
    g->go = go; g->force = force; g->tween_y = tween_y; g->phase = 1;
    g->max_life = 10.0f;      /* GrimmballControl.maxLifeTime; hierarchy.json.gz records 10.0 on the prefab */

    float amount[3] = { 0.0f, tween_y + hk_rng_range_f_site(w->rng, hk_rng_site(go_path(w, go), "GrimmballControl.DoFire", "", 0), -0.2f, 0.2f), 0.0f };
    itween_move_by(w, go, amount, 0 /* Space.World */, 0.7f, 0.0f, 0.0f, false,
                   13 /* easeOutSine */, 0 /* LoopType.none */, false, -1, NULL, false, false, -1, -1);
    /* DoFire's first segment runs inside Fire(): elapsed = 0 < maxLifeTime -> AddForce, then WaitForFixedUpdate */
    float fv[2] = { force, 0.0f };
    go_add_force(w, go, fv);                                       /* :119 body.AddForce(new Vector2(force,0), Force) */
    lc_grimmball_fire(w, go);
}

/* DoHit :83-93 -- stops DoFire and starts Shrink.  Shrink :125-140 stops the particles, disables the collider,
 * scales to zero over 0.5s linear, damps velocity by 0.85 each frame, then Recycles.  Its first segment runs
 * inside DoHit. */
void grimmball_do_hit(fsm_world *w, int32_t go)
{
    grimmball_inst *g = grimmball_slot(w, go);
    if (!g || g->phase == 2) return;                               /* :85 the !hit guard at the call site */
    g->phase = 2; g->shrink = 0.0f;

    col_inst *c = go_first_collider(w, go);                        /* :52 col = GetComponent<Collider2D>() */
    if (c) col_set_enabled(w, c, false);                           /* :128 col.enabled = false */
    float zero[3] = { 0.0f, 0.0f, 0.0f };
    itween_scale_to(w, go, zero, 0.5f, 0.0f, 21 /* linear */, 0, false, -1, NULL, false, false, -1, -1);   /* :131 */
    float v[2]; go_velocity(w, go, v);                             /* loop body, elapsed = 0 < 0.5 */
    v[0] *= 0.85f; v[1] *= 0.85f;                                  /* :134 body.velocity *= 0.85f */
    go_set_velocity(w, go, v);
    lc_grimmball_hit(w, go);                                       /* yield return null */
}

/* the lifecycle's entry points (lifecycle.c) */
int grimmball_phase(fsm_world *w, int32_t go) { grimmball_inst *g = grimmball_slot(w, go); return g ? g->phase : 0; }

/* GrimmballControl.OnEnable :58-67: force = tweenY = 0; col.enabled = true; hit = false; particles Play();
 * localScale = Vector3.one -- a pooled ball is reused after a hit. */
void grimmball_on_enable(fsm_world *w, int32_t go)
{
    grimmball_inst *g = grimmball_slot(w, go);
    if (g) { g->force = 0.0f; g->tween_y = 0.0f; }
    col_inst *c = go_first_collider(w, go);
    if (c) col_set_enabled(w, c, true);
    float one[3] = { 1.0f, 1.0f, 1.0f };
    go_set_local_scale(w, go, one);
}
/* GrimmballControl.OnDisable :69-77: iTween.Stop(gameObject) and StopCoroutine(fireRoutine); deactivating the
 * object stops Shrink as well (a deactivated object's coroutines are stopped: lifecycle.c coro_stop_go). */
void grimmball_on_disable(fsm_world *w, int32_t go)
{
    itween_stop_type(w, go, "");                                   /* iTween.Stop(GameObject): every tween on it */
    grimmball_inst *g = grimmball_slot(w, go);
    if (g) g->phase = 0;
}
/* DoFire after `yield return new WaitForFixedUpdate()` :113-121: elapsed += Time.fixedDeltaTime; while
 * elapsed < maxLifeTime AddForce and yield again; then DoHit().  Returns 1 when it yields again. */
int grimmball_fire_resume(fsm_world *w, int32_t go)
{
    grimmball_inst *g = grimmball_slot(w, go);
    if (!g || g->phase != 1) return 0;
    g->elapsed += w->fixed_dt;                                     /* :117 elapsed += Time.fixedDeltaTime */
    if (g->elapsed < g->max_life) {
        float fv[2] = { g->force, 0.0f };
        go_add_force(w, go, fv);                                   /* :119 */
        return 1;
    }
    grimmball_do_hit(w, go);                                       /* :122 */
    return 0;
}
/* Shrink after `yield return null` :132-138: elapsed += Time.deltaTime; while elapsed < 0.5 damp and yield
 * again; then shrinkRoutine = null and Recycle(). */
int grimmball_shrink_resume(fsm_world *w, int32_t go, float dt)
{
    grimmball_inst *g = grimmball_slot(w, go);
    if (!g || g->phase != 2) return 0;
    g->shrink += dt;
    if (g->shrink < 0.5f) {
        float v[2]; go_velocity(w, go, v);
        v[0] *= 0.85f; v[1] *= 0.85f;
        go_set_velocity(w, go, v);
        return 1;
    }
    g->phase = 0;
    world_pool_recycle(w, go);                                     /* :138 gameObject.Recycle() */
    return 0;
}

const act_vtable *const act_registry_grimm[] = { &AV_FireGrimmBall };
const int act_registry_grimm_n = (int)(sizeof act_registry_grimm / sizeof act_registry_grimm[0]);
