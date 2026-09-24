#pragma once
/* A compiled scene: its static colliders, hero colliders and Physics2D settings, generated per scene by
 * sim/core/gen_scene.py into sim/generated/<scene>/scene.c. */
#include <stdint.h>
#include "phys.h"
#include "hksim.h"

typedef struct {
    const char *path;        /* scene.json#colliders[].path */
    int32_t  instance_id;    /* #instanceID */
    uint32_t scene_order;    /* index in scene.json (FindObjectsOfTypeAll order; HitboxObserver row order, obs-wire.md) */
    uint32_t layer;
    uint8_t  is_trigger, active, unsupported /* 0 ok, 1 shape type unsupported, 2 multi-path polygon */, used_by_composite;
    const char *tag;         /* GameObject.tag (e.g. "HeroWalkable") */
    uint32_t marker_flags;   /* 1 NoHardLanding, 2 SteepSlope, 4 NonSlider (or Roof), 8 NonThunker, 16 Roof, 32 KillOnContact
                              * components on the object (gen_scene.py) */
    phys_shape_type shape;
    float px, py, rot_deg, sx, sy;           /* transform.position / eulerAngles.z / lossyScale */
    float ox, oy, w, h, radius, edge_radius; /* Collider2D.offset, BoxCollider2D.size, CircleCollider2D.radius, edgeRadius */
    const phys_v2 *points; uint32_t n_points;  /* PolygonCollider2D path 0 / EdgeCollider2D points (local) */
    /* scene.json#colliders[].world: Unity's Transform.TransformPoint of the SAME points, dumped at
     * SceneReady.  Statics do not move, so these are exact world outlines and remove the need to
     * reproduce TransformPoint's float sequence under scale/rotation (port-obs.md Q-pobs-1).
     * Order (oracle/Oracle/SceneDumper.cs:91-127): box bl, br, tr, tl; edge/polygon in point order. */
    const phys_v2 *wpoints; uint32_t n_wpoints;
} hk_static_collider;

typedef struct {
    const char *path; const char *type; uint8_t is_trigger, enabled; uint32_t layer;
    float ox, oy, w, h, edge_radius;
    int32_t instance_id;     /* scene.json#colliders[] with the same path and type: #instanceID */
} hk_hero_collider;

typedef struct {
    const char *name;
    phys_v2  gravity;                 /* physics.json#Physics2D.gravity */
    uint32_t velocity_iters, position_iters;
    float    default_contact_offset, baumgarte, baumgarte_toi, velocity_threshold, max_linear_correction;
    uint8_t  queries_hit_triggers, queries_start_in_colliders;
    const char *const *layer_names;   /* 32 */
    const uint32_t *layer_mask;       /* 32; bit j of [i] = layers i and j collide */
    const hk_static_collider *statics; uint32_t n_statics;
    const hk_hero_collider *hero_cols; uint32_t n_hero_cols;
    float hero_gravity_scale, hero_mass; uint8_t hero_freeze_rotation; const char *hero_cd_mode;
    /* hero pose at SceneReady: physics.json#rb2d.position/velocity, hero.json#transform.localScale.x
     * (the dump was taken at SceneReady under R2; r2 traces' first FRAME carries the identical rb pose) */
    float hero_pos_x, hero_pos_y, hero_vel_x, hero_vel_y, hero_scale_x;
    /* Terrain-bucket colliders on Dynamic/Kinematic bodies (layer 8, non-trigger): observer-only, not physics.
     * px/py/wpoints are the SceneReady pose; the live outline comes from sim/fsm (obs_terrain_dyn_live). */
    const hk_static_collider *terrain_dyn; uint32_t n_terrain_dyn;
} hk_scene_def;
