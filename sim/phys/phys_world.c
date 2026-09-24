/* sim/core/phys.h implementation: world, bodies, shapes, the contact manager, the step and the callbacks.
 * The authority is Unity 2020.2.2f1's Box2D fork and its Physics2D glue, read from the native decompile
 * (analysis/decomp_native, cited UP!<addr> <symbol>; rulings in analysis/native_specs/native-box2d.md and
 * native-physics2d.md).  Where the fork is unchanged the Box2D 2.3.1 file:line is cited
 * (analysis/upstream/box2d-v2.3.1/Box2D/Box2D); the broad phase is phys_broadphase.c.  Measured deltas are
 * analysis/specs/port-phys.md#U<n>.
 *
 * Callbacks: Unity keeps one record per collider pair (Collision2D) in PhysicsContacts2D::m_Collisions.  A record
 * is appended when the pair's first b2Contact begins touching and removed by swap-with-last when it exits, and
 * ProcessContacts walks the array once per step (and once per collider on a disable): trigger reports first, then
 * collision reports, each to the lower-instance-id collider first (native-physics2d.md §6). */
#include "phys_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "core/alloc.h"   /* per-instance arena */

#define GROW(arr, n, cap, type) do { if ((n) >= (cap)) { uint32_t nc = (cap) ? (cap) * 2u : 16u; \
    arr = (type *)realloc(arr, nc * sizeof(type)); HKSIM_ASSERT(arr != NULL, "phys: out of memory"); \
    memset((arr) + (cap), 0, (nc - (cap)) * sizeof(type)); (cap) = nc; } } while (0)

static body_t *body_of(phys_world *w, phys_body_id b)
{
    HKSIM_ASSERT(b != 0 && b < w->n_bodies && w->bodies[b].alive, "phys: bad body id %u", b);
    return &w->bodies[b];
}
static const body_t *body_ofc(const phys_world *w, phys_body_id b)
{
    HKSIM_ASSERT(b != 0 && b < w->n_bodies && w->bodies[b].alive, "phys: bad body id %u", b);
    return &w->bodies[b];
}
static shape_t *shape_of(phys_world *w, phys_shape_id s)
{
    HKSIM_ASSERT(s != 0 && s < w->n_shapes && w->shapes[s].alive, "phys: bad shape id %u", s);
    return &w->shapes[s];
}

/* ---- body pose (b2Body::SynchronizeTransform / ResetMassData / SetTransform) ------------------ */
void ph_body_sync_transform(body_t *b)
{
    b->q = ph_rot(b->a);
    b->p = v2_sub(b->c, rot_mul(b->q, b->lc));
}

/* b2Body::ResetMassData (UP!0x180bac920): dynamic bodies only; the fixtures in m_fixtureList order (newest first)
 * with density != 0 and not sensors (Unity leaves the triggers out: port-phys.md#U1).  localCenter = sum(c*m) *
 * (1/sum(m)); Unity's deltas: the mass is Rigidbody2D.mass (useAutoMass is false on every serialized Rigidbody2D,
 * analysis/assets), a free-rotation body's inertia about the centre is scaled by Rigidbody2D.mass / sum(m), and a
 * body whose shapes give no inertia (only triggers, or none) gets I = 1.  Explicit inertia / centre of mass are
 * never set by the ported code.  Then c0 = c = xf*lc and v += cross(w, c - old c).  Kinematic and static bodies have
 * no mass and lc = 0.  Density is Collider2D.density (1.0 on every dumped collider: scene.json#colliders[].density). */
void ph_body_reset_mass_data(phys_world *w, body_t *b)
{
    b->mass = 0.0f; b->inv_mass = 0.0f; b->I = 0.0f; b->inv_I = 0.0f;
    b->lc = V2(0.0f, 0.0f);
    if (b->type != PHYS_BODY_DYNAMIC) {
        b->c0 = b->c = b->p;
        b->a0 = b->a;
        return;
    }
    float mass = 0.0f, I = 0.0f, cx = 0.0f, cy = 0.0f;
    for (phys_shape_id f = b->fixture_list; f; f = w->shapes[f].next_fixture) {
        const shape_t *s = &w->shapes[f];
        if (s->is_trigger) continue;
        for (int k = s->n_pieces - 1; k >= 0; k--) {   /* one b2Fixture per piece, prepended as created */
            float m, pI; v2 center;
            ph_piece_mass(&s->pieces[k], 1.0f, &m, &center, &pI);
            mass = m + mass;
            cx = cx + center.x * m;
            cy = cy + center.y * m;
            I = pI + I;
        }
    }
    if (mass <= 0.0f) mass = 1.0f;
    else { float inv = 1.0f / mass; cx = cx * inv; cy = cy * inv; }
    if (b->free_rotation) {
        if (I <= 0.0f) {
            I = 1.0f;
        } else {
            I = I - (cx * cx + cy * cy) * mass;
            if (I < 1.1920929e-07f) I = 1.1920929e-07f;
            I = (b->rb_mass / mass) * I;
        }
        b->I = I; b->inv_I = 1.0f / I;
    }
    b->mass = b->rb_mass;
    b->inv_mass = 1.0f / b->rb_mass;
    b->lc = V2(cx, cy);
    v2 old = b->c;
    xf_t xf; xf.p = b->p; xf.q = b->q;
    b->c0 = b->c = xf_mul(xf, b->lc);
    b->v.y = (b->c.x - old.x) * b->w + b->v.y;
    b->v.x = -((b->c.y - old.y) * b->w) + b->v.x;
}

/* ---- world ----------------------------------------------------------------------------------- */
phys_world *phys_create(phys_v2 gravity, uint32_t velocity_iters, uint32_t position_iters, const uint32_t layer_matrix[32])
{
    phys_world *w = (phys_world *)calloc(1, sizeof *w);
    HKSIM_ASSERT(w != NULL, "phys: out of memory");
    w->gravity = gravity; w->vel_iters = velocity_iters; w->pos_iters = position_iters;
    memcpy(w->layer_mask, layer_matrix, sizeof w->layer_mask);
    GROW(w->bodies, 1u, w->cap_bodies, body_t); w->n_bodies = 1;     /* id 0 = none */
    GROW(w->shapes, 1u, w->cap_shapes, shape_t); w->n_shapes = 1;
    ph_bp_init(&w->bp);
    w->contact_list = -1; w->contact_free = -1; w->coll_free = -1;
    w->inv_dt0 = 0.0f; w->step_complete = true;
    return w;
}

void phys_destroy(phys_world *w)
{
    if (!w) return;
    for (uint32_t i = 1; i < w->n_shapes; i++) { free(w->shapes[i].points); free(w->shapes[i].pieces); free(w->shapes[i].proxies); }
    for (uint32_t i = 0; i < w->n_coll_pool; i++) free(w->coll_pool[i].man);
    ph_bp_free(&w->bp);
    free(w->bodies); free(w->shapes); free(w->contacts); free(w->carr[0]); free(w->carr[1]); free(w->nonstatic);
    free(w->coll_pool); free(w->colls); free(w->coll_hash);
    free(w->events); free(w->exits); free(w->rep_coll);
    free(w);
}

/* ---- collider-pair records: PhysicsContacts2D (UP!0x180bfee50 BeginContact, UP!0x180c02770 EndContact,
 *      UP!0x180c07260 RemoveContact, UP!0x180c02c00 FlagForRecreate, UP!0x180c05000 PreSolve) ------------ */
/* m_CollisionIndex: (ColliderA, ColliderB) -> record.  Open addressing over the record indices; only lookups
 * go through it, so its layout never decides an order. */
#define CH_EMPTY (-1)
#define CH_TOMB  (-2)
static uint32_t coll_key_hash(phys_shape_id a, phys_shape_id b)
{
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return h;
}
static int32_t coll_find(const phys_world *w, phys_shape_id a, phys_shape_id b)
{
    if (!w->cap_coll_hash) return -1;
    uint32_t mask = w->cap_coll_hash - 1;
    for (uint32_t i = coll_key_hash(a, b) & mask;; i = (i + 1) & mask) {
        int32_t r = w->coll_hash[i];
        if (r == CH_EMPTY) return -1;
        if (r >= 0 && w->coll_pool[r].sa == a && w->coll_pool[r].sb == b) return r;
    }
}
static void coll_hash_put(phys_world *w, int32_t r);
static void coll_hash_grow(phys_world *w)
{
    int32_t *old = w->coll_hash; uint32_t old_cap = w->cap_coll_hash;
    w->cap_coll_hash = old_cap ? old_cap * 2u : 64u;
    w->coll_hash = (int32_t *)malloc(w->cap_coll_hash * sizeof(int32_t));
    HKSIM_ASSERT(w->coll_hash != NULL, "phys: out of memory (collision index)");
    for (uint32_t i = 0; i < w->cap_coll_hash; i++) w->coll_hash[i] = CH_EMPTY;
    w->n_coll_hash_used = 0;
    for (uint32_t i = 0; i < old_cap; i++) if (old[i] >= 0) coll_hash_put(w, old[i]);
    free(old);
}
static void coll_hash_put(phys_world *w, int32_t r)
{
    if ((w->n_coll_hash_used + 1) * 2 > w->cap_coll_hash) coll_hash_grow(w);
    uint32_t mask = w->cap_coll_hash - 1;
    uint32_t i = coll_key_hash(w->coll_pool[r].sa, w->coll_pool[r].sb) & mask;
    while (w->coll_hash[i] >= 0) i = (i + 1) & mask;
    if (w->coll_hash[i] == CH_EMPTY) w->n_coll_hash_used++;
    w->coll_hash[i] = r;
}
static void coll_hash_del(phys_world *w, int32_t r)
{
    uint32_t mask = w->cap_coll_hash - 1;
    for (uint32_t i = coll_key_hash(w->coll_pool[r].sa, w->coll_pool[r].sb) & mask;; i = (i + 1) & mask) {
        HKSIM_ASSERT(w->coll_hash[i] != CH_EMPTY, "phys: collision record %d not indexed", r);
        if (w->coll_hash[i] == r) { w->coll_hash[i] = CH_TOMB; return; }
    }
}

