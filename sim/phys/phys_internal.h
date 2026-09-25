#pragma once
/* sim/phys internals: Unity 2020.2.2f1's Box2D fork and its Physics2D glue.  Authorities, in order: the native
 * decompile (analysis/decomp_native, cited UP!<addr> <symbol>; its rulings in analysis/native_specs/native-box2d.md
 * and native-physics2d.md), Box2D 2.3.1 where the fork is unchanged (analysis/upstream/box2d-v2.3.1), and the
 * measured deltas of analysis/specs/port-phys.md.  IEEE binary32; sim/CMakeLists.txt forbids contraction.
 */
#include <stdint.h>
#include <stdbool.h>
#include <float.h>
#include <math.h>
#include "../core/phys.h"
#include "../core/trap.h"

/* ---- solver constants ---------------------------------------------------------------------- */
#define PH_LINEAR_SLOP        0.005f  /* cite: analysis/specs/port-phys.md#E2 (resting gap = rA+rB-slop) */
#define PH_POLYGON_RADIUS     0.01f   /* cite: dumps/GG_Hornet_1/physics.json#Physics2D.defaultContactOffset ; E2 */
#define PH_ANGULAR_SLOP       (2.0f / 180.0f * 3.14159265359f)  /* cite: box2d-v2.3.1 Common/b2Settings.h:72 (b2_angularSlop) */
#define PH_BAUMGARTE          0.2f    /* cite: dumps/GG_Hornet_1/physics.json#Physics2D.baumgarteScale ; E3 */
#define PH_TOI_BAUMGARTE      0.75f   /* cite: dumps/GG_Hornet_1/physics.json#Physics2D.baumgarteTOIScale ; E4 */
#define PH_MAX_LINEAR_CORR    0.2f    /* cite: dumps/GG_Hornet_1/physics.json#Physics2D.maxLinearCorrection */
#define PH_VELOCITY_THRESHOLD 1.0f    /* cite: dumps/GG_Hornet_1/physics.json#Physics2D.velocityThreshold */
#define PH_MAX_TRANSLATION    100.0f  /* b2_maxTranslation = Physics2D.maxTranslationSpeed, a per-step distance (UP!0x180c08b80
                                        * Physics2DSettings::UpdateBox2D; compared with |h*v| in UP!0x180bad210 b2Island::Solve):
                                        * physics.json#Physics2D.maxTranslationSpeed 100 */
/* b2_maxRotation = Physics2D.maxRotationSpeed * 0.017453292, a per-step angle (UP!0x180c08b80 Physics2DSettings::UpdateBox2D;
 * compared with (h*w)^2 in UP!0x180bad210 b2Island::Solve): physics.json#Physics2D.maxRotationSpeed 360 */
#define PH_MAX_ROTATION       (360.0f * 0.017453292f)
#define PH_AABB_EXTENSION     0.1f    /* cite: box2d-v2.3.1 Common/b2Settings.h:59 (b2_aabbExtension) */
#define PH_AABB_MULTIPLIER    2.0f    /* cite: box2d-v2.3.1 Common/b2Settings.h:64 (b2_aabbMultiplier) */
#define PH_MAX_POLY_VERTS     8
#define PH_MAX_MANIFOLD_PTS   2
#define PH_MAX_SUBSTEPS       8       /* cite: analysis/specs/port-phys.md#E4 (b2World::SolveTOI structure) */
#define PH_MAX_TOI_CONTACTS   32
#define PH_EPSILON            FLT_EPSILON
#define PH_WELD_DIST_SQ       6.25e-06f  /* b2PolygonShape::Set weld, (0.5 * b2_linearSlop)^2 (UP!0x180ba7630) */
/* Ear clipping (add_polygon) emits n-2 triangles and asserts n <= 64, so 62 makes the "too many
 * pieces" trap unreachable for any accepted polygon: a capacity bound, not a tuned number. */
#define PH_MAX_PIECES         62

