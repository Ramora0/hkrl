#pragma once
/* 2D physics interface.  Implemented by sim/phys, consumed by sim/hero and sim/fsm.  Models the subset of Unity Physics2D
 * that the dumped scenes use (dumps/<scene>/physics.json #Physics2D / #layerCollisionMatrix, scene.json #colliders[*]):
 * a translation of Unity's Box2D fork and its PhysicsContacts2D callback machinery (analysis/native_specs/native-box2d.md,
 * native-physics2d.md; Box2D 2.3.1 where the fork is unchanged) plus measured deltas (analysis/specs/port-phys.md).
 *
 * Units: Unity world units, float32.  Angles in degrees (transform.eulerAngles.z) and angular velocities in
 * degrees/s, as Rigidbody2D exposes them; sim/phys keeps Box2D's radians inside.  A free-rotation dynamic body
 * turns under contacts and torques (Box2D's rotational dynamics); a FreezeRotation body turns only by rotation
 * writes.
 * Determinism: every order is the fork's (its contact arrays and lists, proxy ids, the m_Collisions array) or an
 * instance id, never pointer/hash order.
 */
#include <stdint.h>
#include <stdbool.h>
#include "hksim.h"   /* HKSIM_API: exported so tests can drive sim/phys via ctypes */

typedef uint32_t phys_body_id;     /* 0 = none */
typedef uint32_t phys_shape_id;    /* 0 = none */

typedef enum { PHYS_BODY_STATIC = 0, PHYS_BODY_KINEMATIC = 1, PHYS_BODY_DYNAMIC = 2 } phys_body_type;
typedef enum { PHYS_CD_DISCRETE = 0, PHYS_CD_CONTINUOUS = 1 } phys_cd_mode;
typedef enum { PHYS_SHAPE_BOX = 0, PHYS_SHAPE_CIRCLE = 1, PHYS_SHAPE_POLYGON = 2, PHYS_SHAPE_EDGE = 3 } phys_shape_type;

typedef struct { float x, y; } phys_v2;

typedef struct {
    phys_body_type type;
    phys_cd_mode   cd;
    phys_v2        position;        /* Rigidbody2D.position (transform for static) */
    float          rotation_deg;    /* initial; from scene.json transform.eulerZ (phys_body_set_rotation writes it) */
    phys_v2        scale;           /* transform.lossyScale (x may be negative = facing flip) */
    phys_v2        velocity;
    float          gravity_scale;
    float          mass;
    bool           simulated;
    uint32_t       layer;           /* GameObject.layer, 0..31 */
    uint32_t       user;            /* owner handle for callbacks (entity/hero id), opaque to phys */
    bool           free_rotation;   /* Rigidbody2D.constraints without FreezeRotation */
} phys_body_desc;

typedef struct {
    phys_shape_type type;
    bool     is_trigger;
    bool     enabled;
    phys_v2  offset;                /* Collider2D.offset (local) */
    phys_v2  size;                  /* box: size; capsule not supported (traps) */
    float    radius;                /* circle */
    float    edge_radius;           /* box/edge */
    const phys_v2 *points;          /* polygon path / edge points (local, offset NOT applied) */
    uint32_t n_points;
    uint32_t user;                  /* owner collider handle (for obs / damage routing) */
    uint32_t layer;                 /* GameObject.layer of the collider's own object; PHYS_LAYER_INHERIT = the body's
                                       (child colliders such as HeroBox (20) on the Knight body (9), Hornet's detectors
                                       (13/14/22) on her body (11) — port-phys.md interface request 1) */
    int32_t  instance_id;           /* the Collider2D's GetInstanceID(): the lower one of a pair receives first */
} phys_shape_desc;
#define PHYS_LAYER_INHERIT 0xFFFFFFFFu

typedef enum { PHYS_EV_COLLISION_ENTER, PHYS_EV_COLLISION_STAY, PHYS_EV_COLLISION_EXIT,
               PHYS_EV_TRIGGER_ENTER,   PHYS_EV_TRIGGER_STAY,   PHYS_EV_TRIGGER_EXIT } phys_event_kind;

typedef struct {
    phys_event_kind kind;
    phys_body_id  body_a, body_b;   /* a = the body receiving the callback (Unity delivers to both) */
    phys_shape_id shape_a, shape_b;
    phys_v2       normal;           /* contact normal from b to a (collision events only) */
    phys_v2       point;            /* first contact point (collision events only) */
    uint32_t      contact_count;
} phys_event;