/* RemoveContact (UP!0x180c07260): the last record moves into the hole. */
static void coll_remove(phys_world *w, int32_t r)
{
    coll_t *rec = &w->coll_pool[r];
    coll_hash_del(w, r);
    uint32_t slot = (uint32_t)rec->slot, last = w->n_colls - 1;
    if (slot < last) { w->colls[slot] = w->colls[last]; w->coll_pool[w->colls[slot]].slot = (int32_t)slot; }
    w->n_colls--;
    rec->alive = false; rec->n_man = 0;
    rec->next_free = w->coll_free; w->coll_free = r;
}

static void coll_man_push(coll_t *rec, int32_t ci)
{
    if (rec->n_man == rec->cap_man) {
        rec->cap_man = rec->cap_man ? rec->cap_man * 2 : 2;
        rec->man = (coll_man *)realloc(rec->man, (size_t)rec->cap_man * sizeof *rec->man);
        HKSIM_ASSERT(rec->man != NULL, "phys: out of memory (manifold entries)");
    }
    coll_man *e = &rec->man[rec->n_man++];
    e->contact = ci; e->normal = V2(0.0f, 0.0f); e->point = V2(0.0f, 0.0f); e->count = 0;
}

/* BeginContact (UP!0x180bfee50): ColliderA is the collider with the lower GetInstanceID() (asm 180bfeea2: swap when
 * A's is greater).  A new pair is appended to m_Collisions in state Enter; a pair already recorded (another piece
 * touching, or one that ended earlier this step) counts one more contact and turns Exit back into Stay and
 * EnterAndExit back into Enter, unless it is being recreated.  A solid contact appends its manifold entry. */
static void begin_contact(phys_world *w, contact_t *c)
{
    int32_t ci = (int32_t)(c - w->contacts);
    phys_shape_id a = c->sa, b = c->sb;
    if (w->shapes[b].iid < w->shapes[a].iid) { a = c->sb; b = c->sa; }
    bool sensor = w->shapes[a].is_trigger || w->shapes[b].is_trigger;
    int32_t r = coll_find(w, a, b);
    coll_t *rec;
    if (r < 0) {
        if (w->coll_free >= 0) { r = w->coll_free; w->coll_free = w->coll_pool[r].next_free; }
        else { GROW(w->coll_pool, w->n_coll_pool, w->cap_coll_pool, coll_t); r = (int32_t)w->n_coll_pool++; }
        rec = &w->coll_pool[r];
        rec->alive = true; rec->sa = a; rec->sb = b;
        rec->state = CS_ENTER; rec->count = 1; rec->is_trigger = sensor; rec->flag_recreate = false;
        rec->n_man = 0;
        GROW(w->colls, w->n_colls, w->cap_colls, int32_t);
        rec->slot = (int32_t)w->n_colls; w->colls[w->n_colls++] = r;
        coll_hash_put(w, r);
    } else {
        rec = &w->coll_pool[r];
        rec->count++;
        rec->is_trigger = sensor;
        if (!rec->flag_recreate) {
            if (rec->state == CS_ENTER_EXIT) rec->state = CS_ENTER;
            else if (rec->state == CS_EXIT) rec->state = CS_STAY;
        }
    }
    c->coll = r; c->man = -1;
    if (!sensor) { coll_man_push(rec, ci); c->man = rec->n_man - 1; }
}

/* EndContact (UP!0x180c02770): one contact fewer.  Outside a recreate the manifold entry is nulled while the pair
 * is Enter / EnterAndExit and swap-removed otherwise; the last contact of the pair turns Enter into EnterAndExit
 * and Stay into Exit.  During a recreate the entry is left as it is (its b2Contact is gone). */
static void end_contact(phys_world *w, contact_t *c)
{
    coll_t *rec = &w->coll_pool[c->coll];
    rec->count--;
    int32_t i = c->man;
    if (i >= 0) {
        if (rec->flag_recreate) rec->man[i].contact = PH_MAN_STALE;
        else if (rec->state == CS_ENTER || rec->state == CS_ENTER_EXIT) rec->man[i].contact = PH_MAN_NULL;
        else {
            int32_t last = --rec->n_man;
            rec->man[i] = rec->man[last];
            if (i != last && rec->man[i].contact >= 0) w->contacts[rec->man[i].contact].man = i;
        }
    }
    c->man = -1; c->coll = -1;
    if (rec->count < 1 && !rec->flag_recreate) {
        if (rec->state == CS_ENTER) rec->state = CS_ENTER_EXIT;
        else if (rec->state == CS_STAY) rec->state = CS_EXIT;
    }
}

/* PreSolve (UP!0x180c05000): after every Update that leaves a solid contact touching, its manifold entry takes the
 * world manifold at that moment: the normal toward ColliderA (b2's A->B normal negated unless A and B were swapped),
 * the first point and the point count.  A callback reports its pair's first entry. */
static void pre_solve(phys_world *w, contact_t *c)
{
    if (c->man < 0) return;
    coll_t *rec = &w->coll_pool[c->coll];
    const body_t *bA = &w->bodies[c->ba], *bB = &w->bodies[c->bb];
    v2 normal, pts[2]; float seps[2];
    ph_world_manifold(&c->m, ph_body_xf(bA), w->shapes[c->sa].pieces[c->pa].radius,
                      ph_body_xf(bB), w->shapes[c->sb].pieces[c->pb].radius, &normal, pts, seps);
    coll_man *e = &rec->man[c->man];
    e->normal = rec->sa == c->sa ? v2_neg(normal) : normal;
    e->point = pts[0]; e->count = (uint32_t)c->m.point_count;
}

/* Cleanup(kColliderRecreate)'s first half (UP!0x180c00240, as FlagForRecreate UP!0x180c02c00): every pair of the
 * collider keeps its record as Stay and ignores Begin/End until the next ProcessContacts decides it. */
static void flag_for_recreate(phys_world *w, phys_shape_id sid)
{
    for (uint32_t i = 0; i < w->n_colls; i++) {
        coll_t *rec = &w->coll_pool[w->colls[i]];
        if (rec->sa != sid && rec->sb != sid) continue;
        rec->flag_recreate = true; rec->state = CS_STAY;
    }
}

/* ---- reports: ProcessContacts (UP!0x180c06220) + SendCallbackReports' order (UP!0x180c07980) ----------------- */
static void push_event_to(phys_event **arr, uint32_t *n, uint32_t *cap, phys_event_kind kind, phys_body_id a, phys_body_id b,
                          phys_shape_id sa, phys_shape_id sb, v2 normal, v2 point, uint32_t count)
{
    GROW(*arr, *n, *cap, phys_event);
    phys_event *e = &(*arr)[(*n)++];
    e->kind = kind; e->body_a = a; e->body_b = b; e->shape_a = sa; e->shape_b = sb;
    e->normal = normal; e->point = point; e->contact_count = count;
}
/* One report = ColliderA's receivers, then ColliderB's.  kind: 0 Enter, 1 Stay, 2 Exit (AddTriggerReport
 * UP!0x180bfe600 / AddCollisionReport UP!0x180bfe290: Enter for Enter and EnterAndExit).  `with_contacts` false is
 * the EnterAndExit copy whose manifolds were cleared. */
static void add_report(phys_world *w, const coll_t *rec, int kind, bool with_contacts, phys_event **arr, uint32_t *n, uint32_t *cap)
{
    phys_event_kind k = (phys_event_kind)((rec->is_trigger ? PHYS_EV_TRIGGER_ENTER : PHYS_EV_COLLISION_ENTER) + kind);
    v2 normal = V2(0.0f, 0.0f), point = V2(0.0f, 0.0f); uint32_t count = 0;
    if (!rec->is_trigger && with_contacts && rec->n_man > 0) { normal = rec->man[0].normal; point = rec->man[0].point; count = rec->man[0].count; }
    phys_body_id ba = w->shapes[rec->sa].body, bb = w->shapes[rec->sb].body;
    push_event_to(arr, n, cap, k, ba, bb, rec->sa, rec->sb, normal, point, count);
    push_event_to(arr, n, cap, k, bb, ba, rec->sb, rec->sa, v2_neg(normal), point, count);
}
/* Rigidbody2D::IsSleeping, or no Rigidbody2D (a static collider sits on the ground body) */
static bool rb_absent_or_asleep(const phys_world *w, phys_shape_id s)
{
    const body_t *b = &w->bodies[w->shapes[s].body];
    return b->type == PHYS_BODY_STATIC || !b->awake;
}
/* One walk of m_Collisions: every record of `filter` (0 = all).  A recreated record with no contact left exits.
 * Stay is not reported while neither side has an awake Rigidbody2D.  Enter becomes Stay; Exit and EnterAndExit
 * remove the record, the last one moving into its slot, which is visited next (the index does not advance: asm
 * 180c068cf).  Trigger reports go out before collision reports. */
