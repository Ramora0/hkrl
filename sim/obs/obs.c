/* Observation packer: transcription of oracle/Net/BinaryProtocol.cs Pack() (BP:), StateExtractor.GetGlobalState (SE:),
 * HitboxObserver.GetSplitFeatures / EmitTerrainSegments (HO:) and the TrainingEnv payload shapes (TE:).
 * Rules: analysis/specs/obs-wire.md; design: analysis/specs/port-obs.md.  float32 everywhere the C# uses
 * float; no double except where the C# casts ((float)Math.Sqrt(double), obs-wire.md §4.2); build flags forbid FMA. */
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "obs/obs.h"
#include "core/trap.h"

/* ---- little-endian writer (BinaryWriter semantics [ENGINE], obs-wire.md §1.3: LE, f32 = IEEE binary32 bit pattern,
 *      u8 bools).  Counts every byte even past cap so a call with cap 0 sizes the reply. ---------------------------- */
typedef struct { uint8_t *buf; size_t cap, pos; } wr;

static void put_bytes(wr *w, const void *p, size_t n)
{
    if (w->buf && w->pos + n <= w->cap) memcpy(w->buf + w->pos, p, n);
    w->pos += n;
}
static void put_u8(wr *w, uint8_t v) { put_bytes(w, &v, 1); }
static void put_u16(wr *w, uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; put_bytes(w, b, 2); }
static void put_u32(wr *w, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    put_bytes(w, b, 4);
}
static void put_i32(wr *w, int32_t v) { put_u32(w, (uint32_t)v); }
static void put_f32(wr *w, float f) { uint32_t u; memcpy(&u, &f, 4); put_u32(w, u); }   /* bit copy: −0.0f preserved (obs-wire.md §4.2) */
static void patch_u16(wr *w, size_t at, uint16_t v)
{
    if (w->buf && at + 2 <= w->cap) { w->buf[at] = (uint8_t)v; w->buf[at + 1] = (uint8_t)(v >> 8); }
}
/* u8-length string: len = min(255, utf8len), truncated (BP:77-81 / BP:88-92).  Strings in the sim are UTF-8 C strings
 * (names are ASCII in the four dumps, obs-wire.md Q-obs-10). */
static void put_str8(wr *w, const char *s, const char *dflt)
{
    if (!s) s = dflt;
    size_t n = strlen(s);
    if (n > 255) n = 255;
    put_u8(w, (uint8_t)n);
    put_bytes(w, s, n);
}
/* u16-length string: cap 65535 (BP:101-105, BP:151-155). */
static void put_str16(wr *w, const char *s, const char *dflt)
{
    if (!s) s = dflt;
    size_t n = strlen(s);
    if (n > 65535) n = 65535;
    put_u16(w, (uint16_t)n);
    put_bytes(w, s, n);
}

/* ---- Mathf [ENGINE] ------------------------------------------------------------------------------------------------ */
static float mathf_clamp01(float v)          /* UnityEngine.Mathf.Clamp01 [ENGINE]; SE:82, HO:168 */
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}
static float mathf_sqrt(float v)             /* Mathf.Sqrt = (float)Math.Sqrt((double)v) [ENGINE], obs-wire.md §4.2 */
{
    return (float)sqrt((double)v);
}

