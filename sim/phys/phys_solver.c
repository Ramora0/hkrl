/* Islands, contact solver (velocity / position / TOI-position) and the continuous (TOI) pass: b2World::Solve
 * (UP!0x180bad930), b2Island::Solve (UP!0x180bad210), b2ContactSolver (UP!0x180bb07a0..0x180bb3220) and
 * b2World::SolveTOI (UP!0x180bae240) of Unity's fork, which match Box2D 2.3.1 operation for operation apart from the
 * deltas cited below (analysis/native_specs/native-box2d.md §9-§11).  Measured: analysis/specs/port-phys.md E1 (the
 * centre of mass c is integrated; p = c - lc), E2 (skin radii + slop), E3 (Baumgarte position solver, early-out at
 * -3*slop, 2-point sequential corrections), E4 (TOI: 20 position iterations at 0.75, early-out at -1.5*slop, no warm
 * start, impulses not stored), E6 (inexact face normal). */
#include "phys_internal.h"
#include <stdlib.h>
#include <string.h>
#include "core/alloc.h"   /* per-instance arena */

typedef struct { v2 rA, rB; float normalImpulse, tangentImpulse, normalMass, tangentMass, velocityBias; } vcp_t;
typedef struct {
    vcp_t points[2]; v2 normal; float normalMass[4], K[4];   /* b2Mat22 ex.x, ex.y, ey.x, ey.y */
    float friction, restitution, tangentSpeed;
    int indexA, indexB; float invMassA, invMassB, invIA, invIB; int pointCount; contact_t *contact;
} vc_t;
typedef struct {
    v2 localPoints[2], localNormal, localPoint; int indexA, indexB; float invMassA, invMassB;
    v2 localCenterA, localCenterB; float invIA, invIB; manifold_type type; float radiusA, radiusB; int pointCount;
} pc_t;
typedef struct { v2 c; float a; } position_t;
typedef struct { v2 v; float w; } velocity_t;

typedef struct {
    phys_world *w;
    body_t **bodies; int nb;
    contact_t **contacts; int nc;
    position_t *pos; velocity_t *vel;
    vc_t *vcs; pc_t *pcs;
} island_t;

static sweep_t body_sweep(const body_t *b)
{
    sweep_t s; s.c0 = b->c0; s.c = b->c; s.localCenter = b->lc; s.a0 = b->a0; s.a = b->a; s.alpha0 = b->alpha0; return s;
}
static void body_from_sweep(body_t *b, const sweep_t *s) { b->c0 = s->c0; b->c = s->c; b->a0 = s->a0; b->a = s->a; b->alpha0 = s->alpha0; }

/* ---- b2ContactSolver ------------------------------------------------------------------------- */
static void solver_init(island_t *is, float dt_ratio, bool warm_starting)
{
    phys_world *w = is->w;
    for (int i = 0; i < is->nc; i++) {
        contact_t *c = is->contacts[i];
        const shape_t *sA = &w->shapes[c->sa], *sB = &w->shapes[c->sb];
        const piece_t *pcA = &sA->pieces[c->pa], *pcB = &sB->pieces[c->pb];
        body_t *bA = &w->bodies[c->ba], *bB = &w->bodies[c->bb];
        manifold_t *m = &c->m;
        int pointCount = m->point_count;
        HKSIM_ASSERT(pointCount > 0, "phys: solver contact without points");
        vc_t *vc = &is->vcs[i];
        memset(vc, 0, sizeof *vc);
        vc->friction = c->friction; vc->restitution = c->restitution; vc->tangentSpeed = 0.0f;
        vc->indexA = bA->island_index; vc->indexB = bB->island_index;
        vc->invMassA = bA->inv_mass; vc->invMassB = bB->inv_mass;
        vc->invIA = bA->inv_I; vc->invIB = bB->inv_I;
        vc->contact = c; vc->pointCount = pointCount;
        vc->normal = V2(0.0f, 0.0f);
        pc_t *pc = &is->pcs[i];
        memset(pc, 0, sizeof *pc);
        pc->indexA = vc->indexA; pc->indexB = vc->indexB;
        pc->invMassA = bA->inv_mass; pc->invMassB = bB->inv_mass;
        pc->localCenterA = bA->lc; pc->localCenterB = bB->lc;
        pc->invIA = bA->inv_I; pc->invIB = bB->inv_I;
        pc->localNormal = m->local_normal; pc->localPoint = m->local_point;
        pc->pointCount = pointCount; pc->radiusA = pcA->radius; pc->radiusB = pcB->radius; pc->type = m->type;
        for (int j = 0; j < pointCount; j++) {
            manifold_point *cp = &m->points[j];
            vcp_t *vcp = &vc->points[j];
            if (warm_starting) { vcp->normalImpulse = dt_ratio * cp->normal_impulse; vcp->tangentImpulse = dt_ratio * cp->tangent_impulse; }
            else { vcp->normalImpulse = 0.0f; vcp->tangentImpulse = 0.0f; }
            vcp->rA = V2(0.0f, 0.0f); vcp->rB = V2(0.0f, 0.0f);
            vcp->normalMass = 0.0f; vcp->tangentMass = 0.0f; vcp->velocityBias = 0.0f;
            pc->localPoints[j] = cp->local_point;
        }
    }
}