static void process_contacts(phys_world *w, phys_shape_id filter, phys_event **arr, uint32_t *n, uint32_t *cap)
{
    w->n_rep_coll = 0;
    for (uint32_t i = 0; i < w->n_colls; ) {
        int32_t r = w->colls[i];
        coll_t *rec = &w->coll_pool[r];
        if (filter && rec->sa != filter && rec->sb != filter) { i++; continue; }
        if (rec->flag_recreate) { rec->flag_recreate = false; if (rec->count == 0) rec->state = CS_EXIT; }
        bool trig = rec->is_trigger;
        phys_event **ra = trig ? arr : &w->rep_coll;
        uint32_t *rn = trig ? n : &w->n_rep_coll, *rc = trig ? cap : &w->cap_rep_coll;
        int kind = (rec->state == CS_ENTER || rec->state == CS_ENTER_EXIT) ? 0 : rec->state == CS_EXIT ? 2 : 1;
        if (!(rec->state == CS_STAY && rb_absent_or_asleep(w, rec->sa) && rb_absent_or_asleep(w, rec->sb)))
            add_report(w, rec, kind, true, ra, rn, rc);
        if (rec->count != rec->n_man && rec->n_man > 0) {        /* drop the entries whose b2Contact ended */
            for (int32_t j = 0; j < rec->n_man; ) {
                if (rec->man[j].contact != PH_MAN_NULL) { j++; continue; }
                int32_t last = --rec->n_man;
                rec->man[j] = rec->man[last];
                if (j != last && rec->man[j].contact >= 0) w->contacts[rec->man[j].contact].man = j;
            }
        }
        switch (rec->state) {
        case CS_ENTER: rec->state = CS_STAY; i++; break;
        case CS_EXIT: coll_remove(w, r); break;
        case CS_ENTER_EXIT: add_report(w, rec, 2, false, ra, rn, rc); coll_remove(w, r); break;
        default: i++; break;
        }
    }
    for (uint32_t k = 0; k < w->n_rep_coll; k++) {
        GROW(*arr, *n, *cap, phys_event);
        (*arr)[(*n)++] = w->rep_coll[k];
    }
}

uint32_t phys_take_exit_events(phys_world *w, phys_event *out, uint32_t cap)
{
    uint32_t n = w->n_exits < cap ? w->n_exits : cap;
    memcpy(out, w->exits, n * sizeof *out);
    memmove(w->exits, w->exits + n, (w->n_exits - n) * sizeof *out);
    w->n_exits -= n;
    return n;
}

/* ---- contacts (b2ContactManager) ------------------------------------------------------------- */
/* b2ContactManager::Destroy (UP!0x180babb80): EndContact if touching, unlink from m_contactList and both bodies'
 * edge lists, and swap-remove from its contact array. */
static void contact_destroy(phys_world *w, int32_t ci)
{
    contact_t *c = &w->contacts[ci];
    HKSIM_ASSERT(c->alive, "phys: destroy of a dead contact");
    if (c->touching) end_contact(w, c);
    if (c->prev >= 0) w->contacts[c->prev].next = c->next;
    if (c->next >= 0) w->contacts[c->next].prev = c->prev;
    if (ci == w->contact_list) w->contact_list = c->next;
    for (int side = 0; side < 2; side++) {
        body_t *b = &w->bodies[side == 0 ? c->ba : c->bb];
        edge_link *n = &c->node[side];
        if (n->prev >= 0) w->contacts[n->prev >> 1].node[n->prev & 1].next = n->next;
        if (n->next >= 0) w->contacts[n->next >> 1].node[n->next & 1].prev = n->prev;
        if (b->contact_list == ci * 2 + side) b->contact_list = n->next;
    }
    int k = c->toi_class ? 1 : 0;
    uint32_t last = w->n_carr[k] - 1;
    w->carr[k][c->mgr] = w->carr[k][last];
    w->contacts[w->carr[k][c->mgr]].mgr = c->mgr;
    w->n_carr[k]--;
    c->alive = false;
    c->next = w->contact_free; w->contact_free = ci;
    --w->contact_count;
}

/* b2Body::ShouldCollide + WorldContactFilter2D::ShouldCollide (UP!0x180bad1d0, UP!0x180c16b80): only static-static
 * pairs are rejected by the body test; a pair without a dynamic body needs a sensor (useFullKinematicContacts is
 * never set: port-phys.md#U5, Q-pphys-11); the layer matrix on each collider's own GameObject layer
 * (physics.json#layerCollisionMatrix: HeroBox is 20 on the layer-9 Knight). */
static bool should_collide(const phys_world *w, const shape_t *sa, const shape_t *sb)
{
    if (!sa->alive || !sb->alive || !sa->enabled || !sb->enabled) return false;
    if (sa->body == sb->body) return false;
    const body_t *ba = &w->bodies[sa->body], *bb = &w->bodies[sb->body];
    if (!ba->alive || !bb->alive || !ba->simulated || !bb->simulated) return false;
    if (ba->type == PHYS_BODY_STATIC && bb->type == PHYS_BODY_STATIC) return false;
    if (ba->type != PHYS_BODY_DYNAMIC && bb->type != PHYS_BODY_DYNAMIC && !(sa->is_trigger || sb->is_trigger)) return false;
    if (((w->layer_mask[ph_shape_layer(w, sa)] >> ph_shape_layer(w, sb)) & 1u) == 0) return false;
    return true;
}

/* b2Contact::Create's registry (UP!0x180baf780; b2Contact.cpp:48-64 AddType): a mixed pair is created edge/chain
 * first, then polygon, then circle; a same-kind pair keeps the broad phase's order (the lower proxy id = A). */
static bool registry_swaps(piece_kind a, piece_kind b)
{
    if (a == b) return false;
    if (a == PC_EDGE) return false;
    if (b == PC_EDGE) return true;
    return a != PC_POLYGON;
}

/* The pair callback of b2BroadPhase::UpdatePairs (UP!0x180baa590, AddPair inlined): `na` has the lower proxy id.
 * Skip same-body pairs and existing contacts, filter, create (b2Contact::b2Contact UP!0x180baf490), then
 * b2ContactManager::OnContactCreate (UP!0x180bac390): a solid contact with a bullet (Continuous) body is a TOI
 * candidate for its whole life; append to that array, prepend to m_contactList and to both bodies' edge lists. */
static void add_pair(void *ctx, const tree_node *na, const tree_node *nb)
{
    phys_world *w = (phys_world *)ctx;
    phys_shape_id sa = na->shape, sb = nb->shape; int pa = na->piece, pb = nb->piece;
    const shape_t *A = &w->shapes[sa], *B = &w->shapes[sb];
    if (A->body == B->body) return;
    for (int32_t e = w->bodies[B->body].contact_list; e >= 0; e = ph_edge_next(w, e)) {
        if (ph_edge_other(w, e) != A->body) continue;
        const contact_t *c = ph_edge_contact(w, e);
        if (c->sa == sa && c->sb == sb && c->pa == pa && c->pb == pb) return;
        if (c->sa == sb && c->sb == sa && c->pa == pb && c->pb == pa) return;
    }
    if (!should_collide(w, A, B)) return;
    if (registry_swaps(A->pieces[pa].kind, B->pieces[pb].kind)) {
        phys_shape_id ts = sa; sa = sb; sb = ts;
        int tp = pa; pa = pb; pb = tp;
        A = &w->shapes[sa]; B = &w->shapes[sb];
    }
    int32_t ci;
    if (w->contact_free >= 0) { ci = w->contact_free; w->contact_free = w->contacts[ci].next; }
    else { GROW(w->contacts, w->n_contacts, w->cap_contacts, contact_t); ci = (int32_t)w->n_contacts++; }
    contact_t *c = &w->contacts[ci];
    memset(c, 0, sizeof *c);
    c->alive = true;
    c->sa = sa; c->pa = pa; c->sb = sb; c->pb = pb;
    c->ba = A->body; c->bb = B->body;
    c->coll = -1; c->man = -1;
    c->enabled = true;                           /* m_flags = e_enabledFlag */
    c->toi = 1.0f; c->toi_count = 0;
    /* Materials are not in the interface (Q-pphys-3): the hero's FrictionlessSurface makes every hero contact
     * frictionless (mixed sqrt(0*x) = 0) and no rebound is ever recorded: cite: analysis/specs/port-phys.md#E3. */
    c->friction = 0.0f; c->restitution = 0.0f;
    c->toi_class = !A->is_trigger && !B->is_trigger &&
                   (w->bodies[c->ba].cd == PHYS_CD_CONTINUOUS || w->bodies[c->bb].cd == PHYS_CD_CONTINUOUS);
    int k = c->toi_class ? 1 : 0;
    GROW(w->carr[k], w->n_carr[k], w->cap_carr[k], int32_t);
    c->mgr = (int32_t)w->n_carr[k]; w->carr[k][w->n_carr[k]++] = ci;
    c->prev = -1; c->next = w->contact_list;
    if (w->contact_list >= 0) w->contacts[w->contact_list].prev = ci;
    w->contact_list = ci;
    for (int side = 0; side < 2; side++) {
        body_t *b = &w->bodies[side == 0 ? c->ba : c->bb];
        c->node[side].prev = -1; c->node[side].next = b->contact_list;
        if (b->contact_list >= 0) w->contacts[b->contact_list >> 1].node[b->contact_list & 1].prev = ci * 2 + side;
        b->contact_list = ci * 2 + side;
    }
    ++w->contact_count;
}

