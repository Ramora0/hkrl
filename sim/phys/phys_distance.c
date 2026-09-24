/* GJK distance (b2Distance) and time of impact (b2TimeOfImpact), Box2D-2.3 structure.
 * Pinned by analysis/specs/port-phys.md#E4: target = max(slop, totalRadius - 3*slop), tolerance = slop/4,
 * bisection/secant root finder and the sweep Advance below. */
#include "phys_internal.h"
#include <string.h>

void ph_proxy_set(dist_proxy *p, const piece_t *pc)
{
    switch (pc->kind) {
    case PC_POLYGON:
        p->count = pc->count;
        for (int i = 0; i < pc->count; i++) p->vertices[i] = pc->vertices[i];
        p->radius = pc->radius;
        break;
    case PC_CIRCLE:
        p->vertices[0] = pc->center; p->count = 1; p->radius = pc->radius;
        break;
    default:
        p->vertices[0] = pc->e1; p->vertices[1] = pc->e2; p->count = 2; p->radius = pc->radius;
        break;
    }
}

static int proxy_support(const dist_proxy *p, v2 d)
{
    int best = 0;
    float bv = v2_dot(p->vertices[0], d);
    for (int i = 1; i < p->count; i++) {
        float value = v2_dot(p->vertices[i], d);
        if (value > bv) { best = i; bv = value; }
    }
    return best;
}

/* b2Sweep::GetTransform (UP!0x180b9fe80; box2d-v2.3.1 Common/b2Math.h:692-700): p = (1-beta)*c0 + beta*c,
 * q = b2Rot((1-beta)*a0 + beta*a), then shifted by the local centre. */
void ph_sweep_get_transform(const sweep_t *s, xf_t *xf, float beta)
{
    xf->p = v2_add(v2_scale(1.0f - beta, s->c0), v2_scale(beta, s->c));
    xf->q = ph_rot((1.0f - beta) * s->a0 + beta * s->a);
    xf->p = v2_sub(xf->p, rot_mul(xf->q, s->localCenter));
}

/* b2Sweep::Advance (box2d-v2.3.1 Common/b2Math.h:702-709) */
void ph_sweep_advance(sweep_t *s, float alpha)
{
    HKSIM_ASSERT(s->alpha0 < 1.0f, "phys: sweep advance with alpha0 >= 1");
    float beta = (alpha - s->alpha0) / (1.0f - s->alpha0);
    s->c0 = v2_add(s->c0, v2_scale(beta, v2_sub(s->c, s->c0)));
    s->a0 = s->a0 + beta * (s->a - s->a0);
    s->alpha0 = alpha;
}

/* b2Sweep::Normalize (b2Math.h:712-718) as b2TimeOfImpact inlines it (UP!0x180ba46c0): a0 into [0, 2pi), a shifted by the same */
static void sweep_normalize(sweep_t *s)
{
    float d = floorf(s->a0 * 0.15915494f);
    s->a0 = s->a0 - d * 6.2831855f;
    s->a = s->a - d * 6.2831855f;
}

/* ---- simplex (b2Simplex) --------------------------------------------------------------------- */
typedef struct { v2 wA, wB, w; float a; int indexA, indexB; } sv_t;
typedef struct { sv_t v[3]; int count; } simplex_t;

static float simplex_metric(const simplex_t *s)
{
    switch (s->count) {
    case 1: return 0.0f;
    case 2: return v2_len(v2_sub(s->v[0].w, s->v[1].w));
    case 3: return v2_cross(v2_sub(s->v[1].w, s->v[0].w), v2_sub(s->v[2].w, s->v[0].w));
    default: HKSIM_ASSERT(false, "phys: simplex count"); return 0.0f;
    }
}

static void simplex_read_cache(simplex_t *s, const simplex_cache *cache, const dist_proxy *pA, xf_t xfA,
                               const dist_proxy *pB, xf_t xfB)
{
    HKSIM_ASSERT(cache->count <= 3, "phys: simplex cache count");
    s->count = cache->count;
    for (int i = 0; i < s->count; i++) {
        sv_t *v = &s->v[i];
        v->indexA = cache->indexA[i]; v->indexB = cache->indexB[i];
        v2 wALocal = pA->vertices[v->indexA], wBLocal = pB->vertices[v->indexB];
        v->wA = xf_mul(xfA, wALocal); v->wB = xf_mul(xfB, wBLocal);
        v->w = v2_sub(v->wB, v->wA);
        v->a = 0.0f;
    }
    if (s->count > 1) {
        float metric1 = cache->metric, metric2 = simplex_metric(s);
        if (metric2 < 0.5f * metric1 || 2.0f * metric1 < metric2 || metric2 < PH_EPSILON) s->count = 0;
    }
    if (s->count == 0) {
        sv_t *v = &s->v[0];
        v->indexA = 0; v->indexB = 0;
        v->wA = xf_mul(xfA, pA->vertices[0]); v->wB = xf_mul(xfB, pB->vertices[0]);
        v->w = v2_sub(v->wB, v->wA);
        v->a = 1.0f;
        s->count = 1;
    }
}

