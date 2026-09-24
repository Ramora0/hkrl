/* Narrow phase: manifolds (b2CollidePolygons, Unity's b2CollideRadialPolygons, b2CollideCircles,
 * b2CollidePolygonAndCircle), sensor overlap (b2TestOverlap) and per-shape raycasts, as the fork has them
 * (analysis/native_specs/native-box2d.md §7).  Polygon-polygon is pinned by analysis/specs/port-phys.md#E2/E3/E5/E6;
 * the other manifolds carry Q-pphys-4/7 (no trace contact). */
#include "phys_internal.h"
#include <string.h>

typedef struct { v2 v; contact_id id; } clip_vertex;

/* b2FindMaxSeparation (2.3.1 form): max over poly1 edges of the min over poly2 vertices. */
static float find_max_separation(int *edgeIndex, const piece_t *poly1, xf_t xf1, const piece_t *poly2, xf_t xf2)
{
    int count1 = poly1->count, count2 = poly2->count;
    xf_t xf = xf_mulT_xx(xf2, xf1);
    int bestIndex = 0;
    float maxSeparation = -FLT_MAX;
    for (int i = 0; i < count1; i++) {
        v2 n = rot_mul(xf.q, poly1->normals[i]);
        v2 v1 = xf_mul(xf, poly1->vertices[i]);
        float si = FLT_MAX;
        for (int j = 0; j < count2; j++) {
            float sij = v2_dot(n, v2_sub(poly2->vertices[j], v1));
            if (sij < si) si = sij;
        }
        if (si > maxSeparation) { maxSeparation = si; bestIndex = i; }
    }
    *edgeIndex = bestIndex;
    return maxSeparation;
}

static void find_incident_edge(clip_vertex c[2], const piece_t *poly1, xf_t xf1, int edge1, const piece_t *poly2, xf_t xf2)
{
    HKSIM_ASSERT(0 <= edge1 && edge1 < poly1->count, "phys: incident edge index");
    v2 normal1 = rot_mulT(xf2.q, rot_mul(xf1.q, poly1->normals[edge1]));
    int index = 0;
    float minDot = FLT_MAX;
    for (int i = 0; i < poly2->count; i++) {
        float dot = v2_dot(normal1, poly2->normals[i]);
        if (dot < minDot) { minDot = dot; index = i; }
    }
    int i1 = index, i2 = i1 + 1 < poly2->count ? i1 + 1 : 0;
    c[0].v = xf_mul(xf2, poly2->vertices[i1]);
    c[0].id.cf.indexA = (uint8_t)edge1; c[0].id.cf.indexB = (uint8_t)i1; c[0].id.cf.typeA = CF_FACE; c[0].id.cf.typeB = CF_VERTEX;
    c[1].v = xf_mul(xf2, poly2->vertices[i2]);
    c[1].id.cf.indexA = (uint8_t)edge1; c[1].id.cf.indexB = (uint8_t)i2; c[1].id.cf.typeA = CF_FACE; c[1].id.cf.typeB = CF_VERTEX;
}

static int clip_segment_to_line(clip_vertex vOut[2], const clip_vertex vIn[2], v2 normal, float offset, int vertexIndexA)
{
    int numOut = 0;
    float distance0 = v2_dot(normal, vIn[0].v) - offset;
    float distance1 = v2_dot(normal, vIn[1].v) - offset;
    if (distance0 <= 0.0f) vOut[numOut++] = vIn[0];
    if (distance1 <= 0.0f) vOut[numOut++] = vIn[1];
    if (distance0 * distance1 < 0.0f) {
        float interp = distance0 / (distance0 - distance1);
        vOut[numOut].v = v2_add(vIn[0].v, v2_scale(interp, v2_sub(vIn[1].v, vIn[0].v)));
        vOut[numOut].id.cf.indexA = (uint8_t)vertexIndexA;
        vOut[numOut].id.cf.indexB = vIn[0].id.cf.indexB;
        vOut[numOut].id.cf.typeA = CF_VERTEX;
        vOut[numOut].id.cf.typeB = CF_FACE;
        ++numOut;
    }
    return numOut;
}