/* b2ContactManager::FindNewContacts */
void ph_find_new_contacts(phys_world *w) { ph_bp_update_pairs(&w->bp, add_pair, w); }

/* b2Contact::Update (UP!0x180bb0180 = b2Contact.cpp:161-247 plus Unity's re-enable): a sensor pair tests overlap,
 * a solid pair evaluates its manifold and carries the impulses of matching contact ids over; then BeginContact /
 * EndContact on a touching change and PreSolve for a touching solid pair. */
void ph_contact_update(phys_world *w, contact_t *c)
{
    shape_t *sA = &w->shapes[c->sa], *sB = &w->shapes[c->sb];
    body_t *bA = &w->bodies[c->ba], *bB = &w->bodies[c->bb];
    const piece_t *pA = &sA->pieces[c->pa], *pB = &sB->pieces[c->pb];
    xf_t xfA = ph_body_xf(bA), xfB = ph_body_xf(bB);
    manifold_t oldManifold = c->m;
    c->enabled = true;                           /* no e_ignoredFlag: Physics2D.IgnoreCollision is never called */
    bool touching = false, wasTouching = c->touching;
    bool sensor = sA->is_trigger || sB->is_trigger;
    if (sensor) {
        touching = ph_test_overlap(pA, xfA, pB, xfB);
        c->m.point_count = 0;
    } else {
        ph_collide(&c->m, pA, xfA, pB, xfB);
        touching = c->m.point_count > 0;
        for (int i = 0; i < c->m.point_count; i++) {
            manifold_point *mp2 = &c->m.points[i];
            mp2->normal_impulse = 0.0f; mp2->tangent_impulse = 0.0f;
            for (int j = 0; j < oldManifold.point_count; j++) {
                const manifold_point *mp1 = &oldManifold.points[j];
                if (mp1->id.key == mp2->id.key) { mp2->normal_impulse = mp1->normal_impulse; mp2->tangent_impulse = mp1->tangent_impulse; break; }
            }
        }
    }
    c->touching = touching;
    if (!wasTouching && touching) begin_contact(w, c);
    if (wasTouching && !touching) end_contact(w, c);
    if (!sensor && touching) pre_solve(w, c);
}

/* b2ContactManager::Collide over one contact array (UP!0x180bab360): array order, sensors included (the start-of-step
 * poses); a destroyed contact's slot takes the array's last contact, which is examined next. */
static void collide_array(phys_world *w, int k)
{
    for (uint32_t i = 0; i < w->n_carr[k]; ) {
        int32_t ci = w->carr[k][i];
        contact_t *c = &w->contacts[ci];
        shape_t *sA = &w->shapes[c->sa], *sB = &w->shapes[c->sb];
        if (c->filter_flag) {
            if (!should_collide(w, sA, sB)) { contact_destroy(w, ci); continue; }
            c->filter_flag = false;
        }
        const body_t *bA = &w->bodies[c->ba], *bB = &w->bodies[c->bb];
        bool activeA = bA->awake && bA->type != PHYS_BODY_STATIC, activeB = bB->awake && bB->type != PHYS_BODY_STATIC;
        if (!activeA && !activeB) { i++; continue; }
        if (!ph_bp_test_overlap(&w->bp, sA->proxies[c->pa], sB->proxies[c->pb])) { contact_destroy(w, ci); continue; }
        ph_contact_update(w, c);
        i++;
    }
}

/* The fork's trigger pass after SolveTOI (UP!0x180baf000 b2World::Step's last loop): m_contactList from its head
 * (newest first), enabled sensor contacts with an awake non-static side, at the end-of-step poses. */
static void update_sensors(phys_world *w)
{
    for (int32_t ci = w->contact_list; ci >= 0; ci = w->contacts[ci].next) {
        contact_t *c = &w->contacts[ci];
        const shape_t *sA = &w->shapes[c->sa], *sB = &w->shapes[c->sb];
        if (!c->enabled || !(sA->is_trigger || sB->is_trigger)) continue;
        const body_t *bA = &w->bodies[c->ba], *bB = &w->bodies[c->bb];
        bool activeA = bA->awake && bA->type != PHYS_BODY_STATIC, activeB = bB->awake && bB->type != PHYS_BODY_STATIC;
        if (!activeA && !activeB) continue;
        ph_contact_update(w, c);
    }
}

/* ---- fixtures (b2Fixture proxies) -------------------------------------------------------------- */
/* b2Fixture::CreateProxies, one per piece in piece order (Collider2D::CreateFixtures makes one b2Fixture per
 * prepared b2Shape, first to last; a chain's children in order). */
static void fixture_create_proxies(phys_world *w, shape_t *s)
{
    HKSIM_ASSERT(s->n_proxies == 0, "phys: proxies exist");
    xf_t xf = ph_body_xf(&w->bodies[s->body]);
    phys_shape_id sid = (phys_shape_id)(s - w->shapes);
    s->proxies = (int32_t *)realloc(s->proxies, (size_t)(s->n_pieces > 0 ? s->n_pieces : 1) * sizeof *s->proxies);
    HKSIM_ASSERT(s->proxies != NULL, "phys: out of memory (proxies)");
    for (int i = 0; i < s->n_pieces; i++) {
        aabb_t aabb; ph_piece_aabb(&s->pieces[i], xf, &aabb);
        s->proxies[i] = ph_bp_create_proxy(&w->bp, &aabb, sid, i);
    }
    s->n_proxies = s->n_pieces;
}
/* b2Fixture::DestroyProxies.  Polygon pieces are separate b2Fixtures, destroyed last to first
 * (Collider2D::Cleanup UP!0x180c00240); an edge chain is one fixture whose children go in order. */
static void fixture_destroy_proxies(phys_world *w, shape_t *s)
{
    bool chain = s->n_proxies > 0 && s->pieces[0].kind == PC_EDGE;
    for (int k = 0; k < s->n_proxies; k++) ph_bp_destroy_proxy(&w->bp, s->proxies[chain ? k : s->n_proxies - 1 - k]);
    s->n_proxies = 0;
}
/* b2Fixture::Synchronize (b2Fixture.cpp:152-174): the swept AABB of the two transforms */
static void fixture_synchronize(phys_world *w, shape_t *s, xf_t xf1, xf_t xf2)
{
    for (int i = 0; i < s->n_proxies; i++) {
        aabb_t a1, a2; ph_piece_aabb(&s->pieces[i], xf1, &a1); ph_piece_aabb(&s->pieces[i], xf2, &a2);
        aabb_t aabb;
        aabb.lower = V2(ph_min(a1.lower.x, a2.lower.x), ph_min(a1.lower.y, a2.lower.y));
        aabb.upper = V2(ph_max(a1.upper.x, a2.upper.x), ph_max(a1.upper.y, a2.upper.y));
        ph_bp_move_proxy(&w->bp, s->proxies[i], &aabb, v2_sub(xf2.p, xf1.p));
    }
}
/* b2Body::SynchronizeFixtures (UP!0x180baf3a0): from the sweep's start pose to the current transform */
void ph_body_synchronize_fixtures(phys_world *w, body_t *b)
{
    xf_t xf1; xf1.q = ph_rot(b->a0); xf1.p = v2_sub(b->c0, rot_mul(xf1.q, b->lc));
    xf_t xf2 = ph_body_xf(b);
    for (phys_shape_id f = b->fixture_list; f; f = w->shapes[f].next_fixture) fixture_synchronize(w, &w->shapes[f], xf1, xf2);
}
/* b2Body::SetTransform's broad-phase half (UP!0x180bace60): Synchronize(xf, xf) */
static void body_synchronize_at_transform(phys_world *w, body_t *b)
{
    xf_t xf = ph_body_xf(b);
    for (phys_shape_id f = b->fixture_list; f; f = w->shapes[f].next_fixture) fixture_synchronize(w, &w->shapes[f], xf, xf);
}

/* Collider2D::CreateFixtures (UP!0x180c004a0) -> b2Body::CreateFixture (UP!0x180bab800): proxies if the body is
 * active, prepended to the fixture list, e_newFixture; then ResetMassData. */