static void solver_init_velocity_constraints(island_t *is)
{
    for (int i = 0; i < is->nc; i++) {
        vc_t *vc = &is->vcs[i]; pc_t *pc = &is->pcs[i];
        float radiusA = pc->radiusA, radiusB = pc->radiusB;
        const manifold_t *manifold = &vc->contact->m;
        int indexA = vc->indexA, indexB = vc->indexB;
        float mA = vc->invMassA, mB = vc->invMassB, iA = vc->invIA, iB = vc->invIB;
        v2 localCenterA = pc->localCenterA, localCenterB = pc->localCenterB;
        v2 cA = is->pos[indexA].c, vA = is->vel[indexA].v; float aA = is->pos[indexA].a, wA = is->vel[indexA].w;
        v2 cB = is->pos[indexB].c, vB = is->vel[indexB].v; float aB = is->pos[indexB].a, wB = is->vel[indexB].w;
        HKSIM_ASSERT(manifold->point_count > 0, "phys: init vc without points");
        xf_t xfA, xfB;
        xfA.q = ph_rot(aA); xfB.q = ph_rot(aB);
        xfA.p = v2_sub(cA, rot_mul(xfA.q, localCenterA));
        xfB.p = v2_sub(cB, rot_mul(xfB.q, localCenterB));
        v2 wm_normal, wm_points[2]; float wm_seps[2];
        ph_world_manifold(manifold, xfA, radiusA, xfB, radiusB, &wm_normal, wm_points, wm_seps);
        vc->normal = wm_normal;
        int pointCount = vc->pointCount;
        for (int j = 0; j < pointCount; j++) {
            vcp_t *vcp = &vc->points[j];
            vcp->rA = v2_sub(wm_points[j], cA);
            vcp->rB = v2_sub(wm_points[j], cB);
            float rnA = v2_cross(vcp->rA, vc->normal), rnB = v2_cross(vcp->rB, vc->normal);
            float kNormal = mA + mB + iA * rnA * rnA + iB * rnB * rnB;
            vcp->normalMass = kNormal > 0.0f ? 1.0f / kNormal : 0.0f;
            v2 tangent = v2_cross_vs(vc->normal, 1.0f);
            float rtA = v2_cross(vcp->rA, tangent), rtB = v2_cross(vcp->rB, tangent);
            float kTangent = mA + mB + iA * rtA * rtA + iB * rtB * rtB;
            vcp->tangentMass = kTangent > 0.0f ? 1.0f / kTangent : 0.0f;
            vcp->velocityBias = 0.0f;
            float vRel = v2_dot(vc->normal, v2_sub(v2_sub(v2_add(vB, v2_cross_sv(wB, vcp->rB)), vA), v2_cross_sv(wA, vcp->rA)));
            if (vRel < -PH_VELOCITY_THRESHOLD) vcp->velocityBias = -vc->restitution * vRel;
        }
        if (vc->pointCount == 2) {
            vcp_t *vcp1 = &vc->points[0], *vcp2 = &vc->points[1];
            float rn1A = v2_cross(vcp1->rA, vc->normal), rn1B = v2_cross(vcp1->rB, vc->normal);
            float rn2A = v2_cross(vcp2->rA, vc->normal), rn2B = v2_cross(vcp2->rB, vc->normal);
            float k11 = mA + mB + iA * rn1A * rn1A + iB * rn1B * rn1B;
            float k22 = mA + mB + iA * rn2A * rn2A + iB * rn2B * rn2B;
            float k12 = mA + mB + iA * rn1A * rn2A + iB * rn1B * rn2B;
            const float k_maxConditionNumber = 1000.0f;
            if (k11 * k11 < k_maxConditionNumber * (k11 * k22 - k12 * k12)) {
                /* K and its inverse (b2Mat22::GetInverse, b2Math.h:196-208); Unity's grouping (UP!0x180bb1090) is the
                 * same products and sums. */
                vc->K[0] = k11; vc->K[1] = k12; vc->K[2] = k12; vc->K[3] = k22;
                float det = k11 * k22 - k12 * k12;
                if (det != 0.0f) det = 1.0f / det;
                vc->normalMass[0] = det * k22; vc->normalMass[1] = -det * k12;
                vc->normalMass[2] = -det * k12; vc->normalMass[3] = det * k11;
            } else {
                vc->pointCount = 1;   /* "The constraints are redundant, just use one." */
            }
        }
    }
}