static void collide_polygons(manifold_t *manifold, const piece_t *polyA, xf_t xfA, const piece_t *polyB, xf_t xfB)
{
    manifold->point_count = 0;
    float totalRadius = polyA->radius + polyB->radius;
    int edgeA = 0;
    float separationA = find_max_separation(&edgeA, polyA, xfA, polyB, xfB);
    if (separationA > totalRadius) return;
    int edgeB = 0;
    float separationB = find_max_separation(&edgeB, polyB, xfB, polyA, xfA);
    if (separationB > totalRadius) return;
    const piece_t *poly1, *poly2; xf_t xf1, xf2; int edge1; bool flip;
    const float k_tol = 0.1f * PH_LINEAR_SLOP;
    if (separationB > separationA + k_tol) {
        poly1 = polyB; poly2 = polyA; xf1 = xfB; xf2 = xfA; edge1 = edgeB; manifold->type = MT_FACE_B; flip = true;
    } else {
        poly1 = polyA; poly2 = polyB; xf1 = xfA; xf2 = xfB; edge1 = edgeA; manifold->type = MT_FACE_A; flip = false;
    }
    clip_vertex incidentEdge[2];
    find_incident_edge(incidentEdge, poly1, xf1, edge1, poly2, xf2);
    int count1 = poly1->count;
    int iv1 = edge1, iv2 = edge1 + 1 < count1 ? edge1 + 1 : 0;
    v2 v11 = poly1->vertices[iv1], v12 = poly1->vertices[iv2];
    v2 localTangent = v2_sub(v12, v11);
    v2_normalize(&localTangent);
    v2 localNormal = v2_cross_vs(localTangent, 1.0f);
    v2 planePoint = v2_scale(0.5f, v2_add(v11, v12));
    v2 tangent = rot_mul(xf1.q, localTangent);
    v2 normal = v2_cross_vs(tangent, 1.0f);
    v11 = xf_mul(xf1, v11); v12 = xf_mul(xf1, v12);
    float frontOffset = v2_dot(normal, v11);
    float sideOffset1 = -v2_dot(tangent, v11) + totalRadius;
    float sideOffset2 = v2_dot(tangent, v12) + totalRadius;
    clip_vertex clipPoints1[2], clipPoints2[2];
    int np = clip_segment_to_line(clipPoints1, incidentEdge, v2_neg(tangent), sideOffset1, iv1);
    if (np < 2) return;
    np = clip_segment_to_line(clipPoints2, clipPoints1, tangent, sideOffset2, iv2);
    if (np < 2) return;
    manifold->local_normal = localNormal;
    manifold->local_point = planePoint;
    int pointCount = 0;
    for (int i = 0; i < PH_MAX_MANIFOLD_PTS; i++) {
        float separation = v2_dot(normal, clipPoints2[i].v) - frontOffset;
        if (separation <= totalRadius) {
            manifold_point *cp = &manifold->points[pointCount];
            cp->local_point = xf_mulT(xf2, clipPoints2[i].v);
            cp->id = clipPoints2[i].id;
            if (flip) {
                contact_id cf = cp->id;
                cp->id.cf.indexA = cf.cf.indexB; cp->id.cf.indexB = cf.cf.indexA;
                cp->id.cf.typeA = cf.cf.typeB; cp->id.cf.typeB = cf.cf.typeA;
            }
            ++pointCount;
        }
    }
    manifold->point_count = pointCount;
}

/* b2CollideRadialPolygons (UP!0x180ba2a60 [b2CollidePolygon.cpp:246-407]), Unity's routine for a polygon pair of
 * which one has a radius above the contact offset (the Knight's box: 0.01 + edgeRadius 0.0025).  The prologue is
 * b2CollidePolygons' (both max separations, the 0.0005 reference-face tolerance, the incident edge); the side
 * planes are the reference face's own ends, without the radius, and a clip that leaves fewer than two points, or no
 * point within the radii, falls back to a one-point circles manifold between the nearest reference vertex and its
 * nearer incident vertex (native-box2d.md §7.2). */