static void simplex_write_cache(const simplex_t *s, simplex_cache *cache)
{
    cache->metric = simplex_metric(s);
    cache->count = (uint16_t)s->count;
    for (int i = 0; i < s->count; i++) { cache->indexA[i] = (uint8_t)s->v[i].indexA; cache->indexB[i] = (uint8_t)s->v[i].indexB; }
}

static v2 simplex_search_direction(const simplex_t *s)
{
    switch (s->count) {
    case 1: return v2_neg(s->v[0].w);
    case 2: {
        v2 e12 = v2_sub(s->v[1].w, s->v[0].w);
        float sgn = v2_cross(e12, v2_neg(s->v[0].w));
        if (sgn > 0.0f) return v2_cross_sv(1.0f, e12);   /* origin is left of e12 */
        return v2_cross_vs(e12, 1.0f);
    }
    default: HKSIM_ASSERT(false, "phys: search direction"); return V2(0.0f, 0.0f);
    }
}

static v2 simplex_closest_point(const simplex_t *s)
{
    switch (s->count) {
    case 1: return s->v[0].w;
    case 2: return v2_add(v2_scale(s->v[0].a, s->v[0].w), v2_scale(s->v[1].a, s->v[1].w));
    case 3: return V2(0.0f, 0.0f);
    default: HKSIM_ASSERT(false, "phys: closest point"); return V2(0.0f, 0.0f);
    }
}

static void simplex_witness_points(const simplex_t *s, v2 *pA, v2 *pB)
{
    switch (s->count) {
    case 1: *pA = s->v[0].wA; *pB = s->v[0].wB; break;
    case 2:
        *pA = v2_add(v2_scale(s->v[0].a, s->v[0].wA), v2_scale(s->v[1].a, s->v[1].wA));
        *pB = v2_add(v2_scale(s->v[0].a, s->v[0].wB), v2_scale(s->v[1].a, s->v[1].wB));
        break;
    case 3:
        *pA = v2_add(v2_add(v2_scale(s->v[0].a, s->v[0].wA), v2_scale(s->v[1].a, s->v[1].wA)), v2_scale(s->v[2].a, s->v[2].wA));
        *pB = *pA;
        break;
    default: HKSIM_ASSERT(false, "phys: witness points");
    }
}

static void simplex_solve2(simplex_t *s)
{
    v2 w1 = s->v[0].w, w2 = s->v[1].w;
    v2 e12 = v2_sub(w2, w1);
    float d12_2 = -v2_dot(w1, e12);
    if (d12_2 <= 0.0f) { s->v[0].a = 1.0f; s->count = 1; return; }
    float d12_1 = v2_dot(w2, e12);
    if (d12_1 <= 0.0f) { s->v[1].a = 1.0f; s->count = 1; s->v[0] = s->v[1]; return; }
    float inv_d12 = 1.0f / (d12_1 + d12_2);
    s->v[0].a = d12_1 * inv_d12; s->v[1].a = d12_2 * inv_d12; s->count = 2;
}

