/* Observation sources for the glue (sim/obs) -- analysis/specs/obs-wire.md §3, oracle/Game/HitboxObserver.cs (HO) and
 * oracle/Game/FsmObserver.cs (FO).
 *   Membership is one predicate evaluated on the world at observation time (HitboxObserver.Classify), not a set of
 *   colliders registered as they appear: every collider of the FSM world -- the scene's and every runtime clone's --
 *   whose GameObject is active is classified by the first matching rule (bucket_now), in collider order (scene.json
 *   row order = Resources.FindObjectsOfTypeAll order, obs-wire.md §3.8; runtime clones after the scene's).
 *   combat_sources: the Enemy bucket (armed rows included), then the Attack bucket, with the per-row source values
 *   the glue turns into the 14 features (obs_combat_fill).
 *   fsm_snapshots: "<src>|<owner>|<fsm>|<state>" in FsmObserver order (FO:67-131). */
#include "fsm/fsm.h"
#include "obs/obs.h"
#include "world_internal.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "core/alloc.h"   /* per-instance arena */

enum { BK_NONE = -1, BK_KNIGHT = 0, BK_ENEMY = 1, BK_ATTACK = 2, BK_TERRAIN = 3 };   /* HitboxObserver.Buckets */

/* Static facts of a collider's own GameObject (HitboxReader.Facts): components are not added or removed in a fight. */
enum { F_SUPPORTED = 1, F_DAMAGE_SOURCE = 2, F_ATTACK = 4 };

static bool fsm_blacklisted(const char *name);

/* HO:209-215 Strip: cut at the first "(Clone)", Trim() */
static int32_t strip_name(fsm_world *w, const char *name)
{
    char buf[256];
    const char *p = strstr(name, "(Clone)");
    size_t len = p ? (size_t)(p - name) : strlen(name);
    if (len >= sizeof buf) len = sizeof buf - 1;
    memcpy(buf, name, len); buf[len] = 0;
    size_t s = 0; while (buf[s] == ' ' || buf[s] == '\t') s++;
    size_t e = len; while (e > s && (buf[e - 1] == ' ' || buf[e - 1] == '\t')) e--;
    buf[e] = 0;
    return w_intern(w, buf + s);
}
/* HO:144-186 GetParentHm / ClassifyParent: HealthManager on self or up to 7 ancestors (depth < 8) */
static hm_inst *hm_within8(fsm_world *w, int32_t go)
{
    for (int depth = 0; go >= 0 && depth < 8; depth++, go = w->gos[go].parent) {
        hm_inst *h = hm_of_go(w, go);
        if (h) return h;
    }
    return NULL;
}
/* HO:81-89, 188-207 GetKind / ClassifyEntity: nearest transform (self or ≤ 7 ancestors) carrying a tk2dSpriteAnimator; the animator is kept for the clip key */
static int32_t kind_of(fsm_world *w, int32_t go, anim_inst **anim_out)
{
    *anim_out = NULL;
    for (int depth = 0, g = go; g >= 0 && depth < 8; depth++, g = w->gos[g].parent) {
        if (go_has_component(w, g, "tk2dSpriteAnimator")) { *anim_out = anim_of_go(w, g); return strip_name(w, go_name(w, g)); }
    }
    int32_t s = strip_name(w, go_name(w, go));
    return *w_str(w, s) ? s : w_intern(w, "unknown");
}
/* HO:99-122 clip key: Strip(anim.gameObject.name) + "|" + clip.name; "none" without an animator/clip.  The game
 * computes this ONCE per clip asset (clipKeyCache[clip], HO:116-120) -- see obs_clip_key for the cache. */
static int32_t clip_key_of(fsm_world *w, const anim_inst *an)
{
    if (!an || an->cur_clip < 0) return w_intern(w, "none");
    char buf[320];
    snprintf(buf, sizeof buf, "%s|%s", w_str(w, strip_name(w, go_name(w, an->go))), w_str(w, w->sc->clips[an->cur_clip].name));
    return w_intern(w, buf);
}
static col_inst *col_inst_of(fsm_world *w, int32_t ci)
{
    const col_def *cd = &w->sc->cols[ci];
    go_inst *g = &w->gos[cd->go];
    for (int32_t k = 0; k < g->n_cols; k++) if (g->cols[k].def == cd) return &g->cols[k];
    return NULL;
}