static void collide_radial_polygons(manifold_t *manifold, const piece_t *polyA, xf_t xfA, const piece_t *polyB, xf_t xfB)
{
    manifold->point_count = 0;
    float totalRadius = polyA->radius + polyB->radius;
    int edgeA = 0;
    float separationA = find_max_separation(&edgeA, polyA, xfA, polyB, xfB);
    if (separationA > totalRadius) return;
    int edgeB = 0;
    float separationB = find_max_separation(&edgeB, polyB, xfB, polyA, xfA);
    if (separationB > totalRadius) return;
    const piece_t *poly1, *poly2; xf_t xf1, xf2; int edge1;
    bool refA = separationB <= separationA + 0.1f * PH_LINEAR_SLOP;
    if (refA) { poly1 = polyA; poly2 = polyB; xf1 = xfA; xf2 = xfB; edge1 = edgeA; manifold->type = MT_FACE_A; }
    else      { poly1 = polyB; poly2 = polyA; xf1 = xfB; xf2 = xfA; edge1 = edgeB; manifold->type = MT_FACE_B; }
    clip_vertex incidentEdge[2];
    find_incident_edge(incidentEdge, poly1, xf1, edge1, poly2, xf2);
    int count1 = poly1->count;
    int iv1 = edge1, iv2 = edge1 + 1 < count1 ? edge1 + 1 : 0;
    v2 v11 = poly1->vertices[iv1], v12 = poly1->vertices[iv2];
    v2 localTangent = v2_sub(v12, v11);
    v2_normalize(&localTangent);
    v2 localNormal = v2_cross_vs(localTangent, 1.0f);
    v2 planePoint = v2_scale(0.5f, v2_add(v11, v12));
    v2 tangent = rot_mul(xf1.q, localTangent);
    v2 normal = v2_cross_vs(tangent, 1.0f);
    v2 w11 = xf_mul(xf1, v11), w12 = xf_mul(xf1, v12);
    float frontOffset = v2_dot(normal, w11);
    float sideOffset1 = -v2_dot(tangent, w11);
    float sideOffset2 = v2_dot(tangent, w12);
    clip_vertex clipPoints1[2], clipPoints2[2];
    int np = clip_segment_to_line(clipPoints1, incidentEdge, v2_neg(tangent), sideOffset1, iv1);
    if (np == 2) np = clip_segment_to_line(clipPoints2, clipPoints1, tangent, sideOffset2, iv2);
    if (np == 2) {
        manifold->local_normal = localNormal;
        manifold->local_point = planePoint;
        int pointCount = 0;
        for (int i = 0; i < PH_MAX_MANIFOLD_PTS; i++) {
            float separation = v2_dot(normal, clipPoints2[i].v) - frontOffset;
            if (separation <= totalRadius) {
                manifold_point *cp = &manifold->points[pointCount];
                cp->local_point = xf_mulT(xf2, clipPoints2[i].v);
                cp->id = clipPoints2[i].id;
                if (!refA) {
                    contact_id cf = cp->id;
                    cp->id.cf.indexA = cf.cf.indexB; cp->id.cf.indexB = cf.cf.indexA;
                    cp->id.cf.typeA = cf.cf.typeB; cp->id.cf.typeB = cf.cf.typeA;
                }
                ++pointCount;
            }
        }
        manifold->point_count = pointCount;
        if (pointCount != 0) return;
    }
    /* vertex-vertex: the squared distances of the incident clip vertices to both reference vertices */
    v2 i0 = incidentEdge[0].v, i1 = incidentEdge[1].v;
    float d011 = v2_len_sq(v2_sub(i0, w11)), d012 = v2_len_sq(v2_sub(i0, w12));
    float d111 = v2_len_sq(v2_sub(i1, w11)), d112 = v2_len_sq(v2_sub(i1, w12));
    float m11 = d111 <= d011 ? d111 : d011, m12 = d112 <= d012 ? d112 : d012;
    v2 ref; bool second;
    if (m12 <= m11) {
        if (totalRadius * totalRadius < m12) return;
        ref = w12; second = d112 <= d012;
    } else {
        if (totalRadius * totalRadius < m11) return;
        ref = w11; second = d111 <= d011;
    }
    v2 refLocal = xf_mulT(xf1, ref), incLocal = xf_mulT(xf2, second ? i1 : i0);
    manifold->type = MT_CIRCLES;
    manifold->local_normal = V2(0.0f, 0.0f);
    manifold->local_point = refA ? refLocal : incLocal;           /* the point on A, in A's frame */
    manifold->point_count = 1;
    manifold->points[0].local_point = refA ? incLocal : refLocal; /* the point on B, in B's frame */
    manifold->points[0].id.key = 0;
}