/* ---- small math (b2Math semantics, one rounding per operation) ------------------------------ */
typedef phys_v2 v2;
typedef struct { float s, c; } rot_t;           /* b2Rot: sin, cos */
typedef struct { v2 p; rot_t q; } xf_t;         /* b2Transform */

static inline v2 V2(float x, float y) { v2 r; r.x = x; r.y = y; return r; }
static inline v2 v2_add(v2 a, v2 b) { return V2(a.x + b.x, a.y + b.y); }
static inline v2 v2_sub(v2 a, v2 b) { return V2(a.x - b.x, a.y - b.y); }
static inline v2 v2_scale(float s, v2 a) { return V2(s * a.x, s * a.y); }
static inline v2 v2_neg(v2 a) { return V2(-a.x, -a.y); }
static inline float v2_dot(v2 a, v2 b) { return a.x * b.x + a.y * b.y; }
static inline float v2_cross(v2 a, v2 b) { return a.x * b.y - a.y * b.x; }
static inline v2 v2_cross_vs(v2 a, float s) { return V2(s * a.y, -s * a.x); }   /* b2Cross(b2Vec2, float) */
static inline v2 v2_cross_sv(float s, v2 a) { return V2(-s * a.y, s * a.x); }   /* b2Cross(float, b2Vec2) */
static inline float v2_len_sq(v2 a) { return a.x * a.x + a.y * a.y; }
static inline float v2_len(v2 a) { return sqrtf(a.x * a.x + a.y * a.y); }
static inline float v2_normalize(v2 *a) {           /* b2Vec2::Normalize */
    float length = v2_len(*a);
    if (length < PH_EPSILON) return 0.0f;
    float inv = 1.0f / length;
    a->x *= inv; a->y *= inv;
    return length;
}
static inline float ph_min(float a, float b) { return a < b ? a : b; }   /* b2Min */
static inline float ph_max(float a, float b) { return a > b ? a : b; }   /* b2Max */
static inline float ph_clamp(float a, float lo, float hi) { return ph_max(lo, ph_min(a, hi)); } /* b2Clamp */
static inline v2 rot_mul(rot_t q, v2 v) { return V2(q.c * v.x - q.s * v.y, q.s * v.x + q.c * v.y); }
static inline v2 rot_mulT(rot_t q, v2 v) { return V2(q.c * v.x + q.s * v.y, -q.s * v.x + q.c * v.y); }
static inline rot_t rot_mulT_rr(rot_t q, rot_t r) { rot_t o; o.s = q.c * r.s - q.s * r.c; o.c = q.c * r.c + q.s * r.s; return o; }
static inline v2 xf_mul(xf_t T, v2 v) {
    float x = (T.q.c * v.x - T.q.s * v.y) + T.p.x;
    float y = (T.q.s * v.x + T.q.c * v.y) + T.p.y;
    return V2(x, y);
}
static inline v2 xf_mulT(xf_t T, v2 v) {
    float px = v.x - T.p.x, py = v.y - T.p.y;
    return V2(T.q.c * px + T.q.s * py, -T.q.s * px + T.q.c * py);
}
static inline xf_t xf_mulT_xx(xf_t A, xf_t B) { xf_t C; C.q = rot_mulT_rr(A.q, B.q); C.p = rot_mulT(A.q, v2_sub(B.p, A.p)); return C; }
static inline rot_t rot_identity(void) { rot_t q; q.s = 0.0f; q.c = 1.0f; return q; }
/* b2Rot::Set(angle) (box2d-v2.3.1 Common/b2Math.h:312-317): s = sinf(angle), c = cosf(angle); the angle is the
 * transform's z euler times Mathf.Deg2Rad = PI * 2F / 360F (UnityCsReference Runtime/Export/Math/Mathf.cs:171). */
static inline float ph_deg_to_rad(float deg) { return deg * 0.017453292f; }
static inline rot_t ph_rot(float angle) { rot_t q; q.s = sinf(angle); q.c = cosf(angle); return q; }   /* b2Rot::Set */