typedef struct phys_world phys_world;

HKSIM_API phys_world *phys_create(phys_v2 gravity, uint32_t velocity_iters, uint32_t position_iters,
                          const uint32_t layer_matrix[32] /* bit j of [i]: layers i,j collide */);
HKSIM_API void          phys_destroy(phys_world *w);

HKSIM_API phys_body_id  phys_body_add(phys_world *w, const phys_body_desc *d);
HKSIM_API phys_shape_id phys_shape_add(phys_world *w, phys_body_id b, const phys_shape_desc *d);
HKSIM_API void          phys_shape_set_enabled(phys_world *w, phys_shape_id s, bool enabled);
HKSIM_API void          phys_shape_set_box(phys_world *w, phys_shape_id s, phys_v2 offset, phys_v2 size);   /* SetBoxCollider2DSize* */
/* Geometry, trigger flag and body type changes re-create the collider's fixtures and contacts while its collider
 * pairs keep their records (Collider2D::RecreateCollider, native-physics2d.md §2): no Exit or Enter. */
HKSIM_API void          phys_shape_set_points(phys_world *w, phys_shape_id s, const phys_v2 *pts, uint32_t n);   /* re-bake after a transform change (a BOX becomes a POLYGON) */
HKSIM_API void          phys_shape_set_circle(phys_world *w, phys_shape_id s, phys_v2 offset, float radius);    /* circle moved/rescaled relative to its body */
HKSIM_API void          phys_shape_set_trigger(phys_world *w, phys_shape_id s, bool is_trigger);
HKSIM_API void          phys_shape_set_layer(phys_world *w, phys_shape_id s, uint32_t layer);   /* PHYS_LAYER_INHERIT allowed */
HKSIM_API uint32_t      phys_shape_layer(const phys_world *w, phys_shape_id s);                 /* effective layer */

/* Body state (Rigidbody2D / transform writes the ported code performs). */
HKSIM_API phys_v2       phys_body_position(const phys_world *w, phys_body_id b);
HKSIM_API phys_v2       phys_body_velocity(const phys_world *w, phys_body_id b);
HKSIM_API void          phys_body_set_position(phys_world *w, phys_body_id b, phys_v2 p);   /* transform.position / rb.position write */
HKSIM_API void          phys_body_set_velocity(phys_world *w, phys_body_id b, phys_v2 v);
HKSIM_API void          phys_body_add_force(phys_world *w, phys_body_id b, phys_v2 f);   /* b2Body::ApplyForceToCenter */
HKSIM_API void          phys_body_set_gravity_scale(phys_world *w, phys_body_id b, float g);
HKSIM_API void          phys_body_set_scale_x(phys_world *w, phys_body_id b, float sx);      /* facing flip */
HKSIM_API void          phys_body_set_scale(phys_world *w, phys_body_id b, phys_v2 s);       /* lossyScale of the body's GameObject, both axes */
HKSIM_API void          phys_body_set_rotation(phys_world *w, phys_body_id b, float deg);    /* transform.rotation write */
HKSIM_API void          phys_body_set_simulated(phys_world *w, phys_body_id b, bool on);     /* SetActive / enabled */
HKSIM_API void          phys_body_set_type(phys_world *w, phys_body_id b, phys_body_type t);
HKSIM_API void          phys_body_set_layer(phys_world *w, phys_body_id b, uint32_t layer);   /* gameObject.layer = (HeroController writes it) */
HKSIM_API float         phys_body_gravity_scale(const phys_world *w, phys_body_id b);
/* Rotational state (Rigidbody2D.drag / angularDrag, AddTorque, angularVelocity, inertia). */
HKSIM_API void          phys_body_set_damping(phys_world *w, phys_body_id b, float drag, float angular_drag);
HKSIM_API void          phys_body_add_torque(phys_world *w, phys_body_id b, float torque, bool impulse);   /* ForceMode2D Force / Impulse */
HKSIM_API void          phys_body_set_angular_velocity(phys_world *w, phys_body_id b, float deg_per_s);
HKSIM_API float         phys_body_angular_velocity(const phys_world *w, phys_body_id b);   /* degrees/s */
HKSIM_API float         phys_body_inertia(const phys_world *w, phys_body_id b);
HKSIM_API float         phys_body_angle(const phys_world *w, phys_body_id b);              /* b2Body::GetAngle, radians */
/* The z euler the last phys_step handed to a body's Transform (PhysicsManager2D's write-back), once: true when
 * the step turned the body. */