/* b2PolygonShape::SetAsEdge (UP!0x180ba8480): the two-vertex polygon the fork collides a chain child as when either
 * side's radius is above the contact offset (b2ChainAndPolygonContact::Evaluate, UP!0x180bafea0): no ghost vertices. */
static void edge_as_polygon(piece_t *out, const piece_t *edge)
{
    memset(out, 0, sizeof *out);
    out->kind = PC_POLYGON; out->count = 2; out->radius = edge->radius;
    out->vertices[0] = edge->e1; out->vertices[1] = edge->e2;
    out->normals[0] = V2(edge->e2.y - edge->e1.y, (edge->e2.x - edge->e1.x) * -1.0f);
    v2_normalize(&out->normals[0]);
    out->normals[1] = v2_neg(out->normals[0]);
}

static void collide_circles(manifold_t *manifold, const piece_t *circleA, xf_t xfA, const piece_t *circleB, xf_t xfB)
{
    manifold->point_count = 0;
    v2 pA = xf_mul(xfA, circleA->center), pB = xf_mul(xfB, circleB->center);
    v2 d = v2_sub(pB, pA);
    float distSqr = v2_dot(d, d);
    float rA = circleA->radius, rB = circleB->radius;
    float radius = rA + rB;
    if (distSqr > radius * radius) return;
    manifold->type = MT_CIRCLES;
    manifold->local_point = circleA->center;
    manifold->local_normal = V2(0.0f, 0.0f);
    manifold->point_count = 1;
    manifold->points[0].local_point = circleB->center;
    manifold->points[0].id.key = 0;
}

static void collide_polygon_circle(manifold_t *manifold, const piece_t *polygonA, xf_t xfA, const piece_t *circleB, xf_t xfB)
{
    manifold->point_count = 0;
    v2 c = xf_mul(xfB, circleB->center);
    v2 cLocal = xf_mulT(xfA, c);
    int normalIndex = 0;
    float separation = -FLT_MAX;
    float radius = polygonA->radius + circleB->radius;
    int vertexCount = polygonA->count;
    const v2 *vertices = polygonA->vertices, *normals = polygonA->normals;
    for (int i = 0; i < vertexCount; i++) {
        float s = v2_dot(normals[i], v2_sub(cLocal, vertices[i]));
        if (s > radius) return;
        if (s > separation) { separation = s; normalIndex = i; }
    }
    int vertIndex1 = normalIndex, vertIndex2 = vertIndex1 + 1 < vertexCount ? vertIndex1 + 1 : 0;
    v2 v1 = vertices[vertIndex1], v2v = vertices[vertIndex2];
    if (separation < PH_EPSILON) {
        manifold->point_count = 1; manifold->type = MT_FACE_A;
        manifold->local_normal = normals[normalIndex];
        manifold->local_point = v2_scale(0.5f, v2_add(v1, v2v));
        manifold->points[0].local_point = circleB->center; manifold->points[0].id.key = 0;
        return;
    }
    float u1 = v2_dot(v2_sub(cLocal, v1), v2_sub(v2v, v1));
    float u2 = v2_dot(v2_sub(cLocal, v2v), v2_sub(v1, v2v));
    if (u1 <= 0.0f) {
        if (v2_len_sq(v2_sub(cLocal, v1)) > radius * radius) return;
        manifold->point_count = 1; manifold->type = MT_FACE_A;
        manifold->local_normal = v2_sub(cLocal, v1); v2_normalize(&manifold->local_normal);
        manifold->local_point = v1;
        manifold->points[0].local_point = circleB->center; manifold->points[0].id.key = 0;
    } else if (u2 <= 0.0f) {
        if (v2_len_sq(v2_sub(cLocal, v2v)) > radius * radius) return;
        manifold->point_count = 1; manifold->type = MT_FACE_A;
        manifold->local_normal = v2_sub(cLocal, v2v); v2_normalize(&manifold->local_normal);
        manifold->local_point = v2v;
        manifold->points[0].local_point = circleB->center; manifold->points[0].id.key = 0;
    } else {
        v2 faceCenter = v2_scale(0.5f, v2_add(v1, v2v));
        float sep = v2_dot(v2_sub(cLocal, faceCenter), normals[vertIndex1]);
        if (sep > radius) return;
        manifold->point_count = 1; manifold->type = MT_FACE_A;
        manifold->local_normal = normals[vertIndex1];
        manifold->local_point = faceCenter;
        manifold->points[0].local_point = circleB->center; manifold->points[0].id.key = 0;
    }
}