/* ---- combat row features (HO:753-797) ------------------------------------------------------------------------------ */
void obs_combat_fill(obs_combat_row *row, const obs_combat_src *s, const obs_hero_view *hero)
{
    float relX = s->bounds_center_x - hero->knightPos_x;          /* HO:753 */
    float relY = s->bounds_center_y - hero->knightPos_y;          /* HO:754 */
    float w = s->bounds_size_x;                                   /* HO:755 */
    float h = s->bounds_size_y;                                   /* HO:756 */
    float isTrigger = s->isTrigger ? 1.0f : 0.0f;                 /* HO:757 */
    /* HitboxObserver.GivesDamage: an Enemy row whose collider is on and whose hit would get past the source-dependent
     * gates of the damage path: HeroBox.CheckForDamage skips a DamageHero that is a shadowDashHazard while the knight
     * shadow dashes (HeroBox.cs:59); HeroController.TakeDamage returns at damage <= 0 (HeroController.cs:1829) and,
     * for hazardType 1, in HAZARD_ONLY damage mode, while shadow dashing, or while parryInvulnTimer > 0 (:1847).
     * CanTakeDamage (:1845) is the same for every row and is not part of the feature. */
    int hazard1_immune = s->hazard_type == 1 && (hero->damageMode_hazardOnly || hero->cState_shadowDashing
                                                 || hero->parryInvulnTimer > 0.0f);
    float givesDamage = (s->bucket_is_enemy && !s->armed && s->damage_dealt > 0 && !hazard1_immune
                         && !(s->shadow_dash_hazard && hero->cState_shadowDashing)) ? 1.0f : 0.0f;
    float takesDamage = s->has_hm ? 1.0f : 0.0f;                  /* HO:764 */
    float isTarget = (s->has_hm && s->is_boss_hm) ? 1.0f : 0.0f;  /* HO:765 */
    float hpRaw = s->has_hm ? (float)s->hm_hp : 0.0f;             /* HO:766 */
    float hpMaxRaw = s->has_hm ? (float)s->hm_max_hp : 0.0f;      /* HO:767 (ObserveMaxHp HO:208-219) */
    float animPhase = 0.0f;                                       /* HO:768, HO:154 */
    if (s->has_clip && s->clip_frames_Length > 0)                 /* HO:165-166 */
        animPhase = mathf_clamp01((float)s->anim_CurrentFrame / (float)s->clip_frames_Length);   /* HO:168: (float)CurrentFrame / len (int → float) */
    float isInvincible = (s->has_hm && s->hm_IsInvincible) ? 1.0f : 0.0f;   /* HO:776 */
    float velX = 0.0f, velY = 0.0f;                               /* HO:780 */
    if (s->has_prev_rel) {                                        /* HO:783-784: prev.tick == MotionTick − 1 */
        velX = relX - s->prev_rel_x;                              /* HO:785 */
        velY = relY - s->prev_rel_y;                              /* HO:786 */
    }
    row->relX = relX; row->relY = relY; row->w = w; row->h = h; row->velX = velX; row->velY = velY;   /* HO:793-797 */
    row->isTrigger = isTrigger; row->givesDamage = givesDamage; row->takesDamage = takesDamage;
    row->isTarget = isTarget; row->isInvincible = isInvincible;
    row->hpRaw = hpRaw; row->hpMaxRaw = hpMaxRaw; row->animPhase = animPhase;
}

/* ---- global state (SE:30-96) -------------------------------------------------------------------------------------- */
void obs_global_state(const obs_hero_view *h, float g[OBS_GLOBAL_DIM])
{
    float commitLocked = 0.0f, commitReleasing = 0.0f, commitProgress = 0.0f;   /* SE:65 */
    float commitAction[OBS_COMMIT_ACTIONS] = { 0 };                           /* SE:66 */
    /* shim != null on every env call site (TE:337-338, 481-482, 750-751) → the SE:67-84 block always runs */
    commitLocked = h->shim_CState == OBS_COMMIT_LOCKED ? 1.0f : 0.0f;         /* SE:69 */
    commitReleasing = h->shim_CState == OBS_COMMIT_RELEASING ? 1.0f : 0.0f;   /* SE:70 */
    if (h->shim_LockedStepsTotal > 0) {                                       /* SE:71 */
        float left = h->shim_LockedStepsLeft > 0 ? (float)h->shim_LockedStepsLeft : 0.0f;   /* SE:73 */
        /* SE:74: float / int → conv.r4 then the division and the subtraction on the double F stack, rounded at the
         * store (Mono float model, port-obs.md §4). */
        commitProgress = (float)(1.0 - ((double)left / (double)(float)h->shim_LockedStepsTotal));
    } else if (commitReleasing > 0.0f) {                                      /* SE:76 */
        commitProgress = 1.0f;                                                /* SE:79 */
    }
    commitProgress = mathf_clamp01(commitProgress);                           /* SE:81 */
    if (h->shim_LockedAction >= 0 && h->shim_LockedAction < OBS_COMMIT_ACTIONS)   /* SE:82 */
        commitAction[h->shim_LockedAction] = 1.0f;                            /* SE:83 */

    g[0] = h->rb2d_velocity_x;                 /* SE:35 velX (SE:88 order) */
    g[1] = h->rb2d_velocity_y;                 /* SE:36 velY */
    g[2] = (float)h->pd_health;                /* SE:37 hp (int → float) */
    g[3] = (float)h->pd_MPCharge;              /* SE:38 soul */
    g[4] = h->knightW;                         /* SE:89 */
    g[5] = h->knightH;
    g[6] = h->pd_hasDash ? 1.0f : 0.0f;        /* SE:41 */
    g[7] = h->pd_canWallJump ? 1.0f : 0.0f;    /* SE:42 */
    g[8] = h->pd_hasDoubleJump ? 1.0f : 0.0f;  /* SE:43 */
    g[9] = h->pd_hasSuperDash ? 1.0f : 0.0f;   /* SE:44 */
    g[10] = h->pd_hasDreamNail ? 1.0f : 0.0f;  /* SE:45 */
    g[11] = h->pd_hasAcidArmour ? 1.0f : 0.0f; /* SE:46 */
    g[12] = h->pd_hasNailArt ? 1.0f : 0.0f;    /* SE:47 */
    g[13] = h->CanJump ? 1.0f : 0.0f;          /* SE:50 */
    g[14] = h->CanDoubleJump ? 1.0f : 0.0f;    /* SE:51 */
    g[15] = h->CanWallJump ? 1.0f : 0.0f;      /* SE:52 */
    g[16] = h->CanDash ? 1.0f : 0.0f;          /* SE:53 */
    g[17] = h->CanAttack ? 1.0f : 0.0f;        /* SE:54 */
    g[18] = h->CanCast ? 1.0f : 0.0f;          /* SE:55 */
    g[19] = h->CanNailCharge ? 1.0f : 0.0f;    /* SE:56 */
    g[20] = h->CanDreamNail ? 1.0f : 0.0f;     /* SE:57 */
    g[21] = h->CanSuperDash ? 1.0f : 0.0f;     /* SE:58 */
    g[22] = commitLocked;                      /* SE:93 */
    g[23] = commitReleasing;
    g[24] = commitProgress;
    for (int i = 0; i < OBS_COMMIT_ACTIONS; i++) g[25 + i] = commitAction[i];   /* SE:94-95 */
}