/* Per-world observer cache, per scene collider index (grown with runtime clones, obs_sync): the static facts, and
 * the kind / animator / HealthManager latched on a collider's first report (obs_resolve_kind). */
typedef struct obs_cache {
    int32_t n;                            /* colliders covered: w->sc->n_cols at the last obs_sync */
    col_inst **col;
    uint8_t *facts;                       /* F_* */
    int32_t *dh_fsm;                      /* the damages_hero FSM on the collider's GameObject, -1 none (HeroBox.cs:43) */
    int32_t *kind, *anim_go, *hm;         /* kind: interned, -1 until latched; anim_go / hm: -2 unresolved, -1 none */
    uint8_t *ter_mapped;                  /* a core static or dynamic-terrain entry stands for it (world_terrain_live) */
    int32_t *unmapped; int32_t n_unmapped; /* supported colliders no core terrain entry stands for, once the map is built */
    int32_t *rows_ci;                     /* world_combat_sources scratch: the Enemy then the Attack colliders, this step */
    int32_t *clip_key;                    /* per clip_def: the interned key latched on first report, -1 until then */
    int32_t none_key;
    int32_t n_fsm_black; uint8_t *fsm_black;   /* per fsm: blacklisted name */
    int32_t hero_box_go;                  /* the Knight's HeroBox (HeroBox.cs), whose layer the Enemy rule tests */
    /* world_terrain_live: core static / dynamic-terrain index -> scene collider index (-1 not in the FSM world) */
    int32_t *static_col, *dyn_col; uint32_t n_static_map, n_dyn_map; uint8_t terrain_map_built;
    int32_t **boss_order; int32_t *boss_n; /* per HM: subtree GO order (DFS), re-walked every snapshot */
} obs_cache;

static void obs_dfs(fsm_world *w, int32_t go, int32_t **list, int32_t *n, int32_t *cap)
{
    if (*n == *cap) { *cap = *cap ? *cap * 2 : 32; *list = realloc(*list, sizeof(int32_t) * (size_t)*cap); }
    (*list)[(*n)++] = go;
    for (int32_t c = w->gos[go].first_child; c >= 0; c = w->gos[c].next_sibling) obs_dfs(w, c, list, n, cap);
}

/* HitboxReader.FactsOf: the shapes the observer classifies (HO: Box/Polygon/Edge/Circle), a damage source by
 * HeroBox.CheckForDamage's same-object test (a damages_hero FSM, HeroBox.cs:43, or a DamageHero, :58), and the
 * Attack markers (DamageEnemies, a damages_enemy FSM, a "Damager" with a "Damage" FSM). */
static void obs_facts(fsm_world *w, obs_cache *oc, int32_t ci)
{
    const col_def *cd = &w->sc->cols[ci];
    int32_t go = cd->go;
    uint8_t f = 0;
    oc->col[ci] = col_inst_of(w, ci);
    oc->dh_fsm[ci] = -1;
    if (cd->type == COL_BOX || cd->type == COL_POLYGON || cd->type == COL_EDGE || cd->type == COL_CIRCLE) {
        f |= F_SUPPORTED;
        oc->dh_fsm[ci] = world_fsm_locate(w, go, "damages_hero");
        if (oc->dh_fsm[ci] >= 0 || w->gos[go].dh >= 0) f |= F_DAMAGE_SOURCE;
        if (go_has_component(w, go, "DamageEnemies") || world_fsm_locate(w, go, "damages_enemy") >= 0 ||
            (strcmp(go_name(w, go), "Damager") == 0 && world_fsm_locate(w, go, "Damage") >= 0)) f |= F_ATTACK;
    }
    oc->facts[ci] = f;
    oc->kind[ci] = -1; oc->anim_go[ci] = -2; oc->hm[ci] = -2;
    oc->ter_mapped[ci] = 0;
}