typedef struct { v2 lower, upper; } aabb_t;
static inline bool aabb_overlap(const aabb_t *a, const aabb_t *b) {   /* b2TestOverlap(aabb, aabb) */
    v2 d1 = v2_sub(b->lower, a->upper), d2 = v2_sub(a->lower, b->upper);
    if (d1.x > 0.0f || d1.y > 0.0f) return false;
    if (d2.x > 0.0f || d2.y > 0.0f) return false;
    return true;
}

/* ---- broad phase (phys_broadphase.c: b2DynamicTree + b2BroadPhase) --------------------------------- */
#define PH_NULL_NODE (-1)                     /* b2_nullNode, b2BroadPhase::e_nullProxy */
typedef struct {                              /* b2TreeNode; `parent` doubles as the free-list `next` */
    aabb_t aabb;                              /* fat AABB */
    uint32_t shape; int32_t piece;            /* userData: the b2FixtureProxy (shape id, child index) */
    int32_t parent, child1, child2, height;   /* height: leaf 0, free -1 */
} tree_node;
typedef struct { tree_node *nodes; int32_t root, node_count, node_capacity, free_list, insertion_count; } dyn_tree;
typedef struct { int32_t a, b; } bp_pair;    /* b2Pair: a = min proxy id, b = max */
typedef struct {
    dyn_tree tree; int32_t proxy_count;
    int32_t *moves; int32_t move_cap, move_count;
    bp_pair *pairs; int32_t pair_cap, pair_count;
    int32_t query_proxy;
} broadphase;
void    ph_bp_init(broadphase *bp);
void    ph_bp_free(broadphase *bp);
int32_t ph_bp_create_proxy(broadphase *bp, const aabb_t *aabb, uint32_t shape, int32_t piece);
void    ph_bp_destroy_proxy(broadphase *bp, int32_t proxy);
void    ph_bp_move_proxy(broadphase *bp, int32_t proxy, const aabb_t *aabb, phys_v2 displacement);
void    ph_bp_touch_proxy(broadphase *bp, int32_t proxy);
bool    ph_bp_test_overlap(const broadphase *bp, int32_t a, int32_t b);
void    ph_bp_update_pairs(broadphase *bp, void (*add_pair)(void *ctx, const tree_node *a, const tree_node *b), void *ctx);

/* ---- shapes ---------------------------------------------------------------------------------- */
typedef enum { PC_POLYGON = 0, PC_CIRCLE = 1, PC_EDGE = 2 } piece_kind;

/* One Box2D fixture-shape.  A phys shape bakes into 1..N pieces (N>1 only for concave polygons, Q-pphys-5). */
typedef struct {
    piece_kind kind;
    float radius;                   /* b2Shape::m_radius */
    int   count;                    /* polygon vertex count */
    v2    vertices[PH_MAX_POLY_VERTS];
    v2    normals[PH_MAX_POLY_VERTS];
    v2    center;                   /* circle */
    v2    e0, e1, e2, e3;           /* edge: b2EdgeShape m_vertex0..3 (chain ghost vertices) */
    bool  has_e0, has_e3;
} piece_t;

typedef struct {
    bool alive, enabled, is_trigger;
    phys_shape_type type;
    phys_body_id body;
    uint32_t user;
    int32_t iid;                    /* Collider2D.GetInstanceID(): orders the pair's two colliders (UP!0x180bfee50) */
    uint32_t layer;                 /* PHYS_LAYER_INHERIT = the body's layer; Unity filters per collider GameObject */
    v2 offset, size; float radius, edge_radius;
    v2 *points; uint32_t n_points;
    /* Baked pieces, heap-grown by piece_reserve up to PH_MAX_PIECES. */
    int n_pieces, cap_pieces;
    piece_t *pieces;
    /* A re-baked polygon's last two decompositions (ph_shape_bake), NULL until its second bake. */
    struct bake_memo *bake_memo; bool baked;
    /* b2Fixture: a shape is a fixture while it is enabled (Unity destroys the fixture on disable and creates it
     * on enable); its proxies exist while it is a fixture of an active (simulated) body. */
    bool is_fixture;
    phys_shape_id next_fixture;     /* b2Fixture::m_next: the body's fixture list, newest first (b2Body.cpp:186-187) */
    int32_t *proxies; int n_proxies;/* b2Fixture::m_proxies[].proxyId per piece; m_proxyCount */
} shape_t;