static void simplex_solve3(simplex_t *s)
{
    v2 w1 = s->v[0].w, w2 = s->v[1].w, w3 = s->v[2].w;
    v2 e12 = v2_sub(w2, w1);
    float w1e12 = v2_dot(w1, e12), w2e12 = v2_dot(w2, e12);
    float d12_1 = w2e12, d12_2 = -w1e12;
    v2 e13 = v2_sub(w3, w1);
    float w1e13 = v2_dot(w1, e13), w3e13 = v2_dot(w3, e13);
    float d13_1 = w3e13, d13_2 = -w1e13;
    v2 e23 = v2_sub(w3, w2);
    float w2e23 = v2_dot(w2, e23), w3e23 = v2_dot(w3, e23);
    float d23_1 = w3e23, d23_2 = -w2e23;
    float n123 = v2_cross(e12, e13);
    float d123_1 = n123 * v2_cross(w2, w3), d123_2 = n123 * v2_cross(w3, w1), d123_3 = n123 * v2_cross(w1, w2);
    if (d12_2 <= 0.0f && d13_2 <= 0.0f) { s->v[0].a = 1.0f; s->count = 1; return; }
    if (d12_1 > 0.0f && d12_2 > 0.0f && d123_3 <= 0.0f) {
        float inv = 1.0f / (d12_1 + d12_2);
        s->v[0].a = d12_1 * inv; s->v[1].a = d12_2 * inv; s->count = 2; return;
    }
    if (d13_1 > 0.0f && d13_2 > 0.0f && d123_2 <= 0.0f) {
        float inv = 1.0f / (d13_1 + d13_2);
        s->v[0].a = d13_1 * inv; s->v[2].a = d13_2 * inv; s->count = 2; s->v[1] = s->v[2]; return;
    }
    if (d12_1 <= 0.0f && d23_2 <= 0.0f) { s->v[1].a = 1.0f; s->count = 1; s->v[0] = s->v[1]; return; }
    if (d13_1 <= 0.0f && d23_1 <= 0.0f) { s->v[2].a = 1.0f; s->count = 1; s->v[0] = s->v[2]; return; }
    if (d23_1 > 0.0f && d23_2 > 0.0f && d123_1 <= 0.0f) {
        float inv = 1.0f / (d23_1 + d23_2);
        s->v[1].a = d23_1 * inv; s->v[2].a = d23_2 * inv; s->count = 2; s->v[0] = s->v[2]; return;
    }
    float inv_d123 = 1.0f / (d123_1 + d123_2 + d123_3);
    s->v[0].a = d123_1 * inv_d123; s->v[1].a = d123_2 * inv_d123; s->v[2].a = d123_3 * inv_d123;
    s->count = 3;
}

void ph_distance(dist_output *out, simplex_cache *cache, const dist_input *in)
{
    const dist_proxy *pA = &in->proxyA, *pB = &in->proxyB;
    xf_t xfA = in->xfA, xfB = in->xfB;
    simplex_t simplex;
    simplex_read_cache(&simplex, cache, pA, xfA, pB, xfB);
    const int k_maxIters = 20;
    int saveA[3], saveB[3], saveCount = 0;
    float distanceSqr1 = FLT_MAX, distanceSqr2 = distanceSqr1;
    int iter = 0;
    while (iter < k_maxIters) {
        saveCount = simplex.count;
        for (int i = 0; i < saveCount; i++) { saveA[i] = simplex.v[i].indexA; saveB[i] = simplex.v[i].indexB; }
        switch (simplex.count) {
        case 1: break;
        case 2: simplex_solve2(&simplex); break;
        case 3: simplex_solve3(&simplex); break;
        default: HKSIM_ASSERT(false, "phys: simplex count");
        }
        if (simplex.count == 3) break;
        v2 p = simplex_closest_point(&simplex);
        distanceSqr2 = v2_len_sq(p);
        /* (b2Distance 2.3: the "ensure progress" break is commented out) */
        distanceSqr1 = distanceSqr2;
        v2 d = simplex_search_direction(&simplex);
        if (v2_len_sq(d) < PH_EPSILON * PH_EPSILON) break;
        sv_t *vertex = &simplex.v[simplex.count];
        vertex->indexA = proxy_support(pA, rot_mulT(xfA.q, v2_neg(d)));
        vertex->wA = xf_mul(xfA, pA->vertices[vertex->indexA]);
        vertex->indexB = proxy_support(pB, rot_mulT(xfB.q, d));
        vertex->wB = xf_mul(xfB, pB->vertices[vertex->indexB]);
        vertex->w = v2_sub(vertex->wB, vertex->wA);
        ++iter;
        bool duplicate = false;
        for (int i = 0; i < saveCount; i++)
            if (vertex->indexA == saveA[i] && vertex->indexB == saveB[i]) { duplicate = true; break; }
        if (duplicate) break;
        ++simplex.count;
    }
    (void)distanceSqr1;
    simplex_witness_points(&simplex, &out->pointA, &out->pointB);
    out->distance = v2_len(v2_sub(out->pointA, out->pointB));
    out->iterations = iter;
    simplex_write_cache(&simplex, cache);
    if (in->use_radii) {
        float rA = pA->radius, rB = pB->radius;
        if (out->distance > rA + rB && out->distance > PH_EPSILON) {
            out->distance -= rA + rB;
            v2 normal = v2_sub(out->pointB, out->pointA);
            v2_normalize(&normal);
            out->pointA = v2_add(out->pointA, v2_scale(rA, normal));
            out->pointB = v2_sub(out->pointB, v2_scale(rB, normal));
        } else {
            v2 p = v2_scale(0.5f, v2_add(out->pointA, out->pointB));
            out->pointA = p; out->pointB = p; out->distance = 0.0f;
        }
    }
}