/* Cover every collider and FSM the world has now: a runtime clone adds both (world_instantiate). */
static void obs_sync(fsm_world *w, obs_cache *oc)
{
    int32_t n = w->sc->n_cols;
    if (n > oc->n) {
        size_t m = (size_t)n;
        oc->col = realloc(oc->col, sizeof *oc->col * m);
        oc->facts = realloc(oc->facts, m);
        oc->dh_fsm = realloc(oc->dh_fsm, sizeof(int32_t) * m);
        oc->kind = realloc(oc->kind, sizeof(int32_t) * m);
        oc->anim_go = realloc(oc->anim_go, sizeof(int32_t) * m);
        oc->hm = realloc(oc->hm, sizeof(int32_t) * m);
        oc->ter_mapped = realloc(oc->ter_mapped, m);
        oc->unmapped = realloc(oc->unmapped, sizeof(int32_t) * m);
        oc->rows_ci = realloc(oc->rows_ci, sizeof(int32_t) * m);
        HKSIM_ASSERT(oc->col && oc->facts && oc->dh_fsm && oc->kind && oc->anim_go && oc->hm && oc->ter_mapped && oc->unmapped &&
                     oc->rows_ci, "out of memory growing the observer cache");
        for (int32_t ci = oc->n; ci < n; ci++) {
            obs_facts(w, oc, ci);
            if (oc->terrain_map_built && (oc->facts[ci] & F_SUPPORTED)) oc->unmapped[oc->n_unmapped++] = ci;
        }
        oc->n = n;
    }
    if (w->n_fsms > oc->n_fsm_black) {
        oc->fsm_black = realloc(oc->fsm_black, (size_t)w->n_fsms);
        HKSIM_ASSERT(oc->fsm_black != NULL, "out of memory growing the observer cache");
        for (int32_t i = oc->n_fsm_black; i < w->n_fsms; i++) oc->fsm_black[i] = fsm_blacklisted(w_str(w, w->fsms[i].def->fsm_name)) ? 1 : 0;
        oc->n_fsm_black = w->n_fsms;
    }
}

static obs_cache *obs_get(fsm_world *w)
{
    obs_cache *oc = (obs_cache *)w->obs_cache;
    if (!oc) {
        oc = calloc(1, sizeof *oc);
        oc->clip_key = malloc(sizeof(int32_t) * (size_t)(w->sc->n_clips > 0 ? w->sc->n_clips : 1));
        for (int32_t i = 0; i < w->sc->n_clips; i++) oc->clip_key[i] = -1;
        oc->none_key = w_intern(w, "none");
        oc->boss_order = calloc((size_t)(w->n_hms > 0 ? w->n_hms : 1), sizeof(int32_t *));
        oc->boss_n = calloc((size_t)(w->n_hms > 0 ? w->n_hms : 1), sizeof(int32_t));
        oc->hero_box_go = -1;
        if (w->knight_go >= 0) {
            int32_t *sub = NULL, nsub = 0, cap = 0;
            obs_dfs(w, w->knight_go, &sub, &nsub, &cap);
            for (int32_t k = 0; k < nsub && oc->hero_box_go < 0; k++) if (go_has_component(w, sub[k], "HeroBox")) oc->hero_box_go = sub[k];
            free(sub);
        }
        w->obs_cache = oc;
    }
    obs_sync(w, oc);
    return oc;
}

/* Layers i and j collide in the Physics2D layer matrix (dumps/<scene>/physics.json#layerCollisionMatrix). */
static bool layers_collide(const fsm_world *w, int32_t i, int32_t j)
{
    return i >= 0 && i < 32 && j >= 0 && j < 32 && ((w->sc->layer_matrix[i] >> j) & 1u);
}

/* HitboxObserver.Classify's rule for one collider, now.  Enemy includes an armed row (collider disabled, object
 * active); every other bucket needs the collider enabled. */