void obs_box_bounds_size(float px, float py, float ox, float oy, float w, float h, float *size_x, float *size_y)
{
    /* [ENGINE, measured — port-obs.md Q-pobs-5]: Box2D-style AABB of the shape's transformed vertices (local vertex =
     * offset ± half size, then + position, each a float32 op), size = upper − lower; no edgeRadius inflation. */
    float hx = w * 0.5f, hy = h * 0.5f;
    float lo_x = px + (ox - hx), hi_x = px + (ox + hx);
    float lo_y = py + (oy - hy), hi_y = py + (oy + hy);
    *size_x = hi_x - lo_x;
    *size_y = hi_y - lo_y;
}

float obs_damage_landed_accumulate(float acc, int32_t damageDealt, int32_t n_bosses, int32_t maxHP)
{
    /* TE:1183: `+= DamageDealt / (float)(n * maxHP) * 100f`, evaluated on the double F stack, rounded at the field store
     * (port-obs.md §4).  n = _bossHMs.Count, maxHP = _bossMaxHPs[hm] (TE:1447). */
    double q = (double)(float)damageDealt / (double)(float)(n_bosses * maxHP);
    return (float)((double)acc + q * 100.0);
}

/* ---- terrain segments (HO:416-518) --------------------------------------------------------------------------------- */
typedef void (*seg_row_cb)(void *ctx, uint32_t collider, uint32_t seg_idx, const float row[OBS_TERRAIN_FEAT]);

typedef struct {
    float kx, ky;              /* knightPos (HO:711) */
    float isTrigger;           /* HO:483 */
    uint32_t collider, seg_idx;/* HO:484 */
    seg_row_cb cb; void *ctx;
    uint32_t emitted;
} seg_ctx;

/* Transform.TransformPoint(new Vector3(x, y, 0)) → Vector2 [ENGINE].  Only the identity rotation/scale case is pinned:
 * world = local + transform.position, bit-exact against dumps/GG_Hornet_1/scene.json#colliders[*].world for all 54
 * terrain points (port-obs.md §2).  Scaled/rotated transforms diverge from the naive matrix by 1 ULP in the other
 * dumps (port-obs.md Q-pobs-1) → trap. */
static phys_v2 transform_point(const hk_static_collider *c, uint32_t i, float lx, float ly)
{
    /* scene.json#colliders[].world is Unity's own TransformPoint of these points, dumped at SceneReady
     * (oracle/Oracle/SceneDumper.cs:91-127).  Static colliders never move, so it is exact for any transform. */
    if (i < c->n_wpoints)
        return c->wpoints[i];
    if (c->rot_deg != 0.0f || c->sx != 1.0f || c->sy != 1.0f)
        HKSIM_UNIMPLEMENTED("Transform.TransformPoint on rotated/scaled terrain collider '%s' (rot %g, lossyScale %g,%g) "
                      "with no dumped world outline: float sequence unpinned (port-obs.md Q-pobs-1)",
                      c->path, (double)c->rot_deg, (double)c->sx, (double)c->sy);
    phys_v2 p = { lx + c->px, ly + c->py };
    return p;
}