static void solver_warm_start(island_t *is)
{
    for (int i = 0; i < is->nc; i++) {
        vc_t *vc = &is->vcs[i];
        int indexA = vc->indexA, indexB = vc->indexB;
        float mA = vc->invMassA, iA = vc->invIA, mB = vc->invMassB, iB = vc->invIB;
        int pointCount = vc->pointCount;
        v2 vA = is->vel[indexA].v; float wA = is->vel[indexA].w;
        v2 vB = is->vel[indexB].v; float wB = is->vel[indexB].w;
        v2 normal = vc->normal, tangent = v2_cross_vs(normal, 1.0f);
        for (int j = 0; j < pointCount; j++) {
            vcp_t *vcp = &vc->points[j];
            v2 P = v2_add(v2_scale(vcp->normalImpulse, normal), v2_scale(vcp->tangentImpulse, tangent));
            wA -= iA * v2_cross(vcp->rA, P);
            vA = v2_sub(vA, v2_scale(mA, P));
            wB += iB * v2_cross(vcp->rB, P);
            vB = v2_add(vB, v2_scale(mB, P));
        }
        is->vel[indexA].v = vA; is->vel[indexA].w = wA;
        is->vel[indexB].v = vB; is->vel[indexB].w = wB;
    }
}

static void solver_solve_velocity(island_t *is)
{
    for (int i = 0; i < is->nc; i++) {
        vc_t *vc = &is->vcs[i];
        int indexA = vc->indexA, indexB = vc->indexB;
        float mA = vc->invMassA, iA = vc->invIA, mB = vc->invMassB, iB = vc->invIB;
        int pointCount = vc->pointCount;
        v2 vA = is->vel[indexA].v; float wA = is->vel[indexA].w;
        v2 vB = is->vel[indexB].v; float wB = is->vel[indexB].w;
        v2 normal = vc->normal, tangent = v2_cross_vs(normal, 1.0f);
        float friction = vc->friction;
        HKSIM_ASSERT(pointCount == 1 || pointCount == 2, "phys: vc point count");
        /* tangent constraints first (b2ContactSolver: non-penetration is more important than friction) */
        for (int j = 0; j < pointCount; j++) {
            vcp_t *vcp = &vc->points[j];
            v2 dv = v2_sub(v2_sub(v2_add(vB, v2_cross_sv(wB, vcp->rB)), vA), v2_cross_sv(wA, vcp->rA));
            float vt = v2_dot(dv, tangent) - vc->tangentSpeed;
            float lambda = vcp->tangentMass * (-vt);
            float maxFriction = friction * vcp->normalImpulse;
            float newImpulse = ph_clamp(vcp->tangentImpulse + lambda, -maxFriction, maxFriction);
            lambda = newImpulse - vcp->tangentImpulse;
            vcp->tangentImpulse = newImpulse;
            v2 P = v2_scale(lambda, tangent);
            vA = v2_sub(vA, v2_scale(mA, P)); wA -= iA * v2_cross(vcp->rA, P);
            vB = v2_add(vB, v2_scale(mB, P)); wB += iB * v2_cross(vcp->rB, P);
        }
        if (pointCount == 1) {
            vcp_t *vcp = &vc->points[0];
            v2 dv = v2_sub(v2_sub(v2_add(vB, v2_cross_sv(wB, vcp->rB)), vA), v2_cross_sv(wA, vcp->rA));
            float vn = v2_dot(dv, normal);
            float lambda = -vcp->normalMass * (vn - vcp->velocityBias);
            float newImpulse = ph_max(vcp->normalImpulse + lambda, 0.0f);
            lambda = newImpulse - vcp->normalImpulse;
            vcp->normalImpulse = newImpulse;
            v2 P = v2_scale(lambda, normal);
            vA = v2_sub(vA, v2_scale(mA, P)); wA -= iA * v2_cross(vcp->rA, P);
            vB = v2_add(vB, v2_scale(mB, P)); wB += iB * v2_cross(vcp->rB, P);
        } else {
            /* Block solver (box2d-v2.3.1 b2ContactSolver.cpp:388-586 = UP!0x180bb23c0): the total impulse x solves the
             * LCP  vn = A x + b', vn >= 0, x >= 0, vn_i x_i = 0  by trying the four cases in order. */
            vcp_t *cp1 = &vc->points[0], *cp2 = &vc->points[1];
            v2 a = V2(cp1->normalImpulse, cp2->normalImpulse);
            v2 dv1 = v2_sub(v2_sub(v2_add(vB, v2_cross_sv(wB, cp1->rB)), vA), v2_cross_sv(wA, cp1->rA));
            v2 dv2 = v2_sub(v2_sub(v2_add(vB, v2_cross_sv(wB, cp2->rB)), vA), v2_cross_sv(wA, cp2->rA));
            float vn1 = v2_dot(dv1, normal), vn2 = v2_dot(dv2, normal);
            v2 b = V2(vn1 - cp1->velocityBias, vn2 - cp2->velocityBias);
            b = v2_sub(b, V2(vc->K[0] * a.x + vc->K[2] * a.y, vc->K[1] * a.x + vc->K[3] * a.y));   /* b -= K * a */
            v2 x;
            bool solved = true;
            for (;;) {
                /* case 1: vn = 0 */
                x = v2_neg(V2(vc->normalMass[0] * b.x + vc->normalMass[2] * b.y, vc->normalMass[1] * b.x + vc->normalMass[3] * b.y));
                if (x.x >= 0.0f && x.y >= 0.0f) break;
                /* case 2: vn1 = 0, x2 = 0 */
                x.x = -cp1->normalMass * b.x; x.y = 0.0f;
                vn2 = vc->K[1] * x.x + b.y;
                if (x.x >= 0.0f && vn2 >= 0.0f) break;
                /* case 3: vn2 = 0, x1 = 0 */
                x.x = 0.0f; x.y = -cp2->normalMass * b.y;
                vn1 = vc->K[2] * x.y + b.x;
                if (x.y >= 0.0f && vn1 >= 0.0f) break;
                /* case 4: x = 0 */
                x.x = 0.0f; x.y = 0.0f;
                vn1 = b.x; vn2 = b.y;
                if (vn1 >= 0.0f && vn2 >= 0.0f) break;
                solved = false;   /* no solution: this iteration leaves the impulses */
                break;
            }
            if (solved) {
                v2 d = v2_sub(x, a);
                v2 P1 = v2_scale(d.x, normal), P2 = v2_scale(d.y, normal);
                vA = v2_sub(vA, v2_scale(mA, v2_add(P1, P2)));
                wA -= iA * (v2_cross(cp1->rA, P1) + v2_cross(cp2->rA, P2));
                vB = v2_add(vB, v2_scale(mB, v2_add(P1, P2)));
                wB += iB * (v2_cross(cp1->rB, P1) + v2_cross(cp2->rB, P2));
                cp1->normalImpulse = x.x; cp2->normalImpulse = x.y;
            }
        }
        is->vel[indexA].v = vA; is->vel[indexA].w = wA;
        is->vel[indexB].v = vB; is->vel[indexB].w = wB;
    }
}