static int bucket_now(fsm_world *w, obs_cache *oc, int32_t ci)
{
    const col_inst *c = oc->col[ci];
    int32_t go = w->sc->cols[ci].go;
    uint8_t f = oc->facts[ci];
    if (!c || !(f & F_SUPPORTED) || !go_active_in_hierarchy(w, go)) return BK_NONE;
    if (f & F_DAMAGE_SOURCE) {
        if (oc->hero_box_go < 0) HKSIM_UNIMPLEMENTED("observer: the Knight has no HeroBox, so the Enemy rule has no layer to test");
        if (layers_collide(w, go_layer(w, go), go_layer(w, oc->hero_box_go))) return BK_ENEMY;
    }
    if (!c->enabled) return BK_NONE;
    if (go_layer(w, go) == 8 && !c->is_trigger) return BK_TERRAIN;                 /* PhysLayers.TERRAIN */
    if (go == w->knight_go && !c->is_trigger) return BK_KNIGHT;
    if (f & F_ATTACK) return BK_ATTACK;
    return BK_NONE;
}
static bool live(const obs_cache *oc, int32_t ci) { return oc->col[ci]->enabled; }   /* isActiveAndEnabled, given bucket_now */

void world_obs_cache_free(fsm_world *w)
{
    obs_cache *oc = (obs_cache *)w->obs_cache;
    if (!oc) return;
    free(oc->col); free(oc->facts); free(oc->dh_fsm); free(oc->kind); free(oc->anim_go); free(oc->hm); free(oc->ter_mapped);
    free(oc->unmapped); free(oc->rows_ci);
    free(oc->clip_key); free(oc->fsm_black); free(oc->static_col); free(oc->dyn_col);
    for (int32_t i = 0; i < w->n_hms; i++) free(oc->boss_order[i]);
    free(oc->boss_order); free(oc->boss_n); free(oc);
    w->obs_cache = NULL;
}
/* HitboxReader.GetClipKey (oracle/Game/HitboxObserver.cs) caches the key PER CLIP ASSET
 * (Dictionary<tk2dSpriteAnimationClip, string> clipKeyCache), computed from whichever animator reports that clip
 * FIRST in the reader's lifetime; every other animator playing that clip reports the first one's name (e.g.
 * GG_Grimm_Nightmare's `Nightmare Spike (N)` rows).  The reader is recreated on every reset (TrainingEnv
 * RecreateReader), as is this world.  "First" is in this module's row order (collider order). */
static const char *obs_clip_key(fsm_world *w, obs_cache *oc, anim_inst *an)
{
    if (!an || an->cur_clip < 0) return w_str(w, oc->none_key);
    int32_t *k = &oc->clip_key[an->cur_clip];
    if (*k < 0) *k = clip_key_of(w, an);
    return w_str(w, *k);
}

/* TrainingEnv.HasActiveCombatHitboxes: any Enemy-bucket collider that is isActiveAndEnabled.  The Attack bucket is
 * deliberately NOT scanned -- it holds the Knight's own attack colliders, which are live during play and would make
 * this true regardless of the boss. */
bool world_has_active_enemy_hitbox(fsm_world *w)
{
    obs_cache *oc = obs_get(w);
    for (int32_t ci = 0; ci < oc->n; ci++)
        if (bucket_now(w, oc, ci) == BK_ENEMY && live(oc, ci)) return true;
    return false;
}

/* HitboxObserver caches the entity identity PER COLLIDER, latched on the first GetKind(col) call while BUILDING A
 * REPORTED ROW, not at scene load.  It differs when a collider is re-parented before its first report
 * (GG_Ghost_Xero's swords: `xero_nail | Init` SetParent(null) -> "Sword 1", not the boss). */
static void obs_resolve_kind(fsm_world *w, obs_cache *oc, int32_t ci)
{
    if (oc->kind[ci] >= 0) return;                                 /* already latched (kindCache hit) */
    int32_t go = w->sc->cols[ci].go;
    anim_inst *an;
    oc->kind[ci] = kind_of(w, go, &an);                            /* GetKind -> ClassifyEntity */
    oc->anim_go[ci] = -1;                                          /* ClassifyEntity also fills animCache */
    for (int depth = 0, g = go; g >= 0 && depth < 8; depth++, g = w->gos[g].parent)
        if (go_has_component(w, g, "tk2dSpriteAnimator")) { oc->anim_go[ci] = g; break; }
    /* hmCache latches the same way at the same moment: GetParentHm caches the nearest HealthManager per
     * collider, null included. */
    if (oc->hm[ci] == -2) {
        hm_inst *h = hm_within8(w, go);
        oc->hm[ci] = h ? (int32_t)(h - w->hms) : -1;
    }
}