HKSIM_API bool          phys_body_take_rotation(phys_world *w, phys_body_id b, float *deg);
/* Whether the last phys_step wrote this body's position back to its Transform (a simulated, awake, non-static body;
 * PhysicsManager2D::Simulate UP!0x180beb390, native-physics2d.md §3.2), once. */
HKSIM_API bool          phys_body_take_writeback(phys_world *w, phys_body_id b);
/* The Transform's z euler after that write-back, recorded without SetTransform: the body keeps its angle. */
HKSIM_API void          phys_body_note_rotation(phys_world *w, phys_body_id b, float deg);
HKSIM_API float         phys_body_scale_x(const phys_world *w, phys_body_id b);

/* One FixedUpdate physics step (dt = 0.02).  Its callbacks are listed in the order Unity delivers them
 * (PhysicsContacts2D::ProcessContacts, native-physics2d.md §6.3); the caller dispatches them afterwards in order. */
HKSIM_API void          phys_step(phys_world *w, float dt);
HKSIM_API uint32_t      phys_events(const phys_world *w, const phys_event **out);              /* count; valid until next step */
/* The callbacks a collider disable delivers inside the call (Collider2D::Cleanup(kColliderDisable) ->
 * ProcessContacts(collider), UP!0x180c00240; docs/engine-lifecycle.md R5): the caller drains them right after
 * phys_shape_set_enabled(false), in order, up to `cap` per call.  phys_step traps if any are left undelivered. */
HKSIM_API uint32_t      phys_take_exit_events(phys_world *w, phys_event *out, uint32_t cap);

/* Queries the ported code uses. */
HKSIM_API bool          phys_raycast(const phys_world *w, phys_v2 origin, phys_v2 dir, float length, uint32_t layer_mask,
                           phys_v2 *hit_point, phys_v2 *hit_normal, phys_shape_id *hit_shape);
HKSIM_API bool          phys_overlap_any(const phys_world *w, phys_shape_id s, uint32_t layer_mask);
/* Physics2D.OverlapBox(point, size, angle, mask) / OverlapCircle(point, radius, mask): first overlapping shape, 0 = none */
HKSIM_API phys_shape_id phys_overlap_box(const phys_world *w, phys_v2 center, phys_v2 size, float angle_deg, uint32_t layer_mask);
HKSIM_API phys_shape_id phys_overlap_circle(const phys_world *w, phys_v2 center, float radius, uint32_t layer_mask);
/* Physics2D.BoxCast(origin, size, angle 0, dir, distance, mask): sweep an axis-aligned box; first hit only.
 * Semantics (which shapes count, trigger handling per queriesHitTriggers, start-inside per
 * queriesStartInColliders) are pinned by experiment and cited in sim/phys. */
HKSIM_API bool          phys_boxcast(const phys_world *w, phys_v2 origin, phys_v2 size, phys_v2 dir, float length,
                                     uint32_t layer_mask, phys_v2 *hit_point, phys_v2 *hit_normal, phys_shape_id *hit_shape);
/* Owner handle recorded in phys_shape_desc.user / phys_body_desc.user, and the shape's trigger flag + layer. */
HKSIM_API uint32_t      phys_shape_user(const phys_world *w, phys_shape_id s);
HKSIM_API uint32_t      phys_body_user(const phys_world *w, phys_body_id b);
HKSIM_API uint32_t      phys_body_layer(const phys_world *w, phys_body_id b);
HKSIM_API bool          phys_shape_is_trigger(const phys_world *w, phys_shape_id s);
/* Introspection of the baked b2PolygonShape pieces (tests/test_phys_tess.py: known-answer checks on the
 * libtess2 decomposition, Q-pphys-5).  Vertices are shape-local (post offset/scale, pre body transform),
 * in b2PolygonShape::Set's own hull order (bottom-right first). */
HKSIM_API uint32_t      phys_shape_piece_count(const phys_world *w, phys_shape_id s);
HKSIM_API uint32_t      phys_shape_piece_vertices(const phys_world *w, phys_shape_id s, uint32_t piece, phys_v2 *out, uint32_t cap);