static void solver_store_impulses(island_t *is)
{
    for (int i = 0; i < is->nc; i++) {
        vc_t *vc = &is->vcs[i];
        manifold_t *m = &vc->contact->m;
        for (int j = 0; j < vc->pointCount; j++) {
            m->points[j].normal_impulse = vc->points[j].normalImpulse;
            m->points[j].tangent_impulse = vc->points[j].tangentImpulse;
        }
    }
}

/* b2PositionSolverManifold::Initialize */
static void psm_init(const pc_t *pc, xf_t xfA, xf_t xfB, int index, v2 *normal, v2 *point, float *separation)
{
    HKSIM_ASSERT(pc->pointCount > 0, "phys: psm without points");
    switch (pc->type) {
    case MT_CIRCLES: {
        v2 pointA = xf_mul(xfA, pc->localPoint), pointB = xf_mul(xfB, pc->localPoints[0]);
        *normal = v2_sub(pointB, pointA); v2_normalize(normal);
        *point = v2_scale(0.5f, v2_add(pointA, pointB));
        *separation = v2_dot(v2_sub(pointB, pointA), *normal) - pc->radiusA - pc->radiusB;
        break;
    }
    case MT_FACE_A: {
        *normal = rot_mul(xfA.q, pc->localNormal);
        v2 planePoint = xf_mul(xfA, pc->localPoint);
        v2 clipPoint = xf_mul(xfB, pc->localPoints[index]);
        *separation = v2_dot(v2_sub(clipPoint, planePoint), *normal) - pc->radiusA - pc->radiusB;
        *point = clipPoint;
        break;
    }
    default: {
        *normal = rot_mul(xfB.q, pc->localNormal);
        v2 planePoint = xf_mul(xfB, pc->localPoint);
        v2 clipPoint = xf_mul(xfA, pc->localPoints[index]);
        *separation = v2_dot(v2_sub(clipPoint, planePoint), *normal) - pc->radiusA - pc->radiusB;
        *point = clipPoint;
        *normal = v2_neg(*normal);
        break;
    }
    }
}

/* toiIndexA/B < 0: discrete solver (all bodies keep their mass, baumgarte 0.2, early-out -3*slop);
 * otherwise the TOI variant (only the two TOI bodies move, 0.75, early-out -1.5*slop).  E3 / E4. */