static void fixture_create(phys_world *w, shape_t *s)
{
    body_t *b = &w->bodies[s->body];
    phys_shape_id sid = (phys_shape_id)(s - w->shapes);
    HKSIM_ASSERT(!s->is_fixture, "phys: shape %u is already a fixture", sid);
    if (b->simulated) fixture_create_proxies(w, s);
    s->next_fixture = b->fixture_list; b->fixture_list = sid;
    s->is_fixture = true;
    ph_body_reset_mass_data(w, b);
    w->new_fixture = true;
}
/* Collider2D::Cleanup's fixture half (UP!0x180c00240) -> b2Body::DestroyFixture (UP!0x180bac010): unlink, destroy
 * its contacts (the body's edge list; EndContact inside the call), then its proxies; then ResetMassData. */
static void fixture_destroy(phys_world *w, shape_t *s)
{
    body_t *b = &w->bodies[s->body];
    phys_shape_id sid = (phys_shape_id)(s - w->shapes);
    HKSIM_ASSERT(s->is_fixture, "phys: shape %u is not a fixture", sid);
    for (phys_shape_id *node = &b->fixture_list; *node; node = &w->shapes[*node].next_fixture)
        if (*node == sid) { *node = s->next_fixture; break; }
    s->next_fixture = 0;
    int32_t e = b->contact_list;
    while (e >= 0) {
        int32_t ci = e >> 1; e = ph_edge_next(w, e);
        const contact_t *c = &w->contacts[ci];
        if (c->sa == sid || c->sb == sid) contact_destroy(w, ci);
    }
    fixture_destroy_proxies(w, s);
    s->is_fixture = false;
    ph_body_reset_mass_data(w, b);
}

/* Collider2D::RecreateCollider (UP!0x180c07050): Cleanup(kColliderRecreate) -- the collider's pairs flagged, its
 * fixtures and contacts destroyed -- then the shapes prepared again and CreateFixtures.  The new b2Contacts come
 * from the next UpdatePairs (e_newFixture) and join the kept records: no Exit or Enter, no warm start
 * (native-physics2d.md §2, E6). */
static void shape_recreate(phys_world *w, shape_t *s)
{
    body_t *b = &w->bodies[s->body];
    if (s->is_fixture) {
        flag_for_recreate(w, (phys_shape_id)(s - w->shapes));
        fixture_destroy(w, s);
        ph_shape_bake(w, s, b);
        fixture_create(w, s);
    } else {
        ph_shape_bake(w, s, b);
        ph_body_reset_mass_data(w, b);
    }
}

/* b2Fixture::Refilter (b2Fixture.cpp:183-218): flag the fixture's contacts, touch its proxies */
static void fixture_refilter(phys_world *w, shape_t *s)
{
    phys_shape_id sid = (phys_shape_id)(s - w->shapes);
    for (int32_t e = w->bodies[s->body].contact_list; e >= 0; e = ph_edge_next(w, e)) {
        contact_t *c = ph_edge_contact(w, e);
        if (c->sa == sid || c->sb == sid) c->filter_flag = true;
    }
    for (int i = 0; i < s->n_proxies; i++) ph_bp_touch_proxy(&w->bp, s->proxies[i]);
}

/* ---- bodies ---------------------------------------------------------------------------------- */
static void nonstatic_add(phys_world *w, body_t *b)
{
    GROW(w->nonstatic, w->n_nonstatic, w->cap_nonstatic, int32_t);
    b->world_index = (int32_t)w->n_nonstatic;
    w->nonstatic[w->n_nonstatic++] = (int32_t)(b - w->bodies);
}
static void nonstatic_remove(phys_world *w, body_t *b)
{
    uint32_t last = w->n_nonstatic - 1;
    w->nonstatic[b->world_index] = w->nonstatic[last];
    w->bodies[w->nonstatic[b->world_index]].world_index = b->world_index;
    w->n_nonstatic--;
    b->world_index = -1;
}

/* b2World::CreateBody (UP!0x180bab560): a non-static body is appended to m_nonStaticBodies, which seeds the islands.
 * Rigidbody2D::Create (UP!0x180c10e30): bullet = Continuous (Q-pphys-13), fixedRotation = FreezeRotation, angle from
 * the Transform; linear / angular damping come from phys_body_set_damping (b2BodyDef default 0). */
phys_body_id phys_body_add(phys_world *w, const phys_body_desc *d)
{
    HKSIM_ASSERT(d != NULL && d->layer < 32, "phys: bad body desc");
    GROW(w->bodies, w->n_bodies, w->cap_bodies, body_t);
    phys_body_id id = w->n_bodies++;
    body_t *b = &w->bodies[id];
    memset(b, 0, sizeof *b);
    b->alive = true; b->simulated = d->simulated;
    b->awake = true;
    b->type = d->type; b->cd = d->cd; b->free_rotation = d->free_rotation;
    b->p = d->position; b->alpha0 = 0.0f;
    b->rot_deg = d->rotation_deg;
    b->a = b->a0 = ph_deg_to_rad(d->rotation_deg);
    b->q = ph_rot(b->a);
    b->scale = d->scale;
    b->v = d->velocity;
    b->gravity_scale = d->gravity_scale;
    HKSIM_ASSERT(d->type != PHYS_BODY_DYNAMIC || d->mass > 0.0f, "phys: dynamic body mass %g", (double)d->mass);
    b->rb_mass = d->mass;
    b->layer = d->layer; b->user = d->user;
    b->lc = V2(0.0f, 0.0f); b->c = b->c0 = b->p;
    b->fixture_list = 0; b->contact_list = -1;
    b->world_index = -1;
    if (d->type != PHYS_BODY_STATIC) nonstatic_add(w, b);
    ph_body_reset_mass_data(w, b);   /* Rigidbody2D::Create (UP!0x180c10e30): CreateBody, then ResetMassData */
    return id;
}

phys_shape_id phys_shape_add(phys_world *w, phys_body_id bid, const phys_shape_desc *d)
{
    body_t *b = body_of(w, bid);
    GROW(w->shapes, w->n_shapes, w->cap_shapes, shape_t);
    phys_shape_id id = w->n_shapes++;
    shape_t *s = &w->shapes[id];
    memset(s, 0, sizeof *s);
    s->alive = true; s->enabled = d->enabled; s->is_trigger = d->is_trigger;
    s->type = d->type; s->body = bid; s->user = d->user; s->iid = d->instance_id;
    HKSIM_ASSERT(d->layer == PHYS_LAYER_INHERIT || d->layer < 32, "phys: shape layer %u", d->layer);
    s->layer = d->layer;
    s->offset = d->offset; s->size = d->size; s->radius = d->radius; s->edge_radius = d->edge_radius;
    s->n_points = d->n_points;
    if (d->n_points) {
        s->points = (v2 *)calloc(d->n_points, sizeof(v2));
        HKSIM_ASSERT(s->points != NULL, "phys: out of memory");
        memcpy(s->points, d->points, d->n_points * sizeof(v2));
    }
    ph_shape_bake(w, s, b);
    if (s->enabled) fixture_create(w, s);        /* a disabled Collider2D has no fixture */
    else ph_body_reset_mass_data(w, b);
    return id;
}

/* Replace a polygon shape's points.  Shapes are baked in the BODY frame, so any move, rotation or rescale of a
 * collider relative to its body comes through here (Unity re-creates the collider when its relative transform
 * changes: PhysicsManager2D::SyncTransforms part 2).  A BOX becomes a POLYGON: a box rotated relative to its body
 * is passed as its four corners. */
void phys_shape_set_points(phys_world *w, phys_shape_id sid, const phys_v2 *pts, uint32_t n)
{
    shape_t *s = shape_of(w, sid);
    HKSIM_ASSERT(s->type == PHYS_SHAPE_POLYGON || s->type == PHYS_SHAPE_BOX || s->type == PHYS_SHAPE_EDGE,
                 "phys: set_points on shape %u of type %d", sid, (int)s->type);
    if (s->type != PHYS_SHAPE_BOX && n == s->n_points && s->points && memcmp(s->points, pts, n * sizeof(v2)) == 0) return;
    if (s->type == PHYS_SHAPE_BOX) { s->type = PHYS_SHAPE_POLYGON; s->offset = V2(0.0f, 0.0f); s->size = V2(0.0f, 0.0f); }
    free(s->points);
    s->points = NULL;
    s->n_points = n;
    if (n) {
        s->points = (v2 *)calloc(n, sizeof(v2));
        HKSIM_ASSERT(s->points != NULL, "phys: out of memory");
        memcpy(s->points, pts, n * sizeof(v2));
    }
    shape_recreate(w, s);
}

/* Collider2D.enabled (Collider2D::SetEnabled UP!0x180c07f70): on, CreateFixtures; off, Cleanup(kColliderDisable) --
 * the fixtures and their contacts destroyed, then ProcessContacts for this collider's pairs, whose reports the
 * caller delivers inside the call (phys_take_exit_events; physics.json#Physics2D.callbacksOnDisable = true). */
void phys_shape_set_enabled(phys_world *w, phys_shape_id sid, bool enabled)
{
    shape_t *s = shape_of(w, sid);
    if (s->enabled == enabled) return;
    s->enabled = enabled;
    if (enabled) { fixture_create(w, s); return; }
    fixture_destroy(w, s);
    process_contacts(w, sid, &w->exits, &w->n_exits, &w->cap_exits);
}