/* HO:485-516.  Float model [ENGINE, measured — port-obs.md §4 / Q-pobs-4]: the mod's C# runs on Unity's Mono, which
 * evaluates each expression on the ECMA-335 "F" stack in DOUBLE and rounds to float32 only when the value is stored
 * to a float local / passed to a float parameter.  Per-op float32 reproduces 42/64 rows of the reset payload; this
 * double-intermediate model reproduces 64/64 on every payload of r2_move.a + r2_rand1.a (tests/test_obs.py).
 * Single-op expressions are written in float (fl32 == fl32(fl64) for + − × / — innocuous double rounding);
 * compound expressions are written in double with one (float) cast at the C# store. */
static void emit_segment(seg_ctx *s, phys_v2 aw, phys_v2 bw)
{
    float ax = aw.x - s->kx, ay = aw.y - s->ky;     /* HO:487 */
    float bx = bw.x - s->kx, by = bw.y - s->ky;     /* HO:488 */
    float mx = (float)(0.5 * ((double)ax + (double)bx));   /* HO:489 */
    float my = (float)(0.5 * ((double)ay + (double)by));   /* HO:490 */
    float hdx = (float)(0.5 * ((double)bx - (double)ax));  /* HO:491 */
    float hdy = (float)(0.5 * ((double)by - (double)ay));  /* HO:492 */
    if (hdx < 0.0f || (hdx == 0.0f && hdy < 0.0f)) { /* HO:496: canonical hdx ≥ 0, tie → hdy ≥ 0 */
        hdx = -hdx;                                  /* HO:498 (yields −0.0f when hdx == 0, kept on the wire) */
        hdy = -hdy;                                  /* HO:499 */
    }
    float dxs = bx - ax, dys = by - ay;              /* HO:503 (un-canonicalised direction) */
    float denom = (float)((double)dxs * (double)dxs + (double)dys * (double)dys);   /* HO:504 */
    float t = 0.0f;                                  /* HO:505 */
    if (denom > 1e-12f) {                            /* HO:506 */
        t = (float)((-(double)ax * (double)dxs + -(double)ay * (double)dys) / (double)denom);   /* HO:508: double numerator and division */
        if (t < 0.0f) t = 0.0f;                      /* HO:509 */
        else if (t > 1.0f) t = 1.0f;                 /* HO:510 */
    }
    float npx = (float)((double)ax + (double)t * (double)dxs);   /* HO:512 */
    float npy = (float)((double)ay + (double)t * (double)dys);   /* HO:513 */
    float ss = (float)((double)npx * (double)npx + (double)npy * (double)npy);   /* HO:514 argument of Mathf.Sqrt(float): rounded at the call */
    float dist = mathf_sqrt(ss);                     /* HO:514 */
    float row[OBS_TERRAIN_FEAT] = { mx, my, hdx, hdy, npx, npy, dist, s->isTrigger };   /* HO:516 */
    s->cb(s->ctx, s->collider, s->seg_idx, row);     /* HO:517 debug "|seg_idx=" + segIdx */
    s->seg_idx++;                                    /* HO:518 */
    s->emitted++;
}

/* Segment decomposition per shape (HO:421-481).  Points are transformed in the C# order: local + offset first (float
 * add), then TransformPoint. */