/* ---- bodies ---------------------------------------------------------------------------------- */
typedef struct {
    bool alive, simulated, awake;   /* awake: every body stays awake (no sleep: port-phys.md Q-pphys-8) */
    int32_t world_index;            /* slot in phys_world.nonstatic (b2Body::m_worldIndex), -1 for a static body */
    bool free_rotation;             /* !b2Body::e_fixedRotationFlag (Rigidbody2D.constraints without FreezeRotation) */
    phys_body_type type; phys_cd_mode cd;
    v2 p;           /* transform / Rigidbody2D.position == b2Transform::p = c - R*lc (E1) */
    v2 c, c0;       /* b2Sweep::c / c0: the centre of mass is what Box2D integrates (E1) */
    v2 lc;          /* b2Sweep::localCenter = density-weighted centroid of the enabled non-trigger shapes (E1) */
    float a, a0;    /* b2Sweep::a / a0, radians, unwrapped */
    float a_step;   /* a at the start of the current phys_step (the write-back hands on a changed angle) */
    float alpha0;   /* b2Sweep::alpha0 */
    rot_t q;        /* b2Transform::q = b2Rot(a) */
    float rot_deg;  /* the Transform's z euler as last exchanged with the body (SetTransform in, write-back out) */
    bool rot_out;   /* the step turned the body: rot_deg holds the write-back, not yet taken (phys_body_take_rotation) */
    bool pos_out;   /* the step's position write-back to the Transform is due, not yet taken (phys_body_take_writeback) */
    v2 scale;
    v2 v;
    float w;        /* b2Body::m_angularVelocity, rad/s */
    float gravity_scale;
    float rb_mass;  /* Rigidbody2D.m_Mass (useAutoMass is false on every serialized Rigidbody2D: analysis/assets) */
    float mass, inv_mass, I, inv_I;       /* b2Body::m_mass / m_invMass / m_I / m_invI (ResetMassData) */
    float linear_damping, angular_damping;   /* Rigidbody2D.drag / angularDrag (Rigidbody2D::Create UP!0x180c10e30) */
    v2 force;       /* b2Body::m_force: accumulated by ApplyForceToCenter, consumed by island_solve,
                     * cleared at the end of the step (b2World::ClearForces). */
    float torque;   /* b2Body::m_torque: Rigidbody2D.AddTorque(Force), cleared with m_force */
    uint32_t layer, user;
    bool island_flag; int island_index;   /* transient island bookkeeping */
    phys_shape_id fixture_list;           /* b2Body::m_fixtureList */
    int32_t contact_list;                 /* b2Body::m_contactList: edge ref (contact index * 2 + side), -1 = none */
} body_t;

/* ---- contacts --------------------------------------------------------------------------------- */
typedef enum { MT_CIRCLES = 0, MT_FACE_A = 1, MT_FACE_B = 2 } manifold_type;
typedef enum { CF_VERTEX = 0, CF_FACE = 1 } cf_type;
typedef union { struct { uint8_t indexA, indexB, typeA, typeB; } cf; uint32_t key; } contact_id;
typedef struct { v2 local_point; float normal_impulse, tangent_impulse; contact_id id; } manifold_point;
typedef struct { manifold_point points[PH_MAX_MANIFOLD_PTS]; v2 local_normal; v2 local_point; manifold_type type; int point_count; } manifold_t;