/* BoxCollider2D size / offset, in the body frame.  Any change re-creates the collider (BoxCollider2D::SetSize
 * UP!0x180c16730, Collider2D::SetOffset UP!0x180c082e0). */
void phys_shape_set_box(phys_world *w, phys_shape_id sid, phys_v2 offset, phys_v2 size)
{
    shape_t *s = shape_of(w, sid);
    /* A POLYGON here is a BoxCollider2D that phys_shape_set_points turned into its four corners while
     * its chain to the body was rotated; with the rotation gone it is a box again. */
    HKSIM_ASSERT(s->type == PHYS_SHAPE_BOX || s->type == PHYS_SHAPE_POLYGON, "phys: set_box on shape %u of type %d", sid, (int)s->type);
    if (s->type == PHYS_SHAPE_POLYGON) { s->type = PHYS_SHAPE_BOX; free(s->points); s->points = NULL; s->n_points = 0; }
    else if (s->offset.x == offset.x && s->offset.y == offset.y && s->size.x == size.x && s->size.y == size.y) return;
    s->offset = offset; s->size = size;
    shape_recreate(w, s);
}

/* CircleCollider2D moved or rescaled relative to its body: the centre (body frame, before the body's own scale)
 * and the radius together; a change re-creates the collider (CircleCollider2D::SetRadius UP!0x180c08530). */
void phys_shape_set_circle(phys_world *w, phys_shape_id sid, phys_v2 offset, float radius)
{
    shape_t *s = shape_of(w, sid);
    HKSIM_ASSERT(s->type == PHYS_SHAPE_CIRCLE, "phys: set_circle on non-circle shape %u", sid);
    if (s->radius == radius && s->offset.x == offset.x && s->offset.y == offset.y) return;
    s->radius = radius; s->offset = offset;
    shape_recreate(w, s);
}

/* Collider2D::SetIsTrigger (UP!0x180c08050): a change re-creates the collider; a pair that no longer passes the
 * filter (two kinematic solids) gets no new contact and exits at the next ProcessContacts (docs/engine-lifecycle.md
 * R5, f_trigger_toggle_pair_kind); the trigger set defines the centre of mass (U1). */
void phys_shape_set_trigger(phys_world *w, phys_shape_id sid, bool is_trigger)
{
    shape_t *s = shape_of(w, sid);
    if (s->is_trigger == is_trigger) return;
    s->is_trigger = is_trigger;
    shape_recreate(w, s);
}

phys_v2 phys_body_position(const phys_world *w, phys_body_id b) { return body_ofc(w, b)->p; }
phys_v2 phys_body_velocity(const phys_world *w, phys_body_id b) { return body_ofc(w, b)->v; }
float   phys_body_gravity_scale(const phys_world *w, phys_body_id b) { return body_ofc(w, b)->gravity_scale; }
float   phys_body_scale_x(const phys_world *w, phys_body_id b) { return body_ofc(w, b)->scale.x; }
uint32_t phys_body_user(const phys_world *w, phys_body_id b) { return body_ofc(w, b)->user; }
uint32_t phys_body_layer(const phys_world *w, phys_body_id b) { return body_ofc(w, b)->layer; }
uint32_t phys_shape_user(const phys_world *w, phys_shape_id s)
{
    HKSIM_ASSERT(s != 0 && s < w->n_shapes && w->shapes[s].alive, "phys: bad shape id %u", s);
    return w->shapes[s].user;
}
bool phys_shape_is_trigger(const phys_world *w, phys_shape_id s)
{
    HKSIM_ASSERT(s != 0 && s < w->n_shapes && w->shapes[s].alive, "phys: bad shape id %u", s);
    return w->shapes[s].is_trigger;
}
uint32_t phys_shape_piece_count(const phys_world *w, phys_shape_id s)
{
    HKSIM_ASSERT(s != 0 && s < w->n_shapes && w->shapes[s].alive, "phys: bad shape id %u", s);
    return (uint32_t)w->shapes[s].n_pieces;
}
uint32_t phys_shape_piece_vertices(const phys_world *w, phys_shape_id sid, uint32_t piece, phys_v2 *out, uint32_t cap)
{
    HKSIM_ASSERT(sid != 0 && sid < w->n_shapes && w->shapes[sid].alive, "phys: bad shape id %u", sid);
    const shape_t *s = &w->shapes[sid];
    HKSIM_ASSERT(piece < (uint32_t)s->n_pieces, "phys: bad piece index %u", piece);
    const piece_t *pc = &s->pieces[piece];
    HKSIM_ASSERT(pc->kind == PC_POLYGON, "phys: piece %u is not a polygon", piece);
    uint32_t n = (uint32_t)pc->count < cap ? (uint32_t)pc->count : cap;
    for (uint32_t i = 0; i < n; i++) out[i] = pc->vertices[i];
    return (uint32_t)pc->count;
}

/* b2Body::SetTransform (UP!0x180bace60): xf.p = p; sweep c0 = c = p + R*lc; the fixtures are synchronized in the
 * broad phase at the new transform with no e_newFixture (a teleport's new pairs come from the next UpdatePairs). */
void phys_body_set_position(phys_world *w, phys_body_id bid, phys_v2 p)
{
    body_t *b = body_of(w, bid);
    b->p = p;
    xf_t xf; xf.p = p; xf.q = b->q;
    b->c0 = b->c = xf_mul(xf, b->lc);
    b->alpha0 = 0.0f;
    body_synchronize_at_transform(w, b);
}

void phys_body_set_velocity(phys_world *w, phys_body_id bid, phys_v2 v)
{
    body_t *b = body_of(w, bid);
    if (b->type == PHYS_BODY_STATIC) return;   /* b2Body::SetLinearVelocity (b2Body.h:500-512): static bodies ignore it */
    b->v = v;
}

void phys_body_set_gravity_scale(phys_world *w, phys_body_id bid, float g) { body_of(w, bid)->gravity_scale = g; }
/* b2Body::ApplyForceToCenter(force, wake = true) (b2Body.h:754-770) = Rigidbody2D.AddForce(Force):
 * accumulated, consumed once by island_solve, cleared at the end of phys_step (b2World::ClearForces). */
void phys_body_add_force(phys_world *w, phys_body_id bid, phys_v2 f)
{
    body_t *b = body_of(w, bid);
    if (b->type != PHYS_BODY_DYNAMIC) return;
    b->force = v2_add(b->force, V2(f.x, f.y));
}

/* Rigidbody2D.drag / angularDrag -> b2Body::m_linearDamping / m_angularDamping (Rigidbody2D::SetAngularDrag
 * UP!0x180c157a0 clamps to [0, 1e6]; Rigidbody2D::Create UP!0x180c10e30 copies both into the b2BodyDef) */
void phys_body_set_damping(phys_world *w, phys_body_id bid, float linear, float angular)
{
    body_t *b = body_of(w, bid);
    b->linear_damping = ph_clamp(linear, 0.0f, 1e6f);
    b->angular_damping = ph_clamp(angular, 0.0f, 1e6f);
}
/* Rigidbody2D.AddTorque(torque, mode) (UP!0x180c0ec20): dynamic bodies only; Force accumulates m_torque for the
 * next integration, Impulse adds torque * m_invI to the angular velocity at once.  The torque is not converted
 * to radians. */
void phys_body_add_torque(phys_world *w, phys_body_id bid, float torque, bool impulse)
{
    body_t *b = body_of(w, bid);
    if (b->type != PHYS_BODY_DYNAMIC) return;
    if (impulse) b->w = torque * b->inv_I + b->w;
    else b->torque = torque + b->torque;
}
/* Rigidbody2D.angularVelocity setter (UP!0x180c157d0): degrees/s -> rad/s (* 0.017453292); a static body only
 * warns and a FreezeRotation body keeps 0 */
void phys_body_set_angular_velocity(phys_world *w, phys_body_id bid, float deg_per_s)
{
    body_t *b = body_of(w, bid);
    if (b->type == PHYS_BODY_STATIC || !b->free_rotation) return;
    b->w = deg_per_s * 0.017453292f;
}
/* Rigidbody2D.angularVelocity getter (UP!0x180c11a50): rad/s * 57.29578, 0 on a static body */
float phys_body_angular_velocity(const phys_world *w, phys_body_id bid)
{
    const body_t *b = body_ofc(w, bid);
    return b->type == PHYS_BODY_STATIC ? 0.0f : b->w * 57.29578f;
}
/* Rigidbody2D.inertia getter (UP!0x180c11f30): b2Body::m_I, 0 on a static body */
float phys_body_inertia(const phys_world *w, phys_body_id bid)
{
    const body_t *b = body_ofc(w, bid);
    return b->type == PHYS_BODY_STATIC ? 0.0f : b->I;
}
float phys_body_angle(const phys_world *w, phys_body_id bid) { return body_ofc(w, bid)->a; }
bool phys_body_take_rotation(phys_world *w, phys_body_id bid, float *deg)
{
    body_t *b = body_of(w, bid);
    if (!b->rot_out) return false;
    b->rot_out = false;
    *deg = b->rot_deg;
    return true;
}
bool phys_body_take_writeback(phys_world *w, phys_body_id bid)
{
    body_t *b = body_of(w, bid);
    bool out = b->pos_out;
    b->pos_out = false;
    return out;
}
void phys_body_note_rotation(phys_world *w, phys_body_id bid, float deg) { body_of(w, bid)->rot_deg = deg; }