/* The damage and hazardType HeroBox.CheckForDamage would pass on for this collider now (HitboxObserver.GivesDamage):
 * the damages_hero FSM's `damageDealt` and `hazardType` ints win over a DamageHero on the same object
 * (HeroBox.cs:43-57); a DamageHero's current damageDealt, its hazardType and its shadowDashHazard (HeroBox.cs:59-62)
 * otherwise. */
static void obs_damage(fsm_world *w, obs_cache *oc, int32_t ci, obs_combat_src *o)
{
    if (oc->dh_fsm[ci] >= 0) {
        bool fresh;
        fsm_inst *f = &w->fsms[oc->dh_fsm[ci]];
        o->damage_dealt = fsm_get_var(f, VB_INT, w_intern(w, "damageDealt"), &fresh)->i;   /* HeroBox.cs:46 */
        o->hazard_type = fsm_get_var(f, VB_INT, w_intern(w, "hazardType"), &fresh)->i;     /* HeroBox.cs:47 */
        o->shadow_dash_hazard = 0;
        return;
    }
    const dh_inst *dh = &w->dhs[w->gos[w->sc->cols[ci].go].dh];
    o->damage_dealt = dh->damage_dealt;
    o->hazard_type = dh->def->hazard_type;
    o->shadow_dash_hazard = dh->def->shadow_dash_hazard;
}

/* HitboxObserver.GetSplitFeatures, combat part: the Enemy rows then the Attack rows.  Returns the row count; fills
 * at most `cap` rows. */
uint32_t world_combat_sources(fsm_world *w, obs_combat_src *out, const char **kinds, const char **keys, uint32_t cap, float knight_x, float knight_y)
{
    obs_cache *oc = obs_get(w);
    /* A caller whose scratch is too small gets the row count back with NO state touched (no motion tick, no
     * prev_rel, no kind latch), grows, and calls again -- so the tick advances once per observation. */
    int32_t ne = 0, na = 0;
    for (int32_t ci = 0; ci < oc->n; ci++) {                       /* Classify: b.Enemy from the front, b.Attack from the back */
        int b = bucket_now(w, oc, ci);
        if (b == BK_ENEMY) oc->rows_ci[ne++] = ci;
        else if (b == BK_ATTACK) oc->rows_ci[oc->n - 1 - na++] = ci;
    }
    uint32_t rows = (uint32_t)(ne + na);
    if (rows > cap) return rows;
    world_invalidate_body_transforms(w);
    w->obs_tick++;                                                 /* MotionTick++ */
    uint32_t n = 0;
    for (int pass = 0; pass < 2; pass++) {                         /* b.Enemy, then b.Attack, each in collider order */
        int32_t cnt = pass == 0 ? ne : na;
        for (int32_t k = 0; k < cnt; k++) {
            int32_t ci = pass == 0 ? oc->rows_ci[k] : oc->rows_ci[oc->n - na + k];
            col_inst *c = oc->col[ci]; int32_t go = w->sc->cols[ci].go;
            obs_resolve_kind(w, oc, ci);                           /* GetKind, cached per collider */
            obs_combat_src *o = &out[n]; memset(o, 0, sizeof *o);
            float mn[2], mx[2];
            if (c->enabled) col_bounds(w, go, c, mn, mx);          /* col.bounds */
            else col_shape_bounds(w, go, c, mn, mx);               /* HitboxObserver.ShapeBounds: bounds is empty while disabled */
            o->armed = !c->enabled;
            o->bounds_center_x = (mn[0] + mx[0]) * 0.5f; o->bounds_center_y = (mn[1] + mx[1]) * 0.5f;
            o->bounds_size_x = mx[0] - mn[0]; o->bounds_size_y = mx[1] - mn[1];
            o->isTrigger = c->is_trigger;
            o->bucket_is_enemy = pass == 0;
            if (pass == 0) obs_damage(w, oc, ci, o);
            hm_inst *h = oc->hm[ci] >= 0 ? &w->hms[oc->hm[ci]] : NULL;
            o->has_hm = h != NULL;
            if (h) {
                /* `bossHms.Contains(hm)`: the bound set, a subset of bosses.json (every HealthManager in the loaded
                 * scenes, ReflectionDumper.cs Bosses()) */
                o->is_boss_hm = h->bound;
                o->hm_IsInvincible = h->invincible;
                o->hm_hp = h->hp;
                if (!h->obs_seen) { h->obs_seen = 1; h->obs_max_hp = h->hp; }   /* ObserveMaxHp: max(first seen, current) */
                else if (h->hp > h->obs_max_hp) h->obs_max_hp = h->hp;
                o->hm_max_hp = h->obs_max_hp;
            }
            float rel_x = o->bounds_center_x - knight_x, rel_y = o->bounds_center_y - knight_y;
            o->has_prev_rel = c->prev_tick != 0 && c->prev_tick == w->obs_tick - 1;   /* prev.tick == MotionTick - 1 */
            o->prev_rel_x = c->prev_rel[0]; o->prev_rel_y = c->prev_rel[1];
            c->prev_rel[0] = rel_x; c->prev_rel[1] = rel_y; c->prev_tick = w->obs_tick;
            anim_inst *an = oc->anim_go[ci] >= 0 ? anim_of_go(w, oc->anim_go[ci]) : NULL;
            if (kinds) kinds[n] = w_str(w, oc->kind[ci]);
            if (keys) keys[n] = obs_clip_key(w, oc, an);
            if (an && an->cur_clip >= 0) {                         /* GetClipKey */
                o->has_clip = 1;
                o->anim_CurrentFrame = anim_current_frame(w, an);
                o->clip_frames_Length = w->sc->clips[an->cur_clip].n_frames;
            }
            n++;
        }
    }
    return n;
}