typedef struct { int32_t prev, next; } edge_link;   /* b2ContactEdge prev/next as edge refs; side 0 = m_nodeA, 1 = m_nodeB */
typedef struct {
    bool alive;
    bool toi_class;                 /* e_toiCandidateFlag: solid with a bullet body at creation (UP!0x180bac390) */
    int32_t mgr;                    /* m_managerIndex: slot in phys_world.carr[toi_class] */
    int32_t coll, man;              /* m_userData / m_userIndex: its collider pair's record and manifold entry (-1) */
    int32_t prev, next;             /* b2Contact::m_prev / m_next in b2ContactManager::m_contactList */
    edge_link node[2];              /* m_nodeA (on body A's list) / m_nodeB (on body B's list) */
    phys_shape_id sa, sb; int pa, pb;   /* shape ids + piece indices; A/B assignment per E5 */
    phys_body_id ba, bb;
    manifold_t m;
    bool touching, enabled, filter_flag, island_flag;
    bool toi_flag; float toi; int toi_count;
    float friction, restitution;
} contact_t;

/* ---- Unity's per-collider-pair record (Collision2D in PhysicsContacts2D::m_Collisions) --------------- */
enum { CS_ENTER = 1, CS_EXIT = 2, CS_ENTER_EXIT = 3, CS_STAY = 4 };   /* ContactState (types/physics2d.layout.txt) */
#define PH_MAN_NULL  (-1)            /* entry whose b2Contact ended while the pair was Enter / EnterAndExit */
#define PH_MAN_STALE (-2)            /* entry of a b2Contact destroyed by a recreate: never updated or dropped again */
/* Collision2D::m_Manifolds entry: the b2Contact and what PhysicsContacts2D::PreSolve last stored for it */
typedef struct { int32_t contact; v2 normal, point; uint32_t count; } coll_man;
typedef struct {
    bool alive, is_trigger, flag_recreate;
    uint8_t state;                  /* CS_* */
    phys_shape_id sa, sb;           /* ColliderA = the lower GetInstanceID(), ColliderB */
    int32_t count;                  /* m_ContactCount: touching b2Contacts */
    int32_t slot;                   /* index in phys_world.colls (m_CollisionIndex) */
    coll_man *man; int32_t n_man, cap_man;
    int32_t next_free;
} coll_t;

/* ---- world ----------------------------------------------------------------------------------- */
struct phys_world {
    v2 gravity; uint32_t vel_iters, pos_iters; uint32_t layer_mask[32];
    body_t *bodies; uint32_t n_bodies, cap_bodies;       /* index = id (0 unused) */
    shape_t *shapes; uint32_t n_shapes, cap_shapes;
    broadphase bp;                                        /* b2ContactManager::m_broadPhase */
    contact_t *contacts; uint32_t n_contacts, cap_contacts;   /* pool; n_contacts = slots ever used */
    int32_t contact_list, contact_free; uint32_t contact_count;   /* b2ContactManager::m_contactList / m_contactCount */
    int32_t *carr[2]; uint32_t n_carr[2], cap_carr[2];   /* m_contactsNonTOI / m_contactsTOI (UP!0x180bac390) */
    int32_t *nonstatic; uint32_t n_nonstatic, cap_nonstatic;   /* b2World::m_nonStaticBodies (UP!0x180bab560) */
    coll_t *coll_pool; uint32_t n_coll_pool, cap_coll_pool; int32_t coll_free;
    int32_t *colls; uint32_t n_colls, cap_colls;          /* PhysicsContacts2D::m_Collisions */
    int32_t *coll_hash; uint32_t cap_coll_hash, n_coll_hash_used;   /* (ColliderA, ColliderB) -> record */
    phys_event *events; uint32_t n_events, cap_events;    /* the step's reports (ProcessContacts(null)) */
    phys_event *exits; uint32_t n_exits, cap_exits;       /* reports of a collider disable (ProcessContacts(collider)) */
    phys_event *rep_coll; uint32_t n_rep_coll, cap_rep_coll;   /* collision reports while a walk builds them */
    float inv_dt0; bool step_complete;
    bool new_fixture;      /* b2World::e_newFixture -- set by CreateFixture (b2Body.cpp:200) and
                            * cleared by Step; gates the pre-step FindNewContacts (b2World.cpp:902-906). */
};