static bool solver_solve_position(island_t *is, int toiIndexA, int toiIndexB)
{
    bool toi = toiIndexA >= 0;
    float minSeparation = 0.0f;
    for (int i = 0; i < is->nc; i++) {
        pc_t *pc = &is->pcs[i];
        int indexA = pc->indexA, indexB = pc->indexB;
        v2 localCenterA = pc->localCenterA, localCenterB = pc->localCenterB;
        float mA, iA, mB, iB;
        if (toi) {
            mA = 0.0f; iA = 0.0f; if (indexA == toiIndexA || indexA == toiIndexB) { mA = pc->invMassA; iA = pc->invIA; }
            mB = 0.0f; iB = 0.0f; if (indexB == toiIndexA || indexB == toiIndexB) { mB = pc->invMassB; iB = pc->invIB; }
        } else { mA = pc->invMassA; iA = pc->invIA; mB = pc->invMassB; iB = pc->invIB; }
        int pointCount = pc->pointCount;
        v2 cA = is->pos[indexA].c, cB = is->pos[indexB].c;
        float aA = is->pos[indexA].a, aB = is->pos[indexB].a;
        for (int j = 0; j < pointCount; j++) {
            xf_t xfA, xfB;
            xfA.q = ph_rot(aA); xfB.q = ph_rot(aB);
            xfA.p = v2_sub(cA, rot_mul(xfA.q, localCenterA));
            xfB.p = v2_sub(cB, rot_mul(xfB.q, localCenterB));
            v2 normal, point; float separation;
            psm_init(pc, xfA, xfB, j, &normal, &point, &separation);
            v2 rA = v2_sub(point, cA), rB = v2_sub(point, cB);
            minSeparation = ph_min(minSeparation, separation);
            float C = toi ? ph_clamp(PH_TOI_BAUMGARTE * (separation + PH_LINEAR_SLOP), -PH_MAX_LINEAR_CORR, 0.0f)
                          : ph_clamp(PH_BAUMGARTE * (separation + PH_LINEAR_SLOP), -PH_MAX_LINEAR_CORR, 0.0f);
            float rnA = v2_cross(rA, normal), rnB = v2_cross(rB, normal);
            /* Unity's grouping (UP!0x180bb1900 / UP!0x180bb1e40): ((rnA*iA*rnA + mB) + mA) + rnB*iB*rnB */
            float K = ((rnA * iA * rnA + mB) + mA) + rnB * iB * rnB;
            float impulse = K > 0.0f ? -C / K : 0.0f;
            v2 P = v2_scale(impulse, normal);
            cA = v2_sub(cA, v2_scale(mA, P));
            aA -= iA * v2_cross(rA, P);
            cB = v2_add(cB, v2_scale(mB, P));
            aB += iB * v2_cross(rB, P);
        }
        is->pos[indexA].c = cA; is->pos[indexA].a = aA;
        is->pos[indexB].c = cB; is->pos[indexB].a = aB;
    }
    return toi ? minSeparation >= -1.5f * PH_LINEAR_SLOP : minSeparation >= -3.0f * PH_LINEAR_SLOP;
}

static void island_alloc(island_t *is, phys_world *w, int nb_cap, int nc_cap)
{
    is->w = w;
    is->bodies = (body_t **)calloc((size_t)(nb_cap > 0 ? nb_cap : 1), sizeof *is->bodies);
    is->contacts = (contact_t **)calloc((size_t)(nc_cap > 0 ? nc_cap : 1), sizeof *is->contacts);
    is->pos = (position_t *)calloc((size_t)(nb_cap > 0 ? nb_cap : 1), sizeof *is->pos);
    is->vel = (velocity_t *)calloc((size_t)(nb_cap > 0 ? nb_cap : 1), sizeof *is->vel);
    is->vcs = (vc_t *)calloc((size_t)(nc_cap > 0 ? nc_cap : 1), sizeof *is->vcs);
    is->pcs = (pc_t *)calloc((size_t)(nc_cap > 0 ? nc_cap : 1), sizeof *is->pcs);
    is->nb = 0; is->nc = 0;
}

static void island_free(island_t *is)
{
    free(is->bodies); free(is->contacts); free(is->pos); free(is->vel); free(is->vcs); free(is->pcs);
}

static void island_add_body(island_t *is, body_t *b) { b->island_index = is->nb; is->bodies[is->nb++] = b; }

/* b2Island::Solve / SolveTOI position integration (UP!0x180bad210, UP!0x180bade00): the velocity capped at
 * b2_maxTranslation and b2_maxRotation per step, then c += h*v, a += h*w */