static void emit_collider_segments(seg_ctx *s, const hk_static_collider *c)
{
    switch (c->shape) {
    case PHYS_SHAPE_EDGE: {                                          /* HO:424-433 */
        for (uint32_t i = 0; i + 1 < c->n_points; i++) {
            phys_v2 p0 = transform_point(c, i, c->points[i].x + c->ox, c->points[i].y + c->oy);          /* HO:429 */
            phys_v2 p1 = transform_point(c, i + 1, c->points[i + 1].x + c->ox, c->points[i + 1].y + c->oy);  /* HO:430 */
            emit_segment(s, p0, p1);
        }
        break;
    }
    case PHYS_SHAPE_POLYGON: {                                       /* HO:434-449: every path, closed; only path 0 is compiled */
        if (c->unsupported == 2)
            HKSIM_UNIMPLEMENTED("multi-path PolygonCollider2D terrain '%s': scene table carries path 0 only (port-obs.md Q-pobs-3)", c->path);
        uint32_t n = c->n_points;
        if (n == 0) break;                                           /* HO:438 empty path skipped */
        for (uint32_t i = 0; i < n; i++) {                           /* HO:439 */
            phys_v2 p0 = c->points[i];                               /* HO:441 */
            phys_v2 p1 = c->points[(i + 1) % n];                     /* HO:442 */
            phys_v2 a = transform_point(c, i, p0.x + c->ox, p0.y + c->oy);   /* HO:443 */
            phys_v2 b = transform_point(c, (i + 1) % n, p1.x + c->ox, p1.y + c->oy);   /* HO:444 */
            emit_segment(s, a, b);
        }
        break;
    }
    case PHYS_SHAPE_BOX: {                                           /* HO:450-462 */
        float half_x = c->w * 0.5f, half_y = c->h * 0.5f;            /* HO:452 half = size * 0.5f */
        float o_x = c->ox, o_y = c->oy;                              /* HO:453 */
        phys_v2 bl = transform_point(c, 0, o_x - half_x, o_y - half_y); /* HO:454 */
        phys_v2 br = transform_point(c, 1, o_x + half_x, o_y - half_y); /* HO:455 */
        phys_v2 tl = transform_point(c, 3, o_x - half_x, o_y + half_y); /* HO:456 */
        phys_v2 trc = transform_point(c, 2, o_x + half_x, o_y + half_y);/* HO:457 */
        emit_segment(s, bl, br);                                     /* HO:458 bottom */
        emit_segment(s, br, trc);                                    /* HO:459 right */
        emit_segment(s, trc, tl);                                    /* HO:460 top */
        emit_segment(s, tl, bl);                                     /* HO:461 left */
        break;
    }
    case PHYS_SHAPE_CIRCLE: {                                        /* HO:463-480: 12-gon */
        /* No layer-8 non-trigger CircleCollider2D exists in the four dumps; Mathf.Cos/Sin = (float)Math.Cos/Sin(double)
         * [ENGINE] and the C runtime's libm parity with Mono's is unverified → trap until Q-pobs-2 has evidence. */
        HKSIM_UNIMPLEMENTED("CircleCollider2D terrain '%s': Mathf.Cos/Sin libm parity unverified (port-obs.md Q-pobs-2)", c->path);
        const int N = 12;                                            /* HO:468 */
        float r = c->radius;                                         /* HO:469 */
        phys_v2 prev = { 0.0f, 0.0f };                               /* HO:470 */
        for (int i = 0; i <= N; i++) {                               /* HO:471 */
            float ang = 2.0f * 3.14159274f * (float)i / (float)N;    /* HO:473: 2f * Mathf.PI * i / N, left to right in float */
            phys_v2 p = transform_point(c, 0xFFFFFFFFu, c->ox + (float)cos((double)ang) * r,    /* HO:474-476 */
                                           c->oy + (float)sin((double)ang) * r);
            if (i > 0) emit_segment(s, prev, p);                     /* HO:477 */
            prev = p;                                                /* HO:478 */
        }
        break;
    }
    default:
        break;                                                       /* HO:481: other shapes emit nothing */
    }
}

/* Terrain bucket walk (HO:740-742, 803-822) over the compiled statics and the dynamic-body terrain (hk_scene_def.
 * terrain_dyn), merged in scene order (obs-wire.md §3.8, §4.3: the bucket is a HashSet filled in
 * FindObjectsOfTypeAll order).  Returns the emitted row count. */