/* The scene collider compiled from scene.json with this Collider2D.instanceID, or -1. */
static int32_t col_of_iid(fsm_world *w, int32_t iid)
{
    if (iid == -1) return -1;
    for (int32_t i = 0; i < w->sc->n_cols; i++) if (w->sc->cols[i].instance_id == iid) return i;
    return -1;
}

/* A collider's world outline in its own point order -- box bl, br, tr, tl (oracle/Game/HitboxObserver.cs:411-425);
 * edge/polygon path 0 in point order (:387-410) -- from the live transform.  The float sequence is the one
 * col_bounds uses (p + R(s * (local + offset))); Unity's TransformPoint goes through localToWorldMatrix, whose
 * sequence is not pinned (port-obs.md Q-pobs-1); on rotation-0 boxes fl(fl(s*l) + p) matches the dumped outline
 * points within 2 ULP.  Returns the point count. */
static uint32_t col_world_outline(fsm_world *w, int32_t go, const col_inst *c, phys_v2 *out, uint32_t cap)
{
    float p[3], s[3];
    go_world_pos(w, go, p); go_lossy_scale(w, go, s);
    float ang = go_euler_z(w, go) * ((float)M_PI / 180.0f);
    float cs = ang == 0.0f ? 1.0f : m_cos(ang), sn = ang == 0.0f ? 0.0f : m_sin(ang);
    float loc[64][2]; uint32_t n = 0;
    if (c->def->type == COL_BOX) {
        float hx = c->size[0] * 0.5f, hy = c->size[1] * 0.5f;          /* HO:415 */
        float o0 = c->offset[0], o1 = c->offset[1];
        loc[0][0] = o0 - hx; loc[0][1] = o1 - hy; loc[1][0] = o0 + hx; loc[1][1] = o1 - hy;   /* HO:417-418 */
        loc[2][0] = o0 + hx; loc[2][1] = o1 + hy; loc[3][0] = o0 - hx; loc[3][1] = o1 + hy;   /* HO:420, :419 */
        n = 4;
    } else if (c->def->type == COL_POLYGON || c->def->type == COL_EDGE) {
        if (c->def->n_pts > 64) HKSIM_UNIMPLEMENTED("terrain outline of %d points on '%s'", c->def->n_pts, go_path(w, go));
        for (int32_t i = 0; i < c->def->n_pts; i++) {
            const float *q = &w->sc->col_pts[(c->def->pts_start + i) * 2];
            loc[n][0] = q[0] + c->offset[0]; loc[n][1] = q[1] + c->offset[1]; n++;   /* HO:392-393, :407-408 local + offset */
        }
    } else {
        HKSIM_UNIMPLEMENTED("terrain outline for collider type %d on '%s' (CircleCollider2D: port-obs.md Q-pobs-2)", c->def->type, go_path(w, go));
    }
    if (n > cap) HKSIM_UNIMPLEMENTED("terrain outline buffer too small for '%s'", go_path(w, go));
    for (uint32_t i = 0; i < n; i++) {
        float lx = loc[i][0] * s[0], ly = loc[i][1] * s[1];
        out[i].x = p[0] + lx * cs - ly * sn; out[i].y = p[1] + lx * sn + ly * cs;
    }
    return n;
}