/* A body scale change re-creates every collider of the body (SyncTransforms part 2: the rigidbody's scale changed,
 * so each collider's relative matrix did; native-physics2d.md §2), in the order the shapes were added. */
static void body_recreate_shapes(phys_world *w, phys_body_id bid)
{
    for (uint32_t i = 1; i < w->n_shapes; i++) {
        shape_t *s = &w->shapes[i];
        if (s->alive && s->body == bid) shape_recreate(w, s);
    }
}

/* transform.localScale.x flip: the colliders are re-created with the mirrored geometry (E6). */
void phys_body_set_scale_x(phys_world *w, phys_body_id bid, float sx)
{
    body_t *b = body_of(w, bid);
    if (b->scale.x == sx) return;
    b->scale.x = sx;
    body_recreate_shapes(w, bid);
}

/* transform.lossyScale of a body's own GameObject, both axes.  Fixtures follow the colliders' world geometry
 * (physics.json#Physics2D.autoSyncTransforms = true), so a scale write on the object or any ancestor changes the
 * body-frame geometry (e.g. Knight/Spells/Scr Heads, lossy -1.4 under the Knight:
 * dumps_all/GG_Gruz_Mother/hierarchy.json.gz). */
void phys_body_set_scale(phys_world *w, phys_body_id bid, phys_v2 sc)
{
    body_t *b = body_of(w, bid);
    if (b->scale.x == sc.x && b->scale.y == sc.y) return;
    b->scale = sc;
    body_recreate_shapes(w, bid);
}

/* transform.rotation / transform.eulerAngles write -> b2Body::SetTransform(p, angle) (UP!0x180bace60):
 * a0 = a = angle, xf.q = b2Rot(angle), c0 = c = b2Mul(xf, localCenter), the fixtures re-synchronized in the
 * broad phase.  The angle is eulerZ * Deg2Rad (Q-pphys-6: Unity's 2*atan2f(qz, qw) of the quaternion differs in
 * the last bits). */
void phys_body_set_rotation(phys_world *w, phys_body_id bid, float deg)
{
    body_t *b = body_of(w, bid);
    if (b->rot_deg == deg) return;
    b->rot_deg = deg;
    b->a = b->a0 = ph_deg_to_rad(deg);
    b->q = ph_rot(b->a);
    xf_t xf; xf.p = b->p; xf.q = b->q;
    b->c0 = b->c = xf_mul(xf, b->lc);
    b->alpha0 = 0.0f;
    body_synchronize_at_transform(w, b);
}

/* Rigidbody2D.simulated -> b2Body::SetActive (UP!0x180bacc60 = b2Body.cpp:454-497): on, create every fixture's
 * proxies (contacts come from the next UpdatePairs); off, destroy them and then every contact of the body, whose
 * pairs exit at the next ProcessContacts (docs/engine-lifecycle.md R5: simulated = false). */
void phys_body_set_simulated(phys_world *w, phys_body_id bid, bool on)
{
    body_t *b = body_of(w, bid);
    if (b->simulated == on) return;
    b->simulated = on;
    if (on) {
        for (phys_shape_id f = b->fixture_list; f; f = w->shapes[f].next_fixture) fixture_create_proxies(w, &w->shapes[f]);
        return;
    }
    for (phys_shape_id f = b->fixture_list; f; f = w->shapes[f].next_fixture) fixture_destroy_proxies(w, &w->shapes[f]);
    while (b->contact_list >= 0) contact_destroy(w, b->contact_list >> 1);
}

/* Rigidbody2D.bodyType (Rigidbody2D::SetBodyType UP!0x180c158a0): every collider of the body re-created (its pairs
 * kept), then b2Body::SetType (UP!0x180bacf60): m_nonStaticBodies membership, mass data, a static body stops,
 * contacts destroyed, proxies touched. */
void phys_body_set_type(phys_world *w, phys_body_id bid, phys_body_type t)
{
    body_t *b = body_of(w, bid);
    if (b->type == t) return;
    body_recreate_shapes(w, bid);
    if (b->type == PHYS_BODY_STATIC) nonstatic_add(w, b);
    b->type = t;
    HKSIM_ASSERT(t != PHYS_BODY_DYNAMIC || b->rb_mass > 0.0f, "phys: dynamic body mass");
    ph_body_reset_mass_data(w, b);
    if (t == PHYS_BODY_STATIC) {   /* UP!0x180bacf60 b2Body::SetType: v = 0, w = 0, c0 = c, a0 = a */
        b->v = V2(0.0f, 0.0f); b->w = 0.0f; b->c0 = b->c; b->a0 = b->a;
        ph_body_synchronize_fixtures(w, b); nonstatic_remove(w, b);
    }
    b->force = V2(0.0f, 0.0f); b->torque = 0.0f;
    while (b->contact_list >= 0) contact_destroy(w, b->contact_list >> 1);
    for (phys_shape_id f = b->fixture_list; f; f = w->shapes[f].next_fixture)
        for (int i = 0; i < w->shapes[f].n_proxies; i++) ph_bp_touch_proxy(&w->bp, w->shapes[f].proxies[i]);
}

/* gameObject.layer write: every fixture of the body refilters (b2Fixture::Refilter) */
void phys_body_set_layer(phys_world *w, phys_body_id bid, uint32_t layer)
{
    HKSIM_ASSERT(layer < 32, "phys: layer %u", layer);
    body_t *b = body_of(w, bid);
    if (b->layer == layer) return;
    b->layer = layer;
    for (phys_shape_id f = b->fixture_list; f; f = w->shapes[f].next_fixture) fixture_refilter(w, &w->shapes[f]);
}

void phys_shape_set_layer(phys_world *w, phys_shape_id sid, uint32_t layer)
{
    shape_t *s = shape_of(w, sid);
    HKSIM_ASSERT(layer == PHYS_LAYER_INHERIT || layer < 32, "phys: shape layer %u", layer);
    if (s->layer == layer) return;
    s->layer = layer;
    if (s->is_fixture) fixture_refilter(w, s);
}

uint32_t phys_shape_layer(const phys_world *w, phys_shape_id sid)
{
    HKSIM_ASSERT(sid != 0 && sid < w->n_shapes && w->shapes[sid].alive, "phys: bad shape id %u", sid);
    return ph_shape_layer(w, &w->shapes[sid]);
}

uint32_t phys_events(const phys_world *w, const phys_event **out) { *out = w->events; return w->n_events; }

/* ---- step (b2World::Step UP!0x180baf000, then PhysicsManager2D::Simulate's ProcessContacts) ---------------- */
void phys_step(phys_world *w, float dt)
{
    HKSIM_ASSERT(dt > 0.0f, "phys: dt");
    HKSIM_ASSERT(w->n_exits == 0, "phys: %u callbacks of a disable were never delivered (phys_take_exit_events)", w->n_exits);
    /* new fixtures since the last step get their pairs before Collide (e_newFixture, set by CreateFixture); the
     * other UpdatePairs calls end Solve and each TOI event */
    if (w->new_fixture) { ph_find_new_contacts(w); w->new_fixture = false; }
    for (uint32_t i = 0; i < w->n_nonstatic; i++) { body_t *b = &w->bodies[w->nonstatic[i]]; b->a_step = b->a; }
    collide_array(w, 0);                 /* Collide(m_contactsNonTOI), then Collide(m_contactsTOI): start-of-step poses */
    collide_array(w, 1);
    float inv_dt = 1.0f / dt;
    float dt_ratio = w->inv_dt0 * dt;
    ph_solve_islands(w, dt, dt_ratio);   /* integrate, solve, SynchronizeFixtures, UpdatePairs */
    ph_solve_toi(w, dt);                 /* continuous, bullets only */
    w->inv_dt0 = inv_dt;
    /* b2World::ClearForces (m_flags & e_clearForces): a force applied from an FSM tick acts on exactly one
     * integration. */
    for (uint32_t i = 1; i < w->n_bodies; i++) { w->bodies[i].force = V2(0.0f, 0.0f); w->bodies[i].torque = 0.0f; }
    /* PhysicsManager2D::Simulate's write-back (UP!0x180beb390, native-physics2d.md §3.2): a body the step turned
     * hands its angle to its Transform as the z euler (Quaternion.eulerAngles, [0, 360)) */
    for (uint32_t i = 0; i < w->n_nonstatic; i++) {
        body_t *b = &w->bodies[w->nonstatic[i]];
        b->pos_out = b->simulated && b->awake;
        if (b->a == b->a_step) continue;
        float deg = fmodf(b->a * 57.29578f, 360.0f);
        if (deg < 0.0f) deg += 360.0f;
        b->rot_deg = deg; b->rot_out = true;
    }
    update_sensors(w);                   /* the fork's trigger pass on the end-of-step poses */
    w->n_events = 0;
    process_contacts(w, 0, &w->events, &w->n_events, &w->cap_events);   /* PhysicsContacts2D::ProcessContacts(null) */
}