static uint32_t terrain_walk(const hk_scene_def *scene, const uint8_t *active_override, const obs_terrain_dyn_live *dyn,
                             float kx, float ky, seg_row_cb cb, void *ctx)
{
    seg_ctx s = { kx, ky, 0.0f, 0, 0, cb, ctx, 0 };
    uint32_t last_order = 0, nd = scene->terrain_dyn ? scene->n_terrain_dyn : 0;
    for (uint32_t i = 0, j = 0; i < scene->n_statics || j < nd; ) {
        const int from_dyn = j < nd && (i >= scene->n_statics || scene->terrain_dyn[j].scene_order < scene->statics[i].scene_order);
        const hk_static_collider *c = from_dyn ? &scene->terrain_dyn[j] : &scene->statics[i];
        const uint32_t idx = from_dyn ? scene->n_statics + j : i;
        HKSIM_ASSERT(idx == 0 || c->scene_order > last_order || (i + j) == 0, "terrain not in scene order at %u", idx);   /* Q-obs-1: order = scene.json index */
        last_order = c->scene_order;
        if (from_dyn) j++; else i++;
        if (c->unsupported == 1) continue;                           /* HitboxObserver.Classify: only Box/Polygon/Edge/Circle */
        /* bucket Terrain (HitboxObserver.Classify): layer 8 (PhysLayers.TERRAIN) && !isTrigger.  The Enemy rule takes
         * precedence -- no layer-8 static in the dumps carries DamageHero/damages_hero (port-obs.md §2); for the ones
         * the FSM world holds, the live arrays apply it. */
        if (c->layer != 8u || c->is_trigger) continue;
        /* In the Terrain bucket now (HitboxObserver.Classify): the live arrays from sim/fsm carry it. */
        hk_static_collider live;
        uint8_t active;
        if (from_dyn && dyn) {
            active = dyn->active[j - 1];
            if (active) {                                            /* the body has moved since SceneReady: its live outline */
                live = *c;
                live.wpoints = dyn->wpts + dyn->off[j - 1]; live.n_wpoints = dyn->n[j - 1];
                c = &live;
            }
        } else if (!from_dyn && active_override) {
            active = active_override[i - 1];
        } else {
            active = c->active;
        }
        if (!active) continue;
        if (c->used_by_composite) continue;                          /* HO:808-810 */
        s.isTrigger = c->is_trigger ? 1.0f : 0.0f;                   /* HO:483 (always 0 here, obs-wire.md §4.1) */
        s.collider = idx;
        s.seg_idx = 0;                                               /* HO:484 */
        emit_collider_segments(&s, c);
    }
    return s.emitted;
}

typedef struct { obs_terrain_row *out; uint32_t cap, n; } rows_ctx;
static void rows_cb(void *ctx, uint32_t collider, uint32_t seg_idx, const float row[OBS_TERRAIN_FEAT])
{
    rows_ctx *r = ctx;
    if (r->n < r->cap) {
        obs_terrain_row *o = &r->out[r->n];
        o->mx = row[0]; o->my = row[1]; o->hdx = row[2]; o->hdy = row[3];
        o->npx = row[4]; o->npy = row[5]; o->dist = row[6]; o->isTrigger = row[7];
        o->collider = collider; o->seg_idx = seg_idx;
    }
    r->n++;
}

uint32_t obs_terrain_rows(const hk_scene_def *scene, const uint8_t *active_override,
                          float knightPos_x, float knightPos_y, obs_terrain_row *out, uint32_t cap)
{
    rows_ctx r = { out, out ? cap : 0, 0 };
    if (!scene) return 0;
    return terrain_walk(scene, active_override, NULL, knightPos_x, knightPos_y, rows_cb, &r);
}

uint32_t obs_terrain_rows_live(const hk_scene_def *scene, const uint8_t *active_override, const obs_terrain_dyn_live *dyn,
                               float knightPos_x, float knightPos_y, obs_terrain_row *out, uint32_t cap)
{
    rows_ctx r = { out, out ? cap : 0, 0 };
    if (!scene) return 0;
    return terrain_walk(scene, active_override, dyn, knightPos_x, knightPos_y, rows_cb, &r);
}

/* ---- Pack (BP:32-176) --------------------------------------------------------------------------------------------- */
static void wire_row_cb(void *ctx, uint32_t collider, uint32_t seg_idx, const float row[OBS_TERRAIN_FEAT])
{
    (void)collider; (void)seg_idx;
    wr *w = ctx;
    for (int i = 0; i < OBS_TERRAIN_FEAT; i++) put_f32(w, row[i]);   /* BP:55-57 */
}
static void debug_row_cb(void *ctx, uint32_t collider, uint32_t seg_idx, const float row[OBS_TERRAIN_FEAT])
{
    (void)collider; (void)row;
    wr *w = ctx;
    char s[32];
    snprintf(s, sizeof s, "|seg_idx=%u", seg_idx);   /* HO:517 baseDebug + "|seg_idx=" + segIdx, baseDebug = "" (HO:819-821, eval=false) */
    put_str16(w, s, "");                              /* BP:98-106 */
}