static void integrate_position(position_t *pos, velocity_t *vel, float h)
{
    v2 c = pos->c, v = vel->v; float a = pos->a, wv = vel->w;
    v2 translation = v2_scale(h, v);
    if (v2_dot(translation, translation) > PH_MAX_TRANSLATION * PH_MAX_TRANSLATION) {
        float ratio = PH_MAX_TRANSLATION / v2_len(translation);
        v = v2_scale(ratio, v);
    }
    float rotation = wv * h;
    if (PH_MAX_ROTATION * PH_MAX_ROTATION < rotation * rotation) wv = wv * (PH_MAX_ROTATION / fabsf(rotation));
    c = v2_add(c, v2_scale(h, v));
    a = wv * h + a;
    pos->c = c; pos->a = a; vel->v = v; vel->w = wv;
}

/* b2Island::Solve */
static void island_solve(island_t *is, float h, float dt_ratio, v2 gravity, uint32_t vel_iters, uint32_t pos_iters)
{
    for (int i = 0; i < is->nb; i++) {
        body_t *b = is->bodies[i];
        v2 c = b->c, v = b->v; float a = b->a, wv = b->w;
        b->c0 = b->c; b->a0 = b->a;         /* sweep.c0 = sweep.c, a0 = a */
        if (b->type == PHYS_BODY_DYNAMIC) {
            /* UP!0x180bad210: v = (v + (gravityScale*g + invMass*f)*h) * (1/(h*drag + 1)),
             * w = (h*invI*torque + w) * (1/(h*angularDrag + 1)); analysis/specs/port-phys.md#E1 */
            v2 force = b->force;      /* b2Body::m_force; zero unless ApplyForceToCenter ran this step */
            v = v2_add(v, v2_scale(h, v2_add(v2_scale(b->gravity_scale, gravity), v2_scale(b->inv_mass, force))));
            v = v2_scale(1.0f / (h * b->linear_damping + 1.0f), v);
            wv = (h * b->inv_I * b->torque + wv) * (1.0f / (h * b->angular_damping + 1.0f));
        }
        is->pos[i].c = c; is->pos[i].a = a; is->vel[i].v = v; is->vel[i].w = wv;
    }
    solver_init(is, dt_ratio, true);
    solver_init_velocity_constraints(is);
    solver_warm_start(is);
    for (uint32_t i = 0; i < vel_iters; i++) solver_solve_velocity(is);
    solver_store_impulses(is);
    for (int i = 0; i < is->nb; i++) integrate_position(&is->pos[i], &is->vel[i], h);
    for (uint32_t i = 0; i < pos_iters; i++) {
        bool contactsOkay = solver_solve_position(is, -1, -1);
        if (contactsOkay) break;
    }
    for (int i = 0; i < is->nb; i++) {   /* UP!0x180bad210 copies the non-static bodies back */
        body_t *b = is->bodies[i];
        if (b->type == PHYS_BODY_STATIC) continue;
        b->c = is->pos[i].c; b->a = is->pos[i].a; b->v = is->vel[i].v; b->w = is->vel[i].w;
        ph_body_sync_transform(b);
    }
}

/* b2Island::SolveTOI */
static void island_solve_toi(island_t *is, float h, int toiIndexA, int toiIndexB, uint32_t vel_iters)
{
    for (int i = 0; i < is->nb; i++) { body_t *b = is->bodies[i]; is->pos[i].c = b->c; is->pos[i].a = b->a; is->vel[i].v = b->v; is->vel[i].w = b->w; }
    solver_init(is, 1.0f, false);
    for (int i = 0; i < 20; i++) {                      /* subStep.positionIterations = 20 (E4) */
        bool contactsOkay = solver_solve_position(is, toiIndexA, toiIndexB);
        if (contactsOkay) break;
    }
    is->bodies[toiIndexA]->c0 = is->pos[toiIndexA].c;   /* leap of faith to new safe state */
    is->bodies[toiIndexA]->a0 = is->pos[toiIndexA].a;
    is->bodies[toiIndexB]->c0 = is->pos[toiIndexB].c;
    is->bodies[toiIndexB]->a0 = is->pos[toiIndexB].a;
    solver_init_velocity_constraints(is);
    for (uint32_t i = 0; i < vel_iters; i++) solver_solve_velocity(is);
    /* TOI contact impulses are not stored for warm starting (E4/E6: the discrete frame after a TOI frame is fresh) */
    for (int i = 0; i < is->nb; i++) {
        integrate_position(&is->pos[i], &is->vel[i], h);
        body_t *b = is->bodies[i];
        b->c = is->pos[i].c; b->a = is->pos[i].a; b->v = is->vel[i].v; b->w = is->vel[i].w;
        ph_body_sync_transform(b);
    }
}

/* b2World::Solve (UP!0x180bad930): islands seeded in m_nonStaticBodies array order (awake and active bodies not yet
 * in an island), a stack DFS over each body's contact edges (newest first) taking enabled, touching, non-sensor
 * contacts with a dynamic side; then every non-static body that was in an island synchronizes its fixtures, in
 * array order, and the broad phase reports new pairs. */