/* The live Terrain bucket for the observer (HitboxObserver.GetSplitFeatures): for each core static collider and
 * each dynamic-body terrain collider (sim/core/gen_scene.py terrain_dyn), whether it is in the Terrain bucket NOW,
 * and for the dynamic ones the current world outline.  A collider the FSM world does not contain keeps the
 * caller's default (its SceneReady state): nothing in the simulator can change it.  One the FSM world classifies
 * into another bucket (Enemy takes precedence) is not terrain at all.  A Terrain-bucket collider no core entry
 * stands for (a runtime clone's) cannot be observed by the core's terrain walk, so it traps. */
void world_terrain_live(fsm_world *w, const int32_t *static_iids, uint32_t n_static, uint8_t *static_active,
                        const int32_t *dyn_iids, uint32_t n_dyn, uint8_t *dyn_active,
                        phys_v2 *dyn_pts, const uint32_t *dyn_off, uint32_t *dyn_n, uint32_t pts_cap)
{
    obs_cache *oc = obs_get(w);
    if (!oc->terrain_map_built) {
        oc->static_col = malloc(sizeof(int32_t) * (size_t)(n_static > 0 ? n_static : 1));
        oc->dyn_col = malloc(sizeof(int32_t) * (size_t)(n_dyn > 0 ? n_dyn : 1));
        for (uint32_t i = 0; i < n_static; i++) oc->static_col[i] = col_of_iid(w, static_iids[i]);
        for (uint32_t j = 0; j < n_dyn; j++) oc->dyn_col[j] = col_of_iid(w, dyn_iids[j]);
        for (uint32_t i = 0; i < n_static; i++) if (oc->static_col[i] >= 0) oc->ter_mapped[oc->static_col[i]] = 1;
        for (uint32_t j = 0; j < n_dyn; j++) if (oc->dyn_col[j] >= 0) oc->ter_mapped[oc->dyn_col[j]] = 1;
        for (int32_t ci = 0; ci < oc->n; ci++) if (!oc->ter_mapped[ci] && (oc->facts[ci] & F_SUPPORTED)) oc->unmapped[oc->n_unmapped++] = ci;
        oc->n_static_map = n_static; oc->n_dyn_map = n_dyn;
        oc->terrain_map_built = 1;
    }
    HKSIM_ASSERT(oc->n_static_map == n_static && oc->n_dyn_map == n_dyn, "terrain table changed under the world");
    for (uint32_t i = 0; i < n_static; i++) {
        int32_t ci = oc->static_col[i];
        if (ci < 0) continue;
        static_active[i] = bucket_now(w, oc, ci) == BK_TERRAIN;
    }
    for (uint32_t j = 0; j < n_dyn; j++) {
        int32_t ci = oc->dyn_col[j];
        dyn_n[j] = 0;
        if (ci < 0) {
            if (dyn_active[j])
                HKSIM_UNIMPLEMENTED("dynamic-body terrain collider %d is active at SceneReady but not in the FSM world, "
                                    "so its pose cannot be followed", dyn_iids[j]);
            continue;
        }
        dyn_active[j] = bucket_now(w, oc, ci) == BK_TERRAIN;
        if (!dyn_active[j]) continue;
        dyn_n[j] = col_world_outline(w, w->sc->cols[ci].go, oc->col[ci], dyn_pts + dyn_off[j],
                                     pts_cap > dyn_off[j] ? pts_cap - dyn_off[j] : 0);
    }
    for (int32_t k = 0; k < oc->n_unmapped; k++) {
        int32_t ci = oc->unmapped[k];
        if (bucket_now(w, oc, ci) == BK_TERRAIN)
            HKSIM_UNIMPLEMENTED("terrain collider on '%s' is live but no core terrain entry stands for it (a runtime "
                                "clone's): the observation cannot emit its segments", go_path(w, w->sc->cols[ci].go));
    }
}