static size_t pack_body(const obs_view *v, uint8_t msg, uint8_t *buf, size_t cap)
{
    wr w = { buf, cap, 0 };
    const int is_step = (msg == OBS_MSG_STEP);
    /* Done step (TE:735-746): empty combat/terrain lists, 33 zero globals, empty fsm block; scalars, diag and info real. */
    const int done = is_step && v->step.done;
    if (v->eval_mode)
        HKSIM_UNIMPLEMENTED("eval=true terrain_debug (BuildTerrainDebug HO:522-664 = live Physics2D queries) is not reproducible; run parity with eval=false (obs-wire.md §4.5, §5)");

    const uint32_t nc = done ? 0u : v->n_combat;
    HKSIM_ASSERT(nc <= 65535u, "n_combat %u exceeds the u16 count (BP:48)", nc);
    put_u8(&w, msg);                                     /* BP:37 TypeToId[type] */
    put_u16(&w, (uint16_t)nc);                           /* BP:48 (ushort)combat.Count */
    size_t nt_at = w.pos;
    put_u16(&w, 0);                                      /* BP:49 (ushort)terrain.Count — patched below */

    for (uint32_t i = 0; i < nc; i++) {                  /* BP:51-53: rows, each float[14] (HO:793-797) */
        const obs_combat_row *r = &v->combat[i];
        put_f32(&w, r->relX); put_f32(&w, r->relY); put_f32(&w, r->w); put_f32(&w, r->h);
        put_f32(&w, r->velX); put_f32(&w, r->velY);
        put_f32(&w, r->isTrigger); put_f32(&w, r->givesDamage); put_f32(&w, r->takesDamage);
        put_f32(&w, r->isTarget); put_f32(&w, r->isInvincible);
        put_f32(&w, r->hpRaw); put_f32(&w, r->hpMaxRaw); put_f32(&w, r->animPhase);
    }

    uint32_t nt = 0;
    if (!done && v->scene)                               /* BP:55-57: terrain rows in HO:516 order */
        nt = terrain_walk(v->scene, v->terrain_active, v->terrain_dyn, v->hero.knightPos_x, v->hero.knightPos_y, wire_row_cb, &w);
    HKSIM_ASSERT(nt <= 65535u, "n_terrain %u exceeds the u16 count (BP:49)", nt);
    patch_u16(&w, nt_at, (uint16_t)nt);

    {                                                    /* BP:59-60: 33 globals (SE:86-96), zeros on done (TE:741) */
        float g[OBS_GLOBAL_DIM] = { 0 };
        if (!done) obs_global_state(&v->hero, g);
        for (int i = 0; i < OBS_GLOBAL_DIM; i++) put_f32(&w, g[i]);
    }

    if (is_step) {                                       /* BP:62-70 */
        put_f32(&w, v->step.damage_landed);              /* BP:64 */
        put_f32(&w, (float)v->step.hits_taken);          /* BP:65 (float)(int) */
        put_f32(&w, v->step.step_game_time);             /* BP:66 */
        put_f32(&w, v->step.step_real_time);             /* BP:67 */
        put_f32(&w, v->step.hp_healed);                  /* BP:68 */
        put_u8(&w, v->step.done ? 1 : 0);                /* BP:69 */
        put_u8(&w, v->step.action_committed ? 1 : 0);    /* BP:70 */
    }

    for (uint32_t i = 0; i < nc; i++) put_str8(&w, v->combat[i].kind, "unknown");   /* BP:75-82 */
    for (uint32_t i = 0; i < nc; i++) put_str8(&w, v->combat[i].clipKey, "");       /* BP:86-93 */

    if (nt)                                              /* BP:98-106: one u16 string per terrain row */
        terrain_walk(v->scene, v->terrain_active, v->terrain_dyn, v->hero.knightPos_x, v->hero.knightPos_y, debug_row_cb, &w);

    if (is_step) {                                       /* BP:113-120: diag HHHif */
        put_u16(&w, v->step.diag_enemy_count);
        put_u16(&w, v->step.diag_attack_count);
        put_u16(&w, v->step.diag_terrain_count);
        put_i32(&w, v->step.diag_kind_cache_size);
        put_f32(&w, v->step.diag_gc_heap_mb);
    }

    if (msg == OBS_MSG_RESET) {                          /* BP:128-138: u8 branch + 7 × (f32 ms, u16 frames) */
        put_u8(&w, v->reset.reset_branch);
        for (int i = 0; i < OBS_RESET_PHASES; i++) {
            put_f32(&w, v->reset.reset_phase_ms[i]);
            put_u16(&w, v->reset.reset_phase_frames[i]);
        }
    }

    {                                                    /* BP:147-157: fsm block, count capped 65535; empty on done (TE:742) */
        uint32_t n = done ? 0u : v->n_fsm;
        if (n > 65535u) n = 65535u;                      /* BP:148 */
        put_u16(&w, (uint16_t)n);
        for (uint32_t i = 0; i < n; i++) put_str16(&w, v->fsm_snapshots[i], "");   /* BP:150-155 */
    }

    if (is_step) put_str8(&w, v->step.info, "");         /* BP:165-172: info, cap 255 */

    return w.pos;
}