/* ---- queries --------------------------------------------------------------------------------- */
static bool shape_queryable(const phys_world *w, const shape_t *s, uint32_t layer_mask)
{
    if (!s->alive || !s->enabled) return false;
    const body_t *b = &w->bodies[s->body];
    if (!b->alive || !b->simulated) return false;
    return ((layer_mask >> ph_shape_layer(w, s)) & 1u) != 0;
}

/* Physics2D.Raycast: closest hit over the shapes in the mask (triggers included:
 * physics.json#Physics2D.queriesHitTriggers = true); first shape id wins a tie (Q-pphys-9). */
bool phys_raycast(const phys_world *w, phys_v2 origin, phys_v2 dir, float length, uint32_t layer_mask,
                  phys_v2 *hit_point, phys_v2 *hit_normal, phys_shape_id *hit_shape)
{
    v2 d = dir; v2_normalize(&d);
    v2 p2 = v2_add(origin, v2_scale(length, d));
    float best = 1.0f; bool hit = false; v2 bn = V2(0.0f, 0.0f); phys_shape_id bs = 0;
    for (uint32_t i = 1; i < w->n_shapes; i++) {
        const shape_t *s = &w->shapes[i];
        if (!shape_queryable(w, s, layer_mask)) continue;
        xf_t xf = ph_body_xf(&w->bodies[s->body]);
        for (int k = 0; k < s->n_pieces; k++) {
            float fr; v2 n;
            if (ph_piece_raycast(&s->pieces[k], xf, origin, p2, best, &fr, &n) && fr < best) { best = fr; bn = n; bs = i; hit = true; }
        }
    }
    if (hit) {
        if (hit_point) *hit_point = v2_add(origin, v2_scale(best, v2_sub(p2, origin)));
        if (hit_normal) *hit_normal = bn;
        if (hit_shape) *hit_shape = bs;
    }
    return hit;
}

bool phys_overlap_any(const phys_world *w, phys_shape_id sid, uint32_t layer_mask)
{
    HKSIM_ASSERT(sid != 0 && sid < w->n_shapes && w->shapes[sid].alive, "phys: bad shape id %u", sid);
    const shape_t *s = &w->shapes[sid];
    xf_t xf = ph_body_xf(&w->bodies[s->body]);
    for (uint32_t i = 1; i < w->n_shapes; i++) {
        const shape_t *o = &w->shapes[i];
        if (i == sid || o->body == s->body || !shape_queryable(w, o, layer_mask)) continue;
        xf_t xo = ph_body_xf(&w->bodies[o->body]);
        for (int a = 0; a < s->n_pieces; a++)
            for (int b = 0; b < o->n_pieces; b++)
                if (ph_test_overlap(&s->pieces[a], xf, &o->pieces[b], xo)) return true;
    }
    return false;
}

/* Physics2D.BoxCast (angle 0): a shape overlapping the box at its start is ignored (queriesStartInColliders =
 * false, Q-pphys-9); otherwise the swept box's time of impact (same TOI as the solver) gives the first hit.
 * Only hit/no-hit and the collider identity are load-bearing for the callers (HeroController thunk checks). */
bool phys_boxcast(const phys_world *w, phys_v2 origin, phys_v2 size, phys_v2 dir, float length, uint32_t layer_mask,
                  phys_v2 *hit_point, phys_v2 *hit_normal, phys_shape_id *hit_shape)
{
    piece_t box; memset(&box, 0, sizeof box);
    float hx = 0.5f * size.x, hy = 0.5f * size.y;
    v2 c[4] = { V2(-hx, -hy), V2(hx, -hy), V2(hx, hy), V2(-hx, hy) };
    box.kind = PC_POLYGON; box.count = 4; box.radius = 0.0f;
    for (int i = 0; i < 4; i++) box.vertices[i] = c[i];
    for (int i = 0; i < 4; i++) { v2 e = v2_sub(c[i + 1 < 4 ? i + 1 : 0], c[i]); box.normals[i] = v2_cross_vs(e, 1.0f); v2_normalize(&box.normals[i]); }
    v2 d = dir; v2_normalize(&d);
    v2 end = v2_add(origin, v2_scale(length, d));
    xf_t xf0; xf0.p = origin; xf0.q = rot_identity();
    float best = 1.0f; bool hit = false; phys_shape_id bs = 0; v2 bp = V2(0.0f, 0.0f), bn = V2(0.0f, 0.0f);
    for (uint32_t i = 1; i < w->n_shapes; i++) {
        const shape_t *s = &w->shapes[i];
        if (!shape_queryable(w, s, layer_mask)) continue;
        const body_t *b = &w->bodies[s->body];
        xf_t xfs = ph_body_xf(b);
        for (int k = 0; k < s->n_pieces; k++) {
            const piece_t *pc = &s->pieces[k];
            if (ph_test_overlap(&box, xf0, pc, xfs)) continue;
            toi_input in;
            ph_proxy_set(&in.proxyA, &box); ph_proxy_set(&in.proxyB, pc);
            in.sweepA.c0 = origin; in.sweepA.c = end; in.sweepA.alpha0 = 0.0f; in.sweepA.a0 = in.sweepA.a = 0.0f; in.sweepA.localCenter = V2(0.0f, 0.0f);
            in.sweepB.c0 = b->c; in.sweepB.c = b->c; in.sweepB.alpha0 = 0.0f; in.sweepB.a0 = in.sweepB.a = b->a; in.sweepB.localCenter = b->lc;
            in.tMax = 1.0f;
            toi_output out;
            ph_time_of_impact(&out, &in);
            if (out.state != TOI_TOUCHING || out.t >= best) continue;
            best = out.t; bs = i; hit = true;
            dist_input di; di.proxyA = in.proxyA; di.proxyB = in.proxyB; di.use_radii = true;
            ph_sweep_get_transform(&in.sweepA, &di.xfA, out.t); di.xfB = xfs;
            simplex_cache cache; cache.count = 0; dist_output dout;
            ph_distance(&dout, &cache, &di);
            bp = dout.pointB; bn = v2_sub(dout.pointA, dout.pointB); v2_normalize(&bn);
        }
    }
    if (hit) { if (hit_point) *hit_point = bp; if (hit_normal) *hit_normal = bn; if (hit_shape) *hit_shape = bs; }
    return hit;
}

/* Physics2D.OverlapBox / OverlapCircle: is any collider in `layer_mask` overlapping the query shape
 * (b2TestOverlap per fixture, as phys_overlap_any).  Triggers count: physics.json#Physics2D
 * .queriesHitTriggers = true.  The query box is a b2PolygonShape::SetAsBox(hx, hy, center, angle)
 * (upstream/box2d-v2.3.1 Box2D/Collision/Shapes/b2PolygonShape.cpp:44-73) and so carries the default
 * skin m_radius = b2_polygonRadius (b2PolygonShape.h:87-90); whether Unity's native query overrides that
 * skin is not in any source.  Returns the first overlapping shape in creation order (0 = none). */
static phys_shape_id overlap_piece(const phys_world *w, const piece_t *q, uint32_t layer_mask)
{
    xf_t xq; xq.p = V2(0.0f, 0.0f); xq.q = rot_identity();
    for (uint32_t i = 1; i < w->n_shapes; i++) {
        const shape_t *o = &w->shapes[i];
        if (!shape_queryable(w, o, layer_mask)) continue;
        xf_t xo = ph_body_xf(&w->bodies[o->body]);
        for (int b = 0; b < o->n_pieces; b++)
            if (ph_test_overlap(q, xq, &o->pieces[b], xo)) return i;
    }
    return 0;
}
phys_shape_id phys_overlap_box(const phys_world *w, phys_v2 center, phys_v2 size, float angle_deg, uint32_t layer_mask)
{
    HKSIM_ASSERT(size.x > 0.0f && size.y > 0.0f, "phys: OverlapBox size (%g,%g)", (double)size.x, (double)size.y);
    piece_t box; memset(&box, 0, sizeof box);
    float hx = 0.5f * size.x, hy = 0.5f * size.y;
    v2 c[4] = { V2(-hx, -hy), V2(hx, -hy), V2(hx, hy), V2(-hx, hy) };
    xf_t xf; xf.p = center;
    if (angle_deg == 0.0f) xf.q = rot_identity();
    else { float rad = angle_deg * 0.017453292f; xf.q.s = sinf(rad); xf.q.c = cosf(rad); }   /* Mathf.Deg2Rad */
    box.kind = PC_POLYGON; box.count = 4; box.radius = PH_POLYGON_RADIUS;
    for (int i = 0; i < 4; i++) box.vertices[i] = xf_mul(xf, c[i]);
    for (int i = 0; i < 4; i++) { v2 e = v2_sub(c[i + 1 < 4 ? i + 1 : 0], c[i]); v2 n = v2_cross_vs(e, 1.0f); v2_normalize(&n); box.normals[i] = rot_mul(xf.q, n); }
    return overlap_piece(w, &box, layer_mask);
}
phys_shape_id phys_overlap_circle(const phys_world *w, phys_v2 center, float radius, uint32_t layer_mask)
{
    piece_t cp; memset(&cp, 0, sizeof cp);
    cp.kind = PC_CIRCLE; cp.center = center; cp.radius = radius;
    return overlap_piece(w, &cp, layer_mask);
}