void ph_collide(manifold_t *m, const piece_t *A, xf_t xfA, const piece_t *B, xf_t xfB)
{
    if (A->kind == PC_POLYGON && B->kind == PC_POLYGON) {   /* b2PolygonContact::Evaluate (UP!0x180bb0120) */
        if (A->radius <= PH_POLYGON_RADIUS && B->radius <= PH_POLYGON_RADIUS) collide_polygons(m, A, xfA, B, xfB);
        else collide_radial_polygons(m, A, xfA, B, xfB);
        return;
    }
    if (A->kind == PC_CIRCLE && B->kind == PC_CIRCLE) { collide_circles(m, A, xfA, B, xfB); return; }
    if (A->kind == PC_POLYGON && B->kind == PC_CIRCLE) { collide_polygon_circle(m, A, xfA, B, xfB); return; }
    /* EdgeCollider2D chain segments (two-sided, ghost vertices): b2CollideEdgeAndPolygon / b2CollideEdgeAndCircle
     * structure, no trace contact to pin them (E10, Q-pphys-4). */
    if (A->kind == PC_EDGE && B->kind == PC_POLYGON) {      /* b2ChainAndPolygonContact::Evaluate (UP!0x180bafea0) */
        if (PH_POLYGON_RADIUS < A->radius || PH_POLYGON_RADIUS < B->radius) {
            piece_t edge; edge_as_polygon(&edge, A);
            collide_radial_polygons(m, &edge, xfA, B, xfB);
        } else ph_collide_edge_polygon(m, A, xfA, B, xfB);
        return;
    }
    if (A->kind == PC_EDGE && B->kind == PC_CIRCLE) { ph_collide_edge_circle(m, A, xfA, B, xfB); return; }
    HKSIM_UNIMPLEMENTED("phys: solid contact manifold for shape kinds %d vs %d not pinned (Q-pphys-4)", (int)A->kind, (int)B->kind);
}

/* b2WorldManifold::Initialize (normal points from A to B). */
void ph_world_manifold(const manifold_t *m, xf_t xfA, float rA, xf_t xfB, float rB, v2 *normal, v2 points[2], float seps[2])
{
    if (m->point_count == 0) return;
    switch (m->type) {
    case MT_CIRCLES: {
        *normal = V2(1.0f, 0.0f);
        v2 pointA = xf_mul(xfA, m->local_point), pointB = xf_mul(xfB, m->points[0].local_point);
        if (v2_len_sq(v2_sub(pointA, pointB)) > PH_EPSILON * PH_EPSILON) { *normal = v2_sub(pointB, pointA); v2_normalize(normal); }
        v2 cA = v2_add(pointA, v2_scale(rA, *normal)), cB = v2_sub(pointB, v2_scale(rB, *normal));
        points[0] = v2_scale(0.5f, v2_add(cA, cB));
        seps[0] = v2_dot(v2_sub(cB, cA), *normal);
        break;
    }
    case MT_FACE_A: {
        *normal = rot_mul(xfA.q, m->local_normal);
        v2 planePoint = xf_mul(xfA, m->local_point);
        for (int i = 0; i < m->point_count; i++) {
            v2 clipPoint = xf_mul(xfB, m->points[i].local_point);
            v2 cA = v2_add(clipPoint, v2_scale(rA - v2_dot(v2_sub(clipPoint, planePoint), *normal), *normal));
            v2 cB = v2_sub(clipPoint, v2_scale(rB, *normal));
            points[i] = v2_scale(0.5f, v2_add(cA, cB));
            seps[i] = v2_dot(v2_sub(cB, cA), *normal);
        }
        break;
    }
    default: {
        *normal = rot_mul(xfB.q, m->local_normal);
        v2 planePoint = xf_mul(xfB, m->local_point);
        for (int i = 0; i < m->point_count; i++) {
            v2 clipPoint = xf_mul(xfA, m->points[i].local_point);
            v2 cB = v2_add(clipPoint, v2_scale(rB - v2_dot(v2_sub(clipPoint, planePoint), *normal), *normal));
            v2 cA = v2_sub(clipPoint, v2_scale(rA, *normal));
            points[i] = v2_scale(0.5f, v2_add(cA, cB));
            seps[i] = v2_dot(v2_sub(cA, cB), *normal);
        }
        *normal = v2_neg(*normal);
        break;
    }
    }
}