void ph_solve_islands(phys_world *w, float dt, float dt_ratio)
{
    for (uint32_t i = 1; i < w->n_bodies; i++) w->bodies[i].island_flag = false;
    for (int32_t ci = w->contact_list; ci >= 0; ci = w->contacts[ci].next) w->contacts[ci].island_flag = false;
    int nb_cap = (int)w->n_bodies + 1, nc_cap = (int)w->contact_count + 1;
    island_t is; island_alloc(&is, w, nb_cap, nc_cap);
    body_t **stack = (body_t **)calloc((size_t)nb_cap, sizeof *stack);
    for (uint32_t si = 0; si < w->n_nonstatic; si++) {
        body_t *seed = &w->bodies[w->nonstatic[si]];
        if (seed->island_flag) continue;
        if (!seed->awake || !seed->simulated) continue;
        is.nb = 0; is.nc = 0;
        int stackCount = 0;
        stack[stackCount++] = seed; seed->island_flag = true;
        while (stackCount > 0) {
            body_t *b = stack[--stackCount];
            island_add_body(&is, b);
            if (b->type == PHYS_BODY_STATIC) continue;
            for (int32_t e = b->contact_list; e >= 0; e = ph_edge_next(w, e)) {
                contact_t *c = ph_edge_contact(w, e);
                if (c->island_flag) continue;
                if (!c->enabled || !c->touching) continue;
                if (w->shapes[c->sa].is_trigger || w->shapes[c->sb].is_trigger) continue;
                if (w->bodies[c->ba].type != PHYS_BODY_DYNAMIC && w->bodies[c->bb].type != PHYS_BODY_DYNAMIC) continue;
                is.contacts[is.nc++] = c; c->island_flag = true;
                body_t *other = &w->bodies[ph_edge_other(w, e)];
                if (other->island_flag) continue;
                stack[stackCount++] = other; other->island_flag = true;
            }
        }
        island_solve(&is, dt, dt_ratio, w->gravity, w->vel_iters, w->pos_iters);
        for (int i = 0; i < is.nb; i++) if (is.bodies[i]->type == PHYS_BODY_STATIC) is.bodies[i]->island_flag = false;
    }
    free(stack);
    island_free(&is);
    for (uint32_t bi = 0; bi < w->n_nonstatic; bi++) {
        body_t *b = &w->bodies[w->nonstatic[bi]];
        if (b->island_flag) ph_body_synchronize_fixtures(w, b);
    }
    ph_find_new_contacts(w);
}

/* b2Body::Advance (UP!0x180bab250): sweep.Advance(t); c = c0, a = a0; xf synchronised */
static void body_advance(body_t *b, float alpha)
{
    sweep_t s = body_sweep(b);
    ph_sweep_advance(&s, alpha);
    body_from_sweep(b, &s);
    b->c = b->c0; b->a = b->a0;
    ph_body_sync_transform(b);
}
/* b2World::SolveTOI's backup restore: m_sweep = backup; SynchronizeTransform */
static void body_restore_sweep(body_t *b, const body_t *backup)
{
    b->c0 = backup->c0; b->c = backup->c; b->a0 = backup->a0; b->a = backup->a; b->alpha0 = backup->alpha0;
    ph_body_sync_transform(b);
}

/* b2World::SolveTOI (UP!0x180bae240 = b2World.cpp:577-895 except the candidate scan): the minimum TOI is searched
 * over m_contactsTOI in array order (strict <, so the first wins a tie), and a candidate needs a bullet
 * (Continuous) body on either side, even against a static body (native-box2d.md §5.3; docs/engine-lifecycle.md R5:
 * a Discrete body tunnels). */