/* ---- internal APIs --------------------------------------------------------------------------- */
/* phys_shape.c */
void  ph_shape_bake(phys_world *w, shape_t *s, const body_t *b);
void  ph_piece_aabb(const piece_t *pc, xf_t xf, aabb_t *out);
void  ph_piece_mass(const piece_t *pc, float density, float *mass, v2 *center, float *I);   /* b2Shape::ComputeMass */
xf_t  ph_body_xf(const body_t *b);
/* phys_collide.c */
void  ph_collide(manifold_t *m, const piece_t *A, xf_t xfA, const piece_t *B, xf_t xfB);
void  ph_collide_edge_circle(manifold_t *m, const piece_t *edgeA, xf_t xfA, const piece_t *circleB, xf_t xfB);   /* phys_collide_edge.c */
void  ph_collide_edge_polygon(manifold_t *m, const piece_t *edgeA, xf_t xfA, const piece_t *polygonB, xf_t xfB);
bool  ph_test_overlap(const piece_t *A, xf_t xfA, const piece_t *B, xf_t xfB);
bool  ph_piece_raycast(const piece_t *pc, xf_t xf, v2 p1, v2 p2, float max_fraction, float *fraction, v2 *normal);
void  ph_world_manifold(const manifold_t *m, xf_t xfA, float rA, xf_t xfB, float rB, v2 *normal, v2 points[2], float seps[2]);
/* phys_distance.c */
typedef struct { v2 vertices[PH_MAX_POLY_VERTS]; int count; float radius; } dist_proxy;
typedef struct { float metric; uint16_t count; uint8_t indexA[3], indexB[3]; } simplex_cache;
typedef struct { dist_proxy proxyA, proxyB; xf_t xfA, xfB; bool use_radii; } dist_input;
typedef struct { v2 pointA, pointB; float distance; int iterations; } dist_output;
typedef struct { v2 c0, c, localCenter; float a0, a, alpha0; } sweep_t;   /* b2Sweep */
typedef struct { dist_proxy proxyA, proxyB; sweep_t sweepA, sweepB; float tMax; } toi_input;
typedef enum { TOI_UNKNOWN, TOI_FAILED, TOI_OVERLAPPED, TOI_TOUCHING, TOI_SEPARATED } toi_state;
typedef struct { toi_state state; float t; } toi_output;
void  ph_proxy_set(dist_proxy *p, const piece_t *pc);
void  ph_distance(dist_output *out, simplex_cache *cache, const dist_input *in);
void  ph_time_of_impact(toi_output *out, const toi_input *in);
void  ph_sweep_get_transform(const sweep_t *s, xf_t *xf, float beta);
void  ph_sweep_advance(sweep_t *s, float alpha);
/* phys_solver.c */
void  ph_solve_islands(phys_world *w, float dt, float dt_ratio);
void  ph_solve_toi(phys_world *w, float dt);
/* phys_world.c (contact management used by the solver) */
void  ph_contact_update(phys_world *w, contact_t *c);
void  ph_find_new_contacts(phys_world *w);                /* b2ContactManager::FindNewContacts */
void  ph_body_synchronize_fixtures(phys_world *w, body_t *b);   /* b2Body::SynchronizeFixtures */
/* b2Body::m_contactList iteration: first edge ref of body b / next edge ref / the edge's contact and other body */
static inline int32_t ph_edge_next(const phys_world *w, int32_t e) { return w->contacts[e >> 1].node[e & 1].next; }
static inline contact_t *ph_edge_contact(phys_world *w, int32_t e) { return &w->contacts[e >> 1]; }
static inline phys_body_id ph_edge_other(const phys_world *w, int32_t e) { const contact_t *c = &w->contacts[e >> 1]; return (e & 1) ? c->ba : c->bb; }
static inline uint32_t ph_shape_layer(const phys_world *w, const shape_t *s) {   /* effective layer */
    return s->layer == PHYS_LAYER_INHERIT ? w->bodies[s->body].layer : s->layer;
}
void  ph_body_sync_transform(body_t *b);              /* b2Body::SynchronizeTransform: q = b2Rot(a), p = c - R*lc */
void  ph_body_reset_mass_data(phys_world *w, body_t *b); /* b2Body::ResetMassData: lc from fixtures, c = p + R*lc */