/* ---- separation function (b2SeparationFunction) ---------------------------------------------- */
typedef enum { SF_POINTS, SF_FACE_A, SF_FACE_B } sf_type;
typedef struct { const dist_proxy *proxyA, *proxyB; sweep_t sweepA, sweepB; sf_type type; v2 localPoint, axis; } sepfn_t;

static float sepfn_init(sepfn_t *f, const simplex_cache *cache, const dist_proxy *pA, const sweep_t *sA,
                        const dist_proxy *pB, const sweep_t *sB, float t1)
{
    f->proxyA = pA; f->proxyB = pB;
    int count = cache->count;
    HKSIM_ASSERT(0 < count && count < 3, "phys: separation function cache count %d", count);
    f->sweepA = *sA; f->sweepB = *sB;
    xf_t xfA, xfB;
    ph_sweep_get_transform(&f->sweepA, &xfA, t1);
    ph_sweep_get_transform(&f->sweepB, &xfB, t1);
    if (count == 1) {
        f->type = SF_POINTS;
        v2 localPointA = pA->vertices[cache->indexA[0]], localPointB = pB->vertices[cache->indexB[0]];
        v2 pointA = xf_mul(xfA, localPointA), pointB = xf_mul(xfB, localPointB);
        f->axis = v2_sub(pointB, pointA);
        float s = v2_normalize(&f->axis);
        return s;
    } else if (cache->indexA[0] == cache->indexA[1]) {
        f->type = SF_FACE_B;
        v2 localPointB1 = pB->vertices[cache->indexB[0]], localPointB2 = pB->vertices[cache->indexB[1]];
        f->axis = v2_cross_vs(v2_sub(localPointB2, localPointB1), 1.0f);
        v2_normalize(&f->axis);
        v2 normal = rot_mul(xfB.q, f->axis);
        f->localPoint = v2_scale(0.5f, v2_add(localPointB1, localPointB2));
        v2 pointB = xf_mul(xfB, f->localPoint);
        v2 localPointA = pA->vertices[cache->indexA[0]];
        v2 pointA = xf_mul(xfA, localPointA);
        float s = v2_dot(v2_sub(pointA, pointB), normal);
        if (s < 0.0f) { f->axis = v2_neg(f->axis); s = -s; }
        return s;
    } else {
        f->type = SF_FACE_A;
        v2 localPointA1 = pA->vertices[cache->indexA[0]], localPointA2 = pA->vertices[cache->indexA[1]];
        f->axis = v2_cross_vs(v2_sub(localPointA2, localPointA1), 1.0f);
        v2_normalize(&f->axis);
        v2 normal = rot_mul(xfA.q, f->axis);
        f->localPoint = v2_scale(0.5f, v2_add(localPointA1, localPointA2));
        v2 pointA = xf_mul(xfA, f->localPoint);
        v2 localPointB = pB->vertices[cache->indexB[0]];
        v2 pointB = xf_mul(xfB, localPointB);
        float s = v2_dot(v2_sub(pointB, pointA), normal);
        if (s < 0.0f) { f->axis = v2_neg(f->axis); s = -s; }
        return s;
    }
}

static float sepfn_find_min(const sepfn_t *f, int *indexA, int *indexB, float t)
{
    xf_t xfA, xfB;
    ph_sweep_get_transform(&f->sweepA, &xfA, t);
    ph_sweep_get_transform(&f->sweepB, &xfB, t);
    switch (f->type) {
    case SF_POINTS: {
        v2 axisA = rot_mulT(xfA.q, f->axis), axisB = rot_mulT(xfB.q, v2_neg(f->axis));
        *indexA = proxy_support(f->proxyA, axisA); *indexB = proxy_support(f->proxyB, axisB);
        v2 pointA = xf_mul(xfA, f->proxyA->vertices[*indexA]), pointB = xf_mul(xfB, f->proxyB->vertices[*indexB]);
        return v2_dot(v2_sub(pointB, pointA), f->axis);
    }
    case SF_FACE_A: {
        v2 normal = rot_mul(xfA.q, f->axis), pointA = xf_mul(xfA, f->localPoint);
        v2 axisB = rot_mulT(xfB.q, v2_neg(normal));
        *indexA = -1; *indexB = proxy_support(f->proxyB, axisB);
        v2 pointB = xf_mul(xfB, f->proxyB->vertices[*indexB]);
        return v2_dot(v2_sub(pointB, pointA), normal);
    }
    default: {
        v2 normal = rot_mul(xfB.q, f->axis), pointB = xf_mul(xfB, f->localPoint);
        v2 axisA = rot_mulT(xfA.q, v2_neg(normal));
        *indexB = -1; *indexA = proxy_support(f->proxyA, axisA);
        v2 pointA = xf_mul(xfA, f->proxyA->vertices[*indexA]);
        return v2_dot(v2_sub(pointA, pointB), normal);
    }
    }
}