/* FsmObserver.NameBlacklist (FO:39-64) */
static const char *const FSM_BLACKLIST[] = {
    "damages_enemy", "damages_hero", "health_manager_enemy", "health_manager", "Health", "Set HP",
    "Stun Control", "Stun", "Stun Damage", "Audio", "Sounds", "Music Region", "Death", "Death Effects", "Crash Effect",
    "Shake", "shudder", "CameraShake", "Constrain X", "Constrain Y", "Bobble", "Camera Lock", "Roar Lock", "Hero Lock",
    "FSM", "Detect Range", "Recoil", NULL
};
static bool fsm_blacklisted(const char *name)
{
    for (int i = 0; FSM_BLACKLIST[i]; i++) if (strcmp(FSM_BLACKLIST[i], name) == 0) return true;
    return false;
}
static uint32_t snap_push(fsm_world *w, const char **out, uint32_t cap, uint32_t n, const char *src, const char *owner, fsm_inst *f)
{
    if (n < cap) {
        char buf[512];
        const char *state = f->active_state >= 0 ? state_name(f, f->active_state) : "";
        snprintf(buf, sizeof buf, "%s|%s|%s|%s", src, owner, w_str(w, f->def->fsm_name), *state ? state : "(none)");   /* FO:126-131 */
        out[n] = w_str(w, w_intern(w, buf));
    }
    return n + 1;
}
/* every PlayMakerFSM on `go` that isActiveAndEnabled (FO:90) and is not blacklisted (FO:91) */
static uint32_t snap_go(fsm_world *w, obs_cache *oc, const char **out, uint32_t cap, uint32_t n, const char *src, const char *owner, int32_t go)
{
    const go_def *d = w->gos[go].def;
    if (d->n_fsms == 0 || !go_active_in_hierarchy(w, go)) return n;
    for (int32_t k = 0; k < d->n_fsms; k++) {
        int32_t fi = w->sc->fsm_idx[d->fsm_start + k];
        fsm_inst *f = &w->fsms[fi];
        if (!f->component_enabled || oc->fsm_black[fi]) continue;
        n = snap_push(w, out, cap, n, src, owner, f);
    }
    return n;
}
uint32_t world_fsm_snapshots(fsm_world *w, const char **out, uint32_t cap)
{
    obs_cache *oc = obs_get(w);
    uint32_t n = 0;
    for (int32_t i = 0; i < w->n_hms; i++) {                        /* (B) boss roots, GetComponentsInChildren(true) order (FO:75-101) */
        /* FO:80 GetComponentsInChildren walks the LIVE hierarchy at snapshot time: a child that
         * SetParent-ed away (Xero's swords) reaches the observation only as an E entry through its own
         * collider (FO:103-124).  So re-walk the subtree every snapshot. */
        int32_t bcap = 0; free(oc->boss_order[i]); oc->boss_order[i] = NULL; oc->boss_n[i] = 0;
        obs_dfs(w, w->hms[i].go, &oc->boss_order[i], &oc->boss_n[i], &bcap);
        const char *owner = go_name(w, w->hms[i].go);
        for (int32_t k = 0; k < oc->boss_n[i]; k++) n = snap_go(w, oc, out, cap, n, "B", owner, oc->boss_order[i][k]);
    }
    for (int pass = 0; pass < 2; pass++) {                         /* (E) then (A): live bucket colliders, own GameObject only (FO:103-124) */
        int want = pass == 0 ? BK_ENEMY : BK_ATTACK;
        for (int32_t ci = 0; ci < oc->n; ci++) {
            if (bucket_now(w, oc, ci) != want || !live(oc, ci)) continue;
            int32_t go = w->sc->cols[ci].go;
            n = snap_go(w, oc, out, cap, n, pass == 0 ? "E" : "A", go_name(w, go), go);
        }
    }
    return n;
}