/* b2TestOverlap(shape, shape): GJK distance WITH radii < 10*epsilon.  Sensors use this (b2Contact::Update). */
bool ph_test_overlap(const piece_t *A, xf_t xfA, const piece_t *B, xf_t xfB)
{
    dist_input in;
    ph_proxy_set(&in.proxyA, A); ph_proxy_set(&in.proxyB, B);
    in.xfA = xfA; in.xfB = xfB; in.use_radii = true;
    simplex_cache cache; cache.count = 0;
    dist_output out;
    ph_distance(&out, &cache, &in);
    return out.distance < 10.0f * PH_EPSILON;
}

/* Shape raycasts (b2PolygonShape/b2CircleShape/b2EdgeShape::RayCast).  A ray starting inside a polygon
 * or circle reports no hit, which is the queriesStartInColliders=false outcome
 * (cite: dumps/GG_Hornet_1/physics.json#Physics2D.queriesStartInColliders; Q-pphys-9). */
bool ph_piece_raycast(const piece_t *pc, xf_t xf, v2 ip1, v2 ip2, float maxFraction, float *fraction, v2 *normal)
{
    v2 p1 = rot_mulT(xf.q, v2_sub(ip1, xf.p)), p2 = rot_mulT(xf.q, v2_sub(ip2, xf.p));
    v2 d = v2_sub(p2, p1);
    switch (pc->kind) {
    case PC_POLYGON: {
        float lower = 0.0f, upper = maxFraction;
        int index = -1;
        for (int i = 0; i < pc->count; i++) {
            float numerator = v2_dot(pc->normals[i], v2_sub(pc->vertices[i], p1));
            float denominator = v2_dot(pc->normals[i], d);
            if (denominator == 0.0f) { if (numerator < 0.0f) return false; }
            else {
                if (denominator < 0.0f && numerator < lower * denominator) { lower = numerator / denominator; index = i; }
                else if (denominator > 0.0f && numerator < upper * denominator) { upper = numerator / denominator; }
            }
            if (upper < lower) return false;
        }
        HKSIM_ASSERT(0.0f <= lower && lower <= maxFraction, "phys: polygon raycast fraction");
        if (index >= 0) { *fraction = lower; *normal = rot_mul(xf.q, pc->normals[index]); return true; }
        return false;
    }
    case PC_CIRCLE: {
        v2 position = xf_mul(xf, pc->center);
        v2 s = v2_sub(ip1, position);
        float b = v2_dot(s, s) - pc->radius * pc->radius;
        v2 r = v2_sub(ip2, ip1);
        float c = v2_dot(s, r), rr = v2_dot(r, r);
        float sigma = c * c - rr * b;
        if (sigma < 0.0f || rr < PH_EPSILON) return false;
        float a = -(c + sqrtf(sigma));
        if (0.0f <= a && a <= maxFraction * rr) {
            a /= rr;
            *fraction = a;
            *normal = v2_add(s, v2_scale(a, r)); v2_normalize(normal);
            return true;
        }
        return false;
    }
    default: {
        v2 v1 = pc->e1, v2v = pc->e2;
        v2 e = v2_sub(v2v, v1);
        v2 n = V2(e.y, -e.x); v2_normalize(&n);
        float numerator = v2_dot(n, v2_sub(v1, p1));
        float denominator = v2_dot(n, d);
        if (denominator == 0.0f) return false;
        float t = numerator / denominator;
        if (t < 0.0f || maxFraction < t) return false;
        v2 q = v2_add(p1, v2_scale(t, d));
        v2 r = v2_sub(v2v, v1);
        float rr = v2_dot(r, r);
        if (rr == 0.0f) return false;
        float s = v2_dot(v2_sub(q, v1), r) / rr;
        if (s < 0.0f || 1.0f < s) return false;
        *fraction = t;
        *normal = numerator > 0.0f ? v2_neg(rot_mul(xf.q, n)) : rot_mul(xf.q, n);
        return true;
    }
    }
}