static float sepfn_evaluate(const sepfn_t *f, int indexA, int indexB, float t)
{
    xf_t xfA, xfB;
    ph_sweep_get_transform(&f->sweepA, &xfA, t);
    ph_sweep_get_transform(&f->sweepB, &xfB, t);
    switch (f->type) {
    case SF_POINTS: {
        v2 pointA = xf_mul(xfA, f->proxyA->vertices[indexA]), pointB = xf_mul(xfB, f->proxyB->vertices[indexB]);
        return v2_dot(v2_sub(pointB, pointA), f->axis);
    }
    case SF_FACE_A: {
        v2 normal = rot_mul(xfA.q, f->axis), pointA = xf_mul(xfA, f->localPoint);
        v2 pointB = xf_mul(xfB, f->proxyB->vertices[indexB]);
        return v2_dot(v2_sub(pointB, pointA), normal);
    }
    default: {
        v2 normal = rot_mul(xfB.q, f->axis), pointB = xf_mul(xfB, f->localPoint);
        v2 pointA = xf_mul(xfA, f->proxyA->vertices[indexA]);
        return v2_dot(v2_sub(pointA, pointB), normal);
    }
    }
}

void ph_time_of_impact(toi_output *out, const toi_input *in)
{
    out->state = TOI_UNKNOWN; out->t = in->tMax;
    const dist_proxy *pA = &in->proxyA, *pB = &in->proxyB;
    sweep_t sweepA = in->sweepA, sweepB = in->sweepB;
    sweep_normalize(&sweepA); sweep_normalize(&sweepB);
    float tMax = in->tMax;
    float totalRadius = pA->radius + pB->radius;
    float target = ph_max(PH_LINEAR_SLOP, totalRadius - 3.0f * PH_LINEAR_SLOP);   /* E4 */
    float tolerance = 0.25f * PH_LINEAR_SLOP;
    float t1 = 0.0f;
    const int k_maxIterations = 20;
    int iter = 0;
    simplex_cache cache; cache.count = 0;
    dist_input di; di.proxyA = *pA; di.proxyB = *pB; di.use_radii = false;
    for (;;) {
        xf_t xfA, xfB;
        ph_sweep_get_transform(&sweepA, &xfA, t1);
        ph_sweep_get_transform(&sweepB, &xfB, t1);
        di.xfA = xfA; di.xfB = xfB;
        dist_output dout;
        ph_distance(&dout, &cache, &di);
        if (dout.distance <= 0.0f) { out->state = TOI_OVERLAPPED; out->t = 0.0f; break; }
        if (dout.distance < target + tolerance) { out->state = TOI_TOUCHING; out->t = t1; break; }
        sepfn_t fcn;
        sepfn_init(&fcn, &cache, pA, &sweepA, pB, &sweepB, t1);
        bool done = false;
        float t2 = tMax;
        int pushBackIter = 0;
        for (;;) {
            int indexA, indexB;
            float s2 = sepfn_find_min(&fcn, &indexA, &indexB, t2);
            if (s2 > target + tolerance) { out->state = TOI_SEPARATED; out->t = tMax; done = true; break; }
            if (s2 > target - tolerance) { t1 = t2; break; }
            float s1 = sepfn_evaluate(&fcn, indexA, indexB, t1);
            if (s1 < target - tolerance) { out->state = TOI_FAILED; out->t = t1; done = true; break; }
            if (s1 <= target + tolerance) { out->state = TOI_TOUCHING; out->t = t1; done = true; break; }
            int rootIterCount = 0;
            float a1 = t1, a2 = t2;
            for (;;) {
                float t;
                if (rootIterCount & 1) t = a1 + (target - s1) * (a2 - a1) / (s2 - s1);
                else t = 0.5f * (a1 + a2);
                ++rootIterCount;
                float s = sepfn_evaluate(&fcn, indexA, indexB, t);
                if (fabsf(s - target) < tolerance) { t2 = t; break; }
                if (s > target) { a1 = t; s1 = s; } else { a2 = t; s2 = s; }
                if (rootIterCount == 50) break;
            }
            ++pushBackIter;
            if (pushBackIter == PH_MAX_POLY_VERTS) break;
        }
        ++iter;
        if (done) break;
        if (iter == k_maxIterations) { out->state = TOI_FAILED; out->t = t1; break; }
    }
}