void ph_solve_toi(phys_world *w, float dt)
{
    int nb_cap = 2 * PH_MAX_SUBSTEPS + 2, nc_cap = 2 * PH_MAX_TOI_CONTACTS + 2;
    island_t is; island_alloc(&is, w, nb_cap, nc_cap);
    if (w->step_complete) {
        for (uint32_t i = 1; i < w->n_bodies; i++) { w->bodies[i].island_flag = false; w->bodies[i].alpha0 = 0.0f; }
        for (int32_t ci = w->contact_list; ci >= 0; ci = w->contacts[ci].next) {
            contact_t *c = &w->contacts[ci];
            c->toi_flag = false; c->island_flag = false; c->toi_count = 0; c->toi = 1.0f;
        }
    }
    for (;;) {
        contact_t *minContact = NULL;
        float minAlpha = 1.0f;
        for (uint32_t ti = 0; ti < w->n_carr[1]; ti++) {
            contact_t *c = &w->contacts[w->carr[1][ti]];
            if (!c->enabled) continue;
            if (c->toi_count > PH_MAX_SUBSTEPS) continue;
            float alpha = 1.0f;
            if (c->toi_flag) alpha = c->toi;
            else {
                const shape_t *fA = &w->shapes[c->sa], *fB = &w->shapes[c->sb];
                if (fA->is_trigger || fB->is_trigger) continue;
                body_t *bA = &w->bodies[c->ba], *bB = &w->bodies[c->bb];
                bool activeA = bA->awake && bA->type != PHYS_BODY_STATIC, activeB = bB->awake && bB->type != PHYS_BODY_STATIC;
                if (!activeA && !activeB) continue;
                if (bA->cd != PHYS_CD_CONTINUOUS && bB->cd != PHYS_CD_CONTINUOUS) continue;   /* asm 0x180bae4da-0x180bae4e8 */
                float alpha0 = bA->alpha0;
                if (bA->alpha0 < bB->alpha0) {
                    alpha0 = bB->alpha0;
                    sweep_t s = body_sweep(bA); ph_sweep_advance(&s, alpha0); body_from_sweep(bA, &s);
                } else if (bB->alpha0 < bA->alpha0) {
                    alpha0 = bA->alpha0;
                    sweep_t s = body_sweep(bB); ph_sweep_advance(&s, alpha0); body_from_sweep(bB, &s);
                }
                HKSIM_ASSERT(alpha0 < 1.0f, "phys: TOI alpha0");
                toi_input in;
                ph_proxy_set(&in.proxyA, &fA->pieces[c->pa]); ph_proxy_set(&in.proxyB, &fB->pieces[c->pb]);
                in.sweepA = body_sweep(bA); in.sweepB = body_sweep(bB);
                in.tMax = 1.0f;
                toi_output out;
                ph_time_of_impact(&out, &in);
                float beta = out.t;
                if (out.state == TOI_TOUCHING) alpha = ph_min(alpha0 + (1.0f - alpha0) * beta, 1.0f);
                else alpha = 1.0f;
                c->toi = alpha; c->toi_flag = true;
            }
            if (alpha < minAlpha) { minContact = c; minAlpha = alpha; }
        }
        if (minContact == NULL || 1.0f - 10.0f * PH_EPSILON < minAlpha) { w->step_complete = true; break; }
        body_t *bA = &w->bodies[minContact->ba], *bB = &w->bodies[minContact->bb];
        body_t backup1 = *bA, backup2 = *bB;
        body_advance(bA, minAlpha);
        body_advance(bB, minAlpha);
        ph_contact_update(w, minContact);
        minContact->toi_flag = false;
        ++minContact->toi_count;
        if (!minContact->enabled || !minContact->touching) {
            minContact->enabled = false;
            body_restore_sweep(bA, &backup1);
            body_restore_sweep(bB, &backup2);
            continue;
        }
        is.nb = 0; is.nc = 0;
        island_add_body(&is, bA); island_add_body(&is, bB);
        is.contacts[is.nc++] = minContact;
        bA->island_flag = true; bB->island_flag = true; minContact->island_flag = true;
        body_t *bodies[2] = { bA, bB };
        for (int i = 0; i < 2; i++) {
            body_t *body = bodies[i];
            if (body->type != PHYS_BODY_DYNAMIC) continue;
            for (int32_t e = body->contact_list; e >= 0; e = ph_edge_next(w, e)) {
                if (is.nb == nb_cap) break;
                if (is.nc == nc_cap) break;
                contact_t *contact = ph_edge_contact(w, e);
                if (contact->island_flag) continue;
                body_t *other = &w->bodies[ph_edge_other(w, e)];
                if (other->type == PHYS_BODY_DYNAMIC && body->cd != PHYS_CD_CONTINUOUS && other->cd != PHYS_CD_CONTINUOUS) continue;
                if (w->shapes[contact->sa].is_trigger || w->shapes[contact->sb].is_trigger) continue;
                body_t backup = *other;
                if (!other->island_flag) body_advance(other, minAlpha);
                ph_contact_update(w, contact);
                if (!contact->enabled || !contact->touching) {
                    body_restore_sweep(other, &backup);
                    continue;
                }
                contact->island_flag = true;
                is.contacts[is.nc++] = contact;
                if (other->island_flag) continue;
                other->island_flag = true;
                island_add_body(&is, other);
            }
        }
        float sub_dt = (1.0f - minAlpha) * dt;
        island_solve_toi(&is, sub_dt, bA->island_index, bB->island_index, w->vel_iters);
        for (int i = 0; i < is.nb; i++) {                 /* :866-883 */
            body_t *body = is.bodies[i];
            body->island_flag = false;
            if (body->type != PHYS_BODY_DYNAMIC) continue;
            ph_body_synchronize_fixtures(w, body);
            for (int32_t e = body->contact_list; e >= 0; e = ph_edge_next(w, e)) {
                contact_t *c = ph_edge_contact(w, e);
                c->toi_flag = false; c->island_flag = false;
            }
        }
        ph_find_new_contacts(w);
    }
    island_free(&is);
}