size_t obs_pack_step(const obs_view *v, uint8_t *buf, size_t cap)  { return pack_body(v, OBS_MSG_STEP, buf, cap); }
size_t obs_pack_reset(const obs_view *v, uint8_t *buf, size_t cap) { return pack_body(v, OBS_MSG_RESET, buf, cap); }

size_t obs_pack_ack(uint8_t msg_id, uint8_t *buf, size_t cap)
{
    /* BP:37 writes the id; BP:40 adds a body only for step/reset → init (0) / pause (4) / resume (5) replies are one byte
     * (obs-wire.md §1.3).  close has no reply (TE:141-143); action is never echoed. */
    HKSIM_ASSERT(msg_id == OBS_MSG_INIT || msg_id == OBS_MSG_PAUSE || msg_id == OBS_MSG_RESUME,
                 "obs_pack_ack: id %u is not init/pause/resume", msg_id);
    wr w = { buf, cap, 0 };
    put_u8(&w, msg_id);
    return w.pos;
}

/* ---- Unpack (BP:179-205) ------------------------------------------------------------------------------------------ */
typedef struct { const uint8_t *p; size_t len, pos; int ok; } rd;
static uint8_t  rd_u8(rd *r)  { if (r->pos + 1 > r->len) { r->ok = 0; return 0; } return r->p[r->pos++]; }
static uint16_t rd_u16(rd *r) { if (r->pos + 2 > r->len) { r->ok = 0; return 0; } uint16_t v = (uint16_t)(r->p[r->pos] | (r->p[r->pos + 1] << 8)); r->pos += 2; return v; }
static int32_t  rd_i32(rd *r)
{
    if (r->pos + 4 > r->len) { r->ok = 0; return 0; }
    uint32_t v = (uint32_t)r->p[r->pos] | ((uint32_t)r->p[r->pos + 1] << 8) | ((uint32_t)r->p[r->pos + 2] << 16) | ((uint32_t)r->p[r->pos + 3] << 24);
    r->pos += 4;
    return (int32_t)v;
}

int obs_unpack_request(const uint8_t *data, size_t len, obs_request *out)
{
    if (!data || !out) return HKSIM_ERR_BAD_ARG;
    rd r = { data, len, 0, 1 };
    memset(out, 0, sizeof *out);
    uint8_t id = rd_u8(&r);                              /* BP:185 */
    if (!r.ok || id > OBS_MSG_CLOSE) return HKSIM_ERR_BAD_ARG;   /* IdToType[typeId] throws on an unknown id (BP:186) */
    out->type = id;
    switch (id) {
    case OBS_MSG_RESET:                                  /* BP:189-196 */
        out->frames_per_wait = rd_i32(&r);
        out->time_scale = rd_i32(&r);
        out->eval = rd_u8(&r) != 0;
        out->force_full = rd_u8(&r) != 0;
        {
            uint16_t n = rd_u16(&r);
            if (!r.ok || r.pos + n > len) return HKSIM_ERR_BAD_ARG;
            if (n >= sizeof out->level) return HKSIM_ERR_BAD_ARG;   /* sim limit, not a wire rule */
            memcpy(out->level, data + r.pos, n);
            out->level[n] = 0;
            r.pos += n;
        }
        break;
    case OBS_MSG_ACTION:                                 /* BP:197-201 */
        for (int i = 0; i < 4; i++) out->action_vec[i] = rd_i32(&r);
        break;
    default:                                             /* init / pause / resume / close / step: id only */
        break;
    }
    return r.ok ? HKSIM_OK : HKSIM_ERR_BAD_ARG;          /* BinaryReader EndOfStreamException ⇒ bad request */
}

void obs_layout(uint32_t out[8])
{
    out[0] = (uint32_t)sizeof(obs_hero_view);
    out[1] = (uint32_t)sizeof(obs_combat_row);
    out[2] = (uint32_t)sizeof(obs_combat_src);
    out[3] = (uint32_t)sizeof(obs_step_view);
    out[4] = (uint32_t)sizeof(obs_reset_view);
    out[5] = (uint32_t)sizeof(obs_view);
    out[6] = (uint32_t)sizeof(obs_terrain_row);
    out[7] = (uint32_t)sizeof(obs_request);
}

_Static_assert(sizeof(float) == 4, "float must be IEEE binary32");
_Static_assert(offsetof(obs_combat_row, kind) == OBS_COMBAT_FEAT * sizeof(float), "obs_combat_row: 14 contiguous floats");
